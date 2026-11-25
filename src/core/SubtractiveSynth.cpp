#include "SubtractiveSynth.h"
#include <algorithm>
#include "Envelope.h"

namespace t8 {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr float kAccentVelocity = 0.98f;      // the sequencer sends 1.0 for an accented step (0.8 otherwise); MIDI 125+ counts too
constexpr float kSlideSeconds = 0.060f;       // 303 slide time
uint32_t g_seed = 0x1234567u;
}

SubtractiveSynth::SubtractiveSynth() {
    g_seed = g_seed * 1664525u + 1013904223u;   // every voice (and unison copy) gets its own, reproducible drift
    rng_ = g_seed | 1u;
}

void SubtractiveSynth::NoteOn(int midiNote, float velocity, bool slide) {
    const bool wasGateOpen = (stage_ == Stage::Attack || stage_ == Stage::Decay || stage_ == Stage::Sustain);
    targetFreq_ = MidiToFreq(midiNote) * detuneRatio_;
    velocity_ = std::max(0.0f, std::min(1.0f, velocity));
    accentNow_ = patch_.acid && velocity >= kAccentVelocity;
    if (patch_.acid) {
        if (accentNow_) accentCap_ += (1.0f - accentCap_) * 0.55f;
    } else accentCap_ = 0.0f;
    glideLeft_ = 0;
    if (slide && stage_ != Stage::Idle && patch_.acid && std::fabs(targetFreq_ - currentFreq_) > 1e-6) {
        const int n = std::max(1, static_cast<int>(std::max(kSlideSeconds, patch_.glideTime) * sr_));
        glideLog_ = std::log2(std::max(1.0, currentFreq_));
        glideStep_ = (std::log2(targetFreq_) - glideLog_) / n;
        glideLeft_ = n;
    }
    if (!(slide && stage_ != Stage::Idle)) {
        // hard retrigger: snap pitch, restart envelopes from zero
        currentFreq_ = targetFreq_;
        // Only restart phase/filter when the voice is (nearly) silent. If the
        // previous note is still audible, resetting them is a waveform
        // discontinuity = click; keep them running instead (legato-style).
        if (ampEnvLevel_ < kRetriggerSilence) {
            phase_ = phase0_;
            phase2_ = phase0_;
            filter_.Reset();
            dec_.Reset();
            cutoffSmooth_ = -1.0f;   // snap smoother to first target
        }
    }
    // Legato/tie: a slide into a note whose gate is still open (attack/decay/
    // sustain) just changes pitch -- envelopes keep running (303 behaviour;
    // also what makes a looped endless pad restart without any retrigger).
    if (slide && wasGateOpen) {
        if (ampEnvLevel_ < kLegatoReviveLevel) {   // see Envelope.h: a faded plucky note must not stay silent when it is tied
            attackStartLevel_ = ampEnvLevel_;
            stage_ = Stage::Attack;
            stageTime_ = 0.0f;
        }
        return;
    }
    attackStartLevel_ = ampEnvLevel_;
    stage_ = Stage::Attack;
    stageTime_ = 0.0f;
    filterEnvTime_ = 0.0f;
}

void SubtractiveSynth::NoteOff() {
    if (stage_ != Stage::Idle) {
        stage_ = Stage::Release;
        stageTime_ = 0.0f;
        releaseStartLevel_ = ampEnvLevel_;
    }
}

namespace {
// PolyBLEP: subtracts the band-limited residual of a unit step at the phase
// wrap so saw / square no longer alias on high notes. `t` = phase 0..1,
// `dt` = phase increment per sample.
inline double PolyBlep(double t, double dt) {
    if (t < dt) { double x = t / dt; return x + x - x * x - 1.0; }
    if (t > 1.0 - dt) { double x = (t - 1.0) / dt; return x * x + x + x + 1.0; }
    return 0.0;
}
} // namespace

float SubtractiveSynth::Oscillator(double ph, Waveform shape, double dt) const {
    // ph in [0,1). Shared by the main audio-rate oscillator AND the
    // automation LFO below (see Render) -- both just need a -1..1
    // periodic wave from a 0..1 phase, so one function covers both rather
    // than duplicating the same 4 waveform shapes.
    switch (shape) {
        case Waveform::Sine:
            return static_cast<float>(std::sin(2.0 * kPi * ph));
        case Waveform::Triangle: {
            double t = ph < 0.5 ? ph * 2.0 : (1.0 - ph) * 2.0;
            return static_cast<float>(t * 2.0 - 1.0);
        }
        case Waveform::Saw: {
            double v = 2.0 * ph - 1.0;
            if (dt > 0.0) v -= PolyBlep(ph, dt);
            return static_cast<float>(v);
        }
        case Waveform::Square: {
            double v = ph < 0.5 ? 1.0 : -1.0;
            if (dt > 0.0) {
                v += PolyBlep(ph, dt);
                double p2 = ph + 0.5; if (p2 >= 1.0) p2 -= 1.0;
                v -= PolyBlep(p2, dt);
            }
            return static_cast<float>(v);
        }
    }
    return 0.0f;
}

