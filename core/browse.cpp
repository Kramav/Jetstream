#include "browse.hpp"

#include "texture_converter.hpp"

#define NOMINMAX
#include <windows.h>  // GetLogicalDrives

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace remod {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool less_nocase(const std::string& a, const std::string& b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
    });
}

// "name<kind><digits>", e.g. kind ".tex." -> x.tex.143221013 (lower-case name).
bool is_kind(const std::string& name, const std::string& kind) {
    const size_t at = name.rfind(kind);
    if (at == std::string::npos || at + kind.size() == name.size()) return false;
    return std::all_of(name.begin() + std::ptrdiff_t(at + kind.size()), name.end(),
                       [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}

struct Mdf2Material {
    std::string name;
    std::string master;                    // the master material (.mmtr) path, e.g. ".../Character_Hair"
    std::array<float, 4> base_color{1, 1, 1, 1};  // the "BaseColor" parameter, white if none
    std::vector<std::pair<std::string, std::string>> textures;  // (type, path as written without the version suffix)
};

// An .mdf2 material file, layout for mdf versions >= 31 (RE4R's .mdf2.32) [REE-Lib MdfFile.cs, MIT]:
// u16 material count @6; per material, 100 bytes from 0x10: u64 name offset @0, u32 parameter count @16,
// u32 texture count @20, u64 parameter headers offset @52, u64 texture headers offset @60, u64 parameter values
// offset @76, u64 master material path offset @84. Per parameter, 0x18 bytes: u64 name offset, u64 hashes, u32 value
// offset (from the values offset), u16 float count, u16 (rarely set). Per texture, 0x20 bytes: u64 type-name
// offset, u64 hashes, u64 path offset. Strings are UTF-16. Checked 2026-10-01 on all 6,392 RE4R materials: every
// name and path parses.
std::vector<Mdf2Material> read_mdf2(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    const std::string b((std::istreambuf_iterator<char>(in)), {});
    auto need = [&](std::uint64_t at, std::uint64_t n) {
        if (at > b.size() || n > b.size() - at) throw std::runtime_error(file.string() + " is damaged or not an RE4R material");
    };
    auto le = [&](std::uint64_t at, int bytes) {
        need(at, std::uint64_t(bytes));
        std::uint64_t v = 0;
        for (int i = 0; i < bytes; ++i) v |= std::uint64_t(std::uint8_t(b[size_t(at) + i])) << (8 * i);
        return v;
    };
    auto text = [&](std::uint64_t at) {  // ASCII from UTF-16
        std::string s;
        for (; le(at, 2) != 0; at += 2) s += char(le(at, 2) < 0x80 ? le(at, 2) : '?');
        return s;
    };
    std::vector<Mdf2Material> out;
    const auto count = le(6, 2);
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint64_t m = 0x10 + i * 100;
        Mdf2Material mat{text(le(m, 8)), text(le(m + 84, 8))};
        const auto params = le(m + 16, 4), param_headers = le(m + 52, 8), values = le(m + 76, 8);
        for (std::uint64_t p = 0; p < params; ++p) {
            const std::uint64_t h = param_headers + p * 0x18;
            if (le(h + 20, 2) == 4 && text(le(h, 8)) == "BaseColor") {  // the first one, as Noesis shows it
                for (int c = 0; c < 4; ++c)
                    mat.base_color[size_t(c)] = std::bit_cast<float>(std::uint32_t(le(values + le(h + 16, 4) + 4 * c, 4)));
                break;
            }
        }
        const auto textures = le(m + 20, 4), headers = le(m + 60, 8);
        for (std::uint64_t t = 0; t < textures; ++t) {
            std::string path = text(le(headers + t * 0x20 + 16, 8));
            std::erase(path, '@');  // not part of the file's path
            mat.textures.emplace_back(text(le(headers + t * 0x20, 8)), path);
        }
        out.push_back(std::move(mat));
    }
    return out;
}

}  // namespace

AssetIndex index_assets(const fs::path& natives_root) {
    AssetIndex index;
    const std::string base = natives_root.generic_string();
    const size_t skip = base.size() + (base.ends_with('/') ? 0 : 1);  // "<root>/", with or without a final slash
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(natives_root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const std::string name = lower(it->path().filename().string());
        if (it.depth() == 0 && name == "streaming") {
            it.disable_recursion_pending();
            continue;
        }
        if (is_tex_name(name) && !name.ends_with(".tex"))  // game files carry a suffix; materials find them by it
            index.textures.push_back(it->path().generic_string().substr(skip));
        else if (is_kind(name, ".mesh."))
            index.meshes.push_back(it->path().generic_string().substr(skip));
    }
    if (ec) throw std::runtime_error("can't read " + natives_root.string() + ": " + ec.message());
    std::ranges::sort(index.textures, less_nocase);
    std::ranges::sort(index.meshes, less_nocase);
    return index;
}

