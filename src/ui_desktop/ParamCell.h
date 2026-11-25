#pragma once
// One selectable parameter cell for the LIVE screen's context block: a
// caption, a small value box, and the value text -- editable with the mouse and the arrow keys.
//
// Interaction (the same for every parameter, whatever it edits):
//   - click           -> selects the cell (the caller keeps the selection
//                        index; arrow LEFT/RIGHT move it, UP/DOWN change the
//                        selected cell's value -- see ContextPanel.cpp)
//   - drag up / down  -> changes the value; reported as integer `steps`
//                        (pxPerStep pixels of drag per step) so patch params
//                        (AdjustPatchParam-style +1/-1) and float params
//                        (value += steps * stepSize) share one code path
//   - discrete cells  -> (norm < 0) a plain click cycles +1, right-click -1,
//                        for on/off, enum and list-like values
// Continuous cells (norm in 0..1) draw a proportional fill like KnobAbsolute
// in KnobWidgets.h; that file's knobs stay for FxPanel.
#include "imgui.h"
#include "Visual.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace t8::desktop {

struct ParamCellResult {
    bool clicked = false;     // left press landed on the cell (select it)
    bool activated = false;   // a drag gesture just started (push undo once here)
    int steps = 0;            // net +/- steps from drag, or +1/-1 from a click on a discrete cell
};

inline void ParamCellCenteredText(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* text, ImU32 col) {
    ImVec2 ts = ImGui::CalcTextSize(text);
    float x = p0.x + ((p1.x - p0.x) - ts.x) * 0.5f;
    if (x < p0.x) x = p0.x;
    dl->PushClipRect(p0, p1, true);
    dl->AddText(ImVec2(x, p0.y + ((p1.y - p0.y) - ts.y) * 0.5f), col, text);
    dl->PopClipRect();
}

inline ParamCellResult ParamCell(const char* id, const char* caption, const char* valueText,
                                 float norm, bool selected, float* accumPx, float pxPerStep,
                                 ImVec2 size, bool dim = false) {
    ParamCellResult res;
    ImGui::PushID(id);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##cell", size);
    ImVec2 p1(p0.x + size.x, p0.y + size.y);
    bool active = ImGui::IsItemActive();
    bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float lineH = ImGui::GetTextLineHeight();
    ImVec2 capMax(p1.x, p0.y + lineH + 2.0f);
    ImVec2 boxMin(p0.x, capMax.y + 1.0f);
    bool discrete = norm < 0.0f;
    ImVec2 boxMax(p1.x, discrete ? p1.y : p1.y - lineH - 3.0f);

    ParamCellCenteredText(dl, p0, capMax, caption,
                          selected ? vis::kAccentHi : (dim ? IM_COL32(95, 99, 112, 255) : vis::kTextDim));

    ImU32 bg = active ? IM_COL32(40, 36, 28, 255) : (hovered ? vis::kCell : vis::kCell);
    dl->AddRectFilled(boxMin, boxMax, bg, 4.0f);
    if (!discrete) {
        float n = std::clamp(norm, 0.0f, 1.0f);
        float h = (boxMax.y - boxMin.y) * n;
        dl->AddRectFilled(ImVec2(boxMin.x, boxMax.y - h), boxMax,
                          dim ? IM_COL32(120, 90, 50, 255) : (active ? vis::kAccentHi : vis::kAccent), 4.0f);
    }
    dl->AddRect(boxMin, boxMax, selected ? vis::kAccent : vis::kLineHi, 4.0f, 0,
                selected ? 2.0f : 1.0f);
    if (selected) dl->AddRect(ImVec2(boxMin.x - 2, boxMin.y - 2), ImVec2(boxMax.x + 2, boxMax.y + 2),
                              IM_COL32(242, 163, 61, 70), 5.0f, 0, 2.0f);

    if (discrete) {
        ParamCellCenteredText(dl, boxMin, boxMax, valueText, dim ? IM_COL32(120, 124, 138, 255) : vis::kText);
    } else {
        ParamCellCenteredText(dl, ImVec2(p0.x, boxMax.y + 2.0f), p1, valueText, IM_COL32(210, 212, 222, 255));
    }

    if (ImGui::IsItemActivated()) { res.clicked = true; res.activated = true; }
    if (active) {
        *accumPx -= ImGui::GetIO().MouseDelta.y;
        while (*accumPx >= pxPerStep) { res.steps++; *accumPx -= pxPerStep; }
        while (*accumPx <= -pxPerStep) { res.steps--; *accumPx += pxPerStep; }
    } else {
        *accumPx = 0.0f;
    }
    if (discrete) {
        // A click that never turned into a drag cycles forward; a right
        // click cycles back. (Drag on a discrete cell still steps, so an
        // enum can be scrubbed.)
        if (ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
            std::fabs(ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).y) < 3.0f && res.steps == 0) {
            res.steps = 1;
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { res.clicked = true; res.steps = -1; }
    }
    ImGui::PopID();
    return res;
}

} // namespace t8::desktop
