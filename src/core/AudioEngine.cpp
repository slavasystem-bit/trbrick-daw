#include "AudioEngine.h"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace t8 {

void AudioEngine::Init(int sampleRate) {
    sampleRate_ = sampleRate;
    WarmSincBank();   // N1 (LIVE-9): the drum / riser interpolation kernels, built here instead of on the audio thread
    drums_.SetKit(&kit_);
    scratch_.resize(4096 * 2, 0.0f);
    delayBus_.resize(4096 * 2, 0.0f);
    reverbBus_.resize(4096 * 2, 0.0f);
    delayWet_.resize(4096 * 2, 0.0f);
    reverbWet_.resize(4096 * 2, 0.0f);
    for (auto& b : stemBufs_) b.resize(4096 * 2, 0.0f);
    delay_.Init(sampleRate);
    reverb_.Init(sampleRate);
    mixer_.InitInsertFx(sampleRate);
    masterFilter_.Init(sampleRate);
    repeater_.Init(sampleRate);
    multitrack_.Init(sampleRate);
    saturator_.Reset();
    limiter_.Reset();
    masterChain_.Reset();
}

void AudioEngine::ResetDsp() {
    std::lock_guard<std::mutex> lock(mutex_);
    delay_.Init(sampleRate_);
    reverb_.Init(sampleRate_);
    mixer_.InitInsertFx(sampleRate_);
    masterFilter_.Init(sampleRate_);
    repeater_.Init(sampleRate_);
    saturator_.Reset();
    limiter_.Reset();
    masterChain_.Reset();
    sidechainGain_ = 1.0f; sidechainApplied_ = 1.0f; sidechainSmooth_ = 1.0f;
}

void AudioEngine::TriggerDrum(DrumSlot slot, float velocity, int variant, const DrumVoiceParams& params) {
    std::lock_guard<std::mutex> lock(mutex_);
    drums_.Trigger(slot, velocity, variant, params, &mixer_.kickSynth);
    capture_.LogDrum(slot);
    // Every Kick hit re-triggers the sidechain duck (see sidechainGain_'s
    // comment) -- snapping straight back down even mid-release is what
    // produces the classic "pumping" groove under a steady kick, rather
    // than smoothly crossfading between overlapping ducks.
    if (slot == DrumSlot::Kick && mixer_.fx.sidechainEnabled) {
        sidechainGain_ = 1.0f - std::clamp(mixer_.fx.sidechainAmount, 0.0f, 1.0f);
    }
}

void AudioEngine::SynthNoteOn(int synthIndex, int midiNote, float velocity, bool slide) {
    if (synthIndex < 0 || synthIndex >= kNumSynthTracks) return;
    std::lock_guard<std::mutex> lock(mutex_);
    synths_[synthIndex].NoteOn(midiNote, velocity, slide);
    capture_.LogSynth(synthIndex, static_cast<int8_t>(midiNote));
}

void AudioEngine::SamplerNoteOn(int midiNote, float velocity) {
    std::lock_guard<std::mutex> lock(mutex_);
    sampler_.NoteOn(midiNote, velocity, sampleRate_);
}

void AudioEngine::SamplerSet(const SamplePtr& sample, const SamplerPatch& patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sample != sampler_.Sample()) sampler_.SetSample(sample);   // a new sample stops what sounded; a change of the patch alone never does
    sampler_.SetPatch(patch);
}

