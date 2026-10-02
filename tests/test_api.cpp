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
