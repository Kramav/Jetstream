#include "frames.hpp"

#include "browse.hpp"
#include "image.hpp"
#include "mesh.hpp"
#include <algorithm>
#include <cctype>
#include <map>

namespace fs = std::filesystem;

namespace remod {

namespace {

struct Canvas {
    std::string frame;  // the mesh's file name
    fs::path picture;
    MeshModel mesh;
    std::vector<std::string> materials;  // its canvas parts
};

std::vector<const MeshPart*> parts_of(const Canvas& c) {
    std::vector<const MeshPart*> out;
    for (const MeshPart& p : c.mesh.parts)
        if (std::ranges::find(c.materials, p.material) != c.materials.end()) out.push_back(&p);
    return out;
}

// Every canvas on `tex` (a texture file) gets its picture; `overlaps` names canvases sharing pixels (the later wins).
void fill_texture(const fs::path& tex, const std::vector<const Canvas*>& canvases, const fs::path& to,
                  const Profile& profile, ITextureConverter& converter, std::string& overlaps) {
    Bgra image = decode_tex(tex, 1u << 30);
    const unsigned w = image.width, h = image.height;
    std::vector<std::string> owner(size_t(w) * h);
    for (const Canvas* c : canvases) {
        const std::vector<std::uint8_t> mask = uv_mask(parts_of(*c), w, h, 1);  // 1 px over the seams
        unsigned x0 = w, y0 = h, x1 = 0, y1 = 0;
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x)
                if (mask[(size_t(y) * w + x) * 4 + 2] >= 128)
                    x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
        if (x0 > x1) continue;  // its UVs miss the texture: nothing to fill
        const Bgra pic = resize_image(load_image(c->picture), x1 - x0 + 1, y1 - y0 + 1, Fit::Fill);
        for (unsigned y = y0; y <= y1; ++y)
            for (unsigned x = x0; x <= x1; ++x) {
                const size_t i = size_t(y) * w + x;
                if (mask[i * 4 + 2] < 128) continue;
                if (!owner[i].empty() && owner[i] != c->frame &&
                    overlaps.find(owner[i] + " and " + c->frame) == std::string::npos)
                    overlaps += (overlaps.empty() ? "" : ", ") + owner[i] + " and " + c->frame;
                owner[i] = c->frame;
                const std::uint8_t* s = &pic.pixels[(size_t(y - y0) * pic.width + (x - x0)) * 4];
                std::copy(s, s + 3, &image.pixels[i * 4]);  // colour only: the texture's alpha is data (CLAUDE.md §9)
            }
    }
    fs::create_directories(to.parent_path());
    const fs::path png = fs::path(to) += ".png";
    save_png(png, image);
    std::error_code ec;
    fs::remove(to, ec);
    try {
        converter.save_tex(png, tex, to, profile);
    } catch (...) {
        fs::remove(png, ec);
        throw;
    }
    fs::remove(png, ec);
}

}  // namespace

