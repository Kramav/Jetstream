#include "setup.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

const remod::Profile& re4r() {
    static const remod::Profile p = remod::load_profile(REMOD_PROFILES_DIR "/re4r.toml");
    return p;
}

}  // namespace

TEST_CASE("check_noesis explains what's missing") {
    test::TempDir tmp;
    const fs::path exe = tmp.path / "Noesis" / "Noesis64.exe";
    CHECK_THAT(remod::check_noesis({}).message, ContainsSubstring("winget install"));
    CHECK_THAT(remod::check_noesis(exe).message, ContainsSubstring("not found"));

    test::write_file(exe, "fake exe");
    const auto no_plugin = remod::check_noesis(exe);
    CHECK_FALSE(no_plugin.ok);
    CHECK_THAT(no_plugin.message, ContainsSubstring("fmt_RE_MESH.py") && ContainsSubstring("github.com"));

    test::write_file(tmp.path / "Noesis/plugins/python/fmt_RE_MESH.py", "# plugin");
    CHECK(remod::check_noesis(exe).ok);
}

TEST_CASE("find_noesis prefers the saved path") {
    test::TempDir tmp;
    const fs::path exe = tmp.path / "Noesis64.exe";
    test::write_file(exe, "fake exe");
    CHECK(remod::find_noesis(exe.string()) == exe);
    CHECK(remod::find_noesis((tmp.path / "gone.exe").string()) != tmp.path / "gone.exe");  // falls through
}

TEST_CASE("game_files_dir reads the RE plugin's NativesPath file") {
    test::TempDir tmp;
    const fs::path exe = tmp.path / "Noesis" / "Noesis64.exe";
    test::write_file(exe, "fake exe");
    CHECK(re4r().noesis_game == "RE4");
    CHECK(remod::game_files_dir(exe, re4r()).empty());  // no file yet

    const fs::path natives = tmp.path / "REtool/RE4/re_chunk_000/natives/stm";
    fs::create_directories(natives);
    test::write_file(tmp.path / "Noesis/plugins/python/RE4NativesPath.txt", natives.string() + "\\\r\n");
    CHECK(fs::equivalent(remod::game_files_dir(exe, re4r()), natives));

    test::write_file(tmp.path / "Noesis/plugins/python/RE4NativesPath.txt", (tmp.path / "missing").string());
    CHECK(remod::game_files_dir(exe, re4r()).empty());
}
