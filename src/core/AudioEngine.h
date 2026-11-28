#pragma once
// Ties together the drum engine, the 3 monophonic synths, the dedicated
// polyphonic Chord track (Phase 6, see ChordVoice.h), and the mixer. This
// is the single object the sequencer and the SDL audio callback both talk
// to. All public methods are safe to call from either the UI/sequencer
// thread (parameter changes, triggers) or the audio thread (Render); a
// mutex guards the small amount of shared state to keep this simple and
// obviously correct rather than lock-free-clever.
#include "Types.h"
#include "DrumEngine.h"
#include "SynthVoice.h"
#include "ChordVoice.h"
#include "SamplerVoice.h"
#include "Mixer.h"
#include "Effects.h"
#include "MasterChain.h"
#include "Dsp.h"
#include "Limiter.h"
#include "LoudnessMeter.h"
#include "Risers.h"
#include "MidiCapture.h"
#include "MultitrackRecorder.h"
#include "Pump.h"
#include <atomic>
#include <mutex>
#include <vector>
#include <array>

namespace t8 {

class AudioEngine {
public:
    void Init(int sampleRate);
    // v44.3: audio recovery. A NaN / infinity that gets into a feedback path (delay, reverb, master chain, limiter) stays there
    // and the output is silent from then on while the sequencer keeps running. Render() zeroes such samples and raises this
    // flag; the UI thread then calls ResetDsp(), which clears every effect / master state (sounds, patches and the kit stay).
    bool ConsumeDspFault() { return dspFault_.exchange(false); }
    void ResetDsp();

    DrumKit* GetKit() { return &kit_; }
    Mixer& GetMixer() { return mixer_; }
    const MasterChain& GetMasterChain() const { return masterChain_; }   // compressor gain-reduction meter for the UI
    const Limiter& GetLimiter() const { return limiter_; }                // limiter gain reduction
    LoudnessMeter& Loudness() { return loudness_; }                       // LUFS / true-peak readings, RequestReset()
    SynthVoice& Synth(int i) { return synths_[i]; }
    // Phase 6: the dedicated Chord track's polyphonic voice pool -- one
    // instance regardless of kNumChordTracks staying at 1 (see Types.h),
    // named singular/no index to match.
    ChordVoice& Chord() { return chordVoice_; }
    // v44.50: the SAMPLER track (the 11th instrument). The sample is an immutable shared buffer prepared on the UI thread; every call
    // takes the engine lock, so none of them can overlap a Render() block.
    void SamplerNoteOn(int midiNote, float velocity);
    void SamplerSet(const SamplePtr& sample, const SamplerPatch& patch);
    void SamplerSetTempo(double bpm);       // v44.52: SLICES / LOOP fit whole bars of this tempo
    void SamplerLoopRestart();              // v44.52: the LOOP starts again from its beginning (transport start, new scene sample)
    void SamplerLoopPreview();              // v44.52: HEAR a loop once at the project tempo
    void SamplerFadeOut();
    // v44.53: the SAMPLE engine of the synth tracks (slot 0..2) and of the chord track (slot kNumSynthTracks): the sample each voice plays.
    // Same rules as the sampler: immutable shared buffers prepared on the UI thread, every call takes the engine lock.
    void SynthSampleSet(int slot, const SamplePtr& sample);
    SamplePtr SynthSampleCurrent(int slot);
    void SynthPatchSet(int slot, const SynthPatch& patch);   // under the engine lock (HEAR of a scene that is not the played one)
    // v44.51: listen to a sample of the library without touching any track (plays into the master bus, one voice, a new one replaces it).
    void AuditionSample(const SamplePtr& sample, float gain = 0.8f, double speed = 1.0);   // v44.52.1: speed = project tempo / the loop's tempo
    void AuditionStop();
    // C4b: the SAMPLE fill voice (one, into the master bus like the risers). Starts at once at `startFrame` of the sample (> 0 when the
    // window was entered late), plays at `rate` (STRETCH = the sample's length / the window's length) and ends with a short fade so
    // that it is silent exactly `lengthFrames` output frames later. A new start replaces it; Stop fades it out in about 10 ms.
    // Called from the sequencer on the audio thread; the sample must stay referenced elsewhere (AppState's cache).
    void FillSampleStart(const SamplePtr& sample, double startFrame, double rate, float gain, int lengthFrames);
    void FillSampleStop();
    bool FillSamplePlaying() { std::lock_guard<std::mutex> lock(mutex_); return fillSample_ != nullptr; }
    SamplerPatch SamplerCurrentPatch();
    SamplePtr SamplerCurrentSample();

