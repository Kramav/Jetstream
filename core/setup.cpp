#include "setup.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdlib>
#include <fstream>

namespace remod {

namespace fs = std::filesystem;

namespace {

constexpr const char* kPluginUrl = "https://github.com/SilverEzredes/fmt_RE_MESH-Noesis-Plugin_SILVER";

std::string env(const char* name) {
    char* v = nullptr;
    size_t n = 0;
    _dupenv_s(&v, &n, name);
    std::string s = v ? v : "";
    std::free(v);
    return s;
}

bool is_file(const fs::path& p) {
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(p, ec);
}

}  // namespace

fs::path find_noesis(const std::string& saved) {
    if (is_file(saved)) return saved;
    if (const fs::path p = env("REMOD_NOESIS"); is_file(p)) return p;

    wchar_t found[MAX_PATH];
    if (const DWORD n = SearchPathW(nullptr, L"Noesis64.exe", nullptr, MAX_PATH, found, nullptr); n > 0 && n < MAX_PATH)
        return found;

    // winget installs Noesis as a portable zip [inferred: winget's portable layout; `winget show` reports
    // "Installer Type: portable (zip)"], extracted under %LOCALAPPDATA%\Microsoft\WinGet\Packages\<id>_<source>\.
    std::error_code ec;
    const fs::path packages = fs::path(env("LOCALAPPDATA")) / "Microsoft" / "WinGet" / "Packages";
    for (const auto& dir : fs::directory_iterator(packages, ec)) {
        if (!dir.path().filename().string().starts_with("RichWhitehouse.Noesis")) continue;
        for (auto it = fs::recursive_directory_iterator(dir.path(), ec); it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (it.depth() > 2) it.disable_recursion_pending();
            if (it->path().filename() == "Noesis64.exe") return it->path();
        }
    }
    return {};
}

NoesisCheck check_noesis(const fs::path& exe) {
    if (exe.empty())
        return {false, "Noesis not found. Install it (winget install -e --id RichWhitehouse.Noesis) or pick "
                       "Noesis64.exe with the ... button."};
    if (!is_file(exe)) return {false, "Noesis not found at " + exe.string() + "."};
    const fs::path plugin = exe.parent_path() / "plugins" / "python" / "fmt_RE_MESH.py";
    if (!is_file(plugin))
        return {false, "Noesis is there, but the RE Engine plugin isn't: put fmt_RE_MESH.py into " +
                           plugin.parent_path().string() + " (download: " + kPluginUrl + ")."};
    return {true, "Noesis OK, RE Engine plugin found."};
}

fs::path game_files_dir(const fs::path& exe, const Profile& profile) {
    if (exe.empty() || profile.noesis_game.empty()) return {};
    std::ifstream in(exe.parent_path() / "plugins" / "python" / (profile.noesis_game + "NativesPath.txt"));
    std::string line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    std::error_code ec;
    return !line.empty() && fs::is_directory(line, ec) ? fs::path(line) : fs::path();
}

}  // namespace remod
