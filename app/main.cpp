// remod-app: thin ImGui front end over core/. Draws the graph and forwards edits and runs to core;
// all graph rules (valid links, validation, running) live in core/graph.
// Win32 + DX11 setup follows imgui/examples/example_win32_directx11 (v1.92.9-docking).
#define IMGUI_DEFINE_MATH_OPERATORS  // required by the node editor's internal header; before any imgui.h
#include "browser.hpp"
#include "graph.hpp"
#include "profile.hpp"
#include "route.hpp"
#include "settings.hpp"
#include "setup.hpp"
#include "texture_converter.hpp"
#include "zoom_view.hpp"

#include <nfd.h>

#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder (startup layout), SetFontRasterizerDensity (sharp zoomed text)
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <imgui_stdlib.h>
#include <imgui-node-editor/imgui_node_editor.h>
// ponytail: internal API (pinned 0.9.3) for a fit-to-content that doesn't zoom in; the public
// NavigateToContent always fills the view.
#include <imgui-node-editor/imgui_node_editor_internal.h>

#include <d3d11.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
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
bool g_close_requested = false;  // the window's X / Alt+F4: the main loop asks about unsaved changes first

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
    case WM_CLOSE:
        g_close_requested = true;
        return 0;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- Palette ---------------------------------------------------------------------------------------------------
// Dark, cool greys from the canvas (darkest) up to raised controls; one blue accent for selection and primary actions;
// green / amber / red for where a run got to (done / waiting for you / failed). Pins and links have their own colour
// per kind of value (kind_color), so it's visible what can plug into what.
namespace pal {
constexpr ImU32 rgb(unsigned v, unsigned alpha = 0xFF) { return IM_COL32(v >> 16, (v >> 8) & 0xFF, v & 0xFF, alpha); }
constexpr ImU32 bg = rgb(0x15171b);       // the graph canvas
constexpr ImU32 surface = rgb(0x1d2025);  // panels
constexpr ImU32 block = rgb(0x252930);    // block fill
constexpr ImU32 raised = rgb(0x2f343c);   // buttons
constexpr ImU32 line = rgb(0x3a404a);     // borders, dividers
constexpr ImU32 edge = rgb(0x4d5562);     // block outlines
constexpr ImU32 faint = rgb(0x5d6573);    // corner marks, not reached
constexpr ImU32 muted = rgb(0x8e96a3);    // secondary text
constexpr ImU32 text = rgb(0xe4e7ec);
constexpr ImU32 accent = rgb(0x4f8ff0);   // selection, primary buttons
constexpr ImU32 accent_dim = rgb(0x233c5c);
constexpr ImU32 done = rgb(0x4cc38a), waiting = rgb(0xf2b544), failed = rgb(0xf06464);
constexpr ImU32 grid = rgb(0x1e2126);
constexpr ImU32 divider = line;
}  // namespace pal

ImU32 with_alpha(ImU32 c, float k) {
    return (c & ~IM_COL32_A_MASK) | (ImU32(float(c >> IM_COL32_A_SHIFT & 0xFF) * k) << IM_COL32_A_SHIFT);
}

// `a` moved `t` of the way to `b` (opaque).
ImU32 mix(ImU32 a, ImU32 b, float t) {
    const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, 1));
}

// Pin and line colour by what flows (CLAUDE.md §4, link kinds), one hue each, all readable on the dark canvas.
// Reserved for later kinds: script 0xb48cf0 (purple), AI call 0xec8cc0 (pink).
ImU32 kind_color(remod::PortType type) {
    switch (type) {
    case remod::PortType::Tex: return pal::rgb(0xec9455);     // orange
    case remod::PortType::Image: return pal::rgb(0x9ad46a);   // green
    case remod::PortType::Text: return pal::rgb(0x6fa8ff);    // blue
    case remod::PortType::Path: return pal::rgb(0xe3cc5e);    // yellow
    case remod::PortType::Folder: return pal::rgb(0x4fcfcf);  // teal
    case remod::PortType::Any: return pal::rgb(0x9aa1ab);     // grey: a Split with nothing linked in yet
    }
    return pal::text;
}

// ImGui's and the graph editor's colours from the palette; square corners everywhere.
void apply_palette() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = st.ChildRounding = st.FrameRounding = st.PopupRounding = st.ScrollbarRounding =
        st.GrabRounding = st.TabRounding = 0;
    auto set = [&](ImGuiCol c, ImU32 v) { st.Colors[c] = ImGui::ColorConvertU32ToFloat4(v); };
    set(ImGuiCol_Text, pal::text);
    set(ImGuiCol_TextDisabled, pal::muted);
    for (ImGuiCol c : {ImGuiCol_WindowBg, ImGuiCol_PopupBg, ImGuiCol_MenuBarBg, ImGuiCol_TabSelected,
                       ImGuiCol_TabDimmedSelected, ImGuiCol_TitleBgActive})
        set(c, pal::surface);
    for (ImGuiCol c : {ImGuiCol_FrameBg, ImGuiCol_TitleBg, ImGuiCol_TitleBgCollapsed, ImGuiCol_Tab, ImGuiCol_TabDimmed,
                       ImGuiCol_ScrollbarBg, ImGuiCol_DockingEmptyBg})
        set(c, pal::bg);
    set(ImGuiCol_FrameBgHovered, pal::raised);
    set(ImGuiCol_FrameBgActive, pal::line);
    set(ImGuiCol_Border, pal::line);
    set(ImGuiCol_Separator, pal::line);
    set(ImGuiCol_SeparatorHovered, pal::accent);
    set(ImGuiCol_SeparatorActive, pal::accent);
    set(ImGuiCol_Button, pal::raised);
    set(ImGuiCol_ButtonHovered, pal::line);
    set(ImGuiCol_ButtonActive, pal::accent);
    set(ImGuiCol_Header, pal::accent_dim);
    set(ImGuiCol_HeaderHovered, pal::raised);
    set(ImGuiCol_HeaderActive, pal::accent_dim);
    set(ImGuiCol_TabHovered, pal::raised);
    set(ImGuiCol_TabSelectedOverline, pal::accent);
    set(ImGuiCol_TabDimmedSelectedOverline, pal::line);
    set(ImGuiCol_CheckMark, pal::accent);
    set(ImGuiCol_SliderGrab, pal::accent);
    set(ImGuiCol_SliderGrabActive, pal::accent);
    set(ImGuiCol_ScrollbarGrab, pal::raised);
    set(ImGuiCol_ScrollbarGrabHovered, pal::line);
    set(ImGuiCol_ScrollbarGrabActive, pal::edge);
    set(ImGuiCol_ResizeGrip, pal::raised);
    set(ImGuiCol_ResizeGripHovered, pal::line);
    set(ImGuiCol_ResizeGripActive, pal::accent);
    set(ImGuiCol_DockingPreview, with_alpha(pal::accent, 0.45f));
    set(ImGuiCol_TextSelectedBg, pal::accent_dim);
    set(ImGuiCol_NavCursor, pal::accent);

    // The editor's own node frames are switched off: the app paints each block's outline itself (paint_block).
    ed::Style& es = ed::GetStyle();
    es.Colors[ed::StyleColor_Bg] = ImGui::ColorConvertU32ToFloat4(pal::bg);
    es.Colors[ed::StyleColor_Grid] = ImGui::ColorConvertU32ToFloat4(pal::grid);
    for (auto c : {ed::StyleColor_NodeBg, ed::StyleColor_NodeBorder, ed::StyleColor_HovNodeBorder,
                   ed::StyleColor_SelNodeBorder})
        es.Colors[c] = ImVec4(0, 0, 0, 0);
    es.Colors[ed::StyleColor_NodeSelRect] = ImGui::ColorConvertU32ToFloat4(with_alpha(pal::accent, 0.15f));
    es.Colors[ed::StyleColor_NodeSelRectBorder] = ImGui::ColorConvertU32ToFloat4(pal::accent);
    es.Colors[ed::StyleColor_PinRect] = ImGui::ColorConvertU32ToFloat4(with_alpha(pal::text, 0.15f));
    es.Colors[ed::StyleColor_PinRectBorder] = ImVec4(0, 0, 0, 0);
    es.NodeRounding = 0;
    es.NodeBorderWidth = es.HoveredNodeBorderWidth = es.SelectedNodeBorderWidth = 0;
}

// ---- Graph editor UI ---------------------------------------------------------------------------

// Editor ids. Pin id = node * 4096 + (output ? 2048 : 0) + slot * 64 + row, where slot is the input/output's
// index in the node spec and row numbers the lines of a multiple input (one per link, plus an empty one).
// Links aren't editor objects: the app routes and draws them itself (core/route), so they have no ids.
constexpr std::uintptr_t kPerNode = 4096, kOutputBit = 2048, kRows = 64;

ed::PinId pin_id(int node, bool output, size_t slot, size_t row = 0) {
    return ed::PinId(std::uintptr_t(node) * kPerNode + (output ? kOutputBit : 0) + slot * kRows + row);
}

struct PinRef {
    int node;
    bool output;
    size_t slot;
};

PinRef decode(ed::PinId id) {
    const std::uintptr_t v = id.Get(), local = v % kPerNode;
    return {int(v / kPerNode), local >= kOutputBit, size_t((local % kOutputBit) / kRows)};
}

template <class Ports>
size_t slot_of(const Ports& ports, const std::string& name) {
    for (size_t i = 0; i < ports.size(); ++i)
        if (name == ports[i].name) return i;
    return 0;
}

// Two dragged pins -> an output->input link, or nothing if they aren't one output and one input.
// Any row of a multiple input means "add another link".
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

// "Original texture: texture": where a linked input's value comes from.
std::string source_of(const remod::Graph& g, const remod::Link& l) {
    const remod::Node* from = g.find(l.from_node);
    const remod::NodeSpec* spec = from ? remod::find_spec(from->type) : nullptr;
    if (!spec) return "?";
    const auto out = std::ranges::find(spec->outputs, l.from_port, [](const auto& p) { return std::string(p.name); });
    return std::string(spec->title) + ": " + (out != spec->outputs.end() ? out->label : l.from_port.c_str());
}

// "Convert image to texture: original texture": where a link goes.
std::string target_of(const remod::Graph& g, const remod::Link& l) {
    const remod::Node* to = g.find(l.to_node);
    const remod::NodeSpec* spec = to ? remod::find_spec(to->type) : nullptr;
    const remod::InputSpec* in = spec ? remod::find_input(*spec, l.to_port) : nullptr;
    return in ? std::string(spec->title) + ": " + in->label : "?";
}

// Explorer's "Copy as path" wraps paths in quotes.
std::string unquote(std::string s) {
    while (!s.empty() && (s.front() == '"' || s.front() == ' ')) s.erase(s.begin());
    while (!s.empty() && (s.back() == '"' || s.back() == ' ')) s.pop_back();
    return s;
}

std::string env(const char* name) {
    char* v = nullptr;
    size_t n = 0;
    _dupenv_s(&v, &n, name);
    std::string s = v ? v : "";
    std::free(v);
    return s;
}

// Windows file/folder picker. Returns true and updates `value` if the user picked something.
// ponytail: paths are treated as ASCII/ANSI like the rest of the app; non-ASCII paths need UTF-8 handling end to end.
// `start_dir`: where the picker opens when `value` doesn't point anywhere yet.
bool browse(remod::PathKind kind, const char* filter, std::string& value, const std::string& start_dir = {}) {
    std::string dir;
    std::error_code ec;
    if (!value.empty()) {
        const std::filesystem::path p(value);
        dir = (std::filesystem::is_directory(p, ec) ? p : p.parent_path()).string();
        if (!std::filesystem::is_directory(dir, ec)) dir.clear();
    }
    if (dir.empty() && std::filesystem::is_directory(start_dir, ec)) dir = start_dir;
    const char* start = dir.empty() ? nullptr : dir.c_str();
    // Several extensions (e.g. png,tga,jpg) get one "Save as type" entry each, so the save dialog's type list picks
    // the format; open dialogs list them all together first. Texture suffixes stay one entry.
    std::vector<std::pair<std::string, std::string>> types;  // name, extensions
    if (filter) {
        const bool split = kind != remod::PathKind::OpenTexture && std::strchr(filter, ',');
        if (kind != remod::PathKind::SaveFile || !split)
            types.emplace_back(kind == remod::PathKind::OpenTexture ? "RE Engine textures" : "Supported files", filter);
        std::istringstream list(split ? filter : "");
        for (std::string ext; std::getline(list, ext, ',');) {
            std::string name = ext;
            std::ranges::transform(name, name.begin(), [](unsigned char c) { return char(std::toupper(c)); });
            types.emplace_back(name, ext);
        }
    }
    std::vector<nfdu8filteritem_t> items;
    for (const auto& [name, exts] : types) items.push_back({name.c_str(), exts.c_str()});
    const nfdu8filteritem_t* filters = items.empty() ? nullptr : items.data();
    const auto count = nfdfiltersize_t(items.size());
    const std::string name = value.empty() ? "" : std::filesystem::path(value).filename().string();

    nfdu8char_t* out = nullptr;
    nfdresult_t r = NFD_CANCEL;
    switch (kind) {
    case remod::PathKind::OpenFile:
    case remod::PathKind::OpenTexture: r = NFD_OpenDialogU8(&out, filters, count, start); break;
    case remod::PathKind::SaveFile: r = NFD_SaveDialogU8(&out, filters, count, start, name.c_str()); break;
    case remod::PathKind::Folder: r = NFD_PickFolderU8(&out, start); break;
    case remod::PathKind::None: return false;
    }
    if (r != NFD_OKAY) return false;
    value = out;
    NFD_FreePathU8(out);
    // A name typed without an extension in the save dialog gets the filter's first one.
    if (kind == remod::PathKind::SaveFile && filter && std::filesystem::path(value).extension().empty())
        value += "." + std::string(filter).substr(0, std::string(filter).find(','));
    return true;
}

// The graph editor's block style, copied each frame (draw_canvas) so blocks can be drawn outside the editor too.
struct BlockLook {
    ImVec4 padding{8, 8, 8, 8};  // x left, y top, z right, w bottom
};

struct State {
    const std::filesystem::path settings_file = remod::default_settings_path();
    remod::Settings saved = remod::load_settings(settings_file);
    std::string graph_path = saved.graph_path.empty() ? "graph.json" : saved.graph_path;
    // Remembered path, else found automatically (REMOD_NOESIS, PATH, winget).
    std::string noesis_path = [this] {
        const auto found = remod::find_noesis(saved.noesis_path);
        return found.empty() ? saved.noesis_path : found.string();
    }();
    std::string game_files = saved.game_files_dir;  // REtool folder; empty = ask the RE plugin (see game_files_dir)
    bool show_help = saved.show_help;
    bool build_mode = saved.build_mode;  // Build layout (edit structure) vs Use layout (fill in and run)
    bool overview = false;  // zoomed out: blocks show only their title, status and linked rows
    float detail = 1;       // eases to 0 in the overview, back to 1 zoomed in
    bool tidy_requested = false;  // Tidy up clicked: arrange the blocks this frame
    bool show_descriptions = saved.show_descriptions;  // blocks' description text (else a tooltip on the title)
    std::vector<std::string> pinned = saved.pinned_folders;  // the Browser's pinned folders
    // Where a menu was opened, and on what: right-click / let-go position, pin, link index, node.
    ImVec2 menu_pos;
    ed::PinId menu_pin;
    size_t menu_link = 0;
    int menu_node = 0;
    std::string checked_noesis = "\x01";  // path the cached check below is for
    remod::NoesisCheck noesis_check;
    std::vector<std::string> profile_errors;
    const std::vector<remod::Profile> profiles = remod::load_profiles(remod::find_profiles_dir(), &profile_errors);
    // Picker filter for textures of every known game: "tex,143221013,..." (plain .tex and each version suffix).
    const std::string texture_filter = [this] {
        std::string f = "tex";
        for (const auto& p : profiles) f += "," + p.tex_suffix;
        return f;
    }();
    remod::Graph graph;
    std::string status = "New graph. Right-click the canvas to add nodes.";
    bool push_positions = false;  // after a load or an undo: move editor nodes to the positions in the graph
    bool navigate = false;
    // Undo / redo (core History) and unsaved changes: the graph as last loaded or saved. Block positions are synced
    // into the graph every frame (draw_canvas), so moves count too. After a load or New the baseline waits a frame,
    // until the editor has placed (and rounded) the positions.
    remod::History history;
    remod::Graph saved_graph;
    bool baseline_pending = true;
    // Asked before New, Load or closing with unsaved changes: what to do once the user has answered.
    enum class Pending { None, New, Load, Close } pending = Pending::None;
    std::string pending_path;  // the graph to load
    bool quit = false;
    remod::RunValues values;  // what every output gave in the last run (linked fields show it)
    // What links hold before a run (core preview_values), worked out again when the graph or its file changes.
    remod::RunValues preview;
    remod::Graph preview_of;
    // Live previews of the image blocks (core preview_image) at one size, worked out in the background when the graph
    // (positions aside) or the blocks wanted change; a change meanwhile starts the next job once this one is done.
    // `none`: blocks without one, and why when that's known.
    struct Thumb {
        ID3D11ShaderResourceView* srv = nullptr;
        float width = 0, height = 0;
        std::map<std::string, float> found;  // what its "auto" fields came to (core ImagePreview::found)
    };
    struct ShrunkFile {
        std::filesystem::file_time_type time;
        remod::ImagePreview image;
    };
    struct PreviewSet {
        unsigned side = 256;  // pixels, the longer side at most
        std::map<int, Thumb> images;
        std::map<int, std::string> none;
        std::future<std::map<int, std::pair<std::optional<remod::ImagePreview>, std::string>>> job;
        remod::Graph of;
        std::string of_path = "\x01";
        std::vector<int> of_ids;
        // Files shrunk to `side`, by path and write time: only this set's job thread uses it, one job at a time.
        std::shared_ptr<std::map<std::string, ShrunkFile>> shrunk = std::make_shared<std::map<std::string, ShrunkFile>>();
    };
    PreviewSet thumbs{256};  // under every image block
    PreviewSet big{1024};    // the popped-out ones (user, 2026-10-02: "pop out any and all preview images")
    std::map<int, ZoomPan> popouts;  // image blocks popped out into their own windows, with their zoom
    // Use layout: a clicked thumbnail shows in the Browser's viewer (user, 2026-10-02), until a texture or mesh is
    // picked there again (`viewer_on_block`, read from the Browser each frame).
    int viewer_block = 0;
    ZoomPan viewer_view;
    bool want_viewer = false, viewer_on_block = false;
    std::string preview_path = "\x01";
    std::future<remod::RunResult> run;
    std::map<int, remod::NodeStatus> statuses;  // where each node got to in the last run (badges on the nodes)
    // Link drawing: pin centres (canvas coordinates) recorded while drawing the nodes, and the routes, recomputed
    // only when a block or pin moves. routed[i] = the graph link that routes.paths[i] belongs to.
    std::map<std::uintptr_t, ImVec2> pin_pos;
    // Where each pin's link meets its block's box (left or right side, the pin's height): links are routed from there,
    // then drawn on to the pin itself. A pin on a slanted edge or at a Split dot's centre is inside the box, and the
    // router needs a pin's first step out to be clear of every block (else the link becomes a portal).
    std::map<std::uintptr_t, ImVec2> pin_anchor;
    std::vector<remod::Box> route_blocks;
    std::vector<remod::LinkRoute> route_requests;
    remod::Routes routes;
    bool routes_settled = true;  // routed with every pass (a quick single pass while blocks move)
    std::vector<size_t> routed;
    std::mutex log_mutex;
    std::vector<std::string> log;  // written by the run thread
    std::vector<std::string> warnings;  // from the last run, until the user closes the popup
    std::string pending_texture;  // picked in the Browser, put into a block inside the editor (draw_canvas)
    // Placement (core make_room / keep_apart): a block just added, placed once its size is known; blocks being dragged,
    // kept apart from the rest when let go; a load that added blocks (old file), tidied once they're drawn.
    int place_new = 0;
    std::string add_type;  // from the Nodes panel: a block to add next frame, at `add_at` (graph) or mid-view
    std::optional<ImVec2> add_at;
    std::optional<ImVec2> ghost_at;  // where a block dragged from the Nodes panel would land (graph), while over it
    BlockLook look;
    ImVec2 click_in_graph;  // where the left button went down, in graph coordinates (zooming while holding a block)
    bool show_pipeline = true;  // Use layout can close the Pipeline panel; Build layout always shows it
    std::map<int, ImVec2> last_pos;
    std::set<int> dragged;
    bool tidy_after_load = false;
    // "Destination exists" warnings on file blocks (core decides), rechecked when inputs change and every 1.5 s, not
    // every frame: it's the file system.
    std::map<int, std::string> dest_warnings;
    std::string dest_inputs = "\x01";  // every block's values and the link count, as of that check
    double dest_checked = 0;           // ImGui time of that check
    int choice_node = 0;               // the block and input a dropdown (Widget::Choice) is open for
    std::string choice_input;
    int rename_node = 0;      // the block being named (double-click its title, or Rename... in its menu)
    std::string rename_text;
    // Zoom (handoff §3): Far shows blocks as small symbols, Near as full blocks; switched by the zoom (s.overview) or
    // the Far / Near buttons above the graph (1 = Far, 2 = Near, handled inside the editor).
    int zoom_request = 0;
    std::map<int, ImVec2> near_size;  // each block's full size: Far keeps placement and Tidy up on the full layout
    ImVec2 last_view_size;            // the graph view's size last frame (view changes wait until it holds)
    int focus_node = 0;               // select this block and centre the view on it (step list, minimap)
    std::optional<ImVec2> jump_to;    // centre the view here (minimap click)
    int selected = 0;                 // the one selected block, if one is (step list highlight)
    // The minimap's picture of the graph (Near), from the last frame: blocks and the visible part, graph coordinates.
    struct MiniBlock {
        int id;
        remod::Family family;
        ImVec2 min, size;
    };
    std::vector<MiniBlock> mini;
    ImVec2 mini_view_min, mini_view_max;
};

