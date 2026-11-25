#pragma once
// Shared drawing + geometry for step cells and note-length bars, used by the
// Sequencer and the Piano Roll so ratchet / probability / tie / length read the
// same way in both.
//   probability < 100  -> the cell is only filled up to that percentage (from
//                         the bottom) and the number is printed top-left
//   ratchet > 1        -> that many dark ticks across the cell and "xN" top-right
//   SLIDE / full-loop  -> a small chain-link glyph (the note is tied, see
//                         Sequencer::FireTick) and a slanted slash for glide
//   length > 1         -> a translucent bar across the cells the note holds,
//                         with a grip at its right end to drag the length
#include "core/Types.h"
#include "TrackStyle.h"
#include "Visual.h"
#include "imgui.h"
#include <algorithm>
#include <cstdio>

namespace t8::desktop {

// x positions of the 16 columns of one page (same formula the cell loops use).
// Page layout of the step grids per Pattern::stepsPerBeat (v43): 16 columns in groups of 4 on 1/16 and 1/32, 12 in groups of 3 on the triplet grids.
inline int PageStepsFor(int spb) { return (spb == 6 || spb == 3) ? 12 : 16; }
inline int GroupStepsFor(int spb) { return (spb == 6 || spb == 3) ? 3 : 4; }
inline float CellWidthFor(float availW, float labelW, float gap, float groupGap, int pageSteps, int groupSteps) {
    return (availW - labelW - (pageSteps - 1) * gap - (pageSteps / groupSteps - 1) * groupGap) / pageSteps;
}

struct PageGeom {
    float x0 = 0.0f, cellW = 10.0f, gap = 2.0f, group = 6.0f;
    int base = 0;
    int pageSteps = 16;   // columns on a page (16, or 12 on the triplet grids)
    int groupSteps = 4;   // columns between the wider gaps (3 on the triplet grids)
    float X(int c) const { return x0 + c * (cellW + gap) + (c / groupSteps) * group; }
    // Column (0..15) whose cell, or the gap just after it, contains local x.
    int ColAtX(float x) const {
        for (int c = 0; c < pageSteps - 1; ++c) {
            float edge = X(c) + cellW + (X(c + 1) - X(c) - cellW) * 0.5f;
            if (x < edge) return c;
        }
        return pageSteps - 1;
    }
};

// Held-note length of a pitched step, clipped to the track loop.
inline int StepSpan(const Step& s, int col, int trackLen) {
    return std::clamp(static_cast<int>(s.lengthSteps), 1, std::max(1, trackLen - col));
}

// A step is tied (no release / retrigger into the next note) when it has SLIDE
// set or is as long as its whole loop -- mirrors Sequencer::FireTick.
inline bool StepIsTied(const Track& t, const Step& s, int trackLen) {
    return t.type != TrackType::Drum && s.active && (s.slide || static_cast<int>(s.lengthSteps) >= trackLen);
}

inline void DrawStepFace(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 color, int probability, bool accent, float round) {
    if (probability >= 100) {
        dl->AddRectFilled(p0, p1, color, round);
    } else {
        dl->AddRectFilled(p0, p1, ColorAlpha(color, 64), round);
        float fillH = (p1.y - p0.y) * std::clamp(probability, 0, 100) / 100.0f;
        if (fillH > 0.5f)
            dl->AddRectFilled(ImVec2(p0.x, p1.y - fillH), p1, color, round, fillH > round * 2.0f ? ImDrawFlags_RoundCornersBottom : ImDrawFlags_RoundCornersNone);
        dl->AddRect(p0, p1, ColorAlpha(color, 170), round);
    }
    if (accent) dl->AddRectFilled(ImVec2(p0.x + 3, p0.y + 2), ImVec2(p1.x - 3, p0.y + 4), IM_COL32(255, 255, 255, 220), 1.0f);
}

inline void DrawStepBadges(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int ratchet, int probability) {
    const float w = p1.x - p0.x, h = p1.y - p0.y;
    const ImU32 dark = IM_COL32(18, 20, 26, 215);
    if (ratchet > 1) {
        for (int i = 0; i < ratchet; ++i) {
            float x = p0.x + w * (i + 0.5f) / ratchet;
            dl->AddRectFilled(ImVec2(x - 1.0f, p0.y + h * 0.30f), ImVec2(x + 1.0f, p1.y - h * 0.12f), dark, 1.0f);
        }
        if (w >= 20.0f) {
            char b[6]; std::snprintf(b, sizeof(b), "x%d", ratchet);
            ImVec2 ts = ImGui::CalcTextSize(b);
            const float fs = 10.0f; float sc = fs / ImGui::GetFontSize();
            dl->AddText(ImGui::GetFont(), fs, ImVec2(p1.x - ts.x * sc - 2.0f, p0.y + 1.0f), dark, b);
        }
    }
    if (probability < 100 && w >= 20.0f) {
        char b[6]; std::snprintf(b, sizeof(b), "%d", probability);
        // light text when the lit part (bottom-up) does not reach the label
        ImU32 col = probability <= 60 ? vis::kText : dark;
        dl->AddText(ImGui::GetFont(), 10.0f, ImVec2(p0.x + 3.0f, p0.y + 1.0f), col, b);
    }
}

// v39: ghost note = the cell is dimmed and gets a hollow dot; nudge = a marker on the bottom edge that sits left / right of
// the centre in proportion to the offset (+-50 % reaches the cell edge).
inline void DrawMicroBadges(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int nudge, bool ghost) {   // pass StepIsGhost(s)
    const float w = p1.x - p0.x, h = p1.y - p0.y;
    const ImU32 dark = IM_COL32(18, 20, 26, 225);
    if (ghost) {
        dl->AddRectFilled(p0, p1, IM_COL32(18, 20, 26, 105), 4.0f);
        dl->AddCircle(ImVec2(p0.x + w * 0.5f, p0.y + h * 0.5f), std::max(2.5f, std::min(w, h) * 0.14f), IM_COL32(235, 238, 245, 200), 12, 1.4f);
    }
    if (nudge != 0) {
        const float x = p0.x + w * (0.5f + std::clamp(nudge, -50, 50) / 100.0f * 0.9f);
        dl->AddTriangleFilled(ImVec2(x - 3.0f, p1.y - 1.0f), ImVec2(x + 3.0f, p1.y - 1.0f), ImVec2(x, p1.y - 6.0f), dark);
    }
}

// Chain-link glyph (tie) at `at`; slanted slash for a glide.
inline void DrawTieGlyph(ImDrawList* dl, ImVec2 at, ImU32 col) {
    dl->AddCircle(ImVec2(at.x, at.y), 2.8f, col, 12, 1.4f);
    dl->AddCircle(ImVec2(at.x + 3.6f, at.y), 2.8f, col, 12, 1.4f);
}
inline void DrawSlideGlyph(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col) {
    dl->AddLine(ImVec2(p0.x + 3.0f, p1.y - 3.0f), ImVec2(p0.x + 10.0f, p1.y - 10.0f), col, 1.6f);
}

} // namespace t8::desktop
