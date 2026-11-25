#pragma once
// Offline measurement helpers shared by t8_render and t8_audit: peak / RMS,
// ITU-R BS.1770 integrated loudness (LUFS), L/R correlation, spectrum via a
// radix-2 FFT, sine-fit THD residual and an RT60 estimate (Schroeder backward
// integration). Header-only, no dependencies beyond the standard library.
#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>
#include <cstddef>

namespace t8m {

constexpr double kPi = 3.14159265358979323846;

inline double ToDb(double lin) { return 20.0 * std::log10(std::max(lin, 1e-12)); }

// ---- plain level statistics on interleaved stereo ---------------------------
struct Levels {
    double peak = 0, rms = 0, dc = 0, correlation = 0, crestDb = 0;
    size_t clipped = 0;       // samples with |x| >= 0.9999
    size_t nonFinite = 0;
};
inline Levels MeasureLevels(const std::vector<float>& st) {
    Levels r; const size_t n = st.size() / 2; if (!n) return r;
    double sq = 0, sl = 0, sr = 0, sll = 0, srr = 0, slr = 0;
    for (size_t i = 0; i < n; ++i) {
        const double l = st[2 * i], rr = st[2 * i + 1];
        if (!std::isfinite(l) || !std::isfinite(rr)) { ++r.nonFinite; continue; }
        r.peak = std::max({r.peak, std::fabs(l), std::fabs(rr)});
        if (std::fabs(l) >= 0.9999 || std::fabs(rr) >= 0.9999) ++r.clipped;
        sq += l * l + rr * rr; sl += l; sr += rr; sll += l * l; srr += rr * rr; slr += l * rr;
    }
    r.rms = std::sqrt(sq / (2.0 * n));
    r.dc = 0.5 * (sl + sr) / n;
    const double den = std::sqrt(std::max(sll * srr, 1e-30));
    r.correlation = slr / den;
    r.crestDb = ToDb(r.peak) - ToDb(r.rms);
    return r;
}

// ---- BS.1770-4 integrated loudness (K-weighting + gating), any sample rate ---
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    double Run(double x) { double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};
inline void KWeighting(int sr, Biquad& shelf, Biquad& hp) {
    // high shelf (+4 dB @ ~1.68 kHz) and RLB high-pass (~38 Hz), derived per the spec for any rate
    {
        const double G = 3.999843853973347, f0 = 1681.974450955533, Q = 0.7071752369554196;
        const double K = std::tan(kPi * f0 / sr), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        shelf.b0 = (Vh + Vb * K / Q + K * K) / a0; shelf.b1 = 2.0 * (K * K - Vh) / a0; shelf.b2 = (Vh - Vb * K / Q + K * K) / a0;
        shelf.a1 = 2.0 * (K * K - 1.0) / a0;       shelf.a2 = (1.0 - K / Q + K * K) / a0;
    }
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan(kPi * f0 / sr), a0 = 1.0 + K / Q + K * K;
        hp.b0 = 1.0 / a0; hp.b1 = -2.0 / a0; hp.b2 = 1.0 / a0;
        hp.a1 = 2.0 * (K * K - 1.0) / a0; hp.a2 = (1.0 - K / Q + K * K) / a0;
    }
}
// returns LUFS (or -120 if everything is gated out)
inline double IntegratedLufs(const std::vector<float>& st, int sr) {
    const size_t n = st.size() / 2; if (n < static_cast<size_t>(sr) / 2) return -120.0;
    std::vector<double> sq(n);
    for (int ch = 0; ch < 2; ++ch) {
        Biquad s, h; KWeighting(sr, s, h);
        for (size_t i = 0; i < n; ++i) { const double y = h.Run(s.Run(st[2 * i + ch])); sq[i] += y * y; }
    }
    const size_t blk = static_cast<size_t>(0.4 * sr), hop = static_cast<size_t>(0.1 * sr);
    std::vector<double> ms;
    for (size_t p = 0; p + blk <= n; p += hop) { double a = 0; for (size_t i = p; i < p + blk; ++i) a += sq[i]; ms.push_back(a / blk); }
    if (ms.empty()) return -120.0;
    auto lufs = [](double m) { return -0.691 + 10.0 * std::log10(std::max(m, 1e-12)); };
    double sum = 0; size_t cnt = 0;
    for (double m : ms) if (lufs(m) > -70.0) { sum += m; ++cnt; }
    if (!cnt) return -120.0;
    const double rel = lufs(sum / cnt) - 10.0;
    sum = 0; cnt = 0;
    for (double m : ms) if (lufs(m) > -70.0 && lufs(m) > rel) { sum += m; ++cnt; }
    return cnt ? lufs(sum / cnt) : -120.0;
}

