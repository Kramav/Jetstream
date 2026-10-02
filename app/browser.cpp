#define NOMINMAX  // std::min/max, not windows.h's macros
#include "browser.hpp"

#include "texture_converter.hpp"

#include <imgui_impl_dx11.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>

namespace fs = std::filesystem;

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

}  // namespace

Browser::Browser(ID3D11Device* device) : device_(device), view_(device) {
    D3D11_BLEND_DESC desc{};
    desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device_->CreateBlendState(&desc, &opaque_);
}

Browser::~Browser() {
    for (auto& [_, img] : images_)
        if (img.srv) img.srv->Release();
    if (opaque_) opaque_->Release();
}

const Browser::Image& Browser::image(const std::string& rel, unsigned max_side, bool force) {
    static const Image loading;  // not loaded yet: over this frame's budget
    const std::string key = std::to_string(max_side) + "|" + rel;
    auto it = images_.find(key);
    if (it == images_.end()) {
        // ponytail: loads on the UI thread, ~0.5 ms per thumbnail, capped per frame; a worker thread if a slow
        // disk makes scrolling stutter.
        if (!force && load_ms_ > 8) return loading;
        const auto start = std::chrono::steady_clock::now();
        Image img;
        try {
            const fs::path file = max_side > kThumbSide ? remod::preview_file(root_, rel) : root_ / rel;
            const remod::TexPixels p = remod::read_tex_pixels(file, max_side);
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = p.stored_width;
            desc.Height = p.stored_height;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = display_format(p.format);
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            const D3D11_SUBRESOURCE_DATA data{p.data.data(), p.row_pitch, 0};
            ID3D11Texture2D* tex = nullptr;
            if (FAILED(device_->CreateTexture2D(&desc, &data, &tex)))
                throw std::runtime_error("the graphics card can't show this format");
            device_->CreateShaderResourceView(tex, nullptr, &img.srv);
            tex->Release();
            img.width = float(p.width);
            img.height = float(p.height);
            img.u = float(p.width) / float(p.stored_width);
            img.v = float(p.height) / float(p.stored_height);
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

int Browser::draw_tile(const std::string& rel, bool found, float size, bool selected) {
    ImGui::PushID(rel.c_str());
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float line = ImGui::GetTextLineHeight();
    int result = 0;
    if (ImGui::Selectable("##tile", selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(size, size + line)))
        result = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ? 2 : 1;
    const bool hovered = ImGui::IsItemHovered() && !ImGui::GetDragDropPayload();
    if (found) drag_source(rel, false);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    std::string problem = found ? "" : "Not in the extracted files.";
    if (found) {
        const Image& img = image(rel, kThumbSide);
        if (img.srv)
            put_image(img, at, ImVec2(size, size));
        else
            draw->AddText(ImVec2(at.x + 4, at.y + 4), dim, img.error.empty() ? "..." : "?");
        problem = img.error;
    } else {
        draw->AddText(ImVec2(at.x + 4, at.y + 4), dim, "missing");
    }
    draw->PushClipRect(ImVec2(at.x, at.y + size), ImVec2(at.x + size, at.y + size + line), true);
    const std::string nick = remod::nickname(names_, rel);  // the caption: its nickname, else its file name
    draw->AddText(ImVec2(at.x, at.y + size),
                  ImGui::GetColorU32(nick.empty() ? ImGuiCol_Text : ImGuiCol_CheckMark),
                  nick.empty() ? remod::file_name(rel).c_str() : nick.c_str());
    draw->PopClipRect();
    if (hovered)
        ImGui::SetTooltip("%s%s%s%s%s", nick.empty() ? "" : (nick + "\n").c_str(), rel.c_str(), problem.empty() ? "" : "\n",
                          problem.c_str(), "\nRight-click to name it.");
    name_menu(rel);
    ImGui::PopID();
    return result;
}

// Right-click the last item (a folder, file or thumbnail): give it a nickname, saved at once. Empty removes it.
void Browser::name_menu(const std::string& rel) {
    if (!ImGui::BeginPopupContextItem()) return;
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
            searched_ = "\x01";  // search again: it matches nicknames
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::TextDisabled("Only a label here: mods use the real name. Empty removes it.");
    }
    ImGui::EndPopup();
}

// The last item can be dragged onto a block's field in the graph, as its absolute path ("remod_path").
void Browser::drag_source(const std::string& rel, bool folder) {
    if (!ImGui::BeginDragDropSource()) return;
    const std::string path = (root_ / std::filesystem::path(rel)).make_preferred().string();
    ImGui::SetDragDropPayload("remod_path", path.c_str(), path.size() + 1);
    const std::string nick = remod::nickname(names_, rel);
    ImGui::Text("%s%s", nick.empty() ? remod::file_name(rel).c_str() : nick.c_str(), folder ? " (folder)" : "");
    ImGui::TextDisabled("Drop it on a field in a block.");
    ImGui::EndDragDropSource();
}

// The item's nickname beside it; with `above` and none of its own, its nearest named folder's, dimmed.
void Browser::show_nickname(const std::string& rel, bool above) {
    std::string nick = remod::nickname(names_, rel);
    const bool own = !nick.empty();
    if (!own && above) nick = remod::nickname_above(names_, rel);
    if (nick.empty()) return;
    ImGui::SameLine();
    if (own) ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "%s", nick.c_str());
    else ImGui::TextDisabled("(%s)", nick.c_str());
}

void Browser::draw_tree(const remod::FolderTree& tree, size_t folder, const std::string& path) {
    for (size_t c : tree.folders[folder].children) {
        const auto& f = tree.folders[c];
        const std::string child = path.empty() ? f.name : path + "/" + f.name;
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                   ImGuiTreeNodeFlags_SpanAvailWidth;
        if (f.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
        if (folder_ == c) flags |= ImGuiTreeNodeFlags_Selected;
        const bool open = f.files.empty() ? ImGui::TreeNodeEx(reinterpret_cast<void*>(c), flags, "%s", f.name.c_str())
                                          : ImGui::TreeNodeEx(reinterpret_cast<void*>(c), flags, "%s  (%zu)",
                                                              f.name.c_str(), f.files.size());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            folder_ = c;
            mesh_focus_ = false;
        }
        drag_source(child, true);
        name_menu(child);
        show_nickname(child);
        if (open) {
            draw_tree(tree, c, child);
            ImGui::TreePop();
        }
    }
}

void Browser::select_texture(const std::string& rel, const std::vector<remod::Profile>& profiles) {
    texture_ = rel;
    const fs::path file = root_ / rel;
    try {
        if (const remod::Profile* p = remod::profile_for_texture(file, profiles)) {
            const remod::TexMeta m = remod::read_tex_meta(file, *p);
            info_ = std::to_string(m.width) + "x" + std::to_string(m.height) + " " + m.format + ", " +
                    std::to_string(m.mip_count) + " mip" + (m.mip_count == 1 ? "" : "s") + " (" + p->name + ")";
        } else {
            info_ = "No game profile uses this texture's version yet.";
        }
        if (remod::preview_file(root_, rel) != file) {
            const remod::TexPixels big = remod::read_tex_pixels(remod::preview_file(root_, rel), 1u << 16);
            info_ += "\nThe game also has a " + std::to_string(big.width) + "x" + std::to_string(big.height) +
                     " copy in streaming/ (shown here). A mod replaces only the texture above.";
        }
    } catch (const std::exception& e) {
        info_ = e.what();
    }
}

void Browser::select_mesh(const std::string& rel) {
    mesh_focus_ = view_open_ = true;
    if (rel == mesh_) return;  // picked again: shows it again
    mesh_ = rel;
    texture_.clear();  // a texture picked elsewhere would dim the parts not using it
    mesh_error_.clear();
    mesh_textures_.reset();
    model_error_.clear();
    view_.clear();  // until the new one is converted
    try {
        mesh_textures_ = remod::mesh_textures(root_, rel, index_->assets.textures);
    } catch (const std::exception& e) {
        mesh_error_ = e.what();
    }
}

std::string Browser::draw(const std::string& natives_root, const std::string& noesis_exe,
                          const std::vector<remod::Profile>& profiles, const std::string& game, bool browser_only) {
    ++frame_;
    load_ms_ = 0;
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
        searched_ = "\x01";
    }
    if (!names_error_.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "%s", names_error_.c_str());

    // Index the folder once, in the background (a few seconds for RE4R's 26k textures and meshes).
    std::error_code ec;
    const bool usable = !natives_root.empty() && fs::is_directory(natives_root, ec);
    bool start = usable && natives_root != indexed_root_ && looks_like_natives(natives_root);
    if (!usable) {
        ImGui::TextWrapped("Set the Game files folder (your REtool folder, e.g. ...\\re_chunk_000\\natives\\stm) in "
                           "the Pipeline panel to browse its textures and meshes.");
    } else if (natives_root != indexed_root_ && !start) {
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
        shown_.clear();
        folder_ = 0;
        searched_ = "\x01";
        for (auto& [_, img] : images_)
            if (img.srv) img.srv->Release();
        images_.clear();
        indexing_ = std::async(std::launch::async, [root = root_] {
            Index i;
            i.assets = remod::index_assets(root);
            i.files = i.assets.meshes;
            i.files.insert(i.files.end(), i.assets.textures.begin(), i.assets.textures.end());
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
    }
    if (indexing_.valid()) ImGui::TextDisabled("Reading %s...", root_.string().c_str());
    if (!error_.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "%s", error_.c_str());
    const bool ready = index_ && natives_root == indexed_root_;
    if (ready) draw_files(profiles, chosen);
    ImGui::End();

    if (browser_only) return chosen;
    draw_textures(ready, profiles, chosen);
    draw_viewer(noesis_exe);
    return chosen;
}

void Browser::draw_files(const std::vector<remod::Profile>& profiles, std::string& chosen) {
    const auto& list = index_->files;
    const auto& tree = index_->tree;
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", "Search, e.g. ui3200 or wood albd", &query_);
    const bool searching = query_.find_first_not_of(' ') != std::string::npos;
    if (searching && query_ != searched_) {
        hits_ = remod::search(list, query_, &names_);
        searched_ = query_;
        mesh_focus_ = false;
    }
    if (!searching) {
        ImGui::BeginChild("tree", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.45f),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY);
        draw_tree(tree, 0, "");
        ImGui::EndChild();
    }

    // The folder's files or the search hits, meshes first: paths only, the Textures window shows them.
    shown_ = searching ? hits_ : tree.folders[std::min(folder_, tree.folders.size() - 1)].files;
    ImGui::BeginChild("files", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (searching) ImGui::TextDisabled("%zu found", shown_.size());
    else if (shown_.empty()) ImGui::TextDisabled("Nothing directly in this folder.");
    ImGuiListClipper clip;
    clip.Begin(int(shown_.size()));
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
            const size_t file = shown_[size_t(i)];
            const std::string& rel = list[file];
            const bool mesh = file < index_->assets.meshes.size();
            ImGui::PushID(i);
            if (ImGui::Selectable(searching ? rel.c_str() : remod::file_name(rel).c_str(),
                                  rel == (mesh ? mesh_ : texture_), ImGuiSelectableFlags_AllowDoubleClick)) {
                if (mesh) {
                    select_mesh(rel);
                } else {
                    mesh_focus_ = false;  // the 3D view keeps the last mesh
                    select_texture(rel, profiles);
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) chosen = (root_ / rel).string();
                }
            }
            if (ImGui::IsItemHovered() && !ImGui::GetDragDropPayload())
                ImGui::SetTooltip("%s\nRight-click to name it, drag it onto a field in a block.", rel.c_str());
            drag_source(rel, false);
            name_menu(rel);
            show_nickname(rel, searching);  // a search lists files from many folders: say whose they are
            ImGui::PopID();
        }
    ImGui::EndChild();
}

