#pragma once
// One visual system for every desktop panel: named colour / metric tokens
// instead of scattered IM_COL32 literals. The ImGui style colours in
// DesktopTheme.cpp use the same palette (amber accent on a cool charcoal
// ground); anything custom-drawn with ImDrawList should take its colours from
// here so a palette tweak is a one-file change.
//   accent  amber   = "on / selected / primary"      (kAccent, kAccentHi)
//   teal            = synth tracks / "playing"        (kTeal)
//   violet          = chord track                     (kViolet)
//   red             = record / clip / destructive     (kRed)
#include "imgui.h"

namespace t8::desktop::vis {

// -- hues ---------------------------------------------------------------
constexpr ImU32 kAccent    = IM_COL32(242, 163, 61, 255);
constexpr ImU32 kAccentHi  = IM_COL32(255, 196, 122, 255);
constexpr ImU32 kAccentDim = IM_COL32(242, 163, 61, 90);
constexpr ImU32 kAccentGlow= IM_COL32(242, 163, 61, 26);
constexpr ImU32 kTeal      = IM_COL32(95, 225, 201, 255);
constexpr ImU32 kViolet    = IM_COL32(178, 140, 242, 255);
constexpr ImU32 kRed       = IM_COL32(232, 76, 76, 255);
// -- text ---------------------------------------------------------------
constexpr ImU32 kText      = IM_COL32(236, 237, 242, 255);
constexpr ImU32 kTextDim   = IM_COL32(142, 146, 163, 255);
// -- surfaces (dark -> light) ---------------------------------------------
constexpr ImU32 kInset     = IM_COL32(24, 26, 33, 255);    // recessed wells, graph backgrounds
constexpr ImU32 kCell      = IM_COL32(30, 32, 40, 255);    // idle buttons / step cells
constexpr ImU32 kCellHover = IM_COL32(40, 43, 55, 255);
constexpr ImU32 kLine      = IM_COL32(52, 56, 72, 255);    // separators, outlines
constexpr ImU32 kLineHi    = IM_COL32(70, 74, 90, 255);    // stronger outlines, grid emphasis
constexpr ImU32 kGrid      = IM_COL32(255, 255, 255, 14);  // faint graph grid

// -- metrics --------------------------------------------------------------
constexpr float kRound     = 5.0f;   // buttons, cells
constexpr float kRoundWell = 4.0f;   // graph wells

// Recessed graph well with a faint 4-column grid; returns the inner rect via
// out-params. Used by the scope, ADSR and LFO previews so they all read alike.
inline void DrawWell(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    dl->AddRectFilled(a, b, kInset, kRoundWell);
    for (int i = 1; i < 4; ++i) {
        float x = a.x + (b.x - a.x) * i / 4.0f;
        dl->AddLine(ImVec2(x, a.y + 2.0f), ImVec2(x, b.y - 2.0f), kGrid, 1.0f);
    }
    dl->AddRect(a, b, kLine, kRoundWell);
}

} // namespace t8::desktop::vis

namespace vis = t8::desktop::vis;
