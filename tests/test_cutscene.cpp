// The cutscene runtime (CLAUDE.md §10 M3 route 3), outside the game: its camera interpolation run in Lua 5.4 against
// stand-ins for REFramework's tables, the shipped scripts' syntax, and the example cutscene's shape.
#include "browse.hpp"
#include "cutscene.hpp"
#include "game_code.hpp"
#include "graph.hpp"
#include "image.hpp"
#include "profile.hpp"
#include "texture_converter.hpp"

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
        "motion 1: actor must be \"player\" or one of the actors' names",
        "movie 1: t 6 is past the length (5)",
        "movie 2: id must be the movie's name, e.g. \"mva000\"",
    };
    CHECK(p == want);
}

TEST_CASE("Actors: checked, motions name them, and remod's own puppet definitions go with the cutscene") {
    CHECK(remod::check_cutscene(test::read_file(REMOD_SCHEMAS_DIR "/../spikes/actor_test.json")).empty());
    const std::vector<std::string> p = remod::check_cutscene(R"({
        "schema_version": 0, "length": 5,
        "actors": [
            {"name": "luis", "puppet": "luis"},
            {"name": "luis", "puppet": "luis", "position": [1, 2], "offset": [0, 0, 1]},
            {"name": "player", "puppet": "../x", "position": [1, 2, 3], "rotation": [0, 0, 0, 3], "hides": "everyone"},
            {"name": "a", "puppet": "ashley", "offset": [1, 2]},
            "nope"
        ],
        "motions": [{"t": 1, "bank": 1000, "motion": 160, "actor": "luis"},
                    {"t": 1, "bank": 1000, "motion": 160, "actor": "nobody"}]
    })");
    const std::vector<std::string> want{
        "actor 2: name luis is already another actor's",
        "actor 2: puppet luis is already another actor (one actor per puppet for now)",
        "actor 2: position must be [x, y, z] (from Write down Leon's spot, never typed)",
        "actor 2: rotation must be a unit quaternion [x, y, z, w] (from Write down Leon's spot)",
        "actor 2: give a position or an offset, not both",
        "actor 3: name must be letters, digits or _ (not \"player\"): motions name the actor by it",
        "actor 3: puppet must be a definition's name in remod_puppets, e.g. \"luis\"",
        "actor 3: rotation must be a unit quaternion [x, y, z, w] (from Write down Leon's spot)",
        "actor 3: hides can only be \"partner\" (the real partner, hidden while it plays)",
        "actor 4: offset must be [right, up, forward], metres from the player",
        "actor 5 must be an object",
        "motion 2: actor must be \"player\" or one of the actors' names",
    };
    CHECK(p == want);

    // luis is remod's (runtime/puppets), rmc001 comes from the mod that adds it: only luis's definition goes along.
    const auto files = remod::cutscene_files(REMOD_SCHEMAS_DIR "/../spikes/actor_test.json", REMOD_RUNTIME_DIR);
    REQUIRE(files.size() == 3);
    CHECK(files[1].game_path == "reframework/data/remod_cutscenes/actor_test.json");
    CHECK(files[2].game_path == "reframework/data/remod_puppets/luis.json");
    CHECK(files[2].source == fs::path(REMOD_RUNTIME_DIR) / "puppets/luis.json");
    for (const char* def : {"luis", "ashley"}) {  // remod's definitions: game paths, as the runtime reads them
        const auto d = nlohmann::json::parse(test::read_file(fs::path(REMOD_RUNTIME_DIR) / "puppets" / (std::string(def) + ".json")));
        CHECK(d.at("skeleton").get<std::string>().starts_with("_Chainsaw/"));
        for (const auto& part : d.at("parts")) {
            CHECK(part.at("mesh").get<std::string>().ends_with(".mesh"));
            CHECK(part.at("material").get<std::string>().ends_with(".mdf2"));
        }
    }
}

