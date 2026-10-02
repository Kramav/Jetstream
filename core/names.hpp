#pragma once
// Readable names (CLAUDE.md §10): nicknames a user gives the game's cryptic folders and files (cha000 -> "Leon"),
// shown beside the real names in the browser and found by its search. They only label things: real paths stay what's
// packaged and stored.
#include <filesystem>
#include <map>
#include <string>

namespace remod {

// One game's nicknames. Keys are paths relative to the natives root, '/'-separated, lowercase (Windows paths ignore
// case); folders and files alike.
struct Nicknames {
    std::map<std::string, std::string> names;
    bool operator==(const Nicknames&) const = default;
};

// %APPDATA%\remod\names\<game>.json (empty if APPDATA isn't set). A plain {"path": "nickname"} object, so a list can be
// shared by copying the file.
std::filesystem::path default_names_path(const std::string& game);

// A missing file: no nicknames. Throws std::runtime_error if the file can't be read or isn't such an object, so the
// caller doesn't save over (and lose) a file it couldn't read.
Nicknames load_names(const std::filesystem::path& file);
// Creates the folder if needed. Throws std::runtime_error on failure.
void save_names(const Nicknames& names, const std::filesystem::path& file);

std::string nickname(const Nicknames& names, const std::string& path);  // "" if it has none
// The nickname of the nearest folder above `path` that has one, else "".
std::string nickname_above(const Nicknames& names, const std::string& path);
// Trimmed; an empty name removes the nickname.
void set_nickname(Nicknames& names, const std::string& path, const std::string& name);

}  // namespace remod
