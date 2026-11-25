// t8_kitgen: builds the electro / techno / breaks / acid_v2 kits and (v43.1) the 14 classic kits (606, 707, 808, 909, acid,
// cr78, dub, funk, gabber, industrial, linn, lofi, minimal, trap808) as 24-bit, 48 kHz WAV files.
//
// Every sound is synthesised offline at 192 kHz (so square waves, saturation and hard filters do not alias) and
// then converted to 48 kHz with the same band-limited resampler the engine uses. Levels follow real drum-machine
// proportions instead of peak-normalising everything: the kick is the loudest, hats are well below it.
// Hats, claps and percussion are written as stereo files (decorrelated noise), kicks and snares are centred.
//
// usage: t8_kitgen [outdir=kits] [--report]
#include "Resample.h"
#include "WavFile.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace t8;

namespace {

constexpr int kGenRate = 192000;
constexpr int kOutRate = 48000;
constexpr double kPi = 3.14159265358979323846;
using Vec = std::vector<double>;

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 2685821657736338717ull + 88172645463325252ull) {}
    double Next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return static_cast<double>(s >> 11) / 4503599627370496.0 - 1.0; }   // [-1, 1)
};

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    static Biquad Make(int type, double f, double q) {   // 0 LP, 1 HP, 2 BP (0 dB peak)
        Biquad b; const double w = 2.0 * kPi * std::min(f, kGenRate * 0.45) / kGenRate, c = std::cos(w), s = std::sin(w), al = s / (2.0 * q);
        double a0 = 1.0 + al;
        if (type == 0) { b.b0 = (1 - c) / 2; b.b1 = 1 - c; b.b2 = (1 - c) / 2; }
        else if (type == 1) { b.b0 = (1 + c) / 2; b.b1 = -(1 + c); b.b2 = (1 + c) / 2; }
        else { b.b0 = al; b.b1 = 0; b.b2 = -al; }
        b.a1 = -2 * c / a0; b.a2 = (1 - al) / a0; b.b0 /= a0; b.b1 /= a0; b.b2 /= a0;
        return b;
    }
    double Run(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};
Vec Filter(const Vec& x, int type, double f, double q = 0.7071, int passes = 1) {
    Vec y = x;
    for (int p = 0; p < passes; ++p) { Biquad b = Biquad::Make(type, f, q); for (double& v : y) v = b.Run(v); }
    return y;
}

Vec Zeros(double sec) { return Vec(static_cast<size_t>(sec * kGenRate), 0.0); }
double T(size_t i) { return static_cast<double>(i) / kGenRate; }
void Add(Vec& a, const Vec& b, double g = 1.0) { for (size_t i = 0; i < a.size() && i < b.size(); ++i) a[i] += b[i] * g; }
Vec Scale(Vec a, double g) { for (double& v : a) v *= g; return a; }
Vec Mix(const Vec& a, const Vec& b, double wa, double wb) { Vec o(std::max(a.size(), b.size()), 0.0); Add(o, a, wa); Add(o, b, wb); return o; }

Vec Noise(double sec, uint64_t seed) { Vec n = Zeros(sec); Rng r(seed); for (double& v : n) v = r.Next(); return n; }
// attack ramp (seconds) then exponential decay with time constant tau
void Env(Vec& x, double tau, double attack = 0.0004, double start = 0.0) {
    for (size_t i = 0; i < x.size(); ++i) {
        const double t = T(i) - start;
        if (t < 0) { x[i] = 0; continue; }
        x[i] *= (t < attack ? t / attack : 1.0) * std::exp(-(t) / tau);
    }
}
void Drive(Vec& x, double g) { if (g <= 0.01) return; const double n = std::tanh(g); for (double& v : x) v = std::tanh(v * g) / n; }

// ---- sound building blocks ---------------------------------------------------------------------------------------
struct KickP { double f0, f1, pitchTau, ampTau, len, drive, click, clickHz; double sub = 0.0; double thump = 0.0; };
Vec Kick(const KickP& p, uint64_t seed) {
    Vec y = Zeros(p.len); double ph = 0, ph2 = 0;
    for (size_t i = 0; i < y.size(); ++i) {
        const double t = T(i), f = p.f1 + (p.f0 - p.f1) * std::exp(-t / p.pitchTau);
        ph += 2 * kPi * f / kGenRate; ph2 += 2 * kPi * 2.0 * f / kGenRate;
        y[i] = std::sin(ph) + p.thump * std::sin(ph2) * std::exp(-t / 0.02);
    }
    Env(y, p.ampTau, 0.0006);
    Drive(y, p.drive);
    if (p.click > 0) {   // beater / transient: a short band-passed noise burst plus a very short high sine
        Vec c = Noise(0.02, seed); c = Filter(c, 2, p.clickHz, 1.2); Env(c, 0.0012, 0.00005);
        Add(y, c, p.click * 0.6);
    }
    return y;
}

struct SnareP { double f1, f2, toneTau, toneLvl, noiseHp, noiseBp, noiseTau, noiseLvl, len, tailLvl = 0.0, tailTau = 0.3, tailLp = 3000.0; double bend = 0.12; };
Vec Snare(const SnareP& p, uint64_t seed) {
    Vec y = Zeros(p.len), a = Zeros(p.len);
    double ph1 = 0, ph2 = 0;
    for (size_t i = 0; i < y.size(); ++i) {
        const double t = T(i), bnd = 1.0 + p.bend * std::exp(-t / 0.012);
        ph1 += 2 * kPi * p.f1 * bnd / kGenRate; ph2 += 2 * kPi * p.f2 * bnd / kGenRate;
        a[i] = 0.7 * std::sin(ph1) + 0.45 * std::sin(ph2);
    }
    Env(a, p.toneTau, 0.0004);
    Vec n = Noise(p.len, seed); n = Filter(n, 1, p.noiseHp, 0.7, 2); n = Filter(n, 2, p.noiseBp, 0.6);
    Env(n, p.noiseTau, 0.0003);
    Vec s = Mix(a, n, p.toneLvl, p.noiseLvl);
    if (p.tailLvl > 0) { Vec t2 = Noise(p.len, seed + 99); t2 = Filter(t2, 0, p.tailLp, 0.7, 2); t2 = Filter(t2, 1, 400, 0.7); Env(t2, p.tailTau, 0.004); Add(s, t2, p.tailLvl); }
    return s;
}

struct ClapP { int bursts; double spacing, burstTau, tailTau, bpHz, bpQ, tailLvl, hp; };
// returns one channel; call twice with different seeds for L / R
Vec ClapChannel(const ClapP& p, uint64_t seed, double len) {
    Vec out = Zeros(len);
    for (int b = 0; b < p.bursts; ++b) {
        Vec n = Noise(len, seed + 17 * b); n = Filter(n, 2, p.bpHz, p.bpQ); n = Filter(n, 1, p.hp, 0.7);
        Env(n, p.burstTau, 0.0002, b * p.spacing); Add(out, n, 1.0 - 0.1 * b);
    }
    Vec t = Noise(len, seed + 501); t = Filter(t, 2, p.bpHz, p.bpQ * 0.8); t = Filter(t, 1, p.hp, 0.7);
    Env(t, p.tailTau, 0.002, p.bursts * p.spacing * 0.8); Add(out, t, p.tailLvl);
    return out;
}

