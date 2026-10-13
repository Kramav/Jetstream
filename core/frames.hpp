#pragma once
// Pictures in frames (user, 2026-10-12): a picture of its own on each picture-frame mesh in the world. RE4R's
// paintings share atlases (sm34_543_a holds five portraits, one per frame mesh sm34_543 ... sm34_548), so each
// atlas is written once with every frame's picture in it, at the rectangle that frame's canvas UVs cover.
#include "package.hpp"
#include "profile.hpp"
#include "texture_converter.hpp"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace remod {

struct FramePicture {
    std::string mesh;                 // the frame's .mesh, relative to `natives` (as the game names it)
    std::filesystem::path picture;    // its picture (png, jpg, tga); empty: none, left as it is
};

// Each frame's canvas is its parts whose colour texture's file name `is_canvas` says yes to; the picture fills
// (cropped to fit) the box their UVs cover on it, where they cover it. Every atlas touched is written once, at its
// size, format and mips, with its streaming/ copy if it has one; under `out` at their game paths. Frames with no
// canvas part are left out; with a canvas but no picture, named in `report`. Throws PackageError when nothing is left
// to write (naming the frames that have a canvas, so pictures can be named after them).
// ponytail: the picture goes in upright, as RE4R's atlases hold their paintings; a canvas mapped turned or mirrored
// would show it so (read the canvas's orientation from the mesh if one turns up).
std::vector<PackageFile> pictures_in_frames(const std::vector<FramePicture>& frames,
                                            const std::function<bool(const std::string&)>& is_canvas,
                                            const std::filesystem::path& natives, const Profile& profile,
                                            ITextureConverter& converter, const std::filesystem::path& out,
                                            std::string* report = nullptr);

// Which picture goes to which name (user, 2026-10-12: "why do I need to name each of them"): a picture already named
// after one (its name up to the first '.', ignoring case) is that one's; the rest go to the names still without one, in
// order: names as given, pictures by name with numbers in number order (2 before 10). Returns one picture per name,
// empty where pictures ran out; `unused` gets the pictures left over.
std::vector<std::filesystem::path> match_pictures(const std::vector<std::string>& names,
                                                  std::vector<std::filesystem::path> pictures,
                                                  std::vector<std::filesystem::path>* unused = nullptr);

}  // namespace remod
