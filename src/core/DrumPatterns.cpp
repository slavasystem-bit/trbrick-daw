#include "DrumPatterns.h"
#include <algorithm>

namespace t8 {

// Slot order per DrumSlot: Kick=0, Snare=1, Clap=2, ClosedHat=3, OpenHat=4, Perc=5.
// All step numbers are 0-indexed, 0..15 within a 16-step bar.
namespace {

// v44.47: the long forms. A pattern listed here is repeated for `bars` bars (bar 1 = the one-bar template below, unchanged), and
// every later bar gets its own edits: '+' adds a hit, '-' removes one, 'A' adds a hit with an accent. `bar` counts from 0, so the
// edits for bar 2 of AMEN have bar = 1. Slots: 0 kick, 1 snare, 2 clap, 3 closed hat, 4 open hat, 5 perc; `step` is 0..15 in the bar.
struct LongEdit { const char* name; int bar; int slot; int step; char op; };
struct LongForm { const char* name; int bars; };

const LongForm kLongForms[] = {
    {"AMEN", 4}, {"JUNGLE", 2}, {"BREAKBEAT", 2}, {"BREAKS", 2}, {"DRUM AND BASS", 2}, {"UK GARAGE", 2}, {"DUBSTEP", 2},
    {"TRAP", 2}, {"HIP HOP", 2}, {"BOOM BAP", 2}, {"LO-FI HIP HOP", 2}, {"FUNK", 2}, {"AFROBEAT", 2},
};

const LongEdit kLongEdits[] = {
    // AMEN: bars 1 and 3 are the plain template, bar 2 adds the pushed second kick and a ghost snare, bar 4 is the turnaround
    // (the second kick again, the kick on 10 held, a snare run into the loop).
    {"AMEN", 1, 0, 2, '+'}, {"AMEN", 1, 1, 9, '+'},
    {"AMEN", 3, 0, 6, '-'}, {"AMEN", 3, 0, 2, '+'}, {"AMEN", 3, 1, 9, '+'}, {"AMEN", 3, 1, 13, '+'}, {"AMEN", 3, 1, 15, '+'},
    // JUNGLE: the second bar drops a kick and rolls the snare into the loop
    {"JUNGLE", 1, 0, 13, '-'}, {"JUNGLE", 1, 0, 14, '+'}, {"JUNGLE", 1, 1, 13, '+'}, {"JUNGLE", 1, 1, 15, '+'},
    // BREAKBEAT
    {"BREAKBEAT", 1, 0, 7, '-'}, {"BREAKBEAT", 1, 0, 14, '+'}, {"BREAKBEAT", 1, 1, 15, '+'}, {"BREAKBEAT", 1, 4, 10, '-'},
    // BREAKS
    {"BREAKS", 1, 0, 6, '-'}, {"BREAKS", 1, 0, 7, '+'}, {"BREAKS", 1, 1, 14, '-'}, {"BREAKS", 1, 1, 15, '+'}, {"BREAKS", 1, 5, 13, '+'},
    // DRUM AND BASS: the kick leans on the "a" of 3, a ghost snare and a perc before the loop
    {"DRUM AND BASS", 1, 0, 10, '-'}, {"DRUM AND BASS", 1, 0, 11, '+'}, {"DRUM AND BASS", 1, 1, 15, '+'}, {"DRUM AND BASS", 1, 5, 6, '+'},
    // UK GARAGE
    {"UK GARAGE", 1, 0, 6, '-'}, {"UK GARAGE", 1, 0, 7, '+'}, {"UK GARAGE", 1, 5, 5, '+'}, {"UK GARAGE", 1, 4, 6, '+'},
    // DUBSTEP: the half-time snare stays, the kick moves to the "and" of 3, a perc at the end
    {"DUBSTEP", 1, 0, 6, '-'}, {"DUBSTEP", 1, 0, 10, '+'}, {"DUBSTEP", 1, 3, 6, '+'}, {"DUBSTEP", 1, 5, 15, '+'},
    // TRAP: a kick pair at the end of bar 2 and a perc tick
    {"TRAP", 1, 0, 3, '-'}, {"TRAP", 1, 0, 11, '+'}, {"TRAP", 1, 0, 13, '+'}, {"TRAP", 1, 5, 14, '+'},
    // HIP HOP: an extra kick and the open hat at the end
    {"HIP HOP", 1, 0, 7, '+'}, {"HIP HOP", 1, 3, 14, '-'}, {"HIP HOP", 1, 4, 14, '+'},
    // BOOM BAP
    {"BOOM BAP", 1, 0, 7, '-'}, {"BOOM BAP", 1, 0, 8, '+'}, {"BOOM BAP", 1, 5, 14, '-'}, {"BOOM BAP", 1, 5, 13, '+'},
    // LO-FI HIP HOP
    {"LO-FI HIP HOP", 1, 0, 3, '+'}, {"LO-FI HIP HOP", 1, 5, 15, '+'},
    // FUNK
    {"FUNK", 1, 0, 11, '-'}, {"FUNK", 1, 0, 10, '+'}, {"FUNK", 1, 0, 14, '+'}, {"FUNK", 1, 1, 9, '+'}, {"FUNK", 1, 1, 15, '+'},
    // AFROBEAT
    {"AFROBEAT", 1, 0, 6, '-'}, {"AFROBEAT", 1, 0, 7, '+'}, {"AFROBEAT", 1, 5, 14, '-'}, {"AFROBEAT", 1, 5, 15, '+'},
};

void ApplyLongForms(std::array<GenrePattern, kNumGenrePatterns>& all) {
    for (GenrePattern& g : all) {
        const LongForm* lf = nullptr;
        for (const LongForm& f : kLongForms) if (std::string(g.name) == f.name) { lf = &f; break; }
        if (!lf) continue;
        g.bars = lf->bars;
        for (int slot = 0; slot < kNumDrumTracks; ++slot) {
            auto repeat = [&](std::vector<int>& v) {
                const std::vector<int> one = v;
                for (int b = 1; b < lf->bars; ++b) for (int st : one) v.push_back(b * 16 + st);
            };
            repeat(g.hitSteps[static_cast<size_t>(slot)]);
            repeat(g.accentSteps[static_cast<size_t>(slot)]);
        }
        for (const LongEdit& e : kLongEdits) {
            if (std::string(e.name) != g.name || e.bar >= lf->bars) continue;
            auto& hits = g.hitSteps[static_cast<size_t>(e.slot)];
            auto& acc = g.accentSteps[static_cast<size_t>(e.slot)];
            const int at = e.bar * 16 + e.step;
            if (e.op == '-') {
                hits.erase(std::remove(hits.begin(), hits.end(), at), hits.end());
                acc.erase(std::remove(acc.begin(), acc.end(), at), acc.end());
            } else {
                if (std::find(hits.begin(), hits.end(), at) == hits.end()) hits.push_back(at);
                if (e.op == 'A' && std::find(acc.begin(), acc.end(), at) == acc.end()) acc.push_back(at);
            }
        }
        for (auto& v : g.hitSteps) std::sort(v.begin(), v.end());
        for (auto& v : g.accentSteps) std::sort(v.begin(), v.end());
    }
}

} // namespace

const std::array<GenrePattern, kNumGenrePatterns>& AllGenrePatterns() {
    static const std::array<GenrePattern, kNumGenrePatterns> patterns = [] {
    std::array<GenrePattern, kNumGenrePatterns> p = {{
        // ---- FOUR ON THE FLOOR -- the foundational disco/house/techno
        // pulse: kick on every quarter note, clap/snare backbeat doubling
        // beats 2 and 4, straight 8th-note closed hats, one open-hat accent
        // at the very end of the bar before it loops. -----------------------
        GenrePattern{
            "FOUR ON THE FLOOR",
            { std::vector<int>{0,4,8,12}, std::vector<int>{}, std::vector<int>{4,12},
              std::vector<int>{0,2,4,6,8,10,12,14}, std::vector<int>{14}, std::vector<int>{} },
            { std::vector<int>{0}, std::vector<int>{}, std::vector<int>{4,12},
              std::vector<int>{0,4,8,12}, std::vector<int>{}, std::vector<int>{} },
            0.0f
        },
        // ---- HOUSE -- same 4/4 kick foundation as above, but the hats move
        // to the classic house "chick" placement (open hat on the off-beat
        // 8ths, closed hat only on the beat) plus a light shuffle and a
        // couple of shaker-style perc accents. ------------------------------
        GenrePattern{
            "HOUSE",
            { std::vector<int>{0,4,8,12}, std::vector<int>{}, std::vector<int>{4,12},
              std::vector<int>{0,4,8,12}, std::vector<int>{2,6,10,14}, std::vector<int>{3,11} },
            { std::vector<int>{0}, std::vector<int>{}, std::vector<int>{4,12},
              std::vector<int>{}, std::vector<int>{2,6,10,14}, std::vector<int>{} },
            0.10f
        },
        // ---- TECHNO -- relentless straight kick, dense driving 16th-note
        // closed hats with off-beat open-hat accents replacing the closed
        // hat on those steps, sparse clap, a couple of syncopated perc
        // clicks for texture. No swing -- techno reads as mechanical. -------
        GenrePattern{
            "TECHNO",
            { std::vector<int>{0,4,8,12}, std::vector<int>{}, std::vector<int>{12},
              std::vector<int>{0,1,3,4,5,7,8,9,11,12,13,15}, std::vector<int>{2,6,10,14}, std::vector<int>{7,15} },
            { std::vector<int>{0,8}, std::vector<int>{}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{2,10}, std::vector<int>{} },
            0.0f
        },
        // ---- ELECTRO -- 80s electro-funk/Miami-bass style: a syncopated,
        // bouncy kick (NOT four-on-the-floor) mirrored each half-bar, a
        // clap backbeat (the signature electro snare sound), tight straight
        // 8th hats, and a couple of cowbell/perc-style syncopated accents.
        // Deliberately no swing -- electro's bounce comes from the kick
        // syncopation itself, not shuffle; swing would fight the mechanical
        // 808-driven feel. ---------------------------------------------------
        GenrePattern{
            "ELECTRO",
            { std::vector<int>{0,3,8,11}, std::vector<int>{}, std::vector<int>{4,12},
              std::vector<int>{0,2,4,6,8,10,12,14}, std::vector<int>{14}, std::vector<int>{6,10} },
            { std::vector<int>{0,8}, std::vector<int>{}, std::vector<int>{4,12},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
            0.0f
        },
        // ---- BREAKS (breakbeat) -- funk-break-style: syncopated kick
        // (down on 1, plus two off-beat hits), snare backbeat with a ghost
        // hit before the loop restarts, straight 8th hats with a 16th
        // pickup at the very end, one open-hat accent and one syncopated
        // perc hit. A bit of swing since breaks are rooted in played funk
        // drumming, not a machine grid. --------------------------------------
        GenrePattern{
            "BREAKS",
            { std::vector<int>{0,6,10}, std::vector<int>{4,12,14}, std::vector<int>{},
              std::vector<int>{0,2,4,6,8,10,12,14,15}, std::vector<int>{6}, std::vector<int>{9} },
            { std::vector<int>{0}, std::vector<int>{4,12}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
            0.08f
        },
        // ---- AMEN (amen-style) -- see the file header: this captures the
        // break's most recognizable trait, a syncopated/"pushed" snare
        // hitting against a kick that lands early too (steps 6 and 14 are
        // deliberately non-accented "ghost" hits, same texture role the
        // original's chattier snare hits play), over straight 8th-note
        // ride/hats -- a simplified one-bar loop, not a literal
        // transcription of the 4-bar original performance. ------------------
        GenrePattern{
            "AMEN",
            { std::vector<int>{0,6,10}, std::vector<int>{4,6,12,14}, std::vector<int>{},
              std::vector<int>{0,2,4,6,8,10,12,14}, std::vector<int>{}, std::vector<int>{} },
            { std::vector<int>{0}, std::vector<int>{4,12}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
            0.0f
        },
        // ---- HIP HOP (boom bap) -- laid-back 90s-style: kick on 1 plus one
        // syncopated hit, steady snare backbeat, straight 8th hats, and a
        // noticeably wider swing than the other templates -- boom bap's
        // "MPC feel" comes from the shuffle, which is the one thing this
        // grid-based engine CAN approximate (real records also use
        // deliberately off-grid timing, which swing alone won't fully
        // capture). ------------------------------------------------------
        GenrePattern{
            "HIP HOP",
            { std::vector<int>{0,10}, std::vector<int>{4,12}, std::vector<int>{},
              std::vector<int>{0,2,4,6,8,10,12,14}, std::vector<int>{}, std::vector<int>{} },
            { std::vector<int>{0}, std::vector<int>{4,12}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
            0.18f
        },

        // ==== 20 more, added after direct feedback asking for more variety
        // in house/techno/electro/breaks specifically (researched via web
        // search) plus several new genre families. Same slot order/step
        // convention as above; comments kept brief since the format is by
        // now established. ====================================================

        // ---- DEEP HOUSE -- softer than the base HOUSE template: sparser
        // hats (off-beat 8ths only, no closed-hat-on-the-beat), a touch more
        // swing for the laid-back "rolling" feel deep house is known for.
        GenrePattern{
            "DEEP HOUSE",
            { std::vector<int>{0,4,8,12}, std::vector<int>{}, std::vector<int>{4,12},
              std::vector<int>{2,6,10,14}, std::vector<int>{6,14}, std::vector<int>{9} },
            { std::vector<int>{0}, std::vector<int>{}, std::vector<int>{4,12},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
            0.12f
        },
        // ---- TECH HOUSE -- house's 4/4 kick with techno's dense driving
        // 16th hats, plus a syncopated percussion loop (the genre's
        // signature "groove element" over the top of a functional beat).
        GenrePattern{
            "TECH HOUSE",
            { std::vector<int>{0,4,8,12}, std::vector<int>{}, std::vector<int>{4,12},
              std::vector<int>{0,2,4,6,8,10,12,14}, std::vector<int>{10}, std::vector<int>{3,7,11,15} },
            { std::vector<int>{0,8}, std::vector<int>{}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{3,11} },
            0.06f
        },
        // ---- UK GARAGE (2-STEP) -- kick pulled off the straight grid
        // (steps 0/6/10, not 0/4/8/12), heavy shuffle -- 2-step's defining
        // trait is exactly this "broken", swung, NOT four-on-the-floor feel.
        GenrePattern{
            "UK GARAGE",
            { std::vector<int>{0,6,10}, std::vector<int>{}, std::vector<int>{4,12},
              std::vector<int>{0,2,4,6,8,10,12,14}, std::vector<int>{14}, std::vector<int>{9,13} },
            { std::vector<int>{0}, std::vector<int>{}, std::vector<int>{4,12},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
            0.20f
        },
        // ---- MINIMAL TECHNO -- the opposite instinct from TECHNO above:
        // strip it back to just kick, sparse off-beat hat clicks and two
        // percussion accents -- no snare/clap/open-hat at all. Groove comes
        // from restraint and the negative space, not density.
        GenrePattern{
            "MINIMAL TECHNO",
            { std::vector<int>{0,4,8,12}, std::vector<int>{}, std::vector<int>{},
              std::vector<int>{2,6,10,14}, std::vector<int>{}, std::vector<int>{3,11} },
            { std::vector<int>{0}, std::vector<int>{}, std::vector<int>{},
              std::vector<int>{2,10}, std::vector<int>{}, std::vector<int>{} },
            0.0f
        },
        // ---- ACID TECHNO -- driving 4/4 kick with hard open/closed hats
        // (leaves room for a resonant acid bassline on a synth track to
        // carry the melodic interest -- the drums here stay functional).
        GenrePattern{
            "ACID TECHNO",
            { std::vector<int>{0,4,8,12}, std::vector<int>{}, std::vector<int>{12},
              std::vector<int>{0,2,4,6,8,10,12,14}, std::vector<int>{2,6,10,14}, std::vector<int>{7} },
            { std::vector<int>{0,8}, std::vector<int>{}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{2,10}, std::vector<int>{} },
            0.0f
        },
        // ---- DUB TECHNO -- deliberately sparse (per Basic Channel-style
        // convention): kick on 1 and 3 only, clap on the backbeat (2 and 4,
        // matching the "clap only on 2/4" convention research turned up),
        // minimal hats, no perc at all -- the space is the point.
        GenrePattern{
            "DUB TECHNO",
            { std::vector<int>{0,8}, std::vector<int>{}, std::vector<int>{4,12},
              std::vector<int>{6,14}, std::vector<int>{}, std::vector<int>{} },
            { std::vector<int>{0}, std::vector<int>{}, std::vector<int>{4,12},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
            0.0f
        },
        // ---- INDUSTRIAL TECHNO -- relentless: kick AND closed hat on
        // every single 16th, mechanical clang-style perc accents every
        // beat. The most "machine" of the techno family here on purpose.
        GenrePattern{
            "INDUSTRIAL TECHNO",
            { std::vector<int>{0,4,8,12}, std::vector<int>{}, std::vector<int>{8},
              std::vector<int>{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}, std::vector<int>{}, std::vector<int>{2,6,10,14} },
            { std::vector<int>{0,4,8,12}, std::vector<int>{}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{2,10} },
            0.0f
        },
        // ---- EBM (Electronic Body Music) -- pounding kick on every 8th
        // note (double the usual four-on-the-floor density) for the
        // militaristic, driving EBM pulse, clap backbeat, perc on the
        // off-beats for that "marching" quality.
        GenrePattern{
            "EBM",
            { std::vector<int>{0,2,4,6,8,10,12,14}, std::vector<int>{}, std::vector<int>{4,12},
              std::vector<int>{0,4,8,12}, std::vector<int>{}, std::vector<int>{2,6,10,14} },
            { std::vector<int>{0,8}, std::vector<int>{}, std::vector<int>{4,12},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
            0.0f
        },
        // ---- ELECTRO FUNK -- a second, busier take on 80s electro-funk
        // syncopation than the base ELECTRO template (different kick/perc
        // placement, an extra open-hat pickup) for variety when browsing.
        GenrePattern{
            "ELECTRO FUNK",
            { std::vector<int>{0,3,6,10,13}, std::vector<int>{}, std::vector<int>{4,12},
              std::vector<int>{0,2,4,6,8,10,12,14}, std::vector<int>{7,15}, std::vector<int>{2,9} },
            { std::vector<int>{0,10}, std::vector<int>{}, std::vector<int>{4,12},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
            0.0f
        },
        // ---- BREAKBEAT (UK big beat / nu skool breaks) -- chunkier than
        // the base BREAKS template: a snare backbeat added on top of the
        // syncopated kick, hat fills, open-hat accents on the off-beats.
        GenrePattern{
            "BREAKBEAT",
            { std::vector<int>{0,4,7,10}, std::vector<int>{4,12}, std::vector<int>{},
              std::vector<int>{0,2,4,6,8,10,12,14,15}, std::vector<int>{2,10}, std::vector<int>{6,14} },
            { std::vector<int>{0}, std::vector<int>{4,12}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
            0.10f
        },
        // ---- JUNGLE -- a faster, busier breakbeat chop than AMEN: more
        // syncopated kick hits and extra ghost snares scattered through the
        // bar (the "chopped and rearranged" quality jungle is built on),
        // played straight/unswung the way time-stretched breaks usually are.
        GenrePattern{
            "JUNGLE",
            { std::vector<int>{0,3,7,10,13}, std::vector<int>{4,6,10,12,14}, std::vector<int>{},
              std::vector<int>{0,2,4,6,8,10,12,14}, std::vector<int>{}, std::vector<int>{9} },
            { std::vector<int>{0}, std::vector<int>{4,12}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
            0.0f
        },
        // ---- DRUM AND BASS -- a cleaner, more "programmed" 2-step DnB
        // pattern than JUNGLE's amen-chop feel: simple kick/snare
        // syncopation, rolling hats, one open-hat pickup.
        GenrePattern{
            "DRUM AND BASS",
            { std::vector<int>{0,10}, std::vector<int>{4,12}, std::vector<int>{},
              std::vector<int>{0,2,4,6,8,10,12,14}, std::vector<int>{7}, std::vector<int>{2,9} },
            { std::vector<int>{0}, std::vector<int>{4,12}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
            0.0f
        },
        // ---- DUBSTEP -- half-time: kick on 1, snare on 3 (step 8) per the
        // genre's defining "cut time" groove (confirmed via research), with
        // sparse syncopated hats leaving lots of room for a wobble bassline.
        GenrePattern{
            "DUBSTEP",
            { std::vector<int>{0,6}, std::vector<int>{8}, std::vector<int>{},
              std::vector<int>{2,10,14}, std::vector<int>{4}, std::vector<int>{13} },
            { std::vector<int>{0}, std::vector<int>{8}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
            0.0f
        },
        // ---- TRAP -- syncopated kick, clap on beat 3 (half-time, same
        // convention as dubstep/half-time hip-hop), and a dense closed-hat
        // 16th-note bed standing in for the genre's characteristic rapid
        // hi-hat rolls (a fixed 16-step grid can't do true mixed-resolution
        // triplet/32nd rolls, so this is the closest a straight grid gets --
        // for a real roll, try L2+R2+LEFT/RIGHT ratchet on a hat step).
        GenrePattern{
            "TRAP",
            { std::vector<int>{0,3,6,10}, std::vector<int>{}, std::vector<int>{8},
              std::vector<int>{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}, std::vector<int>{15}, std::vector<int>{} },
            { std::vector<int>{0}, std::vector<int>{}, std::vector<int>{8},
              std::vector<int>{0,4,8,12}, std::vector<int>{}, std::vector<int>{} },
            0.0f
        },
        // ---- BOOM BAP -- a second classic-90s-hip-hop take distinct from
        // the base HIP HOP template: different kick syncopation, a dusty
        // perc accent near the end of the bar, slightly less swing.
        GenrePattern{
            "BOOM BAP",
            { std::vector<int>{0,7,10}, std::vector<int>{4,12}, std::vector<int>{},
              std::vector<int>{0,2,4,6,8,10,12,14}, std::vector<int>{}, std::vector<int>{14} },
            { std::vector<int>{0}, std::vector<int>{4,12}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
            0.14f
        },
        // ---- LO-FI HIP HOP -- sparse and laid-back: fewer hat hits than
        // any other template here, the heaviest swing of the set, no
        // accented kick at all -- everything sits back, nothing pushes.
        GenrePattern{
            "LO-FI HIP HOP",
            { std::vector<int>{0,10}, std::vector<int>{4,12}, std::vector<int>{},
              std::vector<int>{0,4,8,12}, std::vector<int>{}, std::vector<int>{7} },
            { std::vector<int>{}, std::vector<int>{4,12}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
            0.22f
        },
        // ---- DISCO -- four-on-the-floor kick, snare AND clap doubling the
        // backbeat, and the classic disco open-hat "chick" on every off-beat
        // 8th note (the single most recognizable disco hat pattern).
        GenrePattern{
            "DISCO",
            { std::vector<int>{0,4,8,12}, std::vector<int>{4,12}, std::vector<int>{4,12},
              std::vector<int>{0,2,4,6,8,10,12,14}, std::vector<int>{2,6,10,14}, std::vector<int>{} },
            { std::vector<int>{0,8}, std::vector<int>{4,12}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
            0.0f
        },
        // ---- FUNK -- syncopated 16th-note kick placement (NOT on the
        // downbeats the way disco/house sit) plus perc-slot "ghost note"
        // accents between the backbeat hits -- the syncopation IS the funk.
        GenrePattern{
            "FUNK",
            { std::vector<int>{0,3,8,11}, std::vector<int>{4,12}, std::vector<int>{},
              std::vector<int>{0,2,4,6,8,10,12,14}, std::vector<int>{}, std::vector<int>{6,14} },
            { std::vector<int>{0,8}, std::vector<int>{4,12}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
            0.08f
        },
        // ---- AFROBEAT -- a simplified, single-bar stylization of afrobeat's
        // polyrhythmic percussion bed: a syncopated kick, dense interlocking
        // perc hits (the genre's real percussion sections use many more
        // simultaneous parts than 6 slots can represent -- this captures
        // the "constantly moving" character, not a literal arrangement).
        GenrePattern{
            "AFROBEAT",
            { std::vector<int>{0,6,10}, std::vector<int>{4,12}, std::vector<int>{},
              std::vector<int>{0,3,6,9,12,15}, std::vector<int>{}, std::vector<int>{2,5,8,11,14} },
            { std::vector<int>{0}, std::vector<int>{}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{2,8} },
            0.0f
        },
        // ---- REGGAETON -- a simplified dembow-riddim stylization ("boom-
        // ch-boom-chick"): kick hits on the classic dembow accents, snare/
        // rimshot filling the gaps. Real dembow has more micro-variation
        // bar to bar than a single fixed loop -- same honesty as AFROBEAT
        // above and the original AMEN template.
        GenrePattern{
            "REGGAETON",
            { std::vector<int>{0,3,6,10}, std::vector<int>{2,4,8,11,14}, std::vector<int>{},
              std::vector<int>{0,2,4,6,8,10,12,14}, std::vector<int>{}, std::vector<int>{7} },
            { std::vector<int>{0,6}, std::vector<int>{4,8}, std::vector<int>{},
              std::vector<int>{}, std::vector<int>{}, std::vector<int>{} },
            0.0f
        },
    }};
    ApplyLongForms(p);
    return p;
    }();
    return patterns;
}

} // namespace t8
