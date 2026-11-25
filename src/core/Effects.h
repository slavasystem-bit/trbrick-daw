#pragma once
// Two shared send effects: a stereo feedback delay (interpolated time, tone, ping-pong) and an 8-line stereo FDN reverb
// (v37). Plain arrays, no allocation in Process().
//
// Both effects are "send" style: AudioEngine accumulates a weighted sum of
// every track's signal (via Mixer::MixTrack's delayBus/reverbBus params,
// weighted by each track's Track::delaySend/reverbSend) into one buffer per
// effect, runs that buffer through Process() here, and mixes the wet output
// back into the master bus scaled by FxSettings::delayLevel/reverbLevel.
#include <array>
#include <cstddef>
#include <vector>

namespace t8 {

// Stereo feedback delay (v2, v37). The delay time is interpolated and glides to a new value instead of jumping, so
// changing the DIVISION, the tempo or the pattern never clicks (it bends the repeats like a tape echo for a moment);
// the feedback path has a high-pass (150 Hz) and a TONE low-pass so the repeats get darker and never build up mud, and a soft
// limiter keeps a high FEEDBACK from running away. PING-PONG sends the repeats alternately left and right.
// Time, feedback, tone and ping-pong are read fresh every Process() call; the delay LINE is persistent state.
class DelayEffect {
public:
    void Init(int sampleRate);

    // input/outWet are interleaved stereo, numFrames frames long. Reads `input` as this block's delay-send bus, writes the
    // delay's wet output (not yet scaled by delayLevel -- the caller does that) to `outWet`. Distinct buffers only.
    // tone 0..1: 0 = dark repeats (1.2 kHz), 1 = bright (12 kHz).
    void Process(const float* input, float* outWet, int numFrames, float timeSec, float feedback, float tone = 0.7f, bool pingPong = false);

private:
    std::vector<float> bufL_, bufR_;
    size_t writePos_ = 0;
    int sampleRate_ = 44100;
    double curDelay_ = -1.0;                 // current (smoothed) delay in samples, -1 = not initialised yet
    float hpL_ = 0, hpR_ = 0, hpPrevL_ = 0, hpPrevR_ = 0, lpL_ = 0, lpR_ = 0;
};

// Reverb v2 (v37): a true stereo 8-line feedback delay network.
//   stereo in -> high-pass (100 Hz) -> pre-delay -> 4 allpass diffusers per side -> injected alternately into the 8 lines
//   -> 8 slowly modulated delay lines (30-65 ms) with a Householder feedback matrix and a damping low-pass in each loop
//   -> left and right are different orthogonal (Hadamard) mixes of the lines, so the two tails are decorrelated.
// SIZE sets the decay time as before (same RT60 curve as the old Freeverb tail), DAMP how fast the highs die away
// and PRE the pre-delay in milliseconds. The tail is stereo: a mono send still comes out wide.
class ReverbEffect {
public:
    void Init(int sampleRate);

    // Same interleaved-stereo in/out convention as DelayEffect::Process.
    // `size` (0..0.97) -> decay time (see Rt60ForSize), `damp` 0..1 (0 = bright, 1 = dark), `preDelayMs` 0..120.
    void Process(const float* input, float* outWet, int numFrames, float size, float damp = 0.5f, float preDelayMs = 12.0f);

    // Decay time in seconds for a SIZE value: 0.3 -> 0.18 s, 0.5 -> 0.3 s, 0.8 -> 0.87 s, 0.9 -> 1.9 s, 0.97 -> 6 s
    static float Rt60ForSize(float size);

private:
    static constexpr int kLines = 8;
    struct Line {
        std::vector<float> buf;
        size_t pos = 0;
        float len = 1000.0f;        // nominal length in samples
        float lp = 0.0f;            // damping low-pass state
        float gain = 0.0f;          // smoothed loop gain (lows)
        float k = 0.0f;             // smoothed damping amount: 1 - highs gain / lows gain
        double cs = 1.0, sn = 0.0, rc = 1.0, rs = 0.0;   // modulation oscillator (rotating phasor) and its step
    };
    struct Allpass { std::vector<float> buf; size_t pos = 0; };
    std::array<Line, kLines> lines_;
    std::array<std::array<Allpass, 4>, 2> diffusers_;
    std::array<std::vector<float>, 2> pre_;
    size_t prePos_ = 0;
    float hpState_[2] = {0, 0}, hpPrev_[2] = {0, 0};
    float hpCoef_ = 0.98f;
    float outGain_ = 0.3f;
    int sampleRate_ = 48000;
};

// Per-track INSERT effects -- unlike DelayEffect/ReverbEffect above (shared
// sends, one instance for the whole project), each track gets its own
// TrackInsertFx instance (see Mixer::TrackState) since it carries small
// persistent DSP state (an EQ filter, a chorus delay line + LFO) that must
// stay independent per track. Applied to a track's own dry signal BEFORE
// its volume/pan/mute (Mixer::MixTrack) -- i.e. pre-fader, like a real
// channel strip's insert slot, as opposed to the post-fader delaySend/
// reverbSend aux sends on Track. Three stages, always in this order:
//   1. EQ -- a single-knob "tilt" tone control (negative = darker/bassier,
//      positive = brighter, 0 = flat/bypassed). Deliberately one knob
//      instead of separate low/high shelf gains -- cheaper to compute (one
//      one-pole filter, not two biquads) and faster to dial in live, which
//      matters more here than surgical control.
//   2. Drive -- the same tanh saturation knee as the existing master-bus
//      DRIVE control (FxSettings::distortionDrive), just applied earlier
//      in the chain and per-track instead of on the whole mix.
//   3. Chorus -- a short (~10-15ms) sine-LFO-modulated delay line mixed
//      with the dry signal, the classic cheap analog-chorus recipe. Mix
//      also scales the LFO depth so a low setting reads as gentle detune
//      rather than an abruptly-audible sweep.
// All three default to 0 (fully bypassed) so existing projects/presets
// sound bit-identical until a user dials one in.
class TrackInsertFx {
public:
    void Init(int sampleRate);

