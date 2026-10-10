// GUI layouts (core gui.*): spare entries appended to the merchant's first menu, nothing of the game's file moved.
#include "cutscene.hpp"
#include "gui.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <fstream>

using Catch::Matchers::ContainsSubstring;

namespace {
std::string game_dir() {
    char* v = nullptr;
    size_t n = 0;
    _dupenv_s(&v, &n, "REMOD_GAME");
    std::string s = v ? v : "";
    std::free(v);
    return s;
}
}  // namespace

TEST_CASE("add_gui_entries refuses what isn't an RE4R GUI layout") {
    CHECK_THROWS_WITH(remod::add_gui_entries(std::string(200, 'x'), "a", {"b"}), ContainsSubstring("no GUIR header"));
    CHECK_THROWS_WITH(remod::gui_list("GUIR", "a"), ContainsSubstring("no GUIR header"));
}

TEST_CASE("add_gui_entries adds spares to the merchant's menu (REMOD_GAME)") {
    const std::string game = game_dir();
    if (game.empty()) SKIP("set REMOD_GAME to the extracted natives/STM");
    std::ifstream in(std::filesystem::path(game) / remod::kMerchantMenuGui, std::ios::binary);
    REQUIRE(in);
    const std::string gui{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

    const auto before = remod::gui_list(gui, remod::kMerchantMenuLast);
    REQUIRE(before.size() == 5);  // hitarea, si_menu_0..3
    CHECK(before[4].name == "si_menu_3");
    CHECK(before[4].y == 150);
    CHECK(before[4].priority == 13);

    const std::vector<std::string> names{"si_menu_4", "si_menu_5", "si_menu_6", "si_menu_7"};
    const std::string out = remod::add_gui_entries(gui, remod::kMerchantMenuLast, names);
    CHECK(out.size() > gui.size());
    // Only the list's offset in its container changes; everything else of the game's is as it was.
    size_t changed = 0;
    for (size_t i = 0; i < gui.size(); ++i) changed += gui[i] != out[i];
    CHECK(changed > 0);
    CHECK(changed <= 8);

    const auto after = remod::gui_list(out, "si_menu_7");
    REQUIRE(after.size() == 9);
    for (int i = 0; i < 4; ++i) {
        CHECK(after[5 + i].name == names[i]);
        CHECK(after[5 + i].y == 150 + 80 * (i + 1));
        CHECK(after[5 + i].priority == 14 + i);
    }
    CHECK(remod::add_gui_entries(gui, remod::kMerchantMenuLast, names) == out);  // the same file every time
    CHECK_THROWS_WITH(remod::add_gui_entries(out, "si_menu_3", {"si_menu_4"}), ContainsSubstring("already"));
    CHECK_THROWS_WITH(remod::add_gui_entries(gui, "no_such", {"x"}), ContainsSubstring("no element named no_such"));
    // What a Cutscene block with a merchant topic adds: made once, at the menu's game path.
    const auto out_dir = std::filesystem::temp_directory_path() / "remod_test_gui";
    std::filesystem::remove_all(out_dir);
    const auto layout = remod::merchant_menu_layout(game, out_dir, true);
    CHECK(layout.game_path == remod::kMerchantMenuGui);
    std::ifstream made(layout.source, std::ios::binary);
    CHECK(std::string{std::istreambuf_iterator<char>(made), std::istreambuf_iterator<char>()} == out);
    CHECK(remod::merchant_menu_layout(game, out_dir, false).source == layout.source);
    made.close();
    std::filesystem::remove_all(out_dir);

    std::string other = gui;
    other[0] = char(other[0] + 1);  // version 540035
    CHECK_THROWS_WITH(remod::gui_list(other, "si_menu_3"), ContainsSubstring("not RE4R's"));
}