// The REtool folder: the one set in the panel, else the one the RE plugin remembers for the graph's game.
std::string game_files_dir(const State& s) {
    if (!s.game_files.empty()) return s.game_files;
    const auto p = std::ranges::find(s.profiles, s.graph.profile, &remod::Profile::id);
    return p == s.profiles.end() ? std::string() : remod::game_files_dir(s.noesis_path, *p).string();
}

// After a texture is picked: switch the graph to that texture's game, or say why not.
void detect_game(State& s, const std::string& texture) {
    if (const remod::Profile* p = remod::profile_for_texture(texture, s.profiles)) {
        s.graph.profile = p->id;
        s.status = "Detected a " + p->name + " texture.";
    } else if (const auto version = remod::read_tex_version(texture)) {
        s.status = "This is an RE Engine texture of version " + std::to_string(*version) +
                   ", but no game profile uses that version yet. Add one to the profiles folder.";
    } else {
        s.status = "That file isn't an RE Engine texture.";
    }
}

// A path dragged from the Browser (payload "remod_path") over the last item, a block's field. Core decides whether it
// fits (`kind`, `filter`: the field's picker); if so, dropping fills the field (a texture also picks the game, as the
// "..." picker does), else `hint` says why and nothing happens.
void accept_path(State& s, std::string& value, remod::PathKind kind, const char* filter, std::string& hint) {
    if (!ImGui::BeginDragDropTarget()) return;
    if (const ImGuiPayload* peek = ImGui::GetDragDropPayload(); peek && peek->IsDataType("remod_path")) {
        const std::string path = static_cast<const char*>(peek->Data);
        std::error_code ec;
        const std::string problem = remod::path_fit(kind, filter, path, std::filesystem::is_directory(path, ec));
        if (!problem.empty()) {
            hint = problem;
        } else if (ImGui::AcceptDragDropPayload("remod_path")) {
            value = path;
            if (kind == remod::PathKind::OpenTexture) detect_game(s, value);
        }
    }
    ImGui::EndDragDropTarget();
}

// Writes the settings file only when something changed.
void remember_paths(State& s) {
    const remod::Settings now{.graph_path = s.graph_path,
                              .noesis_path = s.noesis_path,
                              .show_help = s.show_help,
                              .game_files_dir = s.game_files,
                              .build_mode = s.build_mode,
                              .show_descriptions = s.show_descriptions,
                              .pinned_folders = s.pinned};
    if (now == s.saved || s.settings_file.empty()) return;
    try {
        remod::save_settings(now, s.settings_file);
        s.saved = now;
    } catch (const std::exception& e) {
        s.status = std::string("Couldn't save settings: ") + e.what();
    }
}

void load_graph_file(State& s) {
    s.graph_path = unquote(s.graph_path);
    try {
        bool added = false;
        s.graph = remod::load_graph(s.graph_path, &added);
        s.tidy_after_load = added;  // blocks an old file gained have no place yet
        s.statuses.clear();
        s.values.clear();
        s.push_positions = true;
        s.navigate = true;
        s.baseline_pending = true;  // no undo steps, nothing unsaved (once the positions are placed)
        s.status = "Loaded " + s.graph_path + ": " + std::to_string(s.graph.nodes.size()) + " nodes, " +
                   std::to_string(s.graph.links.size()) + " links";
        remember_paths(s);
    } catch (const std::exception& e) {
        s.status = std::string("Error: ") + e.what();
    }
}

bool unsaved(const State& s) { return !s.baseline_pending && s.graph != s.saved_graph; }

// Saves the graph (block positions are synced every frame). Returns whether it worked.
bool save_graph_file(State& s) {
    s.graph_path = unquote(s.graph_path);
    try {
        remod::save_graph(s.graph, s.graph_path);
        s.saved_graph = s.graph;
        s.status = "Saved " + s.graph_path;
        remember_paths(s);
        return true;
    } catch (const std::exception& e) {
        s.status = std::string("Error: ") + e.what();
        return false;
    }
}

void new_graph(State& s) {
    s.graph = {};
    s.statuses.clear();
    s.values.clear();
    s.baseline_pending = true;
    s.build_mode = true;  // an empty layout can only be built
    remember_paths(s);
    s.status = "New layout. Right-click the canvas to add blocks.";
}

// New, Load or Close, after asking about unsaved changes if there are any (draw_unsaved_prompt).
void do_pending(State& s) {
    if (s.pending == State::Pending::New) new_graph(s);
    if (s.pending == State::Pending::Load) {
        s.graph_path = s.pending_path;
        load_graph_file(s);
    }
    if (s.pending == State::Pending::Close) s.quit = true;
    s.pending = State::Pending::None;
}

void ask(State& s, State::Pending what, const std::string& path = {}) {
    s.pending = what;
    s.pending_path = path;
    if (!unsaved(s)) do_pending(s);
}

// "Save changes?" before New, Load or closing, while the graph has unsaved changes.
void draw_unsaved_prompt(State& s) {
    if (s.pending == State::Pending::None) return;
    if (!ImGui::IsPopupOpen("Unsaved changes")) ImGui::OpenPopup("Unsaved changes");
    if (!ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const char* what = s.pending == State::Pending::New    ? "starting a new layout"
                       : s.pending == State::Pending::Load ? "loading another graph"
                                                           : "closing";
    ImGui::Text("Save the changes to %s before %s?", std::filesystem::path(s.graph_path).filename().string().c_str(), what);
    if (ImGui::Button("Save")) {
        ImGui::CloseCurrentPopup();
        if (save_graph_file(s)) do_pending(s);
        else s.pending = State::Pending::None;  // the status line says why
    }
    ImGui::SameLine();
    if (ImGui::Button("Don't save")) {
        ImGui::CloseCurrentPopup();
        do_pending(s);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
        s.pending = State::Pending::None;
    }
    ImGui::EndPopup();
}

// Ctrl+Z / Ctrl+Y (and the buttons): the graph as it was, its blocks moved back too.
void undo(State& s, bool redo) {
    if (!(redo ? s.history.redo(s.graph) : s.history.undo(s.graph))) return;
    s.push_positions = true;
    s.place_new = 0;
    s.dragged.clear();
    s.status = redo ? "Redone." : "Undone.";
}

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
        const remod::RunResult r = s.run.get();
        remod::apply_run(s.graph, r);  // e.g. a re-exported image un-does an earlier "Done editing"
        s.statuses = r.nodes;
        s.values = r.values;
        s.status = r.message;
        s.warnings = r.warnings;  // shown in a popup (draw_warnings)
    } catch (const remod::RunError& e) {
        remod::apply_run(s.graph, {.state = e.state});
        s.statuses = e.nodes;
        s.values = e.values;
        s.status = std::string("Error: ") + e.what();
    } catch (const std::exception& e) {
        s.statuses.clear();
        s.status = std::string("Error: ") + e.what();
    }
}

const ImVec4 kAmber(1.0f, 0.7f, 0.2f, 1.0f);

// The last run's warnings, as a popup the user closes.
void draw_warnings(State& s) {
    if (s.warnings.empty()) return;
    if (!ImGui::IsPopupOpen("Check this")) ImGui::OpenPopup("Check this");
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 32, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Check this", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextColored(kAmber, "The mod was built, with %zu warning%s:", s.warnings.size(),
                           s.warnings.size() == 1 ? "" : "s");
        for (const auto& w : s.warnings) ImGui::TextWrapped("- %s", w.c_str());
        if (ImGui::Button("OK")) {
            s.warnings.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// The program an Edit image block opens its image with (its "Open with", typed or from a linked Value; core
// known_value), a relative path taken from the graph's folder; empty: Windows' default.
std::filesystem::path editor_of(const State& s, int node) {
    const std::filesystem::path p(unquote(remod::known_value(s.graph, node, "editor")));
    return p.empty() || p.is_absolute() ? p : std::filesystem::absolute(s.graph_path).parent_path() / p;
}

// Opens a file in `editor` (a program, e.g. GIMP) if one is given, else in Windows' editor for its type (the "edit"
// verb, e.g. Paint), else whatever opens it. Returns why it couldn't, or "".
std::string open_in_editor(const std::filesystem::path& file, const std::filesystem::path& editor = {}) {
    auto ok = [](HINSTANCE h) { return reinterpret_cast<INT_PTR>(h) > 32; };
    if (!editor.empty()) {
        const std::wstring args = L"\"" + file.wstring() + L"\"";
        return ok(::ShellExecuteW(nullptr, L"open", editor.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL))
                   ? ""
                   : "Couldn't start " + editor.string() + ". Check the Edit image block's Open with.";
    }
    if (!ok(::ShellExecuteW(nullptr, L"edit", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) &&
        !ok(::ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL)))
        return "Windows has no program to open " + file.filename().string() + " with. Set the block's Open with.";
    return "";
}

// "Open in editor" for an Edit image block: its program, else Windows'; a problem goes to the status line.
void open_edit(State& s, int node, const std::filesystem::path& file) {
    if (const std::string problem = open_in_editor(file, editor_of(s, node)); !problem.empty()) s.status = problem;
}

// The button's tooltip: which program it opens.
std::string open_edit_hint(const State& s, int node) {
    const std::filesystem::path editor = editor_of(s, node);
    return editor.empty() ? "Opens the image in Windows' editor for its file type. Set Open with to pick a program."
                          : "Opens the image in " + editor.filename().string() + " (Open with).";
}

std::optional<remod::NodeState> run_state(const State& s, int id);
std::string status_text(const remod::NodeStatus& st);
ImVec4 state_color(remod::NodeState state);
void corner_marks(ImDrawList* d, ImVec2 min, ImVec2 max, float arm, float offset, ImU32 col, float width);
void dashed(ImDrawList* d, const ImVec2* p, int count, bool closed, ImU32 col, float width, float on, float off,
            float phase);
float unit();

// Use layout (handoff §5): the steps in run order, each with where it got to (a square: green when done, amber
// when waiting for you, red when failed, dashed when not reached); a click selects the block and centres
// the view on it. Above them, the step waiting for you as a card with its buttons; under them, why a step failed.
void draw_steps(State& s) {
    struct Step {
        int id, number;
        const remod::NodeSpec* spec;
        std::optional<remod::NodeState> state;
    };
    std::vector<Step> steps;
    for (const int id : remod::step_order(s.graph)) {
        const remod::Node* n = s.graph.find(id);
        const remod::NodeSpec* spec = n ? remod::find_spec(n->type) : nullptr;
        if (spec && !spec->utility) steps.push_back({id, int(steps.size()) + 1, spec, run_state(s, id)});
    }
    if (steps.empty()) return;
    const float u = unit();
    ImDrawList* d = ImGui::GetWindowDrawList();
    for (const Step& st : steps) {  // the step waiting for you
        const auto status = s.statuses.find(st.id);
        if (st.state != remod::NodeState::Waiting || !st.spec->manual || status == s.statuses.end()) continue;
        ImGui::Spacing();
        ImGui::PushID(st.id);
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(pal::waiting));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.5f);
        ImGui::BeginChild("your_step", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        ImGui::TextDisabled("YOUR STEP \xC2\xB7 %d", st.number);
        ImGui::TextUnformatted(remod::block_title(*s.graph.find(st.id)).c_str());
        ImGui::TextWrapped("%s", status->second.message.c_str());
        if (!status->second.file.empty()) {
            if (ImGui::Button("Open in editor")) open_edit(s, st.id, status->second.file);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", open_edit_hint(s, st.id).c_str());
            ImGui::SameLine();
        }
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(pal::accent));
        if (ImGui::Button("Done editing")) remod::set_edit_done(s.graph, st.id, true);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click once you've saved your changes, then Run again.");
        ImGui::EndChild();
        corner_marks(d, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), 3.5f * u, 0, pal::waiting, 1);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::SeparatorText("Steps");
    const Step* failed = nullptr;
    for (const Step& st : steps) {
        using enum remod::NodeState;
        if (st.state == Failed && !failed) failed = &st;
        ImGui::PushID(st.id);
        if (ImGui::Selectable("##step", st.id == s.selected)) s.focus_node = st.id;
        const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
        const float box = 9 * u, y = (lo.y + hi.y - box) * 0.5f;
        const ImVec2 q[] = {{lo.x, y}, {lo.x + box, y}, {lo.x + box, y + box}, {lo.x, y + box}};
        if (st.state == Done || st.state == Waiting || st.state == Failed)
            d->AddRectFilled(q[0], q[2], st.state == Done ? pal::done : st.state == Waiting ? pal::waiting : pal::failed);
        else
            dashed(d, q, 4, true, pal::faint, 1, 2, 2, 0);
        const std::string title = std::to_string(st.number) + "  " + remod::block_title(*s.graph.find(st.id));
        d->AddText(ImVec2(lo.x + box + ImGui::GetStyle().ItemSpacing.x, lo.y), ImGui::GetColorU32(ImGuiCol_Text),
                   title.c_str());
        const char* label = !st.state ? "" : st.state == Done ? "Done" : st.state == Waiting ? "Your step"
                                           : st.state == Failed ? "Failed" : "Not reached";
        if (st.state)
            d->AddText(ImVec2(hi.x - ImGui::CalcTextSize(label).x, lo.y), ImGui::GetColorU32(state_color(*st.state)), label);
        ImGui::PopID();
    }
    if (failed)
        ImGui::TextWrapped("%s failed: %s", remod::block_title(*s.graph.find(failed->id)).c_str(),
                           s.statuses[failed->id].message.c_str());
}

// The Pipeline panel. Use layout: everything, and it can be closed (the button above the graph reopens it). Build
// layout: only what building uses (graph file, game, status, problems); no run settings or log.
void draw_side_panel(State& s) {
    ImGui::Begin("Pipeline", s.build_mode ? nullptr : &s.show_pipeline);
    if (ImGui::Checkbox("Show help", &s.show_help)) remember_paths(s);

    if (s.show_help && !s.build_mode) {
        ImGui::TextWrapped("1. Original texture: pick the game's .tex file (the picker opens in your REtool folder).");
        ImGui::TextWrapped("2. Run: Export image writes the image, and the run stops at Edit image - your step.");
        ImGui::TextWrapped("3. On Edit image: Open in editor, change and save the image (same size and format), click Done editing.");
        ImGui::TextWrapped("4. Run again: Convert image to texture and Package for Fluffy build the mod .zip.");
        ImGui::TextWrapped("After each run every block and link shows how far it got: done (filled), waiting for "
                           "you (dark, moving dashes into it), failed (light, with the reason), not reached (dashed).");
        ImGui::TextDisabled("To change which blocks there are or how they connect, switch to Build layout.");
        ImGui::Separator();
    } else if (s.show_help) {
        ImGui::TextWrapped("Right-click empty canvas: add a block there.");
        ImGui::TextWrapped("Drag from a pin to a pin: link them. Drag from a pin to empty canvas: add a block there, "
                           "already linked.");
        ImGui::TextWrapped("Right-click a link: insert a block on it, or delete it. Right-click a block: duplicate, "
                           "disconnect or delete it. Delete key removes the selection.");
        ImGui::TextWrapped("Every field has a pin: link a Text block into it to reuse a value. Package takes any "
                           "number of textures and previews: a new line appears as you connect each one.");
        ImGui::TextDisabled("Save the layout, then switch to Use layout to run it.");
        ImGui::Separator();
    }
    ImGui::InputText("##graph", &s.graph_path);
    ImGui::SameLine();
    if (std::string picked = s.graph_path; ImGui::Button("...##graph") && browse(remod::PathKind::OpenFile, "json", picked))
        ask(s, State::Pending::Load, picked);
    ImGui::SameLine();
    ImGui::TextUnformatted("Graph file");
    if (ImGui::Button("New")) ask(s, State::Pending::New);
    ImGui::SameLine();
    if (ImGui::Button("Load")) ask(s, State::Pending::Load, unquote(s.graph_path));
    ImGui::SameLine();
    if (ImGui::Button(unsaved(s) ? "Save*" : "Save")) save_graph_file(s);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(unsaved(s) ? "Unsaved changes. Ctrl+S" : "Ctrl+S");
    ImGui::SameLine();
    if (ImGui::Button("Save As...") && browse(remod::PathKind::SaveFile, "json", s.graph_path)) save_graph_file(s);
    ImGui::SameLine();
    if (ImGui::Button("Fit view")) s.navigate = true;
    ImGui::BeginDisabled(!s.history.can_undo());
    if (ImGui::Button("Undo")) undo(s, false);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Ctrl+Z");
    ImGui::SameLine();
    ImGui::BeginDisabled(!s.history.can_redo());
    if (ImGui::Button("Redo")) undo(s, true);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Ctrl+Y or Ctrl+Shift+Z");

    if (!s.build_mode) {  // run settings
        ImGui::InputText("##noesis", &s.noesis_path);
        ImGui::SameLine();
        if (ImGui::Button("...##noesis") && browse(remod::PathKind::OpenFile, "exe", s.noesis_path)) remember_paths(s);
        ImGui::SameLine();
        ImGui::TextUnformatted("Noesis64.exe");
        if (s.checked_noesis != s.noesis_path) {  // re-check only when the path changes
            s.checked_noesis = s.noesis_path;
            s.noesis_check = remod::check_noesis(unquote(s.noesis_path));
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(s.noesis_check.ok ? ImVec4(0.4f, 0.85f, 0.4f, 1) : ImVec4(1, 0.45f, 0.35f, 1), "%s",
                           s.noesis_check.message.c_str());
        ImGui::PopTextWrapPos();

        // Where the texture picker opens: the REtool folder of extracted game files.
        std::string shown = game_files_dir(s);
        if (ImGui::InputTextWithHint("##gamefiles", "extracted game files (REtool) folder", &shown)) s.game_files = shown;
        ImGui::SameLine();
        if (ImGui::Button("...##gamefiles") && browse(remod::PathKind::Folder, nullptr, shown)) {
            s.game_files = shown;
            remember_paths(s);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("Game files");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Your REtool folder, e.g. ...\\REtool\\RE4\\re_chunk_000\\natives\\stm.\n"
                              "The texture picker opens here. Filled in from the RE plugin's settings if you've set "
                              "it there.");
    }

    // Which game (profile) the graph targets. Picking a texture sets this automatically.
    const auto current = std::ranges::find(s.profiles, s.graph.profile, &remod::Profile::id);
    if (ImGui::BeginCombo("Game", current != s.profiles.end() ? current->name.c_str() : s.graph.profile.c_str())) {
        for (const auto& p : s.profiles)
            if (ImGui::Selectable(p.name.c_str(), p.id == s.graph.profile)) s.graph.profile = p.id;
        ImGui::EndCombo();
    }
    if (current == s.profiles.end())
        ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "No profile '%s' in the profiles folder.", s.graph.profile.c_str());
    for (const auto& e : s.profile_errors) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", e.c_str());
    if (s.build_mode) {
        ImGui::TextDisabled("Building the layout. Switch to Use layout to run it.");
    } else {
        const bool running = s.run.valid();
        ImGui::BeginDisabled(running);
        if (ImGui::Button(running ? "Running..." : "Run")) {
            s.graph_path = unquote(s.graph_path);
            s.noesis_path = unquote(s.noesis_path);
            remember_paths(s);
            start_run(s);
        }
        ImGui::EndDisabled();
        draw_steps(s);
    }

    ImGui::Separator();
    ImGui::TextWrapped("%s", s.status.c_str());
    if (const auto problems = s.graph.validate(); !problems.empty()) {
        ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "Problems (%d):", int(problems.size()));
        for (const auto& p : problems) ImGui::BulletText("%s", p.c_str());
    }
    if (!s.build_mode) {
        ImGui::Separator();
        ImGui::TextUnformatted("Log");
        ImGui::BeginChild("log");
        {
            std::lock_guard lock(s.log_mutex);
            for (const auto& line : s.log) ImGui::TextWrapped("%s", line.c_str());
        }
        ImGui::EndChild();
    }
    ImGui::End();
}


