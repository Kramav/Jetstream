#pragma once
// Scripted real-time cutscenes (CLAUDE.md §10 M3 route 3): a cutscene is a JSON file (schemas/cutscene.v0.example.json)
// played by remod's one generic runtime script (runtime/remod_cutscene.lua). A mod ships the runtime and its cutscenes.
#include "package.hpp"

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace remod {

struct Profile;
class ITextureConverter;

// remod's REFramework runtime scripts: <tool>/runtime, beside profiles (empty if there are no profiles).
std::filesystem::path runtime_dir();

// The cutscene runtime as the game reads it: <runtime>/remod_cutscene.lua at reframework/autorun/remod_cutscene.lua.
// Alone, it's enough to record camera keys (F10) before a cutscene file exists. Throws PackageError if it's missing.
PackageFile cutscene_runtime(const std::filesystem::path& runtime = runtime_dir());

// What a mod ships for a cutscene: the runtime at reframework/autorun/remod_cutscene.lua, then the cutscene at
// reframework/data/remod_cutscenes/<file name>, then each of its actors' puppet definitions that remod ships
// (<runtime>/puppets/<puppet>.json at reframework/data/remod_puppets/; others come from the mod adding that
// character). Throws PackageError if the runtime or the cutscene is missing or the file isn't .json.
std::vector<PackageFile> cutscene_files(const std::filesystem::path& cutscene,
                                        const std::filesystem::path& runtime = runtime_dir());

// What's wrong with a cutscene file (format v0), one problem per entry, e.g. "camera key 3: t 2 isn't after the key
// before it (4)"; empty if nothing. For a person or an AI writing one by hand.
std::vector<std::string> check_cutscene(const std::string& json_text);

// ---- A cutscene file as the app's editor edits it (user, 2026-10-08: "a way to place actors and write motions
// without hand-editing JSON") ----
struct CutsceneActor {
    std::string name, puppet;
    bool at_spot = false;  // position + rotation (written down in game); else offset from the player
    std::array<double, 3> position{0, 0, 0}, offset{0, 0, 1.5};
    std::array<double, 4> rotation{0, 0, 0, 1};
    bool hides_partner = false;
    bool operator==(const CutsceneActor&) const = default;
};
struct CutsceneMotion {
    double t = 0;
    std::string actor = "player";
    long long bank = 0, motion = 0;
    double frame = 0, blend = 10;
    bool operator==(const CutsceneMotion&) const = default;
};
// An animation file (.motlist) put on a character as a bank of the cutscene's own number when it plays: a game
// cutscene's own animations (_Chainsaw/Event/cs/<id>/.../chara/<mesh>/<mesh>.motlist; spikes/event_animation_test.md),
// later a new one. Its motions are then that bank's.
struct CutsceneAnimationFile {
    std::string actor = "player";  // or an actor's name
    std::string file;              // game path, no natives/STM, no suffix
    long long bank = 9000;
    bool operator==(const CutsceneAnimationFile&) const = default;
};
struct CutsceneSubtitle {
    double t = 0, until = 2;
    std::string text;
    bool operator==(const CutsceneSubtitle&) const = default;
};
struct CutsceneFade {
    double t = 0, until = 1, from = 1, to = 0;  // black's opacity, 0-1
    bool operator==(const CutsceneFade&) const = default;
};
struct CutsceneCue {  // a movie or a sound, by its id
    double t = 0;
    std::string id;
    bool operator==(const CutsceneCue&) const = default;
};
// A camera key: where the camera is at time t (recorded in game with F10, never typed) and how it arrives there.
struct CutsceneCameraKey {
    double t = 0;
    std::array<double, 3> position{0, 0, 0};
    std::array<double, 4> rotation{0, 0, 0, 1};
    double fov = 0;    // degrees; 0: not set (the game's)
    std::string ease;  // "smooth" (also when empty), "linear" or "cut"
    bool operator==(const CutsceneCameraKey&) const = default;
};
struct Cutscene {
    std::string name;
    double length = 5;
    std::string start_key;  // empty: none
    double letterbox = 0;
    std::vector<CutsceneCameraKey> camera;
    std::vector<CutsceneActor> actors;
    std::vector<CutsceneMotion> motions;
    std::vector<CutsceneAnimationFile> animation_files;
    std::vector<CutsceneSubtitle> subtitles;
    std::vector<CutsceneFade> fades;
    std::vector<CutsceneCue> movies, sounds;
    bool trigger = false;  // kept as it is (Use trigger sets it)
    std::string kept;      // the file's other fields (trigger, anything else), as JSON
    bool operator==(const Cutscene&) const = default;
};

// Reads a cutscene file; one that doesn't exist yet is a new cutscene named after it. Fields of an entry the editor
// doesn't know are dropped (whole fields it doesn't know are kept). Throws PackageError if it isn't readable JSON.
Cutscene read_cutscene(const std::filesystem::path& file);
// The file's text for `c` (what write_cutscene writes, and what check_cutscene checks).
std::string cutscene_text(const Cutscene& c);
// Writes it, the previous file kept as <file>.bak.
void write_cutscene(const std::filesystem::path& file, const Cutscene& c);
// A starter cutscene for a new file: named after it, 5 s, start key F5, letterbox bars, a fade in and a fade out.
Cutscene new_cutscene(const std::filesystem::path& file);
// Whether a file is a cutscene: a .json (under 1 MB) holding an object with schema_version and length, and not a
// graph (no "nodes"). For the Browser, which lists cutscenes among other JSON files.
bool is_cutscene_file(const std::filesystem::path& file);

// ---- The cutscene along its timeline (the app's Cutscene layout; user, 2026-10-08: "the ui is now not conducive to
// programming cutscenes") ----
enum class CutsceneLane { Camera, Fade, Subtitle, Motion, Movie, Sound };
struct CutsceneItemRef {
    CutsceneLane lane = CutsceneLane::Camera;
    int index = -1;  // in that list of the cutscene (motions: c.motions, whichever actor); -1: none
    bool operator==(const CutsceneItemRef&) const = default;
    explicit operator bool() const { return index >= 0; }
};
struct CutsceneTimelineItem {
    CutsceneItemRef ref;
    double t = 0;
    std::optional<double> until;  // a span (fades, subtitles); else a moment
    std::string label;
};
struct CutsceneTimelineLane {
    CutsceneLane lane;
    std::string actor;  // Motion lanes: "player" or an actor's name
    std::string title;
    std::vector<CutsceneTimelineItem> items;
};
// The lanes, top to bottom: camera, fades, subtitles, the player's motions, each actor's, movies, sounds.
std::vector<CutsceneTimelineLane> cutscene_lanes(const Cutscene& c);
// Sets an item's time (and a span's end), kept inside 0..length (a span at least 0.05 s long). Camera keys stay in
// time order: a key moved past another is re-sorted, and `ref` follows it. Nothing if `ref` isn't an item.
void set_item_time(Cutscene& c, CutsceneItemRef& ref, double t, std::optional<double> until = std::nullopt);
// A new item at t on a lane (motions on `actor`): a 2 s subtitle, a 1 s fade (in near the start, else out), a motion
// (Leon's idle, bank 1000 motion 160), an empty movie or sound. Camera keys can't be added here (they're recorded):
// returns no item for Camera.
CutsceneItemRef add_item(Cutscene& c, CutsceneLane lane, const std::string& actor, double t);
void remove_item(Cutscene& c, CutsceneItemRef ref);

// What the cutscene looks like at time t, as the runtime draws it (runtime/remod_cutscene.lua: draw_overlays,
// camera_at): black's opacity, the subtitle shown, the camera's keys around t, and what each character is playing.
struct CutsceneFrame {
    double black = 0;  // 0 clear, 1 black
    std::string subtitle;
    int camera_from = -1, camera_to = -1;  // key indexes (the same before the first and after the last); -1: no keys
    double camera_progress = 0;            // 0-1 from one to the other, after the ease
    struct Playing {
        std::string actor;
        int motion = -1;  // index in c.motions; -1: nothing started yet
        double since = 0; // seconds since it started
    };
    std::vector<Playing> playing;  // the player, then each actor
};
CutsceneFrame cutscene_frame(const Cutscene& c, double t);

// The editor's Use recording / Use trigger, on the cutscene being edited (the file is written by Save): the recorded
// camera keys replace c.camera and the length grows to a second past the last; the trigger replaces any. Throw
// PackageError as use_recording / use_trigger do.
void apply_recording(Cutscene& c, const std::filesystem::path& recording);
void apply_trigger(Cutscene& c, const std::filesystem::path& trigger);

// What the game wrote down for the editor, under <game>\reframework\data\remod_cutscenes: the runtime's "Write down
// Leon's spot" (spot.json) and the animation previewer's "Use in a cutscene" (animation.json). Nothing if missing or
// not readable.
struct Spot {
    std::array<double, 3> position{};
    std::array<double, 4> rotation{0, 0, 0, 1};
};
std::optional<Spot> read_spot(const std::filesystem::path& game_dir);
struct PickedAnimation {
    std::string actor;  // "player", or the puppet it was played on
    long long bank = 0, motion = 0;
    std::string name;
    double frame = 0;
    std::string file;  // the animation file its bank was added from in the previewer; empty: the character's own bank
};
std::optional<PickedAnimation> read_picked_animation(const std::filesystem::path& game_dir);

// The puppet definitions an actor can use: remod's (<runtime>/puppets) and those in the game's
// reframework\data\remod_puppets (a new character's mod), by name, sorted, each once.
std::vector<std::string> puppet_names(const std::filesystem::path& game_dir,
                                      const std::filesystem::path& runtime = runtime_dir());

// ---- A new character (user, 2026-10-08), as spikes/make_new_character.ps1 made rmc001 by hand ----
// One part of a puppet definition's character (its body, say) copied to new paths with its colour textures
// recoloured, and a definition for it; nothing of the game's replaced. The new name takes the place of the part's
// character folder (cha103 in _Chainsaw/Character/ch/cha1/cha103/00/cha103_00.mesh), so it must be as long: the
// material file's texture paths are changed in place, keeping its layout.
struct NewCharacter {
    std::string name;
    std::filesystem::path definition;  // a puppet definition: remod's (runtime/puppets) or a mod's
    std::string part = "body";
    std::function<bool(const std::string& texture_file_name)> skip;  // colour textures left as they are (skin)
    float hue = 220, saturation = 0.6f;     // colourize: the new colour, keeping each pixel's lightness
    float brightness = 0, contrast = 0;     // then, -1 to 1 (Adjust colour's)
};
// Writes it under `out` at the paths the game reads (out/<game path>, out/reframework/data/remod_puppets/<name>.json)
// from the extracted game files `natives` (natives\STM). Returns its files (game paths as a package takes them), and
// `report` says what it did. Throws PackageError.
std::vector<PackageFile> make_new_character(const NewCharacter& spec, const std::filesystem::path& natives,
                                            const Profile& profile, ITextureConverter& converter,
                                            const std::filesystem::path& out, std::string* report = nullptr);

// The camera keys of an in-game recording (the runtime's F10: remod_cutscenes/recording.json) into `cutscene`: its
// "camera" replaced and its length made long enough, the rest kept (the previous file as <cutscene>.bak); a new file
// (named after it, the recording's length) if it doesn't exist yet. Throws PackageError if the recording is missing or
// has no keys.
void use_recording(const std::filesystem::path& cutscene, const std::filesystem::path& recording);

// The runtime's "Make a trigger here" (remod_cutscenes/trigger.json: Leon's spot and the game's names for where he is)
// into `cutscene` as its "trigger", replacing any; the rest kept (the previous file as .bak). A new file (named after
// it, 5 s long) if it doesn't exist yet. Throws PackageError if the trigger file is missing or not one.
void use_trigger(const std::filesystem::path& cutscene, const std::filesystem::path& trigger);

}  // namespace remod
