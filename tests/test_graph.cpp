#include "graph.hpp"
#include "image.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using Catch::Matchers::ContainsSubstring;
using remod::Graph;
using remod::GraphError;
using remod::NodeState;
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

bool has(const std::vector<std::string>& errors, const std::string& s) {
    return std::ranges::any_of(errors, [&](const std::string& e) { return e.find(s) != std::string::npos; });
}

// 1 LoadTex -> 2 ExportImage -> 3 EditImage -> 4 SaveTex -> 5 PackageMod, like schemas/graph.v0.example.json.
Graph pipeline(const std::string& tex, const std::string& png, const std::string& out) {
    Graph g;
    g.add_node("LoadTex").params["tex"] = tex;
    g.add_node("ExportImage").params["png"] = png;
    g.add_node("EditImage");
    g.add_node("SaveTex");
    auto& pkg = g.add_node("PackageMod");
    pkg.params["name"] = "M";
    pkg.params["out"] = out;
    for (const remod::Link& l : {remod::Link{1, "tex", 2, "tex"}, remod::Link{2, "png", 3, "png"},
                                 remod::Link{3, "image", 4, "image"}, remod::Link{1, "tex", 4, "original"},
                                 remod::Link{4, "tex", 5, "tex"}})
        REQUIRE(g.connect(l).empty());
    return g;
}

}  // namespace

TEST_CASE("node types cover the pipeline") {
    for (const char* t : {"LoadTex", "ExportImage", "EditImage", "ImportImage", "SaveTex", "PackageMod", "Text"})
        CHECK(remod::find_spec(t) != nullptr);
    CHECK(remod::find_spec("Nope") == nullptr);
    CHECK(remod::find_spec("EditImage")->manual);
    CHECK_FALSE(remod::find_spec("ExportImage")->manual);
}

TEST_CASE("add_node assigns ids and empty params") {
    Graph g;
    CHECK(g.add_node("LoadTex").id == 1);
    CHECK(g.add_node("SaveTex").id == 2);
    CHECK(g.find(1)->params == std::map<std::string, std::string>{{"tex", ""}, {"game_path", ""}});
    CHECK(g.add_node("ExportImage").params == std::map<std::string, std::string>{{"png", ""}});  // output field
    CHECK(g.add_node("EditImage").params.empty());  // "done" is state, absent until set
    CHECK_THROWS_AS(g.add_node("Nope"), GraphError);
    g.remove_node(1);
    CHECK(g.add_node("SaveTex").id == 5);
}

TEST_CASE("connect enforces port names, types, single inputs and no loops") {
    Graph g;
    g.add_node("LoadTex");      // 1
    g.add_node("ExportImage");  // 2
    g.add_node("SaveTex");      // 3
    CHECK(g.connect({1, "tex", 2, "tex"}).empty());
    CHECK_THAT(g.connect({1, "tex", 3, "image"}), ContainsSubstring("that input needs an image"));
    CHECK_THAT(g.connect({1, "nope", 3, "original"}), ContainsSubstring("no output 'nope'"));
    CHECK_THAT(g.connect({1, "tex", 3, "nope"}), ContainsSubstring("no input 'nope'"));
    CHECK_THAT(g.connect({1, "tex", 2, "tex"}), ContainsSubstring("already has a link"));
    CHECK_THAT(g.connect({1, "tex", 9, "tex"}), ContainsSubstring("missing node"));
    CHECK(g.connect({2, "png", 3, "image"}).empty());
    CHECK(g.is_connected(1, "tex", true));
    CHECK(g.is_connected(3, "image", false));
    CHECK_FALSE(g.is_connected(3, "original", false));
    CHECK_FALSE(g.is_connected(2, "tex", true));  // ExportImage has an input 'tex', not an output
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
    CHECK(has(errors, "Mod name is required"));
    CHECK(has(errors, "Output folder is required"));
    CHECK(has(errors, "input 'texture' is not connected"));
    CHECK(has(errors, "unknown parameter 'typo'"));
    CHECK(has(errors, "Bogus (node 7): unknown node type"));
    CHECK(pipeline("a", "b.png", "c").validate().empty());

    Graph export_only;
    export_only.add_node("ExportImage");
    CHECK(has(export_only.validate(), "Image file is required"));  // an output field
}

