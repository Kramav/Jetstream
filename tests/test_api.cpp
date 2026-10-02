#include "api.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;

TEST_CASE("api: a program builds, checks and saves a graph with core's rules") {
    remod::ApiSession api;
    CHECK_THAT(api.call(R"({"op":"types"})"), StartsWith(R"({"ok":true)") && ContainsSubstring(R"("type":"ReplacePhoto")"));
    CHECK(api.call(R"({"op":"new"})") == R"({"ok":true})");
    CHECK(api.call(R"({"op":"add","type":"LoadTex"})") == R"({"id":1,"ok":true})");
    CHECK(api.call(R"({"op":"add","type":"ExportImage"})") == R"({"id":2,"ok":true})");
    CHECK_THAT(api.call(R"({"op":"next","id":1,"port":"tex","output":true})"),
               ContainsSubstring(R"({"port":"tex","type":"ExportImage"})"));

    // The rules answer, with their reasons: a texture can't go into a text-only field; unknown things are named.
    CHECK_THAT(api.call(R"({"op":"link","from":1,"from_port":"tex","to":2,"to_port":"nope"})"),
               StartsWith(R"({"error":)") && ContainsSubstring("has no input 'nope'"));
    CHECK(api.call(R"({"op":"link","from":1,"from_port":"tex","to":2,"to_port":"tex"})") == R"({"ok":true})");
    CHECK_THAT(api.call(R"({"op":"set","id":2,"input":"bogus","value":"x"})"), ContainsSubstring("no typed field 'bogus'"));
    CHECK_THAT(api.call(R"({"op":"frobnicate"})"), ContainsSubstring("unknown op 'frobnicate'"));
    CHECK_THAT(api.call("not json"), StartsWith(R"({"error":)"));

    CHECK_THAT(api.call(R"({"op":"validate"})"), ContainsSubstring(R"("problems":[")"));  // nothing typed in yet
    CHECK(api.call(R"({"op":"set","id":1,"input":"tex","value":"a.tex.143221013"})") == R"({"ok":true})");
    CHECK(api.call(R"({"op":"set","id":2,"input":"png","value":"a.png"})") == R"({"ok":true})");  // an output's field
    CHECK_THAT(api.call(R"({"op":"preview"})"), ContainsSubstring(R"("node":2,"output":"png")"));

    test::TempDir tmp;
    const std::string file = (tmp.path / "g.json").generic_string();
    CHECK_THAT(api.call(R"({"op":"save"})"), ContainsSubstring("needs a file"));
    CHECK(api.call(R"({"op":"save","file":")" + file + R"("})") == R"({"ok":true})");
    remod::ApiSession other;
    CHECK(other.call(R"({"op":"open","file":")" + file + R"("})") == R"({"ok":true})");
    CHECK_THAT(other.call(R"({"op":"graph"})"),
               ContainsSubstring(R"("links":[{"from":1,"from_port":"tex","to":2,"to_port":"tex"}])") &&
                   ContainsSubstring(R"("png":"a.png")"));
    CHECK(other.call(R"({"op":"unlink","from":1,"from_port":"tex","to":2,"to_port":"tex"})") == R"({"ok":true})");
    CHECK(other.call(R"({"op":"remove","id":2})") == R"({"ok":true})");
    CHECK_THAT(other.call(R"({"op":"graph"})"), ContainsSubstring(R"("links":[])"));
}

TEST_CASE("api: a run needs a plan, the user's approval where it asks for it, and never touches Noesis's folder") {
    test::TempDir tmp;
    const auto g = tmp.path / "g", noesis = tmp.path / "noesis" / "Noesis64.exe";
    test::write_file(g / "a.txt", "a");
    test::write_file(noesis, "");  // stands in: the run converts nothing
    const std::string n = noesis.generic_string(), out = (tmp.path / "out" / "a.txt").generic_string();
    auto plan_id = [](const std::string& reply) { return reply.substr(reply.find(R"("plan":")") + 8, 16); };

    remod::ApiSession api;
    api.call(R"({"op":"new"})");
    api.call(R"({"op":"add","type":"CopyFile"})");
    api.call(R"({"op":"set","id":1,"input":"source","value":"a.txt"})");
    api.call(R"({"op":"set","id":1,"input":"dest","value":")" + out + R"("})");
    CHECK_THAT(api.call(R"({"op":"plan","noesis":")" + n + R"("})"), ContainsSubstring("save the graph first"));
    api.call(R"({"op":"save","file":")" + (g / "g.json").generic_string() + R"("})");

    const std::string plan = api.call(R"({"op":"plan","noesis":")" + n + R"("})");
    CHECK_THAT(plan, ContainsSubstring(R"("verdict":"needs approval")") && ContainsSubstring(R"("needs_approval":true)"));
    const std::string id = plan_id(plan);
    CHECK_THAT(api.call(R"({"op":"run","noesis":")" + n + R"(","plan":")" + id + R"("})"),
               ContainsSubstring("need the user's approval"));
    CHECK_THAT(api.call(R"({"op":"run","noesis":")" + n + R"(","plan":"0000000000000000","approve":true})"),
               ContainsSubstring("ask for a new plan"));
    CHECK_FALSE(std::filesystem::exists(tmp.path / "out" / "a.txt"));
    CHECK_THAT(api.call(R"({"op":"run","noesis":")" + n + R"(","plan":")" + id + R"(","approve":true})"),
               StartsWith(R"({"message":"Done.")") && ContainsSubstring(R"("state":"done")"));
    CHECK(std::filesystem::exists(tmp.path / "out" / "a.txt"));

    // Into Noesis's folder: refused, and approval doesn't lift it.
    api.call(R"({"op":"set","id":1,"input":"dest","value":")" + (tmp.path / "noesis" / "a.txt").generic_string() + R"("})");
    const std::string bad = api.call(R"({"op":"plan","noesis":")" + n + R"("})");
    CHECK_THAT(bad, ContainsSubstring(R"("verdict":"refused")"));
    CHECK_THAT(api.call(R"({"op":"run","noesis":")" + n + R"(","plan":")" + plan_id(bad) + R"(","approve":true})"),
               ContainsSubstring("refused changes"));
    CHECK_FALSE(std::filesystem::exists(tmp.path / "noesis" / "a.txt"));
}

TEST_CASE("api: Noesis is optional; game_files is refused like Noesis's folder") {
    test::TempDir tmp;
    const auto g = tmp.path / "g", game = tmp.path / "game";
    test::write_file(g / "a.txt", "a");
    std::filesystem::create_directories(game);

    remod::ApiSession api;
    api.call(R"({"op":"new"})");
    api.call(R"({"op":"add","type":"CopyFile"})");
    api.call(R"({"op":"set","id":1,"input":"source","value":"a.txt"})");
    api.call(R"({"op":"set","id":1,"input":"dest","value":"b.txt"})");
    api.call(R"({"op":"save","file":")" + (g / "g.json").generic_string() + R"("})");
    const std::string plan = api.call(R"({"op":"plan"})");
    CHECK_THAT(plan, ContainsSubstring(R"("verdict":"ok")"));
    const std::string id = plan.substr(plan.find(R"("plan":")") + 8, 16);
    CHECK_THAT(api.call(R"({"op":"run","plan":")" + id + R"("})"), StartsWith(R"({"message":"Done.")"));
    CHECK(std::filesystem::exists(g / "b.txt"));

    api.call(R"({"op":"set","id":1,"input":"dest","value":")" + (game / "a.txt").generic_string() + R"("})");
    CHECK_THAT(api.call(R"({"op":"plan","game_files":")" + game.generic_string() + R"("})"),
               ContainsSubstring(R"("verdict":"refused")"));
}

TEST_CASE("api: a list repeats the blocks it feeds; the plan and the run's nodes show each item") {
    test::TempDir tmp;
    const auto g = tmp.path / "g";
    for (const char* name : {"a.txt", "b.txt"}) test::write_file(g / "in" / name, name);

    remod::ApiSession api;
    api.call(R"({"op":"new"})");
    api.call(R"({"op":"add","type":"FilesInFolder"})");
    api.call(R"({"op":"set","id":1,"input":"folder","value":"in"})");
    api.call(R"({"op":"set","id":1,"input":"pattern","value":"*.txt"})");
    api.call(R"({"op":"add","type":"CopyFile"})");
    CHECK(api.call(R"({"op":"link","from":1,"from_port":"files","to":2,"to_port":"source"})") == R"({"ok":true})");
    api.call(R"({"op":"set","id":2,"input":"dest","value":"out/{name}.copy"})");
    api.call(R"({"op":"save","file":")" + (g / "g.json").generic_string() + R"("})");

    const std::string plan = api.call(R"({"op":"plan"})");
    CHECK_THAT(plan, ContainsSubstring("a.copy") && ContainsSubstring("b.copy"));
    const std::string id = plan.substr(plan.find(R"("plan":")") + 8, 16);
    const std::string run = api.call(R"({"op":"run","plan":")" + id + R"("})");
    CHECK_THAT(run, ContainsSubstring(R"("items":[{)") && ContainsSubstring(R"("name":"b")"));
    CHECK(std::filesystem::exists(g / "out" / "a.copy"));
    CHECK(std::filesystem::exists(g / "out" / "b.copy"));
}
