// Custom nodes: block types made from a graph (custom.hpp).
#include "custom.hpp"
#include "api.hpp"
#include "graph.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>

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

struct FakeConverter : remod::ITextureConverter {  // "PNG" = a PNG header of the right size, "tex" = a copy
    remod::TexMeta load_tex(const fs::path& tex, const fs::path& png_out, const remod::Profile& p) override {
        const auto m = remod::read_tex_meta(tex, p);
        test::write_fake_png(png_out, m.width, m.height);
        return m;
    }
    remod::TexMeta save_tex(const fs::path&, const fs::path& original, const fs::path& out,
                            const remod::Profile& p) override {
        fs::copy_file(original, out);
        return remod::read_tex_meta(out, p);
    }
};

bool has(const std::vector<std::string>& errors, const std::string& s) {
    return std::ranges::any_of(errors, [&](const std::string& e) { return e.find(s) != std::string::npos; });
}

// 1 LoadTex -> 6 Split -> 2 Export (a.png) -> 3 Edit -> 4 SaveTex (original from the Split) -> 5 Package.
Graph pipeline(const fs::path& dir) {
    const fs::path tex = dir / "natives/STM/ui/a.tex.143221013";
    test::write_fake_tex(tex, 143221013, 64, 32, 1, 5, 99);
    Graph g;
    g.add_node("LoadTex").params["tex"] = tex.string();
    g.add_node("ExportImage").params["png"] = "a.png";
    g.add_node("EditImage");
    g.add_node("SaveTex");
    auto& pkg = g.add_node("PackageMod");
    pkg.params["name"] = "M";
    pkg.params["out"] = "out";
    g.add_node("Split");
    for (const remod::Link& l : {remod::Link{1, "tex", 6, "in"}, remod::Link{6, "out", 2, "tex"},
                                 remod::Link{2, "png", 3, "png"}, remod::Link{3, "image", 4, "image"},
                                 remod::Link{6, "out", 4, "original"}, remod::Link{4, "tex", 5, "tex"}})
        REQUIRE(g.connect(l).empty());
    return g;
}

// A custom node "Say": Input "word" (default `fallback`) -> Text "{1}!" -> Output "said".
remod::CustomNode say(const std::string& type, const std::string& fallback) {
    remod::CustomNode c{type, "Say", "Adds an exclamation mark.", {}};
    auto& in = c.graph.add_node("NodeInput");
    in.params["name"] = "word";
    in.params["default"] = fallback;
    c.graph.add_node("Text").params["text"] = "{1}!";
    c.graph.add_node("NodeOutput").params["name"] = "said";
    REQUIRE(c.graph.connect({1, "value", 2, "parts"}).empty());
    REQUIRE(c.graph.connect({2, "text", 3, "value"}).empty());
    return c;
}

}  // namespace

TEST_CASE("custom node from a selection: pins from the links crossing it, runs with its Edit image inside") {
    TempDir tmp;
    Graph g = pipeline(tmp.path);
    remod::CustomNode made;
    const int id = remod::make_custom_node(g, {2, 3, 4}, "Edit and convert", &made);
    CHECK(made.type == "custom:edit_and_convert");
    CHECK(g.nodes.size() == 4);  // LoadTex, Package, Split, the custom block
    REQUIRE(g.customs.size() == 1);
    const remod::NodeSpec* spec = remod::find_spec(made.type);
    REQUIRE(spec);
    CHECK(spec->manual);
    REQUIRE(spec->inputs.size() == 2);
    CHECK(spec->inputs[0].type == remod::PortType::Tex);
    REQUIRE(spec->outputs.size() == 1);
    CHECK(std::string(spec->outputs[0].label) == "new texture");
    REQUIRE(g.validate().empty());

    FakeConverter conv;
    const remod::RunOptions opt{.profile = re4r(), .converter = conv, .base_dir = tmp.path};
    const auto first = remod::run_graph(g, opt);
    CHECK(first.paused);
    CHECK(fs::exists(tmp.path / "a.png"));
    const remod::NodeStatus& st = first.nodes.at(id);
    CHECK(st.state == NodeState::Waiting);
    REQUIRE(st.items.size() == 1);
    CHECK(st.items[0].key == "3:done");  // the inner Edit image keeps its id
    CHECK(first.nodes.at(5).state == NodeState::NotReached);
    remod::apply_run(g, first);
    CHECK(g.find(id)->params.contains("2:exported_from"));  // inner state lives on the custom block

    remod::set_edit_done(g, id, true, st.items[0].key);
    const auto built = remod::run_graph(g, opt);
    CHECK_FALSE(built.paused);
    CHECK(built.nodes.at(id).state == NodeState::Done);
    CHECK(built.nodes.at(5).state == NodeState::Done);
    CHECK(fs::is_regular_file(tmp.path / "out/M/natives/STM/ui/a.tex.143221013"));
    CHECK_FALSE(built.values.at({id, spec->outputs[0].name}).empty());  // what the block gave

    remod::set_edit_done(g, id, false);  // Edit again: every inner step
    CHECK_FALSE(g.find(id)->params.contains("3:done"));
}

