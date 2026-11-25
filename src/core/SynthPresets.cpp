#include "SynthPresets.h"
#include <array>
#include <string>
#include <vector>

namespace t8 {

namespace {

// Named-parameter helper so each preset below reads as "here's what this
// KNOB is set to", not a 15-position positional aggregate literal (which
// would be silently wrong forever if two fields were ever transposed).
// Defaults on the tail params match SynthPatch's own struct defaults
// (Off/no LFO) so presets that don't use the automation LFO can omit
// those 4 arguments entirely.
SynthPatch MakePatch(Waveform waveform, float cutoffHz, float resonance, float envAmount,
                      float attack, float decay, float sustain, float release,
                      float filterDecay, float glideTime, float volume,
                      LfoTarget lfoTarget = LfoTarget::Off, float lfoRateHz = 2.0f,
                      float lfoDepth = 0.3f, Waveform lfoShape = Waveform::Sine) {
    SynthPatch p;
    p.waveform = waveform;
    p.cutoffHz = cutoffHz;
    p.resonance = resonance;
    p.envAmount = envAmount;
    p.attack = attack;
    p.decay = decay;
    p.sustain = sustain;
    p.release = release;
    p.filterDecay = filterDecay;
    p.glideTime = glideTime;
    p.volume = volume;
    p.lfoTarget = lfoTarget;
    p.lfoRateHz = lfoRateHz;
    p.lfoDepth = lfoDepth;
    p.lfoShape = lfoShape;
    return p;
}

// Phase 5 FM preset helper -- parallel to MakePatch above but for FM
// patches: sets engineType/fmAlgorithm/fmOps and the shared ADSR/volume/
// LFO fields, leaving the subtractive-only fields (waveform, cutoff,
// resonance, envAmount, filterDecay) at SynthPatch's own defaults since
// FMSynth never reads them.
SynthPatch MakeFmPatch(int algorithm, std::array<FMOperator, 4> ops,
                        float attack, float decay, float sustain, float release, float volume,
                        LfoTarget lfoTarget = LfoTarget::Off, float lfoRateHz = 2.0f,
                        float lfoDepth = 0.3f, Waveform lfoShape = Waveform::Sine) {
    SynthPatch p;
    p.engineType = SynthEngineType::FM;
    p.fmIndexScale = 1.0f;   // v44: the library uses the usual FM index range (see SynthPatch::fmIndexScale)
    p.fmAlgorithm = algorithm;
    p.fmOps = ops;
    p.attack = attack;
    p.decay = decay;
    p.sustain = sustain;
    p.release = release;
    p.volume = volume;
    p.lfoTarget = lfoTarget;
    p.lfoRateHz = lfoRateHz;
    p.lfoDepth = lfoDepth;
    p.lfoShape = lfoShape;
    return p;
}

// Phase 5b Wavetable preset helper -- parallel to MakeFmPatch above:
// sets engineType/wavetableBank/wtPosition plus the shared ADSR/volume/
// LFO fields AND cutoffHz/resonance/envAmount/filterDecay (WavetableSynth
// reuses the same post-oscillator filter block SubtractiveSynth has, see
// WavetableSynth's class doc comment -- so, unlike MakeFmPatch, this one
// still takes filter params).
SynthPatch MakeWtPatch(int wavetableBank, float wtPosition,
                        float cutoffHz, float resonance, float envAmount, float filterDecay,
                        float attack, float decay, float sustain, float release, float volume,
                        LfoTarget lfoTarget = LfoTarget::Off, float lfoRateHz = 2.0f,
                        float lfoDepth = 0.3f, Waveform lfoShape = Waveform::Sine) {
    SynthPatch p;
    p.engineType = SynthEngineType::Wavetable;
    p.wavetableBank = wavetableBank;
    p.wtPosition = wtPosition;
    p.cutoffHz = cutoffHz;
    p.resonance = resonance;
    p.envAmount = envAmount;
    p.filterDecay = filterDecay;
    p.attack = attack;
    p.decay = decay;
    p.sustain = sustain;
    p.release = release;
    p.volume = volume;
    p.lfoTarget = lfoTarget;
    p.lfoRateHz = lfoRateHz;
    p.lfoDepth = lfoDepth;
    p.lfoShape = lfoShape;
    return p;
}


// v26: wraps a base patch with the THICKEN fields (unison/sub/noise/drive).
// osc2=true additionally enables a detuned second saw oscillator.
SynthPatch ThickPatch(SynthPatch p, int unison, float uniDetune, float uniSpread,
                      float subLevel, int subOctave, float noise, float drive,
                      bool osc2 = false) {
    p.unisonVoices = unison;
    p.unisonDetune = uniDetune;
    p.unisonSpread = uniSpread;
    p.subLevel = subLevel;
    p.subOctave = subOctave;
    p.noiseLevel = noise;
    p.drive = drive;
    if (osc2) { p.osc2Wave = Waveform::Saw; p.osc2Level = 0.6f; p.osc2Semi = 0; p.osc2Detune = 9.0f; }
    return p;
}

// v37: ACID filter mode (accent opens the filter, 60 ms linear slide) and the analog DRIFT amount.
SynthPatch Acid(SynthPatch p) { p.acid = true; return p; }
SynthPatch Drift(SynthPatch p, float amount) { p.drift = amount; return p; }


// v44 chainable modifiers: a preset reads as "a base voice plus the layers it uses".
SynthPatch Uni(SynthPatch p, int voices, float detune, float spread) { p.unisonVoices = voices; p.unisonDetune = detune; p.unisonSpread = spread; return p; }
SynthPatch SubL(SynthPatch p, float level, int octave = 1) { p.subLevel = level; p.subOctave = octave; return p; }
SynthPatch Osc2(SynthPatch p, Waveform w, int semi, int cents, float level) { p.osc2Wave = w; p.osc2Semi = semi; p.osc2Detune = cents; p.osc2Level = level; return p; }
SynthPatch Drv(SynthPatch p, float drive) { p.drive = drive; return p; }
SynthPatch Nz(SynthPatch p, float level) { p.noiseLevel = level; return p; }
SynthPatch F24(SynthPatch p) { p.filter24 = true; return p; }
SynthPatch Key(SynthPatch p, float amount) { p.keytrack = amount; return p; }
SynthPatch Sync(SynthPatch p) { p.phaseSync = true; return p; }
SynthPatch Lfo(SynthPatch p, LfoTarget t, float rateHz, float depth, Waveform shape = Waveform::Sine) { p.lfoTarget = t; p.lfoRateHz = rateHz; p.lfoDepth = depth; p.lfoShape = shape; return p; }
SynthPatch ModEnv(SynthPatch p, float amount) { p.fmModEnv = amount; return p; }

struct Entry { const char* name; const char* category; SynthPatch patch; };

// Output level of every preset, measured by t8_presetaudit --write-levels (see SynthPresetLevels.inc): each category lands within a
// couple of dB of its target RMS and under -1 dBFS, so switching presets does not change the loudness of a track.
struct LevelEntry { const char* name; float volume; };
const LevelEntry kLevels[] = {
#include "SynthPresetLevels.inc"
};

} // namespace

const char* const kSynthCategories[kNumSynthCategories] = {"BASS", "LEAD", "PAD", "KEYS", "PLUCK", "FX"};

const std::vector<SynthPresetData>& AllSynthPresets() {
    static const std::vector<SynthPresetData> presets = [] {
        const Waveform Saw = Waveform::Saw, Sq = Waveform::Square, Tri = Waveform::Triangle, Sin = Waveform::Sine;
        const LfoTarget Cut = LfoTarget::Cutoff, Pit = LfoTarget::Pitch, Vol = LfoTarget::Volume, Wtp = LfoTarget::WtPosition;
        std::vector<Entry> e;
        // ============================== BASS ==============================
        // ACID BASS: TB-303 voice -- saw, steep resonant low-pass, a short filter sweep at the front of the note, accent opens the filter, slides
        e.push_back({"ACID BASS", "BASS", Drv(Drift(Acid(MakePatch(Saw, 300.0f, 0.85f, 0.9f, 0.001f, 0.15f, 0.0f, 0.05f, 0.22f, 0.04f, 0.9f)), 0.12f), 0.3f)});
        e.push_back({"ACID SQUARE", "BASS", Drv(Drift(Acid(MakePatch(Sq, 260.0f, 0.9f, 0.95f, 0.001f, 0.2f, 0.0f, 0.05f, 0.3f, 0.06f, 0.85f)), 0.12f), 0.3f)});
        e.push_back({"ACID SQUEAL", "BASS", Drv(Drift(Acid(MakePatch(Saw, 220.0f, 0.95f, 1.0f, 0.001f, 0.3f, 0.25f, 0.06f, 0.35f, 0.06f, 0.8f)), 0.12f), 0.3f)});
        e.push_back({"SUB BASS", "BASS", Drv(MakePatch(Sin, 250.0f, 0.0f, 0.0f, 0.008f, 0.05f, 1.0f, 0.06f, 0.1f, 0.02f, 1.0f), 0.12f)});
        e.push_back({"SUB 808", "BASS", Drv(MakePatch(Sin, 300.0f, 0.0f, 0.0f, 0.002f, 0.7f, 0.0f, 0.25f, 0.1f, 0.03f, 1.0f), 0.4f)});
        e.push_back({"FAT BASS", "BASS", F24(Drv(SubL(Uni(MakePatch(Saw, 700.0f, 0.3f, 0.5f, 0.002f, 0.2f, 0.7f, 0.08f, 0.2f, 0.02f, 1.0f), 3, 0.2f, 0.3f), 0.6f), 0.3f))});
        e.push_back({"SUB+SAW", "BASS", Drv(SubL(Osc2(Uni(MakePatch(Saw, 1200.0f, 0.2f, 0.4f, 0.003f, 0.25f, 0.75f, 0.1f, 0.25f, 0.03f, 1.0f), 2, 0.15f, 0.4f), Saw, 0, 9, 0.6f), 0.7f, 2), 0.2f)});
        e.push_back({"REESE BASS", "BASS", Drift(Drv(SubL(Lfo(F24(Osc2(MakePatch(Saw, 380.0f, 0.2f, 0.25f, 0.02f, 0.3f, 0.85f, 0.12f, 0.4f, 0.12f, 1.0f), Saw, 0, 14, 1.0f)), Cut, 0.3f, 0.1f, Tri), 0.35f), 0.2f), 0.3f)});
        e.push_back({"WOBBLE BASS", "BASS", Drv(SubL(F24(MakePatch(Sq, 700.0f, 0.35f, 0.3f, 0.005f, 0.1f, 0.9f, 0.15f, 0.3f, 0.0f, 1.0f, Cut, 4.7f, 0.65f, Tri)), 0.4f), 0.25f)});
        e.push_back({"DUB BASS", "BASS", Drv(SubL(MakePatch(Tri, 240.0f, 0.15f, 0.35f, 0.004f, 0.3f, 0.8f, 0.2f, 0.2f, 0.05f, 1.0f), 0.55f), 0.15f)});
        e.push_back({"TECHNO BASS", "BASS", Key(Drv(SubL(F24(MakePatch(Saw, 480.0f, 0.3f, 0.6f, 0.001f, 0.2f, 0.0f, 0.08f, 0.13f, 0.0f, 1.0f)), 0.5f), 0.2f), 0.4f)});
        e.push_back({"HOUSE BASS", "BASS", Key(SubL(F24(MakePatch(Saw, 600.0f, 0.2f, 0.5f, 0.002f, 0.3f, 0.35f, 0.1f, 0.2f, 0.0f, 1.0f)), 0.6f), 0.3f)});
        e.push_back({"RUBBER BASS", "BASS", Drv(Osc2(F24(MakePatch(Sq, 400.0f, 0.55f, 0.8f, 0.001f, 0.2f, 0.1f, 0.06f, 0.15f, 0.0f, 1.0f)), Sq, -12, 4, 0.5f), 0.2f)});
        e.push_back({"ELECTRO BASS", "BASS", Drv(Osc2(MakePatch(Sq, 900.0f, 0.4f, 0.6f, 0.001f, 0.14f, 0.3f, 0.05f, 0.1f, 0.0f, 1.0f), Saw, 0, 5, 0.5f), 0.35f)});
        e.push_back({"FM BASS", "BASS", ModEnv(MakeFmPatch(2, {{ FMOperator{1.0f, 0.85f}, FMOperator{0.5f, 0.5f}, FMOperator{1.0f, 0.4f}, FMOperator{2.0f, 0.3f} }}, 0.004f, 0.25f, 0.7f, 0.1f, 1.0f), 0.5f)});
        e.push_back({"FM GROWL", "BASS", Drv(ModEnv(MakeFmPatch(0, {{ FMOperator{1.0f, 0.9f}, FMOperator{1.0f, 0.45f}, FMOperator{2.0f, 0.25f}, FMOperator{0.5f, 0.2f} }}, 0.003f, 0.3f, 0.65f, 0.1f, 1.0f, Pit, 0.3f, 0.0f), 0.6f), 0.15f)});
        e.push_back({"WT VOWEL BASS", "BASS", SubL(MakeWtPatch(2, 0.4f, 1200.0f, 0.3f, 0.2f, 0.15f, 0.005f, 0.2f, 0.75f, 0.1f, 1.0f), 0.3f)});
        // ============================== LEAD ==============================
        e.push_back({"SUPER SAW", "LEAD", Drift(Drv(Uni(MakePatch(Saw, 5500.0f, 0.15f, 0.25f, 0.01f, 0.3f, 0.8f, 0.25f, 0.4f, 0.0f, 0.8f), 5, 0.45f, 0.85f), 0.1f), 0.15f)});
        e.push_back({"DETUNED LEAD", "LEAD", Drv(Uni(F24(Osc2(MakePatch(Saw, 2600.0f, 0.25f, 0.4f, 0.005f, 0.3f, 0.75f, 0.2f, 0.3f, 0.0f, 0.9f), Saw, 0, 12, 0.7f)), 3, 0.3f, 0.6f), 0.1f)});
        e.push_back({"MONO SAW LEAD", "LEAD", Key(Drift(Osc2(MakePatch(Saw, 2200.0f, 0.3f, 0.35f, 0.003f, 0.25f, 0.8f, 0.12f, 0.25f, 0.06f, 0.9f), Saw, 0, 8, 0.6f), 0.1f), 0.4f)});
        e.push_back({"SQUARE LEAD", "LEAD", Key(Uni(Osc2(MakePatch(Sq, 3000.0f, 0.2f, 0.3f, 0.004f, 0.25f, 0.8f, 0.12f, 0.2f, 0.03f, 0.9f), Sq, 12, 6, 0.35f), 2, 0.15f, 0.5f), 0.4f)});
        e.push_back({"SOFT LEAD", "LEAD", Key(Lfo(Osc2(MakePatch(Tri, 3500.0f, 0.1f, 0.2f, 0.02f, 0.3f, 0.85f, 0.25f, 0.3f, 0.05f, 0.9f), Sin, 12, 0, 0.35f), Pit, 5.5f, 0.012f), 0.3f)});
        e.push_back({"ACID LEAD", "LEAD", Drift(Acid(MakePatch(Saw, 900.0f, 0.75f, 0.7f, 0.001f, 0.2f, 0.6f, 0.08f, 0.25f, 0.05f, 0.85f)), 0.1f)});
        e.push_back({"HOOVER", "LEAD", Drv(SubL(Uni(F24(Lfo(MakePatch(Saw, 1800.0f, 0.3f, 0.35f, 0.01f, 0.3f, 0.8f, 0.2f, 0.3f, 0.12f, 0.8f), Pit, 0.35f, 0.012f, Tri)), 6, 0.8f, 1.0f), 0.25f), 0.4f)});
        e.push_back({"TRANCE GATE", "LEAD", Uni(MakePatch(Saw, 3000.0f, 0.25f, 0.3f, 0.001f, 0.1f, 1.0f, 0.05f, 0.2f, 0.0f, 0.85f, Vol, 8.0f, 0.9f, Sq), 3, 0.3f, 0.7f)});
        e.push_back({"WT PWM LEAD", "LEAD", Uni(MakeWtPatch(3, 0.5f, 6000.0f, 0.2f, 0.3f, 0.3f, 0.01f, 0.2f, 0.8f, 0.15f, 0.85f, Wtp, 3.5f, 0.9f, Tri), 2, 0.2f, 0.6f)});
        e.push_back({"WT GLASS LEAD", "LEAD", Key(MakeWtPatch(1, 0.6f, 7000.0f, 0.15f, 0.2f, 0.4f, 0.01f, 0.3f, 0.8f, 0.2f, 0.85f, Wtp, 0.4f, 0.4f, Sin), 0.3f)});
        // ============================== PAD ==============================
        e.push_back({"WARM PAD", "PAD", Drift(Uni(MakePatch(Saw, 2000.0f, 0.15f, 0.2f, 1.8f, 1.5f, 0.65f, 1.9f, 1.5f, 0.0f, 0.7f, Cut, 0.12f, 0.15f, Sin), 3, 0.2f, 0.8f), 0.2f)});
        e.push_back({"WIDE PAD", "PAD", Drift(Uni(MakePatch(Saw, 2500.0f, 0.12f, 0.2f, 1.2f, 1.2f, 0.7f, 1.6f, 1.2f, 0.0f, 0.75f, Cut, 0.15f, 0.2f, Sin), 5, 0.35f, 1.0f), 0.25f)});
        e.push_back({"STRING PAD", "PAD", Drift(Osc2(Uni(MakePatch(Saw, 3200.0f, 0.1f, 0.15f, 0.7f, 1.0f, 0.85f, 1.2f, 0.8f, 0.0f, 0.75f, Pit, 5.0f, 0.006f, Sin), 4, 0.3f, 0.9f), Saw, 0, 11, 0.5f), 0.2f)});
        e.push_back({"DARK PAD", "PAD", Drift(Uni(F24(MakePatch(Saw, 900.0f, 0.2f, 0.35f, 1.5f, 1.8f, 0.7f, 1.9f, 1.8f, 0.0f, 0.75f, Cut, 0.1f, 0.25f, Tri)), 4, 0.25f, 0.9f), 0.3f)});
        e.push_back({"AMBIENT DRONE", "PAD", Drift(Uni(MakePatch(Tri, 800.0f, 0.5f, 0.3f, 1.9f, 1.5f, 0.9f, 1.9f, 1.6f, 0.3f, 0.6f, Cut, 0.08f, 0.4f, Sin), 3, 0.2f, 1.0f), 0.2f)});
        e.push_back({"ANALOG PAD", "PAD", Drift(Osc2(Uni(MakePatch(Saw, 1700.0f, 0.2f, 0.25f, 0.9f, 1.4f, 0.7f, 1.4f, 1.0f, 0.0f, 0.75f, Cut, 0.2f, 0.12f, Sin), 2, 0.25f, 0.8f), Sq, 0, 7, 0.4f), 0.35f)});
        e.push_back({"WT SWEEP PAD", "PAD", Uni(MakeWtPatch(0, 0.3f, 3000.0f, 0.1f, 0.1f, 1.0f, 1.5f, 1.0f, 0.7f, 1.8f, 0.7f, Wtp, 0.1f, 0.5f, Sin), 3, 0.2f, 0.9f)});
        e.push_back({"CHOIR PAD", "PAD", Uni(MakeWtPatch(2, 0.35f, 4500.0f, 0.1f, 0.1f, 1.0f, 1.0f, 1.0f, 0.8f, 1.4f, 0.75f, Wtp, 0.12f, 0.25f, Sin), 3, 0.18f, 0.9f)});
        e.push_back({"GLASS PAD", "PAD", Uni(ModEnv(MakeFmPatch(1, {{ FMOperator{1.0f, 0.5f}, FMOperator{2.0f, 0.35f}, FMOperator{2.0f, 0.4f}, FMOperator{3.0f, 0.2f} }}, 1.0f, 1.2f, 0.7f, 1.6f, 0.7f, Vol, 0.2f, 0.1f, Sin), 0.0f), 3, 0.2f, 0.9f)});
        // ============================== KEYS ==============================
        e.push_back({"FM EPIANO", "KEYS", ModEnv(MakeFmPatch(1, {{ FMOperator{1.0f, 0.55f}, FMOperator{1.0f, 0.35f}, FMOperator{1.0f, 0.3f}, FMOperator{14.0f, 0.1f} }}, 0.001f, 1.1f, 0.2f, 0.4f, 0.85f), 0.9f)});
        e.push_back({"FM BELL", "KEYS", ModEnv(MakeFmPatch(0, {{ FMOperator{1.0f, 0.5f}, FMOperator{3.5f, 0.5f}, FMOperator{1.0f, 0.25f}, FMOperator{7.0f, 0.12f} }}, 0.001f, 1.6f, 0.0f, 1.2f, 0.8f), 1.0f)});
        e.push_back({"TINE KEYS", "KEYS", ModEnv(MakeFmPatch(1, {{ FMOperator{1.0f, 0.5f}, FMOperator{1.0f, 0.3f}, FMOperator{2.0f, 0.3f}, FMOperator{5.0f, 0.08f} }}, 0.001f, 0.9f, 0.15f, 0.3f, 0.85f), 0.8f)});
        e.push_back({"ELECTRIC ORGAN", "KEYS", MakeFmPatch(3, {{ FMOperator{1.0f, 0.6f}, FMOperator{2.0f, 0.4f}, FMOperator{3.0f, 0.2f}, FMOperator{4.0f, 0.15f} }}, 0.004f, 0.1f, 1.0f, 0.06f, 0.8f, Pit, 6.0f, 0.004f, Sin)});
        e.push_back({"CLAV", "KEYS", Key(MakePatch(Sq, 1800.0f, 0.3f, 0.7f, 0.001f, 0.18f, 0.0f, 0.05f, 0.09f, 0.0f, 0.85f), 0.5f)});
        e.push_back({"MARIMBA", "KEYS", ModEnv(MakeFmPatch(0, {{ FMOperator{1.0f, 0.55f}, FMOperator{4.0f, 0.3f}, FMOperator{1.0f, 0.1f}, FMOperator{1.0f, 0.0f} }}, 0.001f, 0.45f, 0.0f, 0.25f, 0.85f), 1.0f)});
        e.push_back({"GLASS BELLS", "KEYS", ModEnv(MakeFmPatch(1, {{ FMOperator{1.0f, 0.4f}, FMOperator{3.0f, 0.4f}, FMOperator{2.0f, 0.3f}, FMOperator{5.19f, 0.2f} }}, 0.001f, 1.8f, 0.0f, 1.4f, 0.8f), 1.0f)});
        // ============================== PLUCK / STAB ==============================
        e.push_back({"TECHNO STAB", "PLUCK", Key(Drv(Uni(MakePatch(Sq, 600.0f, 0.35f, 0.75f, 0.001f, 0.45f, 0.0f, 0.28f, 0.4f, 0.0f, 0.9f), 2, 0.1f, 0.35f), 0.1f), 0.3f)});
        e.push_back({"CHORD STAB", "PLUCK", Key(Uni(Osc2(F24(MakePatch(Saw, 700.0f, 0.2f, 0.7f, 0.002f, 0.35f, 0.0f, 0.15f, 0.3f, 0.0f, 0.9f)), Saw, 0, 9, 0.6f), 3, 0.18f, 0.45f), 0.4f)});
        e.push_back({"HOUSE STAB", "PLUCK", Key(Uni(MakePatch(Sq, 900.0f, 0.15f, 0.55f, 0.002f, 0.28f, 0.0f, 0.12f, 0.25f, 0.0f, 0.9f), 3, 0.15f, 0.4f), 0.35f)});
        e.push_back({"RAVE STAB", "PLUCK", Drv(Uni(F24(MakePatch(Saw, 1500.0f, 0.2f, 0.5f, 0.001f, 0.4f, 0.0f, 0.2f, 0.35f, 0.0f, 0.85f)), 5, 0.45f, 0.7f), 0.3f)});
        e.push_back({"BRASS STAB", "PLUCK", Lfo(Osc2(MakePatch(Saw, 400.0f, 0.2f, 0.65f, 0.08f, 0.05f, 0.95f, 0.15f, 0.8f, 0.02f, 0.9f), Saw, 0, 6, 0.5f), Pit, 5.5f, 0.008f)});
        e.push_back({"PLUCK", "PLUCK", MakePatch(Sq, 2200.0f, 0.45f, 0.7f, 0.001f, 0.09f, 0.0f, 0.06f, 0.08f, 0.0f, 0.85f)});
        e.push_back({"SOFT PLUCK", "PLUCK", Key(Osc2(MakePatch(Tri, 2600.0f, 0.2f, 0.6f, 0.001f, 0.35f, 0.0f, 0.15f, 0.2f, 0.0f, 0.9f), Sin, 12, 0, 0.3f), 0.4f)});
        e.push_back({"ARP PLUCK", "PLUCK", Key(Uni(MakePatch(Saw, 1400.0f, 0.3f, 0.8f, 0.001f, 0.16f, 0.0f, 0.08f, 0.12f, 0.0f, 0.9f), 2, 0.12f, 0.35f), 0.45f)});
        e.push_back({"DUB CHORD", "PLUCK", Key(Uni(F24(MakePatch(Saw, 900.0f, 0.3f, 0.5f, 0.003f, 0.6f, 0.0f, 0.3f, 0.5f, 0.0f, 0.9f)), 3, 0.12f, 0.45f), 0.3f)});
        // ============================== FX ==============================
        e.push_back({"NOISE SWEEP", "FX", Nz(Lfo(MakePatch(Sin, 1500.0f, 0.5f, 0.0f, 0.5f, 0.5f, 1.0f, 0.8f, 0.5f, 0.0f, 0.8f), Cut, 0.25f, 0.9f, Saw), 0.9f)});
        e.push_back({"WIND", "FX", Nz(Lfo(MakePatch(Sin, 1200.0f, 0.6f, 0.0f, 1.2f, 0.5f, 1.0f, 1.5f, 0.5f, 0.0f, 0.7f), Cut, 0.12f, 0.6f, Sin), 1.0f)});
        e.push_back({"LASER ZAP", "FX", Lfo(MakePatch(Saw, 5000.0f, 0.2f, 0.0f, 0.001f, 0.35f, 0.0f, 0.1f, 0.2f, 0.0f, 0.8f), Pit, 4.0f, 0.55f, Saw)});
        e.push_back({"SIREN", "FX", Lfo(MakePatch(Sq, 3000.0f, 0.1f, 0.0f, 0.02f, 0.2f, 1.0f, 0.15f, 0.2f, 0.0f, 0.7f), Pit, 1.2f, 0.3f, Tri)});
        std::vector<SynthPresetData> out;
        out.reserve(e.size());
        for (const Entry& en : e) {
            SynthPresetData d{en.name, en.category, en.patch};
            for (const LevelEntry& l : kLevels) if (std::string(l.name) == en.name) d.patch.volume = l.volume;
            out.push_back(d);
        }
        return out;
    }();
    return presets;
}

std::vector<std::string> PresetTags(const SynthPatch& p) {
    std::vector<std::string> t;
    if (p.unisonVoices > 1) t.push_back("UNI");
    if (p.subLevel > 0.01f) t.push_back("SUB");
    const bool sub = p.engineType == SynthEngineType::Subtractive;
    if (sub && p.osc2Level > 0.01f) t.push_back("OSC2");
    if (sub && p.acid) t.push_back("ACID");
    else if (sub && p.filter24) t.push_back("24dB");
    if (p.lfoTarget != LfoTarget::Off) t.push_back("LFO");
    if (p.engineType == SynthEngineType::FM) t.push_back("FM");
    if (p.engineType == SynthEngineType::Wavetable) t.push_back("WT");
    return t;
}

} // namespace t8
