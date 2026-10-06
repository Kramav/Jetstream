#include "texture_converter.hpp"

#include "process.hpp"

#define NOMINMAX
#include <d3d11.h>  // before DirectXTex.h: declares its GPU (DirectCompute) encoder
#include <wrl/client.h>
#include <DirectXTex.h>  // decodes, converts and encodes the pixel data (never reads or writes a file itself)

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <map>
#include <random>
#include <set>
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

// Bytes per pixel, or per 4x4 block (block = true), by DXGI_FORMAT [official D3D docs].
struct Layout {
    std::uint32_t bytes;
    bool block;
};
const Layout* layout_of(std::uint32_t format) {
    static const std::map<std::uint32_t, Layout> layouts{
        {2, {16, false}}, {10, {8, false}}, {28, {4, false}}, {29, {4, false}}, {49, {2, false}}, {61, {1, false}},
        {71, {8, true}},  {72, {8, true}},  {77, {16, true}}, {78, {16, true}}, {80, {8, true}},  {83, {16, true}},
        {87, {4, false}}, {91, {4, false}}, {95, {16, true}}, {96, {16, true}}, {98, {16, true}}, {99, {16, true}}};
    const auto it = layouts.find(format);
    return it == layouts.end() ? nullptr : &it->second;
}

// The .tex header, by version [REE-Lib TexFile.cs, MIT: Header.ReadWrite and its Versions table]. Both layouts start
// magic@0, version@4, width@8, height@10, depth@12 and have the format@16, and are followed by one 16-byte entry per
// image and mip, image 0's mips first: {u64 offset, u32 row pitch, u32 size}.
//   legacy (RE7, RE2, DMC5, RE3): 32 bytes, mip count@14, image count@15.
//   modern (MH Rise and later):   40 bytes, image count@14, mip table bytes@15 (16 per mip).
// GDeflate games (MH Wilds, RE9, Pragmata) compress the mip data: not read here.
struct TexHeader {
    std::uint32_t version = 0, width = 0, height = 0, depth = 0, images = 0, mips = 0, format = 0;
    std::uint32_t size = 0;  // header bytes; the mip table follows
};

constexpr size_t kTexHeaderMax = 40;

TexHeader parse_tex_header(const std::string& b, const fs::path& tex) {
    if (b.size() < 32 || le32(b, 0) != 0x00584554) throw ConvertError(tex.string() + " is not an RE Engine .tex file");
    TexHeader h;
    h.version = le32(b, 4);
    switch (h.version) {
        case 8: case 10: case 11: case 190820018:  // RE7, RE2, DMC5, RE3
            h.size = 32;
            h.mips = std::uint8_t(b[14]);
            h.images = std::uint8_t(b[15]);
            break;
        case 28: case 30: case 34: case 35: case 143221013:                  // MH Rise, RE8, RE2/3 RT, RE7 RT, RE4R/SF6
        case 760230703: case 251211553: case 240606151: case 240701001:      // DD2 (old), DD2, DR, Onimusha 2
            if (b.size() < 40) throw ConvertError(tex.string() + " is cut short");
            h.size = 40;
            h.images = std::uint8_t(b[14]);
            h.mips = std::uint8_t(b[15]) / 16u;
            break;
        case 241106027: case 250813143: case 251111100:  // MH Wilds, RE9, Pragmata / MH Stories 3
            throw ConvertError(tex.string() + ": tex version " + std::to_string(h.version) +
                               " stores its pixels compressed (GDeflate), which the built-in converter can't read. "
                               "Use the Noesis converter (Pipeline panel) for it.");
        default:
            throw ConvertError(tex.string() + ": unknown tex version " + std::to_string(h.version));
    }
    h.width = le16(b, 8);
    h.height = le16(b, 10);
    h.depth = le16(b, 12);
    h.format = le32(b, 16);
    return h;
}

TexHeader read_tex_header(const fs::path& tex) { return parse_tex_header(read_prefix(tex, kTexHeaderMax), tex); }

}  // namespace

