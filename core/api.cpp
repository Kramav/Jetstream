#include "api.hpp"

#include "settings.hpp"
#include "setup.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <functional>

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

const char* action(ChangeKind k) {
    return k == ChangeKind::Write ? "write" : k == ChangeKind::Remove ? "remove" : "make folder";
}

const char* state_name(NodeState s) {
    switch (s) {
    case NodeState::Done: return "done";
    case NodeState::Waiting: return "waiting for the user";
    case NodeState::Failed: return "failed";
    case NodeState::NotReached: return "not reached";
    }
    return "?";
}

json statuses(const std::map<int, NodeStatus>& nodes) {
    json out = json::array();
    for (const auto& [id, st] : nodes) {
        json n{{"node", id}, {"state", state_name(st.state)}, {"message", st.message}};
        if (!st.file.empty()) n["file"] = st.file.string();
        for (const ItemStatus& it : st.items) {  // a block repeated for a list: each item
            json item{{"name", it.name}, {"item", it.key}, {"state", state_name(it.state)}, {"message", it.message}};
            if (!it.file.empty()) item["file"] = it.file.string();
            n["items"].push_back(std::move(item));
        }
        out.push_back(std::move(n));
    }
    return out;
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
        } else if (op == "plan" || op == "run") {
            // Guardrails (CLAUDE.md §10 M2): a run only after a plan, and only against that plan.
            if (file_.empty()) throw GraphError("save the graph first: a run's folder and relative paths come from its file");
            // Optional: Noesis converts the textures when given; game_files names the extracted game files when
            // Noesis's RE plugin doesn't (its NativesPath.txt).
            const fs::path noesis = r.contains("noesis") ? path_of(r.at("noesis")) : fs::path();
            const fs::path profiles = find_profiles_dir();
            if (profiles.empty()) throw GraphError("no profiles folder found");
            const Profile profile = load_profile_by_id(profiles, graph_.profile);
            const fs::path base = fs::absolute(file_).parent_path();
            Guard guard{.graph_dir = base};
            if (r.contains("game_files")) guard.read_only.push_back(path_of(r.at("game_files")));
            if (const fs::path game = game_files_dir(noesis, profile); !game.empty()) guard.read_only.push_back(game);
            if (!noesis.empty()) guard.read_only.push_back(noesis.parent_path());  // Noesis and its plugins

            const ChangePlan plan = plan_changes(graph_, base);
            json changes = json::array();
            std::vector<FileChange> to_approve;
            bool refused = false;
            for (const FileChange& c : plan.changes) {
                std::string why;
                const Guard::Verdict v = guard.judge(c, &why);
                json item{{"node", c.node}, {"action", action(c.kind)}, {"path", c.path.string()},
                          {"verdict", v == Guard::Verdict::Ok        ? "ok"
                                      : v == Guard::Verdict::Refused ? "refused"
                                                                     : "needs approval"}};
                if (!why.empty()) item["why"] = why;
                changes.push_back(std::move(item));
                refused |= v == Guard::Verdict::Refused;
                if (v == Guard::Verdict::NeedsApproval) to_approve.push_back(c);
            }
            // Its id is its content: a run names the plan it was shown, and the changes it would make now must match.
            char id[17];
            std::snprintf(id, sizeof id, "%016llx",
                          static_cast<unsigned long long>(std::hash<std::string>{}(changes.dump() + json(plan.unknown).dump())));

            if (op == "plan") {
                reply["plan"] = id;
                reply["changes"] = changes;
                reply["decided_in_run"] = plan.unknown;  // their changes must stay inside the graph's folder
                reply["needs_approval"] = !to_approve.empty();
                reply["refused"] = refused;
            } else {
                if (r.at("plan").get<std::string>() != id)
                    throw GraphError("the changes this run would make differ from that plan: ask for a new plan");
                if (refused) throw GraphError("the plan has refused changes; nothing was run");
                if (!to_approve.empty() && !r.value("approve", false))
                    throw GraphError("the plan has changes that need the user's approval (approve: true once they agree)");
                guard.approved = to_approve;
                const auto converter = make_converter(noesis);
                const RunOptions options{.profile = profile, .converter = *converter, .base_dir = base,
                                         .cache_dir = default_cache_dir(),
                                         .check_change = [&guard](const FileChange& c) { return guard.check(c); }};
                try {
                    const RunResult result = run_graph(graph_, options);
                    apply_run(graph_, result);  // like the app: records what the run found (not saved)
                    reply["message"] = result.message;
                    reply["paused"] = result.paused;  // an Edit image waits: hand control back to the user
                    reply["nodes"] = statuses(result.nodes);
                    reply["warnings"] = result.warnings;
                } catch (const RunError& e) {
                    reply = {{"ok", false}, {"error", e.what()}, {"nodes", statuses(e.nodes)}};
                }
            }
        } else if (op == "edit_done") {
            const Node& n = node_of(graph_, r);
            if (n.type != "EditImage") throw GraphError("block " + std::to_string(n.id) + " isn't an Edit image step");
            set_edit_done(graph_, n.id, r.value("done", true), r.value("item", std::string()));
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
