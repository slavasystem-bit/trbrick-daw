#pragma once
// v44.43 CHORD LAB core (no UI, no audio): the palette of chords that belong to a key (plus borrowed ones), how well each one fits the
// melody played by the bass / synth tracks, and the facts behind a chord (WHY: in the key or not, notes shared with the chord before, notes
// of the melody it holds). v44.46: no prediction of the next chord any more (NEXT was a formula, not a fact). Everything here is a fixed
// rule, not a model: the same key, scale, melody and chord always give the same answer, and every sentence is built from counts that can
// be checked on the notes.
#include "core/Scales.h"
#include "core/Types.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

namespace t8 {

// Spelling: keys that live on the flat side (F, Bb, Eb, Ab, Db, Gb major and their relatives and modes) name black keys with flats.
// BuildLabPalette sets it for the key it builds; everything that prints a note afterwards follows.
inline bool& LabFlatMode() { static bool flats = false; return flats; }
inline const char* LabPcName(int pc) {
    static const char* const kFl[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
    const int p = ((pc % 12) + 12) % 12;
    return LabFlatMode() ? kFl[p] : kKeyNames[p];
}

struct LabChord {
    int root = 0;                   // pitch class of the root
    std::vector<int> iv;            // semitones above the root, ascending (may pass 12 for 9ths / add9)
    std::string name;               // "Am7"
    std::string roman;              // "vi", "bVII", "V7"
    int degree = 0;                 // 1..7 = a chord of the key, 0 = borrowed
    int col = 0;                    // 0..6 column of the palette
    int row = 0;                    // 0 triads, 1 7ths (or 9ths), 2 sus / add, 3 borrowed
    bool inKey = true;              // every note belongs to the scale the chords are stacked from
    int kind = 0;                   // 0 major-ish, 1 minor-ish, 2 other (dim, aug, sus)
    float fit = 0.0f;               // 0..1 against the melody (ScoreLabChords)
    int covered = 0, total = 0;     // melody notes the chord contains / melody notes counted
    bool best = false;
    std::vector<int> Pcs() const { std::vector<int> v; for (int x : iv) v.push_back(((root + x) % 12 + 12) % 12); return v; }
    std::vector<int> Notes(int anchor = 48) const { std::vector<int> v; for (int x : iv) v.push_back(std::clamp(anchor + root + x, 0, 127)); return v; }
};

// ---- naming -------------------------------------------------------------------------------------------------------------------------
inline std::string LabSuffix(std::vector<int> iv, int* kind = nullptr) {
    std::sort(iv.begin(), iv.end());
    struct P { std::vector<int> iv; const char* s; int kind; };
    static const std::vector<P> kP = {
        {{0, 4, 7}, "", 0},          {{0, 3, 7}, "m", 1},          {{0, 3, 6}, "dim", 2},          {{0, 4, 8}, "aug", 2},
        {{0, 2, 7}, "sus2", 2},      {{0, 5, 7}, "sus4", 2},
        {{0, 4, 7, 11}, "maj7", 0},  {{0, 3, 7, 10}, "m7", 1},     {{0, 4, 7, 10}, "7", 0},        {{0, 3, 6, 10}, "m7b5", 2},
        {{0, 3, 6, 9}, "dim7", 2},   {{0, 3, 7, 11}, "mMaj7", 1},  {{0, 4, 8, 11}, "maj7#5", 0},   {{0, 4, 7, 14}, "add9", 0},
        {{0, 3, 7, 14}, "madd9", 1}, {{0, 5, 7, 10}, "7sus4", 2}, {{0, 4, 7, 9}, "6", 0}, {{0, 3, 7, 9}, "m6", 1},
        {{0, 4, 7, 11, 14}, "maj9", 0}, {{0, 3, 7, 10, 14}, "m9", 1}, {{0, 4, 7, 10, 14}, "9", 0}, {{0, 3, 6, 10, 14}, "m9b5", 2},
        {{0, 3, 7, 11, 14}, "mMaj9", 1}, {{0, 4, 8, 11, 14}, "maj9#5", 0},
        {{0, 3, 7, 10, 13}, "m7b9", 1}, {{0, 4, 7, 10, 13}, "7b9", 0}, {{0, 3, 6, 10, 13}, "m7b5b9", 2}, {{0, 4, 7, 11, 13}, "maj7b9", 0},
    };
    for (const P& p : kP) if (p.iv == iv) { if (kind) *kind = p.kind; return p.s; }
    if (kind) *kind = 2;
    return "?";
}

inline LabChord MakeLabChord(int root, std::vector<int> iv) {
    LabChord c; c.root = ((root % 12) + 12) % 12; c.iv = std::move(iv);
    c.name = std::string(LabPcName(c.root)) + LabSuffix(c.iv, &c.kind);
    return c;
}

// ---- key membership -----------------------------------------------------------------------------------------------------------------
inline unsigned LabScaleMask(int keyRoot, int scale) {
    const int sc = ChordScaleFor(scale);
    unsigned m = 0;
    for (int i = 0; i < kScaleDegreeCount[sc]; ++i) m |= 1u << ((keyRoot + kScaleIntervals[sc][i]) % 12);
    return m;
}
inline bool LabMinorKey(int scale) { const int sc = ChordScaleFor(scale); return kScaleIntervals[sc][2] == 3; }   // the third above the tonic decides
// The parent major scale of the key decides the spelling: find the major key whose seven notes these are.
inline void LabSetSpelling(int keyRoot, int scale) {
    const unsigned mask0 = LabScaleMask(keyRoot, scale);
    int parent = -1;
    for (int p = 0; p < 12 && parent < 0; ++p) { unsigned m = 0; for (int i = 0; i < 7; ++i) m |= 1u << ((p + kScaleIntervals[kScaleIonian][i]) % 12); if (m == mask0) parent = p; }
    if (parent < 0) parent = LabMinorKey(scale) ? (keyRoot + 3) % 12 : keyRoot;   // harmonic / melodic minor: the relative major
    LabFlatMode() = parent == 5 || parent == 10 || parent == 3 || parent == 8 || parent == 1 || parent == 6;
}

inline const char* LabRomanDegree(int degree, bool upper) {
    static const char* const kUp[7] = {"I", "II", "III", "IV", "V", "VI", "VII"};
    static const char* const kLo[7] = {"i", "ii", "iii", "iv", "v", "vi", "vii"};
    const int k = std::clamp(degree, 1, 7) - 1;
    return upper ? kUp[k] : kLo[k];
}

// The palette. 7 columns (degrees) x rows: 0 triads, 1 sevenths (nine = true: ninths), 2 sus / add, 3 borrowed (only some columns).
inline std::vector<LabChord> BuildLabPalette(int keyRoot, int scale, bool nine) {
    const int sc = ChordScaleFor(scale);
    LabSetSpelling(keyRoot, scale);
    const int n = std::max(7, kScaleDegreeCount[sc]);
    auto tone = [&](int k) { return kScaleIntervals[sc][k % 7] + 12 * (k / 7); };   // k-th scale step above the tonic, any octave
    const unsigned mask = LabScaleMask(keyRoot, scale);
    auto inKey = [&](const LabChord& c) { for (int pc : c.Pcs()) if (!(mask & (1u << pc))) return false; return true; };
    (void)n;
    std::vector<LabChord> out;
    for (int d = 0; d < 7; ++d) {
        const int base = tone(d);
        auto rel = [&](int k) { return tone(d + k) - base; };
        std::vector<int> tri = {0, rel(2), rel(4)};
        std::vector<int> sev = {0, rel(2), rel(4), rel(6)};
        if (nine) sev.push_back(rel(8));
        LabChord a = MakeLabChord(keyRoot + base, tri);
        LabChord b = MakeLabChord(keyRoot + base, sev);
        // sus / add: a major triad gets its 9th, a minor one the sus2 (or sus4 when only that one stays in the key), a diminished one sus4
        std::vector<int> sa;
        if (a.kind == 0) sa = {0, rel(2), rel(4), rel(1) + 12};
        else if (a.kind == 1) {
            const std::vector<int> s2 = {0, rel(1), rel(4)}, s4 = {0, rel(3), rel(4)};
            LabChord t2 = MakeLabChord(keyRoot + base, s2);
            sa = (rel(1) == 2 || !inKey(MakeLabChord(keyRoot + base, s4))) ? s2 : s4;
            (void)t2;
        } else sa = {0, 5, 7};                       // a diminished chord has no sus of its own: the usual sus4 (it will sit outside the key)
        LabChord c = MakeLabChord(keyRoot + base, sa);
        for (LabChord* x : {&a, &b, &c}) {
            x->degree = d + 1; x->col = d; x->inKey = inKey(*x);
        }
        a.row = 0; b.row = 1; c.row = 2;
        const bool upper = a.kind == 0;
        a.roman = LabRomanDegree(d + 1, upper) + std::string(a.kind == 2 && a.iv[2] == 6 ? "o" : "");
        b.roman = a.roman + (nine ? "9" : "7"); c.roman = a.roman + "*";
        out.push_back(a); out.push_back(b); out.push_back(c);
    }
    // borrowed: the other colour of the same tonic (a minor key borrows major chords, a major key borrows minor ones)
    struct Br { int semi; std::vector<int> iv; int col; const char* roman; };
    static const std::vector<Br> kMinorKey = {{0, {0, 4, 7}, 0, "I"}, {1, {0, 4, 7}, 1, "bII"}, {5, {0, 4, 7}, 3, "IV"}, {7, {0, 4, 7, 10}, 4, "V7"}, {11, {0, 3, 6}, 6, "viio"}};
    static const std::vector<Br> kMajorKey = {{0, {0, 3, 7}, 0, "i"}, {2, {0, 4, 7, 10}, 1, "II7"}, {3, {0, 4, 7}, 2, "bIII"}, {5, {0, 3, 7}, 3, "iv"}, {8, {0, 4, 7}, 5, "bVI"}, {10, {0, 4, 7}, 6, "bVII"}};
    for (const Br& b : LabMinorKey(scale) ? kMinorKey : kMajorKey) {
        LabChord c = MakeLabChord(keyRoot + b.semi, b.iv);
        c.inKey = inKey(c);
        if (c.inKey) continue;                                   // it is already in the main rows
        c.degree = 0; c.col = b.col; c.row = 3; c.roman = b.roman;
        if (c.roman[0] == 'b') { static const char* const kFlat[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"}; c.name = std::string(kFlat[c.root]) + LabSuffix(c.iv); }
        out.push_back(c);
    }
    return out;
}

// ---- the melody ---------------------------------------------------------------------------------------------------------------------
struct LabMelody {
    std::array<double, 12> w{};     // weight per pitch class (bass notes on the beat count double)
    std::array<int, 12> count{};    // how many notes of each pitch class
    int notes = 0;
    double total = 0.0;
    void Add(int note, double weight) { const int pc = ((note % 12) + 12) % 12; w[static_cast<size_t>(pc)] += weight; count[static_cast<size_t>(pc)]++; ++notes; total += weight; }
};

// Notes of the given tracks (bass / synths) in bars [barFrom, barTo] (0-based) of the scene. A track that is shorter than the range repeats.
inline LabMelody LabMelodyFromPattern(const Pattern& pat, const std::vector<int>& tracks, int barFrom, int barTo) {
    LabMelody m;
    const int perBar = std::max(1, 4 * pat.stepsPerBeat);
    for (int ti : tracks) {
        if (ti < 0 || ti >= kNumTracks) continue;
        const Track& t = pat.tracks[static_cast<size_t>(ti)];
        if (t.type == TrackType::Chord || t.type == TrackType::Drum || t.type == TrackType::Sampler) continue;
        const int len = std::max(1, t.useOwnLength ? t.length : pat.masterLength);
        const bool bass = ti == kNumDrumTracks;
        for (int i = barFrom * perBar; i < (barTo + 1) * perBar; ++i) {
            const Step& s = t.steps[static_cast<size_t>((i % len) % kMaxSteps)];
            if (!s.active || s.chordSize > 0) continue;
            const bool onBeat = (i % std::max(1, pat.stepsPerBeat)) == 0;
            m.Add(s.note, (bass && onBeat) ? 2.0 : 1.0);
        }
    }
    return m;
}

// v44.46: the same for a stretch of steps [stepFrom, stepTo) of the scene (the notes under one block of the jam loop). A track shorter than
// the range repeats.
inline LabMelody LabMelodyFromRange(const Pattern& pat, const std::vector<int>& tracks, int stepFrom, int stepTo) {
    LabMelody m;
    for (int ti : tracks) {
        if (ti < 0 || ti >= kNumTracks) continue;
        const Track& t = pat.tracks[static_cast<size_t>(ti)];
        if (t.type == TrackType::Chord || t.type == TrackType::Drum || t.type == TrackType::Sampler) continue;
        const int len = std::max(1, t.useOwnLength ? t.length : pat.masterLength);
        const bool bass = ti == kNumDrumTracks;
        for (int i = std::max(0, stepFrom); i < stepTo; ++i) {
            const Step& s = t.steps[static_cast<size_t>((i % len) % kMaxSteps)];
            if (!s.active || s.chordSize > 0) continue;
            const bool onBeat = (i % std::max(1, pat.stepsPerBeat)) == 0;
            m.Add(s.note, (bass && onBeat) ? 2.0 : 1.0);
        }
    }
    return m;
}

// Fit 0..1: the share of the melody (by weight) that the chord contains, minus a little for every melody note that sits a semitone above a
// chord tone without being in the chord (a rub). Then the best one or two of each row get the star.
inline void LabFitOne(LabChord& c, const LabMelody& mel) {
    c.fit = 0; c.covered = 0; c.total = mel.notes; c.best = false;
    if (mel.total <= 0.0) return;
    unsigned have = 0; for (int pc : c.Pcs()) have |= 1u << pc;
    double in = 0.0, rub = 0.0;
    for (int pc = 0; pc < 12; ++pc) {
        if (mel.w[static_cast<size_t>(pc)] <= 0.0) continue;
        if (have & (1u << pc)) { in += mel.w[static_cast<size_t>(pc)]; c.covered += mel.count[static_cast<size_t>(pc)]; }
        else if (have & (1u << ((pc + 11) % 12))) rub += mel.w[static_cast<size_t>(pc)];
    }
    c.fit = static_cast<float>(std::clamp((in - 0.5 * rub) / mel.total, 0.0, 1.0));
}
inline void ScoreLabChords(std::vector<LabChord>& chords, const LabMelody& mel) {
    for (LabChord& c : chords) LabFitOne(c, mel);
    if (mel.total <= 0.0) return;
    for (int row = 0; row < 4; ++row) {
        std::vector<LabChord*> r; for (LabChord& c : chords) if (c.row == row) r.push_back(&c);
        std::sort(r.begin(), r.end(), [](const LabChord* a, const LabChord* b) { return a->fit > b->fit; });
        for (size_t i = 0; i < r.size() && i < 2; ++i) if (r[i]->fit >= 0.6f && (i == 0 || r[i]->fit >= r[0]->fit - 0.05f)) r[i]->best = true;
    }
}

// ---- WHY ----------------------------------------------------------------------------------------------------------------------------
inline int LabCommon(const LabChord& a, const LabChord& b) { int n = 0; unsigned m = 0; for (int pc : b.Pcs()) m |= 1u << pc; for (int pc : a.Pcs()) if (m & (1u << pc)) ++n; return n; }
inline std::string LabCommonNames(const LabChord& a, const LabChord& b) {
    unsigned m = 0; for (int pc : b.Pcs()) m |= 1u << pc;
    std::string s; for (int pc : a.Pcs()) if (m & (1u << pc)) { if (!s.empty()) s += " "; s += LabPcName(pc); }
    return s;
}
inline std::string LabKeyName(int keyRoot, int scale) {
    // the real name of the scale ("c dorian", "a minor"), not just major / minor: C dorian is not C minor
    std::string sn = kScaleNames[std::clamp(scale, 0, kNumScales - 1)];
    const size_t par = sn.find(" (");
    if (par != std::string::npos) sn.resize(par);
    for (char& ch : sn) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return std::string(LabPcName(keyRoot)) + " " + sn;
}
// The place of the chord in the key, as a fact: its degree (or that it is borrowed), no opinion about what it "wants".
inline std::string LabPlace(const LabChord& c, int keyRoot, int scale) {
    if (c.degree == 0) return std::string("It is not a chord built on a degree of ") + LabKeyName(keyRoot, scale) + ": borrowed from the " + (LabMinorKey(scale) ? "major" : "minor") + " side of the same tonic.";
    return std::string("It is built on degree ") + LabRomanDegree(c.degree, true) + " of " + LabKeyName(keyRoot, scale) + " (" + LabPcName(c.root) + ").";
}

// `scope` says which notes `mel` holds: "under this block" or "in the scene".
inline std::vector<std::string> LabWhy(const LabChord& c, const LabChord* prev, int keyRoot, int scale, const LabMelody& mel, const std::string& scope = "in the scene") {
    std::vector<std::string> out;
    const unsigned mask = LabScaleMask(keyRoot, scale);
    std::string outside;
    for (int pc : c.Pcs()) if (!(mask & (1u << pc))) { if (!outside.empty()) outside += " "; outside += LabPcName(pc); }
    std::string notes; for (int pc : c.Pcs()) { if (!notes.empty()) notes += " "; notes += LabPcName(pc); }
    if (outside.empty()) out.push_back("Every note (" + notes + ") is in " + LabKeyName(keyRoot, scale) + ".");
    else out.push_back(outside + " is outside " + LabKeyName(keyRoot, scale) + " (the chord has " + notes + ").");
    out.push_back(LabPlace(c, keyRoot, scale));
    if (prev) {
        const int k = LabCommon(c, *prev);
        const int n = static_cast<int>(c.iv.size());
        if (k >= 1) out.push_back("After " + prev->name + " it shares " + std::to_string(k) + (k == 1 ? " note (" : " notes (") + LabCommonNames(c, *prev) + "); the other " + std::to_string(std::max(0, n - k)) + (n - k == 1 ? " is new." : " are new."));
        else out.push_back("After " + prev->name + " it shares no note: all " + std::to_string(n) + " notes are new.");
    }
    if (mel.notes > 0) {
        std::string miss; int missN = 0;
        unsigned have = 0; for (int pc : c.Pcs()) have |= 1u << pc;
        for (int pc = 0; pc < 12; ++pc) if (mel.count[static_cast<size_t>(pc)] > 0 && !(have & (1u << pc))) { if (!miss.empty()) miss += " "; miss += LabPcName(pc); missN += mel.count[static_cast<size_t>(pc)]; }
        std::string s = "It holds " + std::to_string(c.covered) + " of the " + std::to_string(mel.notes) + " bass / synth notes " + scope + ".";
        if (!miss.empty()) s += " Not in the chord: " + miss + " (" + std::to_string(missN) + (missN == 1 ? " note)." : " notes).");
        out.push_back(s);
    } else out.push_back(std::string("No bass / synth notes ") + scope + " to compare with (press MATCH to read them).");
    return out;
}

// What chord sits on a step: the name for the block in the jam loop. Unknown shapes get the root and a count.
inline LabChord LabChordFromNotes(const int8_t* notes, int n) {
    std::vector<int> v(notes, notes + std::max(0, n)); std::sort(v.begin(), v.end());
    if (v.empty()) return LabChord{};
    // try every note as the root; the first one that gives a known name wins (the lowest first)
    for (size_t r = 0; r < v.size(); ++r) {
        std::vector<int> iv; for (int x : v) { int d = x - v[r]; while (d < 0) d += 12; iv.push_back(d); }
        std::sort(iv.begin(), iv.end());
        // fold a 2nd / 4th / 6th into the 9th / 11th only for the 9 chords; plain stacking otherwise
        LabChord c = MakeLabChord(v[r], iv);
        if (c.name.find('?') == std::string::npos) return c;
        std::vector<int> iv2 = iv; for (int& d : iv2) if (d == 2) d = 14; std::sort(iv2.begin(), iv2.end());
        LabChord c2 = MakeLabChord(v[r], iv2);
        if (c2.name.find('?') == std::string::npos) return c2;
    }
    LabChord c = MakeLabChord(v[0], {0}); c.name = std::string(LabPcName(v[0])) + "(" + std::to_string(v.size()) + ")"; c.kind = 2; return c;
}

}  // namespace t8
