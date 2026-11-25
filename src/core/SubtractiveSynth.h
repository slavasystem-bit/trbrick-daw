#pragma once
// Monophonic subtractive synth voice: selectable oscillator waveform
// (sine/tri/saw/square), a resonant 2-pole low-pass filter with its own
// decay envelope (acid/303-style), an ADSR amplitude envelope, and glide
// (portamento) for legato/slide steps. One instance per synth track (3 total).
#include "Types.h"
#include "SynthFilter.h"
#include <cmath>
#include <cstdint>

namespace t8 {

class SubtractiveSynth {
public:
    SubtractiveSynth();
    void SetPatch(const SynthPatch& p) { patch_ = p; }
    // v44.38: parameter-row offsets (cutoff in octaves, resonance added); runtime only, never part of the patch
    void SetMod(float cutoffOct, float resAdd) { modCutoffOct_ = cutoffOct; modRes_ = resAdd; }
    const SynthPatch& Patch() const { return patch_; }

    // slide: glide from the currently-playing pitch into this note instead
    // of hard-retriggering the envelopes (303-style tied notes).
    void NoteOn(int midiNote, float velocity, bool slide);
    void NoteOff();

    // Renders `numFrames` of stereo audio, ADDING into `out` (interleaved).
    void Render(float* out, int numFrames, int sampleRate);

    bool IsActive() const { return stage_ != Stage::Idle; }

    // Unison copy: detune in cents and the starting phase (0..1) of this copy -- set before NoteOn.
    void SyncPhase() { phase_ = phase0_; phase2_ = phase0_; }   // v30 PHASE SYNC (called while the voice is ducked to silence)
    void SetUnisonCopy(float cents, double phase0) { detuneRatio_ = std::pow(2.0, cents / 1200.0); phase0_ = phase0; }

private:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };

    SynthPatch patch_{};
    Stage stage_ = Stage::Idle;
    double phase_ = 0.0;
    double targetFreq_ = 220.0;
    double currentFreq_ = 220.0;
    float velocity_ = 1.0f;
    float ampEnvLevel_ = 0.0f;
    float stageTime_ = 0.0f;
    float attackStartLevel_ = 0.0f;    // amp level when the attack began (retrigger over a tail)
    float releaseStartLevel_ = 0.0f;   // amp level captured at NoteOff
    static constexpr float kRetriggerSilence = 0.002f;
    float filterEnvTime_ = 0.0f;
    float modCutoffOct_ = 0.0f, modRes_ = 0.0f;   // v44.38
    float cutoffSmooth_ = -1.0f;       // 1-pole smoothed cutoff (anti-zipper/pop)

    // v37: oscillators + filter run at 2x the output rate (SynthFilter.h), then a half-band decimator brings them back
    SynthFilter filter_;
    HalfbandDecimator dec_;
    int sr_ = 48000;                   // last render rate (NoteOn needs it for the slide length)

    // v37 ACID mode: accent builds a "capacitor" that opens the filter further on consecutive accents (303), and a
    // slide is a fixed-length glide that is linear in pitch (log frequency) instead of an exponential approach.
    float accentCap_ = 0.0f;
    bool  accentNow_ = false;
    int   glideLeft_ = 0;
    double glideLog_ = 0.0, glideStep_ = 0.0;

    // v37 analog DRIFT: two slow, independent random walks (pitch, cutoff) per voice, deterministic seed
    uint32_t rng_ = 0x9E3779B9u;
    float drift1_ = 0.0f, drift2_ = 0.0f;
    float NextNoise() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; return static_cast<float>(rng_) * (2.0f / 4294967296.0f) - 1.0f; }
    double phase2_ = 0.0;                      // second oscillator

    // Automation LFO phase (see SynthPatch::lfoTarget/Rate/Depth/Shape) --
    // free-running: NOT reset on NoteOn/NoteOff, so it keeps cycling
    // continuously across notes rather than re-syncing to each new note,
    // matching "live automation" rather than a per-note modulation effect
    // like vibrato-on-attack would be.
    double lfoPhase_ = 0.0;

    double detuneRatio_ = 1.0, phase0_ = 0.0;   // unison copy (see SetUnisonCopy)
    static double MidiToFreq(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }
    float Oscillator(double phase, Waveform shape, double dt = 0.0) const;   // dt > 0: band-limited (PolyBLEP) saw / square
};

} // namespace t8