TEST_CASE("The cutscene editor's model: read, change, write back, keeping what it doesn't edit") {
    test::TempDir tmp;
    const fs::path file = tmp.path / "cutscenes/meet.json";
    remod::Cutscene c = remod::read_cutscene(file);  // not there yet: a new one, named after it
    CHECK(c.name == "meet");
    CHECK(c.actors.empty());

    test::write_file(file, R"({"schema_version": 0, "name": "Meet", "length": 8, "start": {"key": "F11"},
        "camera": [{"t": 0, "position": [1, 2, 3], "rotation": [0, 0, 0, 1]}],
        "trigger": {"near": {"position": [1, 2, 3], "radius": 2}}, "mine": 7,
        "actors": [{"name": "luis", "puppet": "luis", "offset": [-0.8, 0, 2]},
                   {"name": "ash", "puppet": "ashley", "position": [4, 5, 6], "rotation": [0, 1, 0, 0], "hides": "partner"}],
        "motions": [{"t": 2, "actor": "luis", "bank": 1000, "motion": 160}],
        "subtitles": [{"t": 1, "until": 3, "text": "Hola"}], "fades": [{"t": 0, "until": 1, "from": 1, "to": 0}],
        "movies": [{"t": 7, "id": "mva000"}]})");
    c = remod::read_cutscene(file);
    CHECK(c.name == "Meet");
    CHECK(c.start_key == "F11");
    REQUIRE(c.camera.size() == 1);
    CHECK(c.camera[0].position == std::array<double, 3>{1, 2, 3});
    CHECK(c.camera[0].fov == 0);  // not set: the game's
    CHECK(c.trigger);
    REQUIRE(c.actors.size() == 2);
    CHECK_FALSE(c.actors[0].at_spot);
    CHECK(c.actors[0].offset == std::array<double, 3>{-0.8, 0, 2});
    CHECK(c.actors[1].at_spot);
    CHECK(c.actors[1].position == std::array<double, 3>{4, 5, 6});
    CHECK(c.actors[1].hides_partner);
    REQUIRE(c.motions.size() == 1);
    CHECK(c.motions[0].actor == "luis");
    CHECK(c.motions[0].blend == 10);  // the runtime's default when the file has none
    CHECK(c.movies[0].id == "mva000");

    c.motions.push_back({.t = 4, .bank = 1000, .motion = 161});  // on the player
    c.actors[0].hides_partner = true;
    remod::write_cutscene(file, c);
    CHECK(fs::exists(fs::path(file) += ".bak"));
    const std::string text = test::read_file(file);
    CHECK(remod::check_cutscene(text).empty());
    const auto j = nlohmann::json::parse(text);
    CHECK(j["camera"].size() == 1);
    CHECK_FALSE(j["camera"][0].contains("fov"));  // not written when not set
    CHECK_FALSE(j["camera"][0].contains("ease"));
    CHECK(j["trigger"]["near"]["radius"] == 2);
    CHECK(j["mine"] == 7);
    CHECK_FALSE(j["motions"][1].contains("actor"));  // the player: the default
    CHECK(remod::read_cutscene(file) == c);  // read back the same

    test::write_file(file, "{ broken");
    CHECK_THROWS_AS(remod::read_cutscene(file), remod::PackageError);
}

