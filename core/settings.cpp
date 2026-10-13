#include "settings.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace remod {

namespace fs = std::filesystem;

namespace {

fs::path env_dir(const char* name) {
    char* value = nullptr;
    size_t n = 0;
    _dupenv_s(&value, &n, name);
    const fs::path dir = value ? fs::path(value) : fs::path();
    std::free(value);
    return dir;
}

}  // namespace

fs::path default_settings_path() {
    const fs::path dir = env_dir("APPDATA");
    return dir.empty() ? dir : dir / "remod" / "settings.json";
}

fs::path default_cache_dir() {
    const fs::path dir = env_dir("LOCALAPPDATA");
    return dir.empty() ? dir : dir / "remod" / "run_cache";
}

GameSettings open_game(const Settings& s) {
    return {s.graph_path, s.game_files_dir, s.game_dir, s.sdk_dump, s.pinned_folders};
}

void switch_game(Settings& s, const std::string& game) {
    if (game == s.game) return;
    s.other_games[s.game] = open_game(s);
    const GameSettings g = s.other_games[game];
    s.other_games.erase(game);
    s.game = game;
    s.graph_path = g.graph_path;
    s.game_files_dir = g.game_files_dir;
    s.game_dir = g.game_dir;
    s.sdk_dump = g.sdk_dump;
    s.pinned_folders = g.pinned_folders;
}

Settings for_game(Settings s, const std::string& game) {
    switch_game(s, game);
    return s;
}

namespace {

GameSettings game_from(const nlohmann::json& j) {
    return {j.value("graph_path", ""), j.value("game_files_dir", ""), j.value("game_dir", ""), j.value("sdk_dump", ""),
            j.value("pinned_folders", std::vector<std::string>{})};
}

nlohmann::json game_json(const GameSettings& g) {
    return {{"graph_path", g.graph_path}, {"game_files_dir", g.game_files_dir}, {"game_dir", g.game_dir},
            {"sdk_dump", g.sdk_dump}, {"pinned_folders", g.pinned_folders}};
}

}  // namespace

Settings load_settings(const fs::path& file) {
    Settings s;
    std::ifstream in(file);
    if (!in) return s;
    try {
        const auto j = nlohmann::json::parse(in);
        s.noesis_path = j.value("noesis_path", "");
        s.noesis_textures = j.value("noesis_textures", false);
        s.show_help = j.value("show_help", true);
        s.build_mode = j.value("build_mode", false);
        s.show_descriptions = j.value("show_descriptions", true);
        // Per game: {"game": id, "games": {id: {...}}}. Files from before games (2026-10-13) hold one set at the top:
        // RE4R's, the only game then.
        s.game = j.value("game", "re4r");
        std::map<std::string, GameSettings> games;
        if (j.contains("games") && j["games"].is_object())
            for (const auto& [id, g] : j["games"].items()) games[id] = game_from(g);
        else
            games["re4r"] = game_from(j);
        const GameSettings open = games[s.game];
        games.erase(s.game);
        s.other_games = games;
        s.graph_path = open.graph_path;
        s.game_files_dir = open.game_files_dir;
        s.game_dir = open.game_dir;
        s.sdk_dump = open.sdk_dump;
        s.pinned_folders = open.pinned_folders;
    } catch (const nlohmann::json::exception&) {
        return {};
    }
    return s;
}

void save_settings(const Settings& s, const fs::path& file) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file);
    nlohmann::json games = nlohmann::json::object();
    for (const auto& [id, g] : s.other_games) games[id] = game_json(g);
    games[s.game] = game_json(open_game(s));
    out << nlohmann::json{{"game", s.game}, {"games", games}, {"noesis_path", s.noesis_path},
                          {"noesis_textures", s.noesis_textures}, {"show_help", s.show_help},
                          {"build_mode", s.build_mode}, {"show_descriptions", s.show_descriptions}}
               .dump(2)
        << "\n";
    if (!out.flush()) throw std::runtime_error("failed to write " + file.string());
}

}  // namespace remod