TEST_CASE("custom node: a typed pin beats the Input's default; saved with the graph; a library of them") {
    TempDir tmp;
    remod::register_custom(say("custom:say_test", "hello"));
    Graph g;
    const int id = g.add_node("custom:say_test").id;
    REQUIRE(g.customs.size() == 1);  // copied in on first use
    const std::string out = remod::find_spec("custom:say_test")->outputs[0].name;
    CHECK(remod::preview_values(g, tmp.path).at({id, out}) == "hello!");
    const std::string pin = remod::find_spec("custom:say_test")->inputs[0].name;
    g.find(id)->params[pin] = "bye";
    CHECK(remod::preview_values(g, tmp.path).at({id, out}) == "bye!");

    remod::save_graph(g, tmp.path / "g.json");
    CHECK_THAT(test::read_file(tmp.path / "g.json"), ContainsSubstring("\"custom_nodes\""));
    CHECK(remod::load_graph(tmp.path / "g.json") == g);

    remod::save_custom_node(say("custom:say_test", "hi there"), tmp.path / "lib");
    const auto library = remod::load_custom_library(tmp.path / "lib");
    REQUIRE(library.size() == 1);
    CHECK(remod::library_differs(g, library) == std::vector<std::string>{"custom:say_test"});
    remod::update_custom(g, library[0]);
    CHECK(remod::library_differs(g, library).empty());
    g.find(id)->params[pin] = "";
    CHECK(remod::preview_values(g, tmp.path).at({id, out}) == "hi there!");
}

TEST_CASE("custom node inside a custom node; unknown and self-containing ones are reported") {
    TempDir tmp;
    remod::register_custom(say("custom:say_inner", "deep"));
    remod::CustomNode outer{"custom:say_outer", "Say twice", "", {}};
    outer.graph.add_node("custom:say_inner");  // copies say_inner into outer's graph
    auto& o = outer.graph.add_node("NodeOutput");
    o.params["name"] = "result";
    const std::string inner_out = remod::find_spec("custom:say_inner")->outputs[0].name;
    REQUIRE(outer.graph.connect({1, inner_out, o.id, "value"}).empty());
    remod::register_custom(outer);
    Graph g;
    const int id = g.add_node("custom:say_outer").id;
    CHECK(remod::preview_values(g, tmp.path).at({id, remod::find_spec("custom:say_outer")->outputs[0].name}) == "deep!");

    Graph unknown;
    unknown.nodes.push_back({.id = 1, .type = "custom:nobody_made_this"});
    CHECK(has(unknown.validate(), "isn't in this graph or your library"));

    remod::CustomNode loop{"custom:loop_test", "Loop", "", {}};
    loop.graph.nodes.push_back({.id = 1, .type = "custom:loop_test"});
    remod::register_custom(loop);
    Graph looped;
    looped.add_node("custom:loop_test");
    CHECK(has(looped.validate(), "contains itself"));
}

TEST_CASE("custom node: the plan puts its inner blocks' changes down to it; the API offers it") {
    TempDir tmp;
    test::write_file(tmp.path / "a.txt", "A");
    Graph g;
    g.add_node("CopyFile").params["source"] = "a.txt";
    g.find(1)->params["dest"] = "copied.txt";
    const int id = remod::make_custom_node(g, {1}, "Copy kit");
    const auto plan = remod::plan_changes(g, tmp.path);
    REQUIRE(plan.changes.size() == 1);
    CHECK(plan.changes[0].node == id);
    CHECK(plan.changes[0].path == tmp.path / "copied.txt");

    remod::ApiSession api;
    CHECK_THAT(api.call(R"({"op":"types"})"), ContainsSubstring(R"("type":"custom:copy_kit")"));
    api.call(R"({"op":"new"})");
    CHECK(api.call(R"({"op":"add","type":"custom:copy_kit"})") == R"({"id":1,"ok":true})");
}
