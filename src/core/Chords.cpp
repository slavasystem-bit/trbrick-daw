#include "Chords.h"
#include "Scales.h"
#include <algorithm>

namespace t8 {

namespace {

// Same C(octave N) convention as Scales.cpp's own (private) CForOctave --
// duplicated here rather than exported from Scales.cpp, same "small
// duplication over a premature shared-helper header" precedent already
// used for FMSynth/WavetableSynth's near-identical Render() bodies.
int CForOctave(int octave) { return 12 * octave + 12; }

// Semitone offset from the key root for the scale degree at raw index
// `degreeIndex0` (0-based, NOT wrapped -- may be > (degree count - 1),
// meaning "climb an extra octave", which is exactly what stacking thirds
// past the last degree needs: e.g. for a 7-degree scale, degree index 8 =
// the 2nd degree, one octave up). Wraps using THIS scale's own degree
// count (see kScaleDegreeCount in Scales.h) rather than a hardcoded 7, so
// this works correctly for CHROMATIC's 12 degrees too -- there, "degree N"
// literally means "N semitones up", one degree per semitone.
int ScaleDegreeSemitone(int scaleIndex, int degreeIndex0) {
    int n = kScaleDegreeCount[scaleIndex];
    int oct = degreeIndex0 / n;
    int idx = degreeIndex0 % n;
    return oct * 12 + kScaleIntervals[scaleIndex][idx];
}

// Labels a triad from the semitone gaps between its 3 stacked tones
// (third1 = tone1-tone0, third2 = tone2-tone1), then optionally refines it
// into a 7th-chord label using the root-to-7th interval (mod 12 -- always
// 10 or 11 for a diatonic 7th stacked from any of Scales.h's 7 modes,
// since these are all rotations of the plain major scale with no altered/
// harmonic-minor leading tone available).
const char* QualityLabel(int third1, int third2, bool complex, int seventhIntervalMod12) {
    // Base triad quality from the two stacked-third sizes.
    enum class Triad { Maj, Min, Dim, Aug, Other } triad = Triad::Other;
    if (third1 == 4 && third2 == 3) triad = Triad::Maj;
    else if (third1 == 3 && third2 == 4) triad = Triad::Min;
    else if (third1 == 3 && third2 == 3) triad = Triad::Dim;
    else if (third1 == 4 && third2 == 4) triad = Triad::Aug;

    if (!complex) {
        switch (triad) {
            case Triad::Maj: return "MAJ";
            case Triad::Min: return "MIN";
            case Triad::Dim: return "DIM";
            case Triad::Aug: return "AUG";
            default:         return "?";
        }
    }

    bool maj7th = (seventhIntervalMod12 == 11);
    switch (triad) {
        case Triad::Maj: return maj7th ? "MAJ7" : "DOM7";
        case Triad::Min: return maj7th ? "MINMAJ7" : "MIN7"; // MINMAJ7 is rare but honest
        case Triad::Dim: return maj7th ? "DIM(MAJ7)" : "M7B5"; // half-diminished, the common diatonic case
        case Triad::Aug: return "AUG7";
        default:         return "?7";
    }
}

} // namespace

Chord BuildChord(int keyRoot, int scaleIndex, int degree, bool complex, int anchorOctave) {
    scaleIndex = std::clamp(scaleIndex, 0, kNumScales - 1);
    // Only a seven-note scale has diatonic thirds to stack (every other degree). Chromatic, the pentatonics and the blues scale
    // are harmonised from their parent major / minor scale instead (see ChordScaleFor in Scales.h), so the CHORDS tab always
    // produces real triads and sevenths. The melodic randomizers (BuildScaleNotes) use the chosen scale itself.
    scaleIndex = ChordScaleFor(scaleIndex);
    keyRoot = ((keyRoot % 12) + 12) % 12;
    int d0 = ((degree - 1) % 7 + 7) % 7; // normalize to 0..6

    // Stack every-other scale degree: root, 3rd, 5th, (7th if complex).
    std::vector<int> degreeIndices = {d0, d0 + 2, d0 + 4};
    if (complex) degreeIndices.push_back(d0 + 6);

    Chord chord;
    int rootSemitone = ScaleDegreeSemitone(scaleIndex, d0);
    // anchorBase is the ACTUAL pitch of this chord's root -- the key's
    // tonic in the anchor octave, shifted UP by the scale degree's own
    // semitone offset (rootSemitone). Without that +rootSemitone term,
    // every chord's root collapses back onto the key's tonic regardless
    // of which degree was requested (e.g. a IV chord in C would come out
    // as C-E-G instead of F-A-C -- same triad shape, wrong pitch) -- this
    // was a real bug, not just a naming/interval quirk: relToRoot below is
    // 0 for the root tone itself, so omitting +rootSemitone here silently
    // discarded the degree's own transposition entirely.
    int anchorBase = CForOctave(anchorOctave) + keyRoot + rootSemitone;
    for (int di : degreeIndices) {
        int semitone = ScaleDegreeSemitone(scaleIndex, di);
        // Relative to the root tone's own semitone (not the raw key root),
        // so the chord always climbs upward from its root regardless of
        // which degree it started on.
        int relToRoot = semitone - rootSemitone;
        chord.notes.push_back(anchorBase + relToRoot);
    }
    std::sort(chord.notes.begin(), chord.notes.end());
    chord.rootNote = chord.notes.empty() ? anchorBase : chord.notes.front();

    int t0 = ScaleDegreeSemitone(scaleIndex, d0);
    int t1 = ScaleDegreeSemitone(scaleIndex, d0 + 2);
    int t2 = ScaleDegreeSemitone(scaleIndex, d0 + 4);
    int third1 = t1 - t0;
    int third2 = t2 - t1;
    int seventhMod12 = 0;
    if (complex) {
        int t3 = ScaleDegreeSemitone(scaleIndex, d0 + 6);
        seventhMod12 = ((t3 - t0) % 12 + 12) % 12;
    }
    chord.quality = QualityLabel(third1, third2, complex, seventhMod12);
    return chord;
}

} // namespace t8
