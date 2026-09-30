#include "texture_converter.hpp"

#include "process.hpp"

#include <array>
#include <fstream>
#include <map>
#include <random>
#include <sstream>

namespace remod {

namespace fs = std::filesystem;

namespace {

std::string read_prefix(const fs::path& file, size_t n) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw ConvertError("cannot open " + file.string());
    std::string bytes(n, '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(n));
    bytes.resize(static_cast<size_t>(in.gcount()));
    return bytes;
}

std::uint32_t le32(const std::string& b, size_t at) {
    std::uint32_t v = 0;
    for (size_t i = 0; i < 4; ++i) v |= std::uint32_t(std::uint8_t(b[at + i])) << (8 * i);
    return v;
}
std::uint32_t le16(const std::string& b, size_t at) { return std::uint8_t(b[at]) | (std::uint8_t(b[at + 1]) << 8); }
std::uint32_t be32(const std::string& b, size_t at) {
    std::uint32_t v = 0;
    for (size_t i = 0; i < 4; ++i) v = (v << 8) | std::uint8_t(b[at + i]);
    return v;
}

// DXGI_FORMAT values (official D3D enum) for the formats the plugin's writer handles.
std::string format_name(std::uint32_t dxgi) {
    static const std::map<std::uint32_t, const char*> names{
        {10, "R16G16B16A16_FLOAT"}, {28, "R8G8B8A8_UNORM"}, {29, "R8G8B8A8_UNORM_SRGB"}, {61, "R8_UNORM"},
        {71, "BC1_UNORM"},          {72, "BC1_UNORM_SRGB"}, {77, "BC3_UNORM"},           {80, "BC4_UNORM"},
        {83, "BC5_UNORM"},          {95, "BC6H_UF16"},      {98, "BC7_UNORM"},           {99, "BC7_UNORM_SRGB"}};
    const auto it = names.find(dxgi);
    return it != names.end() ? it->second : "DXGI_FORMAT " + std::to_string(dxgi);
}

bool same_texture(const TexMeta& a, const TexMeta& b) {
    return a.width == b.width && a.height == b.height && a.format == b.format && a.mip_count == b.mip_count &&
           a.array_count == b.array_count;
}

std::string describe(const TexMeta& m) {
    return std::to_string(m.width) + "x" + std::to_string(m.height) + " " + m.format + ", " +
           std::to_string(m.mip_count) + " mips, " + std::to_string(m.array_count) + " image(s)";
}

// Scratch folder under %TEMP%, removed on scope exit.
struct TempDir {
    fs::path path = fs::temp_directory_path() / ("remod_" + std::to_string(std::random_device{}()));
    TempDir() { fs::create_directories(path); }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void require_absent(const fs::path& out) {
    if (fs::exists(out)) throw ConvertError("output already exists, refusing to overwrite: " + out.string());
}

}  // namespace

TexMeta read_tex_meta(const fs::path& tex, const Profile& profile) {
    // [plugin source] magic@0 "TEX\0", version@4, width@8, height@10, images@14, mip header bytes@15 (16 per mip),
    // DXGI format@16. ponytail: this is the layout for tex versions > 27 (RE4R); other games need the plugin's
    // other branch, so the version is pinned to the profile's suffix.
    const std::string b = read_prefix(tex, 20);
    if (b.size() < 20 || le32(b, 0) != 0x00584554)
        throw ConvertError(tex.string() + " is not an RE Engine .tex file");
    if (std::to_string(le32(b, 4)) != profile.tex_suffix)
        throw ConvertError(tex.string() + ": tex version " + std::to_string(le32(b, 4)) + " does not match profile " +
                           profile.id + " (expected " + profile.tex_suffix + ")");
    TexMeta m;
    m.game_profile = profile.id;
    m.width = le16(b, 8);
    m.height = le16(b, 10);
    m.array_count = std::uint8_t(b[14]);
    m.mip_count = std::uint8_t(b[15]) / 16u;
    m.format = format_name(le32(b, 16));
    return m;
}

std::pair<std::uint32_t, std::uint32_t> png_size(const fs::path& png) {
    const std::string b = read_prefix(png, 24);
    if (b.size() < 24 || b.compare(0, 8, "\x89PNG\r\n\x1a\n") != 0 || b.compare(12, 4, "IHDR") != 0)
        throw ConvertError(png.string() + " is not a PNG file");
    return {be32(b, 16), be32(b, 20)};
}

NoesisConverter::NoesisConverter(fs::path noesis_exe, std::chrono::milliseconds timeout)
    : exe_(std::move(noesis_exe)), timeout_(timeout) {
    if (!fs::is_regular_file(exe_)) throw ConvertError("Noesis not found: " + exe_.string());
}

// Noesis writes its messages straight to the console, not to stdout/stderr, so they can't be captured
// (CLAUDE.md §9), and it exits 0 even when a conversion fails. Success is judged only from the output file.
TexMeta NoesisConverter::load_tex(const fs::path& tex, const fs::path& png_out, const Profile& profile) {
    const TexMeta meta = read_tex_meta(tex, profile);
    require_absent(png_out);
    const auto r = run_process(exe_, {L"?cmode", tex.wstring(), png_out.wstring()}, timeout_);
    if (r.exit_code != 0 || !fs::is_regular_file(png_out))
        throw ConvertError("Noesis failed to convert " + tex.string() + " to PNG (exit " +
                           std::to_string(r.exit_code) + ")");
    if (png_size(png_out) != std::pair{meta.width, meta.height}) {
        fs::remove(png_out);
        throw ConvertError("Noesis wrote a PNG whose size differs from the .tex (" + describe(meta) + ")");
    }
    return meta;
}

TexMeta NoesisConverter::save_tex(const fs::path& png, const fs::path& original_tex, const fs::path& tex_out,
                                  const Profile& profile) {
    if (profile.noesis_export == "TBD")
        throw ConvertError("profile " + profile.id + " has noesis_export = \"TBD\"; the Noesis export options "
                           "for this game are not known yet");
    const TexMeta original = read_tex_meta(original_tex, profile);
    if (original.array_count != 1)
        throw ConvertError("multi-image textures are not supported: the plugin asks which image to replace "
                           "in a dialog (CLAUDE.md §9)");
    if (png_size(png) != std::pair{original.width, original.height})
        throw ConvertError(png.string() + " must be " + std::to_string(original.width) + "x" +
                           std::to_string(original.height) + " to match " + original_tex.string());
    require_absent(tex_out);

    // With -b the plugin injects into "<X>.tex.<v>" when the output is named "<X>out.tex.<v>" (CLAUDE.md §9).
    // Fixed names in a private folder keep that name matching predictable.
    TempDir tmp;
    const std::string ext = ".tex." + profile.tex_suffix;
    const fs::path src = tmp.path / ("src" + ext), edit = tmp.path / "edit.png", out = tmp.path / ("srcout" + ext);
    fs::copy_file(original_tex, src);
    fs::copy_file(png, edit);

    std::vector<std::wstring> args{L"?cmode", edit.wstring(), out.wstring()};
    std::istringstream options(profile.noesis_export);  // e.g. "-b" for RE4R
    for (std::string opt; options >> opt;) args.emplace_back(opt.begin(), opt.end());
    const auto r = run_process(exe_, args, timeout_);
    if (r.exit_code != 0 || !fs::is_regular_file(out))
        throw ConvertError("Noesis failed to convert " + png.string() + " to .tex (exit " +
                           std::to_string(r.exit_code) + ")");
    const TexMeta result = read_tex_meta(out, profile);
    if (!same_texture(result, original))
        throw ConvertError("Noesis output does not match the original: got " + describe(result) + ", expected " +
                           describe(original));
    fs::copy_file(out, tex_out);
    return result;
}

}  // namespace remod
