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
