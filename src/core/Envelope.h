#pragma once
// Click-free fade helpers shared by the three synth engines.
//
// A fade that is too short for the pitch cuts the waveform off at some point
// in its cycle: on a 40 Hz sub bass (25 ms per cycle) a 4 ms release is a hard
// edge on a large signal = an audible tick. So the shortest allowed attack /
// release depends on the note's frequency, and short fades use a raised-cosine
// curve (zero slope at both ends, no kink) instead of a straight line.
#include <algorithm>
#include <cmath>

namespace t8 {

inline float EnvAttackSpan(float userSec, double freqHz) {
    float floorSec = std::clamp(static_cast<float>(0.15 / std::max(20.0, freqHz)), 0.001f, 0.006f);
    return std::max(userSec, floorSec);
}
inline float EnvReleaseSpan(float userSec, double freqHz) {
    float floorSec = std::clamp(static_cast<float>(0.40 / std::max(20.0, freqHz)), 0.004f, 0.015f);
    return std::max(userSec, floorSec);
}
// 0..1 -> 0..1 fade curve; `spanSec` decides raised-cosine (short) or linear (long).
// v40.2: a legato / tie into a note whose gate is still open keeps the envelopes running (no re-attack click, an endless pad
// restarts seamlessly). But a plucky patch (sustain ~ 0, short decay) has already faded by then, so the tied note was almost
// silent. If the amplitude has fallen below this level the tie lifts it back up with a normal (soft, from the current level)
// attack, which is what a 303 does: the gate stays open and the note keeps sounding. Pads and sustained patches stay untouched.
constexpr float kLegatoReviveLevel = 0.35f;

inline float EnvShape(float x, float spanSec) {
    x = std::clamp(x, 0.0f, 1.0f);
    if (spanSec >= 0.06f) return x;
    return 0.5f - 0.5f * std::cos(3.14159265f * x);
}

} // namespace t8
