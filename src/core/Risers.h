#pragma once
// A small one-shot sample player for "riser" FX sounds -- the classic
// build-up/tension sweep a live performer drops in before a drop or a
// section change. Deliberately NOT part of DrumKit/DrumEngine (those are
// built around the 6 fixed, always-present drum slots with variant
// selection per step; risers are a flat, browsable list of one-off
// one-shots triggered live, closer in spirit to LIVE BREAK than to a kit
// voice) -- see AppState::TriggerRiser/BrowseRiser.
#include "WavFile.h"
#include "Resample.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>
#include <memory>

namespace t8 {

// One playing instance of a riser one-shot. Same shape as DrumEngine's
// DrumVoice, just without the choke/slot bookkeeping that only matters
// for the 6 fixed drum slots. `rate` is the playhead's per-output-sample
// advance (1.0 = the sample's native speed) -- see ComputeRiserBarSyncRate
// below for why this is now usually something other than 1.0.
struct RiserVoice {
    const SampleBuffer* buffer = nullptr;
    double playhead = 0.0;
    bool active = false;
    double rate = 1.0;
    double delay = 0.0;        // output frames of silence before the first sample (back-timed start)
    int fadeInTotal = 0;       // output frames of fade-in (used when we cut into the middle of the sample)
    int fadeInDone = 0;
    float gain = 1.0f;         // v44.13
    std::shared_ptr<const SincTable> sinc;   // band-limited varispeed kernel (null = rate 1, samples read as stored)
};

// A riser's natural length (fixed at however long its WAV is, ~3s for
// every stock one -- see tools/gen_risers.py) has no relationship to the
// project's tempo, so even with its START launch-quantized to the next bar
// boundary (Sequencer::SetPendingRiser/FireTick), it would still end
// mid-bar more often than not -- audibly "not fitting the bars" even
// though it started right on the beat. Rather than a real time-stretch
// (no phase-vocoder DSP exists anywhere in this codebase, and a riser's
// noisy/tonal-sweep content doesn't need one to sound musical), this picks
// whichever WHOLE number of bars is closest to the riser's natural length
// (minimum 1) and returns the playback-speed multiplier that makes it
// finish exactly on that bar boundary -- classic sampler-style varispeed.
// Trade-off: speeding up raises the sample's pitch, slowing down lowers
// it, same as playing a record at the wrong RPM. For the tempo ranges this
// app supports and a ~3s riser, that's usually a fairly small shift and
// reads as "part of the sound" rather than a defect -- and it's what
// actually keeps a riser's END, not just its start, landing on the grid.
inline double ComputeRiserBarSyncRate(double naturalSeconds, double bpm) {
    if (naturalSeconds <= 0.0) return 1.0;
    double barSeconds = (60.0 / std::max(20.0, bpm)) * 4.0; // 16 sixteenth-note ticks/bar, 4/4 -- matches Sequencer's tick % 16 bar convention
    if (barSeconds <= 0.0) return 1.0;
    int bars = std::max(1, static_cast<int>(std::lround(naturalSeconds / barSeconds)));
    double targetSeconds = bars * barSeconds;
    return targetSeconds > 0.0 ? naturalSeconds / targetSeconds : 1.0;
}

// v44.51: the user's own transitions of a project live in <project>/samples/transitions/tNN_name.wav; NN (01..40) is the slot, so the
// riser index a brick stores (RiserPlayer::kFirstUser + NN - 1) stays valid however the folder changes.
struct UserTransition { int slot = 0; std::string name; SampleBuffer audio; };
int TransitionSlotOfStem(const std::string& stem);                 // "t03_big_hit" -> 2, anything else -> -1
std::string TransitionDisplayName(const std::string& stem);        // "t03_big_hit" -> "BIG HIT"
std::vector<UserTransition> LoadUserTransitionFolder(const std::string& dir, int sampleRate);   // every readable tNN_*.wav in `dir`

class RiserPlayer {
public:
    static constexpr int kMaxRisers = 10;      // the stock riserN.wav files
    static constexpr int kNumGenerated = 4;    // v44.5: synthesized transitions after them: DOWNLIFTER, IMPACT, SUB DROP, REVERSE CRASH
    static constexpr int kNumUser = 40;        // v44.51: the user's own transitions (project's samples/transitions/tNN_name.wav), after the generated ones
    static constexpr int kFirstUser = kMaxRisers + kNumGenerated;
    static constexpr int kMaxSounds = kFirstUser + kNumUser;
    static constexpr int kMaxVoices = 2; // enough to avoid a click if re-triggered fast; risers are rarely stacked

