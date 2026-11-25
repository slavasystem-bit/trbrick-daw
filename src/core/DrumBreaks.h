#pragma once
// "Live break" templates: short, one-bar drum fills meant to be triggered
// WHILE the sequencer is playing, for a TR-8/Elektron-style "press the FILL
// button, get one bar of roll/fill, then the pattern automatically picks
// back up exactly where it would have anyway" performance trick -- see
// AppState::UpdateLiveBreakHold/ActivateLiveBreak and Sequencer::
// SetLiveBreak for how this actually gets triggered live without needing a
// multi-button chord (just hold Y a beat longer than a normal play/stop
// tap). It's a ONE-SHOT trigger, not a sustain: once fired, it always plays
// its full 16-step arrangement start to finish and then reverts on its
// own, whether or not the trigger is still held.
//
// Deliberately a SEPARATE, smaller data set from DrumPatterns.h's
// GenrePattern templates: those replace a whole pattern bank's drums
// permanently (an editing action); these override one bar's worth of drum
// triggers only, and never touch the underlying pattern data at all --
// once the one-shot finishes (or is cancelled early, e.g. by releasing the
// RandomView preview row), the exact same steps that were always there
// play again, nothing to undo.
//
// Every break below deliberately concentrates its busiest/most-accented
// hits in the second half of its own 16 steps (steps 8-15): each break
// plays its own full arrangement from its own step 0 regardless of where
// in the underlying pattern's bar you happen to trigger it, but concentrating
// the climax late still matches how fills/rolls conventionally build
// toward the end of a bar in the first place.
#include "Types.h"
#include <array>
#include <string>
#include <vector>

namespace t8 {

struct DrumBreak {
    const char* name;
    // Same per-slot hit/accent convention as GenrePattern (DrumPatterns.h),
    // but always read modulo 16 regardless of the pattern's own length --
    // a live break is a fixed one-bar overlay, not a track edit.
    std::array<std::vector<int>, kNumDrumTracks> hitSteps;
    std::array<std::vector<int>, kNumDrumTracks> accentSteps;
};

constexpr int kNumDrumBreaks = 16;
const std::array<DrumBreak, kNumDrumBreaks>& AllDrumBreaks();

// Level trim of each break in dB (v36): measured as the loudness of the break's bar against a plain kick / snare / hat
// groove across ten kits (t8_audit "fill.*" rows). Rolls built from quiet voices get a lift, kick-heavy breaks are pulled
// down, so every fill lands within about 1.5 dB of the groove it replaces; the deliberately sparse ones (HALF-TIME DROP,
// TENSION STOP, RISER BUILD) stay a little under it on purpose.
float BreakTrimDb(int breakIndex);
float BreakTrimLinear(int breakIndex);

} // namespace t8
