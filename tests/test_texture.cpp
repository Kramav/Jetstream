#include "texture_converter.hpp"

#include "image.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
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

// A complete single-image .tex: header (legacy layout for version 10, else modern), mip table, then each mip's
// pixels, filled with a byte pattern. RGBA8 (28) rows get `extra` padding bytes; BC formats (99) are whole 4x4 blocks.
fs::path real_tex(const fs::path& p, std::uint32_t version, std::uint16_t w, std::uint16_t h, std::uint8_t mips,
                  std::uint32_t dxgi, std::uint32_t extra = 0) {
    const bool legacy = version == 10;
    const size_t header = legacy ? 32 : 40;
    std::string b(header + 16 * size_t(mips), '\0');
    test::put_le(b, 0, 0x00584554, 4);
    test::put_le(b, 4, version, 4);
    test::put_le(b, 8, w, 2);
    test::put_le(b, 10, h, 2);
    test::put_le(b, 12, 1, 2);  // depth
    b[14] = char(legacy ? mips : 1);
    b[15] = char(legacy ? 1 : mips * 16);
    test::put_le(b, 16, dxgi, 4);
    for (std::uint8_t m = 0; m < mips; ++m) {
        const std::uint32_t mw = std::max(1, w >> m), mh = std::max(1, h >> m);
        const bool block = dxgi != 28;
        const std::uint32_t pitch = block ? (mw + 3) / 4 * 16 : mw * 4 + extra, rows = block ? (mh + 3) / 4 : mh;
        const size_t at = header + 16 * size_t(m);
        test::put_le(b, at, std::uint32_t(b.size()), 4);  // offset (high half 0)
        test::put_le(b, at + 8, pitch, 4);
        test::put_le(b, at + 12, pitch * rows, 4);
        for (std::uint32_t i = 0; i < pitch * rows; ++i) b += char(i * 7 + m);
    }
    test::write_file(p, b);
    return p;
}

// w x h BGRA pixels in a smooth pattern, alpha varying (never 0).
remod::Bgra pattern(unsigned w, unsigned h) {
    remod::Bgra img{w, h, std::vector<std::uint8_t>(size_t(w) * h * 4)};
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            std::uint8_t* p = img.pixels.data() + (size_t(y) * w + x) * 4;
            p[0] = std::uint8_t(x * 255 / std::max(1u, w - 1));
            p[1] = std::uint8_t(y * 255 / std::max(1u, h - 1));
            p[2] = 128;
            p[3] = std::uint8_t(64 + (x + y) * 191 / std::max(1u, w + h - 2));
        }
    return img;
}

int max_difference(const remod::Bgra& a, const remod::Bgra& b) {
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    int most = 0;
    for (size_t i = 0; i < a.pixels.size(); ++i) most = std::max(most, std::abs(int(a.pixels[i]) - int(b.pixels[i])));
    return most;
}

// The header and mip table: everything before the first mip's pixels.
std::string head_of(const fs::path& tex, size_t header, std::uint8_t mips) {
    return test::read_file(tex).substr(0, header + 16 * size_t(mips));
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
                      ContainsSubstring("version 36") && ContainsSubstring("Resident Evil 4 (2023)"));
    const fs::path junk = tmp.path / "junk.tex.143221013";
    std::ofstream(junk) << "not a texture at all, just text";
    CHECK_THROWS_WITH(remod::read_tex_meta(junk, re4r()), ContainsSubstring("not an RE Engine .tex"));
    std::ofstream(tmp.path / "short.tex.143221013") << "TEX";
    CHECK_THROWS_AS(remod::read_tex_meta(tmp.path / "short.tex.143221013", re4r()), ConvertError);
    CHECK_THROWS_AS(remod::read_tex_meta(tmp.path / "missing.tex.143221013", re4r()), ConvertError);
}