void AudioEngine::SamplerSetTempo(double bpm) {   // v44.53: also the tempo of the SAMPLE engines of the synths and the chord
    std::lock_guard<std::mutex> lock(mutex_);
    sampler_.SetTempo(bpm);
    for (SynthVoice& v : synths_) v.SetTempo(bpm);
    chordVoice_.SetTempo(bpm);
    sampleTempo_ = bpm;
}
void AudioEngine::SynthSampleSet(int slot, const SamplePtr& sample) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot >= 0 && slot < kNumSynthTracks) { synths_[static_cast<size_t>(slot)].SetSample(sample); synths_[static_cast<size_t>(slot)].SetTempo(sampleTempo_); }
    else if (slot == kNumSynthTracks) { chordVoice_.SetSample(sample); chordVoice_.SetTempo(sampleTempo_); }
}
SamplePtr AudioEngine::SynthSampleCurrent(int slot) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot >= 0 && slot < kNumSynthTracks) return synths_[static_cast<size_t>(slot)].Sample();
    if (slot == kNumSynthTracks) return chordVoice_.Sample();
    return nullptr;
}
void AudioEngine::SynthPatchSet(int slot, const SynthPatch& patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot >= 0 && slot < kNumSynthTracks) synths_[static_cast<size_t>(slot)].SetPatch(patch);
    else if (slot == kNumSynthTracks) chordVoice_.SetPatch(patch);
}
void AudioEngine::SamplerLoopRestart() { std::lock_guard<std::mutex> lock(mutex_); sampler_.LoopRestart(sampleRate_); }
void AudioEngine::SamplerLoopPreview() { std::lock_guard<std::mutex> lock(mutex_); sampler_.LoopPreview(sampleRate_); }

void AudioEngine::ReplaceUserTransitions(std::vector<UserTransition>&& list) {
    std::lock_guard<std::mutex> lock(mutex_);
    risers_.ClearUserSounds();
    for (UserTransition& u : list) risers_.SetUserSound(u.slot, u.name, std::move(u.audio));
}

void AudioEngine::AuditionSample(const SamplePtr& sample, float gain, double speed) {
    std::lock_guard<std::mutex> lock(mutex_);
    audSample_ = sample; audPos_ = 0.0; audGain_ = std::clamp(gain, 0.0f, 1.0f); audFade_ = 0; audSpeed_ = std::clamp(speed, 0.25, 4.0);
}
void AudioEngine::FillSampleStart(const SamplePtr& sample, double startFrame, double rate, float gain, int lengthFrames) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sample || sample->frames() < 2 || lengthFrames <= 0) { if (fillSample_) fillFade_ = std::max(1, sampleRate_ / 100); return; }
    fillSample_ = sample; fillPos_ = std::max(0.0, startFrame); fillRate_ = std::clamp(rate, 0.05, 16.0);
    fillGain_ = std::clamp(gain, 0.0f, 2.0f); fillLeft_ = lengthFrames; fillFade_ = 0;
    fillEndFade_ = std::max(1, std::min(sampleRate_ / 200, lengthFrames / 2));   // the last 5 ms ramp down to silence
}
void AudioEngine::FillSampleStop() { std::lock_guard<std::mutex> lock(mutex_); if (fillSample_ && fillFade_ == 0) fillFade_ = std::max(1, sampleRate_ / 100); }

void AudioEngine::AuditionStop() { std::lock_guard<std::mutex> lock(mutex_); if (audSample_) audFade_ = std::max(1, sampleRate_ / 100); }

void AudioEngine::SamplerFadeOut() { std::lock_guard<std::mutex> lock(mutex_); sampler_.FadeOutAll(sampleRate_); }

SamplerPatch AudioEngine::SamplerCurrentPatch() { std::lock_guard<std::mutex> lock(mutex_); return sampler_.Patch(); }
SamplePtr AudioEngine::SamplerCurrentSample() { std::lock_guard<std::mutex> lock(mutex_); return sampler_.Sample(); }

void AudioEngine::SynthNoteOff(int synthIndex) {
    if (synthIndex < 0 || synthIndex >= kNumSynthTracks) return;
    std::lock_guard<std::mutex> lock(mutex_);
    synths_[synthIndex].NoteOff();
}

void AudioEngine::ChordNoteOn(const int8_t* notes, int count, float velocity, bool slide) {
    std::lock_guard<std::mutex> lock(mutex_);
    chordVoice_.NoteOn(notes, count, velocity, slide);
    capture_.LogChord(notes, count);
}

void AudioEngine::ChordNoteOff() {
    std::lock_guard<std::mutex> lock(mutex_);
    chordVoice_.NoteOff();
}

void AudioEngine::ChordAuditionOn(const int8_t* notes, int count, float velocity) {
    std::lock_guard<std::mutex> lock(mutex_);
    auditionVoice_.SetPatch(chordVoice_.Patch());   // the sound of the chord track as it is now
    auditionVoice_.NoteOn(notes, count, velocity, false);
}

