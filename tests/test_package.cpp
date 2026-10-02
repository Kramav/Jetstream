#include "package.hpp"
#include "process.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "helpers.hpp"

#include <sstream>

using Catch::Matchers::ContainsSubstring;
using remod::PackageError;
using test::read_file;
using test::TempDir;
using test::write_file;
namespace fs = std::filesystem;

namespace {

const remod::Profile kProfile{.id = "re4r",
                              .name = "Resident Evil 4 (2023)",
                              .tex_suffix = "143221013",
                              .natives_root = "natives/STM",
                              .packaging = {"loose_archive"},
                              .pak_script = "Create-PAK-2023.bat",
                              .noesis_export = "TBD",
                              .file_list = "TBD"};

}  // namespace

TEST_CASE("game path maps to <mod>/<natives_root>/<game path>") {
    CHECK(remod::package_path(kProfile, "MyMod", "_chainsaw/ui/load.tex.143221013") ==
          fs::path("MyMod/natives/STM/_chainsaw/ui/load.tex.143221013").lexically_normal());
    // Non-texture assets are not suffix-checked.
    CHECK(remod::package_path(kProfile, "MyMod", "a/b.bin") == fs::path("MyMod/natives/STM/a/b.bin").lexically_normal());
}

TEST_CASE("path mapping rejects unsafe paths, bad mod names and wrong tex suffix") {
    CHECK_THROWS_AS(remod::package_path(kProfile, "M", "C:/x.tex.143221013"), PackageError);
    CHECK_THROWS_AS(remod::package_path(kProfile, "M", "/x.tex.143221013"), PackageError);
    CHECK_THROWS_AS(remod::package_path(kProfile, "M", "../x.tex.143221013"), PackageError);
    CHECK_THROWS_AS(remod::package_path(kProfile, "M", "a/../../x.tex.143221013"), PackageError);
    CHECK_THROWS_AS(remod::package_path(kProfile, "M", ""), PackageError);
    for (const char* bad : {"", ".", "..", "a/b", "a\\b", "a:b", "a?", "a*", "a<", "a|", "a\"", "trailing.", "trailing "})
        CHECK_THROWS_AS(remod::package_path(kProfile, bad, "x.tex.143221013"), PackageError);
    CHECK_THROWS_WITH(remod::package_path(kProfile, "M", "ui/x.tex.999"), ContainsSubstring(".tex.143221013"));
}

TEST_CASE("a plain .tex game path gets the profile's suffix") {
    CHECK(remod::package_path(kProfile, "M", "ui/x.tex") == fs::path("M/natives/STM/ui/x.tex.143221013").lexically_normal());
    CHECK(remod::package_path(kProfile, "M", "ui/X.TEX") == fs::path("M/natives/STM/ui/X.TEX.143221013").lexically_normal());
}

TEST_CASE("modinfo.ini contents") {
    CHECK(remod::write_modinfo({.name = "My Mod",
                                .version = "1.0",
                                .description = "Line one\nLine two\r\nLine three",
                                .author = "me",
                                .screenshot = "shot.png"}) ==
          "name=My Mod\r\nversion=1.0\r\ndescription=Line one\\nLine two\\nLine three\r\nauthor=me\r\nscreenshot=shot.png\r\n");
    CHECK(remod::write_modinfo({.name = "Only"}) == "name=Only\r\n");
    CHECK(remod::write_modinfo({}).empty());
    CHECK_THROWS_WITH(remod::write_modinfo({.name = "a\nb"}), ContainsSubstring("'name'"));
    CHECK_THROWS_AS(remod::write_modinfo({.author = "a\rb"}), PackageError);
}

