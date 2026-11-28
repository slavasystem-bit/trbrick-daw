#pragma once
// Basic per-track mixer: volume/pan/mute/solo for each of the 10 tracks plus
// a master bus, with simple peak metering for the Mixer tab's VU display.
#include "Types.h"
#include "Project.h"
#include "Effects.h"
#include "TrackEq.h"
#include "Stereo.h"
#include "ChannelMix.h"
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>

namespace t8 {

class Mixer {
public:
    // The channel values (volume, pan, sends, insert FX, autopan, stereo, EQ) come from ChannelMix -- the same struct a
    // scene's Track inherits, so SyncFromPattern copies them in one assignment (C3 part 2). Here: mute / solo and the
    // audio-thread-only DSP state.
    struct TrackState : ChannelMix {
        bool mute = false;
        bool solo = false;
        // The DSP instances of the insert FX (TrackInsertFx in Effects.h; AudioEngine::Render runs it on the raw dry buffer,
        // pre-fader), of the channel EQ (applied before the insert FX) and of the stereo stage.
        TrackInsertFx insertFx;
        TrackEq eqDsp;
        StereoWidth stereoDsp;
        // Autopan LFO phase (0..1, audio-thread-only, not saved: a fresh playback starts from center).
        float autopanPhase_ = 0.0f;
        std::atomic<float> peak{0.0f}; // updated by audio thread, read by UI
        // Ramped mute/solo gain -- audio-thread-only, not atomic. See
        // MixTrack() for why this exists: flipping mute/solo used to gate
        // a track's output on or off instantly, which clicks/pops whenever
        // the track's signal wasn't already at zero the instant it happened
        // (e.g. muting mid-decay of a drum hit or mid-sustain of a synth
        // note). This ramps to the new on/off target over one audio block
        // (~5-6ms) instead, which is short enough to feel instant but long
        // enough that the ear doesn't hear the step.
        float smoothedMuteGain = 1.0f;
        float smPanOff = 0.0f, smRevOff = 0.0f, smDelOff = 0.0f;   // v44.38: where the parameter-row offsets were at the end of the last block (audio thread only)
    };

    std::array<TrackState, kNumTracks> tracks;
    float masterVolume = 0.4f;
    // v44.41 MIX LAB: an offline analysis engine points these at per-track mono buffers (>= the block length); MixTrack then writes the
    // post-fader / post-pan mono signal of its track there (pre sends, pre master). nullptr (the default) costs one branch per track per block.
    // Every Render() block advances tapPos by its length (AudioEngine::Render), so sub-chunked blocks land one after the other; tapCap = size of the buffers.
    std::array<float*, kNumTracks> tapMono{};
    size_t tapPos = 0, tapCap = 0;

