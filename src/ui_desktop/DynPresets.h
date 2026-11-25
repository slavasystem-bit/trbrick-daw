#pragma once
// Ready-made dynamics shapes for one brick (v44.10.2). One table for the brick inspector and the right-click menu of the
// DYNAMICS block on the bricks strip, so both always offer the same thing.
#include "core/ArrangeTypes.h"
#include <cstddef>

namespace t8::desktop {

struct DynPreset { const char* label; const char* tip; StepAuto a; };

// StepAuto::Ends(mix start dB, mix end dB, DJ filter start, DJ filter end); DJ filter: negative = low-pass closes, positive = high-pass thins
inline const DynPreset* DynPresets(size_t* count) {
    static const DynPreset k[] = {
        {"FLAT", "No movement: full level, filter open.", StepAuto{}},
        {"BUILD", "Rises: a little quieter at the start, the low end thins out towards the end (filter 0 -> +60).", StepAuto::Ends(-4, 0, 0, 60)},
        {"BREAK", "Darker and a bit quieter for the whole brick (filter -35).", StepAuto::Ends(-3, -3, -35, -35)},
        {"DROP IN", "Full level; starts dark and opens up (filter -50 -> 0).", StepAuto::Ends(0, 0, -50, 0)},
        {"FADE OUT", "Fades to silence by the end of the brick.", StepAuto::Ends(-2, kAutoMinDb, 0, 30)},
        {"FADE IN", "Comes out of silence and opens up.", StepAuto::Ends(kAutoMinDb, -2, -40, 0)}};
    *count = sizeof(k) / sizeof(k[0]);
    return k;
}

// A short name for the shape of a brick's dynamics: the exact preset when it is one, otherwise what it does ("DARK", "OPENS"...).
// nullptr when there are none. Free points are judged by their ends and by the lowest / highest value in between.
inline const char* DynPresetName(const StepAuto& a) {
    if (!a.Any()) return nullptr;
    size_t n = 0; const DynPreset* p = DynPresets(&n);
    for (size_t i = 1; i < n; ++i) if (p[i].a == a) return p[i].label;
    const int ms = a.mix.Start(), me = a.mix.End(), fs = a.filter.Start(), fe = a.filter.End();
    if (me <= kAutoMinDb) return "FADE OUT";
    if (ms <= kAutoMinDb) return "FADE IN";
    if (a.mix.n > 2 || a.filter.n > 2) return "CUSTOM";   // free points: the shape is its own
    if (fe >= fs + 15 && fe > 0) return "THINS OUT";
    if (fs >= fe + 15 && fs > 0) return "FILLS OUT";
    if (fs <= fe - 15 && fs < 0) return "OPENS";
    if (fe <= fs - 15 && fe < 0) return "DARKENS";
    if (fs <= -20 || fe <= -20) return "DARK";
    if (me >= ms + 3) return "SWELL";
    if (ms >= me + 3) return "DIP";
    return "CUSTOM";
}

} // namespace t8::desktop
