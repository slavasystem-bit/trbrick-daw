#include "Types.h"

namespace t8 {

// Default fader level per drum slot -- Kick/Snare/Clap/ClosedHH/OpenHH lead
// with Perc pulled well back (a stock 808-style hat/kick/snare/clap group
// tends to already read as "loud" together, and Perc is usually a garnish
// hit, not a foundation element), rather than one uniform level for all 6
// drum tracks. Indexed by DrumSlot (see Types.h).
constexpr float kDefaultDrumVolume[kNumDrumTracks] = {
    0.85f, // Kick
    0.80f, // Snare
    0.75f, // Clap
    0.75f, // ClosedHat
    0.75f, // OpenHat
    0.40f, // Perc
};

Pattern::Pattern() {
    for (int i = 0; i < kNumDrumTracks; ++i) {
        tracks[i].type = TrackType::Drum;
        tracks[i].slotIndex = i;
        tracks[i].name = DrumSlotName(static_cast<DrumSlot>(i));
        tracks[i].volume = kDefaultDrumVolume[i];
        // new projects start with a stereo picture: the low drums stay in the centre, the top end opens up
        constexpr float kDefaultDrumSpread[kNumDrumTracks] = {0.0f, 0.15f, 0.35f, 0.45f, 0.45f, 0.5f};
        tracks[i].spread = kDefaultDrumSpread[i];
        // default kit acid_v2: kick 1, snare 2, clap 1, closed hat 1, open hat 1, perc 3 (0-based indices)
        constexpr uint8_t kDefaultDrumVariant[kNumDrumTracks] = {0, 1, 0, 0, 0, 2};
        tracks[i].defaultVariant = kDefaultDrumVariant[i];
    }
    for (int i = 0; i < kNumSynthTracks; ++i) {
        int idx = kNumDrumTracks + i;
        tracks[idx].type = TrackType::Synth;
        tracks[idx].slotIndex = i;
        tracks[idx].name = (i == 0) ? std::string("Bass") : "Synth" + std::to_string(i + 1);
        // v26: synths/bass used to start at 0.50 (about 12 dB under the kick peak)
        // and felt weak; new projects now start at 0.75 for bass, 0.65 for synths.
        tracks[idx].volume = (i == 0) ? 0.75f : 0.65f;
        tracks[idx].spread = (i == 0) ? 0.0f : 0.35f;   // the bass stays centred; the other synths open up
        // give the synth tracks a sparse default so a fresh project isn't silent noise
    }
    for (int i = 0; i < kNumChordTracks; ++i) {
        int idx = kNumDrumTracks + kNumSynthTracks + i;
        tracks[idx].type = TrackType::Chord;
        tracks[idx].slotIndex = i;
        tracks[idx].name = "Chord";
        tracks[idx].volume = 0.60f;
        // starts empty (no steps active) -- the Chords tab is how you
        // actually put a progression here, same "starts silent, tab fills
        // it in" story as the drum tracks' typical-pattern templates.
    }
    for (int i = 0; i < kNumSamplerTracks; ++i) {   // v44.50
        int idx = kSamplerTrackIndex + i;
        tracks[idx].type = TrackType::Sampler;
        tracks[idx].slotIndex = i;
        tracks[idx].name = "Sampler";
        tracks[idx].volume = 0.80f;
    }
}

ChordScene::ChordScene() {
    track.type = TrackType::Chord;
    track.slotIndex = 0;
    track.name = "Chord";
}

} // namespace t8

namespace t8 {

const char* const kCompPresetNames[kNumCompPresets] = {"GLUE", "PUNCH", "SOFT"};
const char* const kMasterPresetNames[kNumMasterPresets] = {"CLEAN", "WARM", "LOUD", "LO-FI"};

void ApplyCompPreset(MasterChainSettings& m, int p) {
    if (p < 0 || p >= kNumCompPresets) return;
    m.compPreset = p;
    m.compOn = true;
    switch (p) {
        case 0: // GLUE: low ratio, slow-ish attack, releases with the groove -- binds the mix
            m.compThreshDb = -16.0f; m.compRatio = 2.0f; m.compAttackMs = 30.0f; m.compReleaseMs = 200.0f; m.compMakeupDb = 1.5f; m.compMix = 1.0f; break;
        case 1: // PUNCH: higher ratio but a late attack lets the transient through
            m.compThreshDb = -18.0f; m.compRatio = 4.0f; m.compAttackMs = 35.0f; m.compReleaseMs = 90.0f; m.compMakeupDb = 3.0f; m.compMix = 1.0f; break;
        case 2: // SOFT: gentle, partly parallel
            m.compThreshDb = -20.0f; m.compRatio = 1.5f; m.compAttackMs = 15.0f; m.compReleaseMs = 250.0f; m.compMakeupDb = 1.0f; m.compMix = 0.7f; break;
    }
}

void ApplyMasterPreset(MasterChainSettings& m, int p) {
    if (p < 0 || p >= kNumMasterPresets) return;
    m.chainOn = true; m.chainPreset = p; m.limiterOn = true;
    switch (p) {
        case 0: // CLEAN: transparent polish
            m.hpfHz = 25.0f; m.lowDb = 0.0f; m.midDb = 0.0f; m.highDb = 1.0f; m.width = 1.1f; m.saturation = 0.05f; m.ceilingDb = -1.0f;
            ApplyCompPreset(m, 0); m.compPreset = 0; m.compRatio = 1.6f; m.compMakeupDb = 1.0f; break;
        case 1: // WARM: tape-ish, rounded top
            m.hpfHz = 30.0f; m.lowDb = 1.5f; m.midDb = -0.5f; m.highDb = -1.5f; m.width = 1.0f; m.saturation = 0.30f; m.ceilingDb = -1.0f;
            ApplyCompPreset(m, 0); m.compRatio = 2.0f; m.compMakeupDb = 1.5f; break;
        case 2: // LOUD: dense and wide, hard against the ceiling
            m.hpfHz = 30.0f; m.lowDb = 2.0f; m.midDb = 0.0f; m.highDb = 2.0f; m.width = 1.2f; m.saturation = 0.25f; m.ceilingDb = -0.5f;
            ApplyCompPreset(m, 1); m.compThreshDb = -22.0f; m.compRatio = 3.0f; m.compAttackMs = 12.0f; m.compReleaseMs = 100.0f; m.compMakeupDb = 5.0f; break;
        case 3: // LO-FI: narrow, band-limited, crunchy
            m.hpfHz = 90.0f; m.lowDb = -1.0f; m.midDb = 1.5f; m.highDb = -6.0f; m.width = 0.6f; m.saturation = 0.70f; m.ceilingDb = -1.0f;
            ApplyCompPreset(m, 1); m.compThreshDb = -22.0f; m.compRatio = 4.0f; m.compAttackMs = 5.0f; m.compReleaseMs = 80.0f; m.compMakeupDb = 3.0f; m.compMix = 0.8f; break;
    }
    m.chainPreset = p;
    m.compPreset = -1;   // the compressor numbers are part of the chain preset now
}

} // namespace t8
