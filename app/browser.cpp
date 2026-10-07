#define NOMINMAX  // std::min/max, not windows.h's macros
#include "browser.hpp"

#include "image.hpp"
#include "texture_converter.hpp"

#include <imgui_impl_dx11.h>
#include <imgui_stdlib.h>
#include <mmsystem.h>
#include <nfd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <functional>

namespace fs = std::filesystem;
using remod::FileKind;

namespace {

constexpr unsigned kThumbSide = 128, kPreviewSide = 2048, kMeshTextureSide = 1024;

// sRGB formats are shown as their UNORM twins: the app draws into a UNORM back buffer, so the stored bytes are
// already what the screen wants. RE Engine's format numbers are DXGI's.
DXGI_FORMAT display_format(std::uint32_t f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_BC1_UNORM_SRGB: return DXGI_FORMAT_BC1_UNORM;
    case DXGI_FORMAT_BC3_UNORM_SRGB: return DXGI_FORMAT_BC3_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_BC7_UNORM_SRGB: return DXGI_FORMAT_BC7_UNORM;
    default: return DXGI_FORMAT(f);
    }
}

// Draw callback: blending off, so an image shows its colour whatever its alpha holds.
void draw_opaque(const ImDrawList*, const ImDrawCmd* cmd) {
    auto* state = static_cast<ImGui_ImplDX11_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    const float factor[4] = {};
    state->DeviceContext->OMSetBlendState(static_cast<ID3D11BlendState*>(cmd->UserCallbackData), factor, 0xffffffff);
}

// REtool's layout: index that automatically. Any other folder waits for the Index button (a half-typed "D:\"
// would otherwise start a scan of the whole drive).
bool looks_like_natives(std::string root) {
    while (!root.empty() && (root.back() == '\\' || root.back() == '/')) root.pop_back();
    std::ranges::transform(root, root.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    std::ranges::replace(root, '/', '\\');
    return root.ends_with("natives\\stm");
}

// A path's last part for showing: the file or folder name, or the whole path for a drive (C:\).
std::string display_name(const std::string& abs) {
    const std::string name = fs::path(abs).filename().string();
    return name.empty() ? abs : name;
}

FileKind kind_of(const std::string& abs) { return remod::file_kind(display_name(abs)); }

}  // namespace

Browser::Browser(ID3D11Device* device) : device_(device), view_(device), movie_view_(device) {
    D3D11_BLEND_DESC desc{};
    desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device_->CreateBlendState(&desc, &opaque_);
}

Browser::~Browser() {
    for (auto& [_, img] : images_)
        if (img.srv) img.srv->Release();
    if (opaque_) opaque_->Release();
}

std::string Browser::abs_of(const std::string& rel) const {
    return (root_ / fs::path(rel)).make_preferred().string();
}

std::string Browser::rel_in_game(const std::string& abs) const {
    if (root_.empty()) return "";
    const std::string a = fs::path(abs).generic_string();
    std::string r = root_.generic_string();
    while (!r.empty() && r.back() == '/') r.pop_back();
    if (a.size() <= r.size() + 1 || a[r.size()] != '/') return "";
    for (size_t i = 0; i < r.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(r[i]))) return "";
    return a.substr(r.size() + 1);
}

void Browser::go(const fs::path& folder) {
    in_index_ = false;
    place_ = folder;
    address_ = folder.string();
    listing_error_.clear();
    listing_ = remod::list_folder(folder, &listing_error_);
    mesh_focus_ = false;
    ++version_;
}

void Browser::go_index(size_t folder, const std::string& rel) {
    in_index_ = true;
    folder_ = folder;
    folder_rel_ = rel;
    address_ = rel.empty() ? root_.string() : abs_of(rel);
    mesh_focus_ = false;
}

void Browser::update_entries() {
    const bool searching = query_.find_first_not_of(' ') != std::string::npos;
    const bool game_scope = in_index_ || !rel_in_game(place_.string()).empty();
    const std::string key = (in_index_ ? "i" + std::to_string(folder_) : "d" + place_.string()) + "\n" +
                            (searching ? query_ : std::string()) + "\n" + std::to_string(version_);
    if (key == entries_for_) return;
    entries_for_ = key;
    entries_.clear();
    if (searching && game_scope && index_) {  // the whole game files, nicknames included
        for (const size_t i : remod::search(game_files(), query_, &names_))
            entries_.push_back({abs_of(game_files()[i]), remod::file_kind(game_files()[i])});
    } else if (in_index_) {
        if (index_ && folder_ < game_tree().folders.size())
            for (const size_t i : game_tree().folders[folder_].files)
                entries_.push_back({abs_of(game_files()[i]), remod::file_kind(game_files()[i])});
    } else {  // this folder, filtered by name when searching
        std::vector<std::string> names;
        for (const auto& e : listing_) names.push_back(e.name);
        std::vector<size_t> keep;
        if (searching) keep = remod::search(names, query_);
        else
            for (size_t i = 0; i < names.size(); ++i) keep.push_back(i);
        for (const size_t i : keep) entries_.push_back({(place_ / listing_[i].name).string(), listing_[i].kind});
    }
}

