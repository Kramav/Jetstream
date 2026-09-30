// remod-app: thin ImGui front end over core/. M1 scaffold: docked, empty node editor canvas.
// Win32 + DX11 setup follows imgui/examples/example_win32_directx11 (v1.92.9-docking).
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <imgui-node-editor/imgui_node_editor.h>

#include <d3d11.h>

namespace ed = ax::NodeEditor;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap_chain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
bool g_occluded = false;
UINT g_resize_w = 0, g_resize_h = 0;

void create_render_target() {
    ID3D11Texture2D* back = nullptr;
    g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back));
    g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
}

void cleanup_render_target() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate = {60, 1};
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                               &sd, &g_swap_chain, &g_device, &level, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)  // no GPU: WARP software driver
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                           &g_swap_chain, &g_device, &level, &g_context);
    if (hr != S_OK) return false;
    create_render_target();
    return true;
}

void cleanup_device() {
    cleanup_render_target();
    if (g_swap_chain) { g_swap_chain->Release(); g_swap_chain = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) return 0;
        g_resize_w = LOWORD(lp);  // resize is applied in the main loop
        g_resize_h = HIWORD(lp);
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;  // no ALT menu
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, wnd_proc, 0, 0, instance, nullptr, nullptr, nullptr, nullptr, L"remod", nullptr};
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"remod", WS_OVERLAPPEDWINDOW, 100, 100, int(1280 * scale),
                                int(800 * scale), nullptr, nullptr, instance, nullptr);
    if (!create_device(hwnd)) {
        cleanup_device();
        ::UnregisterClassW(wc.lpszClassName, instance);
        return 1;
    }
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr;  // ponytail: no layout persistence yet (keeps disk writes at zero)
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleDpi = scale;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    ed::Config config;
    config.SettingsFile = nullptr;  // don't write NodeEditor.json
    ed::EditorContext* editor = ed::CreateEditor(&config);

    for (bool done = false; !done;) {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_occluded && g_swap_chain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            ::Sleep(10);
            continue;
        }
        g_occluded = false;

        if (g_resize_w != 0 && g_resize_h != 0) {
            cleanup_render_target();
            g_swap_chain->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_w = g_resize_h = 0;
            create_render_target();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::DockSpaceOverViewport();
        ImGui::Begin("Graph");
        ed::SetCurrentEditor(editor);
        ed::Begin("canvas");
        ed::End();
        ed::SetCurrentEditor(nullptr);
        ImGui::End();

        ImGui::Render();
        const float clear[4] = {0.1f, 0.1f, 0.1f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_occluded = g_swap_chain->Present(1, 0) == DXGI_STATUS_OCCLUDED;
    }

    ed::DestroyEditor(editor);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanup_device();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, instance);
    return 0;
}
