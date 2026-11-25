#pragma once
// Live ITU-R BS.1770-4 / EBU R128 loudness meter for the master output: momentary (400 ms),
// short-term (3 s), integrated (gated, since the last reset) and a true-peak reading.
// Runs on the audio thread; the results are atomics for the UI. The integrated value uses a
// histogram of 400 ms blocks (75 % overlap) so memory stays fixed however long the set runs.
#include <array>
#include <atomic>
#include <cmath>
#include <algorithm>
#include "TruePeak.h"

namespace t8 {

class LoudnessMeter {
public:
    void RequestReset() { resetRequested_.store(true, std::memory_order_release); }
    float Momentary() const { return momentary_.load(std::memory_order_relaxed); }       // LUFS
    float ShortTerm() const { return shortTerm_.load(std::memory_order_relaxed); }       // LUFS
    float Integrated() const { return integrated_.load(std::memory_order_relaxed); }     // LUFS, -120 = none yet
    float TruePeakDb() const { return truePeak_.load(std::memory_order_relaxed); }        // dBTP since reset

    // interleaved stereo, n frames
    void Process(const float* buf, int n, int sampleRate) {
        if (sampleRate != rate_ || resetRequested_.exchange(false, std::memory_order_acq_rel)) Reset(sampleRate);
        for (int i = 0; i < n; ++i) {
            double e = 0.0;
            for (int c = 0; c < 2; ++c) {
                const double x = buf[2 * i + c];
                const double y = hp_[c].Run(shelf_[c].Run(x));
                e += y * y;
                TruePeak(c, buf[2 * i + c]);
            }
            subSum_ += e;
            if (++subN_ >= subLen_) EndSubBlock();
        }
    }

private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double Run(double x) { double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
    };
    static constexpr int kBins = 801;                 // -70 .. +10 LUFS in 0.1 steps
    static constexpr double kPi = 3.14159265358979323846;

    static double Lufs(double meanSquare) { return -0.691 + 10.0 * std::log10(std::max(meanSquare, 1e-12)); }

    void Reset(int sr) {
        rate_ = sr;
        const double G = 3.999843853973347, f0 = 1681.974450955533, Q = 0.7071752369554196;
        double K = std::tan(kPi * f0 / sr), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416), a0 = 1.0 + K / Q + K * K;
        for (auto& s : shelf_) {
            s = Biquad{};
            s.b0 = (Vh + Vb * K / Q + K * K) / a0; s.b1 = 2.0 * (K * K - Vh) / a0; s.b2 = (Vh - Vb * K / Q + K * K) / a0;
            s.a1 = 2.0 * (K * K - 1.0) / a0; s.a2 = (1.0 - K / Q + K * K) / a0;
        }
        const double g0 = 38.13547087602444, q0 = 0.5003270373238773;
        K = std::tan(kPi * g0 / sr); a0 = 1.0 + K / q0 + K * K;
        for (auto& h : hp_) {
            h = Biquad{};
            h.b0 = 1.0 / a0; h.b1 = -2.0 / a0; h.b2 = 1.0 / a0; h.a1 = 2.0 * (K * K - 1.0) / a0; h.a2 = (1.0 - K / q0 + K * K) / a0;
        }
        subLen_ = std::max(1, sr / 10);
        subSum_ = 0; subN_ = 0; ring_.fill(0.0); ringPos_ = 0; ringFilled_ = 0;
        hist_.fill(0); histEnergy_.fill(0.0); sinceInt_ = 0;
        momentary_.store(-120.0f); shortTerm_.store(-120.0f); integrated_.store(-120.0f); truePeak_.store(-120.0f);
        peakLin_ = 0.0f; for (auto& h : tpHist_) h.fill(0.0f);
    }

    void EndSubBlock() {
        ring_[ringPos_] = subSum_ / subN_;   // mean square of this 100 ms (sum of both channels)
        ringPos_ = (ringPos_ + 1) % kRing; ringFilled_ = std::min(kRing, ringFilled_ + 1);
        subSum_ = 0; subN_ = 0;
        auto meanLast = [&](int count) { double s = 0; for (int k = 0; k < count; ++k) s += ring_[(ringPos_ - 1 - k + kRing * 2) % kRing]; return s / count; };
        if (ringFilled_ >= 4) {
            const double m = meanLast(4);
            momentary_.store(static_cast<float>(Lufs(m)), std::memory_order_relaxed);
            const double l = Lufs(m);
            if (l > -70.0) {
                const int b = std::clamp(static_cast<int>((l + 70.0) * 10.0), 0, kBins - 1);
                ++hist_[b]; histEnergy_[b] += m;
            }
        }
        if (ringFilled_ >= 30) shortTerm_.store(static_cast<float>(Lufs(meanLast(30))), std::memory_order_relaxed);
        else if (ringFilled_ >= 4) shortTerm_.store(static_cast<float>(Lufs(meanLast(ringFilled_))), std::memory_order_relaxed);
        if (++sinceInt_ >= 5) { sinceInt_ = 0; UpdateIntegrated(); }
    }

    void UpdateIntegrated() {
        double sum = 0; long cnt = 0;
        for (int b = 0; b < kBins; ++b) { sum += histEnergy_[b]; cnt += hist_[b]; }
        if (!cnt) { integrated_.store(-120.0f, std::memory_order_relaxed); return; }
        const double rel = Lufs(sum / cnt) - 10.0;
        double sum2 = 0; long cnt2 = 0;
        for (int b = 0; b < kBins; ++b) {
            const double binLufs = -70.0 + b * 0.1;
            if (binLufs + 0.1 > rel) { sum2 += histEnergy_[b]; cnt2 += hist_[b]; }
        }
        integrated_.store(cnt2 ? static_cast<float>(Lufs(sum2 / cnt2)) : -120.0f, std::memory_order_relaxed);
    }

    // true peak: 8 samples of history; the interval [j, j+1] is evaluated when sample j+4 arrives (3 samples late)
    void TruePeak(int c, float x) {
        auto& h = tpHist_[c];
        for (int i = 0; i < 7; ++i) h[i] = h[i + 1];
        h[7] = x;
        const float pk = std::max({std::fabs(x), TruePeakKernel::MaxInterpolated(h.data())});
        if (pk > peakLin_) { peakLin_ = pk; truePeak_.store(20.0f * std::log10(std::max(pk, 1e-6f)), std::memory_order_relaxed); }
    }

    static constexpr int kRing = 32;
    std::array<Biquad, 2> shelf_, hp_;
    std::array<double, kRing> ring_{};
    std::array<long, kBins> hist_{};
    std::array<double, kBins> histEnergy_{};
    std::array<std::array<float, 8>, 2> tpHist_{};
    double subSum_ = 0; int subN_ = 0, subLen_ = 4410, ringPos_ = 0, ringFilled_ = 0, sinceInt_ = 0, rate_ = 0;
    float peakLin_ = 0.0f;
    std::atomic<bool> resetRequested_{false};
    std::atomic<float> momentary_{-120.0f}, shortTerm_{-120.0f}, integrated_{-120.0f}, truePeak_{-120.0f};
};

} // namespace t8