FolderTree folder_tree(const std::vector<std::string>& paths) {
    FolderTree tree;
    tree.folders.emplace_back();
    std::map<std::pair<size_t, std::string>, size_t> known;  // (parent, lower-case name) -> folder
    for (size_t i = 0; i < paths.size(); ++i) {
        size_t folder = 0, start = 0;
        for (size_t slash; (slash = paths[i].find('/', start)) != std::string::npos; start = slash + 1) {
            const std::string name = paths[i].substr(start, slash - start);
            const auto [it, added] = known.try_emplace({folder, lower(name)}, tree.folders.size());
            if (added) {
                tree.folders.push_back({name, {}, {}});
                tree.folders[folder].children.push_back(it->second);
            }
            folder = it->second;
        }
        tree.folders[folder].files.push_back(i);
    }
    for (auto& f : tree.folders)
        std::ranges::sort(f.children, [&](size_t a, size_t b) { return less_nocase(tree.folders[a].name, tree.folders[b].name); });
    return tree;
}

std::vector<size_t> search(const std::vector<std::string>& paths, const std::string& query, const Nicknames* names) {
    std::vector<std::string> words;
    std::istringstream in(lower(query));
    for (std::string w; in >> w;) words.push_back(w);
    std::vector<size_t> hits;
    for (size_t i = 0; i < paths.size(); ++i) {
        std::string p = lower(paths[i]);
        if (names && !names->names.empty()) {  // add the nicknames of every folder above it and of the path itself
            std::string nicks;
            for (size_t end = p.find('/');; end = p.find('/', end + 1)) {
                const auto it = names->names.find(p.substr(0, end));
                if (it != names->names.end()) nicks += "\n" + lower(it->second);
                if (end == std::string::npos) break;
            }
            p += nicks;
        }
        if (std::ranges::all_of(words, [&](const std::string& w) { return p.find(w) != std::string::npos; }))
            hits.push_back(i);
    }
    return hits;
}

std::string file_name(const std::string& path) { return path.substr(path.rfind('/') + 1); }

FileKind file_kind(const std::string& name) {
    const std::string n = lower(name);
    if (is_tex_name(n)) return FileKind::Texture;
    if (is_kind(n, ".mesh.")) return FileKind::Mesh;
    for (const char* ext : {".png", ".tga", ".jpg"})
        if (n.ends_with(ext)) return FileKind::Image;
    return FileKind::Other;
}

std::vector<DirEntry> list_folder(const fs::path& folder, std::string* error) {
    std::vector<DirEntry> folders, files;
    std::error_code ec;
    fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        if (error) *error = "Can't open " + folder.string() + ": " + ec.message();
        return {};
    }
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;  // ponytail: stops at the first unreadable entry; per-entry recovery if a folder shows that
        std::error_code ignore;
        const std::string name = it->path().filename().string();
        if (it->is_directory(ignore)) folders.push_back({name, FileKind::Folder});
        else files.push_back({name, file_kind(name)});
    }
    for (auto* group : {&folders, &files})
        std::ranges::sort(*group, [](const DirEntry& a, const DirEntry& b) { return less_nocase(a.name, b.name); });
    folders.insert(folders.end(), files.begin(), files.end());
    return folders;
}

std::vector<fs::path> drive_roots() {
    std::vector<fs::path> out;
    const DWORD drives = ::GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
        if (drives & (1u << i)) out.push_back(std::string(1, char('A' + i)) + ":\\");
    return out;
}

std::vector<std::string> mesh_material_names(const fs::path& mesh_file) {
    std::vector<std::string> names;
    for (const MeshMaterial& m : mesh_textures(mesh_file.parent_path(), mesh_file.filename().string(), {}).materials)
        names.push_back(m.name);
    return names;
}

