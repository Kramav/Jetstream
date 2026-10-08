// The cutscene runtime (CLAUDE.md §10 M3 route 3), outside the game: its camera interpolation run in Lua 5.4 against
// stand-ins for REFramework's tables, the shipped scripts' syntax, and the example cutscene's shape.
#include "cutscene.hpp"
#include "game_code.hpp"
#include "graph.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <lua.hpp>
#include <nlohmann/json.hpp>

#include <fstream>

namespace fs = std::filesystem;

namespace {

// Just enough of REFramework for the runtime to load in a game it doesn't know ("test": no hooks are set).
const char* kStandIns = R"lua(
reframework = { get_game_name = function() return "test" end, is_key_down = function() return false end }
sdk, log, imgui, draw = {}, { info = function() end }, {}, {}
local none = function() end
re = { on_pre_application_entry = none, on_application_entry = none, on_frame = none, on_draw_ui = none,
       on_script_reset = none }
fs = { glob = function() return {} end }
json = { load_file = function() return nil end, dump_file = function() return true end }
Vector3f = { new = function(x, y, z) return { x = x, y = y, z = z } end }
Quaternion = { new = function(w, x, y, z)
    return { w = w, x = x, y = y, z = z, slerp = function(a, b, u) return { w = a.w + (b.w - a.w) * u } end }
end }
)lua";

// Keys: linear into the second, a cut to the third (held until 6 s), the default smooth into the fourth.
const char* kChecks = R"lua(
local m = ...
local keys = {
    { t = 0, position = { 0, 0, 0 }, rotation = { 0, 0, 0, 1 }, fov = 60 },
    { t = 4, position = { 4, 0, 0 }, rotation = { 0, 0, 0, 1 }, fov = 40, ease = "linear" },
    { t = 6, position = { 0, 2, 0 }, rotation = { 0, 0, 0, 1 }, ease = "cut" },
    { t = 8, position = { 0, 0, 8 }, rotation = { 0, 0, 0, 1 }, fov = 50 },
}
local function near(a, b) return math.abs(a - b) < 1e-6 end
local p, q, f = m.camera_at(keys, -1)
assert(p.x == 0 and f == 60, "before the first key: the first")
p, q, f = m.camera_at(keys, 2)
assert(near(p.x, 2) and near(f, 50), "linear: halfway at half the time")
p = m.camera_at(keys, 5)
assert(near(p.x, 4) and near(p.y, 0), "cut: stays on the key before")
p, q, f = m.camera_at(keys, 7)
assert(near(p.y, 1) and near(p.z, 4) and f == nil, "smooth: halfway at half the time; no FOV without both")
p = m.camera_at(keys, 6.5)
assert(near(p.z, 8 * 0.15625), "smooth: eased (a quarter of the time, 15.6% of the way)")
p = m.camera_at(keys, 100)
assert(p.z == 8, "after the last key: the last")
assert(m.camera_at({}, 1) == nil, "no keys: nothing")
)lua";

std::string run_lua(const std::string& runtime) {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    std::string error;
    if (luaL_dostring(L, kStandIns) != LUA_OK || luaL_dofile(L, runtime.c_str()) != LUA_OK ||  // leaves its module
        luaL_loadstring(L, kChecks) != LUA_OK) {
        error = lua_tostring(L, -1);
    } else {
        lua_insert(L, -2);  // the checks, then the module as their argument
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) error = lua_tostring(L, -1);
    }
    lua_close(L);
    return error;
}

}  // namespace

TEST_CASE("cutscene runtime: the camera between its keys") {
    CHECK(run_lua(REMOD_RUNTIME_DIR "/remod_cutscene.lua").empty());
}

TEST_CASE("cutscene runtime and probe: Lua 5.4 syntax; the example cutscene's shape") {
    for (const char* file : {REMOD_RUNTIME_DIR "/remod_cutscene.lua", REMOD_RUNTIME_DIR "/../spikes/cutscene_probe.lua",
                             REMOD_RUNTIME_DIR "/../spikes/new_movie_probe.lua",
                             REMOD_RUNTIME_DIR "/../spikes/sound_probe.lua",
                             REMOD_RUNTIME_DIR "/../spikes/character_probe.lua"}) {
        const std::string source = test::read_file(file);
        REQUIRE_FALSE(source.empty());
        CHECK(remod::check_lua(source, fs::path(file).filename().string(), nullptr).empty());
    }
    std::ifstream in(REMOD_SCHEMAS_DIR "/cutscene.v0.example.json");
    const auto c = nlohmann::json::parse(in);
    CHECK(c.at("schema_version") == 0);
    double last = -1;
    for (const auto& k : c.at("camera")) {
        CHECK(k.at("t").get<double>() > last);  // in time order
        last = k.at("t").get<double>();
        CHECK(k.at("position").size() == 3);
        CHECK(k.at("rotation").size() == 4);  // x y z w
    }
    CHECK(last <= c.at("length").get<double>());
    for (const auto& s : c.at("subtitles")) CHECK(s.at("t").get<double>() < s.at("until").get<double>());
}

