#pragma once
// v44.41 MIX LAB runtime: renders the played scene offline on a private engine (so the music never stutters), taps every track
// after its fader and pan (Mixer::tapMono), and hands the stems to the analysis in core/MixAnalysis.h.
// The tap sits before the delay / reverb sends and the master chain: the picture shows the instruments themselves.
#include "AppState.h"
#include "core/MixAnalysis.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace t8 {

struct MixLabJob {
    Project project;
    int scene = 0;
    std::string projectPath;   // v44.50: where samples/ is, so the sampler is heard in the analysis too
    std::array<bool, kNumTracks> mute{}, solo{};   // the live mute / solo state, so a muted instrument is not counted as a competitor
};

// Blocking. Safe to call from any thread: it builds its own AppState (engine + sequencer + kits).
inline void RenderMixStems(const MixLabJob& job, MixSpectra& out) {
    auto ds = std::make_unique<AppState>();
    ds->project = job.project;
    ds->project.currentPattern = std::clamp(job.scene, 0, kNumPatterns - 1);
    ds->projectPath = job.projectPath;
    ds->engine.Init(kSampleRate);
    ds->sequencer.Init(&ds->engine, &ds->project);
    ds->RefreshSound();
    const Pattern& pat = ds->project.patterns[static_cast<size_t>(ds->project.currentPattern)];
    Mixer& mx = ds->engine.GetMixer();
    mx.SyncFromPattern(pat);
    for (int t = 0; t < kNumTracks; ++t) { mx.tracks[static_cast<size_t>(t)].mute = job.mute[static_cast<size_t>(t)]; mx.tracks[static_cast<size_t>(t)].solo = job.solo[static_cast<size_t>(t)]; }
    for (int t = 0; t < kNumTracks; ++t) mx.tracks[static_cast<size_t>(t)].smoothedMuteGain = mx.ShouldPlay(t) ? 1.0f : 0.0f;   // a muted instrument is silent from the first sample (no fade-out in the picture)
    const double stepSec = 60.0 / (std::max(30.0f, pat.bpm) * std::max(1, pat.stepsPerBeat));
    const double loopSec = std::max(1, pat.masterLength) * stepSec;
    const double seconds = std::clamp(loopSec * 2.0, 3.0, 9.0);
    const int total = static_cast<int>(seconds * kSampleRate);
    std::array<std::vector<float>, kNumTracks> stems;
    for (int t = 0; t < kNumTracks; ++t) { stems[static_cast<size_t>(t)].assign(static_cast<size_t>(total) + 1024, 0.0f); mx.tapMono[static_cast<size_t>(t)] = stems[static_cast<size_t>(t)].data(); }
    mx.tapPos = 0; mx.tapCap = static_cast<size_t>(total);
    ds->sequencer.Play();
    std::vector<float> ob(256 * 2);
    for (int done = 0; done < total; done += 256) { std::fill(ob.begin(), ob.end(), 0.0f); ds->sequencer.RenderAndAdvance(ob.data(), 256, kSampleRate); }
    ds->sequencer.Stop();
    mx.tapCap = 0;
    for (auto& p : mx.tapMono) p = nullptr;
    for (auto& s : stems) s.resize(static_cast<size_t>(total));
    AnalyseStems(stems, kSampleRate, out);
}

// What the MIX LAB tab keeps: the last analysis, the priority list, the fixes being tried (A / B).
class MixLab {
public:
    enum class Phase { Idle, Running, Ready };