void Browser::draw_textures(bool ready, const std::vector<remod::Profile>& profiles, std::string& chosen) {
    ImGui::Begin("Textures");
    if (!ready) {
        ImGui::TextDisabled("The textures of the folder or mesh selected in the Browser show here.");
        ImGui::End();
        return;
    }
    // The selected mesh's textures, else the textures among the Browser's files.
    const bool of_mesh = mesh_focus_ && !mesh_.empty() && mesh_textures_;
    std::vector<std::pair<std::string, bool>> texs;  // (path, in the files)
    if (of_mesh) {
        for (size_t i = 0; i < mesh_textures_->textures.size(); ++i)
            texs.emplace_back(mesh_textures_->textures[i], mesh_textures_->found[i]);
    } else {
        for (const size_t file : shown_)
            if (file >= index_->assets.meshes.size()) texs.emplace_back(index_->files[file], true);
    }

    if (!texture_.empty()) {
        if (ImGui::Button("Use in graph")) chosen = (root_ / texture_).string();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Puts this texture into the selected Original texture block, or the graph's only one.\n"
                              "In Build layout, adds a block if there's none. Double-clicking a texture does the same.");
        ImGui::SameLine();
        ImGui::Checkbox("Transparency", &alpha_);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Show the alpha channel as transparency. Off by default: in many textures it holds other "
                              "data (e.g. metalness), which would hide the picture.");
        ImGui::SameLine();
        ImGui::TextUnformatted(remod::file_name(texture_).c_str());
        show_nickname(texture_, true);
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", info_.c_str());
        ImGui::PopTextWrapPos();
    }
    if (of_mesh) {
        ImGui::TextDisabled("%s: %zu texture%s. Click one to highlight the parts using it, again to show all.",
                            mesh_textures_->material.c_str(), texs.size(), texs.size() == 1 ? "" : "s");
    } else if (texs.empty()) {
        ImGui::TextDisabled("No textures here.");
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
                const auto& [rel, found] = texs[size_t(row * columns + c)];
                if (const int click = draw_tile(rel, found, tile, rel == texture_); click && found) {
                    if (of_mesh && click == 1 && rel == texture_)
                        texture_.clear();  // shows the whole mesh again
                    else
                        select_texture(rel, profiles);
                    if (click == 2) chosen = (root_ / rel).string();
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
        if (!fs::is_regular_file(noesis_exe, ec)) {
            model_error_ = "Set Noesis64.exe in the Pipeline panel to see meshes in 3D.";
        } else {
            mesh_loading_ = std::async(std::launch::async, [exe = fs::path(noesis_exe), file = root_ / mesh_] {
                return remod::NoesisConverter(exe).load_mesh(file);
            });
            shown_mesh_.clear();  // set when it arrives
        }
    }

    // One window, same place: the 3D view while a picked mesh is open (closing it shows the texture again).
    const bool in_3d = view_open_ && !mesh_.empty();
    ImGui::Begin(in_3d ? "3D view###viewer" : "Texture###viewer", in_3d ? &view_open_ : nullptr);
    if (!in_3d) {
        static const Image none;
        const Image& big = texture_.empty() ? none : image(texture_, kPreviewSide, true);
        if (texture_.empty()) ImGui::TextDisabled("Select a texture or a mesh in the Browser.");
        if (!big.error.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "Can't preview: %s", big.error.c_str());
        if (big.srv && big.width > 0 && big.height > 0) {
            // Zoom and pan: the wheel zooms about the mouse, any button drags, double-click fits it again.
            const ImVec2 box(std::max(ImGui::GetContentRegionAvail().x, 1.0f), std::max(ImGui::GetContentRegionAvail().y, 1.0f));
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##preview", box,
                                   ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                       ImGuiButtonFlags_MouseButtonMiddle);
            ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);  // the wheel zooms instead of scrolling
            if (preview_of_ != texture_ || (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
                preview_of_ = texture_;
                preview_zoom_ = 1;
                preview_pan_ = ImVec2(0, 0);
            }
            const ImGuiIO& io = ImGui::GetIO();
            const float fit = std::min(box.x / big.width, box.y / big.height);
            const ImVec2 middle(at.x + box.x * 0.5f, at.y + box.y * 0.5f);
            if (ImGui::IsItemHovered() && io.MouseWheel != 0) {
                const float before = preview_zoom_;  // up to 32 screen pixels per texel
                preview_zoom_ = std::clamp(preview_zoom_ * std::pow(1.25f, io.MouseWheel), 1.0f, std::max(1.0f, 32 / fit));
                const float k = 1 - preview_zoom_ / before;  // keeps the texel under the mouse where it is
                preview_pan_.x += (io.MousePos.x - middle.x - preview_pan_.x) * k;
                preview_pan_.y += (io.MousePos.y - middle.y - preview_pan_.y) * k;
            }
            if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0) ||
                                          ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0) ||
                                          ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0))) {
                preview_pan_.x += io.MouseDelta.x;
                preview_pan_.y += io.MouseDelta.y;
            }
            const float scale = fit * preview_zoom_;
            const ImVec2 size(big.width * scale, big.height * scale);
            const ImVec2 corner(middle.x + preview_pan_.x - size.x * 0.5f, middle.y + preview_pan_.y - size.y * 0.5f);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->PushClipRect(at, ImVec2(at.x + box.x, at.y + box.y), true);
            put_image(big, corner, size);
            draw->PopClipRect();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%.0f%%. Wheel to zoom, drag to move, double-click to fit.", scale * 100);
        }
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted(remod::file_name(mesh_).c_str());
    show_nickname(mesh_, true);
    ImGui::PushTextWrapPos(0);
    if (!mesh_error_.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "%s", mesh_error_.c_str());
    if (mesh_loading_.valid() && loading_mesh_ == mesh_)
        ImGui::TextDisabled("Converting with Noesis... (a big mesh takes several seconds)");
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
