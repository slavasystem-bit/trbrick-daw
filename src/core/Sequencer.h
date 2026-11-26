#pragma once
// Transport + step clock. Owns no audio DSP itself -- it drives AudioEngine
// by rendering in sub-chunks between step boundaries so triggers land on
// the exact sample, not just "somewhere in this audio callback".
//
// All 10 tracks share one step clock; each track's own step pointer wraps at
// its own length (useOwnLength/length), giving Elektron-style polymeter for
// free -- e.g. a 3-step perc pattern against a 16-step kick stays in a
// rotating relationship instead of just looping in lockstep, which is a
// nice trick to have live.
#include "Types.h"
#include "Project.h"
#include "AudioEngine.h"
#include "DrumBreaks.h"
#include <array>
#include <cstring>
#include <mutex>
#include <atomic>
#include <functional>
#include <vector>
#include <random>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cstdio>

namespace t8 {

// v44.44 (audit С1): the scene lock as a type that knows its owner. Everything that changes a scene while the audio thread may read it must hold
// it; before this the rule lived only in a convention (every panel locks for its whole Draw). Operations that change a scene call
// CheckSceneLock(), which counts a violation (and says so once on stderr) instead of letting a data race go unnoticed.
class SceneMutex {
public:
    void lock() { m_.lock(); Acquired(); }
    bool try_lock() { if (!m_.try_lock()) return false; Acquired(); return true; }
    void unlock() {
        if (--depth_ == 0) {
            // N1: how long a thread other than the audio thread kept the lock (the audio thread can wait that long).
            if (owner_.load(std::memory_order_relaxed) != audioThread_.load(std::memory_order_relaxed)) {
                const auto held = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - since_).count();
                const unsigned us = held < 0 ? 0u : static_cast<unsigned>(std::min<long long>(held, 0xFFFFFFFFll));
                unsigned prev = maxHoldUs_.load(std::memory_order_relaxed);
                while (us > prev && !maxHoldUs_.compare_exchange_weak(prev, us, std::memory_order_relaxed)) {}
            }
            owner_.store(std::thread::id{}, std::memory_order_relaxed);
        }
        m_.unlock();
    }
    bool HeldByThisThread() const { return owner_.load(std::memory_order_relaxed) == std::this_thread::get_id(); }
    // N1: the thread that renders audio says so once (RenderAndAdvance); its own holds are the render itself and are not counted.
    void MarkAudioThread() { audioThread_.store(std::this_thread::get_id(), std::memory_order_relaxed); }
    // Longest time any other thread (the screen, a loader) held the lock since the last reset, in microseconds.
    unsigned MaxHoldUs() const { return maxHoldUs_.load(std::memory_order_relaxed); }
    void ResetMaxHold() { maxHoldUs_.store(0, std::memory_order_relaxed); }
private:
    void Acquired() {
        owner_.store(std::this_thread::get_id(), std::memory_order_relaxed);
        if (++depth_ == 1) since_ = std::chrono::steady_clock::now();
    }
    std::recursive_mutex m_;
    std::atomic<std::thread::id> owner_{};
    std::atomic<std::thread::id> audioThread_{};
    std::atomic<unsigned> maxHoldUs_{0};
    int depth_ = 0;                                  // only touched by the owner
    std::chrono::steady_clock::time_point since_{};  // only touched by the owner
};


// The sample variant a drum track is "set to": the one most of its active steps use (all steps when none is active).
// Live breaks / fills have no steps of their own, so they play this variant instead of always the first sample.
int TrackSampleVariant(const Track& t);

class Sequencer {
public:
    void Init(AudioEngine* engine, Project* project);

    void Play();
    void Stop();
    // Releases every sounding synth / chord note and forgets their scheduled note-offs
    // (playback itself keeps running). Used when the project underneath changes.
    void Panic();
    void TogglePlay();
    bool IsPlaying() const { return playing_.load(); }

    // Renders `numFrames` of audio into `out` (interleaved stereo),
    // triggering steps at the correct sample offsets along the way.
    void RenderAndAdvance(float* out, int numFrames, int sampleRate);

    // N1 (audit v44.58 LIVE-5): the audio callback takes the scene lock itself, waiting at most `maxWaitUs`. True = locked (call
    // UnlockForAudio after rendering; RenderAndAdvance re-locks for free, the lock is recursive). False = the screen (or a loader)
    // still holds it: the callback plays a short fade to silence instead of waiting longer, so the device keeps getting buffers,
    // the watchdog does not restart it, and the stall ends as soon as the lock is free. `waitedUs` = time spent waiting.
    bool LockForAudio(unsigned maxWaitUs, unsigned& waitedUs);
    void UnlockForAudio() { patternMutex_.unlock(); }