// 909-style metallic hat: six detuned square oscillators through band-pass / high-pass, plus noise
struct HatP { double scale, tau, len, hp, bp, noiseMix; double decayTilt = 0.0; };
Vec HatChannel(const HatP& p, uint64_t seed, double phaseOffset) {
    static const double kFreq[6] = {205.3, 304.4, 369.6, 522.7, 540.0, 800.0};
    Vec y = Zeros(p.len);
    for (int o = 0; o < 6; ++o) {
        double ph = phaseOffset * (o + 1) * 0.37; const double f = kFreq[o] * p.scale;
        for (size_t i = 0; i < y.size(); ++i) { ph += f / kGenRate; ph -= std::floor(ph); y[i] += (ph < 0.5 ? 1.0 : -1.0) * 0.18; }
    }
    y = Filter(y, 2, p.bp, 1.4); y = Filter(y, 1, p.hp, 0.9, 2);
    if (p.noiseMix > 0) { Vec n = Noise(p.len, seed); n = Filter(n, 1, p.hp * 0.9, 0.7, 2); Vec mixed = Mix(y, n, 1.0 - p.noiseMix, p.noiseMix * 1.4); y = mixed; }
    y = Filter(y, 0, 13500, 0.7071, 2);
    Env(y, p.tau, 0.0003);
    return y;
}

// noise hat for the breaks kit (acoustic-like: band-limited noise with a slow-ish swell)
Vec NoiseHatChannel(double tau, double len, double hp, double bp, uint64_t seed, double attack = 0.0006) {
    Vec n = Noise(len, seed); n = Filter(n, 1, hp, 0.7, 2); n = Filter(n, 2, bp, 0.5); n = Filter(n, 0, 13000, 0.7071, 2); Env(n, tau, attack); return n;
}

struct TomP { double f0, f1, pitchTau, ampTau, len, fm = 0.0, click = 0.3, drive = 0.0; };
Vec Tom(const TomP& p, uint64_t seed) {
    Vec y = Zeros(p.len); double ph = 0, pm = 0;
    for (size_t i = 0; i < y.size(); ++i) {
        const double t = T(i), f = p.f1 + (p.f0 - p.f1) * std::exp(-t / p.pitchTau);
        pm += 2 * kPi * f * 1.51 / kGenRate;
        ph += 2 * kPi * f / kGenRate + p.fm * std::sin(pm) * std::exp(-t / 0.03) * 2 * kPi * f / kGenRate;
        y[i] = std::sin(ph);
    }
    Env(y, p.ampTau, 0.0005); Drive(y, p.drive);
    if (p.click > 0) { Vec c = Noise(0.02, seed); c = Filter(c, 2, 3000, 1.0); Env(c, 0.0015, 0.00005); Add(y, c, p.click * 0.4); }
    return y;
}

Vec Cowbell(double f1, double f2, double tau, double len) {
    Vec y = Zeros(len); double a = 0, b = 0;
    for (size_t i = 0; i < y.size(); ++i) { a += f1 / kGenRate; b += f2 / kGenRate; a -= std::floor(a); b -= std::floor(b); y[i] = (a < 0.5 ? 1.0 : -1.0) + (b < 0.5 ? 1.0 : -1.0); }
    y = Filter(y, 2, (f1 + f2) * 0.95, 1.8); y = Filter(y, 1, 350, 0.7);
    Vec e = y; Env(e, tau, 0.0003);
    for (size_t i = 0; i < y.size(); ++i) y[i] = e[i] * (0.7 + 0.3 * std::exp(-T(i) / 0.012));
    return y;
}

Vec Rim(double f1, double f2, double len, uint64_t seed) {
    Vec y = Zeros(len); double a = 0, b = 0;
    for (size_t i = 0; i < y.size(); ++i) { a += 2 * kPi * f1 / kGenRate; b += 2 * kPi * f2 / kGenRate; y[i] = 0.6 * std::sin(a) + 0.6 * std::sin(b); }
    Env(y, 0.006, 0.00015);
    Vec n = Noise(len, seed); n = Filter(n, 2, 3200, 1.0); Env(n, 0.003, 0.00008); Add(y, n, 0.5);
    return y;
}

Vec Shaker(double len, double hp, uint64_t seed) {
    Vec n = Noise(len, seed); n = Filter(n, 1, hp, 0.7, 2); n = Filter(n, 0, 9000, 0.7);
    for (size_t i = 0; i < n.size(); ++i) { const double t = T(i), e = (t < 0.012 ? t / 0.012 : std::exp(-(t - 0.012) / 0.045)); n[i] *= e; }
    return n;
}

// ---- output ---------------------------------------------------------------------------------------------------------
struct Sound { Vec l, r; bool stereo = false; double peakDb = -1.0; };
Sound Mono(Vec v, double peakDb) { Sound s; s.l = v; s.r = v; s.stereo = false; s.peakDb = peakDb; return s; }
Sound Stereo(Vec l, Vec r, double peakDb) { Sound s; s.l = std::move(l); s.r = std::move(r); s.stereo = true; s.peakDb = peakDb; return s; }

struct Report { std::string name; double peakDb, rmsDb, sec, corr; };
std::vector<Report> g_report;

