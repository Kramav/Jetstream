#pragma once
// Small image jobs via WIC (Windows Imaging Component, built into Windows): no image library dependency.
#include <array>
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

// ---- Replace photo: a picture into a framed photo's place, keeping the old photo's ageing ----

// The old photo's area inside an opaque frame. A frame's inner edge runs parallel to its outer outline (alpha, or the
// image's edge): the edge strength is averaged at each distance from the outline, and the innermost strong ring is
// the photo's edge (the photo's own content doesn't line up with the outline, so it averages out). The outline is
// then snapped to the actual edge within a band around that ring (closed contour, dynamic programming over angles).
// Rectangles, ovals and arches work. `frame_width` > 0 skips the search (distance from the outline, pixels); `grow`
// moves the edge outwards (negative: inwards); `feather` softens it (pixels). `scale` (pixels per real pixel, e.g. on a
// thumbnail) only puts error messages in real pixels.
struct PhotoArea {
    std::vector<std::uint8_t> mask;             // per pixel, 0-255
    std::vector<std::array<float, 2>> outline;  // the edge found, a closed polygon (x, y)
    float frame_width = 0;                      // used (found, when asked for 0)
};
PhotoArea photo_area(const Bgra& frame, float frame_width, float grow, float feather, float scale = 1);

// How much of the old photo's ageing goes onto the new picture, 0-1 each: its tone (brightness, contrast, colour
// cast), its darkening towards its edge (the frame's shadow), its colour damage (stains: colour its toning doesn't
// explain, applied as a darkening tint), its fine detail (scratches, specks: what a median filter removes, so not the
// old picture's outlines).
struct Ageing {
    float tone = 1, shading = 1, stains = 1, detail = 0;
};
// Which part of the picture shows (user, 2026-10-02: heads near an edge got cut off): `zoom` 1 just covers the area,
// 2 shows half as much; `x`, `y` from -1 (its left / top edge) to 1 (right / bottom), 0 the middle.
struct Framing {
    float zoom = 1, x = 0, y = 0;
};
// `picture` covering the masked area end to end (aspect kept, cropped as `framing` says), aged from the old photo
// underneath, blended into `frame` through the mask. The frame's alpha is kept. `scale` = pixels per real pixel (blur
// sizes follow it).
Bgra replace_photo(const Bgra& frame, const Bgra& picture, const std::vector<std::uint8_t>& mask, const Ageing& ageing,
                   float scale = 1, const Framing& framing = {});

}  // namespace remod
