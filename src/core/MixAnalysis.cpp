#include "MixAnalysis.h"
#include "TrackEq.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>

namespace t8 {

namespace {

constexpr int kFftN = 8192, kFftHop = 4096;
constexpr double kPi = 3.14159265358979323846;

void Fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v; a[i + k + len / 2] = u - v; w *= wl;
            }
        }
    }
}

// power spectrum of one Hann frame (bins 0..N/2), scaled so a full-scale sine reads 1.0 (0 dB)
void FramePower(const float* x, std::vector<std::complex<double>>& buf, const std::vector<double>& win, double winSum, std::vector<double>& pw) {
    for (int i = 0; i < kFftN; ++i) buf[static_cast<size_t>(i)] = std::complex<double>(x[i] * win[static_cast<size_t>(i)], 0.0);
    Fft(buf);
    pw.resize(kFftN / 2 + 1);
    const double sc = 4.0 / (winSum * winSum);
    for (int k = 0; k <= kFftN / 2; ++k) pw[static_cast<size_t>(k)] = std::norm(buf[static_cast<size_t>(k)]) * sc;
}

// FFT-bin power -> log bands (power, not dB). Bands narrower than a bin read the interpolated density instead of staying empty.
void BinsToBands(const std::vector<double>& pw, int rate, std::array<double, kMixBands>& band) {
    const double binHz = static_cast<double>(rate) / kFftN;
    for (int b = 0; b < kMixBands; ++b) {
        const double lo = MixBandEdgeHz(static_cast<float>(b)), hi = MixBandEdgeHz(static_cast<float>(b + 1));
        const int k0 = static_cast<int>(std::ceil(lo / binHz)), k1 = static_cast<int>(std::ceil(hi / binHz)) - 1;
        double sum = 0.0;
        if (k1 >= k0) { for (int k = k0; k <= std::min(k1, kFftN / 2); ++k) sum += pw[static_cast<size_t>(k)]; }
        else {
            const double c = 0.5 * (lo + hi) / binHz; const int k = std::clamp(static_cast<int>(c), 0, kFftN / 2 - 1);
            const double fr = c - k; const double d = pw[static_cast<size_t>(k)] * (1.0 - fr) + pw[static_cast<size_t>(k + 1)] * fr;
            sum = d * (hi - lo) / binHz;
        }
        band[static_cast<size_t>(b)] = sum;
    }
}

inline float ToDb(double p) { return static_cast<float>(10.0 * std::log10(p + 1e-12)); }
inline double FromDb(float d) { return std::pow(10.0, static_cast<double>(d) / 10.0); }

int ZoneOf(float hz) { for (int z = 0; z < kMixZones; ++z) if (hz < kMixZoneEdges[z + 1]) return z; return kMixZones - 1; }

std::string Num(float v, const char* fmt) { char b[32]; std::snprintf(b, sizeof(b), fmt, static_cast<double>(v)); return b; }
std::string HzText(float hz) { return hz >= 1000.0f ? Num(hz / 1000.0f, "%.1fk") : Num(std::round(hz), "%.0f"); }

}  // namespace

float MixBandEdgeHz(float b) { return kMixLoHz * std::pow(kMixHiHz / kMixLoHz, b / static_cast<float>(kMixBands)); }
float MixHzToBand(float hz) { return std::log(std::max(hz, 1.0f) / kMixLoHz) / std::log(kMixHiHz / kMixLoHz) * static_cast<float>(kMixBands); }

std::array<int, kNumTracks> DefaultMixPriority() {
    // Kick, Bass, Snare, Chord, Synth2, Clap, Perc, Synth3, Sampler, C.Hat, O.Hat
    return {0, 6, 1, 9, 7, 2, 5, 8, 10, 3, 4};
}

