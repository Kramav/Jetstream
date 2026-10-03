#pragma once
// Node graph (CLAUDE.md §3/§4): editing operations, JSON file format, runner. The node types themselves are in
// nodes.hpp. Front ends (CLI, app) only call into these; they hold no graph logic.
#include "image.hpp"
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

struct CustomNode;

struct Graph {
    std::string profile = "re4r";  // profile id: profiles/<id>.toml
    std::vector<Node> nodes;
    std::vector<Link> links;
    // Links run top to bottom (outputs on blocks' bottom edges, inputs on their top), else left to right. Part of the
    // layout, like block positions (file: "flow": "down").
    bool downward = false;
    // The custom nodes this graph uses (custom.hpp): its own copies, so it opens and runs anywhere (file:
    // "custom_nodes"). add_node copies a definition in from the registry the first time its type is used.
    std::vector<CustomNode> customs;
    bool operator==(const Graph& other) const;  // undo history, unsaved changes

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

// A block type the user made (CLAUDE.md §4 Custom nodes): a graph whose Input and Output blocks are its pins. `type`
// is "custom:<name>"; `graph.customs` holds the custom nodes it uses in turn.
struct CustomNode {
    std::string type;
    std::string title;
    std::string summary;
    Graph graph;
    bool operator==(const CustomNode&) const = default;
};

inline bool Graph::operator==(const Graph& o) const {
    return profile == o.profile && nodes == o.nodes && links == o.links && downward == o.downward &&
           customs == o.customs;
}

// An input's value as known without running: typed, or from a linked Value block (followed back through Splits);
// "" if a step's result decides it (known only in a run). For front ends acting before a run, e.g. Edit image's
// "Open with".
std::string known_value(const Graph& graph, int node, const std::string& input);

// "{game}" in a field stands for the game files folder (the extracted natives\STM, e.g. REtool's), so a graph made on
// one PC (the examples, a shared mod) runs on another: e.g. {game}\_chainsaw\ui\ui3200\tex\x.tex.143221013. Front
// ends set it from their settings (the Game files folder). ponytail: one folder for every game; per profile once a
// second game is supported.
void set_game_files_dir(const std::filesystem::path& dir);
// `text` with {game} filled in; throws GraphError if it uses {game} while no folder is set.
std::string fill_game(const std::string& text);
// The other way, for front ends storing a picked path: one inside the game files folder as {game}\<the rest>, so the
// graph keeps working on another PC; any other path as it is.
std::string with_game_token(const std::string& path);

// Is this destination row flipped (circle on the left, set by a link)? Stored as the node param "flip:<input>".
bool is_flipped(const Node& node, std::string_view input);

// A block's heading: the name the user gave it (node param "title"), else its type's title. E.g. a Value named
// "Mod Output Folder". Run messages name blocks this way too.
std::string block_title(const Node& node);
void set_block_title(Graph& graph, int node, const std::string& title);  // trimmed; "" = back to the type's title

// JSON graph file, schema_version 0. Example: schemas/graph.v0.example.json
// Older files are migrated on load (PackageMod's former `screenshot` field becomes an ImportImage -> preview; an
// output feeding several inputs gets a Split block).
// `added_blocks`: set to whether migrating added blocks, or no block has a position (a graph a program wrote): they have
// no considered place yet, so front ends tidy up.
Graph load_graph(const std::filesystem::path& file, bool* added_blocks = nullptr);
void save_graph(const Graph& graph, const std::filesystem::path& file);

// A change a step makes to files: not counting the run's temporary folder and run cache (CLAUDE.md §10 M2,
// guardrails). Remove: deleted, or moved away from there. Run: starts a program (`path`), which may change any file
// the user can; what it changes isn't known in advance.
enum class ChangeKind { Write, Remove, MakeFolder, Run };
struct FileChange {
    int node = 0;
    ChangeKind kind = ChangeKind::Write;
    std::filesystem::path path;  // absolute
    bool operator==(const FileChange&) const = default;
};

struct RunOptions {
    const Profile& profile;
    ITextureConverter& converter;
    std::filesystem::path base_dir;  // relative paths resolve against this (the graph file's folder)
    std::function<void(const std::string&)> log = {};
    bool edits_done = false;  // treat every Edit image step as done (the CLI's --edited, where there's no button)
    std::filesystem::path cache_dir;  // results reused by later runs (default_cache_dir()); empty = none
    // Asked before every FileChange: "" allows it, else why not, and the step fails with that. Empty: all allowed (the
    // app and the CLI, where the user runs their own graph). A program's runs set it (Guard).
    std::function<std::string(const FileChange&)> check_change = {};
};

// Before a run: the changes it will make as far as previews know them (nothing is touched), and the steps whose
// changes are decided only in the run (a path coming from a step that hasn't run).
struct ChangePlan {
    std::vector<FileChange> changes;
    std::vector<int> unknown;
};
ChangePlan plan_changes(const Graph& graph, const std::filesystem::path& base_dir);

// Guardrails for runs a program (an AI) starts. Never inside `read_only` (the game files): refused, no approval
// lifts it. Removing a file, writing outside `graph_dir`, or running a program (Run program; nothing checks what it
// does) needs the user's approval: allowed only if `approved` holds that change (kind and path). Writing and making
// folders inside `graph_dir` is fine.
struct Guard {
    std::vector<std::filesystem::path> read_only;
    std::filesystem::path graph_dir;
    std::vector<FileChange> approved;  // node ids aren't compared
    enum class Verdict { Ok, NeedsApproval, Refused };
    Verdict judge(const FileChange& change, std::string* why = nullptr) const;
    std::string check(const FileChange& change) const;  // for RunOptions::check_change: "" or why not
};

// Where each node got to in a run, for front ends to show. NotNeeded: a required input got nothing (a branch not
// taken, e.g. an If whose condition was no), so it didn't run and passed nothing on.
enum class NodeState { NotReached, Done, Waiting, Failed, NotNeeded };
// A block repeated for a list (fan-out, list_source): each item's own outcome.
struct ItemStatus {
    std::string name;  // the item's {name}
    std::string key;   // the item's path in its list (relative to the folder): per-item state is kept under it
    NodeState state = NodeState::NotReached;
    std::string message;
    std::filesystem::path file;
};
struct NodeStatus {
    NodeState state = NodeState::NotReached;
    std::string message;          // short, for the node: "exported x.png", the error, ...
    std::filesystem::path file;   // the file concerned, e.g. the image an Edit image step waits on
    std::vector<ItemStatus> items;  // a block repeated for a list: per item, in list order (state/message sum them up)
};

// What each output gave in a run, by (node, output): its text (the text itself, or a file's full path). A block
// repeated for a list gives the shown item's (its list block's "show" param: an item key), else the first one's.
using RunValues = std::map<std::pair<int, std::string>, std::string>;

// One item of a list (fan-out): {name} in the repeated blocks' fields, and the key its per-item state is kept under.
struct ListItem {
    std::string name;  // e.g. "cs_ui3210_file_039_00_iam" for .../cs_ui3210_file_039_00_iam.tex.143221013
    std::string key;   // its path relative to the list's folder, e.g. "ui/cs_ui3210_file_039_00_iam.tex.143221013"
};

struct RunResult {
    bool paused = false;  // an Edit image step is waiting for the user
    std::string message;
    std::map<int, NodeStatus> nodes;
    std::vector<int> reset_edits;  // Edit image steps whose image was just re-exported: their "done" no longer holds
    std::map<int, std::map<std::string, std::string>> state;  // node state params a run records (apply_run sets them)
    std::vector<std::string> warnings;  // things the user should check in game, e.g. a changed mip count
    RunValues values;                   // what every output gave (front ends show what a link held)
};

// A node failed: the message names it, `nodes` says where every node got to.
struct RunError : GraphError {
    std::map<int, NodeStatus> nodes;
    RunValues values;  // what the outputs that ran gave
    std::map<int, std::map<std::string, std::string>> state;  // as RunResult's: steps done before the failure count
    RunError(const std::string& message, std::map<int, NodeStatus> statuses)
        : GraphError(message), nodes(std::move(statuses)) {}
};

// Runs nodes in dependency order. Throws GraphError if the graph isn't runnable, RunError if a node fails.
RunResult run_graph(const Graph& graph, const RunOptions& options);

// Every output's value as far as it's known without running (front ends show what links hold): pure blocks are run
// (NodeSpec::pure), steps give their predictable outputs (NodeSpec::preview, e.g. the path they'll write) and touch
// nothing. Outputs only a run makes (a temporary .tex) are left out, and so is everything that depends on them.
// Relative paths resolve against `base_dir`, as in a run.
// `lists`: each list block's items as far as known (front ends offer which one the previews show).
RunValues preview_values(const Graph& graph, const std::filesystem::path& base_dir,
                         std::map<int, std::vector<ListItem>>* lists = nullptr);

// An image block's result (NodeSpec::thumbnail) worked out in memory, for a live thumbnail of at most `max_side`
// pixels: its input images come from linked image blocks (worked out the same way, through Splits) or from the files
// the previews (`preview`, preview_values) or typed fields name, through the block's operation. Export image hands on
// its image file once it exists, else its texture (so `load` must also read textures, e.g. decode_tex); Edit image and
// Use existing image hand on theirs. `scale` = thumbnail
// pixels per real pixel, so sizes and positions are scaled to match. `load` gives a file's image already shrunk to at
// most max_side, with its scale (front ends cache it). `profile` reads a texture's size for Resize's "Match size of"
// (none: unknown). nullopt if an input image isn't known yet (e.g. not exported) or a value isn't usable; `why` then
// gets the reason when there is one (a file that can't be read, a Frame width past the frame's middle).
struct ImagePreview {
    Bgra image;
    float scale = 1;
    std::map<std::string, float> found;  // what a field left on "auto" (0) came to, in its own units (real pixels)
};
using ImageLoader = std::function<std::optional<ImagePreview>(const std::filesystem::path&)>;
std::optional<ImagePreview> preview_image(const Graph& graph, const RunValues& preview, int node,
                                          const std::filesystem::path& base_dir, unsigned max_side,
                                          const ImageLoader& load, const Profile* profile = nullptr,
                                          std::string* why = nullptr);

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

// Applies what a run found to the graph: clears "done" on Edit image steps whose image was re-exported, and records
// the state params steps recorded (Export image: which texture its file is from and the file's write time).
void apply_run(Graph& graph, const RunResult& result);

// Marks an Edit image step done (the user finished editing) or not. `item`: one item of a step repeated for a list
// (ItemStatus::key); "" = the step itself, or every item of a repeated one.
void set_edit_done(Graph& graph, int node, bool done, const std::string& item = {});

// Fan-out: the block whose list output (PortSpec::list, e.g. Files in folder) a block is repeated for, or 0. A block
// is repeated for a list when a list, or a block repeated for it, feeds one of its inputs that takes a single value;
// one that takes it only into inputs taking many (`multiple`, e.g. Package's textures) runs once and gets every item.
int list_source(const Graph& graph, int node);

// The browser's "Use in graph": the Original texture (LoadTex) block a picked texture goes into. `selected` if it
// is one, else the graph's only one; 0 if neither.
int texture_target(const Graph& graph, int selected);

// A small helper: a utility block with nothing linked into it (a Value, a Text). Its links come into what they feed
// from above (front ends put that input on the block's top edge) and Tidy up puts it in a row above (user,
// 2026-10-02: inputs from the top only from helpers, never from large blocks).
bool is_helper(const Graph& graph, int node);
bool from_above(const Graph& graph, const Link& link);  // its source is a helper

// "Tidy up": positions in columns by step order and rows (user, 2026-10-02: "more vertical inputs", "branches stack
// down", narrower graphs). The longest chain of links is one row; every other block goes as late as it can (just
// before the first block it feeds, so a side branch lines up under where it joins). A helper feeding the main chain
// (is_helper: a Value, a Text) goes in a row above it, right over what it feeds; every other branch (blocks off the
// main chain, linked together; a Preview) in a row below, sharing a row with branches whose columns don't overlap. A
// Split takes no column: it sits in the gap before what it feeds, level with what feeds it. Columns are as wide as
// their widest block, `gap_x` apart; rows as tall as their tallest stack, blocks `gap_y` apart. Past `max_width`
// (0: never) the columns wrap onto a new band of rows underneath, cut where no branch spans the cut if possible. The
// layout starts at the blocks' current top-left corner. `sizes` (width, height) and the result (top-left corners) are
// in `graph.nodes` order. `downward`: the flow runs top to bottom (columns become rows, "above" becomes "left of").
std::vector<std::array<float, 2>> tidy_layout(const Graph& graph, const std::vector<std::array<float, 2>>& sizes,
                                              float gap_x, float gap_y, bool downward = false, float max_width = 0);

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
// `downward`: the flow runs top to bottom ("right" becomes "below").
std::vector<std::array<float, 2>> make_room(const Graph& graph, const std::vector<std::array<float, 2>>& positions,
                                            const std::vector<std::array<float, 2>>& sizes, int id, float gap_x,
                                            float min_gap, bool downward = false);

}  // namespace remod