// ---- spectrum ----------------------------------------------------------------
inline void Fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) { size_t bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(a[i], a[j]); }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * kPi / len; const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) { std::complex<double> w(1); for (size_t k = 0; k < len / 2; ++k) {
            auto u = a[i + k], v = a[i + k + len / 2] * w; a[i + k] = u + v; a[i + k + len / 2] = u - v; w *= wl; } }
    }
}
// Power spectrum (Hann window) of a mono signal, N = power of two; returns N/2 bins of power.
inline std::vector<double> PowerSpectrum(const std::vector<float>& x, size_t start, size_t N) {
    std::vector<std::complex<double>> a(N);
    for (size_t i = 0; i < N; ++i) { const double w = 0.5 - 0.5 * std::cos(2 * kPi * i / N); a[i] = (start + i < x.size() ? x[start + i] : 0.0f) * w; }
    Fft(a);
    std::vector<double> p(N / 2); for (size_t i = 0; i < N / 2; ++i) p[i] = std::norm(a[i]);
    return p;
}
inline double BandEnergy(const std::vector<double>& p, double lo, double hi, int sr, size_t N) {
    const size_t a = static_cast<size_t>(lo * N / sr), b = std::min(p.size(), static_cast<size_t>(hi * N / sr) + 1);
    double e = 0; for (size_t i = a; i < b; ++i) e += p[i]; return e;
}
inline double SpectralCentroid(const std::vector<double>& p, int sr, size_t N) {
    double s = 0, w = 0; for (size_t i = 1; i < p.size(); ++i) { s += p[i] * i * sr / double(N); w += p[i]; } return w > 0 ? s / w : 0;
}

// ---- THD+N of a steady sine via least-squares fit (residual vs fitted sine) -------
inline double SineResidualDb(const std::vector<float>& x, size_t from, size_t to, double freq, int sr) {
    double a = 0, b = 0, aa = 0, bb = 0, ab = 0, ax = 0, bx = 0, xx = 0; const size_t n = to - from;
    for (size_t i = from; i < to; ++i) {
        const double ph = 2 * kPi * freq * i / sr, c = std::cos(ph), s = std::sin(ph), v = x[i];
        aa += c * c; bb += s * s; ab += c * s; ax += c * v; bx += s * v; xx += v * v;
    }
    const double det = aa * bb - ab * ab; a = (ax * bb - bx * ab) / det; b = (bx * aa - ax * ab) / det;
    const double fit = a * ax + b * bx, resid = std::max(xx - fit, 1e-30);
    (void)n; return 10.0 * std::log10(resid / std::max(fit, 1e-30));
}

// ---- RT60 from an impulse response (Schroeder backward integration, T20 extrapolated) ----
inline double Rt60Seconds(const std::vector<float>& ir, int sr) {
    const size_t n = ir.size(); std::vector<double> edc(n); double acc = 0;
    for (size_t i = n; i-- > 0;) { acc += double(ir[i]) * ir[i]; edc[i] = acc; }
    if (acc <= 0) return 0;
    auto at = [&](double db) { for (size_t i = 0; i < n; ++i) if (10 * std::log10(edc[i] / edc[0]) <= db) return double(i); return double(n); };
    const double t5 = at(-5), t25 = at(-25); return (t25 - t5) / sr * 3.0;
}

} // namespace t8m