const Browser::Image& Browser::image(const std::string& abs, unsigned max_side, bool force) {
    static const Image loading;  // not loaded yet: over this frame's budget
    const std::string key = std::to_string(max_side) + "|" + abs;
    auto it = images_.find(key);
    if (it == images_.end()) {
        // ponytail: loads on the UI thread, ~0.5 ms per thumbnail, capped per frame; a worker thread if a slow
        // disk makes scrolling stutter.
        if (!force && load_ms_ > 8) return loading;
        const auto start = std::chrono::steady_clock::now();
        Image img;
        // `levels`: the picture, then (textures) its smaller mips, so the GPU shrinks it smoothly when it's drawn small
        // (a single large mip skips pixels: fine detail turns grainy, user 2026-10-06).
        auto upload = [&](DXGI_FORMAT format, unsigned w, unsigned h,
                          const std::vector<D3D11_SUBRESOURCE_DATA>& levels) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = w;
            desc.Height = h;
            desc.MipLevels = UINT(levels.size());
            desc.ArraySize = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            ID3D11Texture2D* tex = nullptr;
            if (FAILED(device_->CreateTexture2D(&desc, levels.data(), &tex)))
                throw std::runtime_error("the graphics card can't show this format");
            device_->CreateShaderResourceView(tex, nullptr, &img.srv);
            tex->Release();
        };
        try {
            // A game texture's preview shows its high-resolution streaming/ copy if there is one.
            const std::string rel = rel_in_game(abs);
            const fs::path file = max_side > kThumbSide && !rel.empty() ? remod::preview_file(root_, rel) : fs::path(abs);
            const FileKind kind = kind_of(abs);
            if (kind == FileKind::Texture) {
                const std::vector<remod::TexPixels> mips = remod::read_tex_mips(file, max_side);
                std::vector<D3D11_SUBRESOURCE_DATA> levels;
                for (const auto& m : mips) levels.push_back({m.data.data(), m.row_pitch, 0});
                const remod::TexPixels& p = mips[0];
                upload(display_format(p.format), p.stored_width, p.stored_height, levels);
                img.width = float(p.width);
                img.height = float(p.height);
                img.u = float(p.width) / float(p.stored_width);
                img.v = float(p.height) / float(p.stored_height);
            } else if (kind == FileKind::Image) {
                unsigned w = 0, h = 0;
                const std::vector<std::uint8_t> bgra = remod::read_image_bgra(file, w, h);
                upload(DXGI_FORMAT_B8G8R8A8_UNORM, w, h, {{bgra.data(), w * 4, 0}});  // ponytail: one level
                img.width = float(w);
                img.height = float(h);
            } else {
                throw std::runtime_error("no preview for this kind of file");
            }
        } catch (const std::exception& e) {
            img.error = e.what();
        }
        load_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        it = images_.emplace(key, std::move(img)).first;
    }
    it->second.used = frame_;
    return it->second;
}

void Browser::release_unused() {
    if (images_.size() < 400) return;  // ponytail: ~400 thumbnails of GPU memory, kept until something scrolls away
    std::erase_if(images_, [&](auto& item) {
        if (item.second.used >= frame_ - 1) return false;
        if (item.second.srv) item.second.srv->Release();
        return true;
    });
}

// The picture filling the rest of the window: wheel zooms, drag moves, double-click fits (zoom_area).
void Browser::show_zoomed(const Image& img, ZoomPan& view) {
    const ZoomPlace at = zoom_area("##zoomed", img.width, img.height, view);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(at.area_min, at.area_max, true);
    put_image(img, at.corner, at.size);
    draw->PopClipRect();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%.0f%%. Wheel to zoom, drag to move, double-click to fit.", at.scale * 100);
}

// Pictures popped out (right-click → Pop out, or the viewer's button): a window each, any size, until closed.
void Browser::draw_popouts() {
    for (auto it = popped_.begin(); it != popped_.end();) {
        bool open = true;
        const float font = ImGui::GetFontSize();
        ImGui::SetNextWindowSize(ImVec2(font * 32, font * 32), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImGui::GetMousePos(), ImGuiCond_FirstUseEver);
        if (ImGui::Begin((display_name(it->first) + "###pop " + it->first).c_str(), &open, ImGuiWindowFlags_NoDocking)) {
            const Image& img = image(it->first, kPreviewSide, true);
            ImGui::Checkbox("Transparency", &alpha_);
            if (!img.error.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "Can't preview: %s", img.error.c_str());
            if (img.srv && img.width > 0 && img.height > 0) show_zoomed(img, it->second);
        }
        ImGui::End();
        it = open ? std::next(it) : popped_.erase(it);
    }
}