void Save(const std::string& dir, const std::string& name, Sound s, bool dcBlock = true) {
    const size_t n = std::min(s.l.size(), s.r.size());
    s.l.resize(n); s.r.resize(n);
    if (dcBlock) { s.l = Filter(s.l, 1, 14.0, 0.7071); s.r = Filter(s.r, 1, 14.0, 0.7071); }
    const int ch = s.stereo ? 2 : 1;
    std::vector<float> in(n * ch);
    for (size_t i = 0; i < n; ++i) { if (ch == 2) { in[2 * i] = static_cast<float>(s.l[i]); in[2 * i + 1] = static_cast<float>(s.r[i]); } else in[i] = static_cast<float>(s.l[i]); }
    // convert first, then set the level: the peak that matters is the one in the 48 kHz file
    std::vector<float> out = ResampleInterleaved(in, ch, kGenRate, kOutRate);
    size_t fr = out.size() / ch;
    float pk = 0; for (float v : out) pk = std::max(pk, std::fabs(v));
    const float g = pk > 0 ? static_cast<float>(std::pow(10.0, s.peakDb / 20.0)) / pk : 1.0f;
    // trim leading silence (keep ~0.3 ms), fade the tail over the last 25 ms so the file ends at zero
    size_t first = 0;
    while (first < fr && std::fabs(out[first * ch]) < pk * 0.0005f && std::fabs(out[first * ch + (ch - 1)]) < pk * 0.0005f) ++first;
    first = first > 15 ? first - 15 : 0;
    const size_t len = fr - first, fade = std::min<size_t>(static_cast<size_t>(0.025 * kOutRate), len / 4);
    std::vector<float> fin(len * ch);
    for (size_t i = 0; i < len; ++i) {
        float w = 1.0f; if (i + fade > len) { const float x = static_cast<float>(len - i) / static_cast<float>(fade); w = 0.5f - 0.5f * std::cos(static_cast<float>(kPi) * x); }
        for (int c = 0; c < ch; ++c) fin[i * ch + c] = out[(first + i) * ch + c] * g * w;
    }
    float mx = 0; for (float v : fin) mx = std::max(mx, std::fabs(v));
    if (mx > 0.999f) for (float& v : fin) v *= 0.999f / mx;
    const std::string path = dir + "/" + name + ".wav";
    if (!WriteWavPCM24(path, fin, ch, kOutRate)) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); std::exit(1); }
    double e = 0, ab = 0, aa = 0, bb = 0; float opk = 0;
    for (size_t i = 0; i < len; ++i) { const float a = fin[i * ch], b = fin[i * ch + (ch - 1)]; e += 0.5 * (a * a + b * b); ab += a * b; aa += a * a; bb += b * b; opk = std::max({opk, std::fabs(a), std::fabs(b)}); }
    g_report.push_back({dir.substr(dir.rfind('/') + 1) + "/" + name, 20.0 * std::log10(std::max(opk, 1e-9f)), 10.0 * std::log10(std::max(e / std::max<size_t>(len, 1), 1e-12)), len / static_cast<double>(kOutRate), ab / std::sqrt(aa * bb + 1e-30)});
}

// stereo helpers: two independent renders are folded into mid / side and the side is turned down, so the image is real
// (L/R correlation around 0.6) but the mono sum keeps most of the energy
Sound Wide(const Vec& a, const Vec& b, double peakDb, double side = 0.5) {
    const size_t n = std::min(a.size(), b.size()); Vec l(n), r(n);
    for (size_t i = 0; i < n; ++i) { const double m = 0.5 * (a[i] + b[i]), sd = 0.5 * (a[i] - b[i]) * side; l[i] = m + sd; r[i] = m - sd; }
    return Stereo(l, r, peakDb);
}
Sound StereoHat(const HatP& p, uint64_t seed, double peakDb) { return Wide(HatChannel(p, seed, 0.0), HatChannel(p, seed + 1000, 1.0), peakDb); }
Sound StereoClap(const ClapP& p, uint64_t seed, double len, double peakDb) { return Wide(ClapChannel(p, seed, len), ClapChannel(p, seed + 4000, len), peakDb, 0.6); }

void MakeDir(const std::string& d) { ::mkdir(d.c_str(), 0755); }

// ---- the four kits ----------------------------------------------------------------------------------------------------
void Electro(const std::string& root) {
    const std::string d = root + "/electro"; MakeDir(d);
    Save(d, "kick",   Mono(Kick({140, 50, 0.022, 0.10, 0.55, 1.2, 0.5, 2600, 0.0, 0.25}, 1), -0.8));
    Save(d, "kick_2", Mono(Kick({110, 44, 0.030, 0.19, 0.9, 0.6, 0.25, 2200, 0.0, 0.1}, 2), -0.8));
    Save(d, "kick_3", Mono(Kick({175, 56, 0.012, 0.06, 0.35, 1.8, 0.7, 3000, 0.0, 0.3}, 3), -1.2));
    Save(d, "snare",   Mono(Snare({188, 332, 0.07, 0.8, 1700, 4600, 0.11, 0.9, 0.45}, 11), -2.5));
    Save(d, "snare_2", Mono(Snare({210, 360, 0.045, 0.6, 1800, 4800, 0.07, 1.0, 0.3, 0.18, 0.2, 2600}, 12), -2.5));
    Save(d, "clap",   StereoClap({4, 0.0105, 0.0035, 0.085, 1250, 1.3, 0.9, 500}, 21, 0.45, -3.5));
    Save(d, "clap_2", StereoClap({3, 0.0075, 0.003, 0.05, 1500, 1.6, 0.7, 600}, 22, 0.3, -3.5));
    Save(d, "chh",   StereoHat({1.0, 0.016, 0.12, 6000, 8200, 0.2}, 31, -9.0));
    Save(d, "chh_2", StereoHat({1.12, 0.009, 0.08, 7000, 9000, 0.3}, 32, -9.5));
    Save(d, "ohh",   StereoHat({1.0, 0.12, 0.7, 5600, 8000, 0.2}, 41, -6.5));
    Save(d, "perc",   Mono(Tom({330, 205, 0.02, 0.16, 0.5, 0.0, 0.3, 0.3}, 51), -6.0));
    Save(d, "perc_2", Mono(Cowbell(540, 800, 0.16, 0.6), -7.0));
    Save(d, "perc_3", Mono(Rim(1700, 920, 0.2, 53), -6.0));
}

void Techno(const std::string& root) {
    const std::string d = root + "/techno"; MakeDir(d);
    Save(d, "kick",   Mono(Kick({210, 47, 0.030, 0.17, 0.85, 2.4, 0.35, 2800, 0.0, 0.2}, 101), -0.8));
    Save(d, "kick_2", Mono(Kick({160, 52, 0.050, 0.28, 1.1, 1.0, 0.2, 2200, 0.0, 0.1}, 102), -0.8));
    Save(d, "kick_3", Mono(Kick({230, 58, 0.016, 0.09, 0.45, 2.8, 0.55, 3200, 0.0, 0.3}, 103), -1.0));
    Save(d, "snare",   Mono(Snare({165, 300, 0.05, 0.7, 2200, 4800, 0.09, 0.8, 0.4, 0.2, 0.18, 2800}, 111), -3.0));
    Save(d, "snare_2", Mono(Snare({150, 250, 0.03, 0.5, 1900, 4800, 0.06, 0.9, 0.25}, 112), -3.0));
    Save(d, "clap",   StereoClap({3, 0.0115, 0.004, 0.14, 1100, 1.1, 1.0, 450}, 121, 0.55, -3.5));
    Save(d, "clap_2", StereoClap({5, 0.0085, 0.003, 0.07, 1600, 1.4, 0.6, 700}, 122, 0.35, -4.0));
    Save(d, "chh",   StereoHat({1.0, 0.014, 0.10, 6800, 8800, 0.55}, 131, -9.5));
    Save(d, "chh_2", StereoHat({1.2, 0.008, 0.07, 7600, 9600, 0.4}, 132, -10.0));
    Save(d, "ohh",   StereoHat({1.0, 0.16, 0.8, 6200, 8600, 0.45}, 141, -7.0));
    Save(d, "perc",   Mono(Tom({240, 160, 0.03, 0.14, 0.45, 1.8, 0.2, 0.4}, 151), -6.0));
    Save(d, "perc_2", Mono(Cowbell(740, 1110, 0.09, 0.4), -8.0));
    Save(d, "perc_3", Mono(Rim(2200, 1330, 0.15, 153), -6.5));
}