    // ---- Live-performance master-bus FX (item 2 of the sixth revision) --
    // Deliberately NOT part of FxSettings/Project -- these are momentary
    // performance gestures (a filter sweep, a stutter effect engaged mid-
    // set), not mix decisions saved with the project, so they always
    // start bypassed/off on a fresh launch or project load regardless of
    // where a previous session left them. Written from the main thread by
    // the UI, read every block by AudioEngine::Render on
    // the audio thread -- same benign single-writer/single-reader race as
    // masterVolume above, not atomics.
    float masterHpfAmount = 0.0f; // 0..1, see MasterFilter::Process
    float masterLpfAmount = 0.0f; // 0..1, see MasterFilter::Process
    // repeaterOn / repeaterDensityIndex are what the audio engine runs with. The UI
    // writes the *Want* copies; the Sequencer moves them across on a
    // step boundary while playing (stutter on the grid) and at once when stopped.
    bool  repeaterOn = false;
    int   repeaterDensityIndex = 0; // index into kRepeaterDensities below
    bool  repeaterWantOn = false;
    int   repeaterWantDensity = 0;
    // Project-wide FX settings, mirrored here (audio-thread-visible copy)
    // from Project::fx -- see FxSettings in Types.h for what each field
    // does and Types.h's comment for why these are global rather than
    // per-pattern. Written directly by FxView on every edit (both here
    // AND into Project::fx, same two-copies pattern volume/pan already
    // use), and by SyncFxFromProject() below on project load/new/switch.
    FxSettings fx;
    // v44.5.4: the built-in kick of the PLAYED scene (Pattern::kick); FxSettings::kick is only the base value of a project.
    KickSynthSettings kickSynth;
    std::atomic<float> masterPeakL{0.0f};
    std::atomic<float> masterPeakR{0.0f};
    // Seconds remaining to keep showing a "CLIP" indicator, set by the
    // audio thread whenever the master bus exceeds unity gain before the
    // soft-clip knee (see AudioEngine::Render), decremented once per UI
    // frame in the main loop. A brief transient would otherwise flash and
    // vanish between two draw calls, so this holds it visible briefly.
    std::atomic<float> masterClipHold{0.0f};
    // Audio-thread load, written by the SDL callback (see AppBootstrap.h) and read by the UI:
    // share of each buffer's time budget the callback used (smoothed / recent peak) and a
    // count of buffers that took longer than their budget (an audible dropout).
    std::atomic<float> audioLoad{0.0f};
    std::atomic<float> audioLoadPeak{0.0f};
    std::atomic<unsigned> audioXruns{0};
    // N1: what the dropout readout shows. audioLockWaitUs = time the audio thread waited for the scene lock in the
    // current callback (added by the sequencer, taken by the callback). The rest is kept since the last reset (click).
    std::atomic<unsigned> audioLockWaitUs{0};
    std::atomic<int> audioBufferFrames{128};      // frames per callback, as the device delivers them
    std::atomic<unsigned> audioLongestUs{0};      // longest callback (what the dropout lasted)
    std::atomic<unsigned> audioLongestWaitUs{0};  // ... of which waiting for the scene lock (the screen held the sound)
    std::atomic<unsigned> audioSkippedBlocks{0};  // N1 (LIVE-5): buffers played as silence because the scene lock stayed busy
    // Audio thread only: consecutive late buffers make ONE dropout (its length is their sum), logged when a buffer is on time again.
    unsigned runUs = 0, runWaitUs = 0;
    float lastOutL = 0.0f, lastOutR = 0.0f;       // the last frame played, for the fade of a skipped buffer
    static constexpr int kDropoutLog = 8;
    struct Dropout { std::atomic<uint32_t> atMs{0}, lenUs{0}, waitUs{0}; };
    Dropout dropouts[kDropoutLog];                // the last few, newest at (audioXruns - 1) % kDropoutLog
    void ResetDropouts() {
        audioXruns.store(0); audioLongestUs.store(0); audioLongestWaitUs.store(0);
        for (auto& d : dropouts) { d.atMs.store(0); d.lenUs.store(0); d.waitUs.store(0); }
    }
    std::atomic<unsigned> audioBlocks{0};   // v44.3: counted by the audio callback; the watchdog in main() sees a stalled device when it stops moving

    // Allocates each track's TrackInsertFx DSP state (EQ filter + chorus
    // delay line) -- must be called once before Render(), same convention
    // as DelayEffect::Init/ReverbEffect::Init. See AudioEngine::Init.
    void InitInsertFx(int sampleRate) {
        for (auto& t : tracks) { t.insertFx.Init(sampleRate); t.eqDsp.Init(sampleRate); t.stereoDsp.Init(sampleRate); }
    }

    // v44.5.1: mute / solo belong to each scene, so a scene change loads them with everything else (keepMuteSolo = false,
    // the default). keepMuteSolo = true is kept for callers that want to refresh the sound values only.
    // C3 part 2: every channel value of one track (not mute / solo, not the DSP state) from a scene's Track.
    void SyncChannel(int i, const ChannelMix& from) {
        if (i >= 0 && i < kNumTracks) static_cast<ChannelMix&>(tracks[static_cast<size_t>(i)]) = from;
    }
    void SyncFromPattern(const Pattern& p, bool keepMuteSolo = false) {
        kickSynth = p.kick;
        for (int i = 0; i < kNumTracks; ++i) {
            SyncChannel(i, p.tracks[i]);
            if (!keepMuteSolo) {
                tracks[i].mute = p.tracks[i].mute;
                tracks[i].solo = p.tracks[i].solo;
            }
        }
    }