bool is_tex_name(const std::string& name) {
    if (name.ends_with(".tex")) return true;
    const size_t at = name.rfind(".tex.");
    if (at == std::string::npos || at + 5 == name.size() || name.find('.', at + 5) != std::string::npos) return false;
    // ponytail: an image exported from a texture ("x.tex.png") isn't one; a list, since suffixes are free text.
    static const std::set<std::string, std::less<>> images{"bmp", "dds", "jpeg", "jpg", "png", "tga", "tif", "tiff"};
    return !images.contains(std::string_view(name).substr(at + 5));
}

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
    const std::string b = read_prefix(tex, kTexHeaderMax);  // the game first: a clearer message than the layout's
    if (b.size() >= 8 && le32(b, 0) == 0x00584554 && std::to_string(le32(b, 4)) != profile.tex_suffix)
        throw ConvertError(tex.string() + " is an RE Engine texture of version " + std::to_string(le32(b, 4)) +
                           ", but the graph's game is " + profile.name + " (version " + profile.tex_suffix +
                           "). Pick the matching game, or a texture from " + profile.name + ".");
    const TexHeader h = parse_tex_header(b, tex);
    TexMeta m;
    m.game_profile = profile.id;
    m.width = h.width;
    m.height = h.height;
    m.array_count = h.images;
    m.mip_count = h.mips;
    m.format = format_name(h.format);
    return m;
}

