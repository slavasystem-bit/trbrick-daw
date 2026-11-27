#pragma once
// C2 (CHORD LAB v2): the PLAY layer, the one performer of the chord track. A chord step says WHAT sounds (its notes and its length);
// the scene's ChordPlay says HOW: STACK, ARP, STRUM or RHYTHM, at a RATE, with a GATE, over OCTAVES, in an ORDER, with or without
// SMOOTH VOICES. The sequencer asks for the events of a chord one tick at a time (ChordPerfEvents with a window), the CHORD LAB
// draws the same events, and the selftest checks them. Pure functions, no allocation: safe on the audio thread.
#include "Types.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>

namespace t8 {

constexpr int kMaxPerfNotes = 18;   // ARP: 6 chord tones x 3 octaves

struct ChordPerfEvent {
    double at = 0.0;     // steps after the start of the chord
    int8_t voice = -1;   // voice of the chord pool; -1 with on == false = release every voice
    int8_t note = 60;
    bool on = false;
};

// One chord as the performer plays it.
struct ChordPerf {
    std::array<int8_t, kMaxChordVoices> notes{};   // the voiced chord, low to high (OCTAVES for STACK / STRUM / RHYTHM already added)
    int count = 0;
    ChordPlay play;
    double span = 1.0;    // steps the chord lasts (the length of its step)
    double rate = 1.0;    // steps per RATE
    float velocity = 0.8f;
    uint32_t strike = 0;  // how many chords were played before: UP-DN strums alternate, RND changes from chord to chord
};

inline uint32_t PerfHash(uint32_t a, uint32_t b) {   // a small fixed mix: the same chord plays the same "random" order every time
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2));
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}

// A few notes, low to high. An insertion sort with a fixed bound: std::sort on these small arrays makes GCC 15 warn (array-bounds)
// about a range it cannot prove, and the build treats warnings as errors.
template <class T>
inline void PerfSortSmall(T* a, int n) {
    n = std::clamp(n, 0, kMaxChordVoices);
    for (int i = 1; i < n; ++i) {
        const T v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; --j; }
        a[j + 1] = v;
    }
}

inline double ChordPerfRateSteps(const ChordPlay& p, int stepsPerBeat) {
    return std::max(0.25, ChordPlayRateBeats(p.rate) * static_cast<double>(std::max(1, stepsPerBeat)));
}

// SMOOTH VOICES: the inversion of `in` (low to high) nearest the chord that sounded before. Without a chord before, `in` stays.
inline int PerfSmooth(const int8_t* in, int n, const int8_t* prev, int prevN, int8_t* out) {
    n = std::clamp(n, 0, kMaxChordVoices);
    prevN = std::clamp(prevN, 0, kMaxChordVoices);
    for (int i = 0; i < n; ++i) out[i] = in[i];
    if (n <= 1 || prevN <= 0) return n;
    double prevMean = 0.0;
    for (int i = 0; i < prevN; ++i) prevMean += prev[i];
    prevMean /= prevN;
    int best[kMaxChordVoices] = {};
    double bestCost = 1e9;
    for (int r = -n; r <= n; ++r) {
        int c[kMaxChordVoices];
        for (int i = 0; i < n; ++i) c[i] = in[i];
        if (r > 0) for (int i = 0; i < r; ++i) c[i % n] += 12 * (1 + i / n);       // r lowest tones an octave up
        if (r < 0) for (int i = 0; i < -r; ++i) c[n - 1 - (i % n)] -= 12 * (1 + i / n);   // |r| highest tones an octave down
        PerfSortSmall(c, n);
        if (c[0] < 36 || c[n - 1] > 96) continue;
        double cost = 0.0;
        if (n == prevN) for (int i = 0; i < n; ++i) cost += std::abs(c[i] - prev[i]);
        else { double m = 0.0; for (int i = 0; i < n; ++i) m += c[i]; cost = std::abs(m / n - prevMean) * n; }
        cost += std::abs(r) * 0.01;   // a tie keeps the chord as written
        if (cost < bestCost) { bestCost = cost; for (int i = 0; i < n; ++i) best[i] = c[i]; }
    }
    if (bestCost < 1e8) for (int i = 0; i < n; ++i) out[i] = static_cast<int8_t>(best[i]);
    return n;
}

// The chord as the performer plays it: sorted, SMOOTH VOICES against `prev`, and for STACK / STRUM / RHYTHM the OCTAVES doubling
// (6 voices at most). ARP keeps the plain chord: it climbs the octaves itself.
inline int ChordPerfVoice(const int8_t* notes, int count, const ChordPlay& play, const int8_t* prev, int prevN, int8_t* out) {
    count = std::clamp(count, 0, kMaxChordVoices);
    int8_t s[kMaxChordVoices];
    for (int i = 0; i < count; ++i) s[i] = notes[i];
    PerfSortSmall(s, count);
    int n = count;
    if (play.smooth) PerfSmooth(s, count, prev, prevN, out); else for (int i = 0; i < count; ++i) out[i] = s[i];
    if (play.mode != static_cast<uint8_t>(ChordPlayMode::Arp))
        for (int o = 1; o < play.octaves; ++o)
            for (int i = 0; i < count && n < kMaxChordVoices; ++i)
                if (out[i] + 12 * o <= 108) out[n++] = static_cast<int8_t>(out[i] + 12 * o);
    PerfSortSmall(out, n);
    return n;
}

