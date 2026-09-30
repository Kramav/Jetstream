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

}  // namespace remod
