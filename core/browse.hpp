#pragma once
// The texture and mesh browser's data (CLAUDE.md §10): an index of an extracted natives folder (REtool), its
// folder tree, search, and which textures a mesh's material uses. Previews are read with read_tex_pixels.
#include "names.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace remod {

// Paths relative to the natives root (e.g. ...\natives\stm), '/'-separated, as found on disk (with the version
// suffix), sorted ignoring case.
struct AssetIndex {
    std::vector<std::string> textures;  // *.tex.*
    std::vector<std::string> meshes;    // *.mesh.*
};

// Walks the folder once. Skips streaming/: it holds high-resolution copies of textures listed elsewhere.
AssetIndex index_assets(const std::filesystem::path& natives_root);

// Folders of a path list. folders[0] is the root; files are indexes into the list, directly in that folder.
struct FolderTree {
    struct Folder {
        std::string name;
        std::vector<size_t> children;  // into folders, sorted by name
        std::vector<size_t> files;
    };
    std::vector<Folder> folders;
};
FolderTree folder_tree(const std::vector<std::string>& paths);

// Indexes of the paths containing every space-separated word of `query`, ignoring case. With `names`, a word may also
// be in the path's nickname or a folder's above it (naming cha000 "Leon" makes "leon albd" find its colour textures).
std::vector<size_t> search(const std::vector<std::string>& paths, const std::string& query,
                           const Nicknames* names = nullptr);

// The file name after the last '/'.
std::string file_name(const std::string& path);

// The materials of a mesh's material file (.mdf2) and the textures they use, matched to `textures` (an
// AssetIndex list). The file is <mesh>.mdf2.*, <mesh>_mat.mdf2.*, <mesh>_00.mdf2.* (fmt_RE_MESH's guesses
// [plugin source]), else the first .mdf2 in the mesh's folder. Throws if none is found or it can't be read.
struct MeshMaterial {
    std::string name;             // matches the mesh's parts (MeshPart::material)
    std::vector<size_t> textures;  // into MeshTextures::textures
    int albedo = -1;              // its colour texture, or -1
    // As fmt_RE_MESH shows it in Noesis [plugin source]: the colour texture is multiplied by base_color (the colour
    // on its own without one); `opacity` cuts out (alpha test at 0.05) where that texture's channel (0 red, 3 alpha)
    // is low, for hair, decal and dirt master materials only; `hidden` materials aren't drawn (eye shells without
    // textures, tear lines, lenses, "destroy" parts).
    std::array<float, 4> base_color{1, 1, 1, 1};
    struct Opacity {
        int texture = -1;  // into MeshTextures::textures, or -1
        int channel = 0;
    } opacity;
    bool hidden = false;
};
struct MeshTextures {
    std::string material;                 // relative path of the .mdf2
    std::vector<std::string> textures;    // every .tex used, in order, once: as listed in `textures`, or the
                                          // material's own text if not there
    std::vector<bool> found;              // per texture: in the index
    std::vector<MeshMaterial> materials;
};
MeshTextures mesh_textures(const std::filesystem::path& natives_root, const std::string& mesh,
                           const std::vector<std::string>& textures);

// The file to preview: the high-resolution streaming/ copy if there is one, else the texture itself.
std::filesystem::path preview_file(const std::filesystem::path& natives_root, const std::string& texture);

}  // namespace remod
