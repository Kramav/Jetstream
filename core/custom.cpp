#include "custom.hpp"

#include "graph_json.hpp"
#include "node_run.hpp"  // node_label
#include "nodes.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>

namespace remod {

namespace fs = std::filesystem;
using json = nlohmann::json;

bool is_custom(std::string_view type) { return type.starts_with("custom:"); }

// ---- Library ----

fs::path custom_library_dir() {
    char* v = nullptr;
    size_t n = 0;
    _dupenv_s(&v, &n, "APPDATA");
    const std::string dir = v ? v : "";
    std::free(v);
    return dir.empty() ? fs::path() : fs::path(dir) / "remod" / "nodes";
}

std::vector<CustomNode> load_custom_library(const fs::path& dir, std::vector<std::string>* errors) {
    std::vector<CustomNode> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".json") continue;
        try {
            std::ifstream in(e.path());
            out.push_back(custom_from_json(json::parse(in)));
        } catch (const std::exception& x) {
            if (errors) errors->push_back(e.path().string() + ": " + x.what());
        }
    }
    std::ranges::sort(out, {}, &CustomNode::title);
    return out;
}

void save_custom_node(const CustomNode& node, const fs::path& dir) {
    if (dir.empty()) throw GraphError("no folder for custom nodes (APPDATA isn't set)");
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path file = dir / (node.type.substr(std::string_view("custom:").size()) + ".json");
    std::ofstream out(file);
    out << custom_json(node).dump(2) << "\n";
    if (!out.flush()) throw GraphError("failed to write " + file.string());
}

// ---- Registry ----

namespace {

// Registered definitions and their specs. Nothing is ever freed: a pointer handed out (a spec a background preview is
// using) stays valid when a type is registered again. ponytail: a few KB per re-registration; fine for edits by hand.
struct Registry {
    std::mutex mutex;
    std::map<std::string, const CustomNode*, std::less<>> nodes;
    std::map<std::string, const NodeSpec*, std::less<>> specs;
    std::deque<CustomNode> kept_nodes;
    std::deque<NodeSpec> kept_specs;
    std::deque<std::string> text;  // the specs' strings (a deque never moves its elements)
};

Registry& registry() {
    static Registry r;
    return r;
}

struct Pin {
    std::string name, label, initial;
    PortType type = PortType::Any;
};

// An Input or Output block's pin name, by the inner block's id (stable while the block stays).
std::string pin_name(const char* side, int id) { return side + std::to_string(id); }

// Pins top to bottom, as they sit in the custom node's graph.
std::vector<const Node*> by_position(const Graph& g, const char* type) {
    std::vector<const Node*> out;
    for (const Node& n : g.nodes)
        if (n.type == type) out.push_back(&n);
    std::ranges::sort(out, [](const Node* a, const Node* b) { return a->y != b->y ? a->y < b->y : a->x < b->x; });
    return out;
}

std::string param(const Node& n, const char* key) {
    const auto it = n.params.find(key);
    return it == n.params.end() ? std::string() : it->second;
}

bool has_manual_step(const Graph& g) {
    return std::ranges::any_of(g.nodes, [](const Node& n) {
        const NodeSpec* spec = find_spec(n.type);
        return spec && spec->manual;
    });
}

}  // namespace

namespace {
std::set<std::string, std::less<>> g_builtin;  // ponytail: global, set at start; like the registry
}

std::filesystem::path builtin_blocks_dir() {
    const std::filesystem::path profiles = find_profiles_dir();
    return profiles.empty() ? std::filesystem::path() : profiles.parent_path() / "blocks";
}

std::vector<CustomNode> load_block_library(std::vector<std::string>* errors) {
    std::vector<CustomNode> all;
    if (const auto dir = builtin_blocks_dir(); !dir.empty() && std::filesystem::is_directory(dir))
        all = load_custom_library(dir, errors);
    for (const CustomNode& c : all) g_builtin.insert(c.type);
    for (CustomNode& mine : load_custom_library(custom_library_dir(), errors)) {
        const auto same = std::ranges::find(all, mine.type, &CustomNode::type);
        if (same != all.end())
            *same = std::move(mine);
        else
            all.push_back(std::move(mine));
    }
    return all;
}

