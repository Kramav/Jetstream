#include "settings.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("settings round-trip, and bad files fall back to defaults") {
    test::TempDir tmp;
    const auto file = tmp.path / "sub" / "settings.json";  // folder is created on save
    CHECK(remod::load_settings(file) == remod::Settings{});

    const remod::Settings s{.graph_path = "E:/mods/my graph.json",
                            .noesis_path = "D:/Noesis/Noesis64.exe",
                            .show_help = false,
                            .show_descriptions = false,
                            .pinned_folders = {"D:/mods", "E:/work/natives/stm/streaming"}};
    remod::save_settings(s, file);
    CHECK(remod::load_settings(file) == s);

    test::write_file(file, "{ broken");
    CHECK(remod::load_settings(file) == remod::Settings{});
    test::write_file(file, "[1, 2]");
    CHECK(remod::load_settings(file) == remod::Settings{});

    CHECK(remod::default_settings_path().filename() == "settings.json");
}

TEST_CASE("settings keep a set per game; an old file's set is RE4R's") {
    test::TempDir tmp;
    const auto file = tmp.path / "settings.json";
    // From before games: one set at the top.
    test::write_file(file, R"({"graph_path": "a.json", "game_dir": "D:/RE4", "pinned_folders": ["D:/x"], "build_mode": true})");
    remod::Settings s = remod::load_settings(file);
    CHECK(s.game == "re4r");
    CHECK(s.game_dir == "D:/RE4");
    CHECK(s.build_mode);

    remod::switch_game(s, "re9");  // a new game starts empty; RE4R's set is kept aside
    CHECK(s.game == "re9");
    CHECK(s.game_dir.empty());
    CHECK(s.pinned_folders.empty());
    s.game_dir = "C:/RE9";
    CHECK(remod::for_game(s, "re4r").graph_path == "a.json");
    CHECK(s.game_dir == "C:/RE9");  // for_game leaves the settings as they were

    remod::save_settings(s, file);
    const remod::Settings back = remod::load_settings(file);
    CHECK(back == s);
    remod::Settings re4 = back;
    remod::switch_game(re4, "re4r");
    CHECK(re4.game_dir == "D:/RE4");
    CHECK(re4.pinned_folders == std::vector<std::string>{"D:/x"});
    CHECK(re4.other_games.at("re9").game_dir == "C:/RE9");
}