// Every event of the chord whose time lies in [lo, hi) (steps after the chord's start), in time order, a release before a start at
// the same moment. `emit(const ChordPerfEvent&)`.
template <class F>
inline void ChordPerfEvents(const ChordPerf& c, double lo, double hi, F&& emit) {
    if (c.count <= 0 || hi <= lo) return;
    const ChordPlayMode mode = static_cast<ChordPlayMode>(std::min<int>(c.play.mode, kNumChordPlayModes - 1));
    const double gate = std::clamp<int>(c.play.gate, 10, 100) / 100.0;
    const double span = std::max(0.25, c.span);
    const double rate = std::max(0.25, c.rate);
    const int n = std::clamp(c.count, 1, kMaxChordVoices);
    std::array<ChordPerfEvent, 64> buf;
    int nb = 0;
    auto put = [&](double at, int voice, int note, bool on) {
        if (at < lo || at >= hi || nb >= static_cast<int>(buf.size())) return;
        ChordPerfEvent e; e.at = at; e.voice = static_cast<int8_t>(voice); e.note = static_cast<int8_t>(std::clamp(note, 0, 127)); e.on = on;
        buf[static_cast<size_t>(nb++)] = e;
    };
    // only the hits that can touch [lo, hi): k from floor((lo - rate) / rate)
    auto firstHit = [&](double step) { return std::max(0, static_cast<int>((lo - step) / step) - 1); };
    switch (mode) {
        case ChordPlayMode::Stack:
            for (int i = 0; i < n; ++i) put(0.0, i, c.notes[static_cast<size_t>(i)], true);
            put(span * gate, -1, 0, false);
            break;
        case ChordPlayMode::Strum: {
            const double spread = std::min(rate, span * 0.5);
            const double dt = n > 1 ? spread / (n - 1) : 0.0;
            int ord[kMaxChordVoices];
            for (int i = 0; i < n; ++i) ord[i] = i;
            const int o = c.play.order;
            const bool down = o == 1 || (o == 2 && (c.strike & 1u));
            if (down) for (int i = 0; i < n / 2; ++i) std::swap(ord[i], ord[n - 1 - i]);
            if (o == 3) for (int i = n - 1; i > 0; --i) std::swap(ord[i], ord[PerfHash(c.strike, static_cast<uint32_t>(i)) % static_cast<uint32_t>(i + 1)]);
            for (int k = 0; k < n; ++k) put(k * dt, ord[k], c.notes[static_cast<size_t>(ord[k])], true);
            put(std::max(span * gate, spread + 0.01), -1, 0, false);
            break;
        }
        case ChordPlayMode::Rhythm:
            for (int k = firstHit(rate); k * rate < span && k * rate < hi; ++k) {
                const double t = k * rate;
                put(t, -1, 0, false);   // the hit before lets go (a release before a start at the same moment)
                for (int i = 0; i < n; ++i) put(t, i, c.notes[static_cast<size_t>(i)], true);
                put(std::min(t + rate * gate, span), -1, 0, false);
            }
            break;
        case ChordPlayMode::Arp: {
            int seq[kMaxPerfNotes];
            int len = 0;
            for (int oc = 0; oc < std::clamp<int>(c.play.octaves, 1, 3); ++oc)
                for (int i = 0; i < n && len < kMaxPerfNotes; ++i) seq[len++] = c.notes[static_cast<size_t>(i)] + 12 * oc;
            for (int k = firstHit(rate); k * rate < span && k * rate < hi; ++k) {
                int idx = 0;
                if (len > 1) {
                    switch (c.play.order) {
                        case 1: idx = len - 1 - k % len; break;
                        case 2: { const int period = 2 * (len - 1); const int m = k % period; idx = m < len ? m : period - m; break; }
                        case 3: idx = static_cast<int>(PerfHash(c.strike, static_cast<uint32_t>(k)) % static_cast<uint32_t>(len)); break;
                        default: idx = k % len; break;
                    }
                }
                const double t = k * rate;
                const int v = k % kMaxChordVoices;
                put(t, v, seq[idx], true);
                put(std::min(t + rate * gate, span), v, seq[idx], false);
            }
            break;
        }
    }
    auto before = [](const ChordPerfEvent& a, const ChordPerfEvent& b) { return a.at < b.at || (a.at == b.at && !a.on && b.on); };
    for (int i = 1; i < nb; ++i) {   // a stable insertion sort (a few dozen events at most)
        const ChordPerfEvent v = buf[static_cast<size_t>(i)];
        int j = i - 1;
        while (j >= 0 && before(v, buf[static_cast<size_t>(j)])) { buf[static_cast<size_t>(j + 1)] = buf[static_cast<size_t>(j)]; --j; }
        buf[static_cast<size_t>(j + 1)] = v;
    }
    for (int i = 0; i < nb; ++i) emit(buf[static_cast<size_t>(i)]);
}

} // namespace t8
