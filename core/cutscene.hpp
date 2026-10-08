#pragma once
// Scripted real-time cutscenes (CLAUDE.md §10 M3 route 3): a cutscene is a JSON file (schemas/cutscene.v0.example.json)
// played by remod's one generic runtime script (runtime/remod_cutscene.lua). A mod ships the runtime and its cutscenes.
#include "package.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace remod {

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