TEST_CASE("The cutscene's timeline: lanes, moving, stretching, adding and removing") {
    remod::Cutscene c{.length = 10};
    c.actors = {{.name = "luis", .puppet = "luis"}};
    c.camera = {{.t = 0}, {.t = 4, .ease = "cut"}, {.t = 8}};
    c.fades = {{.t = 0, .until = 1, .from = 1, .to = 0}};
    c.subtitles = {{.t = 2, .until = 4, .text = "Hola"}};
    c.motions = {{.t = 1, .actor = "luis", .bank = 1000, .motion = 160}, {.t = 3, .bank = 1, .motion = 2}};
    c.sounds = {{.t = 5, .id = "beeps"}};
    const auto lanes = remod::cutscene_lanes(c);
    REQUIRE(lanes.size() == 7);  // camera, fades, subtitles, Leon, luis, movies, sounds
    CHECK(lanes[0].items.size() == 3);
    CHECK(lanes[3].actor == "player");
    REQUIRE(lanes[3].items.size() == 1);
    CHECK(lanes[3].items[0].ref == remod::CutsceneItemRef{remod::CutsceneLane::Motion, 1});
    CHECK(lanes[4].actor == "luis");
    CHECK(lanes[4].items[0].ref.index == 0);
    CHECK(lanes[2].items[0].until == 4);
    CHECK(lanes[6].items[0].label == "beeps");

    // A span moved keeps inside the cutscene; stretched past the end, it stops there.
    remod::CutsceneItemRef sub{remod::CutsceneLane::Subtitle, 0};
    remod::set_item_time(c, sub, 9, 11);
    CHECK(c.subtitles[0].t == 9);
    CHECK(c.subtitles[0].until == 10);
    remod::set_item_time(c, sub, 12, 10);  // its start can't pass its end
    CHECK(c.subtitles[0].t < c.subtitles[0].until);
    remod::set_item_time(c, sub, -3);
    CHECK(c.subtitles[0].t == 0);
    // A camera key moved past the next one: the keys stay in time order and the ref follows it.
    remod::CutsceneItemRef key{remod::CutsceneLane::Camera, 0};
    remod::set_item_time(c, key, 6);
    CHECK(key.index == 1);
    CHECK(c.camera[1].t == 6);
    CHECK(c.camera[0].t == 4);
    CHECK(remod::check_cutscene(remod::cutscene_text(c)).size() == 0);

    // Adding: none for camera keys (recorded); a fade out late on; a motion on an actor.
    CHECK_FALSE(remod::add_item(c, remod::CutsceneLane::Camera, "", 2));
    const auto fade = remod::add_item(c, remod::CutsceneLane::Fade, "", 9.5);
    REQUIRE(fade);
    CHECK(c.fades[fade.index].to == 1);
    CHECK(c.fades[fade.index].until == 10);
    CHECK(c.fades[fade.index].t == 9.5);
    const auto motion = remod::add_item(c, remod::CutsceneLane::Motion, "luis", 7);
    CHECK(c.motions[motion.index].actor == "luis");
    remod::remove_item(c, motion);
    CHECK(c.motions.size() == 2);
    remod::remove_item(c, {remod::CutsceneLane::Sound, 5});  // no such item: nothing
    CHECK(c.sounds.size() == 1);
    CHECK(remod::check_cutscene(remod::cutscene_text(c)).empty());
}

TEST_CASE("The cutscene at a moment, as the runtime draws it") {
    remod::Cutscene c{.length = 10};
    c.actors = {{.name = "luis", .puppet = "luis"}};
    c.camera = {{.t = 0}, {.t = 4, .ease = "linear"}, {.t = 6, .ease = "cut"}};
    c.fades = {{.t = 0, .until = 2, .from = 1, .to = 0}};
    c.subtitles = {{.t = 1, .until = 3, .text = "Hola"}};
    c.motions = {{.t = 1, .actor = "luis", .bank = 1000, .motion = 160}, {.t = 2, .actor = "luis", .bank = 1000, .motion = 161}};
    auto f = remod::cutscene_frame(c, 1);
    CHECK(f.black == 0.5);
    CHECK(f.subtitle == "Hola");  // from its t
    CHECK(f.camera_from == 0);
    CHECK(f.camera_to == 1);
    CHECK(f.camera_progress == 0.25);  // linear
    REQUIRE(f.playing.size() == 2);
    CHECK(f.playing[0].motion == -1);  // Leon: nothing started
    CHECK(f.playing[1].motion == 0);
    f = remod::cutscene_frame(c, 3);
    CHECK(f.black == 0);
    CHECK(f.subtitle.empty());  // until is the end, not shown
    CHECK(f.playing[1].motion == 1);
    CHECK(f.playing[1].since == 1);
    f = remod::cutscene_frame(c, 5);
    CHECK(f.camera_to == 2);
    CHECK(f.camera_progress == 0);  // a cut holds the key before
    f = remod::cutscene_frame(c, 9);
    CHECK(f.camera_from == 2);
    CHECK(f.camera_to == 2);
    CHECK(remod::cutscene_frame(remod::Cutscene{}, 1).camera_from == -1);
}

