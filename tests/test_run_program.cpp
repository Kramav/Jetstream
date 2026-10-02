// Run program: real programs (cmd.exe, PowerShell, batch files), so nothing here needs anything installed.
#include "api.hpp"
#include "graph.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using Catch::Matchers::ContainsSubstring;
using remod::Graph;
using remod::NodeState;
using test::TempDir;
namespace fs = std::filesystem;

namespace {

const remod::Profile& re4r() {
    static const remod::Profile p = remod::load_profile(REMOD_PROFILES_DIR "/re4r.toml");
    return p;
}

remod::RunResult run(const Graph& g, const fs::path& dir) {
    static remod::NativeConverter converter;  // not used: nothing here converts textures
    return remod::run_graph(g, {.profile = re4r(), .converter = converter, .base_dir = dir});
}

std::string run_error(const Graph& g, const fs::path& dir) {
    try {
        run(g, dir);
    } catch (const remod::RunError& e) {
        return e.what();
    }
    return "(no error)";
}

// A Run program block (node 1) with this program and arguments.
Graph program(const std::string& program, const std::string& arguments, const std::string& out = "") {
    Graph g;
    auto& n = g.add_node("RunProgram");
    n.params["program"] = program;
    n.params["arguments"] = arguments;
    n.params["out_file"] = out;
    return g;
}

}  // namespace

TEST_CASE("Run program: an .exe found on PATH, what it printed passed on") {
    TempDir tmp;
    Graph g = program("cmd", "/d /c echo hello there");
    REQUIRE(g.validate().empty());
    const auto result = run(g, tmp.path);
    CHECK(result.values.at({1, "text"}) == "hello there");
    CHECK_THAT(result.nodes.at(1).message, ContainsSubstring("ran cmd.exe"));
}

TEST_CASE("Run program: a batch file next to the graph gets {in} and writes {out}") {
    TempDir tmp;
    test::write_file(tmp.path / "copy_it.bat", "@copy /y \"%~1\" \"%~2\" >nul\r\n");
    test::write_file(tmp.path / "a.txt", "A");
    Graph g = program("copy_it", "{in} {out}", "out/a copy.txt");
    const int value = g.add_node("Value").id;
    g.find(value)->params["value"] = "a.txt";
    REQUIRE(g.connect({value, "value", 1, "in"}).empty());
    REQUIRE(g.validate().empty());

    const auto result = run(g, tmp.path);
    CHECK(test::read_file(tmp.path / "out/a copy.txt") == "A");
    CHECK(fs::path(result.values.at({1, "file"})) == tmp.path / "out" / "a copy.txt");

    // Characters cmd.exe would run as commands are refused, never passed.
    g.find(1)->params["arguments"] = "{in} {out} & del a.txt";
    CHECK_THAT(run_error(g, tmp.path), ContainsSubstring("is a batch file"));
    CHECK(fs::exists(tmp.path / "a.txt"));
}

TEST_CASE("Run program: failures say why") {
    TempDir tmp;
    test::write_file(tmp.path / "boom.bat", "@echo it went boom\r\n@exit /b 3\r\n");
    CHECK_THAT(run_error(program("boom.bat", ""), tmp.path),
               ContainsSubstring("exit code 3") && ContainsSubstring("it went boom"));

    test::write_file(tmp.path / "lazy.bat", "@echo nothing written\r\n");
    CHECK_THAT(run_error(program("lazy.bat", "{out}", "never.txt"), tmp.path), ContainsSubstring("didn't write"));
    CHECK_THAT(run_error(program("lazy.bat", "{out}"), tmp.path), ContainsSubstring("Output file is empty"));
    CHECK_THAT(run_error(program("lazy.bat", "{in}"), tmp.path), ContainsSubstring("nothing is linked"));
    CHECK_THAT(run_error(program("no_such_program_here", ""), tmp.path), ContainsSubstring("no program called"));

    test::write_file(tmp.path / "slow.bat", "@ping -n 6 127.0.0.1 >nul\r\n");
    Graph slow = program("slow.bat", "");
    slow.find(1)->params["timeout"] = "1";
    CHECK_THAT(run_error(slow, tmp.path), ContainsSubstring("timed out"));
}