void AnalyseStems(const std::array<std::vector<float>, kNumTracks>& stems, int rate, MixSpectra& out) {
    out = MixSpectra();
    out.rate = rate;
    std::vector<double> win(kFftN); double winSum = 0.0;
    for (int i = 0; i < kFftN; ++i) { win[static_cast<size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / kFftN); winSum += win[static_cast<size_t>(i)]; }
    std::vector<std::complex<double>> buf(kFftN);
    std::vector<double> pw;
    std::array<double, kMixBands> mixPow{}; size_t mixLen = 0;
    for (const auto& s : stems) mixLen = std::max(mixLen, s.size());
    double mixSq = 0.0;

    for (int t = 0; t < kNumTracks; ++t) {
        const auto& x = stems[static_cast<size_t>(t)];
        double peak = 0.0;
        for (float v : x) peak = std::max(peak, static_cast<double>(std::fabs(v)));
        if (x.size() < static_cast<size_t>(kFftN) || peak < 2e-4) continue;
        out.present[static_cast<size_t>(t)] = true;
        // frame energies first, to find the frames where the track really plays
        const int frames = static_cast<int>((x.size() - kFftN) / kFftHop) + 1;
        std::vector<double> en(static_cast<size_t>(frames), 0.0); double enMax = 0.0;
        for (int f = 0; f < frames; ++f) {
            const float* p = x.data() + static_cast<size_t>(f) * kFftHop; double e = 0.0;
            for (int i = 0; i < kFftN; ++i) e += static_cast<double>(p[i]) * p[i];
            en[static_cast<size_t>(f)] = e; enMax = std::max(enMax, e);
        }
        std::array<double, kMixBands> acc{}; int used = 0;
        for (int f = 0; f < frames; ++f) {
            if (en[static_cast<size_t>(f)] < 0.1 * enMax) continue;
            FramePower(x.data() + static_cast<size_t>(f) * kFftHop, buf, win, winSum, pw);
            std::array<double, kMixBands> bp{}; BinsToBands(pw, rate, bp);
            for (int b = 0; b < kMixBands; ++b) acc[static_cast<size_t>(b)] += bp[static_cast<size_t>(b)];
            ++used;
        }
        if (used == 0) { out.present[static_cast<size_t>(t)] = false; continue; }
        for (int b = 0; b < kMixBands; ++b) {
            const double p = acc[static_cast<size_t>(b)] / used;
            out.db[static_cast<size_t>(t)][static_cast<size_t>(b)] = ToDb(p);
            mixPow[static_cast<size_t>(b)] += p;
        }
    }
    for (int b = 0; b < kMixBands; ++b) out.mixDb[static_cast<size_t>(b)] = ToDb(mixPow[static_cast<size_t>(b)]);
    for (size_t i = 0; i < mixLen; ++i) { double v = 0.0; for (const auto& s : stems) if (i < s.size()) v += s[i]; mixSq += v * v; }
    if (mixLen) out.mixRmsDb = static_cast<float>(10.0 * std::log10(mixSq / static_cast<double>(mixLen) + 1e-12));
}

bool MixApplyFix(Track& t, const MixFix& f) {
    switch (f.kind) {
        case MixFix::Kind::None: return false;
        case MixFix::Kind::Hpf: t.eqHpfHz = std::clamp(f.hz, 20.0f, 500.0f); return true;
        case MixFix::Kind::LowShelfCut: t.eqLowHz = std::clamp(f.hz, 30.0f, 500.0f); t.eqLowDb = std::clamp(f.value, -kEqMaxDb, kEqMaxDb); return true;
        case MixFix::Kind::BellCut: t.eqMidHz = std::clamp(f.hz, 80.0f, 10000.0f); t.eqMidQ = std::clamp(f.q, 0.3f, 6.0f); t.eqMidDb = std::clamp(f.value, -kEqMaxDb, kEqMaxDb); return true;
        case MixFix::Kind::HighShelfCut: t.eqHighHz = std::clamp(f.hz, 1500.0f, 16000.0f); t.eqHighDb = std::clamp(f.value, -kEqMaxDb, kEqMaxDb); return true;
        case MixFix::Kind::Level: t.volume = std::clamp(t.volume * static_cast<float>(std::pow(10.0, static_cast<double>(f.value) / 20.0)), 0.0f, 1.2f); return true;
        case MixFix::Kind::Pan: t.pan = std::clamp(f.value, -1.0f, 1.0f); return true;
    }
    return false;
}

