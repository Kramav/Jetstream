#include "api.hpp"

#include "game_code.hpp"

#include "image.hpp"
#include "settings.hpp"
#include "setup.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <random>

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
    case PortType::Bool: return "condition";
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
    for (const NodeSpec* spec : all_specs()) {  // built in, then custom nodes
        const NodeSpec& s = *spec;
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
        all.push_back({{"type", s.type}, {"title", s.title}, {"summary", s.summary}, {"ai", ai_note(s.type)},
                       {"family", family(s.family)}, {"manual", s.manual}, {"inputs", inputs}, {"outputs", outputs}});
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
    return k == ChangeKind::Write    ? "write"
           : k == ChangeKind::Remove ? "remove"
           : k == ChangeKind::Run    ? "run program"
                                     : "make folder";
}

const char* state_name(NodeState s) {
    switch (s) {
    case NodeState::Done: return "done";
    case NodeState::Waiting: return "waiting for the user";
    case NodeState::Failed: return "failed";
    case NodeState::NotReached: return "not reached";
    case NodeState::NotNeeded: return "not needed";
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

// A file's picture at most `size` pixels on a side, with its scale (preview_image's loader): a texture by its own mip
// (decode_tex), an image file through WIC or our TGA reader.
std::optional<ImagePreview> shrunk_image(const fs::path& file, unsigned size) {
    try {
        unsigned full_w = 0, full_h = 0;
        Bgra img;
        if (read_tex_version(file)) {
            img = decode_tex(file, size, &full_w, &full_h);
        } else {
            img = load_image(file);
            full_w = img.width, full_h = img.height;
        }
        if (const unsigned side = std::max(img.width, img.height); side > size)
            img = resize_image(img, std::max(1u, img.width * size / side), std::max(1u, img.height * size / side),
                               Fit::Stretch);
        return ImagePreview{img, full_w ? float(img.width) / float(full_w) : 1.0f};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// A picture as base64 PNG text (JSON carries no bytes).
std::string png_base64(const Bgra& image) {
    const fs::path file = fs::temp_directory_path() / ("remod_api_" + std::to_string(std::random_device{}()) + ".png");
    save_png(file, image);
    std::ifstream in(file, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), {});
    in.close();
    std::error_code ec;
    fs::remove(file, ec);
    static constexpr char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned n = std::uint8_t(bytes[i]) << 16 | (i + 1 < bytes.size() ? std::uint8_t(bytes[i + 1]) << 8 : 0) |
                           (i + 2 < bytes.size() ? std::uint8_t(bytes[i + 2]) : 0);
        out += digits[n >> 18 & 63];
        out += digits[n >> 12 & 63];
        out += i + 1 < bytes.size() ? digits[n >> 6 & 63] : '=';
        out += i + 2 < bytes.size() ? digits[n & 63] : '=';
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
            // A list block's "show": which of its files previews and images show (an item key from `preview`'s lists).
            const bool shown = input == "show" && std::ranges::any_of(spec.outputs, [](const PortSpec& o) { return o.list; });
            if (!field && !shown && !(in && in->editable()))
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
        } else if (op == "image") {
            // What a block's picture looks like now, worked out as the app's thumbnails are (CLAUDE.md §10 M4: results
            // an AI can see): an image block's result, a texture, a Preview block's input.
            const Node& n = node_of(graph_, r);
            const unsigned size = std::clamp(r.value("size", 512u), 16u, 1024u);
            const fs::path base = file_.empty() ? fs::current_path() : fs::absolute(file_).parent_path();
            std::optional<Profile> profile;
            if (const fs::path profiles = find_profiles_dir(); !profiles.empty()) try {
                    profile = load_profile_by_id(profiles, graph_.profile);
                } catch (const std::exception&) {  // Resize's "Match size of" then can't read a texture's size
                }
            std::string why;
            const auto made = preview_image(graph_, preview_values(graph_, base), n.id, base, size,
                                            [&](const fs::path& p) { return shrunk_image(p, size); },
                                            profile ? &*profile : nullptr, &why);
            if (!made) throw GraphError(why.empty() ? "no picture for that block yet (nothing it shows is known before a run?)"
                                                    : why);
            reply["png"] = png_base64(made->image);
            reply["width"] = made->image.width;
            reply["height"] = made->image.height;
        } else if (op == "plan" || op == "run") {
            // Guardrails (CLAUDE.md §10 M4): a run only after a plan, and only against that plan.
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
            if (const NodeSpec* spec = find_spec(n.type); !spec || !spec->manual)
                throw GraphError("block " + std::to_string(n.id) + " isn't a manual step (Edit image, Edit video)");
            set_edit_done(graph_, n.id, r.value("done", true), r.value("item", std::string()));
        } else if (op == "game_code") {
            // The game's types, fields and methods, from REFramework's SDK dump (CLAUDE.md §10 M2): real names for a
            // script.
            const GameCode* code = game_code();
            if (!code)
                throw GraphError("no SDK dump set: the user picks REFramework's il2cpp_dump.json in remod's Pipeline "
                                 "panel (made in game: REFramework menu > DeveloperTools > ObjectExplorer > Dump SDK)");
            json hits = json::array();
            for (const CodeHit& h : search_game_code(*code, r.at("query").get<std::string>(), r.value("limit", 60u)))
                hits.push_back({{"type", h.type}, {"member", h.member}, {"detail", h.detail}});
            reply["hits"] = hits;
        } else if (op == "check_script") {
            // A Lua script's problems: syntax, and game names that aren't in the game (when an SDK dump is set).
            std::string source, name = r.value("name", std::string("script.lua"));
            if (r.contains("file")) {
                fs::path file = path_of(r.at("file"));
                if (file.is_relative() && !file_.empty()) file = fs::absolute(file_).parent_path() / file;
                std::ifstream in(file, std::ios::binary);
                if (!in) throw GraphError("can't read " + file.string());
                source.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                name = file.filename().string();
            } else if (r.contains("text")) {
                source = r.at("text").get<std::string>();
            } else {
                throw GraphError("check_script needs a file or a text");
            }
            const GameCode* code = nullptr;
            try {
                code = game_code();
                if (!code) reply["names_unchecked"] = "no SDK dump set (the user picks it in the Pipeline panel)";
            } catch (const std::exception& e) {
                reply["names_unchecked"] = e.what();
            }
            json problems = json::array();
            for (const ScriptProblem& p : check_lua(source, name, code))
                problems.push_back({{"line", p.line}, {"message", p.message}});
            reply["problems"] = problems;
        } else if (op == "validate") {
            reply["problems"] = graph_.validate();
        } else if (op == "preview") {
            const fs::path base = file_.empty() ? fs::current_path() : fs::absolute(file_).parent_path();
            json values = json::array();
            std::map<int, std::vector<ListItem>> lists;
            for (const auto& [key, value] : preview_values(graph_, base, &lists))
                values.push_back({{"node", key.first}, {"output", key.second}, {"value", value}});
            reply["values"] = values;
            for (const auto& [id, items] : lists)  // a list block's files: `set` its "show" to an item to see that one
                for (const ListItem& it : items) reply["lists"][std::to_string(id)].push_back({{"name", it.name}, {"item", it.key}});
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
