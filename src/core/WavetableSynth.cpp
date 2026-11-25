#include "WavetableSynth.h"
#include <algorithm>
#include "Envelope.h"

namespace t8 {

namespace { constexpr double kWtSynthPi = 3.14159265358979323846; }

void WavetableSynth::NoteOn(int midiNote, float velocity, bool slide) {
    const bool wasGateOpen = (stage_ == Stage::Attack || stage_ == Stage::Decay || stage_ == Stage::Sustain);
    targetFreq_ = MidiToFreq(midiNote) * detuneRatio_;
    velocity_ = std::max(0.0f, std::min(1.0f, velocity));
    if (!(slide && stage_ != Stage::Idle)) {
        currentFreq_ = targetFreq_;
        // Only restart phase/filter when the voice is (nearly) silent. If the
        // previous note is still audible, resetting them is a waveform
        // discontinuity = click; keep them running instead (legato-style).
        if (ampEnvLevel_ < kRetriggerSilence) {
            phase_ = phase0_;
            filter_.Reset();
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

void WavetableSynth::NoteOff() {
    if (stage_ != Stage::Idle) {
        stage_ = Stage::Release;
        stageTime_ = 0.0f;
        releaseStartLevel_ = ampEnvLevel_;
    }
}

float WavetableSynth::ReadWavetable(double ph, float pos) const {
    const auto& banks = AllWavetables();
    int bankIdx = std::clamp(patch_.wavetableBank, 0, kNumWavetableBanks - 1);
    const WavetableBank& bank = banks[bankIdx];

    pos = std::clamp(pos, 0.0f, 1.0f);
    float framePosF = pos * (kWtFrames - 1);
    int frameA = static_cast<int>(framePosF);
    int frameB = std::min(frameA + 1, kWtFrames - 1);
    float frameMix = framePosF - static_cast<float>(frameA);

    double sampPosD = ph * kWtFrameSize;
    int sampA = static_cast<int>(sampPosD) % kWtFrameSize;
    int sampB = (sampA + 1) % kWtFrameSize;
    float sampMix = static_cast<float>(sampPosD - std::floor(sampPosD));

    float a0 = bank[frameA][sampA], a1 = bank[frameA][sampB];
    float valA = a0 + (a1 - a0) * sampMix;
    float b0 = bank[frameB][sampA], b1 = bank[frameB][sampB];
    float valB = b0 + (b1 - b0) * sampMix;

    return valA + (valB - valA) * frameMix;
}

void WavetableSynth::Render(float* out, int numFrames, int sampleRate) {
    if (stage_ == Stage::Idle) return;
    const float dt = 1.0f / static_cast<float>(sampleRate);
    const float glideTau = std::max(0.0005f, patch_.glideTime);

    for (int i = 0; i < numFrames; ++i) {
        double diff = targetFreq_ - currentFreq_;
        currentFreq_ += diff * std::min(1.0, static_cast<double>(dt) / glideTau);

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

        filterEnvTime_ += dt;
        float filterEnv = std::exp(-filterEnvTime_ / std::max(0.0005f, patch_.filterDecay));

        // Automation LFO -- same free-running convention/waveform-shape
        // selection as SubtractiveSynth/FMSynth.
        float lfoValue = 0.0f;
        switch (patch_.lfoShape) {
            case Waveform::Sine:
                lfoValue = (patch_.lfoTarget != LfoTarget::Off)
                    ? static_cast<float>(std::sin(2.0 * kWtSynthPi * lfoPhase_)) : 0.0f;
                break;
            case Waveform::Triangle: {
                double t = lfoPhase_ < 0.5 ? lfoPhase_ * 2.0 : (1.0 - lfoPhase_) * 2.0;
                lfoValue = (patch_.lfoTarget != LfoTarget::Off) ? static_cast<float>(t * 2.0 - 1.0) : 0.0f;
                break;
            }
            case Waveform::Saw:
                lfoValue = (patch_.lfoTarget != LfoTarget::Off) ? static_cast<float>(2.0 * lfoPhase_ - 1.0) : 0.0f;
                break;
            case Waveform::Square:
                lfoValue = (patch_.lfoTarget != LfoTarget::Off) ? (lfoPhase_ < 0.5 ? 1.0f : -1.0f) : 0.0f;
                break;
        }
        lfoPhase_ += static_cast<double>(patch_.lfoRateHz) * dt;
        if (lfoPhase_ >= 1.0) lfoPhase_ -= std::floor(lfoPhase_);

        float pitchLfoMult = 1.0f;
        if (patch_.lfoTarget == LfoTarget::Pitch) {
            constexpr float kLfoPitchSemitoneRange = 12.0f;
            pitchLfoMult = std::pow(2.0f, (patch_.lfoDepth * lfoValue * kLfoPitchSemitoneRange) / 12.0f);
        }

        // WtPosition target (Phase 5b, meaningless/no-op on the other two
        // engines -- see LfoTarget's doc comment in Types.h): directly
        // offsets the scan position, clamped into the valid 0..1 range.
        float scanPos = patch_.wtPosition;
        if (patch_.lfoTarget == LfoTarget::WtPosition) {
            scanPos = std::clamp(patch_.wtPosition + patch_.lfoDepth * lfoValue, 0.0f, 1.0f);
        }

        float osc = ReadWavetable(phase_, scanPos);
        phase_ += currentFreq_ * pitchLfoMult * dt;
        if (phase_ >= 1.0) phase_ -= std::floor(phase_);

        float cutoffLfoMult = 1.0f;
        if (patch_.lfoTarget == LfoTarget::Cutoff) {
            constexpr float kLfoCutoffOctaveRange = 3.0f;
            cutoffLfoMult = std::pow(2.0f, patch_.lfoDepth * lfoValue * kLfoCutoffOctaveRange);
        }

        // Same 2-pole state-variable low-pass filter with envelope +
        // resonance as SubtractiveSynth, fed by the wavetable oscillator
        // instead of a formulaic waveform.
        float cutoffMod = patch_.cutoffHz * (1.0f + patch_.envAmount * 4.0f * filterEnv) * cutoffLfoMult;
        if (modCutoffOct_ != 0.0f) cutoffMod *= std::exp2(modCutoffOct_);   // v44.38: a parameter row of the brick
        cutoffMod = std::max(20.0f, std::min(static_cast<float>(sampleRate) * 0.45f, cutoffMod));
        // ~4 ms smoothing: a filter-envelope restart on a still-sounding note would otherwise step the cutoff (pop).
        if (cutoffSmooth_ < 0.0f) cutoffSmooth_ = cutoffMod;
        else cutoffSmooth_ += (cutoffMod - cutoffSmooth_) * (1.0f - std::exp(-dt / 0.004f));
        filter_.Coeffs(SynthFilter::Mode::Pole2, cutoffSmooth_, modRes_ != 0.0f ? std::clamp(patch_.resonance + modRes_, 0.0f, 0.99f) : patch_.resonance, static_cast<float>(sampleRate));   // v37 ZDF filter
        float filtered = filter_.Process(osc);

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