void Browser::put_image(const Image& img, ImVec2 at, ImVec2 box) {
    if (!img.srv || img.width <= 0 || img.height <= 0) return;
    const float scale = std::min(box.x / img.width, box.y / img.height);
    const ImVec2 size(img.width * scale, img.height * scale);
    const ImVec2 a(at.x + (box.x - size.x) / 2, at.y + (box.y - size.y) / 2);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (!alpha_) draw->AddCallback(draw_opaque, opaque_);
    draw->AddImage(ImTextureRef(img.srv), a, ImVec2(a.x + size.x, a.y + size.y), ImVec2(0, 0), ImVec2(img.u, img.v));
    if (!alpha_) draw->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

int Browser::draw_tile(const std::string& abs, bool found, float size, bool selected) {
    ImGui::PushID(abs.c_str());
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float line = ImGui::GetTextLineHeight();
    int result = 0;
    if (ImGui::Selectable("##tile", selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(size, size + line)))
        result = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ? 2 : 1;
    const bool hovered = ImGui::IsItemHovered() && !ImGui::GetDragDropPayload();
    if (found) drag_source(abs, false);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    std::string problem = found ? "" : "Not in the extracted files.";
    if (found) {
        const Image& img = image(abs, kThumbSide);
        if (img.srv)
            put_image(img, at, ImVec2(size, size));
        else
            draw->AddText(ImVec2(at.x + 4, at.y + 4), dim, img.error.empty() ? "..." : "?");
        problem = img.error;
    } else {
        draw->AddText(ImVec2(at.x + 4, at.y + 4), dim, "missing");
    }
    draw->PushClipRect(ImVec2(at.x, at.y + size), ImVec2(at.x + size, at.y + size + line), true);
    const std::string rel = rel_in_game(abs);  // the caption: its nickname (game files), else its file name
    const std::string nick = rel.empty() ? std::string() : remod::nickname(names_, rel);
    draw->AddText(ImVec2(at.x, at.y + size), ImGui::GetColorU32(nick.empty() ? ImGuiCol_Text : ImGuiCol_CheckMark),
                  nick.empty() ? display_name(abs).c_str() : nick.c_str());
    draw->PopClipRect();
    if (hovered)
        ImGui::SetTooltip("%s%s%s%s%s", nick.empty() ? "" : (nick + "\n").c_str(), abs.c_str(), problem.empty() ? "" : "\n",
                          problem.c_str(), rel.empty() ? "" : "\nRight-click to name it.");
    item_menu(abs, false);
    ImGui::PopID();
    return result;
}

// Right-click the last item: Pin / Unpin for a folder (anywhere), and a nickname inside the game files (only there,
// so the nicknames file holds just the game's names), saved at once. Empty removes the nickname.
void Browser::item_menu(const std::string& abs, bool folder) {
    const std::string rel = rel_in_game(abs);
    const bool picture = !folder && (kind_of(abs) == FileKind::Texture || kind_of(abs) == FileKind::Image);
    if (!folder && rel.empty() && !picture) return;  // a file outside the game files: nothing to offer
    if (!ImGui::BeginPopupContextItem()) return;
    if (picture) {
        if (ImGui::MenuItem("Pop out")) popped_.try_emplace(abs);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open it in its own window, to enlarge it.");
        if (!rel.empty()) ImGui::Separator();
    }
    if (folder && pinned_) {
        const auto pin = std::ranges::find(*pinned_, abs);
        if (pin == pinned_->end() ? ImGui::MenuItem("Pin") : ImGui::MenuItem("Unpin")) {
            if (pin == pinned_->end()) pinned_->push_back(abs);
            else pinned_->erase(pin);
        }
        if (!rel.empty()) ImGui::Separator();
    }
    if (!rel.empty()) {
        if (ImGui::IsWindowAppearing()) naming_ = remod::nickname(names_, rel);
        ImGui::TextDisabled("Nickname for %s", remod::file_name(rel).c_str());
        if (names_file_.empty()) {
            ImGui::TextDisabled("Nicknames can't be saved: %s", names_error_.c_str());
        } else {
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            const bool enter =
                ImGui::InputTextWithHint("##nickname", "e.g. Leon", &naming_, ImGuiInputTextFlags_EnterReturnsTrue);
            if (enter || ImGui::Button("OK")) {
                remod::set_nickname(names_, rel, naming_);
                try {
                    remod::save_names(names_, names_file_);
                } catch (const std::exception& e) {
                    names_error_ = e.what();
                }
                ++version_;  // search again: it matches nicknames
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::TextDisabled("Only a label here: mods use the real name. Empty removes it.");
        }
    }
    ImGui::EndPopup();
}

// The last item can be dragged onto a block's field in the graph, as its absolute path ("remod_path").
void Browser::drag_source(const std::string& abs, bool folder) {
    if (!ImGui::BeginDragDropSource()) return;
    ImGui::SetDragDropPayload("remod_path", abs.c_str(), abs.size() + 1);
    const std::string rel = rel_in_game(abs);
    const std::string nick = rel.empty() ? std::string() : remod::nickname(names_, rel);
    ImGui::Text("%s%s", nick.empty() ? display_name(abs).c_str() : nick.c_str(), folder ? " (folder)" : "");
    ImGui::TextDisabled("Drop it on a field in a block.");
    ImGui::EndDragDropSource();
}

// The item's nickname beside it (game files only); with `above` and none of its own, its nearest named folder's, dimmed.
void Browser::show_nickname(const std::string& abs, bool above) {
    const std::string rel = rel_in_game(abs);
    if (rel.empty()) return;
    std::string nick = remod::nickname(names_, rel);
    const bool own = !nick.empty();
    if (!own && above) nick = remod::nickname_above(names_, rel);
    if (nick.empty()) return;
    ImGui::SameLine();
    if (own) ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "%s", nick.c_str());
    else ImGui::TextDisabled("(%s)", nick.c_str());
}

// The game files' index tree (folders only; their files are in the list).
void Browser::draw_tree(const remod::FolderTree& tree, size_t folder, const std::string& path) {
    for (size_t c : tree.folders[folder].children) {
        const auto& f = tree.folders[c];
        const std::string child = path.empty() ? f.name : path + "/" + f.name;
        const std::string abs = abs_of(child);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                   ImGuiTreeNodeFlags_SpanAvailWidth;
        if (f.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
        if (in_index_ && folder_ == c) flags |= ImGuiTreeNodeFlags_Selected;
        // Keyed by path, not number: the filter's trees number folders differently, and open folders stay open.
        const bool open = f.files.empty() ? ImGui::TreeNodeEx(child.c_str(), flags, "%s", f.name.c_str())
                                          : ImGui::TreeNodeEx(child.c_str(), flags, "%s  (%zu)", f.name.c_str(),
                                                              f.files.size());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) go_index(c, child);
        drag_source(abs, true);
        item_menu(abs, true);
        show_nickname(abs);
        if (open) {
            draw_tree(tree, c, child);
            ImGui::TreePop();
        }
    }
}

// A folder on disk in the tree; its subfolders are read when it's opened (kept until Refresh).
void Browser::draw_disk_folder(const fs::path& folder, const std::string& label) {
    const std::string abs = folder.string();
    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (!in_index_ && place_ == folder) flags |= ImGuiTreeNodeFlags_Selected;
    const bool open = ImGui::TreeNodeEx(abs.c_str(), flags, "%s", label.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) go(folder);
    if (ImGui::IsItemHovered() && !ImGui::GetDragDropPayload()) ImGui::SetTooltip("%s\nRight-click to pin it.", abs.c_str());
    drag_source(abs, true);
    item_menu(abs, true);
    show_nickname(abs);
    if (!open) return;
    auto it = subfolders_.find(abs);
    if (it == subfolders_.end()) {
        std::vector<std::string> names;
        for (const auto& e : remod::list_folder(folder))
            if (e.kind == FileKind::Folder) names.push_back(e.name);
        it = subfolders_.emplace(abs, std::move(names)).first;
    }
    for (const std::string& name : it->second) draw_disk_folder(folder / name, name);
    ImGui::TreePop();
}

// Pinned (the game files first, then the user's pins), then This PC's drives.
void Browser::draw_places(const std::string& natives_root) {
    ImGui::TextDisabled("Pinned");
    ImGui::PushID("game");
    const bool indexed = index_ && natives_root == indexed_root_;
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
    if (!indexed) flags |= ImGuiTreeNodeFlags_Leaf;
    if (in_index_ && folder_ == 0) flags |= ImGuiTreeNodeFlags_Selected;
    const bool open = ImGui::TreeNodeEx("##game", flags, "Game files");
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) go_index(0, "");
    if (ImGui::IsItemHovered() && !ImGui::GetDragDropPayload())
        ImGui::SetTooltip("%s", indexed ? root_.string().c_str()
                                        : "Set the Game files folder (your REtool folder) in the Pipeline panel.");
    if (indexed) drag_source(root_.string(), true);
    if (open) {
        if (indexed) draw_tree(game_tree(), 0, "");
        // The index leaves out streaming\ (the high-resolution copies, the movies): browsed on disk, read when opened.
        // ponytail: not searchable from Game files; indexing it would double the index's time and every hit.
        // Not with the sound filter: a package's sounds are listed from its copy outside streaming\.
        if (std::error_code ec; indexed && !sounds_only_ && fs::is_directory(root_ / "streaming", ec))
            draw_disk_folder(root_ / "streaming", "streaming");
        ImGui::TreePop();
    }
    ImGui::PopID();
    ImGui::PushID("pins");
    for (const std::string& pin : std::vector<std::string>(pinned_ ? *pinned_ : std::vector<std::string>{}))
        draw_disk_folder(pin, display_name(pin));  // a copy: Unpin edits the list
    ImGui::PopID();
    ImGui::TextDisabled("This PC");
    ImGui::PushID("drives");
    for (const fs::path& drive : remod::drive_roots()) draw_disk_folder(drive, drive.string());
    ImGui::PopID();
}