MixReport BuildMixReport(const MixSpectra& sp, const std::array<int, kNumTracks>& priority, const Pattern* pat) {
    MixReport rep;
    std::array<int, kNumTracks> rank{};
    for (int i = 0; i < kNumTracks; ++i) rank[static_cast<size_t>(std::clamp(priority[static_cast<size_t>(i)], 0, kNumTracks - 1))] = i;

    // ---- smooth (about a third of an octave, in power) and find the overall level
    std::array<std::array<float, kMixBands>, kNumTracks> sm{};
    float ref = -120.0f;
    for (int t = 0; t < kNumTracks; ++t) {
        for (int b = 0; b < kMixBands; ++b) {
            double s = 0.0; int n = 0;
            for (int k = -2; k <= 2; ++k) { const int bb = b + k; if (bb < 0 || bb >= kMixBands) continue; s += FromDb(sp.db[static_cast<size_t>(t)][static_cast<size_t>(bb)]); ++n; }
            sm[static_cast<size_t>(t)][static_cast<size_t>(b)] = sp.present[static_cast<size_t>(t)] ? ToDb(s / std::max(1, n)) : -120.0f;
            ref = std::max(ref, sm[static_cast<size_t>(t)][static_cast<size_t>(b)]);
        }
    }
    if (ref < -100.0f) { rep.valid = false; return rep; }
    rep.valid = true;

    // ---- per-band contest: how many instruments are within 8 dB of the loudest one, and how loud is that spot compared with the mix's loudest
    std::array<float, kMixBands> sev{};
    std::array<std::array<bool, kMixBands>, kNumTracks> sig{};
    for (int b = 0; b < kMixBands; ++b) {
        float top = -120.0f, second = -120.0f;
        for (int t = 0; t < kNumTracks; ++t) {
            const float v = sm[static_cast<size_t>(t)][static_cast<size_t>(b)];
            if (v > top) { second = top; top = v; } else if (v > second) second = v;
        }
        for (int t = 0; t < kNumTracks; ++t) sig[static_cast<size_t>(t)][static_cast<size_t>(b)] = sm[static_cast<size_t>(t)][static_cast<size_t>(b)] > top - 8.0f && sm[static_cast<size_t>(t)][static_cast<size_t>(b)] > ref - 40.0f;
        const float close = std::clamp(1.0f - (top - second) / 8.0f, 0.0f, 1.0f);
        const float loud = std::clamp((top - (ref - 40.0f)) / 22.0f, 0.0f, 1.0f);
        sev[static_cast<size_t>(b)] = (second > ref - 40.0f) ? close * loud : 0.0f;
    }

    // ---- contiguous contested bands (one-band gaps allowed) become spots
    struct Run { int b0, b1; };
    std::vector<Run> runs;
    for (int b = 0; b < kMixBands; ++b) {
        if (sev[static_cast<size_t>(b)] < 0.35f) continue;
        if (!runs.empty() && b - runs.back().b1 <= 2) runs.back().b1 = b; else runs.push_back({b, b});
    }
    for (const Run& r : runs) {
        if (r.b1 - r.b0 + 1 < 2) continue;
        MixSpot sp2;
        sp2.loHz = MixBandEdgeHz(static_cast<float>(r.b0)); sp2.hiHz = MixBandEdgeHz(static_cast<float>(r.b1 + 1));
        sp2.centerHz = std::sqrt(sp2.loHz * sp2.hiHz);
        sp2.zone = ZoneOf(sp2.centerHz);
        float mean = 0.0f, mx = 0.0f;
        for (int b = r.b0; b <= r.b1; ++b) { mean += sev[static_cast<size_t>(b)]; mx = std::max(mx, sev[static_cast<size_t>(b)]); }
        mean /= static_cast<float>(r.b1 - r.b0 + 1);
        const float width = static_cast<float>(r.b1 - r.b0 + 1) / 12.0f;   // octaves
        sp2.severity = std::clamp((0.5f * mean + 0.5f * mx) * (0.6f + 0.4f * std::min(1.0f, width)), 0.0f, 1.0f);
        std::vector<std::pair<float, int>> who;
        for (int t = 0; t < kNumTracks; ++t) {
            int cnt = 0; float lv = 0.0f;
            for (int b = r.b0; b <= r.b1; ++b) if (sig[static_cast<size_t>(t)][static_cast<size_t>(b)]) { ++cnt; lv += sm[static_cast<size_t>(t)][static_cast<size_t>(b)]; }
            if (cnt * 2 >= r.b1 - r.b0 + 1) who.push_back({lv / static_cast<float>(cnt), t});
        }
        if (who.size() < 2) continue;
        std::sort(who.begin(), who.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& w : who) sp2.tracks.push_back(w.second);
        rep.spots.push_back(std::move(sp2));
    }
    std::sort(rep.spots.begin(), rep.spots.end(), [](const MixSpot& a, const MixSpot& b) { return a.severity > b.severity; });
    {   // v44.45: the same instruments with the same advised change are one fight, not two cards (kick and bass showed twice, in MUD and in SUB)
        std::vector<MixSpot> kept;
        for (MixSpot& sp3 : rep.spots) {
            MixSpot* twin = nullptr;
            std::vector<int> b = sp3.tracks; std::sort(b.begin(), b.end());
            for (MixSpot& k : kept) {
                std::vector<int> a = k.tracks; std::sort(a.begin(), a.end());
                if (a == b && k.fix.kind == sp3.fix.kind && k.fix.track == sp3.fix.track && std::fabs(k.fix.value - sp3.fix.value) < 0.01f) { twin = &k; break; }
            }
            if (twin) {
                char note[96]; std::snprintf(note, sizeof note, " The same two also collide at %.0f-%.0f Hz.", static_cast<double>(sp3.loHz), static_cast<double>(sp3.hiHz));
                twin->advice += note;
            } else kept.push_back(std::move(sp3));
        }
        rep.spots = std::move(kept);
    }
    if (rep.spots.size() > 5) rep.spots.resize(5);

    // ---- advice
    for (MixSpot& s : rep.spots) {
        std::vector<int> byRank = s.tracks;
        std::sort(byRank.begin(), byRank.end(), [&](int a, int b) { return rank[static_cast<size_t>(a)] < rank[static_cast<size_t>(b)]; });
        s.keeper = byRank.front(); s.yielder = byRank.back();
        const int ky = s.keeper, yi = s.yielder;
        const std::string kn = MixTrackName(ky), yn = MixTrackName(yi);
        const bool lowTrack = yi == 0 || yi == 6;
        const bool hats = (ky == 3 || ky == 4) && (yi == 3 || yi == 4);
        const Track* yt = pat ? &pat->tracks[static_cast<size_t>(yi)] : nullptr;
        const bool midFree = !yt || std::fabs(yt->eqMidDb) < 0.5f;
        const bool lowFree = !yt || std::fabs(yt->eqLowDb) < 0.5f;
        const bool highFree = !yt || std::fabs(yt->eqHighDb) < 0.5f;
        const float cutDb = std::round((2.0f + 3.0f * s.severity) * 2.0f) / 2.0f;
        MixFix f; f.track = yi;
        const std::string head = yn + " > ";
        const std::string rng = HzText(s.loHz) + "-" + HzText(s.hiHz) + " Hz";
        std::string who2 = kn + " and " + yn;
        if (s.tracks.size() > 2) who2 = kn + ", " + yn + " and " + std::to_string(s.tracks.size() - 2) + " more";

        if (hats) {
            const float other = pat ? pat->tracks[static_cast<size_t>(ky)].pan : 0.0f;
            const float target = other > 0.05f ? -0.3f : 0.3f;
            f.kind = MixFix::Kind::Pan; f.value = target;
            f.text = head + "PAN " + (target < 0 ? "L " : "R ") + std::to_string(static_cast<int>(std::fabs(target) * 100.0f + 0.5f));
            s.title = "Both hats are bright";
            s.advice = who2 + " fill the same top. Move them apart: " + f.text + " - and shorten the open hat so it does not smear.";
        } else if (s.centerHz < 250.0f && !lowTrack && s.hiHz < 400.0f && yi != 1 && yi != 2) {
            f.kind = MixFix::Kind::Hpf; f.hz = std::clamp(s.hiHz * 0.9f, 60.0f, 300.0f);
            f.text = head + "EQ > HPF " + HzText(f.hz) + " Hz";
            s.title = s.centerHz < 90.0f ? "The sub is shared" : "The low end is shared";
            s.advice = who2 + " both want the bottom (" + rng + "), so they pump into each other. Let " + kn + " own it: " + f.text + ".";
        } else if (s.centerHz < 90.0f) {
            // kick vs bass or a drum in the sub: shelf the yielder down below the zone's top edge
            if (lowFree) { f.kind = MixFix::Kind::LowShelfCut; f.hz = std::clamp(s.hiHz, 40.0f, 200.0f); f.value = -cutDb; f.text = head + "EQ > LOW " + Num(-cutDb, "%.1f") + " dB at " + HzText(f.hz) + " Hz"; }
            else { f.kind = MixFix::Kind::Level; f.value = -2.0f; f.text = head + "LEVEL -2 dB"; }
            s.title = "The sub is shared";
            s.advice = who2 + " both want the lowest octave (" + rng + "). Let " + kn + " own it: " + f.text + ".";
        } else if (s.centerHz > 6000.0f) {
            if (highFree) { f.kind = MixFix::Kind::HighShelfCut; f.hz = std::clamp(s.loHz, 3000.0f, 12000.0f); f.value = -cutDb; f.text = head + "EQ > HIGH " + Num(-cutDb, "%.1f") + " dB at " + HzText(f.hz) + " Hz"; }
            else { f.kind = MixFix::Kind::Level; f.value = -2.0f; f.text = head + "LEVEL -2 dB"; }
            s.title = "The top is crowded";
            s.advice = who2 + " pile up in the air band (" + rng + "). Take some from the less important one: " + f.text + ".";
        } else {
            if (midFree) {
                f.kind = MixFix::Kind::BellCut; f.hz = s.centerHz; f.value = -cutDb;
                f.q = std::clamp(s.centerHz / std::max(20.0f, s.hiHz - s.loHz) * 0.8f, 0.8f, 4.0f);
                f.text = head + "EQ > MID " + Num(-cutDb, "%.1f") + " dB at " + HzText(f.hz) + " Hz";
            } else { f.kind = MixFix::Kind::Level; f.value = -2.0f; f.text = head + "LEVEL -2 dB"; }
            const int z = s.zone;
            s.title = z == 2 ? "Mud" : z == 4 ? "Presence: two things shout" : z == 1 ? "The low end collides" : "The midrange is crowded";
            s.advice = who2 + (z == 2 ? " stack up here (" : " fight for the same room (") + rng + "), and the mix gets cloudy. Cut a little from the less important one: " + f.text + ".";
        }
        s.fix = f;
        rep.overlap += s.severity;
    }

    // ---- tonal balance: energy per octave of the summed mix, against a typical track
    std::array<double, kMixZones> zp{};
    std::array<std::array<double, kNumTracks>, kMixZones> tz{};
    for (int b = 0; b < kMixBands; ++b) {
        const float c = std::sqrt(MixBandEdgeHz(static_cast<float>(b)) * MixBandEdgeHz(static_cast<float>(b + 1)));
        const int z = ZoneOf(c);
        for (int t = 0; t < kNumTracks; ++t) { if (!sp.present[static_cast<size_t>(t)]) continue; const double p = FromDb(sp.db[static_cast<size_t>(t)][static_cast<size_t>(b)]); tz[static_cast<size_t>(z)][static_cast<size_t>(t)] += p; zp[static_cast<size_t>(z)] += p; }
    }
    std::array<float, kMixZones> perOct{};
    for (int z = 0; z < kMixZones; ++z) {
        const double oct = std::log2(static_cast<double>(kMixZoneEdges[z + 1]) / kMixZoneEdges[z]);
        perOct[static_cast<size_t>(z)] = ToDb(zp[static_cast<size_t>(z)] / oct);
        for (int t = 0; t < kNumTracks; ++t) rep.balance.share[static_cast<size_t>(z)][static_cast<size_t>(t)] = zp[static_cast<size_t>(z)] > 0.0 ? static_cast<float>(tz[static_cast<size_t>(z)][static_cast<size_t>(t)] / zp[static_cast<size_t>(z)]) : 0.0f;
    }
    for (int z = 0; z < kMixZones; ++z) {
        rep.balance.deltaDb[static_cast<size_t>(z)] = (perOct[static_cast<size_t>(z)] - perOct[3]) - kMixTypicalDb[z];
        rep.balance.off[static_cast<size_t>(z)] = std::fabs(rep.balance.deltaDb[static_cast<size_t>(z)]) > kMixToleranceDb && z != 3;
    }
    return rep;
}

} // namespace t8