// Text colour for where a node got to in the last run.
ImVec4 state_color(remod::NodeState state) {
    switch (state) {
    case remod::NodeState::Done: return ImGui::ColorConvertU32ToFloat4(pal::done);
    case remod::NodeState::Waiting: return ImGui::ColorConvertU32ToFloat4(pal::waiting);
    case remod::NodeState::Failed: return ImGui::ColorConvertU32ToFloat4(pal::failed);
    case remod::NodeState::NotReached: return ImGui::ColorConvertU32ToFloat4(pal::muted);
    }
    return ImVec4(1, 1, 1, 1);
}

std::string status_text(const remod::NodeStatus& st) {
    switch (st.state) {
    case remod::NodeState::Done: return "Done: " + st.message;
    case remod::NodeState::Waiting: return "Waiting for you: " + st.message;
    case remod::NodeState::Failed: return "Failed: " + st.message;
    case remod::NodeState::NotReached: return "Not reached yet";
    }
    return "";
}

const char* type_name(remod::PortType type) {
    switch (type) {
    case remod::PortType::Tex: return "texture";
    case remod::PortType::Image: return "image";
    case remod::PortType::Text: return "text";
    case remod::PortType::Path: return "path";
    case remod::PortType::Folder: return "folder";
    case remod::PortType::Any: return "any";
    }
    return "";
}

// A block type's tooltip: its description, then its inputs and outputs with what kind each takes or gives.
void spec_tooltip(const remod::NodeSpec& spec) {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24);
    ImGui::TextUnformatted(spec.summary);
    for (const bool outputs : {false, true}) {
        std::string list;
        const size_t count = outputs ? spec.outputs.size() : spec.inputs.size();
        for (size_t i = 0; i < count; ++i)
            list += std::string(i ? ", " : "") + (outputs ? spec.outputs[i].label : spec.inputs[i].label) + " (" +
                    type_name(outputs ? spec.outputs[i].type : spec.inputs[i].type) + ")";
        if (count) ImGui::TextDisabled("%s %s", outputs ? "out:" : "in:", list.c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// ---- Block shapes (docs/design_handoff_node_graph, option 1c) ----------------------------------------------------
// Sizes are the handoff's px at its 11 px body text, times a unit that scales them to our font (unit()).

float unit() { return ImGui::GetFontSize() / 11; }

// A family's outline (handoff §1) around `a` + `s`, as a polygon: source notched on the left, flow a hexagon, file a
// folded corner, value a parallelogram, the rest rectangles.
std::vector<ImVec2> outline(remod::Family f, ImVec2 a, ImVec2 s, float u) {
    using enum remod::Family;
    const float w = s.x, h = s.y;
    std::vector<ImVec2> p;
    switch (f) {
    case Source: p = {{0, 0}, {w, 0}, {w, h}, {0, h}, {8 * u, h / 2}}; break;
    case Flow: p = {{10 * u, 0}, {w - 10 * u, 0}, {w, h / 2}, {w - 10 * u, h}, {10 * u, h}, {0, h / 2}}; break;
    case File: p = {{0, 0}, {w - 10 * u, 0}, {w, 10 * u}, {w, h}, {0, h}}; break;
    case Value: p = {{8 * u, 0}, {w, 0}, {w - 8 * u, h}, {0, h}}; break;
    default: p = {{0, 0}, {w, 0}, {w, h}, {0, h}};
    }
    for (auto& q : p) q += a;
    return p;
}

// Where the outline's left or right edge is at height `y` (from the top): pins sit on it, slanted edges too.
float edge_x(remod::Family f, ImVec2 s, float y, bool right, float u) {
    float in = 0;
    if (s.y > 0 && f == remod::Family::Flow) in = 10 * u * std::abs(y - s.y / 2) / (s.y / 2);
    if (s.y > 0 && f == remod::Family::Value) in = 8 * u * (right ? y / s.y : 1 - y / s.y);
    return right ? s.x - in : in;
}

// The part of a polygon above `y` (a block's header band).
std::vector<ImVec2> clip_above(const std::vector<ImVec2>& poly, float y) {
    std::vector<ImVec2> out;
    for (size_t i = 0; i < poly.size(); ++i) {
        const ImVec2 a = poly[i], b = poly[(i + 1) % poly.size()];
        if (a.y <= y) out.push_back(a);
        if ((a.y < y) != (b.y < y)) out.push_back(a + (b - a) * ((y - a.y) / (b.y - a.y)));
    }
    return out;
}

// A polyline in dashes `on` long, `off` apart, the pattern moved `phase` along it (negative: dashes run forwards).
void dashed(ImDrawList* d, const ImVec2* p, int count, bool closed, ImU32 col, float width, float on, float off,
            float phase = 0) {
    const float period = on + off;
    float t = std::fmod(phase, period);
    if (t < 0) t += period;
    for (int i = 0; i + 1 < count + (closed ? 1 : 0); ++i) {
        const ImVec2 a = p[i], b = p[(i + 1) % count];
        const float len = ImLength(b - a);
        for (float s = 0; s < len;) {
            const bool drawing = t < on;
            const float step = ImMin(len - s, drawing ? on - t : period - t);
            if (drawing) d->AddLine(a + (b - a) * (s / len), a + (b - a) * ((s + step) / len), col, width);
            s += step;
            t = std::fmod(t + step, period);
        }
    }
}

// `+` registration marks at the corners of `min`..`max`, `arm` long, `offset` out.
void corner_marks(ImDrawList* d, ImVec2 min, ImVec2 max, float arm, float offset, ImU32 col, float width) {
    for (const ImVec2 c : {ImVec2(min.x - offset, min.y - offset), ImVec2(max.x + offset, min.y - offset),
                           ImVec2(min.x - offset, max.y + offset), ImVec2(max.x + offset, max.y + offset)}) {
        d->AddLine(c - ImVec2(arm, 0), c + ImVec2(arm, 0), col, width);
        d->AddLine(c - ImVec2(0, arm), c + ImVec2(0, arm), col, width);
    }
}

// How a block is painted: its fill and outline by run state, the header band (Near), the manual step's hatching.
struct BlockPaint {
    ImU32 fill = pal::block, stroke = pal::edge;
    float width = 1.1f;      // outline, in units
    bool dashed = false;     // not reached
    float band = 0;          // Near: the header's height (0: none); a divider under it
    ImU32 band_fill = 0;     // the header's fill by run state (0: none)
    float hatch = 0;         // a manual step: hatched this far down
    ImU32 marks = pal::faint;
    bool selected = false;   // larger marks in the accent
    float alpha = 1;         // not reached, Far: 55%
};

void paint_block(ImDrawList* d, remod::Family f, ImVec2 a, ImVec2 s, float u, const BlockPaint& p) {
    using enum remod::Family;
    auto c = [&](ImU32 col) { return with_alpha(col, p.alpha); };
    const auto shape = outline(f, a, s, u);
    const float lip = f == Output ? 5 * u : 0;  // Output: a second sheet stacked behind, up and right
    if (lip > 0) {
        d->AddRectFilled(a + ImVec2(lip, -lip), a + ImVec2(s.x + lip, s.y - lip), c(pal::bg));
        const ImVec2 sheet[] = {a + ImVec2(lip, 0), a + ImVec2(lip, -lip), a + ImVec2(s.x + lip, -lip),
                                a + ImVec2(s.x + lip, s.y - lip), a + ImVec2(s.x, s.y - lip)};
        d->AddPolyline(sheet, 5, c(p.stroke), ImDrawFlags_None, p.width * u);
    }
    d->AddConcavePolyFilled(shape.data(), int(shape.size()), c(p.fill));
    if (p.band > 0 && p.band_fill) {
        const auto band = clip_above(shape, a.y + p.band);
        d->AddConcavePolyFilled(band.data(), int(band.size()), c(p.band_fill));
    }
    if (p.hatch > 0) {  // 45° lines 5 apart, clipped to the (rectangular) block's top `hatch`
        const float h = p.hatch;
        for (float x = -h; x < s.x; x += 5 * u * 1.41421f) {
            const float t0 = ImMax(0.0f, -x), t1 = ImMin(h, s.x - x);
            if (t0 < t1)
                d->AddLine(a + ImVec2(x + t0, h - t0), a + ImVec2(x + t1, h - t1), c(with_alpha(pal::waiting, 0.3f)),
                           1.6f * u);
        }
    }
    if (p.band > 0 && p.band < s.y)
        d->AddLine(a + ImVec2(edge_x(f, s, p.band, false, u), p.band), a + ImVec2(edge_x(f, s, p.band, true, u), p.band),
                   c(pal::divider), u);
    if (p.dashed) dashed(d, shape.data(), int(shape.size()), true, c(p.stroke), p.width * u, 4 * u, 3 * u);
    else d->AddPolyline(shape.data(), int(shape.size()), c(p.stroke), ImDrawFlags_Closed, p.width * u);
    if (f == Manual) d->AddRect(a + ImVec2(3 * u, 3 * u), a + s - ImVec2(3 * u, 3 * u), c(p.stroke), 0, 0, 0.9f * u);
    if (f == File) {
        const ImVec2 fold[] = {a + ImVec2(s.x - 10 * u, 0), a + ImVec2(s.x - 10 * u, 10 * u), a + ImVec2(s.x, 10 * u)};
        d->AddPolyline(fold, 3, c(p.stroke), ImDrawFlags_None, u);
    }
    if (p.marks || p.selected)
        corner_marks(d, a - ImVec2(0, lip), a + s + ImVec2(lip, 0), (p.selected ? 5 : 3.5f) * u, (p.selected ? 8 : 5) * u,
                     c(p.selected ? pal::accent : p.marks), u);
}

// Pins (handoff §2): shape and colour say the kind (texture square, image circle, text/path/folder diamond); filled
// when linked. Field: an unlinked typed field's small hollow circle. Add: a multiple input's empty slot, dashed.
// Colours go through the style alpha, so pins fade with their row.
enum class PinLook { Port, Field, Add };

void draw_pin_shape(ImDrawList* d, ImVec2 c, remod::PortType type, PinLook look, bool wired, bool dim, float u) {
    const ImU32 ink = ImGui::GetColorU32(dim ? pal::faint : kind_color(type)), bg = ImGui::GetColorU32(pal::bg);
    const ImU32 fill = wired ? ink : bg;
    if (look == PinLook::Field) {
        d->AddCircleFilled(c, 2.75f * u, bg);
        d->AddCircle(c, 2.75f * u, ImGui::GetColorU32(dim ? pal::faint : with_alpha(kind_color(type), 0.8f)), 0, u);
        return;
    }
    const ImVec2 r(3.5f * u, 3.5f * u);
    if (look == PinLook::Add) {
        const ImVec2 q[] = {c - r, ImVec2(c.x + r.x, c.y - r.y), c + r, ImVec2(c.x - r.x, c.y + r.y)};
        d->AddRectFilled(c - r, c + r, bg);
        dashed(d, q, 4, true, ImGui::GetColorU32(kind_color(type)), u, 2 * u, 1.5f * u);
        return;
    }
    switch (type) {
    case remod::PortType::Tex:
        d->AddRectFilled(c - r, c + r, fill);
        d->AddRect(c - r, c + r, ink, 0, 0, 1.25f * u);
        break;
    case remod::PortType::Image:
    case remod::PortType::Any:
        d->AddCircleFilled(c, 4 * u, fill);
        d->AddCircle(c, 4 * u, ink, 0, 1.25f * u);
        break;
    default: {
        const float k = 4.5f * u;
        d->AddQuadFilled(c - ImVec2(0, k), c + ImVec2(k, 0), c + ImVec2(0, k), c - ImVec2(k, 0), fill);
        d->AddQuad(c - ImVec2(0, k), c + ImVec2(k, 0), c + ImVec2(0, k), c - ImVec2(k, 0), ink, 1.25f * u);
    }
    }
}

// Links (handoff §4/§5), in the colour of the kind they carry. Build layout: thicker and brighter on the selected
// block's links. Use layout, from where the link's source got to: done solid; done into a step waiting for the user,
// dashes running forward; failed, red dots; not reached, faint dashes. Widths and dashes in units; `order` = drawing
// order (higher on top).
enum class LinkState { Build, Hot, Done, Active, Failed, Pending };
struct Stroke {
    ImU32 color;
    float width, on = 0, off = 0, speed = 0;  // speed: dash movement in units per second
    int order = 2;
};

Stroke stroke_for(LinkState state, ImU32 kind) {
    switch (state) {
    case LinkState::Build: return {kind, 1.5f};
    case LinkState::Hot: return {mix(kind, pal::text, 0.35f), 2.25f, 0, 0, 0, 3};
    case LinkState::Done: return {kind, 2.25f};
    case LinkState::Active: return {mix(kind, pal::text, 0.25f), 2.25f, 7, 5, 24 / 0.9f, 3};
    case LinkState::Failed: return {pal::failed, 1.75f, 2, 3, 0, 1};
    case LinkState::Pending: return {with_alpha(kind, 0.4f), 1.25f, 3, 3, 0, 0};
    }
    return {kind, 1.5f};
}

// The fonts (loaded in main; nullptr = the default): Segoe UI Semibold for block titles (the handoff's Barlow
// Condensed 600 isn't on Windows) and Consolas for values and status labels.
ImFont* g_title_font = nullptr;
ImFont* g_mono_font = nullptr;
ImFont* font_or(ImFont* f) { return f ? f : ImGui::GetFont(); }

// `t` wrapped to `wrap`, each line centred on `top_center.x`. Returns the height; with no `d` it only measures.
float centered_text(ImDrawList* d, ImFont* f, float size, ImVec2 top_center, float wrap, ImU32 col, const char* t) {
    f = font_or(f);
    float y = top_center.y;
    for (const char *s = t, *end = t + std::strlen(t); s < end; y += size) {
        const char* e = ImMax(f->CalcWordWrapPosition(size, s, end, wrap), s + 1);
        const char* last = e;
        while (last > s && last[-1] == ' ') --last;
        if (d) d->AddText(f, size, ImVec2(top_center.x - f->CalcTextSizeA(size, FLT_MAX, 0, s, last).x / 2, y), col, s, last);
        for (s = e; s < end && *s == ' ';) ++s;
    }
    return y - top_center.y;
}

// What a new block of `spec` looks like in Build layout: its outline, title, then its rows the way draw_canvas lays
// them out (pins on the edges, labels, field boxes, destinations beside their output), without the editor. Drawn at
// `at` (top-left) `scale`d and faded to `alpha`; with no `draw` it only measures. Returns the size. For the Nodes
// panel and the see-through copy of a block dragged onto the graph. ponytail: mirrors draw_canvas's layout by hand
// (no descriptions, a fresh block's rows); keep the two in step when the block layout changes.
ImVec2 draw_block_preview(ImDrawList* draw, ImVec2 at, const remod::NodeSpec& spec, const BlockLook& look, float scale,
                          float alpha) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float font = ImGui::GetFontSize(), u = unit();
    const bool utility = spec.utility;
    const bool fields = std::ranges::any_of(spec.inputs, &remod::InputSpec::editable) ||
                        std::ranges::any_of(spec.outputs, [](const auto& o) { return o.field != nullptr; });
    const float label_w = font * (utility ? 4 : 7), field_w = font * (utility ? 8 : 14);
    const float button_w = ImGui::CalcTextSize("...").x + style.FramePadding.x * 2;
    const float inner = utility && !fields ? font * 7 : label_w + field_w + style.ItemSpacing.x + button_w;
    const float x0 = look.padding.x, width = look.padding.x + inner + look.padding.z;
    const float row = ImGui::GetFrameHeight(), step = row + style.ItemSpacing.y;
    const float title = font * (utility ? 1.1f : 1.6f), band = look.padding.y + title + style.ItemSpacing.y * 0.5f;
    auto fade = [&](ImU32 c) { return with_alpha(c, alpha); };
    auto pos = [&](float x, float y) { return at + ImVec2(x, y) * scale; };
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text), dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 frame = ImGui::GetColorU32(ImGuiCol_FrameBg), button = ImGui::GetColorU32(ImGuiCol_Button);
    float height = 0;  // known once measured

    // The rows, walked twice: once to measure, once to paint.
    auto rows = [&](bool paint) {
        auto label = [&](float x, float y, const char* t, ImU32 col, float size = 0, ImFont* f = nullptr) {
            if (paint) draw->AddText(font_or(f), (size ? size : font) * scale, pos(x, y), fade(col), t);
        };
        auto box = [&](float x, float y, float w, ImU32 col) {
            if (paint) draw->AddRectFilled(pos(x, y), pos(x + w, y + row), fade(col));
        };
        auto pin = [&](bool right, float y, remod::PortType type, PinLook pin_look) {
            if (!paint) return;
            const float cy = y + row / 2;
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
            draw_pin_shape(draw, pos(edge_x(spec.family, ImVec2(width, height), cy, right, u), cy), type, pin_look,
                           false, false, u * scale);
            ImGui::PopStyleVar();
        };
        auto right_label = [&](float y, const char* t) {
            label(x0 + inner - ImGui::CalcTextSize(t).x, y + style.FramePadding.y, t, text);
        };
        float y = look.padding.y;
        label(x0, y, spec.title, utility ? pal::muted : pal::text, title, g_title_font);
        y += title + style.ItemSpacing.y;
        for (const auto& in : spec.inputs) {  // fixed inputs: pin, label, field
            if (in.multiple || in.result) continue;
            pin(false, y, in.type, in.editable() ? PinLook::Field : PinLook::Port);
            label(x0, y + style.FramePadding.y, in.required ? (std::string(in.label) + " *").c_str() : in.label, text);
            if (in.widget == remod::Widget::Checkbox) box(x0 + label_w, y, row, frame);
            else if (in.editable()) box(x0 + label_w, y, field_w, frame);
            if (in.widget == remod::Widget::Path) box(x0 + label_w + field_w + style.ItemSpacing.x, y, button_w, button);
            y += step;
        }
        if (spec.manual) {
            label(x0, y + style.FramePadding.y, "Run first: Export image creates the file to edit.", dim);
            y += step;
            box(x0, y, ImGui::CalcTextSize("Done editing").x + style.FramePadding.x * 2, button);
            label(x0 + style.FramePadding.x, y + style.FramePadding.y, "Done editing", text);
            y += step;
        }
        for (const auto& out : spec.outputs) {
            const auto dest = std::ranges::find_if(spec.inputs, [&](const auto& in) {
                return in.result && std::string_view(in.result) == out.name;
            });
            const std::string name = out.multiple ? std::string("+ ") + out.label : std::string(out.label);
            if (out.field || dest != spec.inputs.end()) {  // where it writes: field, "...", then the output
                const float x =
                    x0 + inner - (field_w + style.ItemSpacing.x * 2 + button_w + ImGui::CalcTextSize(out.label).x);
                box(x, y, field_w, frame);
                label(x + style.FramePadding.x, y + style.FramePadding.y,
                      dest != spec.inputs.end() ? dest->label : out.field_label, dim);
                box(x + field_w + style.ItemSpacing.x, y, button_w, button);
            }
            right_label(y, name.c_str());
            pin(true, y, out.type, out.multiple ? PinLook::Add : PinLook::Port);
            y += step;
        }
        for (const auto& in : spec.inputs) {  // inputs that grow a row per link: the row to connect the first one
            if (!in.multiple) continue;
            pin(false, y, in.type, PinLook::Add);
            label(x0, y + style.FramePadding.y, (std::string("+ ") + in.label).c_str(), text);
            y += step;
        }
        return y - style.ItemSpacing.y + look.padding.w;
    };
    height = rows(false);
    if (draw) {
        paint_block(draw, spec.family, at, ImVec2(width, height) * scale, u * scale,
                    {.band = band * scale, .hatch = spec.manual ? band * scale : 0, .alpha = alpha});
        rows(true);
    }
    return ImVec2(width, height) * scale;
}

