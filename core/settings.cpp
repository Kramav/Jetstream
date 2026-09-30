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
    } catch (const nlohmann::json::exception&) {
        return {};
    }
    return s;
}

void save_settings(const Settings& s, const fs::path& file) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file);
    out << nlohmann::json{{"graph_path", s.graph_path}, {"noesis_path", s.noesis_path}}.dump(2) << "\n";
    if (!out.flush()) throw std::runtime_error("failed to write " + file.string());
}

}  // namespace remod
