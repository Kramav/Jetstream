#pragma once
// LoadTex / SaveTex (CLAUDE.md §4). The Noesis implementation waits on the §9 spike.
#include "profile.hpp"
#include "types.hpp"

#include <filesystem>
#include <stdexcept>

namespace remod {

struct NotImplementedError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct LoadedTex {
    Image image;
    TexMeta meta;
};

class ITextureConverter {
public:
    virtual ~ITextureConverter() = default;
    // Game .tex -> image + metadata read from the file.
    virtual LoadedTex load_tex(const std::filesystem::path& tex, const Profile& profile) = 0;
    // Image + original metadata -> .tex matching the original's width/height/format/mips.
    virtual void save_tex(const Image& image, const TexMeta& meta, const std::filesystem::path& out) = 0;
};

class StubTextureConverter final : public ITextureConverter {
public:
    LoadedTex load_tex(const std::filesystem::path&, const Profile&) override {
        throw NotImplementedError(
            "LoadTex: not implemented - no texture converter yet (Noesis integration is blocked on the CLAUDE.md section 9 spike)");
    }
    void save_tex(const Image&, const TexMeta&, const std::filesystem::path&) override {
        throw NotImplementedError(
            "SaveTex: not implemented - no texture converter yet (Noesis integration is blocked on the CLAUDE.md section 9 spike)");
    }
};

}  // namespace remod
