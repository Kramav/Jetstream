#pragma once
// The game's sounds by id (CLAUDE.md §10, "Game sounds"). A sound (a WEM) lives in a sound package (.spck, the copy
// under streaming/; the one outside holds the header only) or whole in a bank's media (.sbnk); a streamed one also has
// its first part (prefetch) in a bank; and a bank's event data records each one's in-memory size. Replacing a sound
// rewrites all of them consistently, so the game's banks still fit.
#include "wwise.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace remod {

struct SoundFile {
    std::string game_path;  // under the natives root, e.g. "streaming/_chainsaw/sound/wwise/ch_x.spck.1.x64"
    std::string bytes;
};

// A new sound for one id: 16-bit samples in `like`'s channels (a WAV's order), any length; music tracks record their
// sounds' lengths, so a `music` one should keep like.samples.
using SoundSource = std::function<std::vector<std::int16_t>(std::uint32_t id, const WemInfo& like, bool music)>;

struct SoundReplacement {
    std::vector<SoundFile> files;  // every file changed: banks, and both copies of packages
    std::set<std::uint32_t> missing;  // ids in no file
};

// Replaces sounds by id everywhere under <natives>/<sound_dir> and <natives>/streaming/<sound_dir>. Each new sound is
// encoded in its original's form (encode_wem); an id found in several languages' files gets each its own original's.
// Throws std::runtime_error.
SoundReplacement replace_sounds(const std::filesystem::path& natives, const std::string& sound_dir,
                                const std::set<std::uint32_t>& ids, const SoundSource& sound,
                                const std::filesystem::path& codebooks);

// A brand-new sound bank, made from `like`: a game bank holding one event and one sound stored whole in it (e.g. RE4R's
// ch_csa404_se). Every id the bank defines (the bank's own, each event data object's, its sound's) gets a new one made
// from `name`, so it loads beside the game's banks; its sound becomes `pcm(original's info)` in the original's form.
// Gives the bytes and the new bank and event ids (the event is what plays it). Throws std::runtime_error.
struct NewSoundBank {
    std::string bytes;
    std::uint32_t bank_id = 0, event_id = 0;
};
NewSoundBank new_sound_bank(std::string_view like, const std::string& name,
                            const std::function<std::vector<std::int16_t>(const WemInfo&)>& pcm,
                            const std::filesystem::path& codebooks);

// What's wrong with a new sound's name (a New sound block's, the name cutscenes play it by), or "" if nothing:
// 1-32 lowercase letters, digits or _.
std::string new_sound_name_problem(const std::string& name);

// The sounds in one bank or package (for a package outside streaming/, its streaming copy's), for a browser.
struct SoundEntry {
    std::uint32_t id = 0;
    WemInfo info;       // zeros if it isn't a sound (e.g. a reverb's impulse response)
    std::string where;  // "in this bank", "streamed: its first part", "in this package", "not a sound"
};
std::vector<SoundEntry> list_sounds(const std::filesystem::path& file);

// One sound's whole WEM, from `file` (a bank's first part of a streamed sound: from the packages beside it).
std::string sound_wem(const std::filesystem::path& file, std::uint32_t id);

// A sound's 16-bit samples as a WAV file's bytes (for playing and saving). Throws.
std::string wav_bytes(const std::vector<std::int16_t>& pcm, unsigned channels, unsigned rate);

// A game sound as a block's field holds it: "<bank or package>#<id>" (the file says where it was found; the id is
// what's replaced). parse_game_sound: false if `text` isn't one.
std::string game_sound_text(const std::string& file, std::uint32_t id);
bool parse_game_sound(const std::string& text, std::string& file, std::uint32_t& id);

// The sound id in a file name: its last run of 4 or more digits ("vo_leon_880852580.wav" -> 880852580); 0 if none.
std::uint32_t sound_id_in(const std::string& name);

}  // namespace remod