void Breaks(const std::string& root) {
    const std::string d = root + "/breaks"; MakeDir(d);
    Save(d, "kick",   Mono(Kick({125, 62, 0.014, 0.08, 0.35, 0.7, 0.8, 2400, 0.0, 0.5}, 201), -0.8));
    Save(d, "kick_2", Mono(Kick({100, 52, 0.020, 0.13, 0.55, 0.5, 0.6, 2000, 0.0, 0.4}, 202), -0.8));
    Save(d, "snare",   Mono(Snare({205, 360, 0.085, 0.75, 1200, 4000, 0.15, 1.1, 0.65, 0.3, 0.30, 3200}, 211), -2.0));
    Save(d, "snare_2", Mono(Snare({230, 410, 0.04, 0.5, 1600, 4600, 0.07, 0.9, 0.3}, 212), -3.0));
    Save(d, "snare_3", Mono(Snare({180, 300, 0.12, 0.9, 900, 3400, 0.18, 0.8, 0.75, 0.45, 0.35, 2600}, 213), -2.5));
    Save(d, "clap",   StereoClap({4, 0.012, 0.004, 0.11, 1000, 1.0, 0.9, 420}, 221, 0.45, -4.0));
    Save(d, "chh",   Wide(NoiseHatChannel(0.014, 0.16, 5000, 8500, 231), NoiseHatChannel(0.014, 0.16, 5000, 8500, 1231), -9.0));
    Save(d, "chh_2", Wide(NoiseHatChannel(0.008, 0.10, 5800, 9500, 232), NoiseHatChannel(0.008, 0.10, 5800, 9500, 1232), -9.5));
    Save(d, "ohh",   Wide(NoiseHatChannel(0.13, 0.8, 4600, 8000, 241, 0.003), NoiseHatChannel(0.13, 0.8, 4600, 8000, 1241, 0.003), -6.5));
    Save(d, "perc",   Wide(Shaker(0.25, 3500, 251), Shaker(0.25, 3500, 1251), -8.0));
    Save(d, "perc_2", Mono(Tom({420, 290, 0.015, 0.11, 0.4, 0.0, 0.4, 0.0}, 252), -6.0));
    Save(d, "perc_3", Mono(Rim(1900, 1010, 0.18, 253), -6.5));
}

void AcidV2(const std::string& root) {
    const std::string d = root + "/acid_v2"; MakeDir(d);
    Save(d, "kick",   Mono(Kick({230, 54, 0.022, 0.15, 0.7, 1.7, 0.45, 2600, 0.0, 0.15}, 301), -0.6));
    Save(d, "kick_2", Mono(Kick({200, 60, 0.015, 0.09, 0.4, 2.6, 0.6, 3000, 0.0, 0.2}, 302), -0.8));
    Save(d, "snare",   Mono(Snare({182, 330, 0.065, 0.8, 1800, 4600, 0.12, 1.0, 0.4}, 311), -2.5));
    Save(d, "snare_2", Mono(Snare({200, 340, 0.04, 0.55, 1900, 4800, 0.085, 1.0, 0.28, 0.2, 0.15, 2800}, 312), -3.0));
    Save(d, "clap",   StereoClap({4, 0.0105, 0.0036, 0.10, 1200, 1.2, 0.95, 480}, 321, 0.45, -3.5));
    Save(d, "chh",   StereoHat({1.0, 0.015, 0.11, 6400, 8400, 0.25}, 331, -9.0));
    Save(d, "chh_2", StereoHat({1.0, 0.007, 0.07, 7400, 9400, 0.35}, 332, -10.0));
    Save(d, "ohh",   StereoHat({1.0, 0.14, 0.75, 5800, 8200, 0.25}, 341, -6.5));
    Save(d, "perc",   Mono(Cowbell(540, 800, 0.20, 0.7), -7.0));
    Save(d, "perc_2", Mono(Tom({190, 120, 0.025, 0.22, 0.6, 0.0, 0.3, 0.8}, 352), -6.0));
    Save(d, "perc_3", Mono(Rim(1800, 960, 0.2, 353), -6.5));
}

// ---- v43.1: the 14 classic kits, rebuilt with the same chain (192 kHz synthesis, band-limited resample, tail fade) ----
// sample-rate / bit-depth reduction for the lo-fi flavoured kits (the aliasing is the point); hold = samples at 192 kHz
Vec Crush(Vec x, int bits, int hold) {
    const double q = std::pow(2.0, bits - 1);
    double held = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        if (hold <= 1 || i % static_cast<size_t>(hold) == 0) held = std::round(x[i] * q) / q;
        x[i] = held;
    }
    return x;
}
Vec Soft(Vec x, double lp) { return Filter(x, 0, lp, 0.7071, 2); }
Sound SoftMono(Vec v, double lp, double peakDb) { return Mono(Soft(std::move(v), lp), peakDb); }
Sound WideCrushed(const Vec& a, const Vec& b, int bits, int hold, double lp, double peakDb) {
    return Wide(Soft(Crush(a, bits, hold), lp), Soft(Crush(b, bits, hold), lp), peakDb);
}

void K606(const std::string& root) {
    const std::string d = root + "/606"; MakeDir(d);
    Save(d, "kick",   Mono(Kick({140, 58, 0.018, 0.085, 0.4, 0.7, 0.35, 2600, 0.0, 0.15}, 4101), -0.8));
    Save(d, "kick_2", Mono(Kick({170, 66, 0.012, 0.06, 0.3, 1.1, 0.5, 3000, 0.0, 0.1}, 4102), -0.8));
    Save(d, "snare",   Mono(Snare({190, 320, 0.045, 0.7, 2200, 5200, 0.08, 0.85, 0.3}, 4111), -2.5));
    Save(d, "snare_2", Mono(Snare({215, 355, 0.03, 0.5, 2500, 5600, 0.05, 0.9, 0.2}, 4112), -3.0));
    Save(d, "clap",   StereoClap({3, 0.0095, 0.003, 0.06, 1500, 1.4, 0.6, 700}, 4121, 0.3, -4.0));
    Save(d, "chh",   StereoHat({1.0, 0.011, 0.07, 7800, 9500, 0.1}, 4131, -9.0));
    Save(d, "chh_2", StereoHat({1.1, 0.006, 0.05, 8400, 10200, 0.15}, 4132, -9.5));
    Save(d, "ohh",   StereoHat({1.0, 0.085, 0.4, 7000, 9000, 0.1}, 4141, -6.5));
    Save(d, "perc",   Mono(Tom({210, 150, 0.02, 0.14, 0.4, 0.0, 0.3, 0.2}, 4151), -6.0));
    Save(d, "perc_2", Mono(Tom({300, 220, 0.015, 0.1, 0.3, 0.0, 0.3, 0.2}, 4152), -6.5));
    Save(d, "perc_3", Mono(Rim(2000, 1050, 0.15, 4153), -6.5));
}

