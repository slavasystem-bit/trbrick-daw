// t8_render: render a .t8proj offline to a WAV and print loudness / level metrics.
//   t8_render project.t8proj [--scene N] [--bars N] [--tail SEC] [--chain] [--out file.wav] [--rate HZ] [--kit folder]
// Uses exactly the engine setup of the app (no audio device). Run from the repo root so kits/ resolves.
#include "core/AudioEngine.h"
#include "core/Sequencer.h"
#include "core/Persistence.h"
#include "core/WavFile.h"
#include "AudioMetrics.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace t8;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: t8_render project.t8proj [--scene N] [--bars N] [--tail SEC] [--chain] [--out f.wav] [--rate HZ] [--kit dir]\n"); return 2; }
    std::string path = argv[1], out = "render.wav", kit;
    int scene = -1, bars = 8, rate = 44100; double tail = 2.0; bool chain = false;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i]; auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--scene") scene = std::atoi(next().c_str()) - 1;
        else if (a == "--bars") bars = std::max(1, std::atoi(next().c_str()));
        else if (a == "--tail") tail = std::atof(next().c_str());
        else if (a == "--out") out = next();
        else if (a == "--rate") rate = std::atoi(next().c_str());
        else if (a == "--kit") kit = next();
        else if (a == "--chain") chain = true;
    }

    Project project;
    if (!LoadProject(project, path)) { std::fprintf(stderr, "cannot load %s\n", path.c_str()); return 1; }
    if (!kit.empty()) project.kitFolder = kit;
    if (scene >= 0) project.currentPattern = std::clamp(scene, 0, kNumPatterns - 1);

    AudioEngine engine; engine.Init(rate);
    const int loaded = engine.GetKit()->LoadFromFolder(project.kitFolder, rate);
    if (loaded < kNumDrumTracks) std::fprintf(stderr, "warning: only %d/%d drum samples loaded from %s\n", loaded, kNumDrumTracks, project.kitFolder.c_str());
    engine.Risers().LoadFromFolder("kits/risers", rate);
    {   // v44.51: the project's own transitions (samples/transitions/tNN_name.wav)
        std::string dir = path; const size_t sl = dir.find_last_of("/\\"); dir = sl == std::string::npos ? std::string(".") : dir.substr(0, sl);
        engine.ReplaceUserTransitions(LoadUserTransitionFolder(dir + "/samples/transitions", rate));
    }
    for (int i = 0; i < kNumSynthTracks; ++i) engine.Synth(i).SetPatch(project.patterns[0].synthPatches[i]);
    engine.Chord().SetPatch(project.patterns[0].chordPatch);
    const Pattern& pat = project.patterns[project.currentPattern];
    engine.GetMixer().SyncFromPattern(pat);
    engine.GetMixer().SyncFxFromProject(project);

    Sequencer seq; seq.Init(&engine, &project);
    {   // v44.50: the samples/ folder beside the project file feeds the sampler track
        const size_t slash = path.find_last_of('/');
        const std::string dir = (slash == std::string::npos ? std::string(".") : path.substr(0, slash)) + "/samples/";
        std::array<SamplePtr, kNumPatterns> smp{};
        for (int i = 0; i < kNumPatterns; ++i) {
            const std::string& f = project.patterns[static_cast<size_t>(i)].samplerPatch.file;
            if (f.empty()) continue;
            SampleBuffer b;
            if (!LoadWav(dir + f, b, rate) || b.empty()) { std::fprintf(stderr, "warning: cannot read sample %s\n", f.c_str()); continue; }
            auto sd = std::make_shared<SampleData>();
            sd->data = std::move(b.data); sd->channels = std::clamp(b.channels, 1, 2); sd->sampleRate = rate; sd->name = f;
            smp[static_cast<size_t>(i)] = sd;
        }
        seq.SetSceneSamples(smp);
    }
    seq.SetSongFollowsEdit(false);
    if (chain && project.songChainLength > 0) seq.SetSongModeActive(true);
    seq.Play();

    const double secPerBar = 60.0 / pat.bpm * 4.0 * (pat.masterLength / 16.0);
    const size_t frames = static_cast<size_t>((secPerBar * bars + tail) * rate);
    std::vector<float> buf(frames * 2, 0.0f);
    for (size_t done = 0; done < frames;) {
        const int n = static_cast<int>(std::min<size_t>(256, frames - done));
        seq.RenderAndAdvance(buf.data() + done * 2, n, rate);
        done += n;
    }
    if (!WriteWavPCM16(out, buf, 2, rate)) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }

    const t8m::Levels lv = t8m::MeasureLevels(buf);
    const double lufs = t8m::IntegratedLufs(buf, rate);
    std::printf("project   : %s (scene %d, %.1f bpm, %d steps)\n", path.c_str(), project.currentPattern + 1, pat.bpm, pat.masterLength);
    std::printf("rendered  : %.1f s @ %d Hz -> %s\n", frames / double(rate), rate, out.c_str());
    std::printf("peak      : %.2f dBFS   (clipped samples: %zu)\n", t8m::ToDb(lv.peak), lv.clipped);
    std::printf("rms       : %.2f dBFS   crest %.1f dB\n", t8m::ToDb(lv.rms), lv.crestDb);
    std::printf("loudness  : %.1f LUFS integrated\n", lufs);
    std::printf("stereo    : L/R correlation %.2f   dc %.5f\n", lv.correlation, lv.dc);
    if (lv.nonFinite) { std::printf("ERROR     : %zu non-finite samples\n", lv.nonFinite); return 1; }
    return 0;
}
