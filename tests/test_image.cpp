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

namespace {

// A w x h image of one BGRA colour.
remod::Bgra solid(unsigned w, unsigned h, std::array<std::uint8_t, 4> bgra) {
    remod::Bgra image{w, h, {}};
    for (unsigned i = 0; i < w * h; ++i) image.pixels.insert(image.pixels.end(), bgra.begin(), bgra.end());
    return image;
}

std::array<int, 4> at(const remod::Bgra& image, unsigned x, unsigned y) {
    const std::uint8_t* p = &image.pixels[(size_t(y) * image.width + x) * 4];
    return {p[0], p[1], p[2], p[3]};
}

}  // namespace

TEST_CASE("adjust_colour changes colour only, never alpha") {
    const remod::Bgra red = solid(2, 2, {20, 30, 200, 77});  // B, G, R, A
    remod::Bgra same = red;
    remod::adjust_colour(same, 0, 0, 0, 0);
    CHECK(same.pixels == red.pixels);  // all zero: unchanged

    remod::Bgra brighter = red;
    remod::adjust_colour(brighter, 0, 0, 0.2f, 0);
    CHECK(at(brighter, 0, 0)[2] > 200);
    CHECK(at(brighter, 0, 0)[3] == 77);

    remod::Bgra grey = red;
    remod::adjust_colour(grey, 0, -1, 0, 0);
    const auto g = at(grey, 1, 1);
    CHECK(g[0] == g[1]);
    CHECK(g[1] == g[2]);
    CHECK(g[3] == 77);

    remod::Bgra turned = red;
    remod::adjust_colour(turned, 180, 0, 0, 0);  // red turns towards cyan: less red, more green and blue
    CHECK(at(turned, 0, 0)[2] < 200);
    CHECK(at(turned, 0, 0)[1] > 30);
    CHECK(at(turned, 0, 0)[3] == 77);

    remod::Bgra flat = red;
    remod::adjust_colour(flat, 0, 0, 0, -1);  // no contrast: mid grey
    CHECK(at(flat, 0, 0)[2] == 128);
}

TEST_CASE("resize_image: stretch, fit (transparent bars) and fill (cropped)") {
    const remod::Bgra wide = solid(40, 20, {10, 20, 30, 255});
    const remod::Bgra stretched = remod::resize_image(wide, 16, 16, remod::Fit::Stretch);
    CHECK(stretched.width == 16);
    CHECK(stretched.height == 16);
    CHECK(at(stretched, 0, 0)[3] == 255);

    const remod::Bgra fitted = remod::resize_image(wide, 16, 16, remod::Fit::Fit);  // 16x8 band in the middle
    CHECK(fitted.width == 16);
    CHECK(at(fitted, 8, 0)[3] == 0);    // bar above
    CHECK(at(fitted, 8, 8)[3] == 255);  // the image
    CHECK(at(fitted, 8, 15)[3] == 0);   // bar below

    const remod::Bgra filled = remod::resize_image(wide, 16, 16, remod::Fit::Fill);  // 32x16, centre kept
    CHECK(at(filled, 0, 0)[3] == 255);
    CHECK(at(filled, 15, 15)[3] == 255);
    CHECK_THROWS(remod::resize_image(wide, 0, 16, remod::Fit::Fit));
}

TEST_CASE("overlay_image: placed, blended by opacity, clipped, base alpha kept") {
    remod::Bgra base = solid(4, 4, {0, 0, 0, 100});
    const remod::Bgra top = solid(2, 2, {200, 200, 200, 255});
    remod::overlay_image(base, top, 3, 3, 1);  // only its top-left pixel lands on the base
    CHECK(at(base, 3, 3) == std::array<int, 4>{200, 200, 200, 100});
    CHECK(at(base, 2, 2) == std::array<int, 4>{0, 0, 0, 100});

    remod::Bgra half = solid(4, 4, {0, 0, 0, 255});
    remod::overlay_image(half, top, -1, -1, 0.5f);  // half opacity, partly off the left and top
    CHECK(at(half, 0, 0) == std::array<int, 4>{100, 100, 100, 255});
    CHECK(at(half, 1, 1)[0] == 0);
}