MeshTextures mesh_textures(const fs::path& natives_root, const std::string& mesh, const std::vector<std::string>& textures,
                           const fs::path& material_file) {
    const std::string stem = lower(file_name(mesh).substr(0, lower(file_name(mesh)).rfind(".mesh.")));
    fs::path dir = (natives_root / mesh).parent_path();
    std::vector<std::string> names;
    std::error_code ec;
    if (material_file.empty())
        for (const auto& e : fs::directory_iterator(dir, ec)) names.push_back(e.path().filename().string());
    std::ranges::sort(names, less_nocase);
    std::string material;
    if (!material_file.empty()) {  // chosen: e.g. a costume variant
        if (!fs::is_regular_file(material_file, ec)) throw std::runtime_error("no material file " + material_file.string());
        dir = material_file.parent_path();
        material = material_file.filename().string();
    }
    for (const std::string& want : {stem + ".mdf2.", stem + "_mat.mdf2.", stem + "_00.mdf2.", std::string()}) {
        if (!material.empty()) break;
        const auto it = std::ranges::find_if(names, [&](const std::string& n) {
            return is_kind(lower(n), ".mdf2.") && (want.empty() || lower(n).starts_with(want));
        });
        if (it != names.end()) {
            material = *it;
            break;
        }
    }
    if (material.empty()) throw std::runtime_error("no material (.mdf2) found next to " + mesh);

    MeshTextures out;
    out.material = mesh.substr(0, mesh.size() - file_name(mesh).size()) + material;
    for (const Mdf2Material& m : read_mdf2(dir / material)) {
        MeshMaterial mat{m.name, {}, -1};
        mat.base_color = m.base_color;
        // To match how Noesis shows a material (observed, CLAUDE.md §10 3D view): cut-outs
        // only for these master materials; a few materials not drawn at all.
        const std::string master = lower(m.master), mat_name = lower(m.name);
        const bool cutout = master.find("_dirt") != std::string::npos || master.find("_decal") != std::string::npos ||
                            master.find("_hair") != std::string::npos;
        mat.hidden = (mat_name.find("eye") != std::string::npos && m.textures.empty()) ||
                     mat_name.find("tearline") != std::string::npos || mat_name.find("lens") != std::string::npos ||
                     mat_name.find("destroy") != std::string::npos;
        bool translucency = false;
        int base_map = -1;  // the colour texture when there's no "_alb" one
        for (const auto& [type, s] : m.textures) {
            if (!lower(s).ends_with(".tex")) continue;  // e.g. .rtex render targets
            // Materials name textures without the version suffix: find "<path>.<digits>" in the sorted index.
            const std::string key = lower(s) + ".";
            auto it = std::ranges::lower_bound(textures, key, less_nocase);
            const bool found = it != textures.end() && is_tex_name(lower(*it)) && lower(*it).starts_with(key) &&
                               it->find('.', key.size()) == std::string::npos;
            const std::string name = found ? *it : s;
            auto known = std::ranges::find(out.textures, name);
            if (known == out.textures.end()) {
                out.textures.push_back(name);
                out.found.push_back(found);
                known = out.textures.end() - 1;
            }
            const size_t index = size_t(known - out.textures.begin());
            mat.textures.push_back(index);
            // The plugin's chain, in its order: colour texture (a file name with "_alb", "_albd" preferred; its
            // alpha cuts out when the type says so), normal map, translucency map (red cuts out), alpha map.
            const std::string file = lower(file_name(name));
            if (found && file.find("_alb") != std::string::npos &&
                (mat.albedo < 0 || file.find("_albd") != std::string::npos)) {
                mat.albedo = int(index);
                if (cutout && type.find("AlphaMap") != std::string::npos) mat.opacity = {int(index), 3};
            } else if (file.find("_nr") != std::string::npos) {
            } else if (type.find("AlphaTranslucent") != std::string::npos && !translucency) {
                translucency = true;
                if (cutout && found) mat.opacity = {int(index), 0};
            } else if (type == "AlphaMap") {
                if (cutout && found) mat.opacity = {int(index), 0};
            } else if (found && base_map < 0 && type.starts_with("Base") && type.ends_with("Map")) {
                base_map = int(index);
            }
        }
        if (mat.albedo < 0) mat.albedo = base_map;
        out.materials.push_back(std::move(mat));
    }
    return out;
}

fs::path preview_file(const fs::path& natives_root, const std::string& texture) {
    std::error_code ec;
    const fs::path streaming = natives_root / "streaming" / texture;
    return fs::is_regular_file(streaming, ec) ? streaming : natives_root / texture;
}

}  // namespace remod
