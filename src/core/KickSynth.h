#pragma once
// Built-in kick drum: sine body with an exponential pitch sweep, a short noise click and optional saturation.
// One instance of KickSynthState is one playing kick (it lives inside a DrumVoice).
#include "Types.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace t8 {

// Fundamental in Hz for a key root (0 = C .. 11 = B): the octave of the root note that lands in 40..80 Hz.
inline float KickKeyFrequency(int keyRoot) {
    const int r = ((keyRoot % 12) + 12) % 12;
    float f = 32.7031957f * std::pow(2.0f, static_cast<float>(r) / 12.0f);   // C1 upwards
    while (f < 40.0f) f *= 2.0f;
    while (f >= 80.0f) f *= 0.5f;
    return f;
}

struct KickSynthState {
    bool  active = false;
    double phase = 0.0;
    double t = 0.0;            // seconds since the hit
    float f0 = 52.0f, sweep = 4.0f, pitchTau = 0.01f, ampTau = 0.05f, decaySec = 0.38f;
    float click = 0.3f, drive = 0.25f, driveNorm = 1.0f, driveGain = 1.0f;
    float noiseHp = 0.0f;
    uint32_t seed = 22222u;

    // semitones: the track's TUNE knob on top of the chosen fundamental
    void Start(const KickSynthSettings& k, int keyRoot, float semitones, uint32_t noiseSeed) {
        active = true; phase = 0.0; t = 0.0; noiseHp = 0.0f; seed = noiseSeed | 1u;
        float base = (k.keyLock && keyRoot >= 0) ? KickKeyFrequency(keyRoot) : std::clamp(k.pitchHz, 30.0f, 100.0f);
        f0 = base * std::pow(2.0f, semitones / 12.0f);
        sweep = std::clamp(k.sweep, 1.0f, 10.0f);
        pitchTau = std::clamp(k.sweepMs, 3.0f, 200.0f) * 0.001f / 2.0f;     // sweepMs ~ where the pitch has covered 86 %
        decaySec = std::clamp(k.decayMs, 60.0f, 1500.0f) * 0.001f;
        ampTau = decaySec / 6.9f;                                           // -60 dB at decayMs
        click = std::clamp(k.click, 0.0f, 1.0f);
        drive = std::clamp(k.drive, 0.0f, 1.0f);
        driveGain = 1.0f + drive * 4.0f;
        driveNorm = 1.0f / std::tanh(driveGain);
    }

    // next mono sample at 1/sr seconds per step; sets active=false once inaudible
    float Next(double dt) {
        const double fNow = f0 * (1.0 + (sweep - 1.0) * std::exp(-t / pitchTau));
        phase += fNow * dt; phase -= std::floor(phase);
        const float attack = t < 0.0004 ? static_cast<float>(t / 0.0004) : 1.0f;
        const float amp = std::exp(static_cast<float>(-t) / ampTau) * attack;
        float body = std::sin(6.28318530718f * static_cast<float>(phase));
        if (drive > 0.001f) body = std::tanh(body * driveGain) * driveNorm;
        float y = body * amp;
        if (click > 0.0f && t < 0.012) {
            seed = seed * 1664525u + 1013904223u;
            const float n = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
            noiseHp = 0.6f * noiseHp + 0.4f * n;                            // crude high-pass: n - lowpass(n)
            const float hp = n - noiseHp;
            y += hp * click * 0.5f * std::exp(static_cast<float>(-t) / 0.0018f);
        }
        t += dt;
        if (t > decaySec * 1.6 || (t > 0.02 && amp < 0.0008f)) active = false;
        return y;
    }
};

} // namespace t8