    // ---- Live break (see DrumBreaks.h / AppState::UpdateLiveBreakHold) --
    // Launch-quantized, Session-View style: SetLiveBreak only ARMS the
    // break -- it does NOT cut into the bar that's currently playing.
    // FireTick holds it pending until the next bar boundary (tick % 16
    // == 0), and only then substitutes AllDrumBreaks()[breakIndex]'s hits
    // for the 6 DRUM slots' real pattern steps, for EXACTLY one full bar
    // (16 ticks). After that bar it automatically clears itself and
    // reverts to the real pattern on its own -- independent of whether
    // the button that triggered it is still held, and it always plays
    // start to finish once it begins (no early cutoff except an explicit
    // ClearLiveBreak while still pending/mid-bar). Synth tracks are
    // untouched throughout, and the underlying pattern step data is never
    // modified, so once it reverts it's exactly what was already
    // programmed, nothing to undo. Plain atomics rather than the full
    // PatternMutex: this is set from the UI thread on trigger and read
    // every tick from the audio thread, and arming/starting/stopping
    // exactly one tick early or late is inaudible -- not worth adding
    // lock contention to the audio callback.
    void SetLiveBreak(int breakIndex) {
        liveBreakIndex_.store(breakIndex);
        liveBreakPending_.store(true);
        // NOTE: liveBreakActive_ is deliberately NOT set here -- FireTick
        // promotes pending -> active only once it sees a bar boundary.
    }
    // Cancels a pending (not yet started) or in-progress break immediately
    // -- used for the RANDOM tab's B=CANCEL on the LIVE BREAK row. NOT
    // called by the normal one-shot live trigger, which is meant to run
    // to completion on its own once it has actually started.
    void ClearLiveBreak() {
        liveBreakPending_.store(false);
        liveBreakActive_.store(false);
    }
    bool IsLiveBreakActive() const { return liveBreakActive_.load(); }
    bool IsLiveBreakPending() const { return liveBreakPending_.load(); }

    // ---- Riser bar-sync (sixth-revision follow-up) -----------------------
    // Same launch-quantization pattern as SetLiveBreak/liveBreakPending_
    // above: SetPendingRiser only ARMS it, it does NOT fire immediately.
    // FireTick's bar-boundary check (tick % 16 == 0, the same "one bar"
    // convention the live break uses) is what actually calls
    // RiserPlayer::Trigger -- so a riser dropped mid-bar always lands right
    // on the next downbeat alongside the drum pattern, instead of wherever
    // the pad happened to get hit. Unlike a live break there's no ongoing
    // "active for exactly 16 ticks" window to track here: once fired, the
    // riser is a self-contained one-shot that plays for however long its
    // own sample (plus RiserPlayer's release tail) lasts, with no further
    // involvement from the sequencer.
    //
    // v26: the riser is BACK-TIMED. `bars` = which bar line is the drop (1 = the next one that is at
    // least ~0.5 s away, 2 / 4 / 8 = that many bar lines ahead). The sequencer starts the sample
    // late enough (sample-accurately, via RiserPlayer::TriggerAligned) that its loudest moment lands
    // exactly on the drop; if there is less time than the riser is long it is sped up (<= 20 %) or
    // entered part-way through instead of ending after the beat.
    void SetPendingRiser(int riserIndex, int bars = 1, float gain = 1.0f) {   // v44.13: gain = the level of a chain transition
        pendingRiserGain_.store(gain);
        pendingRiserIndex_.store(riserIndex);
        pendingRiserBars_.store(std::max(1, bars));
        riserTicksLeft_.store(-1);
        pendingRiserArmed_.store(true);
    }
    bool IsRiserPending() const { return pendingRiserArmed_.load(); }
    // Ticks (16th notes) until the drop while a riser is pending, -1 if not yet computed / nothing pending.
    int RiserTicksToDrop() const { return pendingRiserArmed_.load() ? riserTicksLeft_.load() : -1; }
    // Cancels a riser that's armed but hasn't actually started yet -- the
    // PERFORM tab's RISER row B press (see RandomView.cpp), symmetrical
    // with ClearLiveBreak above. A no-op if nothing is currently pending.
    void ClearPendingRiser() { pendingRiserArmed_.store(false); riserTicksLeft_.store(-1); }

