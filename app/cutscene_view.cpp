#include "cutscene_view.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fs = std::filesystem;

namespace {

const ImVec4 kRed(1, 0.45f, 0.4f, 1), kAmber(1, 0.75f, 0.3f, 1);

bool edit_double(const char* label, double& v, double speed, double lo, double hi, const char* format = "%.2f") {
    return ImGui::DragScalar(label, ImGuiDataType_Double, &v, float(speed), &lo, &hi, format, ImGuiSliderFlags_AlwaysClamp);
}
bool edit_int(const char* label, long long& v) {
    const long long lo = 0, hi = 0xFFFFFFFFll;
    return ImGui::DragScalar(label, ImGuiDataType_S64, &v, 1.0f, &lo, &hi, "%lld", ImGuiSliderFlags_AlwaysClamp);
}
// A combo over `items` ("" shown as `none`); true when changed.
bool pick(const char* label, std::string& v, const std::vector<std::string>& items, const char* none = "(none)") {
    bool changed = false;
    if (ImGui::BeginCombo(label, v.empty() ? none : v.c_str())) {
        for (const auto& item : items)
            if (ImGui::Selectable(item.empty() ? none : item.c_str(), item == v)) {
                v = item;
                changed = true;
            }
        ImGui::EndCombo();
    }
    return changed;
}
template <class T>
void erase_row(std::vector<T>& rows, int& erase) {
    if (erase >= 0 && erase < int(rows.size())) rows.erase(rows.begin() + erase);
    erase = -1;
}
// Snapped to 0.05 s, unless Shift is held.
double snap(double t) { return ImGui::GetIO().KeyShift ? t : std::round(t * 20) / 20; }

ImU32 lane_colour(remod::CutsceneLane lane) {
    switch (lane) {
    case remod::CutsceneLane::Camera: return IM_COL32(230, 180, 70, 255);
    case remod::CutsceneLane::Fade: return IM_COL32(120, 130, 150, 220);
    case remod::CutsceneLane::Subtitle: return IM_COL32(80, 140, 225, 220);
    case remod::CutsceneLane::Motion: return IM_COL32(240, 150, 70, 255);
    case remod::CutsceneLane::Movie:
    case remod::CutsceneLane::Sound: return IM_COL32(200, 110, 220, 255);
    }
    return IM_COL32_WHITE;
}
const char* lane_noun(remod::CutsceneLane lane) {
    switch (lane) {
    case remod::CutsceneLane::Camera: return "camera key";
    case remod::CutsceneLane::Fade: return "fade";
    case remod::CutsceneLane::Subtitle: return "subtitle";
    case remod::CutsceneLane::Motion: return "animation";
    case remod::CutsceneLane::Movie: return "movie";
    case remod::CutsceneLane::Sound: return "sound";
    }
    return "item";
}

// The selection still names something (after an undo, a remove, a reload).
void keep_selection_valid(CutsceneEdit& e) {
    const auto lanes = remod::cutscene_lanes(e.doc);
    const bool found = std::ranges::any_of(lanes, [&](const auto& lane) {
        return std::ranges::any_of(lane.items, [&](const auto& item) { return item.ref == e.selected; });
    });
    if (!found) e.selected = {};
    if (e.actor >= int(e.doc.actors.size())) e.actor = -1;
    e.playhead = std::clamp(e.playhead, 0.0, std::max(e.doc.length, 0.0));
}

void remove_actor(remod::Cutscene& c, int i) {  // and its animations and animation files, which name it
    const std::string name = c.actors[i].name;
    std::erase_if(c.motions, [&](const auto& m) { return m.actor == name; });
    std::erase_if(c.animation_files, [&](const auto& f) { return f.actor == name; });
    c.actors.erase(c.actors.begin() + i);
}

void add_actor(CutsceneEdit& e) {
    std::string puppet = e.puppets.empty() ? "luis" : e.puppets.front();
    for (const auto& p : e.puppets)  // one actor per puppet: the first one not used yet
        if (std::ranges::none_of(e.doc.actors, [&](const auto& a) { return a.puppet == p; })) {
            puppet = p;
            break;
        }
    e.doc.actors.push_back({.name = puppet, .puppet = puppet, .offset = {0, 0, 1.5}});
    e.actor = int(e.doc.actors.size()) - 1;
    e.selected = {};
}

// The moment at the playhead, as the game's screen would show remod's part of it: the letterbox bars, the fade's
// black and the subtitle (runtime/remod_cutscene.lua, draw_overlays). The game itself isn't drawn.
void draw_preview(const CutsceneEdit& e, const remod::CutsceneFrame& f, ImVec2 size) {
    const ImVec2 at = ImGui::GetCursorScreenPos(), end(at.x + size.x, at.y + size.y);
    ImDrawList* d = ImGui::GetWindowDrawList();
    d->AddRectFilled(at, end, IM_COL32(52, 58, 68, 255));
    const char* note = "the game shows here (not previewed)";
    const ImVec2 note_size = ImGui::CalcTextSize(note);
    d->AddText(ImVec2(at.x + (size.x - note_size.x) / 2, at.y + (size.y - note_size.y) / 2), IM_COL32(120, 128, 140, 255), note);
    const float bar = float(e.doc.letterbox) * size.y;
    if (bar > 0) {
        d->AddRectFilled(at, ImVec2(end.x, at.y + bar), IM_COL32_BLACK);
        d->AddRectFilled(ImVec2(at.x, end.y - bar), end, IM_COL32_BLACK);
    }
    if (!f.subtitle.empty()) {  // 10% from the left, above the bar (or 6% of the height), as the runtime draws it
        const float y = end.y - std::max(bar, size.y * 0.06f) - ImGui::GetFontSize() * 1.4f;
        d->AddText(ImVec2(at.x + size.x * 0.1f, y), IM_COL32_WHITE, f.subtitle.c_str());
    }
    if (f.black > 0) d->AddRectFilled(at, end, IM_COL32(0, 0, 0, int(f.black * 255)));
    d->AddRect(at, end, ImGui::GetColorU32(ImGuiCol_Border));
    ImGui::Dummy(size);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("remod's part of the screen at the playhead: the letterbox bars, fades to black and the "
                          "subtitle. The game's picture, the camera and the characters only show in game.");
}

// Who stands where, seen from above: Leon in the middle facing up, each actor at its offset (right, forward), facing
// him. Dragging an actor moves it (snapped to 0.1 m, unless Shift is held).
void draw_stage(CutsceneEdit& e, float side) {
    remod::Cutscene& c = e.doc;
    double reach = 3;
    for (const auto& a : c.actors)
        if (!a.at_spot) reach = std::max({reach, std::abs(a.offset[0]) + 1, std::abs(a.offset[2]) + 1});
    const ImVec2 at = ImGui::GetCursorScreenPos(), mid(at.x + side / 2, at.y + side / 2);
    const float k = side / float(2 * reach);  // pixels per metre
    ImDrawList* d = ImGui::GetWindowDrawList();
    d->AddRectFilled(at, ImVec2(at.x + side, at.y + side), IM_COL32(36, 40, 46, 255));
    for (int m = -int(reach); m <= int(reach); ++m) {  // a metre grid
        const ImU32 col = m == 0 ? IM_COL32(80, 86, 96, 255) : IM_COL32(52, 56, 64, 255);
        d->AddLine(ImVec2(mid.x + m * k, at.y), ImVec2(mid.x + m * k, at.y + side), col);
        d->AddLine(ImVec2(at.x, mid.y + m * k), ImVec2(at.x + side, mid.y + m * k), col);
    }
    const float r = ImGui::GetFontSize() * 0.45f;
    const ImU32 leon = IM_COL32(110, 200, 120, 255);
    d->AddTriangleFilled(ImVec2(mid.x, mid.y - r * 1.4f), ImVec2(mid.x - r, mid.y + r), ImVec2(mid.x + r, mid.y + r), leon);
    d->AddText(ImVec2(mid.x + r * 1.2f, mid.y), leon, "Leon");
    d->AddText(ImVec2(at.x + 4, at.y + 2), IM_COL32(140, 146, 156, 255), "from above: Leon faces up");
    d->AddRect(at, ImVec2(at.x + side, at.y + side), ImGui::GetColorU32(ImGuiCol_Border));
    std::vector<std::string> at_spots;
    for (int i = 0; i < int(c.actors.size()); ++i) {
        auto& a = c.actors[i];
        if (a.at_spot) {
            at_spots.push_back(a.name);
            continue;
        }
        const ImVec2 p(mid.x + float(a.offset[0]) * k, mid.y - float(a.offset[2]) * k);
        ImGui::SetCursorScreenPos(ImVec2(p.x - r * 1.5f, p.y - r * 1.5f));
        ImGui::PushID(i);
        ImGui::InvisibleButton("##actor", ImVec2(r * 3, r * 3));
        ImGui::PopID();
        if (ImGui::IsItemActivated()) {
            e.actor = i;
            e.selected = {};
        }
        if (ImGui::IsItemActive()) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const double step = ImGui::GetIO().KeyShift ? 0.01 : 0.1;
            a.offset[0] = std::round((mouse.x - mid.x) / k / step) * step;
            a.offset[2] = std::round((mid.y - mouse.y) / k / step) * step;
        }
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetTooltip("%s (%s): %.2f m right, %.2f m forward of Leon. Drag to move.", a.name.c_str(),
                              a.puppet.c_str(), a.offset[0], a.offset[2]);
        const bool chosen = e.actor == i;
        const ImU32 col = chosen ? IM_COL32(110, 170, 255, 255) : IM_COL32(240, 150, 70, 255);
        d->AddCircleFilled(p, r, col);
        if (const float len = std::hypot(mid.x - p.x, mid.y - p.y); len > r)  // it faces Leon
            d->AddLine(p, ImVec2(p.x + (mid.x - p.x) / len * r * 1.8f, p.y + (mid.y - p.y) / len * r * 1.8f), col, 2);
        d->AddText(ImVec2(p.x + r * 1.2f, p.y - r), col, a.name.c_str());
    }
    ImGui::SetCursorScreenPos(at);
    ImGui::Dummy(ImVec2(side, side));
    if (!at_spots.empty()) {
        std::string list;
        for (const auto& n : at_spots) list += (list.empty() ? "" : ", ") + n;
        ImGui::TextDisabled("At a written-down spot: %s", list.c_str());
    }
}

