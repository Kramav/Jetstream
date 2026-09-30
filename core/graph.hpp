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

// Everything flowing along a link is a file on disk.
enum class PortType { Tex, Image };

struct PortSpec {
    const char* name;
    PortType type;
};
// Which picker a front end should offer for a parameter.
enum class PathKind { None, OpenFile, SaveFile, Folder };

struct ParamSpec {
    const char* name;
    bool required;
    const char* hint;
    PathKind path = PathKind::None;
    const char* filter = nullptr;  // extensions for file pickers, e.g. "png,jpg"; nullptr = all files
};
struct NodeSpec {
    const char* type;
    const char* summary;
    std::vector<PortSpec> inputs;  // all inputs must be connected
    std::vector<PortSpec> outputs;
    std::vector<ParamSpec> params;
};

// The M1 node types: LoadTex, ExportImage, ImportImage, SaveTex, PackageMod.
const std::vector<NodeSpec>& node_specs();
const NodeSpec* find_spec(std::string_view type);

struct Node {
    int id = 0;
    std::string type;
    std::map<std::string, std::string> params;
    float x = 0, y = 0;  // canvas position; only the editor uses it
};

struct Link {
    int from_node = 0;
    std::string from_port;
    int to_node = 0;
    std::string to_port;
};

struct Graph {
    std::string profile = "re4r";  // profile id: profiles/<id>.toml
    std::vector<Node> nodes;
    std::vector<Link> links;

    Node& add_node(const std::string& type);  // new unique id, all params present (empty)
    void remove_node(int id);                 // and its links
    std::string can_connect(const Link& link) const;  // why the link isn't allowed, or "" if it is
    std::string connect(const Link& link);            // can_connect, then add; returns the error or ""
    void disconnect(size_t link_index);
    const Node* find(int id) const;
    Node* find(int id);
    std::vector<std::string> validate() const;  // every problem that would stop a run; empty = runnable
};

// JSON graph file, schema_version 0. Example: schemas/graph.v0.example.json
Graph load_graph(const std::filesystem::path& file);
void save_graph(const Graph& graph, const std::filesystem::path& file);

struct RunOptions {
    const Profile& profile;
    ITextureConverter& converter;
    std::filesystem::path base_dir;  // relative paths in params resolve against this (the graph file's folder)
    std::function<void(const std::string&)> log = {};
};

struct RunResult {
    bool paused = false;  // an ExportImage wrote a new PNG: the user edits it, then runs again
    std::string message;
};

// Runs nodes in dependency order. Throws GraphError naming the node that failed.
RunResult run_graph(const Graph& graph, const RunOptions& options);

// "<natives root>/<rest>" in `file` (case-insensitive) -> "<rest>", else "". Lets LoadTex infer the game path
// when the .tex sits inside an extracted natives tree.
std::string game_path_from(const std::filesystem::path& file, const std::string& natives_root);

}  // namespace remod
