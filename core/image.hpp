#pragma once
// Small image jobs via WIC (Windows Imaging Component, built into Windows): no image library dependency.
#include <cstdint>
#include <filesystem>
#include <vector>

namespace remod {

// Tiles `images` (whatever WIC reads: png/jpg/bmp..., plus tga) into one PNG at `out`: a near-square grid of
// tile x tile cells, each image scaled to fit (aspect kept) and centred on a dark background.
// Used to combine several previews into the single screenshot Fluffy shows. Throws std::runtime_error.
void tile_images(const std::vector<std::filesystem::path>& images, const std::filesystem::path& out,
                 unsigned tile = 512);

// Reads an image (TGA by its own reader, anything else via WIC) as 32-bit BGRA pixels, top-down.
std::vector<std::uint8_t> read_image_bgra(const std::filesystem::path& file, unsigned& width, unsigned& height);

// Writes 32-bit BGRA pixels (width * height * 4 bytes, top-down) as a PNG.
void save_png_bgra(const std::filesystem::path& out, unsigned width, unsigned height,
                   const std::vector<std::uint8_t>& bgra);

// ---- Image operations for the image blocks (Adjust colour, Resize image, Overlay image) ----

struct Bgra {
    unsigned width = 0, height = 0;
    std::vector<std::uint8_t> pixels;  // 32-bit BGRA, top-down
};
Bgra load_image(const std::filesystem::path& file);                   // read_image_bgra
void save_png(const std::filesystem::path& file, const Bgra& image);  // save_png_bgra

// Hue in degrees (a luminance-keeping rotation of the colours); saturation, brightness and contrast from -1 to 1, 0 as
// is (saturation -1: grey). Colour only: alpha stays as it is (RE textures often keep other data there).
void adjust_colour(Bgra& image, float hue, float saturation, float brightness, float contrast);

// Stretch: to exactly width x height. Fit: the whole image inside, aspect kept, transparent bars. Fill: covers it,
// aspect kept, the overflow cropped (centred). WIC's Fant scaling. Throws on a zero size.
enum class Fit { Stretch, Fit, Fill };
Bgra resize_image(const Bgra& image, unsigned width, unsigned height, Fit fit);

// `top` over `base` with its top-left at (x, y), weighted by top's alpha times `opacity` (0 to 1), clipped to base.
// Base's alpha is kept.
void overlay_image(Bgra& base, const Bgra& top, int x, int y, float opacity);

}  // namespace remod
