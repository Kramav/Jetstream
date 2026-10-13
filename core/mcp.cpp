#include "mcp.hpp"

#include <nlohmann/json.hpp>

#include <vector>

namespace remod {

namespace {

using nlohmann::json;

constexpr const char* kInstructions =
    "remod builds RE Engine game mods (textures, meshes, movies, sounds, REFramework scripts and cutscenes) as node graphs, one game per graph (profile: re2r, re3r, re4r, re7, re8, re9). Work like this: "
    "list_blocks first (only blocks the open graph's game supports are listed; each type's 'ai' note says when to use it; some games support only textures); new_graph (pick the game with profile) or open_graph; add_block, set_field, "
    "link; validate until it reports no problems; preview and view_image to check results before running; "
    "save_graph (a run needs the graph saved: its folder is where it may write freely); run. "
    "A run asks the user, in a dialog of its own, to allow removing files, writing outside the graph's folder and "
    "starting programs; you cannot approve anything. A run that pauses at an Edit image or Edit video step waits for "
    "the user: tell them which file to edit and call edit_done only after they say they have finished. Game files are never "
    "changed. Texture paths: files named .tex.<version> (the game's profile says which) under the extracted natives folder; use {game} for the Game files folder. "
    "REFramework Lua scripts (a Lua script block, linked into Package's 'other file'): REFramework runs each .lua directly "
    "in reframework/autorun (modules it requires go in a folder named like it, beside it); read the REFramework book "
    "(https://cursey.github.io/reframework-book/) for its API instead of guessing; find every game name with "
    "search_game_code, never from memory; check_script until it reports no problems; check_cutscene for cutscene files. read_script before changing a file, write_script to save it (the user can always edit it by hand too). To test, install_in_game (the user approves), then after they play read game_errors. Behaviour in game can only be "
    "tested by the user.";

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
        {"new_graph", "new", "Start an empty graph.", object({{"profile", text("Game profile id: re2r, re3r, re4r (default), re7, re8 or re9.")}})},
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
         "Returns where every block got to; 'paused' means an Edit image or Edit video step waits for the user.",
         object()},
        {"search_game_code", "game_code",
         "Search the game's code (REFramework's SDK dump): types, fields and methods whose names hold every word of "
         "the query. A query naming a type exactly lists all its members. Use it for every name a script uses.",
         object({{"query", text("Words, e.g. 'player hp', or a full type name, e.g. 'chainsaw.PlayerManager'.")},
                 {"limit", number("At most this many hits (default 60).")}},
                {"query"})},
        {"check_script", "check_script",
         "Check a REFramework Lua script: syntax, and every game type, method and field name it passes as a string. "
         "Fix every problem before telling the user it's ready.",
         object({{"file", text("The .lua file (relative: to the graph's folder).")},
                 {"text", text("Or the script's text.")}})},
        {"read_script", "read_script", "Read a Lua script or cutscene file (always read it before changing it: keep the user's edits).",
         object({{"file", text("The file (relative: to the graph's folder).")}}, {"file"})},
        {"write_script", "write_script",
         "Write a .lua script or .json cutscene file whole; the old one is kept as <file>.bak. Inside the graph's "
         "folder it's fine; elsewhere the user is asked in a dialog. Then check_script / check_cutscene.",
         object({{"file", text("The file (relative: to the graph's folder).")}, {"text", text("Its whole text.")}},
                {"file", "text"})},
        {"check_cutscene", "check_cutscene",
         "Check a cutscene file (format v0, schemas/cutscene.v0.example.json): every problem named. Fix them all.",
         object({{"file", text("The .json file (relative: to the graph's folder).")}, {"text", text("Or its text.")}})},
        {"install_in_game", "install",
         "Test in game: copy a Lua script (.lua, with its modules folder) or a cutscene (.json, with the runtime) into the "
         "game folder. The user is asked in a dialog; you can't approve. Then the user presses Reset Scripts in "
         "REFramework's menu.",
         object({{"file", text("The .lua or cutscene .json (relative: to the graph's folder).")}}, {"file"})},
        {"remove_from_game", "uninstall", "Undo install_in_game for that file. The user is asked in a dialog.",
         object({{"file", text("The same file.")}}, {"file"})},
        {"game_errors", "game_errors",
         "What REFramework's log says went wrong in a script since the game started (needs 'Log Lua Errors to Disk'). "
         "Fix them, install again, and have the user retest. Never claim a script works in game: only the user can see.",
         object({{"file", text("The script's .lua file.")}}, {"file"})},
        {"edit_done", "edit_done",
         "Mark an Edit image or Edit video step done ONLY after the user says they finished that edit.",
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
        if (!options_.game_dir.empty()) request["game_dir"] = options_.game_dir.string();
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
            } else if (name == "write_script") {
                args["op"] = tool->op;
                std::string answer = api_.call(args.dump());
                if (json::parse(answer).value("needs_approval", false)) {
                    if (options_.approve && options_.approve("write " + args.value("file", "") + " (outside the graph's folder)")) {
                        args["approve"] = true;
                        answer = api_.call(args.dump());
                    } else {
                        answer = json{{"ok", false}, {"error", "the user declined; nothing was written"}}.dump();
                    }
                }
                reply["result"] = result(answer);
            } else if (name == "install_in_game" || name == "remove_from_game") {
                const std::string what = (name == "install_in_game" ? "copy into" : "remove from") +
                                         std::string(" the game folder ") + options_.game_dir.string() + ": " +
                                         args.value("file", "");
                if (!(options_.approve && options_.approve(what))) {
                    reply["result"] = result(json{{"ok", false}, {"error", "the user declined; nothing was changed"}}.dump());
                } else {
                    args["op"] = tool->op;
                    args["approve"] = true;
                    reply["result"] = result(api_.call(with_options(args).dump()));
                }
            } else {
                args["op"] = tool->op;
                if (std::string_view(tool->op) == "plan" || std::string_view(tool->op) == "game_errors") args = with_options(args);
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
