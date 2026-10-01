#define NOMINMAX  // std::min/max, not windows.h's macros
#include "mesh_view.hpp"

#include <d3dcompiler.h>
#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace DirectX;

namespace {

// Colour texture (or grey) with a light at the camera; both faces lit, since meshes are drawn without culling.
// The sRGB texture bytes are used as they are, like the 2D previews.
constexpr char kShader[] = R"(
cbuffer Constants : register(b0) { float4x4 view_proj; float4 tint; float4 uv_scale; float4 eye; };
Texture2D colour : register(t0);
SamplerState linear_clamp : register(s0);
struct In { float3 pos : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct Out { float4 pos : SV_POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; float3 world : TEXCOORD1; };
Out vs(In i) {
    Out o;
    o.pos = mul(float4(i.pos, 1), view_proj);
    o.normal = i.normal;
    o.uv = i.uv;
    o.world = i.pos;
    return o;
}
float4 ps(Out i) : SV_Target {
    // frac then scale: tiling UVs still land inside the texture's visible part when its rows are padded.
    float3 base = tint.w > 0 ? colour.Sample(linear_clamp, frac(i.uv) * uv_scale.xy).rgb : float3(0.7, 0.7, 0.7);
    float light = 0.35 + 0.65 * abs(dot(normalize(i.normal), normalize(eye.xyz - i.world)));
    return float4(base * light * tint.rgb, 1);
}
)";

struct Constants {
    XMFLOAT4X4 view_proj;
    float tint[4];
    float uv_scale[4];
    float eye[4];
};

template <class T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

ID3DBlob* compile(const char* entry, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    D3DCompile(kShader, sizeof(kShader) - 1, "mesh_view", nullptr, nullptr, entry, target, 0, 0, &code, &errors);
    release(errors);
    return code;
}

}  // namespace

MeshView::MeshView(ID3D11Device* device) : device_(device) {
    device_->GetImmediateContext(&context_);
    ID3DBlob* vs = compile("vs", "vs_4_0");
    ID3DBlob* ps = compile("ps", "ps_4_0");
    if (!vs || !ps) {
        error_ = "Couldn't set up the 3D view (shader compile failed).";
    } else {
        device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_);
        device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ps_);
        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        device_->CreateInputLayout(elements, 3, vs->GetBufferPointer(), vs->GetBufferSize(), &layout_);
    }
    release(vs);
    release(ps);

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(Constants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device_->CreateBuffer(&cb, nullptr, &constants_);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    device_->CreateRasterizerState(&rd, &raster_);

    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS;
    device_->CreateDepthStencilState(&dd, &depth_state_);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    device_->CreateSamplerState(&sd, &sampler_);
}

MeshView::~MeshView() {
    clear();
    release(color_);
    release(rtv_);
    release(srv_);
    release(dsv_);
    release(sampler_);
    release(depth_state_);
    release(raster_);
    release(constants_);
    release(layout_);
    release(ps_);
    release(vs_);
    release(context_);
}

void MeshView::clear() {
    for (Part& p : parts_) release(p.vertices);
    parts_.clear();
}

void MeshView::set_model(const remod::MeshModel& model) {
    clear();
    for (const remod::MeshPart& part : model.parts) {
        Part p;
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = UINT(part.vertices.size() * sizeof(float));
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        const D3D11_SUBRESOURCE_DATA data{part.vertices.data(), 0, 0};
        device_->CreateBuffer(&desc, &data, &p.vertices);
        p.count = UINT(part.vertices.size() / remod::MeshPart::kStride);
        parts_.push_back(p);  // also when creation failed, so parts stay in step with surfaces
    }
    float extent = 0;
    for (int a = 0; a < 3; ++a) {
        center_[a] = (model.min[a] + model.max[a]) / 2;
        extent += (model.max[a] - model.min[a]) * (model.max[a] - model.min[a]);
    }
    radius_ = std::max(std::sqrt(extent) / 2, 1e-3f);
    reset_camera();
}

void MeshView::reset_camera() {
    yaw_ = 0.6f;
    pitch_ = 0.25f;
    distance_ = radius_ * 2.6f;  // the whole model in a 45-degree view
    pan_[0] = pan_[1] = pan_[2] = 0;
}

void MeshView::resize(int w, int h) {
    release(color_);
    release(rtv_);
    release(srv_);
    release(dsv_);
    width_ = w;
    height_ = h;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = UINT(w);
    desc.Height = UINT(h);
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &color_))) return;
    device_->CreateRenderTargetView(color_, nullptr, &rtv_);
    device_->CreateShaderResourceView(color_, nullptr, &srv_);
    desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D* depth = nullptr;
    if (SUCCEEDED(device_->CreateTexture2D(&desc, nullptr, &depth))) {
        device_->CreateDepthStencilView(depth, nullptr, &dsv_);
        depth->Release();
    }
}

