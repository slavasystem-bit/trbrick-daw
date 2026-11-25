#pragma once
// Per-track look shared by the LIVE panels: the type colour (drums amber,
// synths teal, chord purple -- the same bar the sequencer draws next to a
// track name, reused for the mixer sliders) and the display name.
#include "core/Types.h"
#include "Visual.h"
#include "imgui.h"
#include <cstdio>
#include <string>

namespace t8::desktop {

inline ImU32 TrackTypeColor(TrackType type) {
    switch (type) {
        case TrackType::Drum:  return vis::kAccent;
        case TrackType::Synth: return vis::kTeal;
        case TrackType::Chord: return vis::kViolet;
        case TrackType::Sampler: return vis::kAccent;
    }
    return vis::kAccent;
}

// One distinct colour per instrument (global track index: Kick, Snare, Clap,
// ClosedHH, OpenHH, Perc, Bass, Synth2, Synth3, Chord, Sampler). Drums run warm ->
// cool, synths are the green/teal family, the chord track stays violet, so the
// type is still readable at a glance while every row has its own hue.
inline ImU32 TrackColor(int trackIndex) {
    static const ImU32 kCols[kNumTracks] = {
        IM_COL32(238, 74, 60, 255),     // Kick      red (v44.40: the accent amber is only for "selected / primary")
        IM_COL32(246, 201, 76, 255),    // Snare     yellow
        IM_COL32(240, 112, 156, 255),   // Clap      pink
        IM_COL32(95, 208, 240, 255),    // ClosedHH  cyan
        IM_COL32(86, 148, 232, 255),    // OpenHH    blue
        IM_COL32(214, 218, 232, 255),   // Perc      silver
        IM_COL32(34, 211, 174, 255),    // Bass      teal
        IM_COL32(118, 226, 128, 255),   // Synth2    green
        IM_COL32(196, 232, 104, 255),   // Synth3    lime
        IM_COL32(178, 140, 242, 255),   // Chord     violet
        IM_COL32(252, 150, 70, 255),    // Sampler   orange (a recording, warmer than the greens)
    };
    if (trackIndex < 0 || trackIndex >= kNumTracks) return vis::kAccent;
    return kCols[trackIndex];
}
inline ImU32 ColorAlpha(ImU32 c, int alpha) { return (c & 0x00FFFFFFu) | (static_cast<ImU32>(alpha) << 24); }
inline ImU32 ColorLighten(ImU32 c, float amt) {   // toward white
    int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = (c >> 24) & 0xFF;
    r += static_cast<int>((255 - r) * amt); g += static_cast<int>((255 - g) * amt); b += static_cast<int>((255 - b) * amt);
    return IM_COL32(r, g, b, a);
}
inline ImU32 ColorDarken(ImU32 c, float amt) {    // toward black
    int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = (c >> 24) & 0xFF;
    r = static_cast<int>(r * (1.0f - amt)); g = static_cast<int>(g * (1.0f - amt)); b = static_cast<int>(b * (1.0f - amt));
    return IM_COL32(r, g, b, a);
}

// Synth slot 1 is the bass in every pattern/preset the app ships; show it as
// such (older saved projects still carry the stored name "Synth1").
inline std::string TrackDisplayName(const Track& t) {
    if (t.type == TrackType::Synth && t.slotIndex == 0) return "Bass";
    return t.name;
}

} // namespace t8::desktop
