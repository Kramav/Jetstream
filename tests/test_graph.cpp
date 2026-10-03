#include "graph.hpp"
#include "image.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdio>
#include <share.h>

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
    int mips = 0;  // non-zero: the new texture gets this many mips, like Noesis's 8x8 chain
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
        if (mips) {
            std::string b = test::read_file(out);
            b[15] = char(mips * 16);
            test::write_file(out, b);
        }
        return remod::read_tex_meta(out, p);
    }
};

bool has(const std::vector<std::string>& errors, const std::string& s) {
    return std::ranges::any_of(errors, [&](const std::string& e) { return e.find(s) != std::string::npos; });
}

// 1 LoadTex -> 6 Split -> 2 ExportImage -> 3 EditImage -> 4 SaveTex -> 5 PackageMod; the Split also feeds
// SaveTex's original. Like schemas/graph.v0.example.json.
Graph pipeline(const std::string& tex, const std::string& png, const std::string& out) {
    Graph g;
    g.add_node("LoadTex").params["tex"] = tex;
    g.add_node("ExportImage").params["png"] = png;
    g.add_node("EditImage");
    g.add_node("SaveTex");
    auto& pkg = g.add_node("PackageMod");
    pkg.params["name"] = "M";
    pkg.params["out"] = out;
    g.add_node("Split");  // 6: the original texture goes to Export and to SaveTex
    for (const remod::Link& l : {remod::Link{1, "tex", 6, "in"}, remod::Link{6, "out", 2, "tex"},
                                 remod::Link{2, "png", 3, "png"}, remod::Link{3, "image", 4, "image"},
                                 remod::Link{6, "out", 4, "original"}, remod::Link{4, "tex", 5, "tex"}})
        REQUIRE(g.connect(l).empty());
    return g;
}

}  // namespace

TEST_CASE("{game}: a path inside the game files folder is stored as {game}\\..., and filled in again") {
    remod::set_game_files_dir("C:/Games/RE4/natives/STM");
    CHECK(remod::with_game_token(R"(c:\games\re4\natives\stm\_chainsaw\ui\a.tex.143221013)") ==
          R"({game}\_chainsaw\ui\a.tex.143221013)");  // ignoring case and slashes, as Windows does
    CHECK(remod::with_game_token("D:/elsewhere/a.png") == "D:/elsewhere/a.png");
    CHECK(remod::with_game_token("C:/Games/RE4/natives/STM_old/a.tex") == "C:/Games/RE4/natives/STM_old/a.tex");
    CHECK(fs::path(remod::fill_game(R"({game}\_chainsaw\a.tex)")) == fs::path(R"(C:/Games/RE4/natives/STM\_chainsaw\a.tex)"));
    remod::set_game_files_dir({});
    CHECK(remod::with_game_token("C:/Games/RE4/natives/STM/a.tex") == "C:/Games/RE4/natives/STM/a.tex");
    CHECK_THROWS_WITH(remod::fill_game("{game}/a.tex"), ContainsSubstring("Game files folder"));
}

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
    // Its Open with field; "done" is state, absent until set.
    CHECK(g.add_node("EditImage").params == std::map<std::string, std::string>{{"editor", ""}});
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
    CHECK_FALSE(has(export_only.validate(), "Image file is required"));  // empty: a working copy
    export_only.add_node("EditImage");
    REQUIRE(export_only.connect({1, "png", 2, "png"}).empty());
    CHECK(has(export_only.validate(), "Image file is required: it's the file you edit"));
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
    g.add_node("ImportImage").params["png"] = "shot.gif";  // node 7
    REQUIRE(g.connect({7, "image", 5, "preview"}).empty());
    REQUIRE(g.validate().size() == 1);
    CHECK_THAT(g.validate()[0], ContainsSubstring("Image file must end in .png"));
    g.find(7)->params["png"] = "";  // linked-in values aren't typed values: a Text link satisfies it
    g.add_node("Text").params["text"] = "shot.png";  // node 8
    REQUIRE(g.connect({8, "text", 7, "png"}).empty());
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
    REQUIRE(back.nodes.size() == 6);
    CHECK(back.find(1)->params.at("tex") == "C:/x/a.tex.143221013");
    CHECK(back.find(2)->x == 320.5f);
    CHECK(back.find(2)->y == -40);
    CHECK(back.find(3)->params.at("done") == "true");
    REQUIRE(back.links.size() == 6);
    CHECK(back.links[4].from_node == 6);
    CHECK(back.links[4].to_port == "original");
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
    CHECK(g.nodes.size() == 7);  // its Splits are in the file, not added by migration
}

TEST_CASE("the Replace photo example graph loads and validates") {
    bool added = false;
    const Graph g = remod::load_graph(REMOD_SCHEMAS_DIR "/replace_photo.example.json", &added);
    CHECK(g.validate().empty());
    CHECK_FALSE(added);  // its Splits are in the file
    CHECK(std::ranges::count(g.nodes, std::string("ReplacePhoto"), &remod::Node::type) == 1);
    // Before any run: the replaced image's path (its Save to) is known all the way to Convert and Package.
    const auto preview = remod::preview_values(g, REMOD_SCHEMAS_DIR);
    CHECK_FALSE(preview.at({4, "image"}).empty());  // its Save to, known before a run
    CHECK(preview.at({5, "out"}) == preview.at({4, "image"}));
}

TEST_CASE("run: Convert image to texture reuses the texture converted before from the same bytes") {
    TempDir tmp;
    const fs::path tex = tmp.path / "natives/STM/_chainsaw/ui/a.tex.143221013";
    test::write_fake_tex(tex, 143221013, 64, 32, 1, 5, 99);
    Graph g = pipeline(tex.string(), "a.png", "out");
    g.find(5)->params["replace"] = "true";
    FakeConverter conv;
    const remod::RunOptions opt{
        .profile = re4r(), .converter = conv, .base_dir = tmp.path, .edits_done = true, .cache_dir = tmp.path / "cache"};

    const auto first = remod::run_graph(g, opt);
    CHECK(conv.saves == 1);
    const fs::path made = first.values.at({4, "tex"});
    CHECK(made.parent_path() == tmp.path / "cache");
    CHECK(fs::is_regular_file(made));

    const auto again = remod::run_graph(g, opt);  // nothing changed: no conversion, and the package is left as it is
    CHECK(conv.saves == 1);
    CHECK_THAT(again.nodes.at(4).message, ContainsSubstring("unchanged"));
    CHECK(again.values.at({4, "tex"}) == made.string());
    CHECK_THAT(again.nodes.at(5).message, ContainsSubstring("unchanged"));

    test::write_file(tmp.path / "a.png", test::read_file(tmp.path / "a.png") + "edited");  // the edit changed
    const auto edited = remod::run_graph(g, opt);
    CHECK(conv.saves == 2);
    CHECK(edited.values.at({4, "tex"}) != made.string());
    CHECK(std::ranges::none_of(fs::directory_iterator(tmp.path / "cache"),
                               [](const fs::directory_entry& e) { return e.path().extension() == ".part"; }));
}

