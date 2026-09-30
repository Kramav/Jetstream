// remod-app: thin ImGui front end over core/. Draws the graph and forwards edits and runs to core;
// all graph rules (valid links, validation, running) live in core/graph.
// Win32 + DX11 setup follows imgui/examples/example_win32_directx11 (v1.92.9-docking).
#include "graph.hpp"
#include "profile.hpp"
#include "texture_converter.hpp"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <imgui_stdlib.h>
#include <imgui-node-editor/imgui_node_editor.h>

#include <d3d11.h>

#include <chrono>
#include <cstdlib>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ed = ax::NodeEditor;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

// ---- D3D11 boilerplate -------------------------------------------------------------------------

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

// ---- Graph editor UI ---------------------------------------------------------------------------

// Editor ids. Pin id = node id * 64 + slot, inputs in slots 0..31, outputs in 32..63. Link id = base + index.
constexpr std::uintptr_t kPinsPerNode = 64, kOutputSlot = 32, kLinkBase = std::uintptr_t(1) << 24;

ed::PinId pin_id(int node, bool output, size_t slot) {
    return ed::PinId(std::uintptr_t(node) * kPinsPerNode + (output ? kOutputSlot : 0) + slot);
}

struct PinRef {
    int node;
    bool output;
    size_t slot;
};

PinRef decode(ed::PinId id) {
    const std::uintptr_t v = id.Get(), slot = v % kPinsPerNode;
    return {int(v / kPinsPerNode), slot >= kOutputSlot, size_t(slot % kOutputSlot)};
}

size_t slot_of(const std::vector<remod::PortSpec>& ports, const std::string& name) {
    for (size_t i = 0; i < ports.size(); ++i)
        if (name == ports[i].name) return i;
    return 0;
}

// Two dragged pins -> an output->input link, or nothing if they aren't one output and one input.
std::optional<remod::Link> make_link(const remod::Graph& g, ed::PinId a, ed::PinId b) {
    PinRef from = decode(a), to = decode(b);
    if (from.output == to.output) return std::nullopt;
    if (!from.output) std::swap(from, to);
    const remod::Node* fn = g.find(from.node);
    const remod::Node* tn = g.find(to.node);
    const remod::NodeSpec* fs = fn ? remod::find_spec(fn->type) : nullptr;
    const remod::NodeSpec* ts = tn ? remod::find_spec(tn->type) : nullptr;
    if (!fs || !ts || from.slot >= fs->outputs.size() || to.slot >= ts->inputs.size()) return std::nullopt;
    return remod::Link{from.node, fs->outputs[from.slot].name, to.node, ts->inputs[to.slot].name};
}

std::string env(const char* name) {
    char* v = nullptr;
    size_t n = 0;
    _dupenv_s(&v, &n, name);
    std::string s = v ? v : "";
    std::free(v);
    return s;
}

struct State {
    std::string graph_path = "graph.json";
    std::string noesis_path = env("REMOD_NOESIS");
    remod::Graph graph;
    std::string status = "New graph. Right-click the canvas to add nodes.";
    bool push_positions = false;  // after a load: move editor nodes to the positions in the file
    bool navigate = false;
    bool save_requested = false;  // handled inside the editor, where node positions can be read
    std::future<remod::RunResult> run;
    std::mutex log_mutex;
    std::vector<std::string> log;  // written by the run thread
};

void start_run(State& s) {
    {
        std::lock_guard lock(s.log_mutex);
        s.log.clear();
    }
    s.status = "Running...";
    s.run = std::async(std::launch::async, [&s, graph = s.graph, graph_path = s.graph_path, noesis = s.noesis_path] {
        const auto profiles = remod::find_profiles_dir();
        if (profiles.empty()) throw std::runtime_error("no 'profiles' folder found in the current folder or above the app");
        const remod::Profile profile = remod::load_profile_by_id(profiles, graph.profile);
        remod::NoesisConverter noesis_converter(noesis);
        return remod::run_graph(graph, {.profile = profile,
                                        .converter = noesis_converter,
                                        .base_dir = std::filesystem::absolute(graph_path).parent_path(),
                                        .log = [&s](const std::string& line) {
                                            std::lock_guard lock(s.log_mutex);
                                            s.log.push_back(line);
                                        }});
    });
}

