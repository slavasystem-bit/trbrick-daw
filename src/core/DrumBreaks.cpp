#include "DrumBreaks.h"
#include <cmath>

namespace t8 {

// Slot order per DrumSlot: Kick=0, Snare=1, Clap=2, ClosedHat=3, OpenHat=4, Perc=5.
// All step numbers are 0-indexed, 0..15 -- see DrumBreaks.h for why every
// break's busiest material sits in steps 8-15.
const std::array<DrumBreak, kNumDrumBreaks>& AllDrumBreaks() {
    static const std::array<DrumBreak, kNumDrumBreaks> breaks = {{
        // ---- SNARE ROLL -- the classic "building snare roll into the next
        // section" fill: hats drop out for the back half so the snare roll
        // (accelerating from 8ths to 16ths, increasingly accented) is the
        // whole focus. --------------------------------------------------
        DrumBreak{
            "SNARE ROLL",
            { std::vector<int>{0}, std::vector<int>{8,10,12,13,14,15}, std::vector<int>{},
              std::vector<int>{0,2,4,6}, std::vector<int>{}, std::vector<int>{} },
            { std::vector<int>{}, std::vector<int>{13,14,15}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
        },
        // ---- HAT ROLL -- closed hats accelerate from straight 8ths to
        // 16ths through the back half, open hat pickup right at the end.
        DrumBreak{
            "HAT ROLL",
            { std::vector<int>{0,8}, std::vector<int>{}, std::vector<int>{},
              std::vector<int>{0,2,4,6,8,9,10,11,12,13,14,15}, std::vector<int>{15}, std::vector<int>{} },
            { std::vector<int>{}, std::vector<int>{}, std::vector<int>{},
              std::vector<int>{8,10,12,14}, std::vector<int>{}, std::vector<int>{} },
        },
        // ---- TOM FILL -- uses the flexible perc slot as a tom/conga-style
        // voice, dense hits across the back half like a classic acoustic
        // drum-fill-before-the-chorus.
        DrumBreak{
            "TOM FILL",
            { std::vector<int>{0}, std::vector<int>{}, std::vector<int>{},
              std::vector<int>{0,2,4,6}, std::vector<int>{}, std::vector<int>{8,9,10,11,12,13,14,15} },
            { std::vector<int>{}, std::vector<int>{}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{12,14} },
        },
        // ---- KICK ROLL -- the kick itself accelerates and gets louder
        // through the back half, TR-8 "kick roll" style.
        DrumBreak{
            "KICK ROLL",
            { std::vector<int>{0,2,4,6,8,9,10,11,12,13,14,15}, std::vector<int>{}, std::vector<int>{},
              std::vector<int>{0,4}, std::vector<int>{}, std::vector<int>{} },
            { std::vector<int>{12,13,14,15}, std::vector<int>{}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
        },
        // ---- HALF-TIME DROP -- the opposite of a roll: strip everything
        // back to just a sparse kick+snare breakdown hit, big open-hat
        // swells -- for dropping OUT of the energy for a bar, not into it.
        DrumBreak{
            "HALF-TIME DROP",
            { std::vector<int>{0,8}, std::vector<int>{8}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{4,12}, std::vector<int>{} },
            { std::vector<int>{0}, std::vector<int>{8}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
        },
        // ---- RISER BUILD -- open hat density increases through the bar
        // (no crash/riser sample slot in this engine, so the open hat's
        // own ring stands in for rising tension) into a final 16th flurry.
        DrumBreak{
            "RISER BUILD",
            { std::vector<int>{0}, std::vector<int>{}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{0,4,8,10,12,13,14,15}, std::vector<int>{} },
            { std::vector<int>{}, std::vector<int>{}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{12,13,14,15}, std::vector<int>{} },
        },
        // ---- CLAP ROLL -- same idea as SNARE ROLL but with the clap
        // taking the lead voice instead, for a break with a different
        // timbral character on kits where clap and snare are tuned very
        // differently.
        DrumBreak{
            "CLAP ROLL",
            { std::vector<int>{0,8}, std::vector<int>{}, std::vector<int>{8,10,12,13,14,15},
              std::vector<int>{0,2,4,6}, std::vector<int>{}, std::vector<int>{} },
            { std::vector<int>{}, std::vector<int>{}, std::vector<int>{13,14,15},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
        },
        // ---- GHOST SNARE FUNK -- constant 16th-note snare with only some
        // hits accented (classic funk ghost-note technique) instead of an
        // accelerating roll -- reads as "groove got busier" rather than
        // "here comes a fill".
        DrumBreak{
            "GHOST SNARE FUNK",
            { std::vector<int>{0,7,10}, std::vector<int>{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
              std::vector<int>{}, std::vector<int>{0,4,8,12}, std::vector<int>{}, std::vector<int>{} },
            { std::vector<int>{0,10}, std::vector<int>{0,4,8,10,12,14},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
        },
        // ---- BREAK CHOP -- scattered, syncopated kick/snare/perc hits in
        // the classic chopped-breakbeat/jungle style, distinct from the
        // smoother accelerating rolls above.
        DrumBreak{
            "BREAK CHOP",
            { std::vector<int>{0,6,10}, std::vector<int>{2,5,9,13}, std::vector<int>{},
              std::vector<int>{0,3,7,11}, std::vector<int>{}, std::vector<int>{4,12,14} },
            { std::vector<int>{10}, std::vector<int>{9,13}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
        },
        // ---- MILITARY ROLL -- rudimental-style snare+clap flams building
        // in density through the bar, drill-corps energy rather than a
        // dance-music roll.
        DrumBreak{
            "MILITARY ROLL",
            { std::vector<int>{0}, std::vector<int>{0,2,4,6,8,9,10,11,12,13,14,15},
              std::vector<int>{8,10,12,14}, std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
            { std::vector<int>{}, std::vector<int>{0,8,10,12,14}, std::vector<int>{12,14},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
        },
        // ---- LATIN FLURRY -- clave-adjacent syncopation across
        // clap/hat/perc, afrobeat/latin cross-rhythm energy instead of a
        // straight roll or build.
        DrumBreak{
            "LATIN FLURRY",
            { std::vector<int>{0,6}, std::vector<int>{}, std::vector<int>{3,7,11,14},
              std::vector<int>{0,2,4,6,8,10,12,14}, std::vector<int>{}, std::vector<int>{1,5,9,13,15} },
            { std::vector<int>{}, std::vector<int>{}, std::vector<int>{3,11},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{9,13,15} },
        },
        // ---- STUTTER KICK -- glitchy, gappy kick bursts (IDM/glitch
        // style) instead of a smoothly accelerating roll -- the kick
        // itself sounds like it's skipping.
        DrumBreak{
            "STUTTER KICK",
            { std::vector<int>{0,8,9,11,12,14,15}, std::vector<int>{}, std::vector<int>{},
              std::vector<int>{0,2,8,10,12,13,14,15}, std::vector<int>{}, std::vector<int>{} },
            { std::vector<int>{9,12,15}, std::vector<int>{}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
        },
        // ---- DOUBLE TIME -- everything doubles up into a dense, high-
        // energy drum-and-bass-style pattern for the whole bar, the
        // opposite move from HALF-TIME DROP above.
        DrumBreak{
            "DOUBLE TIME",
            { std::vector<int>{0,3,6,8,10,12,14}, std::vector<int>{4,12}, std::vector<int>{4,12},
              std::vector<int>{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}, std::vector<int>{}, std::vector<int>{2,10} },
            { std::vector<int>{8,14}, std::vector<int>{4,12}, std::vector<int>{4,12},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
        },
        // ---- REVERSE SWELL -- the inverse move from RISER BUILD: starts
        // busy and THINS OUT toward the end of the bar instead of building
        // up, landing on one single open-hat swell right at the boundary
        // -- tension via subtraction instead of addition.
        DrumBreak{
            "REVERSE SWELL",
            { std::vector<int>{0,2,4,6,8}, std::vector<int>{}, std::vector<int>{},
              std::vector<int>{0,1,2,3,4,5,6,7,8,9}, std::vector<int>{15}, std::vector<int>{1,3,5,7} },
            { std::vector<int>{}, std::vector<int>{}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{15}, std::vector<int>{} },
        },
        // ---- TENSION STOP -- near-total silence for the whole bar except
        // one huge unified hit right at the very end (every voice at
        // once) -- a hard "stop before the drop" rather than a rolling
        // fill.
        DrumBreak{
            "TENSION STOP",
            { std::vector<int>{0,15}, std::vector<int>{15}, std::vector<int>{15},
              std::vector<int>{}, std::vector<int>{15}, std::vector<int>{15} },
            { std::vector<int>{15}, std::vector<int>{15}, std::vector<int>{15},
              std::vector<int>{}, std::vector<int>{15}, std::vector<int>{15} },
        },
        // ---- ACID ROLL -- steady four-on-the-floor kick under a rolling
        // 303-adjacent hat/perc interplay, acid-house energy for a break
        // that keeps the floor moving instead of building toward a stop.
        DrumBreak{
            "ACID ROLL",
            { std::vector<int>{0,4,8,12}, std::vector<int>{}, std::vector<int>{12},
              std::vector<int>{2,6,10,14}, std::vector<int>{0,4,8,12}, std::vector<int>{1,3,5,7,9,11,13,15} },
            { std::vector<int>{}, std::vector<int>{}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{12}, std::vector<int>{7,15} },
        },
    }};
    return breaks;
}

float BreakTrimDb(int i) {
    static const float kTrim[kNumDrumBreaks] = {
        +2.0f,  // SNARE ROLL
        +1.8f,  // HAT ROLL
        +2.0f,  // TOM FILL
        -5.3f,  // KICK ROLL
         0.0f,  // HALF-TIME DROP
        +1.0f,  // RISER BUILD
        +1.5f,  // CLAP ROLL
        -1.0f,  // GHOST SNARE FUNK
        -0.3f,  // BREAK CHOP
        +1.5f,  // MILITARY ROLL
         0.0f,  // LATIN FLURRY
        -3.8f,  // STUTTER KICK
        -3.0f,  // DOUBLE TIME
        -2.2f,  // REVERSE SWELL
        +1.0f,  // TENSION STOP
        -0.3f,  // ACID ROLL
    };
    return (i < 0 || i >= kNumDrumBreaks) ? 0.0f : kTrim[i];
}

float BreakTrimLinear(int i) { return std::pow(10.0f, BreakTrimDb(i) / 20.0f); }

} // namespace t8
