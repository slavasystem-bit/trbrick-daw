#pragma once
// Small shared DSP building blocks for the master bus: a 2x oversampled waveshaper
// (so saturation does not fold harmonics back as aliasing) and a Linkwitz-Riley
// crossover used by the mono-bass stage. Audio-thread only, no allocation in Process().
#include <array>
#include <cmath>
#include <algorithm>
#include <vector>

namespace t8 {

// 63-tap windowed-sinc halfband FIR, used for both 2x up- and down-sampling.
// Group delay: 31 samples at the 2x rate per filter = 31 samples at the base rate overall.
class Oversampler2x {
public:
    static constexpr int kTaps = 63;
    static constexpr int kLatency = 31;   // base-rate samples from input to output

    Oversampler2x() {
        const double pi = 3.14159265358979323846;
        const int mid = kTaps / 2;
        const double beta = 8.0;
        auto i0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 40; ++k) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; } return s; };
        const double den = i0(beta);
        for (int n = 0; n < kTaps; ++n) {
            const double k = n - mid;
            const double sinc = k == 0 ? 0.5 : std::sin(pi * k * 0.5) / (pi * k);          // cutoff = fs2 / 4
            const double r = 2.0 * n / (kTaps - 1) - 1.0;
            const double win = i0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / den;
            h_[n] = static_cast<float>(2.0 * sinc * win);                                    // x2 restores the zero-stuffing loss
        }
        Reset();
    }
    void Reset() { upHist_.fill(0.0f); downHist_.fill(0.0f); upPos_ = downPos_ = 0; }

    // Process one base-rate sample through `shape` (called twice, at the doubled rate).
    template <class F>
    float Process(float x, F&& shape) {
        upHist_[upPos_] = x; 
        // the two polyphase outputs of the zero-stuffed interpolation filter
        float e0 = 0.0f, e1 = 0.0f;
        for (int k = 0; k < kTaps; k += 2) e0 += h_[k] * upHist_[(upPos_ - k / 2 + kHist) % kHist];
        for (int k = 1; k < kTaps; k += 2) e1 += h_[k] * upHist_[(upPos_ - (k - 1) / 2 + kHist) % kHist];
        upPos_ = (upPos_ + 1) % kHist;
        const float s0 = shape(e0), s1 = shape(e1);
        downHist_[downPos_] = s0; downPos_ = (downPos_ + 1) % kDown;
        // decimating FIR at the doubled rate, evaluated once per base sample on the newest even sample
        // (h_ is symmetric and has gain 2, hence * 0.5); this phase gives an exact 31-sample delay overall
        float y = 0.0f;
        for (int k = 0; k < kTaps; ++k) y += h_[k] * downHist_[(downPos_ - 1 - k + kDown * 2) % kDown];
        downHist_[downPos_] = s1; downPos_ = (downPos_ + 1) % kDown;
        return y * 0.5f;
    }
private:
    static constexpr int kHist = 32;
    static constexpr int kDown = 64;
    std::array<float, kTaps> h_{};
    std::array<float, kHist> upHist_{};
    std::array<float, kDown> downHist_{};
    int upPos_ = 0, downPos_ = 0;
};

// tanh drive with 2x oversampling, delay-compensated so switching it on or off never moves the signal in time.
class Saturator {
public:
    static constexpr int kLatency = Oversampler2x::kLatency;
    void Reset() { for (auto& o : os_) o.Reset(); for (auto& d : dly_) d.fill(0.0f); pos_ = 0; wet_ = 0.0f; }
    // interleaved stereo; gain >= 1 is the drive into tanh, amount 0..1 the wet share
    void Process(float* buf, int n, float gain, float amount) {
        const float target = std::clamp(amount, 0.0f, 1.0f);
        const float step = 1.0f / 2048.0f;   // short ramp when the amount changes
        for (int i = 0; i < n; ++i) {
            wet_ += std::clamp(target - wet_, -step, step);
            for (int c = 0; c < 2; ++c) {
                const float x = buf[2 * i + c];
                dly_[c][pos_] = x;
                const float d = dly_[c][(pos_ + kDly - kLatency) % kDly];
                const float s = os_[c].Process(x, [gain](float v) { return std::tanh(v * gain); });
                buf[2 * i + c] = d + wet_ * (s - d);
            }
            pos_ = (pos_ + 1) % kDly;
        }
    }
    bool Idle(float amount) const { return amount <= 0.0f && wet_ <= 0.0f; }
private:
    static constexpr int kDly = 64;
    std::array<Oversampler2x, 2> os_;
    std::array<std::array<float, kDly>, 2> dly_{};
    int pos_ = 0;
    float wet_ = 0.0f;
};

} // namespace t8
