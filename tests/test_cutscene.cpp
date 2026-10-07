// The cutscene runtime (CLAUDE.md §10 M3 route 3), outside the game: its camera interpolation run in Lua 5.4 against
// stand-ins for REFramework's tables, the shipped scripts' syntax, and the example cutscene's shape.
#include "game_code.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>

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
    for (const char* file : {REMOD_RUNTIME_DIR "/remod_cutscene.lua", REMOD_RUNTIME_DIR "/../spikes/cutscene_probe.lua"}) {
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