TEST_CASE("guardrails: a plan lists a run's file changes; a guarded run refuses what isn't allowed") {
    TempDir tmp;
    const fs::path dir = tmp.path / "graph", outside = tmp.path / "elsewhere", game = tmp.path / "game";
    fs::create_directories(dir);
    test::write_file(dir / "a.txt", "a");
    Graph g;
    auto& copy = g.add_node("CopyFile");  // 1: out of the graph's folder
    copy.params["source"] = "a.txt";
    copy.params["dest"] = (outside / "a.txt").string();
    g.add_node("DeleteFile").params["source"] = "old.txt";  // 2
    g.add_node("MakeFolder").params["folder"] = "made";     // 3
    g.add_node("LoadTex").params["tex"] = "x.tex.143221013";  // 4 -> 5 SaveTex -> 6 Copy: known only in a run
    g.add_node("SaveTex");
    g.add_node("CopyFile").params["dest"] = "copies";
    REQUIRE(g.connect({4, "tex", 5, "original"}).empty());
    REQUIRE(g.connect({5, "tex", 6, "source"}).empty());

    const remod::ChangePlan plan = remod::plan_changes(g, dir);
    using K = remod::ChangeKind;
    const std::vector<remod::FileChange> want{{1, K::Write, outside / "a.txt"},
                                              {2, K::Remove, dir / "old.txt"},
                                              {3, K::MakeFolder, dir / "made"}};
    CHECK(plan.changes == want);
    CHECK(plan.unknown == std::vector<int>{6});  // SaveTex changes nothing of the user's; the Copy after it is unknown

    remod::Guard guard{.read_only = {game}, .graph_dir = dir};
    CHECK(guard.judge({1, K::Write, dir / "made" / "x.png"}) == remod::Guard::Verdict::Ok);
    CHECK(guard.judge({1, K::Write, outside / "a.txt"}) == remod::Guard::Verdict::NeedsApproval);
    CHECK(guard.judge({1, K::Remove, dir / "old.txt"}) == remod::Guard::Verdict::NeedsApproval);  // even inside
    std::string why;
    CHECK(guard.judge({1, K::Write, tmp.path / "GAME" / "natives" / "x.tex"}, &why) == remod::Guard::Verdict::Refused);
    CHECK_THAT(why, ContainsSubstring("game files"));
    guard.approved = {{0, K::Write, outside / "A.TXT"}};  // approval: kind and path (any case)
    CHECK(guard.judge({1, K::Write, outside / "a.txt"}) == remod::Guard::Verdict::Ok);
    guard.approved.push_back({0, K::Write, game / "x.tex"});
    CHECK(guard.judge({1, K::Write, game / "x.tex"}) == remod::Guard::Verdict::Refused);  // no approval lifts it

    Graph just_copy;
    just_copy.nodes.push_back(g.nodes[0]);
    FakeConverter conv;
    remod::Guard strict{.graph_dir = dir};
    const remod::RunOptions guarded{.profile = re4r(), .converter = conv, .base_dir = dir,
                                    .check_change = [&](const remod::FileChange& c) { return strict.check(c); }};
    try {
        remod::run_graph(just_copy, guarded);
        FAIL("expected a RunError");
    } catch (const remod::RunError& e) {
        CHECK_THAT(std::string(e.what()), ContainsSubstring("outside the graph's folder"));
    }
    CHECK_FALSE(fs::exists(outside / "a.txt"));
    strict.approved = {{1, K::Write, outside / "a.txt"}};
    remod::run_graph(just_copy, guarded);
    CHECK(fs::exists(outside / "a.txt"));
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
    SECTION("another texture: the kept PNG is from the old one, so previews skip it and a run exports over it") {
        remod::apply_run(g, built);
        CHECK(fs::path(g.find(2)->params.at("exported_from")) == tex);
        const fs::path other = tmp.path / "natives/STM/_chainsaw/ui/b.tex.143221013";
        test::write_fake_tex(other, 143221013, 64, 32, 1, 5, 99);
        g.find(1)->params["tex"] = other.string();

        fs::path loaded;
        const remod::ImageLoader load = [&](const fs::path& p) -> std::optional<remod::ImagePreview> {
            loaded = p;
            return remod::ImagePreview{{1, 1, {0, 0, 0, 255}}, 1};
        };
        remod::preview_image(g, remod::preview_values(g, tmp.path), 2, tmp.path, 256, load);
        CHECK(loaded == other);  // the new texture, not a.png

        const auto again = remod::run_graph(g, opt);
        CHECK(conv.loads == 2);
        CHECK(std::ranges::any_of(log, [](const std::string& l) { return l.find("from another texture") != std::string::npos; }));
        CHECK(again.reset_edits == std::vector<int>{3});  // a new image to edit
        remod::apply_run(g, again);
        CHECK(fs::path(g.find(2)->params.at("exported_from")) == other);
    }
    SECTION("without an Edit image the file is a working file: every run exports it, previews use the texture") {
        g.remove_node(3);
        REQUIRE(g.connect({2, "png", 4, "image"}).empty());
        g.find(5)->params["replace"] = "true";
        remod::apply_run(g, remod::run_graph(g, opt));
        CHECK(conv.loads == 2);  // a.png was there, with no record of it: exported again
        remod::apply_run(g, remod::run_graph(g, opt));
        CHECK(conv.loads == 2);  // nothing changed: no needless write
        fs::last_write_time(tmp.path / "a.png", fs::last_write_time(tmp.path / "a.png") + std::chrono::seconds(5));
        remod::run_graph(g, opt);
        CHECK(conv.loads == 3);  // changed outside the tool: exported again

        fs::path loaded;
        const remod::ImageLoader load = [&](const fs::path& p) -> std::optional<remod::ImagePreview> {
            loaded = p;
            return remod::ImagePreview{{1, 1, {0, 0, 0, 255}}, 1};
        };
        remod::preview_image(g, remod::preview_values(g, tmp.path), 2, tmp.path, 256, load);
        CHECK(loaded == tex);
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

TEST_CASE("run: a different mip count builds the mod with a warning") {
    TempDir tmp;
    FakeConverter conv;
    const remod::RunOptions opt{.profile = re4r(), .converter = conv, .base_dir = tmp.path, .edits_done = true};
    const fs::path tex = tmp.path / "natives/stm/ui/a.tex.143221013";
    test::write_fake_tex(tex, 143221013, 64, 32, 1, 1, 99);  // one mip, like most UI textures
    test::write_fake_png(tmp.path / "a.png", 64, 32);

    CHECK(remod::run_graph(pipeline(tex.string(), "a.png", "out"), opt).warnings.empty());  // same mips: quiet
    conv.mips = 4;
    const auto r = remod::run_graph(pipeline(tex.string(), "a.png", "out2"), opt);
    CHECK(fs::is_regular_file(tmp.path / "out2/M.zip"));
    REQUIRE(r.warnings.size() == 1);
    CHECK_THAT(r.warnings[0], ContainsSubstring("Convert image to texture (node 4)") &&
                                  ContainsSubstring("has 1 mip level(s), the new texture has 4"));
}

TEST_CASE("every input can be linked, with type rules") {
    Graph g;
    g.add_node("LoadTex");      // 1
    g.add_node("SaveTex");      // 2
    g.add_node("PackageMod");   // 3
    g.add_node("Text");         // 4
    g.add_node("ImportImage");  // 5
    g.add_node("Text");         // 6
    g.add_node("Text");         // 7
    g.add_node("ImportImage");  // 8
    CHECK(g.connect({4, "text", 3, "name"}).empty());  // text into a text field
    CHECK(g.connect({7, "text", 1, "tex"}).empty());   // text into an editable path field (a typed path)
    CHECK_THAT(g.connect({4, "text", 2, "original"}), ContainsSubstring("needs a texture"));  // link-only input
    CHECK(g.connect({1, "tex", 6, "parts"}).empty());   // anything into a Text input (its path)
    CHECK(g.connect({5, "image", 6, "parts"}).empty());  // multiple input: a second link
    CHECK(g.connect({8, "image", 3, "author"}).empty()); // any output into a text field
    CHECK_THAT(g.connect({5, "image", 3, "tex"}), ContainsSubstring("needs a texture"));
    CHECK_THAT(g.connect({6, "text", 3, "name"}), ContainsSubstring("already has a link"));
    CHECK(g.links_into(6, "parts").size() == 2);
}

TEST_CASE("a multiple input takes any number of links; required means at least one") {
    Graph g = pipeline("a.tex.143221013", "a.png", "out");
    g.disconnect(5);  // PackageMod's only texture
    CHECK(has(g.validate(), "input 'texture' is not connected"));
    REQUIRE(g.connect({4, "tex", 5, "tex"}).empty());
    g.add_node("SaveTex");  // node 7
    REQUIRE(g.connect({7, "tex", 5, "tex"}).empty());
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
        REQUIRE(g.nodes.size() == 5);  // and a Split: the texture goes to two steps
        const remod::Node& edit = g.nodes[3];
        CHECK(edit.type == "EditImage");
        CHECK(edit.x == 450);
        CHECK(g.is_connected(2, "png", true));
        CHECK_FALSE(g.is_connected(2, "image", true));
        REQUIRE(g.links_into(3, "image").size() == 1);
        CHECK(g.links[g.links_into(3, "image")[0]].from_node == edit.id);
        CHECK(g.nodes.back().type == "Split");
        CHECK(g.validate().empty());
    }
    SECTION("an output feeding several inputs gets a Split, which feeds them in the same order") {
        test::write_file(tmp.path / "old.json", R"({"schema_version": 0, "profile": "re4r",
          "nodes": [{"id": 1, "type": "Text", "params": {"text": "M"}, "pos": [100, 50]},
                    {"id": 2, "type": "PackageMod", "params": {"name": "", "out": ""}},
                    {"id": 3, "type": "Text", "params": {"text": ""}}],
          "links": [{"from": [1, "text"], "to": [3, "parts"]}, {"from": [1, "text"], "to": [2, "name"]},
                    {"from": [1, "text"], "to": [2, "out"]}]})");
        const Graph g = remod::load_graph(tmp.path / "old.json");
        REQUIRE(g.nodes.size() == 4);
        const remod::Node& split = g.nodes.back();
        CHECK(split.type == "Split");
        CHECK(split.x == 500);
        CHECK(g.links_from(1, "text").size() == 1);
        CHECK(g.links[g.links_into(split.id, "in")[0]].from_node == 1);
        const auto out = g.links_from(split.id, "out");
        REQUIRE(out.size() == 3);
        CHECK(g.links[out[0]].to_node == 3);
        CHECK(g.links[out[1]].to_port == "name");
        CHECK(g.links[out[2]].to_port == "out");
        CHECK_FALSE(has(g.validate(), "more than one step"));
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
    for (const char* name : {"a", "b"}) {  // LoadTex -> SaveTex <- Split <- ImportImage, per texture
        const fs::path tex = tmp.path / "natives/STM/ui" / (std::string(name) + ".tex.143221013");
        test::write_fake_tex(tex, 143221013, 64, 32, 1, 5, 99);
        remod::save_png_bgra(tmp.path / (std::string(name) + ".png"), 64, 32, std::vector<std::uint8_t>(64 * 32 * 4, 200));
        const int load = g.add_node("LoadTex").id;
        g.find(load)->params["tex"] = tex.string();
        const int img = g.add_node("ImportImage").id;
        g.find(img)->params["png"] = std::string(name) + ".png";
        const int split = g.add_node("Split").id;  // the image also goes to the preview, below
        const int save = g.add_node("SaveTex").id;
        REQUIRE(g.connect({load, "tex", save, "original"}).empty());
        REQUIRE(g.connect({img, "image", split, "in"}).empty());
        REQUIRE(g.connect({split, "out", save, "image"}).empty());
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
    for (int save : {4, 8}) REQUIRE(g.connect({save, "tex", pkg, "tex"}).empty());
    for (int split : {3, 7}) REQUIRE(g.connect({split, "out", pkg, "preview"}).empty());

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
        const int split = g.add_node("Split").id;
        REQUIRE(g.connect({load, "tex", split, "in"}).empty());
        REQUIRE(g.connect({split, "out", exp, "tex"}).empty());
        REQUIRE(g.connect({exp, "png", edit, "png"}).empty());
        REQUIRE(g.connect({edit, "image", save, "image"}).empty());
        REQUIRE(g.connect({split, "out", save, "original"}).empty());
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

    // From an output into a multiple input: added, nothing replaced. A used single output refuses a second link.
    const auto pkg = std::ranges::find(from_tex, std::string("PackageMod"), [](const auto& c) { return std::string(c.spec->type); });
    CHECK_THROWS_WITH(g.add_connected(*pkg, 1, "tex", true), ContainsSubstring("put a Split block"));
    const int pkg2 = g.add_connected(*pkg, 6, "out", true);  // the Split's output takes any number
    CHECK(g.links_into(pkg2, "tex").size() == 1);
    CHECK(g.links_from(6, "out").size() == 3);
}

TEST_CASE("editing: insert a node on a link, duplicate, disconnect") {
    Graph g;
    g.add_node("LoadTex").params["tex"] = "a.tex.143221013";  // 1
    g.add_node("ExportImage").params["png"] = "a.png";        // 2
    g.add_node("SaveTex");                                     // 3
    REQUIRE(g.connect({1, "tex", 2, "tex"}).empty());
    REQUIRE(g.connect({2, "png", 3, "image"}).empty());       // export straight into convert: no editing step

    // The texture to Convert too: a second link from its output takes a Split, put on the first link.
    CHECK_THAT(g.connect({1, "tex", 3, "original"}), ContainsSubstring("already goes to a step"));
    std::vector<std::string> fits;
    for (const auto* s : g.choices_for_link(0)) fits.emplace_back(s->type);
    CHECK(std::ranges::count(fits, "Split") == 1);  // texture in, texture out
    const int split = g.insert_node(0, "Split");  // 4
    REQUIRE(g.connect({split, "out", 3, "original"}).empty());
    CHECK(g.links_from(split, "out").size() == 2);

    const size_t png_link = g.links_into(3, "image")[0];
    fits.clear();
    for (const auto* s : g.choices_for_link(png_link)) fits.emplace_back(s->type);
    CHECK(std::ranges::count(fits, "EditImage") == 1);  // image in, image out
    CHECK(std::ranges::count(fits, "Split") == 1);
    CHECK(std::ranges::count(fits, "LoadTex") == 0);

    const int edit = g.insert_node(png_link, "EditImage");
    CHECK(g.find(edit)->type == "EditImage");
    CHECK(g.links[g.links_into(edit, "png")[0]].from_node == 2);
    CHECK(g.links[g.links_into(3, "image")[0]].from_node == edit);
    CHECK(g.links.size() == 5);
    CHECK_THROWS_AS(g.insert_node(0, "PackageMod"), GraphError);  // it has no output to feed the Split
    CHECK(g.links.size() == 5);  // unchanged after the refusal

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
    CHECK(g.links.size() == 3);
}

TEST_CASE("profiles load by id") {
    CHECK(remod::load_profile_by_id(REMOD_PROFILES_DIR, "re4r").name == "Resident Evil 4 (2023)");
    CHECK_THROWS_AS(remod::load_profile_by_id(REMOD_PROFILES_DIR, "../x"), remod::ProfileError);
    CHECK_THROWS_AS(remod::load_profile_by_id(REMOD_PROFILES_DIR, "nope"), remod::ProfileError);
}

namespace {

// 1 CopyFile -> 2 Text "{1}": the Text block's status shows the path Copy file passed on.
Graph copy_graph(const std::string& source, const std::string& dest, const char* mode = "fail") {
    Graph g;
    auto& copy = g.add_node("CopyFile");
    copy.params["source"] = source;
    copy.params["dest"] = dest;
    copy.params["if_exists"] = mode;
    g.add_node("Text").params["text"] = "{1}";
    REQUIRE(g.connect({1, "path", 2, "parts"}).empty());
    return g;
}

// Runs `g` with base_dir `dir`; the log goes to `log`.
remod::RunResult run_copy(const Graph& g, const fs::path& dir, std::vector<std::string>& log) {
    FakeConverter conv;
    return remod::run_graph(g, {.profile = re4r(), .converter = conv, .base_dir = dir,
                                .log = [&](const std::string& s) { log.push_back(s); }});
}

std::string run_error(const Graph& g, const fs::path& dir) {
    std::vector<std::string> log;
    std::string error = "(ran without an error)";
    try {
        run_copy(g, dir, log);
    } catch (const remod::RunError& e) {
        CHECK(e.nodes.at(1).state == NodeState::Failed);
        CHECK(e.nodes.at(2).state == NodeState::NotReached);  // nothing downstream sees a path
        error = e.what();
    }
    return error;
}

fs::path prefixed(const fs::path& p) { return LR"(\\?\)" + p.native(); }  // past MAX_PATH, for the test's own checks

}  // namespace

TEST_CASE("Copy file: new blocks have defaults, and every value survives save and load") {
    TempDir tmp;
    Graph g;
    CHECK(g.add_node("CopyFile").params == std::map<std::string, std::string>{
                                               {"source", ""}, {"dest", ""}, {"if_exists", "fail"}, {"create_dirs", "true"}});
    g = copy_graph("C:/in/a.txt", "\"D:\\out\\\"", "skip");
    g.find(1)->params["create_dirs"] = "";
    remod::save_graph(g, tmp.path / "g.json");
    const Graph back = remod::load_graph(tmp.path / "g.json");
    CHECK(back.find(1)->params == g.find(1)->params);
    CHECK(back.links.size() == 1);
    CHECK(back.validate().empty());

    g.find(1)->params["if_exists"] = "bogus";
    CHECK(has(g.validate(), "Overwrite mode can't be 'bogus'"));
    g.find(1)->params["source"] = "";
    CHECK(has(g.validate(), "Source file is required"));
}

TEST_CASE("run: Copy file copies once and passes on the copy's full path") {
    TempDir tmp;
    const fs::path source = tmp.path / "my file.txt";
    test::write_file(source, "hello");
    std::vector<std::string> log;

    // Pasted with quotes (Explorer's "Copy as path"), mixed separators, a relative destination in new folders.
    const auto r = run_copy(copy_graph(" \"" + source.string() + "\" ", "out/sub\\copy.txt"), tmp.path, log);
    const fs::path target = (tmp.path / "out/sub/copy.txt").lexically_normal();
    CHECK(test::read_file(target) == "hello");
    CHECK(r.nodes.at(1).file == target);
    CHECK(r.nodes.at(2).message == "\"" + target.string() + "\"");
    CHECK_FALSE(std::ranges::any_of(log, [](const std::string& l) { return l.find("warning") != std::string::npos; }));

    SECTION("into a folder: the copy keeps the source's name") {
        fs::create_directories(tmp.path / "dir");
        run_copy(copy_graph(source.string(), (tmp.path / "dir").string()), tmp.path, log);
        CHECK(test::read_file(tmp.path / "dir/my file.txt") == "hello");
        run_copy(copy_graph(source.string(), "new/"), tmp.path, log);  // a trailing separator: a folder to create
        CHECK(test::read_file(tmp.path / "new/my file.txt") == "hello");
    }
    SECTION("without Create folders, a missing folder fails") {
        Graph g = copy_graph(source.string(), "missing/copy.txt");
        g.find(1)->params["create_dirs"] = "";
        CHECK_THAT(run_error(g, tmp.path), ContainsSubstring("destination folder doesn't exist"));
        CHECK_FALSE(fs::exists(tmp.path / "missing"));
    }
    SECTION("paths past 260 characters, both ways") {
        const fs::path deep = tmp.path / std::string(100, 'a') / std::string(100, 'b') / (std::string(100, 'c') + ".txt");
        REQUIRE(deep.native().size() > 300);
        run_copy(copy_graph(source.string(), deep.string()), tmp.path, log);
        CHECK(fs::is_regular_file(prefixed(deep)));
        const auto back = run_copy(copy_graph(deep.string(), "back.txt"), tmp.path, log);
        CHECK(test::read_file(tmp.path / "back.txt") == "hello");
        CHECK(back.nodes.at(1).file == tmp.path / "back.txt");
        fs::remove_all(prefixed(tmp.path / std::string(100, 'a')));  // TempDir's cleanup can't
    }
}

TEST_CASE("run: Copy file's overwrite modes, each logging that the destination exists") {
    TempDir tmp;
    const fs::path source = tmp.path / "a.txt", dest = tmp.path / "b.txt";
    test::write_file(source, "new");
    test::write_file(dest, "old");
    std::vector<std::string> log;
    auto warned = [&](const std::string& action) {
        return std::ranges::count_if(log, [&](const std::string& l) {
                   return l.find("Copy file (node 1): warning: destination exists: " + dest.string() + " (" + action + ")") !=
                          std::string::npos;
               }) == 1;
    };

    SECTION("Fail if exists") {
        try {
            run_copy(copy_graph(source.string(), dest.string(), "fail"), tmp.path, log);
            FAIL("expected a RunError");
        } catch (const remod::RunError& e) {
            CHECK_THAT(std::string(e.what()), ContainsSubstring("destination exists"));
            CHECK(e.nodes.at(2).state == NodeState::NotReached);
        }
        CHECK(warned("failed"));
        CHECK(test::read_file(dest) == "old");
    }
    SECTION("Overwrite") {
        const auto r = run_copy(copy_graph(source.string(), dest.string(), "overwrite"), tmp.path, log);
        CHECK(warned("overwritten"));
        CHECK(test::read_file(dest) == "new");
        CHECK(r.nodes.at(2).message == "\"" + dest.string() + "\"");
    }
    SECTION("Skip if exists: the existing file is passed on") {
        const auto r = run_copy(copy_graph(source.string(), dest.string(), "skip"), tmp.path, log);
        CHECK(warned("skipped"));
        CHECK(test::read_file(dest) == "old");
        CHECK_THAT(r.nodes.at(1).message, ContainsSubstring("kept existing"));
        CHECK(r.nodes.at(2).message == "\"" + dest.string() + "\"");
    }
}

TEST_CASE("run: Copy file fails clearly") {
    TempDir tmp;
    const fs::path source = tmp.path / "a.txt";
    test::write_file(source, "x");
    CHECK_THAT(run_error(copy_graph((tmp.path / "nope.txt").string(), "b.txt"), tmp.path),
               ContainsSubstring("Copy file (node 1): source file not found"));
    CHECK_THAT(run_error(copy_graph(tmp.path.string(), "b.txt"), tmp.path), ContainsSubstring("is a folder"));
    for (const char* mode : {"fail", "overwrite", "skip"}) {
        CHECK_THAT(run_error(copy_graph(source.string(), "./A.TXT", mode), tmp.path), ContainsSubstring("same file"));
        CHECK_THAT(run_error(copy_graph(source.string(), tmp.path.string(), mode), tmp.path), ContainsSubstring("same file"));
    }
    CHECK(test::read_file(source) == "x");

    // Not writable: open in another program that doesn't share it. (A read-only file doesn't count: MSVC's
    // copy_file overwrites it.)
    const fs::path locked = tmp.path / "locked.txt";
    test::write_file(locked, "keep");
    std::FILE* holder = _wfsopen(locked.c_str(), L"rb", _SH_DENYRW);
    REQUIRE(holder);
    CHECK_THAT(run_error(copy_graph(source.string(), locked.string(), "overwrite"), tmp.path), ContainsSubstring("can't write"));
    std::fclose(holder);
    CHECK(test::read_file(locked) == "keep");

    Graph linked_mode = copy_graph(source.string(), "b.txt");  // a linked mode can say anything
    linked_mode.add_node("Text").params["text"] = "bogus";      // node 3
    REQUIRE(linked_mode.connect({3, "text", 1, "if_exists"}).empty());
    CHECK_THAT(run_error(linked_mode, tmp.path), ContainsSubstring("Overwrite mode can't be 'bogus'"));
}

TEST_CASE("Copy file: the editor's destination-exists warning follows the file") {
    TempDir tmp;
    const fs::path source = tmp.path / "a.txt", dest = (tmp.path / "out/a.txt").lexically_normal();
    test::write_file(source, "x");
    Graph g = copy_graph(source.string(), "\"" + dest.string() + "\"");
    CHECK(remod::destination_warnings(g, tmp.path).empty());
    test::write_file(dest, "y");
    CHECK(remod::destination_warnings(g, tmp.path) == std::map<int, std::string>{{1, "Destination exists: " + dest.string()}});
    fs::remove(dest);
    CHECK(remod::destination_warnings(g, tmp.path).empty());

    test::write_file(dest, "y");
    g.find(1)->params["dest"] = "out";  // a folder: the source's name is added
    CHECK(remod::destination_warnings(g, tmp.path).size() == 1);
    g.find(1)->params["dest"] = "C:\\bad:name?";  // half-typed nonsense: no warning, no exception
    CHECK(remod::destination_warnings(g, tmp.path).empty());

    // Values only known at run time: no guess.
    g.find(1)->params["dest"] = "out";
    g.add_node("Text").params["text"] = source.string();  // node 3
    REQUIRE(g.connect({3, "text", 1, "source"}).empty());
    CHECK(remod::destination_warnings(g, tmp.path).empty());  // linked source into a folder: name unknown
    g.find(1)->params["dest"] = "out/a.txt";
    CHECK(remod::destination_warnings(g, tmp.path).size() == 1);  // a file path doesn't depend on the source
    g.disconnect(1);
    g.flip(1, "dest");  // circle to the left, so a link can set it
    REQUIRE(g.connect({3, "text", 1, "dest"}).empty());
    CHECK(remod::destination_warnings(g, tmp.path).empty());  // linked destination
}

TEST_CASE("a Split passes on whatever is linked into it, and types are checked through it") {
    Graph g;
    g.add_node("LoadTex");      // 1
    g.add_node("ImportImage");  // 2
    g.add_node("Split");        // 3
    g.add_node("Split");        // 4, after 3
    g.add_node("SaveTex");      // 5
    g.add_node("Text");         // 6
    CHECK(g.output_type(3, "out") == remod::PortType::Any);  // nothing linked in yet
    REQUIRE(g.connect({3, "out", 4, "in"}).empty());
    REQUIRE(g.connect({4, "out", 5, "original"}).empty());  // fine while the type is open
    CHECK(has(g.validate(), "input 'in' is not connected"));  // an open type is no error in itself
    CHECK_FALSE(has(g.validate(), "needs"));
    CHECK_THAT(g.connect({2, "image", 3, "in"}), ContainsSubstring("a Split would pass an image on to 'original texture'"));
    REQUIRE(g.connect({1, "tex", 3, "in"}).empty());
    CHECK(g.output_type(4, "out") == remod::PortType::Tex);  // followed back through both Splits
    CHECK_THAT(g.connect({4, "out", 5, "image"}), ContainsSubstring("that input needs an image"));
    CHECK(g.connect({4, "out", 6, "parts"}).empty());  // a texture into text: its path

    const auto onto_split = g.choices_for_link(1);  // Split -> Split carries a texture: texture steps fit
    CHECK(std::ranges::count(onto_split, std::string("SaveTex"), [](const auto* s) { return std::string(s->type); }) == 1);
    CHECK(std::ranges::count(onto_split, std::string("EditImage"), [](const auto* s) { return std::string(s->type); }) == 0);
}

TEST_CASE("step_order follows the links; families set each block's outline") {
    Graph g;
    g.add_node("SaveTex");  // 1, listed first but fed by 2
    g.add_node("LoadTex");  // 2
    REQUIRE(g.connect({2, "tex", 1, "original"}).empty());
    CHECK(remod::step_order(g) == std::vector<int>{2, 1});

    using remod::Family;
    CHECK(remod::find_spec("LoadTex")->family == Family::Source);
    CHECK(remod::find_spec("EditImage")->family == Family::Manual);
    CHECK(remod::find_spec("Split")->family == Family::Flow);
    CHECK(remod::find_spec("PackageMod")->family == Family::Output);
    for (const auto& spec : remod::node_specs())  // manual steps and only they are Manual; utilities draw as values
        CHECK((spec.family == Family::Manual) == spec.manual);
}

TEST_CASE("known_value: typed, or from a Value through Splits; a step's result is unknown before a run") {
    Graph g;
    g.add_node("EditImage");  // 1
    g.add_node("Value");      // 2
    g.add_node("Split");      // 3
    g.add_node("EditImage");  // 4
    g.add_node("Text");       // 5
    g.find(1)->params["editor"] = "C:/Tools/gimp.exe";
    CHECK(remod::known_value(g, 1, "editor") == "C:/Tools/gimp.exe");
    CHECK(remod::known_value(g, 4, "editor").empty());  // nothing typed

    g.find(2)->params["value"] = "D:/GIMP/gimp-2.10.exe";
    REQUIRE(g.connect({2, "value", 3, "in"}).empty());
    REQUIRE(g.connect({3, "out", 1, "editor"}).empty());
    REQUIRE(g.connect({3, "out", 4, "editor"}).empty());
    CHECK(remod::known_value(g, 1, "editor") == "D:/GIMP/gimp-2.10.exe");  // the link wins over the typed value
    CHECK(remod::known_value(g, 4, "editor") == "D:/GIMP/gimp-2.10.exe");

    g.disconnect(0);  // Value -> Split: the Split has nothing to pass on
    CHECK(remod::known_value(g, 4, "editor").empty());
    REQUIRE(g.connect({5, "text", 3, "in"}).empty());  // a step's result: only a run knows it
    CHECK(remod::known_value(g, 4, "editor").empty());
}

TEST_CASE("History: each settled change is one undo step; a new change drops the redo steps") {
    Graph g;
    remod::History h;
    h.reset(g);
    CHECK_FALSE(h.can_undo());
    g.add_node("Value");
    h.track(g);
    h.track(g);  // unchanged: no step
    g.find(1)->params["value"] = "D:/mods";
    g.find(1)->x = 40;
    h.track(g);
    REQUIRE(h.undo(g));
    CHECK(g.find(1)->params["value"].empty());
    CHECK(g.find(1)->x == 0);  // positions too
    REQUIRE(h.undo(g));
    CHECK(g.nodes.empty());
    CHECK_FALSE(h.undo(g));
    REQUIRE(h.redo(g));
    CHECK(g.nodes.size() == 1);
    CHECK(h.can_redo());
    g.add_node("Text");  // a new change after an undo
    h.track(g);
    CHECK_FALSE(h.can_redo());
    h.reset(g);
    CHECK_FALSE(h.can_undo());
}

TEST_CASE("path_fit: which dropped paths a field takes") {
    using remod::PathKind;
    CHECK(remod::path_fit(PathKind::Folder, nullptr, "E:/mods/out", true).empty());
    CHECK_THAT(remod::path_fit(PathKind::Folder, nullptr, "E:/mods/a.zip", false), ContainsSubstring("needs a folder"));
    CHECK(remod::path_fit(PathKind::SaveFile, "png,tga,jpg", "E:/work/edit.PNG", false).empty());
    CHECK_THAT(remod::path_fit(PathKind::OpenFile, "png,tga,jpg", "E:/work/edit.dds", false),
               ContainsSubstring(".png, .tga, .jpg"));
    CHECK_THAT(remod::path_fit(PathKind::OpenFile, nullptr, "E:/work", true), ContainsSubstring("needs a file"));
    CHECK(remod::path_fit(PathKind::OpenTexture, "tex,143221013", "C:/x/cha000_albd.tex.143221013", false).empty());
    CHECK(remod::path_fit(PathKind::OpenTexture, "tex,143221013", "C:/x/a.tex", false).empty());
    CHECK_THAT(remod::path_fit(PathKind::None, nullptr, "C:/x/a.tex", false), ContainsSubstring("takes text"));
}

TEST_CASE("link kinds: paths and folders") {
    using remod::PortType;
    const remod::NodeSpec& copy = *remod::find_spec("CopyFile");
    const remod::InputSpec& source = *remod::find_input(copy, "source");  // a Path
    for (PortType t : {PortType::Tex, PortType::Image, PortType::Text, PortType::Path, PortType::Folder})
        CHECK(remod::accepts(source, t));  // any file (a texture is one); a folder fails at run time, clearly
    const remod::InputSpec& out = *remod::find_input(*remod::find_spec("PackageMod"), "out");  // a Folder
    CHECK(remod::accepts(out, PortType::Text));
    CHECK(remod::accepts(out, PortType::Folder));
    CHECK_FALSE(remod::accepts(out, PortType::Path));  // a file isn't a folder
    CHECK_FALSE(remod::accepts(out, PortType::Tex));
    const remod::NodeSpec& load = *remod::find_spec("LoadTex");
    CHECK(remod::accepts(*remod::find_input(load, "tex"), PortType::Path));  // a typed texture path field
    CHECK_FALSE(remod::accepts(*remod::find_input(*remod::find_spec("SaveTex"), "original"), PortType::Path));  // link-only

    Graph g;
    g.add_node("CopyFile");  // 1
    g.add_node("LoadTex");   // 2
    CHECK(g.output_type(1, "path") == PortType::Path);
    CHECK(g.connect({1, "path", 2, "tex"}).empty());  // copy a texture, then use the copy
}

TEST_CASE("a Value is kept in the graph and becomes the kind of field it feeds") {
    TempDir tmp;
    const fs::path tex = tmp.path / "natives/STM/ui/a.tex.143221013";
    test::write_fake_tex(tex, 143221013, 64, 32, 1, 5, 99);
    Graph g = pipeline(tex.string(), "a.png", "typed, overridden by the link");
    remod::set_edit_done(g, 3, true);
    test::write_fake_png(tmp.path / "a.png", 64, 32);
    g.add_node("Value").params["value"] = "builds";  // 7: the output folder
    g.add_node("Value").params["value"] = "2.0";     // 8: one version, to two fields
    g.add_node("Split");                             // 9
    using remod::PortType;
    CHECK(g.output_type(7, "value") == PortType::Any);  // open until connected
    g.flip(5, "out");  // the output folder from a link: its circle on the left
    REQUIRE(g.connect({7, "value", 5, "out"}).empty());
    CHECK(g.output_type(7, "value") == PortType::Folder);
    CHECK(remod::picker_for(g.output_type(7, "value")) == remod::PathKind::Folder);
    REQUIRE(g.connect({8, "value", 9, "in"}).empty());
    REQUIRE(g.connect({9, "out", 5, "version"}).empty());
    REQUIRE(g.connect({9, "out", 5, "author"}).empty());
    CHECK(g.output_type(9, "out") == PortType::Text);  // through the Split, from what it feeds
    CHECK(remod::picker_for(PortType::Text) == remod::PathKind::None);

    remod::save_graph(g, tmp.path / "g.json");  // the values live in the graph file
    const Graph back = remod::load_graph(tmp.path / "g.json");
    CHECK(back.find(7)->params.at("value") == "builds");
    FakeConverter conv;
    const auto r = remod::run_graph(back, {.profile = re4r(), .converter = conv, .base_dir = tmp.path});
    CHECK_FALSE(r.paused);
    CHECK(fs::is_regular_file(tmp.path / "builds/M.zip"));  // relative to the graph's folder, like a typed one
    CHECK_THAT(test::read_file(tmp.path / "builds/M/modinfo.ini"), ContainsSubstring("version=2.0") && ContainsSubstring("author=2.0"));

    g.find(7)->params["value"] = "";
    CHECK(has(g.validate(), "Value is required"));
    Graph wrong;  // a Value already feeding a folder can't also feed a texture-only input
    wrong.add_node("Value");    // 1
    wrong.add_node("Split");    // 2
    wrong.add_node("PackageMod");  // 3
    wrong.add_node("SaveTex");  // 4
    REQUIRE(wrong.connect({1, "value", 2, "in"}).empty());
    wrong.flip(3, "out");
    REQUIRE(wrong.connect({2, "out", 3, "out"}).empty());
    CHECK_THAT(wrong.connect({2, "out", 4, "original"}), ContainsSubstring("needs a texture"));
}

namespace {

// A Text "{1}" block showing what `node`'s `port` passes on: its status reads "\"<value>\"" after a run.
int observe(Graph& g, int node, const std::string& port) {
    const int id = g.add_node("Text").id;
    g.find(id)->params["text"] = "{1}";
    REQUIRE(g.connect({node, port, id, "parts"}).empty());
    return id;
}

remod::RunResult run_in(const Graph& g, const fs::path& dir, std::vector<std::string>* log = nullptr) {
    FakeConverter conv;
    return remod::run_graph(g, {.profile = re4r(), .converter = conv, .base_dir = dir,
                                .log = [log](const std::string& s) { if (log) log->push_back(s); }});
}

std::string run_fails(const Graph& g, const fs::path& dir) {
    try {
        run_in(g, dir);
    } catch (const remod::RunError& e) {
        return e.what();
    }
    return "(ran without an error)";
}

}  // namespace

TEST_CASE("Move file moves once, with Copy file's overwrite modes") {
    TempDir tmp;
    test::write_file(tmp.path / "a.txt", "new");
    Graph g;
    auto& move = g.add_node("MoveFile");
    move.params["source"] = "a.txt";
    move.params["dest"] = "out\\";  // a folder to create
    const int seen = observe(g, 1, "path");
    const auto r = run_in(g, tmp.path);
    const fs::path target = (tmp.path / "out/a.txt").lexically_normal();
    CHECK(test::read_file(target) == "new");
    CHECK_FALSE(fs::exists(tmp.path / "a.txt"));
    CHECK(r.nodes.at(seen).message == "\"" + target.string() + "\"");
    CHECK_THAT(run_fails(g, tmp.path), ContainsSubstring("moved by an earlier run?"));

    test::write_file(tmp.path / "a.txt", "newer");
    std::vector<std::string> log;
    g.find(1)->params["if_exists"] = "skip";
    run_in(g, tmp.path, &log);
    CHECK(test::read_file(target) == "new");  // kept, and the source stays where it was
    CHECK(fs::exists(tmp.path / "a.txt"));
    CHECK_THAT(log[0], ContainsSubstring("destination exists") && ContainsSubstring("(skipped)"));
    g.find(1)->params["if_exists"] = "overwrite";
    run_in(g, tmp.path);
    CHECK(test::read_file(target) == "newer");
    CHECK_FALSE(fs::exists(tmp.path / "a.txt"));
}

TEST_CASE("Rename file renames in its folder") {
    TempDir tmp;
    test::write_file(tmp.path / "a.png", "x");
    Graph g;
    auto& ren = g.add_node("RenameFile");
    ren.params["source"] = "a.png";
    ren.params["name"] = "b.png";
    CHECK(remod::destination_warnings(g, tmp.path).empty());
    test::write_file(tmp.path / "b.png", "old");
    CHECK(remod::destination_warnings(g, tmp.path).size() == 1);  // the editor warns, as for Copy and Move
    CHECK_THAT(run_fails(g, tmp.path), ContainsSubstring("destination exists"));
    fs::remove(tmp.path / "b.png");
    CHECK(run_in(g, tmp.path).nodes.at(1).message == "renamed to b.png");
    CHECK(test::read_file(tmp.path / "b.png") == "x");
    CHECK_FALSE(fs::exists(tmp.path / "a.png"));
    g.find(1)->params["source"] = "b.png";
    g.find(1)->params["name"] = "sub\\c.png";
    CHECK_THAT(run_fails(g, tmp.path), ContainsSubstring("just a name"));
}

TEST_CASE("Delete file deletes files only, and a missing one is fine unless you say otherwise") {
    TempDir tmp;
    test::write_file(tmp.path / "a.txt", "x");
    Graph g;
    auto& del = g.add_node("DeleteFile");
    del.params["source"] = "a.txt";
    del.params["recycle"] = "";  // for good: the test doesn't fill your Recycle Bin
    CHECK(run_in(g, tmp.path).nodes.at(1).message == "deleted a.txt");
    CHECK_FALSE(fs::exists(tmp.path / "a.txt"));
    CHECK(run_in(g, tmp.path).nodes.at(1).message == "already gone: a.txt");
    g.find(1)->params["missing_ok"] = "";
    CHECK_THAT(run_fails(g, tmp.path), ContainsSubstring("not found"));
    fs::create_directories(tmp.path / "dir");
    g.find(1)->params["source"] = "dir";
    CHECK_THAT(run_fails(g, tmp.path), ContainsSubstring("is a folder"));
    CHECK(fs::is_directory(tmp.path / "dir"));
}

TEST_CASE("Make folder makes it once; Join path builds on it and becomes the kind it feeds") {
    TempDir tmp;
    const fs::path tex = tmp.path / "natives/STM/ui/a.tex.143221013";
    test::write_fake_tex(tex, 143221013, 64, 32, 1, 5, 99);
    Graph g = pipeline(tex.string(), "a.png", "typed, overridden by the link");
    remod::set_edit_done(g, 3, true);
    test::write_fake_png(tmp.path / "a.png", 64, 32);
    g.add_node("MakeFolder").params["folder"] = "builds/v1";  // 7
    auto& join = g.add_node("JoinPath");                      // 8
    join.params["add"] = "fluffy";
    REQUIRE(g.connect({7, "folder", 8, "folder"}).empty());
    g.flip(5, "out");
    REQUIRE(g.connect({8, "path", 5, "out"}).empty());
    CHECK(g.output_type(8, "path") == remod::PortType::Folder);
    const auto r = run_in(g, tmp.path);
    CHECK(fs::is_regular_file(tmp.path / "builds/v1/fluffy/M.zip"));
    CHECK_THAT(r.nodes.at(7).message, ContainsSubstring("made"));
    CHECK_THAT(run_fails(g, tmp.path), ContainsSubstring("Replace existing"));  // a second run: folder already there,
    g.find(5)->params["replace"] = "true";                                      // the build is what stops it
    CHECK_THAT(run_in(g, tmp.path).nodes.at(7).message, ContainsSubstring("already there"));

    g.find(8)->params["add"] = "D:\\elsewhere";
    CHECK_THAT(run_fails(g, tmp.path), ContainsSubstring("must be a relative path"));
    test::write_file(tmp.path / "file", "x");
    g.find(7)->params["folder"] = "file";
    CHECK_THAT(run_fails(g, tmp.path), ContainsSubstring("is a file, not a folder"));
}

TEST_CASE("Path parts, Change extension and Require file") {
    TempDir tmp;
    Graph g;
    g.add_node("PathParts").params["path"] = "\"sub\\shot.tga\"";  // 1
    const int folder = observe(g, 1, "folder"), name = observe(g, 1, "name"), stem = observe(g, 1, "stem"),
              ext = observe(g, 1, "extension");
    auto& change = g.add_node("ChangeExtension");  // 6
    change.params["path"] = "sub/shot.tga";
    change.params["ext"] = "png";
    const int changed = observe(g, 6, "path");
    const auto r = run_in(g, tmp.path);
    CHECK(r.nodes.at(folder).message == "\"" + (tmp.path / "sub").lexically_normal().string() + "\"");
    CHECK(r.nodes.at(name).message == "\"shot.tga\"");
    CHECK(r.nodes.at(stem).message == "\"shot\"");
    CHECK(r.nodes.at(ext).message == "\".tga\"");
    CHECK(r.nodes.at(changed).message == "\"" + (tmp.path / "sub/shot.png").lexically_normal().string() + "\"");
    CHECK(remod::find_spec("PathParts")->utility);
    CHECK_FALSE(remod::find_spec("MoveFile")->utility);

    Graph req;
    req.add_node("RequireFile").params["file"] = "needed.png";
    const int after = observe(req, 1, "file");
    CHECK_THAT(run_fails(req, tmp.path), ContainsSubstring("required file missing"));
    test::write_file(tmp.path / "needed.png", "x");
    CHECK(run_in(req, tmp.path).nodes.at(after).message == "\"" + (tmp.path / "needed.png").string() + "\"");
}

TEST_CASE("cut_text cuts at a marker, ignoring case and slash direction") {
    using remod::CutKeep;
    const std::string p = R"(E:\REtool\re_chunk_000\natives\STM\_chainsaw\ui\tex\a.tex.143221013)";
    CHECK(remod::cut_text(p, "natives/stm/", CutKeep::After, false) == R"(_chainsaw\ui\tex\a.tex.143221013)");
    CHECK(remod::cut_text(p, "natives/stm/", CutKeep::Before, false) == R"(E:\REtool\re_chunk_000\)");
    CHECK(remod::cut_text(p, "NATIVES\\STM", CutKeep::From, false) == R"(natives\STM\_chainsaw\ui\tex\a.tex.143221013)");
    CHECK(remod::cut_text(p, "natives/stm", CutKeep::UpTo, false) == R"(E:\REtool\re_chunk_000\natives\STM)");
    CHECK(remod::cut_text("a/b/c", "/", CutKeep::After, false) == "b/c");
    CHECK(remod::cut_text("a/b/c", "/", CutKeep::After, true) == "c");  // the last one
    CHECK_FALSE(remod::cut_text(p, "streaming/", CutKeep::After, false));
    CHECK_FALSE(remod::cut_text(p, "", CutKeep::After, false));
}

TEST_CASE("run: Cut text passes on the part it keeps, and stops clearly without its marker") {
    TempDir tmp;
    Graph g;
    auto& cut = g.add_node("CutText");  // 1
    cut.params["text"] = "C:/x/natives/stm/_chainsaw/ui/a.tex.143221013";
    cut.params["marker"] = "Natives\\STM\\";
    CHECK(cut.params.at("keep") == "after");  // defaults
    CHECK(cut.params.at("occurrence") == "first");
    const int out = observe(g, 1, "text");
    CHECK(run_in(g, tmp.path).nodes.at(out).message == "\"_chainsaw/ui/a.tex.143221013\"");
    g.find(1)->params["marker"] = "streaming/";
    CHECK_THAT(run_fails(g, tmp.path), ContainsSubstring("'streaming/' isn't in"));
}

TEST_CASE("preview_values: what links hold before a run; link_value prefers it to the last run's") {
    TempDir tmp;
    Graph g;
    auto& load = g.add_node("LoadTex");  // 1
    load.params["tex"] = "tex/ui_main.tex.143221013";
    auto& exp = g.add_node("ExportImage");  // 2
    exp.params["png"] = "work/ui_main.png";
    g.add_node("EditImage");  // 3
    g.add_node("SaveTex");    // 4
    auto& pkg = g.add_node("PackageMod");  // 5
    pkg.params["name"] = "CleanHUD";
    pkg.params["out"] = "build";
    auto& cut = g.add_node("CutText");  // 6
    cut.params["marker"] = "tex/";
    auto& copy = g.add_node("CopyFile");  // 7
    copy.params["dest"] = "backup\\";  // a folder: the copy keeps the source's name
    g.add_node("Split");  // 8: the texture to Export image, Save texture's original and Cut text
    REQUIRE(g.connect({1, "tex", 8, "in"}).empty());
    REQUIRE(g.connect({8, "out", 2, "tex"}).empty());
    REQUIRE(g.connect({8, "out", 4, "original"}).empty());
    REQUIRE(g.connect({8, "out", 6, "text"}).empty());
    REQUIRE(g.connect({2, "png", 3, "png"}).empty());
    REQUIRE(g.connect({3, "image", 4, "image"}).empty());
    REQUIRE(g.connect({4, "tex", 5, "tex"}).empty());
    auto& note = g.add_node("Text");  // 9
    note.params["text"] = "x";
    REQUIRE(g.connect({9, "text", 7, "source"}).empty());

    const remod::RunValues v = remod::preview_values(g, tmp.path);
    auto at = [&](int node, const char* port) {
        const auto it = v.find({node, port});
        return it == v.end() ? std::string("(unknown)") : it->second;
    };
    CHECK(at(1, "tex") == (tmp.path / "tex/ui_main.tex.143221013").lexically_normal().string());
    CHECK(at(8, "out") == at(1, "tex"));                                       // through the Split
    CHECK(at(2, "png") == (tmp.path / "work/ui_main.png").lexically_normal().string());
    CHECK(at(3, "image") == at(2, "png"));                                   // Edit image passes it on
    CHECK(at(4, "tex") == "(unknown)");                                      // a temporary file, only in a run
    CHECK(at(5, "mod") == (tmp.path / "build" / "CleanHUD").lexically_normal().string() + ".zip");  // name + folder
    CHECK(at(6, "text") == "ui_main.tex.143221013");                         // Cut text runs for real
    CHECK(at(7, "path") == (tmp.path / "backup" / "x").lexically_normal().string());
    CHECK(fs::is_empty(tmp.path));                                 // and nothing was written

    remod::RunValues last{{{4, "tex"}, "C:/temp/run/4.tex.143221013"}, {{2, "png"}, "old.png"}};
    bool from_run = true;
    const size_t export_to_edit = g.links_into(3, "png").at(0), save_to_package = g.links_into(5, "tex").at(0);
    CHECK(remod::link_value(g, v, last, export_to_edit, &from_run) == at(2, "png"));  // the preview is current
    CHECK_FALSE(from_run);
    CHECK(remod::link_value(g, v, last, save_to_package, &from_run) == "C:/temp/run/4.tex.143221013");
    CHECK(from_run);
    CHECK(remod::link_value(g, v, {}, save_to_package).empty());

    Graph texts;  // a run records what every output gave
    texts.add_node("Text").params["text"] = "Clean HUD";
    texts.add_node("Text");
    REQUIRE(texts.connect({1, "text", 2, "text"}).empty());
    CHECK(run_in(texts, tmp.path).values.at({1, "text"}) == "Clean HUD");
}

TEST_CASE("run: the image blocks write PNGs; Save to keeps one and is the output, else it's temporary") {
    TempDir tmp;
    remod::Bgra pic{8, 4, {}};
    for (int i = 0; i < 8 * 4; ++i) pic.pixels.insert(pic.pixels.end(), {40, 60, 100, 255});
    remod::save_png(tmp.path / "pic.png", pic);

    Graph g;
    auto& adjust = g.add_node("AdjustColour");  // 1
    adjust.params["image"] = "pic.png";
    adjust.params["brightness"] = "20";
    adjust.params["save_to"] = "out/bright.png";
    const int seen = observe(g, 1, "image");
    const fs::path kept = (tmp.path / "out/bright.png").lexically_normal();
    CHECK(remod::preview_values(g, tmp.path).at({1, "image"}) == kept.string());  // known before the run
    CHECK(run_in(g, tmp.path).nodes.at(seen).message == "\"" + kept.string() + "\"");
    CHECK(remod::load_image(kept).pixels[2] > 100);  // red got brighter
    CHECK(remod::load_image(kept).pixels[3] == 255);

    g.find(1)->params["save_to"] = "";  // temporary: the run works, the file is gone afterwards
    CHECK_FALSE(remod::preview_values(g, tmp.path).contains({1, "image"}));
    const auto r = run_in(g, tmp.path);
    CHECK_FALSE(fs::exists(r.values.at({1, "image"})));
    {  // with a run cache: kept there, named by its pixels, so a second run finds it and writes nothing
        FakeConverter conv;
        const remod::RunOptions cached{.profile = re4r(), .converter = conv, .base_dir = tmp.path,
                                       .cache_dir = tmp.path / "cache"};
        const fs::path made = remod::run_graph(g, cached).values.at({1, "image"});
        CHECK(made.parent_path() == tmp.path / "cache");
        CHECK(remod::run_graph(g, cached).values.at({1, "image"}) == made.string());
        CHECK(remod::load_image(made).pixels[2] > 100);
        g.find(1)->params["brightness"] = "40";  // a different result: another file
        CHECK(remod::run_graph(g, cached).values.at({1, "image"}) != made.string());
    }
    g.find(1)->params["hue"] = "lots";
    CHECK_THAT(run_fails(g, tmp.path), ContainsSubstring("Hue must be a number from -180 to 180"));

    Graph resize;
    auto& rs = resize.add_node("ResizeImage");
    rs.params["image"] = "pic.png";
    rs.params["save_to"] = "r.png";
    CHECK_THAT(run_fails(resize, tmp.path), ContainsSubstring("Width and/or Height"));
    rs.params["width"] = "10";  // height from the shape: 8x4 -> 10x5
    run_in(resize, tmp.path);
    CHECK(remod::load_image(tmp.path / "r.png").height == 5);
    test::write_fake_tex(tmp.path / "orig.tex.143221013", 143221013, 64, 32, 1, 1, 98);
    resize.find(1)->params["match"] = "orig.tex.143221013";  // a texture's size wins
    run_in(resize, tmp.path);
    CHECK(remod::load_image(tmp.path / "r.png").width == 64);
    CHECK(remod::load_image(tmp.path / "r.png").height == 32);

    Graph overlay;
    auto& ov = overlay.add_node("OverlayImage");
    ov.params["base"] = "pic.png";
    ov.params["top"] = "r.png";  // bigger than the base: clipped
    ov.params["x"] = "-4";
    ov.params["save_to"] = "o.png";
    run_in(overlay, tmp.path);
    const remod::Bgra o = remod::load_image(tmp.path / "o.png");
    CHECK(o.width == 8);  // the base's size
    CHECK(o.height == 4);
}

TEST_CASE("preview_image: an image block's result in memory, at thumbnail size, before any run") {
    TempDir tmp;
    remod::Bgra pic{64, 32, {}};
    for (int i = 0; i < 64 * 32; ++i) pic.pixels.insert(pic.pixels.end(), {40, 60, 100, 255});
    remod::save_png(tmp.path / "pic.png", pic);
    const remod::ImageLoader load = [](const fs::path& f) -> std::optional<remod::ImagePreview> {  // shrunk to 16
        const remod::Bgra full = remod::load_image(f);
        const float k = std::min(1.0f, 16.0f / float(std::max(full.width, full.height)));
        return remod::ImagePreview{remod::resize_image(full, unsigned(full.width * k), unsigned(full.height * k),
                                                       remod::Fit::Stretch),
                                   k};
    };

    Graph g;
    auto& adjust = g.add_node("AdjustColour");  // 1
    adjust.params["image"] = "pic.png";
    adjust.params["brightness"] = "50";
    auto& resize = g.add_node("ResizeImage");  // 2: from Adjust colour, no file in between
    resize.params["width"] = "128";
    REQUIRE(g.connect({1, "image", 2, "image"}).empty());
    auto& overlay = g.add_node("OverlayImage");  // 3: the resized one on the original
    overlay.params["base"] = "pic.png";
    overlay.params["x"] = "32";
    REQUIRE(g.connect({2, "image", 3, "top"}).empty());
    const auto preview = remod::preview_values(g, tmp.path);
    auto at = [&](int node) { return remod::preview_image(g, preview, node, tmp.path, 16, load); };

    const auto a = at(1);
    REQUIRE(a);
    CHECK(a->image.width == 16);  // the shrunk copy, adjusted
    CHECK(a->image.pixels[2] > 100);
    const auto r = at(2);
    REQUIRE(r);
    CHECK(r->image.width == 16);  // 128x64 for real, shown at 16x8
    CHECK(r->image.height == 8);
    CHECK(r->scale == 0.125f);
    const auto o = at(3);
    REQUIRE(o);
    CHECK(o->image.width == 16);  // the base's thumbnail; the top went on at the base's scale, from x = 32 -> 8
    CHECK(o->image.pixels[(0 * 16 + 7) * 4 + 2] == 100);  // left of x: the base as it was
    CHECK(o->image.pixels[(0 * 16 + 8) * 4 + 2] > 100);   // from x: the brighter top

    g.find(1)->params["image"] = "missing.png";  // an input image that isn't there: no thumbnail, all the way down
    const auto none = remod::preview_values(g, tmp.path);
    CHECK_FALSE(remod::preview_image(g, none, 1, tmp.path, 16, [](const fs::path&) { return std::optional<remod::ImagePreview>{}; }));
    CHECK_FALSE(remod::preview_image(g, none, 3, tmp.path, 16, [](const fs::path&) { return std::optional<remod::ImagePreview>{}; }));
    CHECK_FALSE(remod::preview_image(g, none, 1, tmp.path, 16, load));  // load_image throws: caught, nothing
}

TEST_CASE("preview_image starts at the texture when nothing is exported yet, and uses an export for editing once it exists") {
    TempDir tmp;
    // A 2x1 R8G8B8A8 texture (format 28), both pixels dark red.
    std::string tex(40 + 16, '\0');
    test::put_le(tex, 0, 0x00584554, 4);
    test::put_le(tex, 4, 143221013, 4);
    test::put_le(tex, 8, 2, 2);
    test::put_le(tex, 10, 1, 2);
    tex[14] = 1;
    tex[15] = 16;
    test::put_le(tex, 16, 28, 4);
    test::put_le(tex, 40, std::uint32_t(tex.size()), 4);
    test::put_le(tex, 48, 8, 4);
    test::put_le(tex, 52, 8, 4);
    tex += std::string("\x80\x00\x00\xff\x80\x00\x00\xff", 8);
    test::write_file(tmp.path / "frame.tex.143221013", tex);

    Graph g;
    g.add_node("LoadTex").params["tex"] = "frame.tex.143221013";  // 1
    g.add_node("ExportImage").params["png"] = "work/frame.png";    // 2: not exported yet
    g.add_node("AdjustColour").params["brightness"] = "50";        // 3
    g.add_node("EditImage");                                        // 4: the export is for editing
    REQUIRE(g.connect({1, "tex", 2, "tex"}).empty());
    REQUIRE(g.connect({2, "png", 4, "png"}).empty());
    REQUIRE(g.connect({4, "image", 3, "image"}).empty());
    const remod::ImageLoader load = [](const fs::path& f) -> std::optional<remod::ImagePreview> {
        if (f.filename().string().find(".tex") != std::string::npos) {
            unsigned w = 0;
            remod::Bgra img = remod::decode_tex(f, 64, &w);
            const float scale = float(img.width) / float(w);
            return remod::ImagePreview{std::move(img), scale};
        }
        return remod::ImagePreview{remod::load_image(f), 1};
    };
    auto thumb = [&] { return remod::preview_image(g, remod::preview_values(g, tmp.path), 3, tmp.path, 64, load); };
    const auto from_texture = thumb();
    REQUIRE(from_texture);
    CHECK(from_texture->image.width == 2);
    CHECK(from_texture->image.pixels[2] > 0x80);  // red, brighter

    fs::create_directories(tmp.path / "work");  // once exported (and maybe edited), the PNG is what comes in
    remod::save_png(tmp.path / "work/frame.png", remod::Bgra{3, 1, std::vector<std::uint8_t>(12, 0)});
    const auto from_png = thumb();
    REQUIRE(from_png);
    CHECK(from_png->image.width == 3);
}

TEST_CASE("run: Replace photo puts the picture in the frame's photo; its preview works before any run") {
    TempDir tmp;
    remod::Bgra frame{120, 120, std::vector<std::uint8_t>(120 * 120 * 4, 0)};
    for (unsigned y = 0; y < 120; ++y)
        for (unsigned x = 0; x < 120; ++x) {
            const unsigned d = std::min({x, y, 119 - x, 119 - y});
            std::uint8_t* p = &frame.pixels[(size_t(y) * 120 + x) * 4];
            p[3] = d < 5 ? 0 : 255;
            p[0] = p[1] = p[2] = std::uint8_t(d < 30 ? 40 : 170);  // wood, then the old photo
        }
    remod::save_png(tmp.path / "frame.png", frame);
    remod::save_png(tmp.path / "pic.png", remod::Bgra{2, 2, {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255}});

    Graph g;
    auto& block = g.add_node("ReplacePhoto");
    block.params["frame"] = "frame.png";
    block.params["picture"] = "pic.png";
    block.params["tone"] = block.params["shading"] = block.params["stains"] = "0";  // the picture as it is
    block.params["save_to"] = "out.png";
    const auto r = run_in(g, tmp.path);
    CHECK_THAT(r.nodes.at(1).message, ContainsSubstring("photo replaced (frame width"));
    const remod::Bgra out = remod::load_image(tmp.path / "out.png");
    CHECK(out.pixels[(60 * 120 + 60) * 4 + 0] == 255);  // blue in the middle
    CHECK(out.pixels[(60 * 120 + 31) * 4 + 0] >= 200);  // right up to the photo's edge (at 30), softened inwards
    CHECK(out.pixels[(60 * 120 + 29) * 4 + 0] == 40);   // not onto the frame
    CHECK(out.pixels[(60 * 120 + 15) * 4 + 0] == 40);   // the wood untouched
    const auto written = fs::last_write_time(tmp.path / "out.png");
    CHECK_THAT(run_in(g, tmp.path).nodes.at(1).message, ContainsSubstring("out.png unchanged"));  // the same result:
    CHECK(fs::last_write_time(tmp.path / "out.png") == written);                                // not written again

    const remod::ImageLoader load = [](const fs::path& f) -> std::optional<remod::ImagePreview> {
        return remod::ImagePreview{remod::load_image(f), 1};
    };
    g.find(1)->params["show_outline"] = "true";
    const auto thumb = remod::preview_image(g, remod::preview_values(g, tmp.path), 1, tmp.path, 256, load);
    REQUIRE(thumb);
    CHECK(thumb->image.width == 120);
    CHECK(thumb->found.at("frame_width") >= 23);  // auto: the edge found, 25 px in from the outline (at 5)
    CHECK(thumb->found.at("frame_width") <= 27);

    g.find(1)->params["frame_width"] = "500";  // past the middle: no thumbnail, and why
    std::string why;
    CHECK_FALSE(remod::preview_image(g, remod::preview_values(g, tmp.path), 1, tmp.path, 256, load, nullptr, &why));
    CHECK_THAT(why, ContainsSubstring("Frame width 500 px") && ContainsSubstring("at most"));
}

TEST_CASE("a Preview block shows what comes in, before and during a run, and changes nothing") {
    TempDir tmp;
    remod::save_png(tmp.path / "pic.png", remod::Bgra{3, 2, std::vector<std::uint8_t>(3 * 2 * 4, 200)});
    Graph g;
    g.add_node("AdjustColour").params["image"] = "pic.png";  // 1
    g.add_node("Preview");                                    // 2
    REQUIRE(g.connect({1, "image", 2, "in"}).empty());
    const remod::ImageLoader load = [](const fs::path& f) -> std::optional<remod::ImagePreview> {
        return remod::ImagePreview{remod::load_image(f), 1};
    };
    const auto shown = remod::preview_image(g, remod::preview_values(g, tmp.path), 2, tmp.path, 256, load);
    REQUIRE(shown);
    CHECK(shown->image.width == 3);
    CHECK(remod::find_spec("Preview")->view_size == std::string("size"));

    const auto r = run_in(g, tmp.path);
    CHECK(r.nodes.at(2).state == NodeState::Done);
    g.add_node("Preview");  // 3: nothing linked in: still fine
    CHECK(run_in(g, tmp.path).nodes.at(3).message == "nothing linked in");
}

TEST_CASE("a destination row has one circle: the result on the right, or flipped to the left for a link") {
    auto result = [](const char* type, const char* input) {
        const char* r = remod::find_input(*remod::find_spec(type), input)->result;
        return std::string(r ? r : "");
    };
    CHECK(result("CopyFile", "dest") == "path");
    CHECK(result("MoveFile", "dest") == "path");
    CHECK(result("RenameFile", "name") == "path");
    CHECK(result("MakeFolder", "folder") == "folder");
    CHECK(result("PackageMod", "out") == "mod");
    CHECK(result("CopyFile", "source").empty());
    CHECK(result("JoinPath", "folder").empty());  // what it starts from, not where it writes

    Graph g;
    g.add_node("Value");       // 1
    g.add_node("PackageMod");  // 2
    g.add_node("Text");        // 3
    REQUIRE(g.connect({2, "mod", 3, "parts"}).empty());  // default: typed, the built mod's path goes on
    CHECK_THAT(g.connect({1, "value", 2, "out"}), ContainsSubstring("is typed on the block; flip its row"));
    CHECK_FALSE(remod::is_flipped(*g.find(2), "out"));

    g.flip(2, "out");  // circle to the left: a Value can set it; the result isn't offered, its link goes
    CHECK(remod::is_flipped(*g.find(2), "out"));
    CHECK(g.links.empty());
    REQUIRE(g.connect({1, "value", 2, "out"}).empty());
    CHECK_THAT(g.connect({2, "mod", 3, "parts"}), ContainsSubstring("isn't passed on"));
    CHECK_FALSE(has(g.validate(), "unknown parameter"));  // "flip:out" is known

    TempDir tmp;
    remod::save_graph(g, tmp.path / "g.json");
    CHECK(remod::is_flipped(*remod::load_graph(tmp.path / "g.json").find(2), "out"));  // kept in the file
    CHECK(remod::is_flipped(*g.find(g.duplicate_node(2)), "out"));                  // and by Duplicate

    g.flip(2, "out");  // back: the Value's link goes
    CHECK_FALSE(remod::is_flipped(*g.find(2), "out"));
    CHECK(g.links.empty());

    // Adding a block onto a pin never picks a typed destination to link into.
    const auto choices = g.choices_for_pin(1, "value", true);
    for (const auto& c : choices)
        if (c.spec->type == std::string("CopyFile")) CHECK(c.port == "source");
}

TEST_CASE("a block can be named; the heading and run messages use the name") {
    Graph g;
    g.add_node("Value").params["value"] = "builds";  // 1
    CHECK(remod::block_title(*g.find(1)) == "Value");
    remod::set_block_title(g, 1, "  Mod Output Folder ");
    CHECK(remod::block_title(*g.find(1)) == "Mod Output Folder");
    CHECK(g.validate().empty());  // "title" is a known parameter
    TempDir tmp;
    remod::save_graph(g, tmp.path / "g.json");
    CHECK(remod::block_title(*remod::load_graph(tmp.path / "g.json").find(1)) == "Mod Output Folder");
    CHECK(remod::block_title(*g.find(g.duplicate_node(1))) == "Mod Output Folder");
    g.find(1)->params["value"] = "";
    CHECK(has(g.validate(), "Mod Output Folder (node 1): Value is required"));
    remod::set_block_title(g, 1, "   ");
    CHECK(remod::block_title(*g.find(1)) == "Value");
    CHECK_FALSE(g.find(1)->params.contains("title"));
}

TEST_CASE("placement: blocks let go too close move apart; added blocks make room and keep the flow's order") {
    using P = std::vector<std::array<float, 2>>;
    // keep_apart: dropped onto the right end of another block, it moves the shortest way out (right and down tie
    // at 40 here; right goes first, keeping the flow).
    const P sizes{{100, 50}, {100, 50}};
    const P moved = remod::keep_apart({{0, 0}, {80, 30}}, sizes, 1, 20);
    CHECK(moved[0] == std::array<float, 2>{0, 0});  // the other block stays
    CHECK(moved[1] == std::array<float, 2>{120, 30});  // its right edge (100) + the 20 gap
    CHECK(remod::keep_apart({{0, 0}, {200, 0}}, sizes, 1, 20)[1] == std::array<float, 2>{200, 0});  // far enough

    // make_room: a Split put on the link Original texture -> Export goes right of Original texture, level with it;
    // Export and what follows it move right to keep the gap.
    Graph g;
    g.add_node("LoadTex");      // 1
    g.add_node("ExportImage");  // 2
    g.add_node("EditImage");    // 3
    REQUIRE(g.connect({1, "tex", 2, "tex"}).empty());
    REQUIRE(g.connect({2, "png", 3, "png"}).empty());
    const int split = g.insert_node(0, "Split");  // 4
    const P at = remod::make_room(g, {{0, 0}, {400, 0}, {800, 0}, {150, 200}},
                                  {{300, 100}, {300, 100}, {300, 100}, {100, 80}}, split, 50, 30);
    CHECK(at[3] == std::array<float, 2>{350, 0});  // 300 wide + 50 right of Original texture
    CHECK(at[1][0] == 500);  // Export: from 400 to the Split's right edge (450) + 50
    CHECK(at[2][0] == 900);  // and Edit after it, by the same 100
    CHECK(at[0] == std::array<float, 2>{0, 0});

    // Loading an old file that gains blocks says so (front ends tidy up then).
    TempDir tmp;
    bool added = false;
    test::write_file(tmp.path / "old.json", R"({"schema_version": 0, "profile": "re4r",
      "nodes": [{"id": 1, "type": "Text"}, {"id": 2, "type": "Text"}, {"id": 3, "type": "Text"}],
      "links": [{"from": [1, "text"], "to": [2, "parts"]}, {"from": [1, "text"], "to": [3, "parts"]}]})");
    remod::load_graph(tmp.path / "old.json", &added);
    CHECK(added);  // a Split for the fan-out
    remod::load_graph(REMOD_SCHEMAS_DIR "/graph.v0.example.json", &added);
    CHECK_FALSE(added);
}

// ---- Fan-out: Files in folder repeats the blocks it feeds, once per file ----

namespace {

// 1 Files in folder (natives/STM/ui) -> 2 LoadTex -> 3 Split -> 4 Export (edits/{name}.png) -> 5 Edit -> 6 SaveTex
// (original from the Split) -> 7 Package (collects every item's texture).
Graph fan_out(const fs::path& dir) {
    for (const char* name : {"a", "b", "c"})
        test::write_fake_tex(dir / "natives/STM/ui" / (std::string(name) + ".tex.143221013"), 143221013, 64, 32, 1, 5, 99);
    Graph g;
    g.add_node("FilesInFolder").params["folder"] = "natives/STM/ui";
    g.add_node("LoadTex");
    g.add_node("Split");
    g.add_node("ExportImage").params["png"] = "edits/{name}.png";
    g.add_node("EditImage");
    g.add_node("SaveTex");
    auto& pkg = g.add_node("PackageMod");
    pkg.params["name"] = "M";
    pkg.params["out"] = "out";
    for (const remod::Link& l : {remod::Link{1, "files", 2, "tex"}, remod::Link{2, "tex", 3, "in"},
                                 remod::Link{3, "out", 4, "tex"}, remod::Link{4, "png", 5, "png"},
                                 remod::Link{5, "image", 6, "image"}, remod::Link{3, "out", 6, "original"},
                                 remod::Link{6, "tex", 7, "tex"}})
        REQUIRE(g.connect(l).empty());
    return g;
}

}  // namespace

TEST_CASE("fan-out: each file runs through the steps, edits are done per item, Package waits for them all") {
    TempDir tmp;
    Graph g = fan_out(tmp.path);
    REQUIRE(g.validate().empty());
    CHECK(remod::list_source(g, 6) == 1);
    CHECK(remod::list_source(g, 7) == 0);  // takes them all
    FakeConverter conv;
    const remod::RunOptions opt{.profile = re4r(), .converter = conv, .base_dir = tmp.path};

    const auto first = remod::run_graph(g, opt);
    CHECK(first.paused);
    CHECK(conv.loads == 3);
    for (const char* name : {"a", "b", "c"}) CHECK(fs::exists(tmp.path / "edits" / (std::string(name) + ".png")));
    CHECK(first.nodes.at(5).state == NodeState::Waiting);
    REQUIRE(first.nodes.at(5).items.size() == 3);
    CHECK(first.nodes.at(5).items[1].name == "b");
    CHECK(first.nodes.at(5).items[1].key == "b.tex.143221013");
    CHECK_THAT(first.nodes.at(5).message, ContainsSubstring("3 waiting for you"));
    CHECK(first.nodes.at(7).state == NodeState::NotReached);
    remod::apply_run(g, first);
    CHECK(g.find(4)->params.contains("exported_from@a.tex.143221013"));

    remod::set_edit_done(g, 5, true, "a.tex.143221013");
    const auto second = remod::run_graph(g, opt);
    CHECK(conv.saves == 1);  // a only
    CHECK(second.nodes.at(6).items[0].state == NodeState::Done);
    CHECK(second.nodes.at(6).items[1].state == NodeState::NotReached);
    CHECK(second.nodes.at(6).state == NodeState::NotReached);  // not finished
    CHECK(second.nodes.at(7).state == NodeState::NotReached);  // never a partial mod
    CHECK_FALSE(fs::exists(tmp.path / "out"));

    for (const char* key : {"b.tex.143221013", "c.tex.143221013"}) remod::set_edit_done(g, 5, true, key);
    const auto built = remod::run_graph(g, opt);
    CHECK_FALSE(built.paused);
    CHECK(built.nodes.at(7).state == NodeState::Done);
    for (const char* name : {"a", "b", "c"})
        CHECK(fs::is_regular_file(tmp.path / "out/M/natives/STM/ui" / (std::string(name) + ".tex.143221013")));

    remod::set_edit_done(g, 5, false);  // "Edit again" on the block: every item
    CHECK_FALSE(std::ranges::any_of(g.find(5)->params, [](const auto& p) { return p.first.starts_with("done"); }));
}

TEST_CASE("fan-out: one file name for every item fails, a bad file stops the run or is skipped") {
    TempDir tmp;
    Graph g = fan_out(tmp.path);
    FakeConverter conv;
    const remod::RunOptions opt{.profile = re4r(), .converter = conv, .base_dir = tmp.path, .edits_done = true};

    g.find(4)->params["png"] = "edits/same.png";
    CHECK_THROWS_WITH(remod::run_graph(g, opt), ContainsSubstring("would both write") && ContainsSubstring("{name}"));
    g.find(4)->params["png"] = "edits/{name}.png";

    test::write_fake_tex(tmp.path / "natives/STM/ui/bad.tex.143221013", 36, 64, 32, 1, 5, 99);  // another game's
    try {
        remod::run_graph(g, opt);
        FAIL("expected a RunError");
    } catch (const remod::RunError& e) {
        CHECK_THAT(std::string(e.what()), ContainsSubstring("(bad)") && ContainsSubstring("version 36"));
        CHECK(e.nodes.at(2).state == NodeState::Failed);
    }

    g.find(1)->params["on_fail"] = "skip";
    const auto result = remod::run_graph(g, opt);
    CHECK_FALSE(result.paused);
    CHECK(std::ranges::any_of(result.warnings, [](const std::string& w) { return w.find("bad skipped") != std::string::npos; }));
    CHECK(result.nodes.at(2).state == NodeState::Failed);  // one item failed, shown
    CHECK(result.nodes.at(7).state == NodeState::Done);
    CHECK(fs::is_regular_file(tmp.path / "out/M/natives/STM/ui/c.tex.143221013"));
    CHECK_FALSE(fs::exists(tmp.path / "out/M/natives/STM/ui/bad.tex.143221013"));
}

TEST_CASE("fan-out: previews and the plan cover every item; {name} and two lists are checked") {
    TempDir tmp;
    Graph g = fan_out(tmp.path);
    const auto plan = remod::plan_changes(g, tmp.path);
    for (const char* name : {"a", "b", "c"})
        CHECK(std::ranges::any_of(plan.changes, [&](const remod::FileChange& c) {
            return c.node == 4 && c.path == tmp.path / "edits" / (std::string(name) + ".png");
        }));
    std::map<int, std::vector<remod::ListItem>> lists;
    CHECK(remod::preview_values(g, tmp.path, &lists).at({4, "png"}) == (tmp.path / "edits" / "a.png").string());
    REQUIRE(lists.at(1).size() == 3);
    CHECK(lists.at(1)[2].name == "c");
    g.find(1)->params["show"] = "b.tex.143221013";  // the previews show b
    CHECK(remod::preview_values(g, tmp.path).at({4, "png"}) == (tmp.path / "edits" / "b.png").string());
    REQUIRE(g.validate().empty());  // "show" is the list block's own state
    g.find(1)->params.erase("show");

    g.find(7)->params["name"] = "{name}";  // Package isn't repeated
    CHECK(has(g.validate(), "{name} only works in a block repeated for a list"));
    g.find(7)->params["name"] = "M";

    const int other = g.add_node("FilesInFolder").id;
    g.find(other)->params["folder"] = "natives/STM/ui";
    g.disconnect(g.links_into(6, "original").at(0));
    REQUIRE(g.connect({other, "files", 6, "original"}).empty());
    CHECK(has(g.validate(), "two lists meet here"));
}

TEST_CASE("run: Merge channels puts a new picture into the original's colour, its alpha kept; Pick channel shows it") {
    TempDir tmp;
    remod::save_png_bgra(tmp.path / "original.png", 2, 1, {10, 10, 10, 77, 10, 10, 10, 200});  // alpha = data
    remod::save_png_bgra(tmp.path / "new.png", 2, 1, {0, 0, 255, 255, 255, 0, 0, 255});
    Graph g;
    g.add_node("ImportImage").params["png"] = "original.png";
    g.add_node("Split");
    g.add_node("ImportImage").params["png"] = "new.png";
    auto& merge = g.add_node("MergeChannels");
    merge.params["save_to"] = "merged.png";
    auto& pick = g.add_node("PickChannel");
    pick.params["save_to"] = "alpha.png";
    for (const remod::Link& l : {remod::Link{1, "image", 2, "in"}, remod::Link{2, "out", 4, "base"},
                                 remod::Link{3, "image", 4, "colour"}, remod::Link{2, "out", 5, "image"}})
        REQUIRE(g.connect(l).empty());
    REQUIRE(g.validate().empty());
    FakeConverter conv;
    remod::run_graph(g, {.profile = re4r(), .converter = conv, .base_dir = tmp.path});
    CHECK(remod::load_image(tmp.path / "merged.png").pixels ==
          std::vector<std::uint8_t>{0, 0, 255, 77, 255, 0, 0, 200});
    CHECK(remod::load_image(tmp.path / "alpha.png").pixels ==
          std::vector<std::uint8_t>{77, 77, 77, 255, 200, 200, 200, 255});

    const remod::ImageLoader load = [](const fs::path& p) -> std::optional<remod::ImagePreview> {
        return remod::ImagePreview{remod::load_image(p), 1};
    };
    const auto thumb = remod::preview_image(g, remod::preview_values(g, tmp.path), 4, tmp.path, 256, load);
    REQUIRE(thumb);  // the live thumbnail works it out the same way
    CHECK(thumb->image.pixels == std::vector<std::uint8_t>{0, 0, 255, 77, 255, 0, 0, 200});

    g.disconnect(g.links_into(4, "colour").at(0));  // nothing to merge in
    CHECK_THROWS_WITH(remod::run_graph(g, {.profile = re4r(), .converter = conv, .base_dir = tmp.path}),
                      ContainsSubstring("nothing to merge in"));
}

TEST_CASE("run: Blend in mask changes only where the mask is white") {
    TempDir tmp;
    remod::save_png_bgra(tmp.path / "original.png", 2, 1, {10, 10, 10, 77, 10, 10, 10, 88});
    remod::save_png_bgra(tmp.path / "ai.png", 2, 1, {200, 200, 200, 255, 200, 200, 200, 255});
    remod::save_png_bgra(tmp.path / "mask.png", 2, 1, {255, 255, 255, 255, 0, 0, 0, 255});
    Graph g;
    g.add_node("ImportImage").params["png"] = "original.png";
    g.add_node("ImportImage").params["png"] = "ai.png";
    g.add_node("ImportImage").params["png"] = "mask.png";
    g.add_node("MaskBlend").params["save_to"] = "out.png";
    for (const remod::Link& l : {remod::Link{1, "image", 4, "base"}, remod::Link{2, "image", 4, "edited"},
                                 remod::Link{3, "image", 4, "mask"}})
        REQUIRE(g.connect(l).empty());
    REQUIRE(g.validate().empty());
    FakeConverter conv;
    remod::run_graph(g, {.profile = re4r(), .converter = conv, .base_dir = tmp.path});
    CHECK(remod::load_image(tmp.path / "out.png").pixels == std::vector<std::uint8_t>{200, 200, 200, 77, 10, 10, 10, 88});
}
