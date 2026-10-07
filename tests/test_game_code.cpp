// REFramework scripts: the SDK dump's game code, and checking a script's names against it (CLAUDE.md §10 M2).
// The dump here is synthetic, in the layout REFramework's ObjectExplorer writes.
#include "api.hpp"
#include "game_code.hpp"
#include "graph.hpp"
#include "script_ai.hpp"
#include "settings.hpp"

#include "helpers.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <nlohmann/json.hpp>

using Catch::Matchers::ContainsSubstring;
using test::TempDir;
using test::write_file;
namespace fs = std::filesystem;

namespace {

// app.Character (fields Hp, static Count; get_HP, set_HP(Int32), a generic definition Find) <- app.Player (field
// _Name; get_Name, Jump(Single, Boolean)); app.PlayerManager (static get_Instance). Junk where REFramework writes more.
const char* kDump = R"({
  "app.Character": {
    "address": "14000", "crc": "ab", "fqn": "12",
    "fields": {
      "Hp": {"id": 1, "type": "System.Int32", "flags": "Private", "offset_from_base": "0x10"},
      "Count": {"id": 2, "type": "System.Int32", "flags": "Public | Static"}
    },
    "methods": {
      "get_HP10": {"id": 10, "flags": "Public | HideBySig", "returns": {"type": "System.Int32", "name": ""},
                   "params": null, "function": "1400"},
      "set_HP11": {"id": 11, "returns": {"type": "System.Void", "name": ""},
                   "params": [{"type": "System.Int32", "name": "value", "flags": "In"}]},
      "Find12": {"id": 12, "returns": {"type": "!!0", "name": ""}, "params": [{"type": "System.String", "name": "key"}]}
    },
    "RSZ": [{"type": "fields", "fields": {"NotAField": {"type": "x"}}}],
    "properties": {"HP": {"id": 3, "getter": "get_HP", "setter": "set_HP"}}
  },
  "app.Player": {
    "parent": "app.Character",
    "fields": {"_Name": {"type": "System.String"}},
    "methods": {
      "get_Name20": {"id": 20, "returns": {"type": "System.String"}, "params": []},
      "Jump21": {"id": 21, "returns": {"type": "System.Void"},
                 "params": [{"type": "System.Single", "name": "height"}, {"type": "System.Boolean", "name": "high"}]}
    }
  },
  "app.PlayerManager": {
    "parent": "via.Behavior",
    "methods": {"get_Instance30": {"id": 30, "flags": "Public | Static", "returns": {"type": "app.PlayerManager"}}}
  }
})";

remod::GameCode dump_code() {
    TempDir tmp;
    write_file(tmp.path / "il2cpp_dump.json", kDump);
    return remod::load_game_code(tmp.path / "il2cpp_dump.json");
}

std::string problems(const std::string& source, const remod::GameCode* code) {
    std::string out;
    for (const auto& p : remod::check_lua(source, "t.lua", code)) out += std::to_string(p.line) + ": " + p.message + "\n";
    return out;
}

}  // namespace

