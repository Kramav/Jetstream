#pragma once
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace remod {

struct ProfileError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// profiles/<game>.toml, [game] table. See CLAUDE.md §5.
struct Profile {
    std::string id;
    std::string name;
    std::string tex_suffix;
    std::string natives_root;
    std::vector<std::string> packaging;  // subset of {"loose_archive", "pak"}
    std::string pak_script;
    std::string noesis_export;
    std::string file_list;

    // Names of fields still set to "TBD" (awaiting the §9 spike).
    std::vector<std::string> unresolved() const;
};

// Throws ProfileError listing every problem found.
Profile load_profile(const std::filesystem::path& file);
Profile parse_profile(std::string_view toml_text);

// Every valid <profiles_dir>/*.toml, sorted by name. A broken profile doesn't hide the others: its problem is
// appended to `errors` (if given) instead.
std::vector<Profile> load_profiles(const std::filesystem::path& profiles_dir,
                                   std::vector<std::string>* errors = nullptr);

// <profiles_dir>/<id>.toml. The id must be a plain name (letters, digits, '_', '-').
Profile load_profile_by_id(const std::filesystem::path& profiles_dir, const std::string& id);

// First "profiles" folder found in the current directory, then the executable's folder and its parents.
// Empty if none.
std::filesystem::path find_profiles_dir();

}  // namespace remod