TEST_CASE("Recording and trigger into the cutscene being edited; cutscene files told from other JSON") {
    test::TempDir tmp;
    remod::Cutscene c = remod::new_cutscene(tmp.path / "door.json");
    CHECK(c.name == "door");
    CHECK(remod::check_cutscene(remod::cutscene_text(c)).empty());
    CHECK_THROWS_AS(remod::apply_recording(c, tmp.path / "recording.json"), remod::PackageError);
    test::write_file(tmp.path / "recording.json", R"({"camera": [{"t": 0, "position": [1, 2, 3], "rotation": [0, 0, 0, 1],
        "fov": 70, "ease": "smooth"}, {"t": 7.5, "position": [1, 2, 4], "rotation": [0, 0, 0, 1], "fov": 70}]})");
    remod::apply_recording(c, tmp.path / "recording.json");
    REQUIRE(c.camera.size() == 2);
    CHECK(c.camera[0].fov == 70);
    CHECK(c.length == 8.5);  // a second past the last key
    test::write_file(tmp.path / "trigger.json", R"({"near": {"position": [1, 2, 3], "radius": 2}, "stage": "st40_100"})");
    remod::apply_trigger(c, tmp.path / "trigger.json");
    CHECK(c.trigger);
    const std::string text = remod::cutscene_text(c);
    CHECK(remod::check_cutscene(text).empty());
    CHECK(nlohmann::json::parse(text)["trigger"]["stage"] == "st40_100");

    remod::write_cutscene(tmp.path / "door.json", c);
    test::write_file(tmp.path / "graph.json", R"({"schema_version": 0, "length": 1, "nodes": []})");
    test::write_file(tmp.path / "junk.json", "{ nope");
    test::write_file(tmp.path / "big.json", R"({"schema_version": 0, "length": 1, "x": ")" + std::string(1 << 20, 'a') + "\"}");
    CHECK(remod::is_cutscene_file(tmp.path / "door.json"));
    CHECK_FALSE(remod::is_cutscene_file(tmp.path / "graph.json"));
    CHECK_FALSE(remod::is_cutscene_file(tmp.path / "junk.json"));
    CHECK_FALSE(remod::is_cutscene_file(tmp.path / "big.json"));
    CHECK_FALSE(remod::is_cutscene_file(tmp.path / "recording.json"));  // no schema_version or length
    for (const auto& e : remod::list_folder(tmp.path))
        CHECK((e.kind == remod::FileKind::Cutscene) == (e.name == "door.json"));
}

TEST_CASE("Animation files: a game cutscene's own animations as a bank of the cutscene's number") {
    const std::string file = "_Chainsaw/Event/cs/csa012/csa012_s00/chara/cha300_00/cha300_00.motlist";
    const std::vector<std::string> p = remod::check_cutscene(R"({
        "schema_version": 0, "length": 5,
        "actors": [{"name": "luis", "puppet": "luis"}],
        "animation_files": [
            {"actor": "luis", "file": ")" + file + R"(", "bank": 9000},
            {"file": "_Chainsaw/Event/cs/csa012/csa012_s00/chara/cha000_00/cha000_00.motlist", "bank": 9000},
            {"actor": "luis", "file": "natives/STM/_Chainsaw/x.motlist", "bank": 9000},
            {"actor": "nobody", "file": "x.motlist.663", "bank": -1},
            "nope"
        ]
    })");
    const std::vector<std::string> want{
        "animation file 3: file must be a game path ending in .motlist, without natives/STM and without the number after "
        "it, e.g. _Chainsaw/Event/cs/csa012/csa012_s00/chara/cha300_00/cha300_00.motlist",
        "animation file 3: bank 9000 is already another animation file's for luis",
        "animation file 4: actor must be \"player\" or one of the actors' names",
        "animation file 4: file must be a game path ending in .motlist, without natives/STM and without the number after "
        "it, e.g. _Chainsaw/Event/cs/csa012/csa012_s00/chara/cha300_00/cha300_00.motlist",
        "animation file 4: bank must be a whole number, 0 or more (e.g. 9000: one of your own, not the character's)",
        "animation file 5 must be an object",
    };
    CHECK(p == want);  // the player and luis may each have a bank 9000

    // The editor's model keeps them; renaming or removing an actor is the app's (it carries them along).
    remod::Cutscene c = remod::new_cutscene("x.json");
    c.actors.push_back({.name = "luis", .puppet = "luis"});
    c.animation_files = {{.actor = "luis", .file = file, .bank = 9000}, {.file = file, .bank = 9001}};
    test::TempDir tmp;
    remod::write_cutscene(tmp.path / "x.json", c);
    CHECK(remod::read_cutscene(tmp.path / "x.json") == c);
    CHECK(remod::check_cutscene(remod::cutscene_text(c)).empty());
    const auto j = nlohmann::json::parse(remod::cutscene_text(c));
    CHECK(j["animation_files"][1].contains("actor") == false);  // the player: left out, as in motions
    CHECK(remod::cutscene_text(remod::new_cutscene("y.json")).find("animation_files") == std::string::npos);
}