TEST_CASE("cutscene files: the runtime and the cutscene, where the game reads them") {
    test::TempDir tmp;
    test::write_file(tmp.path / "cutscenes/door.json", "{}");
    const auto files = remod::cutscene_files(tmp.path / "cutscenes/door.json", REMOD_RUNTIME_DIR);
    REQUIRE(files.size() == 2);
    CHECK(files[0].game_path == "reframework/autorun/remod_cutscene.lua");
    CHECK(files[0].source == fs::path(REMOD_RUNTIME_DIR) / "remod_cutscene.lua");
    CHECK(files[1].game_path == "reframework/data/remod_cutscenes/door.json");
    CHECK(remod::cutscene_files(tmp.path / "cutscenes/door.json").size() == 2);  // the runtime found beside profiles
    CHECK_THROWS_WITH(remod::cutscene_files(tmp.path / "missing.json", REMOD_RUNTIME_DIR),
                      Catch::Matchers::ContainsSubstring("not a cutscene file"));
    test::write_file(tmp.path / "door.txt", "{}");
    CHECK_THROWS_AS(remod::cutscene_files(tmp.path / "door.txt", REMOD_RUNTIME_DIR), remod::PackageError);
    CHECK_THROWS_WITH(remod::cutscene_files(tmp.path / "cutscenes/door.json", tmp.path),
                      Catch::Matchers::ContainsSubstring("runtime is missing"));
    CHECK(remod::cutscene_runtime(REMOD_RUNTIME_DIR).game_path == "reframework/autorun/remod_cutscene.lua");
}

TEST_CASE("check_cutscene names every problem") {
    CHECK(remod::check_cutscene(test::read_file(REMOD_SCHEMAS_DIR "/cutscene.v0.example.json")).empty());
    CHECK(remod::check_cutscene(test::read_file(REMOD_SCHEMAS_DIR "/../spikes/cutscene_test.json")).empty());
    CHECK(remod::check_cutscene(test::read_file(REMOD_SCHEMAS_DIR "/../examples/cutscenes/play_a_movie.json")).empty());
    CHECK(remod::check_cutscene("{ nope") == std::vector<std::string>{"not readable JSON"});
    const std::vector<std::string> p = remod::check_cutscene(R"({
        "schema_version": 1, "length": 5, "start": {"key": "F10"}, "letterbox": 0.6,
        "camera": [
            {"t": 0, "position": [0, 0, 0], "rotation": [0, 0, 0, 1]},
            {"t": 0, "position": [0, 0], "rotation": [0, 0, 0, 3], "fov": 200, "ease": "bounce"},
            {"t": 9, "position": [0, 0, 0], "rotation": [0, 0, 0, 1]}
        ],
        "subtitles": [{"t": 3, "until": 2}],
        "fades": [{"t": 0, "until": 1, "from": 2, "to": 0}],
        "motions": [{"t": 1, "bank": -1, "motion": 2.5, "actor": "ashley"}],
        "movies": [{"t": 6, "id": "mva000"}, {"t": 1, "id": "../x"}]
    })");
    const std::vector<std::string> want{
        "schema_version must be 0",
        "start.key must be one of F1-F9, F11, F12 (F10 records camera keys)",
        "letterbox must be a number from 0 to below 0.5 (each bar's share of the screen's height)",
        "camera key 2: t 0 isn't after the key before it (0)",
        "camera key 2: position must be [x, y, z]",
        "camera key 2: rotation isn't a unit quaternion",
        "camera key 2: fov must be degrees, above 1 and below 179",
        "camera key 2: ease must be smooth, linear or cut",
        "camera key 3: t 9 is past the length (5)",
        "subtitle 1: until 2 must be after t 3",
        "subtitle 1: text must be text",
        "fade 1: from must be from 0 (clear) to 1 (black)",
        "motion 1: bank must be a whole number, 0 or more",
        "motion 1: motion must be a whole number, 0 or more",
        "motion 1: actor can only be \"player\" for now",
        "movie 1: t 6 is past the length (5)",
        "movie 2: id must be the movie's name, e.g. \"mva000\"",
    };
    CHECK(p == want);
}