bool is_builtin_block(std::string_view type) { return g_builtin.contains(type); }

void register_custom(const CustomNode& node) {
    for (const CustomNode& inner : node.graph.customs) register_custom(inner);  // its pins' kinds may depend on them
    // Worked out before taking the lock: the kinds ask find_spec, which takes it too.
    std::vector<Pin> inputs, outputs;
    for (const Node* n : by_position(node.graph, "NodeInput"))
        inputs.push_back({pin_name("in", n->id), param(*n, "name"), param(*n, "default"),
                          node.graph.wanted_type(n->id, "value")});
    for (const Node* n : by_position(node.graph, "NodeOutput")) {
        PortType type = PortType::Any;
        if (const auto in = node.graph.links_into(n->id, "value"); !in.empty())
            type = node.graph.output_type(node.graph.links[in[0]].from_node, node.graph.links[in[0]].from_port);
        outputs.push_back({pin_name("out", n->id), param(*n, "name"), "", type});
    }
    const bool manual = has_manual_step(node.graph);

    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    auto keep = [&](const std::string& s) { return r.text.emplace_back(s).c_str(); };
    const CustomNode& kept = r.kept_nodes.emplace_back(node);
    NodeSpec spec{.type = keep(node.type), .title = keep(node.title), .summary = keep(node.summary)};
    for (const Pin& p : inputs) {
        const bool text = p.type == PortType::Text || p.type == PortType::Any;
        const Widget widget = p.type == PortType::Bool ? Widget::Checkbox : text ? Widget::Text : Widget::Path;
        spec.inputs.push_back({.name = keep(p.name), .label = keep(p.label.empty() ? "input" : p.label),
                               .type = p.type, .widget = widget,
                               .required = p.initial.empty(), .path = picker_for(p.type), .initial = keep(p.initial)});
    }
    for (const Pin& p : outputs)
        spec.outputs.push_back({.name = keep(p.name), .type = p.type, .label = keep(p.label.empty() ? "output" : p.label)});
    spec.manual = manual;
    spec.family = Family::Transform;
    r.nodes[node.type] = &kept;
    r.specs[node.type] = &r.kept_specs.emplace_back(std::move(spec));
}

const CustomNode* find_custom(std::string_view type) {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    const auto it = r.nodes.find(type);
    return it == r.nodes.end() ? nullptr : it->second;
}

const NodeSpec* find_custom_spec(std::string_view type) {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    const auto it = r.specs.find(type);
    return it == r.specs.end() ? nullptr : it->second;
}

std::vector<const NodeSpec*> custom_specs() {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    std::vector<const NodeSpec*> out;
    for (const auto& [_, spec] : r.specs) out.push_back(spec);
    std::ranges::sort(out, [](const NodeSpec* a, const NodeSpec* b) { return std::string_view(a->title) < b->title; });
    return out;
}

// ---- Making and updating ----

namespace {

// Every definition a graph can reach: its own copies, the ones inside them, then the registry.
const CustomNode* definition(const Graph& g, const std::string& type) {
    for (const CustomNode& c : g.customs) {
        if (c.type == type) return &c;
        if (const CustomNode* inner = definition(c.graph, type)) return inner;
    }
    return find_custom(type);
}

}  // namespace


bool uses(const Graph& g, const std::string& type) {
    return std::ranges::any_of(g.nodes, [&](const Node& n) { return n.type == type; });
}

bool has_customs(const Graph& g) {
    return std::ranges::any_of(g.nodes, [](const Node& n) { return is_custom(n.type); });
}