    // ---- Song/chain mode -------------------------------------------------
    // Auto-advances through Project::songChain (see ChainStep in Types.h)
    // at each pattern-loop boundary -- i.e. whenever globalTick_ wraps past
    // the CURRENTLY PLAYING pattern's own masterLength -- instead of only
    // switching scenes manually (which remains available and
    // unaffected; it just won't be where playback ends up on the NEXT loop
    // boundary once song mode takes over again). Ephemeral transport state,
    // like playing_/globalTick_ above -- NOT persisted; only the chain
    // DEFINITION (Project::songChain/songChainLength) is saved, so song
    // mode always starts back at OFF on load/relaunch.
    //
    // Turning it ON takes effect immediately (same "live, not quantized"
    // convention as manual scene switching) -- jumps straight to
    // chain step 0 and resets the tick counter to 0 for a clean start,
    // rather than waiting for the current bar to finish.
    // v44.10: `startPos` is the chain step to start from (0 = the first).
    void SetSongModeActive(bool active, int startPos = 0) {
        std::lock_guard<SceneMutex> lock(patternMutex_);
        songModeActive_ = active;
        pinnedPlay_.store(-1);
        pendingScene_.store(-1);
        if (active && project_ && project_->songChainLength > 0) {
            songChainPos_ = std::clamp(startPos, 0, project_->songChainLength - 1);
            const ChainStep& step = project_->songChain[static_cast<size_t>(songChainPos_)];
            songChainRepeatsLeft_ = std::max(1, step.repeats);
            // The chain drives PLAYBACK only (songPlayPattern_); the pattern
            // being EDITED (project_->currentPattern) is left alone so a
            // live performer can keep working on any scene while the chain
            // runs.
            songPlayPattern_ = std::clamp(step.patternIndex, 0, kNumPatterns - 1);
            if (songFollowsEdit_) project_->currentPattern = songPlayPattern_;
            engine_->GetMixer().SyncFromPattern(project_->patterns[songPlayPattern_]);
            ApplySceneSound(songPlayPattern_);
            if (project_->chordBankOn && step.chordScene >= 0 && step.chordScene < kNumChordScenes) {
                pendingChordScene_.store(-1);
                ApplyChordScene(step.chordScene, false);
            }
            globalTick_ = 0;
            samplesIntoStep_ = 0.0;
        } else if (!active && project_) {
            engine_->GetMixer().SyncFromPattern(CurrentPattern());
            ApplySceneSound(project_->currentPattern);
        }
        riserArmedFor_ = -1;
        ApplyStepOverlay();   // v44.5: the mute mask of chain step 0 (or none when song mode goes off)
    }
    // ---- Player (v44.10) ---------------------------------------------------------------------------------------------------
    // PlayChainFrom: starts (or RESTARTS, also in the middle of playing) the chain at step `pos`. `loopBrick` keeps repeating that
    // one step until it is switched off. With no chain it is a plain Play().
    void PlayChainFrom(int pos, bool loopBrick);
    // true: the playing step repeats instead of moving on (its own repeats still count for the length of one pass).
    // v44.28: LOOP over a RANGE of steps lo..hi (a selection of bricks): after step hi the chain goes back to lo. lo < 0 = no range.
    void SetSongLoopRange(int lo, int hi) { songLoopHi_.store(hi); songLoopLo_.store(lo); }
    int SongLoopLo() const { return songLoopLo_.load(); }
    void SetSongLoopStep(bool on) { songLoopStep_.store(on); }
    bool SongLoopStep() const { return songLoopStep_.load(); }
    // true: the track plays once and stops after the last step; false (the old behaviour): it wraps to step 1.
    void SetSongRunOnce(bool on) { songRunOnce_.store(on); }
    bool SongRunOnce() const { return songRunOnce_.load(); }
    // ---- Sound of a scene (v44.5.4) ---------------------------------------------------------------
    // Every scene carries its own synth / chord patches, built-in kick and drum kit. When the played scene changes (launch, chain
    // step, song mode on / off) its sound is loaded into the engine: a patch only when it differs from the one the voice already
    // has (so a chain of scenes with the same sound never disturbs a sounding note), the kick settings with the mixer values,
    // and the kit as an already-built bank (AppState prepares those on the UI thread: SetSceneBanks), swapped with one atomic store.
    void ApplySceneSound(int p) {
        std::lock_guard<SceneMutex> lock(patternMutex_);
        if (!project_ || !engine_) return;
        p = std::clamp(p, 0, kNumPatterns - 1);
        const Pattern& pat = project_->patterns[static_cast<size_t>(p)];
        for (int i = 0; i < kNumSynthTracks; ++i)
            if (!SamePatch(engine_->Synth(i).Patch(), pat.synthPatches[static_cast<size_t>(i)])) engine_->Synth(i).SetPatch(pat.synthPatches[static_cast<size_t>(i)]);
        if (!SamePatch(engine_->Chord().Patch(), pat.chordPatch)) engine_->Chord().SetPatch(pat.chordPatch);
        if (samplesManaged_) {   // v44.50: the sample of the scene (prepared on the UI thread, like the kit banks)
            const SamplePtr& sp = sceneSamples_[static_cast<size_t>(p)];
            const SamplerPatch cur = engine_->SamplerCurrentPatch();
            const bool sampleChanged = sp != engine_->SamplerCurrentSample();
            if (sampleChanged || pat.samplerPatch != cur) {
                engine_->SamplerSetTempo(pat.bpm);
                engine_->SamplerSet(sp, pat.samplerPatch);
                // a LOOP starts again only when its sample or mode changes; a tweak of START / LENGTH / TUNE is followed live
                if (playing_ && pat.samplerPatch.mode == 2 && (sampleChanged || cur.mode != 2)) engine_->SamplerLoopRestart();
            }
        }
        if (samplesManaged_) {   // v44.53: the samples of the SAMPLE engines (synth slots 0..2, chord = 3), handed over the same way
            for (int k = 0; k <= kNumSynthTracks; ++k) {
                const SynthPatch& sp = k < kNumSynthTracks ? pat.synthPatches[static_cast<size_t>(k)] : pat.chordPatch;
                const SamplePtr want = sp.engineType == SynthEngineType::Sample ? sceneSynthSamples_[static_cast<size_t>(p)][static_cast<size_t>(k)] : SamplePtr();
                if (want != engine_->SynthSampleCurrent(k)) engine_->SynthSampleSet(k, want);
            }
        }
        engine_->GetMixer().kickSynth = pat.kick;
        if (kitsManaged_) {
            const DrumKit::BankPtr& b = sceneBanks_[static_cast<size_t>(p)] ? sceneBanks_[static_cast<size_t>(p)] : defaultBank_;
            if (b && b != engine_->GetKit()->CurrentBank()) engine_->GetKit()->Activate(b);
        }
    }
    // The kit banks per scene (null = the default bank). Called by AppState whenever a scene's kit changes; the banks must stay
    // referenced by the caller's cache too, so the audio thread never drops the last reference.
    void SetSceneBanks(const std::array<DrumKit::BankPtr, kNumPatterns>& banks, const DrumKit::BankPtr& def) {
        std::lock_guard<SceneMutex> lock(patternMutex_);
        sceneBanks_ = banks; defaultBank_ = def; kitsManaged_ = true;
        if (project_ && engine_) ApplySceneSound(PlayPatternIndex());
    }
    // v44.50: the sample per scene (null = none). Called by AppState whenever a scene's sample or sampler settings change; the samples stay
    // referenced by the caller's cache for the whole session, so the audio thread never drops the last reference.
    void SetSceneSamples(const std::array<SamplePtr, kNumPatterns>& samples) {
        std::lock_guard<SceneMutex> lock(patternMutex_);
        sceneSamples_ = samples; samplesManaged_ = true;
        if (project_ && engine_) ApplySceneSound(PlayPatternIndex());
    }
    // v44.53: the samples of the SAMPLE engines per scene: [scene][0..2 = synths, 3 = chord] (null = none)
    void SetSceneSynthSamples(const std::array<std::array<SamplePtr, kNumSynthTracks + 1>, kNumPatterns>& samples) {
        std::lock_guard<SceneMutex> lock(patternMutex_);
        sceneSynthSamples_ = samples;
        if (project_ && engine_) ApplySceneSound(PlayPatternIndex());
    }
    // C4b: the samples of the SAMPLE fills, by file name (the files of samples/ that a fill rule names). A fixed table, set by AppState
    // on the UI thread whenever a fill rule or the project changes; the samples stay referenced by AppState's cache, the audio thread only
    // compares names and copies a pointer.
    static constexpr int kMaxFillSamples = 32;
    struct FillSampleEntry { char name[FillRule::kSampleMax] = {}; SamplePtr sample; };
    void SetFillSamples(const std::array<FillSampleEntry, kMaxFillSamples>& table) {
        std::lock_guard<SceneMutex> lock(patternMutex_);
        fillSamples_ = table;
    }
    static bool SamePatch(const SynthPatch& a, const SynthPatch& b) { return std::memcmp(&a, &b, sizeof(SynthPatch)) == 0; }
    // Index of the pattern that is actually being PLAYED: the chain's
    // current pattern while song mode is driving, otherwise the one being
    // edited (project_->currentPattern).
    int PlayPatternIndex() const {
        if (songModeActive_ && project_ && project_->songChainLength > 0)
            return std::clamp(songPlayPattern_, 0, kNumPatterns - 1);
        const int pin = pinnedPlay_.load();
        if (pin >= 0) return pin;   // v44.51: the jam keeps playing while another tab edits something else
        return project_ ? project_->currentPattern : 0;
    }
    // v44.51: LIVE -> TRACK while the jam plays: the editor moves to a brick's scene, the player stays on the scene it was playing. Cleared
    // by Stop, by song mode, and by PlayHere / the next launch from LIVE (ReleasePin: playback follows the edited scene again).
    void PinPlayToCurrent() {
        std::lock_guard<SceneMutex> lock(patternMutex_);
        if (playing_.load() && project_ && !(songModeActive_ && project_->songChainLength > 0)) pinnedPlay_.store(project_->currentPattern);
    }
    void ReleasePin() { pinnedPlay_.store(-1); }
    // v44.58: the raw pin (-1 = none), for the UI to look at another scene for a moment (the armed scene of a queued launch) without moving the player
    int PinRaw() const { return pinnedPlay_.load(); }
    void SetPinRaw(int p) { pinnedPlay_.store(p); }
    // v44.58: EDIT mode on LIVE: the editor goes to scene `p`, the player stays on the scene it plays (pin it first).
    void PinPlayToPlaying() {
        std::lock_guard<SceneMutex> lock(patternMutex_);
        if (playing_.load() && project_ && !(songModeActive_ && project_->songChainLength > 0) && pinnedPlay_.load() < 0) pinnedPlay_.store(project_->currentPattern);
    }
    bool IsPinned() const { return pinnedPlay_.load() >= 0; }
    // true (default): the chain also moves the EDITED pattern so the
    // screen follows playback. false (desktop LIVE): the chain only moves
    // playback; the editor stays on whatever scene the user selected.
    void SetSongFollowsEdit(bool follow) { songFollowsEdit_ = follow; }
    bool SongFollowsEdit() const { return songFollowsEdit_; }
    // ---- Quantized scene launch ---------------------------------------------
    // Outside song mode the edited scene IS the played scene. LaunchScene()
    // switches to `p` immediately when stopped / quantize = NOW / song mode
    // is driving; otherwise it is queued and applied on the next beat (4 steps)
    // or bar (the playing scene's own length) boundary, Ableton-style.
    // Thread-safe (takes the pattern lock). 0 = NOW, 1 = BEAT, 2 = BAR.
    void SetLaunchQuantize(int q) { launchQuant_.store(std::clamp(q, 0, 2)); }
    int LaunchQuantize() const { return launchQuant_.load(); }
    // Scene waiting for its boundary, or -1.
    int PendingScene() const { return pendingScene_.load(); }
    void ClearPendingScene() { pendingScene_.store(-1); }
    void LaunchScene(int p) {
        std::lock_guard<SceneMutex> lock(patternMutex_);
        if (!project_) return;
        p = std::clamp(p, 0, kNumPatterns - 1);
        if (!playing_.load() || launchQuant_.load() == 0 || songModeActive_) {
            pendingScene_.store(-1);
            ApplyScene(p);
        } else if (p == PlayPatternIndex()) {
            pendingScene_.store(-1);        // already there: cancels a queued launch
        } else {
            pendingScene_.store(p);
        }
    }
    // v44.54: a bar hook. While armed, FireTick calls it once on the next bar line (tick % StepsPerBar() == 0), under the pattern lock, on the
    // audio thread, and disarms it. The app uses it to apply the LIVE switches queued with NEXT BAR (mute / solo / groups).
    // SetBarHook and ArmBarHook take the pattern lock; the hook itself must not block or allocate.
    void SetBarHook(std::function<void()> fn) { std::lock_guard<SceneMutex> lock(patternMutex_); barHook_ = std::move(fn); }
    void ArmBarHook(bool on) { barHookArmed_.store(on); }
    bool BarHookArmed() const { return barHookArmed_.load(); }
    // Ticks until the next bar line (0 when exactly on it); for the UI countdown.
    int TicksUntilBar() const { const int b = std::max(1, ClampGrid(project_ ? project_->patterns[PlayPatternIndex()].stepsPerBeat : 4) * 4); return (b - (globalTick_ % b)) % b; }
    // Ticks until the queued scene starts (0 if none queued); for UI countdown.
    int TicksUntilLaunch() const {
        if (pendingScene_.load() < 0 || !project_) return 0;
        int q = launchQuant_.load() == 1 ? ClampGrid(project_->patterns[PlayPatternIndex()].stepsPerBeat) : std::max(1, project_->patterns[PlayPatternIndex()].masterLength);
        return q - (globalTick_ % q);
    }

