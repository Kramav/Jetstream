#pragma once
// Core only: what a node type's run function (NodeSpec::run, nodes.cpp) gets from the runner (run_graph, graph.cpp).
#include "graph.hpp"

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace remod {

// What flows along a link while a graph runs.
struct Value {
    std::string text;             // what a text input sees: the text itself, or a file's full path
    std::filesystem::path path;   // files only
    std::string game_path;        // textures only; "" if unknown
};

inline Value file_value(const std::filesystem::path& p, std::string game_path = {}) {
    return {p.string(), p, std::move(game_path)};
}

// One item of a list (fan-out): {name} in the repeated blocks' fields, and the key its per-item state is kept under.
struct ListItem {
    std::string name;  // e.g. "cs_ui3210_file_039_00_iam" for .../cs_ui3210_file_039_00_iam.tex.143221013
    std::string key;   // its path relative to the list's folder, e.g. "ui/cs_ui3210_file_039_00_iam.tex.143221013"
};

// A block's output, by (node, output, item); item -1 for a block that isn't repeated.
using OutputKey = std::tuple<int, std::string, int>;

// One run of a graph, shared by its nodes.
struct RunState {
    const Graph& graph;
    const RunOptions& options;
    std::filesystem::path work_dir;  // scratch folder for intermediate files, removed after the run
    std::map<OutputKey, Value> outputs;
    RunResult result;
    // Fan-out: each block's list source (list_source, 0 = none), each list's items, the items left out after failing
    // (the list's "If an item fails: skip it"), and the item a repeated block is running for now (-1: not repeated).
    std::map<int, int> source;
    std::map<int, std::vector<ListItem>> items;
    std::map<int, std::set<int>> skipped;
    int item = -1;
    // Edit image steps still waiting for the user, and everything downstream of them: skipped this run, by (node,
    // item). Other branches and items still run, so one run exports every image that needs editing.
    std::set<std::pair<int, int>> waiting;
    std::set<std::pair<int, int>> fresh_exports;  // Export image steps (and items) that wrote a new image this run
    std::map<std::pair<int, std::filesystem::path>, int> writes;  // which item of a repeated block wrote a file
    std::vector<std::filesystem::path> to_edit;
    std::vector<FileChange>* planned = nullptr;  // plan_changes: previews record their changes here
};

// One node's view of the run: its inputs, its outputs, and where it reports.
class NodeRun {
public:
    NodeRun(RunState& run, const Node& node) : run(run), node(node) {}

    RunState& run;
    const Node& node;

    const Profile& profile() const { return run.options.profile; }
    // Every value linked into `input`, in link order. From a list (or a block repeated for it): the current item's, or
    // into a block that isn't repeated, every item's in list order (the skipped ones left out).
    std::vector<Value> values(const char* input) const;
    // An editable input: its link if connected, else typed. In a block repeated for a list, "{name}" becomes the
    // item's name.
    std::string text(const char* input) const;
    Value input(const char* name) const { return values(name).at(0); }  // a required link-only input
    std::filesystem::path resolve(const std::string& path) const;       // relative to the graph's folder
    void output(const char* port, Value value) { run.outputs[{node.id, port, run.item}] = std::move(value); }
    // A list output (PortSpec::list): one value per item, with the items' names and keys. The blocks it feeds repeat.
    void output_list(const char* port, std::vector<Value> values, std::vector<ListItem> items);
    const ListItem* item() const;  // the item a repeated block runs for now, else nullptr
    // A scratch file in the run's temporary folder, unique to this block (and item): "<id>[_<item>]<suffix>".
    std::filesystem::path temp_file(const std::string& suffix) const;
    // State the block keeps in the graph (NodeSpec::state; apply_run stores it): per item in a repeated block (kept
    // as "<name>@<item key>"). state() is the stored value or nullptr; set_state("") removes it.
    const std::string* state(const std::string& name) const;
    void set_state(const std::string& name, const std::string& value);

    void done(const std::string& message, const std::filesystem::path& file = {});
    void wait(const std::string& message, const std::filesystem::path& file);  // the user has something to do here
    void log(const std::string& line) const;  // "<node label>: <line>"
    void warn(const std::string& warning);    // RunResult::warnings (front ends show it), and the log
    // Before changing a file (not the temporary folder or run cache): a run asks RunOptions::check_change and throws
    // its reason; a preview records the change for plan_changes. Every step that changes files calls it, in its run
    // and in its preview, with the same paths. A write claims the file (below).
    void change(ChangeKind kind, const std::filesystem::path& path);
    // A file this block (and item) writes, or would write but keeps (Export's edited image): two items of a repeated
    // block claiming one file throws (put {name} in its name).
    void claim(const std::filesystem::path& file);
};

// "Export image (node 2)": the user-facing name first, the id to find it by.
std::string node_label(const Node& n);

// True if `path` ends in one of the comma-separated extensions in `filter` ("png,jpg"), ignoring case.
bool has_extension(const std::string& path, const char* filter);

// A Choice input's stored value is one of its options.
bool is_option(const InputSpec& in, const std::string& value);

}  // namespace remod
