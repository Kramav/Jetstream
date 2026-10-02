#include "api.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace remod {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

const char* kind(PortType t) {
    switch (t) {
    case PortType::Tex: return "texture";
    case PortType::Image: return "image";
    case PortType::Text: return "text";
    case PortType::Path: return "path";
    case PortType::Folder: return "folder";
    case PortType::Any: return "any";
    }
    return "?";
}

const char* family(Family f) {
    constexpr const char* names[] = {"source", "transform", "manual", "flow", "file", "output", "value"};
    return names[static_cast<int>(f)];
}

json types() {
    json all = json::array();
    for (const NodeSpec& s : node_specs()) {
        json inputs = json::array(), outputs = json::array();
        for (const InputSpec& in : s.inputs) {
            json i{{"name", in.name}, {"label", in.label}, {"kind", kind(in.type)}, {"required", in.required},
                   {"multiple", in.multiple}, {"typed", in.editable()}};
            if (*in.hint) i["hint"] = in.hint;
            if (in.editable()) i["initial"] = in.initial;
            if (in.filter) i["extensions"] = in.filter;
            if (!in.options.empty()) {
                i["choices"] = json::array();
                for (const auto& [value, _] : in.options) i["choices"].push_back(value);
            }
            if (in.widget == Widget::Number) i["range"] = {in.min, in.max};
            if (in.result) i["writes"] = in.result;  // where the block writes the output of that name
            inputs.push_back(std::move(i));
        }
        for (const PortSpec& out : s.outputs) {
            json o{{"name", out.name}, {"label", out.label}, {"kind", kind(out.type)}, {"multiple", out.multiple}};
            if (out.field) o["field"] = out.field;  // typed with `set`, under this name
            outputs.push_back(std::move(o));
        }
        all.push_back({{"type", s.type}, {"title", s.title}, {"summary", s.summary}, {"family", family(s.family)},
                       {"manual", s.manual}, {"inputs", inputs}, {"outputs", outputs}});
    }
    return all;
}

// JSON text is UTF-8; a narrow std::string path would be read in the ANSI codepage.
fs::path path_of(const json& value) {
    const std::string s = value.get<std::string>();
    return fs::path(std::u8string(s.begin(), s.end()));
}

Link link_of(const json& r) {
    return {r.at("from").get<int>(), r.at("from_port").get<std::string>(), r.at("to").get<int>(),
            r.at("to_port").get<std::string>()};
}

Node& node_of(Graph& g, const json& r) {
    Node* n = g.find(r.at("id").get<int>());
    if (!n) throw GraphError("no block " + std::to_string(r.at("id").get<int>()));
    return *n;
}

}  // namespace

std::string ApiSession::call(const std::string& request) {
    json reply{{"ok", true}};
    try {
        const json r = json::parse(request);
        const std::string op = r.at("op").get<std::string>();
        if (op == "types") {
            reply["types"] = types();
        } else if (op == "new") {
            graph_ = Graph{};
            graph_.profile = r.value("profile", graph_.profile);
            file_.clear();
        } else if (op == "open") {
            const fs::path file = path_of(r.at("file"));
            graph_ = load_graph(file);
            file_ = file;
        } else if (op == "save") {
            const fs::path file = r.contains("file") ? path_of(r["file"]) : file_;
            if (file.empty()) throw GraphError("a new graph needs a file to save to");
            save_graph(graph_, file);
            file_ = file;
        } else if (op == "graph") {
            json nodes = json::array(), links = json::array();
            for (const Node& n : graph_.nodes)
                nodes.push_back({{"id", n.id}, {"type", n.type}, {"title", block_title(n)}, {"params", n.params}});
            for (const Link& l : graph_.links)
                links.push_back({{"from", l.from_node}, {"from_port", l.from_port}, {"to", l.to_node}, {"to_port", l.to_port}});
            reply["profile"] = graph_.profile;
            reply["nodes"] = nodes;
            reply["links"] = links;
        } else if (op == "add") {
            // ponytail: placed in a row to the right; the app's Tidy up arranges it properly.
            float right = -400;
            for (const Node& n : graph_.nodes) right = std::max(right, n.x);
            Node& n = graph_.add_node(r.at("type").get<std::string>());
            n.x = right + 400;
            reply["id"] = n.id;
        } else if (op == "remove") {
            graph_.remove_node(node_of(graph_, r).id);
        } else if (op == "set") {
            Node& n = node_of(graph_, r);
            const std::string input = r.at("input").get<std::string>();
            const NodeSpec& spec = *find_spec(n.type);
            const InputSpec* in = find_input(spec, input);
            const bool field = std::ranges::any_of(spec.outputs, [&](const PortSpec& o) { return o.field && input == o.field; });
            if (!field && !(in && in->editable()))
                throw GraphError(std::string(spec.title) + " has no typed field '" + input + "'");
            n.params[input] = r.at("value").get<std::string>();
        } else if (op == "link") {
            if (const std::string why = graph_.connect(link_of(r)); !why.empty()) throw GraphError(why);
        } else if (op == "unlink") {
            const auto it = std::ranges::find(graph_.links, link_of(r));
            if (it == graph_.links.end()) throw GraphError("no such link");
            graph_.disconnect(size_t(it - graph_.links.begin()));
        } else if (op == "next") {
            const Node& n = node_of(graph_, r);
            json choices = json::array();
            for (const auto& c : graph_.choices_for_pin(n.id, r.at("port").get<std::string>(), r.at("output").get<bool>()))
                choices.push_back({{"type", c.spec->type}, {"port", c.port}});
            reply["choices"] = choices;
        } else if (op == "validate") {
            reply["problems"] = graph_.validate();
        } else if (op == "preview") {
            const fs::path base = file_.empty() ? fs::current_path() : fs::absolute(file_).parent_path();
            json values = json::array();
            for (const auto& [key, value] : preview_values(graph_, base))
                values.push_back({{"node", key.first}, {"output", key.second}, {"value", value}});
            reply["values"] = values;
        } else {
            throw GraphError("unknown op '" + op + "'");
        }
    } catch (const std::exception& e) {
        reply = {{"ok", false}, {"error", e.what()}};
    }
    // ponytail: core's messages and paths are narrow (ANSI) strings; a non-UTF-8 byte becomes U+FFFD, not a throw.
    return reply.dump(-1, ' ', false, json::error_handler_t::replace);
}

}  // namespace remod
