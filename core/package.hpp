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
    bool replace = false;  // overwrite a previous build of the same mod (see build_package)
};

// Builds <out_dir>/<mod_name>/ (and <mod_name>.zip if spec.zip) and returns the folder.
// An existing folder or zip is an error, unless spec.replace is set and it was built for this mod: a folder
// whose modinfo.ini says name=<info.name>, a zip holding only <mod_name>/... including its modinfo.ini.
// Anything else is never deleted.
// `unchanged` (optional): set when the same build is already there (same files, same bytes, same modinfo.ini, its
// zip present), which is then left as it is: nothing written (user, 2026-10-02: no needless write cycles).
fs::path build_package(const Profile& profile, const PackageSpec& spec, bool* unchanged = nullptr);

}  // namespace remod
