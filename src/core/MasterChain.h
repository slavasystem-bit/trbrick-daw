#pragma once
// Master bus processing for a "finished" sound without a separate mastering step.
//
//   Process()  HPF -> low / mid / high tone -> saturation (chain on)  ->  compressor with a detector
//              high-pass (comp on)  ->  stereo width (chain on)  ->  mono bass (everything below
//              monoBassHz summed to the centre, a Linkwitz-Riley crossover so the sum stays flat)
//
// Master volume, the optional master DRIVE, the look-ahead true-peak limiter and the loudness
// meter come after this, in AudioEngine::Render (see Dsp.h, Limiter.h, LoudnessMeter.h).
// Each stage is a no-op when its switch is off. Audio-thread only; the gain-reduction meter is an
// atomic read by the UI.
#include "Types.h"
#include <algorithm>
#include <atomic>
#include <cmath>

namespace t8 {

class MasterChain {
public:
    void Reset() {
        for (auto& b : hpf_) b = Bq{};
        for (auto& b : eq_) b = Bq{};
        for (auto& b : xLp_) b = Bq{};
        for (auto& b : xHp_) b = Bq{};
        for (auto& b : scHp_) b = Bq{};
        env_ = 0.0f; hold_ = 0; grDb_.store(0.0f); lastXover_ = -1; lastScHpf_ = -1;
    }

    // `buf` is interleaved stereo, n frames.
    void Process(float* buf, int n, const MasterChainSettings& m, int sr) {
        const float fs = static_cast<float>(std::max(8000, sr));
        if (m.chainOn) {
            UpdateFilters(m, fs);
            const bool useHpf = m.hpfHz > 21.0f;
            const bool useEq = std::fabs(m.lowDb) > 0.05f || std::fabs(m.midDb) > 0.05f || std::fabs(m.highDb) > 0.05f;
            if (useHpf || useEq) {
                for (int i = 0; i < n; ++i) {
                    for (int c = 0; c < 2; ++c) {
                        float x = buf[2 * i + c];
                        if (useHpf) x = hpf_[c].Run(x);
                        if (useEq) { x = eq_[0 * 2 + c].Run(x); x = eq_[1 * 2 + c].Run(x); x = eq_[2 * 2 + c].Run(x); }
                        buf[2 * i + c] = x;
                    }
                }
            }
            if (m.saturation > 0.005f) {
                // v44: a knee clipper instead of tanh(4x) on everything. The curve is EXACTLY linear up to
                // the knee and rounds the peaks above it (derivative continuous), so quiet material and
                // the body of the mix pass untouched and only the peaks get the colour. The old curve
                // bent every level (-12 dB IMD on a loud kick+hat mix, audible as "distortion").
                // The negative half has a slightly lower knee = a little 2nd harmonic (warmth).
                const float kneeP = 1.0f - 0.85f * std::clamp(m.saturation, 0.0f, 1.0f);
                const float kneeN = kneeP * (1.0f - 0.12f * m.saturation);
                for (int i = 0; i < 2 * n; ++i) buf[i] = KneeClip(buf[i], kneeP, kneeN);
            }
        }
        if (m.compOn) RunCompressor(buf, n, m, fs);
        else { grDb_.store(grDb_.load() * 0.9f); env_ = 0.0f; }
        if (m.chainOn && std::fabs(m.width - 1.0f) > 0.01f) {
            const float w = std::clamp(m.width, 0.0f, 2.0f);
            for (int i = 0; i < n; ++i) {
                float l = buf[2 * i], r = buf[2 * i + 1];
                float mid = 0.5f * (l + r), side = 0.5f * (l - r) * w;
                buf[2 * i] = mid + side; buf[2 * i + 1] = mid - side;
            }
        }
        if (m.monoBassHz >= 30.0f) RunMonoBass(buf, n, m.monoBassHz, fs);
    }

    // linear below the knee, then knee + (1-knee)*tanh((|x|-knee)/(1-knee)): C1-continuous, tends to +-1
    static float KneeClip(float x, float kneeP, float kneeN) {
        const float k = x >= 0.0f ? kneeP : kneeN;
        const float ax = std::fabs(x);
        if (ax <= k) return x;
        const float room = 1.0f - k;
        const float y = k + room * std::tanh((ax - k) / room);
        return x >= 0.0f ? y : -y;
    }

    float GainReductionDb() const { return grDb_.load(std::memory_order_relaxed); }   // >= 0

