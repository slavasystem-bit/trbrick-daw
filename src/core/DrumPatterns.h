#pragma once
// "Typical pattern" templates for the RANDOM tab -- genre-shaped drum
// grooves (not per-hit random noise like RandomizeDrums) so a live
// performer can drop in something that already reads as "a real four-on-
// the-floor" or "a real breakbeat" instead of hand-programming one from
// scratch. Step positions encode well-established, decades-old rhythmic
// CONVENTIONS of each genre (which beats a kick/snare/hat typically falls
// on) -- not a transcription of any specific copyrighted recording. The
// AMEN template in particular is explicitly "amen-STYLE": the real Amen
// break is a 4-bar performance with bar-to-bar variation and heavy
// microtiming that (per every reference consulted while building this)
// can't be reduced to one clean 16-step grid -- what's here is a single-
// bar loop capturing its most recognizable trait (a syncopated, "pushed"
// snare against straight 8th-note hats), same spirit as the synthesized
// drum kits being "808-style"/"909-style" rather than samples.
#include "Types.h"
#include <array>
#include <string>
#include <vector>

namespace t8 {

struct GenrePattern {
    const char* name;
    // Per drum slot (Kick/Snare/Clap/ClosedHat/OpenHat/Perc, see DrumSlot),
    // which of a 16-step bar are active, and which of those are accented.
    // Tiles automatically for pattern lengths >16 (repeats every 16 steps)
    // and truncates cleanly for lengths <16 -- see AppState::ApplyGenrePattern.
    std::array<std::vector<int>, kNumDrumTracks> hitSteps;
    std::array<std::vector<int>, kNumDrumTracks> accentSteps;
    float swing; // suggested pattern.swing to go with this groove (0..0.75)
    // v44.47: typical length in bars (1 = the old one-bar loop). Step numbers are written in 16ths from the start of the pattern,
    // so a 4-bar pattern has steps 0..63. Bar 1 is always the original one-bar template; the later bars are the usual variations
    // of that style (a moved kick, a ghost note, a pickup into the loop), written from convention like everything else here.
    int bars = 1;
};

// 7 originally-shipped templates (FOUR ON THE FLOOR/HOUSE/TECHNO/ELECTRO/
// BREAKS/AMEN/HIP HOP) plus 20 more added in a later round covering more
// of house/techno/electro/breaks (per direct feedback asking specifically
// for those four families) plus several new genre families (hip-hop
// variants, disco/funk, and a few more electronic dance genres). Several
// of the newer ones -- AFROBEAT and REGGAETON especially -- have real-world
// rhythms with more cross-rhythm/microtiming complexity than a single
// straight 16-step grid can capture; same "genre-style, not a literal
// transcription" honesty as the original AMEN template applies to those.
constexpr int kNumGenrePatterns = 27;
const std::array<GenrePattern, kNumGenrePatterns>& AllGenrePatterns();

} // namespace t8
