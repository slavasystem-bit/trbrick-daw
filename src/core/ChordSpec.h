#pragma once
// Hand-built chords: a root note + a quality (maj, min, 7, ...) + an inversion -> MIDI notes (<= 4 voices),
// and the reverse (what is this set of notes?) for the chord editor in the CHAIN panel.
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace t8 {

constexpr int kNumChordQualities = 16;

struct ChordQualityDef { const char* suffix; const char* label; int n; std::array<int, 4> iv; };

inline const ChordQualityDef& ChordQuality(int q) {
    static const ChordQualityDef kQ[kNumChordQualities] = {
        {"",      "MAJ",   3, {0, 4, 7, 0}},
        {"m",     "MIN",   3, {0, 3, 7, 0}},
        {"dim",   "DIM",   3, {0, 3, 6, 0}},
        {"aug",   "AUG",   3, {0, 4, 8, 0}},
        {"sus2",  "SUS2",  3, {0, 2, 7, 0}},
        {"sus4",  "SUS4",  3, {0, 5, 7, 0}},
        {"maj7",  "MAJ7",  4, {0, 4, 7, 11}},
        {"m7",    "MIN7",  4, {0, 3, 7, 10}},
        {"7",     "DOM7",  4, {0, 4, 7, 10}},
        {"m7b5",  "M7b5",  4, {0, 3, 6, 10}},
        {"dim7",  "DIM7",  4, {0, 3, 6, 9}},
        {"6",     "6",     4, {0, 4, 7, 9}},
        {"m6",    "MIN6",  4, {0, 3, 7, 9}},
        {"mMaj7", "mMAJ7", 4, {0, 3, 7, 11}},
        {"add9",  "ADD9",  4, {0, 4, 7, 14}},
        {"7sus4", "7SUS4", 4, {0, 5, 7, 10}},
    };
    return kQ[std::clamp(q, 0, kNumChordQualities - 1)];
}

struct ChordSpec { int root = 48; int quality = 0; int inversion = 0; };   // root = MIDI note of the chord root

// Notes of the chord, ascending. Inversion k moves the k lowest notes up an octave.
inline std::vector<int> BuildChordNotes(const ChordSpec& sp) {
    const ChordQualityDef& d = ChordQuality(sp.quality);
    std::vector<int> v;
    for (int i = 0; i < d.n; ++i) v.push_back(sp.root + d.iv[i]);
    int inv = std::clamp(sp.inversion, 0, d.n - 1);
    for (int k = 0; k < inv; ++k) { v[k] += 12; }
    std::sort(v.begin(), v.end());
    for (int& x : v) x = std::clamp(x, 0, 127);
    return v;
}

// Recognises a note set. Returns false when it is not one of the known qualities (a "custom" chord).
inline bool IdentifyChord(const int8_t* notes, int n, ChordSpec& out) {
    if (n < 3 || n > 4) return false;
    std::vector<int> v(notes, notes + n); std::sort(v.begin(), v.end());
    // try every note as the root, prefer the lowest (root position) when several fit
    for (int ri = 0; ri < n; ++ri) {
        int rootPc = ((v[ri] % 12) + 12) % 12;
        for (int q = 0; q < kNumChordQualities; ++q) {
            const ChordQualityDef& d = ChordQuality(q);
            if (d.n != n) continue;
            unsigned want = 0, have = 0;
            for (int i = 0; i < d.n; ++i) want |= 1u << (d.iv[i] % 12);
            for (int x : v) have |= 1u << ((((x - rootPc) % 12) + 12) % 12);
            if (want == have) {
                out.quality = q;
                out.root = v[ri];                       // the root as it sits in the chord
                int below = 0; for (int x : v) if (x < v[ri]) ++below;
                out.inversion = below;
                // normalise the root to the lowest octave it could have in root position
                if (below > 0) out.root = v[ri] - 12 * 0;
                return true;
            }
        }
    }
    return false;
}

} // namespace t8