TEST_CASE("an SDK dump gives types, parents, fields and methods, nothing else") {
    const remod::GameCode code = dump_code();
    REQUIRE(code.types.size() == 3);
    const remod::GameType& c = *code.find("app.Character");
    CHECK(c.parent.empty());
    REQUIRE(c.fields.size() == 2);  // not RSZ's "fields"
    CHECK(c.fields[0].name == "Hp");
    CHECK(c.fields[0].type == "System.Int32");
    CHECK_FALSE(c.fields[0].is_static);
    CHECK(c.fields[1].is_static);
    REQUIRE(c.methods.size() == 3);
    CHECK(c.methods[0].name == "get_HP");  // its key without the id
    CHECK(c.methods[0].returns == "System.Int32");
    CHECK(c.methods[0].params.empty());
    CHECK(c.methods[1].prototype() == "set_HP(System.Int32)");
    CHECK(c.methods[1].params[0].second == "value");
    CHECK(code.find("app.PlayerManager")->methods[0].is_static);

    // Lookups as REFramework's: through parents, by name or by prototype, generic definitions skipped.
    CHECK(code.method("app.Player", "get_HP"));
    CHECK(code.method("app.Player", "Jump(System.Single, System.Boolean)"));
    CHECK_FALSE(code.method("app.Player", "Jump(System.Single)"));
    CHECK_FALSE(code.method("app.Character", "Find"));
    CHECK(code.field("app.Player", "Hp"));
    CHECK_FALSE(code.field("app.Character", "_Name"));
    CHECK_FALSE(code.method("app.Nope", "get_HP"));

    TempDir tmp;
    write_file(tmp.path / "bad.json", "{\"a\": [");
    CHECK_THROWS_WITH(remod::load_game_code(tmp.path / "bad.json"), ContainsSubstring("isn't a readable SDK dump"));
    write_file(tmp.path / "empty.json", "{}");
    CHECK_THROWS_WITH(remod::load_game_code(tmp.path / "empty.json"), ContainsSubstring("holds no types"));
    CHECK_THROWS_WITH(remod::load_game_code(tmp.path / "missing.json"), ContainsSubstring("not found"));
}

TEST_CASE("the dump is read once into a cache, and again when it changes") {
    TempDir tmp;
    const fs::path dump = tmp.path / "il2cpp_dump.json", cache = tmp.path / "cache/code.txt";
    write_file(dump, kDump);
    CHECK(remod::load_game_code(dump, cache).types.size() == 3);
    REQUIRE(fs::is_regular_file(cache));
    CHECK(remod::load_game_code(dump, cache).method("app.Player", "Jump(System.Single, System.Boolean)"));

    // The cache is what's read while the dump is unchanged: put a type in it that the dump doesn't have.
    std::string text = test::read_file(cache);
    text += "T\tCached.Type\t\n";
    write_file(cache, text);
    CHECK(remod::load_game_code(dump, cache).find("Cached.Type"));
    write_file(cache, text.substr(0, text.find('\n') + 1) + "garbage\n");  // cut or not ours: the dump again
    CHECK(remod::load_game_code(dump, cache).types.size() == 3);

    write_file(dump, std::string(kDump) + " ");  // a new dump (another size): read again
    CHECK_FALSE(remod::load_game_code(dump, cache).find("Cached.Type"));
}

TEST_CASE("searching the game code") {
    const remod::GameCode code = dump_code();
    const auto hits = remod::search_game_code(code, "player");
    REQUIRE(hits.size() >= 2);
    CHECK(hits[0].type == "app.Player");
    CHECK(hits[0].member.empty());
    CHECK(hits[0].detail == "parent app.Character");
    CHECK(hits[1].type == "app.PlayerManager");

    const auto hp = remod::search_game_code(code, "character hp");
    REQUIRE(hp.size() == 3);  // Hp, get_HP, set_HP
    CHECK(hp[0].member == "Hp");
    CHECK(hp[2].detail == "System.Void set_HP(System.Int32 value)");

    const auto all = remod::search_game_code(code, " app.Player ");  // exactly a type: its own members
    REQUIRE(all.size() == 4);
    CHECK(all[3].detail == "System.Void Jump(System.Single height, System.Boolean high)");
    CHECK(remod::search_game_code(code, "player", 1).size() == 1);
    CHECK(remod::search_game_code(code, "   ").empty());
}

TEST_CASE("check_lua: syntax by Lua's own parser") {
    CHECK(problems("local x = 1\nprint(x)\n", nullptr).empty());
    CHECK(problems("for i = 1, 3 do i = i + 1 end", nullptr).empty());  // fine in 5.4 (5.5 refuses it)
    CHECK(problems("local x = 1\nif x then\nprint(x)\n", nullptr) == "4: syntax: 'end' expected (to close 'if' at line 2) near <eof>\n");
    CHECK_THAT(problems("local = 2", nullptr), ContainsSubstring("1: syntax: "));
}