TEST_CASE("validate checks file extensions where a node lists them") {
    Graph g = pipeline("a.tex.143221013", "edit", "out");  // png without extension: Noesis would write edit.png
    const auto errors = g.validate();
    REQUIRE(errors.size() == 1);
    CHECK_THAT(errors[0], ContainsSubstring("Image file must end in .png, .tga, .jpg, .jpeg"));
    for (const char* ok : {"EDIT.TGA", "edit.jpg"}) {
        g.find(2)->params["png"] = ok;
        CHECK(g.validate().empty());
    }
    g.find(2)->params["png"] = "EDIT.PNG";
    g.add_node("ImportImage").params["png"] = "shot.gif";  // node 6
    REQUIRE(g.connect({6, "image", 5, "preview"}).empty());
    REQUIRE(g.validate().size() == 1);
    CHECK_THAT(g.validate()[0], ContainsSubstring("Image file must end in .png"));
    g.find(6)->params["png"] = "";  // linked-in values aren't typed values: a Text link satisfies it
    g.add_node("Text").params["text"] = "shot.png";  // node 7
    REQUIRE(g.connect({7, "text", 6, "png"}).empty());
    CHECK(g.validate().empty());
}

TEST_CASE("graph file round-trips") {
    TempDir tmp;
    Graph g = pipeline("C:/x/a.tex.143221013", "a.png", "out");
    REQUIRE(g.validate().empty());
    g.find(2)->x = 320.5f;
    g.find(2)->y = -40;
    remod::set_edit_done(g, 3, true);
    remod::save_graph(g, tmp.path / "g.json");
    const Graph back = remod::load_graph(tmp.path / "g.json");
    CHECK(back.profile == "re4r");
    REQUIRE(back.nodes.size() == 5);
    CHECK(back.find(1)->params.at("tex") == "C:/x/a.tex.143221013");
    CHECK(back.find(2)->x == 320.5f);
    CHECK(back.find(2)->y == -40);
    CHECK(back.find(3)->params.at("done") == "true");
    REQUIRE(back.links.size() == 5);
    CHECK(back.links[3].from_node == 1);
    CHECK(back.links[3].to_port == "original");
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
    CHECK(std::ranges::count(g.nodes, std::string("EditImage"), &remod::Node::type) == 1);
}

TEST_CASE("game path is inferred from a natives tree") {
    CHECK(remod::game_path_from("D:/mods/natives/stm/_chainsaw/ui/a.tex.1", "natives/STM") == "_chainsaw/ui/a.tex.1");
    CHECK(remod::game_path_from("D:\\x\\NATIVES\\STM\\a.tex.1", "natives/STM") == "a.tex.1");
    CHECK(remod::game_path_from("D:/x/natives/a.tex.1", "natives/STM") == "");
    CHECK(remod::game_path_from("D:/x/natives/STM", "natives/STM") == "");
}

