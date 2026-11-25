#pragma once
// Words for the parameter rows (v44.38 bricks, v44.39 LIVE scenes): a value of a lane in the unit of the knob, and what 100 means.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace t8::desktop {

// v44.38: a parameter-row offset (-100..100) in the unit of the parameter, and what 100 means
inline std::string ParamValueText(int param, int v) {
    char b[32];
    switch (param) {
    case 0: if (v == 0) return "centre"; std::snprintf(b, sizeof b, "%s %d", v < 0 ? "L" : "R", std::abs(v)); return b;
    case 3: std::snprintf(b, sizeof b, "%+.1f st", v * 0.12f); return b;
    case 4: std::snprintf(b, sizeof b, "x%.2f", std::exp2(v / 100.0f)); return b;
    case 5: std::snprintf(b, sizeof b, "%+.1f oct", v * 0.03f); return b;
    default: std::snprintf(b, sizeof b, "%+d%%", v); return b;
    }
}
inline const char* ParamHundred(int param) {
    switch (param) {
    case 0: return "100 = all the way to the side"; case 1: case 2: return "100 = the send is opened by 100%";
    case 3: return "100 = 12 semitones"; case 4: return "100 = twice as long (-100 = half)";
    case 5: return "100 = 3 octaves up (-100 = 3 down)"; default: return "100 = +50% resonance";
    }
}

} // namespace t8::desktop