TEST_CASE("check_lua: game names in strings") {
    const remod::GameCode code = dump_code();
    const auto* c = &code;
    CHECK(problems(R"lua(
local player_t = sdk.find_type_definition("app.Player")
local get_hp = player_t:get_method("get_HP")
local jump = player_t:get_method("Jump(System.Single, System.Boolean)")
local name = player_t:get_field("_Name")
sdk.hook(sdk.find_type_definition("app.Character"):get_method("set_HP(System.Int32)"), function(args) end, nil)
local mgr = sdk.get_managed_singleton("app.PlayerManager")
local list = sdk.typeof("app.Player[]")
local hp = sdk.create_instance("app.Player"):get_field("Hp")
if player_t == sdk.find_type_definition("app.Character") then end
)lua", c).empty());

    CHECK(problems("local t = sdk.find_type_definition(\"app.Playr\")\n", c) ==
          "1: no type \"app.Playr\" in the game (did you mean app.Player?)\n");
    CHECK(problems("local t = sdk.find_type_definition('app.Player')\n\nt:get_method('get_Hp')\n", c) ==
          "3: app.Player has no method \"get_Hp\" (did you mean get_HP, set_HP?)\n");
    CHECK(problems("sdk.find_type_definition(\"app.Player\"):get_method(\"Jump(System.Single)\")", c) ==
          "1: app.Player has no method \"Jump(System.Single)\" (it has Jump(System.Single, System.Boolean))\n");
    CHECK(problems("local m = sdk.get_managed_singleton(\"app.PlayerManager\")\nm:call(\"get_Instance\")\n"
                   "m:call(\"get_Instanse\")\nm:set_field(\"Nope\", 1)\n",
                   c) ==
          "3: app.PlayerManager has no method \"get_Instanse\" (did you mean get_Instance?)\n"
          "4: app.PlayerManager has no field \"Nope\"\n");
    CHECK(problems("local t = sdk.find_type_definition(\"app.Player\")\nt:get_field(\"Hpp\")", c) ==
          "2: app.Player has no field \"Hpp\" (did you mean Hp?)\n");

    // Not code: comments and strings that aren't passed to sdk.
    CHECK(problems("-- sdk.find_type_definition(\"Nope\")\n--[==[\nsdk.find_type_definition(\"Nope\")\n]==]\n"
                   "local s = [[sdk.find_type_definition(\"Nope\")]]\nprint(\"sdk.find_type_definition('Nope')\")\n",
                   c)
              .empty());
    // A variable set to something else is no longer followed; one in a field isn't either.
    CHECK(problems("local t = sdk.find_type_definition(\"app.Player\")\nt = other()\nt:get_method(\"x\")\n"
                   "self.u = sdk.find_type_definition(\"app.Player\")\nu:get_method(\"y\")\n",
                   c)
              .empty());
    // A syntax error and a name problem together, in line order (on one line, the syntax first).
    CHECK(problems("x(\nsdk.find_type_definition(\"Nope.X\")", c) ==
          "2: syntax: ')' expected (to close '(' at line 1) near <eof>\n2: no type \"Nope.X\" in the game\n");
}

TEST_CASE("API and MCP: check_script and game_code") {
    remod::ApiSession api;
    using nlohmann::json;
    auto call = [&](const json& r) { return json::parse(api.call(r.dump())); };

    json reply = call({{"op", "check_script"}, {"text", "if x then"}});
    REQUIRE(reply["ok"] == true);
    CHECK(reply["problems"][0]["line"] == 1);
    CHECK_THAT(reply["problems"][0]["message"].get<std::string>(), ContainsSubstring("syntax"));
    CHECK_THAT(reply["names_unchecked"].get<std::string>(), ContainsSubstring("no SDK dump"));
    CHECK(call({{"op", "game_code"}, {"query", "player"}})["ok"] == false);
    CHECK(call({{"op", "check_script"}})["error"] == "check_script needs a file or a text");

    TempDir tmp;
    write_file(tmp.path / "il2cpp_dump.json", kDump);
    write_file(tmp.path / "m.lua", "sdk.find_type_definition(\"app.Player\"):get_method(\"Jum\")\n");
    remod::set_sdk_dump(tmp.path / "il2cpp_dump.json");
    struct Reset {
        ~Reset() { remod::set_sdk_dump({}); }  // other tests check without a dump
    } reset;
    reply = call({{"op", "check_script"}, {"file", (tmp.path / "m.lua").string()}});
    REQUIRE(reply["ok"] == true);
    CHECK_FALSE(reply.contains("names_unchecked"));
    CHECK(reply["problems"][0]["message"] == "app.Player has no method \"Jum\" (did you mean Jump?)");
    reply = call({{"op", "game_code"}, {"query", "app.Player"}});
    REQUIRE(reply["ok"] == true);
    CHECK(reply["hits"].size() == 4);
    CHECK(reply["hits"][1]["member"] == "_Name");
}

TEST_CASE("Lua script block: a name not in the game warns, a syntax error fails the run") {
    TempDir tmp;
    write_file(tmp.path / "il2cpp_dump.json", kDump);
    write_file(tmp.path / "m.lua", "local t = sdk.find_type_definition('app.Player')\nt:get_field('Hpp')\n");
    remod::set_sdk_dump(tmp.path / "il2cpp_dump.json");
    struct Reset {
        ~Reset() { remod::set_sdk_dump({}); }
    } reset;
    remod::Graph g;
    g.add_node("LuaScript").params["script"] = "m.lua";
    static remod::NativeConverter converter;
    const remod::Profile profile = remod::load_profile(REMOD_PROFILES_DIR "/re4r.toml");
    const remod::RunOptions options{.profile = profile, .converter = converter, .base_dir = tmp.path};

    const auto result = remod::run_graph(g, options);
    CHECK(result.nodes.at(1).message == "m.lua, 1 problem(s): see the warnings");
    REQUIRE(result.warnings.size() == 1);
    CHECK_THAT(result.warnings[0], ContainsSubstring("m.lua:2: app.Player has no field \"Hpp\" (did you mean Hp?)"));

    write_file(tmp.path / "m.lua", "if x then\n");
    CHECK_THROWS_WITH(remod::run_graph(g, options), ContainsSubstring("m.lua:2: syntax: 'end' expected"));
}

TEST_CASE("Write with AI: Claude's answer becomes the script, and hand edits are never overwritten") {
    // The script and the words before it.
    auto [script, notes] = remod::split_answer("Adds a window.\n\n```lua\nprint('a')\n```\n");
    CHECK(script == "print('a')\n");
    CHECK(notes == "Adds a window.");
    std::tie(script, notes) = remod::split_answer("Old:\n```lua\nx()\n```\nNew:\n```\nprint('b')\n```");
    CHECK(script == "x()\n");  // the last ```lua block wins over a later plain one
    CHECK(remod::split_answer("```\ny()\n```").first == "y()\n");
    CHECK_THROWS_WITH(remod::split_answer("I can't do that."), ContainsSubstring("didn't answer with a script"));
    CHECK_THROWS_WITH(remod::split_answer("```lua\nprint("), ContainsSubstring("cut off"));

    // Saving: a new file; a replaced one keeps its previous version; one edited meanwhile is left alone.
    TempDir tmp;
    const fs::path lua = tmp.path / "scripts/my_mod.lua";
    CHECK(remod::save_script(lua, "v1\n", remod::file_stamp(lua)) == lua);
    CHECK(test::read_file(lua) == "v1\n");
    CHECK_FALSE(fs::exists(fs::path(lua) += ".bak"));
    CHECK(remod::save_script(lua, "v2\n", remod::file_stamp(lua)) == lua);
    CHECK(test::read_file(lua) == "v2\n");
    CHECK(test::read_file(fs::path(lua) += ".bak") == "v1\n");
    const std::string asked = remod::file_stamp(lua);
    fs::last_write_time(lua, fs::last_write_time(lua) + std::chrono::seconds(5));  // the user saved meanwhile
    CHECK(remod::save_script(lua, "v3\n", asked) == tmp.path / "scripts/my_mod_claude.lua");
    CHECK(test::read_file(lua) == "v2\n");
    CHECK(test::read_file(tmp.path / "scripts/my_mod_claude.lua") == "v3\n");

    // A stand-in for Claude Code: answers as `claude -p --output-format json` does, if it was given its request.
    const fs::path fake = tmp.path / "bin/claude.cmd";
    write_file(fake,
               "@echo off\r\n"
               "if not exist request.md exit /b 3\r\n"
               "echo {\"result\":\"Says hi.\\n\\n```lua\\nprint(\\\"hi\\\")\\nif x then\\n```\",\"is_error\":false,"
               "\"total_cost_usd\":0.05}\r\n");
    remod::ScriptAsk ask{.script = lua, .request = "say hi", .game = "Resident Evil 4 (2023)", .claude = fake,
                         .remod = tmp.path / "remod.exe"};
    const remod::ScriptAnswer answer = remod::ask_claude(ask, std::chrono::minutes(1));
    CHECK(answer.script == "print(\"hi\")\nif x then\n");
    CHECK(answer.notes == "Says hi.");
    REQUIRE(answer.problems.size() == 1);  // checked like any script
    CHECK_THAT(answer.problems[0].message, ContainsSubstring("syntax"));
    const std::string request = test::read_file(remod::default_cache_dir().parent_path() / "ai/request.md");
    CHECK_THAT(request, ContainsSubstring("say hi") && ContainsSubstring("my_mod.lua") &&
                            ContainsSubstring("It exists: read it first") && ContainsSubstring("search_game_code"));

    write_file(fake, "@echo off\r\necho {\"result\":\"Not logged in\",\"is_error\":true}\r\n");
    CHECK_THROWS_WITH(remod::ask_claude(ask, std::chrono::minutes(1)), ContainsSubstring("Claude Code: Not logged in"));
    write_file(fake, "@echo off\r\necho something else\r\nexit /b 1\r\n");
    CHECK_THROWS_WITH(remod::ask_claude(ask, std::chrono::minutes(1)), ContainsSubstring("didn't answer"));
    // Never per use: an API key (or a key helper, Bedrock, Vertex) turns it off.
    SetEnvironmentVariableW(L"ANTHROPIC_API_KEY", L"sk-test");
    CHECK_THROWS_WITH(remod::ask_claude(ask, std::chrono::minutes(1)), ContainsSubstring("would bill per use"));
    SetEnvironmentVariableW(L"ANTHROPIC_API_KEY", nullptr);
    write_file(tmp.path / "settings.json", R"({"apiKeyHelper": "get-key.cmd"})");
    CHECK(remod::per_use_billing(tmp.path / "settings.json") == "an apiKeyHelper in settings.json");
    write_file(tmp.path / "settings.json", R"({"env": {"CLAUDE_CODE_USE_BEDROCK": "1"}})");
    CHECK(remod::per_use_billing(tmp.path / "settings.json") == "CLAUDE_CODE_USE_BEDROCK in settings.json");
    write_file(tmp.path / "settings.json", R"({"model": "opus"})");
    CHECK(remod::per_use_billing(tmp.path / "settings.json").empty());
    ask.request = "  ";
    CHECK_THROWS_WITH(remod::ask_claude(ask), ContainsSubstring("say what the script should do"));
    ask.claude.clear();
    CHECK_THROWS_WITH(remod::ask_claude(ask), ContainsSubstring("isn't installed"));
}
