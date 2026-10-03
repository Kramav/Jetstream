#include "mcp.hpp"

#include <nlohmann/json.hpp>

#include <vector>

namespace remod {

namespace {

using nlohmann::json;

constexpr const char* kInstructions =
    "remod builds RE Engine game mods (Resident Evil 4 remake textures) as node graphs. Work like this: "
    "list_blocks first (each type's 'ai' note says when to use it); new_graph or open_graph; add_block, set_field, "
    "link; validate until it reports no problems; preview and view_image to check results before running; "
    "save_graph (a run needs the graph saved: its folder is where it may write freely); run. "
    "A run asks the user, in a dialog of its own, to allow removing files, writing outside the graph's folder and "
    "starting programs; you cannot approve anything. A run that pauses at an Edit image step waits for the user: "
    "tell them which file to edit and call edit_done only after they say they have finished. Game files are never "
    "changed. Texture paths: files named .tex.143221013 under an extracted natives/STM folder.";

struct Tool {
    const char* name;
    const char* op;  // the ApiSession op it calls ("" = handled here)
    const char* description;
    json schema;
};

json object(json properties = json::object(), std::vector<std::string> required = {}) {
    json s{{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty()) s["required"] = required;
    return s;
}
json number(const char* about) { return {{"type", "integer"}, {"description", about}}; }
json text(const char* about) { return {{"type", "string"}, {"description", about}}; }

const std::vector<Tool>& tools() {
    static const std::vector<Tool> all{
        {"list_blocks", "types", "Every block type: inputs, outputs, fields, and an 'ai' note on when to use it.", object()},
        {"new_graph", "new", "Start an empty graph.", object({{"profile", text("Game profile, default re4r.")}})},
        {"open_graph", "open", "Open a graph file.", object({{"file", text("The .json graph file.")}}, {"file"})},
        {"save_graph", "save", "Save the graph (to the file opened or saved last, or to `file`).",
         object({{"file", text("Where to save it, a .json file.")}})},
        {"show_graph", "graph", "The graph's blocks (id, type, title, typed values) and links.", object()},
        {"add_block", "add", "Add a block; returns its id.", object({{"type", text("A type from list_blocks.")}}, {"type"})},
        {"remove_block", "remove", "Remove a block and its links.", object({{"id", number("The block.")}}, {"id"})},
        {"set_field", "set",
         "Type a value into a block's field (an input that can be typed, or an output's field such as Export "
         "image's 'png'; a Files in folder block's 'show' picks which file previews show).",
         object({{"id", number("The block.")}, {"input", text("The field's name.")}, {"value", text("The value.")}},
                {"id", "input", "value"})},
        {"link", "link", "Link an output to an input; refused with the reason if the rules don't allow it.",
         object({{"from", number("Source block.")}, {"from_port", text("Its output.")}, {"to", number("Target block.")},
                 {"to_port", text("Its input.")}},
                {"from", "from_port", "to", "to_port"})},
        {"unlink", "unlink", "Remove a link.",
         object({{"from", number("Source block.")}, {"from_port", text("Its output.")}, {"to", number("Target block.")},
                 {"to_port", text("Its input.")}},
                {"from", "from_port", "to", "to_port"})},
        {"next_blocks", "next", "Block types that could attach to a pin, and through which port.",
         object({{"id", number("The block.")}, {"port", text("The pin.")},
                 {"output", {{"type", "boolean"}, {"description", "True for an output pin."}}}},
                {"id", "port", "output"})},
        {"validate", "validate", "Every problem that would stop a run (empty: it can run).", object()},
        {"preview", "preview",
         "Every output's value as known without running (nothing is touched), and each list's files.", object()},
        {"view_image", "image",
         "Look at a block's picture as it is now: an image block's result, a texture, a Preview block's input.",
         object({{"id", number("The block.")}, {"size", number("Longest side in pixels, 16 to 1024 (default 512).")}},
                {"id"})},
        {"plan_run", "plan",
         "The file changes a run would make, each marked ok, needs approval (asked of the user when you run) or "
         "refused. Nothing is touched.",
         object()},
        {"run", "",
         "Run the graph. The user is asked in a dialog to allow any change that needs approval; you can't approve. "
         "Returns where every block got to; 'paused' means an Edit image step waits for the user.",
         object()},
        {"edit_done", "edit_done",
         "Mark an Edit image step done ONLY after the user says they finished editing that image.",
         object({{"id", number("The Edit image block.")},
                 {"item", text("For a block repeated for a list: the item (its 'item' in run's nodes).")},
                 {"done", {{"type", "boolean"}, {"description", "False to mark it not done."}}}},
                {"id"})},
    };
    return all;
}

// A tool's result: the API's reply as text (and a picture, for view_image).
json result(const std::string& reply, bool image = false) {
    const json r = json::parse(reply);
    const bool failed = !r.value("ok", false);
    json content = json::array();
    if (image && !failed) {
        content.push_back({{"type", "image"}, {"data", r.at("png")}, {"mimeType", "image/png"}});
        content.push_back({{"type", "text"},
                           {"text", std::to_string(r.at("width").get<int>()) + "x" +
                                        std::to_string(r.at("height").get<int>()) + " preview"}});
    } else {
        content.push_back({{"type", "text"}, {"text", reply}});
    }
    return {{"content", content}, {"isError", failed}};
}

}  // namespace

std::string McpServer::handle(const std::string& message) {
    json msg;
    try {
        msg = json::parse(message);
    } catch (const json::exception&) {
        return json{{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", "parse error"}}}}.dump();
    }
    if (!msg.contains("id")) return "";  // a notification (initialized, cancelled): nothing to answer
    json reply{{"jsonrpc", "2.0"}, {"id", msg["id"]}};
    const std::string method = msg.value("method", "");
    const json params = msg.value("params", json::object());
    auto with_options = [&](json request) {  // the user's settings, never the AI's
        if (!options_.noesis.empty()) request["noesis"] = options_.noesis.string();
        if (!options_.game_files.empty()) request["game_files"] = options_.game_files.string();
        return request;
    };
    try {
        if (method == "initialize") {
            reply["result"] = {{"protocolVersion", params.value("protocolVersion", "2025-06-18")},
                               {"capabilities", {{"tools", {{"listChanged", false}}}}},
                               {"serverInfo", {{"name", "remod"}, {"version", "0.1"}}},
                               {"instructions", kInstructions}};
        } else if (method == "ping") {
            reply["result"] = json::object();
        } else if (method == "tools/list") {
            json list = json::array();
            for (const Tool& t : tools())
                list.push_back({{"name", t.name}, {"description", t.description}, {"inputSchema", t.schema}});
            reply["result"] = {{"tools", list}};
        } else if (method == "tools/call") {
            const std::string name = params.at("name").get<std::string>();
            json args = params.value("arguments", json::object());
            args.erase("approve");  // approval is the user's (the dialog), whatever a caller sends
            const auto tool = std::ranges::find_if(tools(), [&](const Tool& t) { return name == t.name; });
            if (tool == tools().end()) {
                reply["result"] = result(json{{"ok", false}, {"error", "unknown tool " + name}}.dump());
            } else if (name == "run") {
                const std::string planned = api_.call(with_options({{"op", "plan"}}).dump());
                const json plan = json::parse(planned);
                if (!plan.value("ok", false) || plan.value("refused", false)) {
                    reply["result"] = result(plan.value("ok", false)
                                                 ? json{{"ok", false}, {"error", "the plan has refused changes; nothing "
                                                                                 "was run"}, {"changes", plan["changes"]}}.dump()
                                                 : planned);
                } else {
                    bool allowed = false;
                    if (plan.value("needs_approval", false)) {
                        std::string changes;
                        for (const json& c : plan["changes"])
                            if (c.value("verdict", "") == "needs approval")
                                changes += c.value("action", "") + " " + c.value("path", "") + "\n";
                        allowed = options_.approve && options_.approve(changes);
                        if (!allowed) {
                            reply["result"] = result(json{{"ok", false},
                                                          {"error", "the user declined the changes; nothing was run"}}
                                                         .dump());
                            return reply.dump();
                        }
                    }
                    reply["result"] = result(
                        api_.call(with_options({{"op", "run"}, {"plan", plan["plan"]}, {"approve", allowed}}).dump()));
                }
            } else {
                args["op"] = tool->op;
                if (std::string_view(tool->op) == "plan") args = with_options(args);
                reply["result"] = result(api_.call(args.dump()), std::string_view(tool->op) == "image");
            }
        } else {
            reply["error"] = {{"code", -32601}, {"message", "unknown method " + method}};
        }
    } catch (const std::exception& e) {
        reply.erase("result");
        reply["error"] = {{"code", -32602}, {"message", e.what()}};
    }
    return reply.dump();
}

}  // namespace remod
