#include "zoom_view.hpp"

#include <algorithm>
#include <cmath>

ZoomPlace zoom_area(const char* id, float width, float height, ZoomPan& view) {
    const ImVec2 box(std::max(ImGui::GetContentRegionAvail().x, 1.0f), std::max(ImGui::GetContentRegionAvail().y, 1.0f));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, box,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);  // the wheel zooms instead of scrolling
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) view = {};
    const ImGuiIO& io = ImGui::GetIO();
    const float fit = std::min(box.x / width, box.y / height);
    const ImVec2 middle(at.x + box.x * 0.5f, at.y + box.y * 0.5f);
    if (ImGui::IsItemHovered() && io.MouseWheel != 0) {
        const float before = view.zoom;
        view.zoom = std::clamp(view.zoom * std::pow(1.25f, io.MouseWheel), 1.0f, std::max(1.0f, 32 / fit));
        const float k = 1 - view.zoom / before;  // keeps the pixel under the mouse where it is
        view.pan.x += (io.MousePos.x - middle.x - view.pan.x) * k;
        view.pan.y += (io.MousePos.y - middle.y - view.pan.y) * k;
    }
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0) ||
                                  ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0) ||
                                  ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0))) {
        view.pan.x += io.MouseDelta.x;
        view.pan.y += io.MouseDelta.y;
    }
    ZoomPlace place;
    place.scale = fit * view.zoom;
    place.size = ImVec2(width * place.scale, height * place.scale);
    place.corner = ImVec2(middle.x + view.pan.x - place.size.x * 0.5f, middle.y + view.pan.y - place.size.y * 0.5f);
    place.area_min = at;
    place.area_max = ImVec2(at.x + box.x, at.y + box.y);
    return place;
}
