#pragma once
// v44.41 MIX LAB -- where do the instruments of a scene fight for the same frequencies, and what could be turned to make room?
//
// Pure analysis, no audio engine and no UI in here:
//   * AnalyseStems()  per-track mono signals (the Mixer::tapMono taps of an offline render) -> a log-frequency spectrum per track
//   * BuildMixReport() spectra + the user's priority list -> overlap spots with advice and a fix, a tonal balance, who owns each zone
//   * MixApplyFix()   one advised change written into a Track (channel EQ, level or pan) -- the UI uses it for TRY IT (A/B)
// Everything is deterministic, so the selftest can check it with synthetic signals.
#include "Types.h"
#include <array>
#include <string>
#include <vector>

namespace t8 {

constexpr int kMixBands = 120;                 // log-spaced bands, 20 Hz .. 20 kHz, twelve per octave
constexpr float kMixLoHz = 20.0f, kMixHiHz = 20000.0f;
constexpr int kMixZones = 6;                   // SUB, BASS, LOW-MID, MID, PRESENCE, AIR
constexpr float kMixZoneEdges[kMixZones + 1] = {20.0f, 60.0f, 200.0f, 500.0f, 2000.0f, 6000.0f, 20000.0f};
constexpr const char* kMixZoneNames[kMixZones] = {"SUB", "BASS", "LOW-MID", "MID", "PRESENCE", "AIR"};
// A typical finished track, energy per octave relative to its MID zone (dB). A guide, not a rule.
constexpr float kMixTypicalDb[kMixZones] = {2.0f, 4.0f, 1.0f, 0.0f, -3.0f, -9.0f};
constexpr float kMixToleranceDb = 3.0f;

inline const char* MixTrackName(int t) {
    static const char* k[kNumTracks] = {"Kick", "Snare", "Clap", "C.Hat", "O.Hat", "Perc", "Bass", "Synth2", "Synth3", "Chord", "Sampler"};
    return (t >= 0 && t < kNumTracks) ? k[t] : "?";
}

// Frequency <-> band position (0..kMixBands, fractional). Band b covers MixBandEdgeHz(b) .. MixBandEdgeHz(b + 1).
float MixBandEdgeHz(float b);
float MixHzToBand(float hz);

struct MixSpectra {
    // dB per band, relative to a full-scale sine = 0 dB (the loudest instrument bands of a normal mix sit between -30 and -60)
    std::array<std::array<float, kMixBands>, kNumTracks> db{};
    std::array<bool, kNumTracks> present{};          // the track made sound in the analysed loop
    std::array<float, kMixBands> mixDb{};            // all present tracks summed (power)
    float mixRmsDb = -120.0f;                        // RMS of the summed mono signal, dB full scale
    int rate = kSampleRate;
    MixSpectra() { for (auto& r : db) r.fill(-120.0f); mixDb.fill(-120.0f); present.fill(false); }
};

// `stems[t]` = mono samples of track t (may be empty). Analysed as 8192-point Hann frames, hop 4096; only the frames where
// the track really plays (>= 10 % of its loudest frame's energy) are averaged, so a sparse snare is judged by what it does when it hits.
void AnalyseStems(const std::array<std::vector<float>, kNumTracks>& stems, int rate, MixSpectra& out);

struct MixFix {
    enum class Kind : uint8_t { None = 0, Hpf, LowShelfCut, BellCut, HighShelfCut, Level, Pan };
    Kind kind = Kind::None;
    int track = -1;
    float hz = 0.0f, q = 1.0f, value = 0.0f;         // value: dB (cuts, level), Hz is `hz`; pan target for Pan (-1..1)
    std::string text;                                // "Chord > EQ > MID -3 dB at 215 Hz"
};

struct MixSpot {
    float loHz = 0.0f, hiHz = 0.0f, centerHz = 0.0f;
    float severity = 0.0f;                           // 0..1
    int zone = 0;                                    // index into kMixZoneNames
    std::vector<int> tracks;                         // the instruments that stack up here, loudest first (>= 2)
    int keeper = -1, yielder = -1;                   // who owns the zone (higher in the priority list) / who is asked to make room
    MixFix fix;                                      // the single change advised for the yielder (kind None = advice only)
    std::string title, advice;
};

struct MixBalance {
    std::array<float, kMixZones> deltaDb{};          // zone energy per octave vs a typical track, dB (0 = typical), mid zone is the anchor
    std::array<bool, kMixZones> off{};               // |delta| > kMixToleranceDb
    std::array<std::array<float, kNumTracks>, kMixZones> share{};   // each track's share of the zone's energy (sums to 1)
};

struct MixReport {
    std::vector<MixSpot> spots;                      // worst first, at most 5
    MixBalance balance;
    float overlap = 0.0f;                            // summed severity of the spots -- the "how much do they fight" figure
    bool valid = false;
};

// `priority` = track indices, most important first (who keeps the space). Must hold every track once.
MixReport BuildMixReport(const MixSpectra& sp, const std::array<int, kNumTracks>& priority, const Pattern* pat = nullptr);   // pat: the scene's current pan / EQ, so the fix picks a free EQ band

std::array<int, kNumTracks> DefaultMixPriority();

// Writes the advised change into `t`. Returns false if it does nothing (kind None).
bool MixApplyFix(Track& t, const MixFix& f);

} // namespace t8
