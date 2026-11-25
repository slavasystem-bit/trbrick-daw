// t8_audit: measures the engine's own DSP quality and prints a report of numbers.
//   t8_audit [--kit kits/909] [--out baseline.txt] [--compare baseline.txt]
// Every line is "key = value unit". --out saves them as key=value, --compare prints the change
// against a saved run. This is the yardstick for the audio roadmap (AUDIO_AUDIT_AND_PLAN.md): run it
// before and after each DSP change. Run from the repo root so kits/ resolves.
#include "core/SubtractiveSynth.h"
#include "core/SynthFilter.h"
#include "core/DrumEngine.h"
#include "core/Effects.h"
#include "core/MasterChain.h"
#include "core/Limiter.h"
#include "core/Dsp.h"
#include "core/Stereo.h"
#include "core/Mixer.h"
#include "core/Sequencer.h"
#include "core/AudioEngine.h"
#include "core/DrumBreaks.h"
#include "AudioMetrics.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace t8;
namespace {

constexpr int kSr = 44100;
std::vector<std::pair<std::string, double>> g_rows;
std::vector<std::string> g_units;

void Row(const std::string& key, double v, const char* unit) { g_rows.emplace_back(key, v); g_units.emplace_back(unit); }
double Hz(int midi) { return 440.0 * std::pow(2.0, (midi - 69) / 12.0); }
std::vector<float> Mono(const std::vector<float>& st) { std::vector<float> m(st.size() / 2); for (size_t i = 0; i < m.size(); ++i) m[i] = st[2 * i]; return m; }

// level of everything that is not a harmonic of f0 (aliasing + noise), relative to the harmonics
double InharmonicDb(const std::vector<float>& x, double f0) {
    const size_t N = 65536, start = 8192;
    auto p = t8m::PowerSpectrum(x, start, N);
    std::vector<char> harm(p.size(), 0);
    for (int k = 1; k * f0 < kSr / 2.0; ++k) {
        const long c = std::lround(k * f0 * N / kSr);
        for (long b = c - 4; b <= c + 4; ++b) if (b >= 0 && b < static_cast<long>(p.size())) harm[b] = 1;
    }
    double h = 0, o = 0; for (size_t i = 8; i < p.size(); ++i) (harm[i] ? h : o) += p[i];
    return 10.0 * std::log10(std::max(o, 1e-30) / std::max(h, 1e-30));
}

std::vector<float> RenderSynth(int note, float cutoff, float res, bool f24, int seconds = 2) {
    SubtractiveSynth s; SynthPatch p;
    p.waveform = Waveform::Saw; p.cutoffHz = cutoff; p.resonance = res; p.envAmount = 0; p.attack = 0.001f; p.decay = 0.01f;
    p.sustain = 1; p.release = 0.05f; p.volume = 1; p.filter24 = f24; p.subLevel = 0; p.noiseLevel = 0; p.drive = 0;
    s.SetPatch(p); s.NoteOn(note, 1.0f, false);
    std::vector<float> st(static_cast<size_t>(kSr) * seconds * 2, 0.0f); s.Render(st.data(), kSr * seconds, kSr);
    return Mono(st);
}

void AuditSynth() {
    for (int note : {79, 84, 96}) {   // G5, C6, C7: aliasing grows with pitch
        auto x = RenderSynth(note, 18000, 0, false);
        char k[64]; std::snprintf(k, sizeof k, "synth.saw_n%d.inharmonic", note);
        Row(k, InharmonicDb(x, Hz(note)), "dB re harmonics");
    }
    // how much of the top end survives as the cutoff goes up (the SVF clamps near 7 kHz)
    for (float c : {5000.f, 8000.f, 12000.f, 18000.f}) {
        auto x = RenderSynth(36, c, 0, false);
        auto p = t8m::PowerSpectrum(x, 8192, 65536);
        char k[64]; std::snprintf(k, sizeof k, "synth.c2_cut%d.band_8k_16k", static_cast<int>(c));
        Row(k, 10.0 * std::log10(std::max(t8m::BandEnergy(p, 8000, 16000, kSr, 65536), 1e-30) / std::max(t8m::BandEnergy(p, 40, 8000, kSr, 65536), 1e-30)), "dB re <8k");
    }
    auto r0 = RenderSynth(36, 2000, 0.0f, false), r9 = RenderSynth(36, 2000, 0.9f, false);
    double m0 = 0, m9 = 0; for (float v : r0) m0 = std::max<double>(m0, std::fabs(v)); for (float v : r9) m9 = std::max<double>(m9, std::fabs(v));
    Row("synth.resonance_peak_gain", t8m::ToDb(m9 / std::max(m0, 1e-9)), "dB (res .9 vs 0)");
}


// ---- v37 synth filter: measured on the filter alone at the rate the synth runs it (2 x 48 kHz) ------------------
void AuditSynthFilter() {
    const float sr2 = 2.0f * kSr;
    auto gainDb = [&](SynthFilter::Mode m, float fc, float res, float f) {   // steady-state gain of a sine at f
        SynthFilter flt; flt.Coeffs(m, fc, res, sr2);
        const int n = static_cast<int>(sr2 * 0.25f), skip = n / 2; double a = 0, b = 0;
        for (int i = 0; i < n; ++i) { const float x = 0.1f * std::sin(6.2831853f * f * i / sr2); const float y = flt.Process(x); if (i >= skip) { a += double(y) * y; b += double(x) * x; } }
        return 10.0 * std::log10(std::max(a, 1e-30) / std::max(b, 1e-30));
    };
    struct M { const char* name; SynthFilter::Mode m; };
    const M modes[] = {{"pole2", SynthFilter::Mode::Pole2}, {"ladder", SynthFilter::Mode::Ladder}, {"acid", SynthFilter::Mode::Acid}};
    for (const M& m : modes) {
        char k[96];
        // slope between 2 and 4 octaves above the cutoff (plain, res 0)
        const double s = (gainDb(m.m, 1000, 0.0f, 8000) - gainDb(m.m, 1000, 0.0f, 4000));
        std::snprintf(k, sizeof k, "filter.%s.slope_per_octave", m.name); Row(k, s, "dB/oct (3 -> 4 oct above fc)");
        // the resonant peak sits at the cutoff, also near the top of the range
        for (float fc : {1000.f, 4000.f, 10000.f, 16000.f}) {
            double bestG = -1e9; float bestF = 0;
            for (float f = fc * 0.6f; f <= std::min(fc * 1.5f, 0.46f * sr2); f *= 1.01f) { const double g = gainDb(m.m, fc, 0.8f, f); if (g > bestG) { bestG = g; bestF = f; } }
            std::snprintf(k, sizeof k, "filter.%s.res0.8_peak_hz_fc%d", m.name, static_cast<int>(fc)); Row(k, bestF, "Hz (should be near fc)");
        }
        // self-oscillation: kick the filter with a click, then listen with no input
        for (float fc : {200.f, 1000.f, 4000.f, 12000.f}) {
            SynthFilter flt; flt.Coeffs(m.m, fc, 0.99f, sr2);
            const int n = static_cast<int>(sr2 * 1.0f); std::vector<float> y(n);
            for (int i = 0; i < n; ++i) y[i] = flt.Process(i < 4 ? 0.5f : 0.0f);
            std::vector<float> tail(y.begin() + n / 2, y.end());
            double rms = 0; for (float v : tail) rms += double(v) * v; rms = std::sqrt(rms / tail.size());
            // frequency by zero crossings over the tail
            int zc = 0; for (size_t i = 1; i < tail.size(); ++i) if (tail[i - 1] < 0 && tail[i] >= 0) ++zc;
            const double hz = zc / (tail.size() / double(sr2));
            std::snprintf(k, sizeof k, "filter.%s.selfosc_hz_fc%d", m.name, static_cast<int>(fc)); Row(k, hz, "Hz (res .99, no input)");
            std::snprintf(k, sizeof k, "filter.%s.selfosc_level_fc%d", m.name, static_cast<int>(fc)); Row(k, t8m::ToDb(rms), "dBFS rms");
        }
    }
    {   // half-band decimator: a tone just above the new Nyquist must not fold back
        HalfbandDecimator d; double a = 0, b = 0;
        for (int i = 0; i < 20000; ++i) {
            const float x1 = std::sin(6.2831853f * 30000.f * (2 * i) / sr2) , x2 = std::sin(6.2831853f * 30000.f * (2 * i + 1) / sr2);
            d.Push(x1); d.Push(x2); const float y = d.Out(); if (i > 100) { a += double(y) * y; b += 0.5; }
        }
        Row("filter.halfband.rejection_30k", 10.0 * std::log10(std::max(a, 1e-30) / b), "dB (tone at 30 kHz of 96 kHz)");
        double c = 0, e = 0;
        for (int i = 0; i < 20000; ++i) {
            const float x1 = std::sin(6.2831853f * 15000.f * (2 * i) / sr2), x2 = std::sin(6.2831853f * 15000.f * (2 * i + 1) / sr2);
            d.Push(x1); d.Push(x2); const float y = d.Out(); if (i > 100) { c += double(y) * y; e += 0.5; }
        }
        Row("filter.halfband.passband_15k", 10.0 * std::log10(std::max(c, 1e-30) / e), "dB (tone at 15 kHz)");
    }
    {   // ACID: accent opens the filter further, consecutive accents more so; drift moves pitch a little
        auto brightness = [&](bool accentFirst, int accents) {
            SubtractiveSynth sy; SynthPatch p; p.waveform = Waveform::Saw; p.acid = true; p.cutoffHz = 400; p.resonance = 0.6f; p.envAmount = 0.6f;
            p.filterDecay = 0.25f; p.attack = 0.001f; p.decay = 0.5f; p.sustain = 1; p.release = 0.05f; p.volume = 1;
            sy.SetPatch(p); std::vector<float> st;
            for (int n = 0; n < accents + 1; ++n) {
                const bool acc = (n == accents) ? accentFirst : true;
                sy.NoteOn(36, acc ? 1.0f : 0.8f, false);
                std::vector<float> b(static_cast<size_t>(kSr) / 8 * 2, 0.f); sy.Render(b.data(), kSr / 8, kSr); if (n == accents) st = b; }
            auto m = Mono(st); auto pw = t8m::PowerSpectrum(m, 0, 4096);
            return t8m::SpectralCentroid(pw, kSr, 4096);
        };
        Row("acid.centroid_plain", brightness(false, 0), "Hz");
        Row("acid.centroid_accent", brightness(true, 0), "Hz");
        Row("acid.centroid_accent_after_2_accents", brightness(true, 2), "Hz");
    }
    {   // drift: pitch wander of a sine at DRIFT 1 (cents rms between 50 ms windows)
        SubtractiveSynth sy; SynthPatch p; p.waveform = Waveform::Sine; p.cutoffHz = 12000; p.envAmount = 0; p.attack = 0.001f; p.sustain = 1; p.volume = 1; p.drift = 1.0f;
        sy.SetPatch(p); sy.NoteOn(69, 1.0f, false);
        std::vector<float> st(static_cast<size_t>(kSr) * 8 * 2, 0.f); sy.Render(st.data(), kSr * 8, kSr); auto m = Mono(st);
        std::vector<double> cents; const size_t w = kSr / 4;
        for (size_t at = kSr / 2; at + w < m.size(); at += w) {
            int zc = 0; double first = -1, last = -1;
            for (size_t i = at + 1; i < at + w; ++i) if (m[i - 1] < 0 && m[i] >= 0) { const double fr = (i - 1) + (-m[i - 1]) / double(m[i] - m[i - 1]); if (first < 0) first = fr; last = fr; ++zc; }
            if (zc > 2) cents.push_back(1200.0 * std::log2((zc - 1) / ((last - first) / kSr) / 440.0));
        }
        double mean = 0; for (double c : cents) mean += c; mean /= std::max<size_t>(1, cents.size());
        double var = 0; for (double c : cents) var += (c - mean) * (c - mean);
        Row("synth.drift1_pitch_cents_rms", std::sqrt(var / std::max<size_t>(1, cents.size())), "cents");
    }
}

void AuditDrums(const std::string& kitDir) {
    DrumKit kit; if (kit.LoadFromFolder(kitDir, kSr) < kNumDrumTracks) { std::fprintf(stderr, "kit %s incomplete\n", kitDir.c_str()); return; }
    auto render = [&](DrumSlot sl, float tune, float decay) {
        DrumEngine e; e.SetKit(&kit); DrumVoiceParams vp; vp.tune = tune; vp.decay = decay; vp.level = 1.0f; e.Trigger(sl, 1.0f, 0, vp);
        std::vector<float> st(static_cast<size_t>(kSr) * 2 * 2, 0.0f); e.RenderSlot(sl, st.data(), kSr * 2, kSr); return Mono(st);
    };
    auto tail40 = [&](const std::vector<float>& x) {   // seconds until the 20 ms RMS falls below -40 dB of its peak
        const size_t w = kSr / 50; double pk = 0; std::vector<double> e;
        for (size_t i = 0; i + w < x.size(); i += w / 2) { double a = 0; for (size_t j = 0; j < w; ++j) a += double(x[i + j]) * x[i + j]; e.push_back(std::sqrt(a / w)); pk = std::max(pk, e.back()); }
        for (size_t i = 0; i < e.size(); ++i) if (e[i] < pk * 0.01 && i > 2) return i * (w / 2) / double(kSr);
        return x.size() / double(kSr);
    };
    for (auto [name, sl] : {std::pair<const char*, DrumSlot>{"kick", DrumSlot::Kick}, {"snare", DrumSlot::Snare}, {"chh", DrumSlot::ClosedHat}, {"ohh", DrumSlot::OpenHat}}) {
        auto a = render(sl, 0, 1.0f), b = render(sl, 0, 2.0f);
        char k[64]; std::snprintf(k, sizeof k, "drum.%s.tail40_decay1", name); Row(k, tail40(a), "s");
        std::snprintf(k, sizeof k, "drum.%s.tail40_decay2", name); Row(k, tail40(b), "s");
        double pk = 0; for (float v : a) pk = std::max<double>(pk, std::fabs(v));
        std::snprintf(k, sizeof k, "drum.%s.peak", name); Row(k, t8m::ToDb(pk), "dBFS");
    }
    // pitching hats up folds energy back: measure the energy above 16 kHz after +12 semitones
    for (float t : {0.f, 5.f, 12.f}) {
        auto x = render(DrumSlot::ClosedHat, t, 1.0f);
        auto p = t8m::PowerSpectrum(x, 0, 16384);
        char k[64]; std::snprintf(k, sizeof k, "drum.chh_tune%d.centroid", static_cast<int>(t));
        Row(k, t8m::SpectralCentroid(p, kSr, 16384), "Hz");
    }
    {   // fidelity: the engine output against the sample file itself (tune 0, decay 1)
        auto src = kit.Variant(DrumSlot::Kick, 0); auto o = render(DrumSlot::Kick, 0, 1.0f);
        double err = 0, ref = 0; for (int i = 200; i < src->frames() - 100 && i < static_cast<int>(o.size()); ++i) { double d = o[i] - src->data[i]; err += d * d; ref += double(src->data[i]) * src->data[i]; }
        Row("drum.kick.vs_source_residual", 10.0 * std::log10(std::max(err, 1e-30) / std::max(ref, 1e-30)), "dB (as recorded)");
        auto h = render(DrumSlot::Kick, 0, 0.5f); const size_t i200 = kSr / 5, i400 = kSr * 2 / 5;
        auto lvl = [&](const std::vector<float>& x, size_t at) { double a = 0; for (size_t i = at; i < at + 441 && i < x.size(); ++i) a += double(x[i]) * x[i]; return 10.0 * std::log10(std::max(a / 441.0, 1e-30)); };
        Row("drum.kick.decay0.5_at200ms_vs_1.0", lvl(h, i200) - lvl(o, i200), "dB");
        Row("drum.kick.decay0.5_at400ms_vs_1.0", lvl(h, i400) - lvl(o, i400), "dB");
    }
    auto k0 = render(DrumSlot::Kick, 0, 1.0f), k3 = render(DrumSlot::Kick, 3, 1.0f);
    auto dom = [&](const std::vector<float>& x) {   // strongest low bin in the first 250 ms
        std::vector<float> seg(x.begin() + 2000, x.begin() + 2000 + 11025);
        auto p = t8m::PowerSpectrum(seg, 0, 16384); size_t bi = 1; for (size_t i = 2; i < 700; ++i) if (p[i] > p[bi]) bi = i; return bi * kSr / 16384.0; };
    Row("drum.kick.pitch_tune0", dom(k0), "Hz"); Row("drum.kick.pitch_tune3", dom(k3), "Hz");
}

void AuditDelay() {
    {   // changing the delay time mid-sound: the biggest sample-to-sample step relative to a steady sine (1.0 = no discontinuity)
        DelayEffect d; d.Init(kSr); const int n = kSr * 2; std::vector<float> in(static_cast<size_t>(n) * 2), out(static_cast<size_t>(n) * 2);
        for (int i = 0; i < n; ++i) { const float v = 0.5f * std::sin(2 * t8m::kPi * 330.0 * i / kSr); in[2 * i] = in[2 * i + 1] = v; }
        for (int o = 0; o < n; o += 256) d.Process(in.data() + static_cast<size_t>(o) * 2, out.data() + static_cast<size_t>(o) * 2, std::min(256, n - o), o < n / 2 ? 0.25f : 0.375f, 0.5f, 0.7f, false);
        double normal = 0, atChange = 0;
        for (int i = kSr / 2; i < n / 2 - 512; ++i) normal = std::max<double>(normal, std::fabs(out[2 * (i + 1)] - out[2 * i]));
        for (int i = n / 2 - 256; i < n / 2 + kSr / 4; ++i) atChange = std::max<double>(atChange, std::fabs(out[2 * (i + 1)] - out[2 * i]));
        Row("delay.time_change.max_step_ratio", atChange / std::max(normal, 1e-9), "x steady (was 1.9)");
    }
    {   // repeats: level of the 2nd echo relative to the 1st, and its brightness at TONE 0 / 1
        for (float tone : {0.0f, 1.0f}) {
            DelayEffect d; d.Init(kSr); const int n = kSr; std::vector<float> in(static_cast<size_t>(n) * 2, 0.0f), out(static_cast<size_t>(n) * 2);
            in[0] = in[1] = 1.0f;
            for (int o = 0; o < n; o += 256) d.Process(in.data() + static_cast<size_t>(o) * 2, out.data() + static_cast<size_t>(o) * 2, std::min(256, n - o), 0.2f, 0.7f, tone, false);
            std::vector<float> e1(out.begin() + 2 * (kSr / 5 - 40), out.begin() + 2 * (kSr / 5 + 400)), e3(out.begin() + 2 * (3 * kSr / 5 - 40), out.begin() + 2 * (3 * kSr / 5 + 400));
            auto energy = [](const std::vector<float>& v) { double a = 0; for (size_t i = 0; i < v.size(); i += 2) a += double(v[i]) * v[i]; return a; };
            char k[64]; std::snprintf(k, sizeof k, "delay.tone%.0f.echo3_vs_echo1", tone); Row(k, 10.0 * std::log10(std::max(energy(e3), 1e-30) / std::max(energy(e1), 1e-30)), "dB");
        }
    }
}

void AuditReverb() {
    for (float size : {0.3f, 0.5f, 0.8f, 0.9f}) {
        ReverbEffect r; r.Init(kSr); const int n = kSr * 6;
        std::vector<float> in(static_cast<size_t>(n) * 2, 0.0f), out(static_cast<size_t>(n) * 2, 0.0f); in[0] = in[1] = 1.0f;
        r.Process(in.data(), out.data(), n, size);
        std::vector<float> L(n), R(n); for (int i = 0; i < n; ++i) { L[i] = out[2 * i]; R[i] = out[2 * i + 1]; }
        char k[64]; std::snprintf(k, sizeof k, "reverb.size%.1f.rt60", size); Row(k, t8m::Rt60Seconds(L, kSr), "s");
        std::vector<float> tail(out.begin() + 2 * (kSr / 5), out.end());
        std::snprintf(k, sizeof k, "reverb.size%.1f.tail_lr_corr", size); Row(k, t8m::MeasureLevels(tail).correlation, "");
    }
    {   // damping: how much the highs die faster than the lows (energy 4-12 kHz vs 100-1000 Hz, 0.3-1.0 s after the click)
        for (float damp : {0.0f, 0.5f, 1.0f}) {
            ReverbEffect r; r.Init(kSr); const int n = kSr * 3;
            std::vector<float> in(static_cast<size_t>(n) * 2, 0.0f), out(static_cast<size_t>(n) * 2, 0.0f); in[0] = in[1] = 1.0f;
            r.Process(in.data(), out.data(), n, 0.85f, damp, 12.0f);
            std::vector<float> L(n); for (int i = 0; i < n; ++i) L[i] = out[2 * i];
            auto p = t8m::PowerSpectrum(L, kSr * 3 / 10, 32768);
            char k[64]; std::snprintf(k, sizeof k, "reverb.damp%.1f.tail_hi_vs_lo", damp);
            Row(k, 10.0 * std::log10(std::max(t8m::BandEnergy(p, 4000, 12000, kSr, 32768), 1e-30) / std::max(t8m::BandEnergy(p, 100, 1000, kSr, 32768), 1e-30)), "dB");
        }
    }
    {   // metallic ringing: a smooth tail has a flat spectrum; a few ringing modes make some third-octave bands stick out
        ReverbEffect r; r.Init(kSr); const int n = kSr * 3;
        std::vector<float> in(static_cast<size_t>(n) * 2, 0.0f), out(static_cast<size_t>(n) * 2, 0.0f); in[0] = in[1] = 1.0f;
        r.Process(in.data(), out.data(), n, 0.85f);
        std::vector<float> L(n); for (int i = 0; i < n; ++i) L[i] = out[2 * i];
        auto p = t8m::PowerSpectrum(L, kSr / 10, 65536);
        double sum = 0, sum2 = 0; int cnt = 0;
        for (double f = 200; f < 6000; f *= 1.0594631) {   // semitone bands
            const double e = 10.0 * std::log10(std::max(t8m::BandEnergy(p, f, f * 1.0594631, kSr, 65536), 1e-30) / (f * 0.0594631));
            sum += e; sum2 += e * e; ++cnt;
        }
        const double mean = sum / cnt; Row("reverb.size0.9.band_ripple_rms", std::sqrt(std::max(0.0, sum2 / cnt - mean * mean)), "dB (semitone bands, lower = smoother)");
    }
}

// two-tone IMD: energy of the 1 kHz +- 55*k Hz (k = 1..3) sidebands relative to the 1 kHz tone
double MasterImdDb(const std::vector<float>& m) {
    const size_t N = 65536, st = m.size() - N - 1000;
    auto sp = t8m::PowerSpectrum(m, st, N);
    auto bin = [&](double f) { return static_cast<long>(std::lround(f * N / kSr)); };
    auto pw = [&](double f) { double s = 0; for (long b = bin(f) - 3; b <= bin(f) + 3; ++b) s += sp[static_cast<size_t>(b)]; return s; };
    double side = 0; for (int k = 1; k <= 3; ++k) side += pw(1000.0 + 55.0 * k) + pw(1000.0 - 55.0 * k);
    return 10.0 * std::log10(std::max(side, 1e-30) / std::max(pw(1000.0), 1e-30));
}

void AuditMaster() {
    for (float f : {50.0f, 100.0f, 1000.0f}) {
        MasterChain mc; MasterChainSettings ms; ms.compOn = true; ms.chainOn = false; ms.compThreshDb = -16; ms.compRatio = 2; ms.compAttackMs = 25; ms.compReleaseMs = 180; ms.compMakeupDb = 0; ms.compMix = 1;
        const int n = kSr * 3; std::vector<float> b(static_cast<size_t>(n) * 2);
        for (int i = 0; i < n; ++i) { float v = 0.5f * std::sin(2 * t8m::kPi * f * i / kSr); b[2 * i] = v; b[2 * i + 1] = v; }
        for (int o = 0; o < n; o += 256) mc.Process(b.data() + static_cast<size_t>(o) * 2, std::min(256, n - o), ms, kSr);
        auto m = Mono(b); char k[64]; std::snprintf(k, sizeof k, "master.comp_%dHz.thdn", static_cast<int>(f));
        Row(k, t8m::SineResidualDb(m, kSr * 2, kSr * 3, f, kSr), "dB");
    }
    {   // v44: every master-chain preset, whole path (chain + limiter): THD+N of a sine and two-tone IMD (55 Hz + 1 kHz)
        static const char* names[] = {"clean", "warm", "loud", "lofi"};
        for (int p = 0; p < kNumMasterPresets; ++p) {
            MasterChainSettings ms; ApplyMasterPreset(ms, p);
            for (float lvl : {-12.0f, -6.0f}) {
                const int n = kSr * 3; const float a = std::pow(10.0f, lvl / 20.0f);
                std::vector<float> s1(static_cast<size_t>(n) * 2), s2(static_cast<size_t>(n) * 2);
                for (int i = 0; i < n; ++i) {
                    float v = a * std::sin(2 * t8m::kPi * 1000.0 * i / kSr); s1[2 * i] = s1[2 * i + 1] = v;
                    float w = a * 0.7f * std::sin(2 * t8m::kPi * 55.0 * i / kSr) + a * 0.5f * std::sin(2 * t8m::kPi * 1000.0 * i / kSr); s2[2 * i] = s2[2 * i + 1] = w;
                }
                MasterChain m1, m2; Limiter l1, l2;
                for (int o = 0; o < n; o += 256) {
                    const int c = std::min(256, n - o);
                    m1.Process(s1.data() + static_cast<size_t>(o) * 2, c, ms, kSr); l1.Process(s1.data() + static_cast<size_t>(o) * 2, c, kSr, ms.ceilingDb, ms.limiterOn);
                    m2.Process(s2.data() + static_cast<size_t>(o) * 2, c, ms, kSr); l2.Process(s2.data() + static_cast<size_t>(o) * 2, c, kSr, ms.ceilingDb, ms.limiterOn);
                }
                char k[80];
                std::snprintf(k, sizeof k, "master.preset_%s.%+.0fdB.thdn_1k", names[p], lvl); Row(k, t8m::SineResidualDb(Mono(s1), kSr * 3 / 2, n, 1000.0, kSr), "dB");
                std::snprintf(k, sizeof k, "master.preset_%s.%+.0fdB.imd", names[p], lvl); Row(k, MasterImdDb(Mono(s2)), "dB re 1 kHz tone");
            }
        }
    }
    {   // limiter: 50 Hz at 1.6 amplitude into a -1 dB ceiling (look-ahead true-peak limiter)
        Limiter lim; const int n = kSr * 2; std::vector<float> b(static_cast<size_t>(n) * 2);
        for (int i = 0; i < n; ++i) { float v = 1.6f * std::sin(2 * t8m::kPi * 50.0 * i / kSr); b[2 * i] = v; b[2 * i + 1] = v; }
        for (int o = 0; o < n; o += 256) lim.Process(b.data() + static_cast<size_t>(o) * 2, std::min(256, n - o), kSr, -1.0f, true);
        auto m = Mono(b); double pk = 0; for (float v : m) pk = std::max<double>(pk, std::fabs(v));
        Row("master.limiter_50Hz.peak_over_ceiling", t8m::ToDb(pk) + 1.0, "dB");
        Row("master.limiter_50Hz.thdn", t8m::SineResidualDb(m, kSr, n, 50.0, kSr), "dB");
        Row("master.limiter.latency", lim.Latency() * 1000.0 / kSr, "ms");
    }
    {   // master DRIVE: aliasing of a 6 kHz sine through the oversampled saturator vs a plain tanh
        const int n = kSr * 2; std::vector<float> a(static_cast<size_t>(n) * 2), b2;
        for (int i = 0; i < n; ++i) { float v = 0.8f * std::sin(2 * t8m::kPi * 6000.0 * i / kSr); a[2 * i] = v; a[2 * i + 1] = v; }
        b2 = a; Saturator sat;
        for (int o = 0; o < n; o += 256) sat.Process(b2.data() + static_cast<size_t>(o) * 2, std::min(256, n - o), 4.0f, 1.0f);
        std::vector<float> plain(a.size()); for (size_t i = 0; i < a.size(); ++i) plain[i] = std::tanh(a[i] * 4.0f);
        Row("master.drive_6k.inharmonic_oversampled", InharmonicDb(Mono(b2), 6000.0), "dB re harmonics");
        Row("master.drive_6k.inharmonic_plain_tanh", InharmonicDb(Mono(plain), 6000.0), "dB re harmonics");
    }
}

void AuditStereo() {
    // a mono hi-hat-like noise through the default per-track stereo settings of the mixer
    Pattern pat; Mixer mx; mx.InitInsertFx(kSr); mx.SyncFromPattern(pat);
    const int n = kSr * 2; std::vector<float> src(static_cast<size_t>(n) * 2), out(static_cast<size_t>(n) * 2, 0.0f);
    unsigned seed = 7; for (int i = 0; i < n; ++i) { seed = seed * 1664525u + 1013904223u; src[2 * i] = src[2 * i + 1] = (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f) * 0.4f; }
    for (int o = 0; o < n; o += 256) { const int c = std::min(256, n - o); mx.MixTrack(static_cast<int>(DrumSlot::ClosedHat), src.data() + static_cast<size_t>(o) * 2, out.data() + static_cast<size_t>(o) * 2, c, nullptr, nullptr, kSr); }
    double ab = 0, aa = 0, bb = 0, mono = 0, ref = 0;
    for (int i = n / 2; i < n; ++i) { ab += out[2 * i] * out[2 * i + 1]; aa += out[2 * i] * out[2 * i]; bb += out[2 * i + 1] * out[2 * i + 1]; mono += std::fabs(out[2 * i] + out[2 * i + 1] - 2.0 * mx.tracks[static_cast<int>(DrumSlot::ClosedHat)].volume * src[2 * i]); ref += std::fabs(src[2 * i]); }
    Row("mixer.closedhat_default.lr_corr", ab / std::sqrt(aa * bb + 1e-30), "");
    Row("mixer.closedhat_default.mono_sum_error", mono / std::max(ref, 1e-30), "(0 = mono compatible)");
    float l, r; PanGains(1.0f, l, r);
    Row("mixer.pan_hard_right_gain", t8m::ToDb(r), "dB (constant-power law, centre 0 dB)");
}

// Live breaks (fills) against a plain groove: every break should sit within a few dB of the groove it replaces.
void AuditFills(const std::string& kitDir) {
    const int bpm = 124; const double barSec = 60.0 / bpm * 4.0; const size_t bar = static_cast<size_t>(barSec * kSr);
    auto run = [&](int breakIndex) {
        AudioEngine eng; eng.Init(kSr); eng.GetKit()->LoadFromFolder(kitDir, kSr);
        Project proj; proj.kitFolder = kitDir; Pattern& pat = proj.patterns[0]; pat.bpm = static_cast<float>(bpm);
        for (int s : {0, 4, 8, 12}) pat.tracks[0].steps[s].active = true;
        for (int s : {4, 12}) pat.tracks[1].steps[s].active = true;
        for (int s : {0, 2, 4, 6, 8, 10, 12, 14}) pat.tracks[3].steps[s].active = true;
        for (int s : {2, 6, 10, 14}) pat.tracks[4].steps[s].active = (s == 14);
        eng.GetMixer().SyncFromPattern(pat); eng.GetMixer().SyncFxFromProject(proj);
        eng.GetMixer().masterVolume = 0.45f;
        Sequencer seq; seq.Init(&eng, &proj); seq.SetSongFollowsEdit(false); seq.Play();
        std::vector<float> buf((bar * 2 + 4096) * 2, 0.0f);
        const size_t armAt = bar / 2;
        for (size_t d = 0; d < bar * 2;) {
            const int n = static_cast<int>(std::min<size_t>(256, bar * 2 - d));
            if (breakIndex >= 0 && d <= armAt && d + n > armAt) seq.SetLiveBreak(breakIndex);
            seq.RenderAndAdvance(buf.data() + d * 2, n, kSr); d += n;
        }
        std::vector<float> win(buf.begin() + static_cast<long>(bar) * 2, buf.begin() + static_cast<long>(bar) * 4);
        // short-term style number: loudest 1 s window of the bar (a fill is not a stationary signal)
        double best = -120.0; const size_t w1 = static_cast<size_t>(kSr);
        for (size_t o = 0; o + w1 <= win.size() / 2; o += kSr / 8) {
            std::vector<float> seg(win.begin() + static_cast<long>(o) * 2, win.begin() + static_cast<long>(o + w1) * 2);
            double e = 0; for (float v : seg) e += double(v) * v; e /= seg.size();
            best = std::max(best, -0.691 + 10.0 * std::log10(std::max(e * 2.0, 1e-12)));   // crude (unweighted) but consistent between fills
        }
        double pk = 0; for (float v : win) pk = std::max<double>(pk, std::fabs(v));
        return std::pair<double, double>(best, t8m::ToDb(pk));
    };
    const auto groove = run(-1);
    Row("fill.groove.loud1s", groove.first, "dB (reference bar)");
    double lo = 1e9, hi = -1e9;
    const auto& brk = AllDrumBreaks();
    for (int i = 0; i < kNumDrumBreaks; ++i) {
        const auto r = run(i);
        std::string nm = brk[i].name; for (char& c : nm) if (c == ' ' || c == '-') c = '_';
        Row("fill." + nm + ".rel_to_groove", r.first - groove.first, "dB");
        Row("fill." + nm + ".peak", r.second, "dBFS");
        lo = std::min(lo, r.first - groove.first); hi = std::max(hi, r.first - groove.first);
    }
    Row("fill.spread", hi - lo, "dB (loudest - quietest fill)");
}

// v39: grid accuracy -- hit times of a kick on every step, in milliseconds of error against the ideal grid
void AuditTiming() {
    auto run = [&](float swing, int nudge, int trackIdx) {
        AudioEngine eng; eng.Init(kSr);
        Project proj; Pattern& pat = proj.patterns[0]; pat.bpm = 120; pat.swing = swing; pat.masterLength = 16;
        for (int s = 0; s < 16; ++s) { pat.tracks[0].steps[s].active = true; pat.tracks[0].steps[s].nudge = static_cast<int8_t>(s == 5 ? nudge : 0); }
        (void)trackIdx;
        eng.GetMixer().SyncFromPattern(pat);
        Sequencer seq; seq.Init(&eng, &proj); eng.Capture().Start(); seq.Play();
        std::vector<float> buf(512 * 2);
        for (int n = 0; n < 8 * kSr; n += 512) seq.RenderAndAdvance(buf.data(), 512, kSr);
        std::vector<double> t; for (const auto& e : eng.Capture().Events()) if (e.isDrum && e.drumSlot == DrumSlot::Kick) t.push_back(e.timeSeconds);
        return t;
    };
    const double step = 0.125;
    {   // swing 0.5: bar 3 / bar 4 start, and the delay of the odd steps
        auto t = run(0.5f, 0, 0);
        double barErr = 0, oddErr = 0;
        for (size_t i = 0; i < t.size() && i < 64; ++i) {
            if (i % 16 == 0) barErr = std::max(barErr, std::fabs(t[i] - i * step));
            if (i & 1) oddErr = std::max(oddErr, std::fabs(t[i] - (i * step + 0.25 * step)));
        }
        Row("timing.swing50.bar_start_err", barErr * 1000.0, "ms");
        Row("timing.swing50.odd_step_err", oddErr * 1000.0, "ms");
    }
    {   // nudge: step 5 -> +30 % and -30 %
        auto tp = run(0.0f, 30, 0), tn = run(0.0f, -30, 0);
        Row("timing.nudge_pos30.err", tp.size() > 5 ? std::fabs(tp[5] - (5 * step + 0.30 * step)) * 1000.0 : 999.0, "ms");
        Row("timing.nudge_neg30.err", tn.size() > 5 ? std::fabs(tn[5] - (5 * step - 0.30 * step)) * 1000.0 : 999.0, "ms");
        Row("timing.nudge_neg30.hits_per_8s", static_cast<double>(tn.size()), "hits (64 steps + the one on the 8.0 s line)");
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string kit = "kits/909", out, cmp;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--kit" && i + 1 < argc) kit = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--compare" && i + 1 < argc) cmp = argv[++i];
    }
    AuditSynth(); AuditSynthFilter(); AuditDrums(kit); AuditDelay(); AuditReverb(); AuditMaster(); AuditStereo(); AuditFills(kit); AuditTiming();

    std::map<std::string, double> base;
    if (!cmp.empty()) { std::ifstream f(cmp); std::string line; while (std::getline(f, line)) { auto e = line.find('='); if (e != std::string::npos) base[line.substr(0, e)] = std::atof(line.c_str() + e + 1); } }
    std::printf("TR-Brick DAW audio audit (%d Hz, kit %s)\n", kSr, kit.c_str());
    for (size_t i = 0; i < g_rows.size(); ++i) {
        std::printf("  %-42s %10.3f  %s", g_rows[i].first.c_str(), g_rows[i].second, g_units[i].c_str());
        auto it = base.find(g_rows[i].first);
        if (it != base.end()) std::printf("   (was %.3f, %+.3f)", it->second, g_rows[i].second - it->second);
        std::printf("\n");
    }
    if (!out.empty()) { std::ofstream f(out); for (auto& r : g_rows) f << r.first << "=" << r.second << "\n"; std::printf("saved %s\n", out.c_str()); }
    return 0;
}
