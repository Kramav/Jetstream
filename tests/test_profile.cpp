#include "profile.hpp"

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
