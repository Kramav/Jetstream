#pragma once
// The texture and mesh browser panel: thin UI over core/browse (index, tree, search, folder listings, mesh
// materials) and read_tex_pixels / read_image_bgra (previews, decoded by the GPU or WIC).
#include "browse.hpp"
#include "mesh_view.hpp"
#include "movie_view.hpp"
#include "profile.hpp"
#include "sound.hpp"
#include "zoom_view.hpp"

#include <d3d11.h>
#include <imgui.h>

#include <filesystem>
#include <functional>
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

    // Draws the "Browser" window, the "Textures" window (thumbnails of the picked mesh's or the place's textures and
    // images) and the viewer window: the selected texture or image, or the picked mesh in 3D (converted by Noesis)
    // until the user closes it. The viewer's window id is "###viewer". The Browser goes anywhere: the game files
    // (the REtool folder `natives_root`, indexed for search), `pinned` folders (Pin / Unpin edits the list; the
    // caller saves it), and every drive. Returns the texture the user chose to use in the graph (absolute path), else
    // empty. `game`: the profile id whose nicknames (readable names, core/names; game files only) label the files.
    // `browser_only`: just the Browser window (Build layout). Files, folders and thumbnails can be dragged onto a
    // block's field: payload "remod_path", the absolute path.
    std::string draw(const std::string& natives_root, const std::string& noesis_exe,
                     const std::vector<remod::Profile>& profiles, const std::string& game,
                     std::vector<std::string>& pinned, bool browser_only = false);

    // Something of the caller's in the viewer (e.g. a block's preview, user 2026-10-02): `draw_view` fills the viewer
    // window ("Preview") until a texture or mesh is picked in the Browser again.
    void show_in_viewer(std::function<void()> draw_view) {
        external_ = std::move(draw_view);
        close_movie();
        close_sounds();
    }
    // A movie (a game movie or any video) playing in the viewer, until something else is picked or it's closed.
    void show_movie(const std::string& abs) {
        external_ = nullptr;
        close_sounds();
        movie_ = abs;
        movie_open_ = true;
    }
    bool viewer_shows_external() const { return bool(external_); }

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
    // A row of the file list, a thumbnail: its absolute path and what it is.
    struct Entry {
        std::string path;
        remod::FileKind kind;
    };

    // Every file is known by its absolute path. Inside the game files it also has a path relative to them (the
    // index's, nicknames' and streaming copies'); "" elsewhere.
    std::string abs_of(const std::string& rel) const;
    std::string rel_in_game(const std::string& abs) const;
    void go(const std::filesystem::path& folder);  // a disk folder
    void go_index(size_t folder, const std::string& rel);  // a folder of the game files' index
    void update_entries();                         // the list for the current place and search

    const Image& image(const std::string& abs, unsigned max_side, bool force = false);
    void release_unused();
    void draw_places(const std::string& natives_root);
    void draw_tree(const remod::FolderTree& tree, size_t folder, const std::string& path);
    void draw_disk_folder(const std::filesystem::path& folder, const std::string& label);
    void item_menu(const std::string& abs, bool folder);              // right-click the last item: Pin, Name...
    void drag_source(const std::string& abs, bool folder);           // the last item can be dragged as a path
    void show_nickname(const std::string& abs, bool above = false);  // after the last item, on its line
    int draw_tile(const std::string& abs, bool found, float size, bool selected);  // 1 clicked, 2 double-clicked
    void draw_files(const std::vector<remod::Profile>& profiles, std::string& chosen);
    void draw_textures(const std::vector<remod::Profile>& profiles, std::string& chosen);
    void draw_viewer(const std::string& noesis_exe);
    void put_image(const Image& img, ImVec2 at, ImVec2 box);
    void show_zoomed(const Image& img, ZoomPan& view);  // fills the rest of the window, zoomable
    void draw_popouts();
    void select_texture(const std::string& abs, const std::vector<remod::Profile>& profiles);
    void select_mesh(const std::string& abs);
    void close_movie() {
        movie_open_ = false;
        movie_view_.close();
    }
    void show_sounds(const std::string& abs);  // a bank's or package's sounds in the viewer
    void close_sounds();                        // stops playing too
    void draw_sounds();
    // Game files shown: everything indexed, or (sounds_only_) only the sound banks and packages, of one language.
    void update_filter();
    const std::vector<std::string>& game_files() const { return sounds_only_ ? shown_files_ : index_->files; }
    const remod::FolderTree& game_tree() const { return sounds_only_ ? shown_tree_ : index_->tree; }

    ID3D11Device* device_;
    ID3D11BlendState* opaque_ = nullptr;
    std::filesystem::path root_;  // the game files (indexed)
    std::string indexed_root_ = "\x01";
    std::future<Index> indexing_;
    std::optional<Index> index_;
    std::string error_;
    std::vector<std::string>* pinned_ = nullptr;  // this frame's pinned folders (the caller's)
    bool alpha_ = false;      // draw transparency (off: alpha often holds other data, e.g. metalness)
    // Where the Browser is: a folder of the game files' index, or any folder on disk.
    bool in_index_ = true;
    size_t folder_ = 0;               // the index folder
    std::string folder_rel_;          // its path, so a filter's other tree finds it again
    std::filesystem::path place_;     // the disk folder
    std::string address_;             // the address bar's text
    std::vector<remod::DirEntry> listing_;  // place_'s entries, read when going there or on Refresh
    std::string listing_error_;
    std::map<std::string, std::vector<std::string>> subfolders_;  // the disk tree: folder -> its subfolders' names
    std::string query_;
    float tree_share_ = 0.45f;  // the folder tree's share of the panel's height (the bar under it drags it)
    std::vector<Entry> entries_;      // the file list (and the Textures window's tiles) for the place and search
    std::string entries_for_ = "\x01";  // what entries_ was made for
    int version_ = 0;                 // bumped when the index, a listing or a nickname changes
    std::string texture_, info_;  // the selected texture or image and its description
    std::string mesh_;         // the last mesh picked: the 3D view keeps it while textures are browsed
    bool mesh_focus_ = false;  // the Textures window shows mesh_'s textures (else the place's)
    bool view_open_ = false;   // the viewer shows mesh_ in 3D (else the selected texture)
    std::string preview_of_;   // the texture the zoom and pan below belong to
    ZoomPan preview_view_;
    std::map<std::string, ZoomPan> popped_;  // pictures popped out into their own windows (absolute path)
    std::function<void()> external_;         // show_in_viewer's, while it's shown
    std::optional<remod::MeshTextures> mesh_textures_;  // its textures as absolute paths
    std::string mesh_error_;
    MeshView view_;
    MovieView movie_view_;
    std::string movie_;        // the movie in the viewer while movie_open_
    bool movie_open_ = false;
    // A sound bank or package in the viewer (CLAUDE.md §10 Game sounds): its sounds, one playing.
    std::string sounds_;
    bool sounds_open_ = false;
    std::future<std::vector<remod::SoundEntry>> sounds_loading_;
    // Listings replaced while still running: a std::async future waits for its task when destroyed, so they're
    // kept until done instead of freezing the window.
    std::vector<std::future<std::vector<remod::SoundEntry>>> retired_lists_;
    std::vector<remod::SoundEntry> sound_list_;
    std::string sounds_error_;
    std::future<std::string> decoding_;  // a sound's WAV bytes, for playing
    std::future<std::string> saving_;    // Save as WAV: "" when written, else why not
    std::uint32_t saving_id_ = 0;
    std::string playing_wav_;            // what PlaySound plays (it reads it while playing)
    std::uint32_t playing_id_ = 0;
    // The Game files filter (user, 2026-10-07: folders without sound banks, and other languages' files, filled the
    // Browser). ponytail: for this session only, not saved in the settings.
    bool sounds_only_ = false;
    std::string sound_language_;  // "" = every language; else files without a language and this one's
    std::vector<std::string> shown_files_, sound_languages_;
    remod::FolderTree shown_tree_;
    std::string filter_for_ = "\x01";
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
