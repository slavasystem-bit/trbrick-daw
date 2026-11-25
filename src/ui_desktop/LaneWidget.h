#pragma once
// v44.39: a wide curve editor for ONE parameter row (-100..100) of a StepParams -- used by the AUTO tab of the LIVE instrument block.
// The same gestures as the lanes under the bricks: click on empty space = add a point, drag a point, Shift = finer, Option + click =
// back to 0, double-click an inner point = remove it, Esc while dragging = cancel. The row is created by the first edit, so just
// looking at an instrument never adds anything to the scene. While the scene plays, a line and a dot show where the curve is now.
#include "BrickGlyphs.h"
#include "InfoLine.h"
#include "ParamText.h"
#include "Visual.h"
#include "app/AppState.h"
#include "core/ArrangeTypes.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>

namespace t8::desktop {

namespace lane_detail {
struct DragState {
    ImGuiID id = 0;
    int pt = -1;
    Lane before;
    float y0 = 0.0f, yOff = 0.0f;
    double lastAddT = -10.0;
};
inline ImU32 Alpha(ImU32 c, int a) { return (c & 0x00FFFFFFu) | (static_cast<ImU32>(std::clamp(a, 0, 255)) << 24); }
inline DragState& Drag() { static DragState d; return d; }
} // namespace lane_detail

// Returns true when the lane changed this frame. `playProg` < 0: nothing plays.
inline bool LaneEditor(AppState& app, const char* idStr, StepParams& sp, int track, int param, ImVec2 size, ImU32 col, double playProg) {
    using lane_detail::Drag;
    lane_detail::DragState& ds = Drag();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiID id = ImGui::GetID(idStr);
    const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + size.x, a.y + size.y);
    ImGui::InvisibleButton(idStr, size);
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;

    const float padX = 12.0f, padY = 10.0f;
    const float gx0 = a.x + padX, gx1 = b.x - padX, mid = (a.y + b.y) * 0.5f, half = (size.y * 0.5f) - padY;
    auto xOf = [&](int p) { return gx0 + (gx1 - gx0) * static_cast<float>(p) / static_cast<float>(kLanePosMax); };
    auto yOf = [&](float v) { return mid - std::clamp(v, -100.0f, 100.0f) / 100.0f * half; };
    auto valAt = [&](float y) { const int v = static_cast<int>(std::lround(std::clamp((mid - y) / half, -1.0f, 1.0f) * 100.0f)); return std::abs(v) <= 3 ? 0 : v; };
    auto posAt = [&](float x) { return static_cast<int>(std::lround(std::clamp((x - gx0) / std::max(1.0f, gx1 - gx0), 0.0f, 1.0f) * kLanePosMax)); };

    int rowK = sp.Find(track, param);
    Lane flat;   // what an unchanged instrument shows: a flat line with two ghost ends
    Lane* ln = rowK >= 0 ? &sp.row[rowK].lane : &flat;
    bool changed = false;

    // ---- a drag in progress ------------------------------------------------------------------------------------------------
    const bool mine = ds.id == id;
    if (mine) {
        if (rowK < 0) ds.id = 0;
        else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { *ln = ds.before; app.DropLastUndo(); ds.id = 0; changed = true; }
        else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { ln->Normalize(-100, 100); ds.id = 0; changed = true; }
        else {
            float y = mouse.y;
            if (io.KeyShift) y = ds.y0 + (mouse.y - ds.y0) * 0.2f;
            ln->MovePoint(ds.pt, posAt(mouse.x), valAt(y + ds.yOff), -100, 100, true);
            ValueTip("%s", ParamValueText(param, ln->val[std::min<int>(ds.pt, ln->n - 1)]).c_str());
            changed = true;
        }
    }

    // ---- drawing -------------------------------------------------------------------------------------------------------------
    dl->AddRectFilled(a, b, vis::kInset, 5.0f);
    for (int g : {-100, -50, 50, 100}) dl->AddLine(ImVec2(a.x + 4, yOf(static_cast<float>(g))), ImVec2(b.x - 4, yOf(static_cast<float>(g))), IM_COL32(255, 255, 255, 12));
    for (int q = 1; q < 4; ++q) dl->AddLine(ImVec2(a.x + (b.x - a.x) * q / 4.0f, a.y + 4), ImVec2(a.x + (b.x - a.x) * q / 4.0f, b.y - 4), IM_COL32(255, 255, 255, 10));
    dl->AddLine(ImVec2(a.x + 4, mid), ImVec2(b.x - 4, mid), IM_COL32(255, 255, 255, 40));
    dl->AddText(nullptr, 10.0f, ImVec2(a.x + 6, yOf(100) - 12.0f < a.y ? a.y + 2 : yOf(100) - 12.0f), IM_COL32(220, 224, 236, 90), "+");
    dl->AddText(nullptr, 10.0f, ImVec2(a.x + 6, b.y - 14.0f), IM_COL32(220, 224, 236, 90), "-");
    dl->AddRect(a, b, vis::kLine, 5.0f);

