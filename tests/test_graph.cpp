#include "graph.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using Catch::Matchers::ContainsSubstring;
using remod::Graph;
using remod::GraphError;
using test::TempDir;
namespace fs = std::filesystem;

namespace {

const remod::Profile& re4r() {
    static const remod::Profile p = remod::load_profile(REMOD_PROFILES_DIR "/re4r.toml");
    return p;
}

// Stands in for Noesis: "PNG" = a PNG header of the right size, "tex" = a copy of the original.
struct FakeConverter : remod::ITextureConverter {
    int loads = 0, saves = 0;
    remod::TexMeta load_tex(const fs::path& tex, const fs::path& png_out, const remod::Profile& p) override {
        ++loads;
        const auto m = remod::read_tex_meta(tex, p);
        test::write_fake_png(png_out, m.width, m.height);
        return m;
    }
    remod::TexMeta save_tex(const fs::path&, const fs::path& original, const fs::path& out,
                            const remod::Profile& p) override {
        ++saves;
        fs::copy_file(original, out);
        return remod::read_tex_meta(out, p);
    }
};

// LoadTex -> ExportImage -> SaveTex -> PackageMod, like schemas/graph.v0.example.json.
Graph pipeline(const std::string& tex, const std::string& png, const std::string& out) {
    Graph g;
    g.add_node("LoadTex").params["tex"] = tex;
    g.add_node("ExportImage").params["png"] = png;
    g.add_node("SaveTex");
    auto& pkg = g.add_node("PackageMod");
    pkg.params["name"] = "M";
    pkg.params["out"] = out;
    for (const remod::Link& l : {remod::Link{1, "tex", 2, "tex"}, remod::Link{2, "image", 3, "image"},
                                 remod::Link{1, "tex", 3, "original"}, remod::Link{3, "tex", 4, "tex"}})
        REQUIRE(g.connect(l).empty());
    return g;
}

}  // namespace

TEST_CASE("node types cover the M1 pipeline") {
    for (const char* t : {"LoadTex", "ExportImage", "ImportImage", "SaveTex", "PackageMod"})
        CHECK(remod::find_spec(t) != nullptr);
    CHECK(remod::find_spec("Nope") == nullptr);
}

TEST_CASE("add_node assigns ids and empty params") {
    Graph g;
    CHECK(g.add_node("LoadTex").id == 1);
    CHECK(g.add_node("SaveTex").id == 2);
    CHECK(g.find(1)->params == std::map<std::string, std::string>{{"tex", ""}, {"game_path", ""}});
    CHECK_THROWS_AS(g.add_node("Nope"), GraphError);
    g.remove_node(1);
    CHECK(g.add_node("SaveTex").id == 3);
}

TEST_CASE("connect enforces port names, types, single inputs and no loops") {
    Graph g;
    g.add_node("LoadTex");      // 1
    g.add_node("ExportImage");  // 2
    g.add_node("SaveTex");      // 3
    CHECK(g.connect({1, "tex", 2, "tex"}).empty());
    CHECK_THAT(g.connect({1, "tex", 3, "image"}), ContainsSubstring("different types"));
    CHECK_THAT(g.connect({1, "nope", 3, "original"}), ContainsSubstring("no output 'nope'"));
    CHECK_THAT(g.connect({1, "tex", 3, "nope"}), ContainsSubstring("no input 'nope'"));
    CHECK_THAT(g.connect({1, "tex", 2, "tex"}), ContainsSubstring("already connected"));
    CHECK_THAT(g.connect({1, "tex", 9, "tex"}), ContainsSubstring("missing node"));
    CHECK(g.connect({2, "image", 3, "image"}).empty());
    g.disconnect(0);  // free ExportImage.tex, then try to feed it from SaveTex: a loop
    CHECK_THAT(g.connect({3, "tex", 2, "tex"}), ContainsSubstring("loop"));
    CHECK(g.links.size() == 1);
    g.remove_node(3);
    CHECK(g.links.empty());
}

TEST_CASE("validate lists every problem") {
    Graph g;
    g.add_node("PackageMod");
    g.nodes.push_back({.id = 7, .type = "Bogus"});
    g.find(1)->params["typo"] = "x";
    const auto errors = g.validate();
    auto has = [&](const std::string& s) {
        return std::ranges::any_of(errors, [&](const std::string& e) { return e.find(s) != std::string::npos; });
    };
    CHECK(has("'name' is required"));
    CHECK(has("'out' is required"));
    CHECK(has("input 'tex' is not connected"));
    CHECK(has("unknown parameter 'typo'"));
    CHECK(has("node 7 (Bogus): unknown node type"));
    CHECK(pipeline("a", "b", "c").validate().empty());
}