int make_custom_node(Graph& g, const std::vector<int>& selected, const std::string& title, CustomNode* made) {
    std::string name;  // the type's name: the title in lower case, letters, digits and _
    for (const char c : title)
        if (std::isalnum(static_cast<unsigned char>(c)))
            name += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (!name.empty() && name.back() != '_')
            name += '_';
    while (!name.empty() && name.back() == '_') name.pop_back();
    if (name.empty()) throw GraphError("a custom node needs a title with letters or digits");
    const std::set<int> sel(selected.begin(), selected.end());
    if (sel.empty()) throw GraphError("select the blocks to make a custom node of");
    for (const int id : sel)
        if (!g.find(id)) throw GraphError("no block " + std::to_string(id));

    std::string type = "custom:" + name;
    for (int k = 2; find_custom(type) || std::ranges::any_of(g.customs, [&](const CustomNode& c) { return c.type == type; });
         ++k)
        type = "custom:" + name + "_" + std::to_string(k);
    CustomNode c{type, title, "", {}};
    c.graph.profile = g.profile;
    c.graph.downward = g.downward;
    float x = 0, y = 0;
    for (const Node& n : g.nodes)
        if (sel.contains(n.id)) {
            c.graph.nodes.push_back(n);
            x += n.x / float(sel.size());
            y += n.y / float(sel.size());
            const bool known = std::ranges::any_of(c.graph.customs, [&](const CustomNode& d) { return d.type == n.type; });
            if (is_custom(n.type) && !known)  // custom blocks inside: their definitions go along
                if (const CustomNode* def = definition(g, n.type)) c.graph.customs.push_back(*def);
        }

    // Links crossing the selection become pins: an Input block per link coming in, an Output per link going out.
    int next = 1;
    for (const Node& n : g.nodes) next = std::max(next, n.id + 1);
    std::vector<std::pair<int, Link>> ins, outs;  // (the Input / Output block, the link it stands for)
    std::set<std::string> names;
    auto unique = [&](std::string label) {
        std::string candidate = label;
        for (int k = 2; !names.insert(candidate).second; ++k) candidate = label + " " + std::to_string(k);
        return candidate;
    };
    for (const Link& l : g.links) {
        const bool from = sel.contains(l.from_node), to = sel.contains(l.to_node);
        if (from && to) {
            c.graph.links.push_back(l);
        } else if (to) {
            const Node& target = *g.find(l.to_node);
            const NodeSpec* spec = find_spec(target.type);
            const InputSpec* in = spec ? find_input(*spec, l.to_port) : nullptr;
            Node pin{.id = next++, .type = "NodeInput", .x = target.x - 200, .y = target.y};
            pin.params["name"] = unique(in ? in->label : l.to_port);
            c.graph.links.push_back({pin.id, "value", l.to_node, l.to_port});
            ins.emplace_back(pin.id, l);
            c.graph.nodes.push_back(std::move(pin));
        } else if (from) {
            const Node& source = *g.find(l.from_node);
            const NodeSpec* spec = find_spec(source.type);
            std::string label = l.from_port;
            if (spec)
                for (const PortSpec& p : spec->outputs)
                    if (l.from_port == p.name) label = p.label;
            Node pin{.id = next++, .type = "NodeOutput", .x = source.x + 300, .y = source.y};
            pin.params["name"] = unique(label);
            c.graph.links.push_back({l.from_node, l.from_port, pin.id, "value"});
            outs.emplace_back(pin.id, l);
            c.graph.nodes.push_back(std::move(pin));
        }
    }
    register_custom(c);

    for (const int id : sel) g.remove_node(id);  // and their links
    Node& block = g.add_node(type);  // copies the definition into the graph
    block.x = x;
    block.y = y;
    const int id = block.id;
    for (const auto& [pin, l] : ins) g.links.push_back({l.from_node, l.from_port, id, pin_name("in", pin)});
    for (const auto& [pin, l] : outs) g.links.push_back({id, pin_name("out", pin), l.to_node, l.to_port});
    if (made) *made = c;
    return id;
}

void update_custom(Graph& g, const CustomNode& node) {
    const auto it = std::ranges::find(g.customs, node.type, &CustomNode::type);
    if (it == g.customs.end())
        g.customs.push_back(node);
    else
        *it = node;
    register_custom(node);
}

