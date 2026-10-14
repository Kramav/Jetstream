#pragma once
// Per-user settings remembered between app sessions.
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace remod {

// What remod keeps per game (CLAUDE.md §10 "Games", user 2026-10-13): each game's own folders, dump, pins and last graph.
struct GameSettings {
    std::string graph_path;
    std::string game_files_dir;
    std::string game_dir;
    std::string sdk_dump;
    std::vector<std::string> pinned_folders;
    bool operator==(const GameSettings&) const = default;
};

// The fields marked "the open game's" hold the game workspace open now (`game`); the other games' are kept in
// `other_games` and swapped in by switch_game. So code using them needs no game id.
struct Settings {
    std::string game = "re4r";   // the open game workspace: a profile id
    std::string graph_path;      // the open game's last graph
    std::string noesis_path;     // optional: the 3D mesh view, and texture conversion if noesis_textures
    bool noesis_textures = false;  // convert textures with Noesis instead of the built-in converter
    std::string ai_tool;   // Generate image (CLAUDE.md §10 M4): sd-cli.exe or its folder (stable-diffusion.cpp)
    std::string ai_model;  // its model (.safetensors / .gguf), the user's own download
    std::string ai_vae;    // optional separate VAE (SDXL: sdxl-vae-fp16-fix)
    bool show_help = true;
    std::string game_files_dir;  // the open game's extracted files (REtool); empty = ask the RE plugin's NativesPath.txt
    std::string game_dir;        // the open game's folder (the .exe's): Lua script's Test in game
    std::string sdk_dump;        // the open game's il2cpp_dump.json (REFramework): names scripts are checked against
    bool build_mode = false;     // app: Build layout (edit blocks and links) vs Use layout (fill in and run)
    bool show_descriptions = true;  // app: each block's description text (else on hovering its title)
    std::vector<std::string> pinned_folders;  // the open game's pinned Browser folders, in the order pinned
    std::map<std::string, GameSettings> other_games;  // the other games', by profile id
    bool operator==(const Settings&) const = default;
};

// The open game's fields as a GameSettings, and back.
GameSettings open_game(const Settings& s);
// Opens another game's workspace: the open game's fields go into other_games, that game's come out (empty if new).
void switch_game(Settings& s, const std::string& game);
// A copy of the settings with `game` open (the CLI and remod mcp: a graph's game, whatever the app has open).
Settings for_game(Settings s, const std::string& game);

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