    // v44.40: the same countdown in beats, with the part of the current step already gone taken off (0 if nothing is queued).
    double BeatsUntilLaunch() const;
    double LaunchQuantumBeats() const;   // length of the launch grid in beats (1 for BEAT, the scene's loop for BAR)

    // ---- Chord bank (see ChordScene in Types.h) ------------------------------
    // With Project::chordBankOn the chord track plays the steps of chord scene
    // `ChordSceneCur()`, independent of the pattern scene. LaunchChordScene uses the
    // same launch quantize as LaunchScene (NOW / BEAT / BAR); the old chord is
    // released exactly on the boundary (or tied into the new one when both steps are
    // legato / full-length), so launching never cuts into the sound early.
    void LaunchChordScene(int c) {
        std::lock_guard<SceneMutex> lock(patternMutex_);
        if (!project_) return;
        c = std::clamp(c, 0, kNumChordScenes - 1);
        if (!playing_.load() || launchQuant_.load() == 0) {
            pendingChordScene_.store(-1);
            ApplyChordScene(c, false);
        } else if (c == chordSceneCur_) {
            pendingChordScene_.store(-1);   // already there: cancels a queued launch
        } else {
            pendingChordScene_.store(c);
        }
    }
    // Jump straight to a chord scene (bank switched on, project loaded, ...).
    void SetChordSceneNow(int c) {
        std::lock_guard<SceneMutex> lock(patternMutex_);
        pendingChordScene_.store(-1);
        ApplyChordScene(std::clamp(c, 0, kNumChordScenes - 1), false);
    }
    int ChordSceneCur() const { return chordSceneCur_; }
    int PendingChordScene() const { return pendingChordScene_.load(); }
    void ClearPendingChordScene() { pendingChordScene_.store(-1); }
    int TicksUntilChordLaunch() const {
        if (pendingChordScene_.load() < 0 || !project_) return 0;
        int q = launchQuant_.load() == 1 ? ClampGrid(project_->patterns[PlayPatternIndex()].stepsPerBeat) : std::max(1, project_->patterns[PlayPatternIndex()].masterLength);
        return q - (globalTick_ % q);
    }
    // The track whose STEPS play for track `i` of pattern `p`: the chord scene's
    // track for the chord track while the bank is on, the pattern's own otherwise.
    const Track& StepTrack(const Pattern& p, int i) const {
        if (project_ && project_->chordBankOn && i == kNumDrumTracks + kNumSynthTracks)
            return project_->chordScenes[std::clamp(chordSceneCur_, 0, kNumChordScenes - 1)].track;
        return p.tracks[i];
    }