TEST_CASE("textures are recognised by content, whatever their name") {
    TempDir tmp;
    const fs::path re4 = tmp.path / "plain name.tex";  // no version suffix in the name
    test::write_fake_tex(re4, 143221013, 64, 64, 1, 5, 99);
    const fs::path other = tmp.path / "other.tex.36";
    test::write_fake_tex(other, 36, 64, 64, 1, 5, 99);
    test::write_file(tmp.path / "junk.tex", "definitely not a texture");

    CHECK(remod::read_tex_version(re4) == 143221013u);
    CHECK(remod::read_tex_version(other) == 36u);
    CHECK_FALSE(remod::read_tex_version(tmp.path / "junk.tex"));
    CHECK_FALSE(remod::read_tex_version(tmp.path / "missing.tex"));

    const std::vector<remod::Profile> profiles{re4r()};
    CHECK(remod::profile_for_texture(re4, profiles) == &profiles[0]);
    CHECK(remod::profile_for_texture(other, profiles) == nullptr);
    CHECK_THROWS_WITH(remod::read_tex_meta(other, re4r()), ContainsSubstring("version 36"));
    CHECK(remod::read_tex_meta(re4, re4r()).width == 64);
}

TEST_CASE("NativeConverter: the new .tex keeps the original's header, mip table and padded rows") {
    TempDir tmp;
    remod::NativeConverter conv;
    // 8x4 RGBA8, 2 mips, 16 bytes of padding per row; named the way some tools name textures.
    const fs::path original = real_tex(tmp.path / "ui.tex.re2remake", 143221013, 8, 4, 2, 28, 16);
    const remod::Bgra edit = pattern(8, 4);
    remod::save_image_bgra(tmp.path / "edit.png", edit.width, edit.height, edit.pixels);

    const auto made = conv.save_tex(tmp.path / "edit.png", original, tmp.path / "new.tex.re2remake", re4r());
    CHECK(made.mip_count == 2);
    CHECK(fs::file_size(tmp.path / "new.tex.re2remake") == fs::file_size(original));
    CHECK(bool(head_of(tmp.path / "new.tex.re2remake", 40, 2) == head_of(original, 40, 2)));
    CHECK(max_difference(remod::decode_tex(tmp.path / "new.tex.re2remake", 8), edit) == 0);

    // Back to an image at the visible size (8 wide, not the padded 12), in each edit format.
    for (const char* ext : {".png", ".tga"}) {
        CAPTURE(ext);
        const fs::path out = tmp.path / (std::string("back") + ext);
        CHECK(conv.load_tex(tmp.path / "new.tex.re2remake", out, re4r()).width == 8);
        CHECK(max_difference(remod::load_image(out), edit) == 0);
    }
    CHECK(conv.load_tex(original, tmp.path / "back.jpg", re4r()).width == 8);
    CHECK(remod::image_size(tmp.path / "back.jpg") == std::pair<std::uint32_t, std::uint32_t>{8, 4});
}

TEST_CASE("NativeConverter: BC7 sRGB keeps the mip count and comes back close") {
    TempDir tmp;
    remod::NativeConverter conv;
    const fs::path original = real_tex(tmp.path / "a.tex.143221013", 143221013, 16, 16, 3, 99);
    const remod::Bgra edit = pattern(16, 16);
    remod::save_image_bgra(tmp.path / "edit.png", edit.width, edit.height, edit.pixels);
    const fs::path out = tmp.path / "b.tex";
    CHECK(conv.save_tex(tmp.path / "edit.png", original, out, re4r()).mip_count == 3);
    CHECK(bool(head_of(out, 40, 3) == head_of(original, 40, 3)));
    CHECK(fs::file_size(out) == fs::file_size(original));
    CHECK(max_difference(remod::decode_tex(out, 16), edit) <= 24);  // BC7 is lossy (16 seen on this pattern)
    CHECK(conv.id(re4r()) != remod::NoesisConverter("C:/Windows/System32/cmd.exe").id(re4r()));
}

