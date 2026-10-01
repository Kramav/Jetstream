#include "texture_converter.hpp"

#include "process.hpp"

#include <algorithm>
#include <array>
#include <cctype>
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
std::uint32_t be16(const std::string& b, size_t at) { return (std::uint8_t(b[at]) << 8) | std::uint8_t(b[at + 1]); }

std::string lower_extension(const fs::path& file) {
    std::string ext = file.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

const char* kEditFormatsText = ".png, .tga, .jpg or .jpeg";

// DXGI_FORMAT values (official D3D enum) for the formats the plugin's writer handles.
std::string format_name(std::uint32_t dxgi) {
    static const std::map<std::uint32_t, const char*> names{
        {10, "R16G16B16A16_FLOAT"}, {49, "R8G8_UNORM"}, {28, "R8G8B8A8_UNORM"}, {29, "R8G8B8A8_UNORM_SRGB"}, {61, "R8_UNORM"},
        {71, "BC1_UNORM"},          {72, "BC1_UNORM_SRGB"}, {77, "BC3_UNORM"},           {80, "BC4_UNORM"},
        {83, "BC5_UNORM"},          {95, "BC6H_UF16"},      {98, "BC7_UNORM"},           {99, "BC7_UNORM_SRGB"}};
    const auto it = names.find(dxgi);
    return it != names.end() ? it->second : "DXGI_FORMAT " + std::to_string(dxgi);
}

// Mip counts may differ: the plugin always writes mips down to 8x8 (CLAUDE.md §9). Callers warn about that.
bool same_texture(const TexMeta& a, const TexMeta& b) {
    return a.width == b.width && a.height == b.height && a.format == b.format && a.array_count == b.array_count;
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

std::optional<std::uint32_t> read_tex_version(const fs::path& tex) {
    std::error_code ec;
    if (!fs::is_regular_file(tex, ec)) return std::nullopt;
    const std::string b = read_prefix(tex, 8);
    if (b.size() < 8 || le32(b, 0) != 0x00584554) return std::nullopt;  // "TEX\0"
    return le32(b, 4);
}

const Profile* profile_for_texture(const fs::path& tex, const std::vector<Profile>& profiles) {
    const auto version = read_tex_version(tex);
    if (!version) return nullptr;
    for (const auto& p : profiles)
        if (p.tex_suffix == std::to_string(*version)) return &p;
    return nullptr;
}

TexMeta read_tex_meta(const fs::path& tex, const Profile& profile) {
    // [plugin source] magic@0 "TEX\0", version@4, width@8, height@10, images@14, mip header bytes@15 (16 per mip),
    // DXGI format@16. ponytail: this is the layout for tex versions > 27 (RE4R); other games need the plugin's
    // other branch, so the version is pinned to the profile's suffix.
    const std::string b = read_prefix(tex, 20);
    if (b.size() < 20 || le32(b, 0) != 0x00584554)
        throw ConvertError(tex.string() + " is not an RE Engine .tex file");
    if (std::to_string(le32(b, 4)) != profile.tex_suffix)
        throw ConvertError(tex.string() + " is an RE Engine texture of version " + std::to_string(le32(b, 4)) +
                           ", but the graph's game is " + profile.name + " (version " + profile.tex_suffix +
                           "). Pick the matching game, or a texture from " + profile.name + ".");
    TexMeta m;
    m.game_profile = profile.id;
    m.width = le16(b, 8);
    m.height = le16(b, 10);
    m.array_count = std::uint8_t(b[14]);
    m.mip_count = std::uint8_t(b[15]) / 16u;
    m.format = format_name(le32(b, 16));
    return m;
}

TexPixels read_tex_pixels(const fs::path& tex, std::uint32_t max_side) {
    // [plugin source] for versions > 27: a 40-byte header, then per image and mip {u64 offset, u32 pitch, u32 size}.
    // RE3R's 190820018 is the plugin's exception (it reads it as version 10, the older layout).
    std::string b = read_prefix(tex, 40);
    if (b.size() < 40 || le32(b, 0) != 0x00584554) throw ConvertError(tex.string() + " is not an RE Engine .tex file");
    const std::uint32_t version = le32(b, 4);
    if (version <= 27 || version == 190820018)
        throw ConvertError("tex version " + std::to_string(version) + " uses an older layout (not supported yet)");
    const std::uint32_t mips = std::uint8_t(b[15]) / 16u;
    if (b[14] == 0 || mips == 0) throw ConvertError(tex.string() + " holds no images");

    // Bytes per pixel, or per 4x4 block (block = true), by DXGI_FORMAT [official D3D docs].
    struct Layout { std::uint32_t bytes; bool block; };
    static const std::map<std::uint32_t, Layout> layouts{
        {2, {16, false}}, {10, {8, false}}, {28, {4, false}}, {29, {4, false}}, {49, {2, false}}, {61, {1, false}},
        {71, {8, true}},  {72, {8, true}},  {77, {16, true}}, {78, {16, true}}, {80, {8, true}},  {83, {16, true}},
        {87, {4, false}}, {91, {4, false}}, {95, {16, true}}, {96, {16, true}}, {98, {16, true}}, {99, {16, true}}};
    const auto layout = layouts.find(le32(b, 16));
    if (layout == layouts.end()) throw ConvertError("can't preview " + format_name(le32(b, 16)));
    const auto [bytes, block] = layout->second;

    const std::uint32_t width = le16(b, 8), height = le16(b, 10);
    std::uint32_t mip = 0;
    while (mip + 1 < mips && std::max(width >> mip, height >> mip) > max_side) ++mip;
    b = read_prefix(tex, 40 + 16 * (mip + 1));
    if (b.size() < 40 + 16 * (mip + 1)) throw ConvertError(tex.string() + " is cut short");
    const size_t entry = 40 + 16 * mip;
    const std::uint64_t offset = std::uint64_t(le32(b, entry)) | (std::uint64_t(le32(b, entry + 4)) << 32);
    TexPixels p;
    p.format = layout->first;
    p.width = std::max(1u, width >> mip);
    p.height = std::max(1u, height >> mip);
    p.row_pitch = le32(b, entry + 8);
    const std::uint32_t size = le32(b, entry + 12);
    const std::uint32_t cell = block ? 4 : 1;
    if (p.row_pitch == 0 || p.row_pitch % bytes || size % p.row_pitch)
        throw ConvertError(tex.string() + ": unexpected mip layout");
    p.stored_width = p.row_pitch / bytes * cell;
    p.stored_height = size / p.row_pitch * cell;
    if (p.stored_width < p.width || p.stored_height < p.height)
        throw ConvertError(tex.string() + ": mip smaller than its size");

    std::ifstream in(tex, std::ios::binary);
    in.seekg(std::streamoff(offset));
    p.data.resize(size);
    in.read(reinterpret_cast<char*>(p.data.data()), std::streamsize(size));
    if (!in) throw ConvertError(tex.string() + " is cut short");
    return p;
}

bool is_edit_image(const fs::path& file) {
    const std::string ext = lower_extension(file);
    std::istringstream list(kEditImageFormats);
    for (std::string item; std::getline(list, item, ',');)
        if (ext == "." + item) return true;
    return false;
}

std::pair<std::uint32_t, std::uint32_t> image_size(const fs::path& file) {
    std::string b = read_prefix(file, 24);
    if (b.size() >= 24 && b.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0 && b.compare(12, 4, "IHDR") == 0)
        return {be32(b, 16), be32(b, 20)};  // PNG spec: IHDR is the first chunk
    if (b.size() >= 2 && std::uint8_t(b[0]) == 0xFF && std::uint8_t(b[1]) == 0xD8) {
        // JPEG: walk the marker segments to the first start-of-frame (SOF0..SOF15 minus DHT/JPG/DAC), whose
        // payload is precision(1) height(2) width(2).
        b = read_prefix(file, size_t(fs::file_size(file)));
        for (size_t i = 2; i + 9 <= b.size() && std::uint8_t(b[i]) == 0xFF;) {
            const unsigned marker = std::uint8_t(b[i + 1]);
            if (marker == 0xFF) {  // fill byte
                ++i;
                continue;
            }
            if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC)
                return {be16(b, i + 7), be16(b, i + 5)};
            i += 2 + be16(b, i + 2);
        }
        throw ConvertError(file.string() + " is a JPG file without a readable size");
    }
    // TGA has no signature: trust the extension, then check the header. Types 2/3 = true-colour/grey, 10/11 = RLE.
    const int type = b.size() >= 18 ? b[2] : 0;
    if (lower_extension(file) == ".tga" && (type == 2 || type == 3 || type == 10 || type == 11))
        return {le16(b, 12), le16(b, 14)};
    throw ConvertError(file.string() + " is not a PNG, TGA or JPG file");
}

NoesisConverter::NoesisConverter(fs::path noesis_exe, std::chrono::milliseconds timeout)
    : exe_(std::move(noesis_exe)), timeout_(timeout) {
    if (!fs::is_regular_file(exe_)) throw ConvertError("Noesis not found: " + exe_.string());
}

// Noesis exits 0 even when a conversion fails, and its console text never reaches stdout/stderr (CLAUDE.md §9).
// So success is judged from the output file, and its messages are collected with -logfile to explain failures.
namespace {

std::string noesis_said(const fs::path& log) {
    std::ifstream in(log);
    std::string text, line;
    while (std::getline(in, line) && text.size() < 2000)
        if (!line.empty()) text += "\n  " + line;
    return text.empty() ? " (Noesis gave no reason)" : " Noesis said:" + text;
}

}  // namespace

ProcessResult NoesisConverter::run(std::vector<std::wstring> args, const fs::path& log) const {
    args.push_back(L"-logfile");
    args.push_back(log.wstring());
    try {
        // private desktop: plugin errors open MessageBoxes (CLAUDE.md §9).
        // PYTHONIOENCODING: without it Noesis's embedded Python fails to start when its output is piped (as here)
        // and the parent environment doesn't already set it (e.g. the app launched from Explorer). The plugins
        // then never load and every RE texture is "Detected file type: Unknown" (CLAUDE.md §9).
        return run_process(exe_, args, timeout_, /*private_desktop=*/true, {{L"PYTHONIOENCODING", L"utf-8"}});
    } catch (const ProcessError& e) {  // a dialog or a timeout: add what Noesis logged before it stopped
        throw ConvertError(std::string(e.what()) + noesis_said(log));
    }
}

TexMeta NoesisConverter::load_tex(const fs::path& tex, const fs::path& png_out, const Profile& profile) {
    const TexMeta meta = read_tex_meta(tex, profile);
    // Noesis picks the output format from the extension; without a known one it silently writes "<name>.png".
    if (!is_edit_image(png_out))
        throw ConvertError(std::string("image output path must end in ") + kEditFormatsText + ": " + png_out.string());
    require_absent(png_out);
    TempDir tmp;
    const fs::path log = tmp.path / "noesis.log";
    // Noesis picks its reader by extension (".143221013" -> the plugin's RE texture reader), so a copy named
    // "src.tex.<version>" makes any file name work: plain ".tex", renamed files, etc.
    const fs::path src = tmp.path / ("src.tex." + profile.tex_suffix);
    fs::copy_file(tex, src);
    const auto r = run({L"?cmode", src.wstring(), png_out.wstring()}, log);
    if (r.exit_code != 0 || !fs::is_regular_file(png_out))
        throw ConvertError("Noesis couldn't convert " + tex.string() + " to " + png_out.string() + " (exit " +
                           std::to_string(r.exit_code) + ")." + noesis_said(log));
    if (image_size(png_out) != std::pair{meta.width, meta.height}) {
        fs::remove(png_out);
        throw ConvertError("Noesis wrote an image whose size differs from the .tex (" + describe(meta) + ")");
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
    if (!is_edit_image(png))
        throw ConvertError(png.string() + " must be an image ending in " + kEditFormatsText);
    if (image_size(png) != std::pair{original.width, original.height})
        throw ConvertError(png.string() + " must be " + std::to_string(original.width) + "x" +
                           std::to_string(original.height) + " to match " + original_tex.string());
    require_absent(tex_out);

    // With -b the plugin injects into "<X>.tex.<v>" when the output is named "<X>out.tex.<v>" (CLAUDE.md §9).
    // Fixed names in a private folder keep that name matching predictable. The edit keeps its extension: Noesis
    // reads it in the format that names.
    TempDir tmp;
    const std::string ext = ".tex." + profile.tex_suffix;
    const fs::path src = tmp.path / ("src" + ext), edit = tmp.path / ("edit" + lower_extension(png)),
                   out = tmp.path / ("srcout" + ext);
    fs::copy_file(original_tex, src);
    fs::copy_file(png, edit);

    std::vector<std::wstring> args{L"?cmode", edit.wstring(), out.wstring()};
    std::istringstream options(profile.noesis_export);  // e.g. "-b" for RE4R
    for (std::string opt; options >> opt;) args.emplace_back(opt.begin(), opt.end());
    const fs::path log = tmp.path / "noesis.log";
    const auto r = run(args, log);
    if (r.exit_code != 0 || !fs::is_regular_file(out))
        throw ConvertError("Noesis couldn't convert " + png.string() + " to a texture (exit " +
                           std::to_string(r.exit_code) + ")." + noesis_said(log));
    const TexMeta result = read_tex_meta(out, profile);
    if (!same_texture(result, original))
        throw ConvertError("Noesis output does not match the original: got " + describe(result) + ", expected " +
                           describe(original));
    fs::copy_file(out, tex_out);
    return result;
}

}  // namespace remod
