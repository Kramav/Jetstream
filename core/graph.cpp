#include "node_run.hpp"
#include "custom.hpp"
#include "graph_json.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

namespace remod {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string node_label(const Node& n) { return block_title(n) + " (node " + std::to_string(n.id) + ")"; }

std::string block_title(const Node& n) {
    if (const auto it = n.params.find("title"); it != n.params.end() && !it->second.empty()) return it->second;
    const NodeSpec* spec = find_spec(n.type);
    return spec ? spec->title : n.type;
}

void set_block_title(Graph& g, int node, const std::string& title) {
    Node* n = g.find(node);
    if (!n) return;
    const auto first = title.find_first_not_of(" \t"), last = title.find_last_not_of(" \t");
    if (first == std::string::npos)
        n->params.erase("title");
    else
        n->params["title"] = title.substr(first, last - first + 1);
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

}  // namespace

std::string path_fit(PathKind kind, const char* filter, const std::string& path, bool is_folder) {
    if (kind == PathKind::None) return "This field takes text, not a path.";
    if (kind == PathKind::Folder) return is_folder ? "" : "This field needs a folder.";
    if (is_folder) return "This field needs a file.";
    if (filter && *filter && !has_extension(path, filter)) return "This field needs a " + extension_list(filter) + " file.";
    return "";
}

namespace {

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
    if (in->result && !is_flipped(*to, in->name))
        return std::string("'") + in->label + "' is typed on the block; flip its row (<> in Build layout) to link into it";
    for (const auto& dest : from_spec->inputs)
        if (dest.result && l.from_port == dest.result && is_flipped(*from, dest.name))
            return std::string("'") + out->label + "' isn't passed on while '" + dest.label +
                   "' takes a link; flip that row back (<> in Build layout)";
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
    if (is_custom(type) && std::ranges::none_of(customs, [&](const CustomNode& c) { return c.type == type; }))
        if (const CustomNode* def = find_custom(type)) customs.push_back(*def);  // the graph keeps its own copy
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
        if (in.type == out && !in.result) return &in;
    for (const auto& in : spec.inputs)
        if (!in.result && accepts(in, out) &&
            (in.multiple || out == PortType::Text || out == PortType::Path || in.type == PortType::Any))
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
        for (const NodeSpec* spec : all_specs())
            if (const InputSpec* in = best_input(*spec, output_type(node, port))) out.push_back({spec, in->name});
    } else {
        const InputSpec* in = input_of(*this, node, port);
        if (!in) return out;
        for (const NodeSpec* spec : all_specs())
            if (const PortSpec* p = best_output(*spec, *in)) out.push_back({spec, p->name});
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
    for (const NodeSpec* spec : all_specs())
        if (best_input(*spec, output_type(links[link].from_node, links[link].from_port)) && best_output(*spec, *dst))
            out.push_back(spec);
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

std::string known_value(const Graph& g, int node, const std::string& input) {
    std::string port = input;
    for (int id = node;;) {
        const auto into = g.links_into(id, port);
        if (into.empty()) {  // typed here (a Split with nothing linked in has no value)
            const Node* n = g.find(id);
            if (id != node || !n) return "";
            const auto it = n->params.find(input);
            return it == n->params.end() ? std::string() : it->second;
        }
        const Link& l = g.links[into[0]];
        const Node* from = g.find(l.from_node);
        if (!from) return "";
        if (from->type == "Value") {  // its typed value
            const auto it = from->params.find("value");
            return it == from->params.end() ? std::string() : it->second;
        }
        if (from->type != "Split") return "";  // a step's result: known only in a run
        id = from->id;  // a Split passes on what comes into it
        port = "in";
    }
}

bool is_flipped(const Node& node, std::string_view input) {
    const auto it = node.params.find("flip:" + std::string(input));
    return it != node.params.end() && it->second == "true";
}

void Graph::flip(int node, const std::string& input) {
    Node* n = find(node);
    const NodeSpec* spec = n ? find_spec(n->type) : nullptr;
    const InputSpec* in = spec ? find_input(*spec, input) : nullptr;
    if (!in || !in->result) return;
    const bool to_left = !is_flipped(*n, input);
    std::erase_if(links, [&](const Link& l) {  // the side going away
        return to_left ? l.from_node == node && l.from_port == in->result : l.to_node == node && l.to_port == input;
    });
    if (to_left)
        n->params["flip:" + input] = "true";
    else
        n->params.erase("flip:" + input);
}

std::vector<std::string> Graph::validate() const {
    if (has_customs(*this)) {  // checked as it runs: custom blocks expanded into their inner blocks
        try {
            return expand_customs(*this).graph.validate();
        } catch (const GraphError& e) {
            return {e.what()};
        }
    }
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
            const InputSpec* flips = key.starts_with("flip:") ? find_input(*spec, key.substr(5)) : nullptr;
            const std::string state_key = key.substr(0, key.find('@'));  // per-item state: "<name>@<item key>"
            const bool known = (in && in->editable()) || (flips && flips->result) || key == "title" ||
                               std::ranges::any_of(spec->outputs, [&](const PortSpec& o) { return o.field && key == o.field; }) ||
                               std::ranges::any_of(spec->state, [&](const char* s) { return state_key == s; });
            if (!known) errors.push_back(node_label(n) + ": unknown parameter '" + key + "'");
        }
        // Fan-out: one list per block, and {name} only where there's an item to name.
        const int list = list_source(*this, n.id);
        for (const auto& l : links) {
            if (l.to_node != n.id) continue;
            const InputSpec* in = find_input(*spec, l.to_port);
            const Node* from = find(l.from_node);
            const NodeSpec* from_spec = from ? find_spec(from->type) : nullptr;
            const PortSpec* out = from_spec ? find_output(*from_spec, l.from_port) : nullptr;
            const int other = out && out->list ? l.from_node : from ? list_source(*this, l.from_node) : 0;
            if (in && !in->multiple && other && other != list)
                errors.push_back(node_label(n) + ": two lists meet here (" + node_label(*find(list)) + " and " +
                                 node_label(*find(other)) + "); a block can be repeated for one list only");
        }
        // (A Value feeding a repeated block may hold it: the block fills it in.)
        const bool feeds_repeated = std::ranges::any_of(links, [&](const Link& l) {
            return l.from_node == n.id && list_source(*this, l.to_node) != 0;
        });
        if (!list && !feeds_repeated)
            for (const auto& [key, value] : n.params)
                if (value.find("{name}") != std::string::npos)
                    errors.push_back(node_label(n) + ": {name} only works in a block repeated for a list (Files in folder)");
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

nlohmann::json graph_json(const Graph& g) {
    json j{{"schema_version", 0}, {"profile", g.profile}, {"nodes", json::array()}, {"links", json::array()}};
    if (g.downward) j["flow"] = "down";
    for (const auto& n : g.nodes)
        j["nodes"].push_back({{"id", n.id}, {"type", n.type}, {"params", n.params}, {"pos", {n.x, n.y}}});
    for (const auto& l : g.links)
        j["links"].push_back({{"from", {l.from_node, l.from_port}}, {"to", {l.to_node, l.to_port}}});
    for (const CustomNode& c : g.customs) j["custom_nodes"].push_back(custom_json(c));
    return j;
}

Graph graph_from_json(const nlohmann::json& j) {
    if (j.at("schema_version").get<int>() != 0) throw GraphError("unsupported schema_version (expected 0)");
    Graph g;
    g.profile = j.at("profile").get<std::string>();
    g.downward = j.value("flow", std::string()) == "down";
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
    for (const auto& jc : j.value("custom_nodes", json::array())) g.customs.push_back(custom_from_json(jc));
    return g;
}

nlohmann::json custom_json(const CustomNode& c) {
    return {{"type", c.type}, {"title", c.title}, {"summary", c.summary}, {"graph", graph_json(c.graph)}};
}

CustomNode custom_from_json(const nlohmann::json& j) {
    CustomNode c{j.at("type").get<std::string>(), j.at("title").get<std::string>(), j.value("summary", std::string()),
                 graph_from_json(j.at("graph"))};
    if (!c.type.starts_with("custom:")) throw GraphError("a custom node's type must start with custom: (" + c.type + ")");
    return c;
}

Graph load_graph(const fs::path& file, bool* added_blocks) {
    std::ifstream in(file);
    if (!in) throw GraphError("cannot open graph file " + file.string());
    try {
        Graph g = graph_from_json(json::parse(in));
        for (const CustomNode& c : g.customs) register_custom(c);  // its copies are what it runs and shows
        const size_t before = g.nodes.size();
        migrate(g);
        if (added_blocks) *added_blocks = g.nodes.size() != before;
        return g;
    } catch (const json::exception& e) {
        throw GraphError(file.string() + ": invalid graph file: " + e.what());
    } catch (const GraphError& e) {
        throw GraphError(file.string() + ": " + e.what());
    }
}

void save_graph(const Graph& graph, const fs::path& file) {
    Graph g = graph;
    std::erase_if(g.customs, [&](const CustomNode& c) { return !uses(graph, c.type); });  // only the ones it uses
    std::ofstream out(file);
    out << graph_json(g).dump(2) << "\n";
    if (!out.flush()) throw GraphError("failed to write " + file.string());
}

int list_source(const Graph& g, int node) {
    // The first list reaching one of its single-value inputs (validate() refuses two different ones).
    std::set<int> seen;
    std::function<int(int)> source = [&](int id) -> int {
        if (!seen.insert(id).second) return 0;  // a loop (refused anyway)
        const Node* n = g.find(id);
        const NodeSpec* spec = n ? find_spec(n->type) : nullptr;
        if (!spec) return 0;
        for (const Link& l : g.links) {
            if (l.to_node != id) continue;
            const InputSpec* in = find_input(*spec, l.to_port);
            if (!in || in->multiple) continue;  // an input taking many collects the items
            const Node* from = g.find(l.from_node);
            const NodeSpec* from_spec = from ? find_spec(from->type) : nullptr;
            const PortSpec* out = from_spec ? find_output(*from_spec, l.from_port) : nullptr;
            if (out && out->list) return l.from_node;
            if (const int list = source(l.from_node)) return list;
        }
        return 0;
    };
    return source(node);
}

namespace {

// Fan-out bookkeeping for a run or a preview: each block's list source.
void prepare(RunState& run) {
    for (const Node& n : run.graph.nodes) run.source[n.id] = list_source(run.graph, n.id);
}

// The list an output's values belong to: the list block itself (a list output), the list a repeated block runs for,
// or 0 (one value).
int list_of(const RunState& run, int node, const std::string& port) {
    const Node* n = run.graph.find(node);
    const NodeSpec* spec = n ? find_spec(n->type) : nullptr;
    const PortSpec* out = spec ? find_output(*spec, port) : nullptr;
    if (out && out->list) return node;
    const auto it = run.source.find(node);
    return it == run.source.end() ? 0 : it->second;
}

// A list's items that are still in (not skipped after failing), in list order; none while the list isn't known.
std::vector<int> list_items(const RunState& run, int list) {
    std::vector<int> out;
    const auto items = run.items.find(list);
    if (items == run.items.end()) return out;
    const auto skipped = run.skipped.find(list);
    for (int i = 0; i < int(items->second.size()); ++i)
        if (skipped == run.skipped.end() || !skipped->second.contains(i)) out.push_back(i);
    return out;
}

// The items a block runs for: its list's, or just once (-1) if it isn't repeated.
std::vector<int> items_for(const RunState& run, int node) {
    const auto it = run.source.find(node);
    return it == run.source.end() || it->second == 0 ? std::vector<int>{-1} : list_items(run, it->second);
}

// Must this block (for the current item) wait: does an input come from a step waiting for the user? Into a block that
// isn't repeated, any item of a list waiting holds it up (Package never packages part of a list).
bool blocked(const RunState& run, int node) {
    for (const Link& l : run.graph.links) {
        if (l.to_node != node) continue;
        const int list = list_of(run, l.from_node, l.from_port);
        if (!list) {
            if (run.waiting.contains({l.from_node, -1})) return true;
        } else if (run.item >= 0) {
            if (run.waiting.contains({l.from_node, run.item})) return true;
        } else {
            for (const int item : list_items(run, list))
                if (run.waiting.contains({l.from_node, item})) return true;
        }
    }
    return false;
}

// A block's (or, repeated, its current item's) outcome.
void record(NodeRun& r, NodeState state, const std::string& message, const fs::path& file) {
    NodeStatus& status = r.run.result.nodes[r.node.id];
    if (const ListItem* item = r.item())
        status.items.push_back({item->name, item->key, state, message, file});
    else
        status = {state, message, file, {}};
}

// A repeated block's status from its items': waiting if any item waits, else failed if any failed, else not reached
// if any item isn't done yet (an earlier step waits for it), else done; the message counts them (one item: its own
// message).
void sum_up(NodeStatus& status) {
    if (status.items.empty()) {
        status = {NodeState::NotReached, "no items", {}, {}};
        return;
    }
    std::map<NodeState, int> count;
    for (const ItemStatus& it : status.items) ++count[it.state];
    status.state = count[NodeState::Waiting]      ? NodeState::Waiting
                   : count[NodeState::Failed]     ? NodeState::Failed
                   : count[NodeState::NotReached] ? NodeState::NotReached
                                                  : NodeState::Done;
    const auto first = std::ranges::find(status.items, status.state, &ItemStatus::state);
    status.file = first->file;
    if (status.items.size() == 1) {
        status.message = first->message;
        return;
    }
    std::string message;
    for (const auto& [state, words] : {std::pair{NodeState::Done, " done"}, std::pair{NodeState::Waiting, " waiting for you"},
                                       std::pair{NodeState::Failed, " failed"},
                                       std::pair{NodeState::NotReached, " waiting for an earlier step"}})
        if (count[state]) message += (message.empty() ? "" : ", ") + std::to_string(count[state]) + words;
    status.message = std::to_string(status.items.size()) + " items: " + message;
}

// The item a list's previews show: the one its "show" param names, else the first.
int shown_item(const RunState& run, int list) {
    const Node* n = run.graph.find(list);
    const auto items = run.items.find(list);
    const auto show = n ? n->params.find("show") : std::map<std::string, std::string>::const_iterator();
    if (n && show != n->params.end() && items != run.items.end())
        for (size_t i = 0; i < items->second.size(); ++i)
            if (items->second[i].key == show->second) return int(i);
    return 0;
}

// What each output gave, for front ends: a repeated block's shown item stands for the list (its first, where the
// shown one didn't get that far).
RunValues first_values(const RunState& run) {
    RunValues values;
    for (const auto& [key, value] : run.outputs) {
        const auto& [node, port, item] = key;
        const int list = item < 0 ? 0 : list_of(run, node, port);
        if (!list || item == shown_item(run, list)) values[{node, port}] = value.text;
    }
    for (const auto& [key, value] : run.outputs) values.try_emplace({std::get<0>(key), std::get<1>(key)}, value.text);
    return values;
}

}  // namespace

RunResult run_expanded(const Graph& g, const RunOptions& opt);

RunResult run_graph(const Graph& g, const RunOptions& opt) {
    if (!has_customs(g)) return run_expanded(g, opt);
    if (const auto errors = g.validate(); !errors.empty()) {  // before expanding: reports an unknown custom node
        std::string msg = "graph can't run:";
        for (const auto& e : errors) msg += "\n  " + e;
        throw GraphError(msg);
    }
    const ExpandedGraph e = expand_customs(g);
    try {
        RunResult result = run_expanded(e.graph, opt);
        fold_results(e, &result.nodes, &result.values, &result.state, &result.reset_edits);
        return result;
    } catch (RunError& error) {
        fold_results(e, &error.nodes, &error.values, &error.state, nullptr);
        throw;
    }
}

RunResult run_expanded(const Graph& g, const RunOptions& opt) {
    if (const auto errors = g.validate(); !errors.empty()) {
        std::string msg = "graph can't run:";
        for (const auto& e : errors) msg += "\n  " + e;
        throw GraphError(msg);
    }
    if (g.profile != opt.profile.id)
        throw GraphError("graph is for profile '" + g.profile + "' but '" + opt.profile.id + "' was loaded");

    TempDir work;
    RunState run{.graph = g, .options = opt, .work_dir = work.path};
    prepare(run);
    RunResult& result = run.result;
    for (const Node* node : topo_order(g)) {
        const Node& n = *node;
        const int list = run.source[n.id];
        const Node* list_node = list ? g.find(list) : nullptr;
        const bool skip_failed = list_node && list_node->params.contains("on_fail") && list_node->params.at("on_fail") == "skip";
        for (const int item : items_for(run, n.id)) {
            run.item = item;
            NodeRun node_run(run, n);
            if (blocked(run, n.id)) {
                run.waiting.insert({n.id, item});
                record(node_run, NodeState::NotReached, "waits for an earlier step", {});
                continue;
            }
            try {
                find_spec(n.type)->run(node_run);  // validate() checked the type
            } catch (const std::exception& e) {
                if (skip_failed) {
                    run.skipped[list].insert(item);
                    record(node_run, NodeState::Failed, std::string("skipped: ") + e.what(), {});
                    node_run.warn(node_run.item()->name + " skipped: " + e.what());
                    continue;
                }
                record(node_run, NodeState::Failed, e.what(), {});
                if (list) sum_up(result.nodes[n.id]);
                for (const auto& other : g.nodes) result.nodes.try_emplace(other.id);  // the rest: not reached
                const std::string which = node_run.item() ? " (" + node_run.item()->name + ")" : "";
                RunError error(node_label(n) + which + ": " + e.what(), std::move(result.nodes));
                error.values = first_values(run);
                error.state = result.state;
                throw error;
            }
        }
        run.item = -1;
        if (list) sum_up(result.nodes[n.id]);
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
    result.values = first_values(run);
    return std::move(result);
}

std::vector<Value> NodeRun::values(const char* input) const {
    std::vector<Value> v;
    for (size_t i : run.graph.links_into(node.id, input)) {
        const Link& l = run.graph.links[i];
        const int list = list_of(run, l.from_node, l.from_port);
        if (!list) {
            v.push_back(run.outputs.at({l.from_node, l.from_port, -1}));
        } else if (run.item >= 0) {
            v.push_back(run.outputs.at({l.from_node, l.from_port, run.item}));
        } else {  // a block that isn't repeated takes every item
            for (const int item : list_items(run, list)) v.push_back(run.outputs.at({l.from_node, l.from_port, item}));
        }
    }
    return v;
}

std::string NodeRun::text(const char* input) const {
    std::string t;
    if (auto v = values(input); !v.empty()) {
        t = v.front().text;
    } else if (const auto it = node.params.find(input); it != node.params.end()) {
        t = it->second;
    }
    if (const ListItem* it = item())
        for (size_t at; (at = t.find("{name}")) != std::string::npos;) t.replace(at, 6, it->name);
    return t;
}

void NodeRun::output_list(const char* port, std::vector<Value> values, std::vector<ListItem> items) {
    for (size_t i = 0; i < values.size(); ++i) run.outputs[{node.id, port, int(i)}] = std::move(values[i]);
    run.items[node.id] = std::move(items);
}

const ListItem* NodeRun::item() const {
    if (run.item < 0) return nullptr;
    const auto list = run.source.find(node.id);
    return list == run.source.end() ? nullptr : &run.items.at(list->second).at(size_t(run.item));
}

fs::path NodeRun::temp_file(const std::string& suffix) const {
    return run.work_dir / (std::to_string(node.id) + (run.item >= 0 ? "_" + std::to_string(run.item) : "") + suffix);
}

const std::string* NodeRun::state(const std::string& name) const {
    const ListItem* it = item();
    const auto found = node.params.find(it ? name + "@" + it->key : name);
    return found == node.params.end() ? nullptr : &found->second;
}

void NodeRun::set_state(const std::string& name, const std::string& value) {
    const ListItem* it = item();
    run.result.state[node.id][it ? name + "@" + it->key : name] = value;
}

fs::path NodeRun::resolve(const std::string& p) const {
    if (p.empty()) return {};
    const fs::path path(p);
    return (path.is_absolute() ? path : run.options.base_dir / path).lexically_normal();
}

void NodeRun::done(const std::string& message, const fs::path& file) {
    record(*this, NodeState::Done, message, file);
    log(message);
}

void NodeRun::wait(const std::string& message, const fs::path& file) {
    run.waiting.insert({node.id, run.item});
    run.to_edit.push_back(file);
    record(*this, NodeState::Waiting, message, file);
}

void NodeRun::log(const std::string& line) const {
    if (run.options.log) run.options.log(node_label(node) + (item() ? " [" + item()->name + "]" : "") + ": " + line);
}

void NodeRun::warn(const std::string& warning) {
    run.result.warnings.push_back(node_label(node) + ": " + warning);
    if (run.options.log) run.options.log("warning: " + run.result.warnings.back());
}

void NodeRun::claim(const fs::path& file) {
    if (run.item < 0) return;
    const fs::path path = fs::absolute(file).lexically_normal();
    const auto [it, added] = run.writes.try_emplace({node.id, path}, run.item);
    if (!added && it->second != run.item) {
        const auto& items = run.items.at(run.source.at(node.id));
        throw GraphError(items.at(size_t(it->second)).name + " and " + items.at(size_t(run.item)).name +
                         " would both write " + path.string() + ": put {name} in its file name");
    }
}

void NodeRun::change(ChangeKind kind, const fs::path& path) {
    const FileChange c{node.id, kind, fs::absolute(path).lexically_normal()};
    if (kind == ChangeKind::Write) claim(c.path);
    if (run.planned) {
        run.planned->push_back(c);
    } else if (run.options.check_change) {
        if (const std::string why = run.options.check_change(c); !why.empty()) throw GraphError(why);
    }
}

namespace {

// Every block's preview (pure blocks run), as far as known: the outputs, and the blocks that couldn't say.
struct PreviewPass {
    RunValues values;
    std::set<int> unknown;  // a block with a preview (or pure) that threw, or one with neither
    std::map<int, std::vector<ListItem>> lists;  // each list block's items
};

PreviewPass preview_pass(const Graph& g, const fs::path& base_dir, std::vector<FileChange>* planned = nullptr) {
    struct NoConverter : ITextureConverter {  // previews convert nothing
        TexMeta load_tex(const fs::path&, const fs::path&, const Profile&) override { throw GraphError("not in a preview"); }
        TexMeta save_tex(const fs::path&, const fs::path&, const fs::path&, const Profile&) override {
            throw GraphError("not in a preview");
        }
    } converter;
    const Profile profile;
    const RunOptions options{.profile = profile, .converter = converter, .base_dir = base_dir};
    RunState run{.graph = g, .options = options, .work_dir = {}, .planned = planned};
    prepare(run);
    PreviewPass pass;
    for (const Node* n : topo_order(g)) {
        const NodeSpec* spec = find_spec(n->type);
        void (*fn)(NodeRun&) = !spec ? nullptr : spec->pure ? spec->run : spec->preview;
        const std::vector<int> items = items_for(run, n->id);
        if (!fn || items.empty()) {  // no preview, or its list isn't known
            pass.unknown.insert(n->id);
            continue;
        }
        for (const int item : items) {  // a repeated block: every item, so the plan has every item's changes
            run.item = item;
            try {
                NodeRun node_run(run, *n);
                fn(node_run);
            } catch (const std::exception&) {
                // Not known yet: an input isn't (its source can't be previewed), or a value isn't usable.
                pass.unknown.insert(n->id);
            }
        }
        run.item = -1;
    }
    pass.values = first_values(run);
    pass.lists = run.items;
    return pass;
}

// Could this block change files? Steps that do (File family, Package), one with an output field (Export's image
// file), or a destination row in use (an image block's Save to).
bool may_change_files(const Graph& g, const Node& n) {
    const NodeSpec* spec = find_spec(n.type);
    if (!spec || spec->pure) return false;
    if (spec->family == Family::File || spec->family == Family::Output || n.type == "RunProgram") return true;
    if (std::ranges::any_of(spec->outputs, [](const PortSpec& o) { return o.field != nullptr; })) return true;
    return std::ranges::any_of(spec->inputs, [&](const InputSpec& in) {
        if (!in.result) return false;
        const auto it = n.params.find(in.name);
        return (it != n.params.end() && !it->second.empty()) || g.is_connected(n.id, in.name, false);
    });
}

// Inside `dir` (or it), comparing as Windows does: ignoring case, either slash.
bool inside(const fs::path& path, const fs::path& dir) {
    if (dir.empty()) return false;
    auto norm = [](const fs::path& p) {
        std::wstring s = fs::absolute(p).lexically_normal().generic_wstring();
        while (s.size() > 1 && s.back() == L'/') s.pop_back();
        for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
        return s;
    };
    const std::wstring a = norm(path), d = norm(dir);
    return a == d || (a.size() > d.size() && a.starts_with(d) && a[d.size()] == L'/');
}

}  // namespace

RunValues preview_values(const Graph& g, const fs::path& base_dir, std::map<int, std::vector<ListItem>>* lists) {
    if (!has_customs(g)) {
        PreviewPass pass = preview_pass(g, base_dir);
        if (lists) *lists = std::move(pass.lists);
        return pass.values;
    }
    try {
        const ExpandedGraph e = expand_customs(g);
        PreviewPass pass = preview_pass(e.graph, base_dir);
        RunValues values = std::move(pass.values);
        fold_results(e, nullptr, &values, nullptr, nullptr);
        if (lists)  // the graph's own list blocks (one inside a custom node keeps its first)
            for (auto& [id, items] : pass.lists)
                if (g.find(id)) (*lists)[id] = std::move(items);
        return values;
    } catch (const GraphError&) {
        return {};  // an unknown custom node: validate() says so
    }
}

ChangePlan plan_changes(const Graph& graph, const fs::path& base_dir) {
    // Custom blocks: planned as their inner blocks, each change and unknown one put down to the custom block.
    const std::optional<ExpandedGraph> e = has_customs(graph) ? std::optional(expand_customs(graph)) : std::nullopt;
    const Graph& g = e ? e->graph : graph;
    auto origin = [&](int id) { return e && e->origin.contains(id) ? e->origin.at(id).first : id; };
    ChangePlan plan;
    const PreviewPass pass = preview_pass(g, base_dir, &plan.changes);
    for (FileChange& c : plan.changes) c.node = origin(c.node);
    for (const Node& n : g.nodes)
        if (pass.unknown.contains(n.id) && may_change_files(g, n) &&
            std::ranges::find(plan.unknown, origin(n.id)) == plan.unknown.end())
            plan.unknown.push_back(origin(n.id));
    return plan;
}

Guard::Verdict Guard::judge(const FileChange& c, std::string* why) const {
    auto say = [&](std::string text) {
        if (why) *why = std::move(text);
    };
    const auto approved_change = [&] {
        return std::ranges::any_of(approved, [&](const FileChange& a) {
            return a.kind == c.kind && inside(c.path, a.path) && inside(a.path, c.path);
        });
    };
    if (c.kind == ChangeKind::Run) {  // the program isn't changed; what it changes can't be checked
        if (approved_change()) return Verdict::Ok;
        say("runs " + c.path.string() + ", which may change any of your files: needs the user's approval");
        return Verdict::NeedsApproval;
    }
    for (const fs::path& dir : read_only)
        if (inside(c.path, dir)) {
            say(c.path.string() + " is in the game files (" + dir.string() + "), which are never changed");
            return Verdict::Refused;
        }
    if (approved_change()) return Verdict::Ok;
    if (c.kind == ChangeKind::Remove) {
        say("removes " + c.path.string() + ": needs the user's approval");
        return Verdict::NeedsApproval;
    }
    if (!inside(c.path, graph_dir)) {
        say("writes " + c.path.string() + ", outside the graph's folder: needs the user's approval");
        return Verdict::NeedsApproval;
    }
    return Verdict::Ok;
}

std::string Guard::check(const FileChange& c) const {
    std::string why;
    return judge(c, &why) == Verdict::Ok ? std::string() : why;
}

std::string link_value(const Graph& g, const RunValues& preview, const RunValues& last_run, size_t link,
                       bool* from_run) {
    if (from_run) *from_run = false;
    if (link >= g.links.size()) return "";
    const Link& l = g.links[link];
    if (const auto it = preview.find({l.from_node, l.from_port}); it != preview.end() && !it->second.empty())
        return it->second;
    if (const auto it = last_run.find({l.from_node, l.from_port}); it != last_run.end()) {
        if (from_run) *from_run = true;
        return it->second;
    }
    return "";
}

void History::reset(const Graph& graph) {
    past_.clear();
    future_.clear();
    last_ = graph;
}

void History::track(const Graph& now) {
    if (now == last_) return;
    past_.push_back(std::move(last_));
    if (past_.size() > 200) past_.erase(past_.begin());
    future_.clear();  // a new change: what was undone can't be redone any more
    last_ = now;
}

bool History::undo(Graph& graph) {
    if (past_.empty()) return false;
    future_.push_back(std::move(last_));
    last_ = std::move(past_.back());
    past_.pop_back();
    graph = last_;
    return true;
}

bool History::redo(Graph& graph) {
    if (future_.empty()) return false;
    past_.push_back(std::move(last_));
    last_ = std::move(future_.back());
    future_.pop_back();
    graph = last_;
    return true;
}

std::vector<int> step_order(const Graph& g) {
    std::vector<int> ids;
    for (const Node* n : topo_order(g)) ids.push_back(n->id);
    return ids;
}

void apply_run(Graph& g, const RunResult& result) {
    for (int id : result.reset_edits) set_edit_done(g, id, false);
    for (const auto& [id, params] : result.state)
        if (Node* n = g.find(id))
            for (const auto& [key, value] : params)
                if (value.empty())
                    n->params.erase(key);
                else
                    n->params[key] = value;
}

void set_edit_done(Graph& g, int node, bool done, const std::string& item) {
    Node* n = g.find(node);
    if (n && is_custom(n->type)) {  // an Edit image inside: `item` is its key ("<inner id>:done[@<item key>]")
        if (!item.empty() && done)
            n->params[item] = "true";
        else if (!item.empty())
            n->params.erase(item);
        else if (!done)
            std::erase_if(n->params, [](const auto& p) {
                return p.first.ends_with(":done") || p.first.find(":done@") != std::string::npos;
            });
        return;
    }
    if (!n || n->type != "EditImage") return;
    const std::string key = item.empty() ? "done" : "done@" + item;
    if (done)
        n->params[key] = "true";
    else
        n->params.erase(key);
    if (!done && item.empty())  // not done: every item of it either
        std::erase_if(n->params, [](const auto& p) { return p.first.starts_with("done@"); });
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


namespace {

// Top-to-bottom layouts are left-to-right ones with x and y swapped.
std::vector<std::array<float, 2>> swapped(std::vector<std::array<float, 2>> points) {
    for (auto& p : points) std::swap(p[0], p[1]);
    return points;
}
Graph swapped(Graph g) {
    for (Node& n : g.nodes) std::swap(n.x, n.y);
    return g;
}

}  // namespace

bool is_helper(const Graph& g, int node) {
    const Node* n = g.find(node);
    const NodeSpec* spec = n ? find_spec(n->type) : nullptr;
    return spec && spec->utility && std::ranges::none_of(g.links, [&](const Link& l) { return l.to_node == node; });
}

bool from_above(const Graph& g, const Link& link) { return is_helper(g, link.from_node); }

std::vector<std::array<float, 2>> tidy_layout(const Graph& g, const std::vector<std::array<float, 2>>& sizes,
                                              float gap_x, float gap_y, bool downward, float max_width) {
    if (downward) return swapped(tidy_layout(swapped(g), swapped(sizes), gap_x, gap_y, false, max_width));
    const size_t n = g.nodes.size();
    std::vector<std::array<float, 2>> out(n);
    if (n == 0 || sizes.size() != n) return out;
    std::map<int, size_t> index;
    for (size_t i = 0; i < n; ++i) index[g.nodes[i].id] = i;
    std::vector<std::vector<size_t>> feeders(n), targets(n);  // the blocks linking into / out of each one
    for (const Link& l : g.links)
        if (index.contains(l.from_node) && index.contains(l.to_node) && l.from_node != l.to_node) {
            feeders[index[l.to_node]].push_back(index[l.from_node]);
            targets[index[l.from_node]].push_back(index[l.to_node]);
        }
    // A Split is a junction, not a step (user, 2026-10-02): it takes no column of its own but sits in the gap before
    // the column of what it feeds. `step`: how many columns a block moves what it feeds on.
    std::vector<char> split(n, 0);
    for (size_t i = 0; i < n; ++i)
        if (const NodeSpec* spec = find_spec(g.nodes[i].type)) split[i] = spec->family == Family::Flow;
    auto step = [&](size_t i) { return split[i] ? size_t(0) : size_t(1); };

    // The earliest column: past the furthest block feeding it (the longest chain of links into it). At most n rounds,
    // so a cycle can't loop forever.
    std::vector<size_t> earliest(n, 0);
    for (size_t round = 0; round < n; ++round) {
        bool changed = false;
        for (size_t i = 0; i < n; ++i)
            for (const size_t f : feeders[i])
                if (const size_t c = earliest[f] + step(f); c > earliest[i] && c < n) earliest[i] = c, changed = true;
        if (!changed) break;
    }

    // The main chain: the longest one, back from the furthest block (the first, in file order) through the feeder just
    // before it. It's one row.
    std::vector<char> main(n, 0);
    size_t at = size_t(std::ranges::max_element(earliest) - earliest.begin());
    for (size_t hop = 0; hop < n && !main[at]; ++hop) {
        main[at] = 1;
        const auto before =
            std::ranges::find_if(feeders[at], [&](size_t f) { return earliest[f] + step(f) == earliest[at]; });
        if (before == feeders[at].end()) break;
        at = *before;
    }

    // Everything else as late as it can go: just before the first block it feeds (a side branch lines up under where
    // it joins, instead of starting at the far left); what feeds nothing stays just after what feeds it.
    std::vector<size_t> column = earliest;
    for (size_t round = 0; round < n; ++round) {
        bool changed = false;
        for (size_t i = 0; i < n; ++i) {
            if (main[i] || targets[i].empty()) continue;
            size_t late = n;
            for (const size_t t : targets[i]) late = std::min(late, column[t] >= step(i) ? column[t] - step(i) : 0);
            late = std::max(late, earliest[i]);
            if (late != column[i]) column[i] = late, changed = true;
        }
        if (!changed) break;
    }

    // Rows (user, 2026-10-02: "branches stack down"): a helper feeding the main chain (core is_helper: a Value, a Text)
    // goes in a row above it; every other branch (linked blocks off the main chain, a Preview) in a row below.
    std::vector<int> group(n, -1);  // the branch a block off the main chain belongs to (-1: main or a helper above)
    std::vector<char> above(n, 0);
    std::vector<std::pair<size_t, size_t>> group_span;  // each branch's columns
    for (size_t i = 0; i < n; ++i) {
        if (main[i] || group[i] >= 0 || above[i]) continue;
        if (is_helper(g, g.nodes[i].id) && !targets[i].empty() &&
            std::ranges::all_of(targets[i], [&](size_t t) { return bool(main[t]); })) {
            above[i] = 1;
            continue;
        }
        const int id = int(group_span.size());
        std::vector<size_t> todo{i};
        group[i] = id;
        size_t lo = column[i], hi = column[i];
        while (!todo.empty()) {
            const size_t b = todo.back();
            todo.pop_back();
            lo = std::min(lo, column[b]), hi = std::max(hi, column[b]);
            for (const auto* next : {&feeders[b], &targets[b]})
                for (const size_t k : *next)
                    if (!main[k] && group[k] < 0 && !above[k]) group[k] = id, todo.push_back(k);
        }
        group_span.push_back({lo, hi});
    }

    // Wrapping (user, 2026-10-02): once the columns pass `max_width`, the rest continues on a new band of rows
    // underneath, cut where no branch (or helper and what it feeds) spans the cut if there's such a place.
    const size_t columns = *std::ranges::max_element(column) + 1;
    std::vector<float> width(columns, 0);
    for (size_t i = 0; i < n; ++i)
        if (!split[i]) width[column[i]] = std::max(width[column[i]], sizes[i][0]);
    auto cuttable = [&](size_t c) {  // a cut just before column c
        for (const auto& [lo, hi] : group_span)
            if (lo < c && c <= hi) return false;
        for (size_t i = 0; i < n; ++i)
            if (above[i] && column[i] + 1 == c) return false;
        return true;
    };
    std::vector<size_t> band_of(columns, 0), band_start{0};
    float run = 0;
    for (size_t c = 0; c < columns; ++c) {
        const size_t first = band_start.back();
        const float add = width[c] + (c > first ? gap_x : 0);
        if (max_width > 0 && c > first && run + add > max_width) {
            size_t cut = c;
            while (cut > first + 1 && !cuttable(cut)) --cut;
            if (!cuttable(cut)) cut = c;
            band_start.push_back(cut);
            run = 0;
            for (size_t k = cut; k <= c; ++k) run += width[k] + (k > cut ? gap_x : 0);
        } else {
            run += add;
        }
    }
    for (size_t b = 0; b < band_start.size(); ++b)
        for (size_t c = band_start[b]; c < (b + 1 < band_start.size() ? band_start[b + 1] : columns); ++c) band_of[c] = b;

    // Within a band, a branch's part takes the first row below whose columns it doesn't overlap.
    std::vector<int> row(n, 0);  // -1 above the main chain, 0 the main chain, 1... below
    std::map<std::pair<int, size_t>, int> branch_row;               // (branch, band) -> its row below
    std::map<size_t, std::vector<std::vector<std::pair<size_t, size_t>>>> taken;  // band -> per row below: spans
    for (size_t i = 0; i < n; ++i) {
        if (main[i]) continue;
        if (above[i]) {
            row[i] = -1;
            continue;
        }
        const size_t band = band_of[column[i]];
        const auto key = std::make_pair(group[i], band);
        if (!branch_row.contains(key)) {
            size_t lo = columns, hi = 0;  // the branch's columns in this band
            for (size_t k = 0; k < n; ++k)
                if (group[k] == group[i] && band_of[column[k]] == band) lo = std::min(lo, column[k]), hi = std::max(hi, column[k]);
            auto& rows = taken[band];
            size_t r = 0;
            while (r < rows.size() &&
                   std::ranges::any_of(rows[r], [&](const auto& s) { return lo <= s.second && s.first <= hi; }))
                ++r;
            if (r == rows.size()) rows.emplace_back();
            rows[r].push_back({lo, hi});
            branch_row[key] = int(r) + 1;
        }
        row[i] = branch_row[key];
    }

    // Columns left to right from the current top-left corner (each band starting again at the left), each as wide as
    // its widest block; rows as tall as their tallest stack (blocks of one row in one column stack, in file order),
    // `gap_y` apart; bands two gaps apart.
    float left = g.nodes[0].x, top = g.nodes[0].y;
    for (const Node& node : g.nodes) left = std::min(left, node.x), top = std::min(top, node.y);
    std::vector<float> x(columns, left);
    for (size_t c = 1; c < columns; ++c)
        x[c] = band_of[c] == band_of[c - 1] ? x[c - 1] + width[c - 1] + gap_x : left;
    std::map<std::tuple<size_t, int, size_t>, float> stack;  // (band, row, column) -> height so far
    std::map<std::pair<size_t, int>, float> height;          // (band, row) -> its height
    std::vector<float> offset(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (split[i]) continue;
        float& h = stack[{band_of[column[i]], row[i], column[i]}];
        offset[i] = h;
        h += sizes[i][1] + gap_y;
        float& r = height[{band_of[column[i]], row[i]}];
        r = std::max(r, h);
    }
    std::map<std::pair<size_t, int>, float> row_y;  // (band, row) -> its top
    float y = top;
    for (size_t b = 0; b < band_start.size(); ++b) {
        int deepest = 0;
        for (const auto& [key, h] : height)
            if (key.first == b) deepest = std::max(deepest, key.second);
        for (int r = -1; r <= deepest; ++r) {
            row_y[{b, r}] = y;
            if (const auto h = height.find({b, r}); h != height.end()) y += h->second;
        }
        y += gap_y;  // two gaps between bands (each row's height ends with one)
    }
    for (size_t i = 0; i < n; ++i) {
        if (split[i]) continue;
        const size_t b = band_of[column[i]];
        // Above the main chain, a helper's stack sits on the row's bottom, right over what it feeds.
        const float drop = row[i] == -1 ? height[{b, -1}] - stack[{b, -1, column[i]}] : 0;
        out[i] = {x[column[i]], row_y[{b, row[i]}] + offset[i] + drop};
    }

    // Splits, in order along the flow: in the gap before their column, level with the middle of what feeds them (in
    // the same band; else of what they feed); one under another where two would meet.
    std::vector<size_t> splits;
    for (size_t i = 0; i < n; ++i)
        if (split[i]) splits.push_back(i);
    std::ranges::stable_sort(splits, {}, [&](size_t i) { return earliest[i]; });
    std::vector<size_t> placed;
    for (const size_t i : splits) {
        const size_t b = band_of[column[i]];
        const float sx = x[column[i]] - gap_x * 0.5f - sizes[i][0] * 0.5f;
        float sy = row_y[{b, row[i]}];
        const auto f = feeders[i].empty() ? n : feeders[i][0];
        const auto t = targets[i].empty() ? n : targets[i][0];
        const size_t by = f < n && band_of[column[f]] == b ? f : t < n ? t : f;
        if (by < n) sy = out[by][1] + sizes[by][1] * 0.5f - sizes[i][1] * 0.5f;
        for (bool moved = true; moved;) {
            moved = false;
            for (const size_t k : placed)
                if (std::abs(out[k][0] - sx) < sizes[i][0] && sy < out[k][1] + sizes[k][1] + gap_y &&
                    out[k][1] < sy + sizes[i][1] + gap_y)
                    sy = out[k][1] + sizes[k][1] + gap_y, moved = true;
        }
        out[i] = {sx, sy};
        placed.push_back(i);
    }
    return out;
}

std::vector<std::array<float, 2>> keep_apart(const std::vector<std::array<float, 2>>& positions,
                                             const std::vector<std::array<float, 2>>& sizes, size_t moved,
                                             float min_gap) {
    std::vector<std::array<float, 2>> out = positions;
    if (moved >= out.size() || sizes.size() != out.size()) return out;
    std::array<float, 2>& p = out[moved];
    const std::array<float, 2>& s = sizes[moved];
    // Out of each block it's too close to, the shortest way; a few rounds, as one move can bring it near another.
    // ponytail: greedy, may settle a little further than needed in a crowd; fine for hand-built layouts.
    for (size_t round = 0; round < 4 * out.size(); ++round) {
        bool clear = true;
        for (size_t j = 0; j < out.size(); ++j) {
            if (j == moved) continue;
            const float x0 = out[j][0] - min_gap, x1 = out[j][0] + sizes[j][0] + min_gap;
            const float y0 = out[j][1] - min_gap, y1 = out[j][1] + sizes[j][1] + min_gap;
            if (p[0] + s[0] <= x0 || p[0] >= x1 || p[1] + s[1] <= y0 || p[1] >= y1) continue;
            const float right = x1 - p[0], left = p[0] + s[0] - x0, down = y1 - p[1], up = p[1] + s[1] - y0;
            const float least = std::min({right, left, down, up});
            if (least == right) p[0] += right;
            else if (least == left) p[0] -= left;
            else if (least == down) p[1] += down;
            else p[1] -= up;
            clear = false;
        }
        if (clear) break;
    }
    return out;
}

std::vector<std::array<float, 2>> make_room(const Graph& g, const std::vector<std::array<float, 2>>& positions,
                                            const std::vector<std::array<float, 2>>& sizes, int id, float gap_x,
                                            float min_gap, bool downward) {
    if (downward) return swapped(make_room(swapped(g), swapped(positions), swapped(sizes), id, gap_x, min_gap));
    std::vector<std::array<float, 2>> out = positions;
    std::map<int, size_t> index;
    for (size_t i = 0; i < g.nodes.size(); ++i) index[g.nodes[i].id] = i;
    if (!index.contains(id) || out.size() != g.nodes.size() || sizes.size() != out.size()) return out;
    const size_t me = index[id];
    std::vector<size_t> feeders, targets;
    for (const Link& l : g.links) {
        if (l.to_node == id && index.contains(l.from_node)) feeders.push_back(index[l.from_node]);
        if (l.from_node == id && index.contains(l.to_node)) targets.push_back(index[l.to_node]);
    }
    if (!feeders.empty())
        out[me] = {out[feeders[0]][0] + sizes[feeders[0]][0] + gap_x, out[feeders[0]][1]};
    else if (!targets.empty())
        out[me] = {out[targets[0]][0] - gap_x - sizes[me][0], out[targets[0]][1]};
    float need = 0;  // how far the blocks it feeds must move right to stay `gap_x` after it
    for (const size_t t : targets) need = std::max(need, out[me][0] + sizes[me][0] + gap_x - out[t][0]);
    if (need > 0) {
        std::set<size_t> after;
        std::vector<size_t> todo = targets;
        while (!todo.empty()) {
            const size_t i = todo.back();
            todo.pop_back();
            if (i == me || !after.insert(i).second) continue;
            for (const Link& l : g.links)
                if (l.from_node == g.nodes[i].id && index.contains(l.to_node)) todo.push_back(index[l.to_node]);
        }
        for (const size_t i : after) out[i][0] += need;
    }
    return keep_apart(out, sizes, me, min_gap);
}

}  // namespace remod
