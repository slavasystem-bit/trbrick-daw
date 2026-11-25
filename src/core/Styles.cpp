#include "Styles.h"
#include "ChordProgressions.h"
#include "DrumPatterns.h"
#include "Grooves.h"
#include "SynthPresets.h"

namespace t8 {

namespace {
// Bass lines (16ths of a bar; the semitone is relative to the root of the key). Written from the usual way each style moves its bass.
using B = StyleBassNote;
const std::vector<B> kOffbeat   = {{2, 0, 2}, {6, 0, 2}, {10, 0, 2}, {14, 7, 2}};                     // house: the root on every off-beat, a fifth at the end
const std::vector<B> kDeep      = {{0, 0, 3}, {3, 0, 1}, {6, 7, 2}, {10, 0, 3}, {14, 3, 2}};         // deep house: rolling, a little melody
const std::vector<B> kRolling   = {{2, 0, 1}, {3, 0, 1}, {6, 0, 2}, {10, 0, 1}, {11, 3, 1}, {14, 0, 2}};   // tech house
const std::vector<B> kGarage    = {{0, 0, 3}, {6, 0, 2}, {10, 7, 3}, {14, 5, 1}};                     // 2-step: long subs on the gaps of the kick
const std::vector<B> kTechno    = {{2, 0, 2}, {6, 0, 2}, {10, 0, 2}, {14, 0, 2}};                     // techno: the root, every off-beat
const std::vector<B> kAcid      = {{0, 0, 1}, {2, 12, 1}, {3, 0, 1, true}, {5, 3, 1}, {6, 0, 1}, {8, 0, 1}, {10, 12, 1}, {11, 10, 1, true}, {13, 7, 1}, {14, 0, 1}};   // 303-like
const std::vector<B> kElectro   = {{0, 0, 2}, {3, 0, 1}, {6, 0, 2}, {8, 12, 1}, {10, 0, 2}, {13, 7, 1}};   // electro: short, bouncy
const std::vector<B> kBreaks    = {{0, 0, 2}, {3, 0, 1}, {7, 0, 2}, {10, 7, 2}, {14, 0, 1}};          // breaks: syncopated
const std::vector<B> kJungle    = {{0, 0, 6}, {6, 0, 2}, {10, -2, 4}};                                // jungle / dnb: long subs and a step down
const std::vector<B> kDnb       = {{0, 0, 4}, {6, 0, 2}, {10, 0, 4}, {14, 3, 2}};
const std::vector<B> kBoomBap   = {{0, 0, 4}, {6, 0, 2}, {10, 3, 3}};                                 // hip hop: the kick pattern's shadow
const std::vector<B> kLofi      = {{0, 0, 5}, {8, 0, 3}, {12, 7, 3}};
}

const std::vector<StyleDef>& AllStyles() {
    static const std::vector<StyleDef> all = {
        // name, category, tags, pattern, bpm, kit, groove, amount, bass, syn2, syn3, chord, progression, chordBeats, bassOctave, bass line
        {"HOUSE",         "HOUSE",   "DRIVING",   "HOUSE",         124, "kits/909",     "FOUR FLOOR ACCENT", 0.5f, "HOUSE BASS",   "HOUSE STAB",   "WARM PAD",     "ELECTRIC ORGAN", "1-4-1-5",   1, 2, kOffbeat},
        {"DEEP HOUSE",    "HOUSE",   "LAID BACK", "DEEP HOUSE",    122, "kits/909",     "LAID BACK",         0.6f, "HOUSE BASS",   "CHORD STAB",   "WARM PAD",     "FM EPIANO",      "1-6-2-5",   1, 2, kDeep},
        {"TECH HOUSE",    "HOUSE",   "DRIVING",   "TECH HOUSE",    126, "kits/techno",  "TECHNO DRIVE",      0.5f, "TECHNO BASS",  "TECHNO STAB",  "DARK PAD",     "DUB CHORD",      "1-4 VAMP",  2, 2, kRolling},
        {"UK GARAGE",     "HOUSE",   "SWUNG",     "UK GARAGE",     132, "kits/909",     "MPC 16TH 58",       0.7f, "SUB BASS",     "CHORD STAB",   "GLASS PAD",    "ELECTRIC ORGAN", "6-2-5-1",   2, 2, kGarage},
        {"TECHNO",        "TECHNO",  "DRIVING",   "TECHNO",        132, "kits/techno",  "TECHNO DRIVE",      0.5f, "TECHNO BASS",  "TECHNO STAB",  "DARK PAD",     "DUB CHORD",      "1-4 VAMP",  4, 2, kTechno},
        {"ACID TECHNO",   "TECHNO",  "DARK",      "ACID TECHNO",   134, "kits/acid_v2", "ACID BOUNCE",       0.6f, "ACID BASS",    "ACID LEAD",    "DARK PAD",     "RAVE STAB",      "1-7-6-7",   2, 2, kAcid},
        {"ELECTRO",       "ELECTRO", "DRIVING",   "ELECTRO",       128, "kits/electro", "ELECTRO PUSH",      0.6f, "ELECTRO BASS", "SQUARE LEAD",  "WT GLASS LEAD","CLAV",           "6-4-1-5",   1, 2, kElectro},
        {"BREAKS",        "BREAKS",  "SWUNG",     "BREAKS",        130, "kits/breaks",  "BREAKS SHUFFLE",    0.6f, "RUBBER BASS",  "RAVE STAB",    "WIDE PAD",     "CHORD STAB",     "1-6-4-5",   2, 2, kBreaks},
        {"JUNGLE",        "BREAKS",  "FAST",      "JUNGLE",        165, "kits/breaks",  "",                  0.0f, "SUB BASS",     "DUB CHORD",    "AMBIENT DRONE","CHOIR PAD",      "1-5 VAMP",  4, 2, kJungle},
        {"DRUM AND BASS", "BREAKS",  "FAST",      "DRUM AND BASS", 174, "kits/breaks",  "",                  0.0f, "REESE BASS",   "SOFT PLUCK",   "DARK PAD",     "WIDE PAD",       "1-6-4-5",   4, 2, kDnb},
        {"BOOM BAP",      "HIP HOP", "SWUNG",     "BOOM BAP",       90, "kits/linn",    "MPC 16TH 58",       0.7f, "SUB BASS",     "CLAV",         "MARIMBA",      "TINE KEYS",      "6-2-5-1",   2, 2, kBoomBap},
        {"LO-FI HIP HOP", "HIP HOP", "LAID BACK|SLOW", "LO-FI HIP HOP", 78, "kits/lofi",  "LAID BACK",         0.9f, "SUB BASS",     "SOFT PLUCK",   "ANALOG PAD",   "FM EPIANO",      "6-2-5-1",   4, 2, kLofi},
    };
    return all;
}

int FindGenrePatternIndex(const std::string& name) {
    const auto& a = AllGenrePatterns();
    for (size_t i = 0; i < a.size(); ++i) if (name == a[i].name) return static_cast<int>(i);
    return -1;
}
int FindSynthPresetIndex(const std::string& name) {
    const auto& a = AllSynthPresets();
    for (size_t i = 0; i < a.size(); ++i) if (name == a[i].name) return static_cast<int>(i);
    return -1;
}
int FindProgressionIndex(const std::string& name) {
    const auto& a = AllChordProgressions();
    for (size_t i = 0; i < a.size(); ++i) if (name == a[i].name) return static_cast<int>(i);
    return -1;
}
int FindBuiltinGrooveIndex(const std::string& name) {
    const auto& a = BuiltinGrooves();
    for (size_t i = 0; i < a.size(); ++i) if (name == a[i].name) return static_cast<int>(i);
    return -1;
}

} // namespace t8