    // ---- Live chord override ---------------------------------------------
    // While a slot is selected, every ACTIVE chord-track step plays that
    // chord instead of the notes stored in the step -- so the chord can be
    // switched freely (1 of 4) while the rhythm programmed on the chord
    // track keeps running. slot < 0 = off. Caller holds PatternMutex().
    void SetChordLive(int slot, const int8_t* notes, int count) {
        chordLiveCount_ = static_cast<uint8_t>(std::clamp(count, 0, 6));
        for (int i = 0; i < 6; ++i) chordLiveNotes_[i] = i < count ? notes[i] : 60;
        chordLiveSlot_ = chordLiveCount_ > 0 ? slot : -1;
    }
    void ClearChordLive() { chordLiveSlot_ = -1; chordLiveCount_ = 0; }
    int ChordLiveSlot() const { return chordLiveSlot_; }
    bool IsSongModeActive() const { return songModeActive_; }
    int SongChainPos() const { return songChainPos_; }
    int SongChainRepeatsLeft() const { return songChainRepeatsLeft_; }

    // ---- Arrangement overlays of the playing chain step (v44.5) -------------------------------------------------------
    // The chain step that drives playback, nullptr when song mode is off / the chain is empty.
    const ChainStep* PlayingChainStep() const {
        if (!songModeActive_ || !project_ || project_->songChainLength <= 0) return nullptr;
        return &project_->songChain[static_cast<size_t>(std::max(0, songChainPos_) % project_->songChainLength)];
    }
    // Progress 0..1 through the playing chain step (all its repeats), -1 when no chain step plays.
    double ChainStepProgress() const;
    // Steps in one pass of what plays now: the scene, or the shorter trim of the chain step.
    int LoopSteps() const;
    // v44.39: how far through the scene's loop we are (0..1) while a scene plays on LIVE; -1 when stopped or in song mode.
    double SceneProgress() const;
    // The fill that applies at `tick` (the scene's own, the chain step's override), and where in its window we are.
    FillState FillAt(int tick, FillRule* ruleOut = nullptr) const;
    bool FillActiveNow() const { return fillActiveUi_.load(); }
    FillKind FillKindNow() const { return static_cast<FillKind>(fillKindCur_.load()); }
    // After the chain step's overlays were edited while it plays: take the new mute mask at once.
    void RefreshArrangement() { std::lock_guard<SceneMutex> lock(patternMutex_); ApplyStepOverlay(); }