TEST_CASE("What the game wrote down for the editor: Leon's spot, the picked animation, the puppets") {
    test::TempDir tmp;
    const fs::path game = tmp.path / "game", data = game / "reframework/data";
    CHECK_FALSE(remod::read_spot(game));
    CHECK_FALSE(remod::read_picked_animation(game));
    test::write_file(data / "remod_cutscenes/spot.json", R"({"position": [1, 2, 3], "rotation": [0, 0.7071, 0, 0.7071]})");
    test::write_file(data / "remod_cutscenes/animation.json",
                     R"({"actor": "luis", "bank": 1000, "motion": 160, "name": "idle", "frame": 12})");
    const auto spot = remod::read_spot(game);
    REQUIRE(spot);
    CHECK(spot->position == std::array<double, 3>{1, 2, 3});
    const auto picked = remod::read_picked_animation(game);
    REQUIRE(picked);
    CHECK(picked->actor == "luis");
    CHECK(picked->motion == 160);
    CHECK(picked->frame == 12);
    CHECK(picked->file.empty());  // the character's own bank
    test::write_file(data / "remod_cutscenes/animation.json",
                     R"({"actor": "luis", "bank": 9000, "motion": 3, "file": "_Chainsaw/Event/cs/x.motlist"})");
    CHECK(remod::read_picked_animation(game)->file == "_Chainsaw/Event/cs/x.motlist");

    test::write_file(data / "remod_puppets/rmc001.json", "{}");
    test::write_file(data / "remod_puppets/luis.json", "{}");  // also remod's: listed once
    test::write_file(data / "remod_puppets/bad name.json", "{}");
    CHECK(remod::puppet_names(game, REMOD_RUNTIME_DIR) == std::vector<std::string>{"ashley", "luis", "rmc001"});
    CHECK(remod::puppet_names({}, REMOD_RUNTIME_DIR) == std::vector<std::string>{"ashley", "luis"});
}

TEST_CASE("colourize: every pixel the colour at its own lightness, alpha kept") {
    remod::Bgra img{3, 1, {255, 255, 255, 10, 0, 0, 0, 20, 128, 128, 128, 30}};  // white, black, grey (B G R A)
    remod::colourize(img, 220, 0.6f);
    CHECK(img.pixels[0] == 255);  // white stays white
    CHECK(img.pixels[2] == 255);
    CHECK(img.pixels[4] == 0);  // black stays black
    CHECK(img.pixels[8] > img.pixels[10]);  // grey turns blue: more blue than red
    CHECK(img.pixels[3] == 10);
    CHECK(img.pixels[11] == 30);
}

