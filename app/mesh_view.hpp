#pragma once
// The Browser's 3D view: draws a MeshModel into an offscreen D3D11 texture shown as an ImGui image, with an orbit
// camera. Only drawing: which texture goes on which part is decided by the Browser from core's materials.
#include "mesh.hpp"

#include <d3d11.h>
#include <imgui.h>

#include <vector>

class MeshView {
public:
    explicit MeshView(ID3D11Device* device);
    ~MeshView();
    MeshView(const MeshView&) = delete;
    MeshView& operator=(const MeshView&) = delete;

    // How to draw one part (same order as the model's parts).
    struct Surface {
        ID3D11ShaderResourceView* texture = nullptr;  // colour texture, or none (grey)
        float u = 1, v = 1;                           // visible part of the stored texture
        bool dim = false;                             // not highlighted
    };

    void set_model(const remod::MeshModel& model);  // uploads it and frames the camera
    void clear();
    bool empty() const { return parts_.empty(); }
    // Draws the view as an ImGui item of `size`, handling the mouse over it.
    void draw(ImVec2 size, const std::vector<Surface>& surfaces);

private:
    void resize(int w, int h);
    void reset_camera();

    struct Part {
        ID3D11Buffer* vertices = nullptr;
        unsigned count = 0;
    };
    ID3D11Device* device_;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11InputLayout* layout_ = nullptr;
    ID3D11Buffer* constants_ = nullptr;
    ID3D11RasterizerState* raster_ = nullptr;
    ID3D11DepthStencilState* depth_state_ = nullptr;
    ID3D11SamplerState* sampler_ = nullptr;
    ID3D11Texture2D* color_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    ID3D11ShaderResourceView* srv_ = nullptr;
    ID3D11DepthStencilView* dsv_ = nullptr;
    int width_ = 0, height_ = 0;
    std::vector<Part> parts_;
    float center_[3] = {}, radius_ = 1;
    float yaw_ = 0.6f, pitch_ = 0.25f, distance_ = 3, pan_[3] = {};
    const char* error_ = nullptr;  // shader setup failed
};