void MeshView::draw(ImVec2 size, const std::vector<Surface>& surfaces) {
    if (error_) {
        ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "%s", error_);
        return;
    }
    const int w = std::max(1, int(size.x)), h = std::max(1, int(size.y));
    if (w != width_ || h != height_) resize(w, h);

    // Mouse: drag turns, right-drag moves, wheel zooms, double-click resets.
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##mesh_view", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);  // the wheel zooms here instead of scrolling the panel
    const ImGuiIO& io = ImGui::GetIO();
    const XMVECTOR forward = XMVectorSet(std::cos(pitch_) * std::sin(yaw_), std::sin(pitch_),
                                         std::cos(pitch_) * std::cos(yaw_), 0);
    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(XMVectorSet(0, 1, 0, 0), forward));
    const XMVECTOR up = XMVector3Cross(forward, right);
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0)) {
        yaw_ -= io.MouseDelta.x * 0.01f;
        pitch_ = std::clamp(pitch_ + io.MouseDelta.y * 0.01f, -1.5f, 1.5f);
    }
    if (ImGui::IsItemActive() &&
        (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0))) {
        const float step = distance_ / float(h);
        XMFLOAT3 move;
        XMStoreFloat3(&move, (up * io.MouseDelta.y - right * io.MouseDelta.x) * step);  // the model follows the mouse
        pan_[0] += move.x;
        pan_[1] += move.y;
        pan_[2] += move.z;
    }
    if (ImGui::IsItemHovered()) {
        if (io.MouseWheel != 0) distance_ = std::clamp(distance_ * std::pow(0.85f, io.MouseWheel), radius_ * 0.05f, radius_ * 50);
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) reset_camera();
    }
    if (!rtv_) return;

    const XMVECTOR target = XMVectorSet(center_[0] + pan_[0], center_[1] + pan_[1], center_[2] + pan_[2], 1);
    const XMVECTOR eye = target + forward * distance_;
    const XMMATRIX view = XMMatrixLookAtRH(eye, target, XMVectorSet(0, 1, 0, 0));
    const XMMATRIX proj = XMMatrixPerspectiveFovRH(XMConvertToRadians(45), float(w) / float(h),
                                                   distance_ * 0.01f, distance_ + radius_ * 4);
    Constants c{};
    XMStoreFloat4x4(&c.view_proj, XMMatrixTranspose(view * proj));
    XMFLOAT3 eye3;
    XMStoreFloat3(&eye3, eye);
    c.eye[0] = eye3.x;
    c.eye[1] = eye3.y;
    c.eye[2] = eye3.z;

    // Drawn now, into the view's own texture; ImGui draws that texture with the rest of the frame.
    const float background[4] = {0.13f, 0.14f, 0.16f, 1};
    context_->OMSetRenderTargets(1, &rtv_, dsv_);
    context_->ClearRenderTargetView(rtv_, background);
    if (dsv_) context_->ClearDepthStencilView(dsv_, D3D11_CLEAR_DEPTH, 1.0f, 0);
    const D3D11_VIEWPORT viewport{0, 0, float(w), float(h), 0, 1};
    context_->RSSetViewports(1, &viewport);
    context_->RSSetState(raster_);
    context_->OMSetDepthStencilState(depth_state_, 0);
    context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    context_->IASetInputLayout(layout_);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vs_, nullptr, 0);
    context_->PSSetShader(ps_, nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, &constants_);
    context_->PSSetConstantBuffers(0, 1, &constants_);
    context_->PSSetSamplers(0, 1, &sampler_);
    for (size_t i = 0; i < parts_.size(); ++i) {
        if (!parts_[i].vertices) continue;
        const Surface s = i < surfaces.size() ? surfaces[i] : Surface{};
        const float shade = s.dim ? 0.25f : 1.0f;
        c.tint[0] = c.tint[1] = c.tint[2] = shade;
        c.tint[3] = s.texture ? 1.0f : 0.0f;
        c.uv_scale[0] = s.u;
        c.uv_scale[1] = s.v;
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context_->Map(constants_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) continue;
        std::memcpy(mapped.pData, &c, sizeof(c));
        context_->Unmap(constants_, 0);
        context_->PSSetShaderResources(0, 1, &s.texture);
        const UINT stride = UINT(remod::MeshPart::kStride * sizeof(float)), offset = 0;
        context_->IASetVertexBuffers(0, 1, &parts_[i].vertices, &stride, &offset);
        context_->Draw(parts_[i].count, 0);
    }
    ID3D11ShaderResourceView* none = nullptr;
    context_->PSSetShaderResources(0, 1, &none);
    context_->OMSetRenderTargets(0, nullptr, nullptr);

    ImGui::GetWindowDrawList()->AddImage(ImTextureRef(srv_), at, ImVec2(at.x + size.x, at.y + size.y));
}
