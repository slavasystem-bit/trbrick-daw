#pragma once
// Per-track channel EQ (v29): a high-pass (12 or 24 dB/oct), a low shelf, a
// parametric mid bell (frequency + Q) and a high shelf. Runs on the track's
// own signal before the insert FX and the fader. Everything is bypassed (and
// bit-identical to the old signal path) while the HPF is off and all three
// gains are 0 dB, which is the default.
//
// Filters are RBJ biquads in double precision (a 25-40 Hz high-pass in single
// precision is noisy). Coefficients are ramped across each audio block while a
// knob moves, and a band that is switched on/off ramps from / to a flat
// response, so turning a control never clicks.
#include <algorithm>
#include <cmath>

namespace t8 {

struct TrackEqParams {
    float hpfHz = 20.0f;     // 20 = off, up to 500
    float hpfSteep = 0.0f;   // 0 = 12 dB/oct, 1 = 24 dB/oct
    float lowDb = 0.0f;      // -15..+15, low shelf
    float lowHz = 120.0f;    // 30..500
    float midDb = 0.0f;      // -15..+15, bell
    float midHz = 1000.0f;   // 80..10000
    float midQ = 1.0f;       // 0.3..6
    float highDb = 0.0f;     // -15..+15, high shelf
    float highHz = 6000.0f;  // 1500..16000

    bool HpfOn() const { return hpfHz > 20.5f; }
    bool Flat() const { return !HpfOn() && lowDb == 0.0f && midDb == 0.0f && highDb == 0.0f; }
};

constexpr float kEqMinDb = -15.0f, kEqMaxDb = 15.0f;

// Works for Track and Mixer::TrackState, which both carry the same flat eq* fields.
template <class T>
TrackEqParams TrackEqOf(const T& t) {
    TrackEqParams p;
    p.hpfHz = t.eqHpfHz; p.hpfSteep = t.eqHpfSlope;
    p.lowDb = t.eqLowDb; p.lowHz = t.eqLowHz;
    p.midDb = t.eqMidDb; p.midHz = t.eqMidHz; p.midQ = t.eqMidQ;
    p.highDb = t.eqHighDb; p.highHz = t.eqHighHz;
    return p;
}

namespace eqdetail {

struct Coef { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

inline Coef Normalize(double b0, double b1, double b2, double a0, double a1, double a2) {
    Coef c; c.b0 = b0 / a0; c.b1 = b1 / a0; c.b2 = b2 / a0; c.a1 = a1 / a0; c.a2 = a2 / a0; return c;
}
inline double ClampF(double f, double fs) { return std::clamp(f, 10.0, fs * 0.45); }

inline Coef HighPass(double fs, double f, double q) {
    double w = 2.0 * 3.14159265358979323846 * ClampF(f, fs) / fs, cs = std::cos(w), al = std::sin(w) / (2.0 * q);
    return Normalize((1 + cs) / 2, -(1 + cs), (1 + cs) / 2, 1 + al, -2 * cs, 1 - al);
}
inline Coef Bell(double fs, double f, double q, double db) {
    double A = std::pow(10.0, db / 40.0);
    double w = 2.0 * 3.14159265358979323846 * ClampF(f, fs) / fs, cs = std::cos(w), al = std::sin(w) / (2.0 * q);
    return Normalize(1 + al * A, -2 * cs, 1 - al * A, 1 + al / A, -2 * cs, 1 - al / A);
}
inline Coef LowShelf(double fs, double f, double db) {
    double A = std::pow(10.0, db / 40.0);
    double w = 2.0 * 3.14159265358979323846 * ClampF(f, fs) / fs, cs = std::cos(w), sn = std::sin(w);
    double al = sn / 2.0 * std::sqrt(2.0), tsa = 2.0 * std::sqrt(A) * al;
    return Normalize(A * ((A + 1) - (A - 1) * cs + tsa), 2 * A * ((A - 1) - (A + 1) * cs), A * ((A + 1) - (A - 1) * cs - tsa),
                     (A + 1) + (A - 1) * cs + tsa, -2 * ((A - 1) + (A + 1) * cs), (A + 1) + (A - 1) * cs - tsa);
}
inline Coef HighShelf(double fs, double f, double db) {
    double A = std::pow(10.0, db / 40.0);
    double w = 2.0 * 3.14159265358979323846 * ClampF(f, fs) / fs, cs = std::cos(w), sn = std::sin(w);
    double al = sn / 2.0 * std::sqrt(2.0), tsa = 2.0 * std::sqrt(A) * al;
    return Normalize(A * ((A + 1) + (A - 1) * cs + tsa), -2 * A * ((A - 1) + (A + 1) * cs), A * ((A + 1) + (A - 1) * cs - tsa),
                     (A + 1) - (A - 1) * cs + tsa, 2 * ((A - 1) - (A + 1) * cs), (A + 1) - (A - 1) * cs - tsa);
}
inline double MagnitudeDb(const Coef& c, double fs, double f) {
    double w = 2.0 * 3.14159265358979323846 * f / fs;
    double c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
    double nr = c.b0 + c.b1 * c1 + c.b2 * c2, ni = -(c.b1 * s1 + c.b2 * s2);
    double dr = 1 + c.a1 * c1 + c.a2 * c2, di = -(c.a1 * s1 + c.a2 * s2);
    double m = (nr * nr + ni * ni) / std::max(1e-30, dr * dr + di * di);
    return 10.0 * std::log10(std::max(1e-30, m));
}

// Butterworth section Qs: one 12 dB stage, or two stages for 24 dB.
constexpr double kQ12 = 0.70710678, kQ24a = 0.54119610, kQ24b = 1.30656296;

} // namespace eqdetail

// Combined magnitude response of the whole EQ in dB (used by the UI curve and the tests).
inline double TrackEqResponseDb(const TrackEqParams& p, double sampleRate, double freq) {
    using namespace eqdetail;
    double db = 0.0;
    if (p.HpfOn()) {
        if (p.hpfSteep >= 0.5f) {
            db += MagnitudeDb(HighPass(sampleRate, p.hpfHz, kQ24a), sampleRate, freq);
            db += MagnitudeDb(HighPass(sampleRate, p.hpfHz, kQ24b), sampleRate, freq);
        } else {
            db += MagnitudeDb(HighPass(sampleRate, p.hpfHz, kQ12), sampleRate, freq);
        }
    }
    if (p.lowDb != 0.0f) db += MagnitudeDb(LowShelf(sampleRate, p.lowHz, p.lowDb), sampleRate, freq);
    if (p.midDb != 0.0f) db += MagnitudeDb(Bell(sampleRate, p.midHz, std::max(0.1f, p.midQ), p.midDb), sampleRate, freq);
    if (p.highDb != 0.0f) db += MagnitudeDb(HighShelf(sampleRate, p.highHz, p.highDb), sampleRate, freq);
    return db;
}

class TrackEq {
public:
    void Init(int sampleRate) {
        sampleRate_ = std::max(8000, sampleRate);
        for (auto& s : st_) s = Stage{};
    }