std::vector<std::string> library_differs(const Graph& g, const std::vector<CustomNode>& library) {
    std::vector<std::string> out;
    for (const CustomNode& c : g.customs) {
        const auto it = std::ranges::find(library, c.type, &CustomNode::type);
        if (it != library.end() && !(*it == c)) out.push_back(c.type);
    }
    return out;
}

// ---- Expansion ----

ExpandedGraph expand_customs(const Graph& g) {
    ExpandedGraph e{g, {}, {}};
    for (const Node& n : g.nodes) e.origin[n.id] = {n.id, ""};
    int next = 1;
    for (const Node& n : g.nodes) next = std::max(next, n.id + 1);
    for (int expansions = 0;; ++expansions) {
        const auto found = std::ranges::find_if(e.graph.nodes, [](const Node& n) { return is_custom(n.type); });
        if (found == e.graph.nodes.end()) break;
        const Node block = *found;
        if (expansions > 10000) throw GraphError(node_label(block) + ": a custom node that contains itself");
        const CustomNode* def = definition(g, block.type);
        if (!def)
            throw GraphError(node_label(block) + ": custom node " + block.type +
                             " isn't in this graph or your library");
        const auto [origin, prefix] = e.origin.at(block.id);
        const Graph& inner = def->graph;

        std::map<int, int> ids;  // inner block -> its expanded copy
        for (const Node& m : inner.nodes) {
            if (m.type == "NodeInput" || m.type == "NodeOutput") continue;
            Node copy = m;
            copy.id = next++;
            const std::string mine = std::to_string(m.id) + ":";  // its state, kept on the custom block
            for (const auto& [key, value] : block.params)
                if (key.starts_with(mine)) copy.params[key.substr(mine.size())] = value;
            copy.params["title"] = block_title(m) + " (in " + block_title(block) + ")";
            ids[m.id] = copy.id;
            e.origin[copy.id] = {origin, prefix + mine};
            e.graph.nodes.push_back(std::move(copy));
        }

        // What each Input gives: the link into its pin, else its typed value (or default) as a Value block.
        std::map<int, std::pair<int, std::string>> given;
        for (const Node& m : inner.nodes) {
            if (m.type != "NodeInput") continue;
            const std::string pin = pin_name("in", m.id);
            const auto into = std::ranges::find_if(e.graph.links, [&](const Link& l) {
                return l.to_node == block.id && l.to_port == pin;
            });
            if (into != e.graph.links.end()) {
                given[m.id] = {into->from_node, into->from_port};
                continue;
            }
            std::string value = param(block, pin.c_str());
            if (value.empty()) value = param(m, "default");
            if (value.empty()) continue;  // nothing: what it feeds reports the missing input
            Node v{.id = next++, .type = "Value"};
            v.params["value"] = value;
            v.params["title"] = param(m, "name") + " (in " + block_title(block) + ")";
            e.origin[v.id] = {origin, prefix + pin + ":"};
            given[m.id] = {v.id, "value"};
            e.graph.nodes.push_back(std::move(v));
        }
        auto source = [&](const Link& l) -> std::optional<std::pair<int, std::string>> {
            const Node* from = inner.find(l.from_node);
            if (from && from->type == "NodeInput") {
                const auto it = given.find(from->id);
                return it == given.end() ? std::nullopt : std::optional(it->second);
            }
            return std::pair{ids.at(l.from_node), l.from_port};
        };

        std::vector<Link> links;
        for (const Link& l : inner.links) {
            const Node* to = inner.find(l.to_node);
            if (!to || to->type == "NodeOutput") continue;
            if (const auto from = source(l)) links.push_back({from->first, from->second, ids.at(l.to_node), l.to_port});
        }
        for (const Node& m : inner.nodes) {  // what each Output receives goes on along the block's links
            if (m.type != "NodeOutput") continue;
            const std::string pin = pin_name("out", m.id);
            const auto into = inner.links_into(m.id, "value");
            const auto from = into.empty() ? std::nullopt : source(inner.links[into[0]]);
            if (!from) continue;
            for (const Link& l : e.graph.links)
                if (l.from_node == block.id && l.from_port == pin) links.push_back({from->first, from->second, l.to_node, l.to_port});
            for (auto& [_, target] : e.outputs)  // an outer custom block's output fed by this one
                if (target == std::pair{block.id, pin}) target = *from;
            if (prefix.empty()) e.outputs[{block.id, pin}] = *from;
        }
        e.graph.remove_node(block.id);
        e.origin.erase(block.id);
        e.graph.links.insert(e.graph.links.end(), links.begin(), links.end());
    }
    return e;
}

