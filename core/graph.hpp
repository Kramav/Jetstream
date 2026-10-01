#pragma once
// Node graph (CLAUDE.md §3/§4): node types, editing operations, JSON file format, runner.
// Front ends (CLI, app) only call into this; they hold no graph logic.
#include "profile.hpp"
#include "texture_converter.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace remod {

struct GraphError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// What flows along a link: a texture file, an image file, or plain text.
enum class PortType { Tex, Image, Text };

// Which picker a front end should offer for a path field. OpenTexture: an RE Engine texture of any known game
// (front ends build the filter from the profiles' tex suffixes).
enum class PathKind { None, OpenFile, OpenTexture, SaveFile, Folder };

// How an input can be typed in. None = link-only.
enum class Widget { None, Text, Path, Checkbox };

// `name`s are the stable ids stored in graph files; `title`, `label` and `hint` are what users read.
// An output may carry a typed field for where the node writes it (e.g. Export's "Image file"): front ends show it
// on the output side, and it's stored in Node::params under `field`. Such a field is always required.
struct PortSpec {  // an output
    const char* name;
    PortType type;
    const char* label;
    const char* field = nullptr;
    const char* field_label = "";
    const char* hint = "";
    PathKind path = PathKind::None;
    const char* filter = nullptr;
};

// An input. Every input has a pin. Editable inputs (widget != None) can instead be typed; a link wins over the
// typed value. `multiple` inputs are link-only and take any number of links, in link order.
struct InputSpec {
    const char* name;
    const char* label;
    PortType type;
    Widget widget = Widget::None;
    bool required = false;  // editable: typed or linked; link-only: at least one link
    bool multiple = false;
    const char* hint = "";
    PathKind path = PathKind::None;
    const char* filter = nullptr;  // extensions for typed file paths, e.g. "png,jpg"; nullptr = any
    bool editable() const { return widget != Widget::None; }
};

struct NodeSpec {
    const char* type;
    const char* title;
    const char* summary;
    std::vector<InputSpec> inputs;
    std::vector<PortSpec> outputs;
    std::vector<const char*> state = {};  // other params the node keeps (e.g. EditImage's "done"); no field shown
    bool manual = false;                  // a step the user does by hand (front ends mark it clearly)
};

// LoadTex, ExportImage, EditImage, ImportImage, SaveTex, PackageMod, Text.
const std::vector<NodeSpec>& node_specs();
const NodeSpec* find_spec(std::string_view type);
const InputSpec* find_input(const NodeSpec& spec, std::string_view name);

// Can an output of type `out` feed `in`? Same type; any output into a Text input (as its text/path); or Text
// into an editable input (a typed value, e.g. a path).
bool accepts(const InputSpec& in, PortType out);

struct Node {
    int id = 0;
    std::string type;
    std::map<std::string, std::string> params;  // typed values of editable inputs
    float x = 0, y = 0;                          // canvas position; only the editor uses it
};

struct Link {
    int from_node = 0;
    std::string from_port;
    int to_node = 0;
    std::string to_port;  // an input name
};

struct Graph {
    std::string profile = "re4r";  // profile id: profiles/<id>.toml
    std::vector<Node> nodes;
    std::vector<Link> links;

    Node& add_node(const std::string& type);  // new unique id, every editable input present (empty)
    void remove_node(int id);                 // and its links
    std::string can_connect(const Link& link) const;  // why the link isn't allowed, or "" if it is
    std::string connect(const Link& link);            // can_connect, then add; returns the error or ""
    void disconnect(size_t link_index);
    bool is_connected(int node, const std::string& port, bool output) const;  // any link on that pin?
    std::vector<size_t> links_into(int node, const std::string& input) const;  // link indices, in order
    const Node* find(int id) const;
    Node* find(int id);
    std::vector<std::string> validate() const;  // every problem that would stop a run; empty = runnable

    // ---- Editing helpers for front ends (the rules live here, not in the UI) ----
    struct Choice {
        const NodeSpec* spec;  // a node type that fits
        std::string port;      // which of its inputs/outputs would be connected
    };
    // Node types that could be attached to an existing pin: for an output pin, types with an input that takes it;
    // for an input pin, types with an output it takes. Prefers exact type matches; plain Text fits editable fields.
    std::vector<Choice> choices_for_pin(int node, const std::string& port, bool output) const;
    // Adds a node of `choice` and links it to that pin. A single (non-multiple) input's old link is replaced.
    // Returns the new node's id.
    int add_connected(const Choice& choice, int node, const std::string& port, bool output);
    // Node types that can sit on a link: an input that takes the link's source and an output its target takes.
    std::vector<const NodeSpec*> choices_for_link(size_t link) const;
    // Replaces the link with source -> new node -> target. Returns the new node's id.
    int insert_node(size_t link, const std::string& type);
    // Same type and typed values (not run state such as "done"), no links, placed a little offset.
    int duplicate_node(int id);
    void disconnect_node(int id);  // removes every link to or from it
};

// JSON graph file, schema_version 0. Example: schemas/graph.v0.example.json
// Older files are migrated on load (PackageMod's former `screenshot` field becomes an ImportImage -> preview).
Graph load_graph(const std::filesystem::path& file);
void save_graph(const Graph& graph, const std::filesystem::path& file);

struct RunOptions {
    const Profile& profile;
    ITextureConverter& converter;
    std::filesystem::path base_dir;  // relative paths resolve against this (the graph file's folder)
    std::function<void(const std::string&)> log = {};
    bool edits_done = false;  // treat every Edit image step as done (the CLI's --edited, where there's no button)
};

// Where each node got to in a run, for front ends to show.
enum class NodeState { NotReached, Done, Waiting, Failed };
struct NodeStatus {
    NodeState state = NodeState::NotReached;
    std::string message;          // short, for the node: "exported x.png", the error, ...
    std::filesystem::path file;   // the file concerned, e.g. the image an Edit image step waits on
};

struct RunResult {
    bool paused = false;  // an Edit image step is waiting for the user
    std::string message;
    std::map<int, NodeStatus> nodes;
    std::vector<int> reset_edits;  // Edit image steps whose image was just re-exported: their "done" no longer holds
    std::vector<std::string> warnings;  // things the user should check in game, e.g. a changed mip count
};

// A node failed: the message names it, `nodes` says where every node got to.
struct RunError : GraphError {
    std::map<int, NodeStatus> nodes;
    RunError(const std::string& message, std::map<int, NodeStatus> statuses)
        : GraphError(message), nodes(std::move(statuses)) {}
};

// Runs nodes in dependency order. Throws GraphError if the graph isn't runnable, RunError if a node fails.
RunResult run_graph(const Graph& graph, const RunOptions& options);

// Applies what a run found to the graph: clears "done" on Edit image steps whose image was re-exported.
void apply_run(Graph& graph, const RunResult& result);

// Marks an Edit image step done (the user finished editing) or not.
void set_edit_done(Graph& graph, int node, bool done);

// The browser's "Use in graph": the Original texture (LoadTex) block a picked texture goes into. `selected` if it
// is one, else the graph's only one; 0 if neither.
int texture_target(const Graph& graph, int selected);

// "<natives root>/<rest>" in `file` (case-insensitive) -> "<rest>", else "". Lets LoadTex infer the game path
// when the .tex sits inside an extracted natives tree.
std::string game_path_from(const std::filesystem::path& file, const std::string& natives_root);

// Fills "{1}", "{2}", ... in `text` with `parts` (1-based). Throws GraphError if text uses a part that isn't given.
std::string fill_template(const std::string& text, const std::vector<std::string>& parts);

}  // namespace remod
