#pragma once
// v44.53: the SAMPLE engine of a synth / chord voice. Same public interface as SubtractiveSynth / FMSynth / WavetableSynth, so SynthVoice can
// hold one per unison copy and dispatch by SynthPatch::engineType. Instead of an oscillator it plays a sample of the project (a SamplerVoice,
// shared code with the SAMPLER track): ONE-SHOT at the pitch of the note (ROOT plays it unchanged), SLICES (the note picks the slice) and LOOP
// (the note starts the loop from its beginning, fitted to whole bars). The shape of the sound comes from the synth's own parts: ADSR amplitude
// envelope (a held note holds the sample, NOTE OFF releases it), the low-pass filter with its envelope, the automation LFO (pitch / cutoff /
// volume), glide on slid notes, VOLUME, and (in SynthVoice) UNISON, SUB, NOISE and DRIVE. A new note always restarts the sample.
// Mono per voice: the chord track has one voice per chord tone, so a chord plays the sample at several pitches at once.
#include "SamplerVoice.h"
#include "SynthFilter.h"
#include "Envelope.h"
#include "Types.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace t8 {

class SampleSynth {
public:
    void SetPatch(const SynthPatch& p) {
        patch_ = p;
        FillSamplerPatch(sp_, p.sample);   // the file name stays empty here: the voice gets the sample itself through SetSample
        voice_.SetKeyedLoop(true);
        voice_.SetPatch(sp_);
    }
    const SynthPatch& Patch() const { return patch_; }
    void SetMod(float cutoffOct, float resAdd) { modCutoffOct_ = cutoffOct; modRes_ = resAdd; }
    // The sample (shared, immutable) and the project tempo (SLICES / LOOP fit whole bars of it).
    void SetSample(const SamplePtr& s) { if (s != voice_.Sample()) { voice_.SetSample(s); stage_ = Stage::Idle; ampEnvLevel_ = 0.0f; } }
    const SamplePtr& Sample() const { return voice_.Sample(); }
    void SetTempo(double bpm) { voice_.SetTempo(bpm); }

    void NoteOn(int midiNote, float velocity, bool slide) {
        const bool wasGateOpen = (stage_ == Stage::Attack || stage_ == Stage::Decay || stage_ == Stage::Sustain);
        velocity_ = std::clamp(velocity, 0.0f, 1.0f);
        const bool slices = sp_.mode == 1;
        // legato / glide: a slid note over a sounding one only changes the pitch of the sample that is playing (not for SLICES: a note is a slice)
        if (slide && wasGateOpen && !slices && voice_.IsActive()) {
            targetNote_ = midiNote;
            if (ampEnvLevel_ < kLegatoReviveLevel) { attackStartLevel_ = ampEnvLevel_; stage_ = Stage::Attack; stageTime_ = 0.0f; }
            return;
        }
        // a hard retrigger: the sample starts again from its beginning; the envelope attacks from the level the old note had reached
        if (ampEnvLevel_ < kRetriggerSilence) { filter_[0].Reset(); filter_[1].Reset(); cutoffSmooth_ = -1.0f; }
        voice_.NoteOn(midiNote, 1.0f, rate_);
        if (!voice_.IsActive()) { if (stage_ != Stage::Idle) { stage_ = Stage::Release; stageTime_ = 0.0f; releaseStartLevel_ = ampEnvLevel_; } return; }   // no sample, or no slice for this note
        startNote_ = targetNote_ = curNote_ = midiNote;
        attackStartLevel_ = ampEnvLevel_;
        stage_ = Stage::Attack; stageTime_ = 0.0f; filterEnvTime_ = 0.0f;
    }
    void NoteOff() {
        if (stage_ != Stage::Idle && stage_ != Stage::Release) { stage_ = Stage::Release; stageTime_ = 0.0f; releaseStartLevel_ = ampEnvLevel_; }
    }
    bool IsActive() const { return stage_ != Stage::Idle; }
    void SyncPhase() {}                                                    // nothing to align: a note always starts the sample at its beginning
    void SetUnisonCopy(float cents, double /*phase0*/) { voice_.SetDetune(cents); }

