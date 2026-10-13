#pragma once
#include <algorithm>
#include <array>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace remod {

// A game's features (Profile::features): textures (.tex read and written), meshes (.mesh read), sounds (Wwise banks
// and packages), movies (game movies replaced), cutscenes (remod's cutscene script runs there).
inline constexpr std::array<std::string_view, 5> kFeatures{"textures", "meshes", "sounds", "movies", "cutscenes"};

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
    std::string noesis_export;  // optional (Noesis is): its ?cmode export options for this game
    std::string file_list;
    std::string noesis_game;  // optional: the RE Engine plugin's name for the game, e.g. "RE4" (its <game>NativesPath.txt)
    // optional: the game's Wwise sound files (banks .sbnk, packages .spck) under the natives root and its streaming/,
    // e.g. "_chainsaw/sound/wwise". Empty: sounds can't be replaced.
    std::string sound_dir;
    // optional: a movie's sound packages there, a file name prefix with {id} for the movie's, e.g. "ch_{id}_".
    std::string movie_sound;
    // optional: REFramework's name for the game (reframework:get_game_name(), e.g. "re4"): the cutscene script's game
    // file (runtime/remod_cutscene/<name>.lua).
    std::string reframework;
    // What remod can do for the game (kFeatures): a block needing one it lacks isn't offered (block_available).
    std::vector<std::string> features;
    // What isn't supported yet, or not checked, a sentence each (shown in the app).
    std::vector<std::string> notes;
    bool has(std::string_view feature) const { return std::ranges::find(features, feature) != features.end(); }

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
