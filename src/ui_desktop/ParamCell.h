#pragma once
// One selectable parameter cell for the LIVE screen's context block: a
// caption, a small value box, and the value text -- editable with the mouse and the arrow keys.
//
// Interaction (the same for every parameter, whatever it edits):
//   - click, or the mouse coming onto the cell (C3) -> selects the cell (the
//                        caller keeps the selection index; arrow LEFT/RIGHT move
//                        it, UP/DOWN change the selected cell's value -- see
//                        ContextPanel.cpp), so Up / Down change the cell under the mouse
//   - drag up / down  -> changes the value; reported as integer `steps`
//                        (pxPerStep pixels of drag per step) so patch params
//                        (AdjustPatchParam-style +1/-1) and float params
//                        (value += steps * stepSize) share one code path
//   - discrete cells  -> (norm < 0) a plain click cycles +1, right-click -1,
//                        for on/off, enum and list-like values
//   - C3 (UI-2): the same modifiers as every ValueField (Interaction.h): Shift while dragging = finer (x0.1),
//                Option(Alt)+click = `reset` (the caller puts the default back; that press does not drag), and the
//                gestures are named in the info line on hover.
// Continuous cells (norm in 0..1) draw a proportional fill like KnobAbsolute
// in KnobWidgets.h; that file's knobs stay for FxPanel.
#include "imgui.h"
#include "Visual.h"
#include "Interaction.h"
#include "InfoLine.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace t8::desktop {

struct ParamCellResult {
    bool clicked = false;     // left press landed on the cell (select it)
    bool activated = false;   // a drag gesture just started (push undo once here)
    int steps = 0;            // net +/- steps from drag, or +1/-1 from a click on a discrete cell
    bool reset = false;       // C3 (UI-2): Option(Alt)+click: put the default back (no steps this press)
    bool hoverStarted = false;   // C3: the mouse just came onto the cell (the caller selects it, so Up / Down change the cell under the mouse)
};

// UI-7: a caption or value wider than its cell ("KEY LOCK", "SWEEP TIME", "FORWARD") is drawn smaller to fit instead of being cut
// ("KEY LOCI", "FORWARI"); it never goes below kParamCellMinFont points, and only past that is it clipped.
inline constexpr float kParamCellMinFont = 10.0f;
inline void ParamCellCenteredText(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* text, ImU32 col) {
    float size = ImGui::GetFontSize();
    ImVec2 ts = ImGui::CalcTextSize(text);
    const float room = (p1.x - p0.x) - 2.0f;
    if (ts.x > room && ts.x > 0.0f) {
        const float fit = std::max(kParamCellMinFont, size * room / ts.x);
        if (fit < size) { ts.x *= fit / size; ts.y *= fit / size; size = fit; }
    }
    float x = p0.x + ((p1.x - p0.x) - ts.x) * 0.5f;
    if (x < p0.x) x = p0.x;
    dl->PushClipRect(p0, p1, true);
    dl->AddText(ImGui::GetFont(), size, ImVec2(x, p0.y + ((p1.y - p0.y) - ts.y) * 0.5f), col, text);
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

    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID kSkip = ImGui::GetID("skip");
    if (ImGui::IsItemActivated()) {
        res.clicked = true;
        if (interaction::IsReset()) { res.reset = true; st->SetInt(kSkip, 1); }   // Option+click: the default, and this press does not drag
        else { res.activated = true; st->SetInt(kSkip, 0); }
    }
    const bool skip = st->GetInt(kSkip, 0) != 0;
    {
        const ImGuiID kHov = ImGui::GetID("hov");
        res.hoverStarted = hovered && st->GetInt(kHov, 0) == 0;
        st->SetInt(kHov, hovered ? 1 : 0);
    }
    if (active && !skip) {
        *accumPx -= ImGui::GetIO().MouseDelta.y * (interaction::IsFine() ? interaction::kFineFactor : 1.0f);   // Shift = finer
        while (*accumPx >= pxPerStep) { res.steps++; *accumPx -= pxPerStep; }
        while (*accumPx <= -pxPerStep) { res.steps--; *accumPx += pxPerStep; }
    } else {
        *accumPx = 0.0f;
    }
    if (hovered || active) {
        HintGestures(discrete ? "Click: next   Right-click: back   Drag: scrub   Up / Down: change"
                              : "Drag up / down: change   Shift: finer   Option+click: default   Up / Down: change (Shift: bigger)");
    }
    if (discrete && !skip) {
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