std::vector<PackageFile> pictures_in_frames(const std::vector<FramePicture>& frames,
                                            const std::function<bool(const std::string&)>& is_canvas,
                                            const fs::path& natives, const Profile& profile,
                                            ITextureConverter& converter, const fs::path& out, std::string* report) {
    const std::string suffix = "." + profile.tex_suffix;
    std::vector<Canvas> canvases;
    canvases.reserve(frames.size());  // pointers into it are kept below
    std::map<std::string, std::vector<const Canvas*>> by_texture;  // texture's game path -> its canvases
    std::string no_canvas, no_picture;
    for (const FramePicture& f : frames) {
        const std::string name = fs::path(f.mesh).filename().string();
        // The material's textures that are on disk, then matched to them (as Part texture does: no index needed).
        std::vector<std::string> present;
        MeshTextures m;
        try {
            for (const std::string& t : mesh_textures(natives, f.mesh, {}).textures)
                if (std::error_code ec; fs::is_regular_file(natives / (t + suffix), ec)) present.push_back(t + suffix);
            std::ranges::sort(present, [](const std::string& a, const std::string& b) {  // sorted, as an index is
                return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
                    return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
                });
            });
            m = mesh_textures(natives, f.mesh, present);
        } catch (const std::runtime_error& e) {
            throw PackageError(name + ": " + e.what());
        }
        Canvas c{name.substr(0, name.find(".")), f.picture, {}, {}};
        std::string texture;
        for (const MeshMaterial& mat : m.materials)
            if (mat.albedo >= 0 && m.found[size_t(mat.albedo)] && is_canvas(file_name(m.textures[size_t(mat.albedo)]))) {
                const std::string& t = m.textures[size_t(mat.albedo)];
                if (!texture.empty() && t != texture)
                    throw PackageError(name + ": its canvas parts use two textures (" + file_name(texture) + ", " +
                                       file_name(t) + "): narrow the canvas textures to one of them");
                texture = t;
                c.materials.push_back(mat.name);
            }
        std::ranges::transform(texture, texture.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        if (texture.empty()) {
            no_canvas += (no_canvas.empty() ? "" : ", ") + name;
            continue;
        }
        if (f.picture.empty()) {
            no_picture += (no_picture.empty() ? "" : ", ") + name.substr(0, name.find('.')) + " (" + file_name(texture) + ")";
            continue;
        }
        try {
            c.mesh = read_mesh(natives / f.mesh);
        } catch (const std::runtime_error& e) {
            throw PackageError(name + ": " + e.what());
        }
        canvases.push_back(std::move(c));
        by_texture[texture].push_back(&canvases.back());
    }
    if (by_texture.empty())
        throw PackageError(no_picture.empty()
                               ? "no frame has a canvas texture matching the patterns"
                               : "no pictures for these frames: name each after its frame, e.g. " +
                                     no_picture.substr(0, no_picture.find(' ')) + ".png (frames and their canvas: " +
                                     no_picture + ")");

    std::vector<PackageFile> files;
    std::string overlaps;
    size_t streaming = 0;
    for (const auto& [texture, list] : by_texture) {
        fill_texture(natives / texture, list, out / texture, profile, converter, overlaps);
        files.push_back({out / texture, texture});
        if (std::error_code ec; fs::is_regular_file(natives / "streaming" / texture, ec)) {
            fill_texture(natives / "streaming" / texture, list, out / "streaming" / texture, profile, converter, overlaps);
            files.push_back({out / "streaming" / texture, "streaming/" + texture});
            ++streaming;
        }
    }
    if (report)
        *report = std::to_string(canvases.size()) + " frame(s) on " + std::to_string(by_texture.size()) +
                  " texture(s) (" + std::to_string(streaming) + " with streaming copies)" +
                  (no_picture.empty() ? "" : "; no picture (left as they are): " + no_picture) +
                  (overlaps.empty() ? "" : "; sharing a canvas (the later picture wins): " + overlaps);
    return files;
}

namespace {

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// Natural order, ignoring case: runs of digits compare as numbers.
bool natural_less(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && std::isdigit(static_cast<unsigned char>(a[i2]))) ++i2;
            while (j2 < b.size() && std::isdigit(static_cast<unsigned char>(b[j2]))) ++j2;
            const std::string x = a.substr(i, i2 - i), y = b.substr(j, j2 - j);
            const std::string xs = x.substr(std::min(x.find_first_not_of('0'), x.size())),
                              ys = y.substr(std::min(y.find_first_not_of('0'), y.size()));
            if (xs.size() != ys.size()) return xs.size() < ys.size();
            if (xs != ys) return xs < ys;
            i = i2, j = j2;
            continue;
        }
        const char x = char(std::tolower(static_cast<unsigned char>(a[i]))), y = char(std::tolower(static_cast<unsigned char>(b[j])));
        if (x != y) return x < y;
        ++i, ++j;
    }
    return a.size() - i < b.size() - j;
}

}  // namespace

std::vector<fs::path> match_pictures(const std::vector<std::string>& names, std::vector<fs::path> pictures,
                                     std::vector<fs::path>* unused) {
    std::ranges::sort(pictures, [](const fs::path& a, const fs::path& b) {
        return natural_less(a.filename().string(), b.filename().string());
    });
    std::vector<fs::path> out(names.size());
    std::vector<bool> taken(pictures.size());
    for (size_t p = 0; p < pictures.size(); ++p) {  // named after one: that one's
        const std::string file = pictures[p].filename().string(), stem = lower(file.substr(0, file.find('.')));
        for (size_t n = 0; n < names.size(); ++n)
            if (out[n].empty() && lower(names[n]) == stem) {
                out[n] = pictures[p];
                taken[p] = true;
                break;
            }
    }
    size_t next = 0;
    for (fs::path& o : out) {  // the rest, in order
        while (next < pictures.size() && taken[next]) ++next;
        if (!o.empty() || next == pictures.size()) continue;
        o = pictures[next];
        taken[next] = true;
    }
    if (unused)
        for (size_t p = 0; p < pictures.size(); ++p)
            if (!taken[p]) unused->push_back(pictures[p]);
    return out;
}

}  // namespace remod