void poll_run(State& s) {
    using namespace std::chrono_literals;
    if (!s.run.valid() || s.run.wait_for(0s) != std::future_status::ready) return;
    try {
        s.status = s.run.get().message;
    } catch (const std::exception& e) {
        s.status = std::string("Error: ") + e.what();
    }
}

void draw_side_panel(State& s) {
    ImGui::Begin("Pipeline");
    ImGui::InputText("Graph file", &s.graph_path);
    if (ImGui::Button("New")) {
        s.graph = {};
        s.status = "New graph. Right-click the canvas to add nodes.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        try {
            s.graph = remod::load_graph(s.graph_path);
            s.push_positions = true;
            s.status = "Loaded " + s.graph_path;
        } catch (const std::exception& e) {
            s.status = std::string("Error: ") + e.what();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save")) s.save_requested = true;

    ImGui::InputText("Noesis64.exe", &s.noesis_path);
    const bool running = s.run.valid();
    ImGui::BeginDisabled(running);
    if (ImGui::Button(running ? "Running..." : "Run")) start_run(s);
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextWrapped("%s", s.status.c_str());
    if (const auto problems = s.graph.validate(); !problems.empty()) {
        ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "Problems (%d):", int(problems.size()));
        for (const auto& p : problems) ImGui::BulletText("%s", p.c_str());
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Log");
    ImGui::BeginChild("log");
    {
        std::lock_guard lock(s.log_mutex);
        for (const auto& line : s.log) ImGui::TextWrapped("%s", line.c_str());
    }
    ImGui::EndChild();
    ImGui::End();
}

void draw_canvas(State& s, ed::EditorContext* editor) {
    ImGui::Begin("Graph");
    ed::SetCurrentEditor(editor);
    ed::Begin("canvas");

    if (s.push_positions) {
        for (const auto& n : s.graph.nodes) ed::SetNodePosition(n.id, ImVec2(n.x, n.y));
        s.push_positions = false;
        s.navigate = true;
    } else if (s.navigate) {  // one frame later, once node sizes are known
        ed::NavigateToContent(0.0f);
        s.navigate = false;
    }

    const float field_width = ImGui::GetFontSize() * 16;
    for (auto& n : s.graph.nodes) {
        const remod::NodeSpec* spec = remod::find_spec(n.type);
        ed::BeginNode(n.id);
        ImGui::PushID(n.id);
        ImGui::TextUnformatted(n.type.c_str());
        if (spec) {
            for (const auto& p : spec->params) {
                ImGui::SetNextItemWidth(field_width);
                ImGui::InputText(p.required ? (std::string(p.name) + " *").c_str() : p.name, &n.params[p.name]);
            }
            for (size_t i = 0; i < spec->inputs.size(); ++i) {
                ed::BeginPin(pin_id(n.id, false, i), ed::PinKind::Input);
                ImGui::Text("> %s", spec->inputs[i].name);
                ed::EndPin();
            }
            for (size_t i = 0; i < spec->outputs.size(); ++i) {
                ed::BeginPin(pin_id(n.id, true, i), ed::PinKind::Output);
                ImGui::Text("%s >", spec->outputs[i].name);
                ed::EndPin();
            }
        } else {
            ImGui::TextUnformatted("(unknown node type)");
        }
        ImGui::PopID();
        ed::EndNode();
    }

    for (size_t i = 0; i < s.graph.links.size(); ++i) {
        const remod::Link& l = s.graph.links[i];
        const remod::Node* from = s.graph.find(l.from_node);
        const remod::Node* to = s.graph.find(l.to_node);
        const remod::NodeSpec* fs = from ? remod::find_spec(from->type) : nullptr;
        const remod::NodeSpec* ts = to ? remod::find_spec(to->type) : nullptr;
        if (!fs || !ts) continue;
        ed::Link(kLinkBase + i, pin_id(l.from_node, true, slot_of(fs->outputs, l.from_port)),
                 pin_id(l.to_node, false, slot_of(ts->inputs, l.to_port)));
    }

    // Dragging a new link: core decides whether it's allowed.
    if (ed::BeginCreate()) {
        ed::PinId a, b;
        if (ed::QueryNewLink(&a, &b) && a && b) {
            const auto link = make_link(s.graph, a, b);
            const std::string err = link ? s.graph.can_connect(*link) : "connect an output to an input";
            if (!err.empty()) {
                ed::RejectNewItem(ImVec4(1, 0.3f, 0.3f, 1), 2.0f);
                ed::Suspend();
                ImGui::SetTooltip("%s", err.c_str());
                ed::Resume();
            } else if (ed::AcceptNewItem()) {
                s.graph.connect(*link);
            }
        }
    }
    ed::EndCreate();

    // Delete key / editor deletions: collect first, then apply links (highest index first), then nodes.
    std::vector<size_t> dead_links;
    std::vector<int> dead_nodes;
    if (ed::BeginDelete()) {
        ed::LinkId link;
        while (ed::QueryDeletedLink(&link))
            if (ed::AcceptDeletedItem()) dead_links.push_back(size_t(link.Get() - kLinkBase));
        ed::NodeId node;
        while (ed::QueryDeletedNode(&node))
            if (ed::AcceptDeletedItem()) dead_nodes.push_back(int(node.Get()));
    }
    ed::EndDelete();
    std::ranges::sort(dead_links, std::greater<>());
    for (size_t i : dead_links) s.graph.disconnect(i);
    for (int id : dead_nodes) s.graph.remove_node(id);

    // Right-click on empty canvas: add a node there.
    ed::Suspend();
    static ImVec2 new_node_pos;
    if (ed::ShowBackgroundContextMenu()) {
        new_node_pos = ImGui::GetMousePos();
        ImGui::OpenPopup("add_node");
    }
    if (ImGui::BeginPopup("add_node")) {
        for (const auto& spec : remod::node_specs()) {
            if (ImGui::MenuItem(spec.type)) {
                const remod::Node& n = s.graph.add_node(spec.type);
                ed::SetNodePosition(n.id, ed::ScreenToCanvas(new_node_pos));
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", spec.summary);
        }
        ImGui::EndPopup();
    }
    ed::Resume();

    if (s.save_requested) {
        s.save_requested = false;
        for (auto& n : s.graph.nodes) {
            const ImVec2 p = ed::GetNodePosition(n.id);
            n.x = p.x;
            n.y = p.y;
        }
        try {
            remod::save_graph(s.graph, s.graph_path);
            s.status = "Saved " + s.graph_path;
        } catch (const std::exception& e) {
            s.status = std::string("Error: ") + e.what();
        }
    }

    ed::End();
    ed::SetCurrentEditor(nullptr);
    ImGui::End();
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
    config.SettingsFile = nullptr;  // positions are saved in the graph file instead of NodeEditor.json
    ed::EditorContext* editor = ed::CreateEditor(&config);
    State state;

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

        poll_run(state);
        ImGui::DockSpaceOverViewport();
        draw_side_panel(state);
        draw_canvas(state, editor);

        ImGui::Render();
        const float clear[4] = {0.1f, 0.1f, 0.1f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_occluded = g_swap_chain->Present(1, 0) == DXGI_STATUS_OCCLUDED;
    }

    if (state.run.valid()) state.run.wait();  // let a running graph finish (every tool call has a timeout)
    ed::DestroyEditor(editor);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanup_device();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, instance);
    return 0;
}