void AudioEngine::ChordAuditionOff() {
    std::lock_guard<std::mutex> lock(mutex_);
    auditionVoice_.NoteOff();
}

void AudioEngine::Render(float* out, int numFrames, double samplesPerStep) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Advance the capture clock by exactly this block's frame count BEFORE
    // returning -- Sequencer::RenderAndAdvance renders each chunk up to the
    // next trigger boundary and only THEN fires the tick/ratchet that
    // triggers into TriggerDrum/SynthNoteOn/ChordNoteOn, so by the time
    // that happens capture_'s clock already reflects this chunk's elapsed
    // time and the event lands at its exact sample-accurate position.
    capture_.Advance(numFrames, sampleRate_);
    std::memset(out, 0, sizeof(float) * numFrames * 2);

    size_t needed = static_cast<size_t>(numFrames) * 2;
    if (scratch_.size() < needed) scratch_.resize(needed);
    if (delayBus_.size() < needed) delayBus_.resize(needed);
    if (reverbBus_.size() < needed) reverbBus_.resize(needed);
    if (delayWet_.size() < needed) delayWet_.resize(needed);
    if (reverbWet_.size() < needed) reverbWet_.resize(needed);
    float* dry = scratch_.data();
    float* delayBus = delayBus_.data();
    float* reverbBus = reverbBus_.data();
    std::memset(delayBus, 0, sizeof(float) * needed);
    std::memset(reverbBus, 0, sizeof(float) * needed);

    // Live multitrack stem capture (see MultitrackRecorder.h) -- only
    // costs anything while actually recording: the 7 stem buses are
    // zeroed and handed to MixTrack below only in that case, and
    // multitrack_.Push() itself is a single cheap atomic-load no-op
    // otherwise (same fast-path convention as capture_/Recorder).
    bool stemsActive = multitrack_.IsRecording();
    std::array<float*, kNumStemGroups> stemPtrs{};
    if (stemsActive) {
        for (int g = 0; g < kNumStemGroups; ++g) {
            if (stemBufs_[g].size() < needed) stemBufs_[g].resize(needed);
            std::memset(stemBufs_[g].data(), 0, sizeof(float) * needed);
            stemPtrs[g] = stemBufs_[g].data();
        }
    }

    // 6 drum tracks -- always rendered and mixed, even while muted/soloed
    // out, so a voice that's still decaying keeps decaying naturally
    // instead of being frozen mid-hit and picked back up later. RenderSlot
    // is cheap when a slot has no active voice (just a bounded scan), and
    // Mixer::MixTrack applies mute/solo as a short ramped gain rather than
    // an instant on/off -- that ramp is what actually prevents the
    // click/pop a hard mute used to cause when toggled mid-decay.
    for (int i = 0; i < kNumDrumTracks; ++i) {
        std::memset(dry, 0, sizeof(float) * needed);
        drums_.RenderSlot(static_cast<DrumSlot>(i), dry, numFrames, sampleRate_);
        // Per-track insert FX (EQ tilt / drive / chorus) -- pre-fader, so
        // applied here on the raw dry buffer before MixTrack's volume/pan.
        // No-op (see TrackInsertFx::Process's fast path) when all three
        // knobs are at their default 0, so this costs nothing for tracks
        // that don't use it.
        auto& ts = mixer_.tracks[i];
        ts.eqDsp.Process(dry, numFrames, TrackEqOf(ts));
        ts.insertFx.Process(dry, numFrames, ts.insertDrive, ts.insertEqTilt, ts.insertChorus);
        mixer_.MixTrack(i, dry, out, numFrames, delayBus, reverbBus, sampleRate_,
                         stemsActive ? stemPtrs[kStemGroupForTrack[i]] : nullptr);
    }

    // 3 synth tracks -- same idea, but still gated on IsActive(): once a
    // voice's envelope has genuinely finished there's nothing left to
    // decay, so there's no reason to keep running the (more expensive)
    // synth DSP for it. Mute/solo no longer skips Render()+MixTrack while
    // active, for the same click-avoidance reason as the drum tracks above.
    for (int s = 0; s < kNumSynthTracks; ++s) {
        int trackIndex = kNumDrumTracks + s;
        if (!synths_[s].IsActive()) {
            mixer_.tracks[trackIndex].peak.store(mixer_.tracks[trackIndex].peak.load() * 0.9f);
            continue;
        }
        std::memset(dry, 0, sizeof(float) * needed);
        synths_[s].SetMod(mixer_.ParamOff(trackIndex, AutoParam::Cutoff) * AutoParamScale(5), mixer_.ParamOff(trackIndex, AutoParam::Resonance) * AutoParamScale(6));   // v44.38
        synths_[s].Render(dry, numFrames, sampleRate_);

        // Kick -> Bass sidechain ducking: synth 0 == bass (see Types.h/
        // RandomView's "synth 1 == bass" convention). Applied here, on the
        // raw dry buffer, so it's a plain extra gain stage ahead of the
        // normal volume/pan/mute mixing below -- unaffected by, and not
        // affecting, anything else in the chain.
        if (s == 0) {
            const float downStep = 1.0f / (0.006f * static_cast<float>(sampleRate_));
            const float upStep = 1.0f / (0.010f * static_cast<float>(sampleRate_));
            if (!mixer_.fx.sidechainEnabled) {
                sidechainGain_ = 1.0f; // no stale duck left over if re-enabled later
                if (sidechainApplied_ < 1.0f || sidechainSmooth_ < 0.9999f) {   // glide back to 1 instead of stepping
                    for (int i = 0; i < numFrames; ++i) {
                        sidechainSmooth_ += (sidechainApplied_ - sidechainSmooth_) * 0.04f;
                        dry[static_cast<size_t>(i) * 2 + 0] *= sidechainSmooth_;
                        dry[static_cast<size_t>(i) * 2 + 1] *= sidechainSmooth_;
                        sidechainApplied_ = std::min(1.0f, sidechainApplied_ + upStep);
                    }
                }
            } else {
                float releaseSec = std::max(0.02f, mixer_.fx.sidechainRelease);
                float perSampleStep = 1.0f / (releaseSec * static_cast<float>(sampleRate_));
                for (int i = 0; i < numFrames; ++i) {
                    if (sidechainApplied_ > sidechainGain_) sidechainApplied_ = std::max(sidechainGain_, sidechainApplied_ - downStep);
                    else sidechainApplied_ = std::min(sidechainGain_, sidechainApplied_ + upStep);
                    // one more short smoothing stage rounds the corners of the slewed ramp
                    sidechainSmooth_ += (sidechainApplied_ - sidechainSmooth_) * 0.04f;
                    dry[static_cast<size_t>(i) * 2 + 0] *= sidechainSmooth_;
                    dry[static_cast<size_t>(i) * 2 + 1] *= sidechainSmooth_;
                    sidechainGain_ = std::min(1.0f, sidechainGain_ + perSampleStep);
                }
            }
        }

        // Per-track insert FX -- same as the drum loop above, applied after
        // the sidechain duck (if any) so drive/chorus react to the
        // already-ducked signal rather than fighting it.
        auto& ts = mixer_.tracks[trackIndex];
        ts.eqDsp.Process(dry, numFrames, TrackEqOf(ts));
        ts.insertFx.Process(dry, numFrames, ts.insertDrive, ts.insertEqTilt, ts.insertChorus);
        mixer_.MixTrack(trackIndex, dry, out, numFrames, delayBus, reverbBus, sampleRate_,
                         stemsActive ? stemPtrs[kStemGroupForTrack[trackIndex]] : nullptr);
    }

    // 1 dedicated Chord track (Phase 6) -- same IsActive()-gated pattern as
    // the synth loop just above, one slot past the 3 synth tracks.
    {
        int trackIndex = kNumDrumTracks + kNumSynthTracks;
        const bool audition = auditionVoice_.IsActive();
        if (!chordVoice_.IsActive() && !audition) {
            mixer_.tracks[trackIndex].peak.store(mixer_.tracks[trackIndex].peak.load() * 0.9f);
        } else {
            std::memset(dry, 0, sizeof(float) * needed);
            chordVoice_.SetMod(mixer_.ParamOff(trackIndex, AutoParam::Cutoff) * AutoParamScale(5), mixer_.ParamOff(trackIndex, AutoParam::Resonance) * AutoParamScale(6));   // v44.38
            chordVoice_.Render(dry, numFrames, sampleRate_);
            if (audition) auditionVoice_.Render(dry, numFrames, sampleRate_);
            auto& ts = mixer_.tracks[trackIndex];
            ts.eqDsp.Process(dry, numFrames, TrackEqOf(ts));
        ts.insertFx.Process(dry, numFrames, ts.insertDrive, ts.insertEqTilt, ts.insertChorus);
            mixer_.MixTrack(trackIndex, dry, out, numFrames, delayBus, reverbBus, sampleRate_,
                             stemsActive ? stemPtrs[kStemGroupForTrack[trackIndex]] : nullptr);
        }
    }

    // 1 SAMPLER track (v44.50) -- always mixed, like the drums (not gated on IsActive like the synths): a hit that starts right after
    // a mute / unmute would otherwise meet a mute ramp that never ran while the voice was idle and leak through for a few ms.
    {
        const int trackIndex = kSamplerTrackIndex;
        std::memset(dry, 0, sizeof(float) * needed);
        if (sampler_.IsActive()) sampler_.Render(dry, numFrames, sampleRate_);
        auto& ts = mixer_.tracks[trackIndex];
        ts.eqDsp.Process(dry, numFrames, TrackEqOf(ts));
        ts.insertFx.Process(dry, numFrames, ts.insertDrive, ts.insertEqTilt, ts.insertChorus);
        mixer_.MixTrack(trackIndex, dry, out, numFrames, delayBus, reverbBus, sampleRate_,
                         stemsActive ? stemPtrs[kStemGroupForTrack[trackIndex]] : nullptr);
    }

    // Live multitrack capture: push this block's 7 already-mixed stem
    // buses (see above) into MultitrackRecorder -- after all 10 tracks
    // have been mixed into them, but BEFORE the shared delay/reverb sends
    // and master-bus processing below, so each stem stays clean and
    // independently remixable/masterable.
    if (stemsActive) {
        multitrack_.Push(numFrames, stemPtrs.data());
    }
    if (mixer_.tapCap != 0) mixer_.tapPos += static_cast<size_t>(numFrames);   // v44.41 MIX LAB: next block lands behind this one

    // Shared delay/reverb sends: run the accumulated per-track sends
    // through the two effects and mix their wet output straight into the
    // master bus, scaled by the project-wide return levels. A level of 0
    // means fully bypassed even if individual tracks have a nonzero send,
    // which is the "effect dialed in but currently off" state.
    const FxSettings& fx = mixer_.fx;
    float* delayWet = delayWet_.data();
    float* reverbWet = reverbWet_.data();
    delay_.Process(delayBus, delayWet, numFrames, fx.delayTimeSec, fx.delayFeedback, fx.delayTone, fx.delayPingPong);
    reverb_.Process(reverbBus, reverbWet, numFrames, fx.reverbSize, fx.reverbDamp, fx.reverbPreMs);
    if (fx.delayLevel > 0.0f || fx.reverbLevel > 0.0f) {
        for (int i = 0; i < numFrames; ++i) {
            out[static_cast<size_t>(i) * 2 + 0] +=
                delayWet[static_cast<size_t>(i) * 2 + 0] * fx.delayLevel +
                reverbWet[static_cast<size_t>(i) * 2 + 0] * fx.reverbLevel;
            out[static_cast<size_t>(i) * 2 + 1] +=
                delayWet[static_cast<size_t>(i) * 2 + 1] * fx.delayLevel +
                reverbWet[static_cast<size_t>(i) * 2 + 1] * fx.reverbLevel;
        }
    }

    // Riser one-shots (live performance) -- rendered and added directly
    // into the master bus, same as any other voice, so they're subject to
    // master volume/filter/repeater/drive below exactly like everything
    // else (no separate riser volume knob to keep in sync).
    risers_.Render(out, numFrames, sampleRate_);

    // C4b: the SAMPLE fill voice. Silent exactly at the end of its window: the last few ms before it fade to zero (AS IS cuts a
    // longer sample there; STRETCH reaches the end of the sample there anyway), a Stop fades in 10 ms, and a sample that ends early
    // gets the same short tail as the audition.
    if (fillSample_) {
        const SampleData& sb = *fillSample_;
        const int ch = std::max(1, sb.channels), frames = sb.frames();
        const int endFade = fillEndFade_;
        const int stopFade = std::max(1, sampleRate_ / 100);
        const int tail = std::max(1, static_cast<int>(sampleRate_ / 200 * fillRate_));
        for (int i = 0; i < numFrames; ++i) {
            const int f = static_cast<int>(fillPos_);
            if (f >= frames - 1 || fillLeft_ <= 0) { fillSample_.reset(); break; }
            float g = fillGain_;
            if (fillLeft_ < endFade) g *= static_cast<float>(fillLeft_) / static_cast<float>(endFade);
            if (fillFade_ > 0) { g *= static_cast<float>(fillFade_) / static_cast<float>(stopFade); if (--fillFade_ == 0) { fillSample_.reset(); break; } }
            if (frames - f < tail) g *= static_cast<float>(frames - f) / static_cast<float>(tail);
            const float fr = static_cast<float>(fillPos_ - f);   // linear interpolation between two source frames
            const size_t a = static_cast<size_t>(f) * static_cast<size_t>(ch), b = a + static_cast<size_t>(ch);
            const float l = sb.data[a] + (sb.data[b] - sb.data[a]) * fr;
            const float rr = ch > 1 ? sb.data[a + 1] + (sb.data[b + 1] - sb.data[a + 1]) * fr : l;
            out[static_cast<size_t>(i) * 2] += l * g; out[static_cast<size_t>(i) * 2 + 1] += rr * g;
            fillPos_ += fillRate_;
            --fillLeft_;
        }
    }

    // v44.51: the library audition (one voice, the sample's own speed at the engine rate, a short ramp when stopped or at the end)
    if (audSample_) {
        const SampleData& sb = *audSample_;
        const int ch = std::max(1, sb.channels), frames = sb.frames();
        const int tail = std::max(1, sampleRate_ / 200);
        for (int i = 0; i < numFrames; ++i) {
            const int f = static_cast<int>(audPos_);
            if (f >= frames) { audSample_.reset(); break; }
            float g = audGain_;
            if (audFade_ > 0) { g *= static_cast<float>(audFade_) / static_cast<float>(std::max(1, sampleRate_ / 100)); if (--audFade_ == 0) { audSample_.reset(); break; } }
            if (frames - f < tail) g *= static_cast<float>(frames - f) / static_cast<float>(tail);
            const float l = sb.data[static_cast<size_t>(f) * static_cast<size_t>(ch)];
            const float r = ch > 1 ? sb.data[static_cast<size_t>(f) * static_cast<size_t>(ch) + 1] : l;
            out[static_cast<size_t>(i) * 2] += l * g; out[static_cast<size_t>(i) * 2 + 1] += r * g;
            audPos_ += audSpeed_;
        }
    }

    // Live-performance master filter + repeater (item 2 of the sixth
    // revision) -- both operate directly on the master bus, in that
    // order: filter first (so a stutter loop captures the already-
    // filtered signal, matching how a real DJ mixer's filter sits before
    // its beat-repeat effect in the signal path), repeater second. Both
    // are fast-path no-ops when untouched (see MasterFilter::Process/
    // RepeaterEffect::Process), so leaving them alone costs nothing.
    masterFilter_.Process(out, numFrames, std::max(mixer_.masterHpfAmount, mixer_.arrHpf.load(std::memory_order_relaxed)), std::max(mixer_.masterLpfAmount, mixer_.arrLpf.load(std::memory_order_relaxed)));
    {
        int densityIdx = std::clamp(mixer_.repeaterDensityIndex, 0, kNumRepeaterDensities - 1);
        double captureLenD = samplesPerStep * static_cast<double>(kRepeaterDensities[densityIdx].stepFraction);
        int captureLen = std::clamp(static_cast<int>(captureLenD), 1, sampleRate_ * 7);
        repeater_.Process(out, numFrames, mixer_.repeaterOn, captureLen);
    }

    // Master chain (compressor, HPF / tone / saturation / width) sits before the
    // master volume so its threshold does not move with the fader.
    masterChain_.Process(out, numFrames, fx.master, sampleRate_);

    // Master volume, then the optional master DRIVE (2x oversampled tanh, off at 0), then the
    // look-ahead true-peak limiter. There is no forced soft clip any more: below the ceiling the
    // signal passes untouched. With the limiter switched off the output is only hard-clamped at 0 dBFS.
    const float mv = mixer_.masterVolume;
    float peakL = 0.0f, peakR = 0.0f;
    bool clipped = false;
    {   // v44.6: the chain step's level moves the whole mix; ramped across the block so a fade never steps
        const float gt = mixer_.arrGain.load(std::memory_order_relaxed);
        const float g0 = arrGainApplied_;
        if (g0 == 1.0f && gt == 1.0f) { for (int i = 0; i < 2 * numFrames; ++i) out[i] *= mv; }
        else {
            for (int i = 0; i < numFrames; ++i) {
                const float g = mv * (g0 + (gt - g0) * static_cast<float>(i + 1) / static_cast<float>(numFrames));
                out[static_cast<size_t>(i) * 2] *= g; out[static_cast<size_t>(i) * 2 + 1] *= g;
            }
            arrGainApplied_ = gt;
        }
    }
    {
        const float drive = std::clamp(fx.distortionDrive, 0.0f, 1.0f);
        saturator_.Process(out, numFrames, 1.0f + drive * 4.0f, drive > 0.004f ? std::min(1.0f, drive * 10.0f) : 0.0f);
    }
    if (!fx.master.limiterOn) {
        for (int i = 0; i < 2 * numFrames; ++i) if (std::fabs(out[i]) > 1.0f) { clipped = true; break; }
    }
    // v44.3: never hand a NaN / infinity to the limiter or the sound card; flag it so the UI thread clears the effect state
    auto sanitize = [&]() {
        bool bad = false;
        for (int i = 0; i < 2 * numFrames; ++i) if (!std::isfinite(out[i])) { out[i] = 0.0f; bad = true; }
        if (bad) dspFault_.store(true, std::memory_order_relaxed);
    };
    sanitize();
    limiter_.Process(out, numFrames, sampleRate_, fx.master.ceilingDb, fx.master.limiterOn);
    sanitize();
    loudness_.Process(out, numFrames, sampleRate_);
    for (int i = 0; i < numFrames; ++i) {
        float l = out[static_cast<size_t>(i) * 2 + 0];
        float r = out[static_cast<size_t>(i) * 2 + 1];
        peakL = std::max(peakL, std::fabs(l));
        peakR = std::max(peakR, std::fabs(r));
        scopeAcc_ += 0.5f * (l + r);
        specRing_[(specWrite_.load(std::memory_order_relaxed) + static_cast<unsigned>(i)) & (kSpecSize - 1)] = 0.5f * (l + r);
        if (++scopeAccN_ >= kScopeDecimate) {
            unsigned w = scopeWrite_.load(std::memory_order_relaxed);
            scopeRing_[w & (kScopeSize - 1)] = scopeAcc_ / static_cast<float>(kScopeDecimate);
            scopeWrite_.store(w + 1, std::memory_order_release);
            scopeAcc_ = 0.0f; scopeAccN_ = 0;
        }
    }
    specWrite_.store(specWrite_.load(std::memory_order_relaxed) + static_cast<unsigned>(numFrames), std::memory_order_release);
    mixer_.masterPeakL.store(std::max(peakL, mixer_.masterPeakL.load() * 0.9f));
    mixer_.masterPeakR.store(std::max(peakR, mixer_.masterPeakR.load() * 0.9f));
    // Hold the clip indicator visible for ~1.2s past the last time the
    // pre-clip signal actually exceeded unity, so brief transients (a
    // single accented hit) are still noticeable on screen.
    if (clipped) mixer_.masterClipHold.store(1.2f);
}

} // namespace t8
