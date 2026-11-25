#pragma once
// Monophonic 4-operator FM (phase modulation) synth voice -- the Phase 5
// companion to SubtractiveSynth, exposing the identical public interface
// (SetPatch/Patch/NoteOn/NoteOff/Render/IsActive) so SynthVoice can hold
// one of each and dispatch by SynthPatch::engineType with zero change to
// any UI call site (see SynthVoice.h).
//
// Each operator (see FMOperator in Types.h) is a pure sine oscillator
// whose phase always advances at currentFreq_ * op.ratio * dt -- i.e.
// this is PHASE modulation (a modulator's output is added to a carrier's
// phase at read-time), not literal frequency modulation, which is the
// standard, numerically well-behaved way DX7-style synths are actually
// implemented. Operators are computed in REVERSE index order (3,2,1,0)
// so every algorithm below is defined such that a modulator always has a
// strictly higher operator index than whatever it feeds -- that ordering
// guarantees a single top-to-bottom pass is always dependency-correct,
// no separate topological sort needed.
//
// Unlike a real DX7 (independent envelope per operator), this voice
// applies ONE SHARED ADSR envelope -- reusing SynthPatch::attack/decay/
// sustain/release, same as SubtractiveSynth -- to the summed carrier
// output. That's a deliberate simplification for a small live-performance
// groovebox instrument, not an attempt at 1:1 DX7 patch fidelity; it also
// means an FM patch and a subtractive patch on the same synth slot share
// one mental model for "how fast does this note open/close".
#include "Types.h"
#include <array>
#include <cmath>

namespace t8 {

// The 4 fixed operator-routing algorithms. Deliberately a small, curated
// set (not a full 32-algorithm DX7 matrix) chosen to cover the useful
// range for this engine: a serial chain (deepest/most complex timbre), a
// pair of independent 2-op stacks (detuned/ensemble-ish), one carrier fed
// by 3 parallel modulators (dense/noisy/metallic), and 4 independent
// carriers (clean additive fallback, no modulation at all).
constexpr int kNumFmAlgorithms = 4;
extern const char* kFmAlgorithmNames[kNumFmAlgorithms];

class FMSynth {
public:
    void SetPatch(const SynthPatch& p) { patch_ = p; }
    const SynthPatch& Patch() const { return patch_; }

    void NoteOn(int midiNote, float velocity, bool slide);
    void NoteOff();

    // Renders `numFrames` of stereo audio, ADDING into `out` (interleaved).
    void Render(float* out, int numFrames, int sampleRate);

    bool IsActive() const { return stage_ != Stage::Idle; }

    // Unison copy: detune in cents and the starting phase (0..1) of this copy -- set before NoteOn.
    void SyncPhase() { opPhase_.fill(phase0_); }   // v30 PHASE SYNC
    void SetUnisonCopy(float cents, double phase0) { detuneRatio_ = std::pow(2.0, cents / 1200.0); phase0_ = phase0; }

private:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };

    SynthPatch patch_{};
    Stage stage_ = Stage::Idle;
    std::array<double, 4> opPhase_{};
    double targetFreq_ = 220.0;
    double currentFreq_ = 220.0;
    float velocity_ = 1.0f;
    float ampEnvLevel_ = 0.0f;
    float stageTime_ = 0.0f;
    float attackStartLevel_ = 0.0f;    // amp level when the attack began (retrigger over a tail)
    float releaseStartLevel_ = 0.0f;   // amp level captured at NoteOff
    static constexpr float kRetriggerSilence = 0.002f;

    // Same free-running automation LFO convention as SubtractiveSynth --
    // see SynthPatch::lfoTarget/Rate/Depth/Shape.
    double lfoPhase_ = 0.0;

    double detuneRatio_ = 1.0, phase0_ = 0.0;   // unison copy (see SetUnisonCopy)
    static double MidiToFreq(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }
};

} // namespace t8
