#pragma once
// Custom nodes (CLAUDE.md §4; user, 2026-10-02: "make subgraphs a kind of custom node"): block types the user makes
// from a graph. Inside, Input and Output blocks are its pins. Each lives in a library (one file per custom node), and
// every graph using one keeps a copy (Graph::customs), so it opens and runs anywhere. To run, preview or validate, a
// graph's custom blocks are expanded into their inner blocks (expand_customs), so the engine itself never meets one.
#include "graph.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace remod {

bool is_custom(std::string_view type);  // "custom:..."

// ---- Library: one JSON file per custom node ----

// %APPDATA%\remod\nodes (empty if APPDATA isn't set).
std::filesystem::path custom_library_dir();
// Every custom node in `dir`, sorted by title; a file that can't be read is skipped and reported in `errors`.
std::vector<CustomNode> load_custom_library(const std::filesystem::path& dir, std::vector<std::string>* errors = nullptr);
// Writes <dir>/<name>.json (the type without "custom:"), folders made. Throws GraphError.
void save_custom_node(const CustomNode& node, const std::filesystem::path& dir);

// ---- Registry: custom types as block types ----

// Makes `node` a block type: find_spec finds it, custom_specs lists it. Its pins come from its Input and Output blocks
// (top to bottom), each of the kind it's linked to inside; an Input with a default is a field that can be typed.
// Registering a type again replaces it; the custom nodes it uses are registered too.
void register_custom(const CustomNode& node);
const CustomNode* find_custom(std::string_view type);  // the registered definition, or nullptr
const NodeSpec* find_custom_spec(std::string_view type);
std::vector<const NodeSpec*> custom_specs();  // every registered custom type, sorted by title

// ---- Making and updating ----

// Makes a custom node of the selected blocks: they're replaced by one block of the new type, placed where they were.
// Each link crossing the selection becomes a pin (an Input or Output block inside, named after what it joins). The
// new type is registered and copied into the graph. Returns the new block's id. Throws GraphError (nothing selected,
// no title).
int make_custom_node(Graph& graph, const std::vector<int>& selected, const std::string& title,
                     CustomNode* made = nullptr);

// Puts `node` (e.g. the library's newer version) in place of the graph's copy of its type, and registers it.
void update_custom(Graph& graph, const CustomNode& node);
// The types whose library version differs from the graph's copy (offer an update).
std::vector<std::string> library_differs(const Graph& graph, const std::vector<CustomNode>& library);
// Does the graph have a block of this custom type?
bool uses(const Graph& graph, const std::string& type);
bool has_customs(const Graph& graph);  // any custom block at all

// ---- Expansion ----

// The graph with every custom block replaced by its inner blocks (custom blocks inside them too): what runs.
// - The graph's own blocks keep their ids; inner ones get new ids above them, titled "<block> (in <custom block>)".
// - A link into a pin goes on to what its Input feeds; a typed pin value (or the Input's default) becomes a Value
//   block; what an Output receives goes on along the custom block's links.
// - State the inner blocks keep (Edit image's "done", ...) lives on the custom block as "<inner id>:<key>".
struct ExpandedGraph {
    Graph graph;
    // Expanded block -> (the block of the original graph it belongs to, the prefix its state is kept under there).
    std::map<int, std::pair<int, std::string>> origin;
    // A custom block's output -> the expanded block output that gives it.
    std::map<std::pair<int, std::string>, std::pair<int, std::string>> outputs;
};
ExpandedGraph expand_customs(const Graph& graph);  // throws GraphError (unknown type, a custom node inside itself)

// A run's or preview's results, by expanded block, back onto the original graph: a custom block's status sums up its
// inner blocks' (its `items` list each inner Edit image step, and each item of one, with the key set_edit_done takes);
// its outputs' values are what feeds them; inner state goes to its params under the inner block's prefix.
void fold_results(const ExpandedGraph& expanded, std::map<int, NodeStatus>* nodes, RunValues* values,
                  std::map<int, std::map<std::string, std::string>>* state, std::vector<int>* reset_edits);

}  // namespace remod