// Build layout's node browser: every block type in a folder per family, with a search box. Click adds
// one mid-view; drag one onto the graph to add it there (both placed clear of the others by core).
void draw_nodes_panel(State& s) {
    ImGui::Begin("Nodes");
    static std::string filter;
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "Search blocks", &filter);
    auto lower = [](std::string t) {
        std::ranges::transform(t, t.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return t;
    };
    const std::string want = lower(filter);
    auto matches = [&](const remod::NodeSpec& spec) {
        return want.empty() || lower(spec.title).find(want) != std::string::npos ||
               lower(spec.summary).find(want) != std::string::npos;
    };
    // One folder per family, the main steps first and the utilities (flow, values) last; closed until opened, open
    // while a search finds something in it. Each block is shown as it will look, at most 55% size (less if the panel
    // is short), side by side and wrapping (the panel is a wide strip along the bottom), with room around each for
    // its corner marks.
    using enum remod::Family;
    struct Folder {
        remod::Family family;
        const char* name;
    };
    static constexpr Folder folders[]{{Source, "Sources"}, {Transform, "Transforms"}, {Manual, "Your steps"},
                                      {File, "File steps"},  {Output, "Output"},        {Flow, "Flow"},
                                      {Value, "Values"}};
    const float u = unit(), margin = 10 * u;
    for (const Folder& folder : folders) {
        int count = 0;
        for (const auto& spec : remod::node_specs()) count += spec.family == folder.family && matches(spec);
        if (count == 0) continue;
        if (!want.empty()) ImGui::SetNextItemOpen(true);
        const std::string header = std::string(folder.name) + " (" + std::to_string(count) + ")###" + folder.name;
        if (!ImGui::CollapsingHeader(header.c_str())) continue;
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        float last_right = -1;  // right edge of the block before on this line; < 0: none yet
        for (const auto& spec : remod::node_specs()) {
            if (spec.family != folder.family || !matches(spec)) continue;
            const ImVec2 full = draw_block_preview(nullptr, {}, spec, s.look, 1, 1);
            const float scale = (std::min)({0.55f, (right - ImGui::GetCursorScreenPos().x - margin * 2) / full.x,
                                       ImGui::GetWindowHeight() * 0.75f / full.y});
            if (last_right >= 0 && last_right + margin * 2 + full.x * scale <= right) {
                ImGui::SameLine(0, margin * 2);  // fits beside the one before
            } else {
                ImGui::Dummy(ImVec2(0, margin * 0.5f));
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + margin);
            }
            const ImVec2 at = ImGui::GetCursorScreenPos();
            last_right = at.x + full.x * scale;
            if (ImGui::InvisibleButton(spec.type, full * scale)) s.add_type = spec.type, s.add_at.reset();
            const bool hovered = ImGui::IsItemHovered();
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {  // the graph shows a copy
                ImGui::SetDragDropPayload("remod_block", spec.type, std::strlen(spec.type) + 1);
                ImGui::EndDragDropSource();
            } else if (hovered) {
                spec_tooltip(spec);
            }
            draw_block_preview(ImGui::GetWindowDrawList(), at, spec, s.look, scale, hovered ? 1.0f : 0.8f);
        }
        ImGui::Dummy(ImVec2(0, margin));
    }
    ImGui::End();
}

// Text wrapped to `width`, whatever the position. ImGui's own wrap position is window-local and ignored unless
// positive, which the graph canvas's coordinates often aren't: blocks left of its origin didn't wrap and grew wide.
void wrapped_text(const char* text, float width, ImU32 color) {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(), at, color, text, nullptr, width);
    ImGui::Dummy(ImGui::CalcTextSize(text, nullptr, false, width));
}

// Draws its scope at `alpha` (1 as is, 0 invisible), not clickable while faded; it keeps its place in the layout.
struct Faded {
    bool on;
    explicit Faded(float alpha) : on(alpha < 1) {
        if (!on) return;
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * ImMax(alpha, 0.0f));
        ImGui::BeginDisabled();
    }
    ~Faded() {
        if (!on) return;
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
    }
    Faded(const Faded&) = delete;
    Faded& operator=(const Faded&) = delete;
};

// Draws `body` folded by `k`: as is at 1; fading out from 1 to 0.5; below that invisible and shrinking to no height
// (what follows moves up), gone at 0. Blocks then change size smoothly instead of jumping.
template <class Body>
void folding(float k, const Body& body) {
    if (k <= 0) return;
    if (k >= 1) {
        body();
        return;
    }
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImVec2 start = ImGui::GetCursorScreenPos(), max_before = window->DC.CursorMaxPos;
    {
        const Faded faded(k * 2 - 1);
        body();
    }
    // Take back the space it used, then claim only its folded share.
    const float height = ImGui::GetCursorScreenPos().y - start.y;
    window->DC.CursorMaxPos = max_before;
    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(ImVec2(0, ImMax(0.0f, height * ImMin(1.0f, k * 2) - ImGui::GetStyle().ItemSpacing.y)));
}

// Where a block's outline is (graph coordinates), so pins sit on its edges. Its size is last frame's (known once
// drawn). `dim`: a block the last run didn't reach (grey pins).
struct Outline {
    remod::Family family;
    ImVec2 min, size;
    float u;
    bool dim;
};

// One pin: its shape on the block's edge (links attach to its centre, drags start from it) beside the row's label,
// which is drawn inside the node - left-aligned for inputs, right-aligned for outputs. Rows are frame-height tall so
// labels line up with the text boxes next to them.
void draw_pin(State& s, ed::PinId id, const std::string& label, remod::PortType type, bool output, bool connected,
              float x0, float node_width, const Outline& o, PinLook look = PinLook::Port) {
    ed::BeginPin(id, output ? ed::PinKind::Output : ed::PinKind::Input);
    if (output) ImGui::SetCursorPosX(x0 + node_width - ImGui::CalcTextSize(label.c_str()).x);
    const float y = ImGui::GetCursorScreenPos().y + ImGui::GetFrameHeight() * 0.5f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label.c_str());

    const ImVec2 center(o.min.x + edge_x(o.family, o.size, y - o.min.y, output, o.u), y);
    s.pin_pos[id.Get()] = center;
    s.pin_anchor[id.Get()] = ImVec2(output ? (o.size.x > 0 ? o.min.x + o.size.x : center.x) : o.min.x, y);
    draw_pin_shape(ImGui::GetWindowDrawList(), center, type, look, connected, o.dim, o.u);
    const float r = ImGui::GetFontSize() * 0.6f;
    ed::PinPivotRect(center, center);
    ed::PinRect(center - ImVec2(r, r), center + ImVec2(r, r));
    ed::EndPin();
}

// Frames the graph without magnifying it. Zoom = fit-to-view, capped at kMaxFitZoom (above 100% text gets
// big and soft) and floored at kMinFitZoom so a wide graph stays readable; when floored, the view starts at
// the graph's left edge (where the pipeline begins) instead of its centre.
// The editor adds ~10% margin on top, so on screen these come out at roughly 90% and 55%.
constexpr float kMaxFitZoom = 1.0f, kMinFitZoom = 0.6f;

void fit_view(ed::EditorContext* editor, ImVec2 view, float min_zoom = kMinFitZoom, float max_zoom = kMaxFitZoom) {
    auto* ctx = reinterpret_cast<ed::Detail::EditorContext*>(editor);
    const ImRect content = ctx->GetContentBounds();
    if (content.GetWidth() <= 0 || content.GetHeight() <= 0 || view.x <= 0 || view.y <= 0) return;
    const float fit = ImMin(view.x / content.GetWidth(), view.y / content.GetHeight());
    const float zoom = ImClamp(fit, min_zoom, max_zoom);
    const ImVec2 size = view / zoom;
    const ImVec2 min = zoom > fit ? content.Min : content.GetCenter() - size * 0.5f;
    ctx->NavigateTo(ImRect(min, min + size), true, 0.0f);
}

// Centres the view on `center` (graph coordinates) at exactly `zoom`. The editor fits a rect grown by 10% of its
// longer side (c_NavigationZoomMargin, 0.9.3), so: a square that comes out at `zoom` after growing.
void center_view(ed::EditorContext* editor, ImVec2 view, ImVec2 center, float zoom) {
    const float half = ImMin(view.x, view.y) / (zoom * 1.1f) * 0.5f;
    reinterpret_cast<ed::Detail::EditorContext*>(editor)->NavigateTo(
        ImRect(center - ImVec2(half, half), center + ImVec2(half, half)), true, 0.25f);
}

// A routed link: straight runs joined by rounded corners, solid or dashed (`stroke`, in units `u`; moving dashes go
// from source to target). `inset` keeps both ends off the pins' centres.
void draw_route(ImDrawList* draw, const std::vector<remod::Pt>& path, float radius, float inset, const Stroke& stroke,
                float u, float width_scale = 1) {
    auto v = [](remod::Pt p) { return ImVec2(p.x, p.y); };
    draw->PathLineTo(v(path.front()) + ImVec2(inset, 0));  // every path leaves rightwards ...
    for (size_t i = 1; i + 1 < path.size(); ++i) {
        const ImVec2 a = v(path[i - 1]), c = v(path[i]), b = v(path[i + 1]);
        const float in = ImLength(c - a), out = ImLength(b - c);
        if (in < 0.01f || out < 0.01f) continue;
        const float r = ImMin(radius, ImMin(in, out) * 0.5f);
        draw->PathLineTo(c - (c - a) * (r / in));
        draw->PathBezierQuadraticCurveTo(c, c + (b - c) * (r / out));
    }
    draw->PathLineTo(v(path.back()) - ImVec2(inset, 0));  // ... and arrives from the left
    const float width = stroke.width * u * width_scale;
    if (stroke.on <= 0) {
        draw->PathStroke(stroke.color, ImDrawFlags_None, width);
        return;
    }
    const std::vector<ImVec2> points(draw->_Path.begin(), draw->_Path.end());
    draw->PathClear();
    dashed(draw, points.data(), int(points.size()), false, stroke.color, width, stroke.on * u, stroke.off * u,
           -stroke.speed * u * float(ImGui::GetTime()));
}

// Two modes, switched above the graph: Use a finished layout (fill in, run, edit images) or Build one
// (add, link, arrange blocks). Two buttons centred, the active one highlighted.
void draw_mode_switch(State& s) {
    struct Mode { const char* label; bool build; const char* tip; };
    static constexpr Mode modes[]{
        {"Use layout", false, "Fill in the fields, Run, edit the images. The blocks and links stay as they are."},
        {"Build layout", true, "Add, remove, link and arrange blocks to make or change a layout."}};
    const ImGuiStyle& style = ImGui::GetStyle();
    float width = style.ItemSpacing.x;
    for (const auto& m : modes) width += ImGui::CalcTextSize(m.label).x + style.FramePadding.x * 2;
    const ImVec2 line_start = ImGui::GetCursorPos();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImMax(0.0f, (ImGui::GetContentRegionAvail().x - width) * 0.5f));
    for (const auto& m : modes) {
        const bool active = s.build_mode == m.build;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_ButtonActive]);
        if (ImGui::Button(m.label) && !active) {
            s.build_mode = m.build;
            remember_paths(s);
        }
        if (active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.tip);
        ImGui::SameLine();
    }
    // At the left end: reopen the Pipeline panel if it was closed (Use layout; Run is there), then links with no
    // clean route, drawn as numbered ends (portals). Tidy up usually gives them one.
    const ImVec2 after_switch = ImGui::GetCursorPos();
    ImVec2 left = line_start;
    if (!s.build_mode && !s.show_pipeline) {
        ImGui::SetCursorPos(left);
        if (ImGui::Button("Pipeline")) s.show_pipeline = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the Pipeline panel again (Run, paths, log).");
        left.x = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + style.ItemSpacing.x;
        ImGui::SetCursorPos(after_switch);
    }
    if (const auto portals = std::ranges::count(s.routes.portals, 1); portals > 0) {
        const ImVec2 after = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(left.x, line_start.y + style.FramePadding.y));
        ImGui::TextColored(kAmber, "%d link%s without a clean route", int(portals), portals == 1 ? "" : "s");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Shown as numbered ends instead of a line: the blocks are too close or in the way.\n%s",
                              s.build_mode ? "Tidy up (right) or move the blocks apart." : "Switch to Build layout to tidy up.");
        ImGui::SetCursorPos(after);
    }
    // At the right end of the same line: the zoom (Far / Near), Tidy up (Build layout) and the blocks' description
    // texts, which take a lot of room once known.
    const char* label = "Descriptions";
    const char* tidy = "Tidy up";
    const char* zooms[] = {"Far", "Near"};
    float box = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize(label).x;
    for (const char* z : zooms) box += ImGui::CalcTextSize(z).x + style.FramePadding.x * 2;
    box += style.ItemSpacing.x * 2;
    if (s.build_mode) box += ImGui::CalcTextSize(tidy).x + style.FramePadding.x * 2 + style.ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImMax(0.0f, ImGui::GetContentRegionAvail().x - box));
    // Segmented: the current zoom filled with the accent. Zooming past 55% / 65% switches too.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, style.ItemSpacing.y));
    for (int i = 0; i < 2; ++i) {
        const bool active = s.overview == (i == 0);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(pal::accent));
        if (ImGui::Button(zooms[i])) s.zoom_request = i + 1;
        if (active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(i == 0 ? "Far: every block as a small symbol, to see the whole graph."
                                     : "Near: full blocks with their fields, centred on the selected block.");
        ImGui::SameLine();
    }
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(style.ItemSpacing.x, 0));
    ImGui::SameLine();
    if (s.build_mode) {
        if (ImGui::Button(tidy)) s.tidy_requested = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Line the blocks up in columns, in step order, each level with the blocks feeding it.");
        ImGui::SameLine();
    }
    if (ImGui::Checkbox(label, &s.show_descriptions)) remember_paths(s);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show what each block does under its title. When off, hover a block's title to see it.");

    // Use layout: what the links' styles mean (the run's progress along them).
    if (s.build_mode) return;
    const float u = unit(), sample = u * 22;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (const auto& [state, name] : {std::pair{LinkState::Done, "Done"}, {LinkState::Active, "Waiting for you"},
                                      {LinkState::Failed, "Failed"}, {LinkState::Pending, "Not reached"}}) {
        const ImVec2 at = ImGui::GetCursorScreenPos() + ImVec2(0, ImGui::GetTextLineHeight() * 0.5f);
        draw_route(draw, {{at.x, at.y}, {at.x + sample, at.y}}, 0, 0, stroke_for(state, pal::text), u);
        ImGui::Dummy(ImVec2(sample, ImGui::GetTextLineHeight()));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", name);
        ImGui::SameLine(0, style.ItemSpacing.x * 3);
    }
    ImGui::NewLine();
}

// Where a block got to in the last run, in Use layout once there's been a run (blocks the run didn't report weren't
// reached); nothing otherwise (Build look).
std::optional<remod::NodeState> run_state(const State& s, int id) {
    if (s.build_mode || s.statuses.empty()) return std::nullopt;
    const auto it = s.statuses.find(id);
    return it == s.statuses.end() ? remod::NodeState::NotReached : it->second.state;
}

// A block's paint (handoff §5): a dark tint of its run state's colour (Far the whole block, Near the header band),
// outlined in that colour.
BlockPaint block_paint(std::optional<remod::NodeState> state, bool far_view, bool selected, bool hovered) {
    using enum remod::NodeState;
    BlockPaint p;
    p.marks = hovered ? pal::accent : pal::faint;
    if (state && *state != NotReached) {
        const ImU32 color = *state == Done ? pal::done : *state == Waiting ? pal::waiting : pal::failed;
        const ImU32 tint = mix(pal::block, color, 0.22f);
        if (far_view) p.fill = tint;
        else p.band_fill = tint;
        p.stroke = color;
        p.width = *state == Done ? 1.5f : 2;
    }
    if (state == NotReached) p.stroke = pal::faint, p.dashed = true, p.alpha = far_view ? 0.55f : 1;
    if (selected) p.stroke = pal::accent, p.width = 2, p.dashed = false, p.selected = true;
    return p;
}

// `t` cut to `max_width` with "..." if it's wider.
std::string fit_text(ImFont* f, float size, std::string t, float max_width) {
    auto width = [&](const std::string& x) { return font_or(f)->CalcTextSizeA(size, FLT_MAX, 0, x.c_str()).x; };
    if (width(t) <= max_width) return t;
    while (t.size() > 1 && width(t + "...") > max_width) t.pop_back();
    return t + "...";
}

// A block's key value, shown under it in Far: its first typed value (a path by its file name, text in quotes).
std::string key_value(const remod::Graph& g, const remod::Node& n, const remod::NodeSpec& spec) {
    auto shown = [](const std::string& v, bool path) {
        const std::string name = path ? std::filesystem::path(v).filename().string() : "\"" + v + "\"";
        return name.empty() ? v : name;
    };
    auto value = [&](const char* name) {
        const auto it = n.params.find(name);
        return it == n.params.end() ? std::string() : it->second;
    };
    for (const auto& in : spec.inputs)
        if ((in.widget == remod::Widget::Text || in.widget == remod::Widget::Path) && g.links_into(n.id, in.name).empty())
            if (const std::string v = value(in.name); !v.empty()) return shown(v, in.widget == remod::Widget::Path);
    for (const auto& out : spec.outputs)
        if (out.field)
            if (const std::string v = value(out.field); !v.empty()) return shown(v, out.path != remod::PathKind::None);
    return "";
}

