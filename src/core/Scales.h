#pragma once
// Key/scale support for the melodic randomizers (bass + synth 2/3 on the
// RANDOM tab). 12 keys x 7 classic diatonic modes -- enough range to cover
// "typical" electronic-music melodic/bass material without turning this
// into a full music-theory engine.
#include <cstdint>
#include <vector>

namespace t8 {

constexpr int kNumKeys = 12;
extern const char* const kKeyNames[kNumKeys]; // "C", "C#", "D", ... "B"

// The 7 modes of the major scale, same 7 pitch classes rotated, plus a
// CHROMATIC option (all 12 semitones, no diatonic bias) for when you want
// the melodic randomizers to draw from every note instead of a 7-note
// subset. Dorian is the default (see AppState/Project::randomizerScale) --
// a natural minor feel with a major-6th brightness that sits well under
// both house/techno leads and darker basslines, a common default in
// step-sequencer/groovebox randomizers.
constexpr int kNumScales = 13;
// Index of the CHROMATIC entry -- called out by name because it needs
// special handling anywhere "scale degree" implies diatonic tertian
// harmonization (stacking every OTHER scale tone into thirds), which has
// no coherent meaning across 12 equally-spaced semitones the way it does
// across a 7-note diatonic scale. See Chords.cpp's BuildChord for where
// this matters -- it substitutes Ionian for chord-building purposes
// whenever this scale is selected, since "diatonic chords in a chromatic
// scale" isn't a real music-theory concept, while BuildScaleNotes/
// BuildRootNotes (the melodic randomizers) use it completely normally --
// all 12 notes are exactly as valid a "scale" for picking random melody
// notes from as any of the 7 diatonic modes.
constexpr int kChromaticScaleIndex = 7;
extern const char* const kScaleNames[kNumScales]; // "MAJOR (IONIAN)", "DORIAN", ..., "MINOR (AEOLIAN)", ..., "CHROMATIC", then the v44.2 additions
// v44.2: indices 0..7 are the original eight and keep their meaning (saved projects store the index); 8..12 are new:
//   8 HARMONIC MINOR, 9 MELODIC MINOR (ascending), 10 PENTATONIC MAJOR, 11 PENTATONIC MINOR, 12 BLUES (minor pentatonic + b5).
// kScaleMenuOrder is the order the header's scale picker shows them in: MAJOR and MINOR first, then the other modes, the
// minors, the pentatonics / blues, CHROMATIC last.
constexpr int kScaleIonian = 0, kScaleAeolian = 5, kScalePentMajor = 10, kScalePentMinor = 11;
extern const int kScaleMenuOrder[kNumScales];
// The seven-note scale the CHORDS are stacked from when `scaleIndex` is not heptatonic (pentatonics, blues, chromatic have no
// diatonic thirds to stack): major pentatonic -> MAJOR, minor pentatonic / blues -> MINOR, chromatic -> MAJOR. A seven-note scale
// (every mode, harmonic and melodic minor) harmonises itself.
int ChordScaleFor(int scaleIndex);
// Semitone offsets from the root, one row per scale. Sized for the widest
// scale (CHROMATIC, 12 tones) -- the 7 diatonic modes only populate their
// first kScaleDegreeCount[i] (== 7) entries, see below. Column count is
// intentionally NOT a second `kNumScales`-shaped constant to avoid a
// confusing kNumScales-vs-"notes per scale" name collision.
extern const int kScaleIntervals[kNumScales][12];
// How many of each scale's kScaleIntervals row are actually populated --
// 7 for the diatonic modes, 12 for CHROMATIC. BuildScaleNotes (below) and
// Chords.cpp's ScaleDegreeSemitone both iterate/wrap using this instead of
// a hardcoded 7, so CHROMATIC's scale-degree math (a "degree" advancing by
// exactly one semitone, wrapping every 12 instead of every 7) falls out
// automatically rather than needing a special case in either function.
extern const int kScaleDegreeCount[kNumScales];

// Builds every note of `scaleIndex` in key `keyRoot` (0=C..11=B) that falls
// within octave band [octaveLow, octaveHigh] inclusive, using this project's
// existing octave-label convention (see AppState::RandomizeMelodic): octave
// N spans MIDI [12*N+12, 12*N+23], so octave 1 = MIDI 24-35 ("C1"..), octave
// 4 = MIDI 60-71 ("C4" = 60 = MIDI middle C), matching Step::note's existing
// "60 = C4" comment. Returned notes are sorted ascending.
// Nearest note of the scale (ties go down); a note already in the scale is
// returned unchanged.
int SnapToScale(int note, int keyRoot, int scaleIndex);
std::vector<int> BuildScaleNotes(int keyRoot, int scaleIndex, int octaveLow, int octaveHigh);

// v44.3: the same, for an arbitrary SET of octaves (bit n of `octaveMask` = octave n, 0..kMaxRndOctave), e.g. octaves 1 and 3
// but not 2. Sorted ascending; empty when the mask has no valid bit.
constexpr int kMaxRndOctave = 7;
std::vector<int> BuildScaleNotesMask(int keyRoot, int scaleIndex, unsigned octaveMask);
// Mask of the octaves lo..hi inclusive (clamped to 0..kMaxRndOctave).
unsigned OctaveMaskRange(int lo, int hi);

// Just the root/tonic note in each octave of the band (e.g. C1, C2 for
// octaveLow=1, octaveHigh=2) -- used to weight "landing on the root" more
// often than a random scale degree, same idea the old hardcoded bass
// randomizer used.
std::vector<int> BuildRootNotes(int keyRoot, int octaveLow, int octaveHigh);

} // namespace t8