TEST_CASE("NativeConverter: older games' layout, and what it refuses") {
    TempDir tmp;
    remod::NativeConverter conv;
    remod::Profile re2 = re4r();
    re2.tex_suffix = "10";
    const fs::path original = real_tex(tmp.path / "x.tex.10", 10, 4, 4, 1, 28);
    CHECK(remod::read_tex_meta(original, re2).mip_count == 1);
    const remod::Bgra edit = pattern(4, 4);
    remod::save_image_bgra(tmp.path / "edit.png", 4, 4, edit.pixels);
    conv.save_tex(tmp.path / "edit.png", original, tmp.path / "y.tex.10", re2);
    CHECK(bool(head_of(tmp.path / "y.tex.10", 32, 1) == head_of(original, 32, 1)));
    CHECK(max_difference(remod::decode_tex(tmp.path / "y.tex.10", 4), edit) == 0);

    remod::Profile wilds = re4r();
    wilds.tex_suffix = "241106027";
    CHECK_THROWS_WITH(remod::read_tex_meta(real_tex(tmp.path / "w.tex", 241106027, 4, 4, 1, 28), wilds),
                      ContainsSubstring("GDeflate") && ContainsSubstring("Noesis"));
    CHECK_THROWS_WITH(conv.save_tex(tmp.path / "edit.png", original, tmp.path / "y.tex.10", re2),
                      ContainsSubstring("already exists"));
    CHECK_THROWS_WITH(conv.save_tex(fake_png(tmp.path, 2, 2), original, tmp.path / "z.tex.10", re2),
                      ContainsSubstring("must be 4x4"));
}

TEST_CASE("is_tex_name: .tex with any suffix, not .rtex") {
    for (const char* name : {"a.tex", "a.tex.143221013", "a.tex.re2remake", "a.b.tex.re3remake"})
        CHECK(remod::is_tex_name(name));
    for (const char* name : {"a.rtex", "a.rtex.5", "a.tex.", "a.png", "a.tex.1.2", "a.tex.png", "a.tex.tga"}) CHECK_FALSE(remod::is_tex_name(name));
}

TEST_CASE("load_profiles lists every valid profile and reports broken ones") {
    CHECK(std::ranges::count(remod::load_profiles(REMOD_PROFILES_DIR), std::string("re4r"), &remod::Profile::id) == 1);

    TempDir tmp;
    fs::copy_file(REMOD_PROFILES_DIR "/re4r.toml", tmp.path / "re4r.toml");
    test::write_file(tmp.path / "broken.toml", "[game]\nid = \"x\"\n");
    test::write_file(tmp.path / "notes.txt", "ignored");
    std::vector<std::string> errors;
    const auto profiles = remod::load_profiles(tmp.path, &errors);
    REQUIRE(profiles.size() == 1);
    CHECK(profiles[0].id == "re4r");
    REQUIRE(errors.size() == 1);
    CHECK_THAT(errors[0], ContainsSubstring("'name' must be a non-empty string"));
}

TEST_CASE("image_size reads a PNG's IHDR") {
    TempDir tmp;
    CHECK(remod::image_size(fake_png(tmp.path, 1024, 768)) == std::pair<std::uint32_t, std::uint32_t>{1024, 768});
    std::ofstream(tmp.path / "x.png") << "definitely not a png file here";
    CHECK_THROWS_WITH(remod::image_size(tmp.path / "x.png"), ContainsSubstring("not a PNG"));
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
    CHECK_THROWS_WITH(conv.load_tex(tex, fake_png(tmp.path, 1, 1), re4r()), ContainsSubstring("already exists"));
    CHECK_THROWS_WITH(conv.load_tex(tex, tmp.path / "no_extension", re4r()), ContainsSubstring("must end in .png"));

    remod::Profile tbd = re4r();
    tbd.noesis_export = "TBD";
    CHECK_THROWS_WITH(conv.save_tex(fake_png(tmp.path, 64, 64), tex, tmp.path / "o.tex.143221013", tbd),
                      ContainsSubstring("noesis_export"));
}

TEST_CASE("a failed Noesis conversion reports what Noesis said") {
    const std::string noesis = env("REMOD_NOESIS");
    if (noesis.empty()) SKIP("set REMOD_NOESIS to run");
    remod::NoesisConverter conv(noesis);
    TempDir tmp;
    // Valid header, no image data: passes our checks, then the plugin errors inside Noesis.
    const auto start = std::chrono::steady_clock::now();
    const fs::path tex = fake_tex(tmp.path, 143221013, 64, 64, 1, 5, 99);
    try {
        conv.load_tex(tex, tmp.path / "out.png", re4r());
        FAIL("expected a ConvertError");
    } catch (const ConvertError& e) {
        UNSCOPED_INFO(e.what());
        CHECK_THAT(std::string(e.what()), ContainsSubstring("Noesis said:") || ContainsSubstring("dialog said:"));
    }
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(30));  // not the 2-minute timeout
}

