#pragma once
// Per-user settings remembered between app sessions.
#include <filesystem>
#include <string>

namespace remod {

struct Settings {
    std::string graph_path;
    std::string noesis_path;
    bool operator==(const Settings&) const = default;
};

// %APPDATA%\remod\settings.json (empty if APPDATA isn't set).
std::filesystem::path default_settings_path();

// Missing or unreadable file -> default Settings; never throws (a bad settings file must not stop the app).
Settings load_settings(const std::filesystem::path& file);

// Creates the folder if needed. Throws std::runtime_error on failure.
void save_settings(const Settings& settings, const std::filesystem::path& file);

}  // namespace remod
