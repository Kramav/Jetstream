#pragma once
// Data contracts, CLAUDE.md §5 (v0).
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace remod {

struct TexMeta {
    std::string source_path;   // original .tex path, relative to natives root
    std::string game_profile;  // e.g. "re4r"
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string format;        // read from the original, never assumed
    std::uint32_t mip_count = 0;
    std::uint32_t array_count = 0;
};

// ponytail: RGBA8 only; enough for 2D UI textures, add a float format when HDR/BC6H textures come up.
struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;  // width * height * 4
};

struct ManifestV0 {
    int schema_version = 0;
    std::optional<std::string> runtime_version;  // null for tier 1
    std::string game;
    int tier = 1;
    bool requires_reframework = false;
    std::vector<std::string> assets;  // game paths, relative to natives root
    // triggers: not modeled until tier 2 (always [] for now).
};

}  // namespace remod
