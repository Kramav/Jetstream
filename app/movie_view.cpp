#include "movie_view.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

MovieView::~MovieView() { close(); }

void MovieView::close() {
    player_.reset();
    file_.clear();
    if (srv_) srv_->Release();
    if (texture_) texture_->Release();
    srv_ = nullptr;
    texture_ = nullptr;
    width_ = height_ = 0;
}

void MovieView::upload(const remod::MoviePlayer::Frame& frame) {
    if (frame.width != width_ || frame.height != height_) {
        if (srv_) srv_->Release();
        if (texture_) texture_->Release();
        srv_ = nullptr;
        texture_ = nullptr;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = frame.width;
        desc.Height = frame.height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &texture_))) return;
        device_->CreateShaderResourceView(texture_, nullptr, &srv_);
        width_ = frame.width;
        height_ = frame.height;
    }
    ID3D11DeviceContext* context = nullptr;
    device_->GetImmediateContext(&context);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(context->Map(texture_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        for (unsigned y = 0; y < frame.height; ++y)
            std::memcpy(static_cast<char*>(mapped.pData) + size_t(y) * mapped.RowPitch,
                        frame.bgra.data() + size_t(y) * frame.width * 4, size_t(frame.width) * 4);
        context->Unmap(texture_, 0);
    }
    context->Release();
}

void MovieView::draw(const std::string& file) {
    if (file != file_) {
        close();
        file_ = file;
        player_ = std::make_unique<remod::MoviePlayer>(file);
        view_ = {};
    }
    if (const std::string error = player_->error(); !error.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "Can't play: %s", error.c_str());
        ImGui::PopTextWrapPos();
        return;
    }
    if (player_->take(frame_)) upload(frame_);
    const remod::MovieInfo info = player_->info();
    const bool playing = player_->playing();

    if (ImGui::Button(playing ? "Pause" : "Play", ImVec2(ImGui::GetFontSize() * 4, 0))) {
        if (playing) player_->pause();
        else player_->play();
    }
    ImGui::SameLine();
    const float length = float((std::max)(info.seconds, 0.001));  // (): windows.h's max macro
    float at = scrubbing_ ? scrub_ : float(player_->position());
    char shown[64];
    std::snprintf(shown, sizeof shown, "%.1f / %.1f s", at, info.seconds);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##time", &at, 0, length, shown, ImGuiSliderFlags_NoInput)) {
        scrub_ = at;
        player_->seek(at);
    }
    scrubbing_ = ImGui::IsItemActive();
    if (info.width)
        ImGui::TextDisabled("%ux%u, %.3g fps%s", info.width, info.height, info.fps,
                            width_ && width_ < info.width ? " (shown smaller)" : "");
    if (!srv_) {
        ImGui::TextDisabled("Opening...");
        return;
    }
    const ZoomPlace place = zoom_area("##movie", float(width_), float(height_), view_);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(place.area_min, place.area_max, true);
    draw->AddImage(ImTextureRef(srv_), place.corner,
                   ImVec2(place.corner.x + place.size.x, place.corner.y + place.size.y));
    draw->PopClipRect();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%.0f%%. Wheel to zoom, drag to move, double-click to fit.", place.scale * 100);
}
