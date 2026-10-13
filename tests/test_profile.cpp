#include "profile.hpp"
#include "nodes.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using Catch::Matchers::ContainsSubstring;
using remod::ProfileError;

namespace {

const std::string kValid = R"(
[game]
id            = "x"
name          = "X"
tex_suffix    = "1"
natives_root  = "natives/STM"
packaging     = ["pak"]
pak_script    = "s.bat"
noesis_export = "TBD"
file_list     = "TBD"
noesis_game   = "X"
)";

std::string replace(std::string text, const std::string& from, const std::string& to) {
    text.replace(text.find(from), from.size(), to);
    return text;
}

}  // namespace

TEST_CASE("re4r profile loads with the CLAUDE.md section 5 values") {
    const auto p = remod::load_profile(REMOD_PROFILES_DIR "/re4r.toml");
    CHECK(p.id == "re4r");
    CHECK(p.name == "Resident Evil 4 (2023)");
    CHECK(p.tex_suffix == "143221013");
    CHECK(p.natives_root == "natives/STM");
    CHECK(p.packaging == std::vector<std::string>{"loose_archive", "pak"});
    CHECK(p.pak_script == "Create-PAK-2023.bat");
    CHECK(p.noesis_export == "-b");
    CHECK(p.file_list == "TBD");
    CHECK(p.noesis_game == "RE4");
    CHECK(p.unresolved() == std::vector<std::string>{"file_list"});
}

TEST_CASE("valid minimal profile parses") {
    CHECK(remod::parse_profile(kValid).id == "x");
}

TEST_CASE("profile validation rejects bad input") {
    CHECK_THROWS_WITH(remod::parse_profile(replace(kValid, "\nid ", "\nxid ")),
                      ContainsSubstring("'id' must be a non-empty string"));
    CHECK_THROWS_WITH(remod::parse_profile(replace(kValid, "\"X\"", "\"\"")),
                      ContainsSubstring("'name' must be a non-empty string"));
    CHECK_THROWS_WITH(remod::parse_profile(kValid + "typo_key = \"1\"\n"), ContainsSubstring("unknown key 'typo_key'"));
    CHECK_THROWS_WITH(remod::parse_profile(replace(kValid, "[\"pak\"]", "[\"zip\"]")),
                      ContainsSubstring("'packaging' values"));
    CHECK_THROWS_WITH(remod::parse_profile(replace(kValid, "[\"pak\"]", "[]")),
                      ContainsSubstring("'packaging' must be a non-empty array"));
    CHECK_THROWS_WITH(remod::parse_profile(replace(kValid, "natives/STM", "C:/natives/STM")),
                      ContainsSubstring("'natives_root'"));
    CHECK_THROWS_WITH(remod::parse_profile(replace(kValid, "natives/STM", "../natives")),
                      ContainsSubstring("'natives_root'"));
    CHECK_THROWS_WITH(remod::parse_profile("id = \"x\""), ContainsSubstring("missing [game]"));
    CHECK_THROWS_AS(remod::parse_profile("[game"), ProfileError);
    CHECK_THROWS_AS(remod::load_profile("does/not/exist.toml"), ProfileError);
}

TEST_CASE("profile errors list every problem at once") {
    const std::string bad = replace(replace(kValid, "\"X\"", "\"\""), "[\"pak\"]", "[]");
    CHECK_THROWS_WITH(remod::parse_profile(bad), ContainsSubstring("'name'") && ContainsSubstring("'packaging'"));
}

TEST_CASE("the Noesis keys are optional; empty, they're still refused") {
    const auto p = remod::parse_profile(replace(replace(kValid, "noesis_export = \"TBD\"", ""), "noesis_game   = \"X\"", ""));
    CHECK(p.noesis_export.empty());
    CHECK(p.noesis_game.empty());
    CHECK_THROWS_WITH(remod::parse_profile(replace(kValid, "noesis_game   = \"X\"", "noesis_game = \"\"")),
                      ContainsSubstring("noesis_game"));
}

TEST_CASE("profiles say what works for each game; blocks needing more aren't available") {
    const auto p = remod::parse_profile(kValid + "features = [\"textures\"]\nnotes = [\"Meshes not yet.\"]\nreframework = \"x9\"\n");
    CHECK(p.has("textures"));
    CHECK_FALSE(p.has("meshes"));
    CHECK(p.notes == std::vector<std::string>{"Meshes not yet."});
    CHECK(p.reframework == "x9");
    CHECK_THROWS_WITH(remod::parse_profile(kValid + "features = [\"wings\"]\n"), ContainsSubstring("'features' values"));
    CHECK_THROWS_WITH(remod::parse_profile(kValid + "notes = \"one\"\n"), ContainsSubstring("'notes' must be a list"));

    CHECK(remod::block_available("LoadTex", p));
    CHECK_FALSE(remod::block_available("PartTexture", p));  // meshes too
    CHECK(remod::block_available("AdjustColour", p));       // images: any game
    CHECK(remod::block_available("LuaScript", remod::parse_profile(kValid)));
    CHECK_FALSE(remod::block_available("Cutscene", p));

    // The shipped games, in release order; RE4R can do everything built so far, RE9 nothing of its own yet.
    std::vector<std::string> ids;
    for (const remod::Profile& g : remod::load_profiles(REMOD_PROFILES_DIR)) ids.push_back(g.id);
    CHECK(ids == std::vector<std::string>{"re2r", "re3r", "re4r", "re7", "re8", "re9"});
    const auto re4r = remod::load_profile_by_id(REMOD_PROFILES_DIR, "re4r");
    for (std::string_view f : remod::kFeatures) CHECK(re4r.has(f));
    CHECK(remod::load_profile_by_id(REMOD_PROFILES_DIR, "re9").features.empty());
}
