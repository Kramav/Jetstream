// The Cutscene layout without a screen: an ImGui context of its own (no window, no renderer) draws it frame after
// frame while a seeded random "mouse" clicks, drags and right-clicks all over it and presses its keys. It fails on an
// ImGui assert (unbalanced IDs, a cursor left outside the window, ...), a crash, or an edit core can't write back.
// Nothing on the real screen is touched. Run by ctest; argument: a cutscene file to start from (copied first).
#include "cutscene_view.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <crtdbg.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    // An assert prints and stops, never a dialog waiting for a click (ctest runs this unattended).
    _set_error_mode(_OUT_TO_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    if (argc < 2) {
        std::fprintf(stderr, "usage: cutscene_view_smoke <cutscene.json>\n");
        return 2;
    }
    const fs::path dir = fs::temp_directory_path() / "remod_cutscene_smoke";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path file = dir / "smoke.json";
    fs::copy_file(argv[1], file);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1600, 1000);
    io.DeltaTime = 1.0f / 60;
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

    std::string status;
    auto edit = open_cutscene(file, {}, status);
    if (!edit) {
        std::fprintf(stderr, "couldn't open: %s\n", status.c_str());
        return 1;
    }
    std::mt19937 rng(20261008);
    std::uniform_real_distribution<float> x(0, 1600), y(0, 1000), unit(0, 1);
    int frames = 0, changes = 0, selects = 0;  // frames whose edits changed the cutscene; items selected
    int closes = 0, saved_as = 0;
    float timeline_end = 500;  // where the Cutscene window's content ends (the timeline, then a closed header)
    for (int round = 0; round < 800; ++round) {
        // A gesture: move somewhere, press, drag a little, let go; sometimes a right-click, a key, Escape. Every other
        // one lands in the timeline's band (its items are small: random points anywhere rarely hit one).
        ImVec2 from(x(rng), y(rng));
        if (round % 2) from = ImVec2(260 + unit(rng) * 880, timeline_end - 230 + unit(rng) * 200);
        else if (round % 10 == 4) from = ImVec2(unit(rng) * 900, 8 + unit(rng) * 40);  // the top bar: Save, Close, ...
        if (round == 400) edit = untitled_cutscene({});  // the second half from a new cutscene, never saved
        const ImVec2 to(from.x + float(int(rng() % 200) - 100), from.y + float(int(rng() % 10) - 5));
        const int kind = int(rng() % 10);
        for (int step = 0; step < 6; ++step) {
            const float u = step / 5.0f;
            io.AddMousePosEvent(from.x + (to.x - from.x) * u, from.y + (to.y - from.y) * u);
            if (step == 1) io.AddMouseButtonEvent(kind == 0 ? 1 : 0, true);
            if (step == 4) io.AddMouseButtonEvent(kind == 0 ? 1 : 0, false);
            if (step == 5 && kind == 1) io.AddKeyEvent(ImGuiKey_Delete, true);
            if (step == 5 && kind == 2) io.AddKeyEvent(ImGuiKey_Space, true);
            if (step == 5 && kind == 3) io.AddKeyEvent(ImGuiKey_Escape, true);
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(1150, 1000), ImGuiCond_Always);
            const remod::Cutscene before = edit->doc;
            const remod::CutsceneItemRef selected = edit->selected;
            const CutsceneAction a = draw_cutscene_layout(*edit, {}, false, status);
            changes += edit->doc != before;
            selects += edit->selected && edit->selected != selected;
            if (ImGuiWindow* win = ImGui::FindWindowByName("Cutscene")) timeline_end = win->ContentSize.y;
            ImGui::Render();
            io.AddKeyEvent(ImGuiKey_Delete, false);
            io.AddKeyEvent(ImGuiKey_Space, false);
            io.AddKeyEvent(ImGuiKey_Escape, false);
            ++frames;
            if (a == CutsceneAction::SaveAs) {  // a new cutscene saved: where the app's save dialog would put it
                set_cutscene_file(*edit, dir / "saved_as.json", {});
                if (!save_cutscene(*edit, status)) {
                    std::fprintf(stderr, "round %d: Save as failed: %s\n", round, status.c_str());
                    return 1;
                }
                ++saved_as;
                if (edit->after_save == CutsceneAction::Closed) edit.reset();
            }
            if (a == CutsceneAction::Closed || !edit) {  // closed: open it again, or a new one, and go on
                if (++closes % 2) edit = untitled_cutscene({});
                else edit = open_cutscene(file, {}, status);
                if (!edit) return 1;
            }
            if (rng() % 50 == 0) cutscene_undo(*edit, rng() % 2);
        }
        // Whatever was done, core must write it and read it back the same (as the file has it: an actor at a spot
        // keeps no offset).
        const std::string text = remod::cutscene_text(edit->doc);
        remod::write_cutscene(dir / "check.json", edit->doc);
        if (remod::cutscene_text(remod::read_cutscene(dir / "check.json")) != text) {
            std::fprintf(stderr, "round %d: written and read back differently\n%s\n", round, text.c_str());
            return 1;
        }
    }
    ImGui::DestroyContext();
    std::printf("%d frames, %d of them changing the cutscene, %d items selected, %d closed, %d saved as, without a "
                "problem\n", frames, changes, selects, closes, saved_as);
    if (changes < 20 || selects < 10) {
        std::fprintf(stderr, "too few edits: the gestures missed the layout\n");
        return 1;
    }
    fs::remove_all(dir);
    return 0;
}