    // Static gain computer (also used by the self-test): output level in dB for an
    // input level in dB, with a 6 dB soft knee.
    static float GainComputerDb(float inDb, float threshDb, float ratio) {
        const float knee = 6.0f;
        const float over = inDb - threshDb;
        const float slope = 1.0f - 1.0f / std::max(1.0f, ratio);
        if (2.0f * over < -knee) return inDb;
        if (2.0f * std::fabs(over) <= knee) return inDb - slope * (over + knee * 0.5f) * (over + knee * 0.5f) / (2.0f * knee);
        return threshDb + over / std::max(1.0f, ratio);
    }

private:
    struct Bq {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        float Run(float x) {
            float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void Set(float nb0, float nb1, float nb2, float a0, float na1, float na2) {
            b0 = nb0 / a0; b1 = nb1 / a0; b2 = nb2 / a0; a1 = na1 / a0; a2 = na2 / a0;
        }
    };

    void UpdateFilters(const MasterChainSettings& m, float fs) {
        if (m.hpfHz == lastHpf_ && m.lowDb == lastLow_ && m.midDb == lastMid_ && m.highDb == lastHigh_ && fs == lastFs_) return;
        lastHpf_ = m.hpfHz; lastLow_ = m.lowDb; lastMid_ = m.midDb; lastHigh_ = m.highDb; lastFs_ = fs;
        const float pi = 3.14159265358979f;
        for (int c = 0; c < 2; ++c) {
            // 2nd-order Butterworth high-pass
            {
                float f0 = std::clamp(m.hpfHz, 20.0f, 400.0f);
                float w = 2.0f * pi * f0 / fs, cs = std::cos(w), al = std::sin(w) / (2.0f * 0.70710678f);
                hpf_[c].Set((1 + cs) * 0.5f, -(1 + cs), (1 + cs) * 0.5f, 1 + al, -2 * cs, 1 - al);
            }
            Shelf(eq_[0 * 2 + c], 120.0f, m.lowDb, false, fs);
            Peak(eq_[1 * 2 + c], 1500.0f, m.midDb, 0.8f, fs);
            Shelf(eq_[2 * 2 + c], 9000.0f, m.highDb, true, fs);
        }
    }
    static void Shelf(Bq& q, float f0, float gainDb, bool high, float fs) {
        const float pi = 3.14159265358979f;
        float A = std::pow(10.0f, gainDb / 40.0f);
        float w = 2.0f * pi * std::min(f0, fs * 0.45f) / fs, cs = std::cos(w), sn = std::sin(w);
        float al = sn / 2.0f * std::sqrt(2.0f);   // shelf slope S = 1
        float sa = 2.0f * std::sqrt(A) * al;
        if (!high)
            q.Set(A * ((A + 1) - (A - 1) * cs + sa), 2 * A * ((A - 1) - (A + 1) * cs), A * ((A + 1) - (A - 1) * cs - sa),
                  (A + 1) + (A - 1) * cs + sa, -2 * ((A - 1) + (A + 1) * cs), (A + 1) + (A - 1) * cs - sa);
        else
            q.Set(A * ((A + 1) + (A - 1) * cs + sa), -2 * A * ((A - 1) + (A + 1) * cs), A * ((A + 1) + (A - 1) * cs - sa),
                  (A + 1) - (A - 1) * cs + sa, 2 * ((A - 1) - (A + 1) * cs), (A + 1) - (A - 1) * cs - sa);
    }
    static void Peak(Bq& q, float f0, float gainDb, float Q, float fs) {
        const float pi = 3.14159265358979f;
        float A = std::pow(10.0f, gainDb / 40.0f);
        float w = 2.0f * pi * std::min(f0, fs * 0.45f) / fs, cs = std::cos(w), al = std::sin(w) / (2.0f * Q);
        q.Set(1 + al * A, -2 * cs, 1 - al * A, 1 + al / A, -2 * cs, 1 - al / A);
    }

