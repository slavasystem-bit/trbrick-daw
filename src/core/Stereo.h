#pragma once
// Stereo tools for the mixer.
//
// PanGains: constant-power pan law with the centre at unity (so a centred track is exactly as loud as before and a hard-panned
// one is no longer 3 dB quieter than a centred one: left^2 + right^2 stays 2 at every position).
//
// StereoWidth: per-track mid/side width plus a mono-compatible decorrelator ("spread") for mono sources such as hats and
// percussion. With m = (L+R)/2 and s = (L-R)/2:
//     s' = s * width + spread * D(highpass(m)),   L' = m + s',   R' = m - s'
// D is a cascade of two Schroeder all-passes (flat magnitude, scrambled phase). Because the added part is on the side
// channel only, L'+R' = L+R exactly: a mono sum (club system, phone) hears the original signal untouched. Everything
// below ~220 Hz stays centred, so kick and bass are never smeared.
#include <algorithm>
#include <array>
#include <cmath>

namespace t8 {

inline void PanGains(float pan, float& left, float& right) {
    const float p = std::clamp(pan, -1.0f, 1.0f);
    if (p == 0.0f) { left = right = 1.0f; return; }
    const float a = (p + 1.0f) * 0.78539816339f;       // 0 .. pi/2
    left = 1.41421356f * std::cos(a);
    right = 1.41421356f * std::sin(a);
}

class StereoWidth {
public:
    void Init(int sampleRate) {
        sampleRate_ = std::max(8000, sampleRate);
        d1_ = std::clamp(static_cast<int>(0.0053 * sampleRate_), 8, kMaxDelay - 1);
        d2_ = std::clamp(static_cast<int>(0.0081 * sampleRate_), 8, kMaxDelay - 1);
        hpCoeff_ = 1.0f - std::exp(-2.0f * 3.14159265f * 220.0f / static_cast<float>(sampleRate_));
        Clear();
    }
    void Clear() { b1_.fill(0.0f); b2_.fill(0.0f); w1_ = w2_ = 0; lp_ = 0.0f; }

    // true when the stage does nothing (the fast path); the state is cleared so re-enabling starts clean
    bool Idle(float width, float spread) {
        const bool idle = std::fabs(width - 1.0f) < 1e-4f && spread < 1e-4f;
        if (idle && active_) { Clear(); active_ = false; }
        if (!idle) active_ = true;
        return idle;
    }

    // one stereo frame in place
    void Process(float& l, float& r, float width, float spread) {
        const float m = 0.5f * (l + r);
        float s = 0.5f * (l - r) * width;
        if (spread > 1e-4f) {
            lp_ += hpCoeff_ * (m - lp_);
            const float x = m - lp_;                                    // high-passed mid
            // allpass 1: y = -g x + z, z <- x + g y   (delay line form)
            const float z1 = b1_[(w1_ + kMaxDelay - d1_) % kMaxDelay];
            const float y1 = -kG * x + z1; b1_[w1_] = x + kG * y1; w1_ = (w1_ + 1) % kMaxDelay;
            const float z2 = b2_[(w2_ + kMaxDelay - d2_) % kMaxDelay];
            const float y2 = -kG * y1 + z2; b2_[w2_] = y1 + kG * y2; w2_ = (w2_ + 1) % kMaxDelay;
            s += spread * y2;
        }
        l = m + s; r = m - s;
    }

private:
    static constexpr int kMaxDelay = 1024;
    static constexpr float kG = 0.55f;
    int sampleRate_ = 44100, d1_ = 233, d2_ = 357, w1_ = 0, w2_ = 0;
    float hpCoeff_ = 0.03f, lp_ = 0.0f;
    bool active_ = false;
    std::array<float, kMaxDelay> b1_{}, b2_{};
};

} // namespace t8
