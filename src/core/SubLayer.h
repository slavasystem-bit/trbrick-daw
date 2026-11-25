#pragma once
// Sub oscillator + noise layer that rides along with any synth engine (v26 "thickening").
// It has its own copy of the patch's ADSR so it opens and closes with the note, a sine one or two octaves
// below the note (bypasses the engine's filter so the low end stays clean) and white noise through a
// one-pole low-pass at the patch's cutoff.
#include "Envelope.h"
#include "Types.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace t8 {

class SubLayer {
public:
    void SetPatch(const SynthPatch& p) { patch_ = p; }
    bool Wanted() const { return patch_.subLevel > 0.0f || patch_.noiseLevel > 0.0f; }
    bool IsActive() const { return stage_ != Stage::Idle; }

    void NoteOn(int midiNote, float velocity, bool slide) {
        const bool wasGateOpen = (stage_ == Stage::Attack || stage_ == Stage::Decay || stage_ == Stage::Sustain);
        target_ = 440.0 * std::pow(2.0, (midiNote - 69) / 12.0) / (patch_.subOctave >= 2 ? 4.0 : 2.0);
        vel_ = std::clamp(velocity, 0.0f, 1.0f);
        if (!(slide && stage_ != Stage::Idle)) {
            cur_ = target_;
            if (env_ < 0.002f) phase_ = 0.0;
        }
        if (slide && wasGateOpen) return;
        attackStart_ = env_;
        stage_ = Stage::Attack; t_ = 0.0f;
    }
    void SyncPhase() { phase_ = 0.0; }   // v30 PHASE SYNC
    void NoteOff() {
        if (stage_ != Stage::Idle) { stage_ = Stage::Release; t_ = 0.0f; relStart_ = env_; }
    }

    // ADDS into interleaved stereo `out` (mono duplicated).
    void Render(float* out, int n, int sampleRate) {
        if (stage_ == Stage::Idle) return;
        const float dt = 1.0f / static_cast<float>(sampleRate);
        const float glideTau = std::max(0.0005f, patch_.glideTime);
        const float fc = std::clamp(patch_.cutoffHz * (1.0f + patch_.envAmount), 200.0f, 12000.0f);
        const float lp = 1.0f - std::exp(-6.2831853f * fc * dt);
        for (int i = 0; i < n; ++i) {
            cur_ += (target_ - cur_) * std::min(1.0, static_cast<double>(dt) / glideTau);
            switch (stage_) {
                case Stage::Attack: {
                    float span = EnvAttackSpan(patch_.attack, cur_ * 2.0);
                    t_ += dt; float x = t_ / span;
                    env_ = attackStart_ + (1.0f - attackStart_) * EnvShape(x, span);
                    if (x >= 1.0f) { env_ = 1.0f; stage_ = Stage::Decay; t_ = 0.0f; }
                    break;
                }
                case Stage::Decay: {
                    float span = std::max(0.0005f, patch_.decay);
                    env_ = 1.0f + (patch_.sustain - 1.0f) * std::min(1.0f, t_ / span);
                    t_ += dt;
                    if (t_ >= span) stage_ = Stage::Sustain;
                    break;
                }
                case Stage::Sustain: env_ = patch_.sustain; break;
                case Stage::Release: {
                    float span = EnvReleaseSpan(patch_.release, cur_ * 2.0);
                    t_ += dt; float x = std::min(1.0f, t_ / span);
                    env_ = relStart_ * (1.0f - EnvShape(x, span));
                    if (x >= 1.0f) { stage_ = Stage::Idle; env_ = 0.0f; return; }
                    break;
                }
                default: break;
            }
            float s = 0.0f;
            if (patch_.subLevel > 0.0f) {
                s += static_cast<float>(std::sin(6.283185307179586 * phase_)) * patch_.subLevel * 0.9f;
                phase_ += cur_ * dt; if (phase_ >= 1.0) phase_ -= std::floor(phase_);
            }
            if (patch_.noiseLevel > 0.0f) {
                rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
                float w = (static_cast<float>(rng_ & 0xFFFFFF) / 8388608.0f) - 1.0f;
                nz_ += (w - nz_) * lp;
                s += nz_ * patch_.noiseLevel * 0.6f;
            }
            s *= env_ * vel_ * patch_.volume;
            out[static_cast<size_t>(i) * 2 + 0] += s;
            out[static_cast<size_t>(i) * 2 + 1] += s;
        }
    }

private:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };
    SynthPatch patch_{};
    Stage stage_ = Stage::Idle;
    double target_ = 55.0, cur_ = 55.0, phase_ = 0.0;
    float vel_ = 1.0f, env_ = 0.0f, t_ = 0.0f, attackStart_ = 0.0f, relStart_ = 0.0f, nz_ = 0.0f;
    uint32_t rng_ = 2463534242u;
};

} // namespace t8
