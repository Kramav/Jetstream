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
    const bool hovered = ImGui::IsItemHovered();
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
    draw->AddText(ImVec2(at.x, at.y + size), ImGui::GetColorU32(ImGuiCol_Text), remod::file_name(rel).c_str());
    draw->PopClipRect();
    if (hovered) ImGui::SetTooltip("%s%s%s", rel.c_str(), problem.empty() ? "" : "\n", problem.c_str());
    ImGui::PopID();
    return result;
}

void Browser::draw_tree(const remod::FolderTree& tree, size_t folder) {
    for (size_t c : tree.folders[folder].children) {
        const auto& f = tree.folders[c];
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                   ImGuiTreeNodeFlags_SpanAvailWidth;
        if (f.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
        if (folder_[meshes_] == c) flags |= ImGuiTreeNodeFlags_Selected;
        const bool open = f.files.empty() ? ImGui::TreeNodeEx(reinterpret_cast<void*>(c), flags, "%s", f.name.c_str())
                                          : ImGui::TreeNodeEx(reinterpret_cast<void*>(c), flags, "%s  (%zu)",
                                                              f.name.c_str(), f.files.size());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) folder_[meshes_] = c;
        if (open) {
            draw_tree(tree, c);
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

std::string Browser::draw(const std::string& natives_root, const std::string& noesis_exe,
                          const std::vector<remod::Profile>& profiles) {
    ++frame_;
    load_ms_ = 0;
    release_unused();
    std::string chosen;
    ImGui::Begin("Browser");

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
        folder_[0] = folder_[1] = 0;
        searched_ = "\x01";
        for (auto& [_, img] : images_)
            if (img.srv) img.srv->Release();
        images_.clear();
        indexing_ = std::async(std::launch::async, [root = root_] {
            Index i;
            i.assets = remod::index_assets(root);
            i.texture_tree = remod::folder_tree(i.assets.textures);
            i.mesh_tree = remod::folder_tree(i.assets.meshes);
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
    if (!index_ || natives_root != indexed_root_) {
        ImGui::End();
        return chosen;
    }

    const auto& list = meshes_ ? index_->assets.meshes : index_->assets.textures;
    const auto& tree = meshes_ ? index_->mesh_tree : index_->texture_tree;
    if (ImGui::RadioButton("Textures", !meshes_)) meshes_ = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Meshes", meshes_)) meshes_ = true;
    ImGui::SameLine();
    ImGui::Checkbox("Transparency", &alpha_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show the alpha channel as transparency. Off by default: in many textures it holds other "
                          "data (e.g. metalness), which would hide the picture.");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", "Search, e.g. ui3200 or wood albd", &query_);
    const bool searching = query_.find_first_not_of(' ') != std::string::npos;
    if (const std::string key = query_ + (meshes_ ? "\x01m" : "\x01t"); searching && key != searched_) {
        hits_ = remod::search(list, query_);
        searched_ = key;
    }

    const float font = ImGui::GetFontSize();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float avail_h = ImGui::GetContentRegionAvail().y;
    if (!searching) {
        ImGui::BeginChild("tree", ImVec2(0, avail_h * 0.3f), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY);
        draw_tree(tree, 0);
        ImGui::EndChild();
    }

    // The folder's files or the search hits.
    const std::vector<size_t>& shown = searching ? hits_ : tree.folders[std::min(folder_[meshes_], tree.folders.size() - 1)].files;
    ImGui::BeginChild("items", ImVec2(0, avail_h * 0.35f), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY);
    if (searching) ImGui::TextDisabled("%zu found", shown.size());
    else if (shown.empty()) ImGui::TextDisabled(meshes_ ? "No meshes directly in this folder." : "No textures directly in this folder.");
    if (meshes_) {
        ImGuiListClipper clip;
        clip.Begin(int(shown.size()));
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const std::string& rel = list[shown[size_t(i)]];
                ImGui::PushID(i);
                if (ImGui::Selectable(searching ? rel.c_str() : remod::file_name(rel).c_str(), rel == mesh_)) {
                    mesh_ = rel;
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
                ImGui::PopID();
            }
    } else {
        const float tile = font * 6;
        const int columns = std::max(1, int((ImGui::GetContentRegionAvail().x + style.ItemSpacing.x) / (tile + style.ItemSpacing.x)));
        ImGuiListClipper clip;
        clip.Begin((int(shown.size()) + columns - 1) / columns, tile + ImGui::GetTextLineHeight() + style.ItemSpacing.y);
        while (clip.Step())
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row)
                for (int c = 0; c < columns && size_t(row * columns + c) < shown.size(); ++c) {
                    if (c) ImGui::SameLine();
                    const std::string& rel = list[shown[size_t(row * columns + c)]];
                    if (const int click = draw_tile(rel, true, tile, rel == texture_)) {
                        select_texture(rel, profiles);
                        if (click == 2) chosen = (root_ / rel).string();
                    }
                }
    }
    ImGui::EndChild();

    // The selected mesh's shape, converted by Noesis in the background, one at a time: a mesh picked meanwhile is
    // converted next (waiting on a running conversion would freeze the panel).
    if (mesh_loading_.valid() && mesh_loading_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            const remod::MeshModel model = mesh_loading_.get();
            if (loading_mesh_ == mesh_) {
                view_.set_model(model);
                part_materials_.clear();
                for (const auto& part : model.parts) part_materials_.push_back(part.material);
                triangles_ = model.triangles;
            }
        } catch (const std::exception& e) {
            if (loading_mesh_ == mesh_) model_error_ = e.what();
        }
        shown_mesh_ = loading_mesh_;
    }
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

    // Details: the mesh in 3D and its textures, then the selected texture.
    ImGui::BeginChild("details");
    const bool mesh_details = meshes_ && !mesh_.empty();
    if (mesh_details) {
        ImGui::TextUnformatted(remod::file_name(mesh_).c_str());
        if (!mesh_error_.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "%s", mesh_error_.c_str());
        if (mesh_loading_.valid() && loading_mesh_ == mesh_)
            ImGui::TextDisabled("Converting with Noesis... (a big mesh takes several seconds)");
        if (!model_error_.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "No 3D view: %s", model_error_.c_str());
            ImGui::PopTextWrapPos();
        }
        // Which texture to highlight: the selected one, if this mesh uses it.
        static const std::vector<std::string> no_textures;
        const auto& texs = mesh_textures_ ? mesh_textures_->textures : no_textures;
        const auto picked = std::ranges::find(texs, texture_);
        const int highlight = picked == texs.end() ? -1 : int(picked - texs.begin());
        if (!view_.empty() && shown_mesh_ == mesh_) {
            std::vector<MeshView::Surface> surfaces;
            for (const std::string& name : part_materials_) {
                MeshView::Surface surface;
                const remod::MeshMaterial* mat = nullptr;
                if (mesh_textures_)
                    for (const auto& m : mesh_textures_->materials)
                        if (m.name == name) mat = &m;
                if (mat && mat->albedo >= 0) {
                    const Image& img = image(texs[size_t(mat->albedo)], kMeshTextureSide, true);
                    surface = {img.srv, img.u, img.v, false};
                }
                surface.dim = highlight >= 0 && !(mat && std::ranges::count(mat->textures, size_t(highlight)));
                surfaces.push_back(surface);
            }
            const float w = ImGui::GetContentRegionAvail().x;
            view_.draw(ImVec2(w, std::max(w * 0.75f, font * 8)), surfaces);
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("%zu triangles. Drag to turn, right-drag to move, wheel to zoom, double-click to reset.",
                                triangles_);
            ImGui::PopTextWrapPos();
        }
        if (mesh_textures_) {
            ImGui::TextDisabled("Material: %s", mesh_textures_->material.c_str());
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("%zu texture%s. Click one to highlight the parts using it, again to show all.",
                                texs.size(), texs.size() == 1 ? "" : "s");
            ImGui::PopTextWrapPos();
            const float tile = font * 5;
            const size_t columns = std::max(1, int((ImGui::GetContentRegionAvail().x + style.ItemSpacing.x) / (tile + style.ItemSpacing.x)));
            for (size_t i = 0; i < texs.size(); ++i) {
                const std::string rel = texs[i];
                if (i % columns) ImGui::SameLine();
                if (const int click = draw_tile(rel, mesh_textures_->found[i], tile, rel == texture_);
                    click && mesh_textures_->found[i]) {
                    if (click == 1 && rel == texture_)
                        texture_.clear();
                    else
                        select_texture(rel, profiles);
                    if (click == 2) chosen = (root_ / rel).string();
                }
            }
        }
        ImGui::Separator();
    }
    if (!texture_.empty()) {
        ImGui::TextUnformatted(remod::file_name(texture_).c_str());
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", info_.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Use in graph")) chosen = (root_ / texture_).string();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Puts this texture into the selected Original texture block, or the graph's only one.\n"
                              "In Build layout, adds a block if there's none. Double-clicking a texture does the same.");
        static const Image none;
        const Image& big = mesh_details ? none : image(texture_, kPreviewSide, true);  // a mesh shows the 3D view
        if (!big.error.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "Can't preview: %s", big.error.c_str());
        if (big.srv) {
            const float w = ImGui::GetContentRegionAvail().x;
            const float h = std::min(w * big.height / big.width, big.height * 2);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(w, h));
            put_image(big, at, ImVec2(w, h));
        }
    }
    ImGui::EndChild();
    ImGui::End();
    return chosen;
}