void Browser::select_texture(const std::string& abs, const std::vector<remod::Profile>& profiles) {
    external_ = nullptr;  // the viewer shows the texture again
    close_movie();
    close_sounds();
    texture_ = abs;
    info_.clear();
    if (kind_of(abs) != FileKind::Texture) return;  // an image: the viewer shows it
    const fs::path file(abs);
    try {
        if (const remod::Profile* p = remod::profile_for_texture(file, profiles)) {
            const remod::TexMeta m = remod::read_tex_meta(file, *p);
            info_ = std::to_string(m.width) + "x" + std::to_string(m.height) + " " + m.format + ", " +
                    std::to_string(m.mip_count) + " mip" + (m.mip_count == 1 ? "" : "s") + " (" + p->name + ")";
        } else {
            info_ = "No game profile uses this texture's version yet.";
        }
        if (const std::string rel = rel_in_game(abs); !rel.empty() && remod::preview_file(root_, rel) != file) {
            const remod::TexPixels big = remod::read_tex_pixels(remod::preview_file(root_, rel), 1u << 16);
            info_ += "\nThe game also has a " + std::to_string(big.width) + "x" + std::to_string(big.height) +
                     " copy in streaming/ (shown here). A mod replaces only the texture above.";
        }
    } catch (const std::exception& e) {
        info_ = e.what();
    }
}

void Browser::select_mesh(const std::string& abs) {
    external_ = nullptr;
    close_movie();
    close_sounds();
    mesh_focus_ = view_open_ = true;
    if (abs == mesh_) return;  // picked again: shows it again
    mesh_ = abs;
    texture_.clear();  // a texture picked elsewhere would dim the parts not using it
    mesh_error_.clear();
    mesh_textures_.reset();
    model_error_.clear();
    view_.clear();  // until the new one is converted
    // Its material's textures are found through the game files' index: only for meshes in the game files.
    const std::string rel = rel_in_game(abs);
    if (rel.empty() || !index_) {
        mesh_error_ = "Textures are matched only for meshes in the game files; the shape still shows.";
        return;
    }
    try {
        remod::MeshTextures found = remod::mesh_textures(root_, rel, index_->assets.textures);
        for (std::string& t : found.textures) t = abs_of(t);
        mesh_textures_ = std::move(found);
    } catch (const std::exception& e) {
        mesh_error_ = e.what();
    }
}

std::string Browser::draw(const std::string& natives_root, const std::string& noesis_exe,
                          const std::vector<remod::Profile>& profiles, const std::string& game,
                          std::vector<std::string>& pinned, bool browser_only) {
    ++frame_;
    load_ms_ = 0;
    pinned_ = &pinned;
    release_unused();
    std::string chosen;
    ImGui::Begin("Browser");

    // This game's nicknames, read when the game changes. A file that can't be read is reported and left alone.
    if (game != names_game_) {
        names_game_ = game;
        names_ = {};
        names_error_.clear();
        names_file_ = remod::default_names_path(game);
        try {
            names_ = remod::load_names(names_file_);
        } catch (const std::exception& e) {
            names_error_ = e.what();
            names_file_.clear();
        }
        ++version_;
    }
    if (!names_error_.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "%s", names_error_.c_str());

    // Index the game files once, in the background (a few seconds for RE4R's 26k textures and meshes).
    std::error_code ec;
    const bool usable = !natives_root.empty() && fs::is_directory(natives_root, ec);
    bool start = usable && natives_root != indexed_root_ && looks_like_natives(natives_root);
    if (usable && natives_root != indexed_root_ && !start) {
        ImGui::TextWrapped("%s isn't a natives\\stm folder.", natives_root.c_str());
        start = ImGui::Button("Index it anyway");
    }
    if (start) {
        indexed_root_ = natives_root;
        root_ = natives_root;
        index_.reset();
        error_.clear();
        texture_.clear();
        mesh_.clear();
        mesh_textures_.reset();
        view_.clear();
        go_index(0, "");
        for (auto& [_, img] : images_)
            if (img.srv) img.srv->Release();
        images_.clear();
        indexing_ = std::async(std::launch::async, [root = root_] {
            Index i;
            i.assets = remod::index_assets(root);
            i.files = i.assets.meshes;
            i.files.insert(i.files.end(), i.assets.textures.begin(), i.assets.textures.end());
            i.files.insert(i.files.end(), i.assets.sounds.begin(), i.assets.sounds.end());
            i.tree = remod::folder_tree(i.files);
            return i;
        });
    }
    if (indexing_.valid() && indexing_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            index_ = indexing_.get();
        } catch (const std::exception& e) {
            error_ = e.what();
        }
        ++version_;
    }
    if (indexing_.valid()) ImGui::TextDisabled("Reading %s...", root_.string().c_str());
    if (!error_.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "%s", error_.c_str());
    draw_files(profiles, chosen);
    // The game files' place is the index: without one (not set yet), there's nothing in it.
    if (in_index_ && !(index_ && natives_root == indexed_root_) && !usable)
        ImGui::TextDisabled("Set the Game files folder (your REtool folder, e.g. ...\\re_chunk_000\\natives\\stm) in "
                            "the Pipeline panel, or pick any folder above.");
    ImGui::End();

    draw_popouts();  // both layouts
    if (browser_only) {
        movie_view_.close();  // no viewer in this layout: shown again, from the start, when it's back
        close_sounds();
        return chosen;
    }
    draw_textures(profiles, chosen);
    draw_viewer(noesis_exe);
    return chosen;
}

