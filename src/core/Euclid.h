#pragma once
// Euclidean rhythms and rotation (v44.4). EuclidPattern(3, 8) = x..x..x. (the 3-3-2 "tresillo"); EuclidPattern(5, 16) spreads five
// hits as evenly as the grid allows. RotateSteps slides the whole figure of a track round its own length.
#include "Types.h"
#include <algorithm>
#include <vector>

namespace t8 {

// `hits` onsets spread evenly over `steps` steps (bucket method: the same pattern as Bjorklund up to a rotation).
inline std::vector<bool> EuclidPattern(int hits, int steps) {
    steps = std::clamp(steps, 1, kMaxSteps);
    hits = std::clamp(hits, 0, steps);
    std::vector<bool> p(static_cast<size_t>(steps), false);
    for (int i = 0; i < steps; ++i) p[static_cast<size_t>(i)] = ((i * hits) % steps) < hits;
    return p;
}

// Rotate: result[(i + shift) mod steps] = pattern[i]; shift > 0 = later in time.
inline std::vector<bool> RotatePattern(const std::vector<bool>& p, int shift) {
    const int n = static_cast<int>(p.size());
    std::vector<bool> out(p.size(), false);
    if (n == 0) return out;
    for (int i = 0; i < n; ++i) out[static_cast<size_t>((((i + shift) % n) + n) % n)] = p[static_cast<size_t>(i)];
    return out;
}

// Euclid onto a track: within [0, len) every step is cleared and then written as a copy of `hit` where the (rotated) pattern
// is on; the figure of `steps` steps repeats through the length. Steps beyond len are left alone.
inline int ApplyEuclidToTrack(Track& t, int len, int hits, int steps, int rotate, const Step& hit) {
    len = std::clamp(len, 1, kMaxSteps);
    steps = std::clamp(steps, 1, kMaxSteps);
    const std::vector<bool> pat = RotatePattern(EuclidPattern(hits, steps), rotate);
    int n = 0;
    for (int i = 0; i < len; ++i) {
        Step s = hit;
        const bool on = pat[static_cast<size_t>(i % steps)];
        t.steps[static_cast<size_t>(i)] = on ? s : Step{};
        if (on) { t.steps[static_cast<size_t>(i)].active = true; ++n; }
    }
    return n;
}

// v44.55: EUCLID ON for a melody. `melody` is the list of notes (steps) in time order; the hits of the figure take them in turn and cycle
// when there are more hits than notes. Within [0, len) every step is cleared first. Slide and nudge of a note do not travel to its new place.
inline int EuclidMelodyApply(Track& t, int len, const std::vector<Step>& melody, int hits, int steps, int rotate) {
    len = std::clamp(len, 1, kMaxSteps);
    steps = std::clamp(steps, 1, kMaxSteps);
    const std::vector<bool> pat = RotatePattern(EuclidPattern(hits, steps), rotate);
    int n = 0;
    for (int i = 0; i < len; ++i) {
        const bool on = pat[static_cast<size_t>(i % steps)] && !melody.empty();
        if (!on) { t.steps[static_cast<size_t>(i)] = Step{}; continue; }
        Step s = melody[static_cast<size_t>(n) % melody.size()];
        s.active = true; s.nudge = 0; s.slide = false;
        t.steps[static_cast<size_t>(i)] = s;
        ++n;
    }
    return n;
}

// The two steps sound the same (an inactive step is just "nothing").
inline bool EuclidStepSame(const Step& a, const Step& b) {
    if (!a.active && !b.active) return true;
    return a.active == b.active && a.accent == b.accent && a.variant == b.variant && a.note == b.note && a.lengthSteps == b.lengthSteps &&
           a.slide == b.slide && a.ratchet == b.ratchet && a.probability == b.probability && a.nudge == b.nudge && a.ghost == b.ghost && a.vel == b.vel;
}

// The notes of a track in time order (only the active steps of [0, len)).
inline std::vector<std::pair<uint8_t, Step>> EuclidCaptureSteps(const Track& t, int len) {
    std::vector<std::pair<uint8_t, Step>> out;
    len = std::clamp(len, 1, kMaxSteps);
    for (int i = 0; i < len; ++i) if (t.steps[static_cast<size_t>(i)].active) out.emplace_back(static_cast<uint8_t>(i), t.steps[static_cast<size_t>(i)]);
    return out;
}

// Does the track still hold exactly what EUCLID ON derived from `t.euclidOrig` (nobody edited the steps since)?
inline bool EuclidDerivedIntact(const Track& t, int len, const std::vector<Step>& melody) {
    Track probe = t;
    EuclidMelodyApply(probe, len, melody, t.euclidHits, t.euclidSteps, t.euclidRotate);
    for (int i = 0; i < std::clamp(len, 1, kMaxSteps); ++i)
        if (!EuclidStepSame(t.steps[static_cast<size_t>(i)], probe.steps[static_cast<size_t>(i)])) return false;
    return true;
}

// Slide every step of [0, len) by `shift` steps round the length (notes, velocity, nudge, chords - whole steps move).
inline void RotateSteps(Track& t, int len, int shift) {
    len = std::clamp(len, 1, kMaxSteps);
    shift = ((shift % len) + len) % len;
    if (shift == 0) return;
    std::vector<Step> copy(t.steps.begin(), t.steps.begin() + len);
    for (int i = 0; i < len; ++i) t.steps[static_cast<size_t>((i + shift) % len)] = copy[static_cast<size_t>(i)];
}

} // namespace t8
