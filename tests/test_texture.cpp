#include "texture_converter.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdlib>

using Catch::Matchers::ContainsSubstring;
using remod::ConvertError;
using test::TempDir;
namespace fs = std::filesystem;

namespace {

const remod::Profile& re4r() {
    static const remod::Profile p = remod::load_profile(REMOD_PROFILES_DIR "/re4r.toml");
    return p;
}

std::string env(const char* name) {
    char* v = nullptr;
    size_t n = 0;
    _dupenv_s(&v, &n, name);
    std::string s = v ? v : "";
    std::free(v);
    return s;
}

fs::path fake_tex(const fs::path& dir, std::uint32_t version, std::uint16_t w, std::uint16_t h, std::uint8_t images,
                  std::uint8_t mips, std::uint32_t dxgi) {
    const fs::path p = test::unique(dir, ".tex." + std::to_string(version));
    test::write_fake_tex(p, version, w, h, images, mips, dxgi);
    return p;
}

fs::path fake_png(const fs::path& dir, std::uint32_t w, std::uint32_t h) {
    const fs::path p = test::unique(dir, ".png");
    test::write_fake_png(p, w, h);
    return p;
}

}  // namespace

TEST_CASE("read_tex_meta reads the header fields") {
    TempDir tmp;
    const auto m = remod::read_tex_meta(fake_tex(tmp.path, 143221013, 1024, 512, 1, 8, 99), re4r());
    CHECK(m.game_profile == "re4r");
    CHECK(m.width == 1024);
    CHECK(m.height == 512);
    CHECK(m.format == "BC7_UNORM_SRGB");
    CHECK(m.mip_count == 8);
    CHECK(m.array_count == 1);
    CHECK(remod::read_tex_meta(fake_tex(tmp.path, 143221013, 4, 4, 1, 1, 12345), re4r()).format == "DXGI_FORMAT 12345");
}

TEST_CASE("read_tex_meta rejects non-tex files and other tex versions") {
    TempDir tmp;
    CHECK_THROWS_WITH(remod::read_tex_meta(fake_tex(tmp.path, 36, 4, 4, 1, 1, 99), re4r()),
                      ContainsSubstring("does not match profile re4r"));
    const fs::path junk = tmp.path / "junk.tex.143221013";
    std::ofstream(junk) << "not a texture at all, just text";
    CHECK_THROWS_WITH(remod::read_tex_meta(junk, re4r()), ContainsSubstring("not an RE Engine .tex"));
    std::ofstream(tmp.path / "short.tex.143221013") << "TEX";
    CHECK_THROWS_AS(remod::read_tex_meta(tmp.path / "short.tex.143221013", re4r()), ConvertError);
    CHECK_THROWS_AS(remod::read_tex_meta(tmp.path / "missing.tex.143221013", re4r()), ConvertError);
}

TEST_CASE("png_size reads IHDR") {
    TempDir tmp;
    CHECK(remod::png_size(fake_png(tmp.path, 1024, 768)) == std::pair<std::uint32_t, std::uint32_t>{1024, 768});
    std::ofstream(tmp.path / "x.png") << "definitely not a png file here";
    CHECK_THROWS_WITH(remod::png_size(tmp.path / "x.png"), ContainsSubstring("not a PNG"));
}

TEST_CASE("NoesisConverter checks inputs before running Noesis") {
    CHECK_THROWS_WITH(remod::NoesisConverter("C:/does/not/Noesis64.exe"), ContainsSubstring("Noesis not found"));

    // Any existing exe will do: each case below must fail before it is launched.
    remod::NoesisConverter conv("C:/Windows/System32/cmd.exe");
    TempDir tmp;
    const fs::path tex = fake_tex(tmp.path, 143221013, 64, 64, 1, 5, 99);
    CHECK_THROWS_WITH(conv.save_tex(fake_png(tmp.path, 32, 64), tex, tmp.path / "o.tex.143221013", re4r()),
                      ContainsSubstring("must be 64x64"));
    CHECK_THROWS_WITH(conv.save_tex(fake_png(tmp.path, 64, 64), fake_tex(tmp.path, 143221013, 64, 64, 2, 5, 99),
                                    tmp.path / "o.tex.143221013", re4r()),
                      ContainsSubstring("multi-image"));
    CHECK_THROWS_WITH(conv.save_tex(fake_png(tmp.path, 64, 64), tex, tex, re4r()), ContainsSubstring("already exists"));
    CHECK_THROWS_WITH(conv.load_tex(tex, tex, re4r()), ContainsSubstring("already exists"));

    remod::Profile tbd = re4r();
    tbd.noesis_export = "TBD";
    CHECK_THROWS_WITH(conv.save_tex(fake_png(tmp.path, 64, 64), tex, tmp.path / "o.tex.143221013", tbd),
                      ContainsSubstring("noesis_export"));
}

// CLAUDE.md §8: needs local fixtures + Noesis, never committed. Skips unless both env vars are set.
//   REMOD_FIXTURES = folder of original *.tex.143221013 files
//   REMOD_NOESIS   = path to Noesis64.exe (with the fmt_RE_MESH plugin installed)
TEST_CASE("round trip: tex -> png -> tex reproduces the original's TexMeta") {
    const std::string fixtures = env("REMOD_FIXTURES"), noesis = env("REMOD_NOESIS");
    if (fixtures.empty() || noesis.empty()) SKIP("set REMOD_FIXTURES and REMOD_NOESIS to run");

    remod::NoesisConverter conv(noesis);
    TempDir tmp;
    int count = 0;
    for (const auto& e : fs::directory_iterator(fixtures)) {
        if (!e.path().string().ends_with(".tex." + re4r().tex_suffix)) continue;
        CAPTURE(e.path().string());
        const auto stem = std::to_string(count++);
        const auto loaded = conv.load_tex(e.path(), tmp.path / (stem + ".png"), re4r());
        CHECK(remod::png_size(tmp.path / (stem + ".png")) == std::pair{loaded.width, loaded.height});
        const auto saved =
            conv.save_tex(tmp.path / (stem + ".png"), e.path(), tmp.path / (stem + ".tex." + re4r().tex_suffix), re4r());
        CHECK(saved.width == loaded.width);
        CHECK(saved.height == loaded.height);
        CHECK(saved.format == loaded.format);
        CHECK(saved.mip_count == loaded.mip_count);
        // Pixels aren't compared: BC7 re-encoding is lossy, so exact image data can't round-trip.
    }
    REQUIRE(count > 0);
}