TEST_CASE("run: the Edit image step waits until marked done, and reports where every node got to") {
    TempDir tmp;
    const fs::path tex = tmp.path / "natives/STM/_chainsaw/ui/a.tex.143221013";
    test::write_fake_tex(tex, 143221013, 64, 32, 1, 5, 99);
    Graph g = pipeline(tex.string(), "a.png", "out");  // relative paths: under base_dir
    FakeConverter conv;
    std::vector<std::string> log;
    const remod::RunOptions opt{.profile = re4r(),
                                .converter = conv,
                                .base_dir = tmp.path,
                                .log = [&](const std::string& s) { log.push_back(s); }};

    const auto first = remod::run_graph(g, opt);
    CHECK(first.paused);
    CHECK_THAT(first.message, ContainsSubstring("Waiting for you") && ContainsSubstring("Done editing"));
    CHECK(fs::exists(tmp.path / "a.png"));
    CHECK_FALSE(fs::exists(tmp.path / "out"));
    CHECK(conv.loads == 1);
    CHECK(first.nodes.at(1).state == NodeState::Done);
    CHECK(first.nodes.at(2).state == NodeState::Done);
    CHECK(first.nodes.at(3).state == NodeState::Waiting);
    CHECK(first.nodes.at(3).file == tmp.path / "a.png");
    CHECK(first.nodes.at(4).state == NodeState::NotReached);
    CHECK(first.nodes.at(5).state == NodeState::NotReached);

    const auto still = remod::run_graph(g, opt);  // not marked done yet: still waiting, PNG kept
    CHECK(still.paused);
    CHECK(conv.loads == 1);

    remod::set_edit_done(g, 3, true);
    const auto built = remod::run_graph(g, opt);
    CHECK_FALSE(built.paused);
    CHECK(conv.saves == 1);
    for (int id : {1, 2, 3, 4, 5}) CHECK(built.nodes.at(id).state == NodeState::Done);
    CHECK(built.nodes.at(5).file == fs::absolute(tmp.path / "out/M.zip"));
    CHECK(fs::is_regular_file(tmp.path / "out/M/natives/STM/_chainsaw/ui/a.tex.143221013"));
    CHECK_THAT(log.back(), ContainsSubstring("Package for Fluffy (node 5): packaged"));

    try {
        remod::run_graph(g, opt);
        FAIL("expected a RunError");
    } catch (const remod::RunError& e) {
        CHECK_THAT(std::string(e.what()), ContainsSubstring("Package for Fluffy (node 5)") && ContainsSubstring("already exists"));
        CHECK(e.nodes.at(4).state == NodeState::Done);
        CHECK(e.nodes.at(5).state == NodeState::Failed);
        CHECK_THAT(e.nodes.at(5).message, ContainsSubstring("already exists"));
    }

    SECTION("re-exporting the PNG voids an earlier Done") {
        fs::remove(tmp.path / "a.png");
        const auto again = remod::run_graph(g, opt);
        CHECK(again.paused);
        CHECK(again.nodes.at(3).state == NodeState::Waiting);
        CHECK(again.reset_edits == std::vector<int>{3});
        remod::apply_run(g, again);
        CHECK_FALSE(g.find(3)->params.contains("done"));
    }
}

TEST_CASE("run: failures name the node, and nothing runs on an invalid graph") {
    TempDir tmp;
    FakeConverter conv;
    const remod::RunOptions opt{.profile = re4r(), .converter = conv, .base_dir = tmp.path, .edits_done = true};

    // .tex outside a natives tree and no game_path set
    const fs::path tex = tmp.path / "loose.tex.143221013";
    test::write_fake_tex(tex, 143221013, 64, 32, 1, 5, 99);
    test::write_fake_png(tmp.path / "a.png", 64, 32);
    CHECK_THROWS_WITH(remod::run_graph(pipeline(tex.string(), "a.png", "out"), opt),
                      ContainsSubstring("Package for Fluffy (node 5)") && ContainsSubstring("game path unknown"));

    Graph with_path = pipeline(tex.string(), "a.png", "out");
    with_path.find(1)->params["game_path"] = "ui/loose.tex.143221013";
    CHECK_FALSE(remod::run_graph(with_path, opt).paused);
    CHECK(fs::is_regular_file(tmp.path / "out/M/natives/STM/ui/loose.tex.143221013"));

    Graph invalid = pipeline(tex.string(), "a.png", "out2");
    invalid.find(5)->params["name"] = "";
    CHECK_THROWS_WITH(remod::run_graph(invalid, opt), ContainsSubstring("Mod name is required"));
    CHECK(conv.saves == 2);  // the two runs above reached SaveTex; the invalid graph ran nothing

    Graph other = pipeline(tex.string(), "a.png", "out3");
    other.profile = "re2r";
    CHECK_THROWS_WITH(remod::run_graph(other, opt), ContainsSubstring("profile 're2r'"));
}