    const bool isFlat = ln->n < 2;
    if (isFlat) dl->AddLine(ImVec2(gx0, mid), ImVec2(gx1, mid), lane_detail::Alpha(col, 90), 1.6f);
    for (int k = 0; k + 1 < ln->n; ++k) {
        const ImVec2 pa(xOf(ln->pos[k]), yOf(ln->val[k])), pb(xOf(ln->pos[k + 1]), yOf(ln->val[k + 1]));
        const ImVec2 qa(pa.x, mid), qb(pb.x, mid);
        const ImVec2 q[4] = {pa, pb, qb, qa};
        dl->AddConvexPolyFilled(q, 4, lane_detail::Alpha(col, 34));
        dl->AddLine(pa, pb, col, 2.4f);
    }
    int hotPt = -1;
    const int shown = isFlat ? 2 : ln->n;
    for (int k = 0; k < shown; ++k) {
        const ImVec2 c = isFlat ? ImVec2(k ? gx1 : gx0, mid) : ImVec2(xOf(ln->pos[k]), yOf(ln->val[k]));
        const bool dragged = mine && ds.pt == k;
        const bool near = hovered && ds.id == 0 && std::fabs(mouse.x - c.x) < 10.0f && std::fabs(mouse.y - c.y) < 10.0f;
        if (near) hotPt = k;
        const bool big = dragged || near;
        dl->AddCircleFilled(c, big ? 6.0f : 4.4f, big ? vis::kAccentHi : col);
        dl->AddCircle(c, big ? 6.0f : 4.4f, IM_COL32(20, 22, 28, 255), 0, 1.4f);
    }
    if (playProg >= 0.0) {   // where the scene is now
        const float px = gx0 + (gx1 - gx0) * static_cast<float>(playProg);
        dl->AddLine(ImVec2(px, a.y + 3), ImVec2(px, b.y - 3), IM_COL32(255, 255, 255, 110), 1.2f);
        dl->AddCircleFilled(ImVec2(px, yOf(ln->At(playProg))), 3.4f, IM_COL32(255, 255, 255, 235));
    }

    // ---- gestures ------------------------------------------------------------------------------------------------------------
    if (hovered && ds.id == 0) {
        if (hotPt >= 0) Hint("%s of this scene, point %d: %s.\nDrag: move it.  Shift: finer.  Option + click: back to 0.  Double-click an inner point: remove it.  Esc while dragging: cancel.",
                             AutoParamName(param), hotPt + 1, ParamValueText(param, isFlat ? 0 : ln->val[hotPt]).c_str());
        else Hint("%s over the scene's loop: click on empty space = add a point, drag the points.  The middle line = the sound as it is (0).\n%s.", AutoParamName(param), ParamHundred(param));
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (rowK < 0) {   // the first edit starts the row
                app.PushSoundUndo();
                rowK = sp.Add(track, param);
                if (rowK < 0) { app.DropLastUndo(); app.SetStatus("A SCENE HAS ROOM FOR " + std::to_string(kMaxParamRows) + " PARAMETER ROWS - REMOVE ONE FIRST"); return changed; }
                ln = &sp.row[rowK].lane;
            } else app.PushSoundUndo();
            const float ymid = hotPt >= 0 ? (isFlat ? mid : yOf(ln->val[hotPt])) : mouse.y;
            if (hotPt >= 0 && io.KeyAlt) { ln->Materialize(); ln->MovePoint(hotPt, ln->pos[hotPt], 0, -100, 100); ln->Normalize(-100, 100); changed = true; }
            else if (hotPt >= 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !isFlat && hotPt > 0 && hotPt < ln->n - 1 && ImGui::GetTime() - ds.lastAddT > 0.5) {
                ln->RemovePoint(hotPt); changed = true;
            } else if (hotPt >= 0) {
                ln->Materialize();
                ds.id = id; ds.pt = hotPt; ds.before = *ln; ds.y0 = mouse.y; ds.yOff = ymid - mouse.y;
            } else {
                const Lane before = *ln;
                const int at = ln->AddPoint(posAt(mouse.x), valAt(mouse.y), -100, 100);
                if (at < 0) { *ln = before; app.DropLastUndo(); app.SetStatus(before.n >= kMaxLanePts ? "LANE FULL" : "TOO CLOSE TO ANOTHER POINT"); }
                else { ds.id = id; ds.pt = at; ds.before = before; ds.y0 = mouse.y; ds.yOff = 0.0f; ds.lastAddT = ImGui::GetTime(); changed = true; }
            }
        }
    }
    return changed;
}

} // namespace t8::desktop
