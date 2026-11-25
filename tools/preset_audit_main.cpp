// t8_presetaudit: renders every built-in synth preset the way it is played (a held note in its register) and prints level,
// loudness, crest, spectrum, stereo and tail numbers plus which of the thick / analog layers the preset uses, so presets can be
// compared with each other (a preset that is 8 dB quieter or has no top end shows up at once).
//   t8_presetaudit [--wav dir] [--csv file]
#include "core/SynthPresets.h"
#include "core/SynthVoice.h"
#include "AudioMetrics.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>

using namespace t8;
namespace {
constexpr int kSr = 48000;

struct Result { std::string name; int note; double peakDb, lufs, crest, cen, lowPct, hiPct, corr, tail, monoLoss; };

// register / hold length by sound type (guessed from the name until presets carry a category)
void Register(const std::string& n, int& note, double& hold) {
    note = 60; hold = 1.5;
    if (n.find("BASS") != std::string::npos || n.find("SUB") != std::string::npos) { note = 36; hold = 1.5; }
    else if (n.find("PAD") != std::string::npos || n.find("DRONE") != std::string::npos) { note = 48; hold = 4.0; }
}

std::vector<float> Render(const SynthPatch& p, int note, double hold, double tail) {
    SynthVoice v; v.SetPatch(p); v.NoteOn(note, 0.9f, false);
    const int nOn = static_cast<int>(hold * kSr), nOff = static_cast<int>(tail * kSr);
    std::vector<float> out(static_cast<size_t>(nOn + nOff) * 2, 0.0f);
    int done = 0;
    while (done < nOn) { int n = std::min(512, nOn - done); v.Render(out.data() + static_cast<size_t>(done) * 2, n, kSr); done += n; }
    v.NoteOff();
    while (done < nOn + nOff) { int n = std::min(512, nOn + nOff - done); v.Render(out.data() + static_cast<size_t>(done) * 2, n, kSr); done += n; }
    return out;
}

Result Measure(const std::string& name, const SynthPatch& p, std::vector<float>* keep) {
    int note; double hold; Register(name, note, hold);
    auto st = Render(p, note, hold, 2.5);
    Result r; r.name = name; r.note = note;
    const size_t nOn = static_cast<size_t>(hold * kSr);
    // the window each sound is judged in: a plucked / stabbed sound (sustain ~ 0) in its first 400 ms, a slow pad once it has risen, the rest after the attack
    double w0, w1;
    if (p.sustain < 0.2f) { w0 = 0.02; w1 = 0.42; }
    else if (p.attack > 0.5f) { w0 = std::min<double>(p.attack + 0.2, hold - 1.0); w1 = hold; }
    else { w0 = 0.1; w1 = hold; }
    const size_t i0 = static_cast<size_t>(w0 * kSr), i1 = static_cast<size_t>(w1 * kSr);
    std::vector<float> body(st.begin() + i0 * 2, st.begin() + i1 * 2);
    auto lv = t8m::MeasureLevels(body);
    r.peakDb = t8m::ToDb(lv.peak); r.crest = lv.crestDb; r.corr = lv.correlation;
    r.lufs = t8m::ToDb(lv.rms);
    std::vector<float> mono(body.size() / 2); for (size_t i = 0; i < mono.size(); ++i) mono[i] = 0.5f * (body[2 * i] + body[2 * i + 1]);
    size_t NN = 1; while (NN * 2 <= std::min<size_t>(32768, mono.size())) NN *= 2;
    auto sp = t8m::PowerSpectrum(mono, 0, NN);
    double tot = t8m::BandEnergy(sp, 20, 20000, kSr, NN) + 1e-30;
    r.cen = t8m::SpectralCentroid(sp, kSr, NN);
    r.lowPct = 100.0 * t8m::BandEnergy(sp, 20, 150, kSr, NN) / tot;
    r.hiPct = 100.0 * t8m::BandEnergy(sp, 5000, 20000, kSr, NN) / tot;
    // length: seconds after note-on until the level has fallen 40 dB under the level in the window (held notes: measured after note-off)
    r.tail = 0;
    const size_t w = kSr / 100;
    for (size_t i = i0; i + w < st.size() / 2; i += w) {
        double e = 0; for (size_t k = 0; k < w; ++k) { double a = st[2 * (i + k)]; e += a * a; }
        if (std::sqrt(e / w) > lv.rms * 0.01) r.tail = static_cast<double>(i + w) / kSr;
    }
    double eS = 0, eM = 0; for (size_t i = 0; i < body.size() / 2; ++i) { double l = body[2 * i], rr = body[2 * i + 1]; eS += 0.5 * (l * l + rr * rr); double m = 0.5 * (l + rr); eM += m * m; }
    r.monoLoss = 10.0 * std::log10(std::max(eM, 1e-30) / std::max(eS, 1e-30));
    if (keep) *keep = st;
    return r;
}

std::string Features(const SynthPatch& p) {
    std::string f;
    auto add = [&](const char* s) { if (!f.empty()) f += " "; f += s; };
    if (p.unisonVoices > 1) add(("uni" + std::to_string(p.unisonVoices)).c_str());
    if (p.subLevel > 0) add("sub");
    if (p.osc2Level > 0) add("osc2");
    if (p.drive > 0) add("drv");
    if (p.noiseLevel > 0) add("nz");
    if (p.acid) add("acid"); else if (p.filter24) add("24dB");
    if (p.drift > 0) add("drift");
    if (p.keytrack > 0) add("key");
    if (p.phaseSync) add("sync");
    if (p.lfoTarget != LfoTarget::Off) add("lfo");
    return f.empty() ? "-" : f;
}
} // namespace

