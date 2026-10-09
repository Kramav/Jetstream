#pragma once
// The Cutscene layout (user, 2026-10-08: "the ui is now not conducive to programming cutscenes"): a cutscene file
// edited on a timeline you drag on, the moment at the playhead previewed (bars, fades, subtitles; not the game), a
// top-down stage of where the characters stand, the selected item's settings, and every item in tables. Thin UI over
// core/cutscene (cutscene_lanes, set_item_time, add_item, cutscene_frame, ...). The file stays plain JSON.
#include "cutscene.hpp"
#include "graph.hpp"  // Snapshots: undo

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

enum class CutsceneAction { None, Closed, AddToGraph, TestInGame, RemoveFromGame, SaveAs };

struct CutsceneEdit {
    std::filesystem::path file;  // empty: a new cutscene not saved anywhere yet
    CutsceneAction after_save = CutsceneAction::None;  // what a SaveAs was asked for, done once it's saved
    bool game_copy = false;  // the file is the game's copy (reframework\data\remod_cutscenes): Save is all it takes
    remod::Cutscene doc, saved, checked;  // `problems` are for `checked`
    std::vector<std::string> puppets, problems;
    remod::Snapshots<remod::Cutscene> history;
    remod::CutsceneItemRef selected;  // an item on the timeline, else
    int actor = -1;                   // an actor (its lane's name, or on the stage), else the cutscene itself
    double playhead = 0;
    bool playing = false;
    bool confirm_close = false;
    // A drag on the timeline: what (1 the body, 2 a span's start, 3 its end), and where it started.
    int drag = 0;
    float drag_x = 0;
    double drag_t = 0, drag_until = 0;
    // A right-click on a lane: which lane (its index in cutscene_lanes) and when.
    int menu_lane = -1;
    double menu_t = 0;
};

// Opens a cutscene file (a new one, not written yet, if it doesn't exist). Nothing, and `status` says why, if it
// can't be read.
std::optional<CutsceneEdit> open_cutscene(const std::filesystem::path& file, const std::filesystem::path& game_dir,
                                          std::string& status);
// A new cutscene with no file yet (the Cutscene layout with nothing open, user 2026-10-08: "just make it so it opens
// an empty cutscene"); untouched, it has nothing to save.
CutsceneEdit untitled_cutscene(const std::filesystem::path& game_dir);
// Gives it its file (Save as). The name follows the file while it's still the untitled one's.
void set_cutscene_file(CutsceneEdit& e, const std::filesystem::path& file, const std::filesystem::path& game_dir);
// Writes it (the previous file as .bak); false, and `status` says why, if it couldn't (or it has no file yet).
bool save_cutscene(CutsceneEdit& e, std::string& status);
void cutscene_undo(CutsceneEdit& e, bool redo);
bool cutscene_unsaved(const CutsceneEdit& e);
// The game's remod_cutscenes folder, where Test in game puts cutscenes and the runtime writes what it records.
std::filesystem::path game_cutscenes_dir(const std::filesystem::path& game_dir);

// Draws the "Cutscene" and "Cutscene item" windows. `in_graph`: a Cutscene block of the graph holds this file.
// Returns what the caller does next (the graph and the game folder are its): Closed (the window's
// cutscene closed: drop it), Add to graph, Test in game, Remove from game, SaveAs (it has no file yet: ask where, then
// do e.after_save).
CutsceneAction draw_cutscene_layout(CutsceneEdit& e, const std::filesystem::path& game_dir, bool in_graph,
                                    std::string& status);
