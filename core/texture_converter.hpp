#pragma once
// LoadTex / SaveTex (CLAUDE.md §4). File-based: Noesis converts .tex <-> PNG directly, so pixels never
// pass through this process. ponytail: add in-memory decoding (WIC) when the app needs a texture preview.
#include "profile.hpp"
#include "types.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace remod {

struct ConvertError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Reads TexMeta from a .tex header (source_path left empty; the caller knows the game path).
// Layout from fmt_RE_MESH's reader [plugin source], checked against one RE4R texture (CLAUDE.md §9).
TexMeta read_tex_meta(const std::filesystem::path& tex, const Profile& profile);

// Width and height from a PNG's IHDR chunk (PNG spec).
std::pair<std::uint32_t, std::uint32_t> png_size(const std::filesystem::path& png);

class ITextureConverter {
public:
    virtual ~ITextureConverter() = default;
    // LoadTex: game .tex -> PNG at png_out (must not exist). Returns the .tex's metadata.
    virtual TexMeta load_tex(const std::filesystem::path& tex, const std::filesystem::path& png_out,
                             const Profile& profile) = 0;
    // SaveTex: edited PNG -> .tex at tex_out (must not exist) with original_tex's size, format and mips.
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
    std::filesystem::path exe_;
    std::chrono::milliseconds timeout_;
};

}  // namespace remod