void fold_results(const ExpandedGraph& e, std::map<int, NodeStatus>* nodes, RunValues* values,
                  std::map<int, std::map<std::string, std::string>>* state, std::vector<int>* reset_edits) {
    auto origin = [&](int id) {
        const auto it = e.origin.find(id);
        return it == e.origin.end() ? std::pair{id, std::string()} : it->second;
    };
    if (nodes) {
        std::map<int, NodeStatus> out;
        std::map<int, std::vector<std::pair<int, const NodeStatus*>>> inside;  // custom block -> its inner blocks'
        for (const auto& [id, st] : *nodes) {
            const auto [o, prefix] = origin(id);
            if (prefix.empty())
                out[o] = st;
            else
                inside[o].emplace_back(id, &st);
        }
        for (const auto& [o, list] : inside) {
            NodeStatus sum;
            auto count = [&](NodeState s) {
                return std::ranges::count_if(list, [&](const auto& p) { return p.second->state == s; });
            };
            sum.state = count(NodeState::Waiting)      ? NodeState::Waiting
                        : count(NodeState::Failed)     ? NodeState::Failed
                        : count(NodeState::NotReached) ? NodeState::NotReached
                        : count(NodeState::Done)       ? NodeState::Done
                                                       : NodeState::NotNeeded;
            for (const auto& [id, st] : list) {
                const Node* n = e.graph.find(id);
                const std::string title = n ? block_title(*n) : std::to_string(id);
                if (st->state == sum.state && sum.message.empty()) {
                    sum.message = title + ": " + st->message;
                    sum.file = st->file;
                }
                const NodeSpec* spec = n ? find_spec(n->type) : nullptr;
                if (!spec || !spec->manual) continue;  // the steps for the user, one by one (and each item of one)
                const std::string key = origin(id).second + "done";
                if (st->items.empty())
                    sum.items.push_back({title, key, st->state, st->message, st->file});
                for (const ItemStatus& it : st->items)
                    sum.items.push_back({title + ": " + it.name, key + "@" + it.key, it.state, it.message, it.file});
            }
            if (sum.state == NodeState::Done) sum.message = std::to_string(list.size()) + " inner steps done";
            out[o] = std::move(sum);
        }
        *nodes = std::move(out);
    }
    if (values) {
        RunValues out;
        for (const auto& [key, value] : *values)
            if (origin(key.first).second.empty()) out[key] = value;
        for (const auto& [pin, from] : e.outputs)
            if (const auto it = values->find(from); it != values->end()) out[pin] = it->second;
        *values = std::move(out);
    }
    if (state) {
        std::map<int, std::map<std::string, std::string>> out;
        for (const auto& [id, params] : *state) {
            const auto [o, prefix] = origin(id);
            for (const auto& [key, value] : params) out[o][prefix + key] = value;
        }
        if (reset_edits) {
            std::vector<int> kept;
            for (const int id : *reset_edits) {
                const auto [o, prefix] = origin(id);
                if (prefix.empty())
                    kept.push_back(id);
                else
                    out[o][prefix + "done"] = "";  // apply_run removes it
            }
            *reset_edits = std::move(kept);
        }
        *state = std::move(out);
    }
}

}  // namespace remod
