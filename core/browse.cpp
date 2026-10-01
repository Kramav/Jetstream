#include "browse.hpp"

#include <algorithm>
#include <cctype>
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

// The UTF-16 texts in a file that end in ".tex". ponytail: reads the material's strings rather than its layout;
// every .tex path in RE4R's 6,392 materials resolved this way (2026-10-01). Parse the mdf2 layout if a game
// stores them differently.
std::vector<std::string> tex_strings(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    const std::string b((std::istreambuf_iterator<char>(in)), {});
    std::vector<std::string> out;
    for (size_t i = 0; i + 1 < b.size();) {
        std::string s;
        size_t j = i;
        for (; j + 1 < b.size() && b[j + 1] == 0 && b[j] >= 0x20 && b[j] < 0x7f; j += 2) s += b[j];
        if (s.size() >= 4 && lower(s).ends_with(".tex")) out.push_back(s);
        i = s.empty() ? i + 1 : j;
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
        if (is_kind(name, ".tex."))
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

std::vector<size_t> search(const std::vector<std::string>& paths, const std::string& query) {
    std::vector<std::string> words;
    std::istringstream in(lower(query));
    for (std::string w; in >> w;) words.push_back(w);
    std::vector<size_t> hits;
    for (size_t i = 0; i < paths.size(); ++i) {
        const std::string p = lower(paths[i]);
        if (std::ranges::all_of(words, [&](const std::string& w) { return p.find(w) != std::string::npos; }))
            hits.push_back(i);
    }
    return hits;
}

std::string file_name(const std::string& path) { return path.substr(path.rfind('/') + 1); }

MeshTextures mesh_textures(const fs::path& natives_root, const std::string& mesh, const std::vector<std::string>& textures) {
    const std::string stem = lower(file_name(mesh).substr(0, lower(file_name(mesh)).rfind(".mesh.")));
    const fs::path dir = (natives_root / mesh).parent_path();
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) names.push_back(e.path().filename().string());
    std::ranges::sort(names, less_nocase);
    std::string material;
    for (const std::string& want : {stem + ".mdf2.", stem + "_mat.mdf2.", stem + "_00.mdf2.", std::string()}) {
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
    for (const std::string& s : tex_strings(dir / material)) {
        // The material names textures without the version suffix: find "<path>.<digits>" in the sorted index.
        const std::string key = lower(s) + ".";
        auto it = std::ranges::lower_bound(textures, key, less_nocase);
        const bool found = it != textures.end() && is_kind(lower(*it), ".tex.") && lower(*it).starts_with(key) &&
                           it->find('.', key.size()) == std::string::npos;
        const std::string name = found ? *it : s;
        if (std::ranges::find(out.textures, name) != out.textures.end()) continue;
        out.textures.push_back(name);
        out.found.push_back(found);
    }
    return out;
}

fs::path preview_file(const fs::path& natives_root, const std::string& texture) {
    std::error_code ec;
    const fs::path streaming = natives_root / "streaming" / texture;
    return fs::is_regular_file(streaming, ec) ? streaming : natives_root / texture;
}

}  // namespace remod