    // For UI playhead display: which step index each track is currently on.
    int GlobalTick() const { return globalTick_; }
    int ChordLoopPos(int len) const { return ChordStepIndex(globalTick_, std::max(1, len)); }   // v44.43: where the chord loop is, for the CHORD LAB playhead
    int StepForTrack(int trackIndex) const;

    // Live step/mixer editing from the UI thread must hold this lock --
    // it's the same lock RenderAndAdvance holds for the whole duration of
    // an audio callback, so any field the audio thread touches (pattern
    // bpm/swing/steps, mixer volume/pan/mute/solo) is race-free as long as
    // every UI-side mutation is also taken under it. Recursive because
    // RenderAndAdvance holds it around the whole callback while FireTick
    // (called from inside that callback) takes it again internally.
    SceneMutex& PatternMutex() { return patternMutex_; }
    // Called by scene-changing operations; true when the calling thread holds the lock. A miss is counted in SceneLockViolations().
    bool CheckSceneLock(const char* who) {
        if (patternMutex_.HeldByThisThread()) return true;
        if (sceneLockViolations_.fetch_add(1) == 0) std::fprintf(stderr, "[scene lock] %s called without the scene lock\n", who);
        return false;
    }
    int SceneLockViolations() const { return sceneLockViolations_.load(); }
    void ResetSceneLockViolations() { sceneLockViolations_.store(0); }
    // The EDITED pattern (what the UI shows). Playback uses PlayPattern().
    Pattern& CurrentPattern() { return project_->patterns[project_->currentPattern]; }
    Pattern& PlayPattern() { return project_->patterns[PlayPatternIndex()]; }

private:
    AudioEngine* engine_ = nullptr;
    Project* project_ = nullptr;

    std::atomic<bool> playing_{false};
    std::atomic<bool> liveBreakActive_{false};
    std::atomic<int> liveBreakIndex_{0};
    // Set true by SetLiveBreak, cleared by FireTick once it promotes the
    // break to active at the next bar boundary (tick % 16 == 0) -- this is
    // the launch-quantization queue: "wants to start, waiting for the top
    // of the next bar" vs. liveBreakActive_ meaning "currently playing".
    std::atomic<bool> liveBreakPending_{false};
    int liveBreakStartTick_ = 0; // audio-thread-only