TEST_CASE("every input can be linked, with type rules") {
    Graph g;
    g.add_node("LoadTex");      // 1
    g.add_node("SaveTex");      // 2
    g.add_node("PackageMod");   // 3
    g.add_node("Text");         // 4
    g.add_node("ImportImage");  // 5
    g.add_node("Text");         // 6
    CHECK(g.connect({4, "text", 3, "name"}).empty());  // text into a text field
    CHECK(g.connect({4, "text", 1, "tex"}).empty());   // text into an editable path field (a typed path)
    CHECK_THAT(g.connect({4, "text", 2, "original"}), ContainsSubstring("needs a texture"));  // link-only input
    CHECK(g.connect({1, "tex", 6, "parts"}).empty());   // anything into a Text input (its path)
    CHECK(g.connect({5, "image", 6, "parts"}).empty());  // multiple input: a second link
    CHECK(g.connect({5, "image", 3, "author"}).empty()); // any output into a text field
    CHECK_THAT(g.connect({5, "image", 3, "tex"}), ContainsSubstring("needs a texture"));
    CHECK_THAT(g.connect({6, "text", 3, "name"}), ContainsSubstring("already has a link"));
    CHECK(g.links_into(6, "parts").size() == 2);
}

TEST_CASE("a multiple input takes any number of links; required means at least one") {
    Graph g = pipeline("a.tex.143221013", "a.png", "out");
    g.disconnect(4);  // PackageMod's only texture
    CHECK(has(g.validate(), "input 'texture' is not connected"));
    REQUIRE(g.connect({4, "tex", 5, "tex"}).empty());
    g.add_node("SaveTex");  // node 6
    REQUIRE(g.connect({6, "tex", 5, "tex"}).empty());
    CHECK(g.links_into(5, "tex").size() == 2);
}

TEST_CASE("fill_template substitutes numbered parts") {
    CHECK(remod::fill_template("{1} v{2}", {"Ada", "2"}) == "Ada v2");
    CHECK(remod::fill_template("{2}{1}{2}", {"a", "b"}) == "bab");
    CHECK(remod::fill_template("no placeholders", {"unused"}) == "no placeholders");
    CHECK(remod::fill_template("braces {x} stay", {}) == "braces {x} stay");
    CHECK_THROWS_WITH(remod::fill_template("{3}", {"a", "b"}), ContainsSubstring("{3}") && ContainsSubstring("2 part"));
    CHECK_THROWS_AS(remod::fill_template("{0}", {"a"}), GraphError);
}

TEST_CASE("old graph files are migrated") {
    TempDir tmp;
    SECTION("PackageMod's screenshot becomes an ImportImage preview") {
        test::write_file(tmp.path / "old.json", R"({"schema_version": 0, "profile": "re4r",
          "nodes": [{"id": 1, "type": "PackageMod", "params": {"name": "M", "out": "o", "screenshot": "shot.png"}, "pos": [900, 0]},
                    {"id": 2, "type": "PackageMod", "params": {"name": "N", "out": "o", "screenshot": ""}}],
          "links": []})");
        const Graph g = remod::load_graph(tmp.path / "old.json");
        REQUIRE(g.nodes.size() == 3);
        CHECK_FALSE(g.find(1)->params.contains("screenshot"));
        CHECK_FALSE(g.find(2)->params.contains("screenshot"));
        CHECK(g.find(1)->params.contains("replace"));
        const remod::Node& img = g.nodes.back();
        CHECK(img.type == "ImportImage");
        CHECK(img.params.at("png") == "shot.png");
        REQUIRE(g.links_into(1, "preview").size() == 1);
        CHECK(g.links[g.links_into(1, "preview")[0]].from_node == img.id);
        CHECK(g.links_into(2, "preview").empty());
    }
    SECTION("an Edit PNG step is inserted after each Export PNG") {
        test::write_file(tmp.path / "old.json", R"({"schema_version": 0, "profile": "re4r",
          "nodes": [{"id": 1, "type": "LoadTex", "params": {"tex": "a.tex.143221013", "game_path": ""}},
                    {"id": 2, "type": "ExportImage", "params": {"png": "a.png"}, "pos": [450, 0]},
                    {"id": 3, "type": "SaveTex", "params": {}}],
          "links": [{"from": [1, "tex"], "to": [2, "tex"]}, {"from": [2, "image"], "to": [3, "image"]},
                    {"from": [1, "tex"], "to": [3, "original"]}]})");
        const Graph g = remod::load_graph(tmp.path / "old.json");
        REQUIRE(g.nodes.size() == 4);
        const remod::Node& edit = g.nodes.back();
        CHECK(edit.type == "EditImage");
        CHECK(edit.x == 450);
        CHECK(g.is_connected(2, "png", true));
        CHECK_FALSE(g.is_connected(2, "image", true));
        REQUIRE(g.links_into(3, "image").size() == 1);
        CHECK(g.links[g.links_into(3, "image")[0]].from_node == edit.id);
        CHECK(g.validate().empty());
    }
}

