#pragma once
// A zoomable picture area, shared by the texture viewer and the pop-out preview windows.
#include <imgui.h>

struct ZoomPan {
    float zoom = 1;  // 1: fits the area
    ImVec2 pan;      // the picture's centre from the area's, in screen pixels
};

// Fills the rest of the window with an area showing a `width` x `height` picture: the wheel zooms about the mouse (up to
// 32 screen pixels per picture pixel), any button drags, a double-click fits it again. Returns where to draw the
// picture; the area is the last item, and the caller clips to it (`area_min`, `area_max`).
struct ZoomPlace {
    ImVec2 corner, size, area_min, area_max;
    float scale = 1;  // screen pixels per picture pixel
};
ZoomPlace zoom_area(const char* id, float width, float height, ZoomPan& view);