    // FX settings (and master volume, below) are project-wide (not
    // per-pattern), so they get their own sync call rather than
    // piggybacking on SyncFromPattern -- called wherever a project is
    // loaded/created/switched (see main.cpp, AppState).
    void SyncFxFromProject(const Project& proj) {
        fx = proj.fx;
        // Master volume used to live ONLY here (audio-thread-visible
        // Mixer state), never round-tripped through Project/Save/Load --
        // a user reported it silently resetting on every relaunch. Now
        // Project::masterVolume is the copy that gets saved (MixerView's
        // MASTER column writes there first, same convention as fx above),
        // and this is where it gets mirrored back in on load/new/switch.
        masterVolume = proj.masterVolume;
    }

    bool AnySolo() const {
        for (auto& t : tracks) if (t.solo) return true;
        return false;
    }

    // v44.5: tracks silenced by the chain step that is playing (bit i = track i). Written by the sequencer at a chain step, read by
    // the audio thread; independent of the performer's own mute / solo buttons, which are not touched.
    std::atomic<unsigned> arrMuteMask{0};
    // v44.6: dynamics of the playing chain step (written by the sequencer, read by the audio thread): a gain on the master bus and
    // two filter amounts that are added (max) to the performer's own filter pad.
    std::atomic<float> arrGain{1.0f}, arrHpf{0.0f}, arrLpf{0.0f};
    // v44.13: the level of every instrument over the playing chain step (1 = untouched), multiplied into the track's own mute ramp so
    // a fade never steps; independent of the performer's faders and mute / solo buttons.
    struct ArrTrackGain { std::atomic<float> v{1.0f}; };
    ArrTrackGain arrTrackGain[kNumTracks];
    // v44.38: parameter rows of the playing chain step: the offset of every parameter of every instrument, -1..1 (0 = untouched), see
    // ArrangeTypes.h (AutoParam). Pan and the sends are applied by MixTrack, tune / decay when a drum is struck, cutoff / resonance by the synth.
    struct ArrParam { std::atomic<float> v[kNumAutoParams]; ArrParam() { for (auto& x : v) x.store(0.0f); } };
    ArrParam arrParam[kNumTracks];
    float ParamOff(int track, AutoParam p) const { return arrParam[track].v[static_cast<int>(p)].load(std::memory_order_relaxed); }

    bool ShouldPlay(int trackIndex) const {
        const auto& t = tracks[trackIndex];
        if (t.mute) return false;
        if ((arrMuteMask.load(std::memory_order_relaxed) >> trackIndex) & 1u) return false;
        if (AnySolo() && !t.solo) return false;
        return true;
    }

