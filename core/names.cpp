#include "names.hpp"

#include "settings.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace remod {

namespace fs = std::filesystem;

namespace {

std::string key_of(std::string path) {
    std::ranges::replace(path, '\\', '/');
    std::ranges::transform(path, path.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    while (!path.empty() && path.back() == '/') path.pop_back();
    return path;
}

}  // namespace

fs::path default_names_path(const std::string& game) {
    const fs::path settings = default_settings_path();
    return settings.empty() ? settings : settings.parent_path() / "names" / (game + ".json");
}

Nicknames load_names(const fs::path& file) {
    Nicknames out;
    std::error_code ec;
    if (!fs::exists(file, ec)) return out;
    std::ifstream in(file);
    if (!in) throw std::runtime_error("can't open " + file.string());
    try {
        const auto j = nlohmann::json::parse(in);
        if (!j.is_object()) throw std::runtime_error("not a {\"path\": \"nickname\"} object");
        for (const auto& [path, name] : j.items()) {
            if (!name.is_string()) throw std::runtime_error("the nickname of " + path + " isn't text");
            set_nickname(out, path, name.get<std::string>());
        }
    } catch (const std::exception& e) {
        throw std::runtime_error("can't read the nicknames in " + file.string() + ": " + e.what());
    }
    return out;
}

void save_names(const Nicknames& names, const fs::path& file) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file);
    out << nlohmann::json(names.names).dump(2) << "\n";
    if (!out.flush()) throw std::runtime_error("failed to write " + file.string());
}

std::string nickname(const Nicknames& names, const std::string& path) {
    const auto it = names.names.find(key_of(path));
    return it == names.names.end() ? std::string() : it->second;
}

std::string nickname_above(const Nicknames& names, const std::string& path) {
    for (std::string key = key_of(path); !key.empty();) {
        const size_t slash = key.rfind('/');
        if (slash == std::string::npos) break;
        key.resize(slash);
        if (const auto it = names.names.find(key); it != names.names.end()) return it->second;
    }
    return "";
}

void set_nickname(Nicknames& names, const std::string& path, const std::string& name) {
    const size_t first = name.find_first_not_of(" \t"), last = name.find_last_not_of(" \t");
    const std::string key = key_of(path);
    if (first == std::string::npos || key.empty()) names.names.erase(key);
    else names.names[key] = name.substr(first, last - first + 1);
}

}  // namespace remod
