#pragma once
// Shared test helpers. Everything here is synthetic: no game data.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace test {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path = fs::temp_directory_path() / ("remod_test_" + std::to_string(std::random_device{}()));
    TempDir() { fs::create_directories(path); }
    ~TempDir() { fs::remove_all(path); }
};

inline void write_file(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << bytes;
}

inline std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

inline void put_le(std::string& b, size_t at, std::uint32_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) b[at + i] = char((v >> (8 * i)) & 0xff);
}

// .tex header in the layout read_tex_meta reads.
inline void write_fake_tex(const fs::path& p, std::uint32_t version, std::uint16_t w, std::uint16_t h,
                           std::uint8_t images, std::uint8_t mips, std::uint32_t dxgi) {
    std::string b(64, '\0');
    put_le(b, 0, 0x00584554, 4);
    put_le(b, 4, version, 4);
    put_le(b, 8, w, 2);
    put_le(b, 10, h, 2);
    b[14] = char(images);
    b[15] = char(mips * 16);
    put_le(b, 16, dxgi, 4);
    write_file(p, b);
}

// PNG signature + IHDR width/height (enough for image_size).
inline void write_fake_png(const fs::path& p, std::uint32_t w, std::uint32_t h) {
    std::string b = "\x89PNG\r\n\x1a\n";
    b += std::string("\0\0\0\x0dIHDR", 8);
    for (std::uint32_t v : {w, h})
        for (int i = 3; i >= 0; --i) b += char((v >> (8 * i)) & 0xff);
    write_file(p, b);
}

inline fs::path unique(const fs::path& dir, const std::string& suffix) {
    return dir / (std::to_string(std::random_device{}()) + suffix);
}

}  // namespace test