    // Renders `numFrames` of stereo audio, ADDING into `out` (interleaved).
    void Render(float* out, int numFrames, int sampleRate) {
        rate_ = sampleRate;
        if (stage_ == Stage::Idle) return;
        const float dtS = 1.0f / static_cast<float>(sampleRate);
        const SynthFilter::Mode fmode = (patch_.acid || patch_.filter24) ? SynthFilter::Mode::Ladder : SynthFilter::Mode::Pole2;
        for (int done = 0; done < numFrames && stage_ != Stage::Idle;) {
            const int n = std::min(kChunk, numFrames - done);
            const float dt = dtS * static_cast<float>(n);   // one step of the slow things (glide, LFO, filter) per chunk (< 1 ms)

            // ---- pitch: glide, the LFO ----
            const float glideTau = std::max(0.0005f, patch_.glideTime);
            curNote_ += (targetNote_ - curNote_) * std::min(1.0, static_cast<double>(dt) / glideTau);
            float lfoValue = 0.0f;
            if (patch_.lfoTarget != LfoTarget::Off) {
                switch (patch_.lfoShape) {
                    case Waveform::Sine: lfoValue = static_cast<float>(std::sin(6.283185307179586 * lfoPhase_)); break;
                    case Waveform::Triangle: { const double t = lfoPhase_ < 0.5 ? lfoPhase_ * 2.0 : (1.0 - lfoPhase_) * 2.0; lfoValue = static_cast<float>(t * 2.0 - 1.0); break; }
                    case Waveform::Saw: lfoValue = static_cast<float>(2.0 * lfoPhase_ - 1.0); break;
                    case Waveform::Square: lfoValue = lfoPhase_ < 0.5 ? 1.0f : -1.0f; break;
                }
            }
            lfoPhase_ += static_cast<double>(patch_.lfoRateHz) * dt;
            if (lfoPhase_ >= 1.0) lfoPhase_ -= std::floor(lfoPhase_);
            double pitchSemis = (sp_.mode == 1) ? 0.0 : (curNote_ - startNote_);   // SLICES do not transpose, they only follow an LFO
            if (patch_.lfoTarget == LfoTarget::Pitch) pitchSemis += static_cast<double>(patch_.lfoDepth * lfoValue * 12.0f);
            voice_.SetPitchMod(std::pow(2.0, pitchSemis / 12.0));

            // ---- the sample, one chunk ----
            tmp_.fill(0.0f);
            voice_.Render(tmp_.data(), n, sampleRate);

            // ---- filter (once per chunk) ----
            filterEnvTime_ += dt;
            const float filterEnv = std::exp(-filterEnvTime_ / std::max(0.0005f, patch_.filterDecay));
            float cutoffLfoMult = 1.0f;
            if (patch_.lfoTarget == LfoTarget::Cutoff) cutoffLfoMult = std::pow(2.0f, patch_.lfoDepth * lfoValue * 3.0f);
            float cutoffMod = patch_.cutoffHz * (1.0f + patch_.envAmount * 4.0f * filterEnv) * cutoffLfoMult;
            if (modCutoffOct_ != 0.0f) cutoffMod *= std::exp2(modCutoffOct_);
            if (patch_.keytrack > 0.0f) cutoffMod *= static_cast<float>(std::pow(440.0 * std::pow(2.0, (curNote_ - 69.0) / 12.0) / 261.63, static_cast<double>(patch_.keytrack)));
            cutoffMod = std::max(20.0f, std::min(static_cast<float>(sampleRate) * 0.45f, cutoffMod));
            if (cutoffSmooth_ < 0.0f) cutoffSmooth_ = cutoffMod;
            else cutoffSmooth_ += (cutoffMod - cutoffSmooth_) * (1.0f - std::exp(-dt / 0.004f));
            const float res = modRes_ != 0.0f ? std::clamp(patch_.resonance + modRes_, 0.0f, 0.99f) : patch_.resonance;
            const bool open = cutoffSmooth_ >= 17500.0f && res <= 0.2f;   // wide open: the sample passes untouched (no filter colouring)
            if (!open) { filter_[0].Coeffs(fmode, cutoffSmooth_, res, static_cast<float>(sampleRate)); filter_[1].Coeffs(fmode, cutoffSmooth_, res, static_cast<float>(sampleRate)); }
            else { filter_[0].Reset(); filter_[1].Reset(); }
            const float volMul = patch_.lfoTarget == LfoTarget::Volume ? std::max(0.0f, 1.0f + patch_.lfoDepth * lfoValue) : 1.0f;

            // ---- amplitude envelope, per sample ----
            for (int i = 0; i < n; ++i) {
                switch (stage_) {
                    case Stage::Attack: {
                        const float span = EnvAttackSpan(patch_.attack, 440.0);
                        stageTime_ += dtS;
                        const float x = stageTime_ / span;
                        ampEnvLevel_ = attackStartLevel_ + (1.0f - attackStartLevel_) * EnvShape(x, span);
                        if (x >= 1.0f) { ampEnvLevel_ = 1.0f; stage_ = Stage::Decay; stageTime_ = 0; }
                        break;
                    }
                    case Stage::Decay: {
                        const float span = std::max(0.0005f, patch_.decay);
                        ampEnvLevel_ = 1.0f + (patch_.sustain - 1.0f) * std::min(1.0f, stageTime_ / span);
                        stageTime_ += dtS;
                        if (stageTime_ >= span) stage_ = Stage::Sustain;
                        break;
                    }
                    case Stage::Sustain: ampEnvLevel_ = patch_.sustain; break;
                    case Stage::Release: {
                        const float span = EnvReleaseSpan(patch_.release, 440.0);
                        stageTime_ += dtS;
                        const float t = std::min(1.0f, stageTime_ / span);
                        ampEnvLevel_ = releaseStartLevel_ * (1.0f - EnvShape(t, span));
                        if (t >= 1.0f) { ampEnvLevel_ = 0.0f; stage_ = Stage::Idle; voice_.Stop(); }
                        break;
                    }
                    default: break;
                }
                float l = tmp_[static_cast<size_t>(i) * 2], r = tmp_[static_cast<size_t>(i) * 2 + 1];
                if (!open) { l = filter_[0].Process(l); r = filter_[1].Process(r); }
                const float g = ampEnvLevel_ * velocity_ * patch_.volume * volMul;
                out[static_cast<size_t>(done + i) * 2 + 0] += l * g;
                out[static_cast<size_t>(done + i) * 2 + 1] += r * g;
                if (stage_ == Stage::Idle) break;
            }
            done += n;
            if (stage_ != Stage::Idle && !voice_.IsActive()) { stage_ = Stage::Idle; ampEnvLevel_ = 0.0f; }   // the sample is over: nothing more to hold
        }
    }

private:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };
    static constexpr int kChunk = 32;
    static constexpr float kRetriggerSilence = 0.002f;

    SynthPatch patch_{};
    SamplerPatch sp_{};
    SamplerVoice voice_;
    Stage stage_ = Stage::Idle;
    int rate_ = kSampleRate;
    float velocity_ = 1.0f, ampEnvLevel_ = 0.0f, stageTime_ = 0.0f, attackStartLevel_ = 0.0f, releaseStartLevel_ = 0.0f;
    float filterEnvTime_ = 0.0f, modCutoffOct_ = 0.0f, modRes_ = 0.0f, cutoffSmooth_ = -1.0f;
    double lfoPhase_ = 0.0;
    double startNote_ = 60.0, targetNote_ = 60.0, curNote_ = 60.0;
    std::array<SynthFilter, 2> filter_;
    std::array<float, kChunk * 2> tmp_{};
};

} // namespace t8