TEST_CASE("build_package writes the Fluffy folder layout") {
    TempDir tmp;
    const fs::path tex = tmp.path / "in.tex.143221013";
    const fs::path shot = tmp.path / "Shot.PNG";
    write_file(tex, std::string("TEX\0\x01\xff", 6));
    write_file(shot, "png-bytes");

    remod::PackageSpec spec{.mod_name = "MyMod",
                            .out_dir = tmp.path / "out",
                            .info = {.name = "My Mod", .version = "1"},
                            .files = {{tex, "_chainsaw/ui/load.tex.143221013"}},
                            .screenshot = shot};
    const fs::path root = remod::build_package(kProfile, spec);

    CHECK(root == fs::absolute(tmp.path / "out" / "MyMod"));
    CHECK(read_file(root / "natives/STM/_chainsaw/ui/load.tex.143221013") == std::string("TEX\0\x01\xff", 6));
    CHECK(read_file(root / "Shot.PNG") == "png-bytes");
    CHECK(read_file(root / "modinfo.ini") == "name=My Mod\r\nversion=1\r\nscreenshot=Shot.PNG\r\n");

    size_t entries = 0;
    for ([[maybe_unused]] const auto& e : fs::recursive_directory_iterator(root)) ++entries;
    CHECK(entries == 7);  // natives, STM, _chainsaw, ui, tex, screenshot, modinfo.ini

    // Zip holds the mod folder at its root (CLAUDE.md §9). List it with the same system tar.
    const fs::path zip = tmp.path / "out" / "MyMod.zip";
    REQUIRE(fs::is_regular_file(zip));
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    const auto listing = remod::run_process(fs::path(sys) / L"tar.exe", {L"-tf", zip.wstring()}, std::chrono::seconds(30));
    REQUIRE(listing.exit_code == 0);
    CHECK_THAT(listing.output, ContainsSubstring("MyMod/modinfo.ini") && ContainsSubstring("MyMod/Shot.PNG") &&
                                   ContainsSubstring("MyMod/natives/STM/_chainsaw/ui/load.tex.143221013"));
    std::istringstream lines(listing.output);
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) CHECK(line.starts_with("MyMod/"));

    SECTION("refuses to overwrite an existing package") {
        CHECK_THROWS_WITH(remod::build_package(kProfile, spec), ContainsSubstring("already exists"));
        CHECK(read_file(root / "modinfo.ini") == "name=My Mod\r\nversion=1\r\nscreenshot=Shot.PNG\r\n");
    }
    SECTION("refuses when only the zip exists, and writes nothing") {
        fs::remove_all(root);
        CHECK_THROWS_WITH(remod::build_package(kProfile, spec), ContainsSubstring("zip already exists"));
        CHECK_FALSE(fs::exists(root));
    }
}