TEST_CASE("run: two textures, linked text, combined preview, replace on rebuild") {
    TempDir tmp;
    FakeConverter conv;
    std::vector<std::string> log;
    const remod::RunOptions opt{.profile = re4r(),
                                .converter = conv,
                                .base_dir = tmp.path,
                                .log = [&](const std::string& s) { log.push_back(s); }};
    Graph g;
    for (const char* name : {"a", "b"}) {  // LoadTex -> SaveTex <- ImportImage, per texture
        const fs::path tex = tmp.path / "natives/STM/ui" / (std::string(name) + ".tex.143221013");
        test::write_fake_tex(tex, 143221013, 64, 32, 1, 5, 99);
        remod::save_png_bgra(tmp.path / (std::string(name) + ".png"), 64, 32, std::vector<std::uint8_t>(64 * 32 * 4, 200));
        const int load = g.add_node("LoadTex").id;
        g.find(load)->params["tex"] = tex.string();
        const int img = g.add_node("ImportImage").id;
        g.find(img)->params["png"] = std::string(name) + ".png";
        const int save = g.add_node("SaveTex").id;
        REQUIRE(g.connect({load, "tex", save, "original"}).empty());
        REQUIRE(g.connect({img, "image", save, "image"}).empty());
    }
    const int pkg = g.add_node("PackageMod").id;
    g.find(pkg)->params["name"] = "typed name, overridden by the link";
    g.find(pkg)->params["out"] = "out";
    const int text = g.add_node("Text").id;
    g.find(text)->params["text"] = "Two {1}";
    const int word = g.add_node("Text").id;
    g.find(word)->params["text"] = "Textures";
    REQUIRE(g.connect({word, "text", text, "parts"}).empty());
    REQUIRE(g.connect({text, "text", pkg, "name"}).empty());
    for (int save : {3, 6}) REQUIRE(g.connect({save, "tex", pkg, "tex"}).empty());
    for (int img : {2, 5}) REQUIRE(g.connect({img, "image", pkg, "preview"}).empty());

    CHECK_FALSE(remod::run_graph(g, opt).paused);
    const fs::path mod = tmp.path / "out/Two Textures";
    CHECK(fs::is_regular_file(mod / "natives/STM/ui/a.tex.143221013"));
    CHECK(fs::is_regular_file(mod / "natives/STM/ui/b.tex.143221013"));
    CHECK(remod::image_size(mod / "preview.png") == std::pair<std::uint32_t, std::uint32_t>{1024, 512});
    CHECK_THAT(test::read_file(mod / "modinfo.ini"), ContainsSubstring("name=Two Textures\r\nscreenshot=preview.png"));
    CHECK_THAT(log.back(), ContainsSubstring("packaged 2 texture(s)"));

    CHECK_THROWS_WITH(remod::run_graph(g, opt), ContainsSubstring("turn on 'Replace existing'"));
    g.find(pkg)->params["replace"] = "true";
    CHECK_FALSE(remod::run_graph(g, opt).paused);  // rebuilds over the previous build
    CHECK(fs::is_regular_file(tmp.path / "out/Two Textures.zip"));
}

