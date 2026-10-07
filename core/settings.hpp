#pragma once
// Per-user settings remembered between app sessions.
#include <filesystem>
#include <string>
#include <vector>

namespace remod {

struct Settings {
    std::string graph_path;
    std::string noesis_path;     // optional: the 3D mesh view, and texture conversion if noesis_textures
    bool noesis_textures = false;  // convert textures with Noesis instead of the built-in converter
    bool show_help = true;
    std::string game_files_dir;  // extracted game files (REtool); empty = ask the RE plugin's NativesPath.txt
    std::string game_dir;        // the installed game's folder (the .exe's): Lua script's Test in game
    std::string sdk_dump;        // REFramework's il2cpp_dump.json: game names scripts are checked against (game_code.hpp)
    bool build_mode = false;     // app: Build layout (edit blocks and links) vs Use layout (fill in and run)
    bool show_descriptions = true;  // app: each block's description text (else on hovering its title)
    std::vector<std::string> pinned_folders;  // app: the Browser's pinned folders, in the order pinned
    bool operator==(const Settings&) const = default;
};

// %APPDATA%\remod\settings.json (empty if APPDATA isn't set).
std::filesystem::path default_settings_path();

// %LOCALAPPDATA%\remod\run_cache: Convert image to texture's textures and image blocks' temporary results, reused by
// later runs (empty if LOCALAPPDATA isn't set).
std::filesystem::path default_cache_dir();

// Missing or unreadable file -> default Settings; never throws (a bad settings file must not stop the app).
Settings load_settings(const std::filesystem::path& file);

// Creates the folder if needed. Throws std::runtime_error on failure.
void save_settings(const Settings& settings, const std::filesystem::path& file);

}  // namespace remod
