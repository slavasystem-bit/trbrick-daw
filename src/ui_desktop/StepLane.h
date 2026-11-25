#pragma once
// Value lanes of the step editors (v40): a bar per step of the current 16-step page, edited by dragging.
//   VELOCITY : 1..127 (Step::vel). Steps that were never edited show their classic value (accent / normal / ghost) hatched.
//   NUDGE    : -50..+50 % of a step (Step::nudge); drum tracks can go negative (pulled ahead), the others only positive.
// One widget, two hosts: the drum LANES panel (replaces the mixer when a drum track is selected) and the strip under the
// piano roll. Later lanes (probability, ratchet, swing / groove amount) plug in the same way.
//
// Gestures: press and drag across the steps to draw (every step passed gets the value under the mouse, also when the
// drag is fast); right-click a step to set it back to its default; Option(Alt)-drag draws a straight line from the press.
#include "app/AppState.h"
#include "StepGlyphs.h"
#include "TrackStyle.h"
#include "Visual.h"
#include "imgui.h"
#include "InfoLine.h"
#include "Interaction.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>

namespace t8::desktop {

enum class LaneKind { Velocity = 0, Nudge = 1 };

struct LaneState {
    bool drawing = false;
    bool undone = false;
    int lastCol = -1;
    float lastValue = 0.0f;   // value (in the lane's own units) at lastCol
    bool line = false;        // Alt-drag straight line
    int lineCol = -1;
    float lineValue = 0.0f;
};

namespace lane_detail {

inline int NudgeMin(const Track& t) { return t.type == TrackType::Drum ? -50 : 0; }

inline float ReadValue(const Track& t, const Step& s, LaneKind k) {
    if (k == LaneKind::Velocity) return StepVelocity(s) * 127.0f;
    return static_cast<float>(std::clamp<int>(s.nudge, NudgeMin(t), 50));
}
inline bool IsDefault(const Step& s, LaneKind k) { return k == LaneKind::Velocity ? s.vel == 0 : s.nudge == 0; }

inline void WriteValue(const Track& t, Step& s, LaneKind k, float v) {
    if (k == LaneKind::Velocity) {
        s.vel = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(v)), 1, 127));
        s.accent = false;
        s.ghost = false;
    } else {
        int n = static_cast<int>(std::lround(v));
        if (std::abs(n) <= 2) n = 0;   // a notch at "on the grid"
        s.nudge = static_cast<int8_t>(std::clamp(n, NudgeMin(t), 50));
    }
}
inline void Reset(Step& s, LaneKind k) {
    if (k == LaneKind::Velocity) s.vel = 0;   // back to what the ACCENT / GHOST flags say
    else s.nudge = 0;
}

} // namespace lane_detail

