#pragma once
// Node graph (CLAUDE.md §3/§4): editing operations, JSON file format, runner. The node types themselves are in
// nodes.hpp. Front ends (CLI, app) only call into these; they hold no graph logic.
#include "nodes.hpp"
#include "profile.hpp"
#include "texture_converter.hpp"

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace remod {

struct GraphError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Node {
    int id = 0;
    std::string type;
    std::map<std::string, std::string> params;  // typed values of editable inputs
    float x = 0, y = 0;                          // canvas position; only the editor uses it
    bool operator==(const Node&) const = default;
};

struct Link {
    int from_node = 0;
    std::string from_port;
    int to_node = 0;
    std::string to_port;  // an input name
    bool operator==(const Link&) const = default;
};

struct Graph {
    std::string profile = "re4r";  // profile id: profiles/<id>.toml
    std::vector<Node> nodes;
    std::vector<Link> links;
    bool operator==(const Graph&) const = default;  // undo history, unsaved changes

    Node& add_node(const std::string& type);  // new unique id, every editable input present (its `initial` value)
    void remove_node(int id);                 // and its links
    std::string can_connect(const Link& link) const;  // why the link isn't allowed, or "" if it is
    std::string connect(const Link& link);            // can_connect, then add; returns the error or ""
    void disconnect(size_t link_index);
    bool is_connected(int node, const std::string& port, bool output) const;  // any link on that pin?
    std::vector<size_t> links_into(int node, const std::string& input) const;  // link indices, in order
    std::vector<size_t> links_from(int node, const std::string& output) const;  // link indices, in order
    // The type an output gives: its spec's, or for a pass-through (a Split) whatever is linked into it, followed back
    // through chained Splits. An open output that passes nothing on (a Value) has the kind it feeds (wanted_type).
    // Any if that's still open.
    PortType output_type(int node, const std::string& output) const;
    // The kind the inputs an output feeds want: its first link's input, followed on through Splits. Any if unlinked.
    PortType wanted_type(int node, const std::string& output) const;
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
    // Moves a destination row's circle to the other side (InputSpec::result): left, a link sets it; right (the
    // default), it's typed and the result goes on. The links of the side that goes away are removed.
    void flip(int node, const std::string& input);
};

// An input's value as known without running: typed, or from a linked Value block (followed back through Splits);
// "" if a step's result decides it (known only in a run). For front ends acting before a run, e.g. Edit image's
// "Open with".
std::string known_value(const Graph& graph, int node, const std::string& input);

// Is this destination row flipped (circle on the left, set by a link)? Stored as the node param "flip:<input>".
bool is_flipped(const Node& node, std::string_view input);

// A block's heading: the name the user gave it (node param "title"), else its type's title. E.g. a Value named
// "Mod Output Folder". Run messages name blocks this way too.
std::string block_title(const Node& node);
void set_block_title(Graph& graph, int node, const std::string& title);  // trimmed; "" = back to the type's title

// JSON graph file, schema_version 0. Example: schemas/graph.v0.example.json
// Older files are migrated on load (PackageMod's former `screenshot` field becomes an ImportImage -> preview; an
// output feeding several inputs gets a Split block).
// `added_blocks`: set to whether migrating added blocks (they have no considered place yet: front ends tidy up).
Graph load_graph(const std::filesystem::path& file, bool* added_blocks = nullptr);
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

// What each output gave in a run, by (node, output): its text (the text itself, or a file's full path).
using RunValues = std::map<std::pair<int, std::string>, std::string>;

struct RunResult {
    bool paused = false;  // an Edit image step is waiting for the user
    std::string message;
    std::map<int, NodeStatus> nodes;
    std::vector<int> reset_edits;  // Edit image steps whose image was just re-exported: their "done" no longer holds
    std::vector<std::string> warnings;  // things the user should check in game, e.g. a changed mip count
    RunValues values;                   // what every output gave (front ends show what a link held)
};

// A node failed: the message names it, `nodes` says where every node got to.
struct RunError : GraphError {
    std::map<int, NodeStatus> nodes;
    RunValues values;  // what the outputs that ran gave
    RunError(const std::string& message, std::map<int, NodeStatus> statuses)
        : GraphError(message), nodes(std::move(statuses)) {}
};

// Runs nodes in dependency order. Throws GraphError if the graph isn't runnable, RunError if a node fails.
RunResult run_graph(const Graph& graph, const RunOptions& options);

// Every output's value as far as it's known without running (front ends show what links hold): pure blocks are run
// (NodeSpec::pure), steps give their predictable outputs (NodeSpec::preview, e.g. the path they'll write) and touch
// nothing. Outputs only a run makes (a temporary .tex) are left out, and so is everything that depends on them.
// Relative paths resolve against `base_dir`, as in a run.
RunValues preview_values(const Graph& graph, const std::filesystem::path& base_dir);

// What link `link` holds: its source's preview value (`preview`, from preview_values: the graph as it is now), else
// what the source gave in the last run (`last_run`, RunResult::values; `*from_run` set); "" if neither.
std::string link_value(const Graph& graph, const RunValues& preview, const RunValues& last_run, size_t link,
                       bool* from_run = nullptr);

// Undo / redo (front ends): snapshots of the whole graph (blocks, links, values, positions; small). A front end calls
// track() whenever the graph may have changed and is settled (nothing being dragged or typed): a change since the
// last snapshot becomes one undo step. Up to 200 steps.
class History {
public:
    void reset(const Graph& graph);  // a loaded or new graph: no steps
    void track(const Graph& now);
    bool undo(Graph& graph);  // false if there's nothing to undo; else `graph` is the earlier one
    bool redo(Graph& graph);
    bool can_undo() const { return !past_.empty(); }
    bool can_redo() const { return !future_.empty(); }

private:
    std::vector<Graph> past_, future_;
    Graph last_;
};

// Node ids in the order a run takes them (dependency order, ties in file order). Nodes in a loop are left out.
std::vector<int> step_order(const Graph& graph);

// Applies what a run found to the graph: clears "done" on Edit image steps whose image was re-exported.
void apply_run(Graph& graph, const RunResult& result);

// Marks an Edit image step done (the user finished editing) or not.
void set_edit_done(Graph& graph, int node, bool done);

// The browser's "Use in graph": the Original texture (LoadTex) block a picked texture goes into. `selected` if it
// is one, else the graph's only one; 0 if neither.
int texture_target(const Graph& graph, int selected);

// "Tidy up": positions that line blocks up in columns by step order. A block goes one column right of the furthest
// block linking into it; within a column, blocks follow (and sit level with, where there's room) the blocks feeding
// them. Columns are as wide as their widest block, `gap_x` apart; blocks `gap_y` apart. The layout starts at the
// blocks' current top-left corner. `sizes` (width, height) and the result (top-left corners) are in `graph.nodes`
// order.
std::vector<std::array<float, 2>> tidy_layout(const Graph& graph, const std::vector<std::array<float, 2>>& sizes,
                                              float gap_x, float gap_y);

// Placement that leaves lines room (CLAUDE.md §4, links). Positions (top-left corners) and sizes (width, height) are
// in `graph.nodes` order (keep_apart: any order); both return a position for every block.
//
// A block let go closer than `min_gap` to another moves the least distance that clears them all; the others stay.
std::vector<std::array<float, 2>> keep_apart(const std::vector<std::array<float, 2>>& positions,
                                             const std::vector<std::array<float, 2>>& sizes, size_t moved,
                                             float min_gap);
// A block just added onto a link or a pin (`id`) goes `gap_x` right of the block feeding it, level with it (or left of
// the block it feeds, when nothing feeds it); the blocks after it move right as far as they need to keep `gap_x`,
// with everything after them, so the flow keeps its order; then it keeps `min_gap` from the rest (keep_apart).
std::vector<std::array<float, 2>> make_room(const Graph& graph, const std::vector<std::array<float, 2>>& positions,
                                            const std::vector<std::array<float, 2>>& sizes, int id, float gap_x,
                                            float min_gap);

}  // namespace remod