// The timeline: a lane per kind (and per character for animations), items you click, drag and stretch, the playhead.
void draw_timeline(CutsceneEdit& e) {
    remod::Cutscene& c = e.doc;
    const auto lanes = remod::cutscene_lanes(c);
    const float font = ImGui::GetFontSize(), lane_h = font * 1.6f, ruler_h = font * 1.4f, label_w = font * 7;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, x0 = at.x + label_w, span = std::max(w - label_w - font * 0.5f, font);
    const double length = std::max(c.length, 0.05);
    const auto x_of = [&](double t) { return x0 + float(std::clamp(t / length, 0.0, 1.0)) * span; };
    const auto t_of = [&](float x) { return std::clamp(double(x - x0) / span * length, 0.0, length); };
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled), text = ImGui::GetColorU32(ImGuiCol_Text);
    const ImGuiIO& io = ImGui::GetIO();

    // The seconds ruler: click or drag it to move the playhead.
    ImGui::SetCursorScreenPos(ImVec2(x0, at.y));
    ImGui::InvisibleButton("##ruler", ImVec2(span, ruler_h));
    if (ImGui::IsItemActive()) {
        e.playhead = t_of(io.MousePos.x);
        e.playing = false;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click or drag to move the playhead.");
    d->AddRectFilled(ImVec2(x0, at.y), ImVec2(x0 + span, at.y + ruler_h), IM_COL32(40, 44, 52, 255));
    const int every = std::max(1, int(length / 12));
    for (int sec = 0; sec <= int(length); ++sec) {
        const float x = x_of(sec);
        d->AddLine(ImVec2(x, at.y + ruler_h * 0.6f), ImVec2(x, at.y + ruler_h), dim);
        if (sec % every == 0) d->AddText(ImVec2(x + 2, at.y), dim, (std::to_string(sec) + " s").c_str());
    }

    for (int li = 0; li < int(lanes.size()); ++li) {
        const auto& lane = lanes[li];
        const float y = at.y + ruler_h + lane_h * float(li);
        ImGui::PushID(li);
        // The label: an actor's selects that actor.
        ImGui::SetCursorScreenPos(ImVec2(at.x, y));
        ImGui::InvisibleButton("##label", ImVec2(label_w - font * 0.3f, lane_h));
        const int actor = lane.lane == remod::CutsceneLane::Motion && lane.actor != "player"
                              ? int(std::ranges::find(c.actors, lane.actor, &remod::CutsceneActor::name) - c.actors.begin())
                              : -1;
        if (ImGui::IsItemClicked() && actor >= 0) {
            e.actor = actor;
            e.selected = {};
        }
        if (ImGui::IsItemHovered() && actor >= 0) ImGui::SetTooltip("%s: click to edit this actor.", lane.title.c_str());
        d->AddText(ImVec2(at.x, y + (lane_h - font) / 2), actor >= 0 && actor == e.actor ? IM_COL32(110, 170, 255, 255) : text,
                   lane.title.c_str());
        if (li % 2 == 0) d->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + span, y + lane_h), IM_COL32(255, 255, 255, 8));
        d->AddLine(ImVec2(x0, y + lane_h), ImVec2(x0 + span, y + lane_h), ImGui::GetColorU32(ImGuiCol_Separator));

        // The lane: click an item to select it, drag it to move it, drag a span's ends to stretch it.
        ImGui::SetCursorScreenPos(ImVec2(x0, y));
        ImGui::InvisibleButton("##lane", ImVec2(span, lane_h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool hovered = ImGui::IsItemHovered();
        const float mx = io.MousePos.x;
        // What's under the mouse: the topmost item (the last drawn); a span's ends first (within 5 px).
        int hit = -1, part = 0;
        for (int i = int(lane.items.size()) - 1; i >= 0 && hovered; --i) {
            const auto& item = lane.items[i];
            const float a = x_of(item.t);
            if (item.until) {
                const float b = x_of(*item.until);
                if (std::abs(mx - a) <= 5) part = 2;
                else if (std::abs(mx - b) <= 5) part = 3;
                else if (mx > a && mx < b) part = 1;
            } else if (std::abs(mx - a) <= font * 0.5f) {
                part = 1;
            }
            if (part) {
                hit = i;
                break;
            }
        }
        if (hovered && (part == 2 || part == 3)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (hit >= 0) {
                const auto& item = lane.items[hit];
                e.selected = item.ref;
                e.actor = -1;
                e.drag = part;
                e.drag_x = mx;
                e.drag_t = item.t;
                e.drag_until = item.until.value_or(item.t);
            } else {  // empty: the playhead goes there
                e.selected = {};
                e.playhead = t_of(mx);
                e.playing = false;
            }
        }
        if (ImGui::IsItemActive() && e.drag && e.selected && e.selected.lane == lane.lane) {
            const double dt = double(mx - e.drag_x) / span * length;
            const bool spans = e.drag_until > e.drag_t;
            if (e.drag == 1 && spans) {  // a span moved keeps its length
                const double t = std::clamp(snap(e.drag_t + dt), 0.0, length - (e.drag_until - e.drag_t));
                remod::set_item_time(c, e.selected, t, t + (e.drag_until - e.drag_t));
            } else if (e.drag == 1) {
                remod::set_item_time(c, e.selected, snap(e.drag_t + dt));
            } else if (e.drag == 2) {
                remod::set_item_time(c, e.selected, snap(e.drag_t + dt), e.drag_until);
            } else {
                remod::set_item_time(c, e.selected, e.drag_t, snap(e.drag_until + dt));
            }
        }
        if (ImGui::IsItemDeactivated()) e.drag = 0;
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            if (hit >= 0) {
                e.selected = lane.items[hit].ref;
                e.actor = -1;
            }
            e.menu_lane = li;
            e.menu_t = snap(t_of(mx));
            ImGui::OpenPopup("lane_menu");
        }
        if (hovered && hit >= 0 && !ImGui::IsItemActive()) {
            const auto& item = lane.items[hit];
            if (item.until)
                ImGui::SetTooltip("%s %s\n%.2f to %.2f s\nDrag to move, drag an end to stretch. Right-click for more.",
                                  lane_noun(lane.lane), item.label.c_str(), item.t, *item.until);
            else
                ImGui::SetTooltip("%s %s\nat %.2f s\nDrag to move. Right-click for more.", lane_noun(lane.lane),
                                  item.label.c_str(), item.t);
        }
        if (ImGui::BeginPopup("lane_menu")) {
            const auto& menu = lanes[std::clamp(e.menu_lane, 0, int(lanes.size()) - 1)];
            if (e.selected && e.selected.lane == menu.lane && ImGui::MenuItem((std::string("Remove this ") + lane_noun(menu.lane)).c_str())) {
                remod::remove_item(c, e.selected);
                e.selected = {};
            }
            if (menu.lane == remod::CutsceneLane::Camera) {
                ImGui::TextDisabled("Camera keys are recorded in game (F10 at each shot),\nthen Use recording (Cutscene item, "
                                    "nothing selected).");
            } else {
                char label[64];
                std::snprintf(label, sizeof label, "Add a %s here (%.2f s)", lane_noun(menu.lane), e.menu_t);
                if (ImGui::MenuItem(label)) {
                    e.selected = remod::add_item(c, menu.lane, menu.actor, e.menu_t);
                    e.actor = -1;
                }
            }
            if (ImGui::MenuItem("Move the playhead here")) e.playhead = e.menu_t;
            ImGui::EndPopup();
        }

        for (const auto& item : lane.items) {
            const bool chosen = item.ref == e.selected;
            const ImU32 col = lane_colour(lane.lane);
            const float a = x_of(item.t), top = y + lane_h * 0.15f, bottom = y + lane_h * 0.85f;
            if (item.until) {
                const float b = std::max(x_of(*item.until), a + 3);
                d->AddRectFilled(ImVec2(a, top), ImVec2(b, bottom), col, 3);
                d->PushClipRect(ImVec2(a, top), ImVec2(b, bottom), true);
                d->AddText(ImVec2(a + 4, y + (lane_h - font) / 2), IM_COL32_WHITE, item.label.c_str());
                d->PopClipRect();
                if (chosen) d->AddRect(ImVec2(a - 1, top - 1), ImVec2(b + 1, bottom + 1), IM_COL32_WHITE, 3.0f, ImDrawFlags_None, 2.0f);
            } else {
                const float h = (bottom - top) / 2, cy = (top + bottom) / 2;
                if (lane.lane == remod::CutsceneLane::Camera) {  // a key: a diamond
                    d->AddQuadFilled(ImVec2(a, top), ImVec2(a + h, cy), ImVec2(a, bottom), ImVec2(a - h, cy), col);
                } else if (lane.lane == remod::CutsceneLane::Motion) {  // an animation starts: a triangle
                    d->AddTriangleFilled(ImVec2(a, top), ImVec2(a + h * 1.3f, cy), ImVec2(a, bottom), col);
                } else {
                    d->AddCircleFilled(ImVec2(a, cy), h * 0.8f, col);
                }
                d->AddText(ImVec2(a + h * 1.4f, y + (lane_h - font) / 2), dim, item.label.c_str());
                if (chosen) d->AddRect(ImVec2(a - h - 2, top - 2), ImVec2(a + h * 1.3f + 2, bottom + 2), IM_COL32_WHITE, 2.0f, ImDrawFlags_None, 2.0f);
            }
        }
        ImGui::PopID();
    }
    // The playhead, over every lane.
    const float px = x_of(e.playhead), bottom = at.y + ruler_h + lane_h * float(lanes.size());
    d->AddLine(ImVec2(px, at.y), ImVec2(px, bottom), IM_COL32(255, 80, 80, 255), 2);
    d->AddTriangleFilled(ImVec2(px - 5, at.y), ImVec2(px + 5, at.y), ImVec2(px, at.y + 7), IM_COL32(255, 80, 80, 255));
    ImGui::SetCursorScreenPos(ImVec2(at.x, bottom + font * 0.3f));
    ImGui::Dummy(ImVec2(w, 0));
}

