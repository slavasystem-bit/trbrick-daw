#pragma once
// Final output limiter: look-ahead, true-peak aware, always in the signal path (its latency is a
// constant ~1.5 ms whether it is limiting or not, so switching it never moves the audio in time).
//
// How it works, per sample n (all indices in input samples):
//   1. peak[j]   the largest |sample| of both channels AND of the 3 interpolated points between
//                sample j and j+1 (8-tap windowed sinc = a 4x oversampled true-peak estimate, see TruePeak.h)
//   2. need[j]   min(1, ceiling / peak[j])
//   3. m[j]      the minimum of need over the last L samples (so the gain is already down when a peak arrives)
//   4. e[j]      m[j] with a ~30 ms hold, then an exponential release back to 1 (instant attack because of 3.)
//   5. g[j]      moving average of e over L samples: a smooth ramp whose value at sample k never exceeds need[k]
//   output k = audio[k] * g, delayed by L+3 samples. Nothing here can overshoot the ceiling except the
//   interpolation error of the true-peak estimate (a few thousandths of a dB); a hard clamp stays as the last resort.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>
#include "TruePeak.h"

namespace t8 {

class Limiter {
public:
    static constexpr int kMaxLook = 256;     // ring size (power of two), enough for 1.5 ms up to 150 kHz

    void Reset() {
        for (auto& v : audio_) v.fill(0.0f);
        need_.fill(1.0f); env_.fill(1.0f);
        n_ = 0; avgSum_ = static_cast<double>(look_); e_ = 1.0f; grDb_.store(0.0f); lastCfgRate_ = 0;
    }

    int Latency() const { return look_ + TruePeakKernel::kFuture - 1; }
    float GainReductionDb() const { return grDb_.load(std::memory_order_relaxed); }   // >= 0, fast up / slow down

    // `enabled` false = pass-through (still delayed by Latency()), hard clamp at +-1 only.
    void Process(float* buf, int n, int sampleRate, float ceilingDb, bool enabled, float releaseMs = 120.0f) {
        if (sampleRate != lastCfgRate_) Configure(sampleRate);
        const float ceil = enabled ? std::pow(10.0f, std::clamp(ceilingDb, -12.0f, 0.0f) / 20.0f) : 1.0f;
        const float rel = std::exp(-1.0f / (std::max(10.0f, releaseMs) * 0.001f * static_cast<float>(sampleRate)));
        float maxGr = 0.0f;
        for (int i = 0; i < n; ++i) {
            const float l = buf[2 * i], r = buf[2 * i + 1];
            audio_[0][n_ & kMask] = l; audio_[1][n_ & kMask] = r;
            // true-peak need for the interval [jj, jj+1] (needs samples jj-3 .. jj+4 = n)
            const long jj = n_ - TruePeakKernel::kFuture;
            need_[n_ & kMask] = 1.0f;
            if (jj >= 0) {
                float pk = 0.0f;
                for (int c = 0; c < 2; ++c) {
                    float x[TruePeakKernel::kTaps];
                    for (int t = 0; t < TruePeakKernel::kTaps; ++t) x[t] = At(c, jj - TruePeakKernel::kPast + t);
                    pk = std::max({pk, std::fabs(At(c, jj)), std::fabs(At(c, jj + 1)), TruePeakKernel::MaxInterpolated(x)});
                }
                const float nd = pk > ceil ? ceil / pk : 1.0f;
                need_[(jj + 1) & kMask] = nd;                                     // the interval belongs to both its end samples
                need_[jj & kMask] = std::min(need_[jj & kMask], nd);
            }
            // index of the newest finished need value
            const long j = std::max<long>(0, jj);
            // minimum over the last look_ samples
            float m = 1.0f;
            for (int k = 0; k < look_; ++k) m = std::min(m, need_[(j - k) & kMask]);
            // instant attack, exponential release
            // a peak that needs (almost) as much reduction as is already applied re-arms the hold, so a steady
            // bass tone keeps one constant gain instead of breathing between its peaks
            if (m < e_) e_ = m;
            if (m <= e_ * 1.003f && m < 1.0f) hold_ = holdN_;
            else if (hold_ > 0) --hold_;
            else e_ = std::min(m, 1.0f - (1.0f - e_) * rel);
            env_[j & kMask] = e_;
            // moving average of e over look_ samples (running sum)
            avgSum_ += e_ - static_cast<double>(env_[(j - look_) & kMask]);
            const float g = static_cast<float>(avgSum_ / look_);
            // audio sample being output: j - (look_ - 1)
            const long k = j - (look_ - 1);
            float ol = k >= 0 ? At(0, k) * g : 0.0f, orr = k >= 0 ? At(1, k) * g : 0.0f;
            ol = std::clamp(ol, -1.0f, 1.0f); orr = std::clamp(orr, -1.0f, 1.0f);
            buf[2 * i] = ol; buf[2 * i + 1] = orr;
            maxGr = std::max(maxGr, g < 1.0f ? -20.0f * std::log10(std::max(g, 1e-4f)) : 0.0f);
            ++n_;
        }
        const float cur = grDb_.load();
        grDb_.store(maxGr > cur ? maxGr : cur * 0.9f + maxGr * 0.1f);
    }

private:
    static constexpr int kMask = kMaxLook - 1;
    float At(int c, long idx) const { return idx < 0 ? 0.0f : audio_[c][idx & kMask]; }
    void Configure(int sampleRate) {
        lastCfgRate_ = sampleRate;
        holdN_ = static_cast<int>(0.03 * sampleRate);
        look_ = std::clamp(static_cast<int>(std::lround(0.0015 * sampleRate)), 8, kMaxLook / 2 - 4);
        need_.fill(1.0f); env_.fill(1.0f); avgSum_ = static_cast<double>(look_); e_ = 1.0f; hold_ = 0;
    }
    std::array<std::array<float, kMaxLook>, 2> audio_{};
    std::array<float, kMaxLook> need_{}, env_{};
    long n_ = 0;
    int look_ = 66, lastCfgRate_ = 0;
    double avgSum_ = 66.0;
    float e_ = 1.0f;
    int hold_ = 0, holdN_ = 1323;
    std::atomic<float> grDb_{0.0f};
};

} // namespace t8
