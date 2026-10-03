// Conditions (If, First of, File exists, Text matches, Not), "nothing" in the engine, and Streaming copy.
#include "graph.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using Catch::Matchers::ContainsSubstring;
using remod::Graph;
using remod::Link;
using remod::NodeState;
using test::TempDir;
namespace fs = std::filesystem;

namespace {

const remod::Profile& re4r() {
    static const remod::Profile p = remod::load_profile(REMOD_PROFILES_DIR "/re4r.toml");
    return p;
}

// "PNG" = a PNG header of the texture's size, "tex" = a copy of the original (as in test_graph.cpp).
struct FakeConverter : remod::ITextureConverter {
    int saves = 0;
    remod::TexMeta load_tex(const fs::path& tex, const fs::path& png_out, const remod::Profile& p) override {
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

void link(Graph& g, std::initializer_list<Link> links) {
    for (const Link& l : links) REQUIRE(g.connect(l) == "");
}

}  // namespace

TEST_CASE("If: a branch runs only when its condition is yes; the plan leaves out the branch not taken") {
    TempDir tmp;
    test::write_file(tmp.path / "a.txt", "a");
    Graph g;
    g.add_node("FileExists").params["path"] = "flag.txt";  // 1
    g.add_node("Value").params["value"] = "a.txt";         // 2
    g.add_node("If");                                      // 3
    auto& copy = g.add_node("CopyFile");                   // 4
    copy.params["dest"] = "b.txt";
    link(g, {{2, "value", 3, "value"}, {1, "yes", 3, "condition"}, {3, "value", 4, "source"}});
    REQUIRE(g.validate().empty());
    FakeConverter conv;
    const remod::RunOptions opt{.profile = re4r(), .converter = conv, .base_dir = tmp.path};

    // No flag: the copy isn't needed, writes nothing, and isn't in the plan.
    CHECK(remod::plan_changes(g, tmp.path).changes.empty());
    CHECK(remod::plan_changes(g, tmp.path).unknown.empty());
    const auto skipped = remod::run_graph(g, opt);
    CHECK(skipped.nodes.at(3).state == NodeState::Done);
    CHECK(skipped.nodes.at(4).state == NodeState::NotNeeded);
    CHECK_THAT(skipped.nodes.at(4).message, ContainsSubstring("not needed") && ContainsSubstring("said no"));
    CHECK_THAT(skipped.values.at({3, "value"}), ContainsSubstring("nothing"));
    CHECK_FALSE(fs::exists(tmp.path / "b.txt"));

    // The flag is there: it runs.
    test::write_file(tmp.path / "flag.txt", "");
    CHECK(remod::plan_changes(g, tmp.path).changes.size() == 1);
    CHECK(remod::run_graph(g, opt).nodes.at(4).state == NodeState::Done);
    CHECK(fs::exists(tmp.path / "b.txt"));

    // Unlinked, the condition is a checkbox: off turns the branch off by hand.
    g.disconnect(g.links_into(3, "condition").at(0));
    g.find(3)->params["condition"] = "false";
    CHECK(remod::run_graph(g, opt).nodes.at(4).state == NodeState::NotNeeded);
}

TEST_CASE("First of with Not: if / else passes on whichever branch ran") {
    TempDir tmp;
    Graph g;
    g.add_node("Text").params["text"] = "colour";         // 1
    g.add_node("Text").params["text"] = "data";           // 2
    auto& match = g.add_node("TextMatches");              // 3
    match.params["text"] = R"(ui\cs_albd.tex)";
    match.params["pattern"] = "*/*_albd*";                 // \ and / alike
    g.add_node("Split");                                   // 4: the condition to both branches
    g.add_node("Not");                                     // 5
    g.add_node("If");                                      // 6
    g.add_node("If");                                      // 7
    g.add_node("FirstOf");                                 // 8
    g.add_node("Text").params["text"] = "{1}";             // 9: what came out
    link(g, {{3, "yes", 4, "in"}, {4, "out", 6, "condition"}, {4, "out", 5, "in"}, {5, "yes", 7, "condition"},
             {1, "text", 6, "value"}, {2, "text", 7, "value"}, {6, "value", 8, "first"}, {7, "value", 8, "second"},
             {8, "value", 9, "parts"}});
    REQUIRE(g.validate().empty());
    CHECK(remod::preview_values(g, tmp.path).at({9, "text"}) == "colour");
    g.find(3)->params["text"] = "ui/cs_nrrc.tex";
    CHECK(remod::preview_values(g, tmp.path).at({9, "text"}) == "data");
    // Patterns: ; between several, case ignored.
    g.find(3)->params["pattern"] = "*_ALBD*;*_nrrc*";
    CHECK(remod::preview_values(g, tmp.path).at({9, "text"}) == "colour");
}

TEST_CASE("conditions are their own kind: yes / no into conditions and checkboxes, not into paths") {
    Graph g;
    g.add_node("FileExists");  // 1
    g.add_node("CopyFile");    // 2
    g.add_node("PackageMod");  // 3
    g.add_node("If");          // 4
    CHECK(g.can_connect({1, "yes", 2, "source"}) != "");   // a path
    CHECK(g.can_connect({1, "yes", 3, "replace"}) == "");  // a checkbox
    CHECK(g.can_connect({1, "yes", 4, "condition"}) == "");
    g.add_node("Text");        // 5: text into a condition is a typed yes / no
    CHECK(g.can_connect({5, "text", 4, "condition"}) == "");
    // First of passes either input on, so both must be of one kind.
    g.add_node("FirstOf");     // 6
    g.add_node("Value");       // 7
    link(g, {{5, "text", 6, "first"}});
    CHECK_THAT(g.can_connect({1, "yes", 6, "second"}), ContainsSubstring("one kind"));
}

namespace {

// 1 LoadTex (ui/a) -> 2 Split -> 3 Streaming copy and 7 SaveTex's original. 3's full size -> 4 Export -> 5 Edit ->
// 6 Split -> 7 SaveTex (the base) and 8 SaveTex (original: 3's streaming copy). Both -> 9 Package.
Graph streaming_graph(const std::string& tex) {
    Graph g;
    g.add_node("LoadTex").params["tex"] = tex;
    g.add_node("Split");
    g.add_node("StreamingCopy");
    g.add_node("ExportImage").params["png"] = "edits/{name}.png";
    g.add_node("EditImage");
    g.add_node("Split");
    g.add_node("SaveTex");
    g.add_node("SaveTex");
    auto& pkg = g.add_node("PackageMod");
    pkg.params["name"] = "M";
    pkg.params["out"] = "out";
    pkg.params["replace"] = "true";
    link(g, {{1, "tex", 2, "in"}, {2, "out", 3, "tex"}, {2, "out", 7, "original"}, {3, "full", 4, "tex"},
             {4, "png", 5, "png"}, {5, "image", 6, "in"}, {6, "out", 7, "image"}, {6, "out", 8, "image"},
             {3, "streaming", 8, "original"}, {7, "tex", 9, "tex"}, {8, "tex", 9, "tex"}});
    return g;
}

}  // namespace

TEST_CASE("Streaming copy: a texture with one is replaced together with it; one without, alone") {
    TempDir tmp;
    const fs::path base = tmp.path / "natives/STM/ui/a.tex.143221013";
    const fs::path copy = tmp.path / "natives/STM/streaming/ui/a.tex.143221013";
    test::write_fake_tex(base, 143221013, 64, 32, 1, 2, 99);
    test::write_fake_tex(copy, 143221013, 256, 128, 1, 4, 99);
    Graph g = streaming_graph("natives/STM/ui/a.tex.143221013");
    g.find(4)->params["png"] = "edits/a.png";
    REQUIRE(g.validate().empty());
    FakeConverter conv;
    const remod::RunOptions opt{.profile = re4r(), .converter = conv, .base_dir = tmp.path, .edits_done = true};

    const auto both = remod::run_graph(g, opt);
    CHECK(both.nodes.at(8).state == NodeState::Done);
    CHECK(remod::image_size(tmp.path / "edits/a.png") == std::pair<std::uint32_t, std::uint32_t>{256, 128});  // full size
    CHECK(fs::is_regular_file(tmp.path / "out/M/natives/STM/ui/a.tex.143221013"));
    CHECK(fs::is_regular_file(tmp.path / "out/M/natives/STM/streaming/ui/a.tex.143221013"));
    CHECK(remod::read_tex_meta(tmp.path / "out/M/natives/STM/streaming/ui/a.tex.143221013", re4r()).width == 256);
    CHECK(remod::read_tex_meta(tmp.path / "out/M/natives/STM/ui/a.tex.143221013", re4r()).width == 64);

    fs::remove(copy);
    fs::remove(tmp.path / "edits/a.png");
    const auto alone = remod::run_graph(g, opt);
    CHECK(alone.nodes.at(3).state == NodeState::Done);
    CHECK(alone.nodes.at(8).state == NodeState::NotNeeded);
    CHECK_THAT(alone.nodes.at(8).message, ContainsSubstring("no streaming copy of a.tex.143221013"));
    CHECK(alone.nodes.at(9).state == NodeState::Done);
    CHECK_THAT(alone.nodes.at(9).message, ContainsSubstring("1 texture"));
    CHECK(fs::is_regular_file(tmp.path / "out/M/natives/STM/ui/a.tex.143221013"));
    CHECK_FALSE(fs::exists(tmp.path / "out/M/natives/STM/streaming"));
}

TEST_CASE("Streaming copy over a folder: items with a copy get both, the others one; it refuses what it can't use") {
    TempDir tmp;
    for (const char* name : {"a", "b"})
        test::write_fake_tex(tmp.path / "natives/STM/ui" / (std::string(name) + ".tex.143221013"), 143221013, 64, 32, 1,
                             2, 99);
    test::write_fake_tex(tmp.path / "natives/STM/streaming/ui/a.tex.143221013", 143221013, 256, 128, 1, 4, 99);
    Graph g = streaming_graph("");
    g.find(1)->params.erase("tex");
    auto& files = g.add_node("FilesInFolder");  // 10
    files.params["folder"] = "natives/STM/ui";
    files.params["pattern"] = "*.tex*";  // also matches a tool's leftover: only textures go into texture inputs
    test::write_file(tmp.path / "natives/STM/ui/a.texout.tga", "x");
    link(g, {{10, "files", 1, "tex"}});
    REQUIRE(g.validate().empty());
    FakeConverter conv;
    const remod::RunOptions opt{.profile = re4r(), .converter = conv, .base_dir = tmp.path, .edits_done = true};

    const auto result = remod::run_graph(g, opt);
    REQUIRE(result.nodes.at(8).items.size() == 2);
    CHECK(result.nodes.at(8).items[0].state == NodeState::Done);       // a
    CHECK(result.nodes.at(8).items[1].state == NodeState::NotNeeded);  // b
    CHECK(result.nodes.at(8).state == NodeState::Done);
    CHECK_THAT(result.nodes.at(8).message, ContainsSubstring("1 done, 1 not needed"));
    CHECK_THAT(result.nodes.at(9).message, ContainsSubstring("3 texture"));
    CHECK(fs::is_regular_file(tmp.path / "out/M/natives/STM/streaming/ui/a.tex.143221013"));
    CHECK_FALSE(fs::exists(tmp.path / "out/M/natives/STM/streaming/ui/b.tex.143221013"));

    // A streaming copy picked itself, and a texture outside a natives folder: refused with the reason.
    Graph one = streaming_graph("natives/STM/streaming/ui/a.tex.143221013");
    one.find(4)->params["png"] = "edits/one.png";
    CHECK_THROWS_WITH(remod::run_graph(one, opt), ContainsSubstring("is a streaming copy itself"));
    test::write_fake_tex(tmp.path / "loose/c.tex.143221013", 143221013, 64, 32, 1, 2, 99);
    Graph loose = streaming_graph("loose/c.tex.143221013");
    loose.find(1)->params["game_path"] = "ui/c.tex.143221013";
    loose.find(4)->params["png"] = "edits/loose.png";
    CHECK_THROWS_WITH(remod::run_graph(loose, opt), ContainsSubstring("can't be looked for"));
}

// ---- Blocks made of blocks: the tool's own (blocks\) ----

#include "custom.hpp"
#include "image.hpp"

namespace {

// Writes real PNGs, and like the real converter refuses an image of another size than the original.
struct PngConverter : remod::ITextureConverter {
    remod::TexMeta load_tex(const fs::path& tex, const fs::path& png_out, const remod::Profile& p) override {
        const auto m = remod::read_tex_meta(tex, p);  // no folders made: Export image makes its own (edits\)
        remod::save_png(png_out, {m.width, m.height, std::vector<std::uint8_t>(size_t(m.width) * m.height * 4, 128)});
        return m;
    }
    remod::TexMeta save_tex(const fs::path& image, const fs::path& original, const fs::path& out,
                            const remod::Profile& p) override {
        const auto m = remod::read_tex_meta(original, p);
        if (remod::image_size(image) != std::pair{m.width, m.height}) throw std::runtime_error("image size differs");
        fs::copy_file(original, out);
        return m;
    }
};

}  // namespace

TEST_CASE("built-in Convert with streaming copy: the edit becomes both textures, or one without a copy") {
    std::vector<std::string> errors;
    const auto blocks = remod::load_custom_library(REMOD_BLOCKS_DIR, &errors);
    CHECK(errors.empty());
    const auto def = std::ranges::find(blocks, "custom:convert_with_streaming", &remod::CustomNode::type);
    REQUIRE(def != blocks.end());
    remod::register_custom(*def);
    const remod::NodeSpec* spec = remod::find_spec(def->type);
    REQUIRE(spec->inputs.size() == 3);  // edited image, texture, streaming copy: in their kinds
    CHECK(spec->inputs[0].type == remod::PortType::Image);
    CHECK(spec->inputs[1].type == remod::PortType::Tex);
    CHECK(spec->inputs[2].type == remod::PortType::Tex);
    REQUIRE(spec->outputs.size() == 2);

    TempDir tmp;
    test::write_fake_tex(tmp.path / "natives/STM/ui/a.tex.143221013", 143221013, 64, 32, 1, 2, 99);
    const fs::path copy = tmp.path / "natives/STM/streaming/ui/a.tex.143221013";
    test::write_fake_tex(copy, 143221013, 256, 128, 1, 4, 99);
    Graph g;
    g.add_node("LoadTex").params["tex"] = "natives/STM/ui/a.tex.143221013";  // 1
    g.add_node("Split");                                                     // 2
    g.add_node("StreamingCopy");                                             // 3
    g.add_node("ExportImage").params["png"] = "edits/a.png";                 // 4
    g.add_node("EditImage");                                                 // 5
    g.add_node(def->type);                                                   // 6
    auto& pkg = g.add_node("PackageMod");                                    // 7
    pkg.params["name"] = "M";
    pkg.params["out"] = "out";
    pkg.params["replace"] = "true";
    const std::string image = spec->inputs[0].name, tex = spec->inputs[1].name, streaming = spec->inputs[2].name;
    link(g, {{1, "tex", 2, "in"}, {2, "out", 3, "tex"}, {2, "out", 6, tex}, {3, "full", 4, "tex"},
             {4, "png", 5, "png"}, {5, "image", 6, image}, {3, "streaming", 6, streaming},
             {6, spec->outputs[0].name, 7, "tex"}, {6, spec->outputs[1].name, 7, "tex"}});
    REQUIRE(g.validate().empty());
    PngConverter conv;
    const remod::RunOptions opt{.profile = re4r(), .converter = conv, .base_dir = tmp.path, .edits_done = true};

    const auto both = remod::run_graph(g, opt);  // the 256x128 edit shrunk to 64x32 for the texture
    CHECK(both.nodes.at(6).state == NodeState::Done);
    CHECK(fs::is_regular_file(tmp.path / "out/M/natives/STM/ui/a.tex.143221013"));
    CHECK(fs::is_regular_file(tmp.path / "out/M/natives/STM/streaming/ui/a.tex.143221013"));

    fs::remove(copy);
    fs::remove(tmp.path / "edits/a.png");
    const auto alone = remod::run_graph(g, opt);
    CHECK(alone.nodes.at(6).state == NodeState::Done);  // its streaming conversion wasn't needed
    CHECK_THAT(alone.nodes.at(7).message, ContainsSubstring("1 texture"));
    CHECK_FALSE(fs::exists(tmp.path / "out/M/natives/STM/streaming"));
}

TEST_CASE("Export image without a file: a working copy in the run cache, reused while the texture is unchanged") {
    TempDir tmp;
    test::write_fake_tex(tmp.path / "a.tex.143221013", 143221013, 64, 32, 1, 2, 99);
    Graph g;
    g.add_node("LoadTex").params["tex"] = "a.tex.143221013";  // 1
    g.add_node("ExportImage");                                // 2: no file
    g.add_node("AdjustColour").params["hue"] = "30";          // 3
    link(g, {{1, "tex", 2, "tex"}, {2, "png", 3, "image"}});
    REQUIRE(g.validate().empty());
    PngConverter conv;
    const remod::RunOptions opt{.profile = re4r(), .converter = conv, .base_dir = tmp.path, .cache_dir = tmp.path / "cache"};
    CHECK_THAT(remod::run_graph(g, opt).nodes.at(2).message, ContainsSubstring("exported a working copy"));
    const auto again = remod::run_graph(g, opt);
    CHECK_THAT(again.nodes.at(2).message, ContainsSubstring("unchanged"));
    CHECK(again.nodes.at(3).state == NodeState::Done);
    CHECK_FALSE(fs::exists(tmp.path / ".png"));  // nothing next to the graph
}

TEST_CASE("built-in blocks: each one loads and validates; a block inside another is the shipped one") {
    std::vector<std::string> errors;
    const auto blocks = remod::load_custom_library(REMOD_BLOCKS_DIR, &errors);
    CHECK(errors.empty());
    CHECK(blocks.size() >= 2);
    for (const remod::CustomNode& b : blocks) remod::register_custom(b);
    for (const remod::CustomNode& b : blocks) {
        INFO(b.type);
        CHECK(b.graph.validate().empty());  // its inner graph, as it would run with every pin linked
        for (const remod::CustomNode& inner : b.graph.customs) {  // copied in: must match the shipped definition
            const auto shipped = std::ranges::find(blocks, inner.type, &remod::CustomNode::type);
            REQUIRE(shipped != blocks.end());
            CHECK(inner == *shipped);  // else re-run the script that copied it in (CLAUDE.md, Built from blocks)
        }
    }
    const remod::NodeSpec* recolour = remod::find_spec("custom:recolour_part");
    REQUIRE(recolour);
    CHECK(recolour->inputs[0].type == remod::PortType::Path);  // mesh
    CHECK(recolour->outputs.size() == 3);  // texture, streaming texture, preview
    // Pins are typed like the field they feed inside: hue is Adjust colour's slider, with the pin's default.
    CHECK(recolour->inputs[2].widget == remod::Widget::Number);
    CHECK(recolour->inputs[2].min == -180);
    CHECK(std::string(recolour->inputs[2].initial) == "0");
    CHECK(recolour->inputs[1].widget == remod::Widget::Text);  // materials
}

// ---- The shipped examples (examples\): each one runs on the real game files ----

#include "texture_converter.hpp"

TEST_CASE("every example graph runs on the game files (set REMOD_GAME to the extracted natives/STM)") {
    char* v = nullptr;
    size_t n = 0;
    _dupenv_s(&v, &n, "REMOD_GAME");
    const std::string game = v ? v : "";
    std::free(v);
    if (game.empty()) SKIP("set REMOD_GAME to your extracted natives/STM folder to run the examples");

    for (const remod::CustomNode& c : remod::load_custom_library(REMOD_BLOCKS_DIR)) remod::register_custom(c);
    remod::set_game_files_dir(game);
    TempDir tmp;  // a copy: they write next to themselves (edits\, mods\, backups\)
    fs::copy(REMOD_EXAMPLES_DIR, tmp.path, fs::copy_options::recursive);
    const auto converter = remod::make_converter({});
    int ran = 0;
    for (const auto& e : fs::directory_iterator(tmp.path)) {
        if (e.path().extension() != ".json") continue;
        INFO(e.path().filename().string());
        const Graph g = remod::load_graph(e.path());
        CHECK(g.validate().empty());
        const remod::RunOptions opt{.profile = re4r(), .converter = *converter, .base_dir = tmp.path,
                                    .edits_done = true, .cache_dir = tmp.path / "cache"};
        const auto result = remod::run_graph(g, opt);
        CHECK_FALSE(result.paused);
        CHECK(std::ranges::any_of(result.nodes, [&](const auto& kv) {  // its mod was built
            return g.find(kv.first)->type == "PackageMod" && kv.second.state == NodeState::Done;
        }));
        ++ran;
    }
    CHECK(ran >= 11);
    remod::set_game_files_dir({});
}