    // Riser bar-sync queue -- see SetPendingRiser above.
    std::atomic<bool> pendingRiserArmed_{false};
    std::atomic<int> pendingRiserIndex_{0};
    std::atomic<int> pendingRiserBars_{1};
    std::atomic<float> pendingRiserGain_{1.0f};
    std::atomic<int> riserTicksLeft_{-1};
    int globalTick_ = 0;
    double samplesIntoStep_ = 0.0; // how far we are into the current step's duration

    // Step-probability roll (see Step::probability in Types.h). Owned here
    // rather than reusing AppState's UI-thread RngEngine() since FireTick
    // runs on the audio thread via RenderAndAdvance -- a fresh mt19937
    // seeded from random_device, same idiom AppState already uses for its
    // own randomization, just a separate instance for thread-safety.
    std::mt19937 rng_{std::random_device{}()};

    std::array<int, kNumSynthTracks> synthNoteOffTick_{};
    std::array<bool, kNumSynthTracks> synthNoteOffScheduled_{};

    // Phase 6: same scheduled-note-off bookkeeping as the 3 synths above,
    // but for the single dedicated Chord track (no array needed --
    // kNumChordTracks is fixed at 1, see Types.h).
    int chordNoteOffTick_ = 0;
    bool chordNoteOffScheduled_ = false;
    int chordHeldCount_ = 0;   // voices currently gated by the chord track (for tie detection)

    // A ratcheted step (Step::ratchet > 1) needs extra retriggers fired
    // partway through its own duration, not just at the step boundary.
    // FireTick schedules them here (as a fraction 0..1 of the CURRENT
    // step's length) with everything needed to fire the hit captured up
    // front, so RenderAndAdvance can stop sub-chunks at each one and fire
    // it sample-accurately, the same way it already does for step
    // boundaries. Captured by value rather than re-reading Track/Step
    // later so a pattern-bank switch mid-step can't change what fires.
    struct RatchetEvent {
        float fraction = 0.0f; // 0..1 within the step, when this fires
        bool isDrum = true;
        DrumSlot drumSlot = DrumSlot::Kick;
        uint8_t drumVariant = 0;
        DrumVoiceParams drumParams;
        int synthIndex = 0;
        int8_t synthNote = 60;
        float velocity = 0.8f;
        // Phase 6: a ratcheted Chord-track step re-strikes the SAME set of
        // notes each retrigger (captured up front, same "don't re-read
        // Track/Step later" reasoning as everything else here) -- isChord
        // is a 3rd category alongside isDrum's true/false, not a 3rd bool
        // flag combined with isDrum, so FireRatchetEvent's dispatch stays
        // an unambiguous 3-way branch.
        bool isChord = false;
        bool isSampler = false;   // v44.50: the SAMPLER track
        std::array<int8_t, 6> chordNotes{};
        int chordCount = 0;
        bool slide = false;    // v39: a delayed main hit carries its legato / tie flag
    };
    std::vector<RatchetEvent> pendingRatchets_;
    static constexpr size_t kMaxPendingRatchets = 512;   // N1: reserved in Init (11 tracks x 4 ratchets per tick is far below)

    // v39 microtiming. Every tick has the same length (swing and nudge are offsets of the HITS, not of the grid, so a
    // bar never changes its length). A hit that is due before its own tick (negative nudge, drum tracks only) is
    // scheduled by the lookahead at the end of the previous FireTick; this remembers which (track, tick, pattern) was
    // already handled and whether its probability roll came up, so the next FireTick does not fire it a second time.
    struct Prefire { int tick = -1; int pattern = -1; bool play = false; };
    std::array<Prefire, kNumTracks> prefire_{};
    double HitOffset(const Track& t, const Pattern& p, int tick, const Step& s) const;   // fraction of a step, see Sequencer.cpp
    void ScheduleHit(double x, const RatchetEvent& ev);
    void LookaheadEarlyHits(int tick);

    SceneMutex patternMutex_;
    float lastSamplerBpm_ = 0.0f;   // v44.52: the tempo the sampler voice was last told
    std::atomic<int> sceneLockViolations_{0};

