#include "Wavetables.h"
#include <algorithm>
#include <cmath>

namespace t8 {

namespace {
constexpr double kWtPi = 3.14159265358979323846;

// Same naive (non-band-limited) waveform formulas SubtractiveSynth's own
// Oscillator() already uses -- generating these at wavetable-authoring
// time with the same formulas doesn't introduce any aliasing behavior
// beyond what this project's engines already accept elsewhere.
float NaiveSine(double ph) { return static_cast<float>(std::sin(2.0 * kWtPi * ph)); }
float NaiveTriangle(double ph) {
    double t = ph < 0.5 ? ph * 2.0 : (1.0 - ph) * 2.0;
    return static_cast<float>(t * 2.0 - 1.0);
}
float NaiveSaw(double ph) { return static_cast<float>(2.0 * ph - 1.0); }
float NaiveSquare(double ph) { return ph < 0.5 ? 1.0f : -1.0f; }
float NaivePulse(double ph, double duty) { return ph < duty ? 1.0f : -1.0f; }

void NormalizeFrame(std::array<float, kWtFrameSize>& frame) {
    float peak = 0.0f;
    for (float v : frame) peak = std::max(peak, std::fabs(v));
    if (peak < 1e-6f) return;
    float scale = 0.95f / peak;
    for (float& v : frame) v *= scale;
}

// ---- Bank 0: CLASSIC -- morphs sine -> triangle -> saw -> square across
// the 8 frames, each frame a simple sample-by-sample crossfade of two
// adjacent naive shapes.
WavetableBank MakeClassicBank() {
    WavetableBank bank{};
    for (int f = 0; f < kWtFrames; ++f) {
        double t = static_cast<double>(f) / (kWtFrames - 1); // 0..1
        for (int i = 0; i < kWtFrameSize; ++i) {
            double ph = static_cast<double>(i) / kWtFrameSize;
            float sine = NaiveSine(ph), tri = NaiveTriangle(ph), saw = NaiveSaw(ph), sq = NaiveSquare(ph);
            float v;
            if (t < 1.0 / 3.0) {
                float mix = static_cast<float>(t / (1.0 / 3.0));
                v = sine + (tri - sine) * mix;
            } else if (t < 2.0 / 3.0) {
                float mix = static_cast<float>((t - 1.0 / 3.0) / (1.0 / 3.0));
                v = tri + (saw - tri) * mix;
            } else {
                float mix = static_cast<float>((t - 2.0 / 3.0) / (1.0 / 3.0));
                v = saw + (sq - saw) * mix;
            }
            bank[f][i] = v;
        }
    }
    return bank;
}

// ---- Bank 1: HARMONIC SWEEP -- additive resynthesis: frame 0 is a pure
// sine (1 harmonic), frame 7 sums up to 24 harmonics at 1/n amplitude
// (sawtooth-like) -- a "brightness" sweep from a pure tone to a buzzy,
// harmonically rich one, the classic wavetable "add harmonics" scan.
WavetableBank MakeHarmonicSweepBank() {
    WavetableBank bank{};
    for (int f = 0; f < kWtFrames; ++f) {
        double t = static_cast<double>(f) / (kWtFrames - 1);
        int maxHarmonic = 1 + static_cast<int>(std::round(t * 23.0)); // 1..24
        for (int i = 0; i < kWtFrameSize; ++i) {
            double ph = static_cast<double>(i) / kWtFrameSize;
            double sum = 0.0;
            for (int h = 1; h <= maxHarmonic; ++h) {
                sum += std::sin(2.0 * kWtPi * ph * h) / h;
            }
            bank[f][i] = static_cast<float>(sum);
        }
        NormalizeFrame(bank[f]);
    }
    return bank;
}

// ---- Bank 2: FORMANT -- a fixed, buzzy harmonic source shaped by two
// Gaussian "formant" bumps in harmonic-number space, whose center
// harmonics shift across the 8 frames -- a simplified, honest
// APPROXIMATION of vowel-ish formant movement (real formant synthesis
// needs far more precise frequency-domain filtering than a fixed 8-frame
// wavetable can offer; this is meant to evoke the idea for a small
// live-performance instrument, not to be a vocal-synthesis engine).
WavetableBank MakeFormantBank() {
    WavetableBank bank{};
    constexpr int kNumHarmonics = 40;
    for (int f = 0; f < kWtFrames; ++f) {
        double t = static_cast<double>(f) / (kWtFrames - 1);
        // Two formant centers sweeping from a low, wide-spaced ("ah"-ish)
        // placement toward one low + one very high ("ee"-ish) placement.
        double f1 = 3.0 + t * 2.0;
        double f2 = 8.0 - t * 3.0 + t * t * 20.0;
        for (int i = 0; i < kWtFrameSize; ++i) {
            double ph = static_cast<double>(i) / kWtFrameSize;
            double sum = 0.0;
            for (int h = 1; h <= kNumHarmonics; ++h) {
                double base = 1.0 / h; // buzzy source
                double bump1 = std::exp(-0.5 * std::pow((h - f1) / 1.5, 2.0));
                double bump2 = std::exp(-0.5 * std::pow((h - f2) / 2.5, 2.0));
                double weight = 0.15 + bump1 + 0.7 * bump2;
                sum += base * weight * std::sin(2.0 * kWtPi * ph * h);
            }
            bank[f][i] = static_cast<float>(sum);
        }
        NormalizeFrame(bank[f]);
    }
    return bank;
}

// ---- Bank 3: PULSE WIDTH -- classic PWM wavetable: duty cycle sweeps
// from a plain 50% square (frame 0) down to a thin, nasal 5% pulse
// (frame 7) -- the traditional "sweep the pulse width" synth voice,
// captured as fixed frames instead of computed live.
WavetableBank MakePulseWidthBank() {
    WavetableBank bank{};
    for (int f = 0; f < kWtFrames; ++f) {
        double t = static_cast<double>(f) / (kWtFrames - 1);
        double duty = 0.5 - t * 0.45; // 0.5 .. 0.05
        for (int i = 0; i < kWtFrameSize; ++i) {
            double ph = static_cast<double>(i) / kWtFrameSize;
            bank[f][i] = NaivePulse(ph, duty);
        }
    }
    return bank;
}

} // namespace

const char* kWavetableBankNames[kNumWavetableBanks] = {
    "CLASSIC", "HARMONIC SWEEP", "FORMANT", "PULSE WIDTH"
};

const std::array<WavetableBank, kNumWavetableBanks>& AllWavetables() {
    static const std::array<WavetableBank, kNumWavetableBanks> tables = {{
        MakeClassicBank(), MakeHarmonicSweepBank(), MakeFormantBank(), MakePulseWidthBank()
    }};
    return tables;
}

} // namespace t8
