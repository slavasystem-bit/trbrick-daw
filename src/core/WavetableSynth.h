#pragma once
// Phase 5b: monophonic wavetable synth voice. Same public interface as
// SubtractiveSynth/FMSynth (SetPatch/Patch/NoteOn/NoteOff/Render/
// IsActive) so SynthVoice can hold one of each and dispatch by
// SynthPatch::engineType with no change to any UI call site.
//
// Structurally this is SubtractiveSynth with the oscillator swapped: the
// same 2-pole state-variable low-pass filter (with its own envelope +
// resonance), the same ADSR amplitude envelope, the same glide, and the
// same automation LFO (now with an added WtPosition target, see Types.h)
// are all reused verbatim -- only the audio-rate oscillator itself
// differs, reading from a wavetable bank (see Wavetables.h) at
// SynthPatch::wtPosition instead of computing a fixed Waveform formula.
// Deliberately duplicated rather than shared with SubtractiveSynth (same
// small-duplication-over-premature-abstraction convention FMSynth already
// established for its own ADSR/LFO code) so each engine file stays
// self-contained and easy to reason about on its own.
#include "Types.h"
#include "SynthFilter.h"
#include "Wavetables.h"
#include <cmath>

namespace t8 {

class WavetableSynth {
public:
    void SetPatch(const SynthPatch& p) { patch_ = p; }
    // v44.38: parameter-row offsets (cutoff in octaves, resonance added); runtime only, never part of the patch
    void SetMod(float cutoffOct, float resAdd) { modCutoffOct_ = cutoffOct; modRes_ = resAdd; }
    const SynthPatch& Patch() const { return patch_; }

    void NoteOn(int midiNote, float velocity, bool slide);
    void NoteOff();

    // Renders `numFrames` of stereo audio, ADDING into `out` (interleaved).
    void Render(float* out, int numFrames, int sampleRate);

    bool IsActive() const { return stage_ != Stage::Idle; }

    // Unison copy: detune in cents and the starting phase (0..1) of this copy -- set before NoteOn.
    void SyncPhase() { phase_ = phase0_; }   // v30 PHASE SYNC
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

    SynthFilter filter_;               // v37 zero-delay-feedback filter (SynthFilter.h)

    double lfoPhase_ = 0.0;

    double detuneRatio_ = 1.0, phase0_ = 0.0;   // unison copy (see SetUnisonCopy)
    static double MidiToFreq(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

    // Reads the currently-selected wavetable bank at `pos` (0..1, which
    // frame(s)) and `phase` (0..1, where in the single-cycle frame),
    // linearly interpolating both between adjacent frames and between
    // adjacent samples within a frame.
    float ReadWavetable(double phase, float pos) const;
};

} // namespace t8