void K707(const std::string& root) {
    const std::string d = root + "/707"; MakeDir(d);
    Save(d, "kick",   Mono(Kick({175, 52, 0.020, 0.11, 0.4, 1.5, 0.45, 2800, 0.0, 0.2}, 4201), -0.8));
    Save(d, "kick_2", Mono(Kick({150, 60, 0.028, 0.15, 0.45, 0.9, 0.25, 2400, 0.0, 0.1}, 4202), -0.8));
    Save(d, "snare",   Mono(Snare({200, 345, 0.05, 0.6, 1900, 5600, 0.10, 1.0, 0.3, 0.1, 0.12, 3600}, 4211), -2.5));
    Save(d, "snare_2", Mono(Snare({225, 380, 0.03, 0.5, 2200, 6000, 0.06, 1.0, 0.2}, 4212), -3.0));
    Save(d, "clap",   StereoClap({4, 0.010, 0.003, 0.08, 1400, 1.5, 0.8, 650}, 4221, 0.35, -3.5));
    Save(d, "chh",   StereoHat({1.25, 0.009, 0.06, 8200, 10000, 0.3}, 4231, -9.0));
    Save(d, "chh_2", StereoHat({1.35, 0.005, 0.045, 8800, 10800, 0.35}, 4232, -9.5));
    Save(d, "ohh",   StereoHat({1.25, 0.11, 0.5, 7600, 9800, 0.3}, 4241, -6.5));
    Save(d, "perc",   Mono(Rim(2300, 1250, 0.18, 4251), -6.0));
    Save(d, "perc_2", Mono(Tom({260, 190, 0.012, 0.09, 0.25, 0.0, 0.4, 0.3}, 4252), -6.5));
    Save(d, "perc_3", Mono(Cowbell(800, 1190, 0.07, 0.3), -8.0));
}

void K808(const std::string& root) {
    const std::string d = root + "/808"; MakeDir(d);
    Save(d, "kick",   Mono(Kick({130, 44, 0.032, 0.34, 1.0, 0.45, 0.15, 2000, 0.0, 0.0}, 4301), -0.8));
    Save(d, "kick_2", Mono(Kick({115, 49, 0.026, 0.2, 0.7, 0.8, 0.3, 2200, 0.0, 0.0}, 4302), -0.8));
    Save(d, "snare",   Mono(Snare({175, 330, 0.07, 0.8, 1900, 5000, 0.11, 0.8, 0.4, 0.0, 0.3, 3000, 0.08}, 4311), -2.5));
    Save(d, "snare_2", Mono(Snare({195, 350, 0.04, 0.6, 2100, 5400, 0.07, 0.9, 0.25}, 4312), -3.0));
    Save(d, "clap",   StereoClap({4, 0.0105, 0.0035, 0.11, 1100, 1.2, 0.9, 500}, 4321, 0.45, -3.5));
    Save(d, "chh",   StereoHat({1.0, 0.012, 0.09, 7200, 8600, 0.0}, 4331, -9.0));
    Save(d, "chh_2", StereoHat({1.0, 0.006, 0.06, 8000, 9400, 0.05}, 4332, -9.5));
    Save(d, "ohh",   StereoHat({1.0, 0.13, 0.55, 6600, 8200, 0.0}, 4341, -6.5));
    Save(d, "perc",   Mono(Tom({330, 205, 0.02, 0.16, 0.45, 0.0, 0.1, 0.0}, 4351), -6.0));
    Save(d, "perc_2", Mono(Tom({240, 160, 0.025, 0.18, 0.5, 0.0, 0.1, 0.0}, 4352), -6.0));
    Save(d, "perc_3", Mono(Cowbell(540, 800, 0.15, 0.55), -7.0));
}

void K909(const std::string& root) {
    const std::string d = root + "/909"; MakeDir(d);
    Save(d, "kick",   Mono(Kick({210, 50, 0.032, 0.17, 0.55, 2.2, 0.55, 2800, 0.0, 0.2}, 4401), -0.6));
    Save(d, "kick_2", Mono(Kick({180, 56, 0.020, 0.1, 0.4, 3.0, 0.65, 3200, 0.0, 0.25}, 4402), -0.8));
    Save(d, "snare",   Mono(Snare({180, 330, 0.06, 0.8, 1600, 5200, 0.10, 0.95, 0.3, 0.12, 0.15, 3400}, 4411), -2.5));
    Save(d, "snare_2", Mono(Snare({205, 360, 0.04, 0.6, 1900, 5600, 0.07, 1.0, 0.22}, 4412), -3.0));
    Save(d, "clap",   StereoClap({3, 0.0100, 0.0035, 0.10, 1250, 1.2, 0.85, 550}, 4421, 0.4, -3.5));
    Save(d, "chh",   StereoHat({1.0, 0.013, 0.09, 7000, 9000, 0.12}, 4431, -9.0));
    Save(d, "chh_2", StereoHat({1.08, 0.007, 0.06, 7800, 9800, 0.2}, 4432, -9.5));
    Save(d, "ohh",   StereoHat({1.0, 0.14, 0.6, 6400, 8800, 0.12}, 4441, -6.5));
    Save(d, "perc",   Mono(Tom({260, 170, 0.025, 0.15, 0.4, 1.0, 0.3, 0.3}, 4451), -6.0));
    Save(d, "perc_2", Mono(Tom({200, 130, 0.025, 0.18, 0.45, 1.0, 0.3, 0.3}, 4452), -6.0));
    Save(d, "perc_3", Mono(Rim(1800, 1000, 0.16, 4453), -6.5));
}

void KAcid(const std::string& root) {
    const std::string d = root + "/acid"; MakeDir(d);
    Save(d, "kick",   Mono(Kick({260, 52, 0.020, 0.16, 0.5, 4.5, 0.4, 2600, 0.0, 0.2}, 4501), -0.6));
    Save(d, "kick_2", Mono(Kick({220, 58, 0.014, 0.10, 0.35, 5.5, 0.55, 3000, 0.0, 0.25}, 4502), -0.8));
    Save(d, "snare",   Mono(Snare({185, 335, 0.05, 0.7, 2000, 5000, 0.09, 1.0, 0.3}, 4511), -2.5));
    Save(d, "snare_2", Mono(Snare({205, 360, 0.03, 0.5, 2200, 5400, 0.06, 1.1, 0.2}, 4512), -3.0));
    Save(d, "clap",   StereoClap({5, 0.0085, 0.003, 0.08, 1500, 1.6, 0.8, 650}, 4521, 0.35, -3.5));
    Save(d, "chh",   StereoHat({1.0, 0.012, 0.09, 7000, 9000, 0.35}, 4531, -9.0));
    Save(d, "chh_2", StereoHat({1.15, 0.006, 0.06, 7800, 9800, 0.4}, 4532, -9.5));
    Save(d, "ohh",   StereoHat({1.0, 0.16, 0.6, 6200, 8600, 0.3}, 4541, -6.5));
    Save(d, "perc",   Mono(Cowbell(587, 845, 0.11, 0.4), -7.0));
    Save(d, "perc_2", Mono(Tom({220, 140, 0.02, 0.14, 0.4, 0.0, 0.3, 1.2}, 4552), -6.0));
    Save(d, "perc_3", Mono(Rim(2100, 1150, 0.15, 4553), -6.5));
}

