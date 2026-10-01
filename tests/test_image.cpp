#include "image.hpp"
#include "texture_converter.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>

using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;
using Size = std::pair<std::uint32_t, std::uint32_t>;

namespace {

fs::path png(const fs::path& dir, const char* name, unsigned w, unsigned h) {
    const fs::path p = dir / name;
    remod::save_png_bgra(p, w, h, std::vector<std::uint8_t>(size_t(w) * h * 4, 128));
    return p;
}

// A 2x2 TGA: top row red, green; bottom row blue, half-transparent white. `rle` writes each pixel as a
// one-pixel run packet. Bottom-left origin stores the bottom row first, as GIMP does by default.
fs::path tga(const fs::path& dir, const char* name, bool rle, bool top_left) {
    using P = std::array<std::uint8_t, 4>;  // B, G, R, A
    const P red{0, 0, 255, 255}, green{0, 255, 0, 255}, blue{255, 0, 0, 255}, white{255, 255, 255, 128};
    const std::array<P, 4> order = top_left ? std::array{red, green, blue, white} : std::array{blue, white, red, green};
    std::string b(18, '\0');
    b[2] = char(rle ? 10 : 2);
    b[12] = b[14] = 2;  // width, height
    b[16] = 32;
    b[17] = char(top_left ? 0x28 : 0x08);  // origin bit, 8 alpha bits
    for (const P& p : order) {
        if (rle) b += static_cast<char>(std::uint8_t{0x80});  // run of 1
        b.append(reinterpret_cast<const char*>(p.data()), 4);
    }
    test::write_file(dir / name, b);
    return dir / name;
}

}  // namespace

TEST_CASE("read_image_bgra reads TGA (both origins, RLE) and WIC formats top-down") {
    test::TempDir tmp;
    const std::vector<std::uint8_t> expected{0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, 255, 255, 255, 128};
    for (const auto& [rle, top] : {std::pair{false, false}, {true, true}, {false, true}, {true, false}}) {
        CAPTURE(rle, top);
        const fs::path p = tga(tmp.path, "t.tga", rle, top);
        unsigned w = 0, h = 0;
        CHECK(remod::read_image_bgra(p, w, h) == expected);
        CHECK(Size{w, h} == Size{2, 2});
        CHECK(remod::image_size(p) == Size{2, 2});
        // Through tile_images (tile = image size, so no scaling) and back via WIC.
        remod::tile_images({p}, tmp.path / "t.png", 2);
        CHECK(remod::read_image_bgra(tmp.path / "t.png", w, h) == expected);
    }
    test::write_file(tmp.path / "short.tga", std::string("\0\0\x02", 3));
    unsigned w = 0, h = 0;
    CHECK_THROWS_WITH(remod::read_image_bgra(tmp.path / "short.tga", w, h), ContainsSubstring("short.tga"));
}

TEST_CASE("image_size reads PNG, JPG and TGA headers") {
    test::TempDir tmp;
    CHECK(remod::image_size(png(tmp.path, "a.png", 37, 21)) == Size{37, 21});
    // JPEG: SOI, an APP0 segment to skip, then SOF0 with height 300, width 500.
    std::string jpg = "\xFF\xD8\xFF\xE0";
    jpg += std::string("\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00", 16);
    jpg += std::string("\xFF\xC0\x00\x11\x08\x01\x2C\x01\xF4\x03\x01\x22\x00\x02\x11\x01\x03\x11\x01", 19);
    test::write_file(tmp.path / "a.jpg", jpg);
    CHECK(remod::image_size(tmp.path / "a.jpg") == Size{500, 300});
    test::write_file(tmp.path / "junk.tga", "not a picture at all, just words");
    CHECK_THROWS_WITH(remod::image_size(tmp.path / "junk.tga"), ContainsSubstring("not a PNG, TGA or JPG"));
    CHECK(remod::is_edit_image("x.TGA"));
    CHECK(remod::is_edit_image("x.jpeg"));
    CHECK_FALSE(remod::is_edit_image("x.bmp"));
}

TEST_CASE("save_png_bgra writes a PNG of the given size") {
    test::TempDir tmp;
    CHECK(remod::image_size(png(tmp.path, "a.png", 37, 21)) == Size{37, 21});
    CHECK_THROWS_AS(remod::save_png_bgra(tmp.path / "b.png", 4, 4, std::vector<std::uint8_t>(3)), std::runtime_error);
}

TEST_CASE("tile_images lays previews out in a near-square grid") {
    test::TempDir tmp;
    const auto a = png(tmp.path, "a.png", 100, 50), b = png(tmp.path, "b.png", 64, 64), c = png(tmp.path, "c.png", 30, 90);

    remod::tile_images({a}, tmp.path / "one.png", 256);
    CHECK(remod::image_size(tmp.path / "one.png") == Size{256, 256});
    remod::tile_images({a, b}, tmp.path / "two.png", 256);
    CHECK(remod::image_size(tmp.path / "two.png") == Size{512, 256});
    remod::tile_images({a, b, c}, tmp.path / "three.png", 256);
    CHECK(remod::image_size(tmp.path / "three.png") == Size{512, 512});
    remod::tile_images({a, b, c, a, b}, tmp.path / "five.png", 100);
    CHECK(remod::image_size(tmp.path / "five.png") == Size{300, 200});

    test::write_file(tmp.path / "junk.png", "not an image");
    CHECK_THROWS_WITH(remod::tile_images({a, tmp.path / "junk.png"}, tmp.path / "x.png"), ContainsSubstring("junk.png"));
    CHECK_THROWS_AS(remod::tile_images({}, tmp.path / "y.png"), std::runtime_error);
}
