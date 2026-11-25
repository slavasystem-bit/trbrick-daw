#pragma once
// RHYTHM randomizer (v44.4): a random rhythmic figure with a chosen NUMBER of notes per bar - down to a single note -
// instead of the melodic walk of SIMPLE / COMPLEX. The settings are kept per part (BASS, SYNTH 2, SYNTH 3) in the project.
// The step picking is a pure function here so it can be tested without the app.
#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

namespace t8 {

enum class RhythmPitch : uint8_t { Root = 0, RootFifth = 1, Chord = 2, Scale = 3 };    // which notes the figure plays
enum class RhythmLen : uint8_t { Short = 0, Held = 1, Mix = 2 };                      // 1 step / until the next note / random in between
constexpr int kRhythmPitchCount = 4;
constexpr int kRhythmLenCount = 3;
constexpr int kRhythmMaxNotes = 16;

struct RhythmRnd {
    uint8_t minNotes = 1;                 // notes per bar, 1..16 (a bar is 4 beats)
    uint8_t maxNotes = 3;
    RhythmPitch pitch = RhythmPitch::Root;
    RhythmLen length = RhythmLen::Mix;
    bool onTheOne = true;                 // the first note always sits on the first step of the bar
    bool sameEveryBar = true;             // one bar is drawn and repeated; off = every bar gets its own figure
};

inline RhythmRnd NormalizeRhythm(RhythmRnd c) {
    c.minNotes = static_cast<uint8_t>(std::clamp<int>(c.minNotes, 1, kRhythmMaxNotes));
    c.maxNotes = static_cast<uint8_t>(std::clamp<int>(c.maxNotes, c.minNotes, kRhythmMaxNotes));
    if (static_cast<int>(c.pitch) >= kRhythmPitchCount) c.pitch = RhythmPitch::Root;
    if (static_cast<int>(c.length) >= kRhythmLenCount) c.length = RhythmLen::Mix;
    return c;
}

inline const char* RhythmPitchName(RhythmPitch p) {
    switch (p) { case RhythmPitch::Root: return "ROOT"; case RhythmPitch::RootFifth: return "ROOT+5TH"; case RhythmPitch::Chord: return "CHORD"; default: return "SCALE"; }
}
inline const char* RhythmLenName(RhythmLen l) {
    switch (l) { case RhythmLen::Short: return "SHORT"; case RhythmLen::Held: return "HELD"; default: return "MIX"; }
}

// Picks `count` distinct steps of a bar of `barSteps` steps, sorted. The beats are favoured over the off-beat 8ths, those over
// the 16ths, so a figure with few notes sits on the beat while one with many fills the gaps. forceFirst puts step 0 in.
inline std::vector<int> PickRhythmSteps(std::mt19937& rng, int barSteps, int count, bool forceFirst) {
    barSteps = std::max(1, barSteps);
    count = std::clamp(count, 1, barSteps);
    const int beat = std::max(1, barSteps / 4);
    std::vector<int> weight(static_cast<size_t>(barSteps));
    for (int i = 0; i < barSteps; ++i) weight[static_cast<size_t>(i)] = (i % beat == 0) ? 6 : (beat >= 2 && i % std::max(1, beat / 2) == 0) ? 3 : 1;
    std::vector<int> out;
    if (forceFirst) { out.push_back(0); weight[0] = 0; }
    while (static_cast<int>(out.size()) < count) {
        int total = 0;
        for (int w : weight) total += w;
        if (total <= 0) break;
        int r = std::uniform_int_distribution<int>(0, total - 1)(rng);
        for (int i = 0; i < barSteps; ++i) {
            if (r < weight[static_cast<size_t>(i)]) { out.push_back(i); weight[static_cast<size_t>(i)] = 0; break; }
            r -= weight[static_cast<size_t>(i)];
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace t8