// Far zoom (handoff §3): a block as its family's symbol, the title inside and its key value under it (a failed block:
// the reason), ports spread down its edges without labels (hover for the description). A Split is a dot that all its
// links meet at. Drawn inside the block (after BeginNode, no padding); returns the outline's size (the block also
// holds the key line under it).
ImVec2 draw_far_block(State& s, const remod::Node& n, const remod::NodeSpec& spec, const remod::NodeStatus* status,
                      std::optional<remod::NodeState> state, float u, std::string& hint) {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool dot = spec.family == remod::Family::Flow;
    // Ports: link-only inputs, linked fields (a wired field becomes a port) and outputs; when building, a multiple's
    // empty slot too. A destination typed in place (not flipped) is only its output.
    struct Port {
        ed::PinId id;
        remod::PortType type;
        bool wired;
        PinLook look;
    };
    std::vector<Port> ports[2];  // inputs, outputs
    for (size_t slot = 0; slot < spec.inputs.size(); ++slot) {
        const remod::InputSpec& in = spec.inputs[slot];
        if (in.result && !remod::is_flipped(n, in.name)) continue;
        const auto linked = s.graph.links_into(n.id, in.name);
        const remod::PortType type =
            linked.empty() ? in.type
                           : s.graph.output_type(s.graph.links[linked[0]].from_node, s.graph.links[linked[0]].from_port);
        if (in.multiple) {
            for (size_t row = 0; row < linked.size() && row < kRows; ++row)
                ports[0].push_back({pin_id(n.id, false, slot, row), type, true, PinLook::Port});
            if (s.build_mode) ports[0].push_back({pin_id(n.id, false, slot, linked.size()), in.type, false, PinLook::Add});
        } else if (!linked.empty() || !in.editable()) {
            ports[0].push_back({pin_id(n.id, false, slot), type, !linked.empty(), PinLook::Port});
        }
    }
    for (size_t i = 0; i < spec.outputs.size(); ++i) {
        const remod::PortSpec& out = spec.outputs[i];
        const auto dest = std::ranges::find_if(
            spec.inputs, [&](const remod::InputSpec& in) { return in.result && out.name == std::string_view(in.result); });
        if (dest != spec.inputs.end() && remod::is_flipped(n, dest->name)) continue;
        const remod::PortType type = s.graph.output_type(n.id, out.name);
        if (out.multiple) {
            const auto linked = s.graph.links_from(n.id, out.name);
            for (size_t row = 0; row < linked.size() && row < kRows; ++row)
                ports[1].push_back({pin_id(n.id, true, i, row), type, true, PinLook::Port});
            if (s.build_mode) ports[1].push_back({pin_id(n.id, true, i, linked.size()), type, false, PinLook::Add});
        } else {
            ports[1].push_back({pin_id(n.id, true, i), type, s.graph.is_connected(n.id, out.name, true), PinLook::Port});
        }
    }

    const float title_size = 12.5f * u, key_size = 9.5f * u;
    const std::string title = remod::block_title(n);
    const bool failed = state == remod::NodeState::Failed && status;
    const std::string key = dot ? "" : failed ? status->message : key_value(s.graph, n, spec);
    ImVec2 size(12 * u, 12 * u);
    float title_h = 0;
    if (!dot) {
        title_h = centered_text(nullptr, g_title_font, title_size, {}, 92 * u, 0, title.c_str());
        size = ImVec2(116 * u, (std::max)({40 * u, float((std::max)(ports[0].size(), ports[1].size()) + 1) * 9 * u,
                                         title_h + 10 * u}));
    }
    const float alpha = state == remod::NodeState::NotReached ? 0.55f : 1;
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    for (int side = 0; side < 2; ++side) {
        for (size_t i = 0; i < ports[side].size(); ++i) {
            const Port& p = ports[side][i];
            const float y = dot ? size.y / 2 : size.y * float(i + 1) / float(ports[side].size() + 1);
            const ImVec2 c = at + ImVec2(dot ? size.x / 2 : edge_x(spec.family, size, y, side == 1, u), y);
            const float r = (dot ? 6 : 4.5f) * u;
            ed::BeginPin(p.id, side ? ed::PinKind::Output : ed::PinKind::Input);
            // A zero-size item of its own: ImGui sizes an empty group to reach the last item drawn before it (another
            // block's), which stretched every Far block over the graph, so no link had a clean route.
            ImGui::Dummy(ImVec2(0, 0));
            ed::PinPivotRect(c, c);
            // A dot's inputs take its left half, its outputs the right; links all meet at its centre.
            ed::PinRect(c - ImVec2(dot && side ? 0 : r, r), c + ImVec2(dot && !side ? 0 : r, r));
            ed::EndPin();
            s.pin_pos[p.id.Get()] = c;
            s.pin_anchor[p.id.Get()] = ImVec2(at.x + (side ? size.x : 0), c.y);
            if (!dot) draw_pin_shape(d, c, p.type, p.look, p.wired, state == remod::NodeState::NotReached, u);
        }
    }
    ImGui::PopStyleVar();
    // The block's area, after the pins: each pin is an (empty) ImGui group that moves the cursor down a little.
    ImGui::SetCursorScreenPos(at);
    ImGui::Dummy(ImVec2(size.x, size.y + (key.empty() ? 0 : 4 * u + key_size)));
    if (ImGui::IsItemHovered())
        hint = title + (title != spec.title ? std::string(" (") + spec.title + ")" : "") + "\n" + spec.summary +
               (status ? "\n\n" + status_text(*status) : "");
    if (!dot) {
        centered_text(d, g_title_font, title_size, at + ImVec2(size.x / 2, (size.y - title_h) / 2), 92 * u,
                      with_alpha(pal::text, alpha), title.c_str());
        const std::string shown = fit_text(g_mono_font, key_size, key, size.x + 24 * u);
        const float w = font_or(g_mono_font)->CalcTextSizeA(key_size, FLT_MAX, 0, shown.c_str()).x;
        d->AddText(font_or(g_mono_font), key_size, at + ImVec2((size.x - w) / 2, size.y + 4 * u),
                   with_alpha(failed ? pal::failed : pal::muted, alpha), shown.c_str());
    }
    return size;
}

// The minimap (Near, handoff §3): the whole graph small in the graph's bottom-right corner, the visible part framed;
// a click centres the view there, and selects the block clicked. A child window over the graph, so the editor
// doesn't see the click.
void draw_minimap(State& s, ImVec2 view_min, ImVec2 view_size) {
    if (s.overview || s.mini.empty()) return;
    ImVec2 lo(FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX);
    for (const auto& b : s.mini) lo = ImMin(lo, b.min), hi = ImMax(hi, b.min + b.size);
    const float font = ImGui::GetFontSize(), u = unit(), inset = 18 * u;
    const ImVec2 graph = ImMax(hi - lo, ImVec2(1, 1));
    const float m = ImMin(220 * u / graph.x, view_size.y * 0.35f / graph.y);  // map px per graph unit
    const ImVec2 map = graph * m, caption(map.x, font * 0.9f + 4 * u);
    if (map.x + inset * 2 > view_size.x || map.y + caption.y + inset * 2 > view_size.y) return;  // no room
    const ImVec2 at = view_min + view_size - map - ImVec2(inset, inset);
    ImGui::SetCursorScreenPos(at - ImVec2(0, caption.y));
    ImGui::BeginChild("##minimap", ImVec2(map.x, map.y + caption.y), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* d = ImGui::GetWindowDrawList();
    d->AddText(font_or(g_mono_font), font * 0.8f, at - ImVec2(0, caption.y), pal::muted,
               "OVERVIEW \xC2\xB7 CLICK TO JUMP");
    d->AddRectFilled(at, at + map, pal::bg);
    auto to_map = [&](ImVec2 p) { return at + (p - lo) * m; };
    for (const auto& b : s.mini) {
        auto shape = outline(b.family, to_map(b.min), b.size * m, unit() * m);
        d->AddConcavePolyFilled(shape.data(), int(shape.size()), b.id == s.selected ? pal::accent : pal::edge);
    }
    d->PushClipRect(at, at + map, true);
    d->AddRect(to_map(s.mini_view_min), to_map(s.mini_view_max), pal::text, 0, 0, 1.25f);
    d->PopClipRect();
    d->AddRect(at, at + map, pal::line);
    corner_marks(d, at, at + map, 3.5f * u, 0, pal::faint, 1);
    ImGui::SetCursorScreenPos(at);
    if (ImGui::InvisibleButton("map", map)) {
        const ImVec2 p = lo + (ImGui::GetMousePos() - at) / m;
        s.jump_to = p;
        for (const auto& b : s.mini)
            if (p.x >= b.min.x && p.y >= b.min.y && p.x <= b.min.x + b.size.x && p.y <= b.min.y + b.size.y) s.focus_node = b.id;
    }
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::EndChild();
}

// The graph without block positions: what values, previews and thumbnails depend on (moving a block changes none).
remod::Graph shape_of(const remod::Graph& g) {
    remod::Graph out = g;
    for (auto& n : out.nodes) n.x = n.y = 0;
    return out;
}

void release(State::PreviewSet& set) {
    for (auto& [_, image] : set.images) image.srv->Release();
    set.images.clear();
}

// Starts working out `ids`' previews for `set` (in the background) when the graph or the ids have changed, and takes
// the finished ones onto the GPU.
void update_previews(State& s, State::PreviewSet& set, std::vector<int> ids) {
    using namespace std::chrono_literals;
    if (set.job.valid() && set.job.wait_for(0s) == std::future_status::ready) {
        const auto results = set.job.get();
        release(set);  // gone, or about to be replaced
        set.none.clear();
        for (const auto& [id, made] : results) {
            const auto& [result, why] = made;
            if (!result) {
                set.none[id] = why;
                continue;
            }
            const remod::Bgra& img = result->image;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = img.width;
            desc.Height = img.height;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            const D3D11_SUBRESOURCE_DATA data{img.pixels.data(), img.width * 4, 0};
            ID3D11Texture2D* tex = nullptr;
            State::Thumb thumb{nullptr, float(img.width), float(img.height), result->found};
            if (SUCCEEDED(g_device->CreateTexture2D(&desc, &data, &tex))) {
                g_device->CreateShaderResourceView(tex, nullptr, &thumb.srv);
                tex->Release();
            }
            if (thumb.srv) set.images[id] = thumb;
        }
    }
    if (set.job.valid()) return;  // one job at a time
    remod::Graph shape = shape_of(s.graph);
    if (shape == set.of && s.graph_path == set.of_path && ids == set.of_ids) return;
    set.of = std::move(shape);
    set.of_path = s.graph_path;
    set.of_ids = ids;
    if (ids.empty()) {
        release(set);
        set.none.clear();
        return;
    }
    const auto p = std::ranges::find(s.profiles, s.graph.profile, &remod::Profile::id);
    set.job = std::async(std::launch::async, [graph = s.graph, ids, shrunk = set.shrunk, side = set.side,
                                                   profile = p == s.profiles.end() ? std::optional<remod::Profile>()
                                                                                   : std::optional<remod::Profile>(*p),
                                                   base = std::filesystem::absolute(s.graph_path).parent_path()] {
        const remod::ImageLoader load = [&](const std::filesystem::path& file) -> std::optional<remod::ImagePreview> {
            std::error_code ec;
            const auto time = std::filesystem::last_write_time(file, ec);
            if (ec) return std::nullopt;
            auto& cached = (*shrunk)[file.string()];
            if (cached.time != time || cached.image.image.pixels.empty()) {
                // A texture (x.tex.<version>): the mip that fits, decoded (nothing needs exporting first). Most RE4R
                // UI textures have one mip only, so it's shrunk to thumbnail size like an image.
                unsigned real_width = 0;
                remod::Bgra full;
                if (remod::file_kind(file.filename().string()) == remod::FileKind::Texture) {
                    full = remod::decode_tex(file, side, &real_width);
                } else {
                    full = remod::load_image(file);
                    real_width = full.width;
                }
                const float shrink = ImMin(1.0f, float(side) / float(ImMax(full.width, full.height)));
                if (shrink < 1)
                    full = remod::resize_image(full, ImMax(1u, unsigned(full.width * shrink)),
                                               ImMax(1u, unsigned(full.height * shrink)), remod::Fit::Stretch);
                const float scale = real_width ? float(full.width) / float(real_width) : 1.0f;
                cached = {time, {std::move(full), scale}};
            }
            return cached.image;
        };
        const remod::RunValues preview = remod::preview_values(graph, base);
        std::map<int, std::pair<std::optional<remod::ImagePreview>, std::string>> out;
        for (const int id : ids) {
            std::string why;
            auto made = remod::preview_image(graph, preview, id, base, side, load, profile ? &*profile : nullptr, &why);
            out[id] = {std::move(made), std::move(why)};
        }
        return out;
    });
}

// Every image block's thumbnail, and the popped-out ones larger.
void update_thumbs(State& s) {
    std::vector<int> all, popped;
    for (const auto& n : s.graph.nodes)
        if (const remod::NodeSpec* spec = remod::find_spec(n.type); spec && spec->thumbnail) {
            all.push_back(n.id);
            if (spec->view_size || s.popouts.contains(n.id) || (s.viewer_on_block && s.viewer_block == n.id))
                popped.push_back(n.id);  // shown larger: worked out larger
        }
    update_previews(s, s.thumbs, all);
    update_previews(s, s.big, popped);
}

// A picture over a checkerboard (its transparency shows), within the clip rect.
void checkered_image(ImDrawList* d, ID3D11ShaderResourceView* srv, ImVec2 at, ImVec2 size) {
    const float cell = ImGui::GetFontSize() * 0.5f;
    const ImRect clip(d->GetClipRectMin(), d->GetClipRectMax());
    const ImVec2 from = ImMax(at, clip.Min), to = ImMin(at + size, clip.Max);  // only the visible cells
    for (float y = std::floor((from.y - at.y) / cell) * cell; at.y + y < to.y; y += cell)
        for (float x = std::floor((from.x - at.x) / cell) * cell; at.x + x < to.x; x += cell)
            d->AddRectFilled(at + ImVec2(x, y), at + ImVec2(ImMin(x + cell, size.x), ImMin(y + cell, size.y)),
                             (int(x / cell) + int(y / cell)) % 2 ? pal::raised : pal::line);
    d->AddImage(ImTextureRef(srv), at, at + size);
}

// An image block's preview filling the rest of the window, zoomable: the larger one (State::big) once it's worked out,
// the thumbnail meanwhile. It follows the values live, like the thumbnail.
void block_picture(State& s, int id, ZoomPan& view) {
    const auto big = s.big.images.find(id), thumb = s.thumbs.images.find(id);
    const State::Thumb* shown = big != s.big.images.end() ? &big->second
                                : thumb != s.thumbs.images.end() ? &thumb->second
                                                                 : nullptr;
    if (const auto none = s.big.none.find(id); none != s.big.none.end())
        ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "No preview: %s",
                           none->second.empty() ? "an input image or texture isn't known yet, or a value isn't set"
                                                : none->second.c_str());
    else if (big == s.big.images.end())
        ImGui::TextDisabled("Working out the larger preview...");
    if (!shown) return;
    const ZoomPlace at = zoom_area("##picture", shown->width, shown->height, view);
    ImDrawList* d = ImGui::GetWindowDrawList();
    d->PushClipRect(at.area_min, at.area_max, true);
    checkered_image(d, shown->srv, at.corner, at.size);
    d->PopClipRect();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Worked out at %.0fx%.0f from a smaller copy of the image; Run makes the full-size one.\n"
                          "Wheel to zoom, drag to move, double-click to fit.",
                          shown->width, shown->height);
}

// The viewer's contents while it shows a block (Browser::show_in_viewer).
void draw_block_view(State& s) {
    const remod::Node* n = s.graph.find(s.viewer_block);
    if (!n) {
        ImGui::TextDisabled("That block was removed.");
        return;
    }
    if (ImGui::SmallButton("Pop out")) s.popouts.try_emplace(n->id);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open it in its own window, to enlarge it further.");
    ImGui::SameLine();
    ImGui::TextUnformatted(remod::block_title(*n).c_str());
    block_picture(s, n->id, s.viewer_view);
}

// Image blocks popped out (a thumbnail clicked in Build layout, or the viewer's Pop out): a window each, any size,
// zoomable. Closed with its X, or with the block.
void draw_popouts(State& s) {
    for (auto it = s.popouts.begin(); it != s.popouts.end();) {
        const remod::Node* n = s.graph.find(it->first);
        bool open = n != nullptr;
        if (open) {
            const float font = ImGui::GetFontSize();
            ImGui::SetNextWindowSize(ImVec2(font * 32, font * 32), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImGui::GetMousePos(), ImGuiCond_FirstUseEver);
            const std::string title = remod::block_title(*n) + "###popout " + std::to_string(n->id);
            // Floats, never docks: docked into the Graph's slot, dragging the slot moved the graph with it.
            if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoDocking)) block_picture(s, n->id, it->second);
            ImGui::End();
        }
        it = open ? std::next(it) : s.popouts.erase(it);
    }
}

// An image block's thumbnail, fitted to the block's width and at most eight lines tall, over a checkerboard (its
// transparency shows); or why there's none yet.
// A Preview block's picture size on the graph: its Size field (px at 100% zoom, for the default 16 px font).
float picture_size(const remod::Node& n, const remod::NodeSpec& spec) {
    float v = 320;
    try {
        v = std::stof(n.params.at(spec.view_size));
    } catch (const std::exception&) {
    }
    return ImClamp(v, 60.0f, 4000.0f) * ImGui::GetFontSize() / 16;
}

// `height`: the most it may take; `large`: the larger preview (State::big) when it's there (a Preview block).
void draw_thumb(State& s, int node, float width, float height, bool large, std::string& hint) {
    auto it = s.thumbs.images.find(node);
    if (const auto big = s.big.images.find(node); large && big != s.big.images.end()) it = big;
    if (it == s.thumbs.images.end()) {
        const auto none = s.thumbs.none.find(node);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);  // within the block
        if (none == s.thumbs.none.end()) ImGui::TextDisabled("Working out the preview...");
        else if (none->second.empty()) ImGui::TextDisabled("No preview: an input image or texture isn't known yet, or a value isn't set.");
        else ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "No preview: %s", none->second.c_str());
        ImGui::PopTextWrapPos();
        return;
    }
    const State::Thumb& t = it->second;
    const float k = ImMin(width / t.width, height / t.height);
    const ImVec2 size(t.width * k, t.height * k), at = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##thumb", size);
    if (ImGui::IsItemClicked()) {
        if (s.build_mode) {
            s.popouts.try_emplace(node);  // no viewer in Build layout: its own window (again: the same one)
        } else {
            s.viewer_block = node;  // Use layout: in the viewer
            s.viewer_view = {};
            s.want_viewer = true;
        }
    }
    checkered_image(ImGui::GetWindowDrawList(), t.srv, at, size);
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        hint = "The result, worked out from a small copy of the image. It follows the values as you change them; Run "
               "makes the full-size one. Click to see it larger" +
               std::string(s.build_mode ? ", in its own window." : " in the viewer (its Pop out button gives it a window).");
    }
}

