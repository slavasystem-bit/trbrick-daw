#pragma once
// Synth filter v2 (v37): zero-delay-feedback (TPT) filters whose cutoff is accurate right up to the
// Nyquist region (the old Chamberlin SVF clamped near 7 kHz), with soft saturation inside the loop
// and true self-oscillation at maximum resonance.
//
//   Pole2  : 12 dB / oct state-variable low-pass (resonance 0..0.99, self-oscillates at the top).
//   Ladder : 24 dB / oct four-pole transistor-style ladder (classic big resonance, loses some bass).
//   Acid   : 18-24 dB ladder voiced for 303 lines: output taken between the third and fourth pole, earlier saturation,
//            thinner low end at high resonance (less make-up gain than the plain ladder).
//
// The class is header-only and allocation free. Process() is called once per audio sample at the
// sample rate it was given in Coeffs() (the synth runs it at 2x the output rate).
#include <algorithm>
#include <cmath>

namespace t8 {

class SynthFilter {
public:
    enum class Mode { Pole2, Ladder, Acid };

    void Reset() { s1_ = s2_ = 0; for (float& v : st_) v = 0; }

    // fc in Hz, res 0..0.99 (higher = more resonance), sr = the rate Process() runs at
    void Coeffs(Mode mode, float fc, float res, float sr) {
        mode_ = mode;
        fc = std::clamp(fc, 10.0f, sr * 0.45f);
        g_ = std::tan(3.14159265358979f * fc / sr);
        res = std::clamp(res, 0.0f, 0.999f);
        if (mode == Mode::Pole2) {
            // damping 1/Q; the old filter used q = 1 - res. Goes slightly negative at the very top so the loop
            // grows (fast enough even for low cutoffs) until the saturation in the state limits it: a sine at the cutoff.
            const float q = res < 0.95f ? 1.0f - res : std::max(0.05f - (res - 0.95f) * 2.0f, -0.05f);
            damp_ = q;
        } else {
            G_ = g_ / (1.0f + g_);
            const float kmax = (mode == Mode::Acid) ? 4.25f : 4.15f;
            k_ = res * res * 0.35f * kmax + res * 0.65f * kmax;   // slightly progressive towards the top
            k_ = std::min(k_, kmax);
            makeup_ = 1.0f + k_ * (mode == Mode::Acid ? 0.22f : 0.42f);
            G2_ = G_ * G_; G4_ = G2_ * G2_;
        }
    }

    float Process(float x) {
        if (mode_ == Mode::Pole2) return Pole2(x);
        return Ladder(x);
    }

private:
    // rational tanh (Pade 3/3 style), exact slope at 0, clamps to +-1 beyond |x|=3 -- far cheaper than std::tanh
    static float FastTanh(float x) { if (x > 3.0f) return 1.0f; if (x < -3.0f) return -1.0f; const float x2 = x * x; return x * (27.0f + x2) / (27.0f + 9.0f * x2); }

    float Pole2(float x) {
        // amplitude-dependent damping (van der Pol style): negligible below ~1, strong above ~2, so a self-oscillation settles at
        // a fixed level whatever the cutoff, and a hot resonant peak is limited softly instead of growing without bound
        const float s1sq = s1_ * s1_;
        const float dEff = damp_ + 0.029f * s1sq * s1sq;
        const float a1 = 1.0f / (1.0f + dEff * g_ + g_ * g_);
        const float hp = (x - (dEff + g_) * s1_ - s2_) * a1;
        const float bp = g_ * hp + s1_;
        s1_ = std::clamp(g_ * hp + bp, -8.0f, 8.0f);
        const float lp = g_ * bp + s2_;
        s2_ = g_ * bp + lp;
        if (!std::isfinite(s1_) || !std::isfinite(s2_)) { s1_ = s2_ = 0; return 0; }
        return lp;
    }

    float Ladder(float x) {
        const float om = 1.0f - G_;
        // the part of y4 that is already known from the stage states (delay-free loop solved in closed form)
        const float S = G_ * G2_ * om * st_[0] + G2_ * om * st_[1] + G_ * om * st_[2] + om * st_[3];
        const float y4 = (G4_ * x + S) / (1.0f + k_ * G4_);
        float u = x - k_ * y4;
        const bool acid = mode_ == Mode::Acid;
        // input saturation: symmetric for the ladder, slightly asymmetric (diode) for acid
        u = acid ? FastTanh(0.7f * u) * (1.0f / 0.7f) : FastTanh(0.5f * u) * 2.0f;   // acid saturates a little earlier
        float in = u, y[4];
        for (int i = 0; i < 4; ++i) {
            const float v = (in - st_[i]) * G_;
            y[i] = v + st_[i];
            st_[i] = y[i] + v;
            in = y[i];
            if (i < 3) in = FastTanh(in * 0.7f) * (1.0f / 0.7f);   // light stage saturation (keeps self-oscillation bounded)
        }
        if (!std::isfinite(st_[3])) { Reset(); return 0; }
        const float out = acid ? 0.55f * y[2] + 0.45f * y[3] : y[3];
        return out * makeup_;
    }

    Mode mode_ = Mode::Pole2;
    float g_ = 0.1f, damp_ = 1.0f, G_ = 0.1f, G2_ = 0.01f, G4_ = 1e-4f, k_ = 0.0f, makeup_ = 1.0f;
    float s1_ = 0, s2_ = 0, st_[4] = {0, 0, 0, 0};
};

// Half-band decimator 2:1 (linear-phase Kaiser FIR). Push() both input samples of a pair, then read Out() once.
// Latency: 11.5 output samples (~0.25 ms at 48 kHz).
class HalfbandDecimator {
public:
    static constexpr int kTaps = 47;   // odd; every second tap (except the centre) is zero, the filter is symmetric
    HalfbandDecimator() { Design(); Reset(); }
    void Reset() { for (float& v : buf_) v = 0; pos_ = 0; }
    void Push(float x) { buf_[pos_] = x; buf_[pos_ + kTaps] = x; pos_ = (pos_ + 1) % kTaps; }   // mirrored copy: contiguous reads
    float Out() const {
        const float* b = buf_ + pos_;                     // b[0] = oldest sample
        float acc = h_[kTaps / 2] * b[kTaps / 2];
        for (int i = 0; i < kTaps / 2; i += 2) acc += h_[i] * (b[i] + b[kTaps - 1 - i]);   // nonzero taps sit on even indices
        return acc;
    }
private:
    static double Bessel0(double x) { double s = 1, t = 1; for (int k = 1; k < 30; ++k) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; } return s; }
    void Design() {
        const double beta = 7.5, mid = (kTaps - 1) / 2.0, pi = 3.14159265358979323846;
        double sum = 0;
        for (int i = 0; i < kTaps; ++i) {
            const double n = i - mid;
            const double sinc = n == 0 ? 0.5 : std::sin(pi * n * 0.5) / (pi * n);   // cutoff fs/4
            const double r = n / mid, w = Bessel0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / Bessel0(beta);
            h_[i] = static_cast<float>(sinc * w); sum += h_[i];
        }
        for (float& v : h_) v = static_cast<float>(v / sum);   // unity gain at DC
    }
    float h_[kTaps]{}, buf_[2 * kTaps]{};
    int pos_ = 0;
};

} // namespace t8