void Browser::draw_files(const std::vector<remod::Profile>& profiles, std::string& chosen) {
    // The address bar: Up, the folder's path (type or paste one, Enter goes there), Refresh.
    const ImGuiStyle& style = ImGui::GetStyle();
    if (ImGui::Button("Up") && !address_.empty()) {
        const fs::path up = fs::path(address_).parent_path();
        if (!up.empty() && up != fs::path(address_)) go(up);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The folder above this one.");
    ImGui::SameLine();
    const float refresh_w = ImGui::CalcTextSize("Refresh").x + style.FramePadding.x * 2;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - refresh_w - style.ItemSpacing.x);
    if (ImGui::InputTextWithHint("##address", "A folder, e.g. D:\\mods", &address_, ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::error_code ec;
        std::string typed = address_;
        typed.erase(0, typed.find_first_not_of(" \""));
        while (!typed.empty() && (typed.back() == '"' || typed.back() == ' ')) typed.pop_back();
        if (fs::is_directory(typed, ec)) go(typed);
        else listing_error_ = "No folder " + typed;
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        subfolders_.clear();
        if (in_index_) ++version_;
        else go(place_);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Read the folders again (after files changed outside the tool).");

    const bool game_scope = in_index_ || !rel_in_game(place_.string()).empty();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", game_scope ? "Search the game files, e.g. ui3200 or wood albd"
                                                    : "Filter this folder by name",
                             &query_);
    const bool searching = query_.find_first_not_of(' ') != std::string::npos;

    // What Game files shows: everything indexed, or only the sound banks and packages (one language's, if picked).
    update_filter();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Show");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    if (ImGui::BeginCombo("##show", sounds_only_ ? "Sound files" : "Everything")) {
        for (const bool sounds : {false, true})
            if (ImGui::Selectable(sounds ? "Sound files" : "Everything", sounds == sounds_only_) && sounds != sounds_only_) {
                sounds_only_ = sounds;
                update_filter();
            }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Sound files: only the folders with sound banks (.sbnk) and packages (.spck) in Game files.");
    if (sounds_only_) {
        ImGui::SameLine();
        ImGui::TextDisabled("Language");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        if (ImGui::BeginCombo("##language", sound_language_.empty() ? "All" : sound_language_.c_str())) {
            if (ImGui::Selectable("All", sound_language_.empty())) sound_language_.clear();
            for (const std::string& language : sound_languages_)
                if (ImGui::Selectable(language.c_str(), language == sound_language_)) sound_language_ = language;
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Hides the other languages' banks and packages (dialogue); files without a language stay.");
    }

    if (!searching) {
        // A share of the panel, not ImGui's ResizeY: that keeps the first frame's height, taken before the dock
        // layout gives the panel its size, so the tree started as a sliver.
        const float avail = ImGui::GetContentRegionAvail().y;
        ImGui::BeginChild("tree", ImVec2(0, std::max(3 * ImGui::GetFrameHeight(), avail * tree_share_)),
                          ImGuiChildFlags_Borders);
        draw_places(indexed_root_);
        ImGui::EndChild();
        ImGui::InvisibleButton("##tree_split", ImVec2(-FLT_MIN, style.ItemSpacing.y + 2));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActive() && avail > 0)
            tree_share_ = std::clamp(tree_share_ + ImGui::GetIO().MouseDelta.y / avail, 0.1f, 0.9f);
    }

    // The place's files (a disk folder's folders first) or the search hits: names only, the Textures window shows them.
    update_entries();
    ImGui::BeginChild("files", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (searching) ImGui::TextDisabled("%zu found", entries_.size());
    else if (!in_index_ && !listing_error_.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "%s", listing_error_.c_str());
    else if (entries_.empty()) ImGui::TextDisabled("Nothing directly in this folder.");
    const bool full_paths = searching && game_scope;  // hits from all over the game files
    ImGuiListClipper clip;
    clip.Begin(int(entries_.size()));
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
            const Entry& e = entries_[size_t(i)];
            const bool folder = e.kind == FileKind::Folder;
            const std::string label = full_paths ? rel_in_game(e.path) : display_name(e.path) + (folder ? "\\" : "");
            ImGui::PushID(i);
            const std::string& shown = e.kind == FileKind::Mesh    ? mesh_
                                       : e.kind == FileKind::Movie ? movie_
                                       : e.kind == FileKind::Sound ? sounds_
                                                                   : texture_;
            if (ImGui::Selectable(label.c_str(), e.path == shown,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                const bool twice = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                if (folder) {
                    if (twice) go(e.path);
                } else if (e.kind == FileKind::Mesh) {
                    select_mesh(e.path);
                } else if (e.kind == FileKind::Movie) {
                    show_movie(e.path);
                } else if (e.kind == FileKind::Sound) {
                    show_sounds(e.path);
                } else if (e.kind == FileKind::Texture || e.kind == FileKind::Image) {
                    mesh_focus_ = false;  // the 3D view keeps the last mesh
                    select_texture(e.path, profiles);
                    if (twice && e.kind == FileKind::Texture) chosen = e.path;
                }
            }
            if (ImGui::IsItemHovered() && !ImGui::GetDragDropPayload())
                ImGui::SetTooltip("%s\n%s%s", e.path.c_str(), folder ? "Double-click to open it. " : "",
                                  rel_in_game(e.path).empty() ? "Drag it onto a field in a block."
                                                              : "Right-click to name it, drag it onto a field in a block.");
            drag_source(e.path, folder);
            item_menu(e.path, folder);
            show_nickname(e.path, full_paths);  // a search lists files from many folders: say whose they are
            ImGui::PopID();
        }
    ImGui::EndChild();
}

void Browser::draw_textures(const std::vector<remod::Profile>& profiles, std::string& chosen) {
    ImGui::Begin("Textures");
    // The selected mesh's textures, else the textures and images among the Browser's files.
    const bool of_mesh = mesh_focus_ && !mesh_.empty() && mesh_textures_;
    std::vector<std::pair<std::string, bool>> texs;  // (absolute path, there)
    if (of_mesh) {
        for (size_t i = 0; i < mesh_textures_->textures.size(); ++i)
            texs.emplace_back(mesh_textures_->textures[i], mesh_textures_->found[i]);
    } else {
        for (const Entry& e : entries_)
            if (e.kind == FileKind::Texture || e.kind == FileKind::Image) texs.emplace_back(e.path, true);
    }

    if (!texture_.empty()) {
        if (kind_of(texture_) == FileKind::Texture) {
            if (ImGui::Button("Use in graph")) chosen = texture_;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Puts this texture into the selected Original texture block, or the graph's only one.\n"
                                  "In Build layout, adds a block if there's none. Double-clicking a texture does the same.");
            ImGui::SameLine();
        }
        ImGui::Checkbox("Transparency", &alpha_);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Show the alpha channel as transparency. Off by default: in many textures it holds other "
                              "data (e.g. metalness), which would hide the picture.");
        ImGui::SameLine();
        ImGui::TextUnformatted(display_name(texture_).c_str());
        show_nickname(texture_, true);
        ImGui::PushTextWrapPos(0);
        if (!info_.empty()) ImGui::TextDisabled("%s", info_.c_str());
        ImGui::PopTextWrapPos();
    }
    if (of_mesh) {
        ImGui::TextDisabled("%s: %zu texture%s. Click one to highlight the parts using it, again to show all.",
                            mesh_textures_->material.c_str(), texs.size(), texs.size() == 1 ? "" : "s");
    } else if (texs.empty()) {
        ImGui::TextDisabled("No textures or images here.");
    }
    ImGui::BeginChild("tiles");
    const float font = ImGui::GetFontSize();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float tile = font * 6;
    const int columns = std::max(1, int((ImGui::GetContentRegionAvail().x + style.ItemSpacing.x) / (tile + style.ItemSpacing.x)));
    ImGuiListClipper clip;
    clip.Begin((int(texs.size()) + columns - 1) / columns, tile + ImGui::GetTextLineHeight() + style.ItemSpacing.y);
    while (clip.Step())
        for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row)
            for (int c = 0; c < columns && size_t(row * columns + c) < texs.size(); ++c) {
                if (c) ImGui::SameLine();
                const auto& [abs, found] = texs[size_t(row * columns + c)];
                if (const int click = draw_tile(abs, found, tile, abs == texture_); click && found) {
                    if (of_mesh && click == 1 && abs == texture_)
                        texture_.clear();  // shows the whole mesh again
                    else
                        select_texture(abs, profiles);
                    if (click == 2 && kind_of(abs) == FileKind::Texture) chosen = abs;
                }
            }
    ImGui::EndChild();
    ImGui::End();
}