void KCr78(const std::string& root) {
    const std::string d = root + "/cr78"; MakeDir(d);
    Save(d, "kick",   SoftMono(Kick({105, 70, 0.045, 0.2, 0.6, 0.2, 0.0, 2000, 0.0, 0.0}, 4601), 3200, -0.8));
    Save(d, "kick_2", SoftMono(Kick({120, 78, 0.03, 0.14, 0.45, 0.3, 0.1, 2000, 0.0, 0.0}, 4602), 3600, -0.8));
    Save(d, "snare",   SoftMono(Snare({310, 190, 0.06, 0.9, 1400, 3400, 0.10, 0.5, 0.35, 0.0, 0.3, 3000, 0.04}, 4611), 7500, -3.0));
    Save(d, "snare_2", SoftMono(Snare({340, 210, 0.035, 0.8, 1600, 3800, 0.06, 0.45, 0.22}, 4612), 7500, -3.5));
    Save(d, "clap",   StereoClap({2, 0.012, 0.006, 0.12, 1800, 1.0, 0.5, 700}, 4621, 0.4, -5.0));
    Save(d, "chh",   StereoHat({0.82, 0.016, 0.1, 5200, 6800, 0.5}, 4631, -10.0));
    Save(d, "chh_2", StereoHat({0.9, 0.009, 0.07, 5800, 7400, 0.55}, 4632, -10.0));
    Save(d, "ohh",   StereoHat({0.82, 0.11, 0.5, 4800, 6600, 0.5}, 4641, -7.0));
    Save(d, "perc",   Mono(Rim(2500, 1700, 0.12, 4651), -6.5));
    Save(d, "perc_2", SoftMono(Tom({420, 340, 0.02, 0.09, 0.3, 0.0, 0.2, 0.0}, 4652), 4500, -6.5));
    Save(d, "perc_3", Wide(Shaker(0.22, 4200, 4653), Shaker(0.22, 4200, 5653), -9.0));
}

void KDub(const std::string& root) {
    const std::string d = root + "/dub"; MakeDir(d);
    Save(d, "kick",   SoftMono(Kick({95, 40, 0.05, 0.5, 1.4, 0.5, 0.0, 1500, 0.0, 0.0}, 4701), 2500, -1.0));
    Save(d, "kick_2", SoftMono(Kick({110, 46, 0.035, 0.3, 0.9, 0.6, 0.1, 1800, 0.0, 0.0}, 4702), 3000, -1.0));
    Save(d, "snare",   Mono(Snare({200, 340, 0.09, 0.7, 1400, 3800, 0.14, 0.8, 0.6, 0.5, 0.35, 2400, 0.1}, 4711), -3.0));
    Save(d, "snare_2", Mono(Snare({230, 380, 0.05, 0.6, 1700, 4400, 0.08, 0.8, 0.35, 0.3, 0.2, 2600}, 4712), -3.5));
    Save(d, "clap",   StereoClap({3, 0.014, 0.005, 0.2, 900, 0.9, 1.0, 400}, 4721, 0.7, -4.5));
    Save(d, "chh",   StereoHat({0.9, 0.012, 0.08, 6000, 7600, 0.3}, 4731, -10.0));
    Save(d, "chh_2", StereoHat({1.0, 0.007, 0.06, 6800, 8400, 0.35}, 4732, -10.5));
    Save(d, "ohh",   StereoHat({0.9, 0.2, 0.9, 5400, 7400, 0.3}, 4741, -7.5));
    Save(d, "perc",   SoftMono(Tom({160, 105, 0.04, 0.3, 0.7, 0.0, 0.1, 0.0}, 4751), 4000, -6.0));
    Save(d, "perc_2", Mono(Rim(1500, 840, 0.22, 4752), -7.0));
    Save(d, "perc_3", SoftMono(Cowbell(430, 640, 0.13, 0.5), 5000, -8.0));
}

void KFunk(const std::string& root) {
    const std::string d = root + "/funk"; MakeDir(d);
    Save(d, "kick",   Mono(Kick({135, 68, 0.012, 0.085, 0.4, 0.6, 0.85, 2600, 0.0, 0.45}, 4801), -0.8));
    Save(d, "kick_2", Mono(Kick({120, 74, 0.016, 0.11, 0.45, 0.4, 0.7, 2200, 0.0, 0.4}, 4802), -0.8));
    Save(d, "snare",   Mono(Snare({215, 380, 0.05, 0.6, 1500, 4600, 0.10, 1.1, 0.35, 0.2, 0.12, 3400}, 4811), -2.0));
    Save(d, "snare_2", Mono(Snare({240, 420, 0.03, 0.5, 1800, 5000, 0.06, 1.0, 0.2}, 4812), -3.0));
    Save(d, "clap",   StereoClap({4, 0.0115, 0.004, 0.1, 1050, 1.0, 0.9, 450}, 4821, 0.4, -4.0));
    Save(d, "chh",   Wide(NoiseHatChannel(0.011, 0.1, 5600, 9000, 4831), NoiseHatChannel(0.011, 0.1, 5600, 9000, 5831), -9.0));
    Save(d, "chh_2", Wide(NoiseHatChannel(0.006, 0.07, 6400, 9800, 4832), NoiseHatChannel(0.006, 0.07, 6400, 9800, 5832), -9.5));
    Save(d, "ohh",   Wide(NoiseHatChannel(0.1, 0.55, 5000, 8600, 4841, 0.002), NoiseHatChannel(0.1, 0.55, 5000, 8600, 5841, 0.002), -6.5));
    Save(d, "perc",   Mono(Cowbell(560, 835, 0.11, 0.4), -7.5));
    Save(d, "perc_2", Mono(Tom({360, 250, 0.014, 0.1, 0.3, 0.0, 0.5, 0.0}, 4852), -6.0));
    Save(d, "perc_3", Wide(Shaker(0.2, 3800, 4853), Shaker(0.2, 3800, 5853), -8.0));
}