// Draws the lane in the window being drawn into. `top` / `height` are window-local; geom.x0 is the window-local x of column 0.
// Returns true when something was edited this frame.
inline bool DrawStepLane(AppState& app, int ti, LaneKind kind, LaneState& st, const PageGeom& geom, float top, float height,
                         float totalW, int trackLen, int playStep)
{
    using namespace lane_detail;
    Track& t = app.StepTrack(app.CurrentPattern(), ti);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 win = ImGui::GetWindowPos();
    const float sx = ImGui::GetScrollX(), sy = ImGui::GetScrollY();
    const float L = win.x + geom.x0 - sx - 4.0f, R = win.x + geom.x0 - sx + totalW;
    const float T = win.y + top - sy, B = T + height;
    const ImU32 col = TrackColor(ti);
    const bool signedLane = kind == LaneKind::Nudge && t.type == TrackType::Drum;
    const float lo = kind == LaneKind::Velocity ? 0.0f : static_cast<float>(NudgeMin(t));
    const float hi = kind == LaneKind::Velocity ? 127.0f : 50.0f;
    const float barPadTop = 12.0f;                       // room for the value label above the highest bar
    const float plotT = T + barPadTop, plotB = B - 2.0f;

    // background and grid
    dl->AddRectFilled(ImVec2(L, T), ImVec2(R, B), vis::kInset, 4.0f);
    for (int q = 1; q < 4; ++q) {
        float y = plotT + (plotB - plotT) * q / 4.0f;
        dl->AddLine(ImVec2(L + 2.0f, y), ImVec2(R - 2.0f, y), (signedLane && q == 2) ? vis::kLineHi : vis::kCellHover, 1.0f);
    }
    auto yOf = [&](float v) { return plotB - (plotB - plotT) * (v - lo) / (hi - lo); };
    auto valueAtY = [&](float y) { return lo + (hi - lo) * (plotB - std::clamp(y, plotT, plotB)) / (plotB - plotT); };

    // interaction: one invisible button over the lane
    ImGui::SetCursorPos(ImVec2(geom.x0 - 4.0f, top));
    ImGui::InvisibleButton("##lane", ImVec2(totalW + 4.0f, height));
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    bool edited = false;
    auto colAt = [&](float mx) { return std::clamp(geom.base + geom.ColAtX(mx - win.x + sx), 0, std::max(0, trackLen - 1)); };
    auto begin = [&]() { if (!st.undone) { app.PushUndo(); st.undone = true; } };

    auto applyRange = [&](int c0, float v0, int c1, float v1) {
        const int a = std::min(c0, c1), b = std::max(c0, c1);
        for (int c = a; c <= b && c < kMaxSteps; ++c) {
            Step& s = t.steps[c];
            if (!s.active) continue;
            float f = (c1 == c0) ? 1.0f : static_cast<float>(c - c0) / static_cast<float>(c1 - c0);
            WriteValue(t, s, kind, v0 + (v1 - v0) * std::clamp(f, 0.0f, 1.0f));
            edited = true;
        }
    };

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        st.drawing = true; st.undone = false;
        st.line = io.KeyAlt;
        const int c = colAt(io.MousePos.x);
        const float v = valueAtY(io.MousePos.y);
        app.seqCursorTrack = ti; app.seqCursorStep = c;
        if (st.line && st.lineCol >= 0) {
            begin(); applyRange(st.lineCol, st.lineValue, c, v);
        } else {
            begin(); applyRange(c, v, c, v);
        }
        st.lastCol = c; st.lastValue = v; st.lineCol = c; st.lineValue = v;
    } else if (st.drawing && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !st.line) {
        const int c = colAt(io.MousePos.x);
        const float v = valueAtY(io.MousePos.y);
        if (c != st.lastCol || std::fabs(v - st.lastValue) > 0.5f) {
            begin(); applyRange(st.lastCol, st.lastValue, c, v);
            st.lastCol = c; st.lastValue = v; st.lineCol = c; st.lineValue = v;
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { st.drawing = false; st.lastCol = -1; }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        const int c = colAt(io.MousePos.x);
        if (c < kMaxSteps && t.steps[c].active && !IsDefault(t.steps[c], kind)) { app.PushUndo(); Reset(t.steps[c], kind); edited = true; }
    }

    // bars
    const float baseV = signedLane ? 0.0f : lo;
    const float yBase = yOf(baseV);
    const int hoverCol = hovered ? colAt(io.MousePos.x) : -1;
    for (int c = 0; c < geom.pageSteps; ++c) {
        const int stp = geom.base + c;
        if (stp >= kMaxSteps) break;
        const float x0 = win.x + geom.X(c) - sx, x1 = x0 + geom.cellW;
        const bool inRange = stp < trackLen;
        const Step& s = t.steps[stp];
        if (!inRange) { dl->AddRectFilled(ImVec2(x0, plotT), ImVec2(x1, plotB), IM_COL32(0, 0, 0, 50), 2.0f); continue; }
        if (!s.active) {
            dl->AddRectFilled(ImVec2(x0, plotB - 3.0f), ImVec2(x1, plotB), IM_COL32(120, 124, 140, 70), 1.0f);
            continue;
        }
        const float v = ReadValue(t, s, kind);
        const float y = yOf(v);
        const bool dflt = IsDefault(s, kind);
        const ImU32 fill = dflt ? ColorAlpha(col, 95) : ColorAlpha(col, 235);
        const float ya = std::min(y, yBase), yb = std::max(y, yBase);
        dl->AddRectFilled(ImVec2(x0, ya), ImVec2(x1, std::max(yb, ya + 2.0f)), fill, 2.0f);
        if (dflt) {   // hatching marks a value that was never edited
            dl->PushClipRect(ImVec2(x0, ya), ImVec2(x1, std::max(yb, ya + 2.0f)), true);
            const float hgt = yb - ya + 2.0f;
            for (float hx = x0 - hgt; hx < x1; hx += 6.0f) dl->AddLine(ImVec2(hx, yb + 1.0f), ImVec2(hx + hgt, ya - 1.0f), IM_COL32(18, 20, 26, 110), 1.0f);
            dl->PopClipRect();
        }
        dl->AddRectFilled(ImVec2(x0, y - 1.0f), ImVec2(x1, y + 1.0f), IM_COL32(255, 255, 255, dflt ? 120 : 230), 1.0f);
        if (geom.cellW >= 24.0f) {
            char b[8];
            if (kind == LaneKind::Velocity) std::snprintf(b, sizeof(b), "%d", static_cast<int>(std::lround(v)));
            else std::snprintf(b, sizeof(b), "%+d", static_cast<int>(v));
            ImVec2 ts = ImGui::CalcTextSize(b);
            const float fs = 10.0f, sc = fs / ImGui::GetFontSize();
            dl->AddText(ImGui::GetFont(), fs, ImVec2((x0 + x1) * 0.5f - ts.x * sc * 0.5f, std::max(T + 1.0f, y - 13.0f)),
                        dflt ? IM_COL32(150, 154, 170, 220) : IM_COL32(235, 238, 245, 255), b);
        }
        if (c == playStep - geom.base) dl->AddRect(ImVec2(x0 - 1.0f, plotT), ImVec2(x1 + 1.0f, plotB), IM_COL32(255, 255, 255, 200), 2.0f, 0, 1.5f);
        if (stp == hoverCol && !st.drawing) dl->AddRect(ImVec2(x0 - 1.0f, plotT), ImVec2(x1 + 1.0f, plotB), IM_COL32(255, 255, 255, 90), 2.0f);
    }
    if (hovered && !st.drawing && hoverCol >= 0 && hoverCol < kMaxSteps && t.steps[hoverCol].active) {
        const float v = ReadValue(t, t.steps[hoverCol], kind);
        if (kind == LaneKind::Velocity) ValueTip("step %d  velocity %d\ndrag = draw, right-click = default, %s-drag = line", hoverCol + 1, static_cast<int>(std::lround(v)), interaction::OptionName());
        else ValueTip("step %d  nudge %+d %%\ndrag = draw, right-click = 0, %s-drag = line", hoverCol + 1, static_cast<int>(v), interaction::OptionName());
    }
    return edited;
}