TEST_CASE("replace overwrites only a previous build of the same mod") {
    TempDir tmp;
    write_file(tmp.path / "in.tex.143221013", "v1");
    remod::PackageSpec spec{.mod_name = "M",
                            .out_dir = tmp.path / "out",
                            .info = {.name = "M"},
                            .files = {{tmp.path / "in.tex.143221013", "a.tex.143221013"}}};
    remod::build_package(kProfile, spec);

    write_file(tmp.path / "in.tex.143221013", "v2");
    CHECK_THROWS_WITH(remod::build_package(kProfile, spec), ContainsSubstring("turn on 'Replace existing'"));
    spec.replace = true;
    remod::build_package(kProfile, spec);
    CHECK(read_file(tmp.path / "out/M/natives/STM/a.tex.143221013") == "v2");

    SECTION("the same build again writes nothing; a change rebuilds it") {
        const auto zip_time = fs::last_write_time(tmp.path / "out/M.zip");
        bool unchanged = false;
        remod::build_package(kProfile, spec, &unchanged);
        CHECK(unchanged);
        CHECK(fs::last_write_time(tmp.path / "out/M.zip") == zip_time);
        write_file(tmp.path / "in.tex.143221013", "v3");
        remod::build_package(kProfile, spec, &unchanged);
        CHECK_FALSE(unchanged);
        CHECK(read_file(tmp.path / "out/M/natives/STM/a.tex.143221013") == "v3");
    }
    SECTION("a folder that isn't this mod's build is left alone") {
        fs::remove_all(tmp.path / "out");
        write_file(tmp.path / "out/M/precious.txt", "keep me");
        CHECK_THROWS_WITH(remod::build_package(kProfile, spec), ContainsSubstring("isn't a build of mod 'M'"));
        CHECK(read_file(tmp.path / "out/M/precious.txt") == "keep me");
        write_file(tmp.path / "out/M/modinfo.ini", "name=Some Other Mod\r\n");  // another mod's folder
        CHECK_THROWS_WITH(remod::build_package(kProfile, spec), ContainsSubstring("isn't a build of mod 'M'"));
        CHECK(fs::exists(tmp.path / "out/M/precious.txt"));
    }
    SECTION("a zip with other contents is left alone") {
        fs::remove_all(tmp.path / "out/M");
        write_file(tmp.path / "other/Other/file.txt", "x");
        fs::remove(tmp.path / "out/M.zip");
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        REQUIRE(remod::run_process(fs::path(sys) / L"tar.exe",
                                   {L"-a", L"-c", L"-f", (tmp.path / "out/M.zip").wstring(), L"-C",
                                    (tmp.path / "other").wstring(), L"Other"},
                                   std::chrono::seconds(30))
                    .exit_code == 0);
        const auto before = fs::file_size(tmp.path / "out/M.zip");
        CHECK_THROWS_WITH(remod::build_package(kProfile, spec), ContainsSubstring("isn't a build of mod 'M'"));
        CHECK(fs::file_size(tmp.path / "out/M.zip") == before);
    }
}

TEST_CASE("build_package without zip writes only the folder") {
    TempDir tmp;
    write_file(tmp.path / "in.tex.143221013", "x");
    remod::build_package(kProfile, {.mod_name = "M",
                                    .out_dir = tmp.path,
                                    .files = {{tmp.path / "in.tex.143221013", "a.tex.143221013"}},
                                    .zip = false});
    CHECK(fs::is_regular_file(tmp.path / "M" / "natives/STM/a.tex.143221013"));
    CHECK_FALSE(fs::exists(tmp.path / "M.zip"));
}

TEST_CASE("build_package validates before writing anything") {
    TempDir tmp;
    const fs::path tex = tmp.path / "in.tex.143221013";
    write_file(tex, "x");
    const fs::path out = tmp.path / "out";

    auto spec = [&](remod::PackageSpec s) {
        s.mod_name = "M";
        s.out_dir = out;
        return s;
    };
    CHECK_THROWS_WITH(remod::build_package(kProfile, spec({})), ContainsSubstring("no files"));
    CHECK_THROWS_WITH(remod::build_package(kProfile, spec({.files = {{tmp.path / "missing", "a.tex.143221013"}}})),
                      ContainsSubstring("not found"));
    CHECK_THROWS_WITH(remod::build_package(kProfile, spec({.files = {{tex, "a.tex.143221013"}, {tex, "a.tex.143221013"}}})),
                      ContainsSubstring("twice"));
    write_file(tmp.path / "shot.gif", "g");
    CHECK_THROWS_WITH(remod::build_package(kProfile, spec({.files = {{tex, "a.tex.143221013"}}, .screenshot = tmp.path / "shot.gif"})),
                      ContainsSubstring("jpg, png, tga or bmp"));
    CHECK_THROWS_WITH(remod::build_package(kProfile, spec({.files = {{tex, "a.tex.143221013"}},
                                                           .screenshot = tmp.path / "nope.png"})),
                      ContainsSubstring("screenshot not found"));
    CHECK_THROWS_WITH(remod::build_package(kProfile, spec({.files = {{tex, std::string(300, 'a') + ".tex.143221013"}}})),
                      ContainsSubstring("exceeds"));
    CHECK_FALSE(fs::exists(out));
}
