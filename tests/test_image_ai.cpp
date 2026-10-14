#include "graph.hpp"
#include "image.hpp"
#include "image_ai.hpp"
#include "texture_converter.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using Catch::Matchers::ContainsSubstring;
using remod::Graph;
using test::TempDir;
namespace fs = std::filesystem;

namespace {

const remod::Profile& re4r() {
    static const remod::Profile p = remod::load_profile(REMOD_PROFILES_DIR "/re4r.toml");
    return p;
}

struct NoConverter : remod::ITextureConverter {
    remod::TexMeta load_tex(const fs::path&, const fs::path&, const remod::Profile&) override { return {}; }
    remod::TexMeta save_tex(const fs::path&, const fs::path&, const fs::path&, const remod::Profile&) override {
        return {};
    }
};

// A 64x64 picture: red, alpha 100 (data in RE textures: must be kept).
remod::Bgra red() {
    remod::Bgra b{64, 64, std::vector<std::uint8_t>(64 * 64 * 4)};
    for (size_t i = 0; i < b.pixels.size(); i += 4) b.pixels[i + 2] = 255, b.pixels[i + 3] = 100;
    return b;
}

const std::uint8_t* px(const remod::Bgra& b, unsigned x, unsigned y) { return &b.pixels[(y * b.width + x) * 4]; }

}  // namespace

TEST_CASE("the AI works at the model's size: the longer side, multiples of 64") {
    CHECK(remod::work_size(1024, 1024, 1024) == std::pair{1024u, 1024u});
    CHECK(remod::work_size(468, 440, 1024) == std::pair{1024u, 960u});
    CHECK(remod::work_size(2048, 512, 1024) == std::pair{1024u, 256u});
    CHECK(remod::work_size(10, 1000, 512) == std::pair{64u, 512u});
}

TEST_CASE("the AI tool: sd-cli.exe given, or its folder; else what to set") {
    TempDir tmp;
    CHECK_THROWS_WITH(remod::find_sd_cli({}), ContainsSubstring("Download it"));
    CHECK_THROWS_WITH(remod::find_sd_cli(tmp.path), ContainsSubstring("no sd-cli.exe"));
    test::write_file(tmp.path / "sd-cli.exe", "x");
    CHECK(remod::find_sd_cli(tmp.path) == tmp.path / "sd-cli.exe");
    CHECK(remod::find_sd_cli(tmp.path / "sd-cli.exe") == tmp.path / "sd-cli.exe");
    CHECK(remod::sd_cpp_download_url() ==
          "https://github.com/leejet/stable-diffusion.cpp/releases/download/master-964-1d0a58a/"
          "sd-master-1d0a58a-bin-win-vulkan-x64.zip");
}

TEST_CASE("Generate image: the AI's picture, in the mask only, alpha kept, reused when nothing changed") {
    TempDir tmp;
    remod::save_png(tmp.path / "picture.png", red());
    remod::Bgra mask{64, 64, std::vector<std::uint8_t>(64 * 64 * 4, 0)};
    for (unsigned y = 0; y < 64; ++y)
        for (unsigned x = 0; x < 32; ++x) std::fill_n(&mask.pixels[(y * 64 + x) * 4], 4, std::uint8_t(255));
    remod::save_png(tmp.path / "mask.png", mask);
    test::write_file(tmp.path / "model.safetensors", "weights");
    remod::set_image_ai({FAKE_SD_CLI, tmp.path / "model.safetensors", {}});

    Graph g;
    remod::Node& gen = g.add_node("GenerateImage");
    gen.params["image"] = "picture.png";
    gen.params["prompt"] = "a castle";
    gen.params["feather"] = "0";
    gen.params["save_to"] = "out.png";
    REQUIRE(g.validate().empty());
    NoConverter conv;
    const fs::path cache = tmp.path / "cache";
    const remod::RunOptions opts{.profile = re4r(), .converter = conv, .base_dir = tmp.path, .cache_dir = cache};

    // No mask: all of it, the colour inverted by the stand-in; alpha the original's.
    remod::RunResult r = remod::run_graph(g, opts);
    remod::apply_run(g, r);
    remod::Bgra out = remod::load_image(tmp.path / "out.png");
    CHECK(px(out, 5, 5)[0] >= 253);  // blue: was 0
    CHECK(px(out, 5, 5)[2] <= 2);    // red: was 255 (scaled to the work size and back: +-1)
    CHECK(px(out, 5, 5)[3] == 100);
    CHECK_THAT(r.nodes.at(1).message, ContainsSubstring("generated"));
    CHECK(fs::is_regular_file(g.find(1)->params.at("generated")));  // kept in the run cache

    // Same again: reused from the cache, the tool not started.
    remod::set_image_ai({tmp.path / "missing", tmp.path / "model.safetensors", {}});
    CHECK_THROWS_WITH(remod::run_graph(g, opts), ContainsSubstring("no sd-cli.exe"));  // the tool is looked for first
    remod::set_image_ai({FAKE_SD_CLI, tmp.path / "model.safetensors", {}});
    r = remod::run_graph(g, opts);
    CHECK_THAT(r.nodes.at(1).message, ContainsSubstring("unchanged: reused"));

    // A mask: the left half changes, the right half is the original exactly.
    g.find(1)->params["mask"] = "mask.png";
    r = remod::run_graph(g, opts);
    remod::apply_run(g, r);
    CHECK_THAT(r.nodes.at(1).message, ContainsSubstring("generated"));
    out = remod::load_image(tmp.path / "out.png");
    CHECK(px(out, 5, 5)[0] >= 253);
    CHECK((px(out, 50, 5)[0] == 0 && px(out, 50, 5)[2] == 255 && px(out, 50, 5)[3] == 100));

    // The thumbnail: the last picture through the mask, never generating.
    const remod::ImageLoader load = [](const fs::path& p) -> std::optional<remod::ImagePreview> {
        return remod::ImagePreview{remod::load_image(p), 1};
    };
    const auto thumb = remod::preview_image(g, remod::preview_values(g, tmp.path), 1, tmp.path, 256, load);
    REQUIRE(thumb);
    CHECK(px(thumb->image, 5, 5)[0] >= 253);
    CHECK(px(thumb->image, 50, 5)[2] == 255);

    // A failing tool says so; no model says what to set.
    g.find(1)->params["prompt"] = "fail please";
    CHECK_THROWS_WITH(remod::run_graph(g, opts), ContainsSubstring("exit code 3"));
    remod::set_image_ai({FAKE_SD_CLI, {}, {}});
    CHECK_THROWS_WITH(remod::run_graph(g, opts), ContainsSubstring("no AI model set"));
    remod::set_image_ai({});
}
