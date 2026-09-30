// remod-app: thin ImGui front end over core/. Draws the graph and forwards edits and runs to core;
// all graph rules (valid links, validation, running) live in core/graph.
// Win32 + DX11 setup follows imgui/examples/example_win32_directx11 (v1.92.9-docking).
#include "graph.hpp"
#include "profile.hpp"
#include "settings.hpp"
#include "setup.hpp"
#include "texture_converter.hpp"

#include <nfd.h>

#define IMGUI_DEFINE_MATH_OPERATORS  // required by the node editor's internal header
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

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <future>
#include <map>
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

// Editor ids. Pin id = node * 4096 + (output ? 2048 : 0) + slot * 64 + row, where slot is the input/output's
// index in the node spec and row numbers the lines of a multiple input (one per link, plus an empty one).
// Link id = base + index into graph.links.
constexpr std::uintptr_t kPerNode = 4096, kOutputBit = 2048, kRows = 64, kLinkBase = std::uintptr_t(1) << 24;

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
    const nfdu8filteritem_t item{kind == remod::PathKind::OpenTexture ? "RE Engine textures" : "Files", filter};
    const nfdu8filteritem_t* filters = filter ? &item : nullptr;
    const nfdfiltersize_t count = filter ? 1 : 0;
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
    bool push_positions = false;  // after a load: move editor nodes to the positions in the file
    bool navigate = false;
    bool save_requested = false;  // handled inside the editor, where node positions can be read
    std::future<remod::RunResult> run;
    std::map<int, remod::NodeStatus> statuses;  // where each node got to in the last run (badges on the nodes)
    std::mutex log_mutex;
    std::vector<std::string> log;  // written by the run thread
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

// Writes the settings file only when something changed.
void remember_paths(State& s) {
    const remod::Settings now{.graph_path = s.graph_path,
                              .noesis_path = s.noesis_path,
                              .show_help = s.show_help,
                              .game_files_dir = s.game_files,
                              .build_mode = s.build_mode};
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
        s.graph = remod::load_graph(s.graph_path);
        s.statuses.clear();
        s.push_positions = true;
        s.status = "Loaded " + s.graph_path + ": " + std::to_string(s.graph.nodes.size()) + " nodes, " +
                   std::to_string(s.graph.links.size()) + " links";
        remember_paths(s);
    } catch (const std::exception& e) {
        s.status = std::string("Error: ") + e.what();
    }
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
        remod::apply_run(s.graph, r);  // e.g. a re-exported PNG un-does an earlier "Done editing"
        s.statuses = r.nodes;
        s.status = r.message;
    } catch (const remod::RunError& e) {
        s.statuses = e.nodes;
        s.status = std::string("Error: ") + e.what();
    } catch (const std::exception& e) {
        s.statuses.clear();
        s.status = std::string("Error: ") + e.what();
    }
}