    // In-place stereo process (numFrames frames, interleaved L/R). Knob
    // values are read fresh every call, same "turn it live" convention as
    // DelayEffect/ReverbEffect above.
    void Process(float* buf, int numFrames, float drive, float eqTilt, float chorusMix);

private:
    int sampleRate_ = 44100;
    float eqLpL_ = 0.0f, eqLpR_ = 0.0f; // one-pole low-pass state (tilt EQ), per channel
    float eqCoeff_ = 0.1f;              // one-pole coefficient for a fixed ~800Hz pivot, set in Init
    std::vector<float> chorusBufL_, chorusBufR_;
    size_t chorusWritePos_ = 0;
    float chorusPhase_ = 0.0f; // 0..1, chorus LFO phase
};

// Global master-bus PERFORMANCE FILTER -- a high-pass and a low-pass in
// series, each a 2-stage cascaded one-pole (~-12dB/oct, same one-pole
// building block TrackInsertFx's tilt EQ already uses, just cascaded for a
// steeper/more dramatic sweep) so a live filter sweep is audible without
// the cost of a real biquad. Driven directly by the HPF/LPF XY pad
// (see Mixer::masterHpfAmount/masterLpfAmount) rather
// than a project setting: this is a live performance gesture, not
// something saved with the project, so a fresh launch (or project load)
// always starts fully transparent regardless of what a previous session's
// fader positions were.
class MasterFilter {
public:
    void Init(int sampleRate);
    // In-place stereo process. hpAmount/lpAmount are 0..1 (raw fader
    // value, both 0 = the fast-path no-op every existing project's mix
    // already relies on): hpAmount 0 = HPF fully open (20Hz, transparent)
    // .. 1 = aggressive (4kHz). lpAmount 0 = LPF fully open (20kHz,
    // transparent) .. 1 = aggressive (150Hz) -- "push the fader up" reads
    // as "more effect" on BOTH, even though that means opposite cutoff
    // directions under the hood.
    void Process(float* buf, int numFrames, float hpAmount, float lpAmount);

private:
    int sampleRate_ = 44100;
    float hpLp1L_ = 0.0f, hpLp1R_ = 0.0f, hpLp2L_ = 0.0f, hpLp2R_ = 0.0f; // HPF = input minus a cascaded LP, twice
    float lpL1_ = 0.0f, lpR1_ = 0.0f, lpL2_ = 0.0f, lpR2_ = 0.0f;         // LPF, cascaded twice
    float hpSmooth_ = 0.0f, lpSmooth_ = 0.0f;                              // smoothed fader values (click-free sweeps)
};

// Global master-bus "beat repeat" / stutter effect -- the classic live
// glitch trick: capture a short window of the master signal once, then
// loop that window repeatedly in place of the real audio until switched
// off. Toggled from the LIVE block (AppState::ToggleRepeater) -- this class only
// deals in raw sample counts, turning a density fraction into a frame
// count is the caller's job (see Sequencer::SamplesPerStep).
class RepeaterEffect {
public:
    void Init(int sampleRate);
    // In-place stereo process. `on` false = fully transparent passthrough
    // and resets capture state, so re-enabling always starts a fresh
    // capture rather than resuming a stale one. `captureLenFrames` is
    // re-read every call -- changing it (a live density change) restarts
    // the capture cycle immediately, for a hands-on-the-pad feel.
    void Process(float* buf, int numFrames, bool on, int captureLenFrames);

private:
    std::vector<float> bufferL_, bufferR_;
    int capacity_ = 0;    // allocated size of bufferL_/bufferR_
    int captureLen_ = 0;  // frames in the current capture cycle
    int writePos_ = 0;    // 0..captureLen_ while capturing
    int playPos_ = 0;     // loop read position once capturing is done
    bool capturing_ = false;
    bool wasOn_ = false;
    int fadeLeft_ = 0;    // frames left of the loop -> live crossfade after switching off
};

} // namespace t8