void Browser::draw_viewer(const std::string& noesis_exe) {
    // The selected mesh's shape, converted by Noesis in the background, one at a time: a mesh picked meanwhile is
    // converted next (waiting on a running conversion would freeze the panel).
    if (mesh_loading_.valid() && mesh_loading_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            const remod::MeshModel model = mesh_loading_.get();
            if (loading_mesh_ == mesh_) {
                view_.set_model(model);
                part_materials_.clear();
                part_groups_.clear();
                hidden_groups_.clear();
                for (const auto& part : model.parts) {
                    part_materials_.push_back(part.material);
                    part_groups_.push_back(part.group);
                }
                triangles_ = model.triangles;
            }
        } catch (const std::exception& e) {
            if (loading_mesh_ == mesh_) model_error_ = e.what();
        }
        shown_mesh_ = loading_mesh_;
    }
    std::error_code ec;
    if (!mesh_loading_.valid() && !mesh_.empty() && shown_mesh_ != mesh_) {
        shown_mesh_ = loading_mesh_ = mesh_;
        // Read by the tool itself; Noesis (optional) only for what that can't read (another game's mesh version).
        const fs::path noesis = fs::is_regular_file(noesis_exe, ec) ? fs::path(noesis_exe) : fs::path();
        mesh_loading_ = std::async(std::launch::async, [noesis, file = fs::path(mesh_)] {
            try {
                return remod::read_mesh(file);
            } catch (const std::exception&) {
                if (noesis.empty()) throw;
                return remod::NoesisConverter(noesis).load_mesh(file);
            }
        });
        shown_mesh_.clear();  // set when it arrives
    }

    // One window, same place: the caller's picture (show_in_viewer), else the 3D view while a picked mesh is open
    // (closing it shows the texture again), else the texture.
    if (external_) {
        ImGui::Begin("Preview###viewer");
        external_();
        ImGui::End();
        return;
    }
    if (sounds_open_) {
        draw_sounds();
        return;
    }
    if (movie_open_) {
        ImGui::Begin("Movie###viewer", &movie_open_);
        ImGui::TextUnformatted(display_name(movie_).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", movie_.c_str());
        movie_view_.draw(movie_);
        ImGui::End();
        if (!movie_open_) movie_view_.close();
        return;
    }
    const bool in_3d = view_open_ && !mesh_.empty();
    ImGui::Begin(in_3d ? "3D view###viewer" : "Texture###viewer", in_3d ? &view_open_ : nullptr);
    if (!in_3d) {
        static const Image none;
        const Image& big = texture_.empty() ? none : image(texture_, kPreviewSide, true);
        if (texture_.empty()) ImGui::TextDisabled("Select a texture or a mesh in the Browser.");
        if (!big.error.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "Can't preview: %s", big.error.c_str());
        if (big.srv && big.width > 0 && big.height > 0) {
            if (ImGui::SmallButton("Pop out")) popped_.try_emplace(texture_);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open it in its own window, to enlarge it.");
            if (preview_of_ != texture_) {
                preview_of_ = texture_;
                preview_view_ = {};
            }
            show_zoomed(big, preview_view_);
        }
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted(display_name(mesh_).c_str());
    show_nickname(mesh_, true);
    ImGui::PushTextWrapPos(0);
    if (!mesh_error_.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "%s", mesh_error_.c_str());
    if (mesh_loading_.valid() && loading_mesh_ == mesh_)
        ImGui::TextDisabled("Reading the mesh...");
    if (!model_error_.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "No 3D view: %s", model_error_.c_str());
    ImGui::PopTextWrapPos();
    if (view_.empty() || shown_mesh_ != mesh_) {
        ImGui::End();
        return;
    }

    // Which texture to highlight: the selected one, if this mesh uses it.
    static const std::vector<std::string> no_textures;
    const auto& texs = mesh_textures_ ? mesh_textures_->textures : no_textures;
    const auto picked = std::ranges::find(texs, texture_);
    const int highlight = picked == texs.end() ? -1 : int(picked - texs.begin());
    std::vector<MeshView::Surface> surfaces;
    for (size_t i = 0; i < part_materials_.size(); ++i) {
        MeshView::Surface surface;
        const remod::MeshMaterial* mat = nullptr;
        if (mesh_textures_)
            for (const auto& m : mesh_textures_->materials)
                if (m.name == part_materials_[i]) mat = &m;
        if (mat) {
            std::copy(mat->base_color.begin(), mat->base_color.end(), surface.color);
            if (mat->albedo >= 0) {
                const Image& img = image(texs[size_t(mat->albedo)], kMeshTextureSide, true);
                surface.texture = img.srv;
                surface.u = img.u;
                surface.v = img.v;
            }
            if (mat->opacity.texture >= 0) {
                const Image& img = image(texs[size_t(mat->opacity.texture)], kMeshTextureSide, true);
                surface.opacity = img.srv;
                surface.opacity_u = img.u;
                surface.opacity_v = img.v;
                surface.opacity_channel = mat->opacity.channel;
            }
        }
        surface.dim = highlight >= 0 && !(mat && std::ranges::count(mat->textures, size_t(highlight)));
        surface.hidden = hidden_groups_.contains(part_groups_[i]) || (mat && mat->hidden);
        surfaces.push_back(surface);
    }
    // The view fills the window, above one line of groups.
    const std::set<int> groups(part_groups_.begin(), part_groups_.end());
    const float below = groups.size() > 1 ? ImGui::GetFrameHeightWithSpacing() : 0;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    view_.draw(ImVec2(avail.x, std::max(avail.y - below, ImGui::GetFontSize() * 4)), surfaces);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%zu triangles. Drag to turn, right-drag to move, wheel to zoom, double-click to reset.",
                          triangles_);
    // The mesh's groups. Which ones the game shows is chosen outside the mesh, so the user picks.
    if (groups.size() > 1) {
        ImGui::TextDisabled("Groups:");
        for (const int g : groups) {
            ImGui::SameLine();
            bool visible = !hidden_groups_.contains(g);
            ImGui::PushID(g);
            if (ImGui::Checkbox(g < 0 ? "other" : std::to_string(g).c_str(), &visible)) {
                if (visible)
                    hidden_groups_.erase(g);
                else
                    hidden_groups_.insert(g);
            }
            if (ImGui::IsItemHovered()) {
                std::string tip;
                for (size_t i = 0; i < part_materials_.size(); ++i)
                    if (part_groups_[i] == g) tip += part_materials_[i] + "\n";
                ImGui::SetTooltip("%s", tip.c_str());
            }
            ImGui::PopID();
        }
    }
    ImGui::End();
}