TEST_CASE("run: one run exports every image that needs editing") {
    TempDir tmp;
    FakeConverter conv;
    remod::RunOptions opt{.profile = re4r(), .converter = conv, .base_dir = tmp.path};
    Graph g;
    const int pkg = g.add_node("PackageMod").id;  // 1
    g.find(pkg)->params["name"] = "M";
    g.find(pkg)->params["out"] = "out";
    for (const char* name : {"a", "b"}) {
        const fs::path tex = tmp.path / "natives/STM" / (std::string(name) + ".tex.143221013");
        test::write_fake_tex(tex, 143221013, 64, 32, 1, 5, 99);
        const int load = g.add_node("LoadTex").id;
        g.find(load)->params["tex"] = tex.string();
        const int exp = g.add_node("ExportImage").id;
        g.find(exp)->params["png"] = std::string(name) + ".png";
        const int edit = g.add_node("EditImage").id;
        const int save = g.add_node("SaveTex").id;
        REQUIRE(g.connect({load, "tex", exp, "tex"}).empty());
        REQUIRE(g.connect({exp, "png", edit, "png"}).empty());
        REQUIRE(g.connect({edit, "image", save, "image"}).empty());
        REQUIRE(g.connect({load, "tex", save, "original"}).empty());
        REQUIRE(g.connect({save, "tex", pkg, "tex"}).empty());
    }

    const auto first = remod::run_graph(g, opt);
    CHECK(first.paused);
    CHECK_THAT(first.message, ContainsSubstring("edit 2 images") && ContainsSubstring("Done editing on the Edit image steps"));
    CHECK(conv.loads == 2);
    CHECK(conv.saves == 0);
    CHECK(fs::exists(tmp.path / "a.png"));
    CHECK(fs::exists(tmp.path / "b.png"));
    CHECK_FALSE(fs::exists(tmp.path / "out"));

    opt.edits_done = true;  // the CLI's --edited
    const auto second = remod::run_graph(g, opt);
    CHECK_FALSE(second.paused);
    CHECK(conv.saves == 2);
    CHECK(fs::is_regular_file(tmp.path / "out/M/natives/STM/b.tex.143221013"));
}

TEST_CASE("run: LoadTex notes a streaming copy of the texture") {
    TempDir tmp;
    FakeConverter conv;
    std::vector<std::string> log;
    const remod::RunOptions opt{.profile = re4r(),
                                .converter = conv,
                                .base_dir = tmp.path,
                                .log = [&](const std::string& s) { log.push_back(s); }};
    const fs::path tex = tmp.path / "natives/stm/env/a.tex.143221013";
    test::write_fake_tex(tex, 143221013, 64, 32, 1, 5, 99);
    test::write_fake_tex(tmp.path / "natives/stm/streaming/env/a.tex.143221013", 143221013, 64, 32, 1, 5, 99);
    Graph g;
    g.add_node("LoadTex").params["tex"] = tex.string();
    remod::run_graph(g, opt);
    CHECK(std::ranges::any_of(log, [](const std::string& s) { return s.find("streaming/env/a.tex") != std::string::npos; }));
}