// The optional Noesis converter. CLAUDE.md §8: needs local fixtures + Noesis, never committed. Skips unless both env
// vars are set.
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
        // Padded rows: Noesis exports the padded width and the size check refuses it (CLAUDE.md §9). The built-in
        // converter handles these.
        if (const auto p = remod::read_tex_pixels(e.path(), 1u << 16); p.stored_width != p.width) continue;
        CAPTURE(e.path().string());
        const auto stem = std::to_string(count++);
        const auto loaded = conv.load_tex(e.path(), tmp.path / (stem + ".png"), re4r());
        CHECK(remod::image_size(tmp.path / (stem + ".png")) == std::pair{loaded.width, loaded.height});
        const auto saved =
            conv.save_tex(tmp.path / (stem + ".png"), e.path(), tmp.path / (stem + ".tex." + re4r().tex_suffix), re4r());
        CHECK(saved.width == loaded.width);
        CHECK(saved.height == loaded.height);
        CHECK(saved.format == loaded.format);
        // Mips aren't compared: Noesis writes them down to 8x8 whatever the original has (CLAUDE.md §9; a run warns).
        // Pixels aren't compared: BC7 re-encoding is lossy, so exact image data can't round-trip.

        // The same texture named plain ".tex" must convert too.
        const fs::path plain = tmp.path / (stem + " plain.tex");
        fs::copy_file(e.path(), plain);
        CHECK(conv.load_tex(plain, tmp.path / (stem + " plain.png"), re4r()).width == loaded.width);

        // The other edit formats, both ways: the format comes from the extension.
        for (const char* ext : {".tga", ".jpg"}) {
            CAPTURE(ext);
            const fs::path img = tmp.path / (stem + ext);
            conv.load_tex(e.path(), img, re4r());
            CHECK(remod::image_size(img) == std::pair{loaded.width, loaded.height});
            const auto back = conv.save_tex(img, e.path(), tmp.path / (stem + ext + ".tex." + re4r().tex_suffix), re4r());
            CHECK(back.format == loaded.format);
        }
    }
    REQUIRE(count > 0);
}

// The built-in converter on real textures (REMOD_FIXTURES only, no Noesis): the new file is the original with only its
// pixels changed, so its header and mip table match byte for byte, padded rows included.
TEST_CASE("round trip, built in: tex -> png -> tex keeps the original's header and mip table") {
    const std::string fixtures = env("REMOD_FIXTURES");
    if (fixtures.empty()) SKIP("set REMOD_FIXTURES to run");
    remod::NativeConverter conv;
    TempDir tmp;
    int count = 0;
    for (const auto& e : fs::directory_iterator(fixtures)) {
        if (!remod::profile_for_texture(e.path(), {re4r()})) continue;
        CAPTURE(e.path().string());
        const auto stem = std::to_string(count++);
        const auto start = std::chrono::steady_clock::now();
        const auto loaded = conv.load_tex(e.path(), tmp.path / (stem + ".png"), re4r());
        CHECK(remod::image_size(tmp.path / (stem + ".png")) == std::pair{loaded.width, loaded.height});
        const fs::path out = tmp.path / (stem + ".tex");
        const auto saved = conv.save_tex(tmp.path / (stem + ".png"), e.path(), out, re4r());
        UNSCOPED_INFO(loaded.width << "x" << loaded.height << " " << loaded.format << ": "
                                   << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
                                   << " s");
        CHECK(saved.mip_count == loaded.mip_count);
        CHECK(fs::file_size(out) == fs::file_size(e.path()));
        const bool same_head =  // a bool: Catch can't print the binary strings
            head_of(out, 40, std::uint8_t(loaded.mip_count)) == head_of(e.path(), 40, std::uint8_t(loaded.mip_count));
        CHECK(same_head);
        // The pixels survive: re-encoding what was decoded from the same format loses little.
        const remod::Bgra before = remod::decode_tex(e.path(), 4096), after = remod::decode_tex(out, 4096);
        double total = 0;
        for (size_t i = 0; i < before.pixels.size(); ++i) total += std::abs(int(before.pixels[i]) - int(after.pixels[i]));
        const double mean = total / double(before.pixels.size());
        UNSCOPED_INFO("mean difference " << mean);
        CHECK(mean < 2);
    }
    REQUIRE(count > 0);
}
