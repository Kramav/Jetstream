#include "node_run.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <utility>

namespace remod {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string node_label(const Node& n) {
    const NodeSpec* spec = find_spec(n.type);
    return std::string(spec ? spec->title : n.type.c_str()) + " (node " + std::to_string(n.id) + ")";
}

bool has_extension(const std::string& path, const char* filter) {
    std::string ext = fs::path(path).extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext.size() < 2) return false;
    std::istringstream list(filter);
    for (std::string item; std::getline(list, item, ',');)
        if (ext.substr(1) == item) return true;
    return false;
}

bool is_option(const InputSpec& in, const std::string& value) {
    return std::ranges::any_of(in.options, [&](const auto& o) { return value == o[0]; });
}

namespace {

const PortSpec* find_output(const NodeSpec& spec, const std::string& name) {
    for (const auto& p : spec.outputs)
        if (name == p.name) return &p;
    return nullptr;
}

const char* type_name(PortType t) {
    switch (t) {
    case PortType::Tex: return "a texture";
    case PortType::Image: return "an image";
    case PortType::Text: return "text";
    case PortType::Path: return "a file path";
    case PortType::Folder: return "a folder";
    case PortType::Any: return "anything";
    }
    return "?";
}

// "png,tga" -> ".png, .tga"
std::string extension_list(const char* filter) {
    std::string exts = ".";
    for (const char* c = filter; *c; ++c) exts += *c == ',' ? std::string(", .") : std::string(1, *c);
    return exts;
}

// Kahn's algorithm; ties keep file order so runs are deterministic. Returns fewer nodes than exist on a cycle.
std::vector<const Node*> topo_order(const Graph& g) {
    std::map<int, int> indegree;
    for (const auto& n : g.nodes) indegree[n.id] = 0;
    for (const auto& l : g.links)
        if (indegree.contains(l.from_node) && indegree.contains(l.to_node)) ++indegree[l.to_node];
    std::vector<const Node*> order;
    std::set<int> done;
    for (bool progress = true; progress;) {
        progress = false;
        for (const auto& n : g.nodes) {
            if (done.contains(n.id) || indegree[n.id] != 0) continue;
            done.insert(n.id);
            order.push_back(&n);
            for (const auto& l : g.links)
                if (l.from_node == n.id && indegree.contains(l.to_node)) --indegree[l.to_node];
            progress = true;
        }
    }
    return order;
}

// Checks one link against the node specs; "" if fine.
std::string check_link(const Graph& g, const Link& l) {
    const Node* from = g.find(l.from_node);
    const Node* to = g.find(l.to_node);
    if (!from || !to) return "link refers to a missing node";
    if (from == to) return "a node can't link to itself";
    const NodeSpec* from_spec = find_spec(from->type);
    const NodeSpec* to_spec = find_spec(to->type);
    if (!from_spec || !to_spec) return "link touches a node of unknown type";
    const PortSpec* out = find_output(*from_spec, l.from_port);
    const InputSpec* in = find_input(*to_spec, l.to_port);
    if (!out) return node_label(*from) + " has no output '" + l.from_port + "'";
    if (!in) return node_label(*to) + " has no input '" + l.to_port + "'";
    if (!accepts(*in, g.output_type(l.from_node, l.from_port)))
        return std::string("'") + out->label + "' can't go into '" + in->label + "': that input needs " +
               type_name(in->type);
    return "";
}

// Scratch folder for intermediate files of one run, removed afterwards.
struct TempDir {
    fs::path path = fs::temp_directory_path() / ("remod_run_" + std::to_string(std::random_device{}()));
    TempDir() { fs::create_directories(path); }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

}  // namespace

Node& Graph::add_node(const std::string& type) {
    const NodeSpec* spec = find_spec(type);
    if (!spec) throw GraphError("unknown node type '" + type + "'");
    int id = 1;
    for (const auto& n : nodes) id = std::max(id, n.id + 1);
    Node n{.id = id, .type = type};
    for (const auto& in : spec->inputs)
        if (in.editable()) n.params[in.name] = in.initial;
    for (const auto& out : spec->outputs)
        if (out.field) n.params[out.field] = "";
    return nodes.emplace_back(std::move(n));
}

void Graph::remove_node(int id) {
    std::erase_if(links, [id](const Link& l) { return l.from_node == id || l.to_node == id; });
    std::erase_if(nodes, [id](const Node& n) { return n.id == id; });
}

std::string Graph::can_connect(const Link& link) const {
    if (auto err = check_link(*this, link); !err.empty()) return err;
    const InputSpec* in = find_input(*find_spec(find(link.to_node)->type), link.to_port);
    if (!in->multiple && !links_into(link.to_node, link.to_port).empty())
        return std::string("'") + in->label + "' already has a link; delete that one first";
    const PortSpec* out = find_output(*find_spec(find(link.from_node)->type), link.from_port);
    if (!out->multiple && !links_from(link.from_node, link.from_port).empty())
        return std::string("'") + out->label + "' already goes to a step. To use it in several places, put a Split "
               "block on its link (right-click the link).";
    Graph trial = *this;  // ponytail: copies the graph per check; fine for hand-built graphs of a few nodes
    trial.links.push_back(link);
    if (topo_order(trial).size() != trial.nodes.size()) return "that link would create a loop";
    // Into a Split: what it passes on changes, and every step after it must still take that.
    for (const Link& l : trial.links) {
        const Node* to = trial.find(l.to_node);
        const NodeSpec* spec = to ? find_spec(to->type) : nullptr;
        const InputSpec* after = spec ? find_input(*spec, l.to_port) : nullptr;
        const PortType type = trial.output_type(l.from_node, l.from_port);
        if (after && !accepts(*after, type))
            return std::string("a Split would pass ") + type_name(type) + " on to '" + after->label +
                   "', which needs " + type_name(after->type);
    }
    return "";
}

std::string Graph::connect(const Link& link) {
    auto err = can_connect(link);
    if (err.empty()) links.push_back(link);
    return err;
}

void Graph::disconnect(size_t link_index) {
    if (link_index < links.size()) links.erase(links.begin() + static_cast<std::ptrdiff_t>(link_index));
}

bool Graph::is_connected(int node, const std::string& port, bool output) const {
    return std::ranges::any_of(links, [&](const Link& l) {
        return output ? l.from_node == node && l.from_port == port : l.to_node == node && l.to_port == port;
    });
}

std::vector<size_t> Graph::links_into(int node, const std::string& input) const {
    std::vector<size_t> out;
    for (size_t i = 0; i < links.size(); ++i)
        if (links[i].to_node == node && links[i].to_port == input) out.push_back(i);
    return out;
}

std::vector<size_t> Graph::links_from(int node, const std::string& output) const {
    std::vector<size_t> out;
    for (size_t i = 0; i < links.size(); ++i)
        if (links[i].from_node == node && links[i].from_port == output) out.push_back(i);
    return out;
}

const Node* Graph::find(int id) const {
    for (const auto& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

Node* Graph::find(int id) { return const_cast<Node*>(std::as_const(*this).find(id)); }

namespace {

// The input of `spec` that best takes an output of type `out`: exact type first, then (for text) an editable
// field (from text or a path), a multiple input, or a pass-through (a Split takes anything). nullptr if none.
const InputSpec* best_input(const NodeSpec& spec, PortType out) {
    for (const auto& in : spec.inputs)
        if (in.type == out) return &in;
    for (const auto& in : spec.inputs)
        if (accepts(in, out) && (in.multiple || out == PortType::Text || out == PortType::Path || in.type == PortType::Any))
            return &in;
    return nullptr;
}

// The output of `spec` that `in` best takes: exact type first. nullptr if none.
const PortSpec* best_output(const NodeSpec& spec, const InputSpec& in) {
    for (const auto& out : spec.outputs)
        if (out.type == in.type) return &out;
    for (const auto& out : spec.outputs)
        if (accepts(in, out.type)) return &out;
    return nullptr;
}

const InputSpec* input_of(const Graph& g, int node, const std::string& port) {
    const Node* n = g.find(node);
    const NodeSpec* spec = n ? find_spec(n->type) : nullptr;
    return spec ? find_input(*spec, port) : nullptr;
}

const PortSpec* output_of(const Graph& g, int node, const std::string& port) {
    const Node* n = g.find(node);
    const NodeSpec* spec = n ? find_spec(n->type) : nullptr;
    return spec ? find_output(*spec, port) : nullptr;
}

}  // namespace

PortType Graph::output_type(int node, const std::string& output) const {
    std::string port = output;
    for (size_t hop = 0; hop <= nodes.size(); ++hop) {  // bounded: a loop (refused anyway) can't hang it
        const PortSpec* out = output_of(*this, node, port);
        if (!out || out->type != PortType::Any) return out ? out->type : PortType::Any;
        const auto& inputs = find_spec(find(node)->type)->inputs;
        const auto in = std::ranges::find(inputs, PortType::Any, &InputSpec::type);  // what it passes on
        if (in == inputs.end()) return wanted_type(node, port);  // nothing passed on (a Value): what it feeds
        const auto linked = links_into(node, in->name);
        if (linked.empty()) return PortType::Any;
        node = links[linked[0]].from_node;
        port = links[linked[0]].from_port;
    }
    return PortType::Any;
}

PortType Graph::wanted_type(int node, const std::string& output) const {
    std::string port = output;
    for (size_t hop = 0; hop <= nodes.size(); ++hop) {  // bounded, like output_type
        const auto from = links_from(node, port);
        if (from.empty()) return PortType::Any;
        const Link& l = links[from[0]];
        const InputSpec* in = input_of(*this, l.to_node, l.to_port);
        if (!in || in->type != PortType::Any) return in ? in->type : PortType::Any;
        const auto& outputs = find_spec(find(l.to_node)->type)->outputs;  // into a Split: on to what it feeds
        const auto out = std::ranges::find(outputs, PortType::Any, &PortSpec::type);
        if (out == outputs.end()) return PortType::Any;
        node = l.to_node;
        port = out->name;
    }
    return PortType::Any;
}

std::vector<Graph::Choice> Graph::choices_for_pin(int node, const std::string& port, bool output) const {
    std::vector<Choice> out;
    if (output) {
        const PortSpec* p = output_of(*this, node, port);
        if (!p) return out;
        for (const auto& spec : node_specs())
            if (const InputSpec* in = best_input(spec, output_type(node, port))) out.push_back({&spec, in->name});
    } else {
        const InputSpec* in = input_of(*this, node, port);
        if (!in) return out;
        for (const auto& spec : node_specs())
            if (const PortSpec* p = best_output(spec, *in)) out.push_back({&spec, p->name});
    }
    return out;
}

int Graph::add_connected(const Choice& choice, int node, const std::string& port, bool output) {
    const int id = add_node(choice.spec->type).id;
    if (!output) {  // the new node feeds this input: a single input's old link makes way
        const InputSpec* in = input_of(*this, node, port);
        if (in && !in->multiple) {
            const auto old = links_into(node, port);
            for (auto it = old.rbegin(); it != old.rend(); ++it) disconnect(*it);
        }
    }
    const Link link = output ? Link{node, port, id, choice.port} : Link{id, choice.port, node, port};
    if (auto err = connect(link); !err.empty()) {
        remove_node(id);
        throw GraphError(err);
    }
    return id;
}

std::vector<const NodeSpec*> Graph::choices_for_link(size_t link) const {
    std::vector<const NodeSpec*> out;
    if (link >= links.size()) return out;
    const PortSpec* src = output_of(*this, links[link].from_node, links[link].from_port);
    const InputSpec* dst = input_of(*this, links[link].to_node, links[link].to_port);
    if (!src || !dst) return out;
    for (const auto& spec : node_specs())
        if (best_input(spec, output_type(links[link].from_node, links[link].from_port)) && best_output(spec, *dst))
            out.push_back(&spec);
    return out;
}

int Graph::insert_node(size_t link, const std::string& type) {
    if (link >= links.size()) throw GraphError("no such link");
    const NodeSpec* spec = find_spec(type);
    if (!spec) throw GraphError("unknown node type '" + type + "'");
    const Link old = links[link];
    const PortSpec* src = output_of(*this, old.from_node, old.from_port);
    const InputSpec* dst = input_of(*this, old.to_node, old.to_port);
    const InputSpec* in = src ? best_input(*spec, output_type(old.from_node, old.from_port)) : nullptr;
    const PortSpec* out = dst ? best_output(*spec, *dst) : nullptr;
    if (!in || !out) throw GraphError(std::string(spec->title) + " can't go on that link");

    const Graph before = *this;
    disconnect(link);
    const int id = add_node(type).id;
    const std::string err1 = connect({old.from_node, old.from_port, id, in->name});
    const std::string err2 = err1.empty() ? connect({id, out->name, old.to_node, old.to_port}) : "";
    if (!err1.empty() || !err2.empty()) {
        *this = before;
        throw GraphError(err1.empty() ? err2 : err1);
    }
    return id;
}

int Graph::duplicate_node(int id) {
    const Node* n = find(id);
    if (!n) throw GraphError("no such node");
    const NodeSpec* spec = find_spec(n->type);
    Node copy = *n;
    if (spec)
        for (const char* state : spec->state) copy.params.erase(state);
    int new_id = 1;
    for (const auto& other : nodes) new_id = std::max(new_id, other.id + 1);
    copy.id = new_id;
    copy.x += 40;
    copy.y += 40;
    nodes.push_back(std::move(copy));
    return new_id;
}

void Graph::disconnect_node(int id) {
    std::erase_if(links, [id](const Link& l) { return l.from_node == id || l.to_node == id; });
}

std::vector<std::string> Graph::validate() const {
    std::vector<std::string> errors;
    if (profile.empty()) errors.push_back("graph has no profile");
    if (nodes.empty()) errors.push_back("graph has no nodes");

    std::set<int> ids;
    for (const auto& n : nodes) {
        if (!ids.insert(n.id).second) errors.push_back("duplicate node id " + std::to_string(n.id));
        const NodeSpec* spec = find_spec(n.type);
        if (!spec) {
            errors.push_back(node_label(n) + ": unknown node type");
            continue;
        }
        for (const auto& in : spec->inputs) {
            const size_t linked = links_into(n.id, in.name).size();
            if (linked > 1 && !in.multiple)
                errors.push_back(node_label(n) + ": '" + in.label + "' is connected more than once");
            if (in.editable() && linked == 0) {  // the typed value counts
                const auto it = n.params.find(in.name);
                const bool empty = it == n.params.end() || it->second.empty();
                if (in.required && empty) errors.push_back(node_label(n) + ": " + in.label + " is required");
                if (!empty && in.filter && !has_extension(it->second, in.filter))
                    errors.push_back(node_label(n) + ": " + in.label + " must end in " + extension_list(in.filter));
                if (!empty && !in.options.empty() && !is_option(in, it->second))
                    errors.push_back(node_label(n) + ": " + in.label + " can't be '" + it->second + "'");
            } else if (!in.editable() && in.required && linked == 0) {
                errors.push_back(node_label(n) + ": input '" + in.label + "' is not connected");
            }
        }
        for (const auto& out : spec->outputs) {
            if (!out.multiple && links_from(n.id, out.name).size() > 1)
                errors.push_back(node_label(n) + ": '" + out.label + "' goes to more than one step; use a Split block");
            if (!out.field) continue;  // where the node writes an output: always required
            const auto it = n.params.find(out.field);
            if (it == n.params.end() || it->second.empty())
                errors.push_back(node_label(n) + ": " + out.field_label + " is required");
            else if (out.filter && !has_extension(it->second, out.filter))
                errors.push_back(node_label(n) + ": " + out.field_label + " must end in " + extension_list(out.filter));
        }
        for (const auto& [key, _] : n.params) {
            const InputSpec* in = find_input(*spec, key);
            const bool known = (in && in->editable()) ||
                               std::ranges::any_of(spec->outputs, [&](const PortSpec& o) { return o.field && key == o.field; }) ||
                               std::ranges::any_of(spec->state, [&](const char* s) { return key == s; });
            if (!known) errors.push_back(node_label(n) + ": unknown parameter '" + key + "'");
        }
    }
    for (const auto& l : links)
        if (auto err = check_link(*this, l); !err.empty()) errors.push_back(err);
    if (errors.empty() && topo_order(*this).size() != nodes.size()) errors.push_back("graph contains a loop");
    return errors;
}

namespace {

// Graph files from before inputs were unified: PackageMod had a typed `screenshot` path. An empty one is
// dropped; a set one becomes an ImportImage node feeding the new `preview` input.
void migrate(Graph& g) {
    std::vector<std::pair<int, std::string>> screenshots;
    for (auto& n : g.nodes) {
        if (n.type != "PackageMod") continue;
        if (const auto it = n.params.find("screenshot"); it != n.params.end()) {
            if (!it->second.empty()) screenshots.emplace_back(n.id, it->second);
            n.params.erase(it);
        }
        n.params.try_emplace("replace", "");
    }
    for (const auto& [package, path] : screenshots) {
        const Node* p = g.find(package);
        const float x = p->x - 450, y = p->y + 350;
        Node& img = g.add_node("ImportImage");
        img.params["png"] = path;
        img.x = x;
        img.y = y;
        g.links.push_back({img.id, "image", package, "preview"});
    }

    // Export image used to pause for editing itself, with an output named "image". Editing is now its own
    // Edit image step: insert one after each such export and route the old links through it.
    std::vector<int> exports;
    for (const auto& n : g.nodes)
        if (n.type == "ExportImage") exports.push_back(n.id);
    for (int id : exports) {
        const bool old_links = std::ranges::any_of(g.links, [&](const Link& l) { return l.from_node == id && l.from_port == "image"; });
        if (!old_links) continue;
        const Node* e = g.find(id);
        const float x = e->x, y = e->y + 300;
        Node& edit = g.add_node("EditImage");
        edit.x = x;
        edit.y = y;
        for (auto& l : g.links)
            if (l.from_node == id && l.from_port == "image") l = {edit.id, "image", l.to_node, l.to_port};
        g.links.push_back({id, "png", edit.id, "png"});
    }

    // An output used to feed several inputs at once, its line branching. That's a Split block's job now: one goes
    // after each such output and feeds the same inputs, in the same order. (Last: the steps above can make fan-outs.)
    std::map<std::pair<int, std::string>, std::vector<size_t>> fans;
    for (size_t i = 0; i < g.links.size(); ++i) fans[{g.links[i].from_node, g.links[i].from_port}].push_back(i);
    for (const auto& [from, outgoing] : fans) {
        const PortSpec* out = output_of(g, from.first, from.second);
        if (outgoing.size() < 2 || !out || out->multiple) continue;
        const Node* source = g.find(from.first);
        const float x = source->x + 400, y = source->y + 150;
        Node& split = g.add_node("Split");
        split.x = x;
        split.y = y;
        for (size_t i : outgoing) g.links[i] = {split.id, "out", g.links[i].to_node, g.links[i].to_port};
        g.links.push_back({from.first, from.second, split.id, "in"});
    }
}

}  // namespace

Graph load_graph(const fs::path& file) {
    std::ifstream in(file);
    if (!in) throw GraphError("cannot open graph file " + file.string());
    try {
        const json j = json::parse(in);
        if (j.at("schema_version").get<int>() != 0)
            throw GraphError(file.string() + ": unsupported schema_version (expected 0)");
        Graph g;
        g.profile = j.at("profile").get<std::string>();
        for (const auto& jn : j.at("nodes")) {
            Node n{.id = jn.at("id").get<int>(), .type = jn.at("type").get<std::string>()};
            if (jn.contains("params")) n.params = jn["params"].get<std::map<std::string, std::string>>();
            if (jn.contains("pos")) {
                n.x = jn["pos"].at(0).get<float>();
                n.y = jn["pos"].at(1).get<float>();
            }
            g.nodes.push_back(std::move(n));
        }
        for (const auto& jl : j.value("links", json::array()))
            g.links.push_back({jl.at("from").at(0).get<int>(), jl.at("from").at(1).get<std::string>(),
                               jl.at("to").at(0).get<int>(), jl.at("to").at(1).get<std::string>()});
        migrate(g);
        return g;
    } catch (const json::exception& e) {
        throw GraphError(file.string() + ": invalid graph file: " + e.what());
    }
}

void save_graph(const Graph& g, const fs::path& file) {
    json j{{"schema_version", 0}, {"profile", g.profile}, {"nodes", json::array()}, {"links", json::array()}};
    for (const auto& n : g.nodes)
        j["nodes"].push_back({{"id", n.id}, {"type", n.type}, {"params", n.params}, {"pos", {n.x, n.y}}});
    for (const auto& l : g.links)
        j["links"].push_back({{"from", {l.from_node, l.from_port}}, {"to", {l.to_node, l.to_port}}});
    std::ofstream out(file);
    out << j.dump(2) << "\n";
    if (!out.flush()) throw GraphError("failed to write " + file.string());
}

RunResult run_graph(const Graph& g, const RunOptions& opt) {
    if (const auto errors = g.validate(); !errors.empty()) {
        std::string msg = "graph can't run:";
        for (const auto& e : errors) msg += "\n  " + e;
        throw GraphError(msg);
    }
    if (g.profile != opt.profile.id)
        throw GraphError("graph is for profile '" + g.profile + "' but '" + opt.profile.id + "' was loaded");

    TempDir work;
    RunState run{.graph = g, .options = opt, .work_dir = work.path};
    RunResult& result = run.result;
    for (const Node* node : topo_order(g)) {
        const Node& n = *node;
        if (std::ranges::any_of(g.links, [&](const Link& l) { return l.to_node == n.id && run.waiting.contains(l.from_node); })) {
            run.waiting.insert(n.id);
            result.nodes[n.id] = {NodeState::NotReached, "waits for an earlier step", {}};
            continue;
        }
        try {
            NodeRun node_run(run, n);
            find_spec(n.type)->run(node_run);  // validate() checked the type
        } catch (const std::exception& e) {
            result.nodes[n.id] = {NodeState::Failed, e.what(), {}};
            for (const auto& other : g.nodes) result.nodes.try_emplace(other.id);  // the rest: not reached
            throw RunError(node_label(n) + ": " + e.what(), std::move(result.nodes));
        }
    }

    const std::vector<fs::path>& to_edit = run.to_edit;
    if (!to_edit.empty()) {
        result.paused = true;
        result.message = "Waiting for you: edit ";
        result.message += to_edit.size() == 1 ? to_edit[0].string() : std::to_string(to_edit.size()) + " images:";
        if (to_edit.size() > 1)
            for (const auto& p : to_edit) result.message += "\n  " + p.string();
        result.message += std::string("\nthen click Done editing on the Edit image step") + (to_edit.size() > 1 ? "s" : "") +
                          " and Run again (from the CLI: run again with --edited true).";
    } else {
        result.message = "Done.";
    }
    return std::move(result);
}

std::vector<Value> NodeRun::values(const char* input) const {
    std::vector<Value> v;
    for (size_t i : run.graph.links_into(node.id, input))
        v.push_back(run.outputs.at({run.graph.links[i].from_node, run.graph.links[i].from_port}));
    return v;
}

std::string NodeRun::text(const char* input) const {
    if (auto v = values(input); !v.empty()) return v.front().text;
    const auto it = node.params.find(input);
    return it == node.params.end() ? std::string() : it->second;
}

fs::path NodeRun::resolve(const std::string& p) const {
    if (p.empty()) return {};
    const fs::path path(p);
    return (path.is_absolute() ? path : run.options.base_dir / path).lexically_normal();
}

void NodeRun::done(const std::string& message, const fs::path& file) {
    run.result.nodes[node.id] = {NodeState::Done, message, file};
    log(message);
}

void NodeRun::wait(const std::string& message, const fs::path& file) {
    run.waiting.insert(node.id);
    run.to_edit.push_back(file);
    run.result.nodes[node.id] = {NodeState::Waiting, message, file};
}

void NodeRun::log(const std::string& line) const {
    if (run.options.log) run.options.log(node_label(node) + ": " + line);
}

void NodeRun::warn(const std::string& warning) {
    run.result.warnings.push_back(node_label(node) + ": " + warning);
    if (run.options.log) run.options.log("warning: " + run.result.warnings.back());
}

void apply_run(Graph& g, const RunResult& result) {
    for (int id : result.reset_edits) set_edit_done(g, id, false);
}

void set_edit_done(Graph& g, int node, bool done) {
    Node* n = g.find(node);
    if (!n || n->type != "EditImage") return;
    if (done)
        n->params["done"] = "true";
    else
        n->params.erase("done");
}

int texture_target(const Graph& g, int selected) {
    if (const Node* n = g.find(selected); n && n->type == "LoadTex") return selected;
    int only = 0;
    for (const Node& n : g.nodes) {
        if (n.type != "LoadTex") continue;
        if (only) return 0;
        only = n.id;
    }
    return only;
}


std::vector<std::array<float, 2>> tidy_layout(const Graph& g, const std::vector<std::array<float, 2>>& sizes,
                                              float gap_x, float gap_y) {
    const size_t n = g.nodes.size();
    std::vector<std::array<float, 2>> out(n);
    if (n == 0 || sizes.size() != n) return out;
    std::map<int, size_t> index;
    for (size_t i = 0; i < n; ++i) index[g.nodes[i].id] = i;
    std::vector<std::vector<size_t>> feeders(n);  // the blocks linking into each one
    for (const Link& l : g.links)
        if (index.contains(l.from_node) && index.contains(l.to_node) && l.from_node != l.to_node)
            feeders[index[l.to_node]].push_back(index[l.from_node]);

    // Column: one right of the furthest block feeding it (the longest chain of links into it). At most n rounds,
    // so a cycle can't loop forever.
    std::vector<size_t> column(n, 0);
    for (size_t round = 0; round < n; ++round) {
        bool changed = false;
        for (size_t i = 0; i < n; ++i)
            for (const size_t f : feeders[i])
                if (column[f] + 1 > column[i] && column[f] + 1 < n) column[i] = column[f] + 1, changed = true;
        if (!changed) break;
    }

    // Columns left to right from the current top-left corner, each as wide as its widest block. Within a column,
    // blocks go in the order of (and level with, where there's room) the blocks feeding them; blocks fed by nothing
    // keep their current order.
    float left = g.nodes[0].x, top = g.nodes[0].y;
    for (const Node& node : g.nodes) left = std::min(left, node.x), top = std::min(top, node.y);
    const size_t columns = *std::ranges::max_element(column) + 1;
    float x = left;
    for (size_t c = 0; c < columns; ++c) {
        struct Entry {
            float key;   // order in the column
            bool level;  // sit at `key` if there's room (fed by placed blocks), else just stack
            size_t i;
        };
        std::vector<Entry> blocks;
        float width = 0;
        for (size_t i = 0; i < n; ++i) {
            if (column[i] != c) continue;
            float want = 0, placed = 0;  // feeders already placed (in earlier columns; a cycle's aren't)
            for (const size_t f : feeders[i])
                if (column[f] < c) want += out[f][1], ++placed;
            blocks.push_back({placed > 0 ? want / placed : g.nodes[i].y, placed > 0, i});
            width = std::max(width, sizes[i][0]);
        }
        std::ranges::stable_sort(blocks, {}, &Entry::key);
        float y = top;
        for (const Entry& b : blocks) {
            if (b.level) y = std::max(y, b.key);
            out[b.i] = {x, y};
            y += sizes[b.i][1] + gap_y;
        }
        x += width + gap_x;
    }
    return out;
}

}  // namespace remod
