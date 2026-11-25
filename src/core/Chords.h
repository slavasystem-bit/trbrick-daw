#pragma once
// Phase 6: diatonic chord-building for the CHORDS tab. Builds a chord by
// stacking every-other note of the SCALE ITSELF (not fixed major/minor
// thirds) starting on a given scale degree -- the standard "tertian
// harmonization" technique, and deliberately scale-agnostic: it produces
// the musically correct chord quality in any of Scales.h's 7 modes, not
// just major, the same "works for whatever key/scale is selected" spirit
// as Scales.h's own BuildScaleNotes/BuildRootNotes.
#include <array>
#include <vector>

namespace t8 {

// A single built chord. `notes` is 3 (triad) or 4 (7th chord) MIDI notes,
// ascending, notes[0] == rootNote. `quality` is a short label (MAJ/MIN/
// DIM/AUG/MAJ7/MIN7/DOM7/M7B5/...) derived from the ACTUAL semitone
// intervals between the stacked tones -- correct for whichever mode this
// was built in, not assumed from the scale degree number (e.g. degree 1
// isn't always MAJ -- in Aeolian/Dorian/Phrygian it's MIN).
struct Chord {
    std::vector<int> notes;
    const char* quality = "?";
    int rootNote = 60;
};

// Builds the diatonic chord rooted on scale degree `degree` (1-based,
// wraps via modulo for any int) of `scaleIndex` (see Scales.h) in key
// `keyRoot` (0=C..11=B). `complex`: false = triad (3 tones), true = 7th
// chord (4 tones). The chord's root lands in octave `anchorOctave` (this
// app's C(n) = 12n+12 convention, same as Scales.h), with the higher
// stacked tones climbing naturally above it (may spill into the next
// octave up, which is correct -- a chord voiced in root position often
// does).
Chord BuildChord(int keyRoot, int scaleIndex, int degree, bool complex, int anchorOctave);

} // namespace t8
