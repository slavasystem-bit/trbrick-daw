#pragma once
// A slow, warm one-bar groove for an empty scene (v44.8, "the first sixty seconds").
// It is only a seed: something that sounds right the moment Play is pressed, so that the first thing a newcomer does is
// change it instead of staring at an empty grid. Key: C minor-ish (C dorian friendly); the chord is a Cm7.
#include "Types.h"

namespace t8 {

// Writes the groove into `p` (steps, tempo, a touch of swing). Existing steps are replaced track by track for the tracks it uses;
// other tracks are left alone. The caller handles undo and the lock; `chordTrack` is where the chord steps go (the pattern's own
// chord track, or the chord-bank scene's track).
inline void ApplyStarterGroove(Pattern& p, Track& chordTrack) {
    p.bpm = 72.0f;
    p.masterLength = 16;
    p.stepsPerBeat = 4;
    p.swing = 0.10f;
    auto clear = [](Track& t) { for (auto& s : t.steps) s = Step{}; };
    auto hit = [&](int track, int step, bool accent = false) {
        Step& s = p.tracks[static_cast<size_t>(track)].steps[static_cast<size_t>(step)];
        s.active = true; s.accent = accent;
    };
    for (int t = 0; t < kNumDrumTracks; ++t) clear(p.tracks[static_cast<size_t>(t)]);
    hit(static_cast<int>(DrumSlot::Kick), 0, true);
    hit(static_cast<int>(DrumSlot::Kick), 7);
    hit(static_cast<int>(DrumSlot::Kick), 10);
    hit(static_cast<int>(DrumSlot::Snare), 8, true);
    hit(static_cast<int>(DrumSlot::Clap), 8);
    for (int st : {2, 6, 10, 14}) hit(static_cast<int>(DrumSlot::ClosedHat), st);
    hit(static_cast<int>(DrumSlot::Perc), 11);

    Track& bass = p.tracks[static_cast<size_t>(kNumDrumTracks)];   // BASS = the first synth track
    clear(bass);
    auto note = [&](int step, int midi, int len) {
        Step& s = bass.steps[static_cast<size_t>(step)];
        s.active = true; s.note = static_cast<int8_t>(midi); s.lengthSteps = static_cast<uint8_t>(len);
    };
    note(0, 36, 6);    // C2
    note(7, 39, 2);    // Eb2
    note(10, 34, 4);   // Bb1

    clear(chordTrack);
    Step& c = chordTrack.steps[0];
    c.active = true; c.lengthSteps = 16; c.chordSize = 4;
    c.chordNotes = {48, 55, 58, 63, 0, 0};   // C3 G3 Bb3 Eb4 = Cm7
}

} // namespace t8
