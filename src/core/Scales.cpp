#include "Scales.h"
#include <algorithm>

namespace t8 {

const char* const kKeyNames[kNumKeys] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

const char* const kScaleNames[kNumScales] = {
    "MAJOR (IONIAN)", "DORIAN", "PHRYGIAN", "LYDIAN", "MIXOLYDIAN", "MINOR (AEOLIAN)", "LOCRIAN", "CHROMATIC",
    "HARMONIC MINOR", "MELODIC MINOR", "PENTATONIC MAJOR", "PENTATONIC MINOR", "BLUES"
};

const int kScaleMenuOrder[kNumScales] = {0, 5, 1, 2, 3, 4, 6, 8, 9, 10, 11, 12, 7};

int ChordScaleFor(int scaleIndex) {
    if (scaleIndex < 0 || scaleIndex >= kNumScales) return kScaleIonian;
    if (scaleIndex == kScalePentMajor || scaleIndex == 7) return kScaleIonian;
    if (scaleIndex == kScalePentMinor || scaleIndex == 12) return kScaleAeolian;
    return scaleIndex;
}

// Standard 7 diatonic modes, each just the major scale's pitch classes
// starting from a different degree -- these specific interval sets are
// centuries-old, universally-taught music theory, not sourced from any
// particular product/codebase. Each row is padded out to 12 columns (see
// Scales.h) with unused trailing zeros -- BuildScaleNotes/
// ScaleDegreeSemitone only ever read the first kScaleDegreeCount[i]
// entries of a row, so the padding is never actually consulted; it exists
// purely so every row of this array has the same width. CHROMATIC's row
// is the one exception that uses all 12 columns for real.
const int kScaleIntervals[kNumScales][12] = {
    {0, 2, 4, 5, 7, 9, 11, 0, 0, 0, 0, 0},  // Ionian (major)
    {0, 2, 3, 5, 7, 9, 10, 0, 0, 0, 0, 0},  // Dorian
    {0, 1, 3, 5, 7, 8, 10, 0, 0, 0, 0, 0},  // Phrygian
    {0, 2, 4, 6, 7, 9, 11, 0, 0, 0, 0, 0},  // Lydian
    {0, 2, 4, 5, 7, 9, 10, 0, 0, 0, 0, 0},  // Mixolydian
    {0, 2, 3, 5, 7, 8, 10, 0, 0, 0, 0, 0},  // Aeolian (natural minor)
    {0, 1, 3, 5, 6, 8, 10, 0, 0, 0, 0, 0},  // Locrian
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, // Chromatic (all 12 semitones)
    {0, 2, 3, 5, 7, 8, 11, 0, 0, 0, 0, 0},  // Harmonic minor
    {0, 2, 3, 5, 7, 9, 11, 0, 0, 0, 0, 0},  // Melodic minor (ascending form)
    {0, 2, 4, 7, 9, 0, 0, 0, 0, 0, 0, 0},   // Pentatonic major
    {0, 3, 5, 7, 10, 0, 0, 0, 0, 0, 0, 0},  // Pentatonic minor
    {0, 3, 5, 6, 7, 10, 0, 0, 0, 0, 0, 0},  // Blues (minor pentatonic + the b5 "blue note")
};

// 7 for every diatonic mode, 12 for CHROMATIC -- see Scales.h.
const int kScaleDegreeCount[kNumScales] = {7, 7, 7, 7, 7, 7, 7, 12, 7, 7, 5, 5, 6};

namespace {
// C(octave N) in this app's existing label convention -- see
// AppState::RandomizeBass's old {24, 36} == "C1, C2" comment and
// Step::note's "60 = C4" comment: C(n) = 12*n + 12.
int CForOctave(int octave) { return 12 * octave + 12; }
}

int SnapToScale(int note, int keyRoot, int scaleIndex) {
    auto inScale = [&](int n) {
        int rel = (((n - keyRoot) % 12) + 12) % 12;
        for (int i = 0; i < kScaleDegreeCount[scaleIndex]; ++i)
            if (kScaleIntervals[scaleIndex][i] == rel) return true;
        return false;
    };
    for (int d = 0; d <= 6; ++d) {
        if (inScale(note - d)) return note - d;
        if (inScale(note + d)) return note + d;
    }
    return note;
}

std::vector<int> BuildScaleNotes(int keyRoot, int scaleIndex, int octaveLow, int octaveHigh) {
    std::vector<int> out;
    if (scaleIndex < 0 || scaleIndex >= kNumScales) scaleIndex = 1; // fall back to Dorian
    keyRoot = ((keyRoot % 12) + 12) % 12;
    int lo = CForOctave(octaveLow);
    int hi = CForOctave(octaveHigh) + 11; // through B of the top octave
    int numDegrees = kScaleDegreeCount[scaleIndex];
    for (int n = lo; n <= hi; ++n) {
        int pc = ((n % 12) + 12) % 12;
        for (int d = 0; d < numDegrees; ++d) {
            int iv = kScaleIntervals[scaleIndex][d];
            if (((keyRoot + iv) % 12) == pc) { out.push_back(n); break; }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<int> BuildScaleNotesMask(int keyRoot, int scaleIndex, unsigned octaveMask) {
    std::vector<int> out;
    for (int o = 0; o <= kMaxRndOctave; ++o) {
        if (!((octaveMask >> o) & 1u)) continue;
        const std::vector<int> part = BuildScaleNotes(keyRoot, scaleIndex, o, o);
        out.insert(out.end(), part.begin(), part.end());
    }
    std::sort(out.begin(), out.end());
    return out;
}

unsigned OctaveMaskRange(int lo, int hi) {
    lo = std::clamp(lo, 0, kMaxRndOctave); hi = std::clamp(hi, 0, kMaxRndOctave);
    unsigned m = 0;
    for (int o = lo; o <= hi; ++o) m |= 1u << o;
    return m;
}

std::vector<int> BuildRootNotes(int keyRoot, int octaveLow, int octaveHigh) {
    std::vector<int> out;
    keyRoot = ((keyRoot % 12) + 12) % 12;
    for (int oct = octaveLow; oct <= octaveHigh; ++oct) {
        out.push_back(CForOctave(oct) + keyRoot);
    }
    return out;
}

} // namespace t8
