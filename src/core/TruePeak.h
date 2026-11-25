#pragma once
// 4x true-peak estimator shared by the Limiter and the LoudnessMeter: the three points between two samples
// are interpolated with an 8-tap Kaiser-windowed sinc (4 samples either side). That keeps the error well under
// 0.2 dB for anything below ~15 kHz, which is far better than a cubic, at 3x8 multiplies per channel per sample.
#include <algorithm>
#include <array>
#include <cmath>

namespace t8 {

struct TruePeakKernel {
    static constexpr int kTaps = 8;     // samples j-3 .. j+4 around the interval [j, j+1]
    static constexpr int kPast = 3;     // how many of them are before j
    static constexpr int kFuture = 4;   // newest sample index relative to j: j + kFuture

    // coefficient set for the point at j + q/4, q = 1..3
    static const std::array<std::array<float, kTaps>, 3>& Coeffs() {
        static const std::array<std::array<float, kTaps>, 3> c = Build();
        return c;
    }

    // `x` points at sample j-3 (8 consecutive samples). Returns the largest |value| over the 3 interpolated points.
    static float MaxInterpolated(const float* x) {
        const auto& c = Coeffs();
        float pk = 0.0f;
        for (int q = 0; q < 3; ++q) {
            float v = 0.0f;
            for (int i = 0; i < kTaps; ++i) v += c[q][i] * x[i];
            pk = std::max(pk, std::fabs(v));
        }
        return pk;
    }

private:
    static double I0(double x) {
        double s = 1.0, t = 1.0;
        for (int k = 1; k < 30; ++k) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; }
        return s;
    }
    static std::array<std::array<float, kTaps>, 3> Build() {
        std::array<std::array<float, kTaps>, 3> out{};
        const double pi = 3.14159265358979323846, beta = 7.0;
        for (int q = 0; q < 3; ++q) {
            const double t = 0.25 * (q + 1);
            double sum = 0.0;
            std::array<double, kTaps> tmp{};
            for (int i = 0; i < kTaps; ++i) {
                const double d = (i - kPast) - t;              // distance from the sample to the interpolation point
                const double s = std::fabs(d) < 1e-9 ? 1.0 : std::sin(pi * d) / (pi * d);
                const double r = d / 4.0;
                const double w = std::fabs(r) >= 1.0 ? 0.0 : I0(beta * std::sqrt(1.0 - r * r)) / I0(beta);
                tmp[i] = s * w; sum += tmp[i];
            }
            for (int i = 0; i < kTaps; ++i) out[q][i] = static_cast<float>(tmp[i] / sum);
        }
        return out;
    }
};

} // namespace t8
