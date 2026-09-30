#pragma once
#include "profile.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace remod {

namespace fs = std::filesystem;

struct PackageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// True for a non-empty relative path with no root and no ".." component.
bool is_safe_relative(const fs::path& p);

// Mod-relative path for a game asset: <mod_name>/<natives_root>/<game_path>.
// game_path is relative to the natives root (same as TexMeta::source_path).
fs::path package_path(const Profile& profile, const std::string& mod_name, const fs::path& game_path);

// Fluffy Mod Manager metadata [guide]. All fields optional; empty ones are omitted.
struct ModInfo {
    std::string name;
    std::string version;
    std::string description;
    std::string author;
    std::string screenshot;  // filename in the mod root
};

std::string write_modinfo(const ModInfo& info);

struct PackageFile {
    fs::path source;     // file on disk (e.g. an already-prepared .tex)
    fs::path game_path;  // relative to natives root
};

struct PackageSpec {
    std::string mod_name;
    fs::path out_dir;
    ModInfo info;  // info.screenshot is set from `screenshot`
    std::vector<PackageFile> files;
    fs::path screenshot;  // optional; jpg/png/tga/bmp
    bool zip = true;      // also write <out_dir>/<mod_name>.zip, mod folder at the archive root
};

// Builds <out_dir>/<mod_name>/ (and <mod_name>.zip if spec.zip) and returns the folder.
// Refuses to touch an existing folder or zip.
fs::path build_package(const Profile& profile, const PackageSpec& spec);

}  // namespace remod