void KGabber(const std::string& root) {
    const std::string d = root + "/gabber"; MakeDir(d);
    Save(d, "kick",   Mono(Kick({320, 46, 0.026, 0.24, 0.6, 9.0, 0.5, 2600, 0.0, 0.3}, 4901), -0.3));
    Save(d, "kick_2", Mono(Kick({280, 52, 0.02, 0.17, 0.45, 12.0, 0.6, 3000, 0.0, 0.3}, 4902), -0.3));
    Save(d, "snare",   Mono(Snare({210, 360, 0.04, 0.7, 2000, 5000, 0.12, 1.2, 0.3, 0.0, 0.2, 3000}, 4911), -1.5));
    Save(d, "snare_2", Mono(Snare({240, 400, 0.03, 0.6, 2400, 5600, 0.07, 1.3, 0.2}, 4912), -2.0));
    Save(d, "clap",   StereoClap({5, 0.009, 0.0035, 0.07, 1500, 1.4, 0.7, 700}, 4921, 0.3, -3.0));
    Save(d, "chh",   StereoHat({1.2, 0.009, 0.06, 8000, 10000, 0.45}, 4931, -9.0));
    Save(d, "chh_2", StereoHat({1.3, 0.005, 0.045, 8600, 10800, 0.5}, 4932, -9.5));
    Save(d, "ohh",   StereoHat({1.2, 0.1, 0.4, 7400, 9800, 0.45}, 4941, -6.5));
    Save(d, "perc",   Mono(Tom({300, 150, 0.04, 0.2, 0.4, 0.5, 0.3, 5.0}, 4951), -5.5));
    Save(d, "perc_2", Mono(Tom({200, 100, 0.05, 0.25, 0.5, 0.5, 0.3, 6.0}, 4952), -5.5));
    Save(d, "perc_3", Mono(Rim(2400, 1300, 0.15, 4953), -6.0));
}

void KIndustrial(const std::string& root) {
    const std::string d = root + "/industrial"; MakeDir(d);
    Save(d, "kick",   Mono(Kick({190, 48, 0.03, 0.16, 0.5, 6.0, 0.8, 3400, 0.0, 0.7}, 5001), -0.6));
    Save(d, "kick_2", Mono(Kick({150, 56, 0.045, 0.22, 0.7, 4.0, 0.6, 2600, 0.0, 0.6}, 5002), -0.8));
    Save(d, "snare",   Mono(Snare({420, 710, 0.08, 0.9, 2400, 6200, 0.14, 0.9, 0.4, 0.2, 0.25, 4800, 0.06}, 5011), -2.5));
    Save(d, "snare_2", Mono(Snare({330, 590, 0.05, 0.7, 2800, 6800, 0.08, 1.1, 0.25}, 5012), -3.0));
    Save(d, "clap",   StereoClap({6, 0.0075, 0.003, 0.09, 1800, 1.8, 0.8, 800}, 5021, 0.35, -3.5));
    Save(d, "chh",   StereoHat({1.55, 0.011, 0.08, 9000, 11000, 0.6}, 5031, -9.0));
    Save(d, "chh_2", StereoHat({1.7, 0.006, 0.05, 9600, 11600, 0.65}, 5032, -9.5));
    Save(d, "ohh",   StereoHat({1.55, 0.15, 0.55, 8400, 10800, 0.6}, 5041, -6.5));
    Save(d, "perc",   Mono(Cowbell(1330, 1930, 0.35, 0.8), -8.0));
    Save(d, "perc_2", Mono(Cowbell(910, 1420, 0.2, 0.5), -8.0));
    Save(d, "perc_3", Mono(Rim(3100, 1750, 0.2, 5053), -6.0));
}

void KLinn(const std::string& root) {
    const std::string d = root + "/linn"; MakeDir(d);
    Save(d, "kick",   Mono(Soft(Crush(Kick({150, 62, 0.022, 0.12, 0.4, 0.9, 0.3, 2400, 0.0, 0.2}, 5101), 10, 4), 9000), -0.8));
    Save(d, "kick_2", Mono(Soft(Crush(Kick({130, 70, 0.018, 0.09, 0.3, 0.7, 0.4, 2200, 0.0, 0.2}, 5102), 10, 4), 9000), -0.8));
    Save(d, "snare",   Mono(Soft(Crush(Snare({195, 340, 0.05, 0.7, 1800, 5000, 0.1, 1.0, 0.3, 0.1, 0.1, 3200}, 5111), 10, 4), 10000), -2.5));
    Save(d, "snare_2", Mono(Soft(Crush(Snare({215, 370, 0.03, 0.5, 2000, 5400, 0.06, 1.0, 0.2}, 5112), 10, 4), 10000), -3.0));
    Save(d, "clap",   WideCrushed(ClapChannel({3, 0.0105, 0.004, 0.08, 1200, 1.1, 0.8, 500}, 5121, 0.35), ClapChannel({3, 0.0105, 0.004, 0.08, 1200, 1.1, 0.8, 500}, 9121, 0.35), 9, 4, 9000, -4.0));
    Save(d, "chh",   WideCrushed(HatChannel({1.0, 0.01, 0.07, 6600, 8600, 0.5}, 5131, 0.0), HatChannel({1.0, 0.01, 0.07, 6600, 8600, 0.5}, 6131, 1.0), 9, 4, 10000, -10.0));
    Save(d, "chh_2", WideCrushed(HatChannel({1.1, 0.006, 0.05, 7200, 9200, 0.5}, 5132, 0.0), HatChannel({1.1, 0.006, 0.05, 7200, 9200, 0.5}, 6132, 1.0), 9, 4, 10000, -10.5));
    Save(d, "ohh",   WideCrushed(HatChannel({1.0, 0.1, 0.45, 6200, 8400, 0.5}, 5141, 0.0), HatChannel({1.0, 0.1, 0.45, 6200, 8400, 0.5}, 6141, 1.0), 9, 4, 10000, -7.0));
    Save(d, "perc",   Mono(Soft(Crush(Tom({240, 170, 0.02, 0.12, 0.3, 0.0, 0.3, 0.3}, 5151), 10, 4), 8000), -6.0));
    Save(d, "perc_2", Mono(Soft(Crush(Tom({330, 240, 0.015, 0.1, 0.25, 0.0, 0.3, 0.3}, 5152), 10, 4), 8000), -6.5));
    Save(d, "perc_3", Mono(Soft(Crush(Cowbell(600, 880, 0.1, 0.35), 10, 4), 8000), -8.0));
}