// The selected item's settings.
void draw_item(CutsceneEdit& e) {
    remod::Cutscene& c = e.doc;
    ImGui::PushItemWidth(-ImGui::GetFontSize() * 6);
    double t = 0;
    switch (e.selected.lane) {
    case remod::CutsceneLane::Camera: {
        auto& k = c.camera[e.selected.index];
        ImGui::Text("Camera key %d", e.selected.index + 1);
        t = k.t;
        if (edit_double("at (s)", t, 0.05, 0, c.length)) remod::set_item_time(c, e.selected, t);
        auto& key = c.camera[e.selected.index];  // re-sorted: it may have moved
        pick("arrives", key.ease, {"", "linear", "cut"}, "smooth");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How the camera gets to this key from the one before: smooth (eases in and out), linear, or "
                              "cut (stays on the key before, then jumps).");
        bool fov = key.fov > 0;
        if (ImGui::Checkbox("Field of view", &fov)) key.fov = fov ? 70 : 0;
        if (fov) edit_double("degrees", key.fov, 0.2, 5, 170, "%.1f");
        ImGui::TextDisabled("Position %.2f, %.2f, %.2f", key.position[0], key.position[1], key.position[2]);
        ImGui::TextDisabled("Rotation %.3f, %.3f, %.3f, %.3f", key.rotation[0], key.rotation[1], key.rotation[2], key.rotation[3]);
        ImGui::TextWrapped("Where the camera is comes from recording in game (F10), never typed.");
        break;
    }
    case remod::CutsceneLane::Fade: {
        auto& f = c.fades[e.selected.index];
        ImGui::Text("Fade %d", e.selected.index + 1);
        double from = f.t, until = f.until;
        if (edit_double("from (s)", from, 0.05, 0, c.length) | edit_double("to (s)", until, 0.05, 0, c.length))
            remod::set_item_time(c, e.selected, from, until);
        edit_double("black at start", f.from, 0.01, 0, 1);
        edit_double("black at end", f.to, 0.01, 0, 1);
        ImGui::TextDisabled("0 is clear, 1 is black.");
        break;
    }
    case remod::CutsceneLane::Subtitle: {
        auto& s = c.subtitles[e.selected.index];
        ImGui::Text("Subtitle %d", e.selected.index + 1);
        double from = s.t, until = s.until;
        if (edit_double("from (s)", from, 0.05, 0, c.length) | edit_double("to (s)", until, 0.05, 0, c.length))
            remod::set_item_time(c, e.selected, from, until);
        ImGui::InputTextMultiline("##text", &s.text, ImVec2(-1, ImGui::GetFontSize() * 4));
        break;
    }
    case remod::CutsceneLane::Motion: {
        auto& m = c.motions[e.selected.index];
        ImGui::Text("Animation %d", e.selected.index + 1);
        t = m.t;
        if (edit_double("at (s)", t, 0.05, 0, c.length)) remod::set_item_time(c, e.selected, t);
        std::vector<std::string> who{"player"};
        for (const auto& a : c.actors) who.push_back(a.name);
        pick("who", m.actor, who);
        edit_int("bank", m.bank);
        edit_int("motion", m.motion);
        edit_double("from frame", m.frame, 1, 0, 100000, "%.0f");
        edit_double("blend (frames)", m.blend, 1, 0, 120, "%.0f");
        ImGui::TextWrapped("Find animations in game: REFramework's menu > remod cutscenes > Animations; Use in a cutscene, "
                           "then Add picked animation here.");
        break;
    }
    case remod::CutsceneLane::Movie:
    case remod::CutsceneLane::Sound: {
        const bool movie = e.selected.lane == remod::CutsceneLane::Movie;
        auto& q = movie ? c.movies[e.selected.index] : c.sounds[e.selected.index];
        ImGui::Text("%s %d", movie ? "Movie" : "Sound", e.selected.index + 1);
        t = q.t;
        if (edit_double("at (s)", t, 0.05, 0, c.length)) remod::set_item_time(c, e.selected, t);
        ImGui::InputText("id", &q.id);
        ImGui::TextWrapped("%s", movie ? "The game's movie id (e.g. mva000) or a New movie block's name. The timeline "
                                         "waits while it plays."
                                       : "A New sound block's name.");
        break;
    }
    }
    ImGui::PopItemWidth();
    if (ImGui::Button("Remove")) {
        remod::remove_item(c, e.selected);
        e.selected = {};
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(or Delete)");
}