    void TriggerDrum(DrumSlot slot, float velocity, int variant, const DrumVoiceParams& params);
    // Sidechain pumps: the sequencer calls this on every grid step while it plays (`tick` = its step counter, `stepsPerBeat` =
    // the grid), which fires the pumps whose source is a steady beat (Every 1/4, 1/8, 1/16). See PumpSettings in Types.h.
    void PumpTick(int tick, int stepsPerBeat);
    // The same split in two, so the sequencer can swing the off-beat hits: which steady-beat pumps land on this grid step
    // (bit p = pump p), and fire pump p now (if it is still on).
    unsigned PumpBeatMask(int tick, int stepsPerBeat);
    void PumpFire(int p);
    // For the UI: how far pump p is down right now (0 = untouched, 1 = fully), updated once per audio block.
    // The UI's way to change the pumps: under the engine lock, so a drag in MIX LAB never lands half-copied in a Render block.
    void SetPumps(const std::array<PumpSettings, kNumPumps>& pumps) { std::lock_guard<std::mutex> lock(mutex_); mixer_.fx.pumps = pumps; }
    float PumpLevel(int p) const { return (p >= 0 && p < kNumPumps) ? pumpMeter_[static_cast<size_t>(p)].load(std::memory_order_relaxed) : 0.0f; }
    void SynthNoteOn(int synthIndex, int midiNote, float velocity, bool slide);
    void SynthNoteOff(int synthIndex);
    // notes/count: up to kMaxChordVoices simultaneous MIDI notes -- see
    // ChordVoice::NoteOn.
    void ChordNoteOn(const int8_t* notes, int count, float velocity, bool slide);
    void ChordNoteOff();
    // C2 (the PLAY layer): one chord tone on voice `v` of the chord pool / its release
    void ChordVoiceOn(int v, int8_t note, float velocity, bool slide);
    void ChordVoiceOff(int v);
    // v44.44: a second pool of chord voices for trying a chord by ear (CHORD LAB, HEAR buttons). It sounds through the chord track
    // (same patch, level, EQ, sends) next to whatever the sequencer plays, so an audition never cuts or steals the loop's chord,
    // and it is not written into a MIDI capture.
    void ChordAuditionOn(const int8_t* notes, int count, float velocity);
    void ChordAuditionOff();

    // Live-performance riser one-shots (see Risers.h) -- exposed so
    // AppState can load a kit's worth of samples and trigger
    // them without AudioEngine needing to know anything about pad
    // numbers or the action catalog.
    RiserPlayer& Risers() { return risers_; }
    // v44.51: the user's own transitions of the open project (slot, NAME, audio). Replaces the whole set under the engine's lock.
    void ReplaceUserTransitions(std::vector<UserTransition>&& list);

    // Live MIDI capture (see MidiCapture.h) -- logs every drum hit/synth
    // note/chord voicing that actually triggers through this engine,
    // regardless of source (programmed steps, LIVE BREAK, ratchets, or
    // live NOTE PLAY input), same "whatever really plays" scope Recorder
    // has for audio. A no-op while inactive.
    MidiCapture& Capture() { return capture_; }

    // Live multitrack recording (see MultitrackRecorder.h) -- captures
    // the same live performance Capture()/Recorder do, but as 7 separate
    // stem buses (Kick / Snare+Clap+Perc / Hats / Synth 1/2/3 / Chord)
    // written into one folder of WAVs while playing, ready to drop into Ableton Live. A
    // no-op while inactive.
    MultitrackRecorder& Multitrack() { return multitrack_; }

    // Called from the SDL audio callback (or the offline renderer). Writes
    // `numFrames` of interleaved stereo float audio into `out`.
    // `samplesPerStep` is this block's current step duration (see
    // Sequencer::SamplesPerStep) -- only used to size the global
    // repeater's capture window (Mixer::repeaterDensityIndex) in samples;
    // playback/mixing itself doesn't otherwise depend on it. Defaulted
    // (120bpm's 16th-note length) so every pre-existing direct-Render
    // test/tool call that doesn't care about the repeater (repeaterOn
    // defaults false, so the value is unused unless a test explicitly
    // enables it) keeps compiling unchanged.
    void Render(float* out, int numFrames, double samplesPerStep = 5512.5);

    int SampleRate() const { return sampleRate_; }

    // Oscilloscope feed for the UI: the post-master mono signal decimated by
    // kScopeDecimate (each point = mean of that many frames), kept in a ring.
    // CopyScope() writes the newest `n` points oldest->newest. Read from the UI
    // thread without a lock -- a torn read can only misplace a point or two
    // in a picture, never crash (indices are masked).
    static constexpr int kScopeSize = 2048;       // ring length (power of two)
    static constexpr int kScopeDecimate = 8;
    void CopyScope(float* out, int n) const {
        n = n < kScopeSize ? n : kScopeSize;
        unsigned w = scopeWrite_.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) out[i] = scopeRing_[(w - static_cast<unsigned>(n) + static_cast<unsigned>(i)) & (kScopeSize - 1)];
    }

    // Raw post-master mono ring for the spectrum view (newest n samples, oldest -> newest;
    // same lock-free "a torn read only smudges the picture" rule as the scope).
    static constexpr int kSpecSize = 4096;
    void CopyRecent(float* out, int n) const {
        n = n < kSpecSize ? n : kSpecSize;
        unsigned w = specWrite_.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) out[i] = specRing_[(w - static_cast<unsigned>(n) + static_cast<unsigned>(i)) & (kSpecSize - 1)];
    }