void Browser::update_filter() {
    const std::string key = index_ ? std::to_string(sounds_only_) + sound_language_ + "\n" + indexed_root_ + "\n" +
                                         std::to_string(index_->files.size())
                                   : std::string();
    if (key == filter_for_) return;
    filter_for_ = key;
    shown_files_.clear();
    shown_tree_ = {};
    sound_languages_.clear();
    ++version_;  // the file list again
    if (!index_) return;
    if (sounds_only_) {
        // A file's language: what follows ".x64." ("ch_x.sbnk.1.x64.en" -> "en"), "" for none.
        auto language = [](const std::string& path) {
            std::string low = path;
            std::ranges::transform(low, low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const size_t at = low.rfind(".x64.");
            return at == std::string::npos ? std::string() : low.substr(at + 5);
        };
        std::set<std::string> languages;
        for (const std::string& f : index_->assets.sounds) {
            const std::string l = language(f);
            if (!l.empty()) languages.insert(l);
            if (sound_language_.empty() || l.empty() || l == sound_language_) shown_files_.push_back(f);
        }
        sound_languages_.assign(languages.begin(), languages.end());
        shown_tree_ = remod::folder_tree(shown_files_);
    }
    // Stay on the folder being looked at (user, 2026-10-07): in the new tree, it or its nearest folder still shown.
    if (!in_index_) return;
    const remod::FolderTree& tree = game_tree();
    size_t at = 0;
    std::string rel;
    for (size_t from = 0; !folder_rel_.empty() && from <= folder_rel_.size();) {
        const size_t slash = std::min(folder_rel_.find('/', from), folder_rel_.size());
        const std::string name = folder_rel_.substr(from, slash - from);
        const auto& kids = tree.folders[at].children;
        const auto hit = std::ranges::find_if(kids, [&](size_t c) { return tree.folders[c].name == name; });
        if (hit == kids.end()) break;
        at = *hit;
        rel = folder_rel_.substr(0, slash);
        from = slash + 1;
    }
    folder_ = at;  // folder_rel_ stays: switching back finds the folder itself again
    address_ = rel.empty() ? root_.string() : abs_of(rel);
}

// ---- Sounds (CLAUDE.md §10 Game sounds): a bank's or package's sounds, played by Windows (PlaySound), saved as WAV ----

void Browser::show_sounds(const std::string& abs) {
    close_sounds();
    external_ = nullptr;
    close_movie();
    sounds_ = abs;
    sounds_open_ = true;
    sound_list_.clear();
    sounds_error_.clear();
    if (sounds_loading_.valid()) retired_lists_.push_back(std::move(sounds_loading_));
    sounds_loading_ = std::async(std::launch::async, [file = fs::path(abs)] { return remod::list_sounds(file); });
}

void Browser::close_sounds() {
    if (!playing_wav_.empty() || playing_id_) PlaySoundW(nullptr, nullptr, 0);
    playing_wav_.clear();
    playing_id_ = 0;
    sounds_open_ = false;
}

namespace {

// One sound's WAV bytes (decoded from its WEM; a bank's first part of a streamed one: its whole, from its package).
std::string sound_as_wav(const fs::path& file, std::uint32_t id) {
    const std::string wem = remod::sound_wem(file, id);
    const remod::WemInfo info = remod::read_wem_info(wem);
    return remod::wav_bytes(remod::decode_wem(wem, remod::find_codebooks()), info.channels, info.rate);
}

}  // namespace

void Browser::draw_sounds() {
    ImGui::Begin("Sounds###viewer", &sounds_open_);
    if (sounds_loading_.valid() && sounds_loading_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            sound_list_ = sounds_loading_.get();
        } catch (const std::exception& e) {
            sounds_error_ = e.what();
        }
    }
    std::erase_if(retired_lists_, [](auto& f) { return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
    if (saving_.valid() && saving_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        if (std::string why = saving_.get(); !why.empty()) sounds_error_ = why;
        saving_id_ = 0;
    }
    if (decoding_.valid() && decoding_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            playing_wav_ = decoding_.get();
            if (playing_id_)  // not stopped or closed meanwhile
                PlaySoundW(reinterpret_cast<LPCWSTR>(playing_wav_.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
        } catch (const std::exception& e) {
            sounds_error_ = e.what();
            playing_id_ = 0;
        }
    }
    ImGui::TextUnformatted(display_name(sounds_).c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", sounds_.c_str());
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("To replace a sound, drag its line onto the graph: a Game sound block, linked into Replace "
                        "sounds, where you pick your audio.");
    if (sounds_loading_.valid()) ImGui::TextDisabled("Reading...");
    if (!sounds_error_.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "%s", sounds_error_.c_str());
    ImGui::PopTextWrapPos();
    if (playing_id_ && ImGui::Button("Stop")) {
        PlaySoundW(nullptr, nullptr, 0);
        playing_id_ = 0;
    }
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV;
    if (!sound_list_.empty() && ImGui::BeginTable("sounds", 4, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Id");
        ImGui::TableSetupColumn("Sound");
        ImGui::TableSetupColumn("Where");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin(int(sound_list_.size()));
        while (clip.Step())
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
                const remod::SoundEntry& e = sound_list_[size_t(row)];
                ImGui::PushID(row);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const std::string id = std::to_string(e.id);
                ImGui::Selectable(id.c_str(), false,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
                if (ImGui::IsItemClicked()) ImGui::SetClipboardText(id.c_str());
                // Dragged onto the graph: a Game sound block holding it (or onto a Game sound block's field).
                if (e.info.channels && ImGui::BeginDragDropSource()) {
                    const std::string sound = remod::game_sound_text(sounds_, e.id);
                    ImGui::SetDragDropPayload("remod_sound", sound.c_str(), sound.size() + 1);
                    ImGui::Text("Sound %u", e.id);
                    ImGui::TextDisabled("Drop it on the graph to replace it");
                    ImGui::EndDragDropSource();
                }
                if (ImGui::IsItemHovered() && !ImGui::GetDragDropPayload())
                    ImGui::SetTooltip(e.info.channels ? "Drag onto the graph to replace this sound. Click to copy its id."
                                                      : "Click to copy its id.");
                ImGui::TableNextColumn();
                if (e.info.channels)
                    ImGui::Text("%.1f s, %u ch, %s", e.info.rate ? double(e.info.samples) / e.info.rate : 0.0,
                                e.info.channels, e.info.codec == remod::kWemOpus ? "Opus" : "Vorbis");
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", e.where.c_str());
                ImGui::TableNextColumn();
                if (e.info.channels) {
                    const bool busy = decoding_.valid() && playing_id_ == e.id;
                    if (ImGui::SmallButton(busy ? "..." : playing_id_ == e.id ? "Again" : "Play") && !decoding_.valid()) {
                        PlaySoundW(nullptr, nullptr, 0);
                        playing_id_ = e.id;
                        sounds_error_.clear();
                        decoding_ = std::async(std::launch::async,
                                               [file = fs::path(sounds_), id = e.id] { return sound_as_wav(file, id); });
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton(saving_id_ == e.id ? "Saving..." : "Save as WAV") && !saving_.valid()) {
                        nfdu8char_t* out = nullptr;
                        const nfdu8filteritem_t filter{"WAV", "wav"};
                        const std::string name = std::to_string(e.id) + ".wav";
                        if (NFD_SaveDialogU8(&out, &filter, 1, nullptr, name.c_str()) == NFD_OKAY) {
                            // Decoded and written in the background: a music track takes seconds.
                            saving_id_ = e.id;
                            sounds_error_.clear();
                            saving_ = std::async(std::launch::async, [file = fs::path(sounds_), id = e.id,
                                                                      to = fs::path(reinterpret_cast<const char8_t*>(out))] {
                                try {
                                    const std::string wav = sound_as_wav(file, id);
                                    std::ofstream w(to, std::ios::binary);
                                    w.write(wav.data(), std::streamsize(wav.size()));
                                    return w ? std::string() : "couldn't write " + to.string();
                                } catch (const std::exception& ex) {
                                    return std::string(ex.what());
                                }
                            });
                            NFD_FreePathU8(out);
                        }
                    }
                }
                ImGui::PopID();
            }
        ImGui::EndTable();
    }
    ImGui::End();
    if (!sounds_open_) close_sounds();
}
