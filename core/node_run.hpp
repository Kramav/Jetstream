#pragma once
// Core only: what a node type's run function (NodeSpec::run, nodes.cpp) gets from the runner (run_graph, graph.cpp).
#include "graph.hpp"

#include <filesystem>
#include <map>
#include <set>
#include <string>
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

// One run of a graph, shared by its nodes.
struct RunState {
    const Graph& graph;
    const RunOptions& options;
    std::filesystem::path work_dir;  // scratch folder for intermediate files, removed after the run
    std::map<std::pair<int, std::string>, Value> outputs;
    RunResult result;
    // Edit image steps still waiting for the user, and everything downstream of them: skipped this run. Other
    // branches still run, so one run exports every image that needs editing.
    std::set<int> waiting;
    std::set<int> fresh_exports;  // Export image steps that wrote a new image in this run
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
    std::vector<Value> values(const char* input) const;  // every value linked into `input`, in link order
    std::string text(const char* input) const;           // an editable input: its link if connected, else typed
    Value input(const char* name) const { return values(name).at(0); }  // a required link-only input
    std::filesystem::path resolve(const std::string& path) const;       // relative to the graph's folder
    void output(const char* port, Value value) { run.outputs[{node.id, port}] = std::move(value); }

    void done(const std::string& message, const std::filesystem::path& file = {});
    void wait(const std::string& message, const std::filesystem::path& file);  // the user has something to do here
    void log(const std::string& line) const;  // "<node label>: <line>"
    void warn(const std::string& warning);    // RunResult::warnings (front ends show it), and the log
    // Before changing a file (not the temporary folder or run cache): a run asks RunOptions::check_change and throws
    // its reason; a preview records the change for plan_changes. Every step that changes files calls it, in its run
    // and in its preview, with the same paths.
    void change(ChangeKind kind, const std::filesystem::path& path);
};

// "Export image (node 2)": the user-facing name first, the id to find it by.
std::string node_label(const Node& n);

// True if `path` ends in one of the comma-separated extensions in `filter` ("png,jpg"), ignoring case.
bool has_extension(const std::string& path, const char* filter);

// A Choice input's stored value is one of its options.
bool is_option(const InputSpec& in, const std::string& value);

}  // namespace remod
