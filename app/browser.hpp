#pragma once
// The texture and mesh browser panel: thin UI over core/browse (index, tree, search, mesh materials) and
// read_tex_pixels (previews, decoded by the GPU).
#include "browse.hpp"
#include "mesh_view.hpp"
#include "profile.hpp"

#include <d3d11.h>
#include <imgui.h>

#include <filesystem>
#include <future>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

class Browser {
public:
    explicit Browser(ID3D11Device* device);
    ~Browser();
    Browser(const Browser&) = delete;
    Browser& operator=(const Browser&) = delete;

    // Draws the "Browser" window (the REtool folder `natives_root`'s file paths), the "Textures" window (thumbnails
    // of the picked mesh's or the folder's textures) and the viewer window: the selected texture, or the picked
    // mesh in 3D (converted by Noesis) until the user closes it. The viewer's window id is "###viewer". Returns the
    // texture the user chose to use in the graph (absolute path), else empty. `game`: the profile id whose nicknames
    // (readable names, core/names) label the files. `browser_only`: just the Browser window (Build layout).
    // Files, folders and thumbnails can be dragged onto a block's field: payload "remod_path", the absolute path.
    std::string draw(const std::string& natives_root, const std::string& noesis_exe,
                     const std::vector<remod::Profile>& profiles, const std::string& game, bool browser_only = false);

private:
    struct Index {
        remod::AssetIndex assets;
        std::vector<std::string> files;  // the meshes, then the textures
        remod::FolderTree tree;          // over files
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
    void draw_tree(const remod::FolderTree& tree, size_t folder, const std::string& path);
    void name_menu(const std::string& rel);                         // right-click the last item: name it
    void drag_source(const std::string& rel, bool folder);           // the last item can be dragged as a path
    void show_nickname(const std::string& rel, bool above = false);  // after the last item, on its line
    int draw_tile(const std::string& rel, bool found, float size, bool selected);  // 1 clicked, 2 double-clicked
    void draw_files(const std::vector<remod::Profile>& profiles, std::string& chosen);
    void draw_textures(bool ready, const std::vector<remod::Profile>& profiles, std::string& chosen);
    void draw_viewer(const std::string& noesis_exe);
    void put_image(const Image& img, ImVec2 at, ImVec2 box);
    void select_texture(const std::string& rel, const std::vector<remod::Profile>& profiles);
    void select_mesh(const std::string& rel);

    ID3D11Device* device_;
    ID3D11BlendState* opaque_ = nullptr;
    std::filesystem::path root_;
    std::string indexed_root_ = "\x01";
    std::future<Index> indexing_;
    std::optional<Index> index_;
    std::string error_;
    bool alpha_ = false;      // draw transparency (off: alpha often holds other data, e.g. metalness)
    std::string query_, searched_ = "\x01";
    std::vector<size_t> hits_;
    std::vector<size_t> shown_;  // the Browser's files this frame (folder or search), into Index::files
    size_t folder_ = 0;  // selected folder
    std::string texture_, info_;  // selected texture and its description
    std::string mesh_;         // the last mesh picked: the 3D view keeps it while textures are browsed
    bool mesh_focus_ = false;  // the Textures window shows mesh_'s textures (else the Browser's)
    bool view_open_ = false;   // the viewer shows mesh_ in 3D (else the selected texture)
    std::string preview_of_;   // the texture the zoom and pan below belong to
    float preview_zoom_ = 1;   // 1: fits the viewer
    ImVec2 preview_pan_;       // the picture's centre from the viewer's, in screen pixels
    std::optional<remod::MeshTextures> mesh_textures_;
    std::string mesh_error_;
    MeshView view_;
    std::future<remod::MeshModel> mesh_loading_;
    std::string loading_mesh_, shown_mesh_;  // being converted; in the view (or failed)
    std::string model_error_;
    std::vector<std::string> part_materials_;  // per part of the shown model
    std::vector<int> part_groups_;             // per part: its RE Engine mesh group
    std::set<int> hidden_groups_;              // groups the user turned off
    size_t triangles_ = 0;
    std::map<std::string, Image> images_;  // key: max side + path
    // Readable names: this game's nicknames and their file; no file while it couldn't be read (never saved over).
    remod::Nicknames names_;
    std::filesystem::path names_file_;
    std::string names_game_ = "\x01", names_error_;
    std::string naming_;  // the nickname being typed
    int frame_ = 0;
    double load_ms_ = 0;  // spent loading images this frame
};