TEST_CASE("graph file round-trips") {
    TempDir tmp;
    Graph g = pipeline("C:/x/a.tex.143221013", "a.png", "out");
    g.find(2)->x = 320.5f;
    g.find(2)->y = -40;
    remod::save_graph(g, tmp.path / "g.json");
    const Graph back = remod::load_graph(tmp.path / "g.json");
    CHECK(back.profile == "re4r");
    REQUIRE(back.nodes.size() == 4);
    CHECK(back.find(1)->params.at("tex") == "C:/x/a.tex.143221013");
    CHECK(back.find(2)->x == 320.5f);
    CHECK(back.find(2)->y == -40);
    REQUIRE(back.links.size() == 4);
    CHECK(back.links[2].from_node == 1);
    CHECK(back.links[2].to_port == "original");
    CHECK(back.validate().empty());

    test::write_file(tmp.path / "bad.json", "{ not json");
    CHECK_THROWS_WITH(remod::load_graph(tmp.path / "bad.json"), ContainsSubstring("invalid graph file"));
    test::write_file(tmp.path / "v1.json", R"({"schema_version": 1, "profile": "re4r", "nodes": []})");
    CHECK_THROWS_WITH(remod::load_graph(tmp.path / "v1.json"), ContainsSubstring("schema_version"));
    CHECK_THROWS_AS(remod::load_graph(tmp.path / "missing.json"), GraphError);
}

TEST_CASE("the example graph file loads and validates") {
    const Graph g = remod::load_graph(REMOD_SCHEMAS_DIR "/graph.v0.example.json");
    CHECK(g.validate().empty());
}

TEST_CASE("game path is inferred from a natives tree") {
    CHECK(remod::game_path_from("D:/mods/natives/stm/_chainsaw/ui/a.tex.1", "natives/STM") == "_chainsaw/ui/a.tex.1");
    CHECK(remod::game_path_from("D:\\x\\NATIVES\\STM\\a.tex.1", "natives/STM") == "a.tex.1");
    CHECK(remod::game_path_from("D:/x/natives/a.tex.1", "natives/STM") == "");
    CHECK(remod::game_path_from("D:/x/natives/STM", "natives/STM") == "");
}

TEST_CASE("run: export pauses for editing, second run packages") {
    TempDir tmp;
    const fs::path tex = tmp.path / "natives/STM/_chainsaw/ui/a.tex.143221013";
    test::write_fake_tex(tex, 143221013, 64, 32, 1, 5, 99);
    const Graph g = pipeline(tex.string(), "a.png", "out");  // relative paths: under base_dir
    FakeConverter conv;
    std::vector<std::string> log;
    const remod::RunOptions opt{.profile = re4r(),
                                .converter = conv,
                                .base_dir = tmp.path,
                                .log = [&](const std::string& s) { log.push_back(s); }};

    const auto first = remod::run_graph(g, opt);
    CHECK(first.paused);
    CHECK_THAT(first.message, ContainsSubstring("Edit it"));
    CHECK(fs::exists(tmp.path / "a.png"));
    CHECK_FALSE(fs::exists(tmp.path / "out"));
    CHECK(conv.loads == 1);

    const auto second = remod::run_graph(g, opt);
    CHECK_FALSE(second.paused);
    CHECK(conv.loads == 1);  // the edited PNG is kept, not re-exported
    CHECK(conv.saves == 1);
    CHECK(fs::is_regular_file(tmp.path / "out/M/natives/STM/_chainsaw/ui/a.tex.143221013"));
    CHECK(fs::is_regular_file(tmp.path / "out/M.zip"));
    CHECK_THAT(log.back(), ContainsSubstring("node 4 (PackageMod): packaged"));

    CHECK_THROWS_WITH(remod::run_graph(g, opt), ContainsSubstring("node 4 (PackageMod)") &&
                                                     ContainsSubstring("already exists"));
}

TEST_CASE("run: failures name the node, and nothing runs on an invalid graph") {
    TempDir tmp;
    FakeConverter conv;
    const remod::RunOptions opt{.profile = re4r(), .converter = conv, .base_dir = tmp.path};

    // .tex outside a natives tree and no game_path set
    const fs::path tex = tmp.path / "loose.tex.143221013";
    test::write_fake_tex(tex, 143221013, 64, 32, 1, 5, 99);
    test::write_fake_png(tmp.path / "a.png", 64, 32);
    CHECK_THROWS_WITH(remod::run_graph(pipeline(tex.string(), "a.png", "out"), opt),
                      ContainsSubstring("node 4 (PackageMod)") && ContainsSubstring("game path unknown"));

    Graph with_path = pipeline(tex.string(), "a.png", "out");
    with_path.find(1)->params["game_path"] = "ui/loose.tex.143221013";
    CHECK_FALSE(remod::run_graph(with_path, opt).paused);
    CHECK(fs::is_regular_file(tmp.path / "out/M/natives/STM/ui/loose.tex.143221013"));

    Graph invalid = pipeline(tex.string(), "a.png", "out2");
    invalid.find(4)->params["name"] = "";
    CHECK_THROWS_WITH(remod::run_graph(invalid, opt), ContainsSubstring("'name' is required"));
    CHECK(conv.saves == 2);  // the two runs above reached SaveTex; the invalid graph ran nothing

    Graph other = pipeline(tex.string(), "a.png", "out3");
    other.profile = "re2r";
    CHECK_THROWS_WITH(remod::run_graph(other, opt), ContainsSubstring("profile 're2r'"));
}

TEST_CASE("profiles load by id") {
    CHECK(remod::load_profile_by_id(REMOD_PROFILES_DIR, "re4r").name == "Resident Evil 4 (2023)");
    CHECK_THROWS_AS(remod::load_profile_by_id(REMOD_PROFILES_DIR, "../x"), remod::ProfileError);
    CHECK_THROWS_AS(remod::load_profile_by_id(REMOD_PROFILES_DIR, "nope"), remod::ProfileError);
}
