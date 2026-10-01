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

}  // namespace remod