    // How much of the END of every riser gets linearly ramped down to
    // silence, regardless of what's actually baked into the WAV. Every
    // stock riser's rising_env/swell_env (see tools/gen_risers.py) is
    // deliberately still building right up to its last sample -- great for
    // "still climbing" tension, but it meant playback used to just stop
    // dead the instant the buffer ran out, an audible hard click/gate
    // rather than a tail. Applying the release here in the engine, rather
    // than re-authoring the samples with a baked-in tail, means it's a
    // guaranteed property of playback regardless of what a riser sample
    // actually contains -- including any custom one a user drops in later
    // without a tail of its own.
    static constexpr double kReleaseSeconds = 0.35;
    // The release only starts AFTER the hit (so the climax is never ducked before the beat), but is
    // never shorter than this, to keep the cut click-free.
    static constexpr double kMinReleaseSeconds = 0.03;

    // Loads every riserN.wav (N = 0..9) it finds under `folder` -- missing
    // files are skipped, not a hard failure (same fail-safe convention as
    // DrumKit::LoadFromFolder: a partially-populated set still works,
    // Count() just reports fewer). Returns how many were actually loaded.
    int LoadFromFolder(const std::string& folder, int sampleRate);

    // v44.51: user sounds sit in slots 0..kNumUser-1 (riser index kFirstUser + slot). Replacing or clearing one stops the voices first
    // (a voice holds a raw pointer into the buffer). Called with the engine's lock held (AudioEngine::ReplaceUserTransitions).
    void ClearUserSounds();
    void SetUserSound(int slot, const std::string& name, SampleBuffer&& buf);

    // Number of sound slots (stock files + the generated transitions): the browse list is 0..Count()-1. A stock file that is
    // missing leaves an empty name at its index.
    int Count() const { return countSlots_; }
    const std::string& NameAt(int index) const;

    // The riser's natural (unstretched) length in seconds at the sample
    // rate LoadFromFolder was given -- what ComputeRiserBarSyncRate needs
    // to compute a bar-matching playback rate. 0.0 if index is out of
    // range or wasn't loaded.
    double DurationSeconds(int index) const;

    // Starts playback of riser `index` (0-based, must be < Count()) on the
    // next free voice, stealing the oldest active voice if all are busy.
    // `rate` is a playback-speed multiplier (see ComputeRiserBarSyncRate)
    // -- defaults to 1.0 (native speed) so existing callers/tests that
    // don't care about tempo-sync are unaffected.
    void Trigger(int index, double rate = 1.0);

    // Back-timed start (see Sequencer: riser lands its peak on the drop). The sound waits `delayFrames`
    // output frames, then plays from `startFrame` (source frames, > 0 = cut into the middle) at `rate`.
    void TriggerAligned(int index, double delayFrames, double startFrame, double rate, float gain = 1.0f);   // gain: v44.13 level of a chain transition

    // Source-frame position of the riser's loudest moment (the "hit" that must land on the beat).
    double HitFrame(int index) const {
        return (index < 0 || index >= kMaxSounds) ? 0.0 : hitFrame_[index];
    }
    double HitSeconds(int index) const {
        return loadedSampleRate_ > 0 ? HitFrame(index) / loadedSampleRate_ : 0.0;
    }

    // Renders and ADDS into `out` (interleaved stereo), mixing all
    // currently-active voices. Called once per audio block from
    // AudioEngine::Render, same convention as DrumEngine::RenderSlot.
    void Render(float* out, int numFrames, int sampleRate);

    // True while at least one voice is still playing -- lets the PERFORM
    // tab show "(PLAYING)" the same way it shows LIVE BREAK's state (see
    // RandomView.cpp).
    bool AnyActive() const {
        for (const auto& v : voices_) if (v.active) return true;
        return false;
    }

private:
    void StopAllVoices() { for (auto& v : voices_) v.active = false; }
    std::array<std::shared_ptr<SampleBuffer>, kMaxSounds> samples_;
    std::array<std::string, kMaxSounds> names_;
    std::array<double, kMaxSounds> hitFrame_{};
    int loadedCount_ = 0;      // stock files found
    int countSlots_ = 0;
    void ComputeHit(int index);
    void AddGenerated(int index, const char* name, SampleBuffer&& buf);
    int loadedSampleRate_ = 44100;
    std::array<RiserVoice, kMaxVoices> voices_{};
    int nextVoice_ = 0;
    std::shared_ptr<const SincTable> SincFor(double rate);
    struct CachedSinc { double rate = 0.0; std::shared_ptr<const SincTable> table; };
    std::array<CachedSinc, 8> sincCache_{};
    int sincNext_ = 0;
};

} // namespace t8