void KLofi(const std::string& root) {
    const std::string d = root + "/lofi"; MakeDir(d);
    // dusty bit-reduced one-shots, rolled off above ~6 kHz, with a faint vinyl hiss bed under the tails
    auto dust = [](Vec v, uint64_t seed, double lvl) { Vec n = Noise(static_cast<double>(v.size()) / kGenRate, seed); n = Filter(n, 1, 1500, 0.7); n = Filter(n, 0, 6000, 0.7); Add(v, n, lvl); return v; };
    Save(d, "kick",   Mono(Soft(Crush(dust(Kick({110, 56, 0.03, 0.2, 0.55, 1.2, 0.2, 1800, 0.0, 0.1}, 5201), 5202, 0.004), 9, 6), 5200), -0.8));
    Save(d, "kick_2", Mono(Soft(Crush(dust(Kick({95, 52, 0.04, 0.27, 0.65, 0.8, 0.1, 1500, 0.0, 0.0}, 5203), 5204, 0.004), 9, 6), 4800), -0.8));
    Save(d, "snare",   Mono(Soft(Crush(dust(Snare({185, 320, 0.07, 0.7, 1300, 4000, 0.13, 0.9, 0.4, 0.2, 0.2, 2600}, 5211), 5212, 0.006), 9, 6), 5800), -3.0));
    Save(d, "snare_2", Mono(Soft(Crush(dust(Snare({205, 350, 0.04, 0.6, 1500, 4400, 0.08, 0.9, 0.25}, 5213), 5214, 0.006), 9, 6), 5800), -3.5));
    Save(d, "clap",   WideCrushed(ClapChannel({3, 0.013, 0.005, 0.12, 1000, 0.9, 0.9, 400}, 5221, 0.4), ClapChannel({3, 0.013, 0.005, 0.12, 1000, 0.9, 0.9, 400}, 9221, 0.4), 8, 6, 5500, -4.5));
    Save(d, "chh",   WideCrushed(NoiseHatChannel(0.012, 0.1, 4500, 7000, 5231), NoiseHatChannel(0.012, 0.1, 4500, 7000, 6231), 8, 6, 6000, -10.0));
    Save(d, "chh_2", WideCrushed(NoiseHatChannel(0.007, 0.07, 5200, 7600, 5232), NoiseHatChannel(0.007, 0.07, 5200, 7600, 6232), 8, 6, 6000, -10.5));
    Save(d, "ohh",   WideCrushed(NoiseHatChannel(0.11, 0.55, 4200, 7000, 5241, 0.002), NoiseHatChannel(0.11, 0.55, 4200, 7000, 6241, 0.002), 8, 6, 6000, -7.5));
    Save(d, "perc",   Mono(Soft(Crush(Tom({200, 130, 0.025, 0.16, 0.4, 0.0, 0.2, 0.2}, 5251), 9, 6), 4500), -6.0));
    Save(d, "perc_2", Mono(Soft(Crush(Rim(1400, 800, 0.18, 5252), 9, 6), 5000), -7.0));
    Save(d, "perc_3", Wide(Soft(Crush(Shaker(0.2, 3000, 5253), 8, 6), 6000), Soft(Crush(Shaker(0.2, 3000, 6253), 8, 6), 6000), -9.0));
}

void KMinimal(const std::string& root) {
    const std::string d = root + "/minimal"; MakeDir(d);
    Save(d, "kick",   Mono(Kick({150, 62, 0.012, 0.06, 0.25, 0.8, 0.5, 2800, 0.0, 0.2}, 5301), -0.8));
    Save(d, "kick_2", Mono(Kick({175, 70, 0.009, 0.045, 0.2, 1.2, 0.7, 3200, 0.0, 0.25}, 5302), -0.8));
    Save(d, "snare",   Mono(Snare({230, 400, 0.025, 0.5, 2600, 6000, 0.05, 0.9, 0.18}, 5311), -3.0));
    Save(d, "snare_2", Mono(Snare({260, 440, 0.018, 0.4, 3000, 6600, 0.035, 0.9, 0.14}, 5312), -3.5));
    Save(d, "clap",   StereoClap({2, 0.008, 0.0025, 0.035, 1900, 1.8, 0.5, 900}, 5321, 0.18, -4.5));
    Save(d, "chh",   StereoHat({1.1, 0.005, 0.035, 8800, 10400, 0.4}, 5331, -10.0));
    Save(d, "chh_2", StereoHat({1.25, 0.003, 0.025, 9400, 11200, 0.45}, 5332, -10.0));
    Save(d, "ohh",   StereoHat({1.1, 0.045, 0.22, 8200, 10200, 0.4}, 5341, -7.5));
    Save(d, "perc",   Mono(Rim(2700, 1500, 0.1, 5351), -6.5));
    Save(d, "perc_2", Mono(Tom({380, 300, 0.01, 0.06, 0.18, 0.0, 0.5, 0.0}, 5352), -7.0));
    Save(d, "perc_3", Mono(Cowbell(900, 1350, 0.045, 0.2), -9.0));
}

void KTrap808(const std::string& root) {
    const std::string d = root + "/trap808"; MakeDir(d);
    Save(d, "kick",   Mono(Kick({105, 38, 0.06, 0.65, 1.6, 1.6, 0.2, 1800, 0.0, 0.0}, 5401), -1.2));
    Save(d, "kick_2", Mono(Kick({140, 44, 0.035, 0.38, 1.0, 1.0, 0.3, 2200, 0.0, 0.0}, 5402), -1.0));
    Save(d, "snare",   Mono(Snare({200, 360, 0.05, 0.5, 2400, 6000, 0.1, 1.0, 0.3, 0.0, 0.2, 3000, 0.05}, 5411), -2.5));
    Save(d, "snare_2", Mono(Snare({230, 400, 0.03, 0.4, 2600, 6400, 0.07, 1.0, 0.22}, 5412), -3.0));
    Save(d, "clap",   StereoClap({4, 0.0095, 0.003, 0.13, 1300, 1.2, 1.0, 600}, 5421, 0.5, -3.5));
    Save(d, "chh",   StereoHat({1.1, 0.007, 0.05, 8400, 10200, 0.15}, 5431, -9.5));
    Save(d, "chh_2", StereoHat({1.25, 0.004, 0.035, 9000, 11000, 0.2}, 5432, -10.0));
    Save(d, "ohh",   StereoHat({1.1, 0.11, 0.5, 7800, 9800, 0.15}, 5441, -7.0));
    Save(d, "perc",   Mono(Rim(2100, 1150, 0.12, 5451), -6.5));
    Save(d, "perc_2", Mono(Tom({330, 205, 0.02, 0.16, 0.4, 0.0, 0.1, 0.0}, 5452), -6.5));
    Save(d, "perc_3", Mono(Cowbell(540, 800, 0.1, 0.4), -8.0));
}

} // namespace

int main(int argc, char** argv) {
    std::string root = "kits";
    bool report = true;
    for (int i = 1; i < argc; ++i) { std::string a = argv[i]; if (a == "--quiet") report = false; else root = a; }
    MakeDir(root);
    Electro(root); Techno(root); Breaks(root); AcidV2(root);
    K606(root); K707(root); K808(root); K909(root); KAcid(root); KCr78(root); KDub(root); KFunk(root); KGabber(root);
    KIndustrial(root); KLinn(root); KLofi(root); KMinimal(root); KTrap808(root);
    if (report) {
        std::printf("%-26s %8s %8s %6s %6s\n", "file", "peak dB", "rms dB", "sec", "L/R");
        for (const auto& r : g_report) std::printf("%-26s %8.1f %8.1f %6.2f %6.2f\n", r.name.c_str(), r.peakDb, r.rmsDb, r.sec, r.corr);
    }
    return 0;
}