TEST_CASE("editing: nodes that fit a pin, added already connected") {
    Graph g = pipeline("a.tex.143221013", "a.png", "out");
    auto types = [](const std::vector<Graph::Choice>& cs) {
        std::vector<std::string> t;
        for (const auto& c : cs) t.emplace_back(c.spec->type);
        return t;
    };
    auto contains = [](const std::vector<std::string>& v, const char* s) { return std::ranges::count(v, s) == 1; };

    // Dragging out of Original texture's "texture" output: things that take a texture (or text, as a path).
    const auto from_tex = g.choices_for_pin(1, "tex", true);
    CHECK(contains(types(from_tex), "ExportImage"));
    CHECK(contains(types(from_tex), "SaveTex"));
    CHECK(contains(types(from_tex), "PackageMod"));
    CHECK(contains(types(from_tex), "Text"));  // into its multiple parts input
    CHECK_FALSE(contains(types(from_tex), "EditImage"));  // takes an image, not a texture
    const auto save = std::ranges::find(from_tex, std::string("SaveTex"), [](const auto& c) { return std::string(c.spec->type); });
    CHECK(save->port == "original");

    // Dragging out of Convert's "edited PNG" input: things that output an image.
    const auto into_image = types(g.choices_for_pin(4, "image", false));
    CHECK(contains(into_image, "ExportImage"));
    CHECK(contains(into_image, "EditImage"));
    CHECK(contains(into_image, "ImportImage"));
    CHECK_FALSE(contains(into_image, "LoadTex"));

    // Package's Mod name takes text: a Text node fits.
    CHECK(contains(types(g.choices_for_pin(5, "name", false)), "Text"));

    // Add from an input that's already linked: the new node replaces the old source.
    const auto into = g.choices_for_pin(4, "image", false);
    const auto import = std::ranges::find(into, std::string("ImportImage"), [](const auto& c) { return std::string(c.spec->type); });
    const int id = g.add_connected(*import, 4, "image", false);
    REQUIRE(g.links_into(4, "image").size() == 1);
    CHECK(g.links[g.links_into(4, "image")[0]].from_node == id);
    CHECK_FALSE(g.is_connected(3, "image", true));  // the Edit PNG step's old link made way

    // From an output into a multiple input: added, nothing replaced.
    const auto pkg = std::ranges::find(from_tex, std::string("PackageMod"), [](const auto& c) { return std::string(c.spec->type); });
    const int pkg2 = g.add_connected(*pkg, 1, "tex", true);
    CHECK(g.links_into(pkg2, "tex").size() == 1);
}

TEST_CASE("editing: insert a node on a link, duplicate, disconnect") {
    Graph g;
    g.add_node("LoadTex").params["tex"] = "a.tex.143221013";  // 1
    g.add_node("ExportImage").params["png"] = "a.png";        // 2
    g.add_node("SaveTex");                                     // 3
    REQUIRE(g.connect({1, "tex", 2, "tex"}).empty());
    REQUIRE(g.connect({2, "png", 3, "image"}).empty());       // export straight into convert: no editing step
    REQUIRE(g.connect({1, "tex", 3, "original"}).empty());

    std::vector<std::string> fits;
    for (const auto* s : g.choices_for_link(1)) fits.emplace_back(s->type);
    CHECK(std::ranges::count(fits, "EditImage") == 1);  // image in, image out
    CHECK(std::ranges::count(fits, "LoadTex") == 0);

    const int edit = g.insert_node(1, "EditImage");
    CHECK(g.find(edit)->type == "EditImage");
    CHECK(g.links[g.links_into(edit, "png")[0]].from_node == 2);
    CHECK(g.links[g.links_into(3, "image")[0]].from_node == edit);
    CHECK(g.links.size() == 4);
    CHECK_THROWS_AS(g.insert_node(0, "PackageMod"), GraphError);  // no texture output to feed Export
    CHECK(g.links.size() == 4);  // unchanged after the refusal

    remod::set_edit_done(g, edit, true);
    const int copy = g.duplicate_node(edit);
    CHECK(g.find(copy)->type == "EditImage");
    CHECK_FALSE(g.find(copy)->params.contains("done"));  // run state isn't copied
    CHECK(g.find(copy)->x == g.find(edit)->x + 40);
    const int load2 = g.duplicate_node(1);
    CHECK(g.find(load2)->params.at("tex") == "a.tex.143221013");
    CHECK_FALSE(g.is_connected(load2, "tex", true));  // links aren't copied

    g.disconnect_node(3);
    CHECK_FALSE(g.is_connected(3, "image", false));
    CHECK_FALSE(g.is_connected(3, "original", false));
    CHECK(g.links.size() == 2);
}

TEST_CASE("profiles load by id") {
    CHECK(remod::load_profile_by_id(REMOD_PROFILES_DIR, "re4r").name == "Resident Evil 4 (2023)");
    CHECK_THROWS_AS(remod::load_profile_by_id(REMOD_PROFILES_DIR, "../x"), remod::ProfileError);
    CHECK_THROWS_AS(remod::load_profile_by_id(REMOD_PROFILES_DIR, "nope"), remod::ProfileError);
}