private:
    std::atomic<bool> dspFault_{false};
    std::array<float, kSpecSize> specRing_{};
    std::atomic<unsigned> specWrite_{0};
    int sampleRate_ = kSampleRate;
    DrumKit kit_;
    DrumEngine drums_;
    std::array<SynthVoice, kNumSynthTracks> synths_;
    ChordVoice chordVoice_;
    SamplerVoice sampler_;
    double sampleTempo_ = 120.0;   // v44.53: the tempo the SAMPLE engines were last told
    SamplePtr audSample_;           // v44.51: library audition
    double audPos_ = 0.0;
    float audGain_ = 0.8f;
    double audSpeed_ = 1.0;
    SamplePtr fillSample_;          // C4b: the SAMPLE fill voice
    double fillPos_ = 0.0, fillRate_ = 1.0;
    float fillGain_ = 1.0f;
    int fillLeft_ = 0;              // output frames until it must be silent
    int fillFade_ = 0;              // > 0: fading out (Stop), frames left of the fade
    int fillEndFade_ = 1;           // length of the ramp before fillLeft_ reaches 0
    int audFade_ = 0;               // frames left of a stop ramp (0 = none)
    ChordVoice auditionVoice_;
    Mixer mixer_;
    DelayEffect delay_;
    ReverbEffect reverb_;
    Saturator saturator_;           // master DRIVE (2x oversampled), see Dsp.h
    Limiter limiter_;               // look-ahead true-peak limiter, always in the path, see Limiter.h
    LoudnessMeter loudness_;        // BS.1770 meter on the final output
    MasterChain masterChain_;       // master compressor + finished-track chain, see MasterChain.h
    MasterFilter masterFilter_;     // live-performance HPF/LPF, see Mixer::masterHpfAmount/masterLpfAmount
    RepeaterEffect repeater_;       // live-performance beat-repeat/stutter, see Mixer::repeaterOn/repeaterDensityIndex
    RiserPlayer risers_;            // live-performance riser one-shots, see Risers.h
    MidiCapture capture_;           // live MIDI capture, see MidiCapture.h
    MultitrackRecorder multitrack_; // live multitrack stem recording, see MultitrackRecorder.h

    // Sidechain pumps (see PumpSettings in Types.h, core/Pump.h). Triggered by PumpSourceHit (from TriggerDrum / SynthNoteOn /
    // ChordNoteOn / ChordVoiceOn / SamplerNoteOn) and PumpTick (the sequencer's grid), all under mutex_ like Render, so no atomics.
    // Render computes every active pump's envelope for the block into pumpEnv_ first, then each instrument multiplies its dry
    // buffer by min over pumps of (1 - depth * env), through a short one-pole smoother (pumpGain_); with the 2 ms minimum ATTACK no duck clicks.
    void PumpSourceHit(int sourceTrack);              // mutex_ held
    void PumpPrepareBlock(int numFrames, double samplesPerSixteenth);   // mutex_ held, top of Render
    void PumpApply(int track, float* dry, int numFrames);               // mutex_ held, per instrument before its insert FX
    void PumpSkip(int track, int numFrames);          // an instrument that was not rendered this block: keep its smoother in step
    std::array<PumpEnvelope, kNumPumps> pumpEnv_{};
    std::array<PumpTiming, kNumPumps> pumpTiming_{};
    std::array<bool, kNumPumps> pumpLive_{};         // this block: the envelope moves (or is not 0)
    std::array<bool, kNumPumps> pumpWasOn_{};
    std::array<int, kNumPumps> pumpWasSource_{};
    std::array<std::vector<float>, kNumPumps> pumpBuf_;   // per-sample envelope of this block (sized in Init / grown in Render)
    std::array<float, kNumTracks> pumpGain_{};      // smoothed gain each instrument has now (1 = untouched)
    std::array<std::atomic<float>, kNumPumps> pumpMeter_{};   // for the UI: each pump's envelope at the end of the last block
    float arrGainApplied_ = 1.0f;   // v44.6: chain-step level as applied so far (ramped across each block)

    std::array<float, kScopeSize> scopeRing_{};
    std::atomic<unsigned> scopeWrite_{0};
    float scopeAcc_ = 0.0f;
    int scopeAccN_ = 0;

    std::mutex mutex_;
    std::vector<float> scratch_;   // reusable per-track dry buffer
    std::vector<float> delayBus_;  // accumulated delay-send bus for this block
    std::vector<float> reverbBus_; // accumulated reverb-send bus for this block
    std::vector<float> delayWet_;  // delay effect output for this block
    std::vector<float> reverbWet_; // reverb effect output for this block
    std::array<std::vector<float>, kNumStemGroups> stemBufs_; // per-stem buses, see MultitrackRecorder.h -- only filled while Multitrack() is recording
};

} // namespace t8