    // The fields a fix can touch, so UNDO ALL / A-B put back exactly these and nothing else of the track.
    struct Snap {
        float volume = 0.8f, pan = 0.0f, hpf = 20.0f, slope = 0.0f, lowDb = 0.0f, lowHz = 120.0f, midDb = 0.0f, midHz = 1000.0f, midQ = 1.0f, highDb = 0.0f, highHz = 6000.0f;
        static Snap Of(const Track& t) { Snap s; s.volume = t.volume; s.pan = t.pan; s.hpf = t.eqHpfHz; s.slope = t.eqHpfSlope; s.lowDb = t.eqLowDb; s.lowHz = t.eqLowHz; s.midDb = t.eqMidDb; s.midHz = t.eqMidHz; s.midQ = t.eqMidQ; s.highDb = t.eqHighDb; s.highHz = t.eqHighHz; return s; }
        void To(Track& t) const { t.volume = volume; t.pan = pan; t.eqHpfHz = hpf; t.eqHpfSlope = slope; t.eqLowDb = lowDb; t.eqLowHz = lowHz; t.eqMidDb = midDb; t.eqMidHz = midHz; t.eqMidQ = midQ; t.eqHighDb = highDb; t.eqHighHz = highHz; }
    };
    struct Tried { int track; Snap before, after; int spot; };

    ~MixLab() { Join(); }

    Phase phase() const { return phase_.load(); }
    int Generation() const { return gen_.load(); }   // +1 whenever a finished analysis replaced the result
    bool Busy() const { return phase_.load() == Phase::Running; }

    // Starts a background analysis (ignored while one runs). `mix` = the live mixer's mute / solo.
    void Start(AppState& app) {
        if (Busy()) return;
        Join();
        auto job = std::make_shared<MixLabJob>();
        {
            std::lock_guard<SceneMutex> lock(app.sequencer.PatternMutex());
            job->project = app.project;
        }
        job->projectPath = app.projectPath;
        job->scene = app.sequencer.IsPlaying() ? app.sequencer.PlayPatternIndex() : app.project.currentPattern;
        for (int t = 0; t < kNumTracks; ++t) { job->mute[static_cast<size_t>(t)] = app.engine.GetMixer().tracks[static_cast<size_t>(t)].mute; job->solo[static_cast<size_t>(t)] = app.engine.GetMixer().tracks[static_cast<size_t>(t)].solo; }
        scene_ = job->scene;
        const std::array<int, kNumTracks> prio = priority_;
        phase_.store(Phase::Running);
        worker_ = std::thread([this, job, prio]() {
            auto sp = std::make_unique<MixSpectra>();
            RenderMixStems(*job, *sp);
            const MixReport rep = BuildMixReport(*sp, prio, &job->project.patterns[static_cast<size_t>(job->scene)]);
            { std::lock_guard<std::mutex> lk(mtx_); spectra_ = *sp; report_ = rep; }
            gen_.fetch_add(1);
            phase_.store(Phase::Ready);
        });
    }
    // Re-reads the result of a finished analysis (copy under the lock).
    bool Get(MixSpectra& sp, MixReport& rep) const {
        if (gen_.load() == 0) return false;
        std::lock_guard<std::mutex> lk(mtx_); sp = spectra_; rep = report_; return true;
    }
    void Join() { if (worker_.joinable()) worker_.join(); }

    std::array<int, kNumTracks>& Priority() { return priority_; }
    int Scene() const { return scene_; }
    std::vector<Tried>& TriedFixes() { return tried_; }
    const std::vector<Tried>& TriedFixes() const { return tried_; }
    bool showB = false;     // A / B: false = the mix as it was, true = with the tried fixes
    int selectedSpot = -1;
    int selectedZone = -1;
    bool soloContestants = false;   // SHOW IT: only the instruments of the selected spot sound
    bool hasBaseline = false;       // `baseSpectra` / `baseReport` = the analysis before any fix
    MixSpectra baseSpectra; MixReport baseReport;

private:
    std::atomic<Phase> phase_{Phase::Idle};
    std::atomic<int> gen_{0};
    mutable std::mutex mtx_;
    std::thread worker_;
    MixSpectra spectra_; MixReport report_;
    std::array<int, kNumTracks> priority_ = DefaultMixPriority();
    int scene_ = 0;
    std::vector<Tried> tried_;
};

}  // namespace t8