namespace {

// The header of a texture a preview can show, and the mip read_tex_pixels picks: the largest no bigger than max_side.
std::pair<TexHeader, std::uint32_t> preview_mip(const fs::path& tex, std::uint32_t max_side) {
    const TexHeader h = read_tex_header(tex);
    if (h.images == 0 || h.mips == 0) throw ConvertError(tex.string() + " holds no images");
    if (!layout_of(h.format)) throw ConvertError("can't preview " + format_name(h.format));
    std::uint32_t mip = 0;
    while (mip + 1 < h.mips && std::max(h.width >> mip, h.height >> mip) > max_side) ++mip;
    return {h, mip};
}

TexPixels read_mip(const fs::path& tex, const TexHeader& h, std::uint32_t mip) {
    const auto [bytes, block] = *layout_of(h.format);
    const std::string b = read_prefix(tex, h.size + 16 * (mip + 1));
    if (b.size() < h.size + 16 * (mip + 1)) throw ConvertError(tex.string() + " is cut short");
    const size_t entry = h.size + 16 * mip;
    const std::uint64_t offset = std::uint64_t(le32(b, entry)) | (std::uint64_t(le32(b, entry + 4)) << 32);
    TexPixels p;
    p.format = h.format;
    p.width = std::max(1u, std::uint32_t(h.width) >> mip);
    p.height = std::max(1u, std::uint32_t(h.height) >> mip);
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

}  // namespace

TexPixels read_tex_pixels(const fs::path& tex, std::uint32_t max_side) {
    const auto [h, mip] = preview_mip(tex, max_side);
    return read_mip(tex, h, mip);
}

std::vector<TexPixels> read_tex_mips(const fs::path& tex, std::uint32_t max_side) {
    const auto [h, first] = preview_mip(tex, max_side);
    std::vector<TexPixels> chain{read_mip(tex, h, first)};
    // Each at least the first's stored size halved (what a GPU's mip chain expects), else the chain stops there.
    for (std::uint32_t mip = first + 1, k = 1; mip < h.mips; ++mip, ++k) {
        TexPixels p;
        try {
            p = read_mip(tex, h, mip);
        } catch (const ConvertError&) {  // a damaged smaller mip: the preview does without it
            break;
        }
        if (p.stored_width < std::max(1u, chain[0].stored_width >> k) ||
            p.stored_height < std::max(1u, chain[0].stored_height >> k))
            break;
        chain.push_back(std::move(p));
    }
    return chain;
}

Bgra decode_tex(const fs::path& tex, std::uint32_t max_side, unsigned* full_width, unsigned* full_height) {
    TexPixels p = read_tex_pixels(tex, max_side);
    if (full_width || full_height) {
        const TexHeader h = read_tex_header(tex);
        if (full_width) *full_width = h.width;
        if (full_height) *full_height = h.height;
    }
    // sRGB formats read as their linear twins: the bytes kept as they are, as the browser shows them.
    const auto format = DirectX::MakeLinear(DXGI_FORMAT(p.format));
    const DirectX::Image stored{p.stored_width, p.stored_height, format, p.row_pitch, p.data.size(), p.data.data()};
    DirectX::ScratchImage scratch;
    const DirectX::Image* bgra = &stored;
    if (format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        const HRESULT hr = DirectX::IsCompressed(format)
                               ? DirectX::Decompress(stored, DXGI_FORMAT_B8G8R8A8_UNORM, scratch)
                               // Its own converter, not WIC's (which needs COM on the calling thread).
                               : DirectX::Convert(stored, DXGI_FORMAT_B8G8R8A8_UNORM, DirectX::TEX_FILTER_FORCE_NON_WIC,
                                                  DirectX::TEX_THRESHOLD_DEFAULT, scratch);
        if (FAILED(hr) || !scratch.GetImage(0, 0, 0))
            throw ConvertError("can't decode " + tex.filename().string() + " (" + format_name(p.format) + ")");
        bgra = scratch.GetImage(0, 0, 0);
    }
    Bgra out{p.width, p.height, std::vector<std::uint8_t>(size_t(p.width) * p.height * 4)};
    for (std::uint32_t y = 0; y < p.height; ++y)  // the visible part: padding rows and columns cut off
        std::copy_n(bgra->pixels + y * bgra->rowPitch, size_t(p.width) * 4, out.pixels.data() + size_t(y) * p.width * 4);
    return out;
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

namespace {

// The checks both converters make before writing a .tex: one image, an edit image of the original's size, no file
// at tex_out yet.
TexMeta check_save(const fs::path& image, const fs::path& original_tex, const fs::path& tex_out, const Profile& profile) {
    const TexMeta original = read_tex_meta(original_tex, profile);
    if (original.array_count != 1)
        throw ConvertError("multi-image textures (arrays, cubemaps) are not supported: " + original_tex.string());
    if (!is_edit_image(image))
        throw ConvertError(image.string() + " must be an image ending in " + kEditFormatsText);
    if (image_size(image) != std::pair{original.width, original.height})
        throw ConvertError(image.string() + " must be " + std::to_string(original.width) + "x" +
                           std::to_string(original.height) + " to match " + original_tex.string());
    require_absent(tex_out);
    return original;
}

}  // namespace

TexMeta NativeConverter::load_tex(const fs::path& tex, const fs::path& png_out, const Profile& profile) {
    const TexMeta meta = read_tex_meta(tex, profile);
    if (!is_edit_image(png_out))
        throw ConvertError(std::string("image output path must end in ") + kEditFormatsText + ": " + png_out.string());
    require_absent(png_out);
    const Bgra image = decode_tex(tex, UINT32_MAX);  // mip 0 at its visible size: row padding cut off
    try {
        save_image_bgra(png_out, image.width, image.height, image.pixels);
    } catch (const std::runtime_error& e) {
        std::error_code ec;
        fs::remove(png_out, ec);
        throw ConvertError(std::string("couldn't write ") + png_out.string() + ": " + e.what());
    }
    return meta;
}

TexMeta NativeConverter::save_tex(const fs::path& png, const fs::path& original_tex, const fs::path& tex_out,
                                  const Profile& profile) {
    const TexMeta original = check_save(png, original_tex, tex_out, profile);
    std::string file = read_prefix(original_tex, size_t(fs::file_size(original_tex)));
    const TexHeader h = parse_tex_header(file, original_tex);
    if (h.depth > 1) throw ConvertError("volume textures are not supported: " + original_tex.string());
    if (!layout_of(h.format)) throw ConvertError("can't write " + format_name(h.format) + " textures");
    if (file.size() < h.size + 16 * size_t(h.mips)) throw ConvertError(original_tex.string() + " is cut short");

    Bgra edit;
    try {
        edit = load_image(png);
    } catch (const std::runtime_error& e) {
        throw ConvertError(std::string("couldn't read ") + png.string() + ": " + e.what());
    }
    // sRGB targets: the edit's bytes are sRGB (as decode_tex wrote them), so it's tagged sRGB and its mips are
    // averaged in linear light.
    using namespace DirectX;
    const auto target = DXGI_FORMAT(h.format);
    const bool srgb = IsSRGB(target);
    const Image source{edit.width, edit.height, srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM,
                       size_t(edit.width) * 4, edit.pixels.size(), edit.pixels.data()};
    const TEX_FILTER_FLAGS filter = TEX_FILTER_FORCE_NON_WIC | (srgb ? TEX_FILTER_SRGB : TEX_FILTER_DEFAULT);
    ScratchImage mips, encoded;
    HRESULT hr = h.mips > 1 ? GenerateMipMaps(source, filter, h.mips, mips) : mips.InitializeFromImage(source);
    // BC6H / BC7 on the CPU take minutes in a Debug build (188 s for one 512x512), so they go to the GPU
    // (DirectXTex's DirectCompute encoder) when there is one; the CPU otherwise.
    const bool gpu_format = target == DXGI_FORMAT_BC6H_UF16 || target == DXGI_FORMAT_BC6H_SF16 ||
                            target == DXGI_FORMAT_BC7_UNORM || target == DXGI_FORMAT_BC7_UNORM_SRGB;
    Microsoft::WRL::ComPtr<ID3D11Device> gpu;
    if (SUCCEEDED(hr) && gpu_format) {
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;  // compute shaders as the encoder needs them
        if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                                        &gpu, nullptr, nullptr)) &&
            FAILED(Compress(gpu.Get(), mips.GetImages(), mips.GetImageCount(), mips.GetMetadata(), target,
                            TEX_COMPRESS_DEFAULT, TEX_ALPHA_WEIGHT_DEFAULT, encoded)))
            gpu.Reset();  // fall back to the CPU
    }
    if (SUCCEEDED(hr) && !gpu) {
        if (IsCompressed(target))
            hr = Compress(mips.GetImages(), mips.GetImageCount(), mips.GetMetadata(), target, TEX_COMPRESS_PARALLEL,
                          TEX_THRESHOLD_DEFAULT, encoded);
        else if (target != source.format)
            hr = Convert(mips.GetImages(), mips.GetImageCount(), mips.GetMetadata(), target, filter,
                         TEX_THRESHOLD_DEFAULT, encoded);
        else
            encoded = std::move(mips);
    }
    if (FAILED(hr) || encoded.GetImageCount() != h.mips)
        throw ConvertError("couldn't encode " + png.string() + " as " + format_name(h.format));

    // Each mip goes over the original's bytes: its rows at the original's pitch, the padding zeroed.
    for (std::uint32_t mip = 0; mip < h.mips; ++mip) {
        const size_t entry = h.size + 16 * size_t(mip);
        const std::uint64_t offset = std::uint64_t(le32(file, entry)) | (std::uint64_t(le32(file, entry + 4)) << 32);
        const std::uint32_t pitch = le32(file, entry + 8), size = le32(file, entry + 12);
        const Image* img = encoded.GetImage(mip, 0, 0);
        const size_t rows = img->slicePitch / img->rowPitch;
        if (pitch == 0 || img->rowPitch > pitch || rows > size / pitch || offset + size > file.size())
            throw ConvertError(original_tex.string() + ": mip " + std::to_string(mip) +
                               " doesn't fit its place in the file (unexpected layout)");
        std::fill_n(file.begin() + std::ptrdiff_t(offset), size, '\0');
        for (size_t y = 0; y < rows; ++y)
            std::copy_n(img->pixels + y * img->rowPitch, img->rowPitch, file.begin() + std::ptrdiff_t(offset + y * pitch));
    }
    {
        std::ofstream out(tex_out, std::ios::binary);
        out.write(file.data(), std::streamsize(file.size()));
        if (!out) throw ConvertError("cannot write " + tex_out.string());
    }
    const TexMeta result = read_tex_meta(tex_out, profile);
    if (!same_texture(result, original) || result.mip_count != original.mip_count)
        throw ConvertError("the written texture doesn't match the original: got " + describe(result) + ", expected " +
                           describe(original));
    return result;
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