    // 2nd-order Butterworth high / low pass as a Bq (Q = 1/sqrt2); two in series = Linkwitz-Riley 4th order
    static void Butter(Bq& q, float f0, float fs, bool high) {
        const float pi = 3.14159265358979f;
        const float w = 2.0f * pi * std::min(f0, fs * 0.45f) / fs, cs = std::cos(w), al = std::sin(w) / (2.0f * 0.70710678f);
        if (high) q.Set((1 + cs) * 0.5f, -(1 + cs), (1 + cs) * 0.5f, 1 + al, -2 * cs, 1 - al);
        else      q.Set((1 - cs) * 0.5f,  (1 - cs), (1 - cs) * 0.5f, 1 + al, -2 * cs, 1 - al);
    }
    void UpdateScHpf(float f, float fs) {
        if (f == lastScHpf_) return;
        lastScHpf_ = f;
        for (int c = 0; c < 2; ++c) { Bq keep = scHp_[c]; Butter(scHp_[c], f, fs, true); scHp_[c].z1 = keep.z1; scHp_[c].z2 = keep.z2; }
    }
    // Everything below `hz` becomes mono; the rest keeps its stereo image.
    void RunMonoBass(float* buf, int n, float hz, float fs) {
        if (hz != lastXover_) {
            lastXover_ = hz;
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < 2; ++k) {
                    Bq& lo = xLp_[c * 2 + k]; Bq& hi = xHp_[c * 2 + k];
                    const float z1 = lo.z1, z2 = lo.z2, y1 = hi.z1, y2 = hi.z2;
                    Butter(lo, hz, fs, false); Butter(hi, hz, fs, true);
                    lo.z1 = z1; lo.z2 = z2; hi.z1 = y1; hi.z2 = y2;
                }
        }
        for (int i = 0; i < n; ++i) {
            float lo[2], hi[2];
            for (int c = 0; c < 2; ++c) {
                const float x = buf[2 * i + c];
                lo[c] = xLp_[c * 2 + 1].Run(xLp_[c * 2].Run(x));
                hi[c] = xHp_[c * 2 + 1].Run(xHp_[c * 2].Run(x));
            }
            const float mono = 0.5f * (lo[0] + lo[1]);
            buf[2 * i] = hi[0] + mono; buf[2 * i + 1] = hi[1] + mono;
        }
    }

    void RunCompressor(float* buf, int n, const MasterChainSettings& m, float fs) {
        const float atk = std::exp(-1.0f / (std::max(0.5f, m.compAttackMs) * 0.001f * fs));
        const float rel = std::exp(-1.0f / (std::max(5.0f, m.compReleaseMs) * 0.001f * fs));
        const float makeup = std::pow(10.0f, m.compMakeupDb / 20.0f);
        const float mix = std::clamp(m.compMix, 0.0f, 1.0f);
        float maxGr = 0.0f;
        const int holdN = static_cast<int>(0.014f * fs);
        const bool useSc = m.compScHpfHz > 21.0f;
        if (useSc) UpdateScHpf(m.compScHpfHz, fs);
        for (int i = 0; i < n; ++i) {
            float l = buf[2 * i], r = buf[2 * i + 1];
            float lvl = useSc ? std::max(std::fabs(scHp_[0].Run(l)), std::fabs(scHp_[1].Run(r))) : std::max(std::fabs(l), std::fabs(r));
            // v44: a short peak HOLD (14 ms > half a cycle of a 36 Hz bass) before the release starts, so the
            // detector does not ripple at twice the bass frequency -- that ripple was gain modulation
            // (about -47 dB IMD on a 55 Hz tone with the LOUD preset).
            if (lvl > env_) { env_ = atk * env_ + (1.0f - atk) * lvl; if (lvl >= 0.9f * env_) hold_ = holdN; }
            else if (hold_ > 0) --hold_;
            else env_ = rel * env_ + (1.0f - rel) * lvl;
            float inDb = 20.0f * std::log10(std::max(env_, 1e-6f));
            float outDb = GainComputerDb(inDb, m.compThreshDb, m.compRatio);
            float grDb = inDb - outDb;                       // >= 0
            maxGr = std::max(maxGr, grDb);
            float g = std::pow(10.0f, -grDb / 20.0f) * makeup;
            buf[2 * i] = l * (1.0f - mix) + l * g * mix;
            buf[2 * i + 1] = r * (1.0f - mix) + r * g * mix;
        }
        // meter: fast up, slow down
        float cur = grDb_.load();
        grDb_.store(maxGr > cur ? maxGr : cur * 0.92f + maxGr * 0.08f);
    }

    Bq hpf_[2];
    Bq eq_[6];
    float lastHpf_ = -1, lastLow_ = -99, lastMid_ = -99, lastHigh_ = -99, lastFs_ = 0;
    float env_ = 0.0f;
    int hold_ = 0;
    Bq xLp_[4], xHp_[4], scHp_[2];     // crossover: 2 cascaded biquads per channel, low and high
    float lastXover_ = -1, lastScHpf_ = -1;
    std::atomic<float> grDb_{0.0f};
};

} // namespace t8
