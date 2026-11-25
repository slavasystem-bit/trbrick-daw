#pragma once
// Band-limited sample interpolation (Kaiser-windowed sinc, phase table) used by the drum engine for tune and by
// the WAV loader for sample-rate conversion. `cutoff` is the passband edge as a fraction of the SOURCE Nyquist:
// 1.0 for plain interpolation / pitching down, 1/rate when pitching up so nothing above the new Nyquist folds back.
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace t8 {

class SincTable {
public:
    // taps: even, kernel span in source samples; phases: sub-sample resolution (linear blend between neighbours)
    SincTable(int taps, int phases, double cutoff, double beta = 8.0)
        : taps_(taps), phases_(phases), cutoff_(std::clamp(cutoff, 0.05, 1.0)) {
        k_.assign(static_cast<size_t>(phases + 1) * taps, 0.0f);
        const double pi = 3.14159265358979323846, half = taps * 0.5;
        auto i0 = [](double x) { double s = 1.0, t = 1.0; for (int n = 1; n < 40; ++n) { t *= (x / (2.0 * n)) * (x / (2.0 * n)); s += t; } return s; };
        const double norm = i0(beta);
        for (int p = 0; p <= phases; ++p) {
            const double frac = static_cast<double>(p) / phases;
            double sum = 0.0;
            float* row = &k_[static_cast<size_t>(p) * taps];
            for (int j = 0; j < taps; ++j) {
                const double x = (j - (taps / 2 - 1)) - frac;          // distance from the interpolation point, in source samples
                const double a = pi * cutoff_ * x;
                const double s = std::fabs(a) < 1e-9 ? cutoff_ : cutoff_ * std::sin(a) / a;
                const double r = x / half;
                const double w = std::fabs(r) >= 1.0 ? 0.0 : i0(beta * std::sqrt(1.0 - r * r)) / norm;
                row[j] = static_cast<float>(s * w); sum += s * w;
            }
            for (int j = 0; j < taps; ++j) row[j] = static_cast<float>(row[j] / sum);   // unity gain at DC for every phase
        }
    }
    int Taps() const { return taps_; }
    int Lead() const { return taps_ / 2 - 1; }       // taps before the base sample

    // value at base + frac, from `data` (frames of `stride` floats, channel offset `ch`); samples outside [0, frames) read as 0
    float At(const float* data, int frames, int stride, int ch, int base, double frac) const {
        const double fp = frac * phases_;
        int p = static_cast<int>(fp); if (p >= phases_) p = phases_ - 1;
        const float a = static_cast<float>(fp - p);
        const float* r0 = &k_[static_cast<size_t>(p) * taps_];
        const float* r1 = r0 + taps_;
        const int first = base - Lead();
        float s0 = 0.0f, s1 = 0.0f;
        if (first >= 0 && first + taps_ <= frames) {
            const float* x = data + static_cast<size_t>(first) * stride + ch;
            for (int j = 0; j < taps_; ++j) { const float v = x[static_cast<size_t>(j) * stride]; s0 += r0[j] * v; s1 += r1[j] * v; }
        } else {
            for (int j = 0; j < taps_; ++j) {
                const int idx = first + j;
                const float v = (idx >= 0 && idx < frames) ? data[static_cast<size_t>(idx) * stride + ch] : 0.0f;
                s0 += r0[j] * v; s1 += r1[j] * v;
            }
        }
        return s0 + (s1 - s0) * a;
    }

private:
    int taps_, phases_;
    double cutoff_;
    std::vector<float> k_;
};

// Offline sample-rate conversion of an interleaved buffer (kit loading). Returns the new frame count.
inline std::vector<float> ResampleInterleaved(const std::vector<float>& src, int channels, int srcRate, int dstRate) {
    if (srcRate == dstRate || srcRate <= 0 || dstRate <= 0 || channels < 1 || src.empty()) return src;
    const int srcFrames = static_cast<int>(src.size()) / channels;
    const double ratio = static_cast<double>(srcRate) / dstRate;                 // source samples per output sample
    const int dstFrames = std::max(1, static_cast<int>(std::floor(srcFrames / ratio)));
    SincTable table(48, 256, std::min(1.0, 1.0 / ratio) * 0.97);
    std::vector<float> out(static_cast<size_t>(dstFrames) * channels);
    for (int i = 0; i < dstFrames; ++i) {
        const double pos = i * ratio;
        const int base = static_cast<int>(pos);
        const double frac = pos - base;
        for (int c = 0; c < channels; ++c) out[static_cast<size_t>(i) * channels + c] = table.At(src.data(), srcFrames, channels, c, base, frac);
    }
    return out;
}

} // namespace t8
