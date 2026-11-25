#pragma once
// Phase 6: "typical progression" templates for the CHORDS tab -- same
// spirit as DrumPatterns.h's genre-shaped drum grooves: well-established,
// decades-old chord-progression CONVENTIONS (not a transcription of any
// specific song), ready to drop in instead of hand-picking degrees one at
// a time.
//
// Expressed as SCALE-DEGREE sequences (1-7), not fixed roman numerals like
// "I-IV-V-I" -- because this app's key/scale picker (Scales.h) covers all
// 7 diatonic modes, not just major, a fixed roman-numeral-with-implied-
// quality notation would silently be WRONG outside major/minor (e.g. "I"
// implies a major triad, but scale degree 1 in Dorian or Phrygian is
// minor). Feeding the same degree sequence through Chords.h's BuildChord
// in whatever mode is currently selected always produces the musically
// correct diatonic chords for THAT mode -- an honest generalization of
// "the four chords" etc. beyond just major, not a claim that every
// template will sound identical to the pop-song version people know it
// from (same "genre-style, not literal" honesty DrumPatterns.h/
// SynthPresets.cpp already apply to their own content).
#include <array>
#include <string>
#include <vector>

namespace t8 {

struct ChordProgressionData {
    const char* name;
    std::vector<int> degrees; // 1-based scale degrees, one per chord
};

constexpr int kNumChordProgressions = 19;
const std::array<ChordProgressionData, kNumChordProgressions>& AllChordProgressions();

} // namespace t8
