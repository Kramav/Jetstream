#pragma once
// The texture and mesh browser panel: thin UI over core/browse (index, tree, search, mesh materials) and
// read_tex_pixels (previews, decoded by the GPU).
#include "browse.hpp"
#include "profile.hpp"

#include <d3d11.h>
#include <imgui.h>

#include <filesystem>
#include <future>
#include <map>
#include <optional>
#include <string>
#include <vector>

class Browser {
public:
    explicit Browser(ID3D11Device* device);
    ~Browser();
    Browser(const Browser&) = delete;
    Browser& operator=(const Browser&) = delete;

    // Draws the "Browser" window over the REtool folder `natives_root`. Returns the texture the user chose to use
    // in the graph (absolute path), else empty.
    std::string draw(const std::string& natives_root, const std::vector<remod::Profile>& profiles);

private:
    struct Index {
        remod::AssetIndex assets;
        remod::FolderTree texture_tree, mesh_tree;
    };
    struct Image {
        ID3D11ShaderResourceView* srv = nullptr;
        float width = 0, height = 0;  // visible size
        float u = 1, v = 1;           // visible part of the stored texture (rows can be padded)
        std::string error;
        int used = 0;                 // frame last drawn
    };

    const Image& image(const std::string& rel, unsigned max_side, bool force = false);
    void release_unused();
    void draw_tree(const remod::FolderTree& tree, size_t folder);
    int draw_tile(const std::string& rel, bool found, float size, bool selected);  // 1 clicked, 2 double-clicked
    void put_image(const Image& img, ImVec2 at, ImVec2 box);
    void select_texture(const std::string& rel, const std::vector<remod::Profile>& profiles);

    ID3D11Device* device_;
    ID3D11BlendState* opaque_ = nullptr;
    std::filesystem::path root_;
    std::string indexed_root_ = "\x01";
    std::future<Index> indexing_;
    std::optional<Index> index_;
    std::string error_;
    bool meshes_ = false;     // which list: textures or meshes
    bool alpha_ = false;      // draw transparency (off: alpha often holds other data, e.g. metalness)
    std::string query_, searched_ = "\x01";
    std::vector<size_t> hits_;
    size_t folder_[2] = {0, 0};  // selected folder per list
    std::string texture_, info_;  // selected texture and its description
    std::string mesh_;
    std::optional<remod::MeshTextures> mesh_textures_;
    std::string mesh_error_;
    std::map<std::string, Image> images_;  // key: max side + path
    int frame_ = 0;
    double load_ms_ = 0;  // spent loading images this frame
};