// Opens a file in the user's image editor (the "edit" verb, e.g. Paint), else whatever opens it.
void open_in_editor(const std::filesystem::path& file) {
    if (reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, L"edit", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        ::ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void draw_side_panel(State& s) {
    ImGui::Begin("Pipeline");
    // Two modes: Use a finished layout (fill in, run, edit PNGs) or Build one (add, link, arrange blocks).
    int mode = s.build_mode ? 1 : 0;
    bool mode_changed = ImGui::RadioButton("Use layout", &mode, 0);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fill in the fields, Run, edit the PNGs. The blocks and links stay as they are.");
    ImGui::SameLine();
    mode_changed |= ImGui::RadioButton("Build layout", &mode, 1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add, remove, link and arrange blocks to make or change a layout.");
    if (mode_changed) {
        s.build_mode = mode == 1;
        remember_paths(s);
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Show help", &s.show_help)) remember_paths(s);

    if (s.show_help && !s.build_mode) {
        ImGui::TextWrapped("1. Original texture: pick the game's .tex file (the picker opens in your REtool folder).");
        ImGui::TextWrapped("2. Run: Export PNG writes the PNG, and the run stops at Edit PNG - your step.");
        ImGui::TextWrapped("3. On Edit PNG: Open in editor, change and save the PNG (same size), click Done editing.");
        ImGui::TextWrapped("4. Run again: Convert PNG to texture and Package for Fluffy build the mod .zip.");
        ImGui::TextWrapped("After each run every block shows how far it got: done (green), waiting for you "
                           "(amber), failed (red, with the reason), not reached (grey).");
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
    if (ImGui::Button("...##graph") && browse(remod::PathKind::OpenFile, "json", s.graph_path)) load_graph_file(s);
    ImGui::SameLine();
    ImGui::TextUnformatted("Graph file");
    if (ImGui::Button("New")) {
        s.graph = {};
        s.statuses.clear();
        s.build_mode = true;  // an empty layout can only be built
        remember_paths(s);
        s.status = "New layout. Right-click the canvas to add blocks.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Load")) load_graph_file(s);
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        s.graph_path = unquote(s.graph_path);
        s.save_requested = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Save As...") && browse(remod::PathKind::SaveFile, "json", s.graph_path))
        s.save_requested = true;
    ImGui::SameLine();
    if (ImGui::Button("Fit view")) s.navigate = true;

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
    }

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

const ImVec4 kAmber(1.0f, 0.7f, 0.2f, 1.0f);

// Node border / badge colour for where a node got to in the last run.
ImVec4 state_color(remod::NodeState state) {
    switch (state) {
    case remod::NodeState::Done: return ImVec4(0.35f, 0.85f, 0.45f, 1);
    case remod::NodeState::Waiting: return kAmber;
    case remod::NodeState::Failed: return ImVec4(1.0f, 0.35f, 0.3f, 1);
    case remod::NodeState::NotReached: return ImVec4(0.5f, 0.5f, 0.5f, 1);
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

// Pin colour by data type, so it's visible what can plug into what.
ImU32 port_color(remod::PortType type) {
    switch (type) {
    case remod::PortType::Tex: return IM_COL32(235, 150, 60, 255);
    case remod::PortType::Image: return IM_COL32(90, 200, 110, 255);
    case remod::PortType::Text: return IM_COL32(120, 180, 255, 255);
    }
    return IM_COL32_WHITE;
}

// One pin: a small circle on the node border (links attach to its centre, drags start from it) around the row's
// label, which is drawn inside the node - left-aligned for inputs, right-aligned for outputs. Filled once
// connected. Rows are frame-height tall so labels line up with the text boxes next to them.
void draw_pin(ed::PinId id, const std::string& label, remod::PortType type, bool output, bool connected, float x0,
              float node_width, float edge_x) {
    ed::BeginPin(id, output ? ed::PinKind::Output : ed::PinKind::Input);
    if (output) ImGui::SetCursorPosX(x0 + node_width - ImGui::CalcTextSize(label.c_str()).x);
    const float y = ImGui::GetCursorScreenPos().y + ImGui::GetFrameHeight() * 0.5f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label.c_str());

    const ImVec2 center(edge_x, y);
    const float r = ImGui::GetFontSize() * 0.3f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (connected)
        draw->AddCircleFilled(center, r, port_color(type));
    else
        draw->AddCircle(center, r, port_color(type), 0, 2.0f);
    ed::PinPivotRect(center, center);
    ed::PinRect(center - ImVec2(r * 2, r * 2), center + ImVec2(r * 2, r * 2));
    ed::EndPin();
}

// Frames the graph without magnifying it. Zoom = fit-to-view, capped at kMaxFitZoom (above 100% text gets
// big and soft) and floored at kMinFitZoom so a wide graph stays readable; when floored, the view starts at
// the graph's left edge (where the pipeline begins) instead of its centre.
// The editor adds ~10% margin on top, so on screen these come out at roughly 90% and 55%.
constexpr float kMaxFitZoom = 1.0f, kMinFitZoom = 0.6f;

void fit_view(ed::EditorContext* editor, ImVec2 view) {
    auto* ctx = reinterpret_cast<ed::Detail::EditorContext*>(editor);
    const ImRect content = ctx->GetContentBounds();
    if (content.GetWidth() <= 0 || content.GetHeight() <= 0 || view.x <= 0 || view.y <= 0) return;
    const float fit = ImMin(view.x / content.GetWidth(), view.y / content.GetHeight());
    const float zoom = ImClamp(fit, kMinFitZoom, kMaxFitZoom);
    const ImVec2 size = view / zoom;
    const ImVec2 min = zoom > fit ? content.Min : content.GetCenter() - size * 0.5f;
    ctx->NavigateTo(ImRect(min, min + size), true, 0.0f);
}

void draw_canvas(State& s, ed::EditorContext* editor) {
    ImGui::Begin("Graph");
    const ImVec2 view_size = ImGui::GetContentRegionAvail();
    ed::SetCurrentEditor(editor);
    ed::Begin("canvas");

    // Rasterize node text at the on-screen zoom so it stays sharp when zoomed. Quantized to 1/8 steps so the
    // font cache doesn't get a new size every frame of a zoom animation.
    const float zoom = reinterpret_cast<ed::Detail::EditorContext*>(editor)->GetView().Scale;
    const float old_density = ImGui::GetFontRasterizerDensity();
    ImGui::SetFontRasterizerDensity(old_density * ImClamp(std::round(zoom * 8.0f) / 8.0f, 0.25f, 4.0f));

    if (s.push_positions) {
        for (const auto& n : s.graph.nodes) ed::SetNodePosition(n.id, ImVec2(n.x, n.y));
        s.push_positions = false;
        s.navigate = true;
    } else if (s.navigate) {  // one frame later, once node sizes are known
        fit_view(editor, view_size);
        s.navigate = false;
    }

    const float font = ImGui::GetFontSize();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float label_width = font * 7, field_width = font * 14;
    const float button_width = ImGui::CalcTextSize("...").x + style.FramePadding.x * 2;
    const float node_width = label_width + field_width + style.ItemSpacing.x + button_width;  // content width
    const ImVec4 padding = ed::GetStyle().NodePadding;  // x = left, z = right
    const char* hovered_hint = nullptr;  // tooltip drawn after the nodes, outside the canvas transform
    for (auto& n : s.graph.nodes) {
        const remod::NodeSpec* spec = remod::find_spec(n.type);
        const auto status_it = s.statuses.find(n.id);
        const remod::NodeStatus* status = status_it != s.statuses.end() ? &status_it->second : nullptr;
        const bool manual = spec && spec->manual;
        const bool edit_done = manual && n.params.contains("done");
        // Border = where the node got to in the last run; a manual step is always marked, amber until done.
        const ImVec4 border = status ? state_color(status->state)
                              : manual ? (edit_done ? state_color(remod::NodeState::Done) : kAmber)
                                       : ed::GetStyle().Colors[ed::StyleColor_NodeBorder];
        ed::PushStyleColor(ed::StyleColor_NodeBorder, border);
        ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, status || manual ? 3.0f : 1.0f);
        ed::BeginNode(n.id);
        ImGui::PushID(n.id);
        const float x0 = ImGui::GetCursorPosX();
        const float left_edge = ImGui::GetCursorScreenPos().x - padding.x;  // node border, where pins sit
        const float right_edge = ImGui::GetCursorScreenPos().x + node_width + padding.z;
        if (!spec) {
            ImGui::Text("%s (unknown node type)", n.type.c_str());
        } else {
            if (manual) {
                ImGui::TextColored(kAmber, "YOUR STEP");
                ImGui::SameLine();
            }
            ImGui::TextColored(ImVec4(0.55f, 0.8f, 1, 1), "%s", spec->title);
            ImGui::SameLine();
            ImGui::TextDisabled("%s", n.type.c_str());
            ImGui::PushTextWrapPos(x0 + node_width);
            ImGui::TextDisabled("%s", spec->summary);
            if (status) ImGui::TextColored(state_color(status->state), "%s", status_text(*status).c_str());
            ImGui::PopTextWrapPos();

            for (size_t slot = 0; slot < spec->inputs.size(); ++slot) {
                const remod::InputSpec& in = spec->inputs[slot];
                const std::vector<size_t> linked = s.graph.links_into(n.id, in.name);
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
                        else
                            label += "  <- " + source_of(s.graph, s.graph.links[linked[row]]);
                        draw_pin(pin_id(n.id, false, slot, row), label, in.type, false, row < linked.size(), x0,
                                 node_width, left_edge);
                        if (ImGui::IsItemHovered()) hovered_hint = in.hint;
                    }
                    ImGui::PopID();
                    continue;
                }

                const std::string label = in.required ? std::string(in.label) + " *" : std::string(in.label);
                draw_pin(pin_id(n.id, false, slot), label, in.type, false, !linked.empty(), x0, node_width, left_edge);
                if (ImGui::IsItemHovered()) hovered_hint = in.hint;
                if (!in.editable()) {
                    ImGui::PopID();
                    continue;
                }
                // Not SameLine(x): inside a node (an ImGui group) that offset is group-relative, so x0 would be
                // counted twice and nodes would widen with their canvas position.
                ImGui::SameLine();
                ImGui::SetCursorPosX(x0 + label_width);
                std::string& value = n.params[in.name];
                if (!linked.empty()) {  // a link overrides the typed value
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("<- %s", source_of(s.graph, s.graph.links[linked[0]]).c_str());
                } else if (in.widget == remod::Widget::Checkbox) {
                    bool on = value == "true";
                    if (ImGui::Checkbox("##v", &on)) value = on ? "true" : "";
                } else {
                    ImGui::SetNextItemWidth(field_width);
                    ImGui::InputText("##v", &value);
                    if (ImGui::IsItemHovered()) hovered_hint = in.hint;
                    if (in.widget == remod::Widget::Path) {
                        ImGui::SameLine();
                        const bool texture = in.path == remod::PathKind::OpenTexture;
                        if (ImGui::SmallButton("...") &&
                            browse(in.path, texture ? s.texture_filter.c_str() : in.filter, value,
                                   texture ? game_files_dir(s) : std::string()) &&
                            texture)
                            detect_game(s, value);
                        if (ImGui::IsItemHovered()) hovered_hint = "Browse...";
                    }
                }
                ImGui::PopID();
            }
            if (manual) {  // Edit PNG: the user's own step
                const std::filesystem::path file = status ? status->file : std::filesystem::path();
                if (file.empty()) {
                    ImGui::TextDisabled("Run first: Export PNG creates the file to edit.");
                } else {
                    if (ImGui::Button("Open in editor")) open_in_editor(file);
                    if (ImGui::IsItemHovered()) hovered_hint = "Opens the PNG in your image editor (e.g. Paint).";
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
                if (out.field) {  // where the node writes this output: on the output side, before its pin
                    std::string& value = n.params[out.field];
                    ImGui::SetCursorPosX(x0 + node_width -
                                         (field_width + style.ItemSpacing.x * 2 + button_width +
                                          ImGui::CalcTextSize(out.label).x));
                    ImGui::SetNextItemWidth(field_width);
                    ImGui::InputTextWithHint("##v", out.field_label, &value);
                    if (ImGui::IsItemHovered()) hovered_hint = out.hint;
                    ImGui::SameLine();
                    if (ImGui::SmallButton("...")) browse(out.path, out.filter, value);
                    if (ImGui::IsItemHovered()) hovered_hint = "Browse...";
                    ImGui::SameLine();
                }
                draw_pin(pin_id(n.id, true, i), out.label, out.type, true, s.graph.is_connected(n.id, out.name, true),
                         x0, node_width, right_edge);
                ImGui::PopID();
            }
        }
        ImGui::Dummy(ImVec2(node_width, 0));  // fixes the node width so the right border (and its pins) line up
        ImGui::PopID();
        ed::EndNode();
        ed::PopStyleVar();
        ed::PopStyleColor();
    }
    ImGui::SetFontRasterizerDensity(old_density);  // tooltips and menus below are drawn unzoomed
    if (hovered_hint) {
        ed::Suspend();
        ImGui::SetTooltip("%s", hovered_hint);
        ed::Resume();
    }

    for (size_t i = 0; i < s.graph.links.size(); ++i) {
        const remod::Link& l = s.graph.links[i];
        const remod::Node* from = s.graph.find(l.from_node);
        const remod::Node* to = s.graph.find(l.to_node);
        const remod::NodeSpec* fs = from ? remod::find_spec(from->type) : nullptr;
        const remod::NodeSpec* ts = to ? remod::find_spec(to->type) : nullptr;
        if (!fs || !ts) continue;
        const size_t out_slot = slot_of(fs->outputs, l.from_port);
        // A multiple input's links go to rows 0, 1, ... in link order.
        const auto into = s.graph.links_into(l.to_node, l.to_port);
        const size_t row = size_t(std::ranges::find(into, i) - into.begin());
        ed::Link(kLinkBase + i, pin_id(l.from_node, true, out_slot), pin_id(l.to_node, false, slot_of(ts->inputs, l.to_port), row),
                 ImGui::ColorConvertU32ToFloat4(port_color(fs->outputs[out_slot].type)), 2.0f);
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

    // Delete key / editor deletions: collect first, then apply links (highest index first), then nodes.
    std::vector<size_t> dead_links;
    std::vector<int> dead_nodes;
    if (ed::BeginDelete()) {
        ed::LinkId link;
        while (ed::QueryDeletedLink(&link)) {
            if (!s.build_mode)
                ed::RejectDeletedItem();  // Use layout: the structure is fixed
            else if (ed::AcceptDeletedItem())
                dead_links.push_back(size_t(link.Get() - kLinkBase));
        }
        ed::NodeId node;
        while (ed::QueryDeletedNode(&node)) {
            if (!s.build_mode)
                ed::RejectDeletedItem();
            else if (ed::AcceptDeletedItem())
                dead_nodes.push_back(int(node.Get()));
        }
    }
    ed::EndDelete();
    std::ranges::sort(dead_links, std::greater<>());
    for (size_t i : dead_links) s.graph.disconnect(i);
    for (int id : dead_nodes) s.graph.remove_node(id);

    // Menus: right-click a node, a link or empty canvas; or let go of a dragged link on empty canvas.
    ed::Suspend();
    ed::NodeId clicked_node;
    ed::LinkId clicked_link;
    if (!s.build_mode) {
        // Use layout: no structural menus. A right-click on empty canvas says where to go instead.
        if (ed::ShowBackgroundContextMenu() || ed::ShowNodeContextMenu(&clicked_node) ||
            ed::ShowLinkContextMenu(&clicked_link))
            s.status = "Switch to Build layout (top of the Pipeline panel) to add, remove or relink blocks.";
    } else if (open_pin_menu) {
        s.menu_pos = ImGui::GetMousePos();
        ImGui::OpenPopup("add_connected");
    } else if (ed::ShowNodeContextMenu(&clicked_node)) {
        s.menu_node = int(clicked_node.Get());
        s.menu_pos = ImGui::GetMousePos();
        ImGui::OpenPopup("node_menu");
    } else if (ed::ShowLinkContextMenu(&clicked_link)) {
        s.menu_link = size_t(clicked_link.Get() - kLinkBase);
        s.menu_pos = ImGui::GetMousePos();
        ImGui::OpenPopup("link_menu");
    } else if (ed::ShowBackgroundContextMenu()) {
        s.menu_pos = ImGui::GetMousePos();
        ImGui::OpenPopup("add_node");
    }

    // A menu entry for a node type: its readable name, the description on hover.
    auto node_item = [](const remod::NodeSpec& spec) {
        const std::string label = std::string(spec.manual ? "Your step: " : "") + spec.title;
        const bool picked = ImGui::MenuItem(label.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", spec.summary);
        return picked;
    };
    auto place = [&](int id) { ed::SetNodePosition(id, ed::ScreenToCanvas(s.menu_pos)); };
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
            if (node_item(spec)) place(s.graph.add_node(spec.type).id);
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
            for (const auto& c : choices)
                if (node_item(*c.spec)) attempt([&] { place(s.graph.add_connected(c, ref.node, port, ref.output)); });
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("link_menu")) {
        if (ImGui::BeginMenu("Insert node here")) {
            const auto fits = s.graph.choices_for_link(s.menu_link);
            if (fits.empty()) ImGui::TextDisabled("Nothing fits on this link.");
            for (const auto* spec : fits)
                if (node_item(*spec)) attempt([&] { place(s.graph.insert_node(s.menu_link, spec->type)); });
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
            });
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Same type and values, no connections.");
        if (ImGui::MenuItem("Disconnect all")) s.graph.disconnect_node(s.menu_node);
        if (ImGui::MenuItem("Delete")) s.graph.remove_node(s.menu_node);
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
            remember_paths(s);
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
    ed::SetCurrentEditor(editor);
    ed::GetStyle().LinkStrength = 0.0f;  // straight links; ponytail: restore curves (default 100) later
    ed::SetCurrentEditor(nullptr);
    const bool nfd_ok = NFD_Init() == NFD_OKAY;  // pickers just won't open if this fails
    State state;
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
        const ImGuiID dockspace = ImGui::DockSpaceOverViewport();
        static bool layout_done = false;
        if (!layout_done) {  // Pipeline on the left, Graph filling the rest
            layout_done = true;
            ImGui::DockBuilderRemoveNode(dockspace);
            ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->Size);
            ImGuiID left = 0, right = 0;
            ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Left, 0.3f, &left, &right);
            ImGui::DockBuilderDockWindow("Pipeline", left);
            ImGui::DockBuilderDockWindow("Graph", right);
            ImGui::DockBuilderFinish(dockspace);
        }
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
    remember_paths(state);
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