TEST_CASE("New character: Ashley's body at new paths, its outfit recoloured (set REMOD_GAME)") {
    char* v = nullptr;
    size_t n = 0;
    _dupenv_s(&v, &n, "REMOD_GAME");
    const std::string game = v ? v : "";
    std::free(v);
    if (game.empty()) SKIP("set REMOD_GAME to the extracted natives/STM");
    if (!fs::exists(fs::path(game) / "_chainsaw/character/ch/cha1/cha103/00")) SKIP("needs Ashley's body (cha103)");
    test::TempDir tmp;
    const remod::Profile profile = remod::load_profile(REMOD_PROFILES_DIR "/re4r.toml");
    remod::NativeConverter converter;
    remod::NewCharacter spec{.name = "rmc001", .definition = fs::path(REMOD_RUNTIME_DIR) / "puppets/ashley.json"};
    spec.skip = [](const std::string& f) { return f.find("Hand") != std::string::npos; };
    std::string report;
    const auto files = remod::make_new_character(spec, game, profile, converter, tmp.path, &report);
    INFO(report);
    CHECK_THAT(report, Catch::Matchers::ContainsSubstring("6 colour texture(s) recoloured (6 with their streaming copies)"));
    CHECK_THAT(report, Catch::Matchers::ContainsSubstring("left as they are: cha103_00_Hand_ALBD.tex"));
    CHECK_THAT(report, Catch::Matchers::ContainsSubstring("cha103_00c.mdf2 isn't in the game files"));  // the variant
    CHECK(files.size() == 6 * 2 + 3);  // the textures and their streaming copies, mesh, material, definition

    const auto def = nlohmann::json::parse(test::read_file(tmp.path / "reframework/data/remod_puppets/rmc001.json"));
    CHECK(def["name"] == "rmc001");
    CHECK(def["parts"][0]["mesh"] == "_Chainsaw/Character/ch/cha1/rmc001/00/cha103_00.mesh");
    CHECK(def["parts"][0]["material"] == "_Chainsaw/Character/ch/cha1/rmc001/00/cha103_00.mdf2");
    CHECK(def["parts"][1]["mesh"] == "_Chainsaw/Character/ch/cha1/cha100/10/cha100_10.mesh");  // the head: the game's

    const auto utf16 = [](const std::string& s) {
        std::string out;
        for (const char c : s) out += {c, '\0'};
        return out;
    };
    const std::string mdf2 = test::read_file(tmp.path / "_Chainsaw/Character/ch/cha1/rmc001/00/cha103_00.mdf2.32");
    CHECK(mdf2.size() == fs::file_size(fs::path(game) / "_chainsaw/character/ch/cha1/cha103/00/cha103_00.mdf2.32"));
    CHECK(mdf2.find(utf16("rmc001/00/chc103_00_Upper_ALBD.tex")) != std::string::npos);
    CHECK(mdf2.find(utf16("cha103/00/chc103_00_Upper_ALBD.tex")) == std::string::npos);
    CHECK(mdf2.find(utf16("cha103/00/cha103_00_Hand_ALBD.tex")) != std::string::npos);  // left as it is
    CHECK(mdf2.find(utf16("cha103/00/chc103_00_Upper_NRMR.tex")) != std::string::npos);  // data: the game's

    // A recoloured texture keeps the original's size, format and mips.
    const fs::path upper = tmp.path / "_Chainsaw/Character/ch/cha1/rmc001/00/chc103_00_Upper_ALBD.tex.143221013";
    const auto a = remod::read_tex_meta(upper, profile);
    const auto b = remod::read_tex_meta(fs::path(game) / "_chainsaw/character/ch/cha1/cha103/00/chc103_00_upper_albd.tex.143221013", profile);
    CHECK((a.width == b.width && a.height == b.height && a.format == b.format && a.mip_count == b.mip_count));

    spec.name = "rmc01";
    CHECK_THROWS_WITH(remod::make_new_character(spec, game, profile, converter, tmp.path / "x"),
                      Catch::Matchers::ContainsSubstring("must be 6 letters or digits"));
    spec.name = "rmc001";
    spec.part = "tail";
    CHECK_THROWS_WITH(remod::make_new_character(spec, game, profile, converter, tmp.path / "y"),
                      Catch::Matchers::ContainsSubstring("its parts: body, head, hair, ac2100_10"));
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

TEST_CASE("Triggers: checked, and Make a trigger here's file put into a cutscene") {
    const auto with = [](const std::string& trigger) {
        return remod::check_cutscene(R"({"schema_version": 0, "length": 2, "trigger": )" + trigger + "}");
    };
    CHECK(with(R"({"near": {"position": [1, 2, 3], "radius": 2}, "chapter": "chap01_01", "delay": 0.5, "once": false})").empty());
    CHECK(with(R"({"stage": "st40_100"})").empty());
    CHECK(with("{}") ==
          std::vector<std::string>{"trigger needs a condition: near, talk, flags, after, stage, area, location or chapter"});
    CHECK(with(R"({"near": {"position": [1, 2], "radius": 0}, "chapter": 3, "delay": -1, "once": "yes"})") ==
          std::vector<std::string>{
              "trigger.near.position must be [x, y, z] (from Make a trigger here, never typed)",
              "trigger.near.radius must be a number of metres above 0",
              "trigger.chapter must be the game's name for it (as the menu's Now line shows)",
              "trigger.delay must be seconds, 0 or more",
              "trigger.once must be true (once per save), \"session\" (once each time the game runs) or false "
              "(every time; a talk trigger's default)"});
    // Talking to a character, story flags, after one of the game's movies or cutscenes.
    CHECK(with(R"({"talk": {"npc": "ch3_a8z0", "key": "G", "prompt": "Ask about the castle", "radius": 3},
                   "flags": ["Ch1f0z0LuisArrivedDemoAfter", "!DifficultyHard"], "once": "session"})").empty());
    CHECK(with(R"({"talk": {"npc": "ch3_a8z0"}})").empty());  // key, prompt, radius: defaults
    CHECK(with(R"({"after": {"event": "csa012"}})").empty());
    CHECK(with(R"({"after": {"movie": "mva000"}, "flags": ["X"]})").empty());
    CHECK(with(R"({"talk": {"npc": "a b", "key": "F10", "prompt": "", "radius": 0}, "flags": ["!", 3], "after": {"film": "x"}})") ==
          std::vector<std::string>{
              "trigger.talk.npc must be the character's kind, as the menu's Characters near Leon shows (e.g. \"ch3_a8z0\")",
              "trigger.talk.key must be a capital letter A-Z or F1-F12 (not F10)",
              "trigger.talk.prompt must be text, e.g. \"Talk\"",
              "trigger.talk.radius must be a number of metres above 0",
              "trigger.flags must be a list of story flag names, \"!\" before one that must be off",
              "trigger.after must be {\"movie\": id} or {\"event\": id}, the game's names (e.g. \"mva000\", \"csa012\")"});
    CHECK(with(R"({"talk": {"npc": "x", "key": "g"}, "flags": []})").size() == 2);  // lower-case key; an empty list
    CHECK(with(R"({"talk": {"npc": "x", "key": "F123456789012"}})").size() == 1);   // refused, never thrown
    // remod's own flags: read as "remod:name", set by a cutscene's sets_flags ("!" turns one off).
    CHECK(with(R"({"flags": ["remod:meet_luis", "!remod:chose_to_help"]})").empty());
    const auto sets = [](const std::string& s) {
        return remod::check_cutscene(R"({"schema_version": 0, "length": 2, "sets_flags": )" + s + "}");
    };
    CHECK(sets(R"(["chose_to_help", "!met_luis"])").empty());
    for (const char* bad : {R"("x")", R"(["a b"])", R"(["!"])", R"([3])", R"(["remod:x"])"})
        CHECK(sets(bad) == std::vector<std::string>{
                               "sets_flags must be a list of flag names (letters, digits, _), \"!\" before one to turn off"});

    test::TempDir tmp;
    const fs::path trig = tmp.path / "game/reframework/data/remod_cutscenes/trigger.json";
    const fs::path cut = tmp.path / "cutscenes/door.json";
    CHECK_THROWS_WITH(remod::use_trigger(cut, trig), Catch::Matchers::ContainsSubstring("Make a trigger here"));
    test::write_file(trig, R"({"near": {"position": [186.6, 28.0, 42.2], "radius": 2}, "chapter": "chap01_01", "stage": "st40_100"})");
    remod::use_trigger(cut, trig);  // a new cutscene with it
    auto c = nlohmann::json::parse(test::read_file(cut));
    CHECK(c["trigger"]["stage"] == "st40_100");
    CHECK(remod::check_cutscene(test::read_file(cut)).empty());
    c["subtitles"] = {{{"t", 1}, {"until", 2}, {"text", "Hello"}}};  // an existing one keeps the rest
    test::write_file(cut, c.dump());
    test::write_file(trig, R"({"near": {"position": [0, 0, 0], "radius": 2}})");
    remod::use_trigger(cut, trig);
    const auto again = nlohmann::json::parse(test::read_file(cut));
    CHECK(again["subtitles"][0]["text"] == "Hello");
    CHECK_FALSE(again["trigger"].contains("stage"));  // replaced, not merged
    CHECK(fs::exists(fs::path(cut) += ".bak"));

    // A talk trigger made in game has no spot: still the runtime's. An empty one (Start a new trigger) isn't yet.
    test::write_file(trig, R"({"talk": {"npc": "ch3_a8z0", "key": "G", "prompt": "Talk", "radius": 2.5}})");
    remod::use_trigger(cut, trig);
    CHECK(nlohmann::json::parse(test::read_file(cut))["trigger"]["talk"]["npc"] == "ch3_a8z0");
    test::write_file(trig, "{}");
    CHECK_THROWS_WITH(remod::use_trigger(cut, trig), Catch::Matchers::ContainsSubstring("no conditions yet"));
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
