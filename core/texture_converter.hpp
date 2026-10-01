#pragma once
// LoadTex / SaveTex (CLAUDE.md §4). File-based: Noesis converts .tex <-> PNG/TGA/JPG directly, so pixels never
// pass through this process. ponytail: add in-memory decoding (WIC) when the app needs a texture preview.
#include "process.hpp"
#include "profile.hpp"
#include "types.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace remod {

struct ConvertError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// The version number in an RE Engine .tex header (e.g. 143221013 for RE4R), read from the file's content,
// so it works whatever the file is named. nullopt if the file isn't an RE Engine texture.
std::optional<std::uint32_t> read_tex_version(const std::filesystem::path& tex);

// The profile whose tex_suffix matches the file's header version, or nullptr.
const Profile* profile_for_texture(const std::filesystem::path& tex, const std::vector<Profile>& profiles);

// Reads TexMeta from a .tex header (source_path left empty; the caller knows the game path).
// Layout from fmt_RE_MESH's reader [plugin source], checked against one RE4R texture (CLAUDE.md §9).
TexMeta read_tex_meta(const std::filesystem::path& tex, const Profile& profile);

// Image formats for the edit step, by extension: Noesis picks the format from it, both ways. BMP is left out
// because Noesis writes it without alpha (spike 2026-09-30). JPG works but loses quality and transparency.
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
    // SaveTex: edited image (PNG, TGA or JPG) -> .tex at tex_out (must not exist) with original_tex's size,
    // format and mips.
    // Throws ConvertError if the result doesn't match.
    virtual TexMeta save_tex(const std::filesystem::path& png, const std::filesystem::path& original_tex,
                             const std::filesystem::path& tex_out, const Profile& profile) = 0;
};

// Noesis + fmt_RE_MESH plugin, command-line mode (CLAUDE.md §9 spike results).
class NoesisConverter final : public ITextureConverter {
public:
    explicit NoesisConverter(std::filesystem::path noesis_exe,
                             std::chrono::milliseconds timeout = std::chrono::minutes(2));
    TexMeta load_tex(const std::filesystem::path& tex, const std::filesystem::path& png_out,
                     const Profile& profile) override;
    TexMeta save_tex(const std::filesystem::path& png, const std::filesystem::path& original_tex,
                     const std::filesystem::path& tex_out, const Profile& profile) override;

private:
    ProcessResult run(std::vector<std::wstring> args, const std::filesystem::path& log) const;
    std::filesystem::path exe_;
    std::chrono::milliseconds timeout_;
};

}  // namespace remod