TEST_CASE("Use recording: the recorded camera into a cutscene, keeping the rest") {
    test::TempDir tmp;
    const fs::path rec = tmp.path / "game/reframework/data/remod_cutscenes/recording.json";
    const fs::path cut = tmp.path / "cutscenes/door.json";
    CHECK_THROWS_WITH(remod::use_recording(cut, rec), Catch::Matchers::ContainsSubstring("no recording yet"));
    test::write_file(rec, R"({"schema_version": 0, "camera": []})");
    CHECK_THROWS_WITH(remod::use_recording(cut, rec), Catch::Matchers::ContainsSubstring("no camera keys"));
    test::write_file(rec, R"({"schema_version": 0, "name": "Recording", "length": 3.5, "camera": [
        {"t": 0, "position": [1, 2, 3], "rotation": [0, 0, 0, 1], "fov": 60, "ease": "smooth"},
        {"t": 3.5, "position": [4, 5, 6], "rotation": [0, 0, 0, 1], "fov": 50, "ease": "smooth"}]})");

    remod::use_recording(cut, rec);  // a new cutscene, named after its file
    auto c = nlohmann::json::parse(test::read_file(cut));
    CHECK(c["name"] == "door");
    CHECK(c["camera"].size() == 2);
    CHECK(c["length"] == 4.5);  // a second on the last shot
    CHECK(remod::check_cutscene(test::read_file(cut)).empty());
    CHECK_FALSE(fs::exists(fs::path(cut) += ".bak"));

    // An existing one keeps its subtitles and a longer length; the previous version is kept.
    c["subtitles"] = {{{"t", 1}, {"until", 2}, {"text", "Hello"}}};
    c["length"] = 10;
    test::write_file(cut, c.dump());
    remod::use_recording(cut, rec);
    const auto again = nlohmann::json::parse(test::read_file(cut));
    CHECK(again["subtitles"][0]["text"] == "Hello");
    CHECK(again["length"] == 10);
    CHECK(nlohmann::json::parse(test::read_file(fs::path(cut) += ".bak"))["length"] == 10);
    test::write_file(cut, "{ broken");
    CHECK_THROWS_WITH(remod::use_recording(cut, rec), Catch::Matchers::ContainsSubstring("isn't readable JSON"));
}

TEST_CASE("Cutscene blocks into Package: the runtime once, the run fails on a bad file") {
    test::TempDir tmp;
    test::write_file(tmp.path / "a.json", test::read_file(REMOD_SCHEMAS_DIR "/cutscene.v0.example.json"));
    test::write_file(tmp.path / "b.json", test::read_file(REMOD_SCHEMAS_DIR "/cutscene.v0.example.json"));
    remod::Graph g;
    g.add_node("Cutscene").params["cutscene"] = "a.json";
    g.add_node("Cutscene").params["cutscene"] = "b.json";
    g.nodes[0].params["show"] = "a.json";  // the app's list arrows set it on any list block
    auto& pkg = g.add_node("PackageMod");
    pkg.params["name"] = "M";
    pkg.params["out"] = "out";
    REQUIRE(g.connect({1, "files", 3, "file"}).empty());
    REQUIRE(g.connect({2, "files", 3, "file"}).empty());
    static remod::NativeConverter converter;
    const remod::Profile profile = remod::load_profile(REMOD_PROFILES_DIR "/re4r.toml");
    const remod::RunOptions options{.profile = profile, .converter = converter, .base_dir = tmp.path};

    const auto result = remod::run_graph(g, options);
    CHECK(result.nodes.at(1).message == "a.json checked");
    CHECK(fs::is_regular_file(tmp.path / "out/M/reframework/autorun/remod_cutscene.lua"));
    CHECK(fs::is_regular_file(tmp.path / "out/M/reframework/data/remod_cutscenes/a.json"));
    CHECK(fs::is_regular_file(tmp.path / "out/M/reframework/data/remod_cutscenes/b.json"));

    test::write_file(tmp.path / "b.json", R"({"schema_version": 0, "length": 0})");
    try {
        remod::run_graph(g, options);
        FAIL("the run should fail");
    } catch (const remod::RunError& e) {
        CHECK_THAT(std::string(e.what()), Catch::Matchers::ContainsSubstring("b.json: length (seconds) must be"));
    }

    // Two different files at one game path are still refused.
    test::write_file(tmp.path / "x/a.json", "{}");
    remod::PackageSpec spec{.mod_name = "N", .out_dir = tmp.path / "out", .info = {.name = "N"},
                            .files = {{tmp.path / "a.json", "reframework/data/remod_cutscenes/a.json"},
                                      {tmp.path / "x/a.json", "reframework/data/remod_cutscenes/a.json"}},
                            .zip = false};
    CHECK_THROWS_WITH(remod::build_package(profile, spec), Catch::Matchers::ContainsSubstring("listed twice"));
}