MeshModel NoesisConverter::load_mesh(const fs::path& mesh) {
    TempDir tmp;
    const fs::path log = tmp.path / "noesis.log", obj = tmp.path / "mesh.obj";
    // A copy keeps the extension that selects the plugin's mesh reader, and away from its material files: the viewer
    // applies textures itself, so Noesis needn't load them. -b clicks the plugin's import window's Load button,
    // which otherwise waits forever; -noprompt skips its material prompt (spike 2026-10-01: 0.5 s for a small prop,
    // 5.9 s for a 163k-triangle character).
    const fs::path src = tmp.path / ("mesh" + mesh.extension().string());
    fs::copy_file(mesh, src);
    const auto r = run({L"?cmode", src.wstring(), obj.wstring(), L"-b", L"-noprompt"}, log);
    if (r.exit_code != 0 || !fs::is_regular_file(obj))
        throw ConvertError("Noesis couldn't read " + mesh.string() + " (exit " + std::to_string(r.exit_code) + ")." +
                           noesis_said(log));
    std::ifstream in(obj, std::ios::binary);
    return parse_obj(std::string((std::istreambuf_iterator<char>(in)), {}));
}

TexMeta NoesisConverter::save_tex(const fs::path& png, const fs::path& original_tex, const fs::path& tex_out,
                                  const Profile& profile) {
    if (profile.noesis_export.empty() || profile.noesis_export == "TBD")
        throw ConvertError("profile " + profile.id + " has no noesis_export: the Noesis export options for this game "
                           "are not known. Use the built-in converter.");
    const TexMeta original = check_save(png, original_tex, tex_out, profile);

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

std::unique_ptr<ITextureConverter> make_converter(const fs::path& noesis_exe) {
    if (noesis_exe.empty()) return std::make_unique<NativeConverter>();
    return std::make_unique<NoesisConverter>(noesis_exe);
}

}  // namespace remod