// level targets (RMS dB of the judged window) per category; every preset is also kept under the peak ceiling
double TargetRms(const std::string& cat) {
    if (cat == "BASS") return -11.0;
    if (cat == "LEAD") return -13.0;
    if (cat == "PAD") return -14.0;
    if (cat == "KEYS") return -14.0;
    if (cat == "PLUCK") return -14.0;
    return -14.0;   // FX
}
constexpr double kPeakCeilingDb = -1.0;

int WriteLevels(const char* path) {
    std::ofstream f(path, std::ios::trunc);
    f << "// generated by t8_presetaudit --write-levels (targets: BASS -11, LEAD -13, PAD/KEYS/PLUCK/FX -14 dB RMS, peak below -1 dBFS); do not edit by hand\n";
    for (const auto& pr : AllSynthPresets()) {
        SynthPatch p = pr.patch; double v = p.volume;
        for (int it = 0; it < 6; ++it) {
            p.volume = static_cast<float>(v);
            Result r = Measure(pr.name, p, nullptr);
            double g = TargetRms(pr.category) - r.lufs;
            g = std::min(g, kPeakCeilingDb - r.peakDb);
            v = std::clamp(v * std::pow(10.0, g / 20.0), 0.03, 2.0);
        }
        char buf[96]; std::snprintf(buf, sizeof buf, "    {\"%s\", %.3ff},\n", pr.name, v); f << buf;
    }
    return f.good() ? 0 : 1;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], "--write-levels") == 0 && i + 1 < argc) return WriteLevels(argv[i + 1]);
    std::string wavDir, csv;
    for (int i = 1; i < argc; ++i) { std::string a = argv[i]; if (a == "--wav" && i + 1 < argc) wavDir = argv[++i]; else if (a == "--csv" && i + 1 < argc) csv = argv[++i]; }
    if (!wavDir.empty()) ::mkdir(wavDir.c_str(), 0755);
    std::printf("%-15s %4s %7s %7s %6s %6s %6s %6s %6s %6s %6s  %s\n", "preset", "note", "peak", "RMS dB", "crest", "centHz", "<150%", ">5k%", "L/R", "mono", "sound s", "layers");
    std::vector<Result> all;
    for (const auto& pr : AllSynthPresets()) {
        std::vector<float> st;
        Result r = Measure(pr.name, pr.patch, &st);
        all.push_back(r);
        const char* eng = pr.patch.engineType == SynthEngineType::FM ? "FM " : pr.patch.engineType == SynthEngineType::Wavetable ? "WT " : "";
        std::printf("%-15s %4d %7.1f %7.1f %6.1f %6.0f %6.1f %6.1f %6.2f %6.1f %6.2f  %s%s\n", r.name.c_str(), r.note, r.peakDb, r.lufs, r.crest, r.cen, r.lowPct, r.hiPct, r.corr, r.monoLoss, r.tail, eng, Features(pr.patch).c_str());
        if (!wavDir.empty()) {
            std::string path = wavDir + "/" + r.name + ".raw";
            std::ofstream f(path, std::ios::binary); f.write(reinterpret_cast<const char*>(st.data()), st.size() * sizeof(float));
        }
    }
    return 0;
}