    // Song/chain mode playback state -- see SetSongModeActive above.
    bool songModeActive_ = false;
    std::atomic<int> songLoopLo_{-1}, songLoopHi_{-1};   // v44.28: repeat a range of steps (lo < 0 = off)
    std::atomic<bool> songLoopStep_{false};   // v44.10: repeat the playing step
    std::atomic<bool> songRunOnce_{false};    // v44.10: stop after the last step instead of wrapping
    bool songEnded_ = false;                  // set by AdvanceSongChain when the track has just ended (audio thread)
    int songChainPos_ = 0;         // index into project_->songChain currently playing
    int chordLiveSlot_ = -1;
    uint8_t chordLiveCount_ = 0;
    std::array<int8_t, 6> chordLiveNotes_{{60, 60, 60, 60, 60, 60}};
    bool songFollowsEdit_ = true;
    int songPlayPattern_ = 0;      // pattern the chain is currently playing (song mode only)
    std::atomic<int> launchQuant_{2};   // see LaunchScene (default: next bar)
    std::atomic<int> pendingScene_{-1};
    std::function<void()> barHook_;               // v44.54: see SetBarHook (under the pattern lock)
    std::atomic<bool> barHookArmed_{false};
    // Chord bank playback state (audio thread / under the pattern lock).
    int chordSceneCur_ = 0;
    int chordBaseTick_ = 0;                  // tick at which the playing chord scene started (its step 0)
    std::atomic<int> pendingChordScene_{-1};
    void ApplyChordScene(int c, bool onBoundary);
    int ChordStepIndex(int tick, int len) const {
        if (!project_ || !project_->chordBankOn) return tick % len;
        int d = (tick - chordBaseTick_) % len;
        return d < 0 ? d + len : d;
    }
    void ApplyRepeaterRequest(bool immediate, int tick = 0);   // stutter on / off / density on the grid, see Sequencer.cpp
    std::atomic<int> pinnedPlay_{-1};
    // v44.58: scenes in the same LINK group share mute and solo: arriving from one to another, the new one takes the flags of the old one
    // (a flag changed while they were apart cannot be heard as a jump). Under the pattern lock.
    void CarryLinkedFlags(int from, int to) {
        if (!project_ || from == to || from < 0 || to < 0 || from >= kNumLiveScenes || to >= kNumLiveScenes) return;
        const uint8_t g = project_->sceneLink[static_cast<size_t>(from)];
        if (g == 0 || g != project_->sceneLink[static_cast<size_t>(to)]) return;
        for (int t = 0; t < kNumTracks; ++t) {
            project_->patterns[static_cast<size_t>(to)].tracks[static_cast<size_t>(t)].mute = project_->patterns[static_cast<size_t>(from)].tracks[static_cast<size_t>(t)].mute;
            project_->patterns[static_cast<size_t>(to)].tracks[static_cast<size_t>(t)].solo = project_->patterns[static_cast<size_t>(from)].tracks[static_cast<size_t>(t)].solo;
        }
    }
    void ApplyScene(int p) {
        CarryLinkedFlags(PlayPatternIndex(), p);
        if (pinnedPlay_.load() >= 0 && !(songModeActive_ && project_->songChainLength > 0)) {   // a launch that was queued on LIVE lands while TRACK is open: the editor stays where it is
            pinnedPlay_.store(p == project_->currentPattern ? -1 : p);   // v44.58: launching the scene that is being edited ends the pin
            project_->liveScene = p;
            engine_->GetMixer().SyncFromPattern(project_->patterns[p]);
            ApplySceneSound(p);
            return;
        }
        project_->currentPattern = p;
        if (!(songModeActive_ && project_->songChainLength > 0)) {
            engine_->GetMixer().SyncFromPattern(project_->patterns[p]);
            ApplySceneSound(p);
        }
    }
    std::array<SamplePtr, kNumPatterns> sceneSamples_{};
    std::array<std::array<SamplePtr, kNumSynthTracks + 1>, kNumPatterns> sceneSynthSamples_{};   // v44.53
    bool samplesManaged_ = false;
    std::array<DrumKit::BankPtr, kNumPatterns> sceneBanks_{};
    DrumKit::BankPtr defaultBank_;
    bool kitsManaged_ = false;
    int songChainRepeatsLeft_ = 0; // full pattern loops remaining at songChainPos_ before advancing

    double SamplesPerStep(int sampleRate) const;
    double SamplesPerSixteenth(int sampleRate) const;
    int StepsPerBeat() const;      // 4 on the classic 1/16 grid, see Pattern::stepsPerBeat
    int StepsPerBar() const;
    void FireTick(int tick);
    void FireRatchetEvent(const RatchetEvent& ev);
    // Called from RenderAndAdvance right after the tick that just wrapped
    // past the pattern's own length -- moves to the next chain step (or
    // just decrements the repeat counter if the current step isn't done
    // looping yet). Wraps back to chain step 0 after the last one, so a
    // programmed chain loops indefinitely, matching how a live performer
    // would expect "song mode" to behave (no separate stop-at-the-end
    // concept -- turn it off manually, or switch patterns, to break out).
    void AdvanceSongChain();
    // v44.5: the mute mask of the playing chain step goes to the mixer (0 when stopped / song mode off).
    void ApplyStepOverlay();
    // v44.6: level / filter sweeps of the playing chain step, evaluated at the current position (progress over all its repeats).
    void UpdateAutomation(int sampleRate);
    void ApplyDrumMods(DrumVoiceParams& p, int slot) const;   // v44.38
    std::atomic<int> fillKindCur_{0};      // the kind chosen for the fill that plays now (Random is resolved at the start of a window)
    int fillBreakCur_ = 0;                 // the library break chosen for a Break fill
    bool fillWas_ = false;
    int fillPosPrev_ = -1;                 // the window position of the previous tick (-1 = outside a window)
    bool fillDrumsMute_ = true;            // C4b: the SAMPLE fill that plays now silences the drums
    std::array<FillSampleEntry, kMaxFillSamples> fillSamples_{};
    void StartFillSample(const FillRule& rule, const FillState& fs, float gain);
    std::atomic<bool> fillActiveUi_{false};
    int riserArmedFor_ = -1;               // chain position whose next-step transition was already armed
};

} // namespace t8
