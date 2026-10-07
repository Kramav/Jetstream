#pragma once
#include "profile.hpp"

#include <cstdint>
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

// A game path for the game's own folder, not the natives root: REFramework's, "reframework/..." (its scripts,
// CLAUDE.md §10 M2). Fluffy copies such files to the game's folder [guide, unconfirmed: §9].
bool is_game_root_path(const fs::path& game_path);

// Mod-relative path for a game asset: <mod_name>/<natives_root>/<game_path>, or <mod_name>/<game_path> for a game-root
// path. game_path is relative to the natives root (same as TexMeta::source_path).
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
// A mod with a game-root file (a script) says "Needs REFramework." at the end of its description.
fs::path build_package(const Profile& profile, const PackageSpec& spec, bool* unchanged = nullptr);

// A REFramework script and the modules `require` finds for it [official, ScriptRunner.cpp]: <dir>\<stem>.lua at
// reframework/autorun/<stem>.lua, and every file under <dir>\<stem>\ at reframework/autorun/<stem>/... (REFramework
// runs only autorun's top-level .lua files; require looks in autorun\?.lua and autorun\?\init.lua). The script first,
// then its modules by path. Throws PackageError if `lua` isn't a .lua file.
std::vector<PackageFile> script_files(const fs::path& lua);

// Test in game: copies `files` (game-root paths) into the game's folder, over what's there. Throws PackageError if
// the folder has no REFramework (dinput8.dll [guide], §6) or a path isn't a game-root one.
void install_in_game(const std::vector<PackageFile>& files, const fs::path& game_dir);

// Undoes install_in_game: removes those files, then the folders that leaves empty (never reframework\autorun
// itself). Missing files are fine. ponytail: a module deleted from the script's folder since it was installed stays.
void remove_from_game(const std::vector<PackageFile>& files, const fs::path& game_dir);

// REFramework's log, <game>\re2_framework_log.txt, emptied each time the game starts [official, REFramework.cpp].
fs::path framework_log(const fs::path& game_dir);

// Whether REFramework writes Lua errors to its log: ScriptRunner's "Log Lua Errors to Disk", off by default
// [official, ScriptRunner.hpp], kept in <game>\re2_fw_config.txt as ScriptRunner_LogToDisk=true.
bool lua_errors_logged(const fs::path& game_dir);

// A script's errors in REFramework's log, from byte `from` on (where the log ended at Test in game; the whole log if
// it's shorter now: the game started again). The errors naming the script or its modules folder (<stem>.lua,
// <stem>/), each once, in order, with how often it came (an on_frame error repeats every frame) and its line in the
// script when it names <stem>.lua:<line>.
struct GameError {
    int line = 0;  // 0 if it names none
    std::string message;
    int count = 1;
};
std::vector<GameError> script_errors_in_log(const fs::path& log, std::uintmax_t from, const fs::path& script);

}  // namespace remod
