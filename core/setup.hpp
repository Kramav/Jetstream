#pragma once
// Finding and checking the external tools a user has installed (CLAUDE.md §6).
#include "profile.hpp"

#include <filesystem>
#include <string>

namespace remod {

// First Noesis64.exe found: `saved` (a remembered path), %REMOD_NOESIS%, PATH, then winget's portable-package
// folder. Empty if none.
std::filesystem::path find_noesis(const std::string& saved);

struct NoesisCheck {
    bool ok = false;
    std::string message;  // one line for the user: what's fine, or what's missing and how to get it
};

// Noesis present, and the RE Engine plugin (fmt_RE_MESH.py) installed next to it.
NoesisCheck check_noesis(const std::filesystem::path& noesis_exe);

// The extracted game files folder (e.g. REtool's .../natives/stm) that the RE Engine plugin remembers for this
// game in plugins/python/<noesis_game>NativesPath.txt. Empty if not set or not a folder.
std::filesystem::path game_files_dir(const std::filesystem::path& noesis_exe, const Profile& profile);

}  // namespace remod