    // In-place stereo process (interleaved L/R).
    void Process(float* buf, int numFrames, const TrackEqParams& p) {
        using namespace eqdetail;
        // The flat fast path is only valid when every stage is already off.
        bool anyActive = false;
        for (const auto& s : st_) anyActive = anyActive || s.active;
        if (p.Flat() && !anyActive) return;
        if (numFrames <= 0) return;

        const double fs = sampleRate_;
        const bool hp = p.HpfOn(), steep = p.hpfSteep >= 0.5f;
        Coef tgt[kStages];
        bool want[kStages];
        want[0] = hp;                 tgt[0] = hp ? HighPass(fs, p.hpfHz, steep ? kQ24a : kQ12) : Coef{};
        want[1] = hp && steep;        tgt[1] = want[1] ? HighPass(fs, p.hpfHz, kQ24b) : Coef{};
        want[2] = p.lowDb != 0.0f;    tgt[2] = want[2] ? LowShelf(fs, p.lowHz, p.lowDb) : Coef{};
        want[3] = p.midDb != 0.0f;    tgt[3] = want[3] ? Bell(fs, p.midHz, std::max(0.1f, p.midQ), p.midDb) : Coef{};
        want[4] = p.highDb != 0.0f;   tgt[4] = want[4] ? HighShelf(fs, p.highHz, p.highDb) : Coef{};

        const double inv = 1.0 / numFrames;
        for (int k = 0; k < kStages; ++k) {
            Stage& s = st_[k];
            if (!want[k] && !s.active) continue;
            if (want[k] && !s.active) { s = Stage{}; s.active = true; }   // fade in from flat
            const Coef d{(tgt[k].b0 - s.cur.b0) * inv, (tgt[k].b1 - s.cur.b1) * inv, (tgt[k].b2 - s.cur.b2) * inv,
                         (tgt[k].a1 - s.cur.a1) * inv, (tgt[k].a2 - s.cur.a2) * inv};
            Coef c = s.cur;
            for (int i = 0; i < numFrames; ++i) {
                c.b0 += d.b0; c.b1 += d.b1; c.b2 += d.b2; c.a1 += d.a1; c.a2 += d.a2;
                for (int ch = 0; ch < 2; ++ch) {
                    float& x = buf[static_cast<size_t>(i) * 2 + ch];
                    double in = x;
                    double y = c.b0 * in + s.z1[ch];
                    s.z1[ch] = c.b1 * in - c.a1 * y + s.z2[ch];
                    s.z2[ch] = c.b2 * in - c.a2 * y;
                    x = static_cast<float>(y);
                }
            }
            s.cur = tgt[k];
            if (!want[k]) s = Stage{};   // ramped to flat over this block; now fully off
        }
    }

private:
    static constexpr int kStages = 5;   // HPF a, HPF b (24 dB only), low shelf, mid bell, high shelf
    struct Stage {
        eqdetail::Coef cur;
        double z1[2] = {0, 0}, z2[2] = {0, 0};
        bool active = false;
    };
    int sampleRate_ = 44100;
    Stage st_[kStages];
};

} // namespace t8