void draw_actor(CutsceneEdit& e, const fs::path& game, std::string& status) {
    remod::Cutscene& c = e.doc;
    auto& a = c.actors[e.actor];
    ImGui::Text("Actor: %s", a.name.c_str());
    ImGui::PushItemWidth(-ImGui::GetFontSize() * 6);
    const std::string old = a.name;
    if (ImGui::InputText("name", &a.name, ImGuiInputTextFlags_CharsNoBlank)) {
        for (auto& m : c.motions)  // its animations and animation files follow the new name
            if (m.actor == old) m.actor = a.name;
        for (auto& f : c.animation_files)
            if (f.actor == old) f.actor = a.name;
    }
    pick("puppet", a.puppet, e.puppets);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Puppets: remod's (luis, ashley) and those in the game's reframework\\data\\remod_puppets (a new "
                          "character's mod).");
    int where = a.at_spot ? 1 : 0;
    ImGui::RadioButton("Near Leon", &where, 0);
    ImGui::SameLine();
    ImGui::RadioButton("At a spot", &where, 1);
    a.at_spot = where == 1;
    if (a.at_spot) {
        ImGui::TextDisabled("%.2f, %.2f, %.2f", a.position[0], a.position[1], a.position[2]);
        if (ImGui::Button("Use Leon's spot")) {
            if (const auto spot = remod::read_spot(game)) {
                a.position = spot->position;
                a.rotation = spot->rotation;
            } else {
                status = "No spot written down: in game, REFramework's menu > remod cutscenes > Write down Leon's spot "
                         "(Game folder set?).";
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Where Leon stood, facing his way, when you clicked Write down Leon's spot in game.");
    } else {
        edit_double("right (m)", a.offset[0], 0.05, -50, 50);
        edit_double("up (m)", a.offset[1], 0.05, -50, 50);
        edit_double("forward (m)", a.offset[2], 0.05, -50, 50);
        ImGui::TextDisabled("From Leon when it starts, facing him. Or drag it on the stage.");
    }
    ImGui::Checkbox("Hides the partner", &a.hides_partner);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The real Ashley (or partner) is hidden while it plays.");
    ImGui::PopItemWidth();
    if (ImGui::Button("Remove this actor")) {
        remove_actor(c, e.actor);
        e.actor = -1;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Its animations go too.");
}

// Nothing selected: the cutscene's own settings.
void draw_whole(CutsceneEdit& e, const fs::path& game, std::string& status) {
    remod::Cutscene& c = e.doc;
    ImGui::Text("The cutscene");
    ImGui::PushItemWidth(-ImGui::GetFontSize() * 6);
    ImGui::InputText("name", &c.name);
    edit_double("length (s)", c.length, 0.05, 0.1, 3600);
    pick("start key", c.start_key, {"", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F11", "F12"});
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The key that plays it in game (F10 records camera keys).");
    edit_double("letterbox", c.letterbox, 0.005, 0, 0.45);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Each black bar's share of the screen's height.");
    ImGui::PopItemWidth();
    ImGui::Separator();
    ImGui::Text("Camera: %d key(s)", int(c.camera.size()));
    if (ImGui::Button("Use recording")) {
        try {
            remod::apply_recording(c, game_cutscenes_dir(game) / "recording.json");
            status = "The recorded camera keys are in the cutscene (Save to keep them).";
        } catch (const std::exception& ex) {
            status = std::string("Couldn't use the recording: ") + ex.what();
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Replace the camera keys with the ones you recorded in game (F10 at each shot).");
    ImGui::Text("Starts by itself: %s", c.trigger ? "yes (a trigger)" : "no");
    if (ImGui::Button("Use trigger")) {
        try {
            remod::apply_trigger(c, game_cutscenes_dir(game) / "trigger.json");
            status = "The trigger is in (Save to keep it).";
        } catch (const std::exception& ex) {
            status = std::string("Couldn't use the trigger: ") + ex.what();
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Start it by itself, by the trigger you made in game (REFramework's menu > remod cutscenes: a spot, "
                          "talking to a character, story flags, after the game's own cutscene).");
    ImGui::Separator();
    ImGui::Text("Actors (other characters)");
    for (int i = 0; i < int(c.actors.size()); ++i) {
        ImGui::PushID(i);
        if (ImGui::Selectable((c.actors[i].name + "  (" + c.actors[i].puppet + ")").c_str(), false)) {
            e.actor = i;
            e.selected = {};
        }
        ImGui::PopID();
    }
    if (ImGui::Button("Add actor")) add_actor(e);
    ImGui::Separator();
    // Animation files: a game cutscene's own animations, loaded onto a character as a bank of our number.
    ImGui::Text("Animation files");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Animations from the game's own cutscenes (_Chainsaw/Event/cs/<id>/.../chara/<mesh>/<mesh>.motlist),\n"
                          "put on a character as a bank of your own number when the cutscene plays. Its animations then\n"
                          "use that bank. They move the character as in the game's cutscene (through walls too).");
    std::vector<std::string> who{"player"};
    for (const auto& a : c.actors) who.push_back(a.name);
    int erase = -1;
    for (int i = 0; i < int(c.animation_files.size()); ++i) {
        auto& f = c.animation_files[i];
        ImGui::PushID(1000 + i);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        pick("##who", f.actor, who);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        edit_int("##bank", f.bank);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Its bank number: your own, e.g. 9000 (not one the character has).");
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) erase = i;
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##file", &f.file);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.file.empty() ? "A .motlist's game path" : f.file.c_str());
        ImGui::PopID();
    }
    if (erase >= 0) c.animation_files.erase(c.animation_files.begin() + erase);
    if (ImGui::Button("Add animation file")) {
        long long bank = 9000;
        while (std::ranges::any_of(c.animation_files, [&](const auto& f) { return f.bank == bank; })) ++bank;
        c.animation_files.push_back({.actor = "player", .bank = bank});
    }
    ImGui::Separator();
    ImGui::TextWrapped("Click something on the timeline or the stage to edit it. Right-click a lane to add to it.");
}

// Every item in tables, for typing exact numbers.
void draw_tables(CutsceneEdit& e) {
    remod::Cutscene& c = e.doc;
    std::vector<std::string> who{"player"};
    for (const auto& a : c.actors) who.push_back(a.name);
    const auto row_time = [&](remod::CutsceneLane lane, int i, double t) {
        remod::CutsceneItemRef r{lane, i};
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
        if (edit_double("##t", t, 0.05, 0, c.length)) remod::set_item_time(c, r, t);
    };
    if (ImGui::TreeNode("Animations")) {
        int erase = -1;
        if (ImGui::BeginTable("motions", 7, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
            for (const char* h : {"at (s)", "who", "bank", "motion", "from frame", "blend (frames)", ""}) ImGui::TableSetupColumn(h);
            ImGui::TableHeadersRow();
            for (int i = 0; i < int(c.motions.size()); ++i) {
                auto& m = c.motions[i];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                row_time(remod::CutsceneLane::Motion, i, m.t);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                pick("##actor", m.actor, who);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                edit_int("##bank", m.bank);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                edit_int("##motion", m.motion);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                edit_double("##frame", m.frame, 1, 0, 100000, "%.0f");
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                edit_double("##blend", m.blend, 1, 0, 120, "%.0f");
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Remove")) erase = i;
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        erase_row(c.motions, erase);
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Subtitles")) {
        int erase = -1;
        for (int i = 0; i < int(c.subtitles.size()); ++i) {
            auto& s = c.subtitles[i];
            ImGui::PushID(i);
            double from = s.t, until = s.until;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
            bool moved = edit_double("from##t", from, 0.05, 0, c.length);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
            moved |= edit_double("to##u", until, 0.05, 0, c.length);
            if (moved) {
                remod::CutsceneItemRef r{remod::CutsceneLane::Subtitle, i};
                remod::set_item_time(c, r, from, until);
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 5);
            ImGui::InputText("##text", &s.text);
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) erase = i;
            ImGui::PopID();
        }
        erase_row(c.subtitles, erase);
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Fades to black")) {
        int erase = -1;
        for (int i = 0; i < int(c.fades.size()); ++i) {
            auto& f = c.fades[i];
            ImGui::PushID(i);
            double from = f.t, until = f.until;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
            bool moved = edit_double("from (s)##t", from, 0.05, 0, c.length);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
            moved |= edit_double("to (s)##u", until, 0.05, 0, c.length);
            if (moved) {
                remod::CutsceneItemRef r{remod::CutsceneLane::Fade, i};
                remod::set_item_time(c, r, from, until);
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
            edit_double("black from##f", f.from, 0.01, 0, 1);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
            edit_double("to##o", f.to, 0.01, 0, 1);
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) erase = i;
            ImGui::PopID();
        }
        erase_row(c.fades, erase);
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Movies and sounds")) {
        for (auto [label, lane, cues] : {std::tuple{"movie", remod::CutsceneLane::Movie, &c.movies},
                                         std::tuple{"sound", remod::CutsceneLane::Sound, &c.sounds}}) {
            int erase = -1;
            ImGui::PushID(label);
            for (int i = 0; i < int(cues->size()); ++i) {
                auto& q = (*cues)[i];
                ImGui::PushID(i);
                row_time(lane, i, q.t);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
                ImGui::InputText(label, &q.id);
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) erase = i;
                ImGui::PopID();
            }
            erase_row(*cues, erase);
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Camera keys")) {
        int erase = -1;
        for (int i = 0; i < int(c.camera.size()); ++i) {
            ImGui::PushID(i);
            row_time(remod::CutsceneLane::Camera, i, c.camera[i].t);
            if (i < int(c.camera.size())) {
                auto& k = c.camera[i];
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
                pick("##ease", k.ease, {"", "linear", "cut"}, "smooth");
                ImGui::SameLine();
                ImGui::TextDisabled("at %.2f, %.2f, %.2f%s", k.position[0], k.position[1], k.position[2],
                                    k.fov > 0 ? (", fov " + std::to_string(int(k.fov))).c_str() : "");
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) erase = i;
            }
            ImGui::PopID();
        }
        erase_row(c.camera, erase);
        ImGui::TreePop();
    }
    keep_selection_valid(e);
}

}  // namespace

fs::path game_cutscenes_dir(const fs::path& game_dir) {
    return game_dir.empty() ? fs::path() : game_dir / "reframework" / "data" / "remod_cutscenes";
}

std::optional<CutsceneEdit> open_cutscene(const fs::path& file, const fs::path& game_dir, std::string& status) {
    try {
        if (file.empty()) throw remod::PackageError("type the cutscene's file name first, e.g. cutscenes\\door.json");
        CutsceneEdit e{.file = file};
        std::error_code ec;
        const bool exists = fs::is_regular_file(file, ec);
        e.doc = exists ? remod::read_cutscene(file) : remod::new_cutscene(file);
        e.saved = exists ? e.doc : remod::Cutscene{};  // a new file: unsaved until written
        e.history.reset(e.doc);
        e.puppets = remod::puppet_names(game_dir);
        const fs::path dir = game_cutscenes_dir(game_dir);
        e.game_copy = !dir.empty() && fs::equivalent(dir, file.parent_path(), ec);
        return e;
    } catch (const std::exception& ex) {
        status = std::string("Couldn't open the cutscene: ") + ex.what();
        return std::nullopt;
    }
}

constexpr const char* kUntitled = "new_cutscene";

CutsceneEdit untitled_cutscene(const fs::path& game_dir) {
    CutsceneEdit e;
    e.doc = e.saved = remod::new_cutscene(kUntitled);
    e.history.reset(e.doc);
    e.puppets = remod::puppet_names(game_dir);
    return e;
}

void set_cutscene_file(CutsceneEdit& e, const fs::path& file, const fs::path& game_dir) {
    if (e.doc.name == kUntitled) e.doc.name = file.stem().string();
    e.file = file;
    std::error_code ec;
    const fs::path dir = game_cutscenes_dir(game_dir);
    e.game_copy = !dir.empty() && fs::equivalent(dir, file.parent_path(), ec);
}

bool save_cutscene(CutsceneEdit& e, std::string& status) {
    try {
        if (e.file.empty()) throw remod::PackageError("it has no file yet: Save asks where");
        remod::write_cutscene(e.file, e.doc);
        e.saved = e.doc;
        status = "Saved " + e.file.filename().string() +
                 (e.game_copy ? ". In game: Reload in REFramework's menu > remod cutscenes."
                              : ". Test in game copies it into the game.");
        return true;
    } catch (const std::exception& ex) {
        status = std::string("Couldn't save the cutscene: ") + ex.what();
        return false;
    }
}

void cutscene_undo(CutsceneEdit& e, bool redo) {
    if (redo ? e.history.redo(e.doc) : e.history.undo(e.doc)) keep_selection_valid(e);
}

bool cutscene_unsaved(const CutsceneEdit& e) { return e.doc != e.saved; }

CutsceneAction draw_cutscene_layout(CutsceneEdit& e, const fs::path& game, bool in_graph, std::string& status) {
    CutsceneAction action = CutsceneAction::None;
    remod::Cutscene& c = e.doc;
    const float font = ImGui::GetFontSize();
    const bool unsaved = cutscene_unsaved(e);
    const ImGuiIO& io = ImGui::GetIO();
    if (e.playing) {
        e.playhead += io.DeltaTime;
        if (e.playhead >= c.length) {
            e.playhead = c.length;
            e.playing = false;
        }
    }

    ImGui::Begin("Cutscene");
    // The top bar (the layouts are switched in the app's own bar above every layout).
    ImGui::AlignTextToFramePadding();
    const bool untitled = e.file.empty();
    // Asks the caller where to save it, then does `then`.
    const auto save_as = [&](CutsceneAction then) {
        e.after_save = then;
        return CutsceneAction::SaveAs;
    };
    ImGui::TextUnformatted(((untitled ? "New cutscene (not saved yet)" : e.file.filename().string()) +
                            (unsaved ? " *" : "")).c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", untitled ? "Save asks where to keep it." : e.file.string().c_str());
    if (e.game_copy) {
        ImGui::SameLine();
        ImGui::TextColored(kAmber, "(the game's copy)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("This file is in the game's remod_cutscenes folder: a Test in game from its source file "
                              "replaces it. Edit the source to keep your changes.");
    }
    ImGui::SameLine();
    if (ImGui::Button(unsaved ? "Save*" : "Save")) {
        if (untitled) action = save_as(CutsceneAction::None);
        else save_cutscene(e, status);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(untitled);
    if (ImGui::Button("Reload")) {
        if (auto again = open_cutscene(e.file, game, status)) {
            again->playhead = e.playhead;
            e = std::move(*again);
            keep_selection_valid(e);
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Read the file again, dropping changes not saved.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!e.history.can_undo());
    if (ImGui::Button("Undo")) cutscene_undo(e, false);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!e.history.can_redo());
    if (ImGui::Button("Redo")) cutscene_undo(e, true);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(e.game_copy || game.empty());
    if (ImGui::Button("Test in game")) {
        if (untitled) action = save_as(CutsceneAction::TestInGame);
        else if (!unsaved || save_cutscene(e, status)) action = CutsceneAction::TestInGame;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", game.empty() ? "Set the Game folder in the Pipeline panel first."
                                : e.game_copy ? "This is the game's copy already: Save, then Reload in the game's menu."
                                              : "Save, then copy remod's cutscene runtime, this cutscene and its puppets into "
                                                "the game. In game: Reset Scripts, then its start key.");
    ImGui::SameLine();
    if (ImGui::Button("Remove from game")) action = CutsceneAction::RemoveFromGame;
    ImGui::EndDisabled();
    if (!in_graph) {
        ImGui::SameLine();
        if (ImGui::Button("Add to graph"))
            action = untitled ? save_as(CutsceneAction::AddToGraph) : CutsceneAction::AddToGraph;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A Cutscene block holding this file, linked into the graph's Package block, so it goes into "
                              "your mod.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) e.confirm_close = true;

    if (e.confirm_close) {
        if (!unsaved) {
            ImGui::End();
            return CutsceneAction::Closed;
        }
        ImGui::TextColored(kAmber, "Unsaved changes.");
        ImGui::SameLine();
        if (ImGui::Button("Save and close")) {
            if (untitled) {
                ImGui::End();
                return save_as(CutsceneAction::Closed);
            }
            if (save_cutscene(e, status)) {
                ImGui::End();
                return CutsceneAction::Closed;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Close without saving")) {
            ImGui::End();
            return CutsceneAction::Closed;
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep editing")) e.confirm_close = false;
    }
    if (c != e.checked) {
        e.problems = remod::check_cutscene(remod::cutscene_text(c));
        e.checked = c;
    }
    for (const auto& p : e.problems) ImGui::TextColored(kRed, "%s", p.c_str());
    if (!status.empty()) ImGui::TextDisabled("%s", status.c_str());  // the app's status line (its Pipeline panel is hidden)

    // The preview and the stage, side by side.
    const remod::CutsceneFrame frame = remod::cutscene_frame(c, e.playhead);
    const float avail = ImGui::GetContentRegionAvail().x;
    float h = std::min((avail - font) * 0.62f * 9 / 16, font * 16);
    h = std::max(h, font * 6);
    draw_preview(e, frame, ImVec2(h * 16 / 9, h));
    ImGui::SameLine();
    draw_stage(e, h);

    // What's happening at the playhead, and the play controls.
    if (ImGui::Button(e.playing ? "Pause" : "Play")) {
        if (!e.playing && e.playhead >= c.length) e.playhead = 0;
        e.playing = !e.playing;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Space plays and pauses too (with this window focused).");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(font * 6);
    edit_double("##playhead", e.playhead, 0.02, 0, c.length, "%.2f s");
    ImGui::SameLine();
    std::string now;
    if (frame.camera_from < 0) now = "Camera: the game's (no keys).";
    else if (frame.camera_from == frame.camera_to) now = "Camera: on key " + std::to_string(frame.camera_from + 1) + ".";
    else {
        const std::string& ease = c.camera[frame.camera_to].ease;
        now = "Camera: key " + std::to_string(frame.camera_from + 1) + " to " + std::to_string(frame.camera_to + 1) + " (" +
              (ease.empty() ? "smooth" : ease) + ", " + std::to_string(int(frame.camera_progress * 100)) + "%).";
    }
    for (const auto& p : frame.playing) {
        now += "  " + std::string(p.actor == "player" ? "Leon" : p.actor) + ": ";
        if (p.motion < 0) {
            now += "as he was.";
        } else {
            char buf[96];
            std::snprintf(buf, sizeof buf, "%lld / %lld for %.1f s.", c.motions[p.motion].bank, c.motions[p.motion].motion, p.since);
            now += buf;
        }
    }
    ImGui::TextDisabled("%s", now.c_str());

    // Adding from what the game wrote down.
    const auto picked = remod::read_picked_animation(game);
    ImGui::BeginDisabled(!picked);
    if (ImGui::Button("Add picked animation") && picked) {
        // On the actor playing that puppet (one made for it if none), or the player, at the playhead.
        std::string who = picked->actor;
        if (who != "player") {
            const auto a = std::ranges::find(c.actors, who, &remod::CutsceneActor::puppet);
            if (a != c.actors.end()) who = a->name;
            else c.actors.push_back({.name = who, .puppet = who});
        }
        c.motions.push_back({.t = e.playhead, .actor = who, .bank = picked->bank, .motion = picked->motion, .frame = picked->frame});
        // Picked from an animation file the previewer added: the cutscene loads it too.
        if (!picked->file.empty() && std::ranges::none_of(c.animation_files, [&](const auto& f) {
                return f.actor == who && f.bank == picked->bank;
            }))
            c.animation_files.push_back({.actor = who, .file = picked->file, .bank = picked->bank});
        e.selected = {remod::CutsceneLane::Motion, int(c.motions.size()) - 1};
        e.actor = -1;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", picked ? ("Bank " + std::to_string(picked->bank) + ", motion " + std::to_string(picked->motion) +
                                          " (" + picked->name + ") on " + picked->actor + ", at the playhead.").c_str()
                                       : "Pick one in game first: REFramework's menu > remod cutscenes > Animations, play "
                                         "one, Use in a cutscene.");
    ImGui::SameLine();
    if (ImGui::Button("Add actor")) add_actor(e);
    ImGui::SameLine();
    ImGui::TextDisabled("Right-click a lane to add a subtitle, fade, animation, movie or sound there.");

    draw_timeline(e);
    if (ImGui::CollapsingHeader("Every item (tables)")) draw_tables(e);

    // Keys while this window has focus and no text is being typed.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            if (!e.playing && e.playhead >= c.length) e.playhead = 0;
            e.playing = !e.playing;
        }
        if (e.selected && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            remod::remove_item(c, e.selected);
            e.selected = {};
        }
    }
    ImGui::End();

    ImGui::Begin("Cutscene item");
    keep_selection_valid(e);
    if (e.selected) draw_item(e);
    else if (e.actor >= 0) draw_actor(e, game, status);
    else draw_whole(e, game, status);
    if ((e.selected || e.actor >= 0) && ImGui::Button("The whole cutscene")) {
        e.selected = {};
        e.actor = -1;
    }
    ImGui::End();

    // One undo step per settled change (nothing being dragged or typed).
    if (!ImGui::IsAnyItemActive() && !e.drag) e.history.track(c);
    return action;
}
