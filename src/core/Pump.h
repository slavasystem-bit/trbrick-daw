#pragma once
// The envelope of one sidechain pump (see PumpSettings in Types.h). 0 = untouched, 1 = fully down; the instrument's gain is
// 1 - depth * env. Real-time safe: no allocation, no locking, a handful of float operations per sample.
//
// Stages: a trigger starts ATTACK from wherever the envelope is now (so a hit during the way back dips again from there,
// which is the classic "pumping" under a steady kick), env rises to 1 over attackMs, stays there for holdMs, then RELEASE
// brings it back to 0 along the curve. A pump that is switched off while down comes back over kPumpOffMs instead of jumping.
#include "Types.h"
#include <algorithm>
#include <cmath>

namespace t8 {

constexpr float kPumpOffMs = 10.0f;   // the way back when a pump is switched off (or its source changes) while it is down
// The fastest ATTACK. A 0.5 ms smoother alone cannot stop a deep instant duck from ticking on a sub bass; a 2 ms ramp before
// it can (selftest: ATTACK at its minimum on a 55 Hz sine ducked by 90 % stays below the click threshold).
constexpr float kPumpMinAttackMs = 2.0f;

// The shape of the way back: r = 0..1 progress of RELEASE, returns env relative to where RELEASE started.
inline float PumpReleaseShape(int curve, float r) {
    r = std::clamp(r, 0.0f, 1.0f);
    switch (curve) {
        case static_cast<int>(PumpCurve::Smooth): return 1.0f - r * r * (3.0f - 2.0f * r);   // S-curve: no corner at either end
        case static_cast<int>(PumpCurve::Hard): { const float r2 = r * r; return 1.0f - r2 * r2; }   // stays down, then snaps back
        default: return 1.0f - r;                                                            // LINE
    }
}

// Sample counts of one pump at this tempo, computed once per audio block.
struct PumpTiming {
    float attackStep = 2.0f;    // env per sample on the way down (>= 1 = at once)
    int   holdSamples = 0;
    float releaseStep = 1.0f;   // release progress per sample
    int   curve = 0;
};

inline PumpTiming MakePumpTiming(const PumpSettings& p, float sampleRate, double samplesPerSixteenth) {
    PumpTiming t;
    const float attackSamples = std::clamp(p.attackMs, kPumpMinAttackMs, 50.0f) * 0.001f * sampleRate;
    t.attackStep = attackSamples >= 1.0f ? 1.0f / attackSamples : 2.0f;
    t.holdSamples = static_cast<int>(std::clamp(p.holdMs, 0.0f, 500.0f) * 0.001f * sampleRate);
    float releaseSamples;
    if (p.releaseDiv >= 0) releaseSamples = static_cast<float>(PumpReleaseDivSixteenths(p.releaseDiv) * std::max(1.0, samplesPerSixteenth));
    else releaseSamples = std::clamp(p.releaseMs, 10.0f, 1000.0f) * 0.001f * sampleRate;
    t.releaseStep = 1.0f / std::max(1.0f, releaseSamples);
    t.curve = p.curve;
    return t;
}

class PumpEnvelope {
public:
    enum Stage : int { Idle = 0, Attack, Hold, Release };

    void Trigger() { stage_ = Attack; offStep_ = 0.0f; }   // also cancels a pending "switched off" glide
    void Reset() { stage_ = Idle; env_ = 0.0f; pos_ = 0.0f; relFrom_ = 0.0f; holdLeft_ = 0; offStep_ = 0.0f; }
    // Switched off (or re-pointed at another source) while down: come back over kPumpOffMs along a straight line.
    void Off(float sampleRate) {
        if (stage_ == Idle) return;
        stage_ = Release; pos_ = 0.0f; relFrom_ = env_;
        offStep_ = 1.0f / std::max(1.0f, kPumpOffMs * 0.001f * sampleRate);
    }
    bool Active() const { return stage_ != Idle; }
    float Value() const { return env_; }
    int StageNow() const { return stage_; }

    // One sample. `t` is the pump's timing for this block.
    float Next(const PumpTiming& t) {
        switch (stage_) {
            case Attack:
                env_ += t.attackStep;
                if (env_ >= 1.0f) { env_ = 1.0f; stage_ = t.holdSamples > 0 ? Hold : Release; holdLeft_ = t.holdSamples; pos_ = 0.0f; relFrom_ = 1.0f; }
                break;
            case Hold:
                if (--holdLeft_ <= 0) { stage_ = Release; pos_ = 0.0f; relFrom_ = env_; }
                break;
            case Release: {
                const bool off = offStep_ > 0.0f;
                pos_ += off ? offStep_ : t.releaseStep;
                if (pos_ >= 1.0f) { Reset(); break; }
                env_ = relFrom_ * (off ? 1.0f - pos_ : PumpReleaseShape(t.curve, pos_));
                break;
            }
            default: break;
        }
        return env_;
    }
private:
    int   stage_ = Idle;
    float env_ = 0.0f;
    float pos_ = 0.0f;       // RELEASE progress 0..1
    float relFrom_ = 0.0f;   // env where RELEASE started
    int   holdLeft_ = 0;
    float offStep_ = 0.0f;   // > 0 while gliding back after Off()
};

} // namespace t8
