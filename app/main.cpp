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
    // Link drawing: pin centres (canvas coordinates) recorded while drawing the nodes, and the routes, recomputed
    // only when a block or pin moves. routed[i] = the graph link that routes.paths[i] belongs to.
    std::map<std::uintptr_t, ImVec2> pin_pos;
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
                              .build_mode = s.build_mode,
                              .show_descriptions = s.show_descriptions};
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
        remod::apply_run(s.graph, r);  // e.g. a re-exported image un-does an earlier "Done editing"
        s.statuses = r.nodes;
        s.status = r.message;
        s.warnings = r.warnings;  // shown in a popup (draw_warnings)
    } catch (const remod::RunError& e) {
        s.statuses = e.nodes;
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

// Opens a file in the user's image editor (the "edit" verb, e.g. Paint), else whatever opens it.
void open_in_editor(const std::filesystem::path& file) {
    if (reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, L"edit", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        ::ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void draw_side_panel(State& s) {
    ImGui::Begin("Pipeline");
    if (ImGui::Checkbox("Show help", &s.show_help)) remember_paths(s);

    if (s.show_help && !s.build_mode) {
        ImGui::TextWrapped("1. Original texture: pick the game's .tex file (the picker opens in your REtool folder).");
        ImGui::TextWrapped("2. Run: Export image writes the image, and the run stops at Edit image - your step.");
        ImGui::TextWrapped("3. On Edit image: Open in editor, change and save the image (same size and format), click Done editing.");
        ImGui::TextWrapped("4. Run again: Convert image to texture and Package for Fluffy build the mod .zip.");
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

// Pin and line colour by what flows (CLAUDE.md §4, "Link colours"), so it's visible what can plug into what. One hue
// per kind, all readable on the dark canvas. Reserved for later kinds: script (175, 125, 255) purple, AI call
// (240, 105, 180) pink.
ImU32 port_color(remod::PortType type) {
    switch (type) {
    case remod::PortType::Tex: return IM_COL32(235, 150, 60, 255);    // orange
    case remod::PortType::Image: return IM_COL32(90, 200, 110, 255);   // green
    case remod::PortType::Text: return IM_COL32(120, 180, 255, 255);   // blue
    case remod::PortType::Path: return IM_COL32(235, 215, 90, 255);    // yellow
    case remod::PortType::Folder: return IM_COL32(70, 205, 195, 255);  // teal
    case remod::PortType::Any: return IM_COL32(170, 170, 170, 255);    // grey: a Split with nothing linked in yet
    }
    return IM_COL32_WHITE;
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

// One pin: a small circle on the node border (links attach to its centre, drags start from it) around the row's
// label, which is drawn inside the node - left-aligned for inputs, right-aligned for outputs. Filled once
// connected. Rows are frame-height tall so labels line up with the text boxes next to them.
void draw_pin(State& s, ed::PinId id, const std::string& label, remod::PortType type, bool output, bool connected,
              float x0, float node_width, float edge_x) {
    ed::BeginPin(id, output ? ed::PinKind::Output : ed::PinKind::Input);
    if (output) ImGui::SetCursorPosX(x0 + node_width - ImGui::CalcTextSize(label.c_str()).x);
    const float y = ImGui::GetCursorScreenPos().y + ImGui::GetFrameHeight() * 0.5f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label.c_str());

    const ImVec2 center(edge_x, y);
    s.pin_pos[id.Get()] = center;
    const float r = ImGui::GetFontSize() * 0.3f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (connected)
        draw->AddCircleFilled(center, r, ImGui::GetColorU32(port_color(type)));  // (style alpha: Faded)
    else
        draw->AddCircle(center, r, ImGui::GetColorU32(port_color(type)), 0, 2.0f);
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

// A routed link: straight runs joined by rounded corners. `inset` keeps both ends off the pin circles' centres.
void draw_route(ImDrawList* draw, const std::vector<remod::Pt>& path, float radius, float inset, ImU32 color,
                float thickness) {
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
    draw->PathStroke(color, ImDrawFlags_None, thickness);
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
    // At the left end: links with no clean route, drawn as numbered ends (portals). Tidy up usually gives them one.
    if (const auto portals = std::ranges::count(s.routes.portals, 1); portals > 0) {
        const ImVec2 after = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(line_start.x, line_start.y + style.FramePadding.y));
        ImGui::TextColored(kAmber, "%d link%s without a clean route", int(portals), portals == 1 ? "" : "s");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Shown as numbered ends instead of a line: the blocks are too close or in the way.\n%s",
                              s.build_mode ? "Tidy up (right) or move the blocks apart." : "Switch to Build layout to tidy up.");
        ImGui::SetCursorPos(after);
    }
    // At the right end of the same line: Tidy up (Build layout) and the blocks' description texts, which take a lot
    // of room once known.
    const char* label = "Descriptions";
    const char* tidy = "Tidy up";
    float box = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize(label).x;
    if (s.build_mode) box += ImGui::CalcTextSize(tidy).x + style.FramePadding.x * 2 + style.ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImMax(0.0f, ImGui::GetContentRegionAvail().x - box));
    if (s.build_mode) {
        if (ImGui::Button(tidy)) s.tidy_requested = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Line the blocks up in columns, in step order, each level with the blocks feeding it.");
        ImGui::SameLine();
    }
    if (ImGui::Checkbox(label, &s.show_descriptions)) remember_paths(s);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show what each block does under its title. When off, hover a block's title to see it.");
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
        s.navigate = true;
    } else if (s.navigate) {  // one frame later, once node sizes are known
        fit_view(editor, view_size);
        s.navigate = false;
        if (s.tidy_after_load) s.tidy_requested = true, s.tidy_after_load = false;
    }

    const float font = ImGui::GetFontSize();
    const ImGuiStyle& style = ImGui::GetStyle();
    s.pin_pos.clear();
    const float button_width = ImGui::CalcTextSize("...").x + style.FramePadding.x * 2;
    const ImVec4 padding = ed::GetStyle().NodePadding;  // x = left, z = right
    std::string hovered_hint;  // tooltip drawn after the nodes, outside the canvas transform
    // Zoomed out: the overview, blocks showing only their (bigger) title, status and linked rows; the rest folds
    // away. `detail` eases between 1 (all) and 0 (overview) over a quarter second, so blocks and their lines change
    // smoothly; the switch has some slack (55% / 65% zoom) so it doesn't flicker at the threshold.
    if (zoom < 0.55f) s.overview = true;
    else if (zoom > 0.65f) s.overview = false;
    const float ease = ImGui::GetIO().DeltaTime / 0.25f;
    s.detail = s.overview ? ImMax(0.0f, s.detail - ease) : ImMin(1.0f, s.detail + ease);
    const float detail = s.detail;
    std::string inputs = s.graph_path + "\n" + std::to_string(s.graph.links.size());
    for (const auto& n : s.graph.nodes)
        for (const auto& [key, value] : n.params) inputs += "\n" + std::to_string(n.id) + key + "=" + value;
    if (inputs != s.dest_inputs || ImGui::GetTime() - s.dest_checked > 1.5) {
        s.dest_warnings = remod::destination_warnings(s.graph, std::filesystem::absolute(s.graph_path).parent_path());
        s.dest_inputs = std::move(inputs);
        s.dest_checked = ImGui::GetTime();
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
        // Border = where the node got to in the last run; a manual step is always marked, amber until done; a
        // warning (e.g. the destination exists) is amber too.
        const ImVec4 border = status ? state_color(status->state)
                              : manual ? (edit_done ? state_color(remod::NodeState::Done) : kAmber)
                              : warned ? kAmber
                                       : ed::GetStyle().Colors[ed::StyleColor_NodeBorder];
        ed::PushStyleColor(ed::StyleColor_NodeBorder, border);
        ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, status || manual || warned ? 3.0f : 1.0f);
        ed::BeginNode(n.id);
        ImGui::PushID(n.id);
        // A utility (Split, Text) is a small, quiet block: narrow, a smaller plain title, its description only as
        // the title's tooltip, no type name. The main steps get the room.
        const bool utility = spec && spec->utility;
        const bool fields = spec && (std::ranges::any_of(spec->inputs, &remod::InputSpec::editable) ||
                                     std::ranges::any_of(spec->outputs, [](const auto& o) { return o.field != nullptr; }));
        const float label_width = utility ? font * 4 : font * 7, field_width = utility ? font * 8 : font * 14;
        const float node_width = utility && !fields ? font * 7
                                                    : label_width + field_width + style.ItemSpacing.x + button_width;
        const float x0 = ImGui::GetCursorPosX();
        const float left_edge = ImGui::GetCursorScreenPos().x - padding.x;  // node border, where pins sit
        const float right_edge = ImGui::GetCursorScreenPos().x + node_width + padding.z;
        if (!spec) {
            ImGui::Text("%s (unknown node type)", n.type.c_str());
        } else {
            // A large title, readable without zooming in (larger in the overview), with the type at its right (Build
            // layout only); below it small: the step badge, the description (if shown) and the last run's status.
            // A block the user named ("Mod Output Folder") keeps what it is at the title's right, in both layouts.
            const std::string title = remod::block_title(n);
            const bool named = title != spec->title;
            const char* type = named ? spec->title : s.build_mode && !utility ? n.type.c_str() : nullptr;
            const float type_width = type ? ImGui::CalcTextSize(type).x + style.ItemSpacing.x : 0;
            const ImVec2 title_at = ImGui::GetCursorScreenPos();
            // 1.6 -> 2.6 times the font as detail goes; in 0.05 steps, so a fold doesn't rasterize a size per frame.
            // A utility's: 1.1 -> 1.6, in grey.
            const float scale = utility ? 1.1f + (1 - detail) * 0.5f : 1.6f + (1 - detail);
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * std::round(scale * 20) / 20);
            const float title_size = ImGui::GetFontSize();
            wrapped_text(title.c_str(), node_width - type_width,
                         ImGui::GetColorU32(utility ? ImVec4(0.75f, 0.75f, 0.75f, 1) : ImVec4(0.55f, 0.8f, 1, 1)));
            ImGui::PopFont();
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
                    ImVec2(title_at.x + node_width - ImGui::CalcTextSize(type).x, title_at.y + title_size - font),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled, detail), type);
            if (manual) ImGui::TextColored(kAmber, "YOUR STEP");
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
                auto from = [&](size_t link) {
                    return std::string(in.hint) + "\nFrom: " + source_of(s.graph, s.graph.links[link]);
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
                                     node_width, left_edge);
                            if (ImGui::IsItemHovered()) hovered_hint = row < linked.size() ? from(linked[row]) : in.hint;
                        });
                    }
                    ImGui::PopID();
                    return;
                }

                folding(linked.empty() ? detail : 1.0f, [&] {
                    const std::string label = in.required ? std::string(in.label) + " *" : std::string(in.label);
                    draw_pin(s, pin_id(n.id, false, slot), label, pin_type, false, !linked.empty(), x0, node_width,
                             left_edge);
                    if (ImGui::IsItemHovered()) hovered_hint = linked.empty() ? std::string(in.hint) : from(linked[0]);
                    if (!in.editable()) return;
                    // Not SameLine(x): inside a node (an ImGui group) that offset is group-relative, so x0 would be
                    // counted twice and nodes would widen with their canvas position.
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(x0 + label_width);
                    const Faded field(linked.empty() ? 1.0f : detail);  // a linked row stays; its field fades
                    std::string& value = n.params[in.name];
                    if (!linked.empty()) {  // a link overrides the typed value
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextDisabled("linked");
                        if (ImGui::IsItemHovered()) hovered_hint = from(linked[0]);
                    } else if (in.widget == remod::Widget::Checkbox) {
                        bool on = value == "true";
                        if (ImGui::Checkbox("##v", &on)) value = on ? "true" : "";
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
                    if (ImGui::Button("Open in editor")) open_in_editor(file);
                    if (ImGui::IsItemHovered()) hovered_hint = "Opens the image in your image editor (whatever opens that file type).";
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
                            draw_pin(s, pin_id(n.id, true, i, row), label, s.graph.output_type(n.id, out.name), true, row < linked.size(), x0,
                                     node_width, right_edge);
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
                    ImGui::SetNextItemWidth(field_width - (dest && s.build_mode ? flip_width : 0));
                    ImGui::InputTextWithHint("##v", dest ? dest->label : out.field_label, &value);
                    if (ImGui::IsItemHovered()) hovered_hint = dest ? dest->hint : out.hint;
                    ImGui::SameLine();
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
                draw_pin(s, pin_id(n.id, true, i), out.label, s.graph.output_type(n.id, out.name), true, s.graph.is_connected(n.id, out.name, true),
                         x0, node_width, right_edge);
                ImGui::PopID();
            }
            for (size_t slot = 0; slot < spec->inputs.size(); ++slot)
                if (spec->inputs[slot].multiple) draw_input(slot);
        }
        ImGui::Dummy(ImVec2(node_width, 0));  // fixes the node width so the right border (and its pins) line up
        ImGui::PopID();
        ed::EndNode();
        ed::PopStyleVar();
        ed::PopStyleColor();
    }
    ImGui::SetFontRasterizerDensity(old_density);  // tooltips and menus below are drawn unzoomed
    if (!hovered_hint.empty()) {
        ed::Suspend();
        ImGui::SetTooltip("%s", hovered_hint.c_str());
        ed::Resume();
    }

    // Placement (core): a block just added makes room once its size is known; blocks let go after dragging keep
    // `min_gap` (three line lanes) from the others, so lines always have room between blocks.
    const float lane = font * 0.8f, min_gap = lane * 3;
    {
        std::vector<std::array<float, 2>> positions, sizes;
        for (const auto& n : s.graph.nodes) {
            const ImVec2 p = ed::GetNodePosition(n.id), size = ed::GetNodeSize(n.id);
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
            const ImVec2 p = ed::GetNodePosition(n.id), size = ed::GetNodeSize(n.id);
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
    std::vector<ImU32> colors;
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
        const auto a = s.pin_pos.find(pin_id(l.from_node, true, out_slot, from_row).Get());
        const auto b = s.pin_pos.find(pin_id(l.to_node, false, slot_of(ts->inputs, l.to_port), row).Get());
        if (a == s.pin_pos.end() || b == s.pin_pos.end()) continue;
        // Every link its own net: a value used in several places goes through a Split, so no line branches.
        requests.push_back({int(i), {a->second.x, a->second.y}, {b->second.x, b->second.y}});
        routed.push_back(i);
        colors.push_back(port_color(s.graph.output_type(l.from_node, l.from_port)));
    }
    // While blocks move, one quick pass per frame (Debug: 7 ms for the 5-block example); once they stop, all three
    // passes (19 ms), which untangle crossings. ponytail: cache per link or throttle if layouts get much bigger.
    if (blocks != s.route_blocks || requests != s.route_requests) {
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
    const float pin_r = font * 0.3f;
    int portal_number = 0;
    for (size_t i = 0; i < s.routes.paths.size() && i < colors.size(); ++i) {
        const bool hovered = int(i) == hovered_any;
        if (i >= s.routes.portals.size() || !s.routes.portals[i]) {
            draw_route(draw, s.routes.paths[i], font * 0.6f, pin_r, colors[i], int(i) == hovered_link ? 4.0f : 2.0f);
            continue;
        }
        // No clean route (core portals): a stub at each pin ending in a tag with the same number at both ends;
        // hovering either end shows where it goes, as a dashed straight line.
        const auto& p = s.routes.paths[i];
        const std::string number = std::to_string(++portal_number);
        const ImVec2 out_end(p[1].x, p[1].y), in_end(p[2].x, p[2].y);
        draw->AddLine(ImVec2(p[0].x + pin_r, p[0].y), out_end, colors[i], hovered ? 4.0f : 2.0f);
        draw->AddLine(in_end, ImVec2(p[3].x - pin_r, p[3].y), colors[i], hovered ? 4.0f : 2.0f);
        for (const ImVec2 end : {out_end, in_end}) {
            draw->AddCircleFilled(end, font * 0.6f, colors[i]);
            draw->AddText(end - ImGui::CalcTextSize(number.c_str()) * 0.5f, IM_COL32(20, 20, 20, 255), number.c_str());
        }
        if (hovered) {
            const float length = ImLength(in_end - out_end), dash = font * 0.5f;
            for (float d = 0; d < length; d += dash * 2)
                draw->AddLine(out_end + (in_end - out_end) * (d / length),
                              out_end + (in_end - out_end) * (ImMin(d + dash, length) / length), colors[i], 1.5f);
            if (i < s.routed.size()) {
                const remod::Link& l = s.graph.links[s.routed[i]];
                portal_tip = "No clean route for this link (both ends show " + number + ").\nFrom: " +
                             source_of(s.graph, l) + "\nTo: " + target_of(s.graph, l) + "\n" +
                             (s.build_mode ? "Move the blocks apart, or Tidy up." : "Switch to Build layout to tidy up.");
            }
        }
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
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", spec.summary);
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
    // A scalable font, so text stays sharp at any zoom and DPI (ImGui's default is a 13 px pixel font that turns
    // blocky when scaled). Segoe UI ships with Windows; ImGui's embedded vector font if it's somehow missing.
    char windows[MAX_PATH] = {};
    const std::string segoe = std::string(windows, ::GetWindowsDirectoryA(windows, MAX_PATH)) + "\\Fonts\\segoeui.ttf";
    if (!std::filesystem::is_regular_file(segoe) || !io.Fonts->AddFontFromFileTTF(segoe.c_str(), 16.0f))
        io.Fonts->AddFontDefaultVector();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleDpi = scale;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    ed::Config config;
    config.SettingsFile = nullptr;  // positions are saved in the graph file instead of NodeEditor.json
    ed::EditorContext* editor = ed::CreateEditor(&config);
    ed::SetCurrentEditor(editor);
    ed::GetStyle().LinkStrength = 0.0f;  // the line shown while dragging a new link: straight (links are routed)
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
        if (!layout_done) {  // Browser left, Graph middle, Pipeline right; along the bottom the viewer, then Textures
            layout_done = true;
            ImGui::DockBuilderRemoveNode(dockspace);
            ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->Size);
            ImGuiID top = 0, bottom = 0, corner = 0, textures = 0, left = 0, rest = 0, right = 0, middle = 0;
            ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Down, 0.32f, &bottom, &top);
            ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Left, 0.25f, &corner, &textures);
            ImGui::DockBuilderSplitNode(top, ImGuiDir_Left, 0.25f, &left, &rest);
            ImGui::DockBuilderSplitNode(rest, ImGuiDir_Right, 0.3f, &right, &middle);
            ImGui::DockBuilderDockWindow("Browser", left);
            ImGui::DockBuilderDockWindow("Graph", middle);
            ImGui::DockBuilderDockWindow("Pipeline", right);
            ImGui::DockBuilderDockWindow("###viewer", corner);
            ImGui::DockBuilderDockWindow("Textures", textures);
            ImGui::DockBuilderFinish(dockspace);
        }
        if (const std::string picked = browser->draw(game_files_dir(state), unquote(state.noesis_path), state.profiles); !picked.empty())
            state.pending_texture = picked;
        draw_side_panel(state);
        draw_canvas(state, editor);
        draw_warnings(state);

        ImGui::Render();
        const float clear[4] = {0.1f, 0.1f, 0.1f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_occluded = g_swap_chain->Present(1, 0) == DXGI_STATUS_OCCLUDED;
    }

    if (state.run.valid()) state.run.wait();  // let a running graph finish (every tool call has a timeout)
    remember_paths(state);
    browser.reset();  // its GPU textures, before the device goes
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
