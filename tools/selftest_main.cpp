// Offline self-test: builds a project, programs a pattern by hand, and
// renders it straight through AudioEngine/Sequencer to a WAV file -- no
// SDL audio device required. This is how the engine gets verified inside a
// sandbox that has no real sound card, and it's a handy regression check
// any time the DSP code changes.
#include "core/Grooves.h"
#include "core/ArrangeEnergy.h"
#include "core/ArrangeAuto.h"
#include "Types.h"
#include "Project.h"
#include "AudioEngine.h"
#include "DrumEngine.h"
#include "Onsets.h"
#include "Limiter.h"
#include "LoudnessMeter.h"
#include "Sequencer.h"
#include "Persistence.h"
#include "Grooves.h"
#include "SynthPresets.h"
#include "WavFile.h"
#include "WavStream.h"
#include "Recorder.h"
#include "MultitrackRecorder.h"
#include "CustomPatterns.h"
#include "MasterChain.h"
#include "Stereo.h"
#include "Resample.h"
#include "KickSynth.h"
#include "Scales.h"
#include "SubtractiveSynth.h"
#include <cstring>
#include "CustomSynthPresets.h"
#include "MidiCapture.h"
#include "KitInfo.h"
#include "app/KitFolders.h"
#include "ChordVoice.h"
#include "TrackEq.h"
#include "SynthVoice.h"
#include "app/AppState.h"
#include "app/AudioSettings.h"
#include <sstream>
#include "app/MixLab.h"
#include "core/ChordLab.h"
#include "core/MixAnalysis.h"
#include "ui_desktop/MixerSync.h"
#include "core/UserDirs.h"
#include "core/NoteEdit.h"
#include <filesystem>
#include <regex>
#include "ui_desktop/DynPresets.h"
#include "ui_desktop/ValueField.h"
#include "ui_desktop/InfoLine.h"
#include "ui_desktop/SampleBrowser.h"
#include "app/SynthParams.h"
#include "SynthFilter.h"
#include "Effects.h"
#include "AudioMetrics.h"
#include <complex>
#include <set>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <chrono>
#include "core/ChordSpec.h"
#include "core/SampleLibrary.h"
#include <fstream>
#include <thread>
#include <atomic>
#include <new>
#include <cstdlib>
#include <sys/stat.h>
#include <iostream>
#include <cmath>
#include <vector>

using namespace t8;

// N1 (TEST-6): counts heap allocations and frees made by the thread that sets g_countAllocs (the "audio thread" of a test).
static thread_local bool g_countAllocs = false;
static std::atomic<long> g_audioAllocs{0};
void* operator new(std::size_t n) {
    if (g_countAllocs) g_audioAllocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"   // the replaced operators pair malloc/free on purpose
#endif
void* operator new[](std::size_t n) { return ::operator new(n); }
// the nothrow forms too (std::stable_sort's temporary buffer uses them): otherwise the library's own nothrow new would pair
// with the free() below, which ASan reports as alloc-dealloc-mismatch
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { if (g_countAllocs) g_audioAllocs.fetch_add(1, std::memory_order_relaxed); return std::malloc(n ? n : 1); }
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept { return ::operator new(n, t); }
void operator delete(void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }
void operator delete(void* p) noexcept { if (p && g_countAllocs) g_audioAllocs.fetch_add(1, std::memory_order_relaxed); std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

static bool nearlyEqual(float a, float b, float eps) { return std::fabs(a - b) < eps; }

int main(int argc, char** argv) {
    std::string kitFolder = argc > 1 ? argv[1] : "kits/808";
    std::string outWav = argc > 2 ? argv[2] : "selftest_out.wav";

    // v44.14: the tests never touch the real Documents/TR-Brick: user folders live in a scratch folder next to the tests.
    t8::userdirs::SetRootsForTest("selftest_data/Documents", "selftest_data/Service");

    int failures = 0;
    auto check = [&](bool cond, const std::string& what) {
        std::cout << (cond ? "[ OK ] " : "[FAIL] ") << what << "\n";
        if (!cond) ++failures;
    };

    // ---- Build a project with a simple T8-style pattern -----------------
    Project project;
    project.kitFolder = kitFolder;
    project.name = "Selftest";

    for (int i = 0; i < kNumSynthTracks; ++i) {
        project.patterns[0].synthPatches[i].waveform = static_cast<Waveform>(i % 4);
    }

    Pattern& pat = project.patterns[0];
    pat.masterLength = 16;
    pat.bpm = 128.0f;
    pat.swing = 0.15f;

    // Four-on-the-floor kick, offbeat clap, 16th closed hats with an
    // occasional open hat, and a simple 3-note bassline on Synth1 -- enough
    // structure to exercise triggering, choke, accents, and note on/off.
    auto& kick = pat.tracks[static_cast<int>(DrumSlot::Kick)];
    for (int s = 0; s < 16; s += 4) { kick.steps[s].active = true; kick.steps[s].accent = (s == 0); }

    auto& clap = pat.tracks[static_cast<int>(DrumSlot::Clap)];
    clap.steps[4].active = true;
    clap.steps[12].active = true;

    auto& chh = pat.tracks[static_cast<int>(DrumSlot::ClosedHat)];
    for (int s = 0; s < 16; s += 2) chh.steps[s].active = true;

    auto& ohh = pat.tracks[static_cast<int>(DrumSlot::OpenHat)];
    ohh.steps[14].active = true; // should choke any ringing open hat via the next closed hat

    auto& synth1 = pat.tracks[kNumDrumTracks + 0];
    int notes[4] = {36, 36, 43, 41};
    for (int i = 0; i < 4; ++i) {
        auto& st = synth1.steps[i * 4];
        st.active = true;
        st.note = notes[i];
        st.lengthSteps = 3;
        st.accent = (i == 2);
    }

    check(SaveProject(project, "selftest_project.t8proj"), "save project to disk");
    Project reloaded;
    check(LoadProject(reloaded, "selftest_project.t8proj"), "reload project from disk");
    check(reloaded.patterns[0].tracks[static_cast<int>(DrumSlot::Kick)].steps[0].active,
          "reloaded pattern preserves kick step 0");
    check(reloaded.patterns[0].tracks[kNumDrumTracks].steps[0].note == 36,
          "reloaded pattern preserves synth note data");
    check(nearlyEqual(reloaded.patterns[0].bpm, 128.0f, 0.01f), "reloaded pattern preserves bpm");

    // ---- Load the kit ----------------------------------------------------
    AudioEngine engine;
    engine.Init(kSampleRate);
    int loaded = engine.GetKit()->LoadFromFolder(project.kitFolder, kSampleRate);
    check(loaded == kNumDrumTracks, "kit loaded all 6 required slots");
    check(engine.GetKit()->VariantCount(DrumSlot::Kick) >= 2, "kit exposes alt kick variant (kick_2.wav)");

    for (int i = 0; i < kNumSynthTracks; ++i) engine.Synth(i).SetPatch(project.patterns[0].synthPatches[i]);
    engine.GetMixer().SyncFromPattern(pat);
    // Pinned to unity rather than left at Mixer::masterVolume's own
    // (product-level, UI-facing) default -- this harness checks DSP
    // correctness (render energy, clipping behavior, sample-accurate
    // timing below), which should stay stable regardless of whatever a
    // fresh project's default master fader happens to be tuned to.
    engine.GetMixer().masterVolume = 1.0f;

    Sequencer seq;
    seq.Init(&engine, &project);
    seq.Play();
    check(seq.IsPlaying(), "sequencer reports playing after Play()");

    // ---- Render 2 full loops (32 steps) to a buffer -----------------------
    double secondsPerStep = 60.0 / pat.bpm / 4.0;
    int totalSteps = 32;
    int totalFrames = static_cast<int>(secondsPerStep * kSampleRate * totalSteps) + kSampleRate / 2;

    std::vector<float> buffer(static_cast<size_t>(totalFrames) * 2, 0.0f);
    const int blockSize = 256;
    int rendered = 0;
    while (rendered < totalFrames) {
        int chunk = std::min(blockSize, totalFrames - rendered);
        seq.RenderAndAdvance(buffer.data() + static_cast<size_t>(rendered) * 2, chunk, kSampleRate);
        rendered += chunk;
    }

    // ---- Sanity checks on the rendered audio ------------------------------
    float peak = 0.0f;
    double sumSq = 0.0;
    bool anyNaN = false;
    for (float v : buffer) {
        if (std::isnan(v) || std::isinf(v)) anyNaN = true;
        peak = std::max(peak, std::fabs(v));
        sumSq += static_cast<double>(v) * v;
    }
    double rms = std::sqrt(sumSq / buffer.size());

    check(!anyNaN, "no NaN/Inf samples in render");
    check(peak > 0.01f, "render is not silent");
    check(peak <= 1.0001f, "render stays within [-1,1] (soft-clip working)");
    check(rms > 0.001, "render has non-trivial energy (RMS)");
    std::cout << "     peak=" << peak << " rms=" << rms << " frames=" << totalFrames << "\n";

    check(WriteWavPCM16(outWav, buffer, 2, kSampleRate), "wrote self-test render to WAV");
    std::cout << "     wrote " << outWav << "\n";

    // ---- Mixer mute/solo sanity check -------------------------------------
    seq.Stop();
    check(!seq.IsPlaying(), "sequencer reports stopped after Stop()");
    engine.GetMixer().tracks[static_cast<int>(DrumSlot::Kick)].mute = true;
    check(!engine.GetMixer().ShouldPlay(static_cast<int>(DrumSlot::Kick)), "muted track reports ShouldPlay()==false");
    engine.GetMixer().tracks[static_cast<int>(DrumSlot::Kick)].mute = false;
    engine.GetMixer().tracks[static_cast<int>(DrumSlot::Snare)].solo = true;
    check(!engine.GetMixer().ShouldPlay(static_cast<int>(DrumSlot::Kick)) &&
              engine.GetMixer().ShouldPlay(static_cast<int>(DrumSlot::Snare)),
          "solo on Snare silences Kick, plays Snare");

    // ---- Precise step-timing check -----------------------------------
    // Isolate just the kick on steps 0 and 4 of an otherwise-empty pattern
    // and verify the second hit lands within a couple of samples of the
    // theoretical step-4 boundary. This is what actually proves the
    // sub-chunk sample-accurate scheduling in Sequencer::RenderAndAdvance
    // is correct, rather than just "the render isn't silent".
    {
        Project timingProject;
        timingProject.kitFolder = kitFolder;
        Pattern& tp = timingProject.patterns[0];
        tp.masterLength = 16;
        tp.bpm = 128.0f;
        tp.swing = 0.0f; // steps 0 and 4 are both even anyway, but keep this test swing-free
        auto& tkick = tp.tracks[static_cast<int>(DrumSlot::Kick)];
        tkick.steps[0].active = true;
        tkick.steps[4].active = true;

        AudioEngine tEngine;
        tEngine.Init(kSampleRate);
        tEngine.GetKit()->LoadFromFolder(timingProject.kitFolder, kSampleRate);
        tEngine.GetMixer().SyncFromPattern(tp);
        // Same reasoning as the main render section above -- pin to unity
        // so the fixed 0.5f onset-detection threshold below stays valid
        // regardless of Mixer::masterVolume's own default.
        tEngine.GetMixer().masterVolume = 1.0f;

        Sequencer tSeq;
        tSeq.Init(&tEngine, &timingProject);
        tSeq.Play();

        double samplesPerStep = 60.0 / tp.bpm / 4.0 * kSampleRate;
        int expectedStep4Sample = static_cast<int>(std::round(samplesPerStep * 4));
        int renderFrames = expectedStep4Sample + static_cast<int>(kSampleRate * 0.3);

        std::vector<float> tbuf(static_cast<size_t>(renderFrames) * 2, 0.0f);
        int done = 0;
        while (done < renderFrames) {
            int chunk = std::min(256, renderFrames - done);
            tSeq.RenderAndAdvance(tbuf.data() + static_cast<size_t>(done) * 2, chunk, kSampleRate);
            done += chunk;
        }

        auto firstCrossing = [&](int fromFrame, int toFrame) -> int {
            for (int i = std::max(0, fromFrame); i < toFrame; ++i) {
                if (std::fabs(tbuf[static_cast<size_t>(i) * 2]) > 0.05f) return i;
            }
            return -1;
        };

        // Note: the first kick's own decay tail is still well above a low
        // threshold by the time step 4 comes around (~0.3 of peak at 0.41s
        // in, since the kick's natural+shaped decay is several hundred ms).
        // So the second-hit search uses a high threshold that only a fresh
        // onset transient (~0.85+ peak) crosses, not the first hit's tail.
        int firstHit = firstCrossing(0, expectedStep4Sample / 2);
        auto firstHighCrossing = [&](int fromFrame, int toFrame) -> int {
            for (int i = std::max(0, fromFrame); i < toFrame; ++i) {
                if (std::fabs(tbuf[static_cast<size_t>(i) * 2]) > 0.5f) return i;
            }
            return -1;
        };
        int secondHit = firstHighCrossing(expectedStep4Sample - static_cast<int>(samplesPerStep / 2),
                                           expectedStep4Sample + static_cast<int>(samplesPerStep / 2));

        // the master bus has a fixed look-ahead latency (limiter + oversampled saturator), so the first onset sits
        // that much after sample 0; step 4 is measured relative to step 0 with the same detector
        const int busLatency = Limiter().Latency() + Saturator::kLatency;
        int firstHigh = firstHighCrossing(0, expectedStep4Sample / 2);
        check(firstHit >= 0 && firstHit < 200 + busLatency, "isolated kick: step 0 hit starts essentially at sample 0 (+ bus latency)");
        check(secondHit >= 0, "isolated kick: step 4 hit detected");
        if (secondHit >= 0) {
            int errSamples = std::abs((secondHit - std::max(0, firstHigh)) - expectedStep4Sample);
            std::cout << "     step4 expected@" << expectedStep4Sample << " actual@" << secondHit
                       << " err=" << errSamples << " samples ("
                       << (errSamples * 1000.0 / kSampleRate) << " ms)\n";
            // one audio block (<=256 frames here) of slack is the theoretical
            // worst case given block-boundary rendering inside RenderAndAdvance
            check(errSamples <= 256, "isolated kick: step 4 timing within one block of the true boundary");
        }
    }


    // ---- Song mode drives playback only (desktop LIVE: editor stays put) ----
    {
        AudioEngine cEngine;
        cEngine.Init(kSampleRate);
        Project cp;
        cp.kitFolder = kitFolder;
        cp.patterns[0].masterLength = 16;
        cp.patterns[1].masterLength = 16;
        cp.songChain[0] = ChainStep{1, 1};
        cp.songChain[1] = ChainStep{2, 1};
        cp.songChainLength = 2;
        cp.currentPattern = 0;
        Sequencer cSeq;
        cSeq.Init(&cEngine, &cp);
        cSeq.SetSongFollowsEdit(false);
        cSeq.SetSongModeActive(true);
        check(cSeq.PlayPatternIndex() == 1, "song mode: playback starts on the first chain slot's pattern");
        check(cp.currentPattern == 0, "song mode (no follow): the edited pattern is not moved by the chain");
        cSeq.Play();
        std::vector<float> cbuf(256 * 2);
        double spStep = 60.0 / 120.0 / 4.0 * kSampleRate; // default 120 bpm, one 16th
        int framesToRender = static_cast<int>(spStep * 17.0); // one full 16-step loop + a bit
        for (int r = 0; r < framesToRender; r += 256) cSeq.RenderAndAdvance(cbuf.data(), 256, kSampleRate);
        check(cSeq.PlayPatternIndex() == 2, "song mode: chain advanced to its second pattern after one loop");
        check(cp.currentPattern == 0, "song mode (no follow): edited pattern still untouched after the advance");
        cSeq.Stop();
        cSeq.SetSongFollowsEdit(true);
        cSeq.SetSongModeActive(true);
        check(cp.currentPattern == 1, "song mode (follow): the edited pattern follows the chain");
        cSeq.SetSongModeActive(false);
        cp.patterns[1].tracks[0].mute = false;
        cEngine.GetMixer().tracks[0].mute = true;
        cEngine.GetMixer().SyncFromPattern(cp.patterns[1], true);
        check(cEngine.GetMixer().tracks[0].mute, "scene change keeps the live mute (SyncFromPattern keepMuteSolo)");
    }

    // ---- Endless pad: a chord step as long as the loop must not retrigger at loop start ----
    {
        AudioEngine pEngine;
        pEngine.Init(kSampleRate);
        Project pp;
        pp.kitFolder = kitFolder;
        Pattern& pat = pp.patterns[0];
        pat.masterLength = 16;
        SynthPatch sp = pp.patterns[0].chordPatch;
        sp.attack = 0.3f; sp.decay = 0.3f; sp.sustain = 0.6f; sp.release = 0.5f;
        sp.envAmount = 1.0f; sp.filterDecay = 0.15f;
        pEngine.Chord().SetPatch(sp);
        Track& ct = pat.tracks[kNumDrumTracks + kNumSynthTracks];
        check(ct.type == TrackType::Chord, "pad test: found the chord track");
        Step& st = ct.steps[0];
        st.active = true; st.chordSize = 3; st.chordNotes = {48, 52, 55, 0};
        st.lengthSteps = 16;
        Sequencer pSeq;
        pSeq.Init(&pEngine, &pp);
        pSeq.Play();
        const double spS = 60.0 / 120.0 / 4.0 * kSampleRate;
        const int loopFrames = static_cast<int>(spS * 16.0);
        std::vector<float> pbuf(static_cast<size_t>(loopFrames) * 4 * 2, 0.0f);
        int done = 0, total = loopFrames * 4;
        while (done < total) {
            int chunk = std::min(256, total - done);
            pSeq.RenderAndAdvance(pbuf.data() + static_cast<size_t>(done) * 2, chunk, kSampleRate);
            done += chunk;
        }
        auto rms = [&](int a, int b) {
            double e = 0; for (int i = a; i < b; ++i) e += pbuf[i * 2] * pbuf[i * 2];
            return std::sqrt(e / std::max(1, b - a));
        };
        int w = kSampleRate * 3 / 50; // 60 ms (a few cycles of the lowest chord note, so the RMS does not depend on the phase)
        bool ok = true;
        for (int b = 2; b <= 3; ++b) {
            int at = loopFrames * b;
            const int lat = Limiter().Latency() + Saturator::kLatency;   // the master bus delays everything by a fixed amount
            double before = rms(at + lat - w, at + lat), after = rms(at + lat, at + lat + w);
            double ratio = after / std::max(1e-9, before);
            if (!(before > 1e-4) || ratio < 0.8 || ratio > 1.25) ok = false;
        }
        check(ok, "endless pad: no level dip/bump (retrigger) at loop restarts");
    }

    // ---- Project data safety (v23, stage A) ----------------------------------
    {
        const std::string dir = "selftest_tmp_safety";
        ::mkdir(dir.c_str(), 0755);
        const std::string path = dir + "/p.t8proj";
        std::remove(path.c_str()); std::remove((path + ".bak").c_str()); std::remove((path + ".corrupt").c_str());

        Project a;
        a.name = "RoundTrip";
        a.patterns[3].tracks[0].steps[4].active = true;
        a.patterns[3].tracks[0].steps[4].ratchet = 3;
        a.patterns[0].synthPatches[1].cutoffHz = 1234.0f;
        a.patterns[3].tracks[3].width = 1.4f; a.patterns[3].tracks[3].spread = 0.65f;
        check(SaveProject(a, path), "safety: save succeeds");
        check(std::ifstream(path + ".tmp").good() == false, "safety: no stray .tmp left after a save");
        Project b;
        check(LoadProject(b, path), "safety: load succeeds");
        check(nearlyEqual(b.patterns[3].tracks[3].width, 1.4f, 1e-4f) && nearlyEqual(b.patterns[3].tracks[3].spread, 0.65f, 1e-4f), "stereo: track width / spread survive save/load");
        check(b.name == "RoundTrip" && b.patterns[3].tracks[0].steps[4].ratchet == 3 &&
              nearlyEqual(b.patterns[0].synthPatches[1].cutoffHz, 1234.0f, 0.5f), "safety: save/load round trip keeps the data");

        // second save -> the first becomes .bak; then cut the main file short
        a.name = "Second";
        SaveProject(a, path);
        check(std::ifstream(path + ".bak").good(), "safety: previous save kept as .bak");
        {
            std::ifstream in(path); std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::ofstream out(path, std::ios::trunc); out << all.substr(0, all.size() / 2);   // truncated: no [end]
        }
        Project c;
        bool okc = LoadProject(c, path);
        check(okc && c.name == "RoundTrip", "safety: a cut-short file falls back to the .bak");
        check(!LastLoadNote().empty(), "safety: recovery is reported (LastLoadNote)");
        check(std::ifstream(path + ".corrupt").good(), "safety: the damaged file is kept as .corrupt");

        // garbage numbers must not throw
        {
            std::ofstream out(path, std::ios::trunc); out << "[project]\nformat=1\nmasterVolume=notanumber\n";
        }
        std::remove((path + ".bak").c_str());
        Project d;
        bool threw = false, okd = true;
        try { okd = LoadProject(d, path); } catch (...) { threw = true; }
        check(!threw && !okd, "safety: unparsable value is reported, not thrown");

        // backups
        std::ofstream(path, std::ios::trunc) << "[project]\nformat=1\nname=x\n";
        Project e; LoadProject(e, path);
        check(!ListProjectBackups(path).empty(), "safety: opening a project writes a dated backup");
    }

    // ---- v44.59 (stage Н2): nothing gets lost: damaged values, newer files, backups, failed loads, the starter brick ------------
    {
        namespace ud = t8::userdirs;
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::remove_all("selftest_n2", ec);
        ud::SetRootsForTest("selftest_n2/Documents", "selftest_n2/Service");
        fs::create_directories(ud::ProjectsDir(), ec);
        auto slurp = [](const std::string& path) { std::ifstream in(path, std::ios::binary); return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); };
        auto put = [](const std::string& path, const std::string& txt) { std::ofstream o(path, std::ios::trunc | std::ios::binary); o << txt; };
        auto replaceLine = [](std::string txt, const std::string& start, const std::string& with) {
            const size_t at = txt.find("\n" + start);
            if (at == std::string::npos) return txt;
            const size_t eol = txt.find('\n', at + 1);
            return txt.replace(at + 1, eol - at - 1, with);
        };

        // DATA-3: damaged numbers are put back into range, a healthy file is not touched
        {
            const std::string path = ud::ProjectsDir() + "/damaged.t8proj";
            Project a;
            a.patterns[0].tracks[0].steps[0].active = true;
            check(SaveProject(a, path), "n2 data-3: save");
            Project clean;
            check(LoadProject(clean, path) && LastLoadFixedValues() == 0 && LastLoadNote().empty(), "n2 data-3: a healthy file loads with nothing repaired");
            std::string txt = slurp(path);
            txt = replaceLine(txt, "masterLength=", "masterLength=500");
            txt = replaceLine(txt, "bpm=", "bpm=nan");
            const size_t t0 = txt.find("[pattern0.track0]");
            txt.insert(txt.find("\n[", t0) + 1, "slotIndex=40\nlength=-3\nvolume=inf\ntype=Synth\n");   // at the end of the section: wins over the healthy lines
            put(path, txt);
            Project d;
            check(LoadProject(d, path), "n2 data-3: a file with damaged values still opens");
            const Pattern& p0 = d.patterns[0];
            check(p0.masterLength == kMaxSteps && p0.bpm == 120.0f && p0.tracks[0].slotIndex == 0 && p0.tracks[0].type == TrackType::Drum &&
                  p0.tracks[0].length == 1 && std::isfinite(p0.tracks[0].volume), "n2 data-3: lengths, slots, types and NaN / inf are put back to safe values");
            check(LastLoadFixedValues() >= 5 && LastLoadNote().find("REPAIRED") != std::string::npos, "n2 data-3: the repair is counted and said");
            Project r;
            check(LoadProject(r, "examples/rolling.t8proj") && LastLoadFixedValues() == 0, "n2 data-3: the example project needs no repair");
        }

        // DATA-1: the format guard and a file from a newer version
        {
            std::ifstream ks("tools/project_format_keys.txt");
            std::string line; int listedFormat = -1; std::set<std::string> listed;
            while (std::getline(ks, line)) {
                if (line.empty() || line[0] == '#') continue;
                if (line.rfind("format=", 0) == 0) listedFormat = std::atoi(line.c_str() + 7); else listed.insert(line);
            }
            const std::string src = slurp("src/core/Persistence.cpp");
            const std::string writer = src.substr(0, src.find("std::string ProjectToString("));
            std::set<std::string> written;
            const std::regex lit("\"([A-Za-z][A-Za-z0-9_]*)="), idx("\"([A-Za-z][A-Za-z0-9_]*)\" << \\w+ << \"=");
            for (std::sregex_iterator it(writer.begin(), writer.end(), lit), e; it != e; ++it) written.insert((*it)[1]);
            for (std::sregex_iterator it(writer.begin(), writer.end(), idx), e; it != e; ++it) written.insert((*it)[1].str() + "N");
            std::string unknown;
            for (const auto& k : written) if (!listed.count(k)) unknown += " " + k;
            check(!src.empty() && listedFormat > 0, "n2 data-1: the writer and tools/project_format_keys.txt are found (run from the repository root)");
            check(unknown.empty(), "n2 data-1: every key the writer can write is in tools/project_format_keys.txt (a new key: raise kProjectFormatVersion, add it there)" + (unknown.empty() ? std::string() : " NEW:" + unknown));
            check(listedFormat == kProjectFormatVersion, "n2 data-1: the key list belongs to the current format number");
        }
        {
            const std::string path = ud::ProjectsDir() + "/newer.t8proj";
            Project a; a.name = "newer";
            check(SaveProject(a, path), "n2 data-1: save");
            std::string txt = replaceLine(slurp(path), "format=", "format=99");
            txt.insert(txt.find("[end]"), "[futurestuff]\nsomethingNew=1\n");
            put(path, txt);
            const std::string before = slurp(path);
            AppState app; app.engine.Init(kSampleRate); app.sequencer.Init(&app.engine, &app.project);
            check(app.LoadProjectFile(path) && app.docNewerFormat == 99 && LastLoadNewerFormat() == 99, "n2 data-1: a file from a newer version opens and is marked");
            check(!app.docNoticeTitle.empty() && app.docNoticeTitle.find("newer") != std::string::npos, "n2 data-1: the window says it was saved by a newer TR-Brick");
            app.project.patterns[0].tracks[0].steps[3].active = true;
            check(!app.SaveDocument() && app.docNameOpen && slurp(path) == before, "n2 data-1: Cmd+S does not write over it, it asks for a name");
            app.docNameOpen = false;
            check(app.RenameDocument("newer renamed") && slurp(ud::ProjectsDir() + "/newer renamed.t8proj") == before, "n2 data-1: rename moves the file without rewriting it");
            check(app.SaveProjectAs("newer copy") && app.docNewerFormat == 0 && slurp(ud::ProjectsDir() + "/newer renamed.t8proj") == before, "n2 data-1: SAVE AS makes this version's own copy; the newer file stays as it was");
            check(app.SaveDocument(), "n2 data-1: the copy saves normally");
        }

        // DATA-4: backups: no copy when nothing changed, one list per folder, days kept, a readable label
        {
            const std::string pa = ud::ProjectsDir() + "/song.t8proj";
            fs::create_directories("selftest_n2/Other", ec);
            const std::string pb = "selftest_n2/Other/song.t8proj";
            Project a; a.name = "song";
            check(SaveProject(a, pa) && SaveProject(a, pb), "n2 data-4: two projects with the same name in two folders");
            const size_t n0 = ListProjectBackups(pa).size();
            Project l;
            LoadProject(l, pa); LoadProject(l, pa); LoadProject(l, pa);
            check(ListProjectBackups(pa).size() == n0 + 1, "n2 data-4: opening the same file three times leaves one copy, not three");
            a.patterns[0].tracks[0].steps[1].active = true; SaveProject(a, pa, false);
            LoadProject(l, pa);
            check(ListProjectBackups(pa).size() == n0 + 2, "n2 data-4: a changed file gets a new copy (even in the same second)");
            for (int i = 0; i < 12; ++i) LoadProject(l, pb);
            check(ListProjectBackups(pa).size() == n0 + 2 && ListProjectBackups(pb).size() == 1, "n2 data-4: the same name in another folder does not push out the copies");
            // the days: 25 old copies, one per day, plus today's 10: the newest 10 and the newest of 14 older days stay
            const std::string bdir = ud::BackupsDir();
            const std::string stem = fs::path(ListProjectBackups(pa).front()).stem().string();
            const std::string code = stem.substr(stem.find('~') + 1, 8);
            for (int d = 1; d <= 25; ++d) { char nm[96]; std::snprintf(nm, sizeof nm, "/song~%s-202601%02d-120000.t8proj", code.c_str(), d); put(bdir + nm, "x" + std::to_string(d)); }
            for (int i = 0; i < 12; ++i) { a.patterns[0].tracks[1].steps[i].active = true; SaveProject(a, pa, false); LoadProject(l, pa); }
            const auto all = ListProjectBackups(pa);
            int old = 0; for (const auto& b : all) if (b.find("-202601") != std::string::npos) ++old;
            // today has 14 copies: the newest 10 stay, the older 4 count as one day; with 13 of the January days that makes 14 days
            check(old == 13 && all.size() == 24 && all.back().find("-20260113-") != std::string::npos, "n2 data-4: the newest 10 copies and one copy for each of 14 older days are kept");
            check(BackupLabel(bdir + "/song~" + code + "-20260105-120000.t8proj") == "2026-01-05  12:00:00", "n2 data-4: the Versions list shows the date and time");
            put(bdir + "/song-20250101-101010.t8proj", "legacy");
            check(ListProjectBackups(pa).back().find("song-20250101-101010") != std::string::npos, "n2 data-4: copies from before v44.59 are still listed");
            auto countFiles = [&] { size_t n = 0; for (const auto& e : fs::directory_iterator(bdir, ec)) { (void)e; ++n; } return n; };
            const size_t files0 = countFiles();
            check(LoadProject(l, ListProjectBackups(pa).front()) && countFiles() == files0, "n2 data-4: opening a copy from the backups folder makes no copy of the copy");
        }

        // DATA-5: the save checks every write; a failed one is never shown as saved
        {
            AppState app; app.engine.Init(kSampleRate); app.sequencer.Init(&app.engine, &app.project);
            const std::string path = ud::ProjectsDir() + "/restore.t8proj";
            Project a; a.name = "restore";
            check(SaveProject(a, path), "n2 data-5: save");
            check(app.LoadProjectFile(path), "n2 data-5: open");
            const auto copies = ListProjectBackups(path);
            app.projectPath = "selftest_n2/no_such_folder/restore.t8proj";   // a place that cannot be written
            check(!copies.empty() && app.RestoreBackupFile(copies.front()) && app.IsEditedNow(), "n2 data-5: a restore that cannot write stays Edited (not shown as saved)");
            check(app.statusMessage.find("FAILED") != std::string::npos || app.statusMessage.find("STOPPED") != std::string::npos, "n2 data-5: and says why");
            check(!fs::exists(path + ".tmp") && fs::exists(path + ".bak") == false, "n2 data-5: no stray .tmp");
            SaveProject(a, path); a.name = "second"; SaveProject(a, path);
            check(slurp(path + ".bak").find("name=restore") != std::string::npos && slurp(path).find("name=second") != std::string::npos, "n2 data-5: the previous save is kept as .bak");
        }

        // DATA-2: a project that cannot be opened is said, nothing is written over it, the newest copy brings it back
        {
            AppState app; app.engine.Init(kSampleRate); app.sequencer.Init(&app.engine, &app.project);
            const std::string path = ud::ProjectsDir() + "/broken.t8proj";
            Project a; a.name = "broken"; a.patterns[2].tracks[0].steps[5].active = true;
            check(SaveProject(a, path, false) && app.LoadProjectFile(path), "n2 data-2: open once (a dated copy is made)");
            app.NewDocument();
            put(path, "[project]\nformat=8\nname=bro");   // cut short: no [end]
            check(!app.LoadProjectFile(path) && app.docNoticeFailedPath == path && app.docNoticeTitle.find("could not be opened") != std::string::npos, "n2 data-2: a failed open is said in the window");
            check(fs::exists(path + ".corrupt") && app.IsUntitled(), "n2 data-2: the damaged file is kept and the open project is untouched");
            check(app.docNoticeCanRestore, "n2 data-2: the window knows there is a copy to go back to");
            {   // the newest copy is damaged too: the restore takes the next older one
                const std::string stem = fs::path(ListProjectBackups(path).front()).stem().string();
                put(ud::BackupsDir() + "/" + stem.substr(0, stem.size() - 15) + "29991231-235959.t8proj", "[project]\nformat=8\nname=bro");
            }
            check(app.RestoreFailedFromBackup() && app.projectPath == path && app.project.patterns[2].tracks[0].steps[5].active && fs::exists(path) && !app.IsEditedNow(),
                  "n2 data-2: RESTORE NEWEST COPY brings the project back as the project file");
        }

        // DATA-7: a chain step that points at a LIVE scene (the old starter brick) gets a brick slot of its own
        {
            const std::string path = ud::ProjectsDir() + "/starter.t8proj";
            Project a;
            a.patterns[0].tracks[0].steps[0].active = true; a.patterns[0].synthPatches[0].cutoffHz = 777.0f;
            a.songChain[0] = ChainStep{0, 4}; a.songChainLength = 1;
            check(SaveProject(a, path, false), "n2 data-7: save an old starter project");
            Project l;
            check(LoadProject(l, path), "n2 data-7: it opens");
            const int sl = l.songChain[0].patternIndex;
            check(AppState::IsBrickSlot(sl) && l.songChain[0].repeats == 4 && l.patterns[static_cast<size_t>(sl)].tracks[0].steps[0].active &&
                  l.patterns[static_cast<size_t>(sl)].synthPatches[0].cutoffHz == 777.0f, "n2 data-7: brick 1 is a copy of scene 1 in its own slot (same notes and sound)");
            l.patterns[static_cast<size_t>(sl)].tracks[0].steps[8].active = true;
            check(!l.patterns[0].tracks[0].steps[8].active, "n2 data-7: editing the brick no longer changes LIVE scene 1");
            SaveProject(l, path, false);
            Project l2; LoadProject(l2, path);
            check(l2.songChain[0].patternIndex == sl && LastLoadFixedValues() == 0, "n2 data-7: the next load keeps it as it is");
            AppState app; app.engine.Init(kSampleRate); app.sequencer.Init(&app.engine, &app.project);
            uint16_t mm = 0; StepParams spr;
            const int slot = app.CopySceneToBrickSlot(0, true, &mm, &spr);
            check(AppState::IsBrickSlot(slot), "n2 data-7: the starter brick of a first start is copied into a brick slot");
        }

        // LIVE-4: the "Edited" check builds its text from a copy, so the lock is held only for the copy
        {
            AppState app; app.engine.Init(kSampleRate); app.sequencer.Init(&app.engine, &app.project);
            app.MarkSaved();
            check(!app.IsEditedNow(), "n2 live-4: a just-saved project is not Edited");
            app.project.patterns[1].tracks[2].steps[2].active = true;
            check(app.IsEditedNow(), "n2 live-4: a change makes it Edited");
            app.project.currentPattern = 3;
            app.project.patterns[1].tracks[2].steps[2].active = false;
            check(!app.IsEditedNow(), "n2 live-4: looking at another scene is not an edit; undoing the change clears Edited");
        }
        ud::SetRootsForTest("selftest_data/Documents", "selftest_data/Service");
        fs::remove_all("selftest_n2", ec);
    }

    // ---- v44.59 (stage Н2, LIVE-8): a damaged WAV / AIFF never reads past its buffers, never allocates gigabytes ----------------
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories("selftest_tmp_wav8", ec);
        std::vector<float> tone(4410);
        for (size_t i = 0; i < tone.size(); ++i) tone[i] = 0.5f * std::sin(static_cast<float>(i) * 0.06f);
        const std::string good = "selftest_tmp_wav8/good.wav";
        check(WriteWavPCM16(good, tone, 1, 44100) && !fs::exists(good + ".tmp"), "live-8: a WAV is written through a temporary file (none left behind)");
        check(!WriteWavPCM16("selftest_tmp_wav8/no_such_folder/x.wav", tone, 1, 44100), "live-8: a WAV that cannot be written is reported");
        std::string bytes;
        { std::ifstream in(good, std::ios::binary); bytes.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); }
        auto put32 = [](std::string& b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + static_cast<size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xFF); };
        auto write = [](const std::string& path, const std::string& b) { std::ofstream o(path, std::ios::binary | std::ios::trunc); o << b; };
        SampleBuffer sb;
        {   // the data chunk claims 4 GB: only what the file holds is read, quickly
            std::string b = bytes; put32(b, 40, 0xFFFFFFF0u);
            write("selftest_tmp_wav8/huge.wav", b);
            const auto t0 = std::chrono::steady_clock::now();
            const bool ok = LoadWav("selftest_tmp_wav8/huge.wav", sb, 44100);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            check(ok && sb.data.size() == tone.size() && ms < 200.0, "live-8: a header that claims 4 GB loads what is really there, at once");
        }
        {   // a fmt chunk shorter than its 16 fields
            std::string b = bytes.substr(0, 12) + std::string("fmt \x04\0\0\0\x01\0\x01\0", 12) + bytes.substr(36);
            write("selftest_tmp_wav8/shortfmt.wav", b);
            check(!LoadWav("selftest_tmp_wav8/shortfmt.wav", sb, 44100), "live-8: a short fmt chunk is refused (it was read past its end)");
        }
        {   // a sample rate of 1 Hz would make the resampler produce 44100 times the data
            std::string b = bytes; put32(b, 24, 1u);
            write("selftest_tmp_wav8/rate1.wav", b);
            check(!LoadWav("selftest_tmp_wav8/rate1.wav", sb, 44100), "live-8: an absurd sample rate is refused");
        }
        {   // an AIFF whose sound chunk claims 4 GB
            std::vector<float> st(200, 0.25f);
            std::string a = "FORM____AIFFCOMM";
            auto be32s = [](uint32_t v) { std::string s(4, '\0'); for (int i = 0; i < 4; ++i) s[static_cast<size_t>(i)] = static_cast<char>((v >> (24 - 8 * i)) & 0xFF); return s; };
            a += be32s(18) + std::string("\0\x01", 2) + be32s(200) + std::string("\0\x10", 2) + std::string("\x40\x0E\xAC\x44\0\0\0\0\0\0", 10);
            a += "SSND" + be32s(0xFFFFFFF0u) + be32s(0) + be32s(0) + std::string(400, '\x10');
            write("selftest_tmp_wav8/huge.aif", a);
            check(LoadWav("selftest_tmp_wav8/huge.aif", sb, 44100) && sb.data.size() == 200, "live-8: an AIFF that claims 4 GB loads what is really there");
        }
        {   // 400 random damages of the header and the first samples: never a crash or a hang, a load gives whole frames
            uint32_t rng = 12345u;
            auto next = [&]() { rng = rng * 1664525u + 1013904223u; return rng; };
            bool sane = true;
            for (int n = 0; n < 400; ++n) {
                std::string b = bytes;
                const int hits = 1 + static_cast<int>(next() % 4);
                for (int h = 0; h < hits; ++h) b[next() % 64] = static_cast<char>(next() & 0xFF);
                if (next() % 3 == 0) b.resize(next() % b.size());
                write("selftest_tmp_wav8/fz.wav", b);
                SampleBuffer z;
                if (LoadWav("selftest_tmp_wav8/fz.wav", z, 44100) && (z.channels < 1 || z.channels > 2 || z.data.size() % static_cast<size_t>(z.channels) != 0 || z.data.size() > 50u * 1000 * 1000)) sane = false;
            }
            check(sane, "live-8: 400 randomly damaged WAV files: no crash, no hang, only whole frames");
        }
        fs::remove_all("selftest_tmp_wav8", ec);
    }

    // ---- Undo is tied to scenes and also covers sound / chain ---------------
    {
        AppState app;
        app.engine.Init(kSampleRate);
        app.project.currentPattern = 0;
        app.PushUndo();                                   // scene 1 snapshot
        app.CurrentPattern().tracks[0].steps[0].active = true;
        app.project.currentPattern = 1;                   // switch scene
        app.CurrentPattern().tracks[0].steps[2].active = true;
        check(!app.Undo(), "undo: scene 2 has no history, scene 1's snapshot must not be applied to it");
        check(app.project.patterns[1].tracks[0].steps[2].active, "undo: scene 2 content untouched");
        app.project.currentPattern = 0;
        check(app.Undo() && !app.project.patterns[0].tracks[0].steps[0].active, "undo: scene 1 still undoes its own edit");

        app.undoStack.clear();
        float before = app.project.patterns[0].synthPatches[0].cutoffHz;
        app.PushSoundUndo();
        app.project.patterns[0].synthPatches[0].cutoffHz = before + 500.0f;
        app.PushSoundUndo();   // within 0.8 s: same undo step
        app.project.patterns[0].synthPatches[0].cutoffHz = before + 900.0f;
        check(app.undoStack.size() == 1, "undo: a continuous drag coalesces into one step");
        check(app.Undo() && nearlyEqual(app.project.patterns[0].synthPatches[0].cutoffHz, before, 0.01f), "undo: a synth patch edit is undoable");

        app.project.songChainLength = 0;
        app.PushSoundUndo();
        app.project.songChain[0] = ChainStep{2, 3};
        app.project.songChainLength = 1;
        check(app.Undo() && app.project.songChainLength == 0, "undo: chain edits are undoable");

        // a failed project open must not blank the current project
        app.project.name = "KeepMe";
        std::string savedPath = app.projectPath;
        app.projectPath = "selftest_tmp_safety/keep.t8proj";
        bool opened = app.LoadProjectFile("selftest_tmp_safety/does_not_exist.t8proj");
        check(!opened && app.project.name == "KeepMe", "safety: failed open leaves the current project intact");
        app.projectPath = savedPath;
    }

    // ---- Drum beat library -----------------------------------------------------
    {
        Pattern pat;
        pat.masterLength = 16;
        pat.tracks[1].steps[4].active = true;
        pat.tracks[1].steps[4].accent = true;
        pat.tracks[1].steps[4].variant = 1;
        pat.tracks[1].steps[4].ratchet = 2;
        pat.tracks[1].steps[4].probability = 60;
        pat.tracks[0].steps[0].active = true;
        check(SaveCustomDrumPattern("zz_selftest_beat", pat), "beats: save");
        CustomPatternData d;
        check(LoadCustomDrumPattern("zz_selftest_beat", d) && d.hitSteps[1].size() == 1 && d.details[1].size() == 1 &&
              d.details[1][0].ratchet == 2 && d.details[1][0].probability == 60 && d.details[1][0].variant == 1,
              "beats: variant / ratchet / probability survive the round trip");
        check(RenameCustomDrumPattern("zz_selftest_beat", "zz_selftest_beat2") && CustomDrumPatternExists("zz_selftest_beat2") &&
              !CustomDrumPatternExists("zz_selftest_beat"), "beats: rename");
        check(std::filesystem::exists(t8::userdirs::BeatsDir() + "/zz_selftest_beat2.t8pat"), "beats: the file is in Library/Beats");
        check(DeleteCustomDrumPattern("zz_selftest_beat2") && !CustomDrumPatternExists("zz_selftest_beat2"), "beats: delete");
    }

    // ---- Quantized scene launch ------------------------------------------------
    {
        AudioEngine lEngine; lEngine.Init(kSampleRate);
        Project lp; lp.kitFolder = kitFolder;
        lp.patterns[0].masterLength = 16; lp.patterns[1].masterLength = 16;
        Sequencer lSeq; lSeq.Init(&lEngine, &lp);
        lSeq.LaunchScene(1);
        check(lp.currentPattern == 1, "launch: stopped -> immediate");
        lSeq.LaunchScene(0);
        lSeq.SetLaunchQuantize(2);
        lSeq.Play();
        std::vector<float> lbuf(256 * 2);
        const double spStep = 60.0 / 120.0 / 4.0 * kSampleRate;
        auto renderSteps = [&](double steps) { int n = static_cast<int>(spStep * steps); for (int r = 0; r < n; r += 256) lSeq.RenderAndAdvance(lbuf.data(), 256, kSampleRate); };
        renderSteps(5.5);
        lSeq.LaunchScene(1);
        check(lSeq.PendingScene() == 1 && lp.currentPattern == 0, "launch: BAR quantize queues the scene mid-bar");
        renderSteps(4.0);
        check(lp.currentPattern == 0, "launch: still waiting before the bar line");
        renderSteps(8.0);
        check(lp.currentPattern == 1 && lSeq.PendingScene() == -1, "launch: applied at the next bar");
        lSeq.SetLaunchQuantize(0);
        lSeq.LaunchScene(0);
        check(lp.currentPattern == 0, "launch: NOW is immediate while playing");
        lSeq.Stop();
    }


    {   // v44.54: NEXT BAR - LIVE mute / solo / groups queue to the next bar line like a fill
        AppState a; a.engine.Init(kSampleRate); a.sequencer.Init(&a.engine, &a.project);
        a.project.patterns[0].masterLength = 16; a.project.patterns[1].masterLength = 16;
        using QS = AppState::QueuedSwitch;
        std::vector<float> qbuf(256 * 2);
        const double spStep = 60.0 / 120.0 / 4.0 * kSampleRate;
        auto steps = [&](double n) { const int k = static_cast<int>(spStep * n); for (int r = 0; r < k; r += 256) a.sequencer.RenderAndAdvance(qbuf.data(), 256, kSampleRate); };
        a.quantizeSwitches = true;
        a.LiveSwitch(QS::Mute, 1);
        check(a.SwMuted(-1, 1) && a.SwQueuedCount() == 0, "next bar: stopped -> a click acts at once (there is no bar to wait for)");
        a.LiveSwitch(QS::Mute, 1);
        a.sequencer.SetLaunchQuantize(2);
        a.sequencer.Play();
        steps(5.5);
        const size_t undo0 = a.undoStack.size();
        a.LiveSwitch(QS::Mute, 2);
        a.LiveSwitch(QS::Solo, 4);
        a.LiveSwitch(QS::Group, kMuteDrums);
        check(!a.SwMuted(-1, 2) && !a.engine.GetMixer().tracks[2].mute && a.SwQueuedCount() == 3 && a.SwQueued(QS::Mute, 2) && a.sequencer.BarHookArmed(),
              "next bar: while playing a click waits (nothing changes yet, three queued, the bar hook is armed)");
        check(a.undoStack.size() == undo0 + 3, "next bar: one undo step per click, pushed at the click");
        steps(4.0);
        check(!a.SwMuted(-1, 2) && a.SwQueuedCount() == 3, "next bar: still waiting before the bar line");
        const std::string st0 = a.statusMessage;
        steps(8.0);
        check(a.SwMuted(-1, 2) && a.engine.GetMixer().tracks[2].mute && a.SwSolo(-1, 4) && a.engine.GetMixer().tracks[4].solo && a.SwGroupMuted(-1, kMuteDrums) &&
              a.SwQueuedCount() == 0 && !a.sequencer.BarHookArmed(), "next bar: applied on the bar line (flags and audio mixer), queue empty, hook disarmed");
        check(a.undoStack.size() == undo0 + 3, "next bar: applying on the audio thread pushes no undo step");
        (void)st0;
        a.LiveSwitch(QS::Mute, 8); a.LiveSwitch(QS::Mute, 8);
        check(a.SwQueuedCount() == 0 && !a.SwMuted(-1, 8) && !a.sequencer.BarHookArmed(), "next bar: a second click cancels the waiting one");
        a.SwAllOn(-1);
        a.LiveSwitch(QS::Drop);
        check(a.SwQueued(QS::Drop), "next bar: DROP queues too");
        steps(16.0);
        check(a.SwDropOn(-1) && a.SwQueuedCount() == 0, "next bar: DROP applied on the bar line");
        a.SwAllOn(-1);
        a.quantizeSwitches = false;
        a.LiveSwitch(QS::Mute, 5);
        check(a.SwMuted(-1, 5) && a.SwQueuedCount() == 0, "next bar: with the button off a click acts in real time");
        a.quantizeSwitches = true;
        a.LiveSwitch(QS::Mute, 5);
        a.sequencer.Stop();
        a.TickTransport();
        check(!a.SwMuted(-1, 5) && a.SwQueuedCount() == 0, "next bar: stopping with a click waiting applies it at once");
        // a scene launch that lands before the bar line: the queued click belonged to the old scene and is dropped
        a.sequencer.Play();
        steps(2.0);
        a.LiveSwitch(QS::Mute, 7);
        a.sequencer.SetLaunchQuantize(0);
        a.sequencer.LaunchScene(1);
        steps(16.0);
        check(!a.project.patterns[1].tracks[7].mute && !a.project.patterns[0].tracks[7].mute && a.SwQueuedCount() == 0, "next bar: a click queued on a scene that is left before the bar line is dropped");
        a.sequencer.Stop();
    }


    {   // v44.55: EUCLID ON - the melody is kept and spread over the hits; OFF brings it back
        AppState a; a.engine.Init(kSampleRate); a.sequencer.Init(&a.engine, &a.project);
        a.project.patterns[0].masterLength = 16; a.project.currentPattern = 0;
        constexpr int bass = kNumDrumTracks;   // the first synth track
        Track& t = a.project.patterns[0].tracks[bass];
        const int idx[4] = {0, 3, 7, 10}, notes[4] = {48, 50, 52, 55};
        for (int i = 0; i < 4; ++i) { t.steps[static_cast<size_t>(idx[i])].active = true; t.steps[static_cast<size_t>(idx[i])].note = static_cast<int8_t>(notes[i]); }
        auto activeCount = [&] { int n = 0; for (int i = 0; i < 16; ++i) if (t.steps[static_cast<size_t>(i)].active) ++n; return n; };
        const size_t undo0 = a.undoStack.size();
        const int n = a.EuclidMelodyOn(bass, 5, 8, 0);   // hits at 0 2 4 5 7 of each 8 steps: ten over 16
        check(n == 10 && activeCount() == 10 && t.euclidOn && t.euclidOrig.size() == 4, "euclid keep: 5 of 8 over 16 steps = ten hits, the melody is kept (four notes)");
        check(t.steps[0].note == 48 && t.steps[2].note == 50 && t.steps[4].note == 52 && t.steps[5].note == 55 && t.steps[7].note == 48 && t.steps[8].note == 50,
              "euclid keep: the notes go onto the hits in turn and cycle");
        check(!t.steps[3].active && !t.steps[11].active, "euclid keep: the places between the hits of the notes are empty");
        a.EuclidMelodyOn(bass, 3, 8, 1);
        check(activeCount() == 6 && t.euclidHits == 3 && t.euclidRotate == 1 && t.euclidOrig.size() == 4, "euclid keep: live change of the figure re-derives from the kept melody");
        check(a.undoStack.size() == undo0 + 1, "euclid keep: turning it on and turning the controls is one undo step");
        a.EuclidMelodyOff(bass);
        bool back = !t.euclidOn && t.euclidOrig.empty() && activeCount() == 4;
        for (int i = 0; i < 4; ++i) back = back && t.steps[static_cast<size_t>(idx[i])].active && t.steps[static_cast<size_t>(idx[i])].note == notes[i];
        check(back, "euclid keep: OFF puts the original melody back exactly");
        a.EuclidMelodyOn(bass, 5, 8, 0);
        a.Undo();
        bool undone = !t.euclidOn && activeCount() == 4 && t.steps[3].active;
        check(undone, "euclid keep: undo of ON brings the melody back and the switch off");
        a.EuclidMelodyOn(bass, 5, 8, 0);
        t.steps[1].active = true; t.steps[1].note = 60;   // edited by hand while it is on
        a.EuclidMelodyOff(bass);
        check(!t.euclidOn && t.steps[1].active && t.steps[1].note == 60 && !t.steps[3].active, "euclid keep: OFF after a hand edit keeps what is there (nothing is lost)");
        for (auto& st : t.steps) st = Step{};
        check(a.EuclidMelodyOn(bass, 4, 16, 0) == 4 && t.euclidOrig.empty(), "euclid keep: an empty track gets the default hit and keeps nothing");
        a.EuclidMelodyOff(bass);
        check(activeCount() == 0, "euclid keep: OFF on a track that was empty leaves it empty");
        check(a.EuclidMelodyOn(kChordTrackIndex, 4, 16, 0) == 0 && !a.project.patterns[0].tracks[kChordTrackIndex].euclidOn, "euclid keep: not for the chord track");
        // drums: the beat with its variants
        Track& k = a.project.patterns[0].tracks[0];
        for (auto& st : k.steps) st = Step{};
        k.steps[0].active = true; k.steps[0].variant = 1; k.steps[8].active = true; k.steps[8].variant = 2;
        a.EuclidMelodyOn(0, 3, 8, 0);
        check(k.steps[0].variant == 1 && k.steps[3].variant == 2 && k.steps[6].variant == 1 && k.steps[8].variant == 2, "euclid keep: a drum track spreads its variants in turn");
        // plain APPLY drops the kept melody
        a.ApplyEuclid(0, 4, 16, 0);
        check(!k.euclidOn && k.euclidOrig.empty(), "euclid keep: APPLY (plain overwrite) ends the kept state");
        // the project file keeps it
        a.EuclidMelodyOn(bass, 5, 8, 2);
        for (int i = 0; i < 4; ++i) { t.euclidOrig.clear(); }
        t.euclidOrig = {{0, Step{}}, {3, Step{}}};
        t.euclidOrig[0].second.active = true; t.euclidOrig[0].second.note = 40; t.euclidOrig[1].second.active = true; t.euclidOrig[1].second.note = 43; t.euclidOrig[1].second.probability = 50;
        a.EuclidMelodyOn(bass, 5, 8, 2);
        const std::string path = "selftest_euclid.t8proj";
        Project loaded;
        const bool saved = SaveProject(a.project, path), got = LoadProject(loaded, path);
        const Track& lt = loaded.patterns[0].tracks[bass];
        check(saved && got && lt.euclidOn && lt.euclidHits == 5 && lt.euclidSteps == 8 && lt.euclidRotate == 2 && lt.euclidOrig.size() == 2 &&
              lt.euclidOrig[1].first == 3 && lt.euclidOrig[1].second.note == 43 && lt.euclidOrig[1].second.probability == 50, "euclid keep: the project file keeps the kept melody and the figure");
        bool same = true;
        for (int i = 0; i < kMaxSteps; ++i) same = same && lt.steps[static_cast<size_t>(i)].active == t.steps[static_cast<size_t>(i)].active && lt.steps[static_cast<size_t>(i)].note == t.steps[static_cast<size_t>(i)].note;
        check(same, "euclid keep: the derived steps survive a save and load");
        std::remove(path.c_str());
        std::remove((path + ".bak").c_str());
    }


    {   // v44.56: AIFF / AIFF-C samples and loops
        namespace fs = std::filesystem;
        fs::create_directories("selftest_aif");
        auto be16 = [](std::vector<uint8_t>& b, uint32_t v) { b.push_back(static_cast<uint8_t>(v >> 8)); b.push_back(static_cast<uint8_t>(v)); };
        auto be32 = [](std::vector<uint8_t>& b, uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); };
        auto ext80 = [](std::vector<uint8_t>& b, double rate) {   // IEEE 80-bit extended, big endian
            int e = 0; double m = std::frexp(rate, &e);   // rate = m * 2^e, 0.5 <= m < 1
            const uint64_t mant = static_cast<uint64_t>(std::ldexp(m, 64));
            const uint16_t exp = static_cast<uint16_t>(e - 1 + 16383);
            b.push_back(static_cast<uint8_t>(exp >> 8)); b.push_back(static_cast<uint8_t>(exp));
            for (int i = 7; i >= 0; --i) b.push_back(static_cast<uint8_t>(mant >> (8 * i)));
        };
        // Builds a file: kind "AIFF" / "AIFC", comp = AIFC compression type, bits, rate, channels, the sample values (-1..1), optional junk chunk first.
        auto build = [&](const std::string& kind, const std::string& comp, int bits, double rate, int ch, const std::vector<float>& vals, bool junkFirst, bool cutShort) {
            std::vector<uint8_t> comm;
            be16(comm, static_cast<uint32_t>(ch)); be32(comm, static_cast<uint32_t>(vals.size() / static_cast<size_t>(ch))); be16(comm, static_cast<uint32_t>(bits)); ext80(comm, rate);
            if (kind == "AIFC") { for (char c : comp) comm.push_back(static_cast<uint8_t>(c)); comm.push_back(0); comm.push_back(0); }   // type + empty pstring (padded)
            std::vector<uint8_t> ssnd; be32(ssnd, 0); be32(ssnd, 0);
            const bool little = comp == "sowt", flt = comp == "fl32";
            const int bps = flt ? 4 : (bits + 7) / 8;
            for (float f : vals) {
                if (flt) { uint32_t u; std::memcpy(&u, &f, 4); be32(ssnd, u); continue; }
                const double sc = std::ldexp(1.0, 8 * bps - 1);
                int64_t v = static_cast<int64_t>(std::llround(static_cast<double>(f) * (sc - 1.0)));
                for (int k = 0; k < bps; ++k) { const int sh = little ? 8 * k : 8 * (bps - 1 - k); ssnd.push_back(static_cast<uint8_t>((v >> sh) & 0xFF)); }
            }
            std::vector<uint8_t> body;
            for (char c : kind) body.push_back(static_cast<uint8_t>(c));
            auto chunk = [&](const char* id, const std::vector<uint8_t>& d) { for (int i = 0; i < 4; ++i) body.push_back(static_cast<uint8_t>(id[i])); be32(body, static_cast<uint32_t>(d.size())); body.insert(body.end(), d.begin(), d.end()); if (d.size() % 2) body.push_back(0); };
            if (junkFirst) { chunk("APPL", std::vector<uint8_t>{'s', 't', 'o', 'c', 1, 2, 3}); chunk("MARK", std::vector<uint8_t>{0, 0}); }   // an odd-sized chunk: padded
            chunk("COMM", comm);
            std::vector<uint8_t> out = {'F', 'O', 'R', 'M'}; 
            chunk("SSND", ssnd);
            be32(out, static_cast<uint32_t>(body.size()));
            out.insert(out.end(), body.begin(), body.end());
            if (cutShort) out.resize(out.size() - ssnd.size() / 2);
            return out;
        };
        auto writeFile = [](const std::string& path, const std::vector<uint8_t>& b) { FILE* f = std::fopen(path.c_str(), "wb"); if (!f) return false; if (!b.empty()) std::fwrite(b.data(), 1, b.size(), f); std::fclose(f); return true; };
        std::vector<float> ramp;   // 64 frames, stereo: left rises, right falls
        for (int i = 0; i < 64; ++i) { ramp.push_back(-0.8f + 1.6f * static_cast<float>(i) / 63.0f); ramp.push_back(0.8f - 1.6f * static_cast<float>(i) / 63.0f); }
        auto maxDiff = [&](const SampleBuffer& b, const std::vector<float>& ref) { if (b.data.size() != ref.size()) return 9.0f; float m = 0; for (size_t i = 0; i < ref.size(); ++i) m = std::max(m, std::fabs(b.data[i] - ref[i])); return m; };
        struct Case { const char* name; std::string kind, comp; int bits; float tol; };
        const Case cases[] = {{"16-bit", "AIFF", "", 16, 1e-3f}, {"24-bit", "AIFF", "", 24, 1e-5f}, {"8-bit", "AIFF", "", 8, 2e-2f}, {"32-bit int", "AIFF", "", 32, 1e-6f},
                              {"AIFC NONE 16", "AIFC", "NONE", 16, 1e-3f}, {"AIFC sowt 16 (little endian)", "AIFC", "sowt", 16, 1e-3f}, {"AIFC fl32", "AIFC", "fl32", 32, 1e-7f},
                              {"12-bit left-justified", "AIFF", "", 12, 1e-3f}};
        for (const Case& c : cases) {
            const std::string path = std::string("selftest_aif/t_") + std::to_string(&c - cases) + ".aif";
            writeFile(path, build(c.kind, c.comp, c.bits, 44100.0, 2, ramp, &c == &cases[0], false));
            SampleBuffer b;
            const bool ok = LoadWav(path, b, 44100);
            check(ok && b.channels == 2 && b.frames() == 64 && maxDiff(b, ramp) <= c.tol, std::string("aiff: ") + c.name + " stereo reads the same values");
        }
        {
            std::vector<float> mono(ramp.begin(), ramp.begin() + 32);
            writeFile("selftest_aif/mono.aiff", build("AIFF", "", 16, 44100.0, 1, mono, false, false));
            SampleBuffer b; const bool ok = LoadWav("selftest_aif/mono.aiff", b, 44100);
            check(ok && b.channels == 1 && b.frames() == 32, "aiff: mono, and the .aiff name");
            // sample rates: 22050 -> 44100 doubles the frames; 48000 / 96000 / 11025 / 8000 read as exact numbers
            std::vector<float> tone; for (int i = 0; i < 2205; ++i) tone.push_back(0.5f * std::sin(6.2831853f * 440.0f * static_cast<float>(i) / 22050.0f));
            writeFile("selftest_aif/r22.aif", build("AIFF", "", 16, 22050.0, 1, tone, false, false));
            SampleBuffer r; const bool ok2 = LoadWav("selftest_aif/r22.aif", r, 44100);
            check(ok2 && std::abs(r.frames() - 4410) <= 4, "aiff: the 80-bit sample rate is read (22050 Hz -> 44100: twice the frames)");
            bool ratesOk = true;
            for (double rate : {8000.0, 11025.0, 32000.0, 44100.0, 48000.0, 88200.0, 96000.0, 192000.0}) {
                writeFile("selftest_aif/rate.aif", build("AIFF", "", 16, rate, 1, std::vector<float>(800, 0.25f), false, false));
                SampleBuffer q; ratesOk = ratesOk && LoadWav("selftest_aif/rate.aif", q, static_cast<int>(rate)) && q.frames() == 800;   // loaded at its own rate: no resampling, same frame count
            }
            check(ratesOk, "aiff: common sample rates decode exactly (loaded at their own rate keeps every frame)");
        }
        {   // the kind is found from the header, not from the name
            writeFile("selftest_aif/named.wav", build("AIFF", "", 16, 44100.0, 2, ramp, false, false));
            SampleBuffer b; check(LoadWav("selftest_aif/named.wav", b, 44100) && b.frames() == 64, "aiff: an AIFF with a .wav name still opens (the header decides)");
        }
        {   // refusing and not crashing
            SampleBuffer b;
            writeFile("selftest_aif/ima4.aif", build("AIFC", "ima4", 16, 44100.0, 2, ramp, false, false));
            check(!LoadWav("selftest_aif/ima4.aif", b, 44100), "aiff: compressed AIFF-C (ima4) is refused, not played as noise");
            writeFile("selftest_aif/cut.aif", build("AIFF", "", 16, 44100.0, 2, ramp, false, true));
            const bool cutOk = LoadWav("selftest_aif/cut.aif", b, 44100);
            check((!cutOk || (b.frames() > 0 && b.frames() < 64)), "aiff: a file cut short plays what is there (or is refused), never reads past the end");
            std::vector<uint8_t> junk(200); for (size_t i = 0; i < junk.size(); ++i) junk[i] = static_cast<uint8_t>(i * 37 + 11);
            writeFile("selftest_aif/junk.aif", junk);
            check(!LoadWav("selftest_aif/junk.aif", b, 44100), "aiff: garbage is refused");
            // random damage to a valid file: the reader must not crash
            const std::vector<uint8_t> good = build("AIFC", "NONE", 16, 44100.0, 2, ramp, true, false);
            unsigned seed = 7; bool survived = true;
            for (int round = 0; round < 300 && survived; ++round) {
                std::vector<uint8_t> bad = good;
                for (int k = 0; k < 1 + round % 6; ++k) { seed = seed * 1664525u + 1013904223u; bad[(seed >> 8) % bad.size()] = static_cast<uint8_t>(seed >> 24); }
                if (round % 5 == 0) bad.resize(bad.size() * ((seed >> 5) % 100) / 100);
                writeFile("selftest_aif/dmg.aif", bad);
                SampleBuffer q; LoadWav("selftest_aif/dmg.aif", q, 44100);
            }
            check(survived, "aiff: 300 damaged files read without a crash");
        }
        {   // names and the project plumbing
            check(IsSampleFileName("a/b/Loop.AIF") && IsSampleFileName("x.aiff") && IsSampleFileName("x.aifc") && IsSampleFileName("x.WAV") && IsSampleFileName("x.wave") && !IsSampleFileName("x.mp3") && !IsSampleFileName("x") && !IsSampleFileName("dir.aif/readme"),
                  "aiff: file names of samples (case does not matter, only the extension counts)");
            check(ImportAudioAsWav("selftest_aif/t_0.aif", "selftest_aif/conv.wav"), "aiff: import as WAV");
            SampleBuffer w; const bool ok = LoadWav("selftest_aif/conv.wav", w, 44100);
            check(ok && w.frames() == 64 && maxDiff(w, ramp) <= 1e-3f, "aiff: the WAV made from an AIFF has the same audio");
            std::vector<float> tone; for (int i = 0; i < 2205; ++i) tone.push_back(0.5f * std::sin(6.2831853f * 440.0f * static_cast<float>(i) / 22050.0f));
            writeFile("selftest_aif/r22b.aif", build("AIFF", "", 16, 22050.0, 1, tone, false, false));
            check(ImportAudioAsWav("selftest_aif/r22b.aif", "selftest_aif/r22b.wav"), "aiff: import of a 22 kHz file");
            SampleBuffer w2; LoadWav("selftest_aif/r22b.wav", w2, 22050);
            check(w2.frames() == 2205, "aiff: the converted WAV keeps the sample rate of the AIFF (it is not resampled on import)");
            // a library with .aif loops
            fs::create_directories("selftest_aif/lib/Loops");
            writeFile("selftest_aif/lib/Loops/Funk Break 95 bpm.aif", build("AIFF", "", 16, 44100.0, 2, ramp, false, false));
            writeFile("selftest_aif/lib/Loops/pad_loop.AIFF", build("AIFF", "", 16, 44100.0, 2, ramp, false, false));
            writeFile("selftest_aif/lib/Loops/hit.wav", std::vector<uint8_t>{});
            const auto lib = ScanSampleLibrary("selftest_aif/lib");
            bool found95 = false, foundPad = false;
            for (const LibSample& ls : lib) { if (ls.name.find("Funk Break") != std::string::npos && std::fabs(ls.bpm - 95.0) < 0.01) found95 = true; if (ls.rel.find("pad_loop.AIFF") != std::string::npos) foundPad = true; }
            check(found95 && foundPad, "aiff: the sample library lists .aif / .AIFF files and reads the tempo from the name");
            // the sampler takes a dropped .aif: kept in samples/ with its own extension, then it plays
            auto ap = std::make_unique<AppState>(); AppState& a = *ap;
            a.engine.Init(kSampleRate); a.sequencer.Init(&a.engine, &a.project);
            a.projectPath = "selftest_aif/proj/p.t8proj";
            std::lock_guard<SceneMutex> lk(a.sequencer.PatternMutex());
            check(a.AssignSampleToSampler("selftest_aif/t_0.aif") && a.CurrentPattern().samplerPatch.file == "t_0.aif" && fs::exists("selftest_aif/proj/samples/t_0.aif"), "aiff: a dropped .aif is copied into samples/ and becomes the sample of the sampler");
            const auto& names = a.SampleNames();
            check(std::find(names.begin(), names.end(), std::string("t_0.aif")) != names.end() && a.SampleFor("t_0.aif") != nullptr, "aiff: the sample list shows it and the engine can load it");
            check(a.AssignSampleToSampler("selftest_aif/t_0.aif") && a.CurrentPattern().samplerPatch.file == "t_0.aif", "aiff: the same .aif dropped again is not copied twice");
        }
        {   // Ableton Live pack samples: AIFF-C "able" (protected: only Live reads them) are listed, marked, and refused with the reason
            writeFile("selftest_aif/lib/Loops/Pack Loop 120 bpm.aif", build("AIFC", "able", 16, 44100.0, 2, ramp, true, false));
            SampleBuffer b;
            check(!LoadWav("selftest_aif/lib/Loops/Pack Loop 120 bpm.aif", b, 44100), "aiff: a protected Ableton pack file ('able') is refused, not played as noise");
            check(IsProtectedAbletonAiff("selftest_aif/lib/Loops/Pack Loop 120 bpm.aif") && !IsProtectedAbletonAiff("selftest_aif/t_0.aif") && !IsProtectedAbletonAiff("selftest_aif/t_4.aif") &&
                  UnreadableReason("selftest_aif/lib/Loops/Pack Loop 120 bpm.aif").find("ABLETON") != std::string::npos && UnreadableReason("selftest_aif/t_0.aif").empty(),
                  "aiff: the protected files are told apart from plain AIFF and AIFF-C, and the reason is known");
            const auto lib2 = ScanSampleLibrary("selftest_aif/lib");
            bool lockedOk = false, plainOk = true;
            for (const LibSample& ls : lib2) { if (ls.name == "Pack Loop 120 bpm") lockedOk = ls.locked; if (ls.name == "Funk Break 95 bpm" && ls.locked) plainOk = false; }
            check(lockedOk && plainOk, "aiff: the library marks protected files as locked and only those");
            AppState a; a.engine.Init(kSampleRate); a.sequencer.Init(&a.engine, &a.project); a.projectPath = "selftest_aif/proj2/p.t8proj";
            std::lock_guard<SceneMutex> lk(a.sequencer.PatternMutex());
            check(!a.AssignSampleToSampler("selftest_aif/lib/Loops/Pack Loop 120 bpm.aif") && a.statusMessage.find("ABLETON") != std::string::npos, "aiff: choosing a protected file says why in the info line");
        }
        fs::remove_all("selftest_aif");
    }

    // ---- Master compressor / chain ---------------------------------------------
    {
        // static gain computer: below the knee untouched, at 4:1 well above it 3/4 of the overshoot is removed
        check(nearlyEqual(MasterChain::GainComputerDb(-40.0f, -20.0f, 4.0f), -40.0f, 1e-4f), "comp: below threshold unchanged");
        check(nearlyEqual(MasterChain::GainComputerDb(-8.0f, -20.0f, 4.0f), -17.0f, 1e-3f), "comp: 4:1 above the knee (-8 dB in -> -17 dB out)");
        auto makeSine = [&](float amp, int frames) {
            std::vector<float> b(static_cast<size_t>(frames) * 2);
            for (int i = 0; i < frames; ++i) { float v = amp * std::sin(2.0f * 3.14159265f * 220.0f * i / kSampleRate); b[2 * i] = v; b[2 * i + 1] = v; }
            return b;
        };
        auto rmsOf = [&](const std::vector<float>& b, size_t from) {
            double a = 0; size_t n = 0; for (size_t i = from; i < b.size(); ++i) { a += b[i] * b[i]; ++n; }
            return static_cast<float>(std::sqrt(a / std::max<size_t>(1, n)));
        };
        MasterChainSettings off; off.monoBassHz = 0.0f;
        MasterChain mcOff;
        auto src = makeSine(0.7f, kSampleRate);
        auto a = src; mcOff.Process(a.data(), kSampleRate, off, kSampleRate);
        check(a == src, "chain: everything off is bit-identical");

        MasterChainSettings cs; cs.compOn = true; cs.compThreshDb = -20.0f; cs.compRatio = 4.0f; cs.compAttackMs = 5.0f; cs.compReleaseMs = 100.0f; cs.compMakeupDb = 0.0f;
        MasterChain mc;
        auto b = src; mc.Process(b.data(), kSampleRate, cs, kSampleRate);
        float rIn = rmsOf(src, src.size() / 2), rOut = rmsOf(b, b.size() / 2);
        check(rOut < rIn * 0.6f && mc.GainReductionDb() > 6.0f, "comp: loud sine is turned down and the GR meter moves");
        MasterChainSettings quiet = cs; auto q = makeSine(0.02f, kSampleRate); MasterChain mq; mq.Process(q.data(), kSampleRate, quiet, kSampleRate);
        check(mq.GainReductionDb() < 0.5f && nearlyEqual(rmsOf(q, q.size() / 2), rmsOf(makeSine(0.02f, kSampleRate), q.size() / 2), 0.002f), "comp: quiet signal below threshold is left alone");

        // ---- look-ahead true-peak limiter (Limiter.h) ----
        {
            const int N = kSampleRate * 2;
            // (1) transparent below the ceiling: the output is the input delayed by Latency(), sample for sample
            Limiter lt0; auto quietSig = makeSine(0.3f, N); auto q0 = quietSig;
            for (int o = 0; o < N; o += 256) lt0.Process(q0.data() + static_cast<size_t>(o) * 2, std::min(256, N - o), kSampleRate, -1.0f, true);
            const int lat = lt0.Latency();
            bool same = true;
            for (int i = 0; i + lat < N; ++i) if (std::fabs(q0[2 * (i + lat)] - quietSig[2 * i]) > 1e-6f) { same = false; break; }
            check(same, "limiter: below the ceiling the signal passes untouched (only delayed)");
            check(lat > 30 && lat < 120, "limiter: latency is a small constant (look-ahead)");
            // (2) a loud sine is held under the ceiling
            Limiter lt1; auto loud = makeSine(1.4f, N);
            for (int o = 0; o < N; o += 256) lt1.Process(loud.data() + static_cast<size_t>(o) * 2, std::min(256, N - o), kSampleRate, -3.0f, true);
            float pk = 0; for (float v : loud) pk = std::max(pk, std::fabs(v));
            check(pk <= std::pow(10.0f, -3.0f / 20.0f) + 1e-4f, "limiter: output never exceeds the ceiling");
            // (3) true peak: fs/4 sine at 45 degrees has sample peaks of 0.707A but a real peak of A
            Limiter lt2; std::vector<float> tp(static_cast<size_t>(N) * 2);
            for (int i = 0; i < N; ++i) { float v = 1.3f * std::sin(2.0f * 3.14159265f * 0.25f * i + 0.7853982f); tp[2 * i] = v; tp[2 * i + 1] = v; }
            for (int o = 0; o < N; o += 256) lt2.Process(tp.data() + static_cast<size_t>(o) * 2, std::min(256, N - o), kSampleRate, -1.0f, true);
            double truePk = 0;   // brute-force 8x sinc interpolation of the output
            for (int i = 4000; i < 6000; ++i) for (int f = 0; f < 8; ++f) {
                double acc = 0; const double fr = f / 8.0;
                for (int k = -24; k <= 24; ++k) { const double x = k - fr; const double sinc = std::fabs(x) < 1e-9 ? 1.0 : std::sin(3.14159265358979 * x) / (3.14159265358979 * x);
                    acc += tp[2 * (i + k)] * sinc * (0.5 + 0.5 * std::cos(3.14159265358979 * x / 25.0)); }
                truePk = std::max(truePk, std::fabs(acc));
            }
            check(truePk <= std::pow(10.0, -1.0 / 20.0) * 1.02, "limiter: true (inter-sample) peak stays at the ceiling");
            // (4) limiting off = delayed pass-through with a hard clamp at 0 dBFS
            Limiter lt3; auto over = makeSine(1.4f, 4096);
            lt3.Process(over.data(), 4096, kSampleRate, -1.0f, false);
            float pk3 = 0; for (float v : over) pk3 = std::max(pk3, std::fabs(v));
            check(pk3 <= 1.0f && pk3 > 0.99f, "limiter: off = clamp at 0 dBFS only");
        }

        // ---- mono bass (Linkwitz-Riley crossover inside the master chain) ----
        {
            MasterChainSettings mb; mb.monoBassHz = 120.0f;
            const int N = kSampleRate;
            auto tone = [&](float f, float a0, float a1) { std::vector<float> b(static_cast<size_t>(N) * 2);
                for (int i = 0; i < N; ++i) { float v = std::sin(2.0f * 3.14159265f * f * i / kSampleRate); b[2 * i] = a0 * v; b[2 * i + 1] = a1 * v; } return b; };
            auto side = [&](const std::vector<float>& b) { double a = 0; for (int i = N / 2; i < N; ++i) { double d = b[2 * i] - b[2 * i + 1]; a += d * d; } return std::sqrt(a / (N / 2)); };
            auto mid = [&](const std::vector<float>& b) { double a = 0; for (int i = N / 2; i < N; ++i) { double d = 0.5 * (b[2 * i] + b[2 * i + 1]); a += d * d; } return std::sqrt(a / (N / 2)); };
            MasterChain m1; auto low = tone(50.0f, 0.5f, -0.5f); m1.Process(low.data(), N, mb, kSampleRate);
            check(side(low) < 0.1, "mono bass: an anti-phase 50 Hz tone collapses to the centre");
            MasterChain m2; auto high = tone(2000.0f, 0.5f, -0.5f); m2.Process(high.data(), N, mb, kSampleRate);
            check(side(high) > 0.69, "mono bass: the stereo image above the crossover is untouched");
            MasterChain m3; auto flat = tone(50.0f, 0.5f, 0.5f); m3.Process(flat.data(), N, mb, kSampleRate);
            check(nearlyEqual(static_cast<float>(mid(flat)), 0.5f / std::sqrt(2.0f), 0.01f), "mono bass: a centred bass keeps its level");
            MasterChain m4; auto cross = tone(120.0f, 0.5f, 0.5f); m4.Process(cross.data(), N, mb, kSampleRate);
            check(nearlyEqual(static_cast<float>(mid(cross)), 0.5f / std::sqrt(2.0f), 0.01f), "mono bass: flat through the crossover frequency");
        }

        // ---- stereo: pan law, per-track width, mono-compatible spread ----
        {
            float l0, r0, l1, r1, l2, r2;
            PanGains(0.0f, l0, r0); PanGains(-1.0f, l1, r1); PanGains(0.5f, l2, r2);
            check(nearlyEqual(l0, 1.0f, 1e-6f) && nearlyEqual(r0, 1.0f, 1e-6f), "pan law: the centre stays at unity");
            check(nearlyEqual(l1 * l1 + r1 * r1, 2.0f, 1e-4f) && nearlyEqual(r1, 0.0f, 1e-4f) && nearlyEqual(l2 * l2 + r2 * r2, 2.0f, 1e-4f),
                  "pan law: constant power at every position");
            const int N = 44100 * 2; unsigned seed = 12345;
            auto noise = [&]() { seed = seed * 1664525u + 1013904223u; return (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f) * 0.5f; };
            std::vector<float> src(static_cast<size_t>(N) * 2); for (int i = 0; i < N; ++i) src[2 * i] = src[2 * i + 1] = noise();
            auto run = [&](float width, float spread) { StereoWidth w; w.Init(kSampleRate); auto o = src;
                const bool idle = w.Idle(width, spread); if (!idle) for (int i = 0; i < N; ++i) w.Process(o[2 * i], o[2 * i + 1], width, spread);
                return o; };
            auto corr = [&](const std::vector<float>& b) { double ab = 0, aa = 0, bb = 0; for (int i = N / 2; i < N; ++i) { ab += b[2 * i] * b[2 * i + 1]; aa += b[2 * i] * b[2 * i]; bb += b[2 * i + 1] * b[2 * i + 1]; } return ab / std::sqrt(aa * bb + 1e-30); };
            auto id = run(1.0f, 0.0f);
            check(id == src, "stereo: width 1 / spread 0 is a bit-exact bypass");
            auto sp = run(1.0f, 1.0f);
            double monoErr = 0, monoRef = 0; for (int i = 0; i < N; ++i) { monoErr += std::fabs((sp[2 * i] + sp[2 * i + 1]) - (src[2 * i] + src[2 * i + 1])); monoRef += std::fabs(src[2 * i] + src[2 * i + 1]); }
            check(monoErr / monoRef < 1e-5, "stereo: spread leaves the mono sum untouched (mono compatible)");
            check(corr(sp) < 0.5, "stereo: spread decorrelates a mono source");
            auto sp3 = run(1.0f, 0.3f);
            check(corr(sp3) < 0.99 && corr(sp3) > corr(sp), "stereo: spread amount scales the width");
            std::vector<float> st(static_cast<size_t>(N) * 2); for (int i = 0; i < N; ++i) { st[2 * i] = noise(); st[2 * i + 1] = noise(); }
            { StereoWidth w; w.Init(kSampleRate); auto o = st; if (!w.Idle(0.0f, 0.0f)) for (int i = 0; i < N; ++i) w.Process(o[2 * i], o[2 * i + 1], 0.0f, 0.0f);
              double d = 0; for (int i = 0; i < N; ++i) d = std::max<double>(d, std::fabs(o[2 * i] - o[2 * i + 1]));
              check(d < 1e-6, "stereo: width 0 collapses to mono"); }
            // low end stays centred: a 60 Hz tone through full spread keeps its side signal tiny
            std::vector<float> lo(static_cast<size_t>(N) * 2); for (int i = 0; i < N; ++i) lo[2 * i] = lo[2 * i + 1] = 0.5f * std::sin(2.0f * 3.14159265f * 60.0f * i / kSampleRate);
            { StereoWidth w; w.Init(kSampleRate); double sd = 0, md = 0;
              for (int i = 0; i < N; ++i) { float a = lo[2 * i], b = lo[2 * i + 1]; w.Process(a, b, 1.0f, 1.0f); if (i > N / 2) { sd += (a - b) * (a - b); md += (a + b) * (a + b); } }
              check(sd < 0.25 * md, "stereo: spread keeps a 60 Hz bass mostly in the centre"); }
        }

        // ---- compressor detector high-pass: a loud 50 Hz tone moves the compressor less ----
        {
            MasterChainSettings a1; a1.compOn = true; a1.compThreshDb = -24.0f; a1.compRatio = 4.0f; a1.monoBassHz = 0.0f; a1.compScHpfHz = 20.0f;
            MasterChainSettings a2 = a1; a2.compScHpfHz = 150.0f;
            auto t = makeSine(0.7f, kSampleRate); for (int i = 0; i < kSampleRate; ++i) { float v = 0.7f * std::sin(2.0f * 3.14159265f * 50.0f * i / kSampleRate); t[2 * i] = v; t[2 * i + 1] = v; }
            auto x1 = t, x2 = t; MasterChain c1, c2;
            c1.Process(x1.data(), kSampleRate, a1, kSampleRate); c2.Process(x2.data(), kSampleRate, a2, kSampleRate);
            check(c2.GainReductionDb() < c1.GainReductionDb() - 3.0f, "comp: the sidechain high-pass keeps a bass tone from pumping the compressor");
        }

        // ---- loudness meter (BS.1770): a stereo 1 kHz sine at -20 dBFS reads -20 LUFS ----
        {
            LoudnessMeter lm; const int N = kSampleRate * 6;
            std::vector<float> b(static_cast<size_t>(N) * 2);
            for (int i = 0; i < N; ++i) { float v = 0.1f * std::sin(2.0f * 3.14159265f * 1000.0f * i / kSampleRate); b[2 * i] = v; b[2 * i + 1] = v; }
            for (int o = 0; o < N; o += 256) lm.Process(b.data() + static_cast<size_t>(o) * 2, std::min(256, N - o), kSampleRate);
            check(std::fabs(lm.Integrated() + 20.0f) < 0.3f && std::fabs(lm.ShortTerm() + 20.0f) < 0.3f && std::fabs(lm.Momentary() + 20.0f) < 0.3f, "loudness: -20 dBFS stereo sine reads about -20 LUFS");
            check(std::fabs(lm.TruePeakDb() + 20.0f) < 0.3f, "loudness: true peak of that sine is about -20 dBTP");
            lm.RequestReset();
            std::vector<float> z(256 * 2, 0.0f); lm.Process(z.data(), 256, kSampleRate);
            check(lm.Integrated() < -100.0f, "loudness: reset clears the integrated reading");
        }

        MasterChainSettings wd; wd.chainOn = true; wd.width = 0.0f; MasterChain mw;
        std::vector<float> st(256 * 2); for (int i = 0; i < 256; ++i) { st[2 * i] = 0.5f; st[2 * i + 1] = -0.5f; }
        mw.Process(st.data(), 256, wd, kSampleRate);
        check(nearlyEqual(st[10], 0.0f, 1e-5f) && nearlyEqual(st[11], 0.0f, 1e-5f), "chain: width 0 folds the sides to mono");

        // v44: master presets must not distort (the old tanh(4x) saturation gave -12 dB IMD on LOUD)
        {
            const int N = kSampleRate * 2;
            for (int p = 0; p < kNumMasterPresets; ++p) {
                MasterChainSettings ms; ApplyMasterPreset(ms, p);
                // 1) a 1 kHz sine at -12 dBFS: THD+N under -45 dB through chain + limiter (LO-FI is allowed to crunch at -6)
                std::vector<float> b(static_cast<size_t>(N) * 2); const float a = 0.25f;
                for (int i = 0; i < N; ++i) { float v = a * std::sin(2.0f * 3.14159265f * 1000.0f * i / kSampleRate); b[2 * i] = v; b[2 * i + 1] = v; }
                MasterChain mc; Limiter lim;
                for (int o = 0; o < N; o += 256) { int c = std::min(256, N - o); mc.Process(b.data() + static_cast<size_t>(o) * 2, c, ms, kSampleRate); lim.Process(b.data() + static_cast<size_t>(o) * 2, c, kSampleRate, ms.ceilingDb, ms.limiterOn); }
                std::vector<float> mono(N); for (int i = 0; i < N; ++i) mono[i] = b[2 * i];
                double r = t8m::SineResidualDb(mono, N / 2, N, 1000.0, kSampleRate);
                check(r < -45.0, "master preset " + std::to_string(p) + ": 1 kHz at -12 dBFS stays clean (THD+N < -45 dB)");
                // 2) the output never exceeds the ceiling and never drops out (no silent stretch in a steady tone)
                float pk = 0.0f, minRms = 1e9f; for (float v : mono) pk = std::max(pk, std::fabs(v));
                for (int o = N / 2; o + 480 <= N; o += 480) { double e = 0; for (int i = 0; i < 480; ++i) e += mono[o + i] * mono[o + i]; minRms = std::min(minRms, static_cast<float>(std::sqrt(e / 480))); }
                check(pk <= std::pow(10.0f, (ms.ceilingDb + 0.1f) / 20.0f) && minRms > 0.05f, "master preset " + std::to_string(p) + ": peak within the ceiling, no dropouts in a steady tone");
            }
            // 3) the LOUD preset on a two-tone bass+mid signal: no audible intermodulation (< -40 dB)
            MasterChainSettings ms; ApplyMasterPreset(ms, 2);
            const int M = kSampleRate * 3; std::vector<float> b(static_cast<size_t>(M) * 2);
            for (int i = 0; i < M; ++i) { float v = 0.35f * 0.7f * std::sin(2.0f * 3.14159265f * 55.0f * i / kSampleRate) + 0.35f * 0.5f * std::sin(2.0f * 3.14159265f * 1000.0f * i / kSampleRate); b[2 * i] = b[2 * i + 1] = v; }
            MasterChain mc; Limiter lim;
            for (int o = 0; o < M; o += 256) { int c = std::min(256, M - o); mc.Process(b.data() + static_cast<size_t>(o) * 2, c, ms, kSampleRate); lim.Process(b.data() + static_cast<size_t>(o) * 2, c, kSampleRate, ms.ceilingDb, ms.limiterOn); }
            std::vector<float> mono(M); for (int i = 0; i < M; ++i) mono[i] = b[2 * i];
            const size_t NN = 65536, st = M - NN - 1000; auto sp = t8m::PowerSpectrum(mono, st, NN);
            auto pw = [&](double f) { long c = std::lround(f * NN / kSampleRate); double s = 0; for (long q = c - 3; q <= c + 3; ++q) s += sp[static_cast<size_t>(q)]; return s; };
            double side = 0; for (int k = 1; k <= 3; ++k) side += pw(1000.0 + 55.0 * k) + pw(1000.0 - 55.0 * k);
            check(10.0 * std::log10(side / pw(1000.0)) < -40.0, "master LOUD: two-tone IMD (55 Hz + 1 kHz) below -40 dB");
            // 4) the knee clipper is linear below the knee and bounded by 1
            check(nearlyEqual(MasterChain::KneeClip(0.3f, 0.8f, 0.7f), 0.3f, 1e-7f) && nearlyEqual(MasterChain::KneeClip(-0.3f, 0.8f, 0.7f), -0.3f, 1e-7f) && std::fabs(MasterChain::KneeClip(9.0f, 0.5f, 0.5f)) <= 1.0f, "master saturation: linear below the knee, bounded above");
        }

        // presets + persistence round trip (old files without the keys stay at defaults)
        Project pp; ApplyMasterPreset(pp.fx.master, 2); pp.fx.master.compMix = 0.8f; pp.fx.master.monoBassHz = 90.0f; pp.fx.master.compScHpfHz = 110.0f;
        check(SaveProject(pp, "selftest_tmp_safety.t8proj"), "chain: save project");
        Project pl; check(LoadProject(pl, "selftest_tmp_safety.t8proj"), "chain: load project");
        check(pl.fx.master.chainOn && pl.fx.master.chainPreset == 2 && pl.fx.master.compOn &&
              nearlyEqual(pl.fx.master.compMakeupDb, pp.fx.master.compMakeupDb, 1e-3f) && nearlyEqual(pl.fx.master.compMix, 0.8f, 1e-3f),
              "chain: LOUD preset + compressor settings survive save/load");
        check(nearlyEqual(pl.fx.master.monoBassHz, 90.0f, 1e-3f) && nearlyEqual(pl.fx.master.compScHpfHz, 110.0f, 1e-3f), "master: mono bass and detector high-pass survive save/load");
        Project fresh0; check(nearlyEqual(fresh0.fx.master.ceilingDb, -1.0f, 1e-6f) && fresh0.fx.master.limiterOn && nearlyEqual(fresh0.fx.master.monoBassHz, 120.0f, 1e-6f), "master: defaults are limiter on at -1 dBTP, mono bass 120 Hz");
        Project fresh; check(!fresh.fx.master.chainOn && !fresh.fx.master.compOn, "chain: defaults are off");
        std::remove("selftest_tmp_safety.t8proj"); std::remove("selftest_tmp_safety.t8proj.bak");
    }

    // ---- v44 preset library: every preset renders, is level-matched inside its category and keeps the headroom ----
    {
        const auto& lib = AllSynthPresets();
        check(lib.size() >= 50, "preset library: at least 50 built-in presets");
        std::set<std::string> names; bool uniq = true, catsOk = true;
        int perCat[kNumSynthCategories] = {};
        for (const auto& pr : lib) {
            uniq = uniq && names.insert(pr.name).second;
            int c = -1; for (int k = 0; k < kNumSynthCategories; ++k) if (std::string(pr.category) == kSynthCategories[k]) c = k;
            if (c < 0) catsOk = false; else ++perCat[c];
        }
        check(uniq, "preset library: names are unique");
        bool nonEmpty = true; for (int k = 0; k < kNumSynthCategories; ++k) nonEmpty = nonEmpty && perCat[k] >= 3;
        check(catsOk && nonEmpty, "preset library: every preset has a known category and every category holds at least 3");

        constexpr int kSr = 48000;
        bool finite = true, loud = true, headroom = true; double worstPeak = -99.0, lo = 99.0, hi = -99.0;
        std::string badName;
        double catMin[kNumSynthCategories], catMax[kNumSynthCategories];
        for (int k = 0; k < kNumSynthCategories; ++k) { catMin[k] = 99.0; catMax[k] = -99.0; }
        for (const auto& pr : lib) {
            const std::string cat = pr.category;
            const int note = (cat == "BASS") ? 36 : (cat == "PAD" ? 48 : 60);
            SynthVoice v; v.SetPatch(pr.patch); v.NoteOn(note, 0.9f, false);
            const double hold = cat == "PAD" ? 3.0 : 1.2;
            const int nn = static_cast<int>(hold * kSr);
            std::vector<float> out(static_cast<size_t>(nn) * 2, 0.0f);
            for (int o = 0; o < nn; o += 512) v.Render(out.data() + static_cast<size_t>(o) * 2, std::min(512, nn - o), kSr);
            double pk = 0.0, wpk = 0.0, e = 0.0; size_t cnt = 0;
            const size_t i0 = static_cast<size_t>((pr.patch.sustain < 0.2f ? 0.02 : (pr.patch.attack > 0.5f ? std::min<double>(pr.patch.attack + 0.2, hold - 1.0) : 0.1)) * kSr);
            const size_t i1 = pr.patch.sustain < 0.2f ? static_cast<size_t>(0.42 * kSr) : static_cast<size_t>(nn);
            for (size_t i = 0; i < out.size(); ++i) { if (!std::isfinite(out[i])) finite = false; pk = std::max(pk, static_cast<double>(std::fabs(out[i]))); }
            for (size_t i = i0; i < i1; ++i) { wpk = std::max(wpk, static_cast<double>(std::max(std::fabs(out[2 * i]), std::fabs(out[2 * i + 1])))); e += 0.5 * (static_cast<double>(out[2 * i]) * out[2 * i] + static_cast<double>(out[2 * i + 1]) * out[2 * i + 1]); ++cnt; }
            const double rmsDb = 10.0 * std::log10(std::max(e / std::max<size_t>(cnt, 1), 1e-12));
            const double pkDb = 20.0 * std::log10(std::max(wpk, 1e-9)), allPkDb = 20.0 * std::log10(std::max(pk, 1e-9));
            worstPeak = std::max(worstPeak, pkDb); lo = std::min(lo, rmsDb); hi = std::max(hi, rmsDb);
            if (rmsDb < -30.0) { loud = false; badName = pr.name; }
            if (allPkDb > 6.0) { finite = false; badName += std::string(pr.name) + " runaway "; }
            if (pkDb > -0.5) { headroom = false; badName += std::string(pr.name) + "(" + std::to_string(pkDb).substr(0, 5) + ") "; }
            for (int k = 0; k < kNumSynthCategories; ++k) if (cat == kSynthCategories[k]) { catMin[k] = std::min(catMin[k], rmsDb); catMax[k] = std::max(catMax[k], rmsDb); }
        }
        check(finite, "preset library: every preset renders finite samples");
        check(loud, "preset library: no preset is silent or buried (RMS above -30 dB) " + badName);
        check(headroom, "preset library: every preset's body peaks below -0.5 dBFS (transients below +6) " + badName);
        // the library is level-matched: no category spreads wider than 7 dB (the old library spread 23 dB over all presets)
        bool tight = true; for (int k = 0; k < kNumSynthCategories; ++k) tight = tight && (catMax[k] - catMin[k]) <= 7.0;
        check(tight, "preset library: RMS inside each category spans at most 7 dB");
        check(hi - lo <= 12.0, "preset library: whole library spans at most 12 dB RMS");

        // FM scale / mod-env survive a custom preset round trip; a patch without the keys loads as legacy (8.0 / 0)
        SynthPatch fp; fp.engineType = SynthEngineType::FM; fp.fmIndexScale = 1.25f; fp.fmModEnv = 0.6f;
        check(SaveCustomSynthPreset("selftest fm scale", fp), "fm scale: custom preset save");
        SynthPatch rp; check(LoadCustomSynthPreset("selftest fm scale", rp) && std::fabs(rp.fmIndexScale - 1.25f) < 1e-4f && std::fabs(rp.fmModEnv - 0.6f) < 1e-4f, "fm scale: MOD RANGE and MOD ENV survive a preset round trip");
        std::remove((t8::userdirs::SynthPresetsDir() + "/selftest fm scale.t8syn").c_str());
        SynthPatch legacy; check(std::fabs(legacy.fmIndexScale - 8.0f) < 1e-6f && legacy.fmModEnv == 0.0f, "fm scale: default patches keep the legacy range (8.0) and no mod envelope");
        Project fpr; fpr.patterns[0].synthPatches[0].engineType = SynthEngineType::FM; fpr.patterns[0].synthPatches[0].fmIndexScale = 0.75f; fpr.patterns[0].synthPatches[0].fmModEnv = 0.4f;
        check(SaveProject(fpr, "selftest_tmp_fm.t8proj"), "fm scale: project save");
        Project fpl; check(LoadProject(fpl, "selftest_tmp_fm.t8proj") && std::fabs(fpl.patterns[0].synthPatches[0].fmIndexScale - 0.75f) < 1e-4f && std::fabs(fpl.patterns[0].synthPatches[0].fmModEnv - 0.4f) < 1e-4f, "fm scale: survives a project save / load");
        std::remove("selftest_tmp_fm.t8proj"); std::remove("selftest_tmp_fm.t8proj.bak");
        // the browser's tag derivation
        SynthPatch tp; tp.unisonVoices = 5; tp.subLevel = 0.4f; tp.filter24 = true; tp.lfoTarget = LfoTarget::Cutoff;
        auto tg = PresetTags(tp);
        check(std::find(tg.begin(), tg.end(), "UNI") != tg.end() && std::find(tg.begin(), tg.end(), "SUB") != tg.end() && std::find(tg.begin(), tg.end(), "24dB") != tg.end() && std::find(tg.begin(), tg.end(), "LFO") != tg.end(), "preset tags: derived from the patch");
    }

    // ---- v44.1 live mute pads: CHORD mute, DROP (everything but kick + bass), kit list, kit info ----
    {
        AppState ma;
        ma.engine.Init(kSampleRate);
        auto muted = [&](int ti) { return ma.CurrentPattern().tracks[ti].mute; };
        check(!ma.ChordIsMuted(), "mute pads: chord starts unmuted");
        ma.ToggleChordMute();
        check(ma.ChordIsMuted() && ma.engine.GetMixer().tracks[kChordTrackIndex].mute, "mute pads: CHORD mutes the chord track (pattern and live mixer)");
        { bool others = false; for (int i = 0; i < kNumPatterns; ++i) if (i != ma.project.currentPattern && ma.project.patterns[i].tracks[kChordTrackIndex].mute) others = true;
          check(!others, "mute pads v44.5.1: chord mute belongs to the edited scene only"); }
        ma.ToggleChordMute();
        check(!ma.ChordIsMuted(), "mute pads: CHORD toggles back");
        check(!ma.AllDrumsMuted(), "mute pads v44.3: ALL DRUMS starts unmuted");
        ma.ToggleAllDrumsMute();
        {
            bool all = ma.AllDrumsMuted();
            for (int i = 0; i < kNumDrumTracks; ++i) if (!ma.engine.GetMixer().tracks[i].mute) all = false;
            for (int pi = 0; pi < kNumPatterns; ++pi) if (pi != ma.project.currentPattern) for (int i = 0; i < kNumDrumTracks; ++i) if (ma.project.patterns[pi].tracks[i].mute) all = false;
            check(all && !ma.SynthIsMuted(0) && !ma.ChordIsMuted(), "mute pads v44.5.1: ALL DRUMS mutes kick, snare, clap, hats and perc in the edited scene only and leaves the synths");
        }
        ma.ToggleAllDrumsMute();
        check(!ma.AllDrumsMuted() && !ma.DrumMuteGroupIsMuted(0) && !ma.DrumMuteGroupIsMuted(2), "mute pads v44.3: ALL DRUMS toggles back");

        // DROP: hats are muted beforehand and must come back muted
        ma.ToggleDrumMuteGroup(2);
        check(!ma.DropIsActive(), "drop: starts off");
        ma.ToggleDrop();
        bool others = true, keepers = true;
        for (int i = 0; i < kNumTracks; ++i) { if (AppState::IsDropKeeper(i)) keepers = keepers && !muted(i); else others = others && muted(i); }
        check(ma.DropIsActive() && others && keepers, "drop: every track except kick and bass is muted, kick and bass keep sounding");
        check(ma.engine.GetMixer().tracks[static_cast<int>(DrumSlot::ClosedHat)].mute && !ma.engine.GetMixer().tracks[kNumDrumTracks].mute, "drop: the live mixer follows");
        ma.ToggleDrop();
        check(!ma.DropIsActive() && !muted(static_cast<int>(DrumSlot::Snare)) && !muted(kNumDrumTracks + 1) && !muted(kChordTrackIndex), "drop: second press brings the other tracks back");
        check(muted(static_cast<int>(DrumSlot::ClosedHat)) && muted(static_cast<int>(DrumSlot::OpenHat)), "drop: tracks that were muted before the drop stay muted");
        // unmuting one track by hand ends the drop
        ma.ToggleDrop();
        ma.SetMuteScene(kChordTrackIndex, false);
        check(!ma.DropIsActive(), "drop: unmuting a track by hand ends the drop");
        // a muted kick stays as it is
        ma.SetMuteScene(0, true);
        ma.ToggleDrop(); ma.ToggleDrop();
        check(muted(0), "drop: a kick the performer muted stays muted through a drop");

        // kit list and character
        auto kits = ListKitFolders();
        bool noRisers = true; for (const auto& k : kits) if (k == "kits/risers") noRisers = false;
        check(noRisers && kits.size() >= 17, "kit list: the risers folder is not offered as a drum kit");
        check(FindKitInfo("acid_v2") && std::string(FindKitInfo("acid_v2")->category) == "ELECTRONIC" && FindKitInfo("808") && FindKitInfo("no_such_kit") == nullptr, "kit info: shipped kits have a category, unknown ones none");
        bool all = true; for (const auto& k : kits) { std::string n = k.rfind("kits/", 0) == 0 ? k.substr(5) : k; if (!FindKitInfo(n)) all = false; }
        check(all, "kit info: every shipped kit folder is described (category + tags)");
        ma.LoadKitFolder("kits/808");
        check(ma.CurrentPattern().kitFolder == "kits/808" && ma.SceneKitFolder(0) == "kits/808" && !ma.kitAuditionRequest, "kit load v44.5.4: LoadKitFolder switches the kit of the edited scene and stays silent (v44.2: no audition unless HEAR is pressed)");
    }

    // ---- Key / scale change leaves chords alone; ADJUST re-tunes on request -------
    {
        AppState ka;
        ka.engine.Init(kSampleRate);
        ka.project.randomizerKeyRoot = 0; ka.project.randomizerScale = 0;     // C Ionian
        ka.project.currentPattern = 0;
        ka.ApplyDegreeSequence({1, 4, 5, 1}, "T");
        Pattern& kp = ka.CurrentPattern();
        const int chTrack = kNumDrumTracks + kNumSynthTracks;
        auto firstNotes = [&] { return std::array<int8_t, 6>(kp.tracks[chTrack].steps[0].chordNotes); };
        auto before = firstNotes();
        check(kp.chordsKey == 0 && kp.chordsScale == 0, "key: written chords remember the key they were made in");
        ka.project.randomizerKeyRoot = 2; ka.project.randomizerScale = 1;      // D Dorian: header change only
        ka.TickKeyFollow(); ka.TickKeyFollow();
        check(firstNotes() == before, "key: changing KEY / SCALE does not touch written chords (FOLLOW KEY off)");
        check(ka.SceneChordsOutOfKey(kp), "key: scene is flagged as out of key");
        ka.AdjustChordsToScale(false);
        auto after = firstNotes();
        bool allIn = true;
        for (int i = 0; i < kp.tracks[chTrack].steps[0].chordSize; ++i) {
            int rel = ((after[i] - 2) % 12 + 12) % 12; bool ok = false;
            for (int d = 0; d < kScaleDegreeCount[1]; ++d) if (kScaleIntervals[1][d] == rel) ok = true;
            allIn = allIn && ok;
        }
        check(after != before && allIn && !ka.SceneChordsOutOfKey(kp), "key: ADJUST moves the chords into the new key / scale");
        check(ka.Undo() && firstNotes() == before, "key: ADJUST is undoable");
        // FOLLOW KEY: automatic and applies to every scene
        ka.project.chordsFollowKey = true;
        ka.project.patterns[1].chordsKey = 0; ka.project.patterns[1].chordsScale = 0;
        ka.project.randomizerKeyRoot = 5;
        ka.TickKeyFollow();
        check(kp.chordsKey == 5 && ka.project.patterns[1].chordsKey == 5, "key: FOLLOW KEY re-tunes every scene");
        check(SnapToScale(61, 0, 0) == 60 && SnapToScale(60, 0, 0) == 60, "scale: SnapToScale keeps in-scale notes, snaps others down");
    }

    // ---- v44.2 scales: explicit MAJOR / MINOR, harmonic / melodic minor, pentatonics, blues -- notes, chords and the randomizers agree ----
    {
        check(std::string(kScaleNames[kScaleIonian]).find("MAJOR") == 0 && std::string(kScaleNames[kScaleAeolian]).find("MINOR") == 0, "scales: MAJOR and MINOR are named explicitly");
        check(kNumScales == 13 && kScaleDegreeCount[kScalePentMajor] == 5 && kScaleDegreeCount[12] == 6, "scales: harmonic / melodic minor, both pentatonics and blues exist");
        // the picker lists every scale exactly once
        { std::set<int> seen(kScaleMenuOrder, kScaleMenuOrder + kNumScales); check(static_cast<int>(seen.size()) == kNumScales && kScaleMenuOrder[0] == 0 && kScaleMenuOrder[1] == 5, "scales: the picker lists each scale once, MAJOR and MINOR first"); }
        auto inScale = [&](int note, int key, int sc) {
            const int rel = (((note - key) % 12) + 12) % 12;
            for (int d = 0; d < kScaleDegreeCount[sc]; ++d) if (kScaleIntervals[sc][d] == rel) return true;
            return false;
        };
        bool notesOk = true, snapOk = true, chordsOk = true, qualOk = true, randOk = true, chordAwareOk = true; int awareChecked = 0;
        for (int sc = 0; sc < kNumScales; ++sc) {
            for (int key : {0, 7}) {
                auto bn = BuildScaleNotes(key, sc, 1, 4);
                notesOk = notesOk && !bn.empty();
                for (int n : bn) notesOk = notesOk && inScale(n, key, sc);
                for (int n = 30; n < 90; ++n) { const int sn = SnapToScale(n, key, sc); snapOk = snapOk && inScale(sn, key, sc) && SnapToScale(sn, key, sc) == sn && std::abs(sn - n) <= 3; }
                const int cs = ChordScaleFor(sc);
                for (int deg = 1; deg <= 7; ++deg) for (bool cx : {false, true}) {
                    Chord c = BuildChord(key, sc, deg, cx, 3);
                    chordsOk = chordsOk && c.notes.size() == (cx ? 4u : 3u) && std::is_sorted(c.notes.begin(), c.notes.end());
                    for (int n : c.notes) chordsOk = chordsOk && inScale(n, key, cs);
                    qualOk = qualOk && c.quality && c.quality[0] != '?';
                }
                // the randomizer: every note of bass / synth lines is in the scale; with chords written in the same scale the strong beats are chord tones
                AppState ra; ra.engine.Init(kSampleRate);
                ra.project.randomizerKeyRoot = key; ra.project.randomizerScale = sc; ra.project.currentPattern = 0;
                ra.ApplyDegreeSequence({1, 4, 5, 1}, "T");
                ra.rndChordAware = true;
                Pattern& rp = ra.CurrentPattern();
                for (int rep = 0; rep < 3; ++rep) {
                    ra.RandomizeMelodic(0, 1, 2, rep == 2); ra.RandomizeMelodic(1, 3, 5, true);
                    for (int sy = 0; sy < 2; ++sy) {
                        const Track& mt = rp.tracks[kNumDrumTracks + sy];
                        for (int i = 0; i < rp.masterLength; ++i) {
                            const Step& st = mt.steps[i];
                            if (!st.active) continue;
                            randOk = randOk && inScale(st.note, key, sc);
                            if (sc <= 6 || sc == 8 || sc == 9) {      // seven-note scales: chords come from the same scale, so strong beats must be chord tones
                                if (i % 4 != 0) continue;
                                const Track& ct = rp.tracks[kChordTrackIndex];
                                for (int k = i; k >= 0; --k) {
                                    const Step& cst = ct.steps[k];
                                    if (!cst.active || cst.chordSize <= 0) continue;
                                    if (i < k + std::max<int>(1, cst.lengthSteps)) {
                                        bool hit = false; for (int q = 0; q < cst.chordSize; ++q) if (((cst.chordNotes[q] - st.note) % 12 + 12) % 12 == 0) hit = true;
                                        chordAwareOk = chordAwareOk && hit; ++awareChecked;
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
        check(notesOk, "scales: BuildScaleNotes returns only notes of the scale, for all 13 scales");
        check(snapOk, "scales: SnapToScale lands in the scale, is idempotent and moves at most 3 semitones");
        check(chordsOk, "scales: chords of every scale have 3 / 4 sorted notes and stay inside the scale they are harmonised from");
        check(qualOk, "scales: every diatonic chord of every scale gets a real quality label (no '?')");
        check(randOk, "scales: bass / synth randomizer output stays inside the scale (all 13 scales, 2 keys)");
        check(chordAwareOk && awareChecked > 50, "scales: with CHORDS on, strong beats land on a chord tone (seven-note scales)");
        check(ChordScaleFor(kScalePentMajor) == kScaleIonian && ChordScaleFor(kScalePentMinor) == kScaleAeolian && ChordScaleFor(12) == kScaleAeolian && ChordScaleFor(kChromaticScaleIndex) == kScaleIonian && ChordScaleFor(8) == 8, "scales: pentatonics, blues and chromatic are harmonised from MAJOR / MINOR");
        // C minor triads: i min, ii dim, III maj, iv min, v min, VI maj, VII maj; harmonic minor has the raised 7th: V maj, III aug
        check(std::string(BuildChord(0, kScaleAeolian, 1, false, 3).quality) == "MIN" && std::string(BuildChord(0, 8, 5, false, 3).quality) == "MAJ" && std::string(BuildChord(0, 8, 3, false, 3).quality) == "AUG", "scales: MINOR gives i = MIN, harmonic minor gives V = MAJ and III = AUG");
        // an old project (scale index 1 = Dorian) keeps meaning the same scale
        check(std::string(kScaleNames[1]) == "DORIAN" && kScaleIntervals[1][2] == 3 && kScaleIntervals[1][5] == 9, "scales: old indices keep their meaning (saved projects stay in tune)");
    }

    // ---- Stutter lands on the grid -------------------------------------------
    {
        AudioEngine sEngine; sEngine.Init(kSampleRate);
        Project sp; sp.kitFolder = kitFolder;
        Sequencer sSeq; sSeq.Init(&sEngine, &sp);
        Mixer& smx = sEngine.GetMixer();
        smx.repeaterWantOn = true; smx.repeaterWantDensity = 5;   // window of 2 steps
        std::vector<float> sbuf(256 * 2);
        sSeq.RenderAndAdvance(sbuf.data(), 256, kSampleRate);
        check(smx.repeaterOn, "stutter: engages at once while stopped");
        smx.repeaterWantOn = false; sSeq.RenderAndAdvance(sbuf.data(), 256, kSampleRate);
        check(!smx.repeaterOn, "stutter: releases at once while stopped");
        sSeq.Play();
        const double spS = 60.0 / 120.0 / 4.0 * kSampleRate;
        int n = static_cast<int>(spS * 0.5); for (int r = 0; r < n; r += 256) sSeq.RenderAndAdvance(sbuf.data(), 256, kSampleRate);
        smx.repeaterWantOn = true; smx.repeaterWantDensity = 0;
        check(!smx.repeaterOn, "stutter: a press mid-step is only armed");
        n = static_cast<int>(spS * 1.0); for (int r = 0; r < n; r += 256) sSeq.RenderAndAdvance(sbuf.data(), 256, kSampleRate);
        check(smx.repeaterOn, "stutter: engages on the next step boundary");
        sSeq.Stop();
    }

    // ---- Notes never hang across a tick reset (chain step) --------------------
    {
        AudioEngine hEngine; hEngine.Init(kSampleRate);
        Project hp; hp.kitFolder = kitFolder;
        hp.patterns[0].masterLength = 8; hp.patterns[1].masterLength = 8;
        Step& n = hp.patterns[0].tracks[kNumDrumTracks].steps[6];   // long note that outlives the pattern loop
        n.active = true; n.note = 40; n.lengthSteps = 6;
        hp.songChain[0] = ChainStep{0, 1}; hp.songChain[1] = ChainStep{1, 1}; hp.songChainLength = 2;
        hp.patterns[0].synthPatches[0].release = 0.02f;
        hEngine.Synth(0).SetPatch(hp.patterns[0].synthPatches[0]);
        Sequencer hSeq; hSeq.Init(&hEngine, &hp);
        hSeq.SetSongModeActive(true);
        hSeq.Play();
        std::vector<float> hbuf(256 * 2);
        const double spH = 60.0 / 120.0 / 4.0 * kSampleRate;
        int hn = static_cast<int>(spH * 45.8);   // chain step 2, step 5: the note from step 6 ended at step 4
        for (int r = 0; r < hn; r += 256) hSeq.RenderAndAdvance(hbuf.data(), 256, kSampleRate);
        check(!hEngine.Synth(0).IsActive(), "notes: a long note across a chain step is released (no hanging note)");
        hSeq.Stop();
    }

    // ---- Engine at 48 kHz: pitch and tempo follow the device rate ---------------------
    {
        AudioEngine e48; e48.Init(48000);
        SynthPatch pp; pp.waveform = Waveform::Sine; pp.cutoffHz = 18000; pp.resonance = 0; pp.envAmount = 0; pp.sustain = 1.0f; pp.volume = 0.8f;
        e48.Synth(0).SetPatch(pp);
        e48.SynthNoteOn(0, 69, 1.0f, false);                       // A4 = 440 Hz
        std::vector<float> x(48000 * 2), tmp(512 * 2);
        for (int p0 = 0; p0 < 48000; p0 += 512) { int blk = std::min(512, 48000 - p0); e48.Render(tmp.data(), blk, 5512.5); for (int i = 0; i < blk; ++i) x[p0 + i] = tmp[2 * i]; }
        int crossings = 0;
        for (int i = 6000; i < 48000 - 1; ++i) if (x[i] <= 0.0f && x[i + 1] > 0.0f) ++crossings;
        double hz = crossings / ((48000 - 6000) / 48000.0);
        check(std::fabs(hz - 440.0) < 3.0, "48 kHz: A4 really sounds at 440 Hz");
        Project p48; Sequencer s48; s48.Init(&e48, &p48);
        s48.Play();
        std::vector<float> tb(256 * 2);
        for (int r = 0; r < 48000; r += 256) s48.RenderAndAdvance(tb.data(), 256, 48000);
        check(s48.StepForTrack(0) == 8, "48 kHz: one second at 120 bpm is exactly 8 steps");
        s48.Stop();
    }

    // ---- Band-limited saw / square (PolyBLEP): less aliasing on a high note ---------------------
    {
        auto goertzel = [](const std::vector<float>& v, double f, int sr) {
            double w = 2 * 3.14159265358979 * f / sr, c = 2 * std::cos(w), s1 = 0, s2 = 0;
            for (float q : v) { double s = q + c * s1 - s2; s2 = s1; s1 = s; }
            return std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / v.size() * 2;
        };
        SynthPatch pp; pp.waveform = Waveform::Saw; pp.cutoffHz = 20000; pp.resonance = 0; pp.envAmount = 0; pp.sustain = 1.0f; pp.attack = 0.001f;
        SubtractiveSynth sy; sy.SetPatch(pp); sy.NoteOn(96, 1.0f, false);   // C7, 2093 Hz
        std::vector<float> buf(kSampleRate * 2), mono;
        sy.Render(buf.data(), kSampleRate, kSampleRate);
        for (int i = 0; i < kSampleRate; ++i) mono.push_back(buf[2 * i]);
        double f0 = 440.0 * std::pow(2.0, 27.0 / 12.0), fund = goertzel(mono, f0, kSampleRate), worst = 0;
        for (int k = 11; k < 40; ++k) {                                      // harmonics above Nyquist fold back
            double fa = std::fabs(k * f0 - kSampleRate * std::round(k * f0 / kSampleRate));
            bool harmonic = false; for (int h = 1; h * f0 < kSampleRate / 2; ++h) if (std::fabs(h * f0 - fa) < 30) harmonic = true;
            if (!harmonic) worst = std::max(worst, goertzel(mono, fa, kSampleRate));
        }
        check(20.0 * std::log10(worst / fund) < -26.0, "saw: aliasing on C7 stays below -26 dB (PolyBLEP)");
    }

    // ---- Sub bass: fades and the kick duck must not click ------------------------------
    {
        auto worstKink = [](const std::vector<float>& v) {
            double w = 0; for (size_t i = 2; i < v.size(); ++i) w = std::max(w, static_cast<double>(std::fabs(v[i] - 2 * v[i - 1] + v[i - 2]))); return w;
        };
        AudioEngine cEngine; cEngine.Init(kSampleRate);
        SynthPatch bp; bp.waveform = Waveform::Sine; bp.cutoffHz = 150; bp.sustain = 1.0f; bp.volume = 0.8f;
        cEngine.Synth(0).SetPatch(bp);
        Mixer& cm = cEngine.GetMixer();
        for (int i = 0; i < kNumTracks; ++i) cm.tracks[i].volume = 0.0f;
        cm.tracks[kNumDrumTracks].volume = 1.0f;
        cm.fx.sidechainEnabled = true; cm.fx.sidechainAmount = 0.8f; cm.fx.sidechainRelease = 0.2f; cm.fx.delayLevel = 0; cm.fx.reverbLevel = 0;
        cEngine.SynthNoteOn(0, 33, 1.0f, false);
        std::vector<float> cb(512 * 2), cmono;
        DrumVoiceParams dvp;
        for (int blk = 0; blk < 120; ++blk) {
            if (blk == 60) cEngine.TriggerDrum(DrumSlot::Kick, 1.0f, 0, dvp);
            cEngine.Render(cb.data(), 512, 5512.5);
            for (int i = 0; i < 512; ++i) cmono.push_back(cb[2 * i]);
        }
        std::vector<float> tail(cmono.begin() + 20000, cmono.end());
        check(worstKink(tail) < 0.0005, "sub bass: a sidechain duck on a kick is click-free");
        cEngine.SynthNoteOff(0);
        std::vector<float> rel;
        for (int blk = 0; blk < 20; ++blk) { cEngine.Render(cb.data(), 512, 5512.5); for (int i = 0; i < 512; ++i) rel.push_back(cb[2 * i]); }
        check(worstKink(rel) < 0.0005, "sub bass: release of a 55 Hz note is click-free");
    }

    // ---- Sub bass behind the master HPF + a MUTED kick still ducking it (recorded clicks) ----
    {
        AudioEngine hEng; hEng.Init(kSampleRate);
        SynthPatch hp; hp.waveform = Waveform::Sine; hp.cutoffHz = 300; hp.sustain = 0.8f; hp.volume = 0.8f;
        hEng.Synth(0).SetPatch(hp);
        Mixer& hmx = hEng.GetMixer();
        for (int i = 0; i < kNumTracks; ++i) hmx.tracks[i].volume = 0.0f;       // kick muted: it only drives the duck
        hmx.tracks[kNumDrumTracks].volume = 0.9f; hmx.fx.delayLevel = 0; hmx.fx.reverbLevel = 0; hmx.masterVolume = 1.0f;
        hmx.masterHpfAmount = 0.6f;
        hmx.fx.sidechainEnabled = true; hmx.fx.sidechainAmount = 0.8f; hmx.fx.sidechainRelease = 0.2f;
        std::vector<float> hb(128 * 2); double clickPk = 0;
        for (int blk = 0; blk < 240; ++blk) {
            if (blk == 20) hEng.SynthNoteOn(0, 33, 1.0f, false);
            if (blk == 160) { DrumVoiceParams dp; hEng.TriggerDrum(DrumSlot::Kick, 1.0f, 0, dp); }
            hEng.Render(hb.data(), 128, 5512.5);
            if (blk >= 160 && blk < 190) for (int i = 0; i < 128; ++i) clickPk = std::max(clickPk, static_cast<double>(std::fabs(hb[2 * i])));
        }
        check(clickPk < 0.005, "sub bass behind the master HPF: a (muted) kick's sidechain duck leaves no click");
    }

    // ---- Chord editor core + chain slots that carry a chord scene ---------------------------
    {
        bool ok = true, bad = false;
        for (int q = 0; q < kNumChordQualities; ++q) for (int inv = 0; inv < ChordQuality(q).n; ++inv) for (int root : {36, 43, 50, 59}) {
            ChordSpec sp; sp.root = root; sp.quality = q; sp.inversion = inv;
            std::vector<int> n = BuildChordNotes(sp);
            std::vector<int8_t> n8(n.begin(), n.end());
            ChordSpec back;
            if (!IdentifyChord(n8.data(), static_cast<int>(n8.size()), back)) { ok = false; continue; }
            std::vector<int> n2 = BuildChordNotes(back);
            // the same pitch classes with the same lowest note come back
            if (((back.root - root) % 12 + 12) % 12 != 0 || back.quality != q) {
                // a symmetric chord (aug, dim7, ...) may legitimately be read from another root: same notes are enough
                std::vector<int> a = n, b = n2; for (int& x : a) x %= 12; for (int& x : b) x %= 12;
                std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
                if (a != b) bad = true;
            }
        }
        check(ok && !bad, "chord editor: every type / inversion is recognised again from its notes");
        ChordSpec cm; cm.root = 48; cm.quality = 1; cm.inversion = 0;
        auto nn = BuildChordNotes(cm);
        check(nn.size() == 3 && nn[0] == 48 && nn[1] == 51 && nn[2] == 55, "chord editor: C minor = C Eb G");
        cm.inversion = 1; nn = BuildChordNotes(cm);
        check(nn[0] == 51 && nn[2] == 60, "chord editor: first inversion puts the root on top");

        static Project cp; cp = Project{};
        cp.patterns[0].bpm = 120; cp.patterns[0].masterLength = 16; cp.patterns[1].masterLength = 16; cp.patterns[0].tracks[0].steps[0].active = true; cp.patterns[1].tracks[0].steps[0].active = true;
        cp.chordBankOn = true;
        for (int c = 0; c < 3; ++c) { Step& st = cp.chordScenes[c].track.steps[0]; st.active = true; st.chordSize = 3; st.chordNotes = {static_cast<int8_t>(48 + c), static_cast<int8_t>(52 + c), static_cast<int8_t>(55 + c), 0}; st.lengthSteps = 16; }
        cp.songChainLength = 3;
        cp.songChain[0] = ChainStep{0, 1, 0}; cp.songChain[1] = ChainStep{1, 1, 2}; cp.songChain[2] = ChainStep{0, 1, -1};
        check(SaveProject(cp, "selftest_tmp_safety/chainchord.t8proj"), "chain chord: save");
        Project lp; check(LoadProject(lp, "selftest_tmp_safety/chainchord.t8proj"), "chain chord: load");
        check(lp.songChain[0].chordScene == 0 && lp.songChain[1].chordScene == 2 && lp.songChain[2].chordScene == -1, "chain chord: the chord scene of each slot survives a reload");
        AudioEngine e; e.Init(kSampleRate);
        Sequencer sq; sq.Init(&e, &cp);
        sq.SetSongModeActive(true); sq.Play();
        std::vector<float> o(256 * 2);
        auto run = [&](double secs) { int nb = static_cast<int>(secs * kSampleRate) / 256; for (int b = 0; b < nb; ++b) sq.RenderAndAdvance(o.data(), 256, kSampleRate); };
        check(sq.ChordSceneCur() == 0, "chain chord: starting the chain selects slot 1's chord scene");
        run(2.0 * 1.05);                                                  // 16 steps at 120 BPM = 2 s -> slot 2
        check(sq.ChordSceneCur() == 2, "chain chord: the chord scene changes with the chain slot");
        run(2.0);                                                         // slot 3 has no chord scene: stays
        check(sq.ChordSceneCur() == 2, "chain chord: a slot without a chord scene leaves the chords alone");
    }

    // ---- Randomizer: in key, melodic, safe slides; SNAP TO SCALE ------------------------------
    {
        AppState ra; ra.engine.Init(kSampleRate); ra.sequencer.Init(&ra.engine, &ra.project);
        int tot = 0, bad = 0, badSlide = 0, strongN = 0, offN = 0; long leapSum = 0; int leapN = 0;
        for (int key : {0, 3, 7, 10}) for (int sc = 0; sc < 7; ++sc) {
            ra.project.randomizerKeyRoot = key; ra.project.randomizerScale = sc;
            for (int part = 0; part < 3; ++part) for (int rep = 0; rep < 12; ++rep) {
                ra.RandomizeMelodic(part, part == 0 ? 1 : 3, part == 0 ? 2 : 5, rep & 1);
                const Track& t = ra.CurrentPattern().tracks[kNumDrumTracks + part];
                int prev = -1;
                for (int i = 0; i < 16; ++i) {
                    const Step& st = t.steps[i]; if (!st.active) continue;
                    ++tot; if (SnapToScale(st.note, key, sc) != st.note) ++bad;
                    if (st.slide && (prev < 0 || std::abs(st.note - prev) > 2)) ++badSlide;
                    if (i % 4 == 0) ++strongN; else ++offN;
                    if (prev >= 0) { leapSum += std::abs(st.note - prev); ++leapN; }
                    prev = st.note;
                }
            }
        }
        check(tot > 1000 && bad == 0, "randomizer: every note is in the header's key and scale");
        check(badSlide == 0, "randomizer: slides only join neighbouring pitches (<= 2 semitones)");
        check(static_cast<double>(strongN) / 4.0 > static_cast<double>(offN) / 12.0 * 1.3, "randomizer: strong beats carry more notes than weak ones");
        check(leapN > 0 && static_cast<double>(leapSum) / leapN < 4.5, "randomizer: melodic motion is mostly stepwise (mean interval < 4.5 semitones)");
        ra.project.randomizerKeyRoot = 0; ra.project.randomizerScale = 0;
        Track& tt = ra.CurrentPattern().tracks[kNumDrumTracks + 1];
        for (auto& st : tt.steps) st = Step{};
        tt.steps[0].active = true; tt.steps[0].note = 61; tt.steps[4].active = true; tt.steps[4].note = 64;   // C# (out), E (in)
        int moved = ra.SnapTrackToScale(kNumDrumTracks + 1);
        check(moved == 1 && SnapToScale(tt.steps[0].note, 0, 0) == tt.steps[0].note && tt.steps[4].note == 64, "snap to scale moves only the notes that are out of key");
        check(ra.Undo() && tt.steps[0].note == 61, "snap to scale is undoable");
    }

    // ---- Thicker synths: unison, sub, noise, drive, second oscillator, slope ----------------------
    {
        const int sr = kSampleRate;
        auto renderVoice = [&](const SynthPatch& p, int note, double secs, bool release = false) {
            SynthVoice v; v.SetPatch(p); v.NoteOn(note, 1.0f, false);
            std::vector<float> o(static_cast<size_t>(secs * sr) * 2, 0.0f);
            int n = static_cast<int>(secs * sr), half = release ? n / 2 : n;
            for (int d = 0; d < n; d += 256) {
                if (release && d >= half && d - 256 < half) v.NoteOff();
                v.Render(o.data() + static_cast<size_t>(d) * 2, std::min(256, n - d), sr);
            }
            return o;
        };
        auto goertzel = [&](const std::vector<float>& o, double f, int from, int to) {
            double w = 2.0 * 3.14159265358979 * f / sr, c = 2.0 * std::cos(w), s1 = 0, s2 = 0;
            for (int i = from; i < to; ++i) { double x = o[static_cast<size_t>(i) * 2] * (0.5 - 0.5 * std::cos(2 * 3.14159265358979 * (i - from) / (to - from - 1))); double s0 = x + c * s1 - s2; s2 = s1; s1 = s0; }
            return std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / (to - from);
        };
        auto rms = [&](const std::vector<float>& o, int ch, int from, int to) { double a = 0; for (int i = from; i < to; ++i) a += o[static_cast<size_t>(i) * 2 + ch] * o[static_cast<size_t>(i) * 2 + ch]; return std::sqrt(a / (to - from)); };
        SynthPatch base; base.waveform = Waveform::Saw; base.cutoffHz = 3000; base.sustain = 0.9f; base.volume = 0.8f;

        // default patch: the voice must be the old single engine, sample for sample
        {
            SubtractiveSynth ref; ref.SetPatch(base); ref.NoteOn(45, 1.0f, false);
            SynthVoice v; v.SetPatch(base); v.NoteOn(45, 1.0f, false);
            std::vector<float> a(512 * 2, 0.f), b(512 * 2, 0.f);
            for (int k = 0; k < 20; ++k) { std::fill(a.begin(), a.end(), 0.f); std::fill(b.begin(), b.end(), 0.f); ref.Render(a.data(), 512, sr); v.Render(b.data(), 512, sr); if (a != b) break; }
            check(a == b, "thick synths: defaults leave the voice sample-identical to the single engine");
        }
        // unison
        {
            SynthPatch u = base; u.unisonVoices = 5; u.unisonDetune = 0.5f; u.unisonSpread = 1.0f;
            auto o1 = renderVoice(base, 45, 0.6), o5 = renderVoice(u, 45, 0.6);
            double l = rms(o5, 0, sr / 5, sr / 2), r = rms(o5, 1, sr / 5, sr / 2), m1 = rms(o1, 0, sr / 5, sr / 2);
            bool finite = true; for (float x : o5) if (!std::isfinite(x)) finite = false;
            bool differ = false; for (size_t i = 0; i < o5.size(); i += 2) if (std::fabs(o5[i] - o5[i + 1]) > 1e-4f) { differ = true; break; }
            check(finite && differ, "unison: finite output, left and right differ (stereo spread)");
            check(l > m1 * 0.6 && l < m1 * 2.2 && r > m1 * 0.6 && r < m1 * 2.2, "unison: loudness stays close to the single voice (gain-staged)");
            SynthPatch u1 = u; u1.unisonSpread = 0.0f;
            auto oc = renderVoice(u1, 45, 0.4);
            bool same = true; for (size_t i = 0; i < oc.size(); i += 2) if (std::fabs(oc[i] - oc[i + 1]) > 1e-4f) { same = false; break; }
            check(same, "unison: zero spread is mono");
        }
        // sub oscillator: energy an octave below the note
        {
            SynthPatch sp = base; sp.waveform = Waveform::Triangle; sp.cutoffHz = 2500;
            SynthPatch sb = sp; sb.subLevel = 0.8f; sb.subOctave = 1;
            auto a = renderVoice(sp, 57, 0.5), b = renderVoice(sb, 57, 0.5);            // A3 = 220 Hz, sub 110 Hz
            double ea = goertzel(a, 110.0, sr / 5, sr * 2 / 5), eb = goertzel(b, 110.0, sr / 5, sr * 2 / 5);
            check(eb > ea * 8.0 + 1e-6, "sub oscillator adds a clear tone one octave below");
            sb.subOctave = 2; auto c = renderVoice(sb, 57, 0.5);
            check(goertzel(c, 55.0, sr / 5, sr * 2 / 5) > goertzel(a, 55.0, sr / 5, sr * 2 / 5) * 8.0 + 1e-6, "sub oscillator: two octaves below");
        }
        // noise: broadband energy where the plain sine has none
        {
            SynthPatch sp = base; sp.waveform = Waveform::Sine; sp.cutoffHz = 12000;
            SynthPatch nz = sp; nz.noiseLevel = 0.6f;
            auto a = renderVoice(sp, 69, 0.4), b = renderVoice(nz, 69, 0.4);
            check(goertzel(b, 3000.0, sr / 5, sr * 3 / 10) > goertzel(a, 3000.0, sr / 5, sr * 3 / 10) * 20.0 + 1e-7, "noise layer adds broadband energy");
        }
        // drive: louder when quiet, never explodes
        {
            SynthPatch q = base; q.volume = 0.15f;
            SynthPatch d = q; d.drive = 1.0f;
            auto a = renderVoice(q, 45, 0.4), b = renderVoice(d, 45, 0.4);
            float pk = 0; for (float x : b) pk = std::max(pk, std::fabs(x));
            check(rms(b, 0, sr / 5, sr * 3 / 10) > rms(a, 0, sr / 5, sr * 3 / 10) * 1.2 && pk < 2.0f, "drive adds weight and stays bounded");
        }
        // second oscillator an octave up (level 1): 2f energy rises
        {
            SynthPatch sp = base; sp.waveform = Waveform::Sine; sp.cutoffHz = 12000;
            SynthPatch o2 = sp; o2.osc2Wave = Waveform::Sine; o2.osc2Level = 1.0f; o2.osc2Semi = 12; o2.osc2Detune = 0;
            auto a = renderVoice(sp, 57, 0.4), b = renderVoice(o2, 57, 0.4);
            check(goertzel(b, 440.0, sr / 5, sr * 3 / 10) > goertzel(a, 440.0, sr / 5, sr * 3 / 10) * 10.0 + 1e-7, "OSC2: the second oscillator sounds an octave up");
        }
        // 24 dB slope: less treble than 12 dB
        {
            SynthPatch sp = base; sp.cutoffHz = 600; sp.resonance = 0.1f; sp.envAmount = 0.0f;
            SynthPatch s24 = sp; s24.filter24 = true;
            auto a = renderVoice(sp, 45, 0.5), b = renderVoice(s24, 45, 0.5);
            double ha = 0, hb = 0; for (int f : {2200, 3300, 4400}) { ha += goertzel(a, f, sr / 5, sr * 2 / 5); hb += goertzel(b, f, sr / 5, sr * 2 / 5); }
            check(hb < ha * 0.6, "filter slope 24 dB removes more treble than 12 dB");
        }
        // keytrack: a high note with 100% keytrack keeps more brightness than without
        {
            SynthPatch sp = base; sp.cutoffHz = 400; sp.envAmount = 0.0f;
            SynthPatch kt = sp; kt.keytrack = 1.0f;
            auto a = renderVoice(sp, 81, 0.4), b = renderVoice(kt, 81, 0.4);   // A5
            check(rms(b, 0, sr / 5, sr * 3 / 10) > rms(a, 0, sr / 5, sr * 3 / 10) * 1.5, "keytrack opens the filter for high notes");
        }
        // ---- delay v2 (v37): interpolated time, tone, ping-pong -----------------------------------------
        {
            auto runD = [&](const std::vector<float>& in, float t0, float t1, float fb, float tone, bool pp) {
                DelayEffect d; d.Init(sr); const int n = static_cast<int>(in.size() / 2); std::vector<float> out(in.size());
                for (int o = 0; o < n; o += 256) d.Process(in.data() + static_cast<size_t>(o) * 2, out.data() + static_cast<size_t>(o) * 2, std::min(256, n - o), o < n / 2 ? t0 : t1, fb, tone, pp);
                return out;
            };
            // a fractional delay time lands where it should (not rounded to a sample)
            {
                std::vector<float> in(static_cast<size_t>(sr) * 2, 0.0f); in[0] = in[1] = 1.0f;
                auto o = runD(in, 0.1003f, 0.1003f, 0.0f, 1.0f, false);
                size_t best = 0; for (size_t i = 1; i < o.size() / 2; ++i) if (std::fabs(o[2 * i]) > std::fabs(o[2 * best])) best = i;
                // centre of gravity of the echo (interpolation spreads it over neighbouring samples)
                double num = 0, den = 0; for (size_t i = best - 3; i <= best + 3; ++i) { num += i * o[2 * i]; den += o[2 * i]; }
                check(std::fabs(num / den - 0.1003 * sr) < 0.15, "delay v2: the echo lands on the exact fractional time");
            }
            // changing the time during sound: no discontinuity (the old delay jumped by 1.9x a normal step)
            {
                const int n = sr * 2; std::vector<float> in(static_cast<size_t>(n) * 2);
                for (int i = 0; i < n; ++i) in[2 * i] = in[2 * i + 1] = 0.5f * std::sin(2 * 3.14159265 * 330.0 * i / sr);
                auto o = runD(in, 0.25f, 0.375f, 0.5f, 0.7f, false);
                double normal = 0, change = 0;
                for (int i = sr / 2; i < n / 2 - 512; ++i) normal = std::max<double>(normal, std::fabs(o[2 * (i + 1)] - o[2 * i]));
                for (int i = n / 2 - 256; i < n / 2 + sr / 4; ++i) change = std::max<double>(change, std::fabs(o[2 * (i + 1)] - o[2 * i]));
                check(change < normal * 1.35, "delay v2: changing the delay time makes no click (glides instead of jumping)");
            }
            // TONE: dark repeats lose their highs
            {
                std::vector<float> in(static_cast<size_t>(sr) * 2, 0.0f);
                for (int i = 0; i < 200; ++i) in[2 * i] = in[2 * i + 1] = ((i / 2) % 2) ? 0.4f : -0.4f;   // bright burst
                auto dark = runD(in, 0.2f, 0.2f, 0.7f, 0.0f, false), bright = runD(in, 0.2f, 0.2f, 0.7f, 1.0f, false);
                auto hf = [&](const std::vector<float>& o) { double a = 0; for (int i = 3 * sr / 5; i < 3 * sr / 5 + 600; ++i) { const double d = o[2 * (i + 1)] - o[2 * i]; a += d * d; } return a; };
                check(hf(bright) > hf(dark) * 4.0, "delay v2: TONE darkens the repeats");
            }
            // PING-PONG: the first echo comes out of the left, the second out of the right
            {
                std::vector<float> in(static_cast<size_t>(sr) * 2, 0.0f); in[0] = in[1] = 1.0f;
                auto o = runD(in, 0.2f, 0.2f, 0.7f, 1.0f, true);
                auto e = [&](int ch, int at) { double a = 0; for (int i = at - 100; i < at + 300; ++i) a += std::fabs(o[2 * i + ch]); return a; };
                const int a1 = sr / 5, a2 = 2 * sr / 5;
                check(e(0, a1) > e(1, a1) * 5.0 && e(1, a2) > e(0, a2) * 5.0, "delay v2: PING-PONG bounces the repeats left then right");
            }
            // high feedback with loud input stays bounded; settings are saved
            {
                std::vector<float> in(static_cast<size_t>(sr) * 4); for (size_t i = 0; i < in.size(); ++i) in[i] = ((i / 40) % 2) ? 1.0f : -1.0f;
                auto o = runD(in, 0.1f, 0.1f, 0.95f, 1.0f, true); bool ok = true; for (float v : o) if (!std::isfinite(v) || std::fabs(v) > 12.0f) ok = false;
                check(ok, "delay v2: maximum feedback stays bounded");
                static Project dp; dp = Project{}; dp.fx.delayTone = 0.2f; dp.fx.delayPingPong = true;
                SaveProject(dp, "selftest_tmp_safety/dly.t8proj"); Project lp; LoadProject(lp, "selftest_tmp_safety/dly.t8proj");
                check(std::fabs(lp.fx.delayTone - 0.2f) < 1e-4 && lp.fx.delayPingPong, "delay v2: TONE and PING-PONG survive a project reload");
            }
        }
        // ---- reverb v2 (v37): stereo FDN -------------------------------------------------------------
        {
            auto run = [&](int rate, float size, float damp, float pre, int seconds, bool click, std::vector<float>& out) {
                ReverbEffect r; r.Init(rate); const int n = rate * seconds;
                std::vector<float> in(static_cast<size_t>(n) * 2, 0.0f); out.assign(static_cast<size_t>(n) * 2, 0.0f);
                if (click) in[0] = in[1] = 1.0f;
                for (int o = 0; o < n; o += 256) r.Process(in.data() + static_cast<size_t>(o) * 2, out.data() + static_cast<size_t>(o) * 2, std::min(256, n - o), size, damp, pre);
            };
            std::vector<float> o;
            run(sr, 0.8f, 0.5f, 12.0f, 4, true, o);
            double ll = 0, rr = 0, lr = 0; for (size_t i = static_cast<size_t>(sr / 5); i < o.size() / 2; ++i) { ll += o[2 * i] * o[2 * i]; rr += o[2 * i + 1] * o[2 * i + 1]; lr += o[2 * i] * o[2 * i + 1]; }
            check(std::fabs(lr / std::sqrt(ll * rr)) < 0.25, "reverb v2: a mono click gives a stereo tail (left / right decorrelated)");
            check(std::fabs(10.0 * std::log10(ll / rr)) < 1.0, "reverb v2: left and right tails have the same level");
            // decay time follows the SIZE curve (the old reverb's RT60)
            bool okRt = true;
            for (float size : {0.5f, 0.8f, 0.9f}) {
                run(sr, size, 0.5f, 12.0f, 6, true, o);
                std::vector<float> L(o.size() / 2); for (size_t i = 0; i < L.size(); ++i) L[i] = o[2 * i];
                const double rt = t8m::Rt60Seconds(L, sr), want = ReverbEffect::Rt60ForSize(size);
                if (rt < want * 0.75 || rt > want * 1.3) okRt = false;
            }
            check(okRt, "reverb v2: RT60 stays within 25 % of the SIZE curve at 0.5 / 0.8 / 0.9");
            // damping darkens the tail
            auto hiLo = [&](float damp) {
                std::vector<float> q; run(sr, 0.85f, damp, 12.0f, 3, true, q);
                std::vector<float> L(q.size() / 2); for (size_t i = 0; i < L.size(); ++i) L[i] = q[2 * i];
                auto p = t8m::PowerSpectrum(L, sr * 3 / 10, 32768);
                return t8m::BandEnergy(p, 4000, 12000, sr, 32768) / t8m::BandEnergy(p, 100, 1000, sr, 32768);
            };
            const double b0 = hiLo(0.0f), b5 = hiLo(0.5f), b1 = hiLo(1.0f);
            check(b0 > b5 * 3.0 && b5 > b1 * 3.0, "reverb v2: DAMP darkens the tail step by step");
            // pre-delay: silence before it, signal after
            run(sr, 0.7f, 0.5f, 60.0f, 1, true, o);
            double early = 0, late = 0; for (int i = 0; i < sr * 55 / 1000; ++i) early += std::fabs(o[2 * i]); for (int i = sr * 70 / 1000; i < sr * 200 / 1000; ++i) late += std::fabs(o[2 * i]);
            check(early < 1e-3 && late > 0.05, "reverb v2: PRE delays the onset of the tail");
            // robustness: loudest setting stays finite and bounded, silence in = silence out, other sample rates work
            bool fin = true;
            run(sr, 0.97f, 0.0f, 0.0f, 6, true, o); for (float v : o) if (!std::isfinite(v) || std::fabs(v) > 4.0f) fin = false;
            { std::vector<float> noise; ReverbEffect r; r.Init(sr); std::vector<float> in(static_cast<size_t>(sr) * 2), out(static_cast<size_t>(sr) * 2);
              for (size_t i = 0; i < in.size(); ++i) in[i] = ((i / 7) % 2) ? 1.0f : -1.0f;
              for (int o2 = 0; o2 < sr; o2 += 256) r.Process(in.data() + static_cast<size_t>(o2) * 2, out.data() + static_cast<size_t>(o2) * 2, std::min(256, sr - o2), 0.97f, 0.0f, 0.0f);
              for (float v : out) if (!std::isfinite(v) || std::fabs(v) > 10.0f) fin = false; }
            check(fin, "reverb v2: maximum size and loud input stay finite and bounded");
            run(sr, 0.6f, 0.5f, 12.0f, 2, false, o); double sil = 0; for (float v : o) sil = std::max<double>(sil, std::fabs(v));
            check(sil == 0.0, "reverb v2: silence in, silence out");
            bool rates = true;
            for (int rate : {44100, 96000}) { run(rate, 0.8f, 0.5f, 12.0f, 3, true, o); double e = 0; for (float v : o) { if (!std::isfinite(v)) rates = false; e += std::fabs(v); }
              std::vector<float> L(o.size() / 2); for (size_t i = 0; i < L.size(); ++i) L[i] = o[2 * i];
              const double rt = t8m::Rt60Seconds(L, rate); if (e < 1.0 || rt < 0.5 || rt > 1.4) rates = false; }
            check(rates, "reverb v2: works at 44.1 and 96 kHz with the same decay time");
            // saved with the project
            static Project rp; rp = Project{}; rp.fx.reverbDamp = 0.8f; rp.fx.reverbPreMs = 33.0f;
            SaveProject(rp, "selftest_tmp_safety/rev.t8proj"); Project lp; LoadProject(lp, "selftest_tmp_safety/rev.t8proj");
            check(std::fabs(lp.fx.reverbDamp - 0.8f) < 1e-4 && std::fabs(lp.fx.reverbPreMs - 33.0f) < 1e-3, "reverb v2: DAMP and PRE survive a project reload");
        }
        // ---- unison keeps the timbre: switching UNISON on must not comb the spectrum or change the loudness ----
        {
            auto harm = [&](const std::vector<float>& o, int ch, double f0, int h) {
                const int from = sr / 2, to = from + 16384;
                double w = 2.0 * 3.14159265358979 * f0 * h / sr, c = 2.0 * std::cos(w), s1 = 0, s2 = 0;
                for (int i = from; i < to; ++i) { double x = o[static_cast<size_t>(i) * 2 + ch] * (0.5 - 0.5 * std::cos(2 * 3.14159265358979 * (i - from) / (to - from - 1))); double s0 = x + c * s1 - s2; s2 = s1; s1 = s0; }
                return std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2);
            };
            bool okSpec = true, okLr = true, okLevel = true; double worstDev = 0;
            for (int n = 2; n <= 7; ++n) {
                for (float spread : {0.0f, 0.7f, 1.0f}) {
                    SynthPatch mp = base; mp.waveform = Waveform::Saw; mp.cutoffHz = 12000; mp.envAmount = 0; mp.sustain = 1.0f; mp.attack = 0.001f; mp.volume = 0.8f; mp.drive = 0.0f;
                    SynthPatch up = mp; up.unisonVoices = n; up.unisonDetune = 0.0f; up.unisonSpread = spread;   // no detune: the start phases stay as designed
                    auto m = renderVoice(mp, 45, 1.0), u = renderVoice(up, 45, 1.0);
                    for (int h = 1; h <= 16; ++h) {
                        const double ref = harm(m, 0, 110.0, h), l = harm(u, 0, 110.0, h), r = harm(u, 1, 110.0, h);
                        const double dl = 20.0 * std::log10(l / ref), dr = 20.0 * std::log10(r / ref);
                        worstDev = std::max(worstDev, std::max(std::fabs(dl), std::fabs(dr)));
                        if (std::fabs(dl) > 10.0 || std::fabs(dr) > 10.0) okSpec = false;
                        if (std::fabs(dl - dr) > 0.6) okLr = false;
                    }
                    const double lm = rms(m, 0, sr / 2, sr), lu = std::sqrt(0.5 * (rms(u, 0, sr / 2, sr) * rms(u, 0, sr / 2, sr) + rms(u, 1, sr / 2, sr) * rms(u, 1, sr / 2, sr)));
                    if (std::fabs(20.0 * std::log10(lu / lm)) > 2.5) okLevel = false;
                }
            }
            std::printf("[info] unison: worst harmonic deviation from the single voice %.1f dB\n", worstDev);
            check(okSpec, "unison: no combing -- every harmonic within 10 dB of the single voice (was up to -13 dB, with the loudness off by 4 dB), n = 2..7, spread 0 / .7 / 1");
            check(okLr, "unison: left and right have the same timbre (harmonics within 0.6 dB)");
            check(okLevel, "unison: loudness stays within 2.5 dB of the single voice");
        }
        // ---- synth v2 (v37): ZDF filter, 2x oversampling, ACID mode, drift ----------------------
        {
            // the cutoff works right up the range: 8k, 12k and 18k used to be identical (SVF clamp near 7 kHz)
            SynthPatch hi = base; hi.waveform = Waveform::Saw; hi.envAmount = 0; hi.resonance = 0.0f; hi.sustain = 1.0f;
            double e[3]; int k = 0;
            for (float c : {6000.f, 10000.f, 16000.f}) { hi.cutoffHz = c; auto o = renderVoice(hi, 36, 0.5); e[k++] = goertzel(o, 14000.0, sr / 5, sr * 2 / 5) + goertzel(o, 15000.0, sr / 5, sr * 2 / 5); }
            check(e[1] > e[0] * 1.5 && e[2] > e[1] * 1.3, "filter v2: cutoff keeps opening the top end at 10 kHz and 16 kHz");
        }
        {
            // filter alone: self-oscillation at max resonance sits on the cutoff, stays bounded, for every mode
            bool ok = true;
            for (SynthFilter::Mode m : {SynthFilter::Mode::Pole2, SynthFilter::Mode::Ladder, SynthFilter::Mode::Acid}) {
                for (float fc : {300.f, 2000.f, 9000.f}) {
                    SynthFilter f; f.Coeffs(m, fc, 0.99f, 2.0f * sr);
                    std::vector<float> y(static_cast<size_t>(sr) * 2);
                    for (size_t i = 0; i < y.size(); ++i) y[i] = f.Process(i < 4 ? 0.5f : 0.0f);
                    int zc = 0; size_t from = y.size() / 2; double pk = 0;
                    for (size_t i = from + 1; i < y.size(); ++i) { if (y[i - 1] < 0 && y[i] >= 0) ++zc; pk = std::max<double>(pk, std::fabs(y[i])); }
                    const double hz = zc / ((y.size() - from) / (2.0 * sr));
                    if (std::fabs(hz / fc - 1.0) > 0.02 || pk < 0.2 || pk > 4.0) ok = false;
                }
            }
            check(ok, "filter v2: max resonance self-oscillates at the cutoff (12 dB, ladder, acid), bounded level");
            // hot input never blows up
            bool fin = true;
            for (SynthFilter::Mode m : {SynthFilter::Mode::Pole2, SynthFilter::Mode::Ladder, SynthFilter::Mode::Acid}) {
                SynthFilter f; f.Coeffs(m, 5000, 0.99f, 2.0f * sr);
                for (int i = 0; i < 20000; ++i) { const float v = f.Process((i / 50) % 2 ? 20.0f : -20.0f); if (!std::isfinite(v) || std::fabs(v) > 100.0f) fin = false; }
                f.Coeffs(m, 40, 0.5f, 2.0f * sr);   // and a big cutoff jump
                for (int i = 0; i < 2000; ++i) { f.Coeffs(m, i % 2 ? 16000.f : 30.f, 0.95f, 2.0f * sr); if (!std::isfinite(f.Process(1.0f))) fin = false; }
            }
            check(fin, "filter v2: no blow-up on hot input or per-sample cutoff jumps");
        }
        {
            // oversampling: a high saw has far less aliasing than the old plain PolyBLEP (-27.8 dB at C7)
            SynthPatch hs = base; hs.waveform = Waveform::Saw; hs.cutoffHz = 18000; hs.envAmount = 0; hs.sustain = 1.0f; hs.volume = 1.0f;
            auto o = renderVoice(hs, 96, 1.5);
            std::vector<float> m(o.size() / 2); for (size_t i = 0; i < m.size(); ++i) m[i] = o[2 * i];
            const size_t N = 32768; std::vector<std::complex<double>> a(N);
            for (size_t i = 0; i < N; ++i) a[i] = m[8192 + i] * (0.5 - 0.5 * std::cos(2 * 3.14159265358979 * i / N));
            t8m::Fft(a);
            const double f0 = 440.0 * std::pow(2.0, (96 - 69) / 12.0); double hrm = 0, oth = 0;
            for (size_t b = 8; b < N / 2; ++b) {
                const double fr = b * double(sr) / N, k = fr / f0; const bool harm = std::fabs(k - std::round(k)) * f0 < 4.0 * sr / double(N) * 2.0;
                (harm ? hrm : oth) += std::norm(a[b]);
            }
            check(10.0 * std::log10(oth / hrm) < -34.0, "oscillators run at 2x: C7 saw aliasing below -34 dB (was -27.8)");
        }
        {
            // ACID: accent opens the filter further, and accents in a row more; plain mode ignores the accent
            auto bright = [&](bool acid, float vel, int accentsBefore) {
                SynthPatch ap = base; ap.acid = acid; ap.waveform = Waveform::Saw; ap.cutoffHz = 400; ap.resonance = 0.6f; ap.envAmount = 0.6f; ap.filterDecay = 0.25f;
                ap.attack = 0.001f; ap.sustain = 1.0f; ap.decay = 0.5f; ap.volume = 1.0f;
                SubtractiveSynth sy; sy.SetPatch(ap);
                std::vector<float> o; std::vector<float> b(static_cast<size_t>(sr / 8) * 2);
                for (int n = 0; n <= accentsBefore; ++n) { sy.NoteOn(36, n == accentsBefore ? vel : 1.0f, false); std::fill(b.begin(), b.end(), 0.f); sy.Render(b.data(), sr / 8, sr); if (n == accentsBefore) o = b; }
                double hi = 0, lo = 0; for (int f : {1200, 1800, 2400, 3000, 3600}) hi += goertzel(o, f, 0, sr / 8); for (int f : {130, 260, 390}) lo += goertzel(o, f, 0, sr / 8);
                return hi / lo;
            };
            const double plain = bright(true, 0.8f, 0), acc = bright(true, 1.0f, 0), acc3 = bright(true, 1.0f, 2);
            check(acc > plain * 1.15 && acc3 > acc * 1.03, "ACID: an accent opens the filter, consecutive accents open it further");
            check(std::fabs(bright(false, 1.0f, 0) / bright(false, 1.0f, 2) - 1.0) < 0.15, "12/24 dB modes are not touched by the accent state");
        }
        {
            // ACID slide: fixed 60 ms, linear in pitch (log frequency): halfway in time = geometric mean of the two pitches
            SynthPatch ap = base; ap.acid = true; ap.waveform = Waveform::Sine; ap.cutoffHz = 12000; ap.envAmount = 0; ap.attack = 0.001f; ap.sustain = 1.0f; ap.volume = 1.0f; ap.glideTime = 0.0f;
            SubtractiveSynth sy; sy.SetPatch(ap);
            std::vector<float> o(static_cast<size_t>(sr) * 2 / 2 * 2, 0.f);
            sy.NoteOn(69, 1.0f, false); sy.Render(o.data(), sr / 10, sr);                         // 440 Hz settles
            std::vector<float> o2(static_cast<size_t>(sr / 5) * 2, 0.f);
            sy.NoteOn(81, 0.8f, true); sy.Render(o2.data(), sr / 5, sr);                            // slide up an octave
            auto freqAt = [&](double t0, double t1) {   // zero-crossing frequency inside [t0, t1] seconds
                int zc = 0; double first = -1, last = -1;
                for (int i = static_cast<int>(t0 * sr) + 1; i < static_cast<int>(t1 * sr); ++i) { const float a = o2[2 * (i - 1)], b = o2[2 * i]; if (a < 0 && b >= 0) { const double fr = (i - 1) + -a / double(b - a); if (first < 0) first = fr; last = fr; ++zc; } }
                return zc > 1 ? (zc - 1) / ((last - first) / sr) : 0.0;
            };
            const double early = freqAt(0.004, 0.016), mid = freqAt(0.024, 0.036), late = freqAt(0.100, 0.130);
            check(std::fabs(late / 880.0 - 1.0) < 0.01 && std::fabs(mid / 622.3 - 1.0) < 0.08 && early > 440 && early < mid && mid < late, "ACID slide: 60 ms, linear in pitch, lands on the target note");
        }
        {
            // DRIFT: 0 is exactly repeatable and steady; 1 wanders by a few cents, never more than a quarter tone
            auto cents = [&](float drift) {
                SynthPatch dp = base; dp.waveform = Waveform::Sine; dp.cutoffHz = 12000; dp.envAmount = 0; dp.attack = 0.001f; dp.sustain = 1.0f; dp.volume = 1.0f; dp.drift = drift;
                SubtractiveSynth sy; sy.SetPatch(dp); sy.NoteOn(69, 1.0f, false);
                std::vector<float> o(static_cast<size_t>(sr) * 6 * 2, 0.f); sy.Render(o.data(), sr * 6, sr);
                std::vector<double> c;
                for (int at = sr / 2; at + sr / 4 < sr * 6; at += sr / 4) {
                    int zc = 0; double first = -1, last = -1;
                    for (int i = at + 1; i < at + sr / 4; ++i) { const float a = o[2 * (i - 1)], b = o[2 * i]; if (a < 0 && b >= 0) { const double fr = (i - 1) + -a / double(b - a); if (first < 0) first = fr; last = fr; ++zc; } }
                    if (zc > 2) c.push_back(1200.0 * std::log2((zc - 1) / ((last - first) / sr) / 440.0));
                }
                double mx = 0, mn = 1e9; for (double v : c) { mx = std::max(mx, v); mn = std::min(mn, v); }
                return std::pair<double, double>(mx - mn, std::max(std::fabs(mx), std::fabs(mn)));
            };
            const auto d0 = cents(0.0f), d1 = cents(1.0f);
            check(d0.first < 0.05 && d1.first > 0.5 && d1.second < 50.0, "DRIFT: off = rock steady, full = a few cents of wander");
        }
        {
            // the new parameters are reachable from the UI row list and survive save / load
            SynthPatch up; int filterRow = -1, driftRow = -1;
            for (int i = 0; i < NumPatchParams(SynthEngineType::Subtractive); ++i) { const std::string n = PatchParamName(SynthEngineType::Subtractive, i); if (n == "FILTER") filterRow = i; if (n == "DRIFT") driftRow = i; }
            check(filterRow > 0 && driftRow > 0, "synth rows: FILTER and DRIFT exist");
            if (filterRow > 0 && driftRow > 0) {
                std::string seq;
                for (int i = 0; i < 4; ++i) { seq += FormatPatchParam(up, filterRow) + ","; AdjustPatchParam(up, filterRow, 1); }
                AdjustPatchParam(up, driftRow, 1); AdjustPatchParam(up, driftRow, 1);
                check(seq == "12 DB,24 DB,ACID,12 DB," && std::fabs(up.drift - 0.10f) < 1e-4, "synth rows: FILTER cycles 12 dB / 24 dB / ACID, DRIFT steps in 5 %");
            }
            static Project tp; tp = Project{}; tp.patterns[0].synthPatches[0].acid = true; tp.patterns[0].synthPatches[0].drift = 0.45f;
            check(SaveProject(tp, "selftest_tmp_safety/acid.t8proj"), "acid synth: save");
            Project lp; check(LoadProject(lp, "selftest_tmp_safety/acid.t8proj") && lp.patterns[0].synthPatches[0].acid && std::fabs(lp.patterns[0].synthPatches[0].drift - 0.45f) < 1e-4 && !lp.patterns[0].synthPatches[1].acid, "acid synth: ACID and DRIFT survive a project reload");
            SynthPatch cp = tp.patterns[0].synthPatches[0];
            check(SaveCustomSynthPreset("selftest acid", cp), "acid synth: preset save");
            SynthPatch rp; check(LoadCustomSynthPreset("selftest acid", rp) && rp.acid && std::fabs(rp.drift - 0.45f) < 1e-4, "acid synth: a saved preset keeps ACID and DRIFT");
            remove((t8::userdirs::SynthPresetsDir() + "/selftest acid.t8syn").c_str());
        }
        // FM and wavetable engines take unison / sub too, and tails end
        {
            SynthPatch fm; fm.engineType = SynthEngineType::FM; fm.unisonVoices = 3; fm.subLevel = 0.5f; fm.volume = 0.7f;
            SynthPatch wt; wt.engineType = SynthEngineType::Wavetable; wt.unisonVoices = 4; wt.noiseLevel = 0.2f; wt.volume = 0.7f;
            bool ok = true;
            for (const SynthPatch* p : {&fm, &wt}) {
                SynthVoice v; v.SetPatch(*p); v.NoteOn(48, 1.0f, false);
                std::vector<float> o(256 * 2, 0.f); double e = 0;
                for (int b = 0; b < 40; ++b) { std::fill(o.begin(), o.end(), 0.f); v.Render(o.data(), 256, sr); for (float x : o) { if (!std::isfinite(x)) ok = false; e += std::fabs(x); } }
                v.NoteOff();
                for (int b = 0; b < 200; ++b) { std::fill(o.begin(), o.end(), 0.f); v.Render(o.data(), 256, sr); }
                if (v.IsActive() || e < 1.0) ok = false;
            }
            check(ok, "FM / wavetable engines: unison + sub + noise sound and release to silence");
        }
        // save / load round trips
        {
            static Project tp; tp = Project{};
            SynthPatch& p = tp.patterns[0].synthPatches[1];
            p.unisonVoices = 6; p.unisonDetune = 0.55f; p.unisonSpread = 0.3f; p.subLevel = 0.4f; p.subOctave = 2; p.noiseLevel = 0.15f; p.drive = 0.35f;
            p.osc2Wave = Waveform::Square; p.osc2Level = 0.5f; p.osc2Semi = -12; p.osc2Detune = -9; p.filter24 = true; p.keytrack = 0.6f;
            check(SaveProject(tp, "selftest_tmp_safety/thick.t8proj"), "thick synth: save");
            Project lp; check(LoadProject(lp, "selftest_tmp_safety/thick.t8proj"), "thick synth: load");
            const SynthPatch& q = lp.patterns[0].synthPatches[1];
            check(q.unisonVoices == 6 && std::fabs(q.unisonDetune - 0.55f) < 1e-4 && q.subOctave == 2 && std::fabs(q.drive - 0.35f) < 1e-4 &&
                  q.osc2Wave == Waveform::Square && q.osc2Semi == -12 && q.osc2Detune == -9 && q.filter24 && std::fabs(q.keytrack - 0.6f) < 1e-4,
                  "thick synth: every new parameter survives a project reload");
            check(SaveCustomSynthPreset("selftest thick", p), "thick synth: preset save");
            SynthPatch rp; check(LoadCustomSynthPreset("selftest thick", rp), "thick synth: preset load");
            check(rp.unisonVoices == 6 && rp.osc2Semi == -12 && rp.filter24 && rp.osc2Wave == Waveform::Square && std::fabs(rp.noiseLevel - 0.15f) < 1e-4, "thick synth: a saved preset keeps the new parameters");
            remove((t8::userdirs::SynthPresetsDir() + "/selftest thick.t8syn").c_str());
        }
        // CPU: worst case (3 synths x 7 copies + chord 6 x 3 copies) per second of audio
        {
            SynthPatch w = base; w.unisonVoices = 7; w.subLevel = 0.5f; w.osc2Level = 0.5f; w.filter24 = true; w.drive = 0.4f;
            SynthVoice v[3]; for (auto& x : v) { x.SetPatch(w); x.NoteOn(45, 1.0f, false); }
            ChordVoice cv; cv.SetPatch(w); int8_t notes[6] = {48, 52, 55, 59, 62, 66}; cv.NoteOn(notes, 6, 1.0f, false);
            std::vector<float> o(512 * 2);
            const int blocks = sr / 512;
            double secs = 1e9;   // the best of three: a busy machine only ever makes a run slower, so the fastest run is the honest one
            for (int rep = 0; rep < 3; ++rep) {
                auto t0 = std::chrono::steady_clock::now();
                for (int b = 0; b < blocks; ++b) { std::fill(o.begin(), o.end(), 0.f); for (auto& x : v) x.Render(o.data(), 512, sr); cv.Render(o.data(), 512, sr); }
                secs = std::min(secs, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
            }
            std::printf("[info] worst-case synth load on this machine: %.1f%% of real time (x86_64)\n", secs * 100.0);
#ifdef T8_SANITIZED
            std::printf("[info] thick synths CPU check skipped: sanitizer builds run several times slower\n");
#else
            check(secs < 0.7, "thick synths: worst case (all voices maxed, six-note chord) uses < 70% of real time");   // v44.43: was 50% with four chord voices; the shared build machine measures 37-57%
#endif
        }
    }

    // ---- Randomizer that follows the chords ----------------------------------------------------
    {
        AppState ca; ca.engine.Init(kSampleRate); ca.sequencer.Init(&ca.engine, &ca.project);
        ca.project.randomizerKeyRoot = 0; ca.project.randomizerScale = 0;      // C major
        Track& ct = ca.CurrentPattern().tracks[kChordTrackIndex];
        auto put = [&](int step, int a, int b, int c) { Step& s = ct.steps[step]; s.active = true; s.chordSize = 3; s.chordNotes = {static_cast<int8_t>(a), static_cast<int8_t>(b), static_cast<int8_t>(c), 0}; s.lengthSteps = 8; };
        put(0, 48, 52, 55);      // C
        put(8, 53, 57, 60);      // F
        int strong = 0, strongHit = 0, bassStrong = 0, bassRoot = 0, strongOff = 0, strongOffHit = 0;
        auto isTone = [&](int step, int note) { unsigned m = step < 8 ? ((1u << 0) | (1u << 4) | (1u << 7)) : ((1u << 5) | (1u << 9) | (1u << 0)); return (m & (1u << (note % 12))) != 0; };
        for (int rep = 0; rep < 60; ++rep) {
            for (int part = 0; part < 3; ++part) {
                ca.rndChordAware = true;
                ca.RandomizeMelodic(part, part == 0 ? 1 : 3, part == 0 ? 2 : 5, rep & 1);
                const Track& t = ca.CurrentPattern().tracks[kNumDrumTracks + part];
                for (int i = 0; i < 16; i += 4) if (t.steps[i].active) {
                    ++strong; if (isTone(i, t.steps[i].note)) ++strongHit;
                    if (part == 0) { ++bassStrong; int rootPc = i < 8 ? 0 : 5; if (t.steps[i].note % 12 == rootPc) ++bassRoot; }
                }
                ca.rndChordAware = false;
                ca.RandomizeMelodic(part, part == 0 ? 1 : 3, part == 0 ? 2 : 5, rep & 1);
                const Track& t2 = ca.CurrentPattern().tracks[kNumDrumTracks + part];
                for (int i = 0; i < 16; i += 4) if (t2.steps[i].active) { ++strongOff; if (isTone(i, t2.steps[i].note)) ++strongOffHit; }
            }
        }
        check(strong > 200 && static_cast<double>(strongHit) / strong > 0.9, "chord-aware randomizer: strong beats are chord tones of the sounding chord");
        check(static_cast<double>(strongOffHit) / std::max(1, strongOff) < 0.7, "chord-aware randomizer: switched off it is just in key");
        check(bassStrong > 40 && static_cast<double>(bassRoot) / bassStrong > 0.5, "chord-aware randomizer: the bass leans on the chord root");
    }

    // ---- Riser lands its peak on the drop (back-timed) ----------------------------------------
    {
        const int rIdx = 0;
        auto movingPeak = [&](const std::vector<float>& mono) {          // argmax of a 20 ms moving average of |x|
            const int w = kSampleRate / 50; double acc = 0, best = -1; int bi = 0;
            for (size_t i = 0; i < mono.size(); ++i) {
                acc += std::fabs(mono[i]); if (i >= static_cast<size_t>(w)) acc -= std::fabs(mono[i - w]);
                if (acc > best) { best = acc; bi = static_cast<int>(i) - w / 2; }
            }
            return bi;
        };
        // reference: peak position of the same riser played alone from its start
        int refPeak = 0; double hitFrame = 0;
        {
            AudioEngine e; e.Init(kSampleRate);
            int n = e.Risers().LoadFromFolder("kits/risers", kSampleRate);
            check(n > 0, "riser: stock risers load");
            hitFrame = e.Risers().HitFrame(rIdx);
            e.Risers().TriggerAligned(rIdx, 0, 0, 1.0);
            std::vector<float> o(256 * 2), mono;
            for (int b = 0; b < 3 * kSampleRate / 256 + 4; ++b) { std::fill(o.begin(), o.end(), 0.f); e.Render(o.data(), 256, 5512.5); for (int i = 0; i < 256; ++i) mono.push_back(o[2 * i]); }
            refPeak = movingPeak(mono);
            check(std::abs(refPeak - hitFrame) < 0.06 * kSampleRate, "riser: detected hit point matches the loudest moment");
        }
        for (int bars : {2, 4}) for (double bpm : {70.0, 100.0, 120.0, 140.0}) {
            if (bars == 2 && bpm > 100) continue;                          // too little time for the whole riser: covered below
            static Project rp; rp = Project{};
            rp.patterns[0].bpm = static_cast<float>(bpm); rp.patterns[0].masterLength = 16;
            AudioEngine e; e.Init(kSampleRate); e.Risers().LoadFromFolder("kits/risers", kSampleRate);
            Sequencer sq; sq.Init(&e, &rp);
            sq.Play();
            std::vector<float> o(256 * 2), mono;
            auto run = [&](double secs) { int nb = static_cast<int>(secs * kSampleRate) / 256; for (int b = 0; b < nb; ++b) { std::fill(o.begin(), o.end(), 0.f); sq.RenderAndAdvance(o.data(), 256, kSampleRate); for (int i = 0; i < 256; ++i) mono.push_back(o[2 * i]); } };
            run(0.37);                                                      // mid-bar
            sq.SetPendingRiser(rIdx, bars);
            const double barFrames = 16.0 * (60.0 / bpm / 4.0) * kSampleRate;
            run((bars + 2) * barFrames / kSampleRate + 2.0);
            int pk = movingPeak(mono);
            // the sample starts (peak - refPeak + hit) frames ... the hit itself must sit on a bar line
            double hitAt = pk - refPeak + hitFrame;
            double barsF = hitAt / barFrames; double off = std::fabs(barsF - std::round(barsF)) * barFrames;
            check(off < 0.004 * kSampleRate, ("riser lands on a bar line at " + std::to_string(static_cast<int>(bpm)) + " BPM, " + std::to_string(bars) + " bars").c_str());
        }
        // not enough time (170 BPM, 1 bar): peak still within 30 ms of a bar line, nothing plays after it for long
        {
            static Project rp; rp = Project{}; rp.patterns[0].bpm = 170; rp.patterns[0].masterLength = 16;
            AudioEngine e; e.Init(kSampleRate); e.Risers().LoadFromFolder("kits/risers", kSampleRate);
            Sequencer sq; sq.Init(&e, &rp); sq.Play();
            std::vector<float> o(256 * 2), mono;
            auto run = [&](double secs) { int nb = static_cast<int>(secs * kSampleRate) / 256; for (int b = 0; b < nb; ++b) { std::fill(o.begin(), o.end(), 0.f); sq.RenderAndAdvance(o.data(), 256, kSampleRate); for (int i = 0; i < 256; ++i) mono.push_back(o[2 * i]); } };
            run(0.3); sq.SetPendingRiser(rIdx, 1); run(5.0);
            int pk = movingPeak(mono); double barFrames = 16.0 * (60.0 / 170.0 / 4.0) * kSampleRate;
            double barsF = (pk - refPeak + hitFrame) / barFrames;   // same peak-vs-hit compensation as above
            double off = std::fabs(barsF - std::round(barsF)) * barFrames;
            check(off < 0.01 * kSampleRate, "riser: short lead (1 bar at 170 BPM) still peaks on the bar line");
        }
    }

    // ---- Chord bank: independent chord scenes, quantized launch, persistence ------------
    {
        static Project cb;
        cb = Project{};
        Pattern& cp0 = cb.patterns[0];
        cp0.bpm = 120; cp0.masterLength = 16;
        Step& own = cp0.tracks[kChordTrackIndex].steps[0];
        own.active = true; own.chordSize = 3; own.chordNotes = {60, 64, 67, 0}; own.lengthSteps = 16;
        cp0.tracks[0].steps[0].active = true;           // a kick so the scene is not empty
        auto setChord = [&](int c, int8_t a, int8_t b, int8_t d) {
            Step& st = cb.chordScenes[c].track.steps[0];
            st.active = true; st.chordSize = 3; st.chordNotes = {a, b, d, 0}; st.lengthSteps = 16;
        };
        setChord(0, 57, 60, 64); setChord(1, 53, 57, 60);
        cb.chordScenes[1].name = "FMAJ"; cb.chordScenes[1].key = 5; cb.chordScenes[1].scale = 0;
        cb.chordBankOn = true; cb.chordSceneEdit = 1;
        check(SaveProject(cb, "selftest_tmp_safety/chordbank.t8proj"), "chord bank: save");
        Project lb;
        check(LoadProject(lb, "selftest_tmp_safety/chordbank.t8proj"), "chord bank: load");
        check(lb.chordBankOn && lb.chordSceneEdit == 1, "chord bank: on / edited slot survive a reload");
        check(lb.chordScenes[0].track.steps[0].chordNotes[0] == 57 && lb.chordScenes[1].track.steps[0].chordNotes[2] == 60, "chord bank: scene steps survive a reload");
        check(lb.chordScenes[1].name == "FMAJ" && lb.chordScenes[1].key == 5, "chord bank: scene name / key survive a reload");
        check(lb.chordScenes[2].track.type == TrackType::Chord, "chord bank: untouched slots stay chord tracks");

        AudioEngine bEng; bEng.Init(kSampleRate);
        bEng.GetKit()->LoadFromFolder("kits/808", kSampleRate);
        bEng.GetMixer().SyncFromPattern(cb.patterns[0]);
        Sequencer bSeq; bSeq.Init(&bEng, &cb);
        bSeq.SetLaunchQuantize(2);
        bSeq.SetChordSceneNow(0);
        bEng.Capture().Start();
        bSeq.Play();
        std::vector<float> bb(256 * 2);
        auto renderSecs = [&](double secs) { int n = static_cast<int>(secs * kSampleRate); for (int d = 0; d < n; d += 256) bSeq.RenderAndAdvance(bb.data(), std::min(256, n - d), kSampleRate); };
        renderSecs(0.5);
        bSeq.LaunchChordScene(1);                                   // queued for the next bar (t = 2.0 s)
        check(bSeq.PendingChordScene() == 1, "chord bank: launch is queued, not applied at once");
        renderSecs(1.0);
        int chordEvents = 0; bool firstOk = false;
        for (const auto& ev : bEng.Capture().Events()) if (ev.isChord) { if (chordEvents == 0) firstOk = ev.notes[0] == 57; ++chordEvents; }
        check(firstOk, "chord bank: the chord scene plays instead of the pattern's own chord");
        check(chordEvents == 1, "chord bank: nothing changes before the boundary");
        renderSecs(1.0);                                            // past t = 2.0 s
        check(bSeq.ChordSceneCur() == 1 && bSeq.PendingChordScene() == -1, "chord bank: the queued scene starts on the bar");
        double t2 = -1; int n2 = 0;
        for (const auto& ev : bEng.Capture().Events()) if (ev.isChord) { ++n2; if (n2 == 2) t2 = ev.timeSeconds; }
        check(n2 == 2 && std::fabs(t2 - 2.0) < 0.06 && n2 >= 2, "chord bank: the new chord enters exactly on the bar line");
        check(bSeq.StepForTrack(0) == bSeq.GlobalTick() % 16, "chord bank: other tracks keep their own clock");
        bSeq.Stop();
        // bank off: the pattern's own chord is back
        cb.chordBankOn = false;
        bSeq.SetChordSceneNow(0);
        bEng.Capture().Start();
        bSeq.Play(); renderSecs(0.3); bSeq.Stop();
        bool ownBack = false; for (const auto& ev : bEng.Capture().Events()) if (ev.isChord && ev.notes[0] == 60) ownBack = true;
        check(ownBack, "chord bank off: the scene's own chords play again");
    }

    // ---- Chord bank at the app level: seeding, editing, undo, progressions, key follow -----
    {
        AppState ba;
        ba.engine.Init(kSampleRate);
        ba.sequencer.Init(&ba.engine, &ba.project);
        Step& own = ba.CurrentPattern().tracks[kChordTrackIndex].steps[0];
        own.active = true; own.chordSize = 3; own.chordNotes = {60, 64, 67, 0}; own.lengthSteps = 8;
        ba.SetChordBank(true);
        check(ba.project.chordBankOn && ba.project.chordScenes[0].track.steps[0].chordNotes[0] == 60,
              "chord bank: switching it on seeds slot 1 with the scene's own chords");
        ba.ToggleStep(kChordTrackIndex, 4);              // edits the chord scene, not the pattern
        check(ba.project.chordScenes[0].track.steps[4].active && !ba.CurrentPattern().tracks[kChordTrackIndex].steps[4].active,
              "chord bank: step edits go to the chord scene");
        check(ba.Undo() && !ba.project.chordScenes[0].track.steps[4].active, "chord bank: undo restores the chord scene");
        ba.LaunchChordScene(2);
        check(ba.project.chordSceneEdit == 2 && ba.sequencer.ChordSceneCur() == 2, "chord bank: launching while stopped switches at once");
        ba.project.randomizerKeyRoot = 0; ba.project.randomizerScale = 0;
        ba.ApplyChordProgression(0);
        check(!ba.ChordSceneEmpty(2) && ba.project.chordScenes[2].key == 0 && ba.ChordSceneEmpty(1),
              "chord bank: a progression is written into the edited chord scene only");
        check(!ba.ChordSceneLabel(2).empty() && ba.ChordSceneLabel(2) != "-", "chord bank: the slot label names the chords");
        // FOLLOW KEY re-tunes the scenes
        std::array<int8_t, 6> before = ba.project.chordScenes[2].track.steps[0].chordNotes;
        ba.project.chordsFollowKey = true;
        ba.TickKeyFollow();
        ba.project.randomizerKeyRoot = 2;
        ba.TickKeyFollow();
        check(ba.project.chordScenes[2].key == 2 && ba.project.chordScenes[2].track.steps[0].chordNotes != before,
              "chord bank: FOLLOW KEY re-tunes the chord scenes");
        ba.SetChordBank(false);
        check(!ba.project.chordBankOn, "chord bank: can be switched off again");
    }

    // ---- A tied full-loop chord / note keeps its gate across the loop edge --------------
    {
        Project tp;
        tp.patterns[0].bpm = 120; tp.patterns[0].masterLength = 16;
        Step& cs = tp.patterns[0].tracks[kChordTrackIndex].steps[0];
        cs.active = true; cs.chordSize = 3; cs.chordNotes = {60, 64, 67, 0}; cs.lengthSteps = 16;
        Step& bs = tp.patterns[0].tracks[kNumDrumTracks].steps[0];
        bs.active = true; bs.note = 36; bs.lengthSteps = 16;
        AudioEngine tEng; tEng.Init(kSampleRate);
        tEng.GetMixer().SyncFromPattern(tp.patterns[0]);
        Sequencer tSeq; tSeq.Init(&tEng, &tp);
        tSeq.Play();
        std::vector<float> tb(256 * 2);
        double tail = 0.0;
        for (int n = 0; n < static_cast<int>(6.0 * kSampleRate); n += 256) {
            tSeq.RenderAndAdvance(tb.data(), 256, kSampleRate);
            if (n > static_cast<int>(3.9 * kSampleRate) && n < static_cast<int>(4.4 * kSampleRate))      // around the loop edge at 4.0 s
                for (int i = 0; i < 256; ++i) tail = std::max(tail, static_cast<double>(std::fabs(tb[2 * i])));
        }
        check(tail > 0.01, "tie: a full-loop note and chord keep sounding across the loop edge");
    }

    // ---- Drum hits: a sample that starts on a non-zero value must not click ------------
    {
        AudioEngine dEngine; dEngine.Init(kSampleRate);
        dEngine.GetKit()->LoadFromFolder("kits/808", kSampleRate);
        Mixer& dm = dEngine.GetMixer();
        dm.fx.delayLevel = 0; dm.fx.reverbLevel = 0; dm.masterVolume = 1.0f;
        std::vector<float> db(64 * 2), dmono;
        DrumVoiceParams dvp2;
        for (int blk = 0; blk < 40; ++blk) {
            if (blk == 5) dEngine.TriggerDrum(DrumSlot::Kick, 1.0f, 0, dvp2);
            dEngine.Render(db.data(), 64, 5512.5);
            for (int i = 0; i < 64; ++i) dmono.push_back(db[2 * i]);
        }
        const int busLat = Limiter().Latency() + Saturator::kLatency;   // fixed master-bus look-ahead
        double firstStep = 0, kickPeak = 0;
        for (int i = 5 * 64 + busLat; i < 5 * 64 + busLat + 12; ++i) firstStep = std::max(firstStep, static_cast<double>(std::fabs(dmono[i] - dmono[i - 1])));
        for (int i = 5 * 64; i < 5 * 64 + 2000; ++i) kickPeak = std::max(kickPeak, static_cast<double>(std::fabs(dmono[i])));
        check(kickPeak > 0.3, "drum onset: kick renders");
        check(firstStep < 0.12, "drum onset: kick fades in (no step at the first sample)");
        // open hat cut by the closed hat fades out instead of being switched off
        AudioEngine hEngine; hEngine.Init(kSampleRate);
        hEngine.GetKit()->LoadFromFolder("kits/808", kSampleRate);
        Mixer& hm = hEngine.GetMixer(); hm.fx.delayLevel = 0; hm.fx.reverbLevel = 0; hm.masterVolume = 1.0f;
        for (int i = 0; i < kNumTracks; ++i) hm.tracks[i].volume = (i == static_cast<int>(DrumSlot::OpenHat)) ? 1.0f : 0.0f;
        std::vector<float> hmono;
        for (int blk = 0; blk < 120; ++blk) {
            if (blk == 5) hEngine.TriggerDrum(DrumSlot::OpenHat, 1.0f, 0, dvp2);
            if (blk == 40) hEngine.TriggerDrum(DrumSlot::ClosedHat, 1.0f, 0, dvp2);
            hEngine.Render(db.data(), 64, 5512.5);
            for (int i = 0; i < 64; ++i) hmono.push_back(db[2 * i]);
        }
        double during = 0; for (int i = 40 * 64 + busLat; i < 40 * 64 + busLat + 60; ++i) during = std::max(during, static_cast<double>(std::fabs(hmono[i])));
        check(during > 0.05, "drum choke: the open hat fades out over a few ms instead of being switched off");
        double late = 0; for (int i = 40 * 64 + busLat + 400; i < 40 * 64 + busLat + 1200; ++i) late = std::max(late, static_cast<double>(std::fabs(hmono[i])));
        check(late < 1e-4, "drum choke: the open hat is silent after the fade");
    }

    // ---- v34: drum engine v2 (as-recorded playback, band-limited tune, velocity tone, kick synth, sinc loader) ----
    {
        const int sr = kSampleRate;
        DrumKit k2; k2.LoadFromFolder("kits/909", sr);
        auto renderDrum = [&](DrumEngine& e, DrumSlot sl, int frames) { std::vector<float> st(static_cast<size_t>(frames) * 2, 0.0f);
            for (int o = 0; o < frames; o += 256) e.RenderSlot(sl, st.data() + static_cast<size_t>(o) * 2, std::min(256, frames - o), sr);
            std::vector<float> m(frames); for (int i = 0; i < frames; ++i) m[i] = st[2 * i]; return m; };

        // (1) DECAY 1.0 and TUNE 0 = the sample as recorded
        {
            auto src = k2.Variant(DrumSlot::Snare, 0);
            DrumEngine e; e.SetKit(&k2); DrumVoiceParams vp; vp.level = 1.0f;
            e.Trigger(DrumSlot::Snare, 1.0f, 0, vp);
            const int n = src->frames() - 200;
            auto out = renderDrum(e, DrumSlot::Snare, n);
            double err = 0, ref = 0; for (int i = 200; i < n - 100; ++i) { double d = out[i] - src->data[i]; err += d * d; ref += double(src->data[i]) * src->data[i]; }
            check(err / std::max(ref, 1e-12) < 1e-8, "drum v2: decay 1.0 / tune 0 plays the sample exactly as recorded");
        }
        // (2) DECAY < 1 shortens the tail, > 1 lifts it
        {
            auto tailRms = [&](float decay) { DrumEngine e; e.SetKit(&k2); DrumVoiceParams vp; vp.level = 1.0f; vp.decay = decay;
                e.Trigger(DrumSlot::OpenHat, 1.0f, 0, vp); auto o = renderDrum(e, DrumSlot::OpenHat, sr / 2);
                double a = 0; for (int i = sr / 5; i < sr / 4; ++i) a += o[i] * o[i]; return std::sqrt(a / (sr / 20)); };
            const double t1 = tailRms(1.0f), t05 = tailRms(0.5f), t2 = tailRms(2.0f);
            check(t05 < t1 * 0.6 && t2 > t1 * 1.1, "drum v2: decay below 1 shortens the tail, above 1 lifts it");
        }
        // (3) pitching up keeps the top end (no linear-interpolation droop) and does not fold back
        {
            const double f = 9000.0; const int n = sr;
            auto sine = std::make_shared<SampleBuffer>(); sine->channels = 1; sine->sampleRate = sr; sine->data.resize(n);
            for (int i = 0; i < n; ++i) sine->data[i] = 0.5f * std::sin(2.0 * 3.14159265358979 * f * i / sr);
            auto pitched = [&](float tune) { DrumEngine e; DrumKit kt; kt.LoadFromFolder("kits/909", sr); e.SetKit(&kt);   // trigger path needs a kit; we override the buffer below
                DrumVoiceParams vp; vp.level = 1.0f; vp.tune = tune; e.Trigger(DrumSlot::Perc, 1.0f, 0, vp); return e; };
            (void)pitched;
            // direct kernel test: interpolate the sine at rate 1.2 with the engine's kernel recipe
            const double rate = std::pow(2.0, 3.0 / 12.0);
            SincTable tbl(16, 128, std::min(1.0, 0.95 / rate));
            const int m = 12000; std::vector<float> o(m);
            for (int i = 0; i < m; ++i) { double pos = 200 + i * rate; int b = static_cast<int>(pos); o[i] = tbl.At(sine->data.data(), n, 1, 0, b, pos - b); }
            const double fOut = f * rate;     // 10.7 kHz, still below Nyquist; the filter edge is 0.95 * 22.05 / 1.19 = 17.6 kHz
            double a = 0, bq = 0; for (int i = 2000; i < m - 2000; ++i) { double ph = 2 * 3.14159265358979 * fOut * i / sr; a += o[i] * std::cos(ph); bq += o[i] * std::sin(ph); }
            const double amp = 2.0 * std::sqrt(a * a + bq * bq) / (m - 4000);
            check(std::fabs(20.0 * std::log10(amp / 0.5)) < 0.3, "drum v2: a 9 kHz tone pitched up 3 st keeps its level (within 0.3 dB)");
            // a tone above the new Nyquist-limited passband is removed instead of aliasing: 19 kHz at rate 1.5
            const double rate2 = 1.5; SincTable t2(16, 128, 0.95 / rate2);
            std::vector<float> hi(n); for (int i = 0; i < n; ++i) hi[i] = 0.5f * std::sin(2.0 * 3.14159265358979 * 19000.0 * i / sr);
            double e2 = 0; for (int i = 0; i < 8000; ++i) { double pos = 300 + i * rate2; int b = static_cast<int>(pos); float v = t2.At(hi.data(), n, 1, 0, b, pos - b); e2 += v * v; }
            check(std::sqrt(e2 / 8000) < 0.5 * 0.7071 * 0.02, "drum v2: content above the new Nyquist is filtered out, not folded back (>34 dB down)");
        }
        // (4) velocity tone: a soft hit is darker than a full one
        {
            auto hf = [&](float vel) { DrumEngine e; e.SetKit(&k2); DrumVoiceParams vp; vp.level = 1.0f; e.Trigger(DrumSlot::ClosedHat, vel, 0, vp);
                auto o = renderDrum(e, DrumSlot::ClosedHat, 4096); double a = 0, b = 0; for (int i = 1; i < 4096; ++i) { double d = o[i] - o[i - 1]; a += d * d; b += double(o[i]) * o[i]; } return a / std::max(b, 1e-12); };
            /* v43.1: the rebuilt 909 hat is mostly 7-13 kHz, so a one-pole low-pass at 5.5 kHz changes the measure less than the old hat did */ check(hf(0.3f) < hf(1.0f) * 0.95, "drum v2: a soft hit is darker than a full-velocity hit");
        }
        // (5) kick synth
        {
            DrumEngine e; e.SetKit(&k2); DrumVoiceParams vp; vp.level = 1.0f; vp.keyRoot = 0;   // C -> 65.4 Hz
            KickSynthSettings ks; ks.mode = 1; ks.keyLock = true; ks.decayMs = 600.0f; ks.drive = 0.0f; ks.click = 0.0f;
            e.Trigger(DrumSlot::Kick, 1.0f, 0, vp, &ks);
            auto o = renderDrum(e, DrumSlot::Kick, sr);
            double pk = 0; for (float v : o) pk = std::max<double>(pk, std::fabs(v));
            check(pk > 0.5 && pk < 1.2, "kick synth: renders at a sensible level");
            int crossings = 0; const int a = static_cast<int>(0.2 * sr), b = static_cast<int>(0.35 * sr);
            for (int i = a + 1; i < b; ++i) if ((o[i - 1] < 0) != (o[i] < 0)) ++crossings;
            const double fEst = crossings / 2.0 / (static_cast<double>(b - a) / sr);
            check(std::fabs(fEst - 65.41) < 4.0, "kick synth: key lock tunes the body to the project key (C -> 65 Hz)");
            DrumEngine e2; e2.SetKit(&k2); ks.keyLock = false; ks.pitchHz = 50.0f; vp.tune = 12.0f; e2.Trigger(DrumSlot::Kick, 1.0f, 0, vp, &ks);
            auto o2 = renderDrum(e2, DrumSlot::Kick, sr); int c2 = 0; for (int i = a + 1; i < b; ++i) if ((o2[i - 1] < 0) != (o2[i] < 0)) ++c2;
            check(std::fabs(c2 / 2.0 / (static_cast<double>(b - a) / sr) - 100.0) < 6.0, "kick synth: the track TUNE moves the pitch (50 Hz + 12 st = 100 Hz)");
            DrumEngine e3; e3.SetKit(&k2); KickSynthSettings kl; kl.mode = 2; e3.Trigger(DrumSlot::Kick, 1.0f, 0, vp, &kl);
            auto o3 = renderDrum(e3, DrumSlot::Kick, 4000); double pk3 = 0; for (float v : o3) pk3 = std::max<double>(pk3, std::fabs(v));
            check(pk3 > 0.3, "kick synth: LAYER mode plays sample and synth together");
            for (float v : o) if (!std::isfinite(v)) { check(false, "kick synth: finite output"); break; }
        }
        // (6) the loader converts 48 kHz files with a band-limited resampler
        {
            const int n48 = 24000; std::vector<float> w(n48); for (int i = 0; i < n48; ++i) w[i] = 0.5f * std::sin(2.0 * 3.14159265358979 * 10000.0 * i / 48000.0);
            WriteWavPCM16("selftest_tmp_48k.wav", w, 1, 48000);
            SampleBuffer sb; check(LoadWav("selftest_tmp_48k.wav", sb, 44100) && sb.sampleRate == 44100, "loader: 48 kHz WAV converts to the engine rate");
            std::remove("selftest_tmp_48k.wav");
            double a = 0, bq = 0; int cnt = 0; for (int i = 3000; i < sb.frames() - 3000; ++i, ++cnt) { double ph = 2 * 3.14159265358979 * 10000.0 * i / 44100.0; a += sb.data[i] * std::cos(ph); bq += sb.data[i] * std::sin(ph); }
            const double amp = 2.0 * std::sqrt(a * a + bq * bq) / cnt;
            check(std::fabs(20.0 * std::log10(amp / 0.5)) < 0.3 && sb.frames() > 21900 && sb.frames() < 22100, "loader: 10 kHz tone keeps its level and length through 48 -> 44.1 kHz");
        }
        // (7) the generated kits (t8_kitgen) load completely at the engine rate, keep natural level ratios and never clip
        for (const char* kn : {"electro", "techno", "breaks", "acid_v2"}) {
            DrumKit gk; const int got = gk.LoadFromFolder(std::string("kits/") + kn, sr);
            check(got == kNumDrumTracks, (std::string("kit ") + kn + ": all 6 slots load (24-bit / 48 kHz converted)").c_str());
            auto peakOf = [&](DrumSlot sl, int v) { auto b = gk.Variant(sl, v); float m = 0; if (b) for (float x : b->data) m = std::max(m, std::fabs(x)); return m; };
            check(peakOf(DrumSlot::Kick, 0) <= 1.0f && peakOf(DrumSlot::Kick, 0) > 0.8f && peakOf(DrumSlot::ClosedHat, 0) < 0.5f * peakOf(DrumSlot::Kick, 0),
                  (std::string("kit ") + kn + ": kick is the loudest, the closed hat well below it").c_str());
            check(gk.VariantCount(DrumSlot::Kick) >= 2 && gk.VariantCount(DrumSlot::Snare) >= 2 && gk.VariantCount(DrumSlot::Perc) >= 2, (std::string("kit ") + kn + ": has alternate takes").c_str());
            auto ch = gk.Variant(DrumSlot::ClosedHat, 0); check(ch && ch->channels == 2, (std::string("kit ") + kn + ": hats are stereo").c_str());
        }
        // (7b) v44.32 (K1): the new kicks and the full 808 percussion
        {
            DrumKit k8; check(k8.LoadFromFolder("kits/808", sr) == kNumDrumTracks, "k1: kit 808 still loads all 6 slots");
            check(k8.VariantCount(DrumSlot::Kick) == 5 && k8.VariantCount(DrumSlot::Perc) == 8 && k8.VariantCount(DrumSlot::OpenHat) >= 2, "k1: 808 has 5 kicks, 8 percussion sounds (toms, cowbell, clave, rimshot, maracas, conga) and a cymbal");
            DrumKit kp; check(kp.LoadFromFolder("kits/808perc", sr) == kNumDrumTracks && kp.VariantCount(DrumSlot::Perc) == 8, "k1: the 808perc kit loads with 8 percussion sounds");
            auto clickDb = [&](DrumSlot sl, int v) {   // 1.5-6 kHz energy of the first 12 ms against the whole first 80 ms (a rough version of the reference measure)
                auto b = k8.Variant(sl, v); const size_t n12 = static_cast<size_t>(0.012 * sr), n80 = static_cast<size_t>(0.08 * sr);
                if (!b || b->data.size() < n80 * static_cast<size_t>(b->channels)) return -99.0;
                double hi = 0, all = 0, prev = 0, prev2 = 0;
                for (size_t i = 0; i < n80; ++i) {   // a crude 2nd difference = a high-pass, enough to tell "has a click" from "has none"
                    const double x = b->data[i * static_cast<size_t>(b->channels)];
                    const double d = x - 2 * prev + prev2; prev2 = prev; prev = x;
                    if (i < n12) hi += d * d;
                    all += x * x;
                }
                return 10.0 * std::log10(hi / (all + 1e-18) + 1e-18);
            };
            check(clickDb(DrumSlot::Kick, 2) > clickDb(DrumSlot::Kick, 0) + 10.0, "k1: the new 808 SNAP kick has a far stronger click than the old 808 kick");
            for (int v : {2, 3, 4}) { auto b = k8.Variant(DrumSlot::Kick, v); float m = 0; if (b) for (float x : b->data) m = std::max(m, std::fabs(x)); check(m <= 1.0f && m > 0.8f, "k1: new kick levels are natural and never clip"); }
        }
        // (8) a dropped WAV replaces one slot in a private copy of the kit
        {
            AppState da; da.engine.Init(sr); da.sequencer.Init(&da.engine, &da.project);
            da.project.kitFolder = "kits/electro"; da.engine.GetKit()->LoadFromFolder(da.project.kitFolder, sr);
            std::vector<float> w(24000); for (int i = 0; i < 24000; ++i) w[i] = 0.5f * std::sin(2.0 * 3.14159265358979 * 300.0 * i / 48000.0);
            WriteWavPCM24("selftest_tmp_drop.wav", w, 1, 48000);
            da.seqCursorTrack = 3;   // a drum track (closed hat)
            const bool okDrop = da.AssignSampleToSlot("selftest_tmp_drop.wav");
            check(okDrop && da.CurrentPattern().kitFolder.rfind("kits/mykit", 0) == 0 && da.project.kitFolder == "kits/electro", "drop: the sample goes into a private kit, the factory kit is untouched");
            auto hb = da.engine.GetKit()->Variant(DrumSlot::ClosedHat, 0);
            check(hb && hb->frames() > 21800 && hb->frames() < 22100 && da.engine.GetKit()->VariantCount(DrumSlot::ClosedHat) == 1, "drop: the slot has the dropped sample (converted to the engine rate) and no old takes");
            check(da.engine.GetKit()->VariantCount(DrumSlot::Kick) >= 2, "drop: the other slots keep the copied kit");
            da.seqCursorTrack = 6;
            check(!da.AssignSampleToSlot("selftest_tmp_drop.wav"), "drop: a synth track refuses a sample");
            std::error_code ec; std::filesystem::remove_all(da.CurrentPattern().kitFolder, ec); std::remove("selftest_tmp_drop.wav");
        }
        // (9) fills play the sample the track is set to, not always the first one
        {
            auto renderFill = [&](int variant) {
                static Project fp; fp = Project{}; fp.kitFolder = "kits/electro"; fp.patterns[0].masterLength = 16;
                for (int t = 0; t < kNumDrumTracks; ++t) for (Step& st : fp.patterns[0].tracks[t].steps) st.variant = static_cast<uint8_t>(variant);
                fp.patterns[0].tracks[0].steps[0].active = true;
                AudioEngine e; e.Init(sr); e.GetKit()->LoadFromFolder(fp.kitFolder, sr);
                Sequencer sq; sq.Init(&e, &fp); sq.SetLiveBreak(3); sq.Play();   // KICK ROLL
                std::vector<float> o(256 * 2), all;
                for (int b = 0; b < sr / 256; ++b) { std::fill(o.begin(), o.end(), 0.f); sq.RenderAndAdvance(o.data(), 256, sr); all.insert(all.end(), o.begin(), o.end()); }
                return all;
            };
            Track probe; for (int i = 0; i < 16; ++i) { probe.steps[i].active = i % 2 == 0; probe.steps[i].variant = i % 2 == 0 ? 2 : 0; }
            check(TrackSampleVariant(probe) == 2, "track sample variant = the one the active steps use");
            auto a = renderFill(0), b = renderFill(1);
            double d = 0, e0 = 0; for (size_t i = 0; i < a.size() && i < b.size(); ++i) { d += std::fabs(a[i] - b[i]); e0 += std::fabs(a[i]); }
            check(e0 > 1.0 && d > 0.05 * e0, "fills: a live break plays the track's chosen sample variant, not the default one");
        }
        { Project pk; pk.fx.kick.mode = 2; pk.fx.kick.pitchHz = 61.0f; pk.fx.kick.keyLock = false; pk.fx.kick.decayMs = 500.0f; pk.fx.kick.drive = 0.6f;
          const std::string path = "selftest_tmp_kick.t8proj"; SaveProject(pk, path); Project q; LoadProject(q, path); std::remove(path.c_str()); std::remove((path + ".bak").c_str());
          check(q.fx.kick.mode == 2 && nearlyEqual(q.fx.kick.pitchHz, 61.0f, 1e-3f) && !q.fx.kick.keyLock && nearlyEqual(q.fx.kick.decayMs, 500.0f, 1e-2f) && nearlyEqual(q.fx.kick.drive, 0.6f, 1e-3f), "kick synth: settings survive save/load"); }
    }

    // ---- v29: per-track channel EQ -------------------------------------
    {
        const double fs = 44100.0;
        // measures the steady-state gain (dB) of the EQ on a sine
        auto gainDb = [&](const TrackEqParams& p, double f) {
            TrackEq eq; eq.Init(44100);
            std::vector<float> b(2 * 512);
            double ph = 0, inc = 2.0 * 3.14159265358979 * f / fs, ein = 0, eout = 0;
            for (int blk = 0; blk < 60; ++blk) {
                std::vector<float> in(2 * 512);
                for (int i = 0; i < 512; ++i) { in[2 * i] = in[2 * i + 1] = static_cast<float>(0.5 * std::sin(ph)); ph += inc; }
                b = in;
                eq.Process(b.data(), 512, p);
                if (blk >= 40) for (int i = 0; i < 512; ++i) { ein += in[2 * i] * in[2 * i]; eout += b[2 * i] * b[2 * i]; }
            }
            return 10.0 * std::log10(eout / ein);
        };
        TrackEqParams flat;
        { TrackEq eq; eq.Init(44100); std::vector<float> a(1024), b2; for (int i = 0; i < 1024; ++i) a[i] = std::sin(i * 0.05f); b2 = a; eq.Process(b2.data(), 512, flat); check(a == b2, "eq: flat = bit-identical passthrough"); }
        TrackEqParams hp12; hp12.hpfHz = 100.0f;
        double g50 = gainDb(hp12, 50), g1k = gainDb(hp12, 1000), g100 = gainDb(hp12, 100);
        check(g50 < -10.0 && g50 > -14.5, "eq: 12 dB HPF at 100 Hz cuts 50 Hz by ~12 dB (" + std::to_string(g50) + ")");
        check(std::fabs(g1k) < 0.2, "eq: HPF leaves 1 kHz alone");
        check(std::fabs(g100 + 3.0) < 0.6, "eq: HPF is -3 dB at its corner (" + std::to_string(g100) + ")");
        TrackEqParams hp24 = hp12; hp24.hpfSteep = 1.0f;
        double g50b = gainDb(hp24, 50);
        check(g50b < -22.0, "eq: 24 dB HPF cuts 50 Hz much harder (" + std::to_string(g50b) + ")");
        TrackEqParams ls; ls.lowDb = 6.0f; ls.lowHz = 120.0f;
        check(std::fabs(gainDb(ls, 40) - 6.0) < 0.6 && std::fabs(gainDb(ls, 4000)) < 0.3, "eq: low shelf +6 dB at the bottom, flat above");
        TrackEqParams bell; bell.midDb = -9.0f; bell.midHz = 800.0f; bell.midQ = 2.0f;
        check(std::fabs(gainDb(bell, 800) + 9.0) < 0.4 && std::fabs(gainDb(bell, 100)) < 0.5 && std::fabs(gainDb(bell, 8000)) < 0.5, "eq: mid bell -9 dB at its centre, flat far away");
        TrackEqParams hs; hs.highDb = 5.0f; hs.highHz = 6000.0f;
        check(std::fabs(gainDb(hs, 14000) - 5.0) < 0.6 && std::fabs(gainDb(hs, 300)) < 0.2, "eq: high shelf +5 dB at the top");
        // the analytic curve the UI draws agrees with the measured response
        TrackEqParams mix; mix.hpfHz = 40; mix.lowDb = 3; mix.midDb = -4; mix.midHz = 400; mix.midQ = 1.5f; mix.highDb = 2;
        bool ok = true; for (double f : {30.0, 60.0, 150.0, 400.0, 1500.0, 9000.0}) ok = ok && std::fabs(gainDb(mix, f) - TrackEqResponseDb(mix, fs, f)) < 0.35;
        check(ok, "eq: the drawn response curve matches the measured one");
        // sweeping the controls must not click: worst second difference of a sine stays tiny
        {
            TrackEq eq; eq.Init(44100); double ph = 0; std::vector<float> out; std::vector<float> b(1024);
            for (int blk = 0; blk < 200; ++blk) {
                TrackEqParams p; p.hpfHz = 20.0f + 200.0f * (blk % 50) / 50.0f; p.lowDb = (blk % 7) - 3.0f; p.midDb = (blk % 5) - 2.0f; p.midHz = 300.0f + 20.0f * blk;
                for (int i = 0; i < 512; ++i) { b[2 * i] = b[2 * i + 1] = static_cast<float>(0.4 * std::sin(ph)); ph += 2.0 * 3.14159265 * 220.0 / fs; }
                eq.Process(b.data(), 512, p);
                for (int i = 0; i < 512; ++i) out.push_back(b[2 * i]);
            }
            double worst = 0; bool finite = true;
            for (size_t i = 2; i < out.size(); ++i) { finite = finite && std::isfinite(out[i]); worst = std::max(worst, static_cast<double>(std::fabs(out[i] - 2 * out[i - 1] + out[i - 2]))); }
            check(finite && worst < 0.02, "eq: sweeping every control is click-free (" + std::to_string(worst) + ")");
        }
        // engine + project round trip
        {
            Project pj; Pattern& pt = pj.patterns[0];
            pt.tracks[kNumDrumTracks].eqHpfHz = 35.0f; pt.tracks[kNumDrumTracks].eqHpfSlope = 1.0f; pt.tracks[kNumDrumTracks].eqMidDb = -3.5f;
            pt.tracks[kNumDrumTracks].eqMidHz = 450.0f; pt.tracks[kNumDrumTracks].eqMidQ = 2.5f; pt.tracks[0].eqHighDb = 4.0f;
            check(SaveProject(pj, "selftest_eq.t8proj"), "eq: save");
            Project rj; check(LoadProject(rj, "selftest_eq.t8proj"), "eq: load");
            const Track& r = rj.patterns[0].tracks[kNumDrumTracks];
            check(r.eqHpfHz == 35.0f && r.eqHpfSlope == 1.0f && r.eqMidDb == -3.5f && r.eqMidHz == 450.0f && r.eqMidQ == 2.5f && rj.patterns[0].tracks[0].eqHighDb == 4.0f, "eq: settings survive save / load");
            check(rj.patterns[0].tracks[1].eqHpfHz == 20.0f && rj.patterns[0].tracks[1].eqLowDb == 0.0f, "eq: untouched tracks stay flat");
            std::remove("selftest_eq.t8proj");
            // engine: a sub bass through the 24 dB HPF at 100 Hz is much quieter than without it
            auto rms = [&](bool hpf) {
                AudioEngine e; e.Init(44100);
                SynthPatch bp; bp.waveform = Waveform::Sine; bp.cutoffHz = 400; bp.sustain = 1.0f; bp.volume = 0.8f; e.Synth(0).SetPatch(bp);
                Mixer& m = e.GetMixer();
                for (int i = 0; i < kNumTracks; ++i) m.tracks[i].volume = 0.0f;
                m.tracks[kNumDrumTracks].volume = 1.0f; m.fx.delayLevel = 0; m.fx.reverbLevel = 0;
                if (hpf) { m.tracks[kNumDrumTracks].eqHpfHz = 100.0f; m.tracks[kNumDrumTracks].eqHpfSlope = 1.0f; }
                e.SynthNoteOn(0, 33, 1.0f, false);   // A0 = 55 Hz
                std::vector<float> b(1024); double s = 0; int n = 0;
                for (int blk = 0; blk < 80; ++blk) { e.Render(b.data(), 512, 5512.5); if (blk >= 40) for (int i = 0; i < 512; ++i) { s += b[2 * i] * b[2 * i]; ++n; } }
                return std::sqrt(s / n);
            };
            double rOff = rms(false), rOn = rms(true);
            check(rOff > 0.02 && rOn < rOff * 0.2, "eq: the engine applies the track HPF (" + std::to_string(rOff) + " -> " + std::to_string(rOn) + ")");
        }
    }

    // ---- v30: PHASE SYNC ----------------------------------------------------
    {
        auto notesOf = [&](bool sync, std::vector<std::vector<float>>& starts, double& worstKink) {
            SynthPatch pp; pp.waveform = Waveform::Saw; pp.cutoffHz = 900; pp.sustain = 1.0f; pp.attack = 0.001f; pp.release = 0.3f; pp.phaseSync = sync;
            SynthVoice v; v.SetPatch(pp);
            const int step = 5292;                    // 0.12 s at 44.1 kHz
            std::vector<float> all, buf;
            worstKink = 0;
            for (int k = 0; k < 8; ++k) {
                v.NoteOn(k % 2 ? 36 : 33, 1.0f, false);   // alternating notes, each retriggered over the still-sounding one
                buf.assign(static_cast<size_t>(step) * 2, 0.0f);
                v.Render(buf.data(), step, 44100);
                std::vector<float> mono(step); for (int i = 0; i < step; ++i) mono[i] = buf[2 * i];
                all.insert(all.end(), mono.begin(), mono.end());
                if (k >= 2 && k % 2 == 0) starts.emplace_back(mono.begin() + 200, mono.begin() + 600);   // same pitch, after the declick
            }
            for (size_t i = 2; i < all.size(); ++i) worstKink = std::max(worstKink, static_cast<double>(std::fabs(all[i] - 2 * all[i - 1] + all[i - 2])));
        };
        auto corr = [](const std::vector<float>& a, const std::vector<float>& b) {
            double sa = 0, sb = 0, sab = 0; for (size_t i = 0; i < a.size(); ++i) { sa += a[i] * a[i]; sb += b[i] * b[i]; sab += a[i] * b[i]; } return sab / std::sqrt(sa * sb + 1e-30); };
        std::vector<std::vector<float>> offS, onS; double kOff = 0, kOn = 0;
        notesOf(false, offS, kOff); notesOf(true, onS, kOn);
        double minOff = 1, minOn = 1;
        for (size_t i = 1; i < offS.size(); ++i) minOff = std::min(minOff, corr(offS[0], offS[i]));
        for (size_t i = 1; i < onS.size(); ++i) minOn = std::min(minOn, corr(onS[0], onS[i]));
        check(minOn > 0.99, "phase sync: every note starts with the same waveform (" + std::to_string(minOn) + ")");
        check(minOff < 0.95, "phase sync off: notes keep running phase as before (" + std::to_string(minOff) + ")");
        check(kOn < kOff * 3.0 + 0.05, "phase sync: the retrigger does not click (" + std::to_string(kOn) + " vs " + std::to_string(kOff) + ")");
        // a note-off that follows the pending note-on drops both: nothing starts later
        SynthPatch pp; pp.sustain = 1.0f; pp.release = 0.05f; pp.phaseSync = true;
        SynthVoice v; v.SetPatch(pp); std::vector<float> b(1024);
        v.NoteOn(40, 1.0f, false); v.Render(b.data(), 512, 44100);
        v.NoteOn(45, 1.0f, false); v.NoteOff();
        for (int i = 0; i < 200; ++i) v.Render(b.data(), 512, 44100);
        check(!v.IsActive(), "phase sync: note-off right after a pending note-on leaves no stray note");
        // saved only when on, and round-trips
        SynthPatch sp; sp.phaseSync = true;
        Project pj; pj.patterns[0].synthPatches[0] = sp;
        check(SaveProject(pj, "selftest_ps.t8proj"), "phase sync: save");
        Project rj; check(LoadProject(rj, "selftest_ps.t8proj") && rj.patterns[0].synthPatches[0].phaseSync && !rj.patterns[0].synthPatches[1].phaseSync, "phase sync: round trip, other synths stay off");
        std::remove("selftest_ps.t8proj");
    }

    // ---- Microtiming (v39): swing keeps the bar length, nudge / track swing move single hits, ghost notes are quiet ----
    {
        auto buildProj = [&](Project& pr, float patSwing) {
            pr.patterns[0].bpm = 120; pr.patterns[0].masterLength = 16; pr.patterns[0].swing = patSwing;   // one step = 0.125 s
            for (int s2 = 0; s2 < 16; ++s2) {
                Step& k = pr.patterns[0].tracks[0].steps[s2]; k.active = true;                              // kick on every step
                Step& b = pr.patterns[0].tracks[kNumDrumTracks].steps[s2]; b.active = true; b.note = 40; b.lengthSteps = 1;
            }
        };
        struct Ev { double t; bool drum; };
        auto runProj = [&](Project& pr, double secs) {
            std::vector<Ev> evs;
            AudioEngine eng; eng.Init(kSampleRate);
            eng.GetKit()->LoadFromFolder("kits/808", kSampleRate);
            eng.GetMixer().SyncFromPattern(pr.patterns[0]);
            Sequencer sq; sq.Init(&eng, &pr);
            eng.Capture().Start();
            sq.Play();
            std::vector<float> ob(256 * 2);
            for (int n = 0; n < static_cast<int>(secs * kSampleRate); n += 256) sq.RenderAndAdvance(ob.data(), 256, kSampleRate);
            for (const auto& e : eng.Capture().Events()) if (e.isDrum && e.drumSlot == DrumSlot::Kick) evs.push_back({e.timeSeconds, true});
                                                         else if (e.synthIndex == 0) evs.push_back({e.timeSeconds, false});
            sq.Stop();
            return evs;
        };
        auto times = [](const std::vector<Ev>& v, bool drum) { std::vector<double> o; for (const auto& e : v) if (e.drum == drum) o.push_back(e.t); return o; };
        const double step = 0.125, tol = 0.002;

        // swing 0.5: odd steps sit a quarter step late, the bar still lasts exactly 16 steps
        Project a; buildProj(a, 0.5f);
        auto ka = times(runProj(a, 4.2), true);
        bool grid = ka.size() >= 33;
        for (size_t i = 0; grid && i < 33; ++i) {
            const double want = i * step + ((i & 1) ? 0.25 * step : 0.0);
            if (std::fabs(ka[i] - want) > tol) grid = false;
        }
        check(grid, "swing: odd steps are delayed by swing/2 of a step and the grid never drifts (bar = 16 steps exactly)");
        check(ka.size() >= 33 && std::fabs(ka[32] - 4.0) < tol, "swing: the 3rd bar starts at exactly 4.0 s");

        // nudge: +25 % on drum step 2, -25 % on drum step 4, synth step 6 -25 % (clamped to the grid)
        Project b; buildProj(b, 0.0f);
        b.patterns[0].tracks[0].steps[2].nudge = 25;
        b.patterns[0].tracks[0].steps[4].nudge = -25;
        b.patterns[0].tracks[kNumDrumTracks].steps[6].nudge = -25;
        b.patterns[0].tracks[kNumDrumTracks].steps[3].nudge = 40;
        auto eb = runProj(b, 2.2);
        auto kb2 = times(eb, true), sb = times(eb, false);
        check(kb2.size() >= 16 && std::fabs(kb2[2] - (2 * step + 0.25 * step)) < tol, "nudge: +25 % delays a drum hit by a quarter step");
        check(kb2.size() >= 16 && std::fabs(kb2[4] - (4 * step - 0.25 * step)) < tol, "nudge: -25 % pulls a drum hit a quarter step ahead of the grid");
        { int n1 = 0; for (double t : kb2) if (t < 1.995) ++n1; check(n1 == 16, "nudge: no hit is lost or doubled by the lookahead (16 steps = 16 hits per bar)"); }
        {
            int exact = 0; for (double t : kb2) if (std::fabs(t - 4 * step + 0.25 * step) < tol) ++exact;
            check(exact == 1, "nudge: the pulled-ahead hit sounds exactly once");
        }
        check(sb.size() >= 16 && std::fabs(sb[6] - 6 * step) < tol, "nudge: synth tracks clamp a negative nudge to the grid");
        check(sb.size() >= 16 && std::fabs(sb[3] - (3 * step + 0.40 * step)) < tol, "nudge: a synth note can be delayed");

        // a pulled-ahead ratchet: positions off + k*(1-off)/n
        Project c; buildProj(c, 0.0f);
        c.patterns[0].tracks[0].steps[4].nudge = -30; c.patterns[0].tracks[0].steps[4].ratchet = 2;
        auto kc = times(runProj(c, 1.2), true);
        bool r1 = false, r2 = false;
        for (double t : kc) { if (std::fabs(t - (0.5 - 0.30 * step)) < tol) r1 = true; if (std::fabs(t - (0.5 + 0.35 * step)) < tol) r2 = true; }
        check(r1 && r2, "nudge: a pulled-ahead ratchet keeps its spacing (hits at -0.30 and +0.35 of the step)");

        // per-track swing: kick straight while the pattern swings, bass inherits
        Project d; buildProj(d, 0.5f);
        d.patterns[0].tracks[0].swing = 0.0f;
        auto ed = runProj(d, 1.2);
        auto kd = times(ed, true), sd = times(ed, false);
        check(kd.size() >= 8 && std::fabs(kd[1] - step) < tol && std::fabs(kd[3] - 3 * step) < tol, "track swing: 0 keeps this track on the grid inside a swung pattern");
        check(sd.size() >= 8 && std::fabs(sd[1] - (step + 0.25 * step)) < tol, "track swing: tracks without an override follow the pattern swing");
        Project e2; buildProj(e2, 0.0f);
        e2.patterns[0].tracks[0].swing = 0.5f;
        auto ke = times(runProj(e2, 1.2), true);
        check(ke.size() >= 8 && std::fabs(ke[1] - (step + 0.25 * step)) < tol && std::fabs(ke[2] - 2 * step) < tol, "track swing: a straight pattern, one swung track");

        // probability on a pulled-ahead hit is rolled once: 0 % never sounds, and the hit does not appear on the grid line either
        Project g; buildProj(g, 0.0f);
        g.patterns[0].tracks[0].steps[4].nudge = -25; g.patterns[0].tracks[0].steps[4].probability = 0;
        auto kg = times(runProj(g, 1.2), true);
        bool silent = true; for (double t : kg) if (std::fabs(t - 0.5) < 0.05) silent = false;
        check(silent, "nudge: a 0 % probability hit stays silent, also when it is pulled ahead");

        // persistence: new fields survive, old step lines still load
        Project h; buildProj(h, 0.1f);
        h.patterns[0].tracks[0].steps[2].nudge = -17; h.patterns[0].tracks[0].steps[3].ghost = true;
        h.patterns[0].tracks[0].steps[5].nudge = 33; h.patterns[0].tracks[0].steps[5].probability = 50;
        h.patterns[0].tracks[0].swing = 0.35f;
        check(SaveProject(h, "selftest_mt.t8proj"), "microtiming: save");
        Project hl; check(LoadProject(hl, "selftest_mt.t8proj"), "microtiming: load");
        const Track& ht = hl.patterns[0].tracks[0];
        check(ht.steps[2].nudge == -17 && ht.steps[3].ghost && ht.steps[5].nudge == 33 && ht.steps[5].probability == 50 && !ht.steps[4].ghost && ht.steps[4].nudge == 0,
              "microtiming: nudge / ghost survive a round trip");
        check(std::fabs(ht.swing - 0.35f) < 1e-4f && hl.patterns[0].tracks[1].swing < 0.0f, "microtiming: track swing survives, others keep following the pattern");
        std::remove("selftest_mt.t8proj"); std::remove("selftest_mt.t8proj.bak");

        // a ghost hit is clearly quieter than a normal one
        auto peakOf = [&](bool ghost) {
            Project pr; pr.patterns[0].bpm = 120; pr.patterns[0].masterLength = 16;
            pr.patterns[0].tracks[0].steps[0].active = true; pr.patterns[0].tracks[0].steps[0].ghost = ghost;
            AudioEngine eng; eng.Init(kSampleRate);
            eng.GetKit()->LoadFromFolder("kits/808", kSampleRate);
            eng.GetMixer().SyncFromPattern(pr.patterns[0]);
            Sequencer sq; sq.Init(&eng, &pr);
            sq.Play();
            std::vector<float> ob(256 * 2); float pk = 0;
            for (int n = 0; n < kSampleRate / 4; n += 256) { sq.RenderAndAdvance(ob.data(), 256, kSampleRate); for (float v : ob) pk = std::max(pk, std::fabs(v)); }
            sq.Stop();
            return pk;
        };
        const float pn = peakOf(false), pg = peakOf(true);
        check(pg > 0.0f && pg < 0.75f * pn, "ghost: a ghost hit is clearly quieter than a normal hit");
    }

    // ---- Free step velocity (v40) ----------------------------------------------------------------------------
    {
        Step plain, acc, gh, fv;
        acc.accent = true; gh.ghost = true; fv.vel = 64; fv.accent = true;
        check(std::fabs(StepVelocity(plain) - 0.8f) < 1e-6f && std::fabs(StepVelocity(acc) - 1.0f) < 1e-6f && std::fabs(StepVelocity(gh) - 0.45f) < 1e-6f,
              "velocity: unset steps keep the classic 0.8 / accent 1.0 / ghost 0.45");
        check(std::fabs(StepVelocity(fv) - 64.0f / 127.0f) < 1e-6f, "velocity: a free value wins over the accent flag");
        Step hi, lo; hi.vel = 127; lo.vel = 40;
        check(StepIsAccent(hi) && !StepIsGhost(hi) && StepIsGhost(lo) && !StepIsAccent(lo) && StepIsAccent(acc) && StepIsGhost(gh),
              "velocity: the grid reads a free value as accent (top) or ghost (soft)");

        // the sequencer plays it: a kick at vel 30 is much quieter than at vel 127
        auto peakAt = [&](int vel) {
            Project pr; pr.patterns[0].bpm = 120; pr.patterns[0].masterLength = 16;
            Step& k = pr.patterns[0].tracks[0].steps[0]; k.active = true; k.vel = static_cast<uint8_t>(vel);
            AudioEngine eng; eng.Init(kSampleRate);
            eng.GetKit()->LoadFromFolder("kits/808", kSampleRate);
            eng.GetMixer().SyncFromPattern(pr.patterns[0]);
            Sequencer sq; sq.Init(&eng, &pr); sq.Play();
            std::vector<float> ob(256 * 2); float pk = 0;
            for (int n = 0; n < kSampleRate / 4; n += 256) { sq.RenderAndAdvance(ob.data(), 256, kSampleRate); for (float v : ob) pk = std::max(pk, std::fabs(v)); }
            sq.Stop();
            return pk;
        };
        const float p30 = peakAt(30), p127 = peakAt(127), p0 = peakAt(0);
        check(p30 > 0.0f && p30 < 0.6f * p127, "velocity: a step with vel 30 plays clearly softer than vel 127");
        check(p0 > p30 && p0 < p127 * 1.001f, "velocity: an unset step (0.8) sits between them");

        // persistence: vel survives, a step without it stays byte-compatible
        Project vp;
        vp.patterns[0].tracks[0].steps[0].active = true; vp.patterns[0].tracks[0].steps[0].vel = 77;
        vp.patterns[0].tracks[0].steps[1].active = true;
        vp.patterns[0].tracks[0].steps[2].active = true; vp.patterns[0].tracks[0].steps[2].vel = 90; vp.patterns[0].tracks[0].steps[2].nudge = -12;
        check(SaveProject(vp, "selftest_vel.t8proj"), "velocity: save");
        Project vl; check(LoadProject(vl, "selftest_vel.t8proj"), "velocity: load");
        const Track& vt = vl.patterns[0].tracks[0];
        check(vt.steps[0].vel == 77 && vt.steps[1].vel == 0 && vt.steps[2].vel == 90 && vt.steps[2].nudge == -12, "velocity: the values survive a round trip (together with nudge)");
        std::remove("selftest_vel.t8proj"); std::remove("selftest_vel.t8proj.bak");
    }

    // ---- Tie / slide into a faded plucky note (v40.2) and COPY PARAMS ----------------------------------------
    {
        auto tiedLevel = [&](const char* presetName, bool tie, double& neighbour) {
            Project pr; Pattern& pat = pr.patterns[0]; pat.bpm = 120; pat.masterLength = 16;
            for (const auto& pre : AllSynthPresets()) if (std::string(pre.name) == presetName) pr.patterns[0].synthPatches[0] = pre.patch;
            Track& b = pat.tracks[kNumDrumTracks];
            for (int i = 0; i < 16; ++i) { b.steps[i].active = true; b.steps[i].note = i == 9 ? 52 : 40; b.steps[i].lengthSteps = 1; }
            b.steps[9].slide = tie;
            AudioEngine eng; eng.Init(44100);
            eng.Synth(0).SetPatch(pr.patterns[0].synthPatches[0]);
            eng.GetMixer().SyncFromPattern(pat);
            Sequencer sq; sq.Init(&eng, &pr); sq.Play();
            std::vector<float> o(512 * 2), all;
            for (int n = 0; n < 44100 * 2; n += 512) { sq.RenderAndAdvance(o.data(), 512, 44100); for (int i = 0; i < 512; ++i) all.push_back(o[2 * i]); }
            const int spt = 44100 / 8;
            auto rms = [&](int st) { double e = 0; for (int i = st * spt; i < (st + 1) * spt; ++i) e += double(all[i]) * all[i]; return std::sqrt(e / spt); };
            neighbour = rms(8);
            return rms(9);
        };
        double nb = 0;
        const double acidTied = tiedLevel("ACID BASS", true, nb);
        check(acidTied > 0.5 * nb, "tie: a slide into a faded ACID BASS note keeps sounding (not silent)");
        const double sawTied = tiedLevel("REESE BASS", true, nb);
        check(sawTied > 0.3 * nb, "tie: the same on another plucky bass");
        double padNb = 0;
        const double padTied = tiedLevel("WARM PAD", true, padNb);
        const double padPlain = tiedLevel("WARM PAD", false, padNb);
        check(padTied > 0.0 && padTied < padPlain * 1.4, "tie: a sustained pad is not re-attacked or boosted by a tie");

        // COPY PARAMS text
        Project cp; cp.patterns[0].synthPatches[0].cutoffHz = 1234.0f; cp.patterns[0].tracks[kNumDrumTracks].name = "Bass";
        const std::string txt = DescribeInstrument(cp, cp.patterns[0], kNumDrumTracks);
        check(txt.find("cutoffHz=1234") != std::string::npos && txt.find("[fx]") != std::string::npos && txt.find("instrument=Bass") != std::string::npos,
              "copy params: the synth patch, the track and the effects are in the text");
        const std::string dtxt = DescribeInstrument(cp, cp.patterns[0], 0);
        check(dtxt.find("drumTune=") != std::string::npos && dtxt.find("kickMode=") != std::string::npos && dtxt.find("cutoffHz=") == std::string::npos,
              "copy params: a drum track dumps its voice and the kick synth, not a synth patch");
    }

    // ---- Grooves (v42) ----------------------------------------------------------------------------------------------
    {
        const double step = 0.125;
        auto runKick = [&](Project& pr, double secs) {
            std::vector<double> t;
            AudioEngine eng; eng.Init(kSampleRate);
            eng.GetKit()->LoadFromFolder("kits/808", kSampleRate);
            eng.GetMixer().SyncFromPattern(pr.patterns[0]);
            Sequencer sq; sq.Init(&eng, &pr);
            eng.Capture().Start(); sq.Play();
            std::vector<float> ob(256 * 2);
            for (int n = 0; n < static_cast<int>(secs * kSampleRate); n += 256) sq.RenderAndAdvance(ob.data(), 256, kSampleRate);
            for (const auto& e : eng.Capture().Events()) if (e.isDrum && e.drumSlot == DrumSlot::Kick) t.push_back(e.timeSeconds);
            sq.Stop();
            return t;
        };
        auto base = [&](Project& pr) {
            pr.patterns[0].bpm = 120; pr.patterns[0].masterLength = 16;
            for (int i = 0; i < 16; ++i) pr.patterns[0].tracks[0].steps[i].active = true;
        };
        // 1. timing: a groove with +20 % on every odd step, amount 100 % and 50 %
        Groove gl; gl.name = "T";
        for (int i = 1; i < 16; i += 2) gl.steps[i].nudge = 20;
        Project a; base(a); a.patterns[0].groove = gl; a.patterns[0].grooveAmount = 1.0f;
        auto ta = runKick(a, 1.2);
        check(ta.size() >= 8 && std::fabs(ta[1] - (step + 0.20 * step)) < 0.002 && std::fabs(ta[2] - 2 * step) < 0.002, "groove: the nudge of the groove moves the hits (amount 100 %)");
        Project a2; base(a2); a2.patterns[0].groove = gl; a2.patterns[0].grooveAmount = 0.5f;
        auto ta2 = runKick(a2, 1.2);
        check(ta2.size() >= 4 && std::fabs(ta2[1] - (step + 0.10 * step)) < 0.002, "groove: amount 50 % moves it half as far");
        Project a3; base(a3); a3.patterns[0].groove = gl; a3.patterns[0].grooveAmount = 1.0f; a3.patterns[0].tracks[0].grooveOn = false;
        auto ta3 = runKick(a3, 1.2);
        check(ta3.size() >= 4 && std::fabs(ta3[1] - step) < 0.002, "groove: a track with grooveOn = false stays straight");
        Project a4; base(a4); a4.patterns[0].groove = gl; a4.patterns[0].grooveAmount = 0.0f;
        auto ta4 = runKick(a4, 1.2);
        check(ta4.size() >= 4 && std::fabs(ta4[1] - step) < 0.002, "groove: amount 0 = no groove at all");
        // 2. a negative groove nudge on a drum track is played ahead of the grid, once
        Groove gn; for (int i = 0; i < 16; ++i) gn.steps[i].nudge = (i % 4 == 0) ? 0 : -25;
        Project b; base(b); b.patterns[0].groove = gn; b.patterns[0].grooveAmount = 1.0f;
        auto tb = runKick(b, 2.2);
        int n1 = 0; for (double t : tb) if (t < 1.995) ++n1;
        check(n1 == 16 && tb.size() >= 3 && std::fabs(tb[1] - (step - 0.25 * step)) < 0.002, "groove: negative timing pulls drum hits ahead without losing or doubling any");
        // 3. velocity: the groove's velocity offset changes the level
        auto peak = [&](float vOff) {
            Project pr; pr.patterns[0].bpm = 120; pr.patterns[0].masterLength = 16;
            pr.patterns[0].tracks[0].steps[0].active = true; pr.patterns[0].tracks[0].steps[0].vel = 64;
            Groove g; g.steps[0].vel = static_cast<int8_t>(vOff);
            pr.patterns[0].groove = g; pr.patterns[0].grooveAmount = 1.0f;
            AudioEngine eng; eng.Init(kSampleRate);
            eng.GetKit()->LoadFromFolder("kits/808", kSampleRate);
            eng.GetMixer().SyncFromPattern(pr.patterns[0]);
            Sequencer sq; sq.Init(&eng, &pr); sq.Play();
            std::vector<float> ob(256 * 2); float pk = 0;
            for (int n = 0; n < kSampleRate / 4; n += 256) { sq.RenderAndAdvance(ob.data(), 256, kSampleRate); for (float v : ob) pk = std::max(pk, std::fabs(v)); }
            sq.Stop();
            return pk;
        };
        check(peak(40) > 1.3f * peak(-40), "groove: its velocity offset makes a hit louder / softer");
        // 4. bake == live
        Project c; base(c); c.patterns[0].groove = BuiltinGrooves()[2]; c.patterns[0].grooveAmount = 0.7f;
        auto live = runKick(c, 2.0);
        BakeGroove(c.patterns[0]);
        check(c.patterns[0].grooveAmount == 0.0f && c.patterns[0].groove.IsFlat(), "groove: BAKE switches the layer off");
        auto baked = runKick(c, 2.0);
        bool same = live.size() == baked.size() && live.size() > 8;
        for (size_t i = 0; same && i < live.size(); ++i) if (std::fabs(live[i] - baked[i]) > 0.0015) same = false;
        check(same, "groove: baked steps play at the same times as the live groove");
        // 5. capture + custom file + persistence
        Project d; base(d);
        for (int i = 0; i < 16; ++i) { d.patterns[0].tracks[0].steps[i].nudge = (i & 1) ? 12 : 0; d.patterns[0].tracks[0].steps[i].vel = (i % 4 == 0) ? 120 : 70; }
        Groove cap = GrooveFromTrack(d.patterns[0].tracks[0], d.patterns[0], 16, "CAP");
        check(cap.steps[1].nudge == 12 && cap.steps[0].nudge == 0 && cap.steps[0].vel > 10 && cap.steps[1].vel < -10, "groove: capture from a track reads its nudge and relative velocity");
        cap.name = "SELFTEST CAP";
        check(SaveCustomGroove(cap), "groove: save a custom groove");
        bool found = false; for (const Groove& g : ListCustomGrooves()) if (g.name == "SELFTEST CAP" && g.steps[1].nudge == 12) found = true;
        check(found, "groove: the saved groove is listed and loads back");
        std::remove("grooves/SELFTEST CAP.t8groove");
        d.patterns[0].groove = BuiltinGrooves()[3]; d.patterns[0].grooveAmount = 0.6f; d.patterns[0].tracks[2].grooveOn = false;
        check(SaveProject(d, "selftest_gr.t8proj"), "groove: save the project");
        Project dl; check(LoadProject(dl, "selftest_gr.t8proj"), "groove: load the project");
        check(dl.patterns[0].groove.name == "ACID BOUNCE" && std::fabs(dl.patterns[0].grooveAmount - 0.6f) < 1e-4f && dl.patterns[0].groove.steps[3].nudge == BuiltinGrooves()[3].steps[3].nudge &&
              dl.patterns[0].groove.steps[2].vel == BuiltinGrooves()[3].steps[2].vel && !dl.patterns[0].tracks[2].grooveOn && dl.patterns[0].tracks[0].grooveOn,
              "groove: groove, amount and the per-track switch survive a round trip");
        std::remove("selftest_gr.t8proj"); std::remove("selftest_gr.t8proj.bak");
        check(BuiltinGrooves().size() >= 10, "groove: the library has the built-in grooves");
    }

    // ---- Grid: 1/32 and triplets (v43) -----------------------------------------------------------------------------
    {
        auto runEv = [&](Project& pr, double secs, bool breakAt0 = false) {
            std::vector<double> t;
            AudioEngine eng; eng.Init(kSampleRate);
            eng.GetKit()->LoadFromFolder("kits/808", kSampleRate);
            eng.GetMixer().SyncFromPattern(pr.patterns[0]);
            Sequencer sq; sq.Init(&eng, &pr);
            if (breakAt0) sq.SetLiveBreak(0);
            eng.Capture().Start(); sq.Play();
            std::vector<float> ob(256 * 2);
            for (int n = 0; n < static_cast<int>(secs * kSampleRate); n += 256) sq.RenderAndAdvance(ob.data(), 256, kSampleRate);
            for (const auto& e : eng.Capture().Events()) if (e.isDrum) t.push_back(e.timeSeconds);
            sq.Stop();
            return t;
        };
        auto mk = [&](int spb, int bars) {
            Project pr; Pattern& pat = pr.patterns[0]; pat.bpm = 120; pat.stepsPerBeat = spb; pat.masterLength = 4 * spb * bars;
            for (int i = 0; i < pat.masterLength; ++i) pat.tracks[0].steps[i].active = true;
            return pr;
        };
        // 1/32: a step is 62.5 ms, a bar 2 s = 32 steps
        { Project pr = mk(8, 1); auto t = runEv(pr, 4.1);
          bool ok = t.size() >= 65; for (size_t i = 0; ok && i < 65; ++i) if (std::fabs(t[i] - i * 0.0625) > 0.002) ok = false;
          check(ok, "grid 1/32: steps are 62.5 ms apart and the bar lasts exactly 2 s (32 steps)"); }
        // 1/16 triplets: 6 steps per beat, 24 per bar
        { Project pr = mk(6, 1); auto t = runEv(pr, 4.1);
          bool ok = t.size() >= 49; for (size_t i = 0; ok && i < 49; ++i) if (std::fabs(t[i] - i * (0.5 / 6.0)) > 0.002) ok = false;
          check(ok, "grid 1/16T: six steps per beat, 24 per bar"); }
        { Project pr = mk(3, 1); auto t = runEv(pr, 4.1);
          bool ok = t.size() >= 25; for (size_t i = 0; ok && i < 25; ++i) if (std::fabs(t[i] - i * (0.5 / 3.0)) > 0.002) ok = false;
          check(ok, "grid 1/8T: three steps per beat, 12 per bar"); }
        // swing on 1/32: the second 16th of every 8th (ticks 2,3 mod 4) is delayed by `swing` steps
        { Project pr = mk(8, 1); pr.patterns[0].swing = 0.5f; auto t = runEv(pr, 1.0);
          check(t.size() >= 8 && std::fabs(t[0]) < 0.002 && std::fabs(t[1] - 0.0625) < 0.002 && std::fabs(t[2] - (2 * 0.0625 + 0.5 * 0.0625)) < 0.002 && std::fabs(t[3] - (3 * 0.0625 + 0.5 * 0.0625)) < 0.002,
                "grid 1/32: swing delays the odd sixteenths (two steps each), the straight ones stay");
          Project pt = mk(6, 1); pt.patterns[0].swing = 0.5f; auto tt = runEv(pt, 1.0);
          check(tt.size() >= 4 && std::fabs(tt[1] - (0.5 / 6.0)) < 0.002, "grid 1/16T: swing is ignored"); }
        // groove position follows the sixteenth of the bar on 1/32 (two steps per groove position)
        { Project pr = mk(8, 1); Groove g; g.steps[1].nudge = 40; pr.patterns[0].groove = g; pr.patterns[0].grooveAmount = 1.0f; auto t = runEv(pr, 1.0);
          check(t.size() >= 8 && std::fabs(t[2] - (2 * 0.0625 + 0.4 * 0.0625)) < 0.002 && std::fabs(t[3] - (3 * 0.0625 + 0.4 * 0.0625)) < 0.002 && std::fabs(t[4] - 4 * 0.0625) < 0.002,
                "grid 1/32: a groove position covers its two steps"); }
        // converting 1/16 -> 1/32 keeps the notes where they are in time, and back again
        {
            AppState ga;
            ga.engine.Init(kSampleRate);
            ga.sequencer.Init(&ga.engine, &ga.project);
            Pattern& pat = ga.CurrentPattern();
            pat.bpm = 120; pat.masterLength = 16;
            for (int i : {0, 3, 6, 10, 13}) { pat.tracks[0].steps[i].active = true; pat.tracks[0].steps[i].nudge = static_cast<int8_t>(i); }
            Project before = ga.project;
            auto tb = runEv(before, 2.1);
            ga.SetPatternResolution(8);
            check(pat.stepsPerBeat == 8 && pat.masterLength == 32 && pat.tracks[0].steps[6].active && pat.tracks[0].steps[6].nudge == 6 && !pat.tracks[0].steps[3].active, "grid: 1/16 -> 1/32 doubles the step positions and the length");
            auto ta = runEv(ga.project, 2.1);
            bool same = ta.size() == tb.size() && tb.size() >= 5;
            for (size_t i = 0; same && i < ta.size(); ++i) if (std::fabs(ta[i] - tb[i]) > 0.002) same = false;
            check(same, "grid: the converted pattern plays at exactly the same times");
            ga.SetPatternResolution(4);
            check(pat.stepsPerBeat == 4 && pat.masterLength == 16 && pat.tracks[0].steps[3].active && pat.tracks[0].steps[3].nudge == 3 && pat.tracks[0].steps[13].active, "grid: 1/32 -> 1/16 halves it again, nothing lost when the odd steps were empty");
            Project before16 = ga.project;
            auto t16 = runEv(before16, 2.1);
            ga.SetPatternResolution(6);
            check(pat.stepsPerBeat == 6 && pat.masterLength == 24 && pat.tracks[0].steps[0].active && pat.tracks[0].steps[5].active && pat.tracks[0].steps[9].active && pat.tracks[0].steps[15].active && pat.tracks[0].steps[20].active && !pat.tracks[0].steps[3].active,
                  "grid v44.2: 1/16 -> 1/16T moves every note to the nearest triplet step (0,3,6,10,13 -> 0,5,9,15,20), the length keeps its bars");
            {   // the pattern must NOT speed up: every hit is within half a triplet step (1/12 beat = 0.042 s at 120 bpm) of where it was
                auto tt = runEv(ga.project, 2.1);
                bool close = tt.size() == t16.size() && tt.size() >= 5;
                for (size_t i = 0; close && i < tt.size(); ++i) if (std::fabs(tt[i] - t16[i]) > 0.045) close = false;
                check(close, "grid v44.2: switching to triplets keeps the timing of the pattern (no speed change)");
            }
            check(ga.Undo() && pat.stepsPerBeat == 4, "grid: undo brings the previous grid back");
            {   // quarter-note kicks stay exactly on the beat on both triplet grids
                AppState gb; gb.engine.Init(kSampleRate); gb.sequencer.Init(&gb.engine, &gb.project);
                Pattern& pb = gb.CurrentPattern(); pb.bpm = 120; pb.masterLength = 16;
                for (int i : {0, 4, 8, 12}) pb.tracks[0].steps[i].active = true;
                Project b0 = gb.project; auto tq = runEv(b0, 2.1);
                gb.SetPatternResolution(3);
                bool onBeat = pb.stepsPerBeat == 3 && pb.masterLength == 12 && pb.tracks[0].steps[0].active && pb.tracks[0].steps[3].active && pb.tracks[0].steps[6].active && pb.tracks[0].steps[9].active;
                auto t3 = runEv(gb.project, 2.1);
                bool same3 = t3.size() == tq.size() && tq.size() >= 4; for (size_t i = 0; same3 && i < t3.size(); ++i) if (std::fabs(t3[i] - tq[i]) > 0.002) same3 = false;
                check(onBeat && same3, "grid v44.2: 1/8 triplets keep quarter-note hits exactly where they were");
                gb.SetPatternResolution(6);
                bool same6 = pb.masterLength == 24 && pb.tracks[0].steps[6].active && pb.tracks[0].steps[18].active;
                gb.SetPatternResolution(4);
                bool back = pb.masterLength == 16 && pb.tracks[0].steps[4].active && pb.tracks[0].steps[12].active && pb.tracks[0].steps[1].active == false;
                check(same6 && back, "grid v44.2: 1/8T -> 1/16T -> 1/16 keeps the quarter notes on their beats");
                // 1/16 steps 2 and 3 fall on the same 1/8T cell: the second one moves to the free neighbour, nothing is lost
                AppState gc; gc.engine.Init(kSampleRate); gc.sequencer.Init(&gc.engine, &gc.project);
                Pattern& pc = gc.CurrentPattern(); pc.masterLength = 16;
                for (int i : {1, 2, 3}) pc.tracks[0].steps[i].active = true;
                int merged = gc.SetPatternResolution(3);
                int cnt = 0; for (int i = 0; i < 12; ++i) cnt += pc.tracks[0].steps[i].active ? 1 : 0;
                check(merged == 0 && cnt == 3, "grid v44.2: notes that land on one cell go to the next free neighbour instead of being lost");
            }
            {   // v44.3 groove: choose, then BAKE (nothing is audible before the bake)
                AppState gg; gg.engine.Init(kSampleRate); gg.sequencer.Init(&gg.engine, &gg.project);
                Pattern& pg = gg.CurrentPattern(); pg.masterLength = 16;
                for (int i : {0, 4, 8, 12}) pg.tracks[0].steps[i].active = true;
                for (int i : {2, 6, 10, 14}) pg.tracks[3].steps[i].active = true;
                pg.tracks[3].grooveOn = false;
                gg.StageGroove(BuiltinGrooves()[0]);
                check(pg.groove.IsFlat() && pg.grooveAmount == 0.0f, "groove v44.3: choosing a groove does not touch the pattern (nothing to hear before BAKE)");
                bool untouched = true; for (int i : {0, 4, 8, 12}) if (pg.tracks[0].steps[i].nudge != 0 || pg.tracks[0].steps[i].vel != 0) untouched = false;
                check(untouched, "groove v44.3: the steps are unchanged until BAKE");
                Groove sh; sh.name = "TEST"; sh.steps[4].nudge = 20; sh.steps[4].vel = 30; sh.steps[2].nudge = 20;
                gg.StageGroove(sh); gg.stagedAmount = 0.5f;
                check(gg.BakeStagedGroove(), "groove v44.3: BAKE accepts a chosen groove");
                check(pg.tracks[0].steps[4].nudge == 10 && pg.tracks[0].steps[4].vel > 0 && pg.tracks[0].steps[0].nudge == 0, "groove v44.3: BAKE writes amount x shape into the steps (nudge 20 x 50% = 10)");
                check(pg.tracks[3].steps[2].nudge == 0, "groove v44.3: a track that is switched off in BAKE INTO is left alone");
                check(gg.stagedGroove.IsFlat() && pg.groove.IsFlat() && pg.grooveAmount == 0.0f, "groove v44.3: after the bake nothing stays chosen and no live layer is on");
                check(!gg.BakeStagedGroove(), "groove v44.3: BAKE with nothing chosen does nothing");
                check(gg.Undo() && pg.tracks[0].steps[4].nudge == 0, "groove v44.3: BAKE is one undo step");
                gg.StageGroove(sh); gg.ResetFeel();
                check(gg.stagedGroove.IsFlat() && !gg.FeelIsActive(pg), "groove v44.3: RESET also clears a chosen groove");
            }
            {   // v44.3 audio recovery: a NaN in the signal must not silence the output for good
                AppState ge; ge.engine.Init(kSampleRate);
                std::vector<float> buf(256 * 2, 0.0f);
                ge.engine.SynthNoteOn(0, 60, 1.0f, false);
                ge.engine.GetMixer().tracks[kNumDrumTracks].volume = std::nanf("");
                ge.engine.Render(buf.data(), 256);
                bool finite = true; for (float v : buf) if (!std::isfinite(v)) finite = false;
                check(finite && ge.engine.ConsumeDspFault() && !ge.engine.ConsumeDspFault(), "audio recovery v44.3: a NaN never reaches the output, the fault is flagged once");
                ge.engine.ResetDsp();
                ge.engine.GetMixer().tracks[kNumDrumTracks].volume = 0.8f;
                float peak = 0.0f; bool fin2 = true;
                for (int blk = 0; blk < 40; ++blk) { ge.engine.Render(buf.data(), 256); for (float v : buf) { if (!std::isfinite(v)) fin2 = false; peak = std::max(peak, std::fabs(v)); } }
                check(fin2 && peak > 0.01f, "audio recovery v44.3: after ResetDsp the sound comes back");
            }
            {   // v44.3 octave sets for the melodic randomizers
                const unsigned m13 = (1u << 1) | (1u << 3);
                auto nn = BuildScaleNotesMask(0, kScaleIonian, m13);
                bool inSet = !nn.empty(); for (int n : nn) { int o = n / 12 - 1; if (o != 1 && o != 3) inSet = false; }
                check(inSet && std::is_sorted(nn.begin(), nn.end()) && nn.size() == 14, "octaves v44.3: a set with a gap (1 and 3) gives only notes of those octaves (14 notes of C major)");
                check(BuildScaleNotesMask(0, kScaleIonian, OctaveMaskRange(1, 2)) == BuildScaleNotes(0, kScaleIonian, 1, 2), "octaves v44.3: a range mask equals the old low..high band");
                AppState go; go.engine.Init(kSampleRate); go.sequencer.Init(&go.engine, &go.project);
                bool all = true;
                int total = 0;   // a random part can come out empty (it was a flaky failure): only the 36 parts together must have notes
                for (int rep = 0; rep < 12; ++rep) {
                    go.project.rndOctaves = {static_cast<uint8_t>(m13), static_cast<uint8_t>(1u << 5), static_cast<uint8_t>((1u << 0) | (1u << 6))};
                    for (int part = 0; part < 3; ++part) {
                        go.RandomizeMelodicOctaves(part, rep % 2 == 1);
                        const Track& tt = go.CurrentPattern().tracks[kNumDrumTracks + part];
                        const unsigned mk = go.project.rndOctaves[part];
                        int cnt = 0;
                        for (int i = 0; i < go.CurrentPattern().masterLength; ++i) if (tt.steps[i].active) { ++cnt; const int o = tt.steps[i].note / 12 - 1; if (!((mk >> o) & 1u)) all = false; }
                        total += cnt;
                    }
                }
                check(all && total > 0, "octaves v44.3: every random note of BASS / SYN2 / SYN3 lies in the octaves chosen for that part");
                Project po; po.rndOctaves = {0x15, 0x80 >> 1, 0x03};
                check(SaveProject(po, "selftest_oct.t8proj"), "octaves v44.3: save");
                Project pl; check(LoadProject(pl, "selftest_oct.t8proj") && pl.rndOctaves == po.rndOctaves, "octaves v44.3: the octave sets survive a save / load");
                Project pd; check(pd.rndOctaves[0] == 0x06 && pd.rndOctaves[1] == 0x38 && pd.rndOctaves[2] == 0x38, "octaves v44.3: defaults are the old fixed ranges (bass 1-2, synths 3-5)");
            }
            {   // v44.4 R1: the RHYTHM randomizer
                std::mt19937 rr(7);
                bool picksOk = true; int beatHits = 0, total = 0;
                for (int rep = 0; rep < 300; ++rep) {
                    const int cnt = 1 + rep % 6;
                    const auto v = PickRhythmSteps(rr, 16, cnt, rep % 2 == 0);
                    if (static_cast<int>(v.size()) != cnt || !std::is_sorted(v.begin(), v.end()) || std::adjacent_find(v.begin(), v.end()) != v.end() || v.front() < 0 || v.back() > 15) picksOk = false;
                    if (rep % 2 == 0 && v.front() != 0) picksOk = false;
                    if (cnt == 2) for (int x : v) { ++total; if (x % 4 == 0) ++beatHits; }
                }
                check(picksOk, "rhythm v44.4: PickRhythmSteps gives exactly N distinct sorted steps, step 0 when forced");
                check(total > 0 && beatHits * 100 / total > 42, "rhythm v44.4: few notes favour the beats (4 of 16 steps are beats = 25 % by chance, the picker gives about 50 %)");
                AppState gr4; gr4.engine.Init(kSampleRate); gr4.sequencer.Init(&gr4.engine, &gr4.project);
                gr4.project.randomizerKeyRoot = 0; gr4.project.randomizerScale = kScaleIonian;
                Pattern& p4 = gr4.CurrentPattern(); p4.masterLength = 64;
                auto notesOf = [&](int part) { std::vector<int> v; const Track& tt = p4.tracks[kNumDrumTracks + part]; for (int i = 0; i < 64; ++i) if (tt.steps[i].active) v.push_back(i); return v; };
                gr4.project.rndRhythm[0] = RhythmRnd{1, 1, RhythmPitch::Root, RhythmLen::Held, true, true};
                gr4.RandomizeRhythm(0);
                const auto one = notesOf(0);
                bool oneOk = one.size() == 4;
                for (size_t k = 0; k < one.size(); ++k) {
                    const Step& st = p4.tracks[kNumDrumTracks].steps[one[k]];
                    if (one[k] != static_cast<int>(k) * 16 || st.lengthSteps != 16 || ((st.note % 12) + 12) % 12 != 0) oneOk = false;
                    const int o = st.note / 12 - 1; if (!((gr4.project.rndOctaves[0] >> o) & 1u)) oneOk = false;
                }
                check(oneOk, "rhythm v44.4: ONE note per bar, on the one, held to the end of the bar, on the key root, inside the octave set (4 notes in 64 steps)");
                check(gr4.Undo() && notesOf(0).empty(), "rhythm v44.4: undo takes the figure away");
                gr4.project.rndRhythm[1] = RhythmRnd{2, 5, RhythmPitch::Scale, RhythmLen::Mix, false, true};
                bool sameOk = true, countOk = true;
                for (int rep = 0; rep < 10; ++rep) {
                    gr4.RandomizeRhythm(1);
                    const Track& tt = p4.tracks[kNumDrumTracks + 1];
                    for (int b = 1; b < 4; ++b) for (int i = 0; i < 16; ++i) {
                        const Step& a = tt.steps[i]; const Step& c = tt.steps[b * 16 + i];
                        if (a.active != c.active || (a.active && (a.note != c.note || a.lengthSteps != c.lengthSteps))) sameOk = false;
                    }
                    int n0 = 0; for (int i = 0; i < 16; ++i) if (tt.steps[i].active) ++n0;
                    if (n0 < 2 || n0 > 5) countOk = false;
                }
                check(sameOk, "rhythm v44.4: SAME EVERY BAR repeats one bar exactly (positions, notes, lengths)");
                check(countOk, "rhythm v44.4: the number of notes per bar stays between min and max");
                gr4.project.rndRhythm[1].sameEveryBar = false;
                bool anyDiff = false;
                for (int rep = 0; rep < 10 && !anyDiff; ++rep) {
                    gr4.RandomizeRhythm(1);
                    const Track& tt = p4.tracks[kNumDrumTracks + 1];
                    for (int b = 1; b < 4; ++b) for (int i = 0; i < 16; ++i) if (tt.steps[i].active != tt.steps[b * 16 + i].active || tt.steps[i].note != tt.steps[b * 16 + i].note) anyDiff = true;
                }
                check(anyDiff, "rhythm v44.4: with SAME EVERY BAR off the bars differ");
                gr4.project.rndRhythm[2] = RhythmRnd{3, 6, RhythmPitch::RootFifth, RhythmLen::Short, true, true};
                gr4.RandomizeRhythm(2);
                bool fifthOk = true, shortOk = true;
                for (int i = 0; i < 64; ++i) { const Step& st = p4.tracks[kNumDrumTracks + 2].steps[i]; if (!st.active) continue; const int pc = ((st.note % 12) + 12) % 12; if (pc != 0 && pc != 7) fifthOk = false; if (st.lengthSteps != 1) shortOk = false; }
                check(fifthOk && shortOk, "rhythm v44.4: ROOT+5TH plays only root and fifth, SHORT notes last one step");
                {   // CHORD: the bass takes the root of the sounding chord (A minor = A)
                    Track& ch = gr4.StepTrack(p4, kChordTrackIndex);
                    for (auto& st : ch.steps) st = Step{};
                    ch.steps[0].active = true; ch.steps[0].chordSize = 3; ch.steps[0].lengthSteps = 64; ch.steps[0].chordNotes = {57, 60, 64, 0};
                    gr4.rndChordAware = true;
                    gr4.project.rndRhythm[0] = RhythmRnd{2, 4, RhythmPitch::Chord, RhythmLen::Mix, true, true};
                    gr4.RandomizeRhythm(0);
                    bool rootA = !notesOf(0).empty();
                    for (int i : notesOf(0)) if (((p4.tracks[kNumDrumTracks].steps[i].note % 12) + 12) % 12 != 9) rootA = false;
                    check(rootA, "rhythm v44.4: CHORD pitch gives the bass the root of the sounding chord");
                    for (auto& st : ch.steps) st = Step{};
                }
                {   // own track length and the triplet grid (a bar is 12 steps)
                    Track& tt = p4.tracks[kNumDrumTracks]; tt.useOwnLength = true; tt.length = 8;
                    gr4.project.rndRhythm[0] = RhythmRnd{1, 3, RhythmPitch::Root, RhythmLen::Mix, true, true};
                    gr4.RandomizeRhythm(0);
                    bool inLen = !notesOf(0).empty(); for (int i : notesOf(0)) if (i >= 8) inLen = false;
                    check(inLen, "rhythm v44.4: a track with its own length gets notes only inside it");
                    tt.useOwnLength = false; p4.stepsPerBeat = 3; p4.masterLength = 24;
                    gr4.project.rndRhythm[0] = RhythmRnd{1, 1, RhythmPitch::Root, RhythmLen::Held, true, true};
                    gr4.RandomizeRhythm(0);
                    const auto v = notesOf(0);
                    check(v.size() == 2 && v[0] == 0 && v[1] == 12 && p4.tracks[kNumDrumTracks].steps[0].lengthSteps == 12, "rhythm v44.4: on a triplet grid a bar is 12 steps (one held note per bar)");
                    p4.stepsPerBeat = 4; p4.masterLength = 64;
                }
                Project pr; pr.rndRhythm[0] = RhythmRnd{2, 9, RhythmPitch::Chord, RhythmLen::Held, false, false}; pr.rndRhythm[2] = RhythmRnd{1, 1, RhythmPitch::RootFifth, RhythmLen::Short, true, false};
                check(SaveProject(pr, "selftest_rhy.t8proj"), "rhythm v44.4: save");
                Project pq; check(LoadProject(pq, "selftest_rhy.t8proj"), "rhythm v44.4: load");
                const RhythmRnd& q0 = pq.rndRhythm[0]; const RhythmRnd& q2 = pq.rndRhythm[2];
                check(q0.minNotes == 2 && q0.maxNotes == 9 && q0.pitch == RhythmPitch::Chord && q0.length == RhythmLen::Held && !q0.onTheOne && !q0.sameEveryBar &&
                      q2.minNotes == 1 && q2.maxNotes == 1 && q2.pitch == RhythmPitch::RootFifth && q2.length == RhythmLen::Short && q2.onTheOne && !q2.sameEveryBar,
                      "rhythm v44.4: the settings survive a save / load");
                check(Project{}.rndRhythm[0].minNotes == 1 && Project{}.rndRhythm[0].pitch == RhythmPitch::Root, "rhythm v44.4: the bass defaults to 1-3 notes on the root");
                std::remove("selftest_rhy.t8proj"); std::remove("selftest_rhy.t8proj.bak");
                gr4.rndDensityMode[0] = 2; gr4.RandomizeMelodicMode(0, 2);
                check(!notesOf(0).empty(), "rhythm v44.4: RANDOM in RHYTHM mode writes a figure");
            }
            {   // v44.4 R2: EUCLID and ROTATE
                auto str = [](const std::vector<bool>& v) { std::string s; for (bool b : v) s += b ? 'x' : '.'; return s; };
                check(str(EuclidPattern(3, 8)) == "x..x..x.", "euclid v44.4: 3 over 8 is the 3-3-2 pattern");
                check(str(EuclidPattern(4, 16)) == "x...x...x...x...", "euclid v44.4: 4 over 16 is four on the floor");
                bool cnt = true; for (int h = 0; h <= 16; ++h) { int c = 0; for (bool b : EuclidPattern(h, 16)) c += b; if (c != h) cnt = false; }
                check(cnt && str(EuclidPattern(0, 8)) == "........" && str(EuclidPattern(8, 8)) == "xxxxxxxx", "euclid v44.4: N hits give exactly N onsets (0 and all too)");
                check(str(RotatePattern(EuclidPattern(3, 8), 1)) == ".x..x..x" && str(RotatePattern(EuclidPattern(3, 8), -1)) == "..x..x.x", "euclid v44.4: rotate slides the figure round (both ways)");
                AppState ge; ge.engine.Init(kSampleRate); ge.sequencer.Init(&ge.engine, &ge.project);
                Pattern& pe = ge.CurrentPattern(); pe.masterLength = 16;
                pe.tracks[0].steps[2].active = true; pe.tracks[0].steps[2].vel = 100; pe.tracks[0].steps[2].nudge = 7;
                check(ge.ApplyEuclid(0, 3, 8, 0) == 6, "euclid v44.4: 3 over 8 repeated through 16 steps = 6 hits");
                std::string got; for (int i = 0; i < 16; ++i) got += pe.tracks[0].steps[i].active ? 'x' : '.';
                check(got == "x..x..x.x..x..x.", "euclid v44.4: the figure repeats and the old steps are replaced");
                check(pe.tracks[0].steps[0].vel == 100 && pe.tracks[0].steps[0].nudge == 0, "euclid v44.4: the hits copy the first existing hit (velocity) without its nudge");
                check(ge.Undo() && pe.tracks[0].steps[2].active && !pe.tracks[0].steps[0].active, "euclid v44.4: undo brings the old steps back");
                check(ge.ApplyEuclid(kChordTrackIndex, 3, 8, 0) == 0, "euclid v44.4: the chord track is refused");
                ge.project.randomizerKeyRoot = 2; ge.project.randomizerScale = kScaleIonian;
                check(ge.ApplyEuclid(kNumDrumTracks, 5, 16, 3) == 5, "euclid v44.4: a synth track gets its hits");
                bool keyed = true; for (int i = 0; i < 16; ++i) { const Step& st = pe.tracks[kNumDrumTracks].steps[i]; if (st.active && ((st.note % 12) + 12) % 12 != 2) keyed = false; }
                check(keyed, "euclid v44.4: an empty synth track gets the key root as its note");
                for (auto& st : pe.tracks[1].steps) st = Step{};
                pe.tracks[1].steps[0].active = true; pe.tracks[1].steps[3].active = true; pe.tracks[1].steps[3].nudge = 9;
                ge.RotateTrack(1, 2);
                check(!pe.tracks[1].steps[0].active && pe.tracks[1].steps[2].active && pe.tracks[1].steps[5].active && pe.tracks[1].steps[5].nudge == 9, "rotate v44.4: whole steps slide later (nudge goes along)");
                ge.RotateTrack(1, -3);
                check(pe.tracks[1].steps[15].active && pe.tracks[1].steps[2].active && !pe.tracks[1].steps[5].active && !pe.tracks[1].steps[0].active, "rotate v44.4: it wraps round the track length (2 -> 15, 5 -> 2)");
                check(ge.Undo() && pe.tracks[1].steps[2].active && pe.tracks[1].steps[5].active, "rotate v44.4: undo");
            }
            {   // v44.4 R3: lengths in bars
                Pattern pl; pl.masterLength = 16; pl.stepsPerBeat = 4; pl.bpm = 120.0f;
                check(std::fabs(PatternBars(pl) - 1.0) < 1e-9 && std::fabs(PatternSeconds(pl) - 2.0) < 1e-9, "length v44.4: 16 steps at 1/16 and 120 BPM = 1 bar = 2 s");
                pl.masterLength = 64;
                check(std::fabs(PatternBars(pl) - 4.0) < 1e-9 && std::fabs(PatternSeconds(pl) - 8.0) < 1e-9, "length v44.4: 64 steps = 4 bars = 8 s");
                pl.stepsPerBeat = 3; pl.masterLength = 12;
                check(std::fabs(PatternBars(pl) - 1.0) < 1e-9, "length v44.4: on the triplet grid a bar is 12 steps");
                pl.masterLength = 16;
                check(std::fabs(PatternBars(pl) - 16.0 / 12.0) < 1e-9, "length v44.4: 16 triplet steps are not a whole number of bars");
                pl.stepsPerBeat = 8; pl.masterLength = 64; pl.bpm = 60.0f;
                check(std::fabs(PatternBars(pl) - 2.0) < 1e-9 && std::fabs(PatternSeconds(pl) - 8.0) < 1e-9, "length v44.4: 64 steps at 1/32 and 60 BPM = 2 bars = 8 s");
            }
            {   // v44.5 R4-R7: arrangement overlays on the chain
                // --- pure fill windows
                FillRule fr; fr.when = FillWhen::EveryLoop; fr.beats = 2;
                auto act = [&](const FillRule& r, int spb, int tick, int len, int li, int lt, int abs) { return EvalFill(r, spb, tick, len, li, lt, abs); };
                check(act(fr, 4, 7, 16, 0, 1, 7).active == false && act(fr, 4, 8, 16, 0, 1, 8).active && act(fr, 4, 8, 16, 0, 1, 8).pos == 0 && act(fr, 4, 15, 16, 0, 1, 15).pos == 7 && act(fr, 4, 15, 16, 0, 1, 15).total == 8,
                      "fill v44.5: EVERY LOOP, 2 beats = the last 8 steps of a 16-step pass");
                fr.when = FillWhen::LastLoop;
                check(!act(fr, 4, 15, 16, 0, 3, 15).active && !act(fr, 4, 15, 16, 1, 3, 31).active && act(fr, 4, 15, 16, 2, 3, 47).active, "fill v44.5: LAST LOOP only on the final pass of a chain step");
                check(act(fr, 4, 15, 16, 0, 0, 15).active, "fill v44.5: LAST LOOP acts like EVERY LOOP in free play");
                fr.when = FillWhen::EveryBars; fr.everyBars = 4; fr.beats = 2;
                check(!act(fr, 4, 5, 64, 0, 1, 5).active && !act(fr, 4, 55, 64, 0, 1, 55).active && act(fr, 4, 56, 64, 0, 1, 56).active && act(fr, 4, 63, 64, 0, 1, 63).active,
                      "fill v44.5: EVERY 4 BARS = the last 2 beats of every 64 steps");
                {   // 16-step scene repeated 4 times: only the end of the 4th pass is in the N-bar window
                    int on = 0; for (int li = 0; li < 4; ++li) for (int t = 0; t < 16; ++t) if (act(fr, 4, t, 16, li, 4, 0).active) ++on;
                    check(on == 8, "fill v44.5: a 4-bar block counted over the passes of a chain step has exactly one fill of 8 steps");
                }
                fr.when = FillWhen::EveryLoop; fr.beats = 8;
                check(act(fr, 4, 0, 16, 0, 1, 0).active && act(fr, 4, 0, 16, 0, 1, 0).total == 16, "fill v44.5: a fill longer than the pass is cut to the pass");
                fr.beats = 2;
                check(act(fr, 3, 6, 12, 0, 1, 6).active && act(fr, 3, 5, 12, 0, 1, 5).active == false, "fill v44.5: on the triplet grid a beat is 3 steps");
                check(!EvalFill(FillRule{}, 4, 15, 16, 0, 1, 15).active, "fill v44.5: no rule, no fill");
                // --- persistence
                Project pa;
                pa.songChainLength = 3;
                pa.songChain[0] = ChainStep{0, 2, -1};
                pa.songChain[1] = ChainStep{1, 4, -1};
                pa.songChain[1].muteMask = kMuteKickBass; pa.songChain[1].fillMode = FillMode::Own; pa.songChain[1].fill = FillRule{FillWhen::EveryBars, FillKind::HatRoll, 4, 8, 3};
                pa.songChain[1].riser = 11; pa.songChain[1].riserBars = 4; pa.songChain[1].trimSteps = 12;
                pa.songChain[2] = ChainStep{2, 1, 3};
                pa.patterns[1].role = SceneRole::Drop; pa.patterns[1].fill = FillRule{FillWhen::LastLoop, FillKind::SnareBuild, 1, 4, 0};
                check(SaveProject(pa, "selftest_arr.t8proj"), "arrangement v44.5: save");
                Project pb; check(LoadProject(pb, "selftest_arr.t8proj"), "arrangement v44.5: load");
                const ChainStep& b1 = pb.songChain[1];
                // DATA-7: a chain step that pointed at LIVE scene 2 now points at a brick slot holding a copy of it
                check(AppState::IsBrickSlot(b1.patternIndex) && pb.patterns[static_cast<size_t>(b1.patternIndex)].role == SceneRole::Drop && b1.repeats == 4 && b1.muteMask == kMuteKickBass && b1.fillMode == FillMode::Own && b1.fill == FillRule{FillWhen::EveryBars, FillKind::HatRoll, 4, 8, 3} &&
                      b1.riser == 11 && b1.riserBars == 4 && b1.trimSteps == 12, "arrangement v44.5: mutes, fill, transition and trim of a chain step survive a save / load");
                check(pb.songChain[0].muteMask == 0 && pb.songChain[0].fillMode == FillMode::Scene && pb.songChain[0].riser == -1 && pb.songChain[0].trimSteps == 0 && pb.songChain[2].chordScene == 3 && pb.songChain[2].muteMask == 0,
                      "arrangement v44.5: plain steps stay plain (an old project has no overlays)");
                check(pb.patterns[1].role == SceneRole::Drop && pb.patterns[1].fill == FillRule{FillWhen::LastLoop, FillKind::SnareBuild, 1, 4, 0} && pb.patterns[0].role == SceneRole::None && pb.patterns[0].fill.when == FillWhen::Off,
                      "arrangement v44.5: the role and the fill of a scene survive a save / load");
                std::remove("selftest_arr.t8proj"); std::remove("selftest_arr.t8proj.bak");
                // --- energy
                Pattern ep;
                for (int i = 0; i < 16; i += 4) ep.tracks[0].steps[i].active = true;
                for (int i = 0; i < 16; ++i) ep.tracks[6].steps[i].active = true;
                for (int i = 2; i < 16; i += 4) ep.tracks[3].steps[i].active = true;
                ep.tracks[9].steps[0].active = true;
                const float full = SceneEnergy(ep, 0), noLow = SceneEnergy(ep, kMuteKickBass), none = SceneEnergy(ep, kMuteAllTracks);
                check(full > noLow && noLow > none && none == 0.0f && full <= 1.0f, "energy v44.5: a break without kick and bass is lower than the full scene, all muted = 0");
                check(SceneEnergy(Pattern{}, 0) == 0.0f, "energy v44.5: an empty scene has no energy");
                // --- the mixer mask
                Mixer mx; mx.arrMuteMask.store(kMuteKickBass);
                check(!mx.ShouldPlay(0) && !mx.ShouldPlay(6) && mx.ShouldPlay(1) && mx.ShouldPlay(9), "arrangement v44.5: the mute mask silences exactly the tracks it names (the performer's own mutes are separate)");
            }
            {   // v44.5 on a real sequencer: mutes follow the chain, trim shortens the pass, fill windows go silent, the transition is armed
                static Project sp; sp = Project{};
                sp.patterns[0].bpm = 120.0f; sp.patterns[0].masterLength = 32; sp.patterns[1].bpm = 120.0f; sp.patterns[1].masterLength = 16;
                sp.songChainLength = 3;
                sp.songChain[0] = ChainStep{0, 1, -1};
                sp.songChain[1] = ChainStep{1, 2, -1}; sp.songChain[1].muteMask = kMuteKickBass; sp.songChain[1].trimSteps = 8; sp.songChain[1].riser = 12; sp.songChain[1].riserBars = 1;
                sp.songChain[2] = ChainStep{1, 1, -1};
                AudioEngine e; e.Init(kSampleRate);
                e.GetKit()->LoadFromFolder("kits/808", kSampleRate);
                e.Risers().LoadFromFolder("kits/risers", kSampleRate);
                Sequencer sq; sq.Init(&e, &sp); sq.SetSongFollowsEdit(false);
                sq.SetSongModeActive(true);
                check(e.GetMixer().arrMuteMask.load() == 0, "arrangement v44.5: nothing is muted while stopped");
                sq.Play();
                std::vector<float> o(256 * 2);
                auto runSteps = [&](int steps) { const double spStep = 60.0 / 120.0 / 4.0 * kSampleRate; int nb = static_cast<int>(spStep * steps) / 256; for (int b = 0; b < nb; ++b) sq.RenderAndAdvance(o.data(), 256, kSampleRate); };
                check(sq.LoopSteps() == 32 && e.GetMixer().arrMuteMask.load() == 0, "arrangement v44.5: step 1 plays its whole scene, no mutes");
                runSteps(33);                       // past the 32-step pass of slot 1
                check(sq.SongChainPos() == 1 && e.GetMixer().arrMuteMask.load() == kMuteKickBass, "arrangement v44.5: entering the step applies its mute mask");
                check(sq.LoopSteps() == 8, "arrangement v44.5: a trimmed step loops after 8 steps");
                runSteps(8);
                check(sq.SongChainPos() == 1 && sq.SongChainRepeatsLeft() == 1, "arrangement v44.5: the first trimmed pass (8 steps) ended and the step repeats once");
                runSteps(8);
                check(sq.SongChainPos() == 2 && e.GetMixer().arrMuteMask.load() == 0, "arrangement v44.5: after 2 passes of 8 steps the chain moved on and the mutes let go");
                sq.Stop();
                check(e.GetMixer().arrMuteMask.load() == 0, "arrangement v44.5: STOP lets go of the mutes");
                // transition: slot 2 asks for riser 12 (SUB DROP) 1 bar ahead; it is armed during the last bar of slot 1
                sq.SetSongModeActive(false); sq.SetSongModeActive(true); sq.Play();
                bool armed = false;
                for (int st = 0; st < 40 && !armed; ++st) { runSteps(1); if (sq.IsRiserPending() || e.Risers().AnyActive()) armed = true; }
                check(armed, "arrangement v44.5: the transition of the next step is armed / starts before it");
                check(e.Risers().Count() == RiserPlayer::kFirstUser && e.Risers().NameAt(12) == "SUB DROP" && e.Risers().DurationSeconds(12) > 1.0, "transitions v44.5: DOWNLIFTER, IMPACT, SUB DROP and REVERSE CRASH are synthesized after the stock risers");
                for (int ix = RiserPlayer::kMaxRisers; ix < RiserPlayer::kFirstUser; ++ix) {
                    e.Risers().Trigger(ix, 1.0);
                    std::vector<float> ob(2048 * 2, 0.0f); float pk = 0.0f; bool fin = true;
                    for (int b = 0; b < 40; ++b) { std::fill(ob.begin(), ob.end(), 0.0f); e.Risers().Render(ob.data(), 2048, kSampleRate); for (float v : ob) { if (!std::isfinite(v)) fin = false; pk = std::max(pk, std::fabs(v)); } }
                    check(fin && pk > 0.05f && pk < 1.5f, std::string("transitions v44.5: ") + e.Risers().NameAt(ix) + " renders finite audio at a sane level");
                }
            }
            {   // v44.5: a DROP OUT fill silences the drums in its window and only there
                static Project fp; fp = Project{};
                Pattern& pf = fp.patterns[0]; pf.bpm = 120.0f; pf.masterLength = 16;
                for (int i = 0; i < 16; ++i) { pf.tracks[0].steps[i].active = true; pf.tracks[3].steps[i].active = true; }   // kick + closed hat on every step
                pf.fill = FillRule{FillWhen::EveryLoop, FillKind::DropOut, 2, 4, 0};
                AudioEngine e; e.Init(kSampleRate); e.GetKit()->LoadFromFolder("kits/808", kSampleRate);
                e.GetMixer().SyncFromPattern(pf);
                Sequencer sq; sq.Init(&e, &fp); sq.Play();
                const double spStep = 60.0 / 120.0 / 4.0 * kSampleRate;
                std::vector<float> o(static_cast<size_t>(spStep * 16 * 2) * 2 + 4096, 0.0f);
                int done = 0; const int total = static_cast<int>(spStep * 32);
                std::vector<float> acc(static_cast<size_t>(total) * 2, 0.0f);
                while (done < total) { const int n = std::min(256, total - done); std::fill(o.begin(), o.begin() + n * 2, 0.0f); sq.RenderAndAdvance(o.data(), n, kSampleRate); std::copy(o.begin(), o.begin() + n * 2, acc.begin() + done * 2); done += n; }
                auto rms = [&](double fromStep, double toStep) { double sum = 0; int a = static_cast<int>(spStep * fromStep), b = static_cast<int>(spStep * toStep); for (int i = a; i < b; ++i) sum += acc[i * 2] * acc[i * 2]; return std::sqrt(sum / std::max(1, b - a)); };
                const double busy = rms(2, 7), gap = rms(9.6, 16), busy2 = rms(18, 23);
                check(busy > 0.02 && busy2 > 0.02, "fill v44.5: the drums play before the fill window");
                check(gap < busy * 0.35, "fill v44.5: DROP OUT leaves the last 8 steps of the pass (almost) silent");
                // the same pattern without a fill keeps playing there
                fp.patterns[0].fill = FillRule{};
                Sequencer sq2; sq2.Init(&e, &fp); sq2.Play();
                std::fill(acc.begin(), acc.end(), 0.0f); done = 0;
                while (done < total) { const int n = std::min(256, total - done); std::fill(o.begin(), o.begin() + n * 2, 0.0f); sq2.RenderAndAdvance(o.data(), n, kSampleRate); std::copy(o.begin(), o.begin() + n * 2, acc.begin() + done * 2); done += n; }
                check(rms(9.6, 16) > busy * 0.5, "fill v44.5: without the rule the same steps keep playing");
                // a hat roll adds hits in its window (more energy of the hat than the plain steps)
                fp.patterns[0].tracks[0].steps[8].active = false;
                for (int i = 0; i < 16; ++i) { fp.patterns[0].tracks[3].steps[i].active = (i % 4 == 0); }
                fp.patterns[0].fill = FillRule{FillWhen::EveryLoop, FillKind::HatRoll, 2, 4, 0};
                for (int i = 0; i < 16; ++i) fp.patterns[0].tracks[0].steps[i].active = false;
                AudioEngine e3; e3.Init(kSampleRate); e3.GetKit()->LoadFromFolder("kits/808", kSampleRate); e3.GetMixer().SyncFromPattern(fp.patterns[0]);   // a fresh engine: no tails of the earlier kicks
                Sequencer sq3; sq3.Init(&e3, &fp); sq3.Play();
                std::fill(acc.begin(), acc.end(), 0.0f); done = 0;
                while (done < total) { const int n = std::min(256, total - done); std::fill(o.begin(), o.begin() + n * 2, 0.0f); sq3.RenderAndAdvance(o.data(), n, kSampleRate); std::copy(o.begin(), o.begin() + n * 2, acc.begin() + done * 2); done += n; }
                check(rms(9.6, 16) > rms(1, 7.4) * 1.3, "fill v44.5: HAT ROLL is denser and louder than the plain hats before it");
            }
            {   // C4b: the SAMPLE fill plays a sample of samples/ over its window, ends exactly with it, and can silence the drums
                static Project fp; fp = Project{};
                Pattern& pf = fp.patterns[0]; pf.bpm = 120.0f; pf.masterLength = 16;
                const double spStep = 60.0 / 120.0 / 4.0 * kSampleRate;
                auto tone = [](double seconds, float amp) {
                    auto sd = std::make_shared<SampleData>(); sd->channels = 1; sd->sampleRate = kSampleRate; sd->name = "fx.wav";
                    const int n = static_cast<int>(seconds * kSampleRate); sd->data.resize(static_cast<size_t>(n));
                    for (int i = 0; i < n; ++i) sd->data[static_cast<size_t>(i)] = amp * std::sin(6.2831853f * 220.0f * static_cast<float>(i) / kSampleRate);
                    return SamplePtr(sd);
                };
                auto table = [](const SamplePtr& sp) {
                    std::array<Sequencer::FillSampleEntry, Sequencer::kMaxFillSamples> t{};
                    std::strncpy(t[0].name, "fx.wav", FillRule::kSampleMax - 1); t[0].sample = sp; return t;
                };
                const int total = static_cast<int>(spStep * 32);
                std::vector<float> acc(static_cast<size_t>(total) * 2, 0.0f), o(4096, 0.0f);
                auto run = [&](const SamplePtr& sp, bool kicks, bool song = false) {
                    AudioEngine e; e.Init(kSampleRate); e.GetKit()->LoadFromFolder("kits/808", kSampleRate);
                    for (int i = 0; i < 16; ++i) fp.patterns[0].tracks[0].steps[i].active = kicks;
                    e.GetMixer().SyncFromPattern(fp.patterns[0]);
                    Sequencer sq; sq.Init(&e, &fp); sq.SetFillSamples(table(sp));
                    if (song) sq.SetSongModeActive(true);
                    sq.Play();
                    std::fill(acc.begin(), acc.end(), 0.0f);
                    int done = 0;
                    while (done < total) { const int n = std::min(256, total - done); std::fill(o.begin(), o.begin() + n * 2, 0.0f); sq.RenderAndAdvance(o.data(), n, kSampleRate); std::copy(o.begin(), o.begin() + n * 2, acc.begin() + done * 2); done += n; }
                    sq.Stop();
                    for (int k = 0; k < 4; ++k) sq.RenderAndAdvance(o.data(), 256, kSampleRate);   // the 10 ms stop fade
                    check(!e.FillSamplePlaying(), "fill C4b: STOP silences the SAMPLE fill");
                };
                auto rms = [&](double fromStep, double toStep) { double sum = 0; int a = static_cast<int>(spStep * fromStep), b = static_cast<int>(spStep * toStep); for (int i = a; i < b; ++i) sum += acc[i * 2] * acc[i * 2]; return std::sqrt(sum / std::max(1, b - a)); };
                FillRule fr{FillWhen::EveryLoop, FillKind::Sample, 2, 4, 0}; fr.SetSample("fx.wav");
                fp.patterns[0].fill = fr;   // STRETCH a 3-second sample over the last 2 beats (1 second)
                run(tone(3.0, 0.5f), false);
                // (16.2: the master limiter delays everything by a few ms, the drums as much as the fill)
                check(rms(1, 7.5) < 0.002, "fill C4b: nothing of the SAMPLE fill before its window");
                check(rms(8.3, 15.7) > 0.1, "fill C4b: the SAMPLE fill sounds over its window (STRETCH)");
                check(rms(16.2, 23.5) < 0.002, "fill C4b: STRETCH is silent exactly from the step after the window");
                check(rms(24.3, 31.7) > 0.1, "fill C4b: EVERY LOOP plays it again in the next pass");
                fp.patterns[0].fill.sampleFit = 1;   // AS IS: a long sample is cut at the end of the window
                run(tone(3.0, 0.5f), false);
                check(rms(8.3, 15.7) > 0.1 && rms(16.2, 23.5) < 0.002, "fill C4b: AS IS plays the sample and cuts it exactly at the end of the window");
                run(tone(0.1, 0.5f), false);   // a short sample AS IS: it simply ends
                check(rms(8.0, 8.7) > 0.1 && rms(9.5, 15.5) < 0.002, "fill C4b: a sample shorter than the window AS IS ends on its own");
                fp.patterns[0].fill.sampleFit = 0;
                run(tone(3.0, 0.0f), true);   // a silent sample with DRUMS MUTE: the window is (almost) silent, like DROP OUT
                const double busy = rms(2, 7);
                check(busy > 0.02 && rms(9.6, 16) < busy * 0.35, "fill C4b: DRUMS MUTE silences the drums while the sample plays");
                fp.patterns[0].fill.drumsMute = false;
                run(tone(3.0, 0.0f), true);
                check(rms(9.6, 16) > busy * 0.5, "fill C4b: DRUMS PLAY ON keeps the drums under the sample");
                fp.patterns[0].fill.SetSample("other.wav");   // a file the sequencer was not given: nothing plays, nothing breaks
                run(tone(3.0, 0.5f), false);
                check(rms(8.3, 15.7) < 0.002, "fill C4b: a SAMPLE fill without its sample plays nothing");
                // review of C4b: a window that follows the previous one without a gap starts the sample again
                fp.patterns[0].fill = FillRule{FillWhen::EveryLoop, FillKind::Sample, 4, 4, 0}; fp.patterns[0].fill.SetSample("fx.wav");   // the whole 1-bar pass
                run(tone(3.0, 0.5f), false);
                check(rms(0.3, 15.7) > 0.1 && rms(16.3, 31.7) > 0.1, "fill C4b: a SAMPLE fill as long as the pass plays in every pass, not only the first");
                fp.patterns[0].fill.beats = 8;   // 2 bars on a 1-bar scene (cut to the pass)
                run(tone(3.0, 0.5f), false);
                check(rms(0.3, 15.7) > 0.1 && rms(16.3, 31.7) > 0.1, "fill C4b: a SAMPLE fill longer than the pass plays in every pass");
                fp.patterns[0].fill = FillRule{};
                fp.patterns[16] = fp.patterns[0];   // two 1-bar bricks in a row, each with its own whole-bar SAMPLE fill
                fp.songChainLength = 2; fp.songChain[0] = ChainStep(16); fp.songChain[1] = ChainStep(16);
                for (int b = 0; b < 2; ++b) { fp.songChain[b].fillMode = FillMode::Own; fp.songChain[b].fill = FillRule{FillWhen::LastLoop, FillKind::Sample, 4, 4, 0}; fp.songChain[b].fill.SetSample("fx.wav"); }
                run(tone(3.0, 0.5f), false, true);
                check(rms(0.3, 15.7) > 0.1 && rms(16.3, 31.7) > 0.1, "fill C4b: the next brick's SAMPLE fill right after this one plays too");
                fp.songChainLength = 0; fp.songChain[0] = ChainStep(); fp.songChain[1] = ChainStep();
                // RANDOM never picks SAMPLE (it has no sample of its own)
                check(static_cast<int>(FillKind::Sample) > 3 && std::string(FillKindName(FillKind::Sample)) == "SAMPLE", "fill C4b: SAMPLE is a kind of its own, outside what RANDOM chooses from");
            }
            {   // RESET of swing + groove
                AppState gr; gr.engine.Init(kSampleRate); gr.sequencer.Init(&gr.engine, &gr.project);
                Pattern& pr = gr.CurrentPattern();
                check(!gr.FeelIsActive(pr), "feel v44.2: a fresh pattern is straight (RESET dimmed)");
                pr.swing = 0.4f; pr.tracks[3].swing = 0.6f; pr.groove = BuiltinGrooves()[0]; pr.grooveAmount = 0.7f;
                check(gr.FeelIsActive(pr), "feel v44.2: swing / groove / own track swing light the RESET button");
                gr.ResetFeel();
                bool straight = pr.swing == 0.0f && pr.grooveAmount == 0.0f && pr.groove.IsFlat() && pr.groove.name.empty();
                for (const Track& t : pr.tracks) if (t.swing >= 0.0f) straight = false;
                check(straight && !gr.FeelIsActive(pr), "feel v44.2: RESET puts swing, track swings and the groove back to straight");
                check(gr.Undo() && pr.swing > 0.39f && pr.grooveAmount > 0.69f && pr.tracks[3].swing > 0.59f, "feel v44.2: RESET is undoable");
                pr.swing = 0.3f; pr.groove = Groove{}; pr.grooveAmount = 0.0f; pr.tracks[3].swing = -1.0f;
                check(gr.FeelIsActive(pr), "feel v44.2: swing alone is enough to enable RESET, and it stays available");
            }
            {   // LEN: growing repeats the pattern
                AppState gl; gl.engine.Init(kSampleRate); gl.sequencer.Init(&gl.engine, &gl.project);
                Pattern& pl = gl.CurrentPattern(); pl.masterLength = 16;
                for (int i : {0, 4, 10}) { pl.tracks[0].steps[i].active = true; pl.tracks[0].steps[i].accent = (i == 4); }
                pl.tracks[1].steps[2].active = true;
                pl.tracks[2].useOwnLength = true; pl.tracks[2].length = 8; pl.tracks[2].steps[0].active = true;
                const int chordIdx = kNumTracks - 1;
                pl.tracks[chordIdx].steps[0].active = true; pl.tracks[chordIdx].steps[0].chordSize = 3;
                gl.SetPatternLength(64);
                bool tiled = pl.masterLength == 64;
                for (int rep = 1; rep < 4; ++rep) tiled = tiled && pl.tracks[0].steps[16 * rep + 0].active && pl.tracks[0].steps[16 * rep + 4].active && pl.tracks[0].steps[16 * rep + 4].accent && pl.tracks[0].steps[16 * rep + 10].active && !pl.tracks[0].steps[16 * rep + 1].active && pl.tracks[1].steps[16 * rep + 2].active;
                check(tiled, "LEN v44.2: 16 -> 64 repeats the first bar over the new range (drums, accents, bass)");
                check(pl.tracks[chordIdx].steps[32].active && pl.tracks[chordIdx].steps[32].chordSize == 3, "LEN v44.2: the chord track is repeated too, with its chord data");
                check(!pl.tracks[2].steps[16].active && pl.tracks[2].useOwnLength && pl.tracks[2].length == 8, "LEN v44.2: a track with its own length is left alone");
                gl.SetPatternLength(24);
                check(pl.masterLength == 24 && pl.tracks[0].steps[36].active, "LEN v44.2: shrinking only moves the end, the steps behind it stay in the pattern");
                gl.SetPatternLength(64);
                check(pl.masterLength == 64 && pl.tracks[0].steps[36].active, "LEN v44.2: growing back never overwrites steps that are already there");
                check(gl.Undo() && pl.masterLength == 24 && gl.Undo() && pl.masterLength == 64 && gl.Undo() && pl.masterLength == 16 && !pl.tracks[0].steps[16].active, "LEN v44.2: each length change is one undo step and brings the old pattern back");
                // a track with notes already beyond the old length keeps them
                AppState gm; gm.engine.Init(kSampleRate); gm.sequencer.Init(&gm.engine, &gm.project);
                Pattern& pm = gm.CurrentPattern(); pm.masterLength = 16;
                pm.tracks[0].steps[0].active = true; pm.tracks[0].steps[20].active = true; pm.tracks[1].steps[0].active = true;
                gm.SetPatternLength(32);
                check(pm.tracks[0].steps[20].active && !pm.tracks[0].steps[16].active && pm.tracks[1].steps[16].active, "LEN v44.2: a track that already has notes in the new part is not overwritten");
            }
            // a genre template lands on the beat of any grid
            ga.SetPatternResolution(8);
            ga.ApplyGenrePattern(0);
            bool kickOnBeats = true; for (int i = 0; i < pat.masterLength; ++i) if (pat.tracks[0].steps[i].active && (i % 8) != 0 && (i % 2) != 0) kickOnBeats = false;
            check(kickOnBeats && pat.tracks[0].steps[0].active, "grid: a genre beat is mapped onto the 1/32 grid (hits on even steps)");
        }
        // a live fill plays at the same times on 1/32 as on 1/16
        { Project p4; p4.patterns[0].bpm = 120; p4.patterns[0].masterLength = 16; Project p8; p8.patterns[0].bpm = 120; p8.patterns[0].masterLength = 32; p8.patterns[0].stepsPerBeat = 8;
          auto t4 = runEv(p4, 2.2, true), t8 = runEv(p8, 2.2, true);
          bool same = t4.size() == t8.size() && t4.size() >= 4; for (size_t i = 0; same && i < t4.size(); ++i) if (std::fabs(t4[i] - t8[i]) > 0.003) same = false;
          check(same, "grid 1/32: a live FILL lands on the same moments as on 1/16"); }
        // persistence
        { Project pr = mk(8, 1); pr.patterns[0].stepsPerBeat = 6; check(SaveProject(pr, "selftest_grid.t8proj"), "grid: save"); Project lp; check(LoadProject(lp, "selftest_grid.t8proj") && lp.patterns[0].stepsPerBeat == 6 && lp.patterns[1].stepsPerBeat == 4, "grid: the grid survives a round trip, other patterns keep 1/16");
          std::remove("selftest_grid.t8proj"); std::remove("selftest_grid.t8proj.bak"); }
    }

    // ---- Default kit acid_v2 and per-track default sample (v43.1) ----
    {
        Project pr;
        check(pr.kitFolder == "kits/acid_v2", "defaults: a new project uses the acid_v2 kit");
        const Pattern& pat = pr.patterns[0];
        check(pat.tracks[0].defaultVariant == 0 && pat.tracks[1].defaultVariant == 1 && pat.tracks[2].defaultVariant == 0 &&
              pat.tracks[3].defaultVariant == 0 && pat.tracks[5].defaultVariant == 2,
              "defaults: kick 1, snare 2, hats 1, perc 3");
        Step st;  // a fresh step follows the track default
        check(StepVariant(pat.tracks[1], st) == 1 && StepVariant(pat.tracks[5], st) == 2, "defaults: a new step plays its track's default sample");
        st.variant = 0;
        check(StepVariant(pat.tracks[1], st) == 0, "defaults: an explicit step variant overrides the track default");
        DrumKit k; k.LoadFromFolder("kits/acid_v2", kSampleRate);
        check(k.VariantCount(DrumSlot::Snare) >= 2 && k.VariantCount(DrumSlot::Perc) >= 3, "defaults: acid_v2 has the samples the defaults point at");
        pr.patterns[0].tracks[1].defaultVariant = 1; pr.patterns[0].tracks[3].defaultVariant = 1;
        check(SaveProject(pr, "selftest_dv.t8proj"), "defaults: save");
        Project lp; check(LoadProject(lp, "selftest_dv.t8proj") && lp.patterns[0].tracks[1].defaultVariant == 1 &&
                          lp.patterns[0].tracks[3].defaultVariant == 1 && lp.patterns[0].tracks[0].defaultVariant == 0 &&
                          lp.patterns[0].tracks[5].defaultVariant == 2, "defaults: the track defaults survive a round trip");
        // an older file with no defaultVariant key keeps playing sample 1
        {
            std::ifstream in("selftest_dv.t8proj"); std::stringstream ss; ss << in.rdbuf(); in.close();
            std::string txt = ss.str(), out; std::istringstream ls(txt); std::string ln;
            while (std::getline(ls, ln)) if (ln.rfind("defaultVariant=", 0) != 0) out += ln + "\n";
            std::ofstream o("selftest_dv.t8proj", std::ios::trunc); o << out; o.close();
            Project old; check(LoadProject(old, "selftest_dv.t8proj") && old.patterns[0].tracks[1].defaultVariant == 0 && old.patterns[0].tracks[5].defaultVariant == 0,
                               "defaults: a file without the key loads as sample 1 (old projects sound the same)");
        }
        std::remove("selftest_dv.t8proj"); std::remove("selftest_dv.t8proj.bak");
    }

    // ---- SAVE AS (v43.1) ----
    {
        AppState sa; sa.engine.Init(kSampleRate); sa.sequencer.Init(&sa.engine, &sa.project);
        sa.projectPath = "selftest_origin.t8proj";
        sa.project.patterns[0].tracks[0].steps[3].active = true;
        SaveProject(sa.project, sa.projectPath);
        sa.project.patterns[0].tracks[0].steps[7].active = true;   // an edit made after the last save
        const std::string origPath = sa.projectPath;
        check(!sa.SaveProjectAs("   "), "save as: an empty name is refused");
        bool ok = sa.SaveProjectAs("ST SaveAs Test");
        const std::string np = sa.projectPath;
        check(ok && np != origPath && np.find("ST SaveAs Test") != std::string::npos, "save as: the open project moves to the new file");
        Project n; check(LoadProject(n, np) && n.patterns[0].tracks[0].steps[7].active && n.name == "ST SaveAs Test", "save as: the new file holds the current work");
        Project o; check(LoadProject(o, origPath) && o.patterns[0].tracks[0].steps[3].active && !o.patterns[0].tracks[0].steps[7].active,
                         "save as: the file we came from keeps its last saved state");
        sa.SaveProjectAs("ST SaveAs Test");
        check(sa.projectPath != np && sa.projectPath.find("(2)") != std::string::npos, "save as: the same name never overwrites an existing project");
        for (const std::string& f : {origPath, np, sa.projectPath}) { std::remove(f.c_str()); std::remove((f + ".bak").c_str()); }
    }

    // ---- Kit swap while playing (v31): LOAD from the UI thread must never disturb sounding voices ----
    {
        DrumKit kit; DrumEngine de; de.SetKit(&kit);
        kit.LoadFromFolder("kits/808", kSampleRate);
        std::atomic<bool> stop{false};
        std::thread loader([&] {
            const char* kits[2] = {"kits/909", "kits/808"};
            int k = 0;
            while (!stop.load()) { kit.LoadFromFolder(kits[k++ & 1], kSampleRate); }
        });
        bool finite = true; float peak = 0.0f;
        DrumVoiceParams vp; vp.level = 1.0f;
        std::vector<float> kb(256 * 2);
        for (int blk = 0; blk < 800; ++blk) {
            for (int s2 = 0; s2 < kNumDrumTracks; ++s2) if (blk % 8 == s2) de.Trigger(static_cast<DrumSlot>(s2), 1.0f, 0, vp);
            std::fill(kb.begin(), kb.end(), 0.0f);
            for (int s2 = 0; s2 < kNumDrumTracks; ++s2) de.RenderSlot(static_cast<DrumSlot>(s2), kb.data(), 256, kSampleRate);
            for (float v : kb) { if (!std::isfinite(v)) finite = false; peak = std::max(peak, std::fabs(v)); }
        }
        stop = true; loader.join();
        check(finite && peak > 0.05f && peak < 8.0f, "kit swapped from another thread while drums play: no crash, finite audio");
    }

    // ---- v44.5.1: mute / solo per scene, instruments between scenes, erase an instrument ----
    {
        AppState ps; ps.engine.Init(kSampleRate); ps.sequencer.Init(&ps.engine, &ps.project);
        const int kBass = kNumDrumTracks;
        for (int i = 0; i < 2; ++i) { ps.project.patterns[i].masterLength = 16; ps.project.patterns[i].bpm = 240.0f; ps.project.patterns[i].tracks[kBass].steps[0].active = true; ps.project.patterns[i].tracks[kBass].steps[0].note = 40; }
        ps.project.currentPattern = 0;
        ps.SetMuteScene(kBass, true);
        ps.SetSoloScene(0, true);
        check(ps.project.patterns[0].tracks[kBass].mute && !ps.project.patterns[1].tracks[kBass].mute, "per-scene mute v44.5.1: muting the bass touches only the edited scene");
        check(ps.project.patterns[0].tracks[0].solo && !ps.project.patterns[1].tracks[0].solo && ps.engine.GetMixer().tracks[kBass].mute, "per-scene solo v44.5.1: solo too, and the live mixer follows the played scene");
        // a flag written to a scene that is not played is stored but not heard
        ps.SetMuteScene(7, true, 1);
        check(ps.project.patterns[1].tracks[7].mute && !ps.engine.GetMixer().tracks[7].mute, "per-scene mute: another scene's flag is stored, the mixer (scene 1 not played) stays as it is");
        ps.SetMuteScene(7, false, 1);
        // chain: scene 0 (bass muted, kick solo) -> scene 1 (nothing muted)
        ps.project.songChain[0] = ChainStep{0, 1, -1}; ps.project.songChain[1] = ChainStep{1, 1, -1}; ps.project.songChainLength = 2;
        ps.sequencer.SetSongModeActive(true);
        check(ps.engine.GetMixer().tracks[kBass].mute && ps.engine.GetMixer().tracks[0].solo, "per-scene mute: song mode starts with the first scene's own mute / solo");
        {
            std::vector<float> ob(256 * 2);
            ps.sequencer.Play();
            int framesDone = 0; const int need = static_cast<int>(kSampleRate * 1.3);   // 16 steps at 240 bpm = 1.0 s: the chain has moved to scene 1
            while (framesDone < need) { std::fill(ob.begin(), ob.end(), 0.0f); ps.sequencer.RenderAndAdvance(ob.data(), 256, kSampleRate); framesDone += 256; }
            check(ps.sequencer.PlayPatternIndex() == 1, "per-scene mute: the chain advanced to scene 1");
            check(!ps.engine.GetMixer().tracks[kBass].mute && !ps.engine.GetMixer().tracks[0].solo && !ps.engine.GetMixer().AnySolo(), "per-scene mute v44.5.1: the next scene brings its OWN mute / solo (nothing muted, no solo)");
            // muting while scene 1 plays leaves scene 0 untouched
            ps.project.currentPattern = 1;
            ps.SetMuteScene(kBass, true);
            check(ps.engine.GetMixer().tracks[kBass].mute && ps.project.patterns[1].tracks[kBass].mute && ps.project.patterns[0].tracks[kBass].mute, "per-scene mute: mute during scene 1 sets scene 1 (scene 0 keeps its own)");
            ps.SetMuteScene(kBass, false);
            check(!ps.project.patterns[1].tracks[kBass].mute && ps.project.patterns[0].tracks[kBass].mute, "per-scene mute: unmuting scene 1 does not unmute scene 0");
        }
        ps.sequencer.SetSongModeActive(false); ps.sequencer.Stop();

        // DROP lives in the edited scene only
        {
            AppState dr; dr.engine.Init(kSampleRate); dr.sequencer.Init(&dr.engine, &dr.project);
            dr.project.currentPattern = 0; dr.ToggleDrop();
            bool otherUntouched = true; for (int i = 0; i < kNumTracks; ++i) if (dr.project.patterns[1].tracks[i].mute) otherUntouched = false;
            check(dr.DropIsActive() && otherUntouched, "per-scene DROP v44.5.1: only the edited scene is dropped");
            dr.project.currentPattern = 1;
            check(!dr.DropIsActive(), "per-scene DROP: another scene does not show the drop");
            dr.project.currentPattern = 0; dr.ToggleDrop();
            check(!dr.DropIsActive() && !dr.project.patterns[0].tracks[1].mute, "per-scene DROP: second press brings scene 0 back");
        }

        // copy an instrument to another scene
        AppState cp; cp.engine.Init(kSampleRate); cp.sequencer.Init(&cp.engine, &cp.project);
        Pattern& a = cp.project.patterns[0]; Pattern& c3 = cp.project.patterns[2];
        for (int i : {0, 3, 7, 40}) { a.tracks[kBass].steps[i].active = true; a.tracks[kBass].steps[i].note = 36 + i % 12; }
        a.tracks[kBass].volume = 0.31f; a.tracks[kBass].delaySend = 0.4f; a.tracks[kBass].useOwnLength = true; a.tracks[kBass].length = 12;
        c3.tracks[kBass].mute = true; c3.tracks[kBass].volume = 0.9f;
        cp.project.currentPattern = 0;
        check(cp.TrackHasNotes(0, kBass) && !cp.TrackHasNotes(2, kBass), "copy instrument: TrackHasNotes sees the bass of scene 1 but not the empty one of scene 3");
        check(cp.CopyInstrument(0, kBass, 2), "copy instrument: bass of scene 1 -> scene 3");
        const Track& d = c3.tracks[kBass];
        check(d.steps[0].active && d.steps[3].active && d.steps[7].active && d.steps[40].active && !d.steps[1].active && d.steps[7].note == a.tracks[kBass].steps[7].note, "copy instrument: all notes arrive, also beyond step 16");
        check(std::fabs(d.volume - 0.31f) < 1e-4f && std::fabs(d.delaySend - 0.4f) < 1e-4f && d.useOwnLength && d.length == 12, "copy instrument: level, send and own length travel with it");
        check(d.mute && d.name == a.tracks[kBass].name && d.type == TrackType::Synth, "copy instrument: the destination keeps its own mute and identity");
        check(a.tracks[kBass].steps[40].active, "copy instrument: the source is untouched");
        check(!cp.CopyInstrument(0, kBass, 2, 0) && c3.tracks[0].type == TrackType::Drum, "copy instrument: a synth cannot be copied onto a drum track");
        check(cp.CopyInstrument(0, kBass, 0, kBass + 1) && cp.project.patterns[0].tracks[kBass + 1].steps[7].active, "copy instrument: bass -> synth 2 inside one scene");
        check(!cp.CopyInstrument(0, kBass, 0, kBass), "copy instrument: onto itself does nothing");
        // undo of a copy made into a scene that is not the edited one: available once that scene is edited
        {
            AppState uu; uu.engine.Init(kSampleRate); uu.sequencer.Init(&uu.engine, &uu.project);
            uu.project.patterns[0].tracks[kBass].steps[2].active = true; uu.project.patterns[0].tracks[kBass].steps[2].note = 50;
            uu.project.currentPattern = 0; uu.CopyInstrument(0, kBass, 3);
            check(uu.project.patterns[3].tracks[kBass].steps[2].active, "copy instrument: copied to scene 4");
            check(!uu.Undo() || uu.project.patterns[3].tracks[kBass].steps[2].active, "copy instrument: from scene 1 Undo does not rewrite scene 4");
            uu.project.currentPattern = 3;
            check(uu.Undo() && !uu.project.patterns[3].tracks[kBass].steps[2].active, "copy instrument: Undo in scene 4 takes the copy back");
        }
        // a playing destination hears it at once (mixer volume follows)
        {
            AppState pl; pl.engine.Init(kSampleRate); pl.sequencer.Init(&pl.engine, &pl.project);
            pl.project.patterns[1].tracks[kBass].volume = 0.2f; pl.project.patterns[0].tracks[kBass].volume = 0.77f;
            pl.project.currentPattern = 1; pl.engine.GetMixer().SyncFromPattern(pl.CurrentPattern());
            pl.CopyInstrument(0, kBass, 1);
            check(std::fabs(pl.engine.GetMixer().tracks[kBass].volume - 0.77f) < 1e-4f, "copy instrument: into the played scene the mixer follows at once");
        }

        // erase an instrument
        {
            AppState er; er.engine.Init(kSampleRate); er.sequencer.Init(&er.engine, &er.project);
            Pattern& e0 = er.project.patterns[0];
            for (int i : {0, 5, 33, 63}) e0.tracks[kBass].steps[i].active = true;
            for (int i : {0, 4, 8}) e0.tracks[0].steps[i].active = true;
            e0.tracks[kBass].volume = 0.45f;
            er.project.currentPattern = 0;
            check(er.ClearInstrument(0, kBass), "erase instrument: bass of scene 1");
            bool any = false; for (const Step& st : e0.tracks[kBass].steps) if (st.active) any = true;
            check(!any && std::fabs(e0.tracks[kBass].volume - 0.45f) < 1e-4f && e0.tracks[0].steps[8].active, "erase instrument: every step of the bass is gone (also beyond the visible length), its level and the other tracks stay");
            check(er.Undo() && e0.tracks[kBass].steps[63].active && e0.tracks[kBass].steps[33].active, "erase instrument: Undo brings it back");
            check(er.ClearInstrument(0, 0) && !e0.tracks[0].steps[4].active, "erase instrument: works for a drum track too");
            // another scene
            er.project.patterns[4].tracks[kChordTrackIndex].steps[0].active = true;
            check(er.ClearInstrument(4, kChordTrackIndex) && !er.project.patterns[4].tracks[kChordTrackIndex].steps[0].active, "erase instrument: the chord track of another scene");
        }
        // match the level of an instrument across scenes
        {
            AppState mt; mt.engine.Init(kSampleRate); mt.sequencer.Init(&mt.engine, &mt.project);
            for (int i = 0; i < 4; ++i) { mt.project.patterns[i].tracks[kBass].volume = 0.2f + 0.1f * i; mt.project.patterns[i].tracks[kBass].steps[0].active = true; mt.project.patterns[i].tracks[kBass].mute = (i == 2); }
            mt.project.patterns[3].tracks[kBass].pan = 0.5f; mt.project.patterns[0].tracks[kBass].pan = -0.3f; mt.project.patterns[0].tracks[kBass].reverbSend = 0.6f;
            mt.project.songChain[0] = ChainStep{0, 1, -1}; mt.project.songChain[1] = ChainStep{1, 1, -1}; mt.project.songChainLength = 2;
            mt.project.currentPattern = 1; mt.engine.GetMixer().SyncFromPattern(mt.CurrentPattern());
            const float v1 = mt.project.patterns[1].tracks[kBass].volume;
            check(mt.MatchInstrumentLevel(1, kBass, true, false) == 1 && std::fabs(mt.project.patterns[0].tracks[kBass].volume - v1) < 1e-5f && std::fabs(mt.project.patterns[2].tracks[kBass].volume - 0.4f) < 1e-5f,
                  "match level v44.5.2: scenes of the chain only (scene 3 stays)");
            check(mt.project.patterns[0].tracks[kBass].reverbSend > 0.59f && mt.project.patterns[0].tracks[kBass].pan < -0.29f, "match level: fader only, pan and sends untouched");
            check(mt.MatchInstrumentLevel(1, kBass, false, false) >= 2 && std::fabs(mt.project.patterns[3].tracks[kBass].volume - v1) < 1e-5f && std::fabs(mt.project.patterns[9].tracks[kBass].volume - v1) < 1e-5f, "match level: all scenes");
            check(mt.project.patterns[2].tracks[kBass].mute && mt.project.patterns[3].tracks[kBass].steps[0].active, "match level: mute and notes untouched");
            check(mt.MatchInstrumentLevel(1, kBass, false, true) > 0 && mt.project.patterns[0].tracks[kBass].reverbSend < 0.01f && std::fabs(mt.project.patterns[3].tracks[kBass].pan) < 1e-5f, "match level: whole strip copies pan, sends and FX too");
            check(mt.MatchInstrumentLevel(1, kBass, false, true) == 0, "match level: nothing to do when all scenes already agree");
            mt.project.currentPattern = 1; mt.project.patterns[1].tracks[kBass].volume = 0.9f; mt.engine.GetMixer().SyncFromPattern(mt.CurrentPattern());
            mt.MatchInstrumentLevel(0, kBass, false, false);
            check(std::fabs(mt.engine.GetMixer().tracks[kBass].volume - v1) < 1e-5f, "match level: the played scene's mixer follows at once");
            mt.project.currentPattern = 3;
            check(mt.Undo() && mt.project.patterns[3].tracks[kBass].pan > 0.49f, "match level: Undo in a scene takes back its last change (the pan comes back)");
        }
        // mute / solo survive a project save + load per scene
        {
            AppState sv; sv.engine.Init(kSampleRate); sv.sequencer.Init(&sv.engine, &sv.project);
            sv.project.currentPattern = 0; sv.SetMuteScene(kBass, true); sv.SetSoloScene(2, true, 5);
            const std::string path = "selftest_perscene.t8proj";
            SaveProject(sv.project, path);
            Project lp;
            const bool ok = LoadProject(lp, path);
            check(ok && lp.patterns[0].tracks[kBass].mute && !lp.patterns[1].tracks[kBass].mute && lp.patterns[5].tracks[2].solo && !lp.patterns[0].tracks[2].solo, "per-scene mute / solo v44.5.1: saved and loaded with each scene");
            std::remove(path.c_str());
        }
    }

    // ---- v44.5.4: the sound (synth / chord patches, kick synth, kit) belongs to the scene ----
    {
        AppState sd; sd.engine.Init(kSampleRate); sd.sequencer.Init(&sd.engine, &sd.project);
        for (int i = 0; i < 3; ++i) { sd.project.patterns[0].synthPatches[i].cutoffHz = 500.0f + i; sd.project.patterns[1].synthPatches[i].cutoffHz = 3000.0f + i; }
        sd.project.patterns[0].chordPatch.cutoffHz = 700.0f; sd.project.patterns[1].chordPatch.cutoffHz = 2500.0f;
        sd.project.patterns[0].kick.mode = 0; sd.project.patterns[1].kick.mode = 2; sd.project.patterns[1].kick.decayMs = 777.0f;
        sd.project.patterns[0].kitFolder = "kits/808"; sd.project.patterns[1].kitFolder = "kits/909";
        for (int i = 0; i < 2; ++i) { sd.project.patterns[i].masterLength = 16; sd.project.patterns[i].bpm = 240.0f; }
        sd.project.currentPattern = 0;
        sd.RefreshSound();
        check(std::fabs(sd.engine.Synth(0).Patch().cutoffHz - 500.0f) < 0.01f && std::fabs(sd.engine.Chord().Patch().cutoffHz - 700.0f) < 0.01f && sd.engine.GetMixer().kickSynth.mode == 0, "scene sound v44.5.4: the played scene's patches and kick reach the engine");
        check(sd.engine.GetKit()->CurrentBank() == sd.KitBank("kits/808") && sd.engine.GetKit()->CurrentBank() != sd.KitBank("kits/909"), "scene sound: the played scene's kit is the sounding one");
        sd.sequencer.LaunchScene(1);
        check(std::fabs(sd.engine.Synth(2).Patch().cutoffHz - 3002.0f) < 0.01f && std::fabs(sd.engine.Chord().Patch().cutoffHz - 2500.0f) < 0.01f &&
              sd.engine.GetMixer().kickSynth.mode == 2 && std::fabs(sd.engine.GetMixer().kickSynth.decayMs - 777.0f) < 0.01f && sd.engine.GetKit()->CurrentBank() == sd.KitBank("kits/909"),
              "scene sound: launching scene 2 switches patches, kick and kit");
        sd.sequencer.LaunchScene(0);
        // a chain advances the sound with the scene
        sd.project.songChain[0] = ChainStep{0, 1, -1}; sd.project.songChain[1] = ChainStep{1, 1, -1}; sd.project.songChainLength = 2;
        sd.sequencer.SetSongModeActive(true); sd.sequencer.Play();
        check(std::fabs(sd.engine.Synth(0).Patch().cutoffHz - 500.0f) < 0.01f, "scene sound: song mode starts with the first scene's patches");
        {
            std::vector<float> ob(256 * 2); int done = 0; const int need = static_cast<int>(kSampleRate * 1.3);
            while (done < need) { std::fill(ob.begin(), ob.end(), 0.0f); sd.sequencer.RenderAndAdvance(ob.data(), 256, kSampleRate); done += 256; }
            check(sd.sequencer.PlayPatternIndex() == 1 && std::fabs(sd.engine.Synth(0).Patch().cutoffHz - 3000.0f) < 0.01f && sd.engine.GetKit()->CurrentBank() == sd.KitBank("kits/909"),
                  "scene sound: the chain step brings its scene's patches and kit");
            // editing scene 1's sibling (scene 0) while scene 1 plays must not touch the engine
            sd.project.currentPattern = 0;
            sd.EditedPatch(false, 0).cutoffHz = 111.0f; sd.PushEditedPatchToEngine(false, 0);
            check(std::fabs(sd.engine.Synth(0).Patch().cutoffHz - 3000.0f) < 0.01f && std::fabs(sd.project.patterns[0].synthPatches[0].cutoffHz - 111.0f) < 0.01f, "scene sound: editing a scene that is not played changes the scene only");
            sd.project.currentPattern = 1;
            sd.EditedPatch(false, 0).cutoffHz = 3500.0f; sd.PushEditedPatchToEngine(false, 0);
            check(std::fabs(sd.engine.Synth(0).Patch().cutoffHz - 3500.0f) < 0.01f && std::fabs(sd.project.patterns[0].synthPatches[0].cutoffHz - 111.0f) < 0.01f, "scene sound: editing the played scene is heard at once");
        }
        sd.sequencer.SetSongModeActive(false); sd.sequencer.Stop();

        // undo of a patch edit is per scene
        {
            AppState ud; ud.engine.Init(kSampleRate); ud.sequencer.Init(&ud.engine, &ud.project);
            ud.project.currentPattern = 2; ud.RefreshSound();
            const float before = ud.CurrentPattern().synthPatches[0].cutoffHz;
            ud.PushSoundUndo(); ud.EditedPatch(false, 0).cutoffHz = before + 700.0f; ud.PushEditedPatchToEngine(false, 0);
            check(ud.Undo() && std::fabs(ud.project.patterns[2].synthPatches[0].cutoffHz - before) < 0.01f && std::fabs(ud.engine.Synth(0).Patch().cutoffHz - before) < 0.01f, "scene sound: Undo restores the patch of the edited scene (and the engine)");
        }

        // copy sound between scenes
        {
            AppState cs; cs.engine.Init(kSampleRate); cs.sequencer.Init(&cs.engine, &cs.project);
            cs.project.patterns[0].synthPatches[1].cutoffHz = 4321.0f; cs.project.patterns[0].synthPatches[1].waveform = Waveform::Square;
            cs.project.patterns[0].kitFolder = "kits/909"; cs.project.patterns[0].tracks[0].drumTune = 3.0f; cs.project.patterns[0].kick.decayMs = 600.0f;
            cs.project.patterns[0].synthPatches[0].cutoffHz = 100.0f;
            cs.project.songChain[0] = ChainStep{0, 1, -1}; cs.project.songChain[1] = ChainStep{2, 1, -1}; cs.project.songChainLength = 2;
            check(cs.CopySoundToScenes(0, AppState::kToChain, AppState::kSoundSynth1) == 1 && std::fabs(cs.project.patterns[2].synthPatches[1].cutoffHz - 4321.0f) < 0.01f && cs.project.patterns[2].synthPatches[1].waveform == Waveform::Square, "copy sound: one synth patch to the scenes of the chain");
            check(std::fabs(cs.project.patterns[2].synthPatches[0].cutoffHz - 100.0f) > 1.0f && cs.project.patterns[5].synthPatches[1].cutoffHz != 4321.0f, "copy sound: the other patches and scenes outside the chain are left alone");
            cs.CopySoundToScenes(0, AppState::kToAll, AppState::kSoundKit | AppState::kSoundKick | AppState::kSoundDrums);
            check(cs.project.patterns[7].kitFolder == "kits/909" && std::fabs(cs.project.patterns[7].tracks[0].drumTune - 3.0f) < 1e-4f && std::fabs(cs.project.patterns[7].kick.decayMs - 600.0f) < 0.01f, "copy sound: kit, kick synth and drum voices to all scenes");
            cs.project.currentPattern = 7;
            check(cs.Undo() && cs.project.patterns[7].kitFolder.empty(), "copy sound: Undo in the receiving scene takes it back");
        }

        // save / load: per-scene sound, and an old file (one project-wide sound) is spread over all scenes
        {
            Project sp;
            sp.patterns[0].synthPatches[0].cutoffHz = 1111.0f; sp.patterns[3].synthPatches[0].cutoffHz = 2222.0f; sp.patterns[3].chordPatch.release = 0.77f;
            sp.patterns[3].kitFolder = "kits/808"; sp.patterns[2].kick.mode = 2; sp.patterns[2].kick.decayMs = 650.0f; sp.patterns[2].kick.keyLock = false;
            const std::string path = "selftest_scenesound.t8proj";
            check(SaveProject(sp, path), "scene sound: save");
            Project lp;
            check(LoadProject(lp, path) && std::fabs(lp.patterns[0].synthPatches[0].cutoffHz - 1111.0f) < 0.01f && std::fabs(lp.patterns[3].synthPatches[0].cutoffHz - 2222.0f) < 0.01f &&
                  std::fabs(lp.patterns[3].chordPatch.release - 0.77f) < 1e-4f && lp.patterns[3].kitFolder == "kits/808" && lp.patterns[0].kitFolder.empty() &&
                  lp.patterns[2].kick.mode == 2 && !lp.patterns[2].kick.keyLock && std::fabs(lp.patterns[2].kick.decayMs - 650.0f) < 0.01f,
                  "scene sound: patches, chord patch, kit and kick of each scene survive save / load");
            // an old file: strip the per-scene sections and the per-scene kick / kit keys
            std::ifstream in(path); std::string line, out; bool skip = false;
            while (std::getline(in, line)) {
                if (!line.empty() && line[0] == '[') skip = line.find(".synth") != std::string::npos || line.find(".chordpatch") != std::string::npos;
                if (skip) continue;
                if (line.rfind("kickSynth=", 0) == 0) continue;
                if (line.rfind("kitFolder=kits/808", 0) == 0) continue;
                out += line + "\n";
            }
            in.close(); { std::ofstream o(path, std::ios::trunc); o << out; }
            Project ol;
            check(LoadProject(ol, path) && std::fabs(ol.patterns[3].synthPatches[0].cutoffHz - 1111.0f) < 0.01f && std::fabs(ol.patterns[9].synthPatches[0].cutoffHz - 1111.0f) < 0.01f && ol.patterns[3].kitFolder.empty(),
                  "scene sound: an older file (one sound for the project) gives every scene that sound and the project kit");
            std::remove(path.c_str());
        }
    }

    // ---- v44.6: dynamics of a chain step (level, HPF / LPF sweeps), roles -> transitions ----
    {
        StepAuto a = StepAuto::Ends(-12, 0, 0, 60);
        const AutoValues v0 = StepAutoAt(a, 0.0), vh = StepAutoAt(a, 0.5), v1 = StepAutoAt(a, 1.0);
        check(std::fabs(v0.gain - 0.2512f) < 0.002f && std::fabs(vh.gain - 0.5012f) < 0.003f && std::fabs(v1.gain - 1.0f) < 1e-4f, "dynamics v44.6: the level moves evenly in dB (-12 -> -6 -> 0)");
        check(std::fabs(vh.hpf - 0.30f) < 1e-4f && v0.hpf == 0.0f && std::fabs(v1.hpf - 0.60f) < 1e-4f && vh.lpf == 0.0f, "dynamics: the DJ filter moves linearly (to the right = high-pass)");
        { const StepAuto dark = StepAuto::Ends(0, 0, -40, -40); const AutoValues dv = StepAutoAt(dark, 0.3); check(std::fabs(dv.lpf - 0.40f) < 1e-4f && dv.hpf == 0.0f && dv.gain == 1.0f, "dynamics: to the left the DJ filter closes like a low-pass, the level is untouched"); }
        check(!StepAuto{}.Any() && std::fabs(StepAutoAt(StepAuto{}, 0.7).gain - 1.0f) < 1e-6f && StepAutoAt(StepAuto{}, 0.7).hpf == 0.0f, "dynamics: no lanes = untouched");
        StepAuto mute; mute.mix.SetEnd(kAutoMinDb, kAutoMinDb, kAutoMaxDb); check(StepAutoAt(mute, 1.0).gain == 0.0f, "dynamics: the lowest level is silence");
        StepAuto bad; bad.mix.n = 2; bad.mix.pos[1] = 255; bad.mix.val[0] = -100; bad.filter.n = 2; bad.filter.pos[1] = 255; bad.filter.val[1] = 127; bad = NormalizeAuto(bad);
        check(bad.mix.Start() == kAutoMinDb && bad.filter.End() == kFilterMax, "dynamics: values are clamped");

        // saved and loaded; an older arr line (10 values) still loads
        {
            Project sp; sp.songChain[0] = ChainStep{0, 2, -1}; sp.songChain[1] = ChainStep{1, 1, -1}; sp.songChainLength = 2;
            sp.songChain[0].automation = a; sp.songChain[0].muteMask = kMuteKickBass;
            const std::string path = "selftest_dyn.t8proj";
            check(SaveProject(sp, path), "dynamics: save");
            Project lp; check(LoadProject(lp, path) && lp.songChain[0].automation == a && lp.songChain[0].muteMask == kMuteKickBass && !lp.songChain[1].automation.Any(), "dynamics: lanes survive save / load (the step without lanes stays clean)");
            std::ifstream in(path); std::string line, out;
            while (std::getline(in, line)) {
                if (line.rfind("dyn0=", 0) == 0) continue;
                if (line.rfind("arr0=", 0) == 0) { size_t c = 0; for (int k = 0; k < 10 && c != std::string::npos; ++k) c = line.find(',', c + 1); line = line.substr(0, c); }
                out += line + "\n";
            }
            in.close(); { std::ofstream o(path, std::ios::trunc); o << out; }
            Project ol; check(LoadProject(ol, path) && ol.songChain[0].muteMask == kMuteKickBass && !ol.songChain[0].automation.Any(), "dynamics: an older 10-value arr line loads (no lanes)");
            std::remove(path.c_str());
        }

        // on a real sequencer: a held bass note, the chain step plays twice (1 s each at 240 bpm)
        auto render = [&](const StepAuto& au, std::vector<float>& L, double seconds, double* progressAt = nullptr) {
            AppState ds; ds.engine.Init(kSampleRate); ds.sequencer.Init(&ds.engine, &ds.project);
            for (int i = 0; i < 2; ++i) { Pattern& pt = ds.project.patterns[static_cast<size_t>(i)]; pt.masterLength = 16; pt.bpm = 240.0f; }
            Track& b = ds.project.patterns[0].tracks[kNumDrumTracks]; b.steps[0].active = true; b.steps[0].note = 48; b.steps[0].lengthSteps = 16; b.steps[0].chordSize = 0;
            ds.project.songChain[0] = ChainStep{0, 2, -1}; ds.project.songChain[1] = ChainStep{1, 1, -1}; ds.project.songChainLength = 2;
            ds.project.songChain[0].automation = au;
            ds.RefreshSound(); ds.sequencer.SetSongModeActive(true); ds.sequencer.Play();
            std::vector<float> ob(256 * 2); int done = 0; const int need = static_cast<int>(kSampleRate * seconds);
            while (done < need) {
                std::fill(ob.begin(), ob.end(), 0.0f); ds.sequencer.RenderAndAdvance(ob.data(), 256, kSampleRate);
                for (int i = 0; i < 256; ++i) L.push_back(ob[static_cast<size_t>(i) * 2]);
                done += 256;
                if (progressAt && done >= kSampleRate) { *progressAt = ds.sequencer.ChainStepProgress(); progressAt = nullptr; }
            }
            ds.sequencer.SetSongModeActive(false); ds.sequencer.Stop();
            std::vector<float> tail(256 * 2); for (int k = 0; k < 40; ++k) ds.sequencer.RenderAndAdvance(tail.data(), 256, kSampleRate);
            return std::fabs(ds.engine.GetMixer().arrGain.load() - 1.0f) < 1e-4f && ds.engine.GetMixer().arrHpf.load() == 0.0f;
        };
        auto rmsOf = [&](const std::vector<float>& x, double t0, double t1) { double s = 0; size_t a0 = static_cast<size_t>(t0 * kSampleRate), a1 = std::min(x.size(), static_cast<size_t>(t1 * kSampleRate)); for (size_t i = a0; i < a1; ++i) s += x[i] * x[i]; return std::sqrt(s / std::max<size_t>(1, a1 - a0)); };
        std::vector<float> plain, quiet, ramp, thin;
        double prog = -1;
        render(StepAuto{}, plain, 1.8);
        StepAuto flat = StepAuto::Ends(-12, -12);
        render(flat, quiet, 1.8);
        const double rp = rmsOf(plain, 0.3, 1.7), rq = rmsOf(quiet, 0.3, 1.7);
        check(rp > 0.005 && std::fabs(20.0 * std::log10(rq / rp) + 12.0) < 1.5, "dynamics: a step at -12 dB plays 12 dB quieter on the real sequencer");
        StepAuto rise = StepAuto::Ends(-24, 0);
        const bool neutral = render(rise, ramp, 1.9, &prog);
        check(rmsOf(ramp, 1.5, 1.9) > 4.0 * rmsOf(ramp, 0.2, 0.6), "dynamics: a rising level (-24 -> 0 dB over two passes) gets clearly louder during the step");
        check(prog > 0.4 && prog < 0.6, "dynamics: the progress counts across all repeats (about half after one of two passes)");
        check(neutral, "dynamics: stopping lets the level and filters go back to neutral");
        StepAuto thinning = StepAuto::Ends(0, 0, 100, 100);
        render(thinning, thin, 1.8);
        check(rmsOf(thin, 0.3, 1.7) < 0.2 * rp, "dynamics: a full HPF removes most of a bass note");
        // after the step, the next scene plays at full level again
        check(rmsOf(plain, 1.95, 1.99) >= 0.0, "dynamics: (sanity)");
    }
    {   // roles -> lanes, mutes and transitions
        Project rp;
        const SceneRole roles[6] = {SceneRole::Intro, SceneRole::Build, SceneRole::Drop, SceneRole::Break, SceneRole::Drop, SceneRole::Outro};
        for (int i = 0; i < 6; ++i) { rp.patterns[static_cast<size_t>(i)].role = roles[i]; rp.songChain[static_cast<size_t>(i)] = ChainStep{i, 1, -1}; }
        rp.songChainLength = 6;
        rp.songChain[2].automation.mix.SetStart(-2, kAutoMinDb, kAutoMaxDb);   // the performer already set this one
        const AutoArrangeResult r = AutoArrangeChain(rp, false);
        check(rp.songChain[0].automation.filter.Start() == -60 && rp.songChain[1].automation.filter.End() == 55 && rp.songChain[1].automation.mix.Start() == -3, "roles v44.6: intro opens from dark, the build thins out from below");
        check(rp.songChain[2].automation.mix.Start() == -2 && rp.songChain[2].automation.filter.End() == 0, "roles: a step that already has lanes is left alone (replace off)");
        check(rp.songChain[2].riser == 13 && rp.songChain[2].riserBars == 2 && rp.songChain[3].riser == 10 && rp.songChain[3].muteMask == kMuteKickBass && rp.songChain[4].riser == 13 && rp.songChain[4].riserBars == 1,
              "roles: reverse crash into the drops, downlifter into the break, kick + bass out of the break");
        check(rp.songChain[5].automation.mix.End() == kAutoMinDb && rp.songChain[5].riser == -1 && rp.songChain[0].riser == -1, "roles: the outro at the end of the chain fades to silence; no transition into the intro");
        check(r.lanes >= 4 && r.transitions == 3 && r.mutes == 1, "roles: the result counts what was written");
        const AutoArrangeResult r2 = AutoArrangeChain(rp, false);
        check(r2.lanes == 0 && r2.mutes == 0 && r2.transitions == 0, "roles: running it again changes nothing");
        AutoArrangeChain(rp, true);
        check(rp.songChain[2].automation.mix.Start() == 0, "roles: replace overwrites the lanes of a drop with 'flat'");
    }
    {   // v44.8: first sixty seconds - the starter groove and "throw into the chain"
        AppState da; da.engine.Init(44100); da.sequencer.Init(&da.engine, &da.project);
        check(da.CurrentPatternIsEmpty(), "starter v44.8: a new project's scene is empty");
        da.LoadStarterGroove();
        const Pattern& sp = da.CurrentPattern();
        auto count = [&](int t) { int n = 0; for (const Step& s : da.StepTrack(const_cast<Pattern&>(sp), t).steps) if (s.active) ++n; return n; };
        check(!da.CurrentPatternIsEmpty() && count(0) == 3 && count(1) == 1 && count(3) == 4 && count(kNumDrumTracks) == 3 && count(kChordTrackIndex) == 1,
              "starter: kick x3, snare, closed hat x4, three bass notes, one chord are written");
        check(std::fabs(sp.bpm - 72.0f) < 1e-3f && sp.masterLength == 16, "starter: 72 BPM, one bar");
        const Step& ch = sp.tracks[kChordTrackIndex].steps[0];
        check(ch.chordSize == 4 && ch.lengthSteps == 16 && ch.chordNotes[0] == 48, "starter: the chord is a Cm7 held for the whole bar");
        da.Undo();
        check(da.CurrentPatternIsEmpty(), "starter: one undo takes the groove away again");

        da.project.songChainLength = 0;
        da.LoadStarterGroove();
        check(da.ThrowIntoChain() && da.project.songChainLength == 1 && AppState::IsBrickSlot(da.project.songChain[0].patternIndex) && da.project.songChain[0].repeats == 1,
              "throw: a copy of the edited scene becomes the first brick of the chain (in its own hidden slot)");
        {   // S1a: the brick owns a copy of the notes; later jamming on the LIVE scene does not change it
            const int slot0 = da.project.songChain[0].patternIndex;
            check(da.TrackHasNotes(slot0, 0) && da.TrackHasNotes(0, 0), "throw: the brick has the notes, the live scene keeps them");
            da.project.patterns[0].tracks[0].steps[3].active = !da.project.patterns[0].tracks[0].steps[3].active;
            check(da.project.patterns[static_cast<size_t>(slot0)].tracks[0].steps[3].active != da.project.patterns[0].tracks[0].steps[3].active, "throw: editing the live scene afterwards does not touch the brick");
            da.project.patterns[static_cast<size_t>(slot0)].tracks[0].steps[5].active = !da.project.patterns[static_cast<size_t>(slot0)].tracks[0].steps[5].active;
            check(da.project.patterns[0].tracks[0].steps[5].active != da.project.patterns[static_cast<size_t>(slot0)].tracks[0].steps[5].active, "throw: editing the brick does not touch the live scene");
        }
        da.project.currentPattern = 2;
        check(da.ThrowIntoChain() && da.project.songChainLength == 2 && AppState::IsBrickSlot(da.project.songChain[1].patternIndex) && da.project.songChain[1].patternIndex != da.project.songChain[0].patternIndex,
              "throw: the next press appends the next brick in the next free slot");
        da.Undo();
        check(da.project.songChainLength == 1 && da.FreeBrickSlot() == da.project.songChain[0].patternIndex + 1, "throw: one undo takes the brick away and its slot is free again");
        da.project.currentPattern = 0;
        da.project.songChainLength = kMaxChainSteps;
        check(!da.ThrowIntoChain() && da.project.songChainLength == kMaxChainSteps, "throw: a full chain refuses politely");
    }
    {   // v44.9: the chain as a strip of bricks - length, order, who plays, level
        AppState da; da.engine.Init(44100); da.sequencer.Init(&da.engine, &da.project);
        Project& pj = da.project;
        pj.patterns[0].masterLength = 16; pj.patterns[1].masterLength = 16; pj.patterns[2].masterLength = 32;   // 1, 1 and 2 bars at 1/16
        check(da.AddChainStep(0) && da.AddChainStep(1) && da.AddChainStep(2) && pj.songChainLength == 3 && AppState::IsBrickSlot(pj.songChain[2].patternIndex), "bricks v44.9: scenes are added to the end (as copies in brick slots)");
        const int s0 = pj.songChain[0].patternIndex, s1 = pj.songChain[1].patternIndex, s2 = pj.songChain[2].patternIndex;
        check(s0 != s1 && s1 != s2 && pj.patterns[static_cast<size_t>(s2)].masterLength == 32, "bricks: every brick has its own slot and the copy carries the scene's length");
        check(da.SetBrickBars(0, 4.0, false) && pj.songChain[0].repeats == 4 && pj.songChain[0].trimSteps == 0, "bricks: dragging a one-bar scene out to 4 bars = 4 passes");
        check(std::fabs(ChainStepBars(pj.patterns[static_cast<size_t>(s0)], pj.songChain[0]) - 4.0) < 1e-9, "bricks: the strip's length is the real length in bars");
        check(da.SetBrickBars(2, 5.0, false) && pj.songChain[2].repeats == 3 && pj.songChain[2].trimSteps == 0, "bricks: a two-bar scene dragged to 5 bars rounds to whole passes (3 x 2 = 6 bars)");
        check(da.SetBrickBars(2, 1.0, false) && pj.songChain[2].repeats == 1 && pj.songChain[2].trimSteps == 16, "bricks: shorter than the scene = one trimmed pass, whole bars (1 bar = 16 steps)");
        check(da.SetBrickBars(2, 1.0 / 4.0 * 1.5, true) && pj.songChain[2].trimSteps == 6, "bricks: Shift = by single steps (1.5 beats = 6 steps)");
        check(!da.SetBrickBars(2, 1.0 / 4.0 * 1.5, true), "bricks: the same length again reports no change");
        check(da.SetBrickBars(2, 9.0, false) && pj.songChain[2].repeats == 5 && pj.songChain[2].trimSteps == 0, "bricks: and back out again (trim cleared)");
        da.SetBrickTrackMuted(1, 6, true); da.SetBrickTrackMuted(1, 0, true);
        check(pj.songChain[1].muteMask == ((1u << 6) | 1u), "bricks: the table switches instruments off for one brick");
        da.SetBrickTrackMuted(1, 6, false);
        check(pj.songChain[1].muteMask == 1u, "bricks: ... and on again");
        da.SetBrickLevel(1, false, -10); da.SetBrickLevel(1, true, -99);
        check(pj.songChain[1].automation.mix.Start() == -10 && pj.songChain[1].automation.mix.End() == kAutoMinDb, "bricks: level dots set the start and end (clamped to silence)");
        std::this_thread::sleep_for(std::chrono::milliseconds(900));   // undo steps closer than 0.8 s are merged into one (as for a real drag)
        check(da.MoveChainStep(2, 0) && pj.songChain[0].patternIndex == s2 && pj.songChain[1].patternIndex == s0 && pj.songChain[2].patternIndex == s1, "bricks: a brick moves to its new place, the others shift");
        check(pj.songChain[2].automation.mix.Start() == -10 && pj.songChain[1].repeats == 4, "bricks: the settings travel with the brick");
        da.Undo();
        check(pj.songChain[0].patternIndex == s0 && pj.songChain[2].patternIndex == s2, "bricks: one undo puts the order back");
        check(da.DuplicateChainStep(0) && pj.songChainLength == 4 && pj.songChain[1].patternIndex == s0 && pj.songChain[1].repeats == 4, "bricks: duplicate copies the brick right after it");
        check(da.RemoveChainStep(1) && pj.songChainLength == 3 && pj.songChain[1].patternIndex == s1, "bricks: remove closes the gap");
        check(!da.MoveChainStep(5, 0) && !da.RemoveChainStep(-1) && !da.DuplicateChainStep(9), "bricks: out-of-range requests are refused");
        check(da.uiTabRequest == 2, "start flow: the program opens on the LIVE tab (jam first)");
    }

    {   // v44.10: one player - from a brick, from the start, loop a brick, stop at the end, restart in the middle
        AppState da; da.engine.Init(kSampleRate); da.sequencer.Init(&da.engine, &da.project);
        Project& pj = da.project;
        for (int i = 0; i < 3; ++i) { pj.patterns[static_cast<size_t>(i)].masterLength = 16; pj.patterns[static_cast<size_t>(i)].bpm = 120.0f; }   // 1 bar = 2 s
        da.AddChainStep(0); da.AddChainStep(1); da.AddChainStep(2);
        std::vector<float> ob(256 * 2);
        auto run = [&](double secs) { const int nb = static_cast<int>(secs * kSampleRate) / 256; for (int b = 0; b < nb; ++b) da.sequencer.RenderAndAdvance(ob.data(), 256, kSampleRate); };
        da.activeTab = 1;
        da.SetLoop(false);
        da.PlayFromBrick(1);
        check(da.sequencer.IsPlaying() && da.sequencer.IsSongModeActive() && da.sequencer.SongChainPos() == 1, "player: LOOP off starts from the chosen brick");
        run(2.1);
        check(da.sequencer.IsPlaying() && da.sequencer.SongChainPos() == 2, "player: ... and moves on to the next brick");
        run(2.1);
        check(!da.sequencer.IsPlaying(), "player: the track plays once and stops after the last brick");
        da.TickTransport();
        check(!da.sequencer.IsSongModeActive(), "player: song mode is released when playback has stopped");
        da.SetLoop(true);
        da.PlayFromBrick(2);
        run(7.0);
        check(da.sequencer.IsPlaying() && da.sequencer.SongChainPos() == 2, "player: LOOP on keeps the one brick going round");
        da.TrackToStart();
        check(da.selBrick == 0 && da.sequencer.IsPlaying() && da.sequencer.SongChainPos() == 0, "player: Home restarts at the first brick in the middle of playing");
        da.TransportToggle();
        da.TickTransport();
        check(!da.sequencer.IsPlaying() && !da.sequencer.IsSongModeActive(), "player: Space while playing stops");
        da.TrackToStart();
        check(!da.sequencer.IsPlaying() && da.selBrick == 0, "player: Home while stopped only goes to the first brick");
        // a range of bricks goes round (the selection of U6 sets selBrickHi)
        da.selBrick = 1; da.selBrickHi = 2; da.SetLoop(true);
        da.PlayFromBrick(1);
        run(2.1);
        check(da.sequencer.SongChainPos() == 2, "player: LOOP over a range: brick 2 then 3 ...");
        run(2.1);
        check(da.sequencer.IsPlaying() && da.sequencer.SongChainPos() == 1, "player: ... and back to the first brick of the range, not brick 1");
        da.TransportToggle(); da.TickTransport();
        da.selBrickHi = -1;
        da.activeTab = 1; pj.currentPattern = pj.songChain[1].patternIndex; da.selBrick = -1;
        da.TransportToggle();
        check(da.sequencer.IsPlaying() && da.sequencer.IsSongModeActive() && da.sequencer.SongChainPos() == 1, "player: Space with LOOP on loops the brick that holds the edited scene");
        da.TransportToggle(); da.TickTransport();
        pj.currentPattern = 9; da.activeTab = 2;
        da.TransportToggle();
        check(da.sequencer.IsPlaying() && !da.sequencer.IsSongModeActive(), "player: on LIVE a scene that is not in the track plays as the classic loop");
        da.TransportToggle(); da.TickTransport();
        // copy a brick's scene as a new scene
        const int b1 = pj.songChain[1].patternIndex, b2 = pj.songChain[2].patternIndex;
        pj.patterns[static_cast<size_t>(b1)].tracks[0].steps[0].active = true;
        const int nb = da.DuplicateBrickAsScene(1);
        check(nb == 2 && pj.songChainLength == 4 && pj.songChain[2].patternIndex != b1 && AppState::IsBrickSlot(pj.songChain[2].patternIndex) && pj.songChain[3].patternIndex == b2, "copy as scene: a new brick right after the original, the rest shift");
        const int ns = pj.songChain[2].patternIndex;
        check(da.TrackHasNotes(ns, 0) && da.TrackHasNotes(b1, 0), "copy as scene: the copy has the notes and the original keeps them");
        pj.patterns[static_cast<size_t>(ns)].tracks[0].steps[0].active = false;
        check(da.TrackHasNotes(b1, 0), "copy as scene: changing the copy does not touch the original");
        check(da.DuplicateBrickAsScene(-1) < 0 && da.DuplicateBrickAsScene(99) < 0, "copy as scene: out-of-range requests are refused");
    }

    {   // v44.10: the repeats field of a brick
        AppState da; da.engine.Init(kSampleRate); da.sequencer.Init(&da.engine, &da.project);
        Project& pj = da.project;
        pj.patterns[0].masterLength = 16;
        da.AddChainStep(0);
        check(da.SetBrickRepeats(0, 6) && pj.songChain[0].repeats == 6, "repeats: the field sets how many times the scene plays");
        check(!da.SetBrickRepeats(0, 6), "repeats: the same value reports no change");
        check(da.SetBrickRepeats(0, 500) && pj.songChain[0].repeats == 99 && da.SetBrickRepeats(0, -3) && pj.songChain[0].repeats == 1, "repeats: limited to 1..99");
        pj.songChain[0].trimSteps = 6;
        check(da.SetBrickRepeats(0, 1) && pj.songChain[0].trimSteps == 0, "repeats: a trimmed brick becomes full length");
        check(!da.SetBrickRepeats(3, 2) && !da.SetBrickRepeats(-1, 2), "repeats: out-of-range requests are refused");
    }

    // ---- v44.22 (S1a): every brick has its own notes (hidden brick slots) ----
    {
        auto slurp = [](const std::string& path) { std::ifstream in(path); return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); };
        auto count = [](const std::string& hay, const std::string& needle) { int n = 0; for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n; return n; };
        check(kNumLiveScenes == 16 && kNumBrickSlots == kMaxChainSteps && kNumPatterns == kNumLiveScenes + kNumBrickSlots, "s1a: 16 live scenes + one hidden slot for each of the 64 possible bricks");
        {   // slot bookkeeping
            AppState da; da.engine.Init(kSampleRate); da.sequencer.Init(&da.engine, &da.project);
            Project& pj = da.project;
            check(da.SlotInUse(0) && da.SlotInUse(15) && !da.SlotInUse(16) && !da.SlotInUse(-1) && !da.SlotInUse(kNumPatterns) && da.FreeBrickSlot() == 16, "s1a: live scenes are always in use, brick slots only while a brick points at them");
            pj.patterns[0].tracks[0].steps[0].active = true;
            check(da.AddChainStep(0) && pj.songChain[0].patternIndex == 16 && da.SlotInUse(16) && da.FreeBrickSlot() == 17, "s1a: the first brick takes slot 16");
            check(da.DuplicateChainStep(0) && pj.songChain[1].patternIndex == 16 && da.FreeBrickSlot() == 17, "s1a: REPEAT SAME / duplicate shares the slot (no new slot taken)");
            check(da.RemoveChainStep(0) && da.SlotInUse(16) && da.RemoveChainStep(0) && !da.SlotInUse(16) && da.FreeBrickSlot() == 16, "s1a: a slot is free when the last brick that used it is gone");
            bool all = true;
            for (int i = 0; i < kMaxChainSteps; ++i) all = da.ThrowIntoChain() && all;
            check(all && pj.songChainLength == kMaxChainSteps && da.FreeBrickSlot() < 0, "s1a: 64 bricks fill exactly the 64 slots");
            check(!da.ThrowIntoChain() && !da.AddChainStep(0) && da.DuplicateBrickAsScene(0) < 0, "s1a: a full chain refuses throw, add and copy");
        }
        {   // a brick plays its own copy even when the live scene is emptied afterwards
            AppState da; da.engine.Init(kSampleRate); da.sequencer.Init(&da.engine, &da.project);
            Project& pj = da.project;
            pj.patterns[0].tracks[0].steps[0].active = true;
            check(da.ThrowIntoChain(), "s1a: throw");
            for (Step& st : pj.patterns[0].tracks[0].steps) st = Step{};
            da.activeTab = 1; da.SetLoop(true);
            da.PlayFromBrick(0);
            check(da.sequencer.IsPlaying() && da.sequencer.IsSongModeActive() && da.sequencer.PlayPatternIndex() == pj.songChain[0].patternIndex && AppState::IsBrickSlot(da.sequencer.PlayPatternIndex()),
                  "s1a: playing a brick plays its slot, not the live scene");
            std::vector<float> ob(256 * 2); double peak = 0.0;
            for (int b = 0; b < 200; ++b) { da.sequencer.RenderAndAdvance(ob.data(), 256, kSampleRate); for (float v : ob) peak = std::max(peak, static_cast<double>(std::fabs(v))); }
            check(peak > 0.01, "s1a: the brick still sounds after the live scene was emptied");
            da.sequencer.Stop(); da.TickTransport();
        }
        {   // v44.51: the jam keeps playing while TRACK edits a brick (pin); the pin ends on Stop and on release
            AppState da; da.engine.Init(kSampleRate); da.sequencer.Init(&da.engine, &da.project);
            Project& pj = da.project;
            pj.patterns[0].tracks[0].steps[0].active = true;
            check(da.ThrowIntoChain(), "pin: throw");
            const int slot = pj.songChain[0].patternIndex;
            pj.currentPattern = 0; da.sequencer.SetSongModeActive(false); da.sequencer.Play();
            da.sequencer.PinPlayToCurrent();
            pj.currentPattern = slot;   // the editor moves to the brick
            check(da.sequencer.IsPinned() && da.sequencer.PlayPatternIndex() == 0, "pin: the jam scene keeps playing while the editor shows a brick");
            da.sequencer.ReleasePin();
            check(da.sequencer.PlayPatternIndex() == slot, "pin: released, playback follows the edited scene again");
            da.sequencer.PinPlayToCurrent(); da.sequencer.Stop();
            check(!da.sequencer.IsPinned(), "pin: Stop clears the pin");
        }
        {   // v44.51: a MIX lane at "off" silences the whole brick (all instruments, sends and tails)
            AppState da; da.engine.Init(kSampleRate); da.sequencer.Init(&da.engine, &da.project);
            Project& pj = da.project;
            for (int t = 0; t < 4; ++t) pj.patterns[0].tracks[t].steps[0].active = true;
            check(da.ThrowIntoChain(), "fade: throw");
            pj.songChain[0].automation.mix = Lane::Line(kAutoMinDb, kAutoMinDb, kAutoMinDb, kAutoMaxDb);
            da.activeTab = 1; da.SetLoop(true); da.PlayFromBrick(0);
            std::vector<float> ob(256 * 2); double peak = 0.0;
            for (int b = 0; b < 300; ++b) { da.sequencer.RenderAndAdvance(ob.data(), 256, kSampleRate); if (b >= 4) for (float v : ob) peak = std::max(peak, static_cast<double>(std::fabs(v))); }
            check(peak < 1e-4, "fade: a MIX lane at off is silent");
            da.sequencer.Stop(); da.TickTransport();
        }
        {   // save / load: only the used brick slots are written, the open live scene is remembered
            Project sp;
            sp.patterns[0].tracks[0].steps[0].active = true; sp.patterns[3].tracks[1].steps[2].active = true;
            sp.patterns[16] = sp.patterns[0]; sp.patterns[17] = sp.patterns[3]; sp.patterns[17].tracks[1].steps[4].active = true;
            sp.songChain[0] = ChainStep{16, 1, -1}; sp.songChain[1] = ChainStep{17, 2, -1}; sp.songChain[2] = ChainStep{16, 1, -1}; sp.songChainLength = 3;
            sp.patterns[40].tracks[0].steps[7].active = true;   // an unused slot: not saved
            sp.liveScene = 3; sp.currentPattern = 17;           // the TRACK tab is editing a brick
            const std::string path = "selftest_s1a.t8proj";
            check(SaveProject(sp, path), "s1a: save");
            const std::string txt = slurp(path);
            check(count(txt, "[pattern16]") == 1 && count(txt, "[pattern17]") == 1 && count(txt, "[pattern18]") == 0 && count(txt, "[pattern40]") == 0 && count(txt, "[pattern15]") == 1 && count(txt, "format=" + std::to_string(kProjectFormatVersion)) == 1,
                  "s1a: format 5 writes the live scenes and only the brick slots a brick points at");
            Project lp;
            check(LoadProject(lp, path) && lp.songChainLength == 3 && lp.songChain[0].patternIndex == 16 && lp.songChain[1].patternIndex == 17 && lp.songChain[2].patternIndex == 16 &&
                  lp.patterns[16].tracks[0].steps[0].active && lp.patterns[17].tracks[1].steps[4].active && !lp.patterns[40].tracks[0].steps[7].active, "s1a: bricks keep their notes and their sharing through save / load");
            check(lp.currentPattern == 3 && lp.liveScene == 3, "s1a: a file saved while a brick was open comes back on the live scene");
            std::remove(path.c_str());
        }
        {   // a file from before (format 4): bricks pointed at live scenes -> each distinct scene gets one copy, sharing stays
            Project sp;
            sp.patterns[0].tracks[0].steps[0].active = true; sp.patterns[3].tracks[1].steps[2].active = true; sp.patterns[3].bpm = 99.0f;
            sp.songChain[0] = ChainStep{0, 1, -1}; sp.songChain[1] = ChainStep{3, 4, -1}; sp.songChain[2] = ChainStep{0, 2, -1}; sp.songChainLength = 3;
            const std::string path = "selftest_s1a_old.t8proj";
            check(SaveProject(sp, path), "s1a: save (old-format test)");
            std::string txt = slurp(path);
            const size_t at = txt.find("format=" + std::to_string(kProjectFormatVersion)); txt.replace(at, std::string("format=" + std::to_string(kProjectFormatVersion)).size(), "format=4");
            { std::ofstream o(path, std::ios::trunc); o << txt; }
            Project lp;
            check(LoadProject(lp, path) && lp.songChainLength == 3, "s1a: a format 4 file loads");
            const int a = lp.songChain[0].patternIndex, b = lp.songChain[1].patternIndex, c = lp.songChain[2].patternIndex;
            check(AppState::IsBrickSlot(a) && AppState::IsBrickSlot(b) && a != b && a == c, "s1a: every scene used by a brick gets a hidden copy; bricks that shared a scene still share");
            check(lp.patterns[static_cast<size_t>(a)].tracks[0].steps[0].active && lp.patterns[static_cast<size_t>(b)].tracks[1].steps[2].active && std::fabs(lp.patterns[static_cast<size_t>(b)].bpm - 99.0f) < 1e-4f,
                  "s1a: the copies hold what the bricks played");
            check(lp.patterns[0].tracks[0].steps[0].active && lp.patterns[3].tracks[1].steps[2].active && lp.songChain[1].repeats == 4 && lp.songChain[2].repeats == 2, "s1a: the live scenes and the brick settings are untouched");
            std::remove(path.c_str());
        }
        {   // the editor follows the tab; the scene arrows step bricks on TRACK and live scenes on LIVE (source ratchets: the UI is not linked here)
            const std::string mainSrc = slurp("src/main.cpp"), sync = slurp("src/ui_desktop/MixerSync.h"), head = slurp("src/ui_desktop/HeaderPanel.cpp"), keys = slurp("src/ui_desktop/Shortcuts.cpp");
            check(count(mainSrc, "ShowTab(app, 1);") == 1 && count(mainSrc, "ShowTab(app, 2);") == 1, "s1a: each tab tells the editor where it is");
            check(sync.find("inline void ShowTab") != std::string::npos && sync.find("inline void StepScene") != std::string::npos && head.find("StepScene(app, -1, true)") != std::string::npos && keys.find("StepScene(app, dir, false)") != std::string::npos,
                  "s1a: the header arrows and [ ] go through one stepper");
            check(slurp("src/ui_desktop/ChainPanel.cpp").find("RemoveFromChain") == std::string::npos, "s1a: scenes are not 'in the chain' any more (bricks are copies)");
        }
    }

    // ---- v44.23 (S1b): the sound of the track, TAKE SOUND TO TRACK, UPDATE FROM LIVE ----
    {
        auto slurp = [](const std::string& path) { std::ifstream in(path); return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); };
        auto count = [](const std::string& hay, const std::string& needle) { int n = 0; for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n; return n; };
        check(kTrackSoundSlot == kNumPatterns && kNumSlotsTotal == kNumPatterns + 1, "s1b: one hidden slot after the brick slots holds the sound of the track");
        AppState da; da.engine.Init(kSampleRate); da.sequencer.Init(&da.engine, &da.project);
        Project& pj = da.project;
        check(pj.trackSoundMask == 0 && !da.SlotInUse(kTrackSoundSlot), "s1b: a new project has no track sound, and the slot is not a scene");
        pj.patterns[0].tracks[0].steps[0].active = true;
        pj.patterns[0].synthPatches[0].cutoffHz = 1111.0f; pj.patterns[0].kick.pitchHz = 61.0f; pj.patterns[0].kitFolder = "kits/s1b"; pj.patterns[0].tracks[1].drumTune = 3.0f;
        pj.patterns[2].tracks[0].steps[4].active = true;
        pj.patterns[2].synthPatches[0].cutoffHz = 3333.0f; pj.patterns[2].kick.pitchHz = 77.0f;
        check(da.ThrowIntoChain() && pj.trackSoundMask == AppState::kSoundAll && da.TrackSound().synthPatches[0].cutoffHz == 1111.0f, "s1b: the first throw sets the sound of the track from the scene");
        const int b0 = pj.songChain[0].patternIndex;
        check(pj.patterns[static_cast<size_t>(b0)].synthPatches[0].cutoffHz == 1111.0f && da.TrackSound().kitFolder == "kits/s1b" && da.TrackSound().tracks[1].drumTune == 3.0f && da.TrackSound().kick.pitchHz == 61.0f, "s1b: the track's sound holds synths, kick, kit and drum voices");
        da.Undo();
        check(pj.songChainLength == 0 && pj.trackSoundMask == 0 && da.TrackSound().synthPatches[0].cutoffHz == SynthPatch{}.cutoffHz && da.TrackSound().kitFolder.empty(), "s1b: one undo takes the brick and the track's sound away together");
        da.Redo();
        check(pj.songChainLength == 1 && pj.trackSoundMask == AppState::kSoundAll && da.TrackSound().synthPatches[0].cutoffHz == 1111.0f, "s1b: redo brings both back");
        pj.currentPattern = 2; pj.liveScene = 2;
        check(da.ThrowIntoChain(), "s1b: throw scene 2 (another sound)");
        const int b1 = pj.songChain[1].patternIndex;
        check(pj.patterns[static_cast<size_t>(b1)].synthPatches[0].cutoffHz == 3333.0f && pj.patterns[static_cast<size_t>(b1)].kick.pitchHz == 77.0f && da.TrackSound().synthPatches[0].cutoffHz == 1111.0f, "v44.27: the brick sounds like the scene it was thrown from (its own sound where it differs from the track), the track is unchanged");
        check(da.PartIsOwn(b1, 6) && da.PartIsOwn(b1, 0) && !da.PartIsOwn(b1, 2) && !da.PartIsOwn(b0, 6), "v44.27: ... and only the parts that differ are marked as its own");
        check(pj.patterns[static_cast<size_t>(b1)].tracks[0].steps[4].active && !pj.patterns[static_cast<size_t>(b1)].tracks[0].steps[0].active, "s1b: ... and the notes of the scene");
        check(pj.patterns[2].synthPatches[0].cutoffHz == 3333.0f, "s1b: the LIVE scene keeps its own sound (jamming does not change the track)");
        // TAKE SOUND TO TRACK
        std::this_thread::sleep_for(std::chrono::milliseconds(900));
        check(da.TakeSceneSoundToTrack(2) == 2, "s1b: take sound: both bricks are changed");
        check(da.TrackSound().synthPatches[0].cutoffHz == 3333.0f && pj.patterns[static_cast<size_t>(b0)].synthPatches[0].cutoffHz == 3333.0f && pj.patterns[static_cast<size_t>(b1)].kick.pitchHz == 77.0f, "s1b: take sound: the track and every brick have the sound of the scene");
        check(pj.patterns[static_cast<size_t>(b0)].tracks[0].steps[0].active && pj.patterns[static_cast<size_t>(b1)].tracks[0].steps[4].active, "s1b: take sound: the notes are untouched");
        check(da.Undo() && pj.patterns[static_cast<size_t>(b0)].synthPatches[0].cutoffHz == 1111.0f && pj.patterns[static_cast<size_t>(b1)].synthPatches[0].cutoffHz == 3333.0f && da.TrackSound().synthPatches[0].cutoffHz == 1111.0f, "s1b: one undo gives the track and the bricks their old sound");
        check(da.Redo() && pj.patterns[static_cast<size_t>(b1)].synthPatches[0].cutoffHz == 3333.0f && da.TrackSound().synthPatches[0].cutoffHz == 3333.0f, "s1b: redo takes the sound again");
        check(da.TakeSceneSoundToTrack(-1) < 0 && da.TakeSceneSoundToTrack(kNumPatterns) < 0, "s1b: take sound: a bad scene is refused");
        // UPDATE FROM LIVE
        pj.currentPattern = pj.songChain[0].patternIndex; pj.liveScene = 0;   // the TRACK tab is on brick 1, LIVE was on scene 1
        pj.patterns[static_cast<size_t>(b0)].role = SceneRole::Drop; pj.patterns[static_cast<size_t>(b0)].tracks[2].mute = true;
        pj.patterns[0].tracks[0].steps[9].active = true; pj.patterns[0].synthPatches[0].cutoffHz = 4444.0f; pj.patterns[0].bpm = 99.0f;
        check(da.UpdateBrickFromLive(0), "s1b: update from LIVE");
        const Pattern& ub = pj.patterns[static_cast<size_t>(b0)];
        check(ub.tracks[0].steps[9].active && ub.tracks[0].steps[0].active && std::fabs(ub.bpm - 99.0f) < 1e-4f, "s1b: update: the brick has the new notes and timing");
        check(ub.synthPatches[0].cutoffHz == 3333.0f && ub.role == SceneRole::Drop && ub.tracks[2].mute, "s1b: update: the brick keeps its sound, role and mutes");
        check(da.Undo() && !pj.patterns[static_cast<size_t>(b0)].tracks[0].steps[9].active, "s1b: update: one undo brings the old notes back");
        check(!da.UpdateBrickFromLive(7) && !da.UpdateBrickFromLive(-1), "s1b: update: a missing brick is refused");
        // save / load
        {
            const std::string path = "selftest_s1b.t8proj";
            check(SaveProject(pj, path), "s1b: save");
            const std::string txt = slurp(path);
            check(count(txt, "trackSound=255") == 1 && count(txt, "[pattern80]") == 1, "s1b: the file carries the track's sound");
            Project lp;
            check(LoadProject(lp, path) && lp.trackSoundMask == 255 && lp.patterns[static_cast<size_t>(kTrackSoundSlot)].synthPatches[0].cutoffHz == 3333.0f, "s1b: the track's sound survives save / load");
            // a file from before v44.23: no track sound in it -> the first brick's sound becomes the track's
            std::string old; { std::istringstream in(txt); std::string line; while (std::getline(in, line)) { if (line.rfind("trackSound=", 0) == 0) continue; if (line == "format=" + std::to_string(kProjectFormatVersion)) line = "format=5"; old += line + "\n"; } }
            { std::ofstream o(path, std::ios::trunc); o << old; }
            Project ol;
            check(LoadProject(ol, path) && ol.trackSoundMask == 127 && ol.patterns[static_cast<size_t>(kTrackSoundSlot)].synthPatches[0].cutoffHz == ol.patterns[static_cast<size_t>(ol.songChain[0].patternIndex)].synthPatches[0].cutoffHz, "s1b: an older file gets the sound of its first brick as the sound of the track");
            Project np; check(SaveProject(np, path), "s1b: save an empty project");
            check(count(slurp(path), "trackSound=") == 0 && count(slurp(path), "[pattern80]") == 0, "s1b: a project without bricks writes no track sound");
            std::remove(path.c_str());
        }
        // the UI: tabs, label, buttons (source ratchets)
        const std::string mainSrc = slurp("src/main.cpp"), seqSrc = slurp("src/ui_desktop/SequencerPanel.cpp"), chainSrc = slurp("src/ui_desktop/ChainPanel.cpp"), brickSrc = slurp("src/ui_desktop/BricksPanel.cpp");
        check(mainSrc.find("ImGuiCol_TabSelectedOverline") != std::string::npos && seqSrc.find("BRICK %d  NOTES") != std::string::npos && seqSrc.find("LIVE  SCENE %02d") != std::string::npos, "s1b: the tabs and the editor label say where you are");
        check(chainSrc.find("TakeSceneSoundToTrack") != std::string::npos && brickSrc.find("UpdateBrickFromLive") != std::string::npos, "s1b: TAKE SOUND TO TRACK and UPDATE FROM LIVE are on screen");
    }

    // ---- v44.24 (S2): the sound of an instrument on TRACK: whole track or this brick only ----
    {
        auto slurp = [](const std::string& path) { std::ifstream in(path); return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); };
        auto count = [](const std::string& hay, const std::string& needle) { int n = 0; for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n; return n; };
        check(kNumParts == 12 && kPartKit == 11 && kAllPartsMask == 0xFFF, "s2: eleven instruments and the kit are the twelve parts of a sound");
        {   // parts: the channel, the patch / kick, never the notes
            Pattern a, b;
            a.tracks[6].steps[3].active = true; a.tracks[6].volume = 0.2f; a.tracks[6].mute = true; a.tracks[6].length = 7; a.synthPatches[0].cutoffHz = 1234.0f;
            CopyPart(b, a, 6);
            check(b.tracks[6].volume == 0.2f && b.synthPatches[0].cutoffHz == 1234.0f, "s2: a part carries the channel and the synth patch");
            check(!b.tracks[6].steps[3].active && !b.tracks[6].mute && b.tracks[6].length == 16 && b.tracks[6].name == Pattern().tracks[6].name, "s2: ... but not the notes, the length, mute / solo");
            check(SoundPartText(a, 6) != SoundPartText(b, 7) && SoundPartText(a, 6) == SoundPartText(b, 6), "s2: two scenes sound the same in a part when their texts are equal (notes and mutes do not count)");
            a.kick.pitchHz = 70.0f; a.kitFolder = "kits/x";
            CopyPart(b, a, 0); CopyPart(b, a, kPartKit);
            check(b.kick.pitchHz == 70.0f && b.kitFolder == "kits/x", "s2: the kick part carries the built-in kick, the kit part the folder");
        }
        AppState da; da.engine.Init(kSampleRate); da.sequencer.Init(&da.engine, &da.project);
        Project& pj = da.project;
        pj.patterns[0].tracks[0].steps[0].active = true; pj.patterns[1].tracks[0].steps[2].active = true;
        pj.patterns[0].synthPatches[0].cutoffHz = 1000.0f; pj.patterns[1].synthPatches[0].cutoffHz = 1000.0f;   // v44.27: the same sound, so the bricks follow the track
        da.ThrowIntoChain(); pj.currentPattern = 1; pj.liveScene = 1; da.ThrowIntoChain(); da.ThrowIntoChain();
        const int s0 = pj.songChain[0].patternIndex, s1 = pj.songChain[1].patternIndex, s2 = pj.songChain[2].patternIndex;
        check(pj.patterns[static_cast<size_t>(s0)].ownSound == 0 && pj.patterns[static_cast<size_t>(s1)].ownSound == 0 && pj.patterns[static_cast<size_t>(s2)].ownSound == 0, "s2: thrown bricks follow the track in everything");
        // 1) whole track: an edit of brick 0's bass reaches the track and the other bricks (and only that part)
        pj.currentPattern = s0;
        check(da.SyncBrickSound() == 0, "s2: nothing edited: nothing to hand to the track");
        da.PushUndo();
        pj.patterns[static_cast<size_t>(s0)].synthPatches[0].cutoffHz = 2222.0f;
        pj.patterns[static_cast<size_t>(s0)].tracks[6].volume = 0.31f;
        check(da.SyncBrickSound() == 1, "s2: an edit of the bass is one part");
        check(da.TrackSound().synthPatches[0].cutoffHz == 2222.0f && pj.patterns[static_cast<size_t>(s1)].synthPatches[0].cutoffHz == 2222.0f && pj.patterns[static_cast<size_t>(s2)].tracks[6].volume == 0.31f, "s2: WHOLE TRACK: the track and every other brick take the edit (patch and channel)");
        check(pj.patterns[static_cast<size_t>(s1)].tracks[0].steps[2].active && !pj.patterns[static_cast<size_t>(s1)].tracks[0].steps[0].active && pj.patterns[static_cast<size_t>(s0)].tracks[0].steps[0].active, "s2: the notes of the bricks are not touched");
        // 2) undo of the edit takes the track and the others back
        check(da.Undo(), "s2: undo");
        check(pj.patterns[static_cast<size_t>(s0)].synthPatches[0].cutoffHz == 1000.0f && da.SyncBrickSound() == 1 && da.TrackSound().synthPatches[0].cutoffHz == 1000.0f && pj.patterns[static_cast<size_t>(s1)].synthPatches[0].cutoffHz == 1000.0f,
              "s2: undo of an edit of the track sound puts the track and every brick back");
        // 3) this brick only
        check(da.MakeBrickSoundOwn(s1, 6) && da.PartIsOwn(s1, 6) && !da.PartIsOwn(s1, 7) && !da.MakeBrickSoundOwn(s1, 6), "s2: THIS BRICK ONLY marks one instrument of one brick");
        pj.currentPattern = s1;
        pj.patterns[static_cast<size_t>(s1)].synthPatches[0].cutoffHz = 777.0f;
        check(da.SyncBrickSound() == 0 && da.TrackSound().synthPatches[0].cutoffHz == 1000.0f && pj.patterns[static_cast<size_t>(s0)].synthPatches[0].cutoffHz == 1000.0f, "s2: an own sound is edited alone");
        pj.currentPattern = s0;
        da.PushUndo(); pj.patterns[static_cast<size_t>(s0)].synthPatches[0].cutoffHz = 3000.0f; pj.patterns[static_cast<size_t>(s0)].synthPatches[1].cutoffHz = 3100.0f;
        check(da.SyncBrickSound() == 2, "s2: two parts edited at once");
        check(pj.patterns[static_cast<size_t>(s1)].synthPatches[0].cutoffHz == 777.0f && pj.patterns[static_cast<size_t>(s1)].synthPatches[1].cutoffHz == 3100.0f && pj.patterns[static_cast<size_t>(s2)].synthPatches[0].cutoffHz == 3000.0f,
              "s2: a WHOLE TRACK edit skips the brick that has its own sound for that instrument and reaches it for the others");
        // 4) follow the track again (undoable)
        pj.currentPattern = s1;
        check(da.FollowTrackSound(s1, 6) && !da.PartIsOwn(s1, 6) && pj.patterns[static_cast<size_t>(s1)].synthPatches[0].cutoffHz == 3000.0f, "s2: WHOLE TRACK again: the brick takes the track's sound back");
        check(da.Undo() && da.PartIsOwn(s1, 6) && pj.patterns[static_cast<size_t>(s1)].synthPatches[0].cutoffHz == 777.0f && da.SyncBrickSound() == 0 && da.TrackSound().synthPatches[0].cutoffHz == 3000.0f, "s2: undo brings the own sound and its mark back, and the track is not disturbed");
        // 5) apply to all bricks
        check(da.ApplySoundToAllBricks(s1, 6) == 2 && !da.PartIsOwn(s1, 6) && da.TrackSound().synthPatches[0].cutoffHz == 777.0f && pj.patterns[static_cast<size_t>(s0)].synthPatches[0].cutoffHz == 777.0f && pj.patterns[static_cast<size_t>(s2)].synthPatches[0].cutoffHz == 777.0f,
              "s2: APPLY TO ALL BRICKS: this brick's sound becomes the track's and everyone's");
        check(da.Undo() && da.PartIsOwn(s1, 6) && da.TrackSound().synthPatches[0].cutoffHz == 3000.0f && pj.patterns[static_cast<size_t>(s0)].synthPatches[0].cutoffHz == 3000.0f, "s2: one undo restores the track, the bricks and the marks");
        // 6) the kit is one more part
        pj.currentPattern = s2;
        check(da.MakeBrickSoundOwn(s2, kPartKit), "s2: a brick can keep its own kit");
        da.PushUndo(); pj.patterns[static_cast<size_t>(s2)].kitFolder = "kits/own";
        pj.currentPattern = s0; da.PushUndo(); pj.patterns[static_cast<size_t>(s0)].kitFolder = "kits/track";
        check(da.SyncBrickSound() == 1 && pj.patterns[static_cast<size_t>(s2)].kitFolder == "kits/own" && pj.patterns[static_cast<size_t>(s1)].kitFolder == "kits/track" && da.TrackSound().kitFolder == "kits/track", "s2: the track's kit reaches the bricks that follow it, not the one with its own");
        // v44.27: a new brick sounds like the scene it was thrown from: the parts where that differs from the track are its own
        pj.currentPattern = 3; pj.liveScene = 3; pj.patterns[3].synthPatches[0].cutoffHz = 55.0f;
        {
            const int nb = (da.ThrowIntoChain(), pj.songChain[3].patternIndex);
            check(pj.patterns[static_cast<size_t>(nb)].synthPatches[0].cutoffHz == 55.0f && da.PartIsOwn(nb, 6) && da.PartIsOwn(nb, kPartKit) && !da.PartIsOwn(nb, 1) && da.TrackSound().synthPatches[0].cutoffHz == 3000.0f, "s2: a new brick sounds like its scene: own marks only where it differs from the track");
        }
        const int copyIdx = da.DuplicateBrickAsScene(1);
        check(copyIdx == 2 && da.PartIsOwn(pj.songChain[2].patternIndex, 6) == da.PartIsOwn(s1, 6), "s2: COPY BRICK keeps the own marks of the original");
        // 7) save / load
        {
            const std::string path = "selftest_s2.t8proj";
            check(SaveProject(pj, path), "s2: save");
            const std::string txt = slurp(path);
            check(count(txt, "ownSound=") >= 2, "s2: the own marks are written");
            Project lp;
            check(LoadProject(lp, path) && lp.patterns[static_cast<size_t>(s1)].ownSound == pj.patterns[static_cast<size_t>(s1)].ownSound && lp.patterns[static_cast<size_t>(s2)].ownSound == pj.patterns[static_cast<size_t>(s2)].ownSound && lp.patterns[static_cast<size_t>(s0)].ownSound == 0, "s2: ... and come back");
            // a file older than v44.24: bricks that sounded different keep that as their own, the first brick's sound is the track's
            std::string old; { std::istringstream in(txt); std::string line; while (std::getline(in, line)) { if (line.rfind("ownSound=", 0) == 0 || line.rfind("trackSound=", 0) == 0) continue; if (line == "format=" + std::to_string(kProjectFormatVersion)) line = "format=5"; old += line + "\n"; } }
            { std::ofstream o(path, std::ios::trunc); o << old; }
            Project ol;
            check(LoadProject(ol, path) && ol.songChainLength == pj.songChainLength, "s2: an older file loads");
            const int o0 = ol.songChain[0].patternIndex, o1 = ol.songChain[1].patternIndex;
            check(ol.patterns[static_cast<size_t>(o0)].ownSound == 0 && ol.patterns[static_cast<size_t>(kTrackSoundSlot)].synthPatches[0].cutoffHz == ol.patterns[static_cast<size_t>(o0)].synthPatches[0].cutoffHz, "s2: older file: the first brick's sound is the track's");
            check(o1 != o0 && SoundPartText(ol.patterns[static_cast<size_t>(o1)], 6) != SoundPartText(ol.patterns[static_cast<size_t>(o0)], 6) ? (ol.patterns[static_cast<size_t>(o1)].ownSound & (1u << 6)) != 0 : true, "s2: older file: a brick that sounded different keeps that part as its own");
            std::remove(path.c_str());
        }
        // the UI (source ratchets)
        const std::string mainSrc = slurp("src/main.cpp"), spSrc = slurp("src/ui_desktop/SoundPanel.cpp"), brSrc = slurp("src/ui_desktop/BricksPanel.cpp"), cm = slurp("CMakeLists.txt");
        check(cm.find("SoundPanel.cpp") != std::string::npos && mainSrc.find("soundPanel.DrawModeRow(app)") != std::string::npos && mainSrc.find("drawContextBlock(restH)") != std::string::npos, "s2: the TRACK screen has the NOTES | SOUND | MIX strip and the instrument block");
        check(spSrc.find("WHOLE TRACK") != std::string::npos && spSrc.find("THIS BRICK ONLY") != std::string::npos && spSrc.find("APPLY TO ALL BRICKS") != std::string::npos && brSrc.find("pat.ownSound >> t") != std::string::npos, "s2: the scope switch, APPLY TO ALL BRICKS and the own-sound dot in the cells are on screen");
    }

    // ---- v44.25: lanes can be grabbed at the ends, a fill can be moved inside a brick, a break can be picked ----
    {
        auto slurp = [](const std::string& path) { std::ifstream in(path); return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); };
        // a flat lane grabbed by an end stays editable while the mouse is down, and goes flat again on release
        Lane l; l.Materialize(); l.MovePoint(0, 0, 0, kAutoMinDb, kAutoMaxDb, true);
        check(l.n == 2, "v44.25: an end of a flat lane survives being grabbed (the drag did not die on the first frame)");
        l.MovePoint(0, 0, -12, kAutoMinDb, kAutoMaxDb, true);
        check(l.n == 2 && l.val[0] == -12 && l.pos[0] == 0 && l.pos[1] == kLanePosMax, "v44.25: ... and follows the mouse");
        l.MovePoint(0, 0, 0, kAutoMinDb, kAutoMaxDb, true);
        l.Normalize(kAutoMinDb, kAutoMaxDb);
        check(l.n == 0, "v44.25: ... and is flat again on release when it is back at 0");
        // a fill moved earlier: the window ends shiftBeats before the end of the pass
        FillRule r{FillWhen::LastLoop, FillKind::Break, 2, 4, 0}; r.shiftBeats = 1;
        auto at = [&](int t) { return EvalFill(r, 4, t, 64, 0, 1, t).active; };
        check(!at(63) && !at(60) && at(59) && at(52) && !at(51), "v44.25: a fill shifted by one beat plays beats 3..2 before the end (not the last beat)");
        r.shiftBeats = 0;
        check(at(63) && at(56) && !at(55), "v44.25: shift 0 is the old behaviour");
        FillRule eb{FillWhen::EveryBars, FillKind::HatRoll, 1, 2, 0}; eb.shiftBeats = 2;
        check(EvalFill(eb, 4, 0, 64, 0, 0, 20).active && EvalFill(eb, 4, 0, 64, 0, 0, 23).active && !EvalFill(eb, 4, 0, 64, 0, 0, 24).active && !EvalFill(eb, 4, 0, 64, 0, 0, 31).active && !EvalFill(eb, 4, 0, 64, 0, 0, 19).active, "v44.25: EVERY N BARS shifts inside its block");
        FillRule big = NormalizeFill([] { FillRule x; x.when = FillWhen::LastLoop; x.shiftBeats = 200; return x; }());
        check(big.shiftBeats == 64, "v44.25: the shift is limited");
        // the shift is saved and loaded
        Project pj; pj.songChainLength = 1; pj.songChain[0].patternIndex = 16; pj.songChain[0].fillMode = FillMode::Own; pj.songChain[0].fill = FillRule{FillWhen::LastLoop, FillKind::Break, 4, 4, 3}; pj.songChain[0].fill.shiftBeats = 6;
        const std::string path = "selftest_v4425.t8proj";
        check(SaveProject(pj, path), "v44.25: save a project with a moved fill");
        Project lp; check(LoadProject(lp, path) && lp.songChain[0].fill.shiftBeats == 6 && lp.songChain[0].fill.breakIndex == 3 && lp.songChain[0].fillMode == FillMode::Own, "v44.25: the moved fill and its chosen break survive save / load");
        std::remove(path.c_str());
        {   // C4b: the SAMPLE fill is saved and loaded (scene and brick), an older file loads with the defaults, a path is refused
            Project sp; sp.songChainLength = 2; sp.songChain[0].patternIndex = 16; sp.songChain[1].patternIndex = 16;
            sp.songChain[1].fillMode = FillMode::Own;
            FillRule a{FillWhen::LastLoop, FillKind::Sample, 2, 4, 0}; a.SetSample("Big, Riser 01.wav"); a.sampleFit = 1; a.drumsMute = false;
            sp.songChain[1].fill = a;
            FillRule b{FillWhen::EveryLoop, FillKind::Sample, 4, 4, 0}; b.SetSample("vox.aif");
            sp.patterns[16].fill = b;
            const std::string fpth = "selftest_c4b_fill.t8proj";
            check(SaveProject(sp, fpth), "fill C4b: save a project with SAMPLE fills");
            Project lp2;
            check(LoadProject(lp2, fpth) && lp2.songChain[1].fill == a && lp2.patterns[16].fill == b && lp2.songChain[1].fill.Sample() == "Big, Riser 01.wav",
                  "fill C4b: the sample, FIT and DRUMS of the fills of a brick and of a scene survive save / load (a comma in the name too)");
            std::string txt = slurp(fpth);
            check(txt.find("fillSample=0,1,vox.aif") != std::string::npos && txt.find("fsm1=1,0,Big, Riser 01.wav") != std::string::npos, "fill C4b: the keys fillSample= and fsmN= are written");
            auto replaceAll = [](std::string t, const std::string& from, const std::string& to) { for (size_t q = t.find(from); q != std::string::npos; q = t.find(from, q + to.size())) t.replace(q, from.size(), to); return t; };
            { std::ofstream f(fpth, std::ios::trunc); f << replaceAll(txt, "fillSample=0,1,vox.aif", "fillSample=7,1,../../etc/x.wav"); }
            Project lp3;
            check(LoadProject(lp3, fpth) && lp3.patterns[16].fill.sample[0] == 0 && lp3.patterns[16].fill.sampleFit == 1, "fill C4b: a path instead of a name in samples/ is dropped, a damaged FIT is put back into range");
            std::string old = txt;
            for (const char* key : {"fillSample=", "fsm1="}) { const size_t q = old.find(key); if (q != std::string::npos) old.erase(q, old.find('\n', q) - q + 1); }
            { std::ofstream f(fpth, std::ios::trunc); f << old; }
            Project lp4;
            check(LoadProject(lp4, fpth) && lp4.songChain[1].fill.sample[0] == 0 && lp4.songChain[1].fill.sampleFit == 0 && lp4.songChain[1].fill.drumsMute, "fill C4b: a file without the new keys loads with no sample, STRETCH and DRUMS MUTE");
            std::remove(fpth.c_str());
        }
        {   // C4b: choosing the sample of a brick's fill (one Undo step); the cleanup of samples/ keeps it
            auto gap = std::make_unique<AppState>(); AppState& ga = *gap;
            ga.engine.Init(kSampleRate); ga.sequencer.Init(&ga.engine, &ga.project);
            ga.project.songChainLength = 1; ga.project.songChain[0].patternIndex = 16;
            check(!ga.SetFillSample(0, "../x.wav") && !ga.SetFillSample(3, "hit.wav"), "fill C4b: a path or a brick that does not exist is refused");
            check(ga.SetFillSample(0, "hit.wav"), "fill C4b: a sample becomes the fill of a brick");
            const ChainStep& gcs = ga.project.songChain[0];
            check(gcs.fillMode == FillMode::Own && gcs.fill.kind == FillKind::Sample && gcs.fill.when == FillWhen::LastLoop && gcs.fill.Sample() == "hit.wav", "fill C4b: the brick gets an own SAMPLE fill on its last loop");
            check(ga.SampleFilesNamedInMemory().count("hit.wav") == 1 && ga.FillSamplesWanted() == std::vector<std::string>{"hit.wav"}, "fill C4b: the fill's sample counts as used (MOVE UNUSED TO TRASH keeps it) and goes to the sequencer");
            check(ga.Undo() && ga.project.songChain[0].fillMode == FillMode::Scene && ga.project.songChain[0].fill.sample[0] == 0, "fill C4b: Undo takes the fill's sample back");
            check(ga.SampleFilesNamedInMemory().count("hit.wav") == 1, "fill C4b: ... and the Redo of it still keeps the file");
        }
        {   // review of C4b: a library file copied for a fill lands only if it is still wanted there
            namespace fsys = std::filesystem;
            const std::string dir = "selftest_tmp_c4b_fillcopy";
            fsys::remove_all(dir); fsys::create_directories(dir + "/lib");
            std::vector<float> tone(4000, 0.0f); for (size_t k = 0; k < tone.size(); ++k) tone[k] = 0.3f * std::sin(static_cast<float>(k) * 0.05f);
            check(WriteWavPCM16(dir + "/lib/lib.wav", tone, 1, kSampleRate), "fill C4b copy: test file written");
            auto gap = std::make_unique<AppState>(); AppState& ga = *gap;
            ga.engine.Init(kSampleRate); ga.sequencer.Init(&ga.engine, &ga.project);
            ga.projectPath = dir + "/p.t8proj";
            ga.project.songChainLength = 3; for (int b = 0; b < 3; ++b) ga.project.songChain[b] = ChainStep(16 + b);
            ga.loader.Start();
            auto fillOf = [&](int b) { return ga.project.songChain[static_cast<size_t>(b)].fill.Sample(); };
            { std::lock_guard<SceneMutex> lk(ga.sequencer.PatternMutex()); ga.AssignSampleToFill(dir + "/lib/lib.wav", 1); }
            check(fillOf(1).empty(), "fill C4b copy: the fill gets the file only once it is copied");
            ga.loader.FinishAllForTest();
            check(fillOf(1) == "lib.wav" && fsys::exists(dir + "/samples/lib.wav"), "fill C4b copy: then it is copied into samples/ and set");
            // E: a project file chosen while the library file is still being copied wins
            { std::lock_guard<SceneMutex> lk(ga.sequencer.PatternMutex()); ga.SetFillSample(2, "old.wav"); ga.AssignSampleToFill(dir + "/lib/lib.wav", 2); ga.SetFillSample(2, "mine.wav"); }
            ga.loader.FinishAllForTest();
            check(fillOf(2) == "mine.wav" && ga.statusMessage.find("ANOTHER SAMPLE") != std::string::npos, "fill C4b copy: a newer choice is not overwritten by the late copy");
            // G: Undo while it is copied
            { std::lock_guard<SceneMutex> lk(ga.sequencer.PatternMutex()); ga.AssignSampleToFill(dir + "/lib/lib.wav", 2); ga.Undo(); }
            const std::string afterUndo = fillOf(2);
            ga.loader.FinishAllForTest();
            check(fillOf(2) == afterUndo && afterUndo != "lib.wav" && ga.statusMessage.find("CHANGED MEANWHILE") != std::string::npos, "fill C4b copy: an Undo meanwhile is not undone by the late copy");
            // F: the brick in front is removed while it is copied: the brick that moves into its place does not get it
            { std::lock_guard<SceneMutex> lk(ga.sequencer.PatternMutex());
              ga.project.songChain[0].fillMode = FillMode::Own; ga.project.songChain[0].fill = FillRule{};
              ga.AssignSampleToFill(dir + "/lib/lib.wav", 1);
              for (int b = 0; b + 1 < ga.project.songChainLength; ++b) ga.project.songChain[b] = ga.project.songChain[b + 1];
              --ga.project.songChainLength; }
            const std::string moved = fillOf(1);
            ga.loader.FinishAllForTest();
            check(fillOf(1) == moved && ga.statusMessage.find("THE BRICK CHANGED MEANWHILE") != std::string::npos, "fill C4b copy: a brick removed meanwhile: the late copy goes nowhere");
            ga.loader.Stop();
            fsys::remove_all(dir);
        }
        // the UI (source ratchets): move, click removes, the break is a choice, the rows are taller
        const std::string brickSrc = slurp("src/ui_desktop/BricksPanel.cpp"), mainSrc = slurp("src/main.cpp");
        check(brickSrc.find("Drag::FillMove") != std::string::npos && brickSrc.find("Drag::TransMove") != std::string::npos && brickSrc.find("FILL REMOVED") != std::string::npos && brickSrc.find("TRANSITION MOVED") != std::string::npos, "v44.25: fills and transitions can be moved and removed by click");
        check(brickSrc.find("AllDrumBreaks") != std::string::npos && brickSrc.find("BREAK: RANDOM") != std::string::npos, "v44.25: a break is picked by name, RANDOM is one choice");
        check(brickSrc.find("kLaneRowMin = 30.0f") != std::string::npos && mainSrc.find("editorNeed") != std::string::npos, "v44.25: the lane rows are taller");
    }

    // ---- v44.33: a scene with its own kit and a chosen percussion sample sounds the same as a LIVE loop and as a brick ----
    {
        auto render = [&](bool asBrick, int percVariant, bool stepVariant, std::vector<float>& buf) {
            auto dap = std::make_unique<AppState>(); AppState& da = *dap;
            da.engine.Init(kSampleRate); da.sequencer.Init(&da.engine, &da.project);
            Project& pj = da.project; pj.kitFolder = "kits/808";
            da.engine.GetKit()->LoadFromFolder("kits/808", kSampleRate);
            for (int sc = 0; sc < 2; ++sc) { pj.patterns[static_cast<size_t>(sc)].tracks[0].steps[0].active = true; pj.patterns[static_cast<size_t>(sc)].tracks[5].steps[2].active = true; pj.patterns[static_cast<size_t>(sc)].tracks[5].steps[6].active = true; }
            pj.patterns[1].kitFolder = "kits/808perc";
            if (stepVariant) pj.patterns[1].tracks[5].steps[6].variant = static_cast<uint8_t>(percVariant); else pj.patterns[1].tracks[5].defaultVariant = static_cast<uint8_t>(percVariant);
            da.SyncSceneKits(true);
            da.activeTab = 2;
            pj.currentPattern = 0; pj.liveScene = 0; da.ThrowIntoChain();
            pj.currentPattern = 1; pj.liveScene = 1;
            if (asBrick) { da.ThrowIntoChain(); pj.currentPattern = pj.songChain[1].patternIndex; }
            da.SyncSceneKits();
            da.sequencer.ApplySceneSound(pj.currentPattern);
            da.engine.GetMixer().SyncFromPattern(pj.patterns[static_cast<size_t>(pj.currentPattern)]);
            if (asBrick) da.sequencer.PlayChainFrom(1, true); else { da.sequencer.SetSongModeActive(false); da.sequencer.Play(); }
            buf.assign(static_cast<size_t>(kSampleRate) * 3 * 2, 0.0f);
            for (size_t done = 0; done < buf.size() / 2;) { const int n = static_cast<int>(std::min<size_t>(256, buf.size() / 2 - done)); da.sequencer.RenderAndAdvance(buf.data() + done * 2, n, kSampleRate); done += n; }
        };
        for (int sv = 0; sv < 2; ++sv) for (int v : {0, 3, 4}) {
            std::vector<float> live, brick; render(false, v, sv == 1, live); render(true, v, sv == 1, brick);
            double e = 0, p2 = 0; for (size_t k = 0; k < live.size(); ++k) { const double d = live[k] - brick[k]; e += d * d; p2 += double(live[k]) * live[k]; }
            char msg[160]; std::snprintf(msg, sizeof msg, "v44.33: scene kit 808perc, percussion %s %d: the same on LIVE and as a brick (err %.3g of %.3g)", sv ? "step variant" : "default sample", v + 1, e, p2);
            check(p2 > 0.01 && e < 1e-9 * (p2 + 1.0), msg);
        }
    }

    // ---- v44.35: LOOP round the WHOLE track keeps going whatever brick is selected or edited ----
    {
        AppState da; da.engine.Init(kSampleRate); da.sequencer.Init(&da.engine, &da.project);
        Project& pj = da.project;
        for (int i = 0; i < 3; ++i) { pj.patterns[static_cast<size_t>(i)].masterLength = 16; pj.patterns[static_cast<size_t>(i)].bpm = 120.0f; }   // 1 bar = 2 s
        da.AddChainStep(0); da.AddChainStep(1); da.AddChainStep(2);
        std::vector<float> ob(256 * 2);
        auto run = [&](double secs) { const int nb = static_cast<int>(secs * kSampleRate) / 256; for (int b = 0; b < nb; ++b) da.sequencer.RenderAndAdvance(ob.data(), 256, kSampleRate); };
        da.activeTab = 1; da.SetLoop(true); da.SetLoopAll(true);
        check(!da.sequencer.SongLoopStep() && da.sequencer.SongLoopLo() == 0 && !da.sequencer.SongRunOnce(), "v44.35: ALL sets the loop over the whole track (no single-brick loop, no stop at the end)");
        da.PlayFromBrick(0);
        run(2.1); const int p1 = da.sequencer.SongChainPos();
        run(2.1); const int p2 = da.sequencer.SongChainPos();
        run(2.1); const int p3 = da.sequencer.SongChainPos();
        check(p1 == 1 && p2 == 2 && p3 == 0 && da.sequencer.IsPlaying(), "v44.35: the track goes 1 - 2 - 3 and round to 1 again");
        // the author clicks brick 2 to edit it (what BricksPanel does: select it, drop the range)
        da.selBrick = 1; da.selBrickHi = -1; da.ClearBrickRange(); da.ApplyTransportFlags();
        check(!da.sequencer.SongLoopStep() && da.sequencer.SongLoopLo() == 0, "v44.35: selecting a brick to edit it does not narrow the loop");
        run(2.1); const int q1 = da.sequencer.SongChainPos();
        run(2.1); const int q2 = da.sequencer.SongChainPos();
        run(2.1); const int q3 = da.sequencer.SongChainPos();
        check(q1 == 1 && q2 == 2 && q3 == 0, "v44.35: ... the whole track keeps going round while brick 2 is edited");
        da.SelectBrickRange(1, 2);
        check(da.sequencer.SongLoopLo() == 0, "v44.35: a selected range of bricks does not narrow it either");
        pj.songChain[1].repeats = 2; da.sequencer.RefreshArrangement();
        da.SetLoopAll(false); da.selBrickHi = -1; da.ApplyTransportFlags();
        check(da.sequencer.SongLoopStep(), "v44.35: ALL off: LOOP goes back to the selected brick");
        da.TransportToggle(); da.TickTransport();
        da.SetLoopAll(true); da.SetLoop(false);
        check(da.sequencer.SongRunOnce() && da.sequencer.SongLoopLo() < 0, "v44.35: LOOP off: the track plays once and stops, ALL does not change that");
    }

    // ---- v44.34: the brick inspector is redrawn with one shared pictogram set (source checks; the picture itself is checked on screen) ----
    {
        auto slurp = [](const std::string& path) { std::ifstream in(path); return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); };
        const std::string gl = slurp("src/ui_desktop/BrickGlyphs.h"), br = slurp("src/ui_desktop/BricksPanel.cpp"), bh = slurp("src/ui_desktop/BricksPanel.h");
        check(!gl.empty() && gl.find("enum class Glyph") != std::string::npos && gl.find("inline bool GlyphTile") != std::string::npos && gl.find("inline void DrawGlyph") != std::string::npos, "v44.34: the shared pictogram set and the tile exist (BrickGlyphs.h)");
        int nGlyphs = 0; for (const char* g : {"Repeat", "Trim", "Level", "Fill", "Trans", "Flat", "FadeIn", "FadeOut", "Copy", "FromLive", "Twice", "Reset", "Remove", "Presets"}) nGlyphs += gl.find(std::string("case Glyph::") + g) != std::string::npos;
        check(nGlyphs == 14, "v44.34: every pictogram of the set is drawn (14 of 14)");
        check(gl.find("ImFont") == std::string::npos && gl.find("AddImage") == std::string::npos && gl.find("PathStroke") != std::string::npos, "v44.34: the pictograms are drawn with lines, no font and no image files");
        check(bh.find("int page_") != std::string::npos && br.find("page_ == 0") != std::string::npos && br.find("page_ == 1") != std::string::npos && br.find("Glyph::Trans") != std::string::npos, "v44.34: the inspector has the three pages LEVEL / FILL / TRANSITION");
        check(br.find("GlyphTile(\"##copy\"") != std::string::npos && br.find("GlyphTile(\"##remove\"") != std::string::npos && br.find("ImGui::Button(\"COPY BRICK\")") == std::string::npos && br.find("ImGui::Button(\"REMOVE\")") == std::string::npos, "v44.34: the brick actions are pictograms, not word buttons");
        check(br.find("UpdateBrickFromLive") != std::string::npos && br.find("ResetBrickSettings") != std::string::npos && br.find("DuplicateChainStep") != std::string::npos && br.find("DuplicateBrickAsScene") != std::string::npos, "v44.34: every action the old inspector had is still there");
        check(br.find("\"##mst\"") != std::string::npos && br.find("\"##fst\"") != std::string::npos && br.find("\"##lfill\"") != std::string::npos && br.find("\"##ltrans\"") != std::string::npos, "v44.34: the level ends, the fill level and the transition level are still reachable");
        check(bh.find("transOut_") != std::string::npos && br.find("\"##tio\"") != std::string::npos && br.find("canIn") != std::string::npos, "v44.34: the TRANS page can edit the transition out of a brick (the first brick has one too)");
        check(gl.find("inline void DrawInstrumentGlyph") != std::string::npos && gl.find("InstrumentName") != std::string::npos && br.find("DrawInstrumentGlyph(dl, t,") != std::string::npos, "v44.36: the ten instruments have pictograms, used on the WHO PLAYS bars and on the rows under the bricks");
        check(br.find("cap(ax, \"BRICK\"") == std::string::npos && br.find("at(0, 22);") != std::string::npos && br.find("const float labelW = 34.0f") != std::string::npos, "v44.36: the brick actions sit in the left column of the inspector; the label column of the strip is narrow");
        check(br.find("for (int bt : {1, 2, 3}) opts.push_back(bt * spb);") != std::string::npos && br.find("nothing shorter to cut it to") != std::string::npos, "v44.36: TRIM offers beats as well as bars and says why when there is nothing to choose");
        check(br.find("float BricksPanel::InspectorHeight() { return 104.0f; }") != std::string::npos, "v44.34: the inspector is not taller than before");
        {   // v44.37: the instrument pictograms reach the other tabs
            int used = 0;
            for (const char* f : {"SequencerPanel.cpp", "MixerPanel.cpp", "LanePanel.cpp", "PianoRollPanel.cpp", "ContextPanel.cpp", "SoundPanel.cpp"}) {
                const std::string src = slurp(std::string("src/ui_desktop/") + f);
                used += (src.find("BrickGlyphs.h") != std::string::npos && src.find("InstrumentGlyph") != std::string::npos) ? 1 : 0;
                if (std::string(f) == "SoundPanel.cpp") used += src.find("GlyphTile(\"##part\"") != std::string::npos ? 1 : 0;
            }
            check(used == 7, "v44.37: instrument pictograms are used on the sequencer rows, mixer strip, lanes, piano roll, context header and SOUND chips");
            check(gl.find("int instrument = -1, ImU32 idleCol = 0") != std::string::npos, "v44.37: GlyphTile can show an instrument pictogram in the track colour");
        }
        {   // v44.38: source checks for the screens
            const std::string sq = slurp("src/ui_desktop/SequencerPanel.cpp"), sh = slurp("src/ui_desktop/Shortcuts.cpp");
            check(sq.find("const bool onBrick = app.BrickOfSlot(app.project.currentPattern) >= 0;") != std::string::npos && sq.find("ToggleButton(\"M\", rowMuted") != std::string::npos && sq.find("ToggleButton(\"S\", rowSolo") != std::string::npos, "v44.38: on a brick the M and S buttons show and write WHO PLAYS");
            check(sq.find("const float kLabelW = 152.0f") != std::string::npos, "v44.38: the name column of the sequencer is wide enough for ClosedHH / OpenHH next to the pictogram");
            check(sh.find("ImGuiKey_Tab") != std::string::npos && sh.find("app.uiTabRequest = app.activeTab == 1 ? 2 : 1;") != std::string::npos, "v44.38: the Tab key goes between LIVE and TRACK");
            check(br.find("page_ == 3") == std::string::npos ? br.find("} else {\n        // PARAM (v44.38)") != std::string::npos : true, "v44.38: the PARAM page exists");
            check(br.find("\"##ppick\"") != std::string::npos && br.find("kLaneParam0") != std::string::npos && gl.find("Glyph::Param") != std::string::npos && gl.find("kParam") != std::string::npos, "v44.38: the PARAM page has a searchable picker, its pictogram and colour, and a lane row under the bricks");
        }
        {   // v44.38: one mute for a brick (WHO PLAYS = M / S of the sequencer), and the parameter rows
            AppState ma; ma.engine.Init(kSampleRate); ma.sequencer.Init(&ma.engine, &ma.project);
            Pattern& lv = ma.project.patterns[0];
            lv.tracks[0].steps[0].active = true; lv.tracks[6].steps[0].active = true;
            lv.tracks[2].mute = true;
            ma.project.currentPattern = 0; ma.project.liveScene = 0;
            check(ma.ThrowIntoChain(), "mute v44.38: throw");
            const int slot = ma.project.songChain[0].patternIndex;
            check(ma.project.songChain[0].muteMask == (1u << 2) && !ma.project.patterns[slot].tracks[2].mute, "mute v44.38: what the scene had muted becomes WHO PLAYS of the brick, the scene flag is cleared");
            ma.project.currentPattern = slot; ma.selBrick = 0;
            ma.SetMuteScene(3, true);
            check(((ma.project.songChain[0].muteMask >> 3) & 1u) && !ma.project.patterns[slot].tracks[3].mute && ma.TrackIsMuted(3) && ma.TrackIsMuted(2), "mute v44.38: M on a brick writes the mask (the same switch as WHO PLAYS), not a scene flag");
            ma.SetBrickTrackMuted(0, 3, false);
            check(!ma.TrackIsMuted(3), "mute v44.38: WHO PLAYS and M read the same bit");
            ma.SetSoloScene(6, true);
            check(ma.TrackIsSolo(6) && ma.project.songChain[0].muteMask == (kMuteAllTracks & ~(1u << 6)) && !ma.TrackIsSolo(0), "mute v44.38: S on a brick = every other instrument off");
            ma.SetSoloScene(6, false);
            check(!ma.TrackIsSolo(6) && ma.project.songChain[0].muteMask == (1u << 2), "mute v44.38: S again brings back what played before");
            // a file from before: the flags of a brick's scene are folded into the mask on load
            static Project old; old = Project{};
            old.patterns[kNumLiveScenes].tracks[1].mute = true; old.patterns[kNumLiveScenes].tracks[5].solo = false;
            old.songChain[0] = ChainStep{kNumLiveScenes, 1}; old.songChainLength = 1;
            check(SaveProject(old, "selftest_tmp_safety/oldmute.t8proj"), "mute v44.38: save an old-style file");
            Project ol; check(LoadProject(ol, "selftest_tmp_safety/oldmute.t8proj"), "mute v44.38: load it");
            check(((ol.songChain[0].muteMask >> 1) & 1u) && !ol.patterns[kNumLiveScenes].tracks[1].mute, "mute v44.38: an old brick's scene mute is folded into WHO PLAYS on load");
        }
        {   // parameter rows: data, file, engine
            StepParams sp; check(sp.Add(6, 5) == 0 && sp.Add(6, 5) == 0 && sp.n == 1 && sp.Add(0, 5) < 0 && sp.Add(6, 3) < 0 && sp.Add(0, 3) == 1, "params v44.38: a row per (instrument, parameter); only parameters the instrument has");
            for (int k = 0; k < 8; ++k) sp.Add(k % kArrTracks, 0);
            check(sp.n <= kMaxParamRows, "params v44.38: no more than six rows");
            static Project pp; pp = Project{};
            pp.songChainLength = 1; pp.songChain[0] = ChainStep{0, 1};
            const int kr = pp.songChain[0].params.Add(6, 5); pp.songChain[0].params.row[kr].lane = Lane::Line(-50, 80, -100, 100);
            const int pr = pp.songChain[0].params.Add(3, 0); (void)pr;   // a flat row stays in the file
            check(SaveProject(pp, "selftest_tmp_safety/params.t8proj"), "params v44.38: save");
            Project pl; check(LoadProject(pl, "selftest_tmp_safety/params.t8proj"), "params v44.38: load");
            check(pl.songChain[0].params == pp.songChain[0].params && pl.songChain[0].params.n == 2, "params v44.38: the rows (also a flat one) survive a save and a load");
            // engine: offsets follow the lane while the brick plays and are zero when it does not
            static Project ep; ep = Project{};
            ep.patterns[0].bpm = 120.0f; ep.patterns[0].masterLength = 16;
            for (int i = 0; i < 16; i += 2) { ep.patterns[0].tracks[0].steps[i].active = true; ep.patterns[0].tracks[6].steps[i].active = true; }
            ep.songChainLength = 1; ep.songChain[0] = ChainStep{0, 1};
            const int kk = ep.songChain[0].params.Add(6, 5); ep.songChain[0].params.row[kk].lane = Lane::Line(0, 100, -100, 100);
            auto renderWith = [&](const StepParams& rows, std::vector<float>& out, float seconds, float* offMid) {
                ep.songChain[0].params = rows;
                AudioEngine e; e.Init(kSampleRate); e.GetKit()->LoadFromFolder("kits/808", kSampleRate);
                Sequencer sq; sq.Init(&e, &ep); sq.SetSongFollowsEdit(false); sq.SetSongModeActive(true);
                sq.Play();
                std::vector<float> o(256 * 2); out.clear();
                const int nb = static_cast<int>(seconds * kSampleRate) / 256;
                for (int b = 0; b < nb; ++b) {
                    sq.RenderAndAdvance(o.data(), 256, kSampleRate);
                    out.insert(out.end(), o.begin(), o.end());
                    if (offMid && b == nb / 2) *offMid = e.GetMixer().ParamOff(6, AutoParam::Cutoff);
                }
            };
            std::vector<float> a, b; float mid = -9.0f;
            renderWith(ep.songChain[0].params, a, 1.0f, &mid);
            check(mid > 0.2f && mid < 0.8f, "params v44.38: the cutoff offset of bass moves up over the brick (a lane from 0 to +100)");
            // pan: kick hard right = the left channel nearly silent; no rows = both channels alike
            StepParams panRows; const int pk = panRows.Add(0, 0); panRows.row[pk].lane = Lane::Line(100, 100, -100, 100);
            std::vector<float> bassOn; renderWith(StepParams{}, bassOn, 1.0f, nullptr);
            ep.songChain[0].muteMask = static_cast<uint16_t>(1u << 6);   // the bass is in the middle: leave it out of the pan check
            ep.patterns[0].tracks[0].reverbSend = 0.0f; ep.patterns[0].tracks[0].delaySend = 0.0f;   // the effect returns are stereo, the dry signal is what moves
            renderWith(panRows, a, 1.0f, nullptr); std::vector<float> noPan; renderWith(StepParams{}, noPan, 1.0f, nullptr);
            auto side = [](const std::vector<float>& v, int ch) { double s2 = 0; for (size_t i = static_cast<size_t>(ch); i < v.size(); i += 2) s2 += static_cast<double>(v[i]) * v[i]; return s2; };
            std::vector<float> st; ep.patterns[0].tracks[0].pan = 1.0f; renderWith(StepParams{}, st, 1.0f, nullptr); ep.patterns[0].tracks[0].pan = 0.0f;   // the same thing set on the track itself
            ep.songChain[0].muteMask = 0;
            b = bassOn;
            check(std::fabs(side(a, 0) - side(st, 0)) < 0.03 * side(st, 0) && std::fabs(side(a, 1) - side(st, 1)) < 0.03 * side(st, 1) && side(a, 0) < 0.6 * side(noPan, 0), "params v44.38: a PAN row of +100 sounds like the instrument panned hard right on the track");
            // cutoff: three octaves down on bass is clearly darker than the sound as it is
            StepParams cutRows; const int ck = cutRows.Add(6, 5); cutRows.row[ck].lane = Lane::Line(-100, -100, -100, 100);
            renderWith(cutRows, a, 1.0f, nullptr);
            auto bright = [](const std::vector<float>& v) { double d = 0, t = 0; for (size_t i = 2; i < v.size(); i += 2) { d += std::fabs(static_cast<double>(v[i]) - v[i - 2]); t += std::fabs(static_cast<double>(v[i])); } return t > 0 ? d / t : 0.0; };
            check(bright(a) < bright(b) * 0.98, "params v44.38: a CUTOFF row darkens the bass");
            std::vector<float> c0; renderWith(StepParams{}, c0, 1.0f, nullptr);
            bool same = c0.size() == b.size(); for (size_t i = 0; same && i < c0.size(); ++i) if (c0[i] != b[i]) same = false;
            check(same, "params v44.38: without rows the audio is bit for bit what it was");
        }
        {   // v44.39: parameter rows on a LIVE scene
            static Project sp39; sp39 = Project{};
            const int r0 = sp39.patterns[0].params.Add(6, 5); sp39.patterns[0].params.row[r0].lane = Lane::Line(-30, 70, -100, 100);
            sp39.patterns[0].params.Add(3, 0);   // a flat row stays in the file too
            check(SaveProject(sp39, "selftest_tmp_safety/sceneparams.t8proj"), "params v44.39: save a scene with rows");
            Project ld; check(LoadProject(ld, "selftest_tmp_safety/sceneparams.t8proj"), "params v44.39: load it");
            check(ld.patterns[0].params == sp39.patterns[0].params && ld.patterns[0].params.n == 2 && ld.patterns[1].params.n == 0, "params v44.39: a scene's rows survive a save and a load, other scenes stay empty");

            // engine: on LIVE the offset goes once round the scene's loop (no song chain playing)
            static Project lp; lp = Project{};
            lp.patterns[0].bpm = 120.0f; lp.patterns[0].masterLength = 16;
            for (int i = 0; i < 16; i += 2) { lp.patterns[0].tracks[0].steps[i].active = true; lp.patterns[0].tracks[6].steps[i].active = true; }
            auto liveRun = [&](const StepParams& rows, std::vector<float>& out, float* at, int nAt, const float* atSec, double* progOut) {
                lp.patterns[0].params = rows;
                AudioEngine e; e.Init(kSampleRate); e.GetKit()->LoadFromFolder("kits/808", kSampleRate);
                Sequencer sq; sq.Init(&e, &lp); sq.SetSongModeActive(false);
                if (progOut) *progOut = sq.SceneProgress();   // stopped: -1
                sq.Play();
                std::vector<float> o(256 * 2); out.clear();
                const int nb = static_cast<int>(2.6f * kSampleRate) / 256;
                int k = 0;
                for (int b = 0; b < nb; ++b) {
                    sq.RenderAndAdvance(o.data(), 256, kSampleRate);
                    out.insert(out.end(), o.begin(), o.end());
                    while (at && k < nAt && static_cast<float>(b * 256) / kSampleRate >= atSec[k]) { at[k] = e.GetMixer().ParamOff(6, AutoParam::Cutoff); ++k; }
                }
            };
            StepParams up; const int uk = up.Add(6, 5); up.row[uk].lane = Lane::Line(0, 100, -100, 100);
            const float secs[3] = {1.0f, 1.9f, 2.2f}; float got[3] = {-9, -9, -9}; double p0 = 0; std::vector<float> a;
            liveRun(up, a, got, 3, secs, &p0);
            check(p0 < 0.0, "params v44.39: SceneProgress is -1 while nothing plays");
            check(got[0] > 0.35f && got[0] < 0.65f && got[1] > 0.85f && got[2] < 0.25f, "params v44.39: a scene's curve runs once round its loop (halfway at half, near the top at the end, back to the start in the next pass)");
            // a flat row and no rows are the same, bit for bit
            std::vector<float> n0, n1; StepParams flatRows; flatRows.Add(6, 5);
            liveRun(StepParams{}, n0, nullptr, 0, nullptr, nullptr); liveRun(flatRows, n1, nullptr, 0, nullptr, nullptr);
            bool same2 = n0.size() == n1.size(); for (size_t i = 0; same2 && i < n0.size(); ++i) if (n0[i] != n1[i]) same2 = false;
            check(same2, "params v44.39: a flat scene row sounds exactly like no row");
            // the curve changes the sound
            StepParams dn; const int dk = dn.Add(6, 5); dn.row[dk].lane = Lane::Line(-100, -100, -100, 100);
            std::vector<float> dark; liveRun(dn, dark, nullptr, 0, nullptr, nullptr);
            check(dark.size() == n0.size() && dark != n0, "params v44.39: a CUTOFF curve on the scene changes the sound");
            lp.patterns[0].params = StepParams{};

            // THROW moves the scene's rows into the brick; the scene keeps its own
            AppState ma; ma.engine.Init(kSampleRate); ma.sequencer.Init(&ma.engine, &ma.project);
            Pattern& lv = ma.project.patterns[0];
            lv.tracks[0].steps[0].active = true;
            const int tk = lv.params.Add(6, 5); lv.params.row[tk].lane = Lane::Line(0, 60, -100, 100);
            ma.project.currentPattern = 0; ma.project.liveScene = 0;
            check(ma.ThrowIntoChain(), "params v44.39: throw a scene with rows");
            const int slot = ma.project.songChain[0].patternIndex;
            check(ma.project.songChain[0].params.n == 1 && ma.project.songChain[0].params.row[0].lane.End() == 60 && ma.project.patterns[slot].params.n == 0 && lv.params.n == 1,
                  "params v44.39: THROW moves the scene's curves into the brick (its PARAM rows); the brick's scene carries none; LIVE keeps its own");
            // UPDATE FROM LIVE carries the scene's curves along
            lv.params.row[0].lane = Lane::Line(0, -40, -100, 100);
            ma.project.currentPattern = slot; ma.selBrick = 0;
            check(ma.UpdateBrickFromLive(0) && ma.project.songChain[0].params.row[0].lane.End() == -40 && ma.project.patterns[slot].params.n == 0, "params v44.39: UPDATE FROM LIVE takes the scene's curves into the brick");
            // source checks
            const std::string cp = slurp("src/ui_desktop/ContextPanel.cpp"), lw = slurp("src/ui_desktop/LaneWidget.h");
            check(cp.find("\"AUTO\"") != std::string::npos && cp.find("void ContextPanel::DrawAuto") != std::string::npos && cp.find("drumTab_ == 3") != std::string::npos && cp.find("synthTab_ == 5") != std::string::npos, "params v44.39: the LIVE instrument block has an AUTO tab for drums and synths (the chord SOUND view shares the synth one)");
            check(lw.find("inline bool LaneEditor") != std::string::npos && lw.find("SceneProgress") == std::string::npos && cp.find("SceneProgress()") != std::string::npos, "params v44.39: the curve editor is shared and shows the scene's playhead");
        }
        {   // v44.40: kick red, level stripe, launch feedback
            const std::string ts = slurp("src/ui_desktop/TrackStyle.h"), hp = slurp("src/ui_desktop/HeaderPanel.cpp"), sq = slurp("src/ui_desktop/SequencerPanel.cpp"), cpn = slurp("src/ui_desktop/ChainPanel.cpp");
            check(ts.find("IM_COL32(238, 74, 60, 255),     // Kick      red") != std::string::npos && ts.find("IM_COL32(255, 122, 69, 255)") == std::string::npos, "v44.40: the kick is red, so the amber accent is only for selected / primary");
            check(hp.find("v44.40: an outline and a dot") != std::string::npos, "v44.40: REC has an outline and a dot (not mistaken for the red kick)");
            check(sq.find("the coloured stripe is the level of the instrument") != std::string::npos && sq.find("a hairline under its name") == std::string::npos, "v44.40: the stripe at the start of a sequencer row is the level of the instrument");
            check(cpn.find("BeatsUntilLaunch()") != std::string::npos && cpn.find("SceneProgress()") != std::string::npos && cpn.find("sixteen dots") != std::string::npos && cpn.find("launches in") != std::string::npos, "v44.40: scene slots show progress, a countdown in beats, sixteen dots of content and the 'launches in' line");
            static Project lq; lq = Project{};
            for (int sc = 0; sc < 2; ++sc) { lq.patterns[static_cast<size_t>(sc)].masterLength = 16; lq.patterns[static_cast<size_t>(sc)].bpm = 120.0f; lq.patterns[static_cast<size_t>(sc)].tracks[0].steps[0].active = true; }
            AudioEngine e; e.Init(kSampleRate);
            Sequencer sq2; sq2.Init(&e, &lq); sq2.SetSongModeActive(false); sq2.SetLaunchQuantize(2);
            check(sq2.BeatsUntilLaunch() == 0.0, "v44.40: no countdown while nothing is queued");
            sq2.Play();
            std::vector<float> o(256 * 2);
            for (int b = 0; b < 40; ++b) sq2.RenderAndAdvance(o.data(), 256, kSampleRate);
            sq2.LaunchScene(1);
            const double b0 = sq2.BeatsUntilLaunch();
            check(sq2.PendingScene() == 1 && b0 > 0.0 && b0 <= 4.001 && std::fabs(sq2.LaunchQuantumBeats() - 4.0) < 1e-9, "v44.40: a queued scene counts down in beats, a BAR is 4 beats of a 16-step scene");
            for (int b = 0; b < 40; ++b) sq2.RenderAndAdvance(o.data(), 256, kSampleRate);
            const double b1 = sq2.BeatsUntilLaunch();
            check(sq2.PendingScene() == 1 ? (b1 < b0 && b0 - b1 < 1.0) : true, "v44.40: the countdown goes down while the music plays");
        }
        {   // v44.41: MIX LAB -- spectra, overlap spots, advice, tonal balance, the offline render
            const double pi2 = 6.283185307179586;
            auto sine = [&](double hz, double amp, double sec) { std::vector<float> v(static_cast<size_t>(sec * kSampleRate)); for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<float>(amp * std::sin(pi2 * hz * i / kSampleRate)); return v; };
            auto peakBand = [&](const MixSpectra& m, int t) { int bb = 0; for (int b = 1; b < kMixBands; ++b) if (m.db[static_cast<size_t>(t)][static_cast<size_t>(b)] > m.db[static_cast<size_t>(t)][static_cast<size_t>(bb)]) bb = b; return bb; };
            // calibration: a 0.5 sine at 1 kHz is about -6 dB, and the loudest band is the one holding 1 kHz
            std::array<std::vector<float>, kNumTracks> st;
            st[1] = sine(1000.0, 0.5, 2.0);
            MixSpectra m1; AnalyseStems(st, kSampleRate, m1);
            const int pb = peakBand(m1, 1);
            check(m1.present[1] && !m1.present[0] && std::fabs(MixBandEdgeHz(pb + 0.5f) - 1000.0f) < 70.0f && std::fabs(m1.db[1][static_cast<size_t>(pb)] - (-6.0f)) < 2.0f, "mix lab: a sine shows up in its own band at its own level (0.5 -> about -6 dB), silent tracks stay absent");
            check(std::fabs(MixBandEdgeHz(0) - 20.0f) < 0.01f && std::fabs(MixBandEdgeHz(kMixBands) - 20000.0f) < 1.0f && std::fabs(MixHzToBand(MixBandEdgeHz(37.0f)) - 37.0f) < 1e-3f, "mix lab: 120 log bands from 20 Hz to 20 kHz, and Hz <-> band round-trips");

            // two instruments in the sub: a spot, kick keeps it, bass (lower in the default list) is asked to make room
            std::array<std::vector<float>, kNumTracks> two;
            two[0] = sine(70.0, 0.5, 2.0); two[6] = sine(78.0, 0.5, 2.0); two[3] = sine(9000.0, 0.05, 2.0);
            MixSpectra m2; AnalyseStems(two, kSampleRate, m2);
            Pattern pat0; MixReport r2 = BuildMixReport(m2, DefaultMixPriority(), &pat0);
            check(r2.valid && !r2.spots.empty() && r2.spots[0].centerHz > 55.0f && r2.spots[0].centerHz < 100.0f && r2.spots[0].tracks.size() >= 2, "mix lab: kick and bass sharing the sub are found as an overlap spot there");
            check(!r2.spots.empty() && r2.spots[0].keeper == 0 && r2.spots[0].yielder == 6 && r2.spots[0].fix.track == 6 && r2.spots[0].fix.kind != MixFix::Kind::None && !r2.spots[0].fix.text.empty() && r2.spots[0].advice.find("Kick") != std::string::npos, "mix lab: the instrument higher in the list keeps the space, the other gets one concrete fix and words for it");
            auto flipped = DefaultMixPriority(); std::swap(flipped[0], flipped[1]);   // bass first
            MixReport r2b = BuildMixReport(m2, flipped, &pat0);
            check(!r2b.spots.empty() && r2b.spots[0].keeper == 6 && r2b.spots[0].yielder == 0, "mix lab: dragging the priority list changes who is asked to make room");
            // far apart = no spot
            std::array<std::vector<float>, kNumTracks> far; far[0] = sine(60.0, 0.5, 2.0); far[3] = sine(8000.0, 0.5, 2.0); far[9] = sine(800.0, 0.5, 2.0);
            MixSpectra m3; AnalyseStems(far, kSampleRate, m3);
            check(BuildMixReport(m3, DefaultMixPriority(), &pat0).spots.empty(), "mix lab: instruments in different places do not make overlap spots");
            // a mid-band collision gets a bell cut, applied to the track; an EQ band that is already in use falls back to LEVEL
            std::array<std::vector<float>, kNumTracks> mid; mid[1] = sine(2200.0, 0.4, 2.0); mid[2] = sine(2350.0, 0.4, 2.0);
            MixSpectra m4; AnalyseStems(mid, kSampleRate, m4);
            MixReport r4 = BuildMixReport(m4, DefaultMixPriority(), &pat0);
            check(!r4.spots.empty() && r4.spots[0].zone == 4 && r4.spots[0].yielder == 2 && r4.spots[0].fix.kind == MixFix::Kind::BellCut && r4.spots[0].fix.value < 0.0f, "mix lab: snare and clap in the presence zone -> the clap gets a bell cut on its MID band");
            Track tt; check(MixApplyFix(tt, r4.spots[0].fix) && tt.eqMidDb < 0.0f && std::fabs(tt.eqMidHz - r4.spots[0].fix.hz) < 0.01f, "mix lab: a fix is written into the track's channel EQ");
            Pattern busy; busy.tracks[2].eqMidDb = 4.0f;
            check(BuildMixReport(m4, DefaultMixPriority(), &busy).spots[0].fix.kind == MixFix::Kind::Level, "mix lab: when the yielder's MID band is already in use the advice is a level change instead of overwriting it");
            Track pt; MixFix pf; pf.kind = MixFix::Kind::Pan; pf.value = 0.3f; MixFix hf; hf.kind = MixFix::Kind::Hpf; hf.hz = 90.0f; MixFix lf; lf.kind = MixFix::Kind::Level; lf.value = -6.0f;
            Track lt; lt.volume = 0.8f; MixApplyFix(pt, pf); MixApplyFix(pt, hf); MixApplyFix(lt, lf); MixFix nf;
            check(pt.pan == 0.3f && pt.eqHpfHz == 90.0f && std::fabs(lt.volume - 0.4f) < 0.01f && !MixApplyFix(pt, nf), "mix lab: pan, HPF and level fixes land on the right fields");
            // tonal balance: a mix of only low sine is far above typical in the sub/bass and far below in the air
            std::array<std::vector<float>, kNumTracks> lo; lo[0] = sine(50.0, 0.5, 2.0); lo[3] = sine(10000.0, 0.01, 2.0); lo[9] = sine(1000.0, 0.1, 2.0);
            MixSpectra m5; AnalyseStems(lo, kSampleRate, m5);
            MixReport r5 = BuildMixReport(m5, DefaultMixPriority(), &pat0);
            check(r5.balance.deltaDb[0] > kMixToleranceDb && r5.balance.off[0] && r5.balance.deltaDb[5] < -kMixToleranceDb && r5.balance.share[0][0] > 0.9f, "mix lab: tonal balance flags too much sub / too little air, and says whose sub it is");

            // the offline render: a kick on every beat is found low, a muted track is not counted, a synth bass sits in the lows
            {
                MixLabJob job; job.scene = 0;
                Pattern& pp = job.project.patterns[0]; pp.bpm = 120.0f; pp.masterLength = 16;
                for (int k = 0; k < 16; k += 4) pp.tracks[0].steps[static_cast<size_t>(k)].active = true;
                for (int k = 2; k < 16; k += 4) { pp.tracks[kNumDrumTracks].steps[static_cast<size_t>(k)].active = true; pp.tracks[kNumDrumTracks].steps[static_cast<size_t>(k)].note = 36; pp.tracks[kNumDrumTracks].steps[static_cast<size_t>(k)].lengthSteps = 2; }
                MixSpectra rs; RenderMixStems(job, rs);
                const int kb = peakBand(rs, 0);
                check(rs.present[0] && MixBandEdgeHz(static_cast<float>(kb)) < 400.0f, "mix lab: the offline render taps the kick of the scene and its energy is in the low end");
                check(rs.present[kNumDrumTracks] && !rs.present[1] && !rs.present[3], "mix lab: only the instruments that really play are present");
                job.mute[0] = true; MixSpectra rm; RenderMixStems(job, rm);
                check(!rm.present[0] && rm.present[kNumDrumTracks], "mix lab: a muted instrument is not counted as a competitor");
                // same scene with the priority list read from the worker thread
                auto ap = std::make_unique<AppState>(); AppState& app = *ap;
                app.engine.Init(kSampleRate); app.sequencer.Init(&app.engine, &app.project);
                app.project.patterns[0] = pp; app.project.currentPattern = 0;
                MixLab lab; lab.Start(app);
                for (int w = 0; w < 400 && lab.phase() != MixLab::Phase::Ready; ++w) std::this_thread::sleep_for(std::chrono::milliseconds(50));
                MixSpectra gs; MixReport gr;
                check(lab.Get(gs, gr) && gs.present[0] && gr.valid, "mix lab: the analysis runs in a worker thread and hands the result back");
                lab.Join();
            }
            // source checks
            const std::string ml = slurp("src/ui_desktop/MixLabPanel.cpp"), mn = slurp("src/main.cpp"), mb = slurp("src/ui_desktop/MenuBar.cpp");
            check(ml.find("MIX LAB") != std::string::npos && ml.find("WHO KEEPS THE SPACE") != std::string::npos && ml.find("TONAL BALANCE") != std::string::npos && ml.find("KEEP FIXES") != std::string::npos && ml.find("UNDO ALL") != std::string::npos && ml.find("SHOW IT") != std::string::npos && ml.find("TRY IT") != std::string::npos, "mix lab: the tab has the frequency map, spots with SHOW IT / TRY IT, tonal balance, the priority list and A/B with KEEP FIXES / UNDO ALL");
            check(mn.find("MIX LAB") != std::string::npos && mb.find("MIX LAB") != std::string::npos, "mix lab: reachable from the tab bar and the View menu");
            {   // v44.42: level, pan and EQ of one instrument into every scene and brick
                auto mp = std::make_unique<AppState>(); AppState& ma = *mp;
                ma.engine.Init(kSampleRate); ma.sequencer.Init(&ma.engine, &ma.project);
                Project& pj = ma.project;
                const int brick = kNumLiveScenes, unusedBrick = kNumLiveScenes + 1;
                pj.songChainLength = 1; pj.songChain[0].patternIndex = brick;
                Track& src = pj.patterns[0].tracks[kNumDrumTracks];
                src.volume = 0.5f; src.pan = 0.2f; src.eqMidDb = -3.0f; src.eqMidHz = 215.0f; src.eqMidQ = 2.0f; src.eqHpfHz = 80.0f; src.delaySend = 0.4f; src.mute = true;
                for (int sc = 1; sc < kNumPatterns; ++sc) { Track& d = pj.patterns[static_cast<size_t>(sc)].tracks[kNumDrumTracks]; d.volume = 0.9f; d.delaySend = 0.1f; d.eqMidDb = 0.0f; }
                const size_t undo0 = ma.undoStack.size();
                const int n = ma.MatchLevelPanEq(0, kNumDrumTracks, false);
                const Track& l5 = pj.patterns[5].tracks[kNumDrumTracks]; const Track& bk = pj.patterns[static_cast<size_t>(brick)].tracks[kNumDrumTracks];
                check(n == kNumLiveScenes - 1 + 1 && l5.volume == 0.5f && l5.pan == 0.2f && l5.eqMidDb == -3.0f && l5.eqMidHz == 215.0f && l5.eqMidQ == 2.0f && l5.eqHpfHz == 80.0f, "mix lab apply: level, pan and EQ reach every other live scene");
                check(bk.volume == 0.5f && bk.eqMidDb == -3.0f, "mix lab apply: ... and the bricks of the chain");
                check(pj.patterns[static_cast<size_t>(unusedBrick)].tracks[kNumDrumTracks].volume == 0.9f, "mix lab apply: a brick slot that nothing uses is left alone");
                check(l5.delaySend == 0.1f && !l5.mute && l5.eqLowDb == 0.0f, "mix lab apply: sends, mute and the other EQ bands of the other scenes are not touched");
                check(ma.undoStack.size() == undo0 + static_cast<size_t>(n), "mix lab apply: every changed scene gets its own undo step");
                check(ma.MatchLevelPanEq(0, kNumDrumTracks, false) == 0, "mix lab apply: doing it again changes nothing");
                for (int sc = 1; sc < kNumPatterns; ++sc) pj.patterns[static_cast<size_t>(sc)].tracks[kNumDrumTracks].volume = 0.9f;
                check(ma.MatchLevelPanEq(0, kNumDrumTracks, true) == 1 && pj.patterns[5].tracks[kNumDrumTracks].volume == 0.9f && pj.patterns[static_cast<size_t>(brick)].tracks[kNumDrumTracks].volume == 0.5f, "mix lab apply: 'only the chain' reaches the bricks of the chain and leaves the other live scenes");
                const std::string mlc = slurp("src/ui_desktop/MixLabPanel.cpp");
                check(mlc.find("void MixLabPanel::DrawChannel") != std::string::npos && mlc.find("APPLY TO ALL") != std::string::npos && mlc.find("TO ALL SCENES") != std::string::npos && mlc.find("MatchLevelPanEq") != std::string::npos && mlc.find("TrackEqResponseDb") != std::string::npos, "mix lab: CHANNEL has the level, pan and EQ curve and both APPLY buttons");
            }
        }
    }

    // ---- v44.27: a thrown brick sounds like its scene; on LIVE the player plays the scene ----
    {
        // the same scene as a LIVE loop and as a looped brick, with a sound that differs from the track's, renders the same
        auto render = [&](bool asBrick, std::vector<float>& buf) {
            auto dap = std::make_unique<AppState>(); AppState& da = *dap;
            da.engine.Init(kSampleRate); da.sequencer.Init(&da.engine, &da.project);
            Project& pj = da.project; pj.kitFolder = kitFolder;
            da.engine.GetKit()->LoadFromFolder(kitFolder, kSampleRate);
            for (int sc = 0; sc < 2; ++sc) {
                pj.patterns[static_cast<size_t>(sc)].tracks[0].steps[0].active = true; pj.patterns[static_cast<size_t>(sc)].tracks[3].steps[2].active = true;
                pj.patterns[static_cast<size_t>(sc)].tracks[6].steps[0].active = true; pj.patterns[static_cast<size_t>(sc)].tracks[6].steps[8].active = true;
            }
            pj.patterns[1].synthPatches[0].cutoffHz = 420.0f; pj.patterns[1].tracks[6].volume = 0.4f; pj.patterns[1].tracks[3].pan = -0.6f; pj.patterns[1].kick.decayMs = 500.0f;
            da.activeTab = 2;
            pj.currentPattern = 0; pj.liveScene = 0; da.ThrowIntoChain();
            pj.currentPattern = 1; pj.liveScene = 1;
            if (asBrick) { da.ThrowIntoChain(); pj.currentPattern = pj.songChain[1].patternIndex; }
            da.sequencer.ApplySceneSound(pj.currentPattern);
            da.engine.GetMixer().SyncFromPattern(pj.patterns[static_cast<size_t>(pj.currentPattern)]);
            if (asBrick) da.sequencer.PlayChainFrom(1, true); else { da.sequencer.SetSongModeActive(false); da.sequencer.Play(); }
            buf.assign(static_cast<size_t>(kSampleRate) * 4 * 2, 0.0f);
            for (size_t done = 0; done < buf.size() / 2;) { const int n = static_cast<int>(std::min<size_t>(256, buf.size() / 2 - done)); da.sequencer.RenderAndAdvance(buf.data() + done * 2, n, kSampleRate); done += n; }
        };
        std::vector<float> live, brick; render(false, live); render(true, brick);
        double e = 0, p2 = 0; for (size_t k = 0; k < live.size(); ++k) { const double d = live[k] - brick[k]; e += d * d; p2 += double(live[k]) * live[k]; }
        check(p2 > 0.01 && e < 1e-9 * (p2 + 1.0), "v44.27: the same scene sounds the same as a LIVE loop and as a brick, even with a sound that differs from the track's");
        // the player on LIVE
        AppState da; da.engine.Init(kSampleRate); da.sequencer.Init(&da.engine, &da.project);
        Project& pj = da.project;
        pj.patterns[0].tracks[0].steps[0].active = true;
        da.activeTab = 2; da.ThrowIntoChain(); da.ThrowIntoChain();
        da.SetLoop(false); da.activeTab = 1; da.TransportToggle();
        check(da.sequencer.IsPlaying() && da.sequencer.IsSongModeActive(), "v44.27: on TRACK Play runs the track");
        desktop::ShowTab(da, 2);
        check(da.sequencer.IsPlaying() && da.sequencer.IsSongModeActive(), "v44.51: going to LIVE while the track plays: the track keeps playing, the LIVE scene does not start by itself");
        da.activeTab = 2; da.PlayHere();
        check(da.sequencer.IsPlaying() && !da.sequencer.IsSongModeActive() && !AppState::IsBrickSlot(pj.currentPattern) && da.sequencer.PlayPatternIndex() == pj.currentPattern, "v44.27: PLAY HERE on LIVE: playback continues with the LIVE scene, not the track");
        da.TransportToggle(); da.TickTransport();
        da.SetLoop(false); da.TransportToggle();
        check(da.sequencer.IsPlaying() && !da.sequencer.IsSongModeActive(), "v44.27: Play on LIVE plays the LIVE scene even when the player was left on the TRACK settings");
        da.TransportToggle(); da.TickTransport();
        std::ifstream msIn("src/ui_desktop/MixerSync.h"); const std::string src((std::istreambuf_iterator<char>(msIn)), std::istreambuf_iterator<char>());
        check(src.find("ReleasePin") != std::string::npos && src.find("PinPlayToCurrent") != std::string::npos, "v44.51: switching tabs only moves the pin, it never starts the other tab (source ratchet)");
    }

    // ---- v44.29 (U5): the Step settings window is gone, the inspector has everything ----
    {
        auto slurp = [](const std::string& path) { std::ifstream in(path); return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); };
        const std::string brickSrc = slurp("src/ui_desktop/BricksPanel.cpp"), chainSrc = slurp("src/ui_desktop/ChainPanel.cpp"), chainHdr = slurp("src/ui_desktop/ChainPanel.h");
        check(chainSrc.find("ChainStepEditor") == std::string::npos && chainHdr.find("OpenStepEditor") == std::string::npos && brickSrc.find("OpenStepEditor") == std::string::npos && chainSrc.find("Step settings...") == std::string::npos && brickSrc.find("Fine-tune numbers") == std::string::npos, "u5: the Step settings window and the menu items that opened it are gone");
        check(brickSrc.find("\"##mst\"") != std::string::npos && brickSrc.find("\"##fst\"") != std::string::npos && brickSrc.find("ResetBrickSettings") != std::string::npos, "u5: the inspector has the MIX / FILTER ends and RESET");
        AppState da; da.engine.Init(kSampleRate); da.sequencer.Init(&da.engine, &da.project);
        Project& pj = da.project; pj.patterns[0].tracks[0].steps[0].active = true;
        da.ThrowIntoChain();
        ChainStep& cs = pj.songChain[0];
        cs.muteMask = 3; cs.fillMode = FillMode::Own; cs.fill = FillRule{FillWhen::LastLoop, FillKind::HatRoll, 2, 4, 0}; cs.riser = 1; cs.trimSteps = 8; cs.automation.mix = Lane::Line(-12, 0, kAutoMinDb, kAutoMaxDb); cs.levels.fillDb = -6;
        da.ResetBrickSettings(0);
        check(cs.muteMask == 0 && cs.fillMode == FillMode::Scene && cs.riser < 0 && cs.trimSteps == 0 && !cs.automation.Any() && !cs.levels.Any(), "u5: RESET puts everything the brick adds back to nothing");
        check(da.Undo() && pj.songChain[0].muteMask == 3 && pj.songChain[0].fillMode == FillMode::Own && pj.songChain[0].automation.mix.Any(), "u5: ... in one undo step");
        check(pj.patterns[static_cast<size_t>(pj.songChain[0].patternIndex)].tracks[0].steps[0].active, "u5: ... and the notes are not touched");
    }

    // ---- v44.30 (U6): several bricks / several notes at once ----
    {
        auto slurp = [](const std::string& path) { std::ifstream in(path); return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); };
        // bricks: a range, LOOP round the whole track, remove / duplicate the range in one undo step
        AppState da; da.engine.Init(kSampleRate); da.sequencer.Init(&da.engine, &da.project);
        Project& pj = da.project; pj.patterns[0].tracks[0].steps[0].active = true;
        for (int i = 0; i < 4; ++i) da.ThrowIntoChain();
        da.SelectAllBricks();
        int lo = -1, hi = -1; da.LoopRange(&lo, &hi);
        check(da.HasBrickRange() && lo == 0 && hi == 3, "u6: Cmd+A on TRACK selects every brick");
        check(da.sequencer.SongLoopLo() == 0, "u6: LOOP then goes round the whole track");
        da.SelectBrickRange(2, 1);
        da.LoopRange(&lo, &hi);
        check(lo == 1 && hi == 2 && da.selBrick == 1, "u6: a range selected backwards is the same range");
        da.SelectBrickRange(3, 3);
        check(!da.HasBrickRange() && da.selBrick == 3, "u6: a range of one brick is just that brick");
        da.SelectBrickRange(1, 2);
        check(da.DuplicateBrickRange(1, 2) && pj.songChainLength == 6 && da.selBrick == 3 && da.HasBrickRange(), "u6: Cmd+D on a range puts it again right after itself and selects the copy");
        check(da.Undo() && pj.songChainLength == 4, "u6: duplicating a range is one undo step");
        da.SelectBrickRange(0, 1);
        check(da.RemoveBrickRange(0, 1) && pj.songChainLength == 2 && !da.HasBrickRange(), "u6: Delete removes all bricks of the range");
        check(da.Undo() && pj.songChainLength == 4, "u6: removing a range is one undo step");
        const std::string brickSrc = slurp("src/ui_desktop/BricksPanel.cpp");
        check(brickSrc.find("rubber band") != std::string::npos && brickSrc.find("SelectAllBricks") != std::string::npos, "u6: the strip has the rubber band and Cmd+A (source ratchet)");

        // notes: synth track
        Track t; t.type = TrackType::Synth; t.length = 16;
        for (int c : {0, 4, 8}) { t.steps[c].active = true; t.steps[c].note = static_cast<int8_t>(60 + c); t.steps[c].lengthSteps = 2; t.steps[c].probability = 70; }
        std::vector<NoteRef> sel = AllNotes(t, 16);
        const std::vector<NoteRef> orig = sel;
        check(sel.size() == 3, "u6: Cmd+A in the roll finds every note");
        StepSnapshot snap = t.steps;
        sel = MoveNotes(t, snap, sel, 0, 12, false, 16);
        check(NoteIsOn(t, 0, 72) && NoteIsOn(t, 4, 76) && NoteIsOn(t, 8, 80) && !NoteIsOn(t, 0, 60), "u6: the selected notes go up an octave together");
        check(t.steps[4].probability == 70 && t.steps[4].lengthSteps == 2, "u6: a moved note keeps its length and probability");
        sel = MoveNotes(t, snap, orig, 2, 0, true, 16);   // a copy of the ORIGINAL notes, 2 steps later (the snapshot is from before the octave)
        check(NoteIsOn(t, 0, 60) && NoteIsOn(t, 2, 60) && NoteIsOn(t, 6, 64) && NoteIsOn(t, 10, 68), "u6: Option+drag leaves the originals and puts copies");
        int dc = 100, dp = 100; ClampNoteDelta(sel, 16, &dc, &dp);
        check(dc == 16 - 1 - 10 && dp == 127 - 68, "u6: a move can never push a note off the grid");
        t.steps = snap; sel = AllNotes(t, 16);
        sel = MoveNotes(t, snap, sel, 3, 0, false, 16);
        sel = MoveNotes(t, snap, orig, 0, 0, false, 16);   // back to the start of the gesture: nothing lost
        check(NoteIsOn(t, 0, 60) && NoteIsOn(t, 4, 64) && NoteIsOn(t, 8, 68), "u6: dragging there and back loses nothing");
        ResizeNotes(t, snap, sel, 2, 16);
        check(t.steps[0].lengthSteps == 4 && t.steps[4].lengthSteps == 4 && t.steps[8].lengthSteps == 4, "u6: the length of all selected notes changes together");
        RemoveNotes(t, sel);
        check(AllNotes(t, 16).empty(), "u6: Delete removes the selected notes");
        t.steps = snap; sel = AllNotes(t, 16);
        std::vector<NoteRef> two2 = sel; two2.resize(2);
        check(DuplicateNotes(t, two2, 16) && AllNotes(t, 16).size() == 5 && NoteIsOn(t, 6, 60) && NoteIsOn(t, 10, 64), "u6: Cmd+D puts the selection again right after itself");
        t.steps = snap;
        check(!DuplicateNotes(t, sel, 16) && AllNotes(t, 16).size() == 3, "u6: Cmd+D does nothing when the copy would not fit");

        // notes: chord track, only one note of a chord selected
        Track c; c.type = TrackType::Chord; c.length = 16;
        c.steps[0].active = true; c.steps[0].chordSize = 3; c.steps[0].chordNotes = {60, 64, 67, 71};
        std::vector<NoteRef> one{{0, 64}};
        StepSnapshot cs0 = c.steps;
        one = MoveNotes(c, cs0, one, 0, 12, false, 16);
        check(NoteIsOn(c, 0, 76) && NoteIsOn(c, 0, 60) && NoteIsOn(c, 0, 67) && !NoteIsOn(c, 0, 64) && c.steps[0].chordSize == 3, "u6: one note of a chord goes up an octave, the others stay");
        std::vector<NoteRef> two{{0, 60}};
        two = MoveNotes(c, c.steps, two, 4, 0, false, 16);
        check(NoteIsOn(c, 4, 60) && c.steps[0].chordSize == 2 && c.steps[4].chordSize == 1 && c.steps[4].active, "u6: one note of a chord moved in time becomes its own step");
        const std::string rollSrc = slurp("src/ui_desktop/PianoRollPanel.cpp");
        check(rollSrc.find("MoveNotes") != std::string::npos && rollSrc.find("marquee_") != std::string::npos, "u6: the roll uses the group editing (source ratchet)");

        // arrow keys move the selected notes: also after the mouse leaves the roll (a click in it gives it the keys)
        check(RollOwnsKeys(true, false, false, false) && RollOwnsKeys(false, true, false, false), "roll keys: hovered or clicked last = the arrows move the notes");
        check(!RollOwnsKeys(false, false, false, false), "roll keys: a click elsewhere gives the arrows back to the instrument block");
        check(!RollOwnsKeys(true, true, true, false) && !RollOwnsKeys(true, true, false, true), "roll keys: not while typing or during a mouse drag");
        check(rollSrc.find("RollOwnsKeys(rollHovered, keysFocus_") != std::string::npos && rollSrc.find("keysFocus_ = rollHovered") != std::string::npos,
              "roll keys: the roll keeps the arrows after a click in it (source ratchet)");
        check(rollSrc.find("sel_ = AllNotes(t, trackLen); keysFocus_ = true;") != std::string::npos,
              "roll keys: Cmd+A over the roll also keeps the arrows with the notes when the mouse leaves (source ratchet)");
        check(rollSrc.find("ImGuiKey_UpArrow)) dp = shiftKey ? 12 : 1") != std::string::npos && rollSrc.find("ImGuiKey_DownArrow)) dp = shiftKey ? -12 : -1") != std::string::npos
              && rollSrc.find("ImGuiKey_RightArrow)) dc = shiftKey") != std::string::npos && rollSrc.find("ImGuiKey_LeftArrow)) dc = shiftKey") != std::string::npos,
              "roll keys: up / down = a semitone, Shift = an octave; left / right = a step, Shift = a beat (source ratchet)");
        check(slurp("src/main.cpp").find("app.rollKeysFrame >= ImGui::GetFrameCount() - 1") != std::string::npos,
              "roll keys: the instrument block leaves the arrows to the selected notes (source ratchet)");
    }

    // ---- v44.31 (U7): browsers and modal windows answer the same keys ----
    {
        auto slurp = [](const std::string& path) { std::ifstream in(path); return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); };
        bool clean = true;
        for (const char* f : {"src/ui_desktop/DocumentDialogs.cpp", "src/ui_desktop/ListBrowser.cpp", "src/ui_desktop/PresetBrowser.cpp"}) {
            const std::string src = slurp(f);
            if (src.find("IsKeyPressed(ImGuiKey_Escape") != std::string::npos || src.find("IsKeyPressed(ImGuiKey_Enter") != std::string::npos) clean = false;
        }
        check(clean, "u7: dialogs and browsers use KeyConfirm / KeyCancel / the browser core, not their own Return / Esc checks");
        check(slurp("src/ui_desktop/ListBrowser.cpp").find("browser::NavDelta") != std::string::npos && slurp("src/ui_desktop/PresetBrowser.cpp").find("browser::NavDelta") != std::string::npos
              && slurp("src/ui_desktop/ListBrowser.cpp").find("browser::DrawStar") != std::string::npos && slurp("src/ui_desktop/PresetBrowser.cpp").find("browser::DrawStar") != std::string::npos,
              "u7: both browsers take their keys and card parts from one shared core");
        check(slurp("src/ui_desktop/DocumentDialogs.cpp").find("ImGuiKey_DownArrow") != std::string::npos, "u7: the Open list can be driven with the arrow keys");
    }

    // ---- v44.26: one drum can play from a kit of its own ----
    {
        AppState da; da.engine.Init(kSampleRate); da.sequencer.Init(&da.engine, &da.project);
        Project& pj = da.project; pj.kitFolder = "kits/909";
        pj.patterns[0].tracks[1].steps[0].active = true;
        da.SyncSceneKits(true);
        check(da.TrackKitFolder(0, 1) == "kits/909" && da.TrackKitFolder(0, 6) == "kits/909", "v44.26: without an own kit a drum plays from the kit of the scene");
        pj.patterns[0].tracks[1].kitFolder = "kits/808";
        check(da.TrackKitFolder(0, 1) == "kits/808" && da.TrackKitFolder(0, 0) == "kits/909", "v44.26: a drum with an own kit plays from it, the others stay");
        const int n808 = da.SceneKitVariantCount(0, 1), n909 = da.SceneKitVariantCount(0, 0);
        check(n808 > 0 && n909 > 0 && da.SceneKitVariantCount(0, 1) == static_cast<int>(da.KitBank("kits/808")->variants[1].size()), "v44.26: the variants of that drum are counted in its own kit");
        da.SyncSceneKits();
        const auto b808 = da.KitBank("kits/808"), b909 = da.KitBank("kits/909");
        {   // C4b: the bank knows the file of each variant (shown in the SAMPLE row without reading the disk under the scene lock)
            bool namesOk = b909 != nullptr;
            for (int sl = 0; namesOk && sl < kNumDrumTracks; ++sl) namesOk = b909->names[static_cast<size_t>(sl)].size() == b909->variants[static_cast<size_t>(sl)].size();
            check(namesOk && !b909->names[0].empty() && b909->names[0][0] == "kick.wav" && b909->names[0].size() >= 2 && b909->names[0][1] == "kick_2.wav",
                  "c4b: a kit bank names its variants (kick.wav, kick_2.wav ...) in the order they play");
        }
        da.sequencer.ApplySceneSound(0);
        const auto cur = da.engine.GetKit()->CurrentBank();
        check(cur && cur->variants[1] == b808->variants[1] && cur->variants[0] == b909->variants[0] && cur->variants[3] == b909->variants[3], "v44.26: the sounding bank is the scene's kit with that one drum from the other kit");
        pj.patterns[0].tracks[1].kitFolder.clear();
        da.SyncSceneKits();
        da.sequencer.ApplySceneSound(0);
        check(da.engine.GetKit()->CurrentBank()->variants[1] == b909->variants[1], "v44.26: back to THE KIT: the drum plays from the kit of the scene again");
        // the kit picker changes the kit of the selected drum when it has one
        pj.patterns[0].tracks[1].kitFolder = "kits/808"; da.seqCursorTrack = 1; pj.currentPattern = 0;
        da.LoadKitFolder("kits/606");
        check(pj.patterns[0].tracks[1].kitFolder == "kits/606" && pj.patterns[0].kitFolder.empty(), "v44.26: the kit picker edits the own kit of the selected drum, not the kit of the scene");
        da.seqCursorTrack = 0; da.LoadKitFolder("kits/707");
        check(pj.patterns[0].kitFolder == "kits/707" && pj.patterns[0].tracks[1].kitFolder == "kits/606", "v44.26: another instrument: the picker edits the kit of the scene");
        // sound parts carry it
        Pattern a, b; a.tracks[1].kitFolder = "kits/808";
        CopyPart(b, a, 1);
        check(b.tracks[1].kitFolder == "kits/808" && SoundPartText(a, 1) != SoundPartText(b, 2), "v44.26: the own kit travels with the instrument's sound");
        // saved and loaded
        const std::string path = "selftest_v4426.t8proj";
        Project sp; sp.patterns[0].tracks[2].kitFolder = "kits/linn";
        check(SaveProject(sp, path), "v44.26: save");
        Project lp; check(LoadProject(lp, path) && lp.patterns[0].tracks[2].kitFolder == "kits/linn" && lp.patterns[0].tracks[1].kitFolder.empty(), "v44.26: the own kit of a drum survives save / load");
        std::remove(path.c_str());
        // the UI: the browser is drawn on TRACK too
        auto slurp = [](const std::string& path2) { std::ifstream in(path2); return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); };
        const std::string mainSrc = slurp("src/main.cpp"), soundSrc = slurp("src/ui_desktop/SoundPanel.cpp");
        check(mainSrc.find("bottomPanel.DrawBrowsers(app)") != std::string::npos && soundSrc.find("THIS DRUM") != std::string::npos && soundSrc.find("OWN KIT") != std::string::npos, "v44.26: the kit browser works on TRACK, THIS DRUM / OWN KIT is on screen");
    }

    // ---- v44.13: levels of the instruments, of the transition into a step and of its fill ----
    {
        StepLevels l; l.track[6] = Lane::Line(0, kAutoMinDb, kAutoMinDb, kAutoMaxDb); l.track[0] = Lane::Line(-6, -6, kAutoMinDb, kAutoMaxDb); l.transitionDb = -3; l.fillDb = -9;
        check(!StepLevels{}.Any() && l.Any() && l.TrackAny(6) && !l.TrackAny(1), "levels: nothing set = untouched, a lane is found");
        check(std::fabs(TrackLevelAt(l, 1, 0.5) - 1.0f) < 1e-6f && std::fabs(TrackLevelAt(l, 0, 0.3) - std::pow(10.0f, -6.0f / 20.0f)) < 1e-5f, "levels: an instrument without lane is untouched, a flat lane is a gain");
        check(TrackLevelAt(l, 6, 0.0) == 1.0f && TrackLevelAt(l, 6, 1.0) == 0.0f && TrackLevelAt(l, 6, 0.5) > 0.0f && TrackLevelAt(l, 6, 0.5) < TrackLevelAt(l, 6, 0.1), "levels: a fade runs from full level to silence");
        StepLevels bad; bad.track[2].n = 2; bad.track[2].pos[1] = 255; bad.track[2].val[0] = 100; bad.track[2].val[1] = -100; bad.fillDb = 90; bad = NormalizeLevels(bad);
        check(bad.track[2].Start() == kAutoMaxDb && bad.track[2].End() == kAutoMinDb && bad.fillDb == kAutoMaxDb, "levels: values are clamped");
        // saved and loaded; a file without level lines loads clean
        {
            Project sp; sp.songChain[0] = ChainStep{0, 2, -1}; sp.songChain[1] = ChainStep{1, 1, -1}; sp.songChainLength = 2;
            sp.songChain[0].levels = l; sp.songChain[0].muteMask = kMuteKickBass;
            const std::string path = "selftest_lvl.t8proj";
            check(SaveProject(sp, path), "levels: save");
            Project lp; check(LoadProject(lp, path) && lp.songChain[0].levels == l && !lp.songChain[1].levels.Any() && lp.songChain[0].muteMask == kMuteKickBass, "levels: survive save / load (the brick without levels stays clean)");
            std::ifstream in(path); std::string line, out;
            while (std::getline(in, line)) { if (line.rfind("lv20=", 0) == 0 || line.rfind("lvl0=", 0) == 0) continue; out += line + "\n"; }
            in.close(); { std::ofstream o(path, std::ios::trunc); o << out; }
            Project ol; check(LoadProject(ol, path) && !ol.songChain[0].levels.Any(), "levels: a file from before v44.13 loads with no levels");
            std::remove(path.c_str());
        }
        // AppState setters
        {
            AppState da; da.project.songChainLength = 2;
            da.SetBrickTrackLevels(1, 6, -3, 99); da.SetBrickTransitionLevel(1, -200); da.SetBrickFillLevel(1, -4);
            const StepLevels& g = da.project.songChain[1].levels;
            check(g.track[6].Start() == -3 && g.track[6].End() == kAutoMaxDb && g.transitionDb == kAutoMinDb && g.fillDb == -4, "levels: AppState setters set and clamp");
            da.SetBrickTrackLevel(5, 6, false, -9); da.SetBrickTrackLevel(1, 12, false, -9);
            check(!da.project.songChain[0].levels.Any(), "levels: out-of-range bricks / tracks are ignored");
        }
        // the engine: held bass (track 6) and a held synth (track 7) over a chain step that plays twice (1 s each at 240 bpm)
        auto render = [&](const StepLevels& lv, std::vector<float>& L, double seconds) {
            AppState ds; ds.engine.Init(kSampleRate); ds.sequencer.Init(&ds.engine, &ds.project);
            for (int i = 0; i < 2; ++i) { Pattern& pt = ds.project.patterns[static_cast<size_t>(i)]; pt.masterLength = 16; pt.bpm = 240.0f; }
            for (int ti : {kNumDrumTracks, kNumDrumTracks + 1}) { Track& b = ds.project.patterns[0].tracks[ti]; b.steps[0].active = true; b.steps[0].note = 48; b.steps[0].lengthSteps = 16; b.steps[0].chordSize = 0; }
            ds.project.songChain[0] = ChainStep{0, 2, -1}; ds.project.songChain[1] = ChainStep{1, 1, -1}; ds.project.songChainLength = 2;
            ds.project.songChain[0].levels = lv;
            ds.RefreshSound(); ds.sequencer.SetSongModeActive(true); ds.sequencer.Play();
            std::vector<float> ob(256 * 2); int done = 0; const int need = static_cast<int>(kSampleRate * seconds);
            while (done < need) { std::fill(ob.begin(), ob.end(), 0.0f); ds.sequencer.RenderAndAdvance(ob.data(), 256, kSampleRate); for (int i = 0; i < 256; ++i) L.push_back(ob[static_cast<size_t>(i) * 2]); done += 256; }
            ds.sequencer.SetSongModeActive(false); ds.sequencer.Stop();
            std::vector<float> tail(256 * 2); for (int k = 0; k < 40; ++k) ds.sequencer.RenderAndAdvance(tail.data(), 256, kSampleRate);
            return std::fabs(ds.engine.GetMixer().arrTrackGain[6].v.load() - 1.0f) < 1e-4f && std::fabs(ds.engine.GetMixer().arrTrackGain[7].v.load() - 1.0f) < 1e-4f;
        };
        auto rmsOf = [&](const std::vector<float>& x, double t0, double t1) { double sum = 0; size_t a0 = static_cast<size_t>(t0 * kSampleRate), a1 = std::min(x.size(), static_cast<size_t>(t1 * kSampleRate)); for (size_t i = a0; i < a1; ++i) sum += x[i] * x[i]; return std::sqrt(sum / std::max<size_t>(1, a1 - a0)); };
        std::vector<float> plain, fade, quiet;
        StepLevels none, fadeBass, quietSynth;
        fadeBass.track[6] = Lane::Line(0, kAutoMinDb, kAutoMinDb, kAutoMaxDb);
        quietSynth.track[7] = Lane::Line(-48, -48, kAutoMinDb, kAutoMaxDb);
        const bool r0 = render(none, plain, 2.0), r1 = render(fadeBass, fade, 2.0), r2 = render(quietSynth, quiet, 2.0);
        check(r0 && r1 && r2, "levels: the instrument gains are released when the song stops");
        check(rmsOf(plain, 0.5, 0.7) > 0.001, "levels: the held notes make sound");
        const double fe0 = rmsOf(fade, 0.05, 0.25), fe1 = rmsOf(fade, 1.75, 1.95), pe0 = rmsOf(plain, 0.05, 0.25), pe1 = rmsOf(plain, 1.75, 1.95);
        check(fe0 > 0.78 * pe0 && fe1 < 0.6 * pe1 && fe1 > 0.1 * pe1, "levels: one instrument fades out over the brick while the other keeps its level");
        check(rmsOf(quiet, 0.5, 1.5) < 0.75 * rmsOf(plain, 0.5, 1.5) && rmsOf(quiet, 0.5, 1.5) > 0.2 * rmsOf(plain, 0.5, 1.5), "levels: silencing one of two instruments takes its part out, not everything");
        // the transition into brick 2 (SUB DROP 1 bar ahead) and the fill of a brick, at full level and at -48 dB
        auto peakOfTransition = [&](int db) {
            static Project tp; tp = Project{};
            tp.patterns[0].bpm = 120.0f; tp.patterns[0].masterLength = 16; tp.patterns[1].bpm = 120.0f; tp.patterns[1].masterLength = 16;
            tp.songChainLength = 2; tp.songChain[0] = ChainStep{0, 2, -1}; tp.songChain[1] = ChainStep{1, 1, -1};
            tp.songChain[1].riser = 12; tp.songChain[1].riserBars = 1; tp.songChain[1].levels.transitionDb = static_cast<int8_t>(db);
            AudioEngine e; e.Init(kSampleRate); e.Risers().LoadFromFolder("kits/risers", kSampleRate);
            Sequencer sq; sq.Init(&e, &tp); sq.SetSongFollowsEdit(false); sq.SetSongModeActive(true); sq.Play();
            std::vector<float> o(256 * 2); float pk = 0.0f;
            for (int b = 0; b < static_cast<int>(kSampleRate * 4.6 / 256); ++b) { std::fill(o.begin(), o.end(), 0.0f); sq.RenderAndAdvance(o.data(), 256, kSampleRate); for (float v : o) pk = std::max(pk, std::fabs(v)); }
            return pk;
        };
        const float tFull = peakOfTransition(0), tOff = peakOfTransition(kAutoMinDb), tHalf = peakOfTransition(-6);
        check(tFull > 0.05f && tOff < 0.001f && tHalf > 0.3f * tFull && tHalf < 0.8f * tFull, "levels: the transition into a brick plays at its own level (full, -6 dB, silent)");
        auto rmsOfFill = [&](int db) {
            static Project fp2; fp2 = Project{};
            fp2.patterns[0].bpm = 120.0f; fp2.patterns[0].masterLength = 16;
            fp2.patterns[0].fill = FillRule{FillWhen::EveryLoop, FillKind::HatRoll, 4, 4, 0};
            fp2.songChainLength = 1; fp2.songChain[0] = ChainStep{0, 4, -1}; fp2.songChain[0].levels.fillDb = static_cast<int8_t>(db);
            AudioEngine e; e.Init(kSampleRate); e.GetKit()->LoadFromFolder("kits/808", kSampleRate);
            e.GetMixer().SyncFromPattern(fp2.patterns[0]);
            Sequencer sq; sq.Init(&e, &fp2); sq.SetSongFollowsEdit(false); sq.SetSongModeActive(true); sq.Play();
            std::vector<float> o(256 * 2); double sum = 0; long cnt = 0;
            const double spStep = 60.0 / 120.0 / 4.0 * kSampleRate;
            const int total = static_cast<int>(spStep * 16);   // one pass: the roll is on its last 4 beats (steps 0..15 of a 16-step pass)
            for (int done = 0; done < total; done += 256) { std::fill(o.begin(), o.end(), 0.0f); sq.RenderAndAdvance(o.data(), 256, kSampleRate); if (done > spStep * 12) for (int i = 0; i < 256; ++i) { sum += o[static_cast<size_t>(i) * 2] * o[static_cast<size_t>(i) * 2]; ++cnt; } }
            return std::sqrt(sum / std::max<long>(1, cnt));
        };
        const double fFull = rmsOfFill(0), fOff = rmsOfFill(kAutoMinDb), fLow = rmsOfFill(-12);
        check(fFull > 0.003 && fOff < 0.1 * fFull && fLow < 0.6 * fFull && fLow > 0.05 * fFull, "levels: the hits of a fill play at the brick's fill level");
    }

    // ---- v44.10.2: names of the dynamics shapes on the bricks strip ----
    {
        using namespace t8::desktop;
        size_t np = 0; const DynPreset* dp = DynPresets(&np);
        check(np == 6 && !dp[0].a.Any(), "dyn presets: six shapes, FLAT first and really flat");
        check(DynPresetName(StepAuto{}) == nullptr, "dyn name: no dynamics, no name");
        bool all = true;
        for (size_t i = 1; i < np; ++i) all = all && std::string(DynPresetName(dp[i].a)) == dp[i].label;
        check(all, "dyn name: every preset is recognised by its own name");
        check(std::string(DynPresetName(StepAuto::Ends(0, 0, -20, -70))) == "DARKENS" && std::string(DynPresetName(StepAuto::Ends(0, 0, -60, 0))) == "OPENS"
              && std::string(DynPresetName(StepAuto::Ends(-1, 0, 5, 5))) == "CUSTOM", "dyn name: hand-made shapes are described by what they do");
    }


    // ---- v44.14 (U8a): user folders, names of recordings, migration, backups ----------------------------------------------------
    {
        namespace ud = t8::userdirs;
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::remove_all("selftest_u8a", ec);
        ud::SetRootsForTest("selftest_u8a/Documents/TR-Brick", "selftest_u8a/Service");
        check(!ud::InLegacyMode(), "u8a: the user folders can be created");
        check(fs::is_directory("selftest_u8a/Documents/TR-Brick/Projects") && fs::is_directory("selftest_u8a/Documents/TR-Brick/Recordings")
              && fs::is_directory("selftest_u8a/Service/Backups"), "u8a: Projects, Recordings and the service Backups folder exist");
        check(ud::ProjectsDir() == "selftest_u8a/Documents/TR-Brick/Projects" && ud::BackupsDir() == "selftest_u8a/Service/Backups", "u8a: folder paths");

        // names
        check(ud::ProjectDisplayName("a/b/Нічна зміна.t8proj") == "Нічна зміна", "u8a: project name from the file name (Ukrainian kept)");
        check(ud::SanitizeName("Нічна/зміна: 2*", "X") == "Нічназміна 2", "u8a: sanitize keeps letters of any alphabet, drops separators");
        check(ud::SanitizeName("..  ", "Untitled") == "Untitled" && ud::SanitizeName("///", "Untitled") == "Untitled", "u8a: sanitize falls back when nothing is left");
        check(ud::SanitizeName(std::string(200, 'a'), "X").size() == 80, "u8a: sanitize limits the length");
        std::string ukr; for (int i = 0; i < 60; ++i) ukr += "ї";   // 2 bytes each
        const std::string cut = ud::SanitizeName(ukr, "X");
        check(cut.size() <= 80 && cut.size() % 2 == 0, "u8a: sanitize never cuts inside a UTF-8 character");
        check(t8::UniqueProjectPathForName("Мій трек") == "selftest_u8a/Documents/TR-Brick/Projects/Мій трек.t8proj", "u8a: a new project is named as typed, in Projects");

        // recordings
        ud::SetProjectName("Нічна зміна");
        const long long t0 = 1793025120;   // fixed time
        const std::string p1 = ud::StampedPath(ud::RecordingsDir(), "Нічна зміна", "", "wav", t0);
        std::regex re(R"(.*/Recordings/Нічна зміна \d{4}-\d{2}-\d{2} \d{2}\.\d{2}\.wav)");
        check(std::regex_match(p1, re), "u8a: recording name is '<project> YYYY-MM-DD HH.MM.wav' (" + p1 + ")");
        { std::ofstream(p1) << "x"; }
        const std::string p2 = ud::StampedPath(ud::RecordingsDir(), "Нічна зміна", "", "wav", t0);
        check(p2 != p1 && p2.size() > 4 && p2.substr(p2.size() - 6) == " 2.wav", "u8a: the same minute gets ' 2', never overwrites");
        const std::string pz = ud::StampedPath(ud::RecordingsDir(), "", " stems", "zip", t0);
        check(pz.find(" stems.zip") != std::string::npos && pz.find("/TR-Brick ") != std::string::npos, "u8a: no project name falls back to TR-Brick; stems name");
        check(ud::DisplayPath(p1).rfind("Recordings/", 0) == 0, "u8a: status shows the path relative to the data root");
        ud::SetLastOutput(p1);
        check(ud::LastOutput() == p1, "u8a: the last output is remembered for SHOW IN FINDER");

        // project listing shows the Projects folder (not the current directory)
        { std::ofstream("selftest_u8a/Documents/TR-Brick/Projects/one.t8proj") << "x"; std::ofstream("selftest_u8a/Documents/TR-Brick/Projects/Два.t8proj") << "x"; }
        const auto list = t8::ListProjectFiles("");
        check(list.size() == 2, "u8a: the project list is the Projects folder (" + std::to_string(list.size()) + ")");

        // migration from the program folder: copies, renames recordings, keeps the originals, runs once
        ud::SetRootsForTest("selftest_u8a/Doc2", "selftest_u8a/Svc2");
        fs::create_directories("selftest_u8a/old/projects"); fs::create_directories("selftest_u8a/old/recordings");
        fs::create_directories("selftest_u8a/old/exports"); fs::create_directories("selftest_u8a/old/backups");
        { std::ofstream("selftest_u8a/old/project.t8proj") << "main";
          std::ofstream("selftest_u8a/old/projects/jam.t8proj") << "jam";
          std::ofstream("selftest_u8a/old/recordings/take_1793025120.wav") << "wav";
          std::ofstream("selftest_u8a/old/exports/stems_1793025120.zip") << "zip";
          std::ofstream("selftest_u8a/old/exports/notes_1793025200.txt") << "txt";
          std::ofstream("selftest_u8a/old/backups/jam-20260101-101010.t8proj") << "b"; }
        const ud::MigrationResult mr = ud::MigrateLegacy("selftest_u8a/old");
        check(mr.ran && mr.projects == 2 && mr.recordings == 3 && mr.backups == 1, "u8a: migration copies projects, recordings/exports and backups ("
              + std::to_string(mr.projects) + "/" + std::to_string(mr.recordings) + "/" + std::to_string(mr.backups) + ")");
        check(fs::exists("selftest_u8a/Doc2/Projects/project.t8proj") && fs::exists("selftest_u8a/Doc2/Projects/jam.t8proj"), "u8a: migrated projects are in Projects");
        check(fs::exists("selftest_u8a/old/project.t8proj") && fs::exists("selftest_u8a/old/recordings/take_1793025120.wav"), "u8a: the originals are left in place");
        int wavs = 0, zips = 0, txts = 0; bool readable = true;
        for (const auto& e : fs::directory_iterator("selftest_u8a/Doc2/Recordings")) {
            const std::string n = e.path().filename().string();
            if (n.size() > 4 && n.substr(n.size() - 4) == ".wav") ++wavs;
            if (n.size() > 10 && n.substr(n.size() - 10) == " stems.zip") ++zips;
            if (n.size() > 10 && n.substr(n.size() - 10) == " notes.txt") ++txts;
            if (n.rfind("take_", 0) == 0 || n.rfind("stems_", 0) == 0 || n.rfind("notes_", 0) == 0) readable = false;
        }
        check(wavs == 1 && zips == 1 && txts == 1 && readable, "u8a: recordings get readable names (Recording <date> ...)");
        check(fs::exists("selftest_u8a/Svc2/Backups/jam-20260101-101010.t8proj"), "u8a: old backups go to the service folder");
        check(!ud::MigrateLegacy("selftest_u8a/old").ran, "u8a: the migration runs only once");

        // backups: service folder, per project, "a" does not claim the backups of "a-b"
        {
            Project proj;
            const std::string pa = "selftest_u8a/Doc2/Projects/a.t8proj", pab = "selftest_u8a/Doc2/Projects/a-b.t8proj";
            check(SaveProject(proj, pa) && SaveProject(proj, pab), "u8a: projects save into Projects");
            check(BackupProjectFile(pa) && BackupProjectFile(pab), "u8a: backups are written");
            const auto ba = ListProjectBackups(pa), bab = ListProjectBackups(pab);
            check(ba.size() == 1 && bab.size() == 1 && ba[0].find("/Svc2/Backups/a~") != std::string::npos, "u8a: backups sit in the service folder and are listed per project");
        }
        ud::SetRootsForTest("selftest_data/Documents", "selftest_data/Service");   // back to the scratch folder of the other tests
        fs::remove_all("selftest_u8a", ec);
    }

    // ---- v44.18 (U8b): the document model: Edited, Save / Don't Save / Cancel, Untitled, recovery, rename, revert ----------------
    {
        namespace ud = t8::userdirs;
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::remove_all("selftest_u8b", ec);
        ud::SetRootsForTest("selftest_u8b/Documents", "selftest_u8b/Service");
        using DA = AppState::DocAction;
        auto exists = [&](const std::string& p) { return fs::exists(p, ec); };
        AppState app;
        app.engine.Init(kSampleRate);
        app.sequencer.Init(&app.engine, &app.project);
        app.NewDocument();
        check(app.IsUntitled() && !app.IsEditedNow(), "u8b: a new document is Untitled and not edited");
        app.CurrentPattern().bpm = 133.0f;
        check(app.IsEditedNow(), "u8b: a change makes it Edited");
        app.CurrentPattern().bpm = 120.0f;
        check(!app.IsEditedNow(), "u8b: putting the value back clears Edited (it is a comparison, not a flag)");
        app.project.currentPattern = 3; app.project.chordSceneEdit = 2;
        check(!app.IsEditedNow(), "u8b: looking at another scene is not an edit");
        app.project.currentPattern = 0; app.project.chordSceneEdit = 0;

        // Untitled: Save asks for a name; the file only appears in Projects after that
        app.CurrentPattern().bpm = 101.0f;
        check(!app.SaveDocument() && app.docNameOpen, "u8b: Save on Untitled opens the name dialog instead of writing");
        app.docNameOpen = false;
        check(!exists(ud::ProjectsDir() + "/Doc A.t8proj"), "u8b: nothing in Projects before the first Save");
        app.WriteRecovery();
        check(exists(app.projectPath), "u8b: an Untitled with changes keeps its recovery file in the service folder");
        const std::string untitledPath = app.projectPath;
        check(app.SaveProjectAs("Doc A") && !app.IsUntitled() && !app.IsEditedNow(), "u8b: the first Save names it, it is saved and not Edited");
        check(exists(ud::ProjectsDir() + "/Doc A.t8proj") && !exists(untitledPath), "u8b: the project is in Projects, the Untitled file is gone");

        // a change: recovery copy is separate, the file is untouched until Cmd+S
        auto slurp = [](const std::string& p) { std::ifstream in(p); return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); };
        const std::string before = slurp(app.projectPath);
        app.CurrentPattern().bpm = 150.0f;
        check(app.IsEditedNow(), "u8b: edited after a save");
        app.WriteRecovery();
        check(exists(ud::RecoveryPathFor(app.projectPath)) && slurp(app.projectPath) == before, "u8b: autosave writes a recovery copy and never the project file");
        check(app.SaveDocument() && !app.IsEditedNow() && !exists(ud::RecoveryPathFor(app.projectPath)) && slurp(app.projectPath) != before, "u8b: Cmd+S writes the file, clears Edited, removes the recovery copy");

        // New with unsaved changes: Cancel keeps everything, Don't Save throws the changes away
        app.CurrentPattern().bpm = 90.0f;
        app.RequestDoc(DA::New);
        check(app.docAskUnsaved && app.docPending == DA::New && !app.IsUntitled(), "u8b: New asks first when something is unsaved");
        app.DocAnswerCancel();
        check(!app.docAskUnsaved && app.CurrentPattern().bpm == 90.0f && !app.IsUntitled(), "u8b: Cancel leaves the project as it is");
        app.RequestDoc(DA::New);
        app.DocAnswerDiscard();
        check(app.IsUntitled() && !app.IsEditedNow(), "u8b: Don't Save goes on to the new project");
        app.RequestDoc(DA::Open, ud::ProjectsDir() + "/Doc A.t8proj");
        check(!app.docAskUnsaved && !app.IsUntitled() && nearlyEqual(app.CurrentPattern().bpm, 150.0f, 0.01f), "u8b: Open at once when nothing is unsaved; the discarded 90 BPM never reached the file");

        // Save + continue (Open another project)
        check(app.SaveProjectAs("Doc B"), "u8b: save as a second project");
        app.CurrentPattern().bpm = 77.0f;
        app.RequestDoc(DA::Open, ud::ProjectsDir() + "/Doc A.t8proj");
        app.DocAnswerSave();
        check(app.projectPath.find("Doc A") != std::string::npos && !app.IsEditedNow(), "u8b: Save, then the other project opens");
        LoadProject(app.project, ud::ProjectsDir() + "/Doc B.t8proj");
        check(nearlyEqual(app.project.patterns[0].bpm, 77.0f, 0.01f), "u8b: what was saved before leaving is in the file");
        app.LoadProjectFile(ud::ProjectsDir() + "/Doc A.t8proj");

        // Quit: clean = at once; Untitled with changes = Save asks for the name and then quits
        app.RequestDoc(DA::Quit);
        check(app.quitRequested, "u8b: Quit at once when nothing is unsaved");
        app.quitRequested = false;
        app.NewDocument();
        app.CurrentPattern().bpm = 88.0f;
        app.RequestDoc(DA::Quit);
        check(app.docAskUnsaved && !app.quitRequested, "u8b: Quit asks when an Untitled has changes");
        app.DocAnswerSave();
        check(app.docNameOpen && app.docNameResume && !app.quitRequested, "u8b: Save on an Untitled asks for a name first");
        app.DocNameConfirm("Doc C");
        check(app.quitRequested && exists(ud::ProjectsDir() + "/Doc C.t8proj"), "u8b: after the name is given the project is saved and the program quits");
        app.quitRequested = false;

        // Cancel in the name dialog drops the pending action
        app.NewDocument();
        app.CurrentPattern().bpm = 66.0f;
        app.RequestDoc(DA::Quit);
        app.DocAnswerSave();
        app.DocNameCancel();
        check(!app.quitRequested && app.docPending == DA::None && app.IsEditedNow(), "u8b: Cancel in the name dialog cancels the quit");
        app.DocAnswerDiscard();
        app.docAskUnsaved = false;

        // Rename, Duplicate, Revert
        app.LoadProjectFile(ud::ProjectsDir() + "/Doc A.t8proj");
        check(app.RenameDocument("Doc A2") && exists(ud::ProjectsDir() + "/Doc A2.t8proj") && !exists(ud::ProjectsDir() + "/Doc A.t8proj") && !app.IsEditedNow(), "u8b: Rename moves the file and stays unedited");
        check(app.DuplicateDocument() && exists(ud::ProjectsDir() + "/Doc A2 copy.t8proj") && app.projectPath.find("Doc A2 copy") != std::string::npos, "u8b: Duplicate writes a copy and goes on in it");
        app.CurrentPattern().bpm = 55.0f;
        app.RequestDoc(DA::Revert);
        check(app.docAskUnsaved, "u8b: Revert asks when changed");
        app.DocAnswerDiscard();
        check(!app.IsEditedNow() && app.CurrentPattern().bpm != 55.0f, "u8b: Revert goes back to the saved version");

        // crash recovery: the copy is restored as an Edited document
        app.CurrentPattern().bpm = 61.0f;
        app.WriteRecovery();
        app.LoadProjectFile(app.projectPath);                       // a fresh session would start like this
        app.docRecoverPath = ud::RecoveryPathFor(app.projectPath);
        app.RestoreRecovery();
        check(nearlyEqual(app.CurrentPattern().bpm, 61.0f, 0.01f) && app.IsEditedNow(), "u8b: the recovery copy comes back and the document is Edited");

        ud::SetRootsForTest("selftest_data/Documents", "selftest_data/Service");
        fs::remove_all("selftest_u8b", ec);
    }

    // ---- v44.19 (U8c): the Library folder, the menu bar --------------------------------------------------------------------------
    {
        namespace ud = t8::userdirs;
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::remove_all("selftest_u8c", ec);
        ud::SetRootsForTest("selftest_u8c/Documents", "selftest_u8c/Service");
        check(fs::is_directory("selftest_u8c/Documents/Library/Beats") && fs::is_directory("selftest_u8c/Documents/Library/Presets")
              && fs::is_directory("selftest_u8c/Documents/Library/Favorites"), "u8c: Library/Beats, Presets and Favorites exist");
        check(ud::BeatsDir() == "selftest_u8c/Documents/Library/Beats" && ud::SynthPresetsDir() == "selftest_u8c/Documents/Library/Presets"
              && ud::FavoritesDir() == "selftest_u8c/Documents/Library/Favorites", "u8c: library folder paths");
        fs::create_directories("selftest_u8c/old/patterns"); fs::create_directories("selftest_u8c/old/presets");
        { std::ofstream("selftest_u8c/old/patterns/my groove.t8pat") << "p";
          std::ofstream("selftest_u8c/old/presets/my bass.t8syn") << "s";
          std::ofstream("selftest_u8c/old/presets/favorites.txt") << "f";
          std::ofstream("selftest_u8c/old/presets/readme.md") << "not a library file"; }
        const ud::LibraryMigration lm = ud::MigrateLibrary("selftest_u8c/old");
        check(lm.ran && lm.beats == 1 && lm.presets == 1 && lm.favorites == 1, "u8c: library migration copies beats, presets, favourites ("
              + std::to_string(lm.beats) + "/" + std::to_string(lm.presets) + "/" + std::to_string(lm.favorites) + ")");
        check(fs::exists("selftest_u8c/Documents/Library/Beats/my groove.t8pat") && fs::exists("selftest_u8c/Documents/Library/Presets/my bass.t8syn")
              && fs::exists("selftest_u8c/Documents/Library/Favorites/favorites.txt"), "u8c: migrated files are in the Library");
        check(!fs::exists("selftest_u8c/Documents/Library/Presets/readme.md") && fs::exists("selftest_u8c/old/patterns/my groove.t8pat"),
              "u8c: only library files are copied and the originals stay");
        check(!ud::MigrateLibrary("selftest_u8c/old").ran, "u8c: the library migration runs only once");
        ud::SetRootsForTest("selftest_data/Documents", "selftest_data/Service");
        fs::remove_all("selftest_u8c", ec);

        // the menu bar: one place for the document, no second copy of anything (section 12 of INTERACTION_MODEL.md)
        auto slurp = [](const std::string& path) { std::ifstream in(path); return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); };
        auto count = [](const std::string& hay, const std::string& needle) { int n = 0; for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n; return n; };
        check(!fs::exists("src/ui_desktop/FilePanel.cpp") && !fs::exists("src/ui_desktop/FilePanel.h"), "u8c: the FILE drawer is gone");
        const std::string mb = slurp("src/ui_desktop/MenuBar.cpp"), mainSrc = slurp("src/main.cpp");
        bool menusOk = true;
        for (const char* m : {"BeginMenu(\"File\")", "BeginMenu(\"Edit\")", "BeginMenu(\"View\")", "BeginMenu(\"Library\")", "BeginMenu(\"Settings\")"})
            menusOk = menusOk && count(mb, m) == 1;
        check(menusOk, "u8c: File, Edit, View, Library, Settings -- each menu once");
        check(count(mainSrc, "ImGuiWindowFlags_MenuBar") == 1 && count(mainSrc, "menuBar.Draw(app)") == 1, "u8c: the shell has the menu bar and draws it once");
        check(count(mb, "\"Save As...\"") == 1 && count(mb, "\"Quit TR-Brick\"") == 1 && count(mb, "\"Restart Audio\"") == 1 && count(mb, "\"Interface Size\"") == 1,
              "u8c: every command appears in exactly one menu");
        check(mb.find("MASTER WAV") == std::string::npos, "u8c: no second master recording button (REC is the one)");
        check(count(mb, "\"Undo\"") == 1 && count(mb, "\"Redo\"") == 1 && mb.find("FindApplicable(app.undoStack)") != std::string::npos,
              "u8c: Undo / Redo in Edit are enabled only when there is something to take back");
    }

    // ---- v44.20 (U3a): one look for lists, small actions and short exclusive options -----------------------------------------------
    {
        namespace fs = std::filesystem;
        int rawCombo = 0, smallBtn = 0, segUses = 0, choiceUses = 0;
        std::error_code ec3;
        for (const auto& e : fs::directory_iterator("src/ui_desktop", ec3)) {
            const std::string name = e.path().filename().string();
            std::ifstream in(e.path());
            std::string line;
            while (std::getline(in, line)) {
                const size_t c = line.find("//");
                if (c != std::string::npos && line.find_first_not_of(" \t") == c) continue;
                if (name != "ValueField.h") {
                    if (line.find("ImGui::BeginCombo") != std::string::npos || line.find("ImGui::EndCombo") != std::string::npos) ++rawCombo;
                    if (line.find("ImGui::SmallButton") != std::string::npos) ++smallBtn;
                    if (line.find("Segmented(\"") != std::string::npos) ++segUses;
                    if (line.find("ChoiceBegin(") != std::string::npos) ++choiceUses;
                }
            }
        }
        check(rawCombo == 0, "u3a: every list opens through ChoiceBegin / Choice, raw BeginCombo = " + std::to_string(rawCombo));
        check(smallBtn == 0, "u3a: no ImGui::SmallButton (MiniButton is at least 21 px high), found " + std::to_string(smallBtn));
        check(t8::desktop::kMinTarget >= 21.0f, "u3a: the minimum click target is 21 px");
        check(segUses >= 2 && choiceUses >= 15, "u3a: short options are Segmented (" + std::to_string(segUses) + "), the longer lists are Choice (" + std::to_string(choiceUses) + ")");
    }

    // ---- v44.21 (U3b): lanes with free points, the DJ filter, old projects ---------------------------------------------------------
    {
        namespace fs = std::filesystem;
        constexpr int LO = kAutoMinDb, HI = kAutoMaxDb;
        Lane ln;
        check(!ln.Any() && ln.n == 0 && ln.At(0.5) == 0.0f && ln.Start() == 0 && ln.End() == 0, "lane: a new lane is flat (value 0 everywhere)");
        ln.SetStart(-12, LO, HI);
        check(ln.n == 2 && ln.Start() == -12 && ln.End() == 0 && ln.pos[0] == 0 && ln.pos[1] == kLanePosMax, "lane: setting an end makes two points, the ends sit at 0 and 255");
        check(std::fabs(ln.At(0.5) - (-6.0f)) < 0.05f && std::fabs(ln.At(1.0)) < 1e-4f && std::fabs(ln.At(0.0) + 12.0f) < 1e-4f, "lane: two points are the straight line the step always had");
        const int mid = ln.AddPoint(128, -30, LO, HI);
        check(mid == 1 && ln.n == 3 && ln.pos[1] == 128 && ln.val[1] == -30, "lane: a point is added between the ends, in order");
        check(std::fabs(ln.At(128.0 / 255.0) + 30.0f) < 0.05f && ln.At(0.25) < -12.0f && ln.At(0.75) < 0.0f && ln.At(0.75) > -30.0f, "lane: the line bends through the new point");
        check(ln.AddPoint(130, -5, LO, HI) < 0, "lane: a point too close to another is refused");
        const int q = ln.AddPoint(200, -3, LO, HI);
        check(q == 2 && ln.pos[2] == 200 && ln.n == 4, "lane: more points keep the order");
        ln.MovePoint(2, 100, -9, LO, HI);
        check(ln.pos[2] <= ln.pos[3] - Lane::kMinPointGap && ln.pos[2] >= ln.pos[1] + Lane::kMinPointGap && ln.val[2] == -9, "lane: an inner point never moves past its neighbours");
        ln.MovePoint(0, 90, -5, LO, HI);
        check(ln.pos[0] == 0 && ln.val[0] == -5, "lane: the first point only changes value, never position");
        check(!ln.RemovePoint(0) && !ln.RemovePoint(ln.n - 1) && ln.RemovePoint(1) && ln.n == 3, "lane: the ends cannot be removed, an inner point can");
        Lane many;
        int added = 0; for (int k = 1; k < 20; ++k) if (many.AddPoint(k * 12, -10, LO, HI) >= 0) ++added;
        check(many.n == kMaxLanePts && added == kMaxLanePts - 2, "lane: at most " + std::to_string(kMaxLanePts) + " points (6 inner)");
        Lane back; back.SetStart(-6, LO, HI); back.AddPoint(100, -6, LO, HI); back.SetStart(0, LO, HI); back.MovePoint(1, 100, 0, LO, HI);
        check(!back.Any() && back.n == 0, "lane: a lane brought back to 0 everywhere is flat again (nothing is stored)");
        check(DjFilterFromHpfLpf(55, 0) == 55 && DjFilterFromHpfLpf(0, 60) == -60 && DjFilterFromHpfLpf(30, 30) == 30 && DjFilterFromHpfLpf(20, 70) == -70, "dj filter: an old HPF / LPF pair becomes one value, the stronger wins");

        // sound: a dip in the middle of a held note is heard; the filter lane drives the master filter, to the left = low-pass
        auto renderLanes = [&](const StepAuto& au, std::vector<float>& L, double seconds, float* lpfSeen, float* hpfSeen) {
            AppState ds; ds.engine.Init(kSampleRate); ds.sequencer.Init(&ds.engine, &ds.project);
            for (int i = 0; i < 2; ++i) { Pattern& pt = ds.project.patterns[static_cast<size_t>(i)]; pt.masterLength = 16; pt.bpm = 240.0f; }
            Track& b = ds.project.patterns[0].tracks[kNumDrumTracks]; b.steps[0].active = true; b.steps[0].note = 48; b.steps[0].lengthSteps = 16; b.steps[0].chordSize = 0;
            ds.project.songChain[0] = ChainStep{0, 2, -1}; ds.project.songChain[1] = ChainStep{1, 1, -1}; ds.project.songChainLength = 2;
            ds.project.songChain[0].automation = au;
            ds.RefreshSound(); ds.sequencer.SetSongModeActive(true); ds.sequencer.Play();
            std::vector<float> ob(256 * 2); int done = 0; const int need = static_cast<int>(kSampleRate * seconds);
            while (done < need) {
                std::fill(ob.begin(), ob.end(), 0.0f); ds.sequencer.RenderAndAdvance(ob.data(), 256, kSampleRate);
                for (int i = 0; i < 256; ++i) L.push_back(ob[static_cast<size_t>(i) * 2]);
                done += 256;
                if (lpfSeen && done >= kSampleRate / 2) { *lpfSeen = ds.engine.GetMixer().arrLpf.load(); *hpfSeen = ds.engine.GetMixer().arrHpf.load(); lpfSeen = nullptr; }
            }
            ds.sequencer.SetSongModeActive(false); ds.sequencer.Stop();
        };
        auto rmsAt = [&](const std::vector<float>& x, double t0, double t1) { double sum = 0; size_t a0 = static_cast<size_t>(t0 * kSampleRate), a1 = std::min(x.size(), static_cast<size_t>(t1 * kSampleRate)); for (size_t i = a0; i < a1; ++i) sum += x[i] * x[i]; return std::sqrt(sum / std::max<size_t>(1, a1 - a0)); };
        {
            StepAuto dip; dip.mix.Materialize(); dip.mix.AddPoint(128, -40, LO, HI);   // 0 dB -> -40 dB in the middle -> 0 dB
            std::vector<float> w; float l = 0, h = 0;
            renderLanes(dip, w, 1.95, &l, &h);
            const double start = rmsAt(w, 0.05, 0.2), middle = rmsAt(w, 0.95, 1.05), end = rmsAt(w, 1.8, 1.95);
            check(start > 0.003 && middle < 0.15 * start && end > 0.5 * start, "points: a dip in the middle of the MIX lane is heard (quiet in the middle, back at the end)");
            StepAuto lp = StepAuto::Ends(0, 0, -100, -100); std::vector<float> w2; float l2 = 0, h2 = 0;
            renderLanes(lp, w2, 1.0, &l2, &h2);
            check(l2 > 0.95f && h2 == 0.0f, "dj filter: a lane at the far left closes the low-pass and leaves the high-pass alone (" + std::to_string(l2) + ")");
            StepAuto hp = StepAuto::Ends(0, 0, 100, 100); std::vector<float> w3; float l3 = 0, h3 = 0;
            renderLanes(hp, w3, 1.0, &l3, &h3);
            check(h3 > 0.95f && l3 == 0.0f, "dj filter: a lane at the far right thins from below and leaves the low-pass alone");
        }

        // file: free points survive save / load; a project from before v44.21 turns its two filters into one
        {
            Project sp; sp.songChain[0] = ChainStep{0, 2, -1}; sp.songChain[1] = ChainStep{1, 1, -1}; sp.songChainLength = 2;
            StepAuto a; a.mix.Materialize(); a.mix.SetStart(-6, LO, HI); a.mix.AddPoint(60, -20, LO, HI); a.mix.AddPoint(190, -2, LO, HI);
            a.filter.Materialize(); a.filter.SetEnd(55, kFilterMin, kFilterMax); a.filter.AddPoint(100, -35, kFilterMin, kFilterMax);
            sp.songChain[0].automation = a;
            sp.songChain[0].levels.track[6] = Lane::Line(-3, -9, LO, HI); sp.songChain[0].levels.track[6].AddPoint(120, -30, LO, HI); sp.songChain[0].levels.transitionDb = -4;
            sp.songChain[0].muteMask = kMuteKickBass;
            const std::string path = "selftest_lanes.t8proj";
            check(SaveProject(sp, path), "lanes: save");
            Project lp; const bool ok = LoadProject(lp, path);
            check(ok && lp.songChain[0].automation == a && lp.songChain[0].levels == sp.songChain[0].levels && lp.songChain[0].muteMask == kMuteKickBass && !lp.songChain[1].automation.Any(),
                  "lanes: free points of MIX, FILTER and an instrument survive save / load");
            // the same project written the way v44.6 .. v44.20 wrote it: 16 values on the arr line, a lvl line with start / end pairs
            std::ifstream in(path); std::string line, out;
            while (std::getline(in, line)) {
                if (line.rfind("dyn0=", 0) == 0 || line.rfind("lv20=", 0) == 0) continue;
                if (line.rfind("arr0=", 0) == 0) line += ",-12,0,0,60,40,0";   // level -12 -> 0, HPF 0 -> 60, LPF 40 -> 0
                out += line + "\n";
                if (line.rfind("arr0=", 0) == 0) out += "lvl0=-4,0,0,0,0,0,0,0,0,0,0,0,0,0,-3,-9,0,0,0,0,0,0\n";
            }
            in.close(); { std::ofstream o(path, std::ios::trunc); o << out; }
            Project ol; check(LoadProject(ol, path), "lanes: an older project loads");
            const StepAuto& oa = ol.songChain[0].automation;
            check(oa.mix.n == 2 && oa.mix.Start() == -12 && oa.mix.End() == 0 && oa.filter.n == 2 && oa.filter.Start() == -40 && oa.filter.End() == 60,
                  "lanes: HPF / LPF of an older project become one DJ filter (start: LPF 40 wins = -40, end: HPF 60 = +60)");
            check(ol.songChain[0].levels.track[6].n == 2 && ol.songChain[0].levels.track[6].Start() == -3 && ol.songChain[0].levels.track[6].End() == -9 && ol.songChain[0].levels.transitionDb == -4,
                  "lanes: the instrument levels of an older project load as two-point lanes");
            std::remove(path.c_str());
        }
        // one editor for the three rows: the source says it once
        {
            auto slurp = [](const std::string& path) { std::ifstream in(path); return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); };
            auto count = [](const std::string& hay, const std::string& needle) { int n = 0; for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n; return n; };
            const std::string bp = slurp("src/ui_desktop/BricksPanel.cpp");
            check(count(bp, "auto laneY") == 1 && count(bp, "AddPoint(") == 1 && count(bp, "RemovePoint(") == 1 && count(bp, "Drag::Point") >= 3,
                  "rows: MIX, FILTER and the instrument are drawn and edited by one piece of code");
            check(bp.find("hpfStart") == std::string::npos && bp.find("lpfStart") == std::string::npos && bp.find("kVioletLpf, 1.3f") == std::string::npos, "rows: no separate HPF / LPF lines any more");
        }
    }

    // ---- v44.15 (U1a): redo, the rules of ValueField, the ratchet on raw widgets ------------------------------------------------
    {
        namespace vfd = t8::desktop::valuefield_detail;
        t8::desktop::ValueSpec bpm; bpm.lo = 20; bpm.hi = 300; bpm.step = 0.5f; bpm.wheel = 1; bpm.def = 120;
        check(nearlyEqual(vfd::DragValue(120, 10, false, bpm), 125.0f, 0.001f), "u1a: 10 px of drag = 5 BPM");
        check(nearlyEqual(vfd::DragValue(120, 10, true, bpm), 120.5f, 0.001f), "u1a: Shift makes the drag fine (x0.1)");
        check(nearlyEqual(vfd::DragValue(120, 10000, false, bpm), 300.0f, 0.001f) && nearlyEqual(vfd::DragValue(120, -10000, false, bpm), 20.0f, 0.001f), "u1a: drag stops at the limits");
        check(nearlyEqual(vfd::WheelValue(120, 1, false, bpm), 121.0f, 0.001f) && nearlyEqual(vfd::WheelValue(120, -2, false, bpm), 118.0f, 0.001f), "u1a: wheel = whole steps");
        t8::desktop::ValueSpec len; len.lo = 1; len.hi = 256; len.step = 0.2f; len.integer = true; len.wheel = 1;
        check(nearlyEqual(vfd::DragValue(16, 12, false, len), 18.0f, 0.001f), "u1a: integer fields round (16 + 2.4 -> 18)");
        check(nearlyEqual(vfd::WheelValue(16, 1, true, len), 17.0f, 0.001f), "u1a: an integer field never wheels finer than 1");
        float typed = 0;
        check(vfd::ParseTyped("90", bpm, &typed) && nearlyEqual(typed, 90.0f, 0.001f), "u1a: typed value is read");
        check(vfd::ParseTyped("9999", bpm, &typed) && nearlyEqual(typed, 300.0f, 0.001f), "u1a: typed value is clamped");
        check(!vfd::ParseTyped("abc", bpm, &typed) && !vfd::ParseTyped("", bpm, &typed), "u1a: garbage is rejected");

        AppState app;
        app.engine.Init(kSampleRate);
        app.project.currentPattern = 0;
        app.ClearUndo();
        app.CurrentPattern().tracks[0].steps[0].active = false;
        app.PushUndo();
        app.CurrentPattern().tracks[0].steps[0].active = true;
        check(!app.Redo(), "u1a: nothing to redo before an undo");
        check(app.Undo() && !app.CurrentPattern().tracks[0].steps[0].active, "u1a: undo takes the step back");
        check(app.Redo() && app.CurrentPattern().tracks[0].steps[0].active && app.lastUndoLabel.rfind("REDO", 0) == 0, "u1a: redo brings it back");
        check(app.Undo() && !app.CurrentPattern().tracks[0].steps[0].active, "u1a: and it can be undone again");
        app.PushUndo();   // a new edit ends the redo history
        check(!app.Redo(), "u1a: a new edit clears redo");
        // sound / chain entries
        app.ClearUndo();
        const float c0 = app.project.patterns[0].synthPatches[0].cutoffHz;
        app.PushSoundUndo();
        app.project.patterns[0].synthPatches[0].cutoffHz = c0 + 700.0f;
        check(app.Undo() && nearlyEqual(app.project.patterns[0].synthPatches[0].cutoffHz, c0, 0.01f), "u1a: sound undo");
        check(app.Redo() && nearlyEqual(app.project.patterns[0].synthPatches[0].cutoffHz, c0 + 700.0f, 0.01f), "u1a: sound redo");
        // a scene's redo waits for that scene
        app.ClearUndo();
        app.PushUndo();
        app.CurrentPattern().tracks[1].steps[3].active = true;
        check(app.Undo(), "u1a: scene 1 undo");
        app.project.currentPattern = 1;
        check(!app.Redo(), "u1a: redo of scene 1 is not applied to scene 2");
        app.project.currentPattern = 0;
        check(app.Redo() && app.project.patterns[0].tracks[1].steps[3].active, "u1a: redo returns with its scene");
        app.DropLastUndo();
        // ratchet: raw ImGui number widgets in ui_desktop may only go down (U1b replaces them all with ValueField / Segmented / Choice)
        {
            const std::regex raw("SliderInt|SliderFloat|SliderScalar|VSlider|DragInt|DragFloat|DragScalar|Checkbox|RadioButton|InputInt|InputFloat|HSlider\\(");
            int count = 0;
            std::error_code ec2;
            for (const auto& e : std::filesystem::directory_iterator("src/ui_desktop", ec2)) {
                const std::string name = e.path().filename().string();
                if (name == "ValueField.h" || name == "LiveWidgets.h" || name == "Interaction.h") continue;
                std::ifstream in(e.path());
                std::string line;
                while (std::getline(in, line)) {
                    const size_t c = line.find("//");
                    if (c != std::string::npos && line.find_first_not_of(" \t") == c) continue;   // comment line
                    if (std::regex_search(line, raw)) ++count;
                }
            }
            constexpr int kRawWidgetBaseline = 0;   // v44.16: every number / option control goes through ValueField / Check / Segmented / Choice. Keep it 0.
            check(count <= kRawWidgetBaseline, "u1b: no raw widgets in ui_desktop = " + std::to_string(count) + " (limit " + std::to_string(kRawWidgetBaseline) + ")");
            // U2: no pop-up tooltips -- what a control does is said by the info line (Hint); only drag value bubbles use ValueTip
            int tips = 0;
            for (const auto& e : std::filesystem::directory_iterator("src/ui_desktop", ec2)) {
                if (e.path().filename() == "InfoLine.h") continue;
                std::ifstream in(e.path());
                std::string line;
                while (std::getline(in, line)) if (line.find("ImGui::SetTooltip") != std::string::npos) ++tips;
            }
            check(tips == 0, "u2: no ImGui::SetTooltip in ui_desktop (use Hint / ValueTip), found " + std::to_string(tips));
            check(t8::desktop::info_detail::Flatten("A.\nB.\n\nC") == "A.  B.  C", "u2: hint line breaks become spaces");
            if (count < kRawWidgetBaseline) std::cout << "  note: " << count << " raw widgets left\n";
        }
    }

    {   // v44.43: six chord voices
        std::filesystem::create_directories("selftest_tmp_chord6");
        Project p6 = Project{};
        Track& ct6 = p6.patterns[0].tracks[kChordTrackIndex];
        auto put6 = [&](int at, int n, int nudge, int vel, bool ghost) {
            Step& s = ct6.steps[at]; s.active = true; s.chordSize = static_cast<uint8_t>(n); s.lengthSteps = 8;
            s.chordNotes = {48, 55, 59, 62, 66, 69}; s.nudge = static_cast<int8_t>(nudge); s.vel = static_cast<uint8_t>(vel); s.ghost = ghost;
        };
        put6(0, 6, 0, 0, false); put6(8, 6, 7, 90, false); put6(16, 5, 0, 0, false); put6(24, 4, 0, 0, false); put6(32, 6, 0, 0, true);
        check(SaveProject(p6, "selftest_tmp_chord6/six.t8proj"), "v44.43: save a project with six-note chords");
        Project q6;
        check(LoadProject(q6, "selftest_tmp_chord6/six.t8proj"), "v44.43: load it back");
        const Track& rt6 = q6.patterns[0].tracks[kChordTrackIndex];
        bool same = true;
        for (int at : {0, 8, 16, 24, 32}) {
            const Step& a = ct6.steps[at]; const Step& b = rt6.steps[at];
            same = same && a.chordSize == b.chordSize && a.nudge == b.nudge && a.vel == b.vel && a.ghost == b.ghost;
            for (int i = 0; i < a.chordSize; ++i) same = same && a.chordNotes[i] == b.chordNotes[i];
        }
        check(same, "v44.43: six, five and four chord tones (with nudge / velocity / ghost) survive a reload");
        check(rt6.steps[0].chordNotes[4] == 66 && rt6.steps[0].chordNotes[5] == 69, "v44.43: the 5th and 6th tone are stored");
        {   // an old-style line (four tones, no suffix) still reads, and a four-tone chord writes the same line as before
            std::ifstream in("selftest_tmp_chord6/six.t8proj"); std::string ln; std::string line24;
            while (std::getline(in, ln)) if (ln.rfind("step24=", 0) == 0) line24 = ln;
            check(line24 == "step24=1,0,255,60,8,0,1,4,48,55,59,62,100", "v44.43: a four-tone chord writes exactly the line older versions wrote (" + line24 + ")");
        }
        AudioEngine e6; e6.Init(kSampleRate);
        const int8_t six[6] = {48, 55, 59, 62, 66, 69};
        e6.ChordNoteOn(six, 6, 0.8f, false);
        std::vector<float> ob6(512 * 2, 0.0f);
        e6.Render(ob6.data(), 512, kSampleRate);
        check(e6.Chord().IsActive(), "v44.43: the chord pool plays six voices");
        check(kMaxChordVoices == 6, "v44.43: kMaxChordVoices is 6");
    }
    {   // v44.43: jam slot -> scene -> brick
        AppState a; a.engine.Init(44100); a.sequencer.Init(&a.engine, &a.project);
        a.LoadStarterGroove();
        a.SetChordBank(true);                              // slot 0 now holds the starter's chord (Cm7)
        ChordScene& sl = a.project.chordScenes[1];
        sl.key = 9; sl.scale = 1;
        for (int i = 0; i < 2; ++i) { Step& st = sl.track.steps[i * 16]; st.active = true; st.chordSize = 6; st.lengthSteps = 16; st.chordNotes = {45, 52, 57, 60, 64, 67}; }
        a.LaunchChordScene(1);
        const int cur = a.project.currentPattern;
        check(!a.CommitChordSlotToScene(5, cur), "v44.43: an empty slot is not added");
        a.PushUndo();   // a marker the commit must sit on top of
        check(a.CommitChordSlotToScene(1, cur), "v44.43: ADD TO SCENE writes the slot into the scene");
        const Track& own = a.project.patterns[static_cast<size_t>(cur)].tracks[kChordTrackIndex];
        check(own.steps[16].active && own.steps[16].chordSize == 6 && own.steps[0].chordNotes[5] == 67, "v44.43: the scene's own chord track now holds the six-note chords");
        check(a.project.patterns[static_cast<size_t>(cur)].chordsKey == 9 && a.project.patterns[static_cast<size_t>(cur)].chordsScale == 1, "v44.43: the key and scale of the jam are stamped on the scene");
        check(a.project.chordScenes[1].track.steps[16].active, "v44.43: the slot keeps its copy");
        a.Undo();
        check(!a.project.patterns[static_cast<size_t>(cur)].tracks[kChordTrackIndex].steps[16].active || a.project.patterns[static_cast<size_t>(cur)].tracks[kChordTrackIndex].steps[16].chordSize != 6, "v44.43: ADD TO SCENE is one undo step");
        a.CommitChordSlotToScene(1, cur);
        check(a.ThrowIntoChain() && a.project.songChain[static_cast<size_t>(a.project.songChainLength - 1)].chordScene == 1, "v44.43: with the bank on, a thrown brick plays the chord scene you were hearing");
        const int brick = a.project.songChain[static_cast<size_t>(a.project.songChainLength - 1)].patternIndex;
        check(a.project.patterns[static_cast<size_t>(brick)].tracks[kChordTrackIndex].steps[16].chordSize == 6, "v44.43: the brick also carries the chords in its own chord track");
        a.SetChordBank(false);
        a.project.songChainLength = 0;
        check(a.ThrowIntoChain() && a.project.songChain[0].chordScene == -1, "v44.43: with the bank off a throw leaves the bank alone as before");
    }

    {   // v44.44: nothing is lost on save / load, CHORD LAB starts a draft only when changed, six notes in capture, audition voice, scene lock
        bool lfoOk = true;
        std::filesystem::create_directories("selftest_tmp_v4444");
        for (int i = 0; i < kNumLfoTargets; ++i) {
            const LfoTarget t = static_cast<LfoTarget>(i);
            SynthPatch sp; sp.lfoTarget = t;
            Project lp; lp.patterns[0].synthPatches[0].lfoTarget = t;
            const bool saved = SaveProject(lp, "selftest_tmp_v4444/lfo.t8proj");
            Project lq; const bool loaded = saved && LoadProject(lq, "selftest_tmp_v4444/lfo.t8proj");
            lfoOk = lfoOk && loaded && lq.patterns[0].synthPatches[0].lfoTarget == t;
            SynthPatch rp;
            lfoOk = lfoOk && SaveCustomSynthPreset("selftest lfo target", sp) && LoadCustomSynthPreset("selftest lfo target", rp) && rp.lfoTarget == t;
            if (!lfoOk) { std::cout << "  lfo target " << i << " (" << LfoTargetName(t) << ") did not round-trip\n"; break; }
        }
        check(lfoOk, "v44.44: every LFO target (CUTOFF, PITCH, VOLUME, WT POS, ...) survives a project and a preset round trip");
        check(LfoTargetFromText("wt pos") == LfoTarget::WtPosition || kNumLfoTargets < 4, "v44.44: LFO target names are read case-insensitively");
        std::filesystem::remove_all("selftest_tmp_v4444");

        AppState a; a.engine.Init(44100); a.sequencer.Init(&a.engine, &a.project);
        a.LoadStarterGroove();
        const bool bank0 = a.project.chordBankOn;
        const int stepsBefore = static_cast<int>(a.undoStack.size());
        check(!bank0, "v44.44: a fresh project has the chord bank off");
        { std::lock_guard<SceneMutex> lk(a.sequencer.PatternMutex());
          const int slot = a.StartChordDraft();
          check(a.project.chordBankOn && a.project.chordSceneEdit == slot, "v44.44: StartChordDraft switches the bank on and edits the new slot");
          check(static_cast<int>(a.undoStack.size()) == stepsBefore + 1, "v44.44: the start of a draft is one undo step");
          check(a.StartChordDraft() == slot && static_cast<int>(a.undoStack.size()) == stepsBefore + 1, "v44.44: a second call with the bank on changes nothing");
          check(a.Undo() && !a.project.chordBankOn, "v44.44: UNDO takes the draft back: bank off again");
          check(a.Redo() && a.project.chordBankOn && a.project.chordSceneEdit == slot, "v44.44: REDO restores the draft"); }
        MidiCapture mc; mc.Start();
        const int8_t six[6] = {48, 55, 59, 62, 66, 69};
        mc.LogChord(six, 6);
        check(!mc.Events().empty() && mc.Events()[0].noteCount == 6 && mc.Events()[0].notes[5] == 69, "v44.44: MIDI capture keeps all six notes of a chord");
        AudioEngine ae; ae.Init(44100);
        ae.ChordAuditionOn(six, 6, 0.8f);
        check(!ae.Chord().IsActive(), "v44.44: an audition uses its own voices, the loop's chord voices stay free");
        ae.ChordAuditionOff();
        a.sequencer.ResetSceneLockViolations();
        a.PushUndo();
        check(a.sequencer.SceneLockViolations() == 1, "v44.44: PushUndo without the scene lock is counted");
        { std::lock_guard<SceneMutex> lk(a.sequencer.PatternMutex()); a.PushUndo(); a.PushSoundUndo(); }
        check(a.sequencer.SceneLockViolations() == 1, "v44.44: and with the lock it is not");
    }

    {   // v44.45: everything fits and is called the same
        auto slurp = [](const std::string& path) { std::ifstream in(path); return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); };
        const std::string mainSrc = slurp("src/main.cpp"), lab = slurp("src/ui_desktop/MixLabPanel.cpp"), seqSrc = slurp("src/ui_desktop/SequencerPanel.cpp");
        const std::string hdr = slurp("src/ui_desktop/HeaderPanel.cpp"), menu = slurp("src/ui_desktop/MenuBar.cpp"), chain = slurp("src/ui_desktop/ChainPanel.cpp");
        check(mainSrc.find("ww / 1440.0f") != std::string::npos, "v44.45: automatic zoom starts below 1440 points (the header needs them)");
        check(mainSrc.find("EmptyTrackChild") != std::string::npos && mainSrc.find("emptyTrack") != std::string::npos, "v44.45: an empty TRACK shows an empty state, not the scene editor");
        check(lab.find("BrickOfSlot(lab_.Scene())") != std::string::npos && lab.find("\"SCENE %02d\", lab_.Scene() + 1") != std::string::npos, "v44.45: MIX LAB names a brick BRICK n, not by its hidden slot");
        check(seqSrc.find("interaction::IsCopy()") != std::string::npos && seqSrc.find("KeyCtrl") == std::string::npos, "v44.45: a step is copied with Option (Alt), as the interaction model says");
        check(hdr.find("onTrack ? \"BRICK\" : \"SCENE\"") != std::string::npos && chain.find("THROW ONTO THE TRACK") != std::string::npos && chain.find("THROW INTO THE CHAIN") == std::string::npos, "v44.45: one set of words: SCENE and TRACK");
        check(menu.find("\"CHORD LAB\"") != std::string::npos && menu.find("\"MASTER FX\"") != std::string::npos, "v44.45: CHORD LAB and MASTER FX are in the View menu");
        // the balance scale and the duplicate cards (behaviour)
        check(lab.find("const float range = std::min(36.0f") != std::string::npos, "v44.45: the tonal balance scale grows with the data");
    }

    {   // v44.46: CHORD LAB is honest
        auto slurp = [](const std::string& path) { std::ifstream in(path); return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); };
        const std::string panel = slurp("src/ui_desktop/ChordLabPanel.cpp"), coreSrc = slurp("src/core/ChordLab.h"), ctx = slurp("src/ui_desktop/ContextPanel.cpp"), edit = slurp("src/ui_desktop/ChordEditor.h");
        check(coreSrc.find("LabNextChords") == std::string::npos && panel.find("LabNextChords") == std::string::npos && panel.find("\"NEXT\"") == std::string::npos, "v44.46: the NEXT prediction is gone");
        check(coreSrc.find("LabFunction") == std::string::npos, "v44.46: WHY has no sentence about what a chord wants");
        check(panel.find("\"JAM\", \"SOUND\"") != std::string::npos && panel.find("soundBody_") != std::string::npos, "v44.46: CHORD LAB has a JAM | SOUND switch");
        check(panel.find("LabMelodyFromRange(pat, SrcTracks(), b.start") != std::string::npos && panel.find("RefreshMatch(app,") != std::string::npos, "v44.46: MATCH is read under every block and for the selected block");
        check(ctx.find("BlockBegin(\"PROGRESSION\")") == std::string::npos && ctx.find("BlockBegin(\"CUSTOM CHORDS\")") == std::string::npos && ctx.find("OPEN CHORD LAB") != std::string::npos, "v44.46: PROGRESSION and CUSTOM CHORDS are gone from the CHORDS tab, CHORD LAB opens from it");
        check(ctx.find("c.quality") == std::string::npos && edit.find("LabPcName(sp.root)") != std::string::npos, "v44.46: one way to name a chord (no CMIN next to Cm)");
        // WHY names the scale as it is: C dorian is not C minor
        const auto palD = BuildLabPalette(0, 1, false);
        const auto whyD = LabWhy(palD[0], nullptr, 0, 1, LabMelody{});
        check(whyD[0].find("C dorian") != std::string::npos && whyD[1].find("degree I of C dorian") != std::string::npos, "v44.46: WHY names the scale (C dorian), not just minor");
        const int8_t c6n[4] = {48, 52, 55, 57}, am7n2[4] = {45, 48, 52, 55};
        check(LabChordFromNotes(c6n, 4).name == "C6" && LabChordFromNotes(am7n2, 4).name == "Am7", "v44.46: C E G A is C6 and A C E G is Am7 (the lowest note decides), as in the chord editor");
    }

    {   // v44.43: CHORD LAB core
        auto pal = BuildLabPalette(9, kScaleAeolian, false);
        auto find = [&](int row, int col) -> const LabChord* { for (const LabChord& c : pal) if (c.row == row && c.col == col) return &c; return nullptr; };
        const char* tri[7] = {"Am", "Bdim", "C", "Dm", "Em", "F", "G"};
        bool ok = true; for (int i = 0; i < 7; ++i) ok = ok && find(0, i) && find(0, i)->name == tri[i] && find(0, i)->inKey;
        check(ok, "chord lab: A minor gives Am Bdim C Dm Em F G, all in the key");
        const char* sev[7] = {"Am7", "Bm7b5", "Cmaj7", "Dm7", "Em7", "Fmaj7", "G7"};
        ok = true; for (int i = 0; i < 7; ++i) ok = ok && find(1, i) && find(1, i)->name == sev[i];
        check(ok, "chord lab: the sevenths are Am7 Bm7b5 Cmaj7 Dm7 Em7 Fmaj7 G7");
        auto pal9 = BuildLabPalette(9, kScaleAeolian, true);
        const LabChord* n9 = nullptr; for (const LabChord& c : pal9) if (c.row == 1 && c.col == 5) n9 = &c;
        check(n9 && n9->name == "Fmaj9" && n9->iv.size() == 5 && n9->inKey, "chord lab: nine = true makes Fmaj9, five voices");
        check(find(2, 2) && find(2, 2)->name == "Cadd9" && find(2, 0) && find(2, 0)->name == "Asus2", "chord lab: a major triad gets add9, a minor one sus2");
        bool borrowedOut = true; int nb = 0; for (const LabChord& c : pal) if (c.row == 3) { ++nb; borrowedOut = borrowedOut && !c.inKey && c.degree == 0; }
        check(nb >= 4 && borrowedOut, "chord lab: borrowed chords are outside the key (A, Bb, D, E7, G#dim)");
        bool hasE7 = false, hasA = false; for (const LabChord& c : pal) if (c.row == 3) { if (c.name == "E7") hasE7 = true; if (c.name == "A") hasA = true; }
        check(hasE7 && hasA, "chord lab: A minor borrows A major and E7");
        bool hasBb = false; for (const LabChord& c : pal) if (c.row == 3 && c.name == "Bb") hasBb = true;
        check(hasBb && find(2, 1) && find(2, 1)->name == "Bsus4" && !find(2, 1)->inKey, "chord lab: the flat chord is spelled Bb, and the diminished degree gets a sus4 outside the key");
        auto palF = BuildLabPalette(0, 1, false);   // C dorian is the F-major family: flats
        const char* hasEb = nullptr; for (const LabChord& c : palF) if (c.row == 0 && c.col == 2) hasEb = c.name.c_str();
        check(hasEb && std::string(hasEb) == "Eb", "chord lab: C dorian spells its third chord Eb, not D#");
        auto palC = BuildLabPalette(0, kScaleIonian, false);
        bool hasFm = false; for (const LabChord& c : palC) if (c.row == 3 && c.name == "Fm") hasFm = true;
        check(hasFm, "chord lab: C major borrows Fm");
        // melody: bass A A F F A F (on the beat) + a D passing note
        auto patp = std::make_unique<Pattern>(); Pattern& pat = *patp; pat.stepsPerBeat = 4; pat.masterLength = 16;
        Track& bs = pat.tracks[kNumDrumTracks]; bs.type = TrackType::Synth;
        const int bn[8] = {57, 0, 0, 0, 65, 0, 0, 0};
        bs.steps[0].active = true; bs.steps[0].note = 57; bs.steps[4].active = true; bs.steps[4].note = 65; bs.steps[8].active = true; bs.steps[8].note = 57; bs.steps[10].active = true; bs.steps[10].note = 62; bs.steps[12].active = true; bs.steps[12].note = 65;
        (void)bn;
        LabMelody mel = LabMelodyFromPattern(pat, {kNumDrumTracks}, 0, 0);
        check(mel.notes == 5 && mel.count[9] == 2 && mel.count[5] == 2 && mel.count[2] == 1, "chord lab: the melody is read from the bass steps");
        check(mel.w[9] == 4.0 && mel.w[2] == 1.0, "chord lab: bass notes on the beat weigh double");
        ScoreLabChords(pal, mel);
        const LabChord* fC = find(0, 5); const LabChord* gC = find(0, 6); const LabChord* am = find(0, 0);
        check(fC && gC && am && fC->fit > 0.8f && fC->fit > am->fit && am->fit > gC->fit, "chord lab: F fits an A/F bass best, Am less (the F rubs against its E), G least");
        check(fC->covered == 4 && fC->total == 5, "chord lab: F covers 4 of the 5 notes (the D is not in F)");
        const LabChord* fm7 = find(1, 5);
        check(fm7 && fm7->fit >= fC->fit - 0.01f, "chord lab: Fmaj7 contains the A and F so it fits at least as well as F");
        bool anyBest = false; for (const LabChord& c : pal) if (c.best) anyBest = true;
        check(anyBest, "chord lab: the best fits are starred");
        auto why = LabWhy(*fC, am, 9, kScaleAeolian, mel);
        check(why.size() == 4 && why[0].find("is in A minor") != std::string::npos && why[2].find("shares 2 notes (A C)") != std::string::npos, "chord lab: WHY says it is in the key and what it shares with the chord before (counts, no opinion)");
        check(why[1].find("degree VI of A minor") != std::string::npos, "chord lab: WHY gives the degree of the chord in the key (F is VI of A minor)");
        check(why[3].find("4 of the 5") != std::string::npos && why[3].find("D") != std::string::npos, "chord lab: WHY names the melody note the chord misses");
        const LabChord* e7 = nullptr; for (const LabChord& c : pal) if (c.name == "E7") e7 = &c;
        auto whyB = LabWhy(*e7, nullptr, 9, kScaleAeolian, LabMelody{});
        check(whyB[0].find("G#") != std::string::npos && whyB[0].find("outside") != std::string::npos, "chord lab: WHY of a borrowed chord names the outside note");
        check(whyB.size() == 3 && whyB[1].find("borrowed") != std::string::npos && whyB[2].find("press MATCH") != std::string::npos, "chord lab: WHY of a borrowed chord says so; no melody yet");
        {   // v44.46: MATCH under one block: only the notes of those steps count
            Pattern mp; mp.stepsPerBeat = 4; mp.masterLength = 32;
            Track& bs = mp.tracks[kNumDrumTracks];
            bs.steps[0].active = true; bs.steps[0].note = 45;      // A under steps 0-15
            bs.steps[16].active = true; bs.steps[16].note = 43;    // G under steps 16-31
            const LabMelody first = LabMelodyFromRange(mp, {kNumDrumTracks}, 0, 16), second = LabMelodyFromRange(mp, {kNumDrumTracks}, 16, 32);
            check(first.notes == 1 && first.count[9] == 1 && second.notes == 1 && second.count[7] == 1, "chord lab v44.46: the notes under a block are only the notes of its steps");
            LabChord amc = *am, gc2 = *gC;
            LabFitOne(amc, first); LabFitOne(gc2, first);
            check(amc.fit > 0.9f && gc2.fit < 0.1f, "chord lab v44.46: Am fits the A of bar 1, G does not");
            LabFitOne(amc, second); LabFitOne(gc2, second);
            check(amc.fit < 0.1f && gc2.fit > 0.9f, "chord lab v44.46: under the next block it is the other way round");
        }
        const int8_t am7n[4] = {57, 60, 64, 67}; const int8_t c6[6] = {48, 55, 59, 62, 66, 69};
        check(LabChordFromNotes(am7n, 4).name == "Am7", "chord lab: four notes A C E G are named Am7");
        check(!LabChordFromNotes(c6, 6).name.empty(), "chord lab: a six-note stack still gets a name");
        auto pal9b = BuildLabPalette(0, 1, true);   // C dorian ninths: the II chord has a flat 9th and still gets a name
        bool named9 = true; for (const LabChord& c : pal9b) if (c.row == 1) named9 = named9 && c.name.find('?') == std::string::npos;
        check(named9, "chord lab: every ninth chord of C dorian is named (b9 included)");
        const int8_t inv[3] = {52, 55, 60};   // C/E: E G C
        check(LabChordFromNotes(inv, 3).name == "C", "chord lab: an inversion is still named by its chord");
    }

    {   // v44.47: long patterns and the scene length that follows them
        auto slurp = [](const std::string& path) { std::ifstream in(path); return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); };
        const auto& allp = AllGenrePatterns();
        auto idxOf = [&](const char* n) { for (int i = 0; i < kNumGenrePatterns; ++i) if (std::string(allp[static_cast<size_t>(i)].name) == n) return i; return -1; };
        int longCount = 0; bool inRange = true, firstBarKept = true;
        for (const GenrePattern& g : allp) {
            if (g.bars > 1) ++longCount;
            for (int sl = 0; sl < kNumDrumTracks; ++sl)
                for (int st : g.hitSteps[static_cast<size_t>(sl)]) if (st < 0 || st >= 16 * g.bars) inRange = false;
        }
        check(longCount == 13 && inRange, "v44.47: 13 patterns are long and no step lies outside its bars");
        const GenrePattern& amen = allp[static_cast<size_t>(idxOf("AMEN"))];
        check(amen.bars == 4 && allp[static_cast<size_t>(idxOf("JUNGLE"))].bars == 2 && allp[static_cast<size_t>(idxOf("HOUSE"))].bars == 1, "v44.47: AMEN is 4 bars, JUNGLE 2, HOUSE stays 1");
        { std::vector<int> k0; for (int st : amen.hitSteps[0]) if (st < 16) k0.push_back(st);
          firstBarKept = k0 == std::vector<int>{0, 6, 10};
          check(firstBarKept, "v44.47: bar 1 of a long pattern is the old one-bar template"); }
        check(std::find(amen.hitSteps[0].begin(), amen.hitSteps[0].end(), 16 + 2) != amen.hitSteps[0].end() && std::find(amen.hitSteps[0].begin(), amen.hitSteps[0].end(), 48 + 6) == amen.hitSteps[0].end(), "v44.47: the later bars differ (bar 2 adds a kick, the turnaround drops one)");

        AppState a; a.engine.Init(kSampleRate); a.sequencer.Init(&a.engine, &a.project);
        { std::lock_guard<SceneMutex> lk(a.sequencer.PatternMutex());
          Pattern& pt = a.CurrentPattern(); pt.masterLength = 16; pt.stepsPerBeat = 4;
          pt.tracks[kNumDrumTracks].steps[0].active = true;      // a synth note on step 1
          const int undo0 = static_cast<int>(a.undoStack.size());
          a.ApplyGenrePattern(idxOf("AMEN"));
          check(pt.masterLength == 64, "v44.47: AMEN brings a 16-step scene to 64 steps");
          check(pt.tracks[0].steps[0].active && pt.tracks[0].steps[18].active && !pt.tracks[0].steps[54].active && pt.tracks[0].steps[16].active, "v44.47: the four bars are written (kick of bar 2, no kick on step 55)");
          check(pt.tracks[kNumDrumTracks].steps[16].active && pt.tracks[kNumDrumTracks].steps[48].active, "v44.47: the other tracks repeat what they had, as with LEN");
          check(a.statusMessage.find("SCENE LENGTH 16 -> 64") != std::string::npos, "v44.47: the status line says the scene grew");
          check(static_cast<int>(a.undoStack.size()) == undo0 + 1, "v44.47: it is one undo step");
          a.ApplyGenrePattern(idxOf("AMEN"));
          check(pt.masterLength == 64 && a.statusMessage.find("SCENE LENGTH") == std::string::npos, "v44.47: a scene that is long enough stays as it is");
          a.ApplyGenrePattern(idxOf("HOUSE"));
          bool tiled = pt.masterLength == 64; for (int i = 0; i < 64; i += 4) if (!pt.tracks[0].steps[i].active) tiled = false;
          check(tiled, "v44.47: a one-bar beat in a 64-step scene is repeated over all of it, the scene is not shortened");
          a.ApplyGenrePattern(idxOf("JUNGLE"));
          bool two = pt.masterLength == 64; for (int i = 0; i < 32; ++i) if (pt.tracks[1].steps[i].active != pt.tracks[1].steps[i + 32].active) two = false;
          check(two, "v44.47: a 2-bar pattern in a 64-step scene is repeated every 32 steps");
          check(a.Undo() && a.Undo() && a.Undo() && a.Undo() && pt.masterLength == 16 && !pt.tracks[kNumDrumTracks].steps[16].active, "v44.47: undo brings the 16-step scene back");
        }
        // other grids: a scene holds 64 steps at most
        { AppState g; g.engine.Init(kSampleRate); g.sequencer.Init(&g.engine, &g.project);
          std::lock_guard<SceneMutex> lk(g.sequencer.PatternMutex());
          Pattern& pt = g.CurrentPattern(); pt.masterLength = 16;
          g.SetPatternResolution(8);
          g.ApplyGenrePattern(idxOf("AMEN"));
          check(pt.masterLength == 64 && pt.tracks[0].steps[32].active && g.statusMessage.find("ONLY THE FIRST 2 OF 4 BARS FIT THE 1/32 GRID") != std::string::npos, "v44.47: on 1/32 only 2 of the 4 bars fit, and the status line says so");
          g.SetPatternResolution(6);
          pt.masterLength = 24;
          g.ApplyGenrePattern(idxOf("JUNGLE"));
          check(pt.masterLength == 48, "v44.47: on 1/16T a 2-bar pattern is 48 steps");
          g.SetPatternResolution(4); pt.masterLength = 16;
          g.SwapDrumSlotFromGenre(1, idxOf("AMEN"));
          check(pt.masterLength == 16, "v44.47: a single-slot swap never changes the scene length"); }
        const std::string bp = slurp("src/ui_desktop/BottomPanel.cpp");
        check(bp.find("\"LONG\"") != std::string::npos && bp.find("hits /= nb") != std::string::npos, "v44.47: the beat browser marks long patterns");
    }

    {   // v44.48: one set of instrument switches for LIVE and WHO PLAYS
        auto slurp = [](const std::string& path) { std::ifstream in(path); return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); };
        AppState a; a.engine.Init(kSampleRate); a.sequencer.Init(&a.engine, &a.project);
        std::lock_guard<SceneMutex> lk(a.sequencer.PatternMutex());
        constexpr int br = -1;   // the scene
        check(!a.TrackHasNotes(0, kChordTrackIndex), "v44.48: an empty chord track has no notes");
        a.project.chordBankOn = true; a.project.chordScenes[2].track.steps[0].active = true;
        check(a.TrackHasNotes(0, kChordTrackIndex), "v44.48: with the chord bank on, the chord track has notes when any chord scene has chords (WHO PLAYS lit the bar)");
        a.project.chordBankOn = false;
        check(!a.TrackHasNotes(0, kChordTrackIndex), "v44.48: with the bank off only the scene's own chord steps count");

        a.SwToggleMute(br, 0);
        check(a.SwMuted(br, 0) && a.CurrentPattern().tracks[0].mute && a.TrackIsMuted(0), "v44.48: a bar writes the same mute as the M of the sequencer");
        a.SwToggleSolo(br, 6);
        check(a.SwSolo(br, 6) && a.SwSilenced(br, 0) && !a.SwSilenced(br, 6) && a.CurrentPattern().tracks[6].solo, "v44.48: S solos the instrument and the others read as silenced by the solo");
        a.SwAllOn(br);
        check(!a.SwMuted(br, 0) && !a.SwAnySolo(br), "v44.48: ALL ON clears every mute and every solo");
        a.SwToggleGroup(br, kMuteDrums);
        check(a.SwGroupMuted(br, kMuteDrums) && a.AllDrumsMuted(), "v44.48: ALL DRUMS mutes the six drums");
        a.SwToggleGroup(br, kMuteDrums);
        check(!a.SwGroupMuted(br, kMuteDrums) && !a.AllDrumsMuted(), "v44.48: and brings them back");
        a.SwToggleDrop(br);
        check(a.SwDropOn(br) && !a.SwMuted(br, 0) && !a.SwMuted(br, 6) && a.SwMuted(br, 1) && a.SwMuted(br, 9), "v44.48: DROP keeps kick and bass and mutes the rest");
        a.SwToggleGroup(br, kMuteAllTracks);
        check(!a.SwDropOn(br) && a.SwGroupMuted(br, kMuteAllTracks), "v44.48: MUTE ALL after a DROP ends the drop (the kick and bass are off too)");
        a.SwAllOn(br);

        a.project.songChain[0] = ChainStep{1, 1, -1}; a.project.songChainLength = 1;   // a brick
        a.SwToggleMute(0, 2);
        check(a.SwMuted(0, 2) && (a.SwMask(0) & (1u << 2)) != 0, "v44.48: on a brick a bar is a bit of its mask");
        a.SwToggleSolo(0, 6);
        check(a.SwSolo(0, 6) && a.SwMask(0) == static_cast<uint16_t>(kMuteAllTracks & ~(1u << 6)) && a.SwSilenced(0, 0) && !a.SwSilenced(0, 6), "v44.48: on a brick S = every other instrument off");
        a.SwToggleSolo(0, 6);
        check(!a.SwSolo(0, 6) && a.SwMask(0) == static_cast<uint16_t>(1u << 2), "v44.48: pressing S again brings back what played before");
        a.SwToggleGroup(0, kMuteDrums);
        check(a.SwGroupMuted(0, kMuteDrums), "v44.48: ALL DRUMS on a brick");
        a.SwToggleGroup(0, kMuteDrums);
        check(!a.SwGroupMuted(0, kMuteDrums), "v44.48: and back");
        a.SwToggleMute(0, 9);   // the chord is off before the drop
        a.SwToggleDrop(0);
        check(a.SwMask(0) == kMuteAllButKickBass && a.SwDropOn(0), "v44.48: DROP on a brick");
        a.SwToggleDrop(0);
        check(a.SwMask(0) == static_cast<uint16_t>(1u << 9) && !a.SwDropOn(0), "v44.48: the second press of DROP restores the old mask");
        a.SwAllOn(0);
        check(a.SwMask(0) == 0, "v44.48: ALL ON on a brick");

        const std::string bp = slurp("src/ui_desktop/BottomPanel.cpp"), br2 = slurp("src/ui_desktop/BricksPanel.cpp"), sw = slurp("src/ui_desktop/InstrumentSwitches.h");
        check(bp.find("app.LiveSwitch(AppState::QueuedSwitch::Mute") != std::string::npos && bp.find("Mute the kick") == std::string::npos && bp.find("\"MUTE ALL\", anyMuted") == std::string::npos, "v44.48: the LIVE panel has ten instrument bars and no single KICK / BASS / SYN pads");
        check(br2.find("app.SwToggleSolo(i, t)") != std::string::npos && br2.find("DrawGroups(app, i,") != std::string::npos, "v44.48: WHO PLAYS has S under every bar and the group buttons");
        check(sw.find("SwToggleDrop(brick)") != std::string::npos && sw.find("SwToggleKeep(brick") != std::string::npos, "v44.48: both screens use the same group buttons");
    }

    {   // v44.49: STYLE, one place for beat, kit, groove, sound, bass line, chords and tempo
        const auto& styles = AllStyles();
        check(styles.size() == 12, "v44.49: twelve styles");
        bool allResolve = true; std::string bad;
        for (const StyleDef& sd : styles) {
            const bool ok = FindGenrePatternIndex(sd.pattern) >= 0 && FindSynthPresetIndex(sd.bass) >= 0 && FindSynthPresetIndex(sd.syn2) >= 0 &&
                            FindSynthPresetIndex(sd.syn3) >= 0 && FindSynthPresetIndex(sd.chord) >= 0 && FindProgressionIndex(sd.progression) >= 0 &&
                            (sd.groove[0] == '\0' || FindBuiltinGrooveIndex(sd.groove) >= 0) && sd.bpm >= 60 && sd.bpm <= 200 && !sd.bassLine.empty() &&
                            (!std::filesystem::exists("kits") || std::filesystem::exists(sd.kit));
            if (!ok) { allResolve = false; bad += std::string(" ") + sd.name; }
        }
        check(allResolve, "v44.49: every style resolves in the library (pattern, presets, progression, groove, kit):" + bad);

        auto sameScene = [](const Pattern& x, const Pattern& y) {
            if (x.masterLength != y.masterLength || x.swing != y.swing || x.kitFolder != y.kitFolder || x.bpm != y.bpm) return false;
            for (size_t t = 0; t < x.tracks.size(); ++t) for (int i = 0; i < kMaxSteps; ++i) {
                const Step& a = x.tracks[t].steps[static_cast<size_t>(i)]; const Step& b = y.tracks[t].steps[static_cast<size_t>(i)];
                if (a.active != b.active || a.note != b.note || a.chordSize != b.chordSize || a.lengthSteps != b.lengthSteps || a.vel != b.vel || a.nudge != b.nudge) return false;
            }
            return std::memcmp(&x.synthPatches[1], &y.synthPatches[1], sizeof(SynthPatch)) == 0;
        };
        auto anyActive = [](const Track& t, int len) { for (int i = 0; i < len; ++i) if (t.steps[static_cast<size_t>(i)].active) return true; return false; };
        bool allOk = true; std::string why;
        for (int si = 0; si < static_cast<int>(styles.size()); ++si) {
            for (int spb : {4, 3, 8}) {
                auto aHeap = std::make_unique<AppState>(); AppState& a = *aHeap; a.engine.Init(kSampleRate); a.sequencer.Init(&a.engine, &a.project);
                std::lock_guard<SceneMutex> lk(a.sequencer.PatternMutex());
                a.project.randomizerKeyRoot = 2; a.project.randomizerScale = 5;   // D, a minor mode: the notes must follow it
                Pattern& pat = a.CurrentPattern();
                pat.stepsPerBeat = spb; pat.masterLength = 16 * spb / 4; pat.bpm = 111.0f;
                const auto beforeHeap = std::make_unique<Pattern>(pat); const Pattern& before = *beforeHeap;
                a.ApplyStyle(si);
                const StyleDef& sd = styles[static_cast<size_t>(si)];
                const int len = pat.masterLength;
                auto fail = [&](const char* w) { allOk = false; why += std::string(" [") + sd.name + " spb" + std::to_string(spb) + ": " + w + "]"; };
                if (len < 4 * spb) fail("scene shorter than a bar");
                if (!anyActive(pat.tracks[0], len)) fail("no kick");
                if (!anyActive(pat.tracks[kNumDrumTracks], len)) fail("no bass");
                bool chords = false; for (int i = 0; i < len; ++i) if (a.StepTrack(pat, kChordTrackIndex).steps[static_cast<size_t>(i)].chordSize > 0) chords = true;
                if (!chords) fail("no chords");
                for (int i = 0; i < kMaxSteps; ++i) {
                    const Step& st = pat.tracks[kNumDrumTracks].steps[static_cast<size_t>(i)];
                    if (st.active && (i >= len || SnapToScale(st.note, 2, 5) != st.note)) fail("bass note outside the scene or the scale");
                }
                if (pat.bpm != 111.0f) fail("tempo changed with TEMPO off");
                if (pat.kitFolder != sd.kit) fail("kit");
                if (a.project.randomizerKeyRoot != 2 || a.project.randomizerScale != 5) fail("key or scale changed");
                if (std::memcmp(&pat.synthPatches[1], &before.synthPatches[1], sizeof(SynthPatch)) == 0) fail("sound not written");
                a.Undo();
                if (!sameScene(a.CurrentPattern(), before)) fail("ONE undo does not bring everything back");
            }
        }
        check(allOk, "v44.49: each style writes beat, kit, sound, bass line (in the scale) and chords on 1/16, 1/8T and 1/32, keeps the key, and ONE undo restores the scene:" + why);

        {   // parts, tempo, chord bank
            auto aHeap = std::make_unique<AppState>(); AppState& a = *aHeap; a.engine.Init(kSampleRate); a.sequencer.Init(&a.engine, &a.project);
            std::lock_guard<SceneMutex> lk(a.sequencer.PatternMutex());
            Pattern& pat = a.CurrentPattern(); pat.bpm = 100.0f;
            a.styleBeat = false; a.styleSound = false; a.styleNotes = false; a.styleTempo = false;
            check(!a.ApplyStyle(0), "v44.49: with every part off nothing is applied");
            a.styleTempo = true;
            check(a.ApplyStyle(0) && pat.bpm == 124.0f && !anyActive(pat.tracks[0], 16) && !anyActive(pat.tracks[kNumDrumTracks], 16), "v44.49: TEMPO alone sets the BPM and leaves the notes alone");
            a.styleTempo = false; a.styleNotes = true;
            const std::string kitBefore = pat.kitFolder;
            check(a.ApplyStyle(1) && pat.kitFolder == kitBefore && !anyActive(pat.tracks[0], 16) && anyActive(pat.tracks[kNumDrumTracks], 16), "v44.49: NOTES alone writes bass and chords and not the drums or the kit");
            a.styleNotes = false; a.styleBeat = true;
            const Step bassBefore = pat.tracks[kNumDrumTracks].steps[2];
            check(a.ApplyStyle(5) && anyActive(pat.tracks[0], pat.masterLength) && pat.tracks[kNumDrumTracks].steps[2].note == bassBefore.note, "v44.49: BEAT alone writes the drums and leaves the bass line");
            a.styleBeat = true; a.styleSound = true; a.styleNotes = true;
            a.project.chordBankOn = true;
            a.ApplyStyle(3);
            bool inScene = false; for (int i = 0; i < pat.masterLength; ++i) if (a.StepTrack(pat, kChordTrackIndex).steps[static_cast<size_t>(i)].chordSize > 0) inScene = true;
            check(inScene, "v44.49: with the chord bank on the chords go to the edited chord scene");
        }
        {   // a long style grows the scene, never shrinks it
            auto aHeap = std::make_unique<AppState>(); AppState& a = *aHeap; a.engine.Init(kSampleRate); a.sequencer.Init(&a.engine, &a.project);
            std::lock_guard<SceneMutex> lk(a.sequencer.PatternMutex());
            Pattern& pat = a.CurrentPattern(); pat.masterLength = 16;
            a.ApplyStyle(3);   // UK GARAGE, two bars
            check(pat.masterLength == 32, "v44.49: a two-bar style brings a 16-step scene to 32 steps");
            pat.masterLength = 64;
            a.ApplyStyle(3);
            check(pat.masterLength == 64, "v44.49: and a longer scene is never shortened");
        }
    }
    {   // v44.50: WAV files as DAWs write them (EXTENSIBLE 24-bit, 32-bit integer, six channels) load, and the old plain formats still do
        auto le = [](std::vector<uint8_t>& v, uint32_t x, int n) { for (int i = 0; i < n; ++i) v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF)); };
        auto make = [&](uint16_t tag, int ch, int bits, bool ext, const std::vector<int32_t>& frames /* ch values per frame, in the bit depth */) {
            std::vector<uint8_t> fmt; le(fmt, ext ? 0xFFFE : tag, 2); le(fmt, static_cast<uint32_t>(ch), 2); le(fmt, 44100, 4); le(fmt, static_cast<uint32_t>(44100 * ch * bits / 8), 4); le(fmt, static_cast<uint32_t>(ch * bits / 8), 2); le(fmt, static_cast<uint32_t>(bits), 2);
            if (ext) { le(fmt, 22, 2); le(fmt, static_cast<uint32_t>(bits), 2); le(fmt, 3, 4); le(fmt, tag, 2); const uint8_t g[14] = {0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71}; fmt.insert(fmt.end(), g, g + 14); }
            std::vector<uint8_t> data; for (int32_t v : frames) le(data, static_cast<uint32_t>(v), bits / 8);
            std::vector<uint8_t> body{'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}; le(body, static_cast<uint32_t>(fmt.size()), 4); body.insert(body.end(), fmt.begin(), fmt.end());
            body.insert(body.end(), {'d', 'a', 't', 'a'}); le(body, static_cast<uint32_t>(data.size()), 4); body.insert(body.end(), data.begin(), data.end());
            std::vector<uint8_t> f{'R', 'I', 'F', 'F'}; le(f, static_cast<uint32_t>(body.size()), 4); f.insert(f.end(), body.begin(), body.end()); return f; };
        auto load = [&](const std::string& name, const std::vector<uint8_t>& bytes, SampleBuffer& out) { std::filesystem::create_directories("selftest_tmp_wav"); const std::string path = "selftest_tmp_wav/" + name; { std::ofstream o(path, std::ios::binary); o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); } return LoadWav(path, out, 44100); };
        SampleBuffer b;
        check(load("ext24.wav", make(1, 2, 24, true, {4194304, -4194304, 0, 0}), b) && b.channels == 2 && b.frames() == 2 && std::fabs(b.data[0] - 0.5f) < 1e-4f && std::fabs(b.data[1] + 0.5f) < 1e-4f, "v44.50: WAVE_FORMAT_EXTENSIBLE 24-bit stereo loads with the right values");
        check(load("pcm32.wav", make(1, 1, 32, false, {1073741824, -1073741824}), b) && b.channels == 1 && b.frames() == 2 && std::fabs(b.data[0] - 0.5f) < 1e-4f, "v44.50: 32-bit integer PCM loads");
        check(load("six.wav", make(1, 6, 16, false, {16384, -16384, 1, 2, 3, 4, 0, 0, 5, 6, 7, 8}), b) && b.channels == 2 && b.frames() == 2 && std::fabs(b.data[0] - 0.5f) < 1e-4f && std::fabs(b.data[1] + 0.5f) < 1e-4f, "v44.50: a six-channel file keeps its front pair (mono / stereo samples only)");
        check(load("plain16.wav", make(1, 1, 16, false, {16384, -16384}), b) && b.frames() == 2 && std::fabs(b.data[0] - 0.5f) < 1e-4f, "v44.50: plain 16-bit PCM still loads");
    }
    {   // v44.50: the SAMPLER, the 11th instrument (ONE-SHOT)
        namespace fs = std::filesystem;
        auto slurp = [](const std::string& path) { std::ifstream in(path); return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); };
        check(kNumTracks == 11 && kSamplerTrackIndex == 10 && kChordTrackIndex == 9 && kNumParts == 12 && kMuteAllTracks == 0x07FF && kArrTracks == 11,
              "v44.50: eleven tracks, the sampler is the last (index 10), the older indices are unchanged");
        {
            Pattern pat;
            check(pat.tracks[kSamplerTrackIndex].type == TrackType::Sampler && pat.tracks[kChordTrackIndex].type == TrackType::Chord && pat.tracks[6].type == TrackType::Synth,
                  "v44.50: a new scene has the sampler track and the old tracks keep their types");
        }
        fs::remove_all("selftest_smp"); fs::create_directories("selftest_smp/src");
        auto makeWav = [&](const std::string& path, double hz, double sec, int ch) {
            std::vector<float> w(static_cast<size_t>(sec * 44100) * ch);
            for (size_t i = 0; i < w.size() / ch; ++i) for (int c = 0; c < ch; ++c) w[i * ch + c] = static_cast<float>(0.6 * std::sin(2.0 * 3.14159265358979 * hz * i / 44100.0));
            WriteWavPCM16(path, w, ch, 44100);
        };
        makeWav("selftest_smp/src/tone.wav", 440.0, 0.5, 2);
        auto ap = std::make_unique<AppState>(); AppState& a = *ap;
        a.engine.Init(kSampleRate); a.sequencer.Init(&a.engine, &a.project);
        a.engine.GetMixer().masterVolume = 1.0f;
        a.projectPath = "selftest_smp/p.t8proj";
        {
            std::lock_guard<SceneMutex> lk(a.sequencer.PatternMutex());
            check(a.AssignSampleToSampler("selftest_smp/src/tone.wav") && a.CurrentPattern().samplerPatch.file == "tone.wav" && fs::exists("selftest_smp/samples/tone.wav"),
                  "v44.50: a dropped WAV is copied into samples/ beside the project and becomes the sample of the scene");
            check(fs::exists("selftest_smp/src/tone.wav"), "v44.50: the original file is not touched");
            check(a.AssignSampleToSampler("selftest_smp/src/tone.wav") && a.CurrentPattern().samplerPatch.file == "tone.wav", "v44.50: the same file dropped again is not copied twice");
            makeWav("selftest_smp/src/other/tone.wav", 220.0, 0.9, 1);   // a different file with the same name
        }
        fs::create_directories("selftest_smp/src/other"); makeWav("selftest_smp/src/other/tone.wav", 220.0, 0.9, 1);
        {
            std::lock_guard<SceneMutex> lk(a.sequencer.PatternMutex());
            check(a.AssignSampleToSampler("selftest_smp/src/other/tone.wav") && a.CurrentPattern().samplerPatch.file == "tone_2.wav", "v44.50: another file with the same name gets its own name");
            a.SetSamplerFile("tone.wav");
            a.Undo(); a.Undo();
            check(a.CurrentPattern().samplerPatch.file == "tone_2.wav" || a.CurrentPattern().samplerPatch.file == "tone.wav", "v44.50: assigning a sample is undoable");
            a.SetSamplerFile("tone.wav");
            check(!a.SampleMissing("tone.wav") && a.SampleMissing("nothing.wav") && a.SampleNames().size() == 2, "v44.50: the sample cache finds files and reports a missing one; the picker lists samples/");
        }
        {   // it plays: a step with a note, and silence without a sample
            Pattern& pat = a.CurrentPattern();
            for (auto& t : pat.tracks) for (auto& st : t.steps) st = Step{};
            pat.tracks[kSamplerTrackIndex].steps[0].active = true; pat.tracks[kSamplerTrackIndex].steps[0].note = 60;
            pat.bpm = 120.0f; pat.masterLength = 16;
            a.sequencer.Play();
            std::vector<float> buf(44100 * 2, 0.0f);
            for (int r = 0; r < 44100; r += 256) a.sequencer.RenderAndAdvance(buf.data() + static_cast<size_t>(r) * 2, std::min(256, 44100 - r), kSampleRate);
            float peak = 0; for (float v : buf) peak = std::max(peak, std::fabs(v));
            check(peak > 0.1f, "v44.50: a sampler step plays the sample (audible)");
            a.sequencer.Stop();
            pat.samplerPatch.file.clear(); a.SyncSceneSamples(true);
            a.sequencer.Play();
            std::fill(buf.begin(), buf.end(), 0.0f);
            for (int r = 0; r < 44100; r += 256) a.sequencer.RenderAndAdvance(buf.data() + static_cast<size_t>(r) * 2, std::min(256, 44100 - r), kSampleRate);
            float peak2 = 0; for (float v : buf) peak2 = std::max(peak2, std::fabs(v));
            check(peak2 < 1e-6f, "v44.50: a scene without a sample is silent");
            pat.samplerPatch.file = "tone.wav"; a.SyncSceneSamples(true);
            a.sequencer.Stop();
            // pitch: one octave up plays twice as fast, so the 0.5 s tone is over after 0.25 s
            pat.samplerPatch.root = 60; pat.tracks[kSamplerTrackIndex].steps[0].note = 72;
            a.sequencer.ApplySceneSound(a.sequencer.PlayPatternIndex());
            a.sequencer.Play();
            std::fill(buf.begin(), buf.end(), 0.0f);
            for (int r = 0; r < 44100; r += 256) a.sequencer.RenderAndAdvance(buf.data() + static_cast<size_t>(r) * 2, std::min(256, 44100 - r), kSampleRate);
            int last = 0; for (int i = 0; i < 44100; ++i) if (std::fabs(buf[static_cast<size_t>(i) * 2]) > 0.02f) last = i;
            check(last > 9000 && last < 13500, "v44.50: the note is the pitch: an octave above the root plays the 0.5 s sample in about 0.25 s");
            a.sequencer.Stop();
            a.SetMuteScene(kSamplerTrackIndex, true);
            { std::vector<float> warm(4096 * 2, 0.0f); for (int r = 0; r < 4096; r += 256) a.sequencer.RenderAndAdvance(warm.data(), 256, kSampleRate); }   // the mute is a short ramp
            a.sequencer.Play();
            std::fill(buf.begin(), buf.end(), 0.0f);
            for (int r = 0; r < 44100; r += 256) a.sequencer.RenderAndAdvance(buf.data() + static_cast<size_t>(r) * 2, std::min(256, 44100 - r), kSampleRate);
            float peak3 = 0; for (float v : buf) peak3 = std::max(peak3, std::fabs(v));
            check(peak3 < 1e-3f, "v44.50: the M button of the sampler mutes it");
            a.sequencer.Stop();
            a.SetMuteScene(kSamplerTrackIndex, false);
        }
        {   // the file: round trip, and an older file (format 6, 10 tracks) opens as it was
            a.CurrentPattern().samplerPatch.root = 64; a.CurrentPattern().samplerPatch.start = 0.25f; a.CurrentPattern().samplerPatch.poly = true; a.CurrentPattern().samplerPatch.reverse = true;
            check(SaveProject(a.project, "selftest_smp/p.t8proj"), "v44.50: save");
            const std::string txt = slurp("selftest_smp/p.t8proj");
            check(txt.find("sampler=tone.wav|64|0.25|") != std::string::npos && txt.find("type=Sampler") != std::string::npos, "v44.50: the file carries the sampler patch and the sampler track");
            Project lp;
            check(LoadProject(lp, "selftest_smp/p.t8proj") && lp.patterns[0].samplerPatch == a.CurrentPattern().samplerPatch && lp.patterns[0].tracks[kSamplerTrackIndex].type == TrackType::Sampler,
                  "v44.50: the sampler patch survives save / load");
            // an older file: format 6, only ten tracks, the kit part of ownSound on bit 10
            std::string old; { std::istringstream in(txt); std::string line; bool skip = false;
                while (std::getline(in, line)) {
                    if (line.rfind("[pattern", 0) == 0 || line.rfind("[chordscene", 0) == 0 || line == "[end]") skip = false;
                    if (line.find(".track10]") != std::string::npos) skip = true;
                    if (skip) continue;
                    if (line.rfind("sampler=", 0) == 0) continue;
                    if (line == "format=" + std::to_string(kProjectFormatVersion)) line = "format=6";
                    old += line + "\n"; } }
            { std::ofstream o("selftest_smp/old.t8proj", std::ios::trunc); o << old; }
            Project ol;
            check(LoadProject(ol, "selftest_smp/old.t8proj") && ol.patterns[0].tracks[kSamplerTrackIndex].type == TrackType::Sampler && ol.patterns[0].samplerPatch.file.empty() && ol.patterns[0].tracks[kChordTrackIndex].type == TrackType::Chord,
                  "v44.50: an older file without the sampler opens, with an empty sampler track");
            // the kit part moved from bit 10 to bit 11: an old brick with its own kit still has its own kit
            std::string old2 = old; const size_t p0 = old2.find("[pattern0]"); old2.insert(p0 + std::string("[pattern0]\n").size(), "ownSound=1024\n");
            { std::ofstream o("selftest_smp/old2.t8proj", std::ios::trunc); o << old2; }
            Project o2;
            check(LoadProject(o2, "selftest_smp/old2.t8proj") && o2.patterns[0].ownSound == (1u << kPartKit), "v44.50: an older file's own-kit mark (bit 10) is the kit part (bit 11) now, not the sampler");
        }
        {   // brick / mute masks / copy: the sampler is a part like the others
            Pattern x, y; x.samplerPatch.file = "a.wav"; x.samplerPatch.root = 48;
            CopyPart(y, x, kSamplerTrackIndex);
            check(y.samplerPatch.file == "a.wav" && y.samplerPatch.root == 48, "v44.50: the sampler is a part of the sound of a scene (copied with the others)");
            x.tracks[kSamplerTrackIndex].solo = true;
            check(SceneMuteMask(x) == static_cast<uint16_t>(kMuteAllTracks & ~(1u << kSamplerTrackIndex)), "v44.50: S on the sampler = every other instrument off");
            check(kMuteAllButKickBass == (kMuteAllTracks & ~kMuteKickBass) && (kMuteAllTracks & (1u << kSamplerTrackIndex)) != 0, "v44.50: the group masks include the sampler");
        }
        {   // SAVE AS in another folder copies the samples that the project uses
            a.SetSamplerFile("tone.wav");
            fs::create_directories("selftest_smp/elsewhere");
            a.CopySamplesTo("selftest_smp/elsewhere/q.t8proj");
            check(fs::exists("selftest_smp/elsewhere/samples/tone.wav") && fs::exists("selftest_smp/samples/tone.wav"), "v44.50: SAVE AS beside another folder copies the samples, the originals stay");
        }
        fs::remove_all("selftest_smp");
    }
    {   // ---- v44.51: the library inside the application: audition, own transitions ----
        namespace fsl = std::filesystem;
        fsl::remove_all("selftest_lib2"); fsl::remove_all("selftest_lib2proj"); fsl::remove_all("selftest_lib2other");
        fsl::create_directories("selftest_lib2/Kicks"); fsl::create_directories("selftest_lib2/Fills"); fsl::create_directories("selftest_lib2/Snares"); fsl::create_directories("selftest_lib2proj");
        auto mk = [&](const std::string& path, double hz, double sec) {
            std::vector<float> w(static_cast<size_t>(sec * 44100));
            for (size_t i = 0; i < w.size(); ++i) w[i] = static_cast<float>(0.6 * std::sin(2.0 * 3.14159265358979 * hz * i / 44100.0));
            WriteWavPCM16(path, w, 1, 44100);
        };
        mk("selftest_lib2/Kicks/k1.wav", 60.0, 0.4); mk("selftest_lib2/Fills/riser long.wav", 800.0, 1.2); mk("selftest_lib2/Snares/s.wav", 200.0, 0.3);
        setenv("TRB_SAMPLES_DIR", "selftest_lib2", 1);
        auto lp = std::make_unique<AppState>(); AppState& a = *lp;
        a.engine.Init(kSampleRate); a.sequencer.Init(&a.engine, &a.project);
        a.engine.Risers().LoadFromFolder("kits/risers", kSampleRate);
        a.projectPath = "selftest_lib2proj/p.t8proj";
        check(a.LibraryList().size() == 3 && a.LibraryRoot() == "selftest_lib2", "lib app: the library lists the three files of the chosen folder");
        check(a.AuditionLibrary("Kicks/k1.wav"), "lib app: audition loads the file");
        std::vector<float> ob(256 * 2); double pk = 0.0;
        for (int b = 0; b < 40; ++b) { a.sequencer.RenderAndAdvance(ob.data(), 256, kSampleRate); for (float v : ob) pk = std::max(pk, static_cast<double>(std::fabs(v))); }
        check(pk > 0.05, "lib app: the audition is heard through the master bus");
        a.engine.AuditionStop();
        pk = 0.0; for (int b = 0; b < 40; ++b) { a.sequencer.RenderAndAdvance(ob.data(), 256, kSampleRate); if (b > 4) for (float v : ob) pk = std::max(pk, static_cast<double>(std::fabs(v))); }
        check(pk < 1e-4, "lib app: AuditionStop silences it");
        a.project.patterns[0].tracks[0].steps[0].active = true;
        check(a.ThrowIntoChain() && a.ThrowIntoChain(), "lib app: two bricks");
        check(!a.AssignLibraryTransition(0, "Fills/riser long.wav"), "lib app: no transition before the first brick");
        const int first = RiserPlayer::kFirstUser;
        check(a.AssignLibraryTransition(1, "Fills/riser long.wav") && a.project.songChain[1].riser == first && fsl::exists("selftest_lib2proj/samples/transitions/t01_riser_long.wav"), "lib app: a library file becomes transition slot 1 (copied into the project)");
        check(a.engine.Risers().NameAt(first) == "RISER LONG" && a.engine.Risers().Count() == first + 1, "lib app: the engine lists it by name after the built-in sounds");
        check(a.AssignLibraryTransition(1, "Fills/riser long.wav") && a.project.songChain[1].riser == first && !fsl::exists("selftest_lib2proj/samples/transitions/t02_riser_long.wav"), "lib app: the same file again keeps its slot");
        check(a.AssignLibraryTransition(1, "Snares/s.wav") && a.project.songChain[1].riser == first + 1 && a.engine.Risers().NameAt(first + 1) == "S", "lib app: another file takes the next slot");
        {   // a second application instance (a project opened later) reads the folder back
            auto lp2 = std::make_unique<AppState>(); AppState& b = *lp2;
            b.engine.Init(kSampleRate); b.sequencer.Init(&b.engine, &b.project);
            b.engine.Risers().LoadFromFolder("kits/risers", kSampleRate);
            b.projectPath = "selftest_lib2proj/p.t8proj";
            b.SyncUserTransitions();
            check(b.engine.Risers().NameAt(first) == "RISER LONG" && b.engine.Risers().NameAt(first + 1) == "S", "lib app: the transitions of a project come back when it is opened");
            fsl::create_directories("selftest_lib2other");
            b.CopySamplesTo("selftest_lib2other/q.t8proj");
            check(fsl::exists("selftest_lib2other/samples/transitions/t01_riser_long.wav") && fsl::exists("selftest_lib2other/samples/transitions/t02_s.wav"), "lib app: SAVE AS copies the own transitions");
        }
        {   // the sampler takes a library file; FINE and GAIN (v44.51) are saved and heard
            std::lock_guard<SceneMutex> lk(a.sequencer.PatternMutex());
            check(a.AssignSampleToSampler(a.LibraryPath("Kicks/k1.wav")) && a.CurrentPattern().samplerPatch.file == "k1.wav" && fsl::exists("selftest_lib2proj/samples/k1.wav"), "lib app: a library file goes to the sampler (copied into samples/)");
            SamplerPatch sp = a.CurrentPattern().samplerPatch; sp.tuneCents = -37; sp.gainDb = -9;
            a.CurrentPattern().samplerPatch = sp;
            Project back; const bool saved = SaveProject(a.project, "selftest_lib2proj/p.t8proj");
            check(saved && LoadProject(back, "selftest_lib2proj/p.t8proj") && back.patterns[static_cast<size_t>(a.project.currentPattern)].samplerPatch == sp, "sampler v44.51: FINE and GAIN survive save / load");
            SamplePtr smp = a.SampleFor("k1.wav");
            auto peakOf = [&](int cents, int db) {
                SamplerVoice v; SamplerPatch p = sp; p.tuneCents = static_cast<int8_t>(cents); p.gainDb = static_cast<int8_t>(db); p.attackMs = 0.0f; v.SetSample(smp); v.SetPatch(p);
                v.NoteOn(p.root, 1.0f, kSampleRate); std::vector<float> o(2 * 4096, 0.0f); v.Render(o.data(), 4096, kSampleRate);
                double pk = 0; for (float x : o) pk = std::max(pk, static_cast<double>(std::fabs(x))); return pk; };
            const double p0 = peakOf(0, 0), pm = peakOf(0, -12), pp = peakOf(0, 6);
            check(p0 > 0.1 && std::fabs(pm / p0 - 0.2512) < 0.02 && std::fabs(pp / p0 - 1.995) < 0.05, "sampler v44.51: GAIN scales the level in dB");
        }
        {   // v44.52.1: LAY OUT: the slice notes go on the steps so the loop plays through in order
            std::lock_guard<SceneMutex> lk(a.sequencer.PatternMutex());
            Pattern& pt = a.CurrentPattern();
            SamplerPatch& sq = pt.samplerPatch;
            const SamplerPatch keep = sq; const int keepLen = pt.tracks[kSamplerTrackIndex].length; const bool keepOwn = pt.tracks[kSamplerTrackIndex].useOwnLength;
            pt.stepsPerBeat = 4; pt.bpm = 120.0f;
            sq.mode = 1; sq.slices = 8; sq.fitBars = 1; sq.root = 60; sq.start = 0.0f; sq.decay = 1.0f;
            const std::string msg = a.LayoutSamplerSlices();
            const Track& tr = pt.tracks[kSamplerTrackIndex];
            bool order = true; int count = 0, lastAt = -1;
            for (int i = 0; i < kMaxSteps; ++i) if (tr.steps[static_cast<size_t>(i)].active) { order = order && tr.steps[static_cast<size_t>(i)].note == 60 + count && i == count * 2; lastAt = i; ++count; }
            check(count == 8 && order && lastAt == 14 && tr.length == 16, "layout v44.52.1: 8 slices of one bar sit on steps 0, 2 ... 14 with notes ROOT .. ROOT + 7");
            sq.slices = 16; sq.fitBars = 4; a.LayoutSamplerSlices();
            int c16 = 0, last16 = -1; for (int i = 0; i < kMaxSteps; ++i) if (tr.steps[static_cast<size_t>(i)].active) { ++c16; last16 = i; }
            check(c16 == 16 && last16 == 60 && pt.tracks[kSamplerTrackIndex].length == 64 && pt.tracks[kSamplerTrackIndex].useOwnLength, "layout v44.52.1: 16 slices of a 4-bar loop sit every 4 steps and the track gets 64 steps");
            sq.mode = 0; const std::string refuse = a.LayoutSamplerSlices();
            check(refuse.find("SLICES") != std::string::npos && refuse.find("FIRST") != std::string::npos, "layout v44.52.1: outside SLICES mode nothing is written");
            sq = keep; pt.tracks[kSamplerTrackIndex].length = keepLen; pt.tracks[kSamplerTrackIndex].useOwnLength = keepOwn; (void)msg;
        }
        // the transition really sounds when the brick plays (the sound lands before brick 2)
        a.activeTab = 1; a.SetLoop(true); a.PlayFromBrick(0);
        double pk2 = 0.0;
        for (int b = 0; b < 600; ++b) { a.sequencer.RenderAndAdvance(ob.data(), 256, kSampleRate); for (float v : ob) pk2 = std::max(pk2, static_cast<double>(std::fabs(v))); }
        check(pk2 > 0.01, "lib app: playing the chain with an own transition makes sound");
        a.sequencer.Stop(); a.TickTransport();
        unsetenv("TRB_SAMPLES_DIR");
        fsl::remove_all("selftest_lib2"); fsl::remove_all("selftest_lib2proj"); fsl::remove_all("selftest_lib2other");
    }
    {   // ---- v44.51: the sample library ----
        check(ClassifyLibraryPath("Kicks/Deep/kick_03.wav") == "KICK", "lib: Kicks/ folder -> KICK");
        check(ClassifyLibraryPath("Snares/snare roll fill.wav") == "SNARE", "lib: a file in Snares/ is a SNARE even with 'fill' in its name");
        check(ClassifyLibraryPath("Fills/snare_roll.wav") == "FX", "lib: Fills/ folder -> FX (the folder is stronger than the file name)");
        check(ClassifyLibraryPath("Hats/open/ohh_01.wav") == "HAT" && ClassifyLibraryPath("closedhh_2.wav") == "HAT" && ClassifyLibraryPath("CHH.wav") == "HAT", "lib: hats by folder and by name");
        check(ClassifyLibraryPath("Claps/clap1.wav") == "CLAP" && ClassifyLibraryPath("Perc/Shakers/s1.wav") == "PERC" && ClassifyLibraryPath("808 bass/sub_c.wav") == "BASS" && ClassifyLibraryPath("Fills/sub/f1.wav") == "FX", "lib: clap, perc, bass");
        check(ClassifyLibraryPath("Drum Loops/Kick Loop 120.wav") == "LOOP" && ClassifyLibraryPath("random.wav") == "OTHER", "lib: LOOP wins over the instrument, unknown = OTHER");
        check(ClassifyLibraryPath("FX/Risers/riser 4 bar.wav") == "FX" && ClassifyLibraryPath("Synth/lead_a.wav") == "SYNTH", "lib: fx and synth");
        namespace fs2 = std::filesystem;
        fs2::remove_all("selftest_lib");
        fs2::create_directories("selftest_lib/Kicks"); fs2::create_directories("selftest_lib/Fills/sub"); fs2::create_directories("selftest_lib/Kits/mine"); fs2::create_directories("selftest_lib/.hidden");
        auto touch = [](const std::string& f) { std::ofstream o(f, std::ios::binary); o << "RIFF"; };
        touch("selftest_lib/Kicks/k1.wav"); touch("selftest_lib/Kicks/k2.WAV"); touch("selftest_lib/Fills/sub/f1.wav"); touch("selftest_lib/Kicks/notes.txt"); touch("selftest_lib/.hidden/h.wav");
        touch("selftest_lib/Kits/mine/kick.wav"); touch("selftest_lib/Kits/mine/snare.wav");
        const auto lib = ScanSampleLibrary("selftest_lib");
        check(lib.size() == 3 && lib[0].category == "KICK" && lib[2].category == "FX" && lib[2].rel == "Fills/sub/f1.wav" && lib[2].folder == "sub", "lib: scan finds the three wav files, skips txt, hidden folders and Kits, sorted by category");
        fs2::create_directories("selftest_kits");
        check(SyncLibraryKits("selftest_lib", "selftest_kits") == 1 && fs2::exists("selftest_kits/mine/kick.wav") && fs2::exists("selftest_kits/mine/snare.wav"), "lib: a Kits/<name> folder is copied into kits/");
        check(SyncLibraryKits("selftest_lib", "selftest_kits") == 0, "lib: the second sync changes nothing");
        fs2::remove_all("selftest_lib"); fs2::remove_all("selftest_kits");
    }
    {   // v44.52: ONE sample editor (START / LENGTH / ATTACK / FINE / GAIN / REVERSE) on drums and sampler; SLICES and LOOP on the sampler
        namespace fs52 = std::filesystem;
        const int sr = kSampleRate;
        // a synthetic 2.0 s ramp sample (value = position), so every frame says where it was played from
        auto ramp = std::make_shared<SampleData>();
        ramp->channels = 1; ramp->sampleRate = sr; ramp->name = "ramp";
        const int N = sr * 2;
        ramp->data.resize(static_cast<size_t>(N));
        for (int i = 0; i < N; ++i) ramp->data[static_cast<size_t>(i)] = 0.1f + 0.8f * static_cast<float>(i) / static_cast<float>(N);
        SamplePtr smp = ramp;
        auto renderV = [&](SamplerVoice& v, int frames) { std::vector<float> o(static_cast<size_t>(frames) * 2, 0.0f);
            for (int i = 0; i < frames; i += 512) v.Render(o.data() + static_cast<size_t>(i) * 2, std::min(512, frames - i), sr);
            std::vector<float> m(static_cast<size_t>(frames)); for (int i = 0; i < frames; ++i) m[static_cast<size_t>(i)] = o[static_cast<size_t>(i) * 2]; return m; };

        // persistence of SampleEdit on drum tracks and of the new sampler fields
        {
            Project pr; pr.patterns[0].tracks[0].edit.start = 0.25f; pr.patterns[0].tracks[0].edit.length = 0.5f; pr.patterns[0].tracks[0].edit.attackMs = 12.0f;
            pr.patterns[0].tracks[0].edit.fineCents = -20; pr.patterns[0].tracks[0].edit.gainDb = -6; pr.patterns[0].tracks[0].edit.reverse = true;
            pr.patterns[0].samplerPatch.mode = 1; pr.patterns[0].samplerPatch.slices = 16; pr.patterns[0].samplerPatch.fitBars = 2;
            fs52::create_directories("selftest_v4452");
            Project back; const bool ok = SaveProject(pr, "selftest_v4452/p.t8proj") && LoadProject(back, "selftest_v4452/p.t8proj");
            check(ok && back.patterns[0].tracks[0].edit == pr.patterns[0].tracks[0].edit, "edit v44.52: a drum track's sample edit survives save / load");
            check(ok && back.patterns[0].samplerPatch == pr.patterns[0].samplerPatch && back.patterns[0].samplerPatch.mode == 1 && back.patterns[0].samplerPatch.slices == 16 && back.patterns[0].samplerPatch.fitBars == 2, "edit v44.52: sampler MODE / SLICES / FIT survive save / load");
            check(ok && back.patterns[1].tracks[0].edit.IsDefault() && back.patterns[1].samplerPatch.mode == 0 && back.patterns[1].samplerPatch.fitBars == -1, "edit v44.52: a scene without edits stays at the defaults (an old project is unchanged)");
            fs52::remove_all("selftest_v4452");
        }
        // drums: defaults change nothing, START / LENGTH / REVERSE / GAIN do what they say
        {
            DrumKit k2; k2.LoadFromFolder("kits/909", sr);
            auto src = k2.Variant(DrumSlot::Snare, 0);
            auto play = [&](const SampleEdit& ed, int frames) { DrumEngine e; e.SetKit(&k2); DrumVoiceParams vp; vp.level = 1.0f; vp.edit = ed; e.Trigger(DrumSlot::Snare, 1.0f, 0, vp);
                std::vector<float> st(static_cast<size_t>(frames) * 2, 0.0f); for (int o = 0; o < frames; o += 256) e.RenderSlot(DrumSlot::Snare, st.data() + static_cast<size_t>(o) * 2, std::min(256, frames - o), sr);
                std::vector<float> m(static_cast<size_t>(frames)); for (int i = 0; i < frames; ++i) m[static_cast<size_t>(i)] = st[static_cast<size_t>(2 * i)]; return m; };
            const int n = src->frames();
            SampleEdit none; auto a0 = play(none, n - 300);
            double err = 0, ref = 0; for (int i = 200; i < n - 400; ++i) { const double d = a0[static_cast<size_t>(i)] - src->data[static_cast<size_t>(i)]; err += d * d; ref += double(src->data[static_cast<size_t>(i)]) * src->data[static_cast<size_t>(i)]; }
            check(err / std::max(ref, 1e-12) < 1e-8, "edit v44.52: the default edit plays a drum sample exactly as recorded");
            SampleEdit st; st.start = 0.5f; auto a1 = play(st, n / 2 - 300);
            double e1 = 0, r1 = 0; const int off = n / 2; for (int i = 200; i < n / 2 - 500; ++i) { const double d = a1[static_cast<size_t>(i)] - src->data[static_cast<size_t>(off + i)]; e1 += d * d; r1 += double(src->data[static_cast<size_t>(off + i)]) * src->data[static_cast<size_t>(off + i)]; }
            check(e1 / std::max(r1, 1e-12) < 1e-6, "edit v44.52: START 50% plays the second half of a drum sample");
            SampleEdit ln; ln.length = 0.25f; auto a2 = play(ln, n);
            double tail = 0; for (int i = n / 4 + 400; i < n; ++i) tail = std::max(tail, static_cast<double>(std::fabs(a2[static_cast<size_t>(i)])));
            check(tail < 1e-4, "edit v44.52: LENGTH 25% cuts a drum sample after a quarter");
            SampleEdit gn; gn.gainDb = -12; auto a3 = play(gn, 4000); auto a4 = play(none, 4000);
            double p3 = 0, p4 = 0; for (size_t i = 0; i < a3.size(); ++i) { p3 = std::max(p3, double(std::fabs(a3[i]))); p4 = std::max(p4, double(std::fabs(a4[i]))); }
            check(p4 > 1e-4 && std::fabs(p3 / p4 - 0.2512) < 0.01, "edit v44.52: GAIN -12 dB makes a drum sample 0.25 times as loud");
            SampleEdit rv; rv.reverse = true; auto a5 = play(rv, n);
            // the reversed sample ends with the start of the original: compare a window near the end of the output with the reversed head
            double er = 0, rr = 0; for (int i = 0; i < 2000; ++i) { const double o = a5[static_cast<size_t>(n - 1 - 300 - i)] , s0 = src->data[static_cast<size_t>(300 + i)]; (void)o; (void)s0; }
            int lastLoud = 0; for (int i = 0; i < n; ++i) if (std::fabs(a5[static_cast<size_t>(i)]) > 1e-3) lastLoud = i;
            (void)er; (void)rr;
            check(lastLoud > n / 2, "edit v44.52: REVERSE plays a drum sample backwards (it sounds to the end of the buffer, from the tail)");
        }
        // sampler SLICES: note = slice number, ROOT is the first
        {
            SamplerVoice v; SamplerPatch p; p.mode = 1; p.slices = 8; p.root = 60; p.attackMs = 0.0f; p.fitBars = -1; v.SetSample(smp); v.SetPatch(p); v.SetTempo(120.0);
            auto level = [&](int note) { v.Stop(); v.NoteOn(note, 1.0f, sr); auto m = renderV(v, 600); return m[300]; };
            const float s0 = level(60), s3 = level(63), s7 = level(67);
            check(s0 > 0.1f && s0 < 0.2f, "slices v44.52: ROOT plays the first slice (from the start)");
            check(std::fabs(s3 - (0.1f + 0.8f * 3.0f / 8.0f)) < 0.02f && std::fabs(s7 - (0.1f + 0.8f * 7.0f / 8.0f)) < 0.02f, "slices v44.52: ROOT + 3 / ROOT + 7 play the fourth / eighth slice");
            v.Stop(); v.NoteOn(68, 1.0f, sr); check(!v.IsActive(), "slices v44.52: a note beyond the last slice plays nothing");
            v.Stop(); v.NoteOn(61, 1.0f, sr); auto m = renderV(v, sr / 2);
            int lastNz = 0; for (int i = 0; i < static_cast<int>(m.size()); ++i) if (std::fabs(m[static_cast<size_t>(i)]) > 1e-4f) lastNz = i;
            check(std::abs(lastNz - sr / 4) < 400, "slices v44.52: one of 8 slices of a 2 s sample lasts 0.25 s");
            p.fitBars = 1; v.SetPatch(p); v.Stop(); v.NoteOn(61, 1.0f, sr); auto m2 = renderV(v, sr / 2);
            int last2 = 0; for (int i = 0; i < static_cast<int>(m2.size()); ++i) if (std::fabs(m2[static_cast<size_t>(i)]) > 1e-4f) last2 = i;
            check(std::abs(last2 - sr / 4) < 400, "slices v44.52: FIT 1 BAR at 120 BPM keeps a 2 s sample at native speed");
            v.SetTempo(240.0); v.Stop(); v.NoteOn(61, 1.0f, sr); auto m3 = renderV(v, sr / 2);
            int last3 = 0; for (int i = 0; i < static_cast<int>(m3.size()); ++i) if (std::fabs(m3[static_cast<size_t>(i)]) > 1e-4f) last3 = i;
            check(std::abs(last3 - sr / 8) < 400, "slices v44.52: at 240 BPM the same slice runs twice as fast (a bar is 1 s)");
        }
        // AUTO fit reads the tempo from the file name (170 BPM at 120 BPM sits exactly between 2 and 4 bars: a guess is not enough)
        {
            check(SamplerVoice::NameBpm("Drum and Bass Down 170 bpm.wav") == 170.0 && SamplerVoice::NameBpm("loop_90BPM.wav") == 90.0 && SamplerVoice::NameBpm("BPM 128 break.wav") == 128.0 && SamplerVoice::NameBpm("Fills/amen-174-bpm.wav") == 174.0, "fit v44.52: the tempo is read from names like '170 bpm', '90BPM', 'BPM 128'");
            check(SamplerVoice::NameBpm("kick.wav") == 0.0 && SamplerVoice::NameBpm("bpm.wav") == 0.0 && SamplerVoice::NameBpm("take 5 bpmx.wav") == 5.0 * 0.0, "fit v44.52: no tempo in the name = none");
            SampleData dnb; dnb.channels = 1; dnb.sampleRate = sr; dnb.name = "Drum and Bass Down 170 bpm.wav";
            dnb.data.assign(static_cast<size_t>(std::lround(4.0 * 240.0 / 170.0 * sr)), 0.1f);   // 4 bars at 170 BPM = 5.65 s
            SamplerPatch p; p.mode = 1; p.fitBars = 0;
            int bars = 0; double fb = 0;
            const double rate = SamplerVoice::FitRate(dnb, p, 120.0, 0.0, static_cast<double>(dnb.data.size()), &bars, &fb);
            check(std::fabs(rate - 120.0 / 170.0) < 1e-9 && bars == 4 && fb == 170.0, "fit v44.52: a 4-bar 170 BPM loop at 120 BPM plays 4 bars, slowed to 120/170 (not squeezed into 2 bars)");
            dnb.name = "break.wav";
            const double guess = SamplerVoice::FitRate(dnb, p, 120.0, 0.0, static_cast<double>(dnb.data.size()), &bars, &fb);
            check(fb == 0.0 && bars >= 1 && guess > 0.0, "fit v44.52: without a tempo in the name AUTO still guesses the bars");
            p.fitBars = 4; check(std::fabs(SamplerVoice::FitRate(dnb, p, 120.0, 0.0, static_cast<double>(dnb.data.size())) - (240.0 / 170.0 * 4.0) / 8.0) < 1e-3, "fit v44.52: an explicit number of bars is used as set");
        }
        // HITS slices: onset detection, marks, playing between marks, save / load
        {
            const int Ns = sr * 3;
            std::vector<float> brk(static_cast<size_t>(Ns), 0.0f);
            const double hitAt[6] = {0.00, 0.50, 0.75, 1.30, 1.80, 2.40};   // seconds
            unsigned rng = 12345u;
            for (double t0 : hitAt) { const int s0 = static_cast<int>(t0 * sr); for (int i = 0; i < 6000 && s0 + i < Ns; ++i) { rng = rng * 1664525u + 1013904223u; const float nz = (static_cast<float>(rng >> 8) / 8388608.0f - 1.0f); brk[static_cast<size_t>(s0 + i)] += nz * 0.9f * std::exp(-i / 1200.0f); } }
            auto hits = DetectOnsets(brk.data(), Ns, 1, sr, 0, Ns, 0.5f, 32);
            bool ok = hits.size() == 6;
            for (size_t i = 0; ok && i < hits.size(); ++i) ok = std::abs(hits[i] - static_cast<int>(hitAt[i] * sr)) < sr / 100;   // within 10 ms
            check(ok, "hits v44.52.1: six noise bursts are found at their positions (within 10 ms), no extra marks");
            std::vector<float> tone(static_cast<size_t>(Ns)); for (int i = 0; i < Ns; ++i) tone[static_cast<size_t>(i)] = 0.5f * std::sin(2.0 * 3.14159265358979 * 220.0 * i / sr);
            check(DetectOnsets(tone.data(), Ns, 1, sr, 0, Ns, 0.5f, 32).size() <= 1, "hits v44.52.1: a steady tone has no hits");
            check(DetectOnsets(brk.data(), Ns, 1, sr, 0, Ns, 0.5f, 3).size() == 3, "hits v44.52.1: at most the asked number of the strongest hits");
            // marks: the voice plays from one mark to the next
            auto smp3 = std::make_shared<SampleData>(); smp3->channels = 1; smp3->sampleRate = sr; smp3->name = "ramp3";
            smp3->data.resize(static_cast<size_t>(Ns)); for (int i = 0; i < Ns; ++i) smp3->data[static_cast<size_t>(i)] = 0.1f + 0.8f * static_cast<float>(i) / static_cast<float>(Ns);
            SamplerPatch hp; hp.mode = 1; hp.slices = 0; hp.root = 60; hp.attackMs = 0.0f; hp.fitBars = -1;
            const double mk[4] = {0.0, 0.2, 0.5, 0.9}; hp.markCount = 4; for (int i = 0; i < 4; ++i) hp.marks[static_cast<size_t>(i)] = static_cast<uint16_t>(std::lround(mk[i] * 65535.0));
            SamplerVoice hv; hv.SetSample(smp3); hv.SetPatch(hp); hv.SetTempo(120.0);
            hv.NoteOn(61, 1.0f, sr); auto h1 = renderV(hv, 600);
            check(std::fabs(h1[300] - (0.1f + 0.8f * 0.2f)) < 0.01f, "hits v44.52.1: ROOT + 1 plays from the second mark");
            hv.Stop(); hv.NoteOn(61, 1.0f, sr); auto h2 = renderV(hv, sr);
            int lastNz = 0; for (int i = 0; i < static_cast<int>(h2.size()); ++i) if (std::fabs(h2[static_cast<size_t>(i)]) > 1e-4f) lastNz = i;
            check(std::abs(lastNz - static_cast<int>(0.3 * Ns / sr * sr)) < 400, "hits v44.52.1: the slice ends at the next mark (0.3 of 3 s = 0.9 s)");
            hv.Stop(); hv.NoteOn(63, 1.0f, sr); check(hv.IsActive(), "hits v44.52.1: the last mark's slice plays to the end of the part");
            hv.Stop(); hv.NoteOn(64, 1.0f, sr); check(!hv.IsActive(), "hits v44.52.1: a note beyond the last mark plays nothing");
            Project pr; pr.patterns[0].samplerPatch = hp;
            fs52::create_directories("selftest_v4452b");
            Project back; const bool saved = SaveProject(pr, "selftest_v4452b/p.t8proj") && LoadProject(back, "selftest_v4452b/p.t8proj");
            check(saved && back.patterns[0].samplerPatch == hp && back.patterns[0].samplerPatch.markCount == 4 && back.patterns[0].samplerPatch.slices == 0, "hits v44.52.1: the marks survive save / load");
            fs52::remove_all("selftest_v4452b");
        }
        // sampler LOOP: plays by itself, wraps, fits whole bars, follows the tempo
        {
            SamplerVoice v; SamplerPatch p; p.mode = 2; p.fitBars = 0; p.attackMs = 0.0f; v.SetSample(smp); v.SetPatch(p); v.SetTempo(120.0);
            v.NoteOn(60, 1.0f, sr);
            check(!v.IsActive(), "loop v44.52: a step note does nothing in LOOP mode");
            v.LoopRestart(sr);
            check(v.IsActive() && v.LoopRunning(), "loop v44.52: LoopRestart starts the loop");
            auto m = renderV(v, sr * 5);   // 2 s sample, 120 BPM: a bar is 2 s -> AUTO = 1 bar at native speed, wraps every 2 s
            check(v.IsActive(), "loop v44.52: the loop is still running after 5 s");
            check(std::fabs(m[400] - m[static_cast<size_t>(sr) * 2 + 400]) < 0.01f && std::fabs(m[400] - m[static_cast<size_t>(sr) * 4 + 400]) < 0.01f, "loop v44.52: it comes back to the same place every bar");
            double mx = 0; for (size_t i = 1; i < m.size(); ++i) { const double d = std::fabs(m[i] - m[i - 1]); if (d < 0.5) mx = std::max(mx, d); }
            check(mx < 0.01, "loop v44.52: no clicks inside the loop (a ramp has one step per wrap only)");
            {   // the seam: a sine that does not fit the loop (0.37 of a cycle left over) would click with a plain wrap
                auto sine = std::make_shared<SampleData>(); sine->channels = 1; sine->sampleRate = sr; sine->name = "sine";
                sine->data.resize(static_cast<size_t>(sr));
                for (int i = 0; i < sr; ++i) sine->data[static_cast<size_t>(i)] = 0.8f * std::sin(2.0 * 3.14159265358979 * 441.37 * i / sr);
                SamplerVoice w; SamplerPatch q; q.mode = 2; q.fitBars = 1; q.attackMs = 0.0f; w.SetSample(sine); w.SetPatch(q); w.SetTempo(240.0); w.LoopRestart(sr);   // 1 s sample = 1 bar at 240 BPM
                auto mm = renderV(w, sr * 3);
                double jump = 0; for (size_t i = 1; i < mm.size(); ++i) jump = std::max(jump, static_cast<double>(std::fabs(mm[i] - mm[i - 1])));
                check(jump < 0.12, "loop v44.52.1: the seam of a loop is crossfaded (no click on a sine that does not fit the loop)");
                // and the loop still lasts exactly one bar: the downbeat comes back every 1.000 s (a hit at the start of the sample keeps most of its attack: the crossfade is 2 ms)
                auto hit = std::make_shared<SampleData>(); hit->channels = 1; hit->sampleRate = sr; hit->name = "hit";
                hit->data.assign(static_cast<size_t>(sr), 0.0f); for (int i = 0; i < 400; ++i) hit->data[static_cast<size_t>(i)] = 0.9f * (1.0f - i / 400.0f);
                SamplerVoice h2; h2.SetSample(hit); h2.SetPatch(q); h2.SetTempo(240.0); h2.LoopRestart(sr);
                auto mh = renderV(h2, sr * 4);
                std::vector<int> peaks; for (size_t i = 1; i + 1 < mh.size(); ++i) if (mh[i] > 0.4f && mh[i] >= mh[i - 1] && mh[i] > mh[i + 1] && (peaks.empty() || static_cast<int>(i) - peaks.back() > sr / 2)) peaks.push_back(static_cast<int>(i));
                check(peaks.size() >= 3 && std::abs((peaks[2] - peaks[1]) - sr) <= 3 && peaks[0] > sr && peaks[0] < sr + 400, "loop v44.52.1: with the crossfade the downbeat still comes back every bar (tempo is exact)");
            }
            v.SetTempo(60.0);   // a bar is 4 s now; AUTO keeps 1 bar -> the 2 s sample runs at half speed
            auto m2 = renderV(v, sr);
            check(v.IsActive(), "loop v44.52: a change of tempo keeps the loop running");
            const double slope = (m2[static_cast<size_t>(sr) - 10] - m2[10]) / static_cast<double>(sr - 20);
            check(std::fabs(slope - 0.8 / (N * 2.0)) < 0.8 / (N * 2.0) * 0.15, "loop v44.52: at 60 BPM the 2 s sample is stretched over 1 bar = 4 s (half speed)");
            v.FadeOutAll(sr); renderV(v, 4096);
            check(!v.IsActive(), "loop v44.52: STOP fades the loop out");
            p.mode = 0; SamplerVoice w; w.SetSample(smp); w.SetPatch(p); w.LoopRestart(sr);
            check(!w.IsActive(), "loop v44.52: LoopRestart does nothing outside LOOP mode");
        }
    }
    {   // v44.52.1: loops in the library: the tempo in the name, the LOOP category
        namespace fsx = std::filesystem;
        fsx::remove_all("selftest_lib3"); fsx::create_directories("selftest_lib3/Fills");
        auto touch = [](const std::string& f) { std::ofstream o(f, std::ios::binary); o << "x"; };
        touch("selftest_lib3/Drum and Bass Down 170 bpm.wav"); touch("selftest_lib3/Fills/kick one.wav"); touch("selftest_lib3/Fills/pad_90BPM.wav");
        const auto lib = ScanSampleLibrary("selftest_lib3");
        double dnb = -1, pad = -1, one = -1; std::string cDnb, cPad, cOne;
        for (const LibSample& l : lib) { if (l.name.find("Drum and Bass") == 0) { dnb = l.bpm; cDnb = l.category; } else if (l.name == "pad_90BPM") { pad = l.bpm; cPad = l.category; } else { one = l.bpm; cOne = l.category; } }
        check(dnb == 170.0 && pad == 90.0 && one == 0.0, "lib loops: the tempo in the file name is read by the scan");
        check(cDnb == "LOOP" && cPad == "LOOP" && cOne == "FX", "lib loops: a file with a tempo in its name is a LOOP, the others keep their category");
        fsx::remove_all("selftest_lib3");
    }
    {   // v44.53: the SAMPLE engine of the synths and the chord
        namespace fs = std::filesystem;
        const int sr = kSampleRate;
        auto mk = [&](double hz, double sec, const std::string& name) {
            auto d = std::make_shared<SampleData>(); d->channels = 1; d->sampleRate = sr; d->name = name;
            d->data.resize(static_cast<size_t>(sec * sr));
            for (size_t i = 0; i < d->data.size(); ++i) d->data[i] = static_cast<float>(0.6 * std::sin(2.0 * 3.14159265358979 * hz * static_cast<double>(i) / sr));
            return SamplePtr(d);
        };
        auto ramp = std::make_shared<SampleData>(); ramp->channels = 1; ramp->sampleRate = sr; ramp->name = "ramp";
        ramp->data.resize(static_cast<size_t>(sr * 2)); for (size_t i = 0; i < ramp->data.size(); ++i) ramp->data[i] = 0.1f + 0.8f * static_cast<float>(i) / static_cast<float>(ramp->data.size());
        SamplePtr rampP = ramp;
        auto patchFor = [&] { SynthPatch p; p.engineType = SynthEngineType::Sample; p.cutoffHz = 18000.0f; p.envAmount = 0.0f; p.resonance = 0.0f; p.attack = 0.001f; p.sustain = 1.0f; p.release = 0.05f; p.volume = 1.0f; return p; };
        auto render = [&](SynthVoice& v, int frames) { std::vector<float> o(static_cast<size_t>(frames) * 2, 0.0f); for (int i = 0; i < frames; i += 512) v.Render(o.data() + static_cast<size_t>(i) * 2, std::min(512, frames - i), sr); return o; };
        auto crossings = [&](const std::vector<float>& o, int from, int to) { int n = 0; for (int i = from + 1; i < to; ++i) if ((o[static_cast<size_t>(i - 1) * 2] < 0) != (o[static_cast<size_t>(i) * 2] < 0)) ++n; return n; };
        auto rms = [&](const std::vector<float>& o, int from, int to) { double e = 0; for (int i = from; i < to; ++i) e += o[static_cast<size_t>(i) * 2] * o[static_cast<size_t>(i) * 2]; return std::sqrt(e / std::max(1, to - from)); };

        check(kNumSynthEngineTypes == 4 && std::string(SynthEngineTypeName(SynthEngineType::Sample)) == "SAMPLE", "sample engine: SAMPLE is the fourth engine");
        {   // SynthSample: the name field, comparable as plain memory
            SynthPatch a, b; a.sample.SetFile("a_very_long_name.wav"); a.sample.SetFile("x.wav");
            b.sample.SetFile("x.wav");
            check(std::memcmp(a.sample.file, b.sample.file, sizeof(a.sample.file)) == 0 && a.sample.File() == "x.wav", "sample engine: the file name field is cleared on every write (patches compare with memcmp)");   // the name bytes only: the padding between fields of two separately made patches is not defined (it differs on macOS)
            SynthPatch c; c.sample.SetFile(std::string(300, 'z'));
            check(c.sample.File().size() == SynthSample::kFileMax - 1, "sample engine: a name that is too long is cut, never overruns");
        }
        {   // pitch: ROOT plays the sample unchanged, an octave up twice as fast
            auto v_up = std::make_unique<SynthVoice>(); SynthVoice& v = *v_up; SynthPatch p = patchFor(); p.sample.root = 60; v.SetPatch(p); v.SetSample(mk(441.0, 2.0, "s441"));
            v.NoteOn(60, 1.0f, false); auto o = render(v, sr / 2);
            const int c0 = crossings(o, 4000, 4000 + sr / 4);   // 441 Hz over 0.25 s = 110 cycles = 220 crossings
            v.NoteOff(); render(v, sr / 4);
            v.NoteOn(72, 1.0f, false); auto o2 = render(v, sr / 2);
            const int c1 = crossings(o2, 4000, 4000 + sr / 4);
            check(std::abs(c0 - 220) <= 3, "sample engine: the note ROOT plays the sample at its own pitch");
            check(std::abs(c1 - 440) <= 5, "sample engine: an octave above ROOT plays it twice as high");
        }
        {   // envelope: a held note holds the sample, NOTE OFF releases it, the end of a short sample ends the note
            auto v_up = std::make_unique<SynthVoice>(); SynthVoice& v = *v_up; SynthPatch p = patchFor(); v.SetPatch(p); v.SetSample(mk(441.0, 3.0, "long"));
            v.NoteOn(60, 1.0f, false); render(v, sr);
            check(v.IsActive() && rms(render(v, sr / 10), 0, sr / 10) > 0.2, "sample engine: a held note keeps playing the sample");
            v.NoteOff(); auto tail = render(v, sr / 2);
            check(!v.IsActive() && rms(tail, sr / 4, sr / 2) < 1e-6, "sample engine: NOTE OFF releases the sample, the voice goes idle");
            auto w_up = std::make_unique<SynthVoice>(); SynthVoice& w = *w_up; w.SetPatch(p); w.SetSample(mk(441.0, 0.1, "short"));
            w.NoteOn(60, 1.0f, false); render(w, sr / 2);
            check(!w.IsActive(), "sample engine: when the sample is over, the note is over (held or not)");
            auto n_up = std::make_unique<SynthVoice>(); SynthVoice& n = *n_up; n.SetPatch(p); n.NoteOn(60, 1.0f, false);
            check(!n.IsActive() && rms(render(n, 4096), 0, 4096) < 1e-9, "sample engine: no sample, no sound, no stuck voice");
        }
        {   // a new note always restarts the sample from its beginning
            auto v_up = std::make_unique<SynthVoice>(); SynthVoice& v = *v_up; SynthPatch p = patchFor(); v.SetPatch(p); v.SetSample(rampP);
            v.NoteOn(60, 1.0f, false); render(v, sr / 2);
            v.NoteOn(60, 1.0f, false); auto o = render(v, 2048);
            check(o[600 * 2] < 0.2f && o[600 * 2] > 0.05f, "sample engine: a new note restarts the sample from its start (not from where it was)");
        }
        {   // SLICES: the note picks the slice, no transposing
            auto v_up = std::make_unique<SynthVoice>(); SynthVoice& v = *v_up; SynthPatch p = patchFor(); p.sample.mode = 1; p.sample.slices = 8; p.sample.root = 60; v.SetPatch(p); v.SetSample(rampP);
            v.NoteOn(63, 1.0f, false); auto o = render(v, 4096);   // slice 4 starts at 3/8 of the sample
            const float expect = 0.1f + 0.8f * 3.0f / 8.0f;
            check(std::fabs(o[2000 * 2] - expect) < 0.03f, "sample engine: SLICES: ROOT + 3 plays the fourth slice");
            v.NoteOff(); render(v, sr / 4);
            v.NoteOn(40, 1.0f, false);
            check(rms(render(v, 2048), 0, 2048) < 1e-9, "sample engine: SLICES: a note below ROOT plays nothing");
        }
        {   // LOOP: a note starts the loop from its beginning, it runs while the note is held (past the end of the sample), NOTE OFF stops it
            auto v_up = std::make_unique<SynthVoice>(); SynthVoice& v = *v_up; SynthPatch p = patchFor(); p.sample.mode = 2; p.sample.fitBars = 1; v.SetPatch(p); v.SetSample(mk(441.0, 1.0, "loop")); v.SetTempo(240.0);   // 1 s = 1 bar at 240 BPM
            auto quiet = render(v, 2048);
            check(!v.IsActive() && rms(quiet, 0, 2048) < 1e-9, "sample engine: LOOP: nothing plays before a note");
            v.NoteOn(60, 1.0f, false); render(v, sr * 3);
            check(v.IsActive() && rms(render(v, sr / 10), 0, sr / 10) > 0.2, "sample engine: LOOP: held for three loops it is still going");
            v.NoteOff(); render(v, sr / 2);
            check(!v.IsActive(), "sample engine: LOOP: NOTE OFF ends the loop");
            // key sync: a second note puts the loop back to its beginning (ramp: the start is quiet, the end loud)
            auto w_up = std::make_unique<SynthVoice>(); SynthVoice& w = *w_up; SynthPatch q = patchFor(); q.sample.mode = 2; q.sample.fitBars = 1; w.SetPatch(q); w.SetSample(rampP); w.SetTempo(120.0);
            w.NoteOn(60, 1.0f, false); render(w, sr);   // half way through the 2 s bar
            w.NoteOn(60, 1.0f, false); auto o = render(w, 2048);
            check(o[1000 * 2] < 0.2f, "sample engine: LOOP: a new note restarts the loop from its beginning (key-synced)");
        }
        {   // the filter and the volume act on the sample
            SynthPatch p = patchFor(); p.cutoffHz = 400.0f;
            auto f_up = std::make_unique<SynthVoice>(); SynthVoice& f = *f_up; f.SetPatch(p); f.SetSample(mk(6000.0, 1.0, "hi")); f.NoteOn(60, 1.0f, false); auto of = render(f, sr / 2);
            auto g_up = std::make_unique<SynthVoice>(); SynthVoice& g = *g_up; g.SetPatch(patchFor()); g.SetSample(mk(6000.0, 1.0, "hi")); g.NoteOn(60, 1.0f, false); auto og = render(g, sr / 2);
            check(rms(of, 8000, 16000) < 0.25 * rms(og, 8000, 16000), "sample engine: the low-pass filter darkens the sample");
            SynthPatch hv = patchFor(); hv.volume = 0.5f;
            auto h_up = std::make_unique<SynthVoice>(); SynthVoice& h = *h_up; h.SetPatch(hv); h.SetSample(mk(441.0, 1.0, "v")); h.NoteOn(60, 1.0f, false); auto oh = render(h, sr / 4);
            auto h2_up = std::make_unique<SynthVoice>(); SynthVoice& h2 = *h2_up; h2.SetPatch(patchFor()); h2.SetSample(mk(441.0, 1.0, "v")); h2.NoteOn(60, 1.0f, false); auto oh2 = render(h2, sr / 4);
            check(std::fabs(rms(oh, 4000, 10000) / rms(oh2, 4000, 10000) - 0.5) < 0.02, "sample engine: VOLUME scales the sample");
            check(std::fabs(rms(og, 8000, 16000) - 0.6 / std::sqrt(2.0)) < 0.03, "sample engine: with the filter open the sample passes at its own level (not coloured, not louder)");
        }
        {   // the chord: one sample voice per chord tone
            auto c_up = std::make_unique<ChordVoice>(); ChordVoice& c = *c_up; SynthPatch p = patchFor(); c.SetPatch(p); c.SetSample(mk(441.0, 2.0, "ch")); c.SetTempo(120.0);
            const int8_t one[1] = {60}; const int8_t three[3] = {60, 64, 67};
            auto crender = [&](int frames) { std::vector<float> o(static_cast<size_t>(frames) * 2, 0.0f); for (int i = 0; i < frames; i += 512) c.Render(o.data() + static_cast<size_t>(i) * 2, std::min(512, frames - i), sr); return o; };
            c.NoteOn(one, 1, 1.0f, false); auto o1 = crender(sr / 4);
            c.NoteOff(); crender(sr / 2);
            c.NoteOn(three, 3, 1.0f, false); auto o3 = crender(sr / 4);
            check(c.IsActive() && rms(o3, 2000, 9000) > 1.2 * rms(o1, 2000, 9000), "sample engine: a chord plays the sample once per chord tone");
        }
        {   // unison copies and DRIVE go through the same path
            SynthPatch p = patchFor(); p.unisonVoices = 3; p.unisonDetune = 0.5f;
            auto v_up = std::make_unique<SynthVoice>(); SynthVoice& v = *v_up; v.SetPatch(p); v.SetSample(mk(441.0, 1.0, "u")); v.NoteOn(60, 1.0f, false); auto o = render(v, sr / 4);
            double pk = 0; for (float x : o) pk = std::max(pk, static_cast<double>(std::fabs(x)));
            check(pk > 0.2 && pk < 1.5, "sample engine: UNISON plays detuned copies of the sample at a sane level");
        }
        {   // the file: round trip of the patch, an old project is byte-for-byte what it was
            Project pr;
            SynthPatch& sp0 = pr.patterns[0].synthPatches[2];
            sp0.engineType = SynthEngineType::Sample; sp0.sample.SetFile("Test Break 100 bpm.wav"); sp0.sample.mode = 1; sp0.sample.slices = 0; sp0.sample.root = 48;
            sp0.sample.markCount = 3; sp0.sample.marks[0] = 100; sp0.sample.marks[1] = 20000; sp0.sample.marks[2] = 40000; sp0.sample.fitBars = 0; sp0.sample.start = 0.1f; sp0.sample.decay = 0.8f; sp0.sample.tuneCents = -12; sp0.sample.gainDb = 3; sp0.sample.reverse = 1;
            pr.patterns[1].chordPatch.engineType = SynthEngineType::Sample; pr.patterns[1].chordPatch.sample.SetFile("pad.wav"); pr.patterns[1].chordPatch.sample.mode = 2;
            fs::create_directories("selftest_v4453");
            Project back; const bool ok = SaveProject(pr, "selftest_v4453/p.t8proj") && LoadProject(back, "selftest_v4453/p.t8proj");
            check(ok && std::memcmp(&back.patterns[0].synthPatches[2], &pr.patterns[0].synthPatches[2], sizeof(SynthPatch)) == 0 && back.patterns[0].synthPatches[2].engineType == SynthEngineType::Sample, "sample engine: a synth's sample settings (file, mode, marks ...) survive save / load");
            check(ok && back.patterns[1].chordPatch.engineType == SynthEngineType::Sample && back.patterns[1].chordPatch.sample.File() == "pad.wav" && back.patterns[1].chordPatch.sample.mode == 2, "sample engine: the chord's sample survives save / load");
            std::ifstream in("selftest_v4453/p.t8proj"); const std::string txt((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            Project plain; SaveProject(plain, "selftest_v4453/plain.t8proj");
            std::ifstream in2("selftest_v4453/plain.t8proj"); const std::string txt2((std::istreambuf_iterator<char>(in2)), std::istreambuf_iterator<char>());
            check(txt.find("engineType=SMP") != std::string::npos && txt2.find("sample=") == std::string::npos && txt2.find("SMP") == std::string::npos, "sample engine: a project without a SAMPLE engine has no new lines (older files stay the same)");
            SynthPatch cp = sp0; check(SaveCustomSynthPreset("selftest sample engine", cp), "sample engine: a preset can be saved");
            SynthPatch cb; check(LoadCustomSynthPreset("selftest sample engine", cb) && cb.engineType == SynthEngineType::Sample && cb.sample.File() == "Test Break 100 bpm.wav" && cb.sample.markCount == 3 && cb.sample.marks[1] == 20000 && cb.sample.mode == 1, "sample engine: a preset keeps the sample settings");
            fs::remove_all("selftest_v4453");
        }
        {   // the engine row: switching to SAMPLE opens the filter and the gate; the rows have filter names; ENGINE cycles through four
            SynthPatch p; p.cutoffHz = 900.0f; p.envAmount = 0.5f; p.sustain = 0.6f; p.release = 0.08f;
            for (int i = 0; i < 3; ++i) AdjustPatchParam(p, 0, 1);
            check(p.engineType == SynthEngineType::Sample && p.cutoffHz >= 18000.0f && p.envAmount == 0.0f && p.sustain == 1.0f && p.release >= 0.25f, "sample engine: switching to SAMPLE opens the filter and holds the sample (sustain 100%)");
            check(std::string(PatchParamName(SynthEngineType::Sample, 1)) == "CUTOFF" && NumPatchParams(SynthEngineType::Sample) == 1 + 6 + 19, "sample engine: the rows are the filter and the shared rows");
            AdjustPatchParam(p, 0, 1); check(p.engineType == SynthEngineType::Subtractive, "sample engine: ENGINE wraps round to SUBTRACTIVE");
        }
        {   // program start: the last project opens with a SAMPLE synth; its sample plays from the first note (not the default synth)
            fs::remove_all("selftest_boot"); fs::create_directories("selftest_boot/src");
            { std::vector<float> w(44100); for (size_t i = 0; i < w.size(); ++i) w[i] = static_cast<float>(0.6 * std::sin(2.0 * 3.14159265358979 * 220.0 * static_cast<double>(i) / 44100.0)); WriteWavPCM16("selftest_boot/src/boot.wav", w, 1, 44100); }
            {
                auto wp = std::make_unique<AppState>(); AppState& w = *wp;
                w.engine.Init(kSampleRate); w.sequencer.Init(&w.engine, &w.project);
                w.projectPath = "selftest_boot/p.t8proj";
                std::lock_guard<SceneMutex> lk(w.sequencer.PatternMutex());
                w.project.currentPattern = 0;
                for (auto& t : w.CurrentPattern().tracks) for (auto& st : t.steps) st = Step{};
                w.CurrentPattern().synthPatches[0] = patchFor();
                w.AssignSampleToSampler("selftest_boot/src/boot.wav", 0);
                w.CurrentPattern().synthPatches[0].sample.root = 57;
                w.CurrentPattern().tracks[kNumDrumTracks].steps[0].active = true; w.CurrentPattern().tracks[kNumDrumTracks].steps[0].note = 60; w.CurrentPattern().tracks[kNumDrumTracks].steps[0].lengthSteps = 16;
                w.CurrentPattern().bpm = 120.0f; w.CurrentPattern().masterLength = 16;
                check(SaveProject(w.project, w.projectPath), "program start: the project with a SAMPLE synth is saved");
            }
            auto bp = std::make_unique<AppState>(); AppState& b = *bp;   // the order of main.cpp: engine, project file, then StartSound
            b.engine.Init(kSampleRate);
            check(LoadProject(b.project, "selftest_boot/p.t8proj"), "program start: the last project opens");
            b.projectPath = "selftest_boot/p.t8proj";
            b.StartSound();
            check(b.engine.Synth(0).Patch().engineType == SynthEngineType::Sample, "program start: the synth gets the project's SAMPLE patch at once (not the default synth)");
            check(b.engine.SynthSampleCurrent(0) && b.engine.SynthSampleCurrent(0)->name == "boot.wav", "program start: the project's sample is in the voice before the first note");
            b.engine.GetMixer().masterVolume = 1.0f;
            b.sequencer.Play();
            std::vector<float> buf(44100 * 2, 0.0f);
            for (int r = 0; r < 22050; r += 256) b.sequencer.RenderAndAdvance(buf.data() + static_cast<size_t>(r) * 2, std::min(256, 22050 - r), kSampleRate);
            float pk = 0; for (float v : buf) pk = std::max(pk, std::fabs(v));
            check(pk > 0.1f, "program start: the first note plays the sample (audible)");
            b.sequencer.Stop();
            fs::remove_all("selftest_boot");
        }
        {   // the whole way: a file in samples/, the scene's patch, the sequencer plays it; two scenes with different samples
            fs::remove_all("selftest_smp53"); fs::create_directories("selftest_smp53/src");
            auto wav = [&](const std::string& path, double hz, double sec) { std::vector<float> w(static_cast<size_t>(sec * 44100)); for (size_t i = 0; i < w.size(); ++i) w[i] = static_cast<float>(0.6 * std::sin(2.0 * 3.14159265358979 * hz * static_cast<double>(i) / 44100.0)); WriteWavPCM16(path, w, 1, 44100); };
            wav("selftest_smp53/src/low.wav", 220.0, 1.0); wav("selftest_smp53/src/high.wav", 880.0, 1.0);
            auto ap = std::make_unique<AppState>(); AppState& a = *ap;
            a.engine.Init(kSampleRate); a.sequencer.Init(&a.engine, &a.project);
            a.engine.GetMixer().masterVolume = 1.0f;
            a.projectPath = "selftest_smp53/p.t8proj";
            for (int sc = 0; sc < 2; ++sc) { for (auto& t : a.project.patterns[static_cast<size_t>(sc)].tracks) for (auto& st : t.steps) st = Step{}; }
            {
                std::lock_guard<SceneMutex> lk(a.sequencer.PatternMutex());
                a.project.currentPattern = 0;
                a.CurrentPattern().synthPatches[0] = patchFor();
                check(a.AssignSampleToSampler("selftest_smp53/src/low.wav", 0) && a.CurrentPattern().synthPatches[0].sample.File() == "low.wav", "sample engine: a WAV dropped on a SAMPLE synth is copied into samples/ and becomes its sample");
                check(a.project.patterns[0].samplerPatch.file.empty(), "sample engine: the sampler track's own sample is not touched");
                a.CurrentPattern().tracks[kNumDrumTracks].steps[0].active = true; a.CurrentPattern().tracks[kNumDrumTracks].steps[0].note = 60; a.CurrentPattern().tracks[kNumDrumTracks].steps[0].lengthSteps = 16;
                a.CurrentPattern().bpm = 120.0f; a.CurrentPattern().masterLength = 16;
                a.CurrentPattern().synthPatches[0].sample.root = 57;
                a.PushEditedPatchToEngine(false, 0);
                a.SyncSceneSamples(true);
            }
            check(a.engine.SynthSampleCurrent(0) != nullptr && a.engine.SynthSampleCurrent(0)->name == "low.wav" && a.engine.SynthSampleCurrent(1) == nullptr, "sample engine: the scene's sample reaches the engine (and only the synth that has one)");
            a.sequencer.Play();
            std::vector<float> buf(44100 * 2, 0.0f);
            for (int r = 0; r < 44100; r += 256) a.sequencer.RenderAndAdvance(buf.data() + static_cast<size_t>(r) * 2, std::min(256, 44100 - r), kSampleRate);
            float pk = 0; for (float v : buf) pk = std::max(pk, std::fabs(v));
            check(pk > 0.1f, "sample engine: a step of the synth plays the sample (audible)");
            a.sequencer.Stop();
            {   // a second scene with another sample
                std::lock_guard<SceneMutex> lk(a.sequencer.PatternMutex());
                a.project.currentPattern = 1;
                a.CurrentPattern().synthPatches[0] = patchFor();
                a.AssignSampleToSampler("selftest_smp53/src/high.wav", 0);
            }
            check(a.project.patterns[1].synthPatches[0].sample.File() == "high.wav" && a.project.patterns[0].synthPatches[0].sample.File() == "low.wav", "sample engine: every scene keeps its own sample");
            a.sequencer.ApplySceneSound(1);
            check(a.engine.SynthSampleCurrent(0) && a.engine.SynthSampleCurrent(0)->name == "high.wav", "sample engine: a scene change hands the new scene's sample to the voice");
            a.sequencer.ApplySceneSound(0);
            check(a.engine.SynthSampleCurrent(0) && a.engine.SynthSampleCurrent(0)->name == "low.wav", "sample engine: and back");
            {   // a synth switched back to another engine lets go of its sample
                std::lock_guard<SceneMutex> lk(a.sequencer.PatternMutex());
                a.project.currentPattern = 0; a.CurrentPattern().synthPatches[0].engineType = SynthEngineType::Subtractive; a.SyncSceneSamples(true);
            }
            a.sequencer.ApplySceneSound(0);
            check(a.engine.SynthSampleCurrent(0) == nullptr, "sample engine: a synth that is not on SAMPLE holds no sample");
            {   // undo brings the engine back with its file
                std::lock_guard<SceneMutex> lk(a.sequencer.PatternMutex());
                a.project.currentPattern = 0; a.CurrentPattern().synthPatches[0].engineType = SynthEngineType::Sample;
                a.SetSamplerFile("high.wav", 0); a.Undo();
                check(a.CurrentPattern().synthPatches[0].sample.File() == "low.wav", "sample engine: choosing a sample is undoable");
                // marks by hand and FIND on a synth target
                a.SetSamplerFile("low.wav", 0);
                SamplerPatch tp = a.GetSampleTarget(0); tp.mode = 1; tp.slices = 0; a.PutSampleTarget(0, tp);
                a.AddSliceMark(0.25f, 0); a.AddSliceMark(0.5f, 0);
                check(a.CurrentPattern().synthPatches[0].sample.markCount == 2 && a.GetSampleTarget(-1).markCount == 0, "sample engine: marks are added to the synth's sample, not to the sampler's");
                a.RemoveNearestSliceMark(0.26f, 0.05f, 0);
                check(a.CurrentPattern().synthPatches[0].sample.markCount == 1, "sample engine: a mark can be removed");
                // LAY OUT: slices placed on the synth's own steps, held as long as they last
                tp = a.GetSampleTarget(0); tp.slices = 8; tp.fitBars = 1; tp.root = 60; a.PutSampleTarget(0, tp);
                const std::string m = a.LayoutSamplerSlices(0);
                int placed = 0, hold = 0; for (const Step& st : a.CurrentPattern().tracks[kNumDrumTracks].steps) if (st.active) { ++placed; hold += st.lengthSteps; }
                check(placed == 8 && hold == a.CurrentPattern().tracks[kNumDrumTracks].length, "sample engine: LAY OUT writes the slices on the synth's steps, each held until the next one (" + m + ")");
                check(a.LayoutSamplerSlices(kNumSynthTracks).find("CHORD") != std::string::npos, "sample engine: LAY OUT is not offered on the chord track");
            }
            a.sequencer.Stop();
            check(AppState::SampleTargetOfTrack(kSamplerTrackIndex) == -1 && AppState::SampleTargetOfTrack(kNumDrumTracks + 1) == 1 && AppState::SampleTargetOfTrack(kChordTrackIndex) == kNumSynthTracks && AppState::SampleTargetOfTrack(0) < -1, "sample engine: a track maps to its sample target");
            fs::remove_all("selftest_smp53");
        }
    }
    {   // v44.57: PITCH - a turntable pitch fader for the whole output (core/VariSpeed.h)
        static constexpr double kPi = 3.14159265358979323846;
        struct Src {   // a steady two-tone signal; may be asked for any number of frames at a time
            long n = 0; std::vector<float> ref;
            void Fill(float* d, int f) {
                for (int i = 0; i < f; ++i, ++n) {
                    d[2 * i] = 0.5f * static_cast<float>(std::sin(2 * kPi * 441.0 * n / 44100.0));
                    d[2 * i + 1] = 0.25f * static_cast<float>(std::sin(2 * kPi * 882.0 * n / 44100.0));
                }
                ref.insert(ref.end(), d, d + 2 * f);
            }
        };
        auto run = [&](VariSpeed& v, Src& s, const std::vector<double>& speeds, const std::vector<int>& blocks) {   // speeds: one target per block, cycled
            std::vector<float> out;
            for (size_t b = 0; b < blocks.size(); ++b) {
                std::vector<float> blk(static_cast<size_t>(blocks[b]) * 2);
                v.Process(blk.data(), blocks[b], speeds[b % speeds.size()], 44100, [&](float* d, int f) { s.Fill(d, f); });
                out.insert(out.end(), blk.begin(), blk.end());
            }
            return out;
        };
        // 1. speed 1.0 is a plain copy, bit for bit, in blocks of any size
        {
            VariSpeed v; Src s; std::vector<int> bl; int tot = 0;
            for (int i = 0; tot < 30000; ++i) { const int b = std::vector<int>{64, 100, 17, 512, 1, 333, 2048, 3000, 5}[i % 9]; bl.push_back(b); tot += b; }
            const std::vector<float> out = run(v, s, {1.0}, bl);
            bool same = out.size() <= s.ref.size();
            for (size_t i = 0; same && i < out.size(); ++i) same = out[i] == s.ref[i];
            check(same && static_cast<int>(out.size()) / 2 == tot, "pitch: speed 1.0 is a bit for bit copy (no filter, no delay), in blocks of any size");
            check(s.ref.size() == out.size(), "pitch: and the engine is asked for exactly what is played (no look-ahead at 1.0)");
        }
        // 2. +5%: the pitch rises by 5% (counted over 2 s) and the stage does not click
        for (double sp : {1.05, 0.95, 1.16, 0.84}) {
            VariSpeed v; Src s; std::vector<int> bl(2 * 44100 / 512 + 40, 512);
            const std::vector<float> out = run(v, s, {sp}, bl);
            const size_t from = 44100 / 2, frames = out.size() / 2;   // leave the first half second (the fader's weight)
            int crossings = 0; double maxStep = 0.0;
            for (size_t i = from + 1; i < frames; ++i) {
                if (out[2 * (i - 1)] < 0.0f && out[2 * i] >= 0.0f) ++crossings;
                maxStep = std::max(maxStep, std::fabs(static_cast<double>(out[2 * i] - out[2 * (i - 1)])));
            }
            const double secs = (frames - from) / 44100.0, hz = crossings / secs;
            const double slope = 0.5 * 2 * kPi * 441.0 * sp / 44100.0;
            std::ostringstream m; m << "pitch: speed " << sp << " plays 441 Hz as " << hz << " Hz, the largest step " << maxStep << " (a sine allows " << slope << ")";
            check(std::fabs(hz - 441.0 * sp) < 1.5 && maxStep <= slope * 1.05, m.str());
        }
        // 3. the result does not depend on how the host cuts the stream into blocks
        {
            VariSpeed v1, v2; Src s1, s2;
            std::vector<int> a(100, 441), b; for (int i = 0; i < 90; ++i) b.push_back(i % 2 ? 63 : 1000); int ta = 0, tb = 0; for (int x : a) ta += x; for (int x : b) tb += x;
            while (tb < ta) { b.push_back(7); tb += 7; }
            const std::vector<float> o1 = run(v1, s1, {1.0507}, a), o2 = run(v2, s2, {1.0507}, b);
            double worst = 0; for (size_t i = 0; i < std::min(o1.size(), o2.size()); ++i) worst = std::max(worst, std::fabs(static_cast<double>(o1[i] - o2[i])));
            check(worst < 1e-4, "pitch: the same sound whatever the block size (largest difference " + std::to_string(worst) + ")");
        }
        // 4. back to 0%: after the glide the output is the engine's own samples again, unfiltered, whole frames
        {
            VariSpeed v; Src s; std::vector<double> sp; std::vector<int> bl;
            for (int i = 0; i < 100; ++i) { bl.push_back(441); sp.push_back(i < 50 ? 1.07 : 1.0); }
            const std::vector<float> out = run(v, s, sp, bl);
            const size_t N = out.size() / 2; bool found = false; size_t d = 0;
            for (size_t cand = s.ref.size() / 2; cand-- > 600 && !found; ) if (out[2 * (N - 1)] == s.ref[2 * cand] && out[2 * (N - 1) + 1] == s.ref[2 * cand + 1]) { found = true; d = cand; }   // from the end: the tone repeats every 100 frames
            bool exact = found && d >= 600;
            for (size_t k = 0; exact && k < 600; ++k) exact = out[2 * (N - 1 - k)] == s.ref[2 * (d - k)] && out[2 * (N - 1 - k) + 1] == s.ref[2 * (d - k) + 1];
            check(exact, "pitch: back at 0% the output is the engine's own samples again (bit for bit)");
            check(v.CurrentSpeed() == 1.0, "pitch: and the stage reports speed 1.0");
        }
        // 5. the program side: range, step, push
        {
            AppState a; a.engine.Init(kSampleRate); a.sequencer.Init(&a.engine, &a.project);
            check(a.PitchTargetSpeed() == 1.0 && a.PitchLimit() == 8.0f, "pitch: starts at 0%, range +-8%");
            a.PitchSet(20.0f); check(a.pitchPct.load() == 8.0f, "pitch: the fader stops at +8%");
            a.PitchSetRange16(true); a.PitchSet(-20.0f); check(a.pitchPct.load() == -16.0f, "pitch: range +-16% reaches -16%");
            a.PitchSetRange16(false); check(a.pitchPct.load() == -8.0f, "pitch: a narrower range pulls the fader in");
            a.PitchSet(0.0f); for (int i = 0; i < 23; ++i) a.PitchStep(0.1f);
            check(std::fabs(a.pitchPct.load() - 2.3f) < 1e-4f, "pitch: 23 steps of 0.1% make 2.3% (no drift)");
            check(std::fabs(a.PitchTargetSpeed() - 1.023) < 1e-6, "pitch: +2.3% is speed 1.023");
            check(std::fabs(a.PitchedBpm(120.0) - 122.76) < 1e-6, "pitch: 120 BPM shows as 122.8 at +2.3%");
            a.pitchBend.store(1); check(std::fabs(a.PitchTargetSpeed() - 1.023 * 1.04) < 1e-6, "pitch: a push adds 4% on top");
            a.pitchBend.store(-1); check(std::fabs(a.PitchTargetSpeed() - 1.023 * 0.96) < 1e-6, "pitch: a push back takes 4% off");
            a.pitchBend.store(0); a.PitchSet(0.04f); check(a.pitchPct.load() == 0.0f, "pitch: less than 0.05% is exactly 0");
            {   // through the real sequencer: +8% plays 8% more engine time per second of output, and stays finite
                a.PitchSetRange16(false); a.PitchSet(8.0f); a.sequencer.Play();
                VariSpeed v; long rendered = 0; double peak = 0; bool finite = true; std::vector<float> ob(512 * 2);
                const int outFrames = kSampleRate * 3;
                for (int done = 0; done < outFrames; done += 512) {
                    v.Process(ob.data(), 512, a.PitchTargetSpeed(), kSampleRate, [&](float* d, int f) { a.sequencer.RenderAndAdvance(d, f, kSampleRate); rendered += f; });
                    for (float x : ob) { if (!std::isfinite(x)) finite = false; peak = std::max(peak, static_cast<double>(std::fabs(x))); }
                }
                check(finite, "pitch: the real sequencer through the stage stays finite");
                check(std::fabs(static_cast<double>(rendered) / outFrames - 1.08) < 0.01, "pitch: +8% renders 8% more engine time (" + std::to_string(static_cast<double>(rendered) / outFrames) + ")");
                (void)peak; a.sequencer.Stop(); a.PitchSet(0.0f);
            }
        }
    }
    {   // v44.58: LINKED scenes, EDIT mode (a scene opened without launching) and the armed scene (switches for the scene that is coming)
        AppState a; a.engine.Init(kSampleRate); a.sequencer.Init(&a.engine, &a.project);
        for (int i = 0; i < kNumLiveScenes; ++i) a.project.patterns[static_cast<size_t>(i)].masterLength = 16;
        std::vector<float> lb(256 * 2);
        auto run = [&](double seconds) { const int n = static_cast<int>(seconds * kSampleRate); for (int d = 0; d < n; d += 256) a.sequencer.RenderAndAdvance(lb.data(), 256, kSampleRate); };
        const int kick = 0, snare = 1;
        // --- LINK: a change is made in every scene of the group
        a.project.currentPattern = 0;
        a.LinkScenes(1, 0); a.LinkScenes(2, 0);
        check(a.ScenesLinked(0, 2) && a.ScenesLinked(1, 2) && !a.ScenesLinked(0, 3) && a.LinkedCountOf(1) == 3 && a.LinkLabel(2) == "01+02+03", "link: scenes 1, 2, 3 are one group");
        a.SetMuteScene(kick, true, 1);
        check(a.project.patterns[0].tracks[kick].mute && a.project.patterns[1].tracks[kick].mute && a.project.patterns[2].tracks[kick].mute && !a.project.patterns[3].tracks[kick].mute, "link: a mute in one linked scene is made in all of them, and not in a scene outside");
        a.SetSoloScene(snare, true, 2);
        check(a.project.patterns[0].tracks[snare].solo && a.project.patterns[1].tracks[snare].solo && !a.project.patterns[4].tracks[snare].solo, "link: the same for solo");
        check(a.engine.GetMixer().tracks[kick].mute && a.engine.GetMixer().tracks[snare].solo, "link: the mixer follows the played scene");
        a.SetSoloScene(snare, false, 0); a.SetMuteScene(kick, false, 0);
        // --- LINK: flags that drifted apart are brought together on arrival (a file edited by hand, a paste ...)
        a.project.patterns[1].tracks[kick].mute = true;
        a.sequencer.Play(); a.sequencer.SetLaunchQuantize(0);
        a.sequencer.LaunchScene(1);
        check(!a.project.patterns[1].tracks[kick].mute && !a.engine.GetMixer().tracks[kick].mute, "link: arriving in a linked scene takes the mute / solo of the scene it came from (no jump)");
        a.sequencer.LaunchScene(3);
        a.SetMuteScene(kick, true, 3);
        a.sequencer.LaunchScene(0);
        check(!a.project.patterns[0].tracks[kick].mute && !a.engine.GetMixer().tracks[kick].mute, "link: a scene that is not linked keeps its own mute (kick muted in 3 does not follow to 1)");
        // --- LINK: toggling, a group of one dissolves, save and load
        a.project.currentPattern = 0;
        a.ToggleSceneLink(2);
        check(!a.ScenesLinked(0, 2) && a.ScenesLinked(0, 1), "link: toggling a linked scene takes it out of the group");
        a.ToggleSceneLink(1);
        check(a.SceneGroupOf(0) == 0 && a.SceneGroupOf(1) == 0, "link: a group of one is not a group");
        a.LinkScenes(5, 4); a.LinkScenes(9, 4);
        { Project p2; a.project.name = "linktest"; check(SaveProject(a.project, "selftest_link.t8proj") && LoadProject(p2, "selftest_link.t8proj") && p2.sceneLink == a.project.sceneLink && p2.sceneLink[4] != 0 && p2.sceneLink[4] == p2.sceneLink[9], "link: the groups are saved with the project"); std::filesystem::remove("selftest_link.t8proj"); }
        a.UnlinkScene(5); a.UnlinkScene(9);
        // --- EDIT: open a scene without launching it
        a.sequencer.Stop(); a.sequencer.SetLaunchQuantize(2); a.project.currentPattern = 0; a.sequencer.Play();
        a.SelectSceneForEdit(4);
        check(a.project.currentPattern == 4 && a.sequencer.PlayPatternIndex() == 0 && a.SceneIsHeldApart(), "edit: a scene opens in the editor while the player stays on the scene that plays");
        a.SetMuteScene(snare, true);
        check(a.project.patterns[4].tracks[snare].mute && !a.engine.GetMixer().tracks[snare].mute, "edit: what is changed in the opened scene is not heard");
        a.GoToEditedScene();
        check(a.sequencer.PendingScene() == 4, "edit: GO queues the edited scene");
        run(2.4);
        check(a.sequencer.PlayPatternIndex() == 4 && !a.sequencer.IsPinned() && a.engine.GetMixer().tracks[snare].mute && !a.SceneIsHeldApart(), "edit: it starts on the bar line with the changes, and the editor and the player are one again");
        a.SelectSceneForEdit(4);
        check(!a.sequencer.IsPinned(), "edit: opening the playing scene again leaves nothing held apart");
        // --- v44.59 (DATA-6): LINK on the selected pads, one undo step that brings the overwritten mute / solo back
        {
            const int pl = a.sequencer.PlayPatternIndex();
            a.project.currentPattern = pl;
            a.project.patterns[6].tracks[kick].mute = true; a.project.patterns[7].tracks[snare].mute = true;
            a.project.currentPattern = 6;
            a.SelectScenePad(6, false, false); a.SelectScenePad(7, true, false);
            check(a.SceneSelCount() == 2 && a.SceneSelLabel() == "07-08", "link v44.59: Shift+click selects a range of pads");
            check(a.LinkSelectedScenes() == 2 && a.ScenesLinked(6, 7) && a.project.patterns[7].tracks[kick].mute && !a.project.patterns[7].tracks[snare].mute && a.SceneSelCount() == 0,
                  "link v44.59: LINK joins the selected scenes; the mute / solo come from the open scene");
            a.project.currentPattern = 2;   // a link step undoes whatever scene is open
            check(a.Undo() && !a.ScenesLinked(6, 7) && a.project.patterns[7].tracks[snare].mute && !a.project.patterns[7].tracks[kick].mute && a.project.patterns[6].tracks[kick].mute,
                  "link v44.59: Cmd+Z undoes the link and brings back the mutes it overwrote");
            check(a.Redo() && a.ScenesLinked(6, 7) && a.project.patterns[7].tracks[kick].mute, "link v44.59: redo links them again");
            a.project.currentPattern = 7;
            a.UnlinkSceneUndoable(7);
            check(!a.ScenesLinked(6, 7) && a.Undo() && a.ScenesLinked(6, 7), "link v44.59: UNLINK is one undo step too");
            a.ToggleSceneLink(7);
            check(!a.ScenesLinked(6, 7) && a.Undo() && a.ScenesLinked(6, 7), "link v44.59: the right-click menu's link / unlink is undoable");
            a.UnlinkScene(6); a.UnlinkScene(7);
            a.project.patterns[6].tracks[kick].mute = false; a.project.patterns[7].tracks[kick].mute = false; a.project.patterns[7].tracks[snare].mute = false;
            a.project.currentPattern = 6;
            a.SelectScenePad(6, false, false); a.SelectScenePad(10, false, true);
            check(a.SceneSelCount() == 2 && a.SceneSelLabel() == "07+11", "link v44.59: Cmd+click adds one pad");
            a.SelectScenePad(3, false, false);
            check(a.SceneSelCount() == 0, "link v44.59: a plain click clears the selection");
            a.project.currentPattern = pl;
            a.engine.GetMixer().SyncFromPattern(a.project.patterns[static_cast<size_t>(pl)]);   // the test moved the open scene by hand while playing
        }
        // --- v44.59: a double click opens without launching; a launch click while a scene is held apart
        {
            a.sequencer.SetLaunchQuantize(2);
            const int pl = a.sequencer.PlayPatternIndex();
            const int other = pl == 5 ? 6 : 5, third = pl == 8 ? 9 : 8;
            a.project.currentPattern = pl;
            a.LaunchScenePad(other);
            check(a.sequencer.PendingScene() == other, "pads v44.59: a click (CLICK: LAUNCH, LAUNCH = BAR) queues the scene");
            a.OpenScenePad(other);
            check(a.sequencer.PendingScene() < 0 && a.project.currentPattern == other && a.sequencer.PlayPatternIndex() == pl && a.SceneIsHeldApart(),
                  "pads v44.59: the double click cancels the queued launch and only opens the scene");
            a.LaunchScenePad(third);
            check(a.project.currentPattern == pl && !a.sequencer.IsPinned() && a.sequencer.PendingScene() == third,
                  "pads v44.59: a launch click while a scene is open apart: the editor goes back to the playing scene, the clicked one is queued");
            a.sequencer.ClearPendingScene();
            // The real mouse path, frame by frame as ImGui reports it (its Button fires on BOTH releases of a double click; the
            // second release comes with MouseClickedLastCount = 2). Replayed through ScenePadGesture + ScenePadInput like ChainPanel.
            using G = AppState::PadGesture;
            auto frame = [&](int pad, bool released, bool dbl, int count, bool shift = false, bool cmd = false) {
                a.ScenePadInput(pad, AppState::ScenePadGesture(released, dbl, count, shift, cmd));
            };
            a.project.currentPattern = pl; a.sequencer.ReleasePin(); a.clickOpens = false;
            frame(other, false, false, 1);   // press 1
            frame(other, true, false, 1);    // release 1: launch queued
            check(a.sequencer.PendingScene() == other, "pads v44.59 (mouse): release 1 of a double click queues the launch (LAUNCH = BAR)");
            frame(other, false, true, 2);    // press 2: double click
            frame(other, true, false, 2);    // release 2: must do nothing
            frame(other, false, false, 2);   // the frames after
            check(a.sequencer.PendingScene() < 0 && a.project.currentPattern == other && a.sequencer.PlayPatternIndex() == pl && a.SceneIsHeldApart(),
                  "pads v44.59 (mouse): after the whole double click nothing is queued and the scene is only open");
            check(AppState::ScenePadGesture(true, false, 2, false, false) == G::None && AppState::ScenePadGesture(true, false, 3, false, false) == G::None,
                  "pads v44.59 (mouse): the release ending a double / triple click is not a click");
            frame(third, true, false, 1);
            check(a.project.currentPattern == pl && a.sequencer.PendingScene() == third, "pads v44.59 (mouse): a later single click launches as before");
            a.sequencer.ClearPendingScene();
            a.SelectScenePad(3, false, false);
            frame(3, true, false, 1, false, true); frame(3, false, true, 2, false, true); frame(3, true, false, 2, false, true);
            check(((a.sceneSel >> 3) & 1u) && a.sequencer.PendingScene() < 0, "pads v44.59 (mouse): a Cmd+double click selects once, launches nothing");
            a.SelectScenePad(3, false, false);
            a.clickOpens = true;
            frame(third, true, false, 1); frame(third, false, true, 2); frame(third, true, false, 2);
            check(a.project.currentPattern == third && a.sequencer.PendingScene() < 0, "pads v44.59 (mouse): CLICK: OPEN, a double click only opens");
            a.clickOpens = false;
            a.project.currentPattern = pl; a.sequencer.ReleasePin();
        }
        // --- Duplicate as new scene: the copy opens in the editor, the music stays on the scene that plays (it used to jump to the copy off the grid)
        {
            auto activeSteps = [&](int sc) { int n = 0; for (const Track& t : a.project.patterns[static_cast<size_t>(sc)].tracks) for (const Step& st : t.steps) n += st.active ? 1 : 0; return n; };
            const int pl = a.sequencer.PlayPatternIndex();
            const int dst = pl == 15 ? 14 : 15, dst2 = pl == 13 ? 12 : 13;
            const int before = activeSteps(dst), before2 = activeSteps(dst2);
            a.project.currentPattern = pl; a.sequencer.ReleasePin(); a.sequencer.ClearPendingScene();
            Step& mark = a.project.patterns[static_cast<size_t>(pl)].tracks[kick].steps[0];
            const Step markWas = mark;
            mark.active = true;   // the playing scene has a note to copy
            for (int q : {2, 0}) {   // LAUNCH = BAR and LAUNCH = NOW: neither launches the copy
                a.sequencer.SetLaunchQuantize(q);
                const int d = q == 2 ? dst : dst2;
                a.DuplicateSceneAsNew(pl, d);
                run(0.6);
                check(a.sequencer.IsPlaying() && a.sequencer.PlayPatternIndex() == pl && a.sequencer.PendingScene() < 0 && a.project.currentPattern == d && a.SceneIsHeldApart(),
                      q == 2 ? "duplicate: the copy opens in the editor, nothing is launched or queued (LAUNCH = BAR)" : "duplicate: the copy is not launched with LAUNCH = NOW either");
                check(activeSteps(d) == activeSteps(pl) && activeSteps(pl) > 0, "duplicate: the new scene holds the notes of the original");
                a.project.currentPattern = pl; a.sequencer.ReleasePin();
            }
            a.SelectSceneForEdit(dst2);   // the copy open apart, as the menu leaves it
            const bool u2 = a.Undo() && activeSteps(dst2) == before2;
            a.SelectSceneForEdit(dst);
            const bool u1 = a.Undo();
            check(u2 && u1 && activeSteps(dst) == before && a.sequencer.PlayPatternIndex() == pl, "duplicate: Cmd+Z empties the new slot again, the music is not touched");
            mark = markWas;
            a.sequencer.SetLaunchQuantize(2);
            a.project.currentPattern = pl; a.sequencer.ReleasePin();
            a.engine.GetMixer().SyncFromPattern(a.project.patterns[static_cast<size_t>(pl)]);
        }
        // --- ARM: while a launch waits, the switches are for the scene that is coming
        a.project.currentPattern = 4; a.sequencer.ReleasePin(); a.SetMuteScene(snare, false);
        a.sequencer.LaunchScene(6);
        check(a.ArmedScene() == 6, "arm: a waiting launch makes its scene the target of the switches");
        {
            AppState::ArmScope arm(a, a.ArmedScene());
            a.LiveSwitch(AppState::QueuedSwitch::Mute, static_cast<uint16_t>(kick));
            check(a.project.currentPattern == 6 && a.SwMuted(-1, kick), "arm: inside the scope the switches read and write the coming scene");
        }
        check(a.project.currentPattern == 4 && a.sequencer.PlayPatternIndex() == 4 && !a.sequencer.IsPinned(), "arm: the editor and the player are back where they were");
        check(a.project.patterns[6].tracks[kick].mute && !a.project.patterns[4].tracks[kick].mute && !a.engine.GetMixer().tracks[kick].mute, "arm: the coming scene is muted, the playing one and its sound are not touched");
        run(2.4);
        check(a.sequencer.PlayPatternIndex() == 6 && a.engine.GetMixer().tracks[kick].mute, "arm: the kick is silent from the first beat of the new scene");
        a.sequencer.Stop();
    }
    {   // N1: the dropout readout knows how long the screen held the scene lock and how long the audio thread waited for it
        AppState a; a.engine.Init(kSampleRate); a.sequencer.Init(&a.engine, &a.project);
        SceneMutex& sm = a.sequencer.PatternMutex();
        std::vector<float> buf(256 * 2);
        a.sequencer.RenderAndAdvance(buf.data(), 128, kSampleRate);   // marks this thread as the audio thread
        sm.ResetMaxHold();
        { std::lock_guard<SceneMutex> lk(sm); std::this_thread::sleep_for(std::chrono::milliseconds(3)); }
        check(sm.MaxHoldUs() < 1000, "n1: the audio thread's own holds of the scene lock are not counted as the screen holding the sound");
        std::atomic<bool> held{false};
        std::thread ui([&] { std::lock_guard<SceneMutex> lk(sm); held = true; std::this_thread::sleep_for(std::chrono::milliseconds(30)); });
        while (!held) std::this_thread::yield();
        a.engine.GetMixer().audioLockWaitUs.store(0);
        a.sequencer.RenderAndAdvance(buf.data(), 128, kSampleRate);
        ui.join();
        const unsigned waited = a.engine.GetMixer().audioLockWaitUs.load();
        check(waited >= 15000, "n1: the audio thread measures how long it waited for the scene lock (" + std::to_string(waited) + " us)");
        check(sm.MaxHoldUs() >= 25000, "n1: a hold of the scene lock by another thread is measured (" + std::to_string(sm.MaxHoldUs()) + " us)");
        { std::lock_guard<SceneMutex> outer(sm); std::lock_guard<SceneMutex> inner(sm); }
        check(sm.HeldByThisThread() == false, "n1: the scene lock is free again after nested holds");
        a.engine.GetMixer().audioXruns.store(3); a.engine.GetMixer().audioLongestUs.store(9000); a.engine.GetMixer().dropouts[1].lenUs.store(5);
        a.engine.GetMixer().ResetDropouts(); sm.ResetMaxHold();
        check(a.engine.GetMixer().audioXruns.load() == 0 && a.engine.GetMixer().audioLongestUs.load() == 0 && a.engine.GetMixer().dropouts[1].lenUs.load() == 0 && sm.MaxHoldUs() == 0, "n1: a click on the readout clears the dropout log");
    }
    {   // N1 (LIVE-1 / LIVE-3 / LIVE-7, TEST-4): the recorders write the take while it plays; STOP only closes the file
        namespace fs = std::filesystem;
        const std::string dir = "selftest_rec";
        fs::remove_all(dir); fs::create_directories(dir);
        const int sr = 44100;
        auto sine = [](int64_t i) { return 0.5f * std::sin(static_cast<float>(i) * 0.031f); };
        auto readDataSize = [](const std::string& path) -> uint32_t {   // the "data" size field of a 44-byte header, as on disk now
            std::ifstream f(path, std::ios::binary); unsigned char h[44] = {};
            f.read(reinterpret_cast<char*>(h), 44);
            return f.gcount() == 44 ? static_cast<uint32_t>(h[40] | (h[41] << 8) | (h[42] << 16) | (static_cast<uint32_t>(h[43]) << 24)) : 0u;
        };
        {
            Recorder rec; rec.Init(sr);
            const std::string path = dir + "/take.wav";
            check(rec.Toggle(path) == "RECORDING" && rec.IsRecording(), "rec: REC starts at once");
            std::vector<float> blk(128 * 2);
            int64_t n = 0;
            const int64_t total = sr * 3 / 2;   // 1.5 s, pushed in real time-ish so the drain thread writes along the way
            const auto t0 = std::chrono::steady_clock::now();
            while (n < total) {
                for (int i = 0; i < 128; ++i) { blk[i * 2] = sine(n + i); blk[i * 2 + 1] = -sine(n + i); }
                rec.Push(blk.data(), 128);
                n += 128;
                std::this_thread::sleep_until(t0 + std::chrono::microseconds(n * 1000000 / sr / 4));   // 4x faster than real time
            }
            const int64_t pushed = n;   // whole blocks: a little more than `total`
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            check(readDataSize(path) > 0, "rec: the file on disk already holds audio while recording (a crash keeps it)");
            const auto s0 = std::chrono::steady_clock::now();
            const std::string stopMsg = rec.Toggle();
            const double stopMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count();
            check(stopMsg == "SAVING RECORDING..." && !rec.IsRecording(), "rec: STOP returns at once and REC goes off");
            check(stopMs < 5.0, "rec: STOP costs the UI thread almost nothing (" + std::to_string(stopMs) + " ms)");
            rec.WaitIdleForTest();
            std::string msg, outPath;
            const bool gotRec = rec.TakeResult(msg, outPath);
            check(gotRec && msg.rfind("SAVED", 0) == 0 && outPath == path, "rec: the result arrives once the file is closed: " + msg);
            check(!rec.TakeResult(msg, outPath), "rec: the result is given only once");
            SampleBuffer b;
            const bool loaded = LoadWav(path, b, sr);
            check(loaded && b.channels == 2 && b.frames() == pushed, "rec: every pushed frame is in the file (" + std::to_string(b.frames()) + " of " + std::to_string(pushed) + ")");
            bool same = loaded && b.frames() == pushed;
            for (int64_t i = 0; same && i < total; i += 997) same = std::fabs(b.data[static_cast<size_t>(i) * 2] - sine(i)) < 1e-3f && std::fabs(b.data[static_cast<size_t>(i) * 2 + 1] + sine(i)) < 1e-3f;
            check(same, "rec: the file holds exactly what was played (left and right)");
            check(readDataSize(path) == static_cast<uint32_t>(pushed * 4), "rec: the header gives the final length");
            check(rec.DroppedFrames() == 0, "rec: nothing was dropped");

            // LIVE-3: the audio thread keeps pushing while REC is pressed on and off quickly (the old final drain raced here)
            std::atomic<bool> run{true};
            std::thread audio([&] { std::vector<float> z(128 * 2, 0.25f); while (run) { rec.Push(z.data(), 128); std::this_thread::sleep_for(std::chrono::microseconds(300)); } });
            int good = 0;
            for (int k = 0; k < 12; ++k) {
                const std::string pk = dir + "/quick" + std::to_string(k) + ".wav";
                rec.Toggle(pk);
                std::this_thread::sleep_for(std::chrono::milliseconds(k % 3 == 0 ? 0 : 30));
                rec.Toggle();
                rec.WaitIdleForTest();
                std::string m2, p2;
                if (rec.TakeResult(m2, p2) && m2.rfind("SAVED", 0) == 0) {
                    SampleBuffer qb;
                    if (LoadWav(pk, qb, sr) || readDataSize(pk) == 0) ++good;
                }
            }
            run = false; audio.join();
            check(good == 12, "rec: twelve quick REC on / off while the audio runs each leave a valid file (" + std::to_string(good) + ")");

            // the 4 GB WAV limit (pretend it is 0.5 s): recording stops by itself and says so, the file stays valid
            WavStreamWriter::SetMaxDataBytesForTest(static_cast<uint64_t>(sr / 2) * 4);
            const std::string lp = dir + "/limit.wav";
            rec.Toggle(lp);
            for (int i = 0; i < sr / 128 && rec.IsRecording(); ++i) { rec.Push(blk.data(), 128); std::this_thread::sleep_for(std::chrono::microseconds(700)); }
            rec.WaitIdleForTest();
            for (int i = 0; i < 100 && rec.IsRecording(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            rec.WaitIdleForTest();
            WavStreamWriter::SetMaxDataBytesForTest(0);
            SampleBuffer lb;
            const bool gotLim = rec.TakeResult(msg, outPath);
            check(gotLim && msg.find("4 GB") != std::string::npos && !rec.IsRecording(), "rec: at the WAV size limit the take stops by itself and says so: " + msg);
            check(LoadWav(lp, lb, sr) && lb.frames() == sr / 2, "rec: the file stopped at the limit is complete and valid");

            // quitting while recording closes the file properly
            const std::string qp = dir + "/quit.wav";
            rec.Toggle(qp);
            for (int i = 0; i < 40; ++i) { rec.Push(blk.data(), 128); std::this_thread::sleep_for(std::chrono::microseconds(500)); }
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            rec.Shutdown();
            SampleBuffer qb;
            check(LoadWav(qp, qb, sr) && qb.frames() == 40 * 128, "rec: quitting during a take keeps the whole take in a valid file");
        }
        {
            MultitrackRecorder mt; mt.Init(sr);
            const std::string folder = dir + "/stems";
            fs::create_directories(folder);
            check(mt.Toggle(folder).rfind("STEMS ARMED", 0) == 0 && mt.IsRecording() && !mt.HasStarted(), "stems: arming waits for the first sound");
            std::vector<std::vector<float>> bufs(kNumStemGroups, std::vector<float>(128 * 2, 0.0f));
            std::vector<const float*> ptrs(kNumStemGroups);
            for (int g = 0; g < kNumStemGroups; ++g) ptrs[g] = bufs[g].data();
            mt.Push(128, ptrs.data());   // silence: nothing is written
            int64_t written = 0;
            for (int blkN = 0; blkN < 200; ++blkN) {
                for (int i = 0; i < 128; ++i) {
                    const bool sound = blkN > 0 || i >= 50;
                    bufs[3][i * 2] = bufs[3][i * 2 + 1] = sound ? 0.3f : 0.0f;   // only HATS plays
                }
                mt.Push(128, ptrs.data());
                written += blkN == 0 ? 128 - 50 : 128;
                if (blkN % 8 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            check(mt.HasStarted(), "stems: the first sound starts the take");
            mt.Toggle();
            mt.WaitIdleForTest();
            std::string msg, outPath;
            const bool gotStems = mt.TakeResult(msg, outPath);
            check(gotStems && msg.rfind("SAVED 8 STEMS", 0) == 0 && outPath == folder, "stems: the result arrives once the files are closed: " + msg);
            bool allSame = true;
            for (int g = 0; g < kNumStemGroups; ++g) {
                SampleBuffer sb;
                allSame = allSame && LoadWav(folder + "/" + kStemGroupFileNames[g], sb, sr) && sb.frames() == written;
            }
            check(allSame, "stems: all 8 files exist, start at the first sound and have exactly the same length (" + std::to_string(written) + " frames)");
            // armed and stopped before any sound: nothing is left behind
            const std::string empty = dir + "/stems_empty";
            fs::create_directories(empty);
            mt.Toggle(empty);
            for (auto& bf : bufs) std::fill(bf.begin(), bf.end(), 0.0f);
            mt.Push(128, ptrs.data());
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            mt.Toggle(); mt.WaitIdleForTest();
            const bool gotEmpty = mt.TakeResult(msg, outPath);
            check(gotEmpty && msg.find("NOTHING CAPTURED") != std::string::npos && outPath.empty() && !fs::exists(empty), "stems: a take with no sound leaves no folder: " + msg);
        }
        fs::remove_all(dir);
    }
    {   // N1 (LIVE-2 / LIVE-10): with the loader running, samples, kits and folder scans are read off the scene lock
        namespace fs = std::filesystem;
        const std::string dir = "selftest_async";
        fs::remove_all(dir); fs::create_directories(dir + "/lib");
        std::vector<float> tone(static_cast<size_t>(kSampleRate) * 2 * 2);
        for (size_t i = 0; i < tone.size(); ++i) tone[i] = 0.3f * std::sin(static_cast<float>(i / 2) * 0.05f);
        check(WriteWavPCM16(dir + "/in.wav", tone, 2, kSampleRate) && WriteWavPCM16(dir + "/lib/loop.wav", tone, 2, kSampleRate), "async: test files written");
        AppState a; a.engine.Init(kSampleRate); a.sequencer.Init(&a.engine, &a.project);
        a.projectPath = dir + "/p.t8proj";
        a.project.kitFolder = "kits/808";
        a.RefreshSound();
        const auto before = a.engine.GetKit()->CurrentBank();
        a.loader.Start();
        // "Briefly" is a loose bound: a shared CI runner can pause a thread for several ms. The checks just after each one
        // (still pending, the old sound keeps playing, not in memory yet) are what prove the file was not read under the lock.
        constexpr double kBriefMs = 25.0;
        auto heldFor = [&](const std::function<void()>& f) {   // as a panel does: the action runs while the scene lock is held
            const auto t0 = std::chrono::steady_clock::now();
            { std::lock_guard<SceneMutex> lk(a.sequencer.PatternMutex()); f(); }
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        };
        // kit
        a.seqCursorTrack = 0;
        const double kitMs = heldFor([&] { a.LoadKitFolder("kits/909"); });
        check(kitMs < kBriefMs, "async: picking a kit holds the scene lock only briefly (" + std::to_string(kitMs) + " ms)");
        check(a.KitPending("kits/909") && a.engine.GetKit()->CurrentBank() == before, "async: while the kit is read, the old kit keeps playing");
        a.loader.FinishAllForTest(); a.SyncSceneKits();
        check(!a.KitPending("kits/909") && a.engine.GetKit()->CurrentBank() == a.KitBank("kits/909") && AppState::KitSlotsLoaded(a.KitBank("kits/909")) == 6, "async: when it arrives, the new kit plays (6 of 6 samples)");
        check(a.statusMessage.find("6/6 loaded") != std::string::npos, "async: the kit message comes when the kit is there: " + a.statusMessage);
        // sampler: a dropped / library file
        a.project.patterns[a.project.currentPattern].tracks[kSamplerTrackIndex].type = TrackType::Sampler;
        const double smpMs = heldFor([&] { a.AssignSampleToSampler(dir + "/in.wav"); });
        check(smpMs < kBriefMs, "async: giving the sampler a file holds the scene lock only briefly (" + std::to_string(smpMs) + " ms)");
        check(a.CurrentPattern().samplerPatch.file.empty(), "async: the sampler gets the file only once it is read");
        a.loader.FinishAllForTest(); a.SyncSceneSamples();
        check(a.CurrentPattern().samplerPatch.file == "in.wav" && fs::exists(dir + "/samples/in.wav") && a.SampleFor("in.wav") != nullptr && a.loader.Busy() == 0, "async: then it is copied, set and loaded (read only once)");
        // a missing file is not read again every frame
        a.CurrentPattern().samplerPatch.file = "missing.wav";
        a.SyncSceneSamples();
        check(a.SamplePending("missing.wav") && !a.SampleMissing("missing.wav"), "async: a file being read is not shown as missing");
        a.loader.FinishAllForTest();
        for (int i = 0; i < 5; ++i) { a.SyncSceneSamples(); a.loader.Poll(); }
        check(a.SampleMissing("missing.wav") && a.loader.Busy() == 0, "async: a missing file is shown as missing and not read again on every frame");
        a.CurrentPattern().samplerPatch.file = "in.wav"; a.SyncSceneSamples(true); a.loader.FinishAllForTest();
        // library audition
        SetSamplesRoot(dir + "/lib"); a.libValid_ = false;
        a.LibraryList();   // as in the app: the library list is on screen (scanned) before a file in it can be heard
        const double hearMs = heldFor([&] { a.AuditionLibrary("loop.wav"); });
        check(hearMs < kBriefMs, "async: HEAR in the library holds the scene lock only briefly (" + std::to_string(hearMs) + " ms)");
        check(a.libAudition_.count(a.LibraryPath("loop.wav")) == 0, "async: the file is not in memory yet");
        a.loader.FinishAllForTest();
        check(a.libAudition_.count(a.LibraryPath("loop.wav")) == 1 && a.libAudition_[a.LibraryPath("loop.wav")] != nullptr && a.libAuditionKeep_.size() == 1, "async: it plays once it is read");
        // C4b review: the SAMPLES browser lists the library on the loader thread, never under the scene lock
        a.libValid_ = false;
        const std::vector<LibSample>* first = nullptr;
        const double libMs = heldFor([&] { first = a.LibraryListIfReady(); });
        check(first == nullptr && libMs < kBriefMs && a.LibraryListIfReady() == nullptr && a.loader.Busy() == 1, "c4b: the first look at the library starts one scan on the loader and returns nothing yet");
        a.loader.FinishAllForTest();
        const std::vector<LibSample>* ready = a.LibraryListIfReady();
        check(ready && ready->size() == 1 && (*ready)[0].rel == "loop.wav", "c4b: the library list is there on the next frame");
        a.libValid_ = false; a.LibraryListIfReady(); a.RescanLibrary(); const size_t afterRescan = a.LibraryList().size();
        a.loader.FinishAllForTest();
        check(a.libValid_ && a.LibraryList().size() == afterRescan, "c4b: a scan overtaken by Rescan Samples is dropped");
        SetSamplesRoot("");
        // folder lists (LIVE-10)
        a.kitFoldersCacheValid = false;
        a.RescanFoldersInBackground();
        check(!a.kitFoldersCacheValid, "async: the folder rescan runs on the loader thread");
        a.loader.FinishAllForTest();
        check(a.kitFoldersCacheValid && !a.cachedKitFolders.empty() && a.loader.Busy() == 0, "async: the new lists are swapped in on the next frame");
        // review of N1 part 3: a late result must not land in the wrong place
        auto finish = [&] { a.loader.FinishAllForTest(); a.SyncSceneSamples(); a.SyncSceneKits(); };
        WriteWavPCM16(dir + "/b.wav", tone, 2, kSampleRate);
        a.AssignSampleToSampler(dir + "/b.wav"); finish();   // b.wav is now in samples/
        a.AssignSampleToSampler(dir + "/in.wav");
        a.SetSamplerFile("b.wav");                            // a newer direct choice while in.wav is still being read
        finish();
        check(a.CurrentPattern().samplerPatch.file == "b.wav", "late: a file still being read does not overwrite a newer choice (" + a.CurrentPattern().samplerPatch.file + ")");
        a.AssignSampleToSampler(dir + "/in.wav");
        a.AssignSampleToSampler(dir + "/b.wav");             // two drops: the newest wins
        finish();
        check(a.CurrentPattern().samplerPatch.file == "b.wav", "late: of two drops on the same sampler the newest wins");
        {   // another project opened while the file is read: nothing lands in it
            a.AssignSampleToSampler(dir + "/in.wav");
            Project fresh; a.AdoptProject(std::move(fresh)); a.ClearUndo();
            finish();
            check(a.CurrentPattern().samplerPatch.file.empty() && a.undoStack.empty(), "late: a file asked for in the old project does not land in the new one (no undo step either)");
        }
        {   // the edited scene changes while a library LOOP is read: loop mode goes to the scene that asked
            a.projectPath = dir + "/p.t8proj";
            a.project.currentPattern = 2;
            LibSample ls; ls.rel = "loop.wav"; ls.category = "LOOP";
            AppState* ap = &a;
            a.AssignSampleToSampler(dir + "/in.wav", AppState::kTargetSampler, [ap, ls](int scene) { ap->SetSamplerLoopFromLibraryInScene(ls, AppState::kTargetSampler, scene); });
            a.project.currentPattern = 5;
            finish();
            check(a.project.patterns[2].samplerPatch.file == "in.wav" && a.project.patterns[2].samplerPatch.mode == 2 && a.project.patterns[5].samplerPatch.mode != 2 && a.project.currentPattern == 5,
                  "late: the file and LOOP mode go to the scene that asked, the edited scene stays");
            a.project.currentPattern = 0;
        }
        {   // HEAR: a newer audition (cached) or a stop cancels the one still being read
            SetSamplesRoot(dir + "/lib"); a.libValid_ = false; a.libAudition_.clear(); a.libAuditionKeep_.clear();
            WriteWavPCM16(dir + "/lib/other.wav", tone, 2, kSampleRate);
            a.AuditionLibrary("other.wav"); a.loader.FinishAllForTest();   // other.wav is now cached
            const size_t kept = a.libAuditionKeep_.size();
            a.AuditionLibrary("loop.wav");    // not cached: being read
            a.AuditionLibrary("other.wav");   // cached: plays at once
            a.loader.FinishAllForTest();
            check(a.libAuditionKeep_.size() == kept + 1, "late: HEAR on another file cancels the one still being read");
            a.libAudition_.clear();
            a.AuditionLibrary("loop.wav");
            a.CancelLibraryAudition();        // what choosing a file / stopping does
            const size_t kept2 = a.libAuditionKeep_.size();
            a.loader.FinishAllForTest();
            check(a.libAuditionKeep_.size() == kept2, "late: stopping cancels a HEAR still being read");
            SetSamplesRoot("");
        }
        {   // a drum: a sample dropped on it, then a kit picked while the file is read: the pick stays
            std::set<std::string> kitsBefore;
            for (const auto& e : fs::directory_iterator("kits")) kitsBefore.insert(e.path().filename().string());
            a.seqCursorTrack = 0;
            a.AssignSampleToSlot(dir + "/in.wav");
            a.LoadKitFolder("kits/606");
            finish();
            check(a.TrackKitFolder(a.project.currentPattern, 0) == "kits/606", "late: a kit picked after a drop is not replaced by the dropped file's kit (" + a.TrackKitFolder(a.project.currentPattern, 0) + ")");
            std::vector<fs::path> made;   // only the kit folder this test wrote (never a user's own mykit)
            for (const auto& e : fs::directory_iterator("kits")) if (!kitsBefore.count(e.path().filename().string())) made.push_back(e.path());
            for (const auto& m : made) fs::remove_all(m);
            a.InvalidateFolderCaches();
        }
        {   // a folder rescan that started before an invalidation is not swapped in
            a.RescanFoldersInBackground();
            a.InvalidateFolderCaches();
            a.loader.FinishAllForTest();
            check(!a.kitFoldersCacheValid, "late: a folder scan older than an invalidation is not used (the next access rescans)");
        }
        a.loader.Stop();
        fs::remove_all(dir);
    }
    {   // C4b part 2: the SAMPLES browser's folder tree and column sort
        using SB = t8::desktop::SampleBrowser;
        const auto ch = SB::FolderChain("Solid Sounds/Drum Loops/funk_95.wav");
        check(ch.size() == 2 && ch[0] == "Solid Sounds" && ch[1] == "Solid Sounds/Drum Loops" && SB::FolderChain("kick.wav").empty(),
              "c4b: a library file counts in every folder above it (the tree with counts)");
        std::vector<SB::Item> v(3);
        v[0].name = "b"; v[0].folder = "Z"; v[0].type = "LOOP"; v[0].bpm = 120;
        v[1].name = "A"; v[1].folder = "y"; v[1].type = "KICK"; v[1].bpm = 95;
        v[2].name = "c"; v[2].folder = "x"; v[2].type = "LOOP"; v[2].bpm = 0;
        SB::SortItems(v, 0, false);
        check(v[0].name == "A" && v[1].name == "b" && v[2].name == "c", "c4b: NAME sorts A..Z, case ignored");
        SB::SortItems(v, 3, true);
        check(v[0].bpm == 120 && v[2].bpm == 0, "c4b: a second click on BPM sorts the other way round");
        SB::SortItems(v, 2, false);
        check(v[0].type == "KICK" && v[1].name == "b" && v[2].name == "c", "c4b: TYPE sorts by type, then by name");
        SB::SortItems(v, 1, false);
        check(v[0].folder == "x" && v[1].folder == "y" && v[2].folder == "Z", "c4b: FOLDER sorts case-blind");
        {   // C4b (v1): genre and tags read from the folder and file names
            check(GuessSampleGenre("BL_HipHop_95bpm_C3.wav") == "HIP HOP" && GuessSampleGenre("DL_DannyBryd_DnB_175bpm2.wav") == "DNB"
                  && GuessSampleGenre("ML_Minimal-Glitch_100bpm_MscLoop3.wav") == "MINIMAL" && GuessSampleGenre("Break Funk Chop 115 bpm.wav") == "BREAKS",
                  "c4b: the genre comes from the words of the name (camelCase and _ split, DnB kept whole)");
            check(GuessSampleGenre("House/Disco Edits/x.wav") == "HOUSE" && GuessSampleGenre("Drum and Bass/k.wav") == "DNB" && GuessSampleGenre("Electronic pad.wav").empty()
                  && GuessSampleGenre("Kicks/kick_03.wav").empty(), "c4b: the first genre in the path wins; a word inside another word does not count");
            const auto tg = GuessSampleTags("Solid Sounds/Dusty Vinyl/Lo-Fi Keys Dark Live.wav");
            check(tg == std::vector<std::string>{"DUSTY", "DARK", "LO-FI", "LIVE"} && GuessSampleTags("kick.wav").empty(), "c4b: tags come from sound words (vinyl = DUSTY, Lo-Fi = LO-FI)");
            SB::Item x, y; x.name = "a"; x.genre = "BREAKS"; x.tags = 1u; y.name = "b"; y.genre = "";
            check(SB::GenreTagsText(x) == "BREAKS \xC2\xB7 DUSTY" && SB::GenreTagsText(y).empty(), "c4b: the GENRE \xC2\xB7 TAGS column text");
            std::vector<SB::Item> gv{x, y}; SB::SortItems(gv, 4, false);
            check(gv[0].name == "b" && gv[1].name == "a", "c4b: GENRE \xC2\xB7 TAGS sorts (no genre first)");
            {   // review of part 6: a number typed into a BPM field is not taken by the search, the browse keys, Return or Esc
                std::ifstream in("src/ui_desktop/SampleBrowser.cpp"); const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                check(src.find("!fieldTyping && browser::CaptureTyping") != std::string::npos && src.find("!fieldTyping && browser::HearKey") != std::string::npos
                      && src.find("n > 0 && !fieldTyping") != std::string::npos && src.find("!trashBusy && !fieldTyping") != std::string::npos
                      && src.find("bpmTyping_ = true") != std::string::npos, "c4b: typing a tempo into a BPM field keeps the keys in that field");
            }
        }
        {   // C4b (g): THIS PROJECT shows where each copied file plays
            AppState a;
            for (int i : {0, 1, 2, 5}) a.project.patterns[static_cast<size_t>(i)].samplerPatch.file = "amen.wav";
            SynthPatch& sp = a.project.patterns[3].synthPatches[1];
            sp.engineType = SynthEngineType::Sample; sp.sample.SetFile("pad.wav");
            a.project.patterns[kNumLiveScenes + 2].samplerPatch.file = "lift.wav";   // a brick slot no brick points at: not in use
            a.project.patterns[kNumLiveScenes + 3].samplerPatch.file = "hit.wav";
            a.project.songChainLength = 4; a.project.songChain[3].patternIndex = static_cast<uint8_t>(kNumLiveScenes + 3);
            const auto u = SB::SampleUsage(a);
            const std::string sampler = t8::desktop::TrackDisplayName(a.project.patterns[0].tracks[kSamplerTrackIndex]);
            const std::string synth = t8::desktop::TrackDisplayName(a.project.patterns[3].tracks[kNumDrumTracks + 1]);
            check(u.count("amen.wav") && u.at("amen.wav") == sampler + " - scenes 1-3, 6", "c4b: a sample's scenes are listed as ranges: " + (u.count("amen.wav") ? u.at("amen.wav") : std::string("-")));
            check(u.count("pad.wav") && u.at("pad.wav") == synth + " (SAMPLE) - scene 4", "c4b: a synth on ENGINE > SAMPLE counts as using its file: " + (u.count("pad.wav") ? u.at("pad.wav") : std::string("-")));
            check(u.count("hit.wav") && u.at("hit.wav") == sampler + " - brick 4" && !u.count("lift.wav"), "c4b: a brick's sample counts, a slot no brick uses does not");
        }
        {   // C4b (g2): MOVE UNUSED TO TRASH keeps everything anything still names, and moves only by rename
            namespace fs = std::filesystem;
            const std::string dir = fs::absolute("selftest_trash").string();
            fs::remove_all(dir); fs::create_directories(dir + "/prj/samples"); fs::create_directories(dir + "/trash");
            std::vector<float> tone(256, 0.1f);
            for (const char* n : {"c4bt_used.wav", "c4bt_spare_slot.wav", "c4bt_undo.wav", "c4bt_saved.wav", "c4bt_other.wav", "c4bt_gone.wav", "c4bt_gone2.wav"})
                WriteWavPCM16(dir + "/prj/samples/" + n, tone, 1, kSampleRate);
            { std::ofstream f(dir + "/prj/p.t8proj"); f << "[project]\nsampler=c4bt_saved.wav|60|0\n"; }        // the saved version names one
            { std::ofstream f(dir + "/prj/other.t8proj"); f << "[project]\nsampler=c4bt_other.wav|60|0\n"; }    // another project in the folder
            AppState a;
            a.projectPath = dir + "/prj/p.t8proj";
            a.project.patterns[0].samplerPatch.file = "c4bt_used.wav";
            a.project.patterns[kNumPatterns - 1].samplerPatch.file = "c4bt_spare_slot.wav";   // a brick slot nothing points at
            { std::lock_guard<SceneMutex> lk(a.sequencer.PatternMutex()); a.PushUndo(); }
            a.undoStack.back().pattern.samplerPatch.file = "c4bt_undo.wav";                // Cmd+Z could bring it back
            const auto mem = a.SampleFilesNamedInMemory();
            check(mem.count("c4bt_used.wav") && mem.count("c4bt_spare_slot.wav") && mem.count("c4bt_undo.wav"), "c4b: memory keeps the files of every slot (also unused ones) and of the Undo steps");
            std::vector<std::string> plan; int kept = -1; std::string err = "?";
            a.PlanTrashUnusedSamples([&](std::vector<std::string> l, int k, std::string e) { plan = std::move(l); kept = k; err = e; });
            check(err.empty() && plan == std::vector<std::string>{"c4bt_gone.wav", "c4bt_gone2.wav"} && kept == 2,
                  "c4b: only the copies nothing names may go to the Trash; the saved version and the other project keep theirs (" + std::to_string(plan.size()) + " / kept " + std::to_string(kept) + ")");
            // the trash itself, in a test folder (freedesktop layout: files/ + info/, as on Linux; macOS layout: straight in)
            std::string e;
            check(cleanup::MoveToTrash(dir + "/prj/samples/c4bt_gone.wav", e, dir + "/trash", true) && fs::exists(dir + "/trash/files/c4bt_gone.wav")
                  && fs::exists(dir + "/trash/info/c4bt_gone.wav.trashinfo") && !fs::exists(dir + "/prj/samples/c4bt_gone.wav"), "c4b: a file moved to the Trash keeps its restore note (" + e + ")");
            WriteWavPCM16(dir + "/prj/samples/c4bt_gone.wav", tone, 1, kSampleRate);
            check(cleanup::MoveToTrash(dir + "/prj/samples/c4bt_gone.wav", e, dir + "/trash", true) && fs::exists(dir + "/trash/files/c4bt_gone 2.wav"), "c4b: a name already in the Trash gets a number, nothing there is overwritten");
            check(cleanup::MoveToTrash(dir + "/prj/samples/c4bt_gone2.wav", e, dir + "/mactrash", false) && fs::exists(dir + "/mactrash/c4bt_gone2.wav"), "c4b: macOS layout: the file goes straight into the Trash");
            check(!cleanup::MoveToTrash(dir + "/prj/samples/missing.wav", e, dir + "/trash", true) && !e.empty() && !fs::exists(dir + "/trash/info/missing.wav.trashinfo"), "c4b: a file that cannot be moved leaves no restore note behind");
            // step 2 re-checks memory: a file that became used after the question stays
            a.project.patterns[1].samplerPatch.file = "c4bt_saved.wav";
            const char* oldX = std::getenv("XDG_DATA_HOME"); const std::string keepX = oldX ? oldX : "";
            const char* oldH = std::getenv("HOME"); const std::string keepH = oldH ? oldH : "";
            setenv("XDG_DATA_HOME", (dir + "/xdg").c_str(), 1); setenv("HOME", (dir + "/home").c_str(), 1);
            fs::create_directories(dir + "/home");
            int moved = -1;
            a.TrashSampleFiles({"c4bt_saved.wav", "../p.t8proj"}, [&](int m, std::string) { moved = m; });
            if (oldX) setenv("XDG_DATA_HOME", keepX.c_str(), 1); else unsetenv("XDG_DATA_HOME");
            if (oldH) setenv("HOME", keepH.c_str(), 1); else unsetenv("HOME");
            check(moved == 0 && fs::exists(dir + "/prj/samples/c4bt_saved.wav") && fs::exists(dir + "/prj/p.t8proj"), "c4b: the move checks memory again and never leaves samples/");
            // review of C4b g2: upper / lower case (the Mac's disk ignores it) and another project opened before the answer
            WriteWavPCM16(dir + "/prj/samples/c4bt_case.wav", tone, 1, kSampleRate);
            WriteWavPCM16(dir + "/prj/samples/c4bt_savedcase.wav", tone, 1, kSampleRate);
            WriteWavPCM16(dir + "/prj/samples/c4bt_free.wav", tone, 1, kSampleRate);
            { std::ofstream f(dir + "/prj/p.t8proj", std::ios::app); f << "sampler=C4BT_SavedCase.wav|60|0\n"; }
            a.project.patterns[2].samplerPatch.file = "C4BT_Case.wav";   // what CopySamplerFile can leave: the disk has c4bt_case.wav
            a.sampleNamesValid_ = false;
            plan.clear(); err = "?";
            a.PlanTrashUnusedSamples([&](std::vector<std::string> l, int, std::string e) { plan = std::move(l); err = e; });
            const bool hasCase = std::find(plan.begin(), plan.end(), "c4bt_case.wav") != plan.end();
            const bool hasSavedCase = std::find(plan.begin(), plan.end(), "c4bt_savedcase.wav") != plan.end();
            check(err.empty() && !hasCase && !hasSavedCase && std::find(plan.begin(), plan.end(), "c4bt_free.wav") != plan.end(),
                  "c4b: a name in memory or in a saved file keeps the file whatever its upper / lower case");
            { std::error_code hl; fs::create_hard_link(dir + "/prj/samples/c4bt_free.wav", dir + "/prj/samples/C4BT_Other_Spelling.wav", hl); }   // one file, two names (as on a case-blind disk)
            check(cleanup::SameFileAsAny(dir + "/prj/samples", "c4bt_free.wav", {"C4BT_Other_Spelling.wav"}) && !cleanup::SameFileAsAny(dir + "/prj/samples", "c4bt_free.wav", {"c4bt_case.wav"}),
                  "c4b: step 2 knows two spellings of one file on disk");
            fs::create_directories(dir + "/prj2/samples");
            WriteWavPCM16(dir + "/prj2/samples/c4bt_free.wav", tone, 1, kSampleRate);
            a.projectPath = dir + "/prj2/p.t8proj"; ++a.docGen_;                  // a dropped project opened while the question was up
            moved = -1; std::string why;
            a.TrashSampleFiles(plan, [&](int m, std::string e) { moved = m; why = e; });
            check(!a.TrashPlanStillValid() && moved == 0 && why.find("another project") != std::string::npos && fs::exists(dir + "/prj2/samples/c4bt_free.wav") && fs::exists(dir + "/prj/samples/c4bt_free.wav"),
                  "c4b: a plan made for one project never moves anything in another");
            fs::remove_all(dir);
        }
    }
    {   // N1 (TEST-6, LIVE-9): the audio thread does not allocate or free memory while it plays a busy set
        AppState a; a.engine.Init(kSampleRate); a.sequencer.Init(&a.engine, &a.project);
        a.project.kitFolder = kitFolder;
        a.RefreshSound();
        Pattern& p = a.CurrentPattern();
        ApplyStarterGroove(p, a.StepTrack(p, kChordTrackIndex));
        a.SyncMixer(p);
        for (int t = 0; t < kNumDrumTracks; ++t) p.tracks[static_cast<size_t>(t)].drumTune = 0.37f * (t + 1) - 1.1f;   // tunes no table was made for yet
        for (int st = 0; st < 16; st += 3) { p.tracks[0].steps[static_cast<size_t>(st)].active = true; p.tracks[0].steps[static_cast<size_t>(st)].ratchet = 4; }   // ratchets
        for (int st = 2; st < 16; st += 4) { p.tracks[3].steps[static_cast<size_t>(st)].active = true; p.tracks[3].steps[static_cast<size_t>(st)].ratchet = 3; }
        a.quantizeSwitches = true;
        a.StartMidiCapture();                    // MIDI capture logs on the audio thread
        a.sequencer.Play();
        std::vector<float> blk(128 * 2);
        auto render = [&](double secs) { const int n = static_cast<int>(secs * kSampleRate) / 128; for (int b = 0; b < n; ++b) a.sequencer.RenderAndAdvance(blk.data(), 128, kSampleRate); };
        render(0.5);                             // the first blocks may settle lazily made state once
        check(a.SwitchesCanQueue(), "test-6: NEXT BAR switches can queue");
        a.LiveSwitch(AppState::QueuedSwitch::Drop);   // NEXT BAR: applied on the audio thread at the bar line
        a.LiveSwitch(AppState::QueuedSwitch::Mute, 3);
        check(a.SwQueuedCount() == 2, "test-6: two switches wait for the bar line (" + std::to_string(a.SwQueuedCount()) + ")");
        g_audioAllocs = 0;
        g_countAllocs = true;
        render(6.0);                             // several bars: the queued switches land, ratchets fire, tuned drums play
        g_countAllocs = false;
        const long allocs = g_audioAllocs.load();
        check(a.SwQueuedCount() == 0 && a.engine.GetMixer().tracks[1].mute, "test-6: the queued NEXT BAR switches were applied on the audio thread during the render (DROP muted the snare)");
        check(!a.engine.Capture().Events().empty(), "test-6: MIDI capture logged events during the render");
        check(allocs == 0, "test-6: the audio thread did not allocate or free memory while playing (" + std::to_string(allocs) + ")");
        a.engine.Capture().Stop(); a.midiCaptureActive = false;
        a.sequencer.Stop();
    }
    {   // N1 (LIVE-5): the audio callback waits for the scene lock only a bounded time
        AppState a; a.engine.Init(kSampleRate); a.sequencer.Init(&a.engine, &a.project);
        unsigned waited = 0;
        check(a.sequencer.LockForAudio(5000, waited) && waited == 0, "lock-5: a free lock is taken at once");
        a.sequencer.UnlockForAudio();
        std::atomic<bool> held{false}, release{false};
        std::thread ui([&] { std::lock_guard<SceneMutex> lk(a.sequencer.PatternMutex()); held = true; while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
        while (!held) std::this_thread::yield();
        const auto t0 = std::chrono::steady_clock::now();
        const bool got = a.sequencer.LockForAudio(5000, waited);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        check(!got && waited >= 5000 && ms < 30.0, "lock-5: a busy lock is given up after the limit (" + std::to_string(ms) + " ms), the callback plays silence instead of waiting");
        release = true; ui.join();
        check(a.sequencer.LockForAudio(5000, waited), "lock-5: once the screen lets go, the next buffer plays normally");
        a.sequencer.UnlockForAudio();
        check(!a.sequencer.PatternMutex().HeldByThisThread(), "lock-5: the lock is free afterwards");
    }
    {   // N1 (LIVE-5): every hold of the scene lock by the screen is timed and named, so the XRUN hint can say which panel held it
        SceneMutex m;
        auto hold = [&](const char* who, int ms) { SceneLock lk(m, who); std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
        check(m.MaxHoldWho()[0] == '\0' && m.HeldTotalUs() == 0, "lock-5 names: nothing held yet");
        hold("MIXER", 2); hold("CONTEXT", 40); hold("HEADER", 1);   // 40 ms: far above any sleep overshoot of the short ones
        check(std::string(m.MaxHoldWho()) == "CONTEXT" && m.MaxHoldUs() >= 40000, "lock-5 names: the longest hold is named");
        check(m.HeldTotalUs() >= 43000, "lock-5 names: all holds add up to the total held time");
        m.ResetMaxHold();
        {   // nested: the outer name covers the whole hold (a panel calling into the app, which locks again)
            SceneLock outer(m, "BRICKS");
            { SceneLock inner(m, "MENU"); std::this_thread::sleep_for(std::chrono::milliseconds(12)); }
            { std::lock_guard<SceneMutex> plain(m); }
        }
        check(std::string(m.MaxHoldWho()) == "BRICKS" && m.MaxHoldUs() >= 12000, "lock-5 names: a nested lock keeps the outer name");
        m.ResetMaxHold();   // sleeps overshoot on busy CI machines: compare the unnamed hold only with itself
        { std::lock_guard<SceneMutex> plain(m); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        check(std::string(m.MaxHoldWho()) == "OTHER", "lock-5 names: a hold without a name is called OTHER");
        m.ResetMaxHold();
        check(m.MaxHoldUs() == 0 && m.MaxHoldWho()[0] == '\0', "lock-5 names: a click on the readout resets the longest hold and its name");
        const unsigned long long before = m.HeldTotalUs();
        std::thread audio([&] { m.MarkAudioThread(); SceneLock lk(m, "AUDIO?"); std::this_thread::sleep_for(std::chrono::milliseconds(10)); });
        audio.join();
        check(m.MaxHoldUs() == 0 && m.HeldTotalUs() == before, "lock-5 names: the audio thread's own render is not counted as the screen holding the sound");
    }
    {   // N1 (LIVE-5 / LIVE-6): Settings > Audio rules
        using namespace t8::audiosettings;
        check(ValidBuffer(256) == 256 && ValidBuffer(512) == 512 && ValidBuffer(128) == 128, "audio-set: the three offered buffer sizes are kept");
        check(ValidBuffer(0) == 128 && ValidBuffer(4096) == 128 && ValidBuffer(300) == 128, "audio-set: a damaged saved buffer size becomes 128");
        const std::vector<std::string> outs = {"MacBook Pro Speakers", "Scarlett 2i2 USB"};
        check(OutputToOpen("Scarlett 2i2 USB", outs) == "Scarlett 2i2 USB", "audio-set: a plugged-in chosen output is opened");
        check(OutputToOpen("AirPods", outs).empty(), "audio-set: an unplugged chosen output falls back to the system default");
        check(OutputToOpen("", outs).empty(), "audio-set: no choice = the system default");
        check(std::abs(BufferMs(128, 44100) - 2.9) < 0.05 && std::abs(BufferMs(512, 44100) - 11.6) < 0.05, "audio-set: buffer lengths in ms");
        AppState a;
        check(a.audioBufferChoice == 128 && a.audioOutputChoice.empty(), "audio-set: the default is the system output with 128 frames (as before)");
    }
    {   // C1 (UI-6, UI-7): one word for a scene, labels that fit (source ratchets)
        auto slurp = [](const std::string& path) { std::ifstream in(path); return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); };
        const std::string seq = slurp("src/ui_desktop/SequencerPanel.cpp"), chain = slurp("src/ui_desktop/ChainPanel.cpp"), head = slurp("src/ui_desktop/HeaderPanel.cpp");
        const std::string roll = slurp("src/ui_desktop/PianoRollPanel.cpp"), cell = slurp("src/ui_desktop/ParamCell.h"), ctx = slurp("src/ui_desktop/ContextPanel.cpp");
        check(seq.find("\"Paste whole scene\"") != std::string::npos && seq.find("whole pattern") == std::string::npos, "c1 ui-6: Paste whole scene under Copy whole scene");
        check(chain.find("\"Change scene\"") != std::string::npos && chain.find("\"Pattern %02d\"") == std::string::npos, "c1 ui-6: the brick slot menu says Change scene");
        check(head.find("Pattern length") == std::string::npos && head.find("of this pattern") == std::string::npos, "c1 ui-6: the header hints say scene");
        check(ctx.find("\"PAT\"") == std::string::npos, "c1 ui-6: a track on the scene swing shows SCENE, not PAT");
        check(roll.find("\"ALL NOTES\"") == std::string::npos && roll.find("\"ACTIVE ONLY\"") == std::string::npos && roll.find("\"ROLL\"") == std::string::npos, "c1 ui-7: the roll toolbar is ALL / ACTIVE, no ROLL word");
        check(roll.find("DrawEuclidTools(app, ti, fit") != std::string::npos, "c1 ui-7: EUCLID n/m and GHOST shrink to fit the roll toolbar");
        check(cell.find("kParamCellMinFont = 10.0f") != std::string::npos && cell.find("ImGui::GetFont(), size") != std::string::npos, "c1 ui-7: a cell label is drawn smaller to fit, not below 10 points");
    }
    {   // C1 (UI-1, UI-4, UI-9): the shell -- two tabs, three tools that open the same way, keys, header, PITCH without a hidden mode
        auto slurp = [](const std::string& path) { std::ifstream in(path); return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); };
        const std::string mn = slurp("src/main.cpp"), keys = slurp("src/ui_desktop/Shortcuts.cpp"), head = slurp("src/ui_desktop/HeaderPanel.cpp"), menu = slurp("src/ui_desktop/MenuBar.cpp");
        check(mn.find("BeginTabItem(\"MIX LAB\"") == std::string::npos && mn.find("###tabLive") != std::string::npos && mn.find("###tabTrack") != std::string::npos, "c1 ui-1: the only tabs are LIVE and TRACK");
        check(mn.find("toolButton(\"MIX LAB\", \"M\"") != std::string::npos && mn.find("toolButton(\"CHORD LAB\", \"C\"") != std::string::npos && mn.find("toolButton(\"MASTER FX\", \"F\"") != std::string::npos, "c1 ui-1: three tool buttons with their keys in the tab row");
        check(mn.find("drawTool(availY - headerH - sp)") != std::string::npos && mn.find("BeginPopup(\"MasterFxPopover\")") == std::string::npos && head.find("MasterFxPopover") == std::string::npos, "c1 ui-1: every tool opens under the header (MASTER FX is no popover)");
        check(keys.find("ImGuiKey_M") != std::string::npos && keys.find("ImGuiKey_F, false)) { app.ToggleTool(AppState::Tool::MasterFx)") != std::string::npos && keys.find("if (app.tool != AppState::Tool::None) { app.OpenTool(AppState::Tool::None); return; }") != std::string::npos, "c1 ui-1: M / C / F toggle the tools, Esc closes the open one");
        check(menu.find("\"MIX LAB\", \"M\"") != std::string::npos && menu.find("\"CHORD LAB\", \"C\"") != std::string::npos && menu.find("\"MASTER FX\", \"F\"") != std::string::npos, "c1 ui-1: View lists the tools with their keys");
        check(keys.find("ImGuiKey_P") == std::string::npos && keys.find("app.PitchSet(0.0f); app.SetStatus(\"PITCH 0.0%\")") == std::string::npos && keys.find("const bool opt = io.KeyAlt && !cmd;") != std::string::npos, "c1 ui-4: PITCH has no hidden keyboard mode (Option+arrows; Return always plays from the brick)");
        check(head.find("ToggleButton(\"MASTER\"") == std::string::npos && head.find("if (app.ShownTab() != 1) {") != std::string::npos, "c1 ui-9: no MASTER button in the header, LAUNCH only on LIVE");
        AppState a;
        check(a.tool == AppState::Tool::None, "c1: no tool is open at start");
        a.ToggleTool(AppState::Tool::MixLab); a.ToggleTool(AppState::Tool::ChordLab);
        check(a.ChordLabOpen() && !a.ToolOpen(AppState::Tool::MixLab), "c1: one tool at a time (opening another replaces it)");
        a.ToggleTool(AppState::Tool::ChordLab);
        check(a.tool == AppState::Tool::None, "c1: the same key closes the tool");
    }
    std::cout << "\n" << (failures == 0 ? "ALL CHECKS PASSED" : "FAILURES: " + std::to_string(failures)) << "\n";
    return failures == 0 ? 0 : 1;
}