void SubtractiveSynth::Render(float* out, int numFrames, int sampleRate) {
    if (stage_ == Stage::Idle) return;
    sr_ = sampleRate;
    const float dt = 1.0f / static_cast<float>(sampleRate);
    const float smoothCoef = 1.0f - std::exp(-dt / 0.004f);
    const float driftCoef = 1.0f - std::exp(-6.2831853f * 0.6f * dt);                 // ~0.6 Hz wander
    const float driftNorm = std::sqrt(2.0f / driftCoef) / 0.5774f;             // roughly unit RMS (uniform noise = 0.577)
    const SynthFilter::Mode fmode = patch_.acid ? SynthFilter::Mode::Acid : (patch_.filter24 ? SynthFilter::Mode::Ladder : SynthFilter::Mode::Pole2);
    const float glideTau = std::max(0.0005f, patch_.glideTime);
    const double osc2Ratio = std::pow(2.0, patch_.osc2Semi / 12.0 + patch_.osc2Detune / 1200.0);

    for (int i = 0; i < numFrames; ++i) {
        // Pitch glide toward target frequency (exponential approach).
        if (glideLeft_ > 0) {                       // ACID slide: fixed length, linear in log frequency
            glideLog_ += glideStep_;
            currentFreq_ = std::exp2(glideLog_);
            if (--glideLeft_ == 0) currentFreq_ = targetFreq_;
        } else {
            double diff = targetFreq_ - currentFreq_;
            currentFreq_ += diff * std::min(1.0, static_cast<double>(dt) / glideTau);
        }
        if (accentCap_ > 0.0f) accentCap_ *= 1.0f - dt / 0.45f;     // the accent "capacitor" empties in ~0.5 s
        drift1_ += (NextNoise() - drift1_) * driftCoef;
        drift2_ += (NextNoise() - drift2_) * driftCoef;

        // Amplitude ADSR
        switch (stage_) {
            case Stage::Attack:
            {   // attack: pitch-aware minimum, raised-cosine when short (see Envelope.h)
                float span = EnvAttackSpan(patch_.attack, currentFreq_);
                stageTime_ += dt;
                float x = stageTime_ / span;
                ampEnvLevel_ = attackStartLevel_ + (1.0f - attackStartLevel_) * EnvShape(x, span);
                if (x >= 1.0f) { ampEnvLevel_ = 1.0f; stage_ = Stage::Decay; stageTime_ = 0; }
                break;
            }
            case Stage::Decay: {
                float span = std::max(0.0005f, patch_.decay);
                ampEnvLevel_ = 1.0f + (patch_.sustain - 1.0f) * std::min(1.0f, stageTime_ / span);
                stageTime_ += dt;
                if (stageTime_ >= span) { stage_ = Stage::Sustain; }
                break;
            }
            case Stage::Sustain:
                ampEnvLevel_ = patch_.sustain;
                break;
            case Stage::Release: {
                float span = EnvReleaseSpan(patch_.release, currentFreq_);   // pitch-aware anti-click floor
                // Linear fall from the level captured at NoteOff (NOT re-read
                // each sample -- that made the tail decay geometrically and
                // collapse to a few ms regardless of the release knob).
                stageTime_ += dt;
                float t = std::min(1.0f, stageTime_ / span);
                ampEnvLevel_ = releaseStartLevel_ * (1.0f - EnvShape(t, span));
                if (t >= 1.0f) { stage_ = Stage::Idle; ampEnvLevel_ = 0.0f; return; }
                break;
            }
            default: break;
        }

        // Filter envelope: simple exponential decay from 1 -> 0, acid-style.
        filterEnvTime_ += dt;
        const float fDecay = (accentNow_ ? 0.6f : 1.0f) * patch_.filterDecay;       // ACID accent: snappier sweep
        float filterEnv = std::exp(-filterEnvTime_ / std::max(0.0005f, fDecay));

        // Automation LFO (see SynthPatch::lfoTarget/Rate/Depth/Shape):
        // free-running (phase always advances, even when lfoTarget is Off,
        // so re-enabling it later doesn't jump), -1..1 bipolar value reused
        // for whichever single target is selected below.
        float lfoValue = (patch_.lfoTarget != LfoTarget::Off)
                              ? Oscillator(lfoPhase_, patch_.lfoShape) : 0.0f;
        lfoPhase_ += static_cast<double>(patch_.lfoRateHz) * dt;
        if (lfoPhase_ >= 1.0) lfoPhase_ -= std::floor(lfoPhase_);

        // Pitch target: up to +-1 octave of vibrato/sweep at full depth,
        // applied as a multiplier on the oscillator's frequency for this
        // sample (so it's actual pitch modulation, not just amplitude).
        float pitchLfoMult = 1.0f;
        if (patch_.lfoTarget == LfoTarget::Pitch) {
            constexpr float kLfoPitchSemitoneRange = 12.0f;
            pitchLfoMult = std::pow(2.0f, (patch_.lfoDepth * lfoValue * kLfoPitchSemitoneRange) / 12.0f);
        }

        // Oscillators + filter at 2x the output rate; the half-band decimator below brings the result back.
        const float driftPitch = patch_.drift > 0.0f ? std::exp2(patch_.drift * 5.0f * driftNorm * drift1_ / 1200.0f) : 1.0f;   // about 5 cents rms at DRIFT 1
        const double phaseInc = currentFreq_ * pitchLfoMult * driftPitch * dt;
        const double incH = phaseInc * 0.5, inc2H = incH * osc2Ratio;

        // Cutoff target: up to +-3 octaves of filter sweep at full depth (classic "wobble"/filter-LFO territory)
        float cutoffLfoMult = 1.0f;
        if (patch_.lfoTarget == LfoTarget::Cutoff) {
            constexpr float kLfoCutoffOctaveRange = 3.0f;
            cutoffLfoMult = std::pow(2.0f, patch_.lfoDepth * lfoValue * kLfoCutoffOctaveRange);
        }
        const float accentOpen = patch_.acid ? 1.0f + 1.6f * accentCap_ + (accentNow_ ? 0.5f : 0.0f) : 1.0f;
        float cutoffMod = patch_.cutoffHz * (1.0f + patch_.envAmount * 4.0f * filterEnv * accentOpen) * cutoffLfoMult;
        if (patch_.drift > 0.0f) cutoffMod *= std::exp2(patch_.drift * 0.08f * driftNorm * drift2_);
        if (patch_.keytrack > 0.0f)   // the cutoff follows the played pitch (1 = one octave per octave, C4 = neutral)
            cutoffMod *= static_cast<float>(std::pow(currentFreq_ / 261.63, static_cast<double>(patch_.keytrack)));
        if (modCutoffOct_ != 0.0f) cutoffMod *= std::exp2(modCutoffOct_);   // v44.38: a parameter row of the brick
        cutoffMod = std::max(20.0f, std::min(static_cast<float>(sampleRate) * 0.42f, cutoffMod));
        // ~4 ms smoothing: a filter-envelope restart on a still-sounding note would otherwise step the cutoff and ring the resonance (pop).
        if (cutoffSmooth_ < 0.0f) cutoffSmooth_ = cutoffMod;
        else cutoffSmooth_ += (cutoffMod - cutoffSmooth_) * smoothCoef;
        float res = std::min(0.99f, patch_.resonance + modRes_ + (patch_.acid ? 0.05f * accentCap_ : 0.0f));
        if (modRes_ != 0.0f) res = std::max(0.0f, res);   // v44.38: a parameter row cannot push it below zero
        filter_.Coeffs(fmode, cutoffSmooth_, res, 2.0f * static_cast<float>(sampleRate));

        for (int h = 0; h < 2; ++h) {
            float osc = Oscillator(phase_, patch_.waveform, std::min(0.45, incH));
            phase_ += incH;
            if (phase_ >= 1.0) phase_ -= std::floor(phase_);
            if (patch_.osc2Level > 0.0f) {
                // second oscillator (own wave, semitone + cents offset) -- mixed before the filter
                float o2 = Oscillator(phase2_, patch_.osc2Wave, std::min(0.45, inc2H));
                phase2_ += inc2H;
                if (phase2_ >= 1.0) phase2_ -= std::floor(phase2_);
                osc = (osc + patch_.osc2Level * o2) * (1.0f / (1.0f + 0.5f * patch_.osc2Level));
            }
            dec_.Push(filter_.Process(osc));
        }
        const float filtered = dec_.Out();

        // Volume target: tremolo, from near-silent to a slight boost at
        // full depth (1 +- depth around unity), clamped non-negative.
        float volLfoMult = 1.0f;
        if (patch_.lfoTarget == LfoTarget::Volume) {
            volLfoMult = std::max(0.0f, 1.0f + patch_.lfoDepth * lfoValue);
        }

        float sample = filtered * ampEnvLevel_ * velocity_ * patch_.volume * volLfoMult;
        out[static_cast<size_t>(i) * 2 + 0] += sample;
        out[static_cast<size_t>(i) * 2 + 1] += sample;
    }
}

} // namespace t8
