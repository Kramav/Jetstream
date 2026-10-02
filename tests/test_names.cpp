#include "browse.hpp"
#include "names.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("nicknames: set, look up ignoring case, save and load") {
    test::TempDir tmp;
    const auto file = tmp.path / "names" / "re4r.json";  // folder is created on save
    CHECK(remod::load_names(file) == remod::Nicknames{});  // no file yet: none

    remod::Nicknames n;
    remod::set_nickname(n, "_chainsaw/character/ch/cha0/cha000/", "  Leon ");  // trimmed, trailing '/' ignored
    remod::set_nickname(n, "_chainsaw/ui/ui3200/tex/cs_ui3210_main_iam.tex.143221013", "Quest file");
    CHECK(remod::nickname(n, "_Chainsaw/Character/ch/cha0/CHA000") == "Leon");
    CHECK(remod::nickname(n, "_chainsaw/character") == "");
    CHECK(remod::nickname_above(n, "_chainsaw/character/ch/cha0/cha000/00/cha000_00.mesh.221108797") == "Leon");
    CHECK(remod::nickname_above(n, "_chainsaw/character/ch/cha0/cha000") == "");  // its own isn't "above"

    remod::save_names(n, file);
    CHECK(remod::load_names(file) == n);

    remod::set_nickname(n, "_chainsaw/character/ch/cha0/cha000", "");  // empty removes it
    CHECK(remod::nickname(n, "_chainsaw/character/ch/cha0/cha000") == "");

    // A file it can't read is an error, not "no nicknames": saving over it would lose them.
    test::write_file(file, "{ broken");
    CHECK_THROWS(remod::load_names(file));
    test::write_file(file, R"({"a/b": 3})");
    CHECK_THROWS(remod::load_names(file));
}

TEST_CASE("search finds paths by their own and their folders' nicknames") {
    const std::vector<std::string> paths{"_chainsaw/character/ch/cha0/cha000/00/cha000_00_body_albd.tex.143221013",
                                         "_chainsaw/character/ch/cha0/cha000/00/cha000_00_body_nrmr.tex.143221013",
                                         "_chainsaw/character/ch/cha0/cha001/00/cha001_00_body_albd.tex.143221013"};
    remod::Nicknames n;
    remod::set_nickname(n, "_chainsaw/character/ch/cha0/cha000", "Leon");
    remod::set_nickname(n, paths[2], "Ashley dress");
    CHECK(remod::search(paths, "leon albd", &n) == std::vector<size_t>{0});
    CHECK(remod::search(paths, "Ashley", &n) == std::vector<size_t>{2});
    CHECK(remod::search(paths, "leon").empty());  // without nicknames, only the paths count
}
