#pragma once
// C2 (CHORD LAB v2): "what fits in this place of the loop". No model, no statistics from songs: three things that can be measured on the
// notes, each shown as its own bar, plus one sentence built from the same facts.
//   FUNCTION  home / away / tension, from the degree of the chord in the key (textbook functional harmony), and how that suits the chord
//             that comes next (tension before home is the strongest pull, and so on).
//   VOICES    how far the voices of the chord before have to move to reach this chord, and from it to the chord after (in semitones,
//             each voice to its nearest tone of the new chord, every tone used once where it can be).
//   MELODY    how many of the bass / synth notes under this place the chord holds (core/ChordLab.h LabFitOne).
// The same key, loop and notes always give the same answer.
#include "core/ChordLab.h"
#include <algorithm>
#include <array>
#include <numeric>
#include <string>
#include <vector>

namespace t8 {

enum class LabFn { Home = 0, Away = 1, Tension = 2 };

inline const char* LabFnName(LabFn f) { return f == LabFn::Home ? "home" : (f == LabFn::Away ? "away" : "tension"); }
inline float LabFnLevel(LabFn f) { return f == LabFn::Home ? 0.0f : (f == LabFn::Away ? 0.5f : 1.0f); }

// A dominant 7th (major third + minor seventh) or a diminished triad pulls, whatever its degree.
inline bool LabIsDominantShape(const LabChord& c) {
    bool m3 = false, M3 = false, m7 = false, b5 = false, p5 = false;
    for (int x : c.iv) { const int d = x % 12; m3 |= d == 3; M3 |= d == 4; m7 |= d == 10; b5 |= d == 6; p5 |= d == 7; }
    return (M3 && m7 && p5) || (m3 && b5 && !p5);
}

// Degrees I and III rest (home), II and IV move away, V and VII pull back (tension). VI is home in a major key (the relative minor) and
// away in a minor key (it leads to IV / V). A borrowed chord: on the tonic it is home; a dominant shape or a chord on the 5th / 7th
// degree pulls; any other borrowed colour moves away.
inline LabFn LabFunctionOf(const LabChord& c, int keyRoot, int scale) {
    const int semi = ((c.root - keyRoot) % 12 + 12) % 12;
    if (c.degree >= 1 && c.degree <= 7) {
        if (c.degree == 5 || c.degree == 7) return LabFn::Tension;
        if (LabIsDominantShape(c) && c.degree != 1) return LabFn::Tension;
        if (c.degree == 1 || c.degree == 3) return LabFn::Home;
        if (c.degree == 6) return LabMinorKey(scale) ? LabFn::Away : LabFn::Home;
        return LabFn::Away;
    }
    if (semi == 0 && !LabIsDominantShape(c)) return LabFn::Home;
    if (LabIsDominantShape(c) || semi == 7 || semi == 11) return LabFn::Tension;
    return LabFn::Away;
}

// ---- voices ---------------------------------------------------------------------------------------------------------------------------
struct LabVoiceMove {
    int semitones = 0;     // total movement of the voices that were there
    int stay = 0;          // voices that keep their note
    int half = 0;          // voices that move a half step
    int further = 0;       // voices that move a whole step or more
    int added = 0;         // tones of the new chord no voice reached (a voice is added)
};

inline int LabPcDist(int a, int b) { const int d = ((a - b) % 12 + 12) % 12; return std::min(d, 12 - d); }

// The smallest total movement from the pitch classes `from` to `to`: each old voice goes to a different new tone where there are enough
// tones (the best of every assignment is taken, at most 6 x 6 voices); left-over old voices go to their nearest new tone.
inline LabVoiceMove LabVoices(const std::vector<int>& from, const std::vector<int>& to) {
    LabVoiceMove best;
    if (from.empty() || to.empty()) { best.added = static_cast<int>(to.size()); return best; }
    const int m = static_cast<int>(from.size()), n = static_cast<int>(to.size());
    std::vector<int> idx(static_cast<size_t>(n));
    std::iota(idx.begin(), idx.end(), 0);
    int bestCost = 1 << 30;
    do {
        LabVoiceMove v;
        int cost = 0;
        for (int i = 0; i < m; ++i) {
            int d;
            if (i < n) d = LabPcDist(from[static_cast<size_t>(i)], to[static_cast<size_t>(idx[static_cast<size_t>(i)])]);
            else { d = 12; for (int t : to) d = std::min(d, LabPcDist(from[static_cast<size_t>(i)], t)); }
            cost += d;
            if (d == 0) ++v.stay; else if (d == 1) ++v.half; else ++v.further;
        }
        v.semitones = cost;
        v.added = std::max(0, n - m);
        if (cost < bestCost) { bestCost = cost; best = v; }
    } while (std::next_permutation(idx.begin(), idx.end()));
    return best;
}

// ---- the advice for one place of the loop ---------------------------------------------------------------------------------------------
enum class LabAdviceMode { Best = 0, Smooth = 1, MoreTension = 2, NewColour = 3 };

struct LabAdvice {
    const LabChord* chord = nullptr;
    LabFn fn = LabFn::Home;
    float fnFit = 0.0f;         // 0..1: how the function suits the chord after this place
    std::string fnWords;        // "tension · wants Am"
    LabVoiceMove in, out;       // from the chord before / to the chord after
    bool haveIn = false, haveOut = false;
    float voiceFit = 0.0f;      // 0..1: 1 = nothing moves
    int covered = 0, total = 0; // melody notes under the place that the chord holds / all of them
    float melFit = 0.0f;        // 0..1 (LabFitOne)
    bool inLoop = false;        // the chord is already somewhere in the loop
    float score = 0.0f;         // what the list is sorted by, in the chosen mode
};

struct LabSlot {
    const LabChord* prev = nullptr;   // the chord before this place (the loop comes round)
    const LabChord* next = nullptr;   // the chord after it
    LabMelody melody;                 // the notes under this place
    std::vector<unsigned> loopMasks;  // the pitch-class sets already in the loop (NEW COLOUR)
};

inline unsigned LabMask(const LabChord& c) { unsigned m = 0; for (int pc : c.Pcs()) m |= 1u << pc; return m; }
inline bool LabSameChord(const LabChord& a, const LabChord& b) { return a.root == b.root && LabMask(a) == LabMask(b); }

// The pull towards the next chord as a fact: the chord holds the note a half step under the next root (a leading tone), or its root is a
// fifth above the next root.
inline bool LabLeadsInto(const LabChord& c, const LabChord& next) {
    const unsigned m = LabMask(c);
    const int lead = (next.root + 11) % 12;
    return (m & (1u << lead)) != 0 || ((c.root - next.root + 12) % 12) == 7;
}

inline float LabFnFitBefore(LabFn f, const LabChord* next, int keyRoot, int scale) {
    if (!next) return f == LabFn::Home ? 0.8f : (f == LabFn::Away ? 0.6f : 0.5f);
    const LabFn nf = LabFunctionOf(*next, keyRoot, scale);
    static const float kFit[3][3] = {
        // next:   home  away  tension          this chord:
        {0.40f, 0.80f, 0.70f},                // home
        {0.70f, 0.50f, 1.00f},                // away
        {1.00f, 0.40f, 0.40f},                // tension
    };
    return kFit[static_cast<int>(f)][static_cast<int>(nf)];
}

inline LabAdvice LabAdviseOne(const LabChord& c, const LabSlot& slot, int keyRoot, int scale) {
    LabAdvice a;
    a.chord = &c;
    a.fn = LabFunctionOf(c, keyRoot, scale);
    a.fnFit = LabFnFitBefore(a.fn, slot.next, keyRoot, scale);
    const bool same = (slot.prev && LabSameChord(c, *slot.prev)) || (slot.next && LabSameChord(c, *slot.next));
    const bool sameRoot = !same && ((slot.prev && c.root == slot.prev->root) || (slot.next && c.root == slot.next->root));
    if (same) a.fnFit *= 0.5f;            // nothing changes there
    else if (sameRoot) a.fnFit *= 0.75f;  // the bass stays: only the colour changes
    a.fnWords = LabFnName(a.fn);
    if (slot.next && LabSameChord(c, *slot.next)) a.fnWords += " \xC2\xB7 the same as the next";
    else if (slot.prev && LabSameChord(c, *slot.prev)) a.fnWords += " \xC2\xB7 the same as before";
    else if (sameRoot) a.fnWords += std::string(" \xC2\xB7 same root as ") + (slot.prev && c.root == slot.prev->root ? slot.prev->name : slot.next->name);
    else if (a.fn == LabFn::Tension && slot.next && LabLeadsInto(c, *slot.next)) a.fnWords += " \xC2\xB7 wants " + slot.next->name;
    else if (a.fn == LabFn::Tension) a.fnWords += " \xC2\xB7 open, floats";
    else if (a.fn == LabFn::Away && slot.next && LabFunctionOf(*slot.next, keyRoot, scale) == LabFn::Tension) a.fnWords += " \xC2\xB7 leads on";
    else if (a.fn == LabFn::Away) a.fnWords += " \xC2\xB7 stays away";
    else a.fnWords += " \xC2\xB7 rests";
    const std::vector<int> pcs = c.Pcs();
    int moves = 0, counted = 0;
    if (slot.prev) { a.in = LabVoices(slot.prev->Pcs(), pcs); a.haveIn = true; moves += a.in.semitones; counted += static_cast<int>(slot.prev->Pcs().size()); }
    if (slot.next) { a.out = LabVoices(pcs, slot.next->Pcs()); a.haveOut = true; moves += a.out.semitones; counted += static_cast<int>(pcs.size()); }
    a.voiceFit = counted > 0 ? std::clamp(1.0f - static_cast<float>(moves) / (2.0f * static_cast<float>(counted)), 0.0f, 1.0f) : 0.5f;
    LabChord tmp = c;
    LabFitOne(tmp, slot.melody);
    a.covered = tmp.covered; a.total = slot.melody.notes; a.melFit = slot.melody.notes > 0 ? tmp.fit : 0.0f;
    const unsigned m = LabMask(c);
    for (unsigned lm : slot.loopMasks) if (lm == m) a.inLoop = true;
    return a;
}

inline float LabAdviceScore(const LabAdvice& a, LabAdviceMode mode, bool haveMelody) {
    const float mel = haveMelody ? a.melFit : 0.5f;
    switch (mode) {
        case LabAdviceMode::Smooth:      return 0.70f * a.voiceFit + 0.15f * a.fnFit + 0.15f * mel;
        case LabAdviceMode::MoreTension: return 0.60f * LabFnLevel(a.fn) + 0.25f * a.fnFit + 0.15f * mel - (a.chord && a.chord->iv.size() > 3 ? 0.0f : 0.02f);
        case LabAdviceMode::NewColour: {
            const float colour = (a.inLoop ? 0.0f : 0.5f) + (a.chord && (a.chord->row == 2 || a.chord->row == 3) ? 0.3f : (a.chord && a.chord->row == 1 ? 0.15f : 0.0f));
            return colour + 0.2f * mel + 0.1f * a.fnFit;
        }
        case LabAdviceMode::Best:
        default:                         return 0.40f * a.fnFit + 0.30f * a.voiceFit + 0.30f * mel;
    }
}

// Every chord of the palette for this place, sorted by the mode (best first). Ties keep the palette order (row, then column).
inline std::vector<LabAdvice> LabAdvise(const std::vector<LabChord>& palette, const LabSlot& slot, int keyRoot, int scale, LabAdviceMode mode) {
    std::vector<LabAdvice> out;
    out.reserve(palette.size());
    const bool haveMel = slot.melody.notes > 0;
    for (const LabChord& c : palette) {
        LabAdvice a = LabAdviseOne(c, slot, keyRoot, scale);
        a.score = LabAdviceScore(a, mode, haveMel);
        out.push_back(a);
    }
    std::stable_sort(out.begin(), out.end(), [](const LabAdvice& x, const LabAdvice& y) { return x.score > y.score; });
    return out;
}

// The sentence under the list: what the chord is in the key, how the voices move, what it does with the notes under it. Every clause is a
// count or a note name that can be checked.
inline std::string LabAdviceSentence(const LabAdvice& a, const LabSlot& slot, int keyRoot, int scale) {
    if (!a.chord) return std::string();
    const LabChord& c = *a.chord;
    const char* fnArticle = a.fn == LabFn::Away ? "an away chord" : (a.fn == LabFn::Home ? "a home chord" : "a tension chord");
    std::string s = c.name + " is " + fnArticle + " of " + LabKeyName(keyRoot, scale);
    if (c.degree == 0) s += std::string(", borrowed from the ") + (LabMinorKey(scale) ? "major" : "minor") + " side";
    if (slot.next && a.fn == LabFn::Tension && LabLeadsInto(c, *slot.next)) {
        const int lead = (slot.next->root + 11) % 12;
        if (LabMask(c) & (1u << lead)) s += std::string(": its ") + LabPcName(lead) + " is a half step under " + LabPcName(slot.next->root) + " and wants to go there, so the loop pulls on to " + slot.next->name;
        else s += std::string(": its root is a fifth above ") + LabPcName(slot.next->root) + ", so the loop pulls on to " + slot.next->name;
    }
    s += ".";
    if (a.haveIn && slot.prev) {
        const LabVoiceMove& v = a.in;
        auto count = [](int n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); };
        std::vector<std::string> parts;
        if (v.half) parts.push_back(count(v.half, "voice moves a half step", "voices move a half step"));
        if (v.further) parts.push_back(count(v.further, "voice moves further", "voices move further"));
        if (v.stay) parts.push_back(count(v.stay, "stays", "stay"));
        std::string p;
        for (size_t i = 0; i < parts.size(); ++i) p += (i == 0 ? "" : (i + 1 == parts.size() ? " and " : ", ")) + parts[i];
        if (!p.empty()) { p[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(p[0]))); s += " From " + slot.prev->name + ": " + p; }
        if (v.added) s += v.added == 1 ? ", one voice is added" : ", " + std::to_string(v.added) + " voices are added";
        s += ".";
    }
    if (slot.melody.notes > 0) {
        const unsigned m = LabMask(c);
        std::string in, rub;
        for (int pc = 0; pc < 12; ++pc) {
            if (slot.melody.count[static_cast<size_t>(pc)] == 0) continue;
            if (m & (1u << pc)) { if (!in.empty()) in += " "; in += LabPcName(pc); }
            else {
                for (int d : {1, 11}) {
                    const int t = (pc + d) % 12;
                    if (m & (1u << t)) { if (!rub.empty()) rub += ", "; rub += std::string(LabPcName(pc)) + " rubs against " + LabPcName(t); break; }
                }
            }
        }
        s += " It holds " + std::to_string(a.covered) + " of the " + std::to_string(slot.melody.notes) + " notes under this place";
        if (!in.empty()) s += " (" + in + ")";
        s += ".";
        if (!rub.empty()) s += " Half-step rub: " + rub + ".";
    }
    return s;
}

} // namespace t8