TEST_CASE("Run program: a PowerShell script, quoted arguments kept whole") {
    TempDir tmp;
    test::write_file(tmp.path / "say.ps1", "param($a, $b)\r\nWrite-Output \"[$a] [$b]\"\r\n");
    const auto result = run(program("say.ps1", "\"two words\" three"), tmp.path);
    CHECK(result.values.at({1, "text"}) == "[two words] [three]");
}

TEST_CASE("Run program: repeated for a list, {name} per file") {
    TempDir tmp;
    test::write_file(tmp.path / "copy_it.bat", "@copy /y \"%~1\" \"%~2\" >nul\r\n");
    for (const char* name : {"a.txt", "b.txt"}) test::write_file(tmp.path / "in" / name, name);
    Graph g = program("copy_it.bat", "{in} {out}", "out/{name}.copy");
    const int files = g.add_node("FilesInFolder").id;
    g.find(files)->params["folder"] = "in";
    g.find(files)->params["pattern"] = "*.txt";
    REQUIRE(g.connect({files, "files", 1, "in"}).empty());
    REQUIRE(g.validate().empty());
    const auto result = run(g, tmp.path);
    CHECK(result.nodes.at(1).items.size() == 2);
    CHECK(test::read_file(tmp.path / "out/b.copy") == "b.txt");
}

TEST_CASE("Run program: an API run needs the user's approval to start a program") {
    TempDir tmp;
    remod::ApiSession api;
    api.call(R"({"op":"new"})");
    api.call(R"({"op":"add","type":"RunProgram"})");
    api.call(R"({"op":"set","id":1,"input":"program","value":"cmd"})");
    api.call(R"({"op":"set","id":1,"input":"arguments","value":"/d /c echo hi"})");
    api.call(R"({"op":"save","file":")" + (tmp.path / "g.json").generic_string() + R"("})");
    const std::string plan = api.call(R"({"op":"plan"})");
    CHECK_THAT(plan, ContainsSubstring(R"("action":"run program")") && ContainsSubstring(R"("verdict":"needs approval")"));
    const std::string id = plan.substr(plan.find(R"("plan":")") + 8, 16);
    CHECK_THAT(api.call(R"({"op":"run","plan":")" + id + R"("})"), ContainsSubstring("need the user's approval"));
    CHECK_THAT(api.call(R"({"op":"run","plan":")" + id + R"(","approve":true})"), ContainsSubstring(R"("message":"Done.")"));
}

TEST_CASE("Run program: runs again only when what it gets changed, or its output did; Always run always") {
    TempDir tmp;
    // Counts its runs, prints, copies its input to its output.
    test::write_file(tmp.path / "copy_it.bat",
                     "@echo ran>>count.txt\r\n@echo copied %~nx1\r\n@copy /y \"%~1\" \"%~2\" >nul\r\n");
    test::write_file(tmp.path / "a.txt", "A");
    Graph g = program("copy_it.bat", "{in} {out}", "out/a copy.txt");
    const int value = g.add_node("Value").id;
    g.find(value)->params["value"] = "a.txt";
    REQUIRE(g.connect({value, "value", 1, "in"}).empty());
    auto runs = [&] {
        const std::string count = test::read_file(tmp.path / "count.txt");
        return std::ranges::count(count, '\n');
    };
    auto again = [&] {
        const auto result = run(g, tmp.path);
        remod::apply_run(g, result);  // the app and the API keep what a run recorded
        return result;
    };

    again();
    CHECK(runs() == 1);
    const auto kept = again();
    CHECK(runs() == 1);
    CHECK_THAT(kept.nodes.at(1).message, ContainsSubstring("unchanged"));
    CHECK(kept.values.at({1, "text"}) == "copied a.txt");  // what it printed, kept
    CHECK(fs::path(kept.values.at({1, "file"})) == tmp.path / "out" / "a copy.txt");

    test::write_file(tmp.path / "a.txt", "B");  // the input's contents
    again();
    CHECK(runs() == 2);
    CHECK(test::read_file(tmp.path / "out/a copy.txt") == "B");

    fs::remove(tmp.path / "out/a copy.txt");  // its output gone
    again();
    CHECK(runs() == 3);

    g.find(1)->params["arguments"] = "{in}  {out}";  // the same arguments, split the same: still unchanged
    again();
    CHECK(runs() == 3);

    g.find(1)->params["always"] = "true";
    again();
    CHECK(runs() == 4);
}
