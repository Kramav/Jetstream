#pragma once
// LoadTex / SaveTex (CLAUDE.md §4): .tex <-> PNG/TGA/JPG, file to file. Built in (NativeConverter, DirectXTex), or
// optionally through Noesis (NoesisConverter).
#include "image.hpp"
#include "mesh.hpp"
#include "process.hpp"
#include "profile.hpp"
#include "types.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

namespace remod {

struct ConvertError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// A texture by (lower-case) name: "x.tex", or "x.tex.<suffix>" with any suffix (the version, 143221013, or a tool's
// name for the game, re2remake...). Not ".rtex" (render targets). Which game it is comes from its header.
bool is_tex_name(const std::string& name);

// The version number in an RE Engine .tex header (e.g. 143221013 for RE4R), read from the file's content,
// so it works whatever the file is named. nullopt if the file isn't an RE Engine texture.
std::optional<std::uint32_t> read_tex_version(const std::filesystem::path& tex);

// The profile whose tex_suffix matches the file's header version, or nullptr.
const Profile* profile_for_texture(const std::filesystem::path& tex, const std::vector<Profile>& profiles);

// Reads TexMeta from a .tex header (source_path left empty; the caller knows the game path). Layout by version
// [REE-Lib TexFile.cs, MIT]; the file's name doesn't matter (.tex, .tex.143221013, .tex.re2remake...).
TexMeta read_tex_meta(const std::filesystem::path& tex, const Profile& profile);

// One mip level of a .tex exactly as stored, for previews: the GPU decodes `format` itself (RE Engine uses DXGI's
// format numbers). Read straight from the file, layout as read_tex_meta. Image 0 of an array or cubemap.
struct TexPixels {
    std::uint32_t format = 0;                           // DXGI_FORMAT
    std::uint32_t width = 0, height = 0;                // this mip's visible size
    std::uint32_t stored_width = 0, stored_height = 0;  // as stored: rows padded to the pitch, BC 4x4 blocks whole
    std::uint32_t row_pitch = 0;                        // bytes per row (per row of blocks for BC formats)
    std::vector<std::uint8_t> data;
};

// The largest mip no bigger than max_side on either side, else the smallest. Throws ConvertError for formats
// the browser can't show or a damaged file.
TexPixels read_tex_pixels(const std::filesystem::path& tex, std::uint32_t max_side);
// read_tex_pixels' mip, then each smaller one stored, for a preview the GPU shrinks smoothly (one large mip drawn small
// skips pixels: fine detail turns grainy). Stops early where a mip's stored size isn't half the one above's.
std::vector<TexPixels> read_tex_mips(const std::filesystem::path& tex, std::uint32_t max_side);

// The same mip (read_tex_pixels) decoded to 32-bit BGRA at its visible size (padding cut off): BC1-7 decompressed and
// other formats converted by DirectXTex. sRGB formats keep their bytes, as the browser shows them. `full_width` and
// `full_height` get the texture's own size (mip 0). For previews before anything is exported. Throws ConvertError.
Bgra decode_tex(const std::filesystem::path& tex, std::uint32_t max_side, unsigned* full_width = nullptr,
                unsigned* full_height = nullptr);

// Image formats for the edit step, by extension, both ways. BMP is left out because Noesis writes it without alpha
// (spike 2026-09-30). JPG works but loses quality and transparency.
inline constexpr const char* kEditImageFormats = "png,tga,jpg,jpeg";

// True if `file` ends in one of kEditImageFormats, ignoring case.
bool is_edit_image(const std::filesystem::path& file);

// Width and height from an image's header: PNG (IHDR chunk), JPG (SOF marker) or, by extension, TGA.
std::pair<std::uint32_t, std::uint32_t> image_size(const std::filesystem::path& file);

class ITextureConverter {
public:
    virtual ~ITextureConverter() = default;
    // LoadTex: game .tex -> image at png_out (must not exist; PNG, TGA or JPG by its extension).
    // Returns the .tex's metadata.
    virtual TexMeta load_tex(const std::filesystem::path& tex, const std::filesystem::path& png_out,
                             const Profile& profile) = 0;
    // SaveTex: edited image (PNG, TGA or JPG) -> .tex at tex_out (must not exist) with original_tex's size and
    // format. The mip count may differ (Noesis writes mips down to 8x8); compare the result to warn about it.
    // Throws ConvertError if the result doesn't match.
    virtual TexMeta save_tex(const std::filesystem::path& png, const std::filesystem::path& original_tex,
                             const std::filesystem::path& tex_out, const Profile& profile) = 0;
    // Which converter and settings made a result, for the run cache's key: a different one converts again.
    virtual std::string id(const Profile&) const { return {}; }
};

// Built in: decodes with DirectXTex (decode_tex), and writes a .tex by copying the original and replacing each mip's
// pixels in place, at the original's pitch and size. So the header, flags, mip count and layout (padded rows too)
// stay the original's. Image 0 of single-image textures only, as Noesis.
class NativeConverter final : public ITextureConverter {
public:
    TexMeta load_tex(const std::filesystem::path& tex, const std::filesystem::path& png_out,
                     const Profile& profile) override;
    TexMeta save_tex(const std::filesystem::path& png, const std::filesystem::path& original_tex,
                     const std::filesystem::path& tex_out, const Profile& profile) override;
    std::string id(const Profile&) const override { return "native1"; }
};

// Optional: Noesis + fmt_RE_MESH plugin, command-line mode (CLAUDE.md §9 spike results). Neither is ours to ship;
// the user installs them.
class NoesisConverter final : public ITextureConverter {
public:
    explicit NoesisConverter(std::filesystem::path noesis_exe,
                             std::chrono::milliseconds timeout = std::chrono::minutes(2));
    TexMeta load_tex(const std::filesystem::path& tex, const std::filesystem::path& png_out,
                     const Profile& profile) override;
    TexMeta save_tex(const std::filesystem::path& png, const std::filesystem::path& original_tex,
                     const std::filesystem::path& tex_out, const Profile& profile) override;
    std::string id(const Profile& p) const override { return "noesis " + p.noesis_export; }
    // A game .mesh's shape (highest LOD) for the 3D view, via a temporary OBJ. Throws ConvertError.
    MeshModel load_mesh(const std::filesystem::path& mesh);

private:
    ProcessResult run(std::vector<std::wstring> args, const std::filesystem::path& log) const;
    std::filesystem::path exe_;
    std::chrono::milliseconds timeout_;
};

// The converter a run uses: Noesis when `noesis_exe` is given (ConvertError if it isn't there), else the built-in one.
std::unique_ptr<ITextureConverter> make_converter(const std::filesystem::path& noesis_exe);

}  // namespace remod