void draw_canvas(State& s, ed::EditorContext* editor) {
    ImGui::Begin("Graph");
    draw_mode_switch(s);
    const ImVec2 view_size = ImGui::GetContentRegionAvail();
    const ImVec2 view_center = ImGui::GetCursorScreenPos() + view_size * 0.5f;
    ed::SetCurrentEditor(editor);
    // Use layout: blocks stay where they are. The editor has no per-node lock, so dragging moves to a mouse button
    // that's rarely used. ponytail: the side (X2) button still drags there; patch AcceptDrag if that matters.
    const_cast<ed::Config&>(ed::GetConfig(editor)).DragButtonIndex = s.build_mode ? 0 : 4;
    ed::Begin("canvas");

    // Rasterize node text at the on-screen zoom so it stays sharp when zoomed. Quantized to 1/8 steps so the
    // font cache doesn't get a new size every frame of a zoom animation.
    const float zoom = reinterpret_cast<ed::Detail::EditorContext*>(editor)->GetView().Scale;
    const float old_density = ImGui::GetFontRasterizerDensity();
    ImGui::SetFontRasterizerDensity(old_density * ImClamp(std::round(zoom * 8.0f) / 8.0f, 0.25f, 4.0f));

    if (s.push_positions) {
        for (const auto& n : s.graph.nodes) ed::SetNodePosition(n.id, ImVec2(n.x, n.y));
        s.push_positions = false;
    }

    const float font = ImGui::GetFontSize();
    const ImGuiStyle& style = ImGui::GetStyle();
    s.pin_pos.clear();
    s.pin_anchor.clear();
    const float button_width = ImGui::CalcTextSize("...").x + style.FramePadding.x * 2;
    const ImVec4 padding = ed::GetStyle().NodePadding;  // x = left, z = right
    s.look = {padding};
    std::string hovered_hint;  // tooltip drawn after the nodes, outside the canvas transform
    // Zoomed out: Far (handoff §3), every block a small symbol. Going there, a block first folds down to its title,
    // status and linked rows (`detail` eases from 1 to 0 over a quarter second, so blocks and lines change smoothly),
    // then becomes its symbol; coming back it unfolds. The switch has some slack (55% / 65% zoom) so it doesn't
    // flicker at the threshold. Far blocks are drawn at two units: at about the handoff's size on screen when zoomed
    // out that far.
    if (zoom < 0.55f) s.overview = true;
    else if (zoom > 0.65f) s.overview = false;
    const float ease = ImGui::GetIO().DeltaTime / 0.25f;
    s.detail = s.overview ? ImMax(0.0f, s.detail - ease) : ImMin(1.0f, s.detail + ease);
    const float detail = s.detail;
    const bool far_view = detail <= 0;
    const float u = unit(), block_u = far_view ? 2 * u : u;

    ed::NodeId selected_node;
    s.selected = ed::GetSelectedObjectCount() == 1 && ed::GetSelectedNodes(&selected_node, 1) ? int(selected_node.Get()) : 0;
    s.mini.clear();
    std::string inputs = s.graph_path + "\n" + std::to_string(s.graph.links.size());
    for (const auto& n : s.graph.nodes)
        for (const auto& [key, value] : n.params) inputs += "\n" + std::to_string(n.id) + key + "=" + value;
    if (inputs != s.dest_inputs || ImGui::GetTime() - s.dest_checked > 1.5) {
        s.dest_warnings = remod::destination_warnings(s.graph, std::filesystem::absolute(s.graph_path).parent_path());
        s.dest_inputs = std::move(inputs);
        s.dest_checked = ImGui::GetTime();
    }
    if (remod::Graph shape = shape_of(s.graph); shape != s.preview_of || s.graph_path != s.preview_path) {
        s.preview = remod::preview_values(s.graph, std::filesystem::absolute(s.graph_path).parent_path());
        s.preview_of = std::move(shape);
        s.preview_path = s.graph_path;
    }
    bool open_choice_menu = false, open_rename = false;
    for (auto& n : s.graph.nodes) {
        const remod::NodeSpec* spec = remod::find_spec(n.type);
        const auto status_it = s.statuses.find(n.id);
        const remod::NodeStatus* status = status_it != s.statuses.end() ? &status_it->second : nullptr;
        const bool manual = spec && spec->manual;
        const bool edit_done = manual && n.params.contains("done");
        const auto warning = s.dest_warnings.find(n.id);
        const bool warned = warning != s.dest_warnings.end();
        // The outline is the block's family (paint_block, after the block); its paint says where the last run got
        // to (Use layout). Far blocks have no padding: their symbol is the whole block.
        const auto state = run_state(s, n.id);
        const remod::Family family = spec ? spec->family : remod::Family::Transform;
        const ImVec2 last_size = ed::GetNodeSize(n.id);  // pins go on its edges (last frame's: known once drawn)
        ed::PushStyleVar(ed::StyleVar_NodePadding, far_view ? ImVec4(0, 0, 0, 0) : padding);
        ed::BeginNode(n.id);
        ImGui::PushID(n.id);
        const float top = ImGui::GetCursorScreenPos().y - (far_view ? 0 : padding.y);
        float band = 0;   // Near: the header's height
        ImVec2 far_size;  // Far: the symbol's size
        // A utility (Split, Text) is a small, quiet block: narrow, a smaller plain title, its description only as
        // the title's tooltip, no type name. The main steps get the room.
        const bool utility = spec && spec->utility;
        const bool fields = spec && (std::ranges::any_of(spec->inputs, &remod::InputSpec::editable) ||
                                     std::ranges::any_of(spec->outputs, [](const auto& o) { return o.field != nullptr; }));
        const float label_width = utility ? font * 4 : font * 7, field_width = utility ? font * 8 : font * 14;
        float node_width = utility && !fields ? font * 7 : label_width + field_width + style.ItemSpacing.x + button_width;
        if (spec && spec->view_size) node_width = ImMax(node_width, picture_size(n, *spec));  // as wide as its picture
        const float x0 = ImGui::GetCursorPosX();
        const float left_edge = ImGui::GetCursorScreenPos().x - padding.x;  // node border, where pins sit
        const Outline o{family, ImVec2(left_edge, top), last_size, u, state == remod::NodeState::NotReached};
        if (!spec) {
            ImGui::Text("%s (unknown node type)", n.type.c_str());
        } else if (far_view) {
            far_size = draw_far_block(s, n, *spec, status, state, block_u, hovered_hint);
        } else {
            // The header: a large title, readable without zooming in, with the type at its right (Build layout
            // only) or, after a run, where the block got to (DONE / YOUR STEP / FAILED); filled by that state.
            // Below it small: the description (if shown) and the last run's status. A block the user named ("Mod
            // Output Folder") keeps what it is at the title's right, in both layouts unless a state is shown there.
            const std::string title = remod::block_title(n);
            const bool named = title != spec->title;
            const char* state_label = state == remod::NodeState::Done      ? "DONE"
                                      : state == remod::NodeState::Waiting ? "YOUR STEP"
                                      : state == remod::NodeState::Failed  ? "FAILED"
                                                                           : nullptr;
            const char* type = state_label ? nullptr
                               : named     ? spec->title
                               : s.build_mode && !utility ? n.type.c_str()
                                                          : nullptr;
            const float label_size = 9 * u;
            const float side_width = state_label ? font_or(g_mono_font)->CalcTextSizeA(label_size, FLT_MAX, 0, state_label).x
                                      : type      ? ImGui::CalcTextSize(type).x
                                                  : 0;
            const ImVec2 title_at = ImGui::GetCursorScreenPos();
            // A utility's title is smaller, in grey.
            ImGui::PushFont(g_title_font, ImGui::GetStyle().FontSizeBase * (utility ? 1.1f : 1.6f));
            const float title_size = ImGui::GetFontSize();
            const ImU32 title_col = utility ? pal::muted : pal::text;
            wrapped_text(title.c_str(), node_width - (side_width > 0 ? side_width + style.ItemSpacing.x : 0), title_col);
            ImGui::PopFont();
            band = ImGui::GetItemRectMax().y + style.ItemSpacing.y * 0.5f - top;
            if (ImGui::IsItemHovered()) {  // the description (when hidden), and how to name the block
                const bool summary = !s.show_descriptions || utility || detail < 1;
                hovered_hint = (summary ? std::string(spec->summary) + "\n\n" : std::string()) +
                               "Double-click the title to name this block.";
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    s.rename_node = n.id;
                    s.rename_text = named ? title : std::string();
                    open_rename = true;
                }
            }
            if (type && detail > 0)  // on the title's first line, bottoms level
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(title_at.x + node_width - side_width, title_at.y + title_size - font),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled, detail), type);
            if (state_label)  // level with the title's first line
                ImGui::GetWindowDrawList()->AddText(
                    font_or(g_mono_font), label_size,
                    ImVec2(title_at.x + node_width - side_width, title_at.y + (title_size - label_size) * 0.5f),
                    ImGui::GetColorU32(state_color(*state)), state_label);
            if (s.show_descriptions && !utility)
                folding(detail, [&] { wrapped_text(spec->summary, node_width, ImGui::GetColorU32(ImGuiCol_TextDisabled)); });
            if (status) wrapped_text(status_text(*status).c_str(), node_width, ImGui::GetColorU32(state_color(status->state)));
            if (warned) {
                wrapped_text(warning->second.c_str(), node_width, ImGui::GetColorU32(kAmber));
                if (ImGui::IsItemHovered()) hovered_hint = warning->second + "\nOverwrite mode decides what a run does.";
            }

            // One input: a pin and its field, or (multiple) one row per link plus a row to connect the next one.
            // Where a link comes from is in the row's tooltip (the line shows it too). The overview folds away
            // everything but linked rows.
            // Build layout: a destination row's <> moves its one circle to the other side. Right (the default): typed
            // here, the result goes on. Left: a link sets it (e.g. a Value). Core decides what that changes.
            const float flip_width = button_width + style.ItemSpacing.x;  // "<>" is about as wide as "..."
            auto flip_button = [&](const remod::InputSpec& in) {
                if (!s.build_mode || !in.result) return;
                ImGui::SameLine();
                const bool left = remod::is_flipped(n, in.name);
                if (ImGui::SmallButton("<>")) s.graph.flip(n.id, in.name);
                if (ImGui::IsItemHovered())
                    hovered_hint = left ? "Circle on the left: a link sets this, e.g. a Value. Click to type it here and "
                                          "pass the result on instead (circle on the right)."
                                        : "Circle on the right: typed here, and the result goes on. Click to set it "
                                          "from a link instead, e.g. a Value (circle on the left).";
            };
            auto draw_input = [&](size_t slot) {
                const remod::InputSpec& in = spec->inputs[slot];
                const std::vector<size_t> linked = s.graph.links_into(n.id, in.name);
                // A pass-through's pin takes the colour of what comes in.
                const remod::PortType pin_type =
                    in.type == remod::PortType::Any && !linked.empty()
                        ? s.graph.output_type(s.graph.links[linked[0]].from_node, s.graph.links[linked[0]].from_port)
                        : in.type;
                // A link's tooltip: the value it holds (the last run's, or a Value's), else the input's hint; its source.
                auto from = [&](size_t link) {
                    bool from_run = false;
                    const std::string held = remod::link_value(s.graph, s.preview, s.values, link, &from_run);
                    return (held.empty() ? std::string(in.hint) : held + (from_run ? "\n(as of the last run)" : "")) +
                           "\nFrom: " + source_of(s.graph, s.graph.links[link]);
                };
                ImGui::PushID(in.name);
                if (in.multiple) {  // one row per link, plus (when building) an empty row to connect the next one
                    const bool numbered = std::string(in.name) == "parts";  // Text node: rows are {1}, {2}, ...
                    const size_t rows = linked.size() + (s.build_mode || linked.empty() ? 1 : 0);
                    for (size_t row = 0; row < rows && row < kRows; ++row) {
                        const std::string n_str = std::to_string(row + 1);
                        std::string label = numbered ? "{" + n_str + "}" : std::string(in.label) + " " + n_str;
                        if (row == linked.size())
                            label = s.build_mode ? "+ " + (numbered ? "{" + n_str + "}" : std::string(in.label))
                                                 : std::string(in.label) + ": none connected";
                        folding(row < linked.size() ? 1.0f : detail, [&] {
                            draw_pin(s, pin_id(n.id, false, slot, row), label, pin_type, false, row < linked.size(), x0,
                                     node_width, o, row < linked.size() || !s.build_mode ? PinLook::Port : PinLook::Add);
                            if (ImGui::IsItemHovered()) hovered_hint = row < linked.size() ? from(linked[row]) : in.hint;
                        });
                    }
                    ImGui::PopID();
                    return;
                }

                folding(linked.empty() ? detail : 1.0f, [&] {
                    const std::string label = in.required ? std::string(in.label) + " *" : std::string(in.label);
                    draw_pin(s, pin_id(n.id, false, slot), label, pin_type, false, !linked.empty(), x0, node_width, o,
                             linked.empty() && in.editable() ? PinLook::Field : PinLook::Port);
                    if (ImGui::IsItemHovered()) hovered_hint = linked.empty() ? std::string(in.hint) : from(linked[0]);
                    if (!in.editable()) return;
                    // Not SameLine(x): inside a node (an ImGui group) that offset is group-relative, so x0 would be
                    // counted twice and nodes would widen with their canvas position.
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(x0 + label_width);
                    const Faded field(linked.empty() ? 1.0f : detail);  // a linked row stays; its field fades
                    std::string& value = n.params[in.name];
                    if (!linked.empty()) {  // a link overrides the typed value: show what it holds, if known yet
                        const std::string held = remod::link_value(s.graph, s.preview, s.values, linked[0]);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextDisabled(
                            "%s", fit_text(nullptr, font, held.empty() ? "linked (known after a run)" : held, field_width).c_str());
                        if (ImGui::IsItemHovered()) hovered_hint = from(linked[0]);
                    } else if (in.widget == remod::Widget::Checkbox) {
                        bool on = value == "true";
                        if (ImGui::Checkbox("##v", &on)) value = on ? "true" : "";
                    } else if (in.widget == remod::Widget::Number) {
                        // Drag left / right (about 400 pixels across the range), or Ctrl+click to type. Stored as
                        // text, a whole number when it is one; a typed value core can't read shows as 0 until set.
                        float v = 0;
                        try {
                            v = std::stof(value);
                        } catch (const std::exception&) {
                        }
                        ImGui::SetNextItemWidth(field_width);
                        const float speed = ImMax(0.1f, (in.max - in.min) / 400);
                        // On "auto", what it came to (e.g. "auto (~91 px)", worked out with the thumbnail, so ~), and
                        // dragging starts from there; a click alone leaves it on auto.
                        std::string shown = v == 0 && in.zero ? in.zero : in.format;
                        if (const auto t = s.thumbs.images.find(n.id); v == 0 && in.zero && t != s.thumbs.images.end())
                            if (const auto f = t->second.found.find(in.name); f != t->second.found.end()) {
                                char buf[64];
                                std::snprintf(buf, sizeof buf, in.format, std::round(f->second));
                                shown = std::string(in.zero) + " (~" + buf + ")";
                                for (size_t i = 0; (i = shown.find('%', i)) != std::string::npos; i += 2)
                                    shown.insert(i, "%");  // literal text for DragFloat's format
                                v = std::round(f->second);
                            }
                        if (ImGui::DragFloat("##v", &v, speed, in.min, in.max, shown.c_str(),
                                             ImGuiSliderFlags_AlwaysClamp)) {
                            v = std::round(v);  // ponytail: whole numbers; a step setting if a field needs fractions
                            value = std::to_string(int(v));
                        }
                        if (ImGui::IsItemHovered())
                            hovered_hint = std::string(in.hint) + "\nDrag to change, Ctrl+click to type.";
                    } else if (in.widget == remod::Widget::Choice) {
                        // A button that opens the list (drawn with the menus, outside the canvas: a combo's popup
                        // inside a node lands in the wrong place when zoomed).
                        const auto current = std::ranges::find_if(in.options, [&](const auto& o) { return value == o[0]; });
                        if (ImGui::Button(current != in.options.end() ? (*current)[1] : value.c_str(),
                                          ImVec2(field_width, 0))) {
                            s.choice_node = n.id;
                            s.choice_input = in.name;
                            open_choice_menu = true;
                        }
                        const ImRect r(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                        ImGui::RenderArrow(ImGui::GetWindowDrawList(),
                                           ImVec2(r.Max.x - font - style.FramePadding.x, r.Min.y + style.FramePadding.y),
                                           ImGui::GetColorU32(ImGuiCol_Text), ImGuiDir_Down);
                        if (ImGui::IsItemHovered()) hovered_hint = in.hint;
                    } else {
                        ImGui::SetNextItemWidth(field_width - (in.result && s.build_mode ? flip_width : 0));
                        ImGui::InputText("##v", &value);
                        if (ImGui::IsItemHovered()) hovered_hint = value.empty() ? std::string(in.hint) : value;  // all of it
                        accept_path(s, value, in.path,
                                    in.path == remod::PathKind::OpenTexture ? s.texture_filter.c_str() : in.filter,
                                    hovered_hint);
                        if (in.widget == remod::Widget::Path) {
                            ImGui::SameLine();
                            const bool texture = in.path == remod::PathKind::OpenTexture;
                            if (ImGui::SmallButton("...") &&
                                browse(in.path, texture ? s.texture_filter.c_str() : in.filter, value,
                                       texture                                         ? game_files_dir(s)
                                       : in.filter && std::string_view(in.filter) == "exe" ? env("ProgramFiles")
                                                                                          : std::string()) &&
                                texture)
                                detect_game(s, value);
                            if (ImGui::IsItemHovered()) hovered_hint = "Browse...";
                        }
                    }
                    flip_button(in);  // a flipped destination row can flip back
                });
                ImGui::PopID();
            };

            // Fixed inputs first; destinations keep the row of the output they share, flipped or not (only the circle
            // changes side); inputs that grow a row per link come last, at the bottom of the node, so the fields don't
            // move down as links are added.
            for (size_t slot = 0; slot < spec->inputs.size(); ++slot) {
                const remod::InputSpec& in = spec->inputs[slot];
                if (!in.multiple && !in.result) draw_input(slot);
            }
            if (manual) {  // Edit image: the user's own step
                const std::filesystem::path file = status ? status->file : std::filesystem::path();
                if (file.empty()) {
                    folding(detail, [] { ImGui::TextDisabled("Run first: Export image creates the file to edit."); });
                } else {
                    if (ImGui::Button("Open in editor")) open_edit(s, n.id, file);
                    if (ImGui::IsItemHovered()) hovered_hint = open_edit_hint(s, n.id);
                    ImGui::SameLine();
                }
                if (!edit_done) {
                    if (ImGui::Button("Done editing")) remod::set_edit_done(s.graph, n.id, true);
                    if (ImGui::IsItemHovered())
                        hovered_hint = "Click once you've saved your changes. The next Run continues from here.";
                } else {
                    ImGui::TextColored(state_color(remod::NodeState::Done), "Done");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Edit again")) remod::set_edit_done(s.graph, n.id, false);
                }
            }

            for (size_t i = 0; i < spec->outputs.size(); ++i) {
                const remod::PortSpec& out = spec->outputs[i];
                ImGui::PushID(out.name);
                if (out.multiple) {  // a Split: one row per link, plus (when building) an empty row for the next one
                    const std::vector<size_t> linked = s.graph.links_from(n.id, out.name);
                    const size_t rows = linked.size() + (s.build_mode || linked.empty() ? 1 : 0);
                    for (size_t row = 0; row < rows && row < kRows; ++row) {
                        const std::string label = row < linked.size() ? std::string(out.label) + " " + std::to_string(row + 1)
                                                  : s.build_mode     ? "+ " + std::string(out.label)
                                                                     : std::string(out.label) + ": none connected";
                        folding(row < linked.size() ? 1.0f : detail, [&] {
                            draw_pin(s, pin_id(n.id, true, i, row), label, s.graph.output_type(n.id, out.name), true,
                                     row < linked.size(), x0, node_width, o,
                                     row < linked.size() || !s.build_mode ? PinLook::Port : PinLook::Add);
                            if (ImGui::IsItemHovered())
                                hovered_hint = row < linked.size() ? "To: " + target_of(s.graph, s.graph.links[linked[row]])
                                                                   : "Drag from here to pass this to one more step.";
                        });
                    }
                    ImGui::PopID();
                    continue;
                }
                // The destination that shares this output's row (Copy's destination, Package's output folder); if it's
                // flipped, the row is drawn here as an input (circle on the left) and this output isn't offered.
                const auto shared = std::ranges::find_if(
                    spec->inputs, [&](const remod::InputSpec& in) { return in.result && out.name == std::string_view(in.result); });
                const remod::InputSpec* dest = shared != spec->inputs.end() ? &*shared : nullptr;
                if (dest && remod::is_flipped(n, dest->name)) {  // same height either way: only the circle moves
                    ImGui::PopID();
                    draw_input(size_t(dest - spec->inputs.data()));
                    continue;
                }
                if (out.field || dest) {  // where the node writes this output: on the output side, before its pin
                    const Faded faded(detail);
                    std::string& value = n.params[dest ? dest->name : out.field];
                    ImGui::SetCursorPosX(x0 + node_width -
                                         (field_width + style.ItemSpacing.x * 2 + button_width +
                                          ImGui::CalcTextSize(out.label).x));
                    // A Value's field (an open kind) gets the picker of the kind it feeds; text gets none.
                    const remod::PortType kind = s.graph.output_type(n.id, out.name);
                    const remod::PathKind picker = dest                              ? dest->path
                                                   : out.type == remod::PortType::Any ? remod::picker_for(kind)
                                                                                      : out.path;
                    const bool texture = picker == remod::PathKind::OpenTexture;
                    const char* filter = texture                           ? s.texture_filter.c_str()
                                         : dest                            ? dest->filter
                                         : out.type != remod::PortType::Any ? out.filter
                                         : kind == remod::PortType::Image   ? remod::kEditImageFormats
                                                                            : nullptr;
                    ImGui::SetNextItemWidth(field_width - (dest && s.build_mode ? flip_width : 0));
                    ImGui::InputTextWithHint("##v", dest ? dest->label : out.field_label, &value);
                    if (ImGui::IsItemHovered())  // the whole value (a long path doesn't fit the field), else the hint
                        hovered_hint = !value.empty() ? value : dest ? dest->hint : out.hint;
                    accept_path(s, value, picker, filter, hovered_hint);
                    ImGui::SameLine();
                    if (picker == remod::PathKind::None) {
                        ImGui::Dummy(ImVec2(button_width, 0));  // keeps the field where the button would push it
                    } else {
                        if (ImGui::SmallButton("...") &&
                            browse(picker, filter, value, texture ? game_files_dir(s) : std::string()) && texture)
                            detect_game(s, value);
                        if (ImGui::IsItemHovered()) hovered_hint = "Browse...";
                    }
                    if (dest) flip_button(*dest);
                    ImGui::SameLine();
                }
                draw_pin(s, pin_id(n.id, true, i), out.label, s.graph.output_type(n.id, out.name), true,
                         s.graph.is_connected(n.id, out.name, true), x0, node_width, o);
                ImGui::PopID();
            }
            for (size_t slot = 0; slot < spec->inputs.size(); ++slot)
                if (spec->inputs[slot].multiple) draw_input(slot);
        }
        if (!far_view && spec && spec->thumbnail)
            folding(detail, [&] {
                const bool large = spec->view_size != nullptr;
                draw_thumb(s, n.id, node_width, large ? picture_size(n, *spec) : font * 8, large, hovered_hint);
            });
        if (!far_view) ImGui::Dummy(ImVec2(node_width, 0));  // fixes the node width so the right edge (and its pins) line up
        ImGui::PopID();
        ed::EndNode();
        ed::PopStyleVar();

        // The block's outline, under its contents. Far: the symbol (a Split: a dot); Near: the whole block.
        const ImVec2 pos = ed::GetNodePosition(n.id), size = ed::GetNodeSize(n.id);
        if (!far_view) s.near_size[n.id] = size;
        s.mini.push_back({n.id, family, pos, far_view ? far_size : size});
        ImDrawList* bg = ed::GetNodeBackgroundDrawList(n.id);
        if (!bg) continue;
        const bool selected = ed::IsNodeSelected(n.id), hovered = ed::GetHoveredNode() == ed::NodeId(n.id);
        BlockPaint paint = block_paint(state, far_view, selected, hovered);
        if (far_view && family == remod::Family::Flow) {
            const ImVec2 c = pos + far_size * 0.5f;
            bg->AddCircleFilled(c, 6 * block_u, with_alpha(paint.stroke, paint.alpha));
            if (selected) {  // a dashed ring
                std::vector<ImVec2> ring;
                for (int k = 0; k < 32; ++k) ring.push_back(c + ImVec2(std::cos(k * IM_PI / 16), std::sin(k * IM_PI / 16)) * 10 * block_u);
                dashed(bg, ring.data(), int(ring.size()), true, pal::accent, 1.25f * block_u, 2 * block_u, 2 * block_u);
            }
            continue;
        }
        if (!far_view) paint.band = band;
        // A manual step's header (all of it, Far) is hatched while it isn't filled by a run state.
        const bool plain = !state || state == remod::NodeState::NotReached;
        if (family == remod::Family::Manual && plain) paint.hatch = far_view ? far_size.y : band;
        paint_block(bg, family, pos, far_view ? far_size : size, block_u, paint);
    }
    // View changes go here, after the blocks: the editor only counts blocks drawn this frame (content bounds,
    // positions), so before them a fit saw an empty graph and did nothing. And only once the view's size has held for
    // a frame: when it changes (the dock layout settling at startup) the editor restores its previous view, which
    // would undo a fit made the frame before.
    const bool view_settled = view_size.x == s.last_view_size.x && view_size.y == s.last_view_size.y;
    s.last_view_size = view_size;
    if (view_settled) {
        if (s.navigate) {  // after a load or Tidy up
            fit_view(editor, view_size);
            s.navigate = false;
            if (s.tidy_after_load) s.tidy_requested = true, s.tidy_after_load = false;
        }
        // Far / Near buttons; a step or minimap click: select and centre that block; a minimap click elsewhere: centre.
        if (s.zoom_request == 1) fit_view(editor, view_size, 0.1f, 0.5f);
        if (s.zoom_request == 2) {
            ed::NodeId sel;
            const ImVec2 at = ed::GetSelectedNodes(&sel, 1) ? ed::GetNodePosition(sel) + ed::GetNodeSize(sel) * 0.5f
                                                            : ed::ScreenToCanvas(view_center);
            center_view(editor, view_size, at, 1.0f);
        }
        s.zoom_request = 0;
        if (s.focus_node && s.graph.find(s.focus_node)) {
            ed::SelectNode(s.focus_node);
            if (!s.jump_to) s.jump_to = ed::GetNodePosition(s.focus_node) + ed::GetNodeSize(s.focus_node) * 0.5f;
        }
        s.focus_node = 0;
        if (s.jump_to) center_view(editor, view_size, *s.jump_to, zoom);
        s.jump_to.reset();
    }
    ImGui::SetFontRasterizerDensity(old_density);  // tooltips and menus below are drawn unzoomed
    if (!hovered_hint.empty()) {
        ed::Suspend();
        ImGui::SetTooltip("%s", hovered_hint.c_str());
        ed::Resume();
    }

    // Placement (core): a block just added makes room once its size is known; blocks let go after dragging keep
    // `min_gap` (three line lanes) from the others, so lines always have room between blocks.
    // Far places by the blocks' full sizes (as last drawn Near), so the full layout stays clear when zooming back in.
    const float lane = font * 0.8f, min_gap = lane * 3;
    auto layout_size = [&](int id) {
        const auto full = s.near_size.find(id);
        return far_view && full != s.near_size.end() ? full->second : ed::GetNodeSize(id);
    };
    {
        std::vector<std::array<float, 2>> positions, sizes;
        for (const auto& n : s.graph.nodes) {
            const ImVec2 p = ed::GetNodePosition(n.id), size = layout_size(n.id);
            positions.push_back({p.x, p.y});
            sizes.push_back({size.x, size.y});
        }
        auto apply = [&](const std::vector<std::array<float, 2>>& at) {
            for (size_t i = 0; i < at.size(); ++i)
                if (at[i] != positions[i]) ed::SetNodePosition(s.graph.nodes[i].id, ImVec2(at[i][0], at[i][1]));
        };
        const auto it = std::ranges::find(s.graph.nodes, s.place_new, &remod::Node::id);
        if (it == s.graph.nodes.end()) {
            s.place_new = 0;
        } else if (sizes[size_t(it - s.graph.nodes.begin())][0] > 0) {
            apply(remod::make_room(s.graph, positions, sizes, s.place_new, font * 4, min_gap));
            s.place_new = 0;
        }
        const bool dragging = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        for (size_t i = 0; i < s.graph.nodes.size(); ++i) {
            const int id = s.graph.nodes[i].id;
            const ImVec2 now(positions[i][0], positions[i][1]);
            if (const auto last = s.last_pos.find(id); dragging && last != s.last_pos.end() && (last->second.x != now.x || last->second.y != now.y))
                s.dragged.insert(id);
            s.last_pos[id] = now;
        }
        if (!dragging && !s.dragged.empty()) {
            for (size_t i = 0; i < s.graph.nodes.size(); ++i)
                if (s.dragged.contains(s.graph.nodes[i].id)) positions = remod::keep_apart(positions, sizes, i, min_gap);
            apply(positions);
            s.dragged.clear();
        }
    }

    if (s.tidy_requested) {  // Build layout's Tidy up: columns by step order (core decides where)
        s.tidy_requested = false;
        std::vector<std::array<float, 2>> sizes;
        for (auto& n : s.graph.nodes) {
            const ImVec2 p = ed::GetNodePosition(n.id), size = layout_size(n.id);
            n.x = p.x;
            n.y = p.y;
            sizes.push_back({size.x, size.y});
        }
        const auto at = remod::tidy_layout(s.graph, sizes, font * 8, min_gap);
        for (size_t i = 0; i < at.size(); ++i) ed::SetNodePosition(s.graph.nodes[i].id, ImVec2(at[i][0], at[i][1]));
        s.navigate = true;  // fit the view to the result
    }

    // Links: routed around the blocks with right angles (core/route), one line per link, drawn on the editor's top
    // layer so no block ever hides one.
    std::vector<remod::Box> blocks;
    for (const auto& n : s.graph.nodes) {
        const ImVec2 p = ed::GetNodePosition(n.id), size = ed::GetNodeSize(n.id);
        if (size.x > 0 && size.y > 0) blocks.push_back({p.x, p.y, p.x + size.x, p.y + size.y});
    }
    std::vector<remod::LinkRoute> requests;
    std::vector<size_t> routed;
    std::vector<Stroke> strokes;
    std::vector<std::array<remod::Pt, 2>> ends;  // each routed link's pins (its path runs between their anchors)
    for (size_t i = 0; i < s.graph.links.size(); ++i) {
        const remod::Link& l = s.graph.links[i];
        const remod::Node* from = s.graph.find(l.from_node);
        const remod::Node* to = s.graph.find(l.to_node);
        const remod::NodeSpec* fs = from ? remod::find_spec(from->type) : nullptr;
        const remod::NodeSpec* ts = to ? remod::find_spec(to->type) : nullptr;
        if (!fs || !ts) continue;
        const size_t out_slot = slot_of(fs->outputs, l.from_port);
        // A multiple input's links go to rows 0, 1, ... in link order; a Split's links leave from rows the same way.
        const auto into = s.graph.links_into(l.to_node, l.to_port);
        const size_t row = size_t(std::ranges::find(into, i) - into.begin());
        const auto from_links = s.graph.links_from(l.from_node, l.from_port);
        const size_t from_row = fs->outputs[out_slot].multiple ? size_t(std::ranges::find(from_links, i) - from_links.begin()) : 0;
        const auto from_pin = pin_id(l.from_node, true, out_slot, from_row).Get();
        const auto to_pin = pin_id(l.to_node, false, slot_of(ts->inputs, l.to_port), row).Get();
        if (!s.pin_pos.contains(from_pin) || !s.pin_pos.contains(to_pin)) continue;
        const ImVec2 a = s.pin_anchor[from_pin], b = s.pin_anchor[to_pin];
        // Every link its own net: a value used in several places goes through a Split, so no line branches.
        requests.push_back({int(i), {a.x, a.y}, {b.x, b.y}});
        routed.push_back(i);
        ends.push_back({remod::Pt{s.pin_pos[from_pin].x, s.pin_pos[from_pin].y}, remod::Pt{s.pin_pos[to_pin].x, s.pin_pos[to_pin].y}});
        const auto source = run_state(s, l.from_node), target = run_state(s, l.to_node);
        using enum remod::NodeState;
        const LinkState state = !source ? (ed::IsNodeSelected(l.from_node) || ed::IsNodeSelected(l.to_node)
                                               ? LinkState::Hot
                                               : LinkState::Build)
                                : source == Failed  ? LinkState::Failed
                                : source != Done    ? LinkState::Pending
                                : target == Waiting ? LinkState::Active
                                                    : LinkState::Done;
        strokes.push_back(stroke_for(state, kind_color(s.graph.output_type(l.from_node, l.from_port))));
    }
    // While blocks are being dragged: plain elbows between the pins, no routing (it ran every frame and made dragging
    // lag in a Debug build). Once they're let go: one quick pass, then all three (19 ms in Debug for the 5-block
    // example), which untangle crossings.
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !s.dragged.empty()) {
        const float gap = font * 0.8f;
        s.routes = {};
        for (const remod::LinkRoute& l : requests) {
            const remod::Pt a = l.from, b = l.to;
            if (b.x - a.x >= 2 * gap) {  // forwards: across, down, across
                const float mx = (a.x + b.x) / 2;
                s.routes.paths.push_back({a, {mx, a.y}, {mx, b.y}, b});
            } else {  // backwards: out, over to the middle height, back, in
                const float my = (a.y + b.y) / 2;
                s.routes.paths.push_back({a, {a.x + gap, a.y}, {a.x + gap, my}, {b.x - gap, my}, {b.x - gap, b.y}, b});
            }
        }
        s.routes.portals.assign(s.routes.paths.size(), 0);
        s.route_blocks.clear();  // routes properly once the blocks are let go
        s.route_requests.clear();
    } else if (blocks != s.route_blocks || requests != s.route_requests) {
        s.routes = remod::route_links(blocks, requests, font * 0.8f, 1);
        s.routes_settled = false;
        s.route_blocks = std::move(blocks);
        s.route_requests = std::move(requests);
    } else if (!s.routes_settled) {
        s.routes = remod::route_links(s.route_blocks, s.route_requests, font * 0.8f);
        s.routes_settled = true;
    }
    s.routed = std::move(routed);
    const ImVec2 mouse = ImGui::GetMousePos();  // canvas coordinates here, inside the editor
    // The link under the mouse (a portal only by its stubs); its menu is Build layout's, a portal's hint both layouts'.
    const int hovered_any = ed::GetHoveredNode().Get() == 0 && ImGui::IsWindowHovered()
                                ? remod::hit_link(s.routes.paths, {mouse.x, mouse.y}, font * 0.4f, s.routes.portals)
                                : -1;
    const int hovered_link = s.build_mode ? hovered_any : -1;
    std::string portal_tip;
    // SetUserContext also moves the layout cursor to the mouse; put it back afterwards. Left at the mouse, ImGui stops
    // the app at ed::End when the mouse is below or right of the blocks (e.g. over the panels under the graph).
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    reinterpret_cast<ed::Detail::EditorContext*>(editor)->SetUserContext();  // the top layer, above the blocks
    ImDrawList* draw = ImGui::GetWindowDrawList();
    // That layer expects its clip rectangle in screen coordinates (the editor converts it once more at the end),
    // while the current one is in canvas coordinates. Without this the lines vanish at some zoom levels.
    draw->PushClipRect(ed::CanvasToScreen(draw->GetClipRectMin()), ed::CanvasToScreen(draw->GetClipRectMax()), false);
    const float pin_r = 4 * block_u;  // lines stop at the pin's edge
    // Drawn in order: not reached, failed, done, then the active ones on top. Portals keep their numbers by link.
    std::vector<size_t> order, portal_number(s.routes.paths.size());
    for (size_t i = 0, number = 0; i < s.routes.paths.size() && i < strokes.size(); ++i) {
        order.push_back(i);
        if (i < s.routes.portals.size() && s.routes.portals[i]) portal_number[i] = ++number;
    }
    std::ranges::stable_sort(order, {}, [&](size_t i) { return strokes[i].order; });
    for (const size_t i : order) {
        const bool hovered = int(i) == hovered_any;
        const Stroke& stroke = strokes[i];
        if (!portal_number[i]) {  // pin, anchor, the route, anchor, pin
            std::vector<remod::Pt> path = s.routes.paths[i];
            path.insert(path.begin(), ends[i][0]);
            path.push_back(ends[i][1]);
            draw_route(draw, path, font * 0.6f, pin_r, stroke, block_u, int(i) == hovered_link ? 2.0f : 1.0f);
            continue;
        }
        // No clean route (core portals): a stub at each pin ending in a tag with the same number at both ends;
        // hovering either end shows where it goes, as a dashed straight line.
        const auto& p = s.routes.paths[i];
        const std::string number = std::to_string(portal_number[i]);
        const ImVec2 out_end(p[1].x, p[1].y), in_end(p[2].x, p[2].y);
        draw_route(draw, {ends[i][0], p[0], p[1]}, 0, pin_r, stroke, block_u, hovered ? 2.0f : 1.0f);
        draw_route(draw, {p[2], p[3], ends[i][1]}, 0, pin_r, stroke, block_u, hovered ? 2.0f : 1.0f);
        for (const ImVec2 end : {out_end, in_end}) {
            draw->AddCircleFilled(end, font * 0.6f, stroke.color);
            draw->AddText(end - ImGui::CalcTextSize(number.c_str()) * 0.5f, pal::bg, number.c_str());
        }
        if (hovered) {
            const float length = ImLength(in_end - out_end), dash = font * 0.5f;
            for (float d = 0; d < length; d += dash * 2)
                draw->AddLine(out_end + (in_end - out_end) * (d / length),
                              out_end + (in_end - out_end) * (ImMin(d + dash, length) / length), stroke.color, 1.5f);
            if (i < s.routed.size()) {
                const remod::Link& l = s.graph.links[s.routed[i]];
                portal_tip = "No clean route for this link (both ends show " + number + ").\nFrom: " +
                             source_of(s.graph, l) + "\nTo: " + target_of(s.graph, l) + "\n" +
                             (s.build_mode ? "Move the blocks apart, or Tidy up." : "Switch to Build layout to tidy up.");
            }
        }
    }
    // A block dragged from the Nodes panel over the graph: a see-through copy where it will land, already spaced from
    // the others the way it will be once dropped (core keep_apart), at the graph's zoom.
    s.ghost_at.reset();
    const ImGuiPayload* drag = ImGui::GetDragDropPayload();
    const ImVec2 view_lo = ed::ScreenToCanvas(view_center - view_size * 0.5f);
    const ImVec2 view_hi = ed::ScreenToCanvas(view_center + view_size * 0.5f);
    s.mini_view_min = view_lo;
    s.mini_view_max = view_hi;
    const remod::NodeSpec* dragged_spec =
        drag && drag->IsDataType("remod_block") ? remod::find_spec(static_cast<const char*>(drag->Data)) : nullptr;
    if (const remod::NodeSpec* spec = dragged_spec;
        spec && mouse.x > view_lo.x && mouse.x < view_hi.x && mouse.y > view_lo.y && mouse.y < view_hi.y) {
        const ImVec2 size = draw_block_preview(nullptr, {}, *spec, s.look, 1, 1);
        std::vector<std::array<float, 2>> positions, sizes;
        for (const auto& n : s.graph.nodes) {
            const ImVec2 p = ed::GetNodePosition(n.id), sz = layout_size(n.id);
            positions.push_back({p.x, p.y});
            sizes.push_back({sz.x, sz.y});
        }
        positions.push_back({mouse.x, mouse.y});
        sizes.push_back({size.x, size.y});
        const auto at = remod::keep_apart(positions, sizes, positions.size() - 1, min_gap).back();
        s.ghost_at = ImVec2(at[0], at[1]);
        draw_block_preview(draw, *s.ghost_at, *spec, s.look, 1, 0.45f);
    }
    draw->PopClipRect();
    ImGui::SetCursorScreenPos(cursor);
    ImGui::Dummy(ImVec2(0, 0));  // an item at the cursor, as ImGui requires after moving it
    if (!portal_tip.empty()) {
        ed::Suspend();
        ImGui::SetTooltip("%s", portal_tip.c_str());
        ed::Resume();
    }

    // Dragging a new link: core decides whether it's allowed. Letting go on empty canvas offers nodes to add
    // there, already connected ("add node here").
    bool open_pin_menu = false;
    if (ed::BeginCreate()) {
        ed::PinId a, b;
        if (ed::QueryNewLink(&a, &b) && a && b) {
            const auto link = make_link(s.graph, a, b);
            const std::string err = !s.build_mode ? "Switch to Build layout to change links."
                                    : link        ? s.graph.can_connect(*link)
                                                  : "connect an output to an input";
            if (!err.empty()) {
                ed::RejectNewItem(ImVec4(1, 0.3f, 0.3f, 1), 2.0f);
                ed::Suspend();
                ImGui::SetTooltip("%s", err.c_str());
                ed::Resume();
            } else if (ed::AcceptNewItem()) {
                s.graph.connect(*link);
            }
        }
        ed::PinId pin;
        if (ed::QueryNewNode(&pin) && pin) {
            ed::Suspend();
            ImGui::SetTooltip(s.build_mode ? "+ Add a block here" : "Switch to Build layout to add blocks.");
            ed::Resume();
            if (!s.build_mode) {
                ed::RejectNewItem(ImVec4(1, 0.3f, 0.3f, 1), 2.0f);
            } else if (ed::AcceptNewItem()) {
                s.menu_pin = pin;
                open_pin_menu = true;
            }
        }
    }
    ed::EndCreate();

    // Delete key / editor deletions (blocks only: a link is removed from its right-click menu).
    std::vector<int> dead_nodes;
    if (ed::BeginDelete()) {
        ed::NodeId node;
        while (ed::QueryDeletedNode(&node)) {
            if (!s.build_mode)
                ed::RejectDeletedItem();  // Use layout: the structure is fixed
            else if (ed::AcceptDeletedItem())
                dead_nodes.push_back(int(node.Get()));
        }
    }
    ed::EndDelete();
    for (int id : dead_nodes) s.graph.remove_node(id);

    // Menus: right-click a node, a link or empty canvas; or let go of a dragged link on empty canvas.
    ed::Suspend();
    if (open_choice_menu) ImGui::OpenPopup("choice_menu");  // a field's dropdown: both layouts (it's filling in)
    if (ImGui::BeginPopup("choice_menu")) {
        remod::Node* n = s.graph.find(s.choice_node);
        const remod::NodeSpec* spec = n ? remod::find_spec(n->type) : nullptr;
        const remod::InputSpec* in = spec ? remod::find_input(*spec, s.choice_input) : nullptr;
        if (in)
            for (const auto& [value, label] : in->options)
                if (ImGui::MenuItem(label, nullptr, n->params[in->name] == value)) n->params[in->name] = value;
        ImGui::EndPopup();
    }
    ed::NodeId clicked_node;
    if (!s.build_mode) {
        // Use layout: no structural menus. A right-click says where to go instead.
        if (ed::ShowBackgroundContextMenu() || ed::ShowNodeContextMenu(&clicked_node))
            s.status = "Switch to Build layout (above the graph) to add, remove or relink blocks.";
    } else if (open_pin_menu) {
        s.menu_pos = ImGui::GetMousePos();
        ImGui::OpenPopup("add_connected");
    } else if (ed::ShowNodeContextMenu(&clicked_node)) {
        s.menu_node = int(clicked_node.Get());
        s.menu_pos = ImGui::GetMousePos();
        ImGui::OpenPopup("node_menu");
    } else if (ed::ShowBackgroundContextMenu()) {  // links aren't editor objects: a click on one lands here
        s.menu_pos = ImGui::GetMousePos();
        if (hovered_link >= 0 && size_t(hovered_link) < s.routed.size()) {
            s.menu_link = s.routed[hovered_link];
            ImGui::OpenPopup("link_menu");
        } else {
            ImGui::OpenPopup("add_node");
        }
    }

    // A menu entry for a node type: its readable name, the description on hover.
    auto node_item = [](const remod::NodeSpec& spec) {
        const std::string label = std::string(spec.manual ? "Your step: " : "") + spec.title;
        const bool picked = ImGui::MenuItem(label.c_str());
        if (ImGui::IsItemHovered()) spec_tooltip(spec);
        return picked;
    };
    auto place = [&](int id) {  // where the menu was, until core makes room for it (next frame, once it has a size)
        ed::SetNodePosition(id, ed::ScreenToCanvas(s.menu_pos));
        s.place_new = id;
    };
    auto attempt = [&](auto&& edit) {  // editing refusals go to the status line
        try {
            edit();
        } catch (const std::exception& e) {
            s.status = e.what();
        }
    };

    if (ImGui::BeginPopup("add_node")) {
        ImGui::TextDisabled("Add a node");
        ImGui::Separator();
        for (const auto& spec : remod::node_specs())
            if (!spec.utility && node_item(spec)) place(s.graph.add_node(spec.type).id);
        if (ImGui::BeginMenu("Utilities")) {
            for (const auto& spec : remod::node_specs())
                if (spec.utility && node_item(spec)) place(s.graph.add_node(spec.type).id);
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("add_connected")) {
        ImGui::TextDisabled("Add a node connected here");
        ImGui::Separator();
        const PinRef ref = decode(s.menu_pin);
        const remod::Node* n = s.graph.find(ref.node);
        const remod::NodeSpec* spec = n ? remod::find_spec(n->type) : nullptr;
        const auto& ports_size = spec ? (ref.output ? spec->outputs.size() : spec->inputs.size()) : 0;
        if (spec && ref.slot < ports_size) {
            const std::string port = ref.output ? spec->outputs[ref.slot].name : spec->inputs[ref.slot].name;
            const auto choices = s.graph.choices_for_pin(ref.node, port, ref.output);
            if (choices.empty()) ImGui::TextDisabled("Nothing fits this pin.");
            for (const bool utility : {false, true}) {
                if (utility && std::ranges::any_of(choices, [](const auto& c) { return c.spec->utility; }))
                    ImGui::Separator();
                for (const auto& c : choices)
                    if (c.spec->utility == utility && node_item(*c.spec))
                        attempt([&] { place(s.graph.add_connected(c, ref.node, port, ref.output)); });
            }
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("link_menu")) {
        if (ImGui::BeginMenu("Insert node here")) {
            const auto fits = s.graph.choices_for_link(s.menu_link);
            if (fits.empty()) ImGui::TextDisabled("Nothing fits on this link.");
            for (const bool utility : {false, true}) {
                if (utility && std::ranges::any_of(fits, &remod::NodeSpec::utility)) ImGui::Separator();
                for (const auto* spec : fits)
                    if (spec->utility == utility && node_item(*spec))
                        attempt([&] { place(s.graph.insert_node(s.menu_link, spec->type)); });
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Delete link")) s.graph.disconnect(s.menu_link);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("node_menu")) {
        if (ImGui::MenuItem("Duplicate"))
            attempt([&] {
                const int copy = s.graph.duplicate_node(s.menu_node);
                ed::SetNodePosition(copy, ed::GetNodePosition(s.menu_node) + ImVec2(40, 40));
                s.place_new = copy;
            });
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Same type and values, no connections.");
        if (ImGui::MenuItem("Disconnect all")) s.graph.disconnect_node(s.menu_node);
        if (ImGui::MenuItem("Rename...")) {
            const remod::Node* n = s.graph.find(s.menu_node);
            const remod::NodeSpec* spec = n ? remod::find_spec(n->type) : nullptr;
            s.rename_node = s.menu_node;
            s.rename_text = n && spec && remod::block_title(*n) != spec->title ? remod::block_title(*n) : std::string();
            open_rename = true;
        }
        if (ImGui::MenuItem("Delete")) s.graph.remove_node(s.menu_node);
        ImGui::EndPopup();
    }
    // Naming a block (both layouts: it's a label, not structure). Empty goes back to the type's title.
    if (open_rename) ImGui::OpenPopup("rename");
    if (ImGui::BeginPopup("rename")) {
        const remod::Node* n = s.graph.find(s.rename_node);
        const remod::NodeSpec* spec = n ? remod::find_spec(n->type) : nullptr;
        ImGui::TextDisabled("Name this block (empty: \"%s\")", spec ? spec->title : "");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(font * 16);
        const bool enter = ImGui::InputTextWithHint("##name", "e.g. Mod Output Folder", &s.rename_text,
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        if (enter || ImGui::Button("OK")) {
            remod::set_block_title(s.graph, s.rename_node, s.rename_text);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ed::Resume();

    // A block from the Nodes panel: where it was dropped, else mid-view; core places it clear of the others.
    if (!s.add_type.empty()) {
        if (s.build_mode && remod::find_spec(s.add_type)) {
            const int id = s.graph.add_node(s.add_type).id;
            ed::SetNodePosition(id, s.add_at.value_or(ed::ScreenToCanvas(view_center)));
            s.place_new = id;
        }
        s.add_type.clear();
    }

    // A texture from the Browser goes into the selected Original texture block, else the only one; in Build layout
    // a new block is added if there's none.
    if (!s.pending_texture.empty()) {
        ed::NodeId selected;
        const int picked = ed::GetSelectedNodes(&selected, 1) ? int(selected.Get()) : 0;
        int target = remod::texture_target(s.graph, picked);
        const bool any = std::ranges::any_of(s.graph.nodes, [](const remod::Node& n) { return n.type == "LoadTex"; });
        if (!target && !any && s.build_mode) {
            target = s.graph.add_node("LoadTex").id;
            ed::SetNodePosition(target, ed::ScreenToCanvas(view_center));
            s.place_new = target;
        }
        if (remod::Node* n = s.graph.find(target)) {
            n->params["tex"] = s.pending_texture;
            detect_game(s, s.pending_texture);
            s.status = "Original texture: " + std::filesystem::path(s.pending_texture).filename().string() + ". " + s.status;
        } else {
            s.status = any ? "The layout has several Original texture blocks: click the one to fill, then use the texture again."
                           : "The layout has no Original texture block. Switch to Build layout to add one.";
        }
        s.pending_texture.clear();
    }

    // Block positions into the graph, every frame: undo, the unsaved-changes check and saving see where blocks are.
    // (A block the editor hasn't placed yet reports FLT_MAX.)
    for (auto& n : s.graph.nodes)
        if (const ImVec2 p = ed::GetNodePosition(n.id); p.x < FLT_MAX / 2) n.x = p.x, n.y = p.y;

    // Zooming while holding a block keeps it under the cursor. The editor drags by the mouse's distance from the
    // click, both in graph coordinates, but converts the screen click through the current zoom every frame, so a
    // zoom moved the click point (and the block) in the graph. Pinning the click to where it happened fixes that.
    // Here, just before ed::End (where the drag is processed, after all our Suspend/Resume, which re-convert), the
    // IO is in graph coordinates; the editor restores the screen values at its end.
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        s.click_in_graph = io.MouseClickedPos[ImGuiMouseButton_Left];
    else if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        io.MouseClickedPos[ImGuiMouseButton_Left] = s.click_in_graph;
    ed::End();
    ed::SetCurrentEditor(nullptr);
    draw_minimap(s, view_center - view_size * 0.5f, view_size);

    // A block type dragged from the Nodes panel and dropped on the graph: added where its copy was (next frame).
    const ImVec2 view_min = view_center - view_size * 0.5f;
    if (ImGui::BeginDragDropTargetCustom(ImRect(view_min, view_min + view_size), ImGui::GetID("graph_drop"))) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("remod_block")) {
            s.add_type = static_cast<const char*>(payload->Data);
            s.add_at = s.ghost_at;
        }
        ImGui::EndDragDropTarget();
    }
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
    // A scalable font, so text stays sharp at any zoom and DPI (ImGui's default is a 13 px pixel font that turns
    // blocky when scaled). Segoe UI ships with Windows; ImGui's embedded vector font if it's somehow missing.
    char windows[MAX_PATH] = {};
    const std::string fonts = std::string(windows, ::GetWindowsDirectoryA(windows, MAX_PATH)) + "\\Fonts\\";
    if (!std::filesystem::is_regular_file(fonts + "segoeui.ttf") ||
        !io.Fonts->AddFontFromFileTTF((fonts + "segoeui.ttf").c_str(), 16.0f))
        io.Fonts->AddFontDefaultVector();
    // Block titles and values (g_title_font, g_mono_font); the default font stands in for a missing one.
    if (std::filesystem::is_regular_file(fonts + "seguisb.ttf"))
        g_title_font = io.Fonts->AddFontFromFileTTF((fonts + "seguisb.ttf").c_str(), 16.0f);
    if (std::filesystem::is_regular_file(fonts + "consola.ttf"))
        g_mono_font = io.Fonts->AddFontFromFileTTF((fonts + "consola.ttf").c_str(), 16.0f);
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleDpi = scale;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    ed::Config config;
    config.SettingsFile = nullptr;  // positions are saved in the graph file instead of NodeEditor.json
    ed::EditorContext* editor = ed::CreateEditor(&config);
    ed::SetCurrentEditor(editor);
    ed::GetStyle().LinkStrength = 0.0f;  // the line shown while dragging a new link: straight (links are routed)
    apply_palette();
    ed::SetCurrentEditor(nullptr);
    const bool nfd_ok = NFD_Init() == NFD_OKAY;  // pickers just won't open if this fails
    State state;
    std::optional<Browser> browser(std::in_place, g_device);
    if (std::error_code ec; !state.saved.graph_path.empty() && std::filesystem::is_regular_file(state.graph_path, ec))
        load_graph_file(state);  // reopen the last graph, fitted to the view
    if (!nfd_ok) state.status = std::string("File picker unavailable: ") + NFD_GetError();

    for (bool done = false; !done;) {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        // Closing asks about unsaved changes first (before the minimized-window skip below, so it's never lost).
        if (g_close_requested) {
            g_close_requested = false;
            if (unsaved(state)) {
                ::ShowWindow(hwnd, SW_RESTORE);
                g_occluded = false;
            }
            ask(state, State::Pending::Close);
        }
        if (state.quit) break;

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
        // Undo / redo and save, from anywhere in the app (a text box being typed in keeps its own Ctrl+Z).
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)) undo(state, false);
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_RouteGlobal) ||
            ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal))
            undo(state, true);
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal)) save_graph_file(state);
        const ImGuiID dockspace = ImGui::DockSpaceOverViewport();
        // Panels show only where they're used. Both layouts: Browser left (its paths drag onto block fields), Graph
        // middle, Pipeline right (closable in Use layout). Along the bottom: Use layout the viewer, then Textures;
        // Build layout the Nodes. Rebuilt when the mode or the Pipeline's visibility changes, so a hidden panel
        // leaves no empty space.
        if (state.build_mode) state.show_pipeline = true;
        static int layout_key = -1;
        if (const int key = int(state.build_mode) * 2 + int(state.show_pipeline); key != layout_key) {
            layout_key = key;
            ImGui::DockBuilderRemoveNode(dockspace);
            ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->Size);
            ImGuiID top = dockspace, bottom = 0, corner = 0, textures = 0, left = 0, rest = 0, right = 0, middle = 0;
            ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Down, 0.32f, &bottom, &top);
            ImGui::DockBuilderSplitNode(top, ImGuiDir_Left, 0.25f, &left, &rest);
            middle = rest;
            if (state.show_pipeline) ImGui::DockBuilderSplitNode(rest, ImGuiDir_Right, 0.3f, &right, &middle);
            ImGui::DockBuilderDockWindow("Browser", left);
            ImGui::DockBuilderDockWindow("Graph", middle);
            if (state.show_pipeline) ImGui::DockBuilderDockWindow("Pipeline", right);
            if (state.build_mode) {
                ImGui::DockBuilderDockWindow("Nodes", bottom);
            } else {
                ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Left, 0.25f, &corner, &textures);
                ImGui::DockBuilderDockWindow("###viewer", corner);
                ImGui::DockBuilderDockWindow("Textures", textures);
            }
            ImGui::DockBuilderFinish(dockspace);
        }
        if (state.build_mode) draw_nodes_panel(state);
        if (state.want_viewer) {
            browser->show_in_viewer([&state] { draw_block_view(state); });
            state.want_viewer = false;
        }
        if (const std::string picked = browser->draw(game_files_dir(state), unquote(state.noesis_path), state.profiles,
                                                     state.graph.profile, state.pinned, state.build_mode);
            !picked.empty())
            state.pending_texture = picked;
        if (state.pinned != state.saved.pinned_folders) remember_paths(state);  // a folder was pinned or unpinned
        if (state.build_mode || state.show_pipeline) draw_side_panel(state);
        state.viewer_on_block = !state.build_mode && browser->viewer_shows_external();
        update_thumbs(state);
        draw_canvas(state, editor);
        draw_popouts(state);
        // Undo steps and the unsaved-changes baseline, once the graph is settled: nothing dragged or typed, no block
        // still being placed. After a load or New, the graph as the editor placed it is the baseline.
        if (state.baseline_pending) {
            state.history.reset(state.graph);
            state.saved_graph = state.graph;
            state.baseline_pending = false;
        } else if (!ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !state.place_new) {
            state.history.track(state.graph);
        }
        draw_unsaved_prompt(state);
        draw_warnings(state);
        // The window's title: the graph's file name, with * while it has unsaved changes.
        static std::wstring title;
        if (std::wstring now = L"remod - " + std::filesystem::path(state.graph_path).filename().wstring() +
                               (unsaved(state) ? L"*" : L"");
            now != title) {
            title = now;
            ::SetWindowTextW(hwnd, title.c_str());
        }

        ImGui::Render();
        const ImVec4 bg = ImGui::ColorConvertU32ToFloat4(pal::bg);
        const float clear[4] = {bg.x, bg.y, bg.z, 1.0f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_occluded = g_swap_chain->Present(1, 0) == DXGI_STATUS_OCCLUDED;
    }

    if (state.run.valid()) state.run.wait();  // let a running graph finish (every tool call has a timeout)
    remember_paths(state);
    browser.reset();  // its GPU textures, before the device goes
    for (State::PreviewSet* set : {&state.thumbs, &state.big}) {
        if (set->job.valid()) set->job.wait();
        release(*set);
    }
    if (nfd_ok) NFD_Quit();
    ed::DestroyEditor(editor);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanup_device();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, instance);
    return 0;
}
