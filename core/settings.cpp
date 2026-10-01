#include "settings.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace remod {

namespace fs = std::filesystem;

fs::path default_settings_path() {
    char* appdata = nullptr;
    size_t n = 0;
    _dupenv_s(&appdata, &n, "APPDATA");
    const fs::path dir = appdata ? fs::path(appdata) : fs::path();
    std::free(appdata);
    return dir.empty() ? dir : dir / "remod" / "settings.json";
}

Settings load_settings(const fs::path& file) {
    Settings s;
    std::ifstream in(file);
    if (!in) return s;
    try {
        const auto j = nlohmann::json::parse(in);
        s.graph_path = j.value("graph_path", "");
        s.noesis_path = j.value("noesis_path", "");
        s.show_help = j.value("show_help", true);
        s.game_files_dir = j.value("game_files_dir", "");
        s.build_mode = j.value("build_mode", false);
        s.show_descriptions = j.value("show_descriptions", true);
    } catch (const nlohmann::json::exception&) {
        return {};
    }
    return s;
}

void save_settings(const Settings& s, const fs::path& file) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file);
    out << nlohmann::json{{"graph_path", s.graph_path}, {"noesis_path", s.noesis_path}, {"show_help", s.show_help},
                          {"game_files_dir", s.game_files_dir}, {"build_mode", s.build_mode},
                          {"show_descriptions", s.show_descriptions}}
               .dump(2)
        << "\n";
    if (!out.flush()) throw std::runtime_error("failed to write " + file.string());
}

}  // namespace remod