    // Applies this track's volume/pan/mute-ramp to a per-track dry buffer
    // and mixes it into the master buffer, updating peak meters as it goes.
    // Callers should call this every block regardless of mute/solo state
    // (as long as the track actually has audio to offer) -- muting is
    // applied here as a ramped gain, not by the caller skipping the call,
    // so a still-sounding voice fades out smoothly instead of clicking.
    //
    // delayBus/reverbBus (optional, pass nullptr to skip) accumulate this
    // track's post-volume/pan/mute signal weighted by its delaySend/
    // reverbSend, for AudioEngine to run through the shared delay/reverb
    // effects afterward -- taken from the SAME already-computed l/r as the
    // dry mix below (not a second independent gain pass), so a track's
    // send always matches what you actually hear it playing at.
    //
    // stemBus (optional, pass nullptr to skip) accumulates the SAME l/r --
    // post volume/pan/mute-ramp, but BEFORE any delay/reverb send weighting
    // -- for MultitrackRecorder's live stem capture (see
    // MultitrackRecorder.h): several tracks can share one stem (e.g.
    // Snare+Clap+Perc all feed the same buffer across separate MixTrack
    // calls within one Render() block), which is exactly why this adds
    // into stemBus rather than assigning.
    void MixTrack(int trackIndex, const float* trackBuf, float* master, int numFrames,
                   float* delayBus = nullptr, float* reverbBus = nullptr, int sampleRate = 44100,
                   float* stemBus = nullptr) {
        auto& t = tracks[trackIndex];
        float* const tap = tapMono[static_cast<size_t>(trackIndex)];
        float targetGain = ShouldPlay(trackIndex) ? arrTrackGain[trackIndex].v.load(std::memory_order_relaxed) : 0.0f;
        float startGain = t.smoothedMuteGain;
        float peak = 0.0f;
        // Autopan (see Track::autopanRate/autopanDepth): depth<=0 is the
        // fast, unchanged path -- pl/pr computed once from the static pan,
        // same as before this feature existed. depth>0 recomputes pl/pr
        // every SAMPLE from an LFO-modulated pan instead, since a
        // per-block-only pan would make the sweep audibly steppy at
        // anything but a glacial rate.
        bool autopan = t.autopanDepth > 0.0f;
        // v44.38: parameter-row offsets for pan and the two sends; all zero (and were zero) = the unchanged fast path
        const float tgPan = ParamOff(trackIndex, AutoParam::Pan), tgRev = ParamOff(trackIndex, AutoParam::Reverb), tgDel = ParamOff(trackIndex, AutoParam::Delay);
        const bool modPan = t.smPanOff != 0.0f || tgPan != 0.0f, modRev = t.smRevOff != 0.0f || tgRev != 0.0f, modDel = t.smDelOff != 0.0f || tgDel != 0.0f;
        const float smPan = t.smPanOff, smRev = t.smRevOff, smDel = t.smDelOff;
        float phaseInc = autopan ? (t.autopanRate / static_cast<float>(std::max(1, sampleRate))) : 0.0f;
        float pl, pr;
        PanGains(t.pan, pl, pr);
        const bool widen = !t.stereoDsp.Idle(t.width, t.spread);
        for (int i = 0; i < numFrames; ++i) {
            float frac = numFrames > 1 ? static_cast<float>(i) / static_cast<float>(numFrames - 1) : 1.0f;
            float g = startGain + (targetGain - startGain) * frac;
            if (autopan) {
                float sweep = std::sin(2.0f * 3.14159265f * t.autopanPhase_) * t.autopanDepth;
                float effPan = std::clamp(t.pan + (modPan ? smPan + (tgPan - smPan) * frac : 0.0f) + sweep, -1.0f, 1.0f);
                PanGains(effPan, pl, pr);
                t.autopanPhase_ += phaseInc;
                if (t.autopanPhase_ >= 1.0f) t.autopanPhase_ -= 1.0f;
            } else if (modPan) {
                PanGains(std::clamp(t.pan + smPan + (tgPan - smPan) * frac, -1.0f, 1.0f), pl, pr);
            }
            const float dSend = modDel ? std::clamp(t.delaySend + smDel + (tgDel - smDel) * frac, 0.0f, 1.0f) : t.delaySend;
            const float rSend = modRev ? std::clamp(t.reverbSend + smRev + (tgRev - smRev) * frac, 0.0f, 1.0f) : t.reverbSend;
            float inL = trackBuf[static_cast<size_t>(i) * 2 + 0], inR = trackBuf[static_cast<size_t>(i) * 2 + 1];
            if (widen) t.stereoDsp.Process(inL, inR, t.width, t.spread);
            float l = inL * t.volume * pl * g;
            float r = inR * t.volume * pr * g;
            master[static_cast<size_t>(i) * 2 + 0] += l;
            master[static_cast<size_t>(i) * 2 + 1] += r;
            if (tap && tapPos + static_cast<size_t>(i) < tapCap) tap[tapPos + static_cast<size_t>(i)] = 0.5f * (l + r);
            if (stemBus) {
                stemBus[static_cast<size_t>(i) * 2 + 0] += l;
                stemBus[static_cast<size_t>(i) * 2 + 1] += r;
            }
            if (delayBus && dSend > 0.0f) {
                delayBus[static_cast<size_t>(i) * 2 + 0] += l * dSend;
                delayBus[static_cast<size_t>(i) * 2 + 1] += r * dSend;
            }
            if (reverbBus && rSend > 0.0f) {
                reverbBus[static_cast<size_t>(i) * 2 + 0] += l * rSend;
                reverbBus[static_cast<size_t>(i) * 2 + 1] += r * rSend;
            }
            peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
        }
        t.smoothedMuteGain = targetGain;
        t.smPanOff = tgPan; t.smRevOff = tgRev; t.smDelOff = tgDel;
        // decaying peak-hold so meters don't flicker to zero between hits
        float prev = t.peak.load(std::memory_order_relaxed);
        t.peak.store(std::max(peak, prev * 0.9f), std::memory_order_relaxed);
    }
};

} // namespace t8
