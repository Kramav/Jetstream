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

namespace {

// A framed photo, `size` square: transparent outside `margin`, dark wood (40) up to `lip`, a lighter lip (150) up to
// `photo`, then a dark-grey photo (60) with a black block touching its bottom edge and a white one in the middle (its
// own strong edges, which must not be taken for the frame).
remod::Bgra framed(unsigned size, unsigned margin, unsigned lip, unsigned photo) {
    remod::Bgra img{size, size, std::vector<std::uint8_t>(size_t(size) * size * 4, 0)};
    for (unsigned y = 0; y < size; ++y)
        for (unsigned x = 0; x < size; ++x) {
            const unsigned d = std::min({x, y, size - 1 - x, size - 1 - y});  // distance from the image's edge
            std::uint8_t v = 0, a = 255;
            if (d < margin) a = 0;
            else if (d < lip) v = 40;
            else if (d < photo) v = 150;
            else v = 60;
            if (d >= photo && x >= size * 35 / 100 && x < size * 50 / 100 && y >= size * 55 / 100) v = 0;  // to the bottom
            if (d >= photo && x >= size * 55 / 100 && x < size * 65 / 100 && y >= size * 35 / 100 && y < size * 45 / 100)
                v = 255;
            std::uint8_t* p = &img.pixels[(size_t(y) * size + x) * 4];
            p[0] = p[1] = p[2] = v;
            p[3] = a;
        }
    return img;
}

}  // namespace

TEST_CASE("photo_area finds the photo's edge, not the frame's other rings or the photo's content") {
    const remod::Bgra frame = framed(200, 10, 45, 50);
    const remod::PhotoArea area = remod::photo_area(frame, 0, 0, 0);
    CHECK(area.frame_width >= 38);  // the lip/photo edge: 40 px in from the outline (at 10)
    CHECK(area.frame_width <= 43);
    auto in = [&](unsigned x, unsigned y) { return area.mask[size_t(y) * 200 + x]; };
    CHECK(in(100, 100) == 255);
    CHECK(in(53, 53) == 255);   // just inside the photo's corner
    CHECK(in(80, 147) == 255);  // the black block reaching the photo's edge: still the photo
    CHECK(in(46, 100) == 0);    // the lip
    CHECK(in(20, 100) == 0);    // the wood
    CHECK_FALSE(area.outline.empty());

    const remod::PhotoArea grown = remod::photo_area(frame, 0, 3, 0);  // under the lip
    CHECK(grown.mask[100 * 200 + 48] == 255);
    const remod::PhotoArea given = remod::photo_area(frame, 25, 0, 0);  // a width given: no search
    CHECK(given.frame_width == 25);
    const remod::PhotoArea soft = remod::photo_area(frame, 0, 0, 3);  // feathered: a ramp across the edge
    CHECK(soft.mask[100 * 200 + 50] > 0);
    CHECK(soft.mask[100 * 200 + 50] < 255);
}

TEST_CASE("photo_area follows an oval frame") {
    constexpr unsigned W = 240, H = 180;
    remod::Bgra img{W, H, std::vector<std::uint8_t>(size_t(W) * H * 4, 0)};
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = 0; x < W; ++x) {
            const double dx = (double(x) - 120) / 110, dy = (double(y) - 90) / 80;  // the frame's outline
            const double ix = (double(x) - 120) / 80, iy = (double(y) - 90) / 55;   // its oval opening
            std::uint8_t* p = &img.pixels[(size_t(y) * W + x) * 4];
            if (dx * dx + dy * dy > 1) continue;
            p[3] = 255;
            p[0] = p[1] = p[2] = std::uint8_t(ix * ix + iy * iy <= 1 ? 170 : 50);
        }
    const remod::PhotoArea area = remod::photo_area(img, 0, 0, 0);
    auto in = [&](unsigned x, unsigned y) { return area.mask[size_t(y) * W + x]; };
    CHECK(in(120, 90) == 255);
    CHECK(in(195, 90) == 255);  // near the opening's right end (x 200)
    CHECK(in(120, 140) == 255); // near its bottom (y 145)
    CHECK(in(185, 50) == 0);    // a corner of its box: frame, not photo
    CHECK(in(208, 90) == 0);
}

TEST_CASE("replace_photo fills the area end to end and carries the old photo's ageing over") {
    remod::Bgra frame = framed(200, 10, 45, 50);
    for (unsigned y = 60; y < 80; ++y)  // a brown stain on the old grey photo
        for (unsigned x = 60; x < 80; ++x) {
            std::uint8_t* p = &frame.pixels[(size_t(y) * 200 + x) * 4];
            p[0] = 30, p[1] = 60, p[2] = 100;
        }
    const remod::PhotoArea area = remod::photo_area(frame, 0, 0, 0);
    const remod::Bgra red{4, 3, {0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0,
                                 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255,
                                 0, 0, 255, 255}};
    auto px = [](const remod::Bgra& img, unsigned x, unsigned y) {
        const std::uint8_t* p = &img.pixels[(size_t(y) * img.width + x) * 4];
        return std::array<int, 4>{p[0], p[1], p[2], p[3]};
    };

    const remod::Bgra plain = remod::replace_photo(frame, red, area.mask, {0, 0, 0, 0});  // no ageing: just red
    CHECK(px(plain, 100, 100) == std::array<int, 4>{0, 0, 255, 255});
    CHECK(px(plain, 55, 145) == std::array<int, 4>{0, 0, 255, 255});  // end to end, corners too
    CHECK(px(plain, 20, 100) == px(frame, 20, 100));                   // the frame untouched
    CHECK(px(plain, 5, 5)[3] == 0);                                    // its alpha kept

    const remod::Bgra grey{1, 1, {128, 128, 128, 255}};
    const remod::Bgra stained = remod::replace_photo(frame, grey, area.mask, {0, 0, 1, 0});
    CHECK(px(stained, 70, 70)[2] > px(stained, 70, 70)[0] + 30);  // the stain's brown on the new picture: more red
    CHECK(std::abs(px(stained, 120, 120)[2] - px(stained, 120, 120)[0]) < 5);  // still grey elsewhere
    CHECK(px(plain, 70, 70) == px(plain, 120, 120));  // and not without Stains

    const remod::Bgra toned = remod::replace_photo(frame, red, area.mask, {1, 0, 0, 0});
    const auto t = px(toned, 120, 120);  // red becomes the old photo's dark grey
    CHECK(std::abs(t[2] - t[1]) < 30);
    CHECK(t[2] < 120);
}