// Humanize: random jitter of the lane's value on every active step of the track. amount = percent of the lane range.
inline void HumanizeLane(AppState& app, int ti, LaneKind kind, float amountPct) {
    using namespace lane_detail;
    Track& t = app.StepTrack(app.CurrentPattern(), ti);
    const int len = std::min(kMaxSteps, std::max(1, t.useOwnLength ? t.length : app.CurrentPattern().masterLength));
    static std::mt19937 rng{std::random_device{}()};
    const float range = kind == LaneKind::Velocity ? 127.0f : 40.0f;
    std::uniform_real_distribution<float> d(-1.0f, 1.0f);
    app.PushUndo();
    for (int c = 0; c < len; ++c) {
        Step& s = t.steps[c];
        if (!s.active) continue;
        float v = ReadValue(t, s, kind) + d(rng) * range * amountPct / 100.0f;
        v = std::clamp(v, kind == LaneKind::Velocity ? 8.0f : static_cast<float>(NudgeMin(t)), kind == LaneKind::Velocity ? 127.0f : 50.0f);
        if (kind == LaneKind::Nudge && std::fabs(v) < 3.0f) v = 3.0f * (d(rng) < 0 ? -1.0f : 1.0f);   // never "snap to zero" on a humanize
        WriteValue(t, s, kind, v);
        if (kind == LaneKind::Nudge) s.nudge = static_cast<int8_t>(std::clamp<int>(static_cast<int>(std::lround(v)), NudgeMin(t), 50));
    }
}

inline void ResetLane(AppState& app, int ti, LaneKind kind) {
    Track& t = app.StepTrack(app.CurrentPattern(), ti);
    app.PushUndo();
    for (int c = 0; c < kMaxSteps; ++c) lane_detail::Reset(t.steps[c], kind);
}

} // namespace t8::desktop
