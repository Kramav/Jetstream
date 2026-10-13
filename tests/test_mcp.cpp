// The MCP server (core/mcp.hpp) driven as an MCP client would: JSON-RPC messages in, replies out.
#include "mcp.hpp"
#include "image.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <nlohmann/json.hpp>

using Catch::Matchers::ContainsSubstring;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

int next_id = 1;

// A tools/call, its result.
json call(remod::McpServer& server, const std::string& tool, json arguments = json::object()) {
    const json request{{"jsonrpc", "2.0"}, {"id", next_id++}, {"method", "tools/call"},
                       {"params", {{"name", tool}, {"arguments", std::move(arguments)}}}};
    const json reply = json::parse(server.handle(request.dump()));
    REQUIRE(reply.contains("result"));
    return reply["result"];
}

std::string text_of(const json& result) { return result["content"][0].value("text", ""); }

}  // namespace

TEST_CASE("mcp: the handshake, the tool list, and nothing that lets the AI approve") {
    remod::McpServer server({});
    const json init = json::parse(server.handle(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"test","version":"1"}}})"));
    CHECK(init["result"]["protocolVersion"] == "2025-06-18");
    CHECK(init["result"]["capabilities"].contains("tools"));
    CHECK_THAT(init["result"]["instructions"].get<std::string>(), ContainsSubstring("you cannot approve"));
    CHECK(server.handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").empty());

    const json list = json::parse(server.handle(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"));
    const json& tools = list["result"]["tools"];
    CHECK(std::ranges::any_of(tools, [](const json& t) { return t["name"] == "run"; }));
    CHECK(std::ranges::any_of(tools, [](const json& t) { return t["name"] == "view_image"; }));
    CHECK_FALSE(list.dump().find("\"approve\"") != std::string::npos);  // no tool takes it

    CHECK_THAT(text_of(call(server, "list_blocks")), ContainsSubstring("\"ai\":\"Start of most graphs"));
    CHECK(json::parse(server.handle(R"({"jsonrpc":"2.0","id":3,"method":"no/such"})"))["error"]["code"] == -32601);
    CHECK(call(server, "no_such_tool")["isError"] == true);
}

TEST_CASE("mcp: a run's changes are approved by the user's answer only, never by the AI") {
    test::TempDir tmp;
    const fs::path g = tmp.path / "g", outside = tmp.path / "elsewhere" / "a.txt";
    test::write_file(g / "a.txt", "A");
    std::vector<std::string> asked;
    bool answer = false;
    remod::McpServer server({.approve = [&](const std::string& changes) {
        asked.push_back(changes);
        return answer;
    }});
    call(server, "new_graph");
    CHECK_THAT(text_of(call(server, "add_block", {{"type", "CopyFile"}})), ContainsSubstring("\"id\":1"));
    call(server, "set_field", {{"id", 1}, {"input", "source"}, {"value", "a.txt"}});
    call(server, "set_field", {{"id", 1}, {"input", "dest"}, {"value", outside.generic_string()}});
    call(server, "save_graph", {{"file", (g / "g.json").generic_string()}});
    CHECK_THAT(text_of(call(server, "plan_run")), ContainsSubstring("needs approval"));

    const json declined = call(server, "run", {{"approve", true}});  // what the AI sends counts for nothing
    CHECK(declined["isError"] == true);
    CHECK_THAT(text_of(declined), ContainsSubstring("declined"));
    REQUIRE(asked.size() == 1);
    CHECK_THAT(asked[0], ContainsSubstring("write") && ContainsSubstring("a.txt"));
    CHECK_FALSE(fs::exists(outside));

    answer = true;
    const json ran = call(server, "run");
    CHECK(ran["isError"] == false);
    CHECK_THAT(text_of(ran), ContainsSubstring("\"message\":\"Done.\""));
    CHECK(fs::exists(outside));
}

TEST_CASE("mcp: view_image hands the AI the picture") {
    test::TempDir tmp;
    remod::save_png_bgra(tmp.path / "pic.png", 4, 2, std::vector<std::uint8_t>(4 * 2 * 4, 200));
    remod::McpServer server({});
    call(server, "new_graph");
    call(server, "add_block", {{"type", "ImportImage"}});
    call(server, "set_field", {{"id", 1}, {"input", "png"}, {"value", (tmp.path / "pic.png").generic_string()}});
    call(server, "add_block", {{"type", "Preview"}});
    call(server, "link", {{"from", 1}, {"from_port", "image"}, {"to", 2}, {"to_port", "in"}});
    const json seen = call(server, "view_image", {{"id", 2}, {"size", 64}});
    REQUIRE(seen["isError"] == false);
    CHECK(seen["content"][0]["type"] == "image");
    CHECK(seen["content"][0]["mimeType"] == "image/png");
    CHECK(seen["content"][0]["data"].get<std::string>().starts_with("iVBORw0KGgo"));  // PNG's signature in base64
    CHECK_THAT(seen["content"][1]["text"].get<std::string>(), ContainsSubstring("4x2"));
    CHECK(call(server, "view_image", {{"id", 99}})["isError"] == true);
}

TEST_CASE("mcp: scripts read, written, checked, installed in game and their errors read back") {
    test::TempDir tmp;
    const fs::path g = tmp.path / "g", game = tmp.path / "game";
    fs::create_directories(g);
    test::write_file(game / "dinput8.dll", "");  // REFramework is there
    test::write_file(game / "re2_fw_config.txt", "ScriptRunner_LogToDisk=true\n");
    std::vector<std::string> asked;
    bool answer = false;
    remod::McpServer server({.game_dir = game, .approve = [&](const std::string& what) {
                                 asked.push_back(what);
                                 return answer;
                             }});
    call(server, "new_graph");
    call(server, "save_graph", {{"file", (g / "g.json").generic_string()}});

    // Inside the graph's folder: written without asking; a second write keeps the first as .bak.
    CHECK(call(server, "write_script", {{"file", "my_mod.lua"}, {"text", "print(1)"}})["isError"] == false);
    CHECK(call(server, "write_script", {{"file", "my_mod.lua"}, {"text", "print(2)"}})["isError"] == false);
    CHECK(test::read_file(g / "my_mod.lua.bak") == "print(1)");
    CHECK_THAT(text_of(call(server, "read_script", {{"file", "my_mod.lua"}})), ContainsSubstring("print(2)"));
    CHECK(asked.empty());
    CHECK(call(server, "write_script", {{"file", "notes.txt"}, {"text", "x"}})["isError"] == true);

    // Outside it: the user decides, whatever the AI sends.
    const fs::path outside = tmp.path / "elsewhere.lua";
    CHECK(call(server, "write_script", {{"file", outside.generic_string()}, {"text", "x"}, {"approve", true}})["isError"] == true);
    CHECK(asked.size() == 1);
    CHECK_FALSE(fs::exists(outside));

    CHECK_THAT(text_of(call(server, "check_cutscene", {{"text", "{}"}})), ContainsSubstring("problems\":[\""));

    // Test in game: asked first; declined changes nothing.
    CHECK(call(server, "install_in_game", {{"file", "my_mod.lua"}})["isError"] == true);
    CHECK_FALSE(fs::exists(game / "reframework/autorun/my_mod.lua"));
    answer = true;
    CHECK(call(server, "install_in_game", {{"file", "my_mod.lua"}})["isError"] == false);
    CHECK(fs::exists(game / "reframework/autorun/my_mod.lua"));

    test::write_file(game / "re2_framework_log.txt",
                     "[2026-10-13 10:00:00.000] [REFramework] [error] my_mod.lua:1: attempt to call a nil value\n");
    const std::string errors = text_of(call(server, "game_errors", {{"file", "my_mod.lua"}}));
    CHECK_THAT(errors, ContainsSubstring("\"line\":1") && ContainsSubstring("nil value"));

    CHECK(call(server, "remove_from_game", {{"file", "my_mod.lua"}})["isError"] == false);
    CHECK_FALSE(fs::exists(game / "reframework/autorun/my_mod.lua"));
}
