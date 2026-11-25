#include "ChordProgressions.h"

namespace t8 {

const std::array<ChordProgressionData, kNumChordProgressions>& AllChordProgressions() {
    static const std::array<ChordProgressionData, kNumChordProgressions> kAll = {{
        // ---- The essentials -------------------------------------------
        {"1-4-5-1",       {1, 4, 5, 1}},       // the oldest cadence there is
        {"1-5-6-4",       {1, 5, 6, 4}},       // "the four chords" -- probably
                                                // the single most-used pop
                                                // progression of the last 60
                                                // years
        {"6-4-1-5",       {6, 4, 1, 5}},       // same 4 chords, minor-feel start
        {"1-6-4-5",       {1, 6, 4, 5}},       // the 50s/doo-wop progression
        {"1-6-2-5",       {1, 6, 2, 5}},       // jazz "turnaround" (I-vi-ii-V)
        {"2-5-1",         {2, 5, 1}},          // jazz ii-V-I, the workhorse cadence
        {"1-4-1-5",       {1, 4, 1, 5}},       // simple two-chord-pair alternation
        {"1-5-4-5",       {1, 5, 4, 5}},
        {"4-5-1-1",       {4, 5, 1, 1}},       // plagal-ish build into a held tonic
        {"1-4-6-5",       {1, 4, 6, 5}},
        {"6-5-4-5",       {6, 5, 4, 5}},       // "Andalusian"-adjacent descending feel
        {"1-7-6-7",       {1, 7, 6, 7}},       // modal/rock-leaning (bright in
                                                // Mixolydian/Lydian especially)
        {"1-3-4-5",       {1, 3, 4, 5}},
        {"6-2-5-1",       {6, 2, 5, 1}},       // circle-of-fifths run into the tonic
        {"2-4-1-5",       {2, 4, 1, 5}},
        {"1-2-4-1",       {1, 2, 4, 1}},

        // ---- Longer forms ------------------------------------------------
        {"1-5-6-3-4-1-4-5", {1, 5, 6, 3, 4, 1, 4, 5}}, // the "Canon" progression,
                                                          // 8 chords, a lot of
                                                          // well-known pieces
                                                          // are built on this

        // ---- Two-chord vamps (great for arpeggio mode) --------------------
        {"1-4 VAMP",      {1, 4}},
        {"1-5 VAMP",      {1, 5}},
    }};
    return kAll;
}

} // namespace t8
