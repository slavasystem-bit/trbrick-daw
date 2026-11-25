#include "FMSynth.h"
#include <algorithm>
#include "Envelope.h"

namespace t8 {

namespace {
constexpr double kFmPi = 3.14159265358979323846;
// Maps an operator's 0..1 `level` field, when that operator is acting as
// a MODULATOR, onto a phase-modulation index. 8.0 gives a wide range from
// "barely audible sideband" at low levels up to harsh/noisy at max --
// deliberately generous since FM timbres are famous for needing a lot of
// headroom to reach their brighter/metallic extremes.
constexpr float kModIndexScale = 8.0f;

inline float Sine01(double phase) {
    return static_cast<float>(std::sin(2.0 * kFmPi * phase));
}
} // namespace

const char* kFmAlgorithmNames[kNumFmAlgorithms] = {
    "SERIAL CHAIN", "TWO PAIRS", "3>1 STACK", "ADDITIVE"
};

void FMSynth::NoteOn(int midiNote, float velocity, bool slide) {
    const bool wasGateOpen = (stage_ == Stage::Attack || stage_ == Stage::Decay || stage_ == Stage::Sustain);
    targetFreq_ = MidiToFreq(midiNote) * detuneRatio_;
    velocity_ = std::max(0.0f, std::min(1.0f, velocity));
    if (!(slide && stage_ != Stage::Idle)) {
        // hard retrigger: snap pitch, restart every operator's phase and
        // the amp envelope from zero -- same convention as SubtractiveSynth.
        currentFreq_ = targetFreq_;
        // Reset operator phases only when silent (else: waveform click).
        if (ampEnvLevel_ < kRetriggerSilence) opPhase_.fill(phase0_);
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
}

void FMSynth::NoteOff() {
    if (stage_ != Stage::Idle) {
        stage_ = Stage::Release;
        stageTime_ = 0.0f;
        releaseStartLevel_ = ampEnvLevel_;
    }
}

void FMSynth::Render(float* out, int numFrames, int sampleRate) {
    if (stage_ == Stage::Idle) return;
    const float dt = 1.0f / static_cast<float>(sampleRate);
    const float glideTau = std::max(0.0005f, patch_.glideTime);
    const auto& ops = patch_.fmOps;

    for (int i = 0; i < numFrames; ++i) {
        // Pitch glide toward target frequency (exponential approach) --
        // identical to SubtractiveSynth::Render.
        double diff = targetFreq_ - currentFreq_;
        currentFreq_ += diff * std::min(1.0, static_cast<double>(dt) / glideTau);

        // Amplitude ADSR -- identical state machine to SubtractiveSynth,
        // reused verbatim since both engines share the same ADSR fields
        // on SynthPatch (see the class doc comment for why: one shared
        // envelope model instead of per-operator DX7-style envelopes).
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

        // Automation LFO -- same free-running convention as
        // SubtractiveSynth, including its waveform-shape selection.
        // Cutoff is meaningless for an FM voice (no filter in this
        // engine), so it's a documented no-op target here; Pitch and
        // Volume behave identically to the subtractive engine.
        float lfoValue = 0.0f;
        switch (patch_.lfoShape) {
            case Waveform::Sine: lfoValue = (patch_.lfoTarget != LfoTarget::Off) ? Sine01(lfoPhase_) : 0.0f; break;
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

        // Advance every operator's own phase at currentFreq_ * ratio --
        // modulation is applied only at READ time below (phase
        // modulation), never by perturbing these running phases directly,
        // so operator pitch stays exact regardless of modulation depth.
        for (int op = 0; op < 4; ++op) {
            opPhase_[op] += currentFreq_ * ops[op].ratio * pitchLfoMult * dt;
            if (opPhase_[op] >= 1.0) opPhase_[op] -= std::floor(opPhase_[op]);
        }

        // Operators are combined in strictly decreasing index order (a
        // modulator always has a higher index than what it feeds), so a
        // single top-to-bottom pass per algorithm is always
        // dependency-correct -- see the class doc comment.
        float carrierSum = 0.0f;
        // v44 MOD ENV: the modulation index follows the amp envelope, so the timbre darkens as the note fades
        const float idxScale = patch_.fmIndexScale * (1.0f - patch_.fmModEnv + patch_.fmModEnv * ampEnvLevel_ * (0.5f + 0.5f * velocity_));
        switch (patch_.fmAlgorithm) {
            case 0: { // SERIAL CHAIN: op0(carrier) <- op1 <- op2 <- op3
                float raw3 = Sine01(opPhase_[3]);
                float raw2 = Sine01(opPhase_[2] + raw3 * ops[3].level * idxScale);
                float raw1 = Sine01(opPhase_[1] + raw2 * ops[2].level * idxScale);
                float raw0 = Sine01(opPhase_[0] + raw1 * ops[1].level * idxScale);
                carrierSum = raw0 * ops[0].level;
                break;
            }
            case 1: { // TWO PAIRS: op0<-op1, op2<-op3, both carriers
                float raw1 = Sine01(opPhase_[1]);
                float raw0 = Sine01(opPhase_[0] + raw1 * ops[1].level * idxScale);
                float raw3 = Sine01(opPhase_[3]);
                float raw2 = Sine01(opPhase_[2] + raw3 * ops[3].level * idxScale);
                carrierSum = raw0 * ops[0].level + raw2 * ops[2].level;
                break;
            }
            case 2: { // 3>1 STACK: op0 <- (op1 + op2 + op3), 3 parallel modulators
                float raw1 = Sine01(opPhase_[1]);
                float raw2 = Sine01(opPhase_[2]);
                float raw3 = Sine01(opPhase_[3]);
                float totalMod = raw1 * ops[1].level * idxScale
                                + raw2 * ops[2].level * idxScale
                                + raw3 * ops[3].level * idxScale;
                float raw0 = Sine01(opPhase_[0] + totalMod);
                carrierSum = raw0 * ops[0].level;
                break;
            }
            case 3: default: { // ADDITIVE: 4 independent carriers, no modulation
                float raw0 = Sine01(opPhase_[0]);
                float raw1 = Sine01(opPhase_[1]);
                float raw2 = Sine01(opPhase_[2]);
                float raw3 = Sine01(opPhase_[3]);
                carrierSum = 0.5f * (raw0 * ops[0].level + raw1 * ops[1].level
                                    + raw2 * ops[2].level + raw3 * ops[3].level);
                break;
            }
        }

        float volLfoMult = 1.0f;
        if (patch_.lfoTarget == LfoTarget::Volume) {
            volLfoMult = std::max(0.0f, 1.0f + patch_.lfoDepth * lfoValue);
        }

        float sample = carrierSum * ampEnvLevel_ * velocity_ * patch_.volume * volLfoMult;
        out[static_cast<size_t>(i) * 2 + 0] += sample;
        out[static_cast<size_t>(i) * 2 + 1] += sample;
    }
}

} // namespace t8
