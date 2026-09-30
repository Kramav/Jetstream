#include "image.hpp"
#include "texture_converter.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;
using Size = std::pair<std::uint32_t, std::uint32_t>;

namespace {

fs::path png(const fs::path& dir, const char* name, unsigned w, unsigned h) {
    const fs::path p = dir / name;
    remod::save_png_bgra(p, w, h, std::vector<std::uint8_t>(size_t(w) * h * 4, 128));
    return p;
}

}  // namespace

TEST_CASE("save_png_bgra writes a PNG of the given size") {
    test::TempDir tmp;
    CHECK(remod::png_size(png(tmp.path, "a.png", 37, 21)) == Size{37, 21});
    CHECK_THROWS_AS(remod::save_png_bgra(tmp.path / "b.png", 4, 4, std::vector<std::uint8_t>(3)), std::runtime_error);
}

TEST_CASE("tile_images lays previews out in a near-square grid") {
    test::TempDir tmp;
    const auto a = png(tmp.path, "a.png", 100, 50), b = png(tmp.path, "b.png", 64, 64), c = png(tmp.path, "c.png", 30, 90);

    remod::tile_images({a}, tmp.path / "one.png", 256);
    CHECK(remod::png_size(tmp.path / "one.png") == Size{256, 256});
    remod::tile_images({a, b}, tmp.path / "two.png", 256);
    CHECK(remod::png_size(tmp.path / "two.png") == Size{512, 256});
    remod::tile_images({a, b, c}, tmp.path / "three.png", 256);
    CHECK(remod::png_size(tmp.path / "three.png") == Size{512, 512});
    remod::tile_images({a, b, c, a, b}, tmp.path / "five.png", 100);
    CHECK(remod::png_size(tmp.path / "five.png") == Size{300, 200});

    test::write_file(tmp.path / "junk.png", "not an image");
    CHECK_THROWS_WITH(remod::tile_images({a, tmp.path / "junk.png"}, tmp.path / "x.png"), ContainsSubstring("junk.png"));
    CHECK_THROWS_AS(remod::tile_images({}, tmp.path / "y.png"), std::runtime_error);
}
