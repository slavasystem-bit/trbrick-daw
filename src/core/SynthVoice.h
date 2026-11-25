#pragma once
// Phase 5 dispatch wrapper: holds one SubtractiveSynth, one FMSynth, and
// (Phase 5b) one WavetableSynth per synth track slot, and routes NoteOn/
// NoteOff to whichever matches the current patch's engineType, while
// ALWAYS rendering all three every block. Exposes the exact same
// 5-method interface every engine already shares (SetPatch/Patch/NoteOn/
// NoteOff/Render/IsActive), so it drops into AudioEngine::synths_ in
// place of a bare SubtractiveSynth with no change needed at any UI call
// site (every caller only ever touches .SetPatch() via engine.Synth(i),
// confirmed by grep across src/ before this was written).
//
// Why always render all three: if a track's engineType is switched WHILE
// a note/release-tail is still sounding on the previously-active engine,
// rendering only the "current" engine would abruptly cut that tail off
// (a click). Every engine fast-exits immediately when idle (see each
// one's own Render()'s `if (stage_ == Stage::Idle) return;`), so
// rendering the inactive ones costs nothing once they're actually
// silent -- the tail just finishes naturally on its own engine while the
// newly-selected engine waits for the next NoteOn.
#include "SubtractiveSynth.h"
#include "FMSynth.h"
#include "WavetableSynth.h"
#include "SampleSynth.h"
#include "SubLayer.h"
#include "Types.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

namespace t8 {

// v26: every synth voice can run up to kMaxUnison detuned, panned copies of its engine (UNISON), a sub / noise
// layer and a soft-saturation DRIVE after the engines. With the defaults (1 copy, no drive, no layer) the old
// direct path is used, sample for sample as before.
// v37: start phases of the unison copies (index 0 = leftmost). The copies of one note start in phase relationships that
// give a flat spectrum, mirrored left/right (so both ears get the same timbre) -- found by a numerical search that
// minimises the worst harmonic dip / peak (was -13 dB) and the loudness change over harmonics 1..16 at spreads 0, 0.7 and 1. The previous golden-ratio phases
// combed the sound (dips of -8 ... -13 dB on some harmonics) so a patch changed character when UNISON was switched on.
inline double UnisonStartPhase(int n, int i) {
    static const double kPhase[17][16] = {
{0},
        {0},
        {0.8333, 0.1667},
        {0.8000, 0.0000, 0.2000},
        {0.7043, 0.3038, 0.6962, 0.2957},
        {0.2103, 0.8118, 0.0000, 0.1882, 0.7897},
        {0.5653, 0.8258, 0.4290, 0.5710, 0.1742, 0.4347},
        {0.9492, 0.2347, 0.2427, 0.0000, 0.7573, 0.7653, 0.0508},
        {0.8865, 0.8660, 0.1276, 0.5606, 0.4394, 0.8724, 0.1340, 0.1135},
        {0.7312, 0.7080, 0.3926, 0.2911, 0.0000, 0.7089, 0.6074, 0.2920, 0.2688},
        {0.0836, 0.0744, 0.5971, 0.6816, 0.9650, 0.0350, 0.3184, 0.4029, 0.9256, 0.9164},
        {0.2357, 0.1210, 0.2948, 0.7659, 0.7017, 0.0000, 0.2983, 0.2341, 0.7052, 0.8790, 0.7643},
        {0.2705, 0.4989, 0.7828, 0.4088, 0.9234, 0.4810, 0.5190, 0.0766, 0.5912, 0.2172, 0.5011, 0.7295},
        {0.8037, 0.7773, 0.3075, 0.0365, 0.3153, 0.2322, 0.0000, 0.7678, 0.6847, 0.9635, 0.6925, 0.2227, 0.1963},
        {0.5467, 0.0603, 0.2485, 0.1507, 0.7870, 0.7873, 0.2159, 0.7841, 0.2127, 0.2130, 0.8493, 0.7515, 0.9397, 0.4533},
        {0.8546, 0.9754, 0.2355, 0.4413, 0.8467, 0.5171, 0.9785, 0.0000, 0.0215, 0.4829, 0.1533, 0.5587, 0.7645, 0.0246, 0.1454},
        {0.4173, 0.7977, 0.1975, 0.2008, 0.2120, 0.3309, 0.8896, 0.1513, 0.8487, 0.1104, 0.6691, 0.7880, 0.7992, 0.8025, 0.2023, 0.5827},
    };
    return (n >= 2 && n <= 16 && i >= 0 && i < n) ? kPhase[n][i] : std::fmod(0.381966 * i, 1.0);
}

class SynthVoice {
public:
    static constexpr int kMaxUnison = kMaxUnisonVoices;

    void SetPatch(const SynthPatch& p) {
        patch_ = p;
        unison_ = std::clamp(p.unisonVoices, 1, kMaxUnison);
        for (int i = 0; i < kMaxUnison; ++i) { sub_[i].SetPatch(p); fm_[i].SetPatch(p); wt_[i].SetPatch(p); smp_[i].SetPatch(p); }
        layer_.SetPatch(p);
    }
    const SynthPatch& Patch() const { return patch_; }
    // v44.38: parameter-row offsets for the filter (not part of the patch)
    void SetMod(float cutoffOct, float resAdd) { for (int i = 0; i < kMaxUnison; ++i) { sub_[i].SetMod(cutoffOct, resAdd); wt_[i].SetMod(cutoffOct, resAdd); smp_[i].SetMod(cutoffOct, resAdd); } }
    // v44.53: the sample of the SAMPLE engine (shared, immutable) and the project tempo (SLICES / LOOP fit whole bars of it)
    void SetSample(const SamplePtr& s) { sample_ = s; for (int i = 0; i < kMaxUnison; ++i) smp_[i].SetSample(s); }
    const SamplePtr& Sample() const { return sample_; }
    void SetTempo(double bpm) { for (int i = 0; i < kMaxUnison; ++i) smp_[i].SetTempo(bpm); }

    void NoteOn(int midiNote, float velocity, bool slide) {
        // v30 PHASE SYNC: a hard retrigger over a still-sounding note first ducks the old note to silence (~1.5 ms),
        // then starts the new note with every oscillator back at its start phase, so each note begins identically
        // (and keeps the same phase relation to a kick). Slides and notes starting from silence are unchanged.
        if (patch_.phaseSync && !slide && (pendingSync_ || IsActive())) {
            if (!pendingSync_) fadeOutDone_ = 0;
            pendingSync_ = true; fadeInLeft_ = 0; pendNote_ = midiNote; pendVel_ = velocity;
            return;
        }
        StartNote(midiNote, velocity, slide, false);
    }

    void StartNote(int midiNote, float velocity, bool slide, bool syncPhase) {
        const int n = unison_;
        for (int i = 0; i < n; ++i) {
            // copies spread evenly from -1 to +1: detune in cents, stereo position, start phase
            const float off = n == 1 ? 0.0f : (static_cast<float>(i) - 0.5f * (n - 1)) / (0.5f * (n - 1));
            const float cents = off * patch_.unisonDetune * 40.0f;
            const double ph0 = n == 1 ? 0.0 : UnisonStartPhase(n, i);
            pan_[i] = off * patch_.unisonSpread;
            gain_[i] = n == 1 ? 1.0f : 1.0f / std::sqrt(static_cast<float>(n));   // power-normalised: same loudness as one voice
            switch (patch_.engineType) {
                case SynthEngineType::FM:        fm_[i].SetUnisonCopy(cents, ph0); fm_[i].NoteOn(midiNote, velocity, slide); break;
                case SynthEngineType::Wavetable: wt_[i].SetUnisonCopy(cents, ph0); wt_[i].NoteOn(midiNote, velocity, slide); break;
                case SynthEngineType::Sample:    smp_[i].SetUnisonCopy(cents, ph0); smp_[i].NoteOn(midiNote, velocity, slide); break;
                case SynthEngineType::Subtractive: default: sub_[i].SetUnisonCopy(cents, ph0); sub_[i].NoteOn(midiNote, velocity, slide); break;
            }
        }
        if (layer_.Wanted()) layer_.NoteOn(midiNote, velocity, slide);
        if (syncPhase) {
            for (int i = 0; i < n; ++i) { sub_[i].SyncPhase(); fm_[i].SyncPhase(); wt_[i].SyncPhase(); smp_[i].SyncPhase(); }
            layer_.SyncPhase();
        }
    }
    void NoteOff() {
        if (pendingSync_) { pendingSync_ = false; fadeOutDone_ = 0; }   // a note-off that follows the pending note-on belongs to it: drop both
        // graceful release of every copy / engine (an engine switch or a smaller unison count can leave tails)
        for (int i = 0; i < kMaxUnison; ++i) { sub_[i].NoteOff(); fm_[i].NoteOff(); wt_[i].NoteOff(); smp_[i].NoteOff(); }
        layer_.NoteOff();
    }

    void Render(float* out, int numFrames, int sampleRate) {
        if (pendingSync_ || fadeInLeft_ > 0) { RenderSynced(out, numFrames, sampleRate); return; }
        RenderPlain(out, numFrames, sampleRate);
    }

private:
    // PHASE SYNC rendering: duck the old note, start the new one with reset phases, fade it in.
    void RenderSynced(float* out, int numFrames, int sampleRate) {
        const int fade = std::max(16, static_cast<int>(sampleRate * 0.0015));
        int pos = 0;
        std::vector<float>& tmp = syncTmp_;
        auto renderRamped = [&](int n, bool down) {
            tmp.assign(static_cast<size_t>(n) * 2, 0.0f);
            RenderPlain(tmp.data(), n, sampleRate);
            for (int i = 0; i < n; ++i) {
                float g;
                if (down) g = 1.0f - static_cast<float>(fadeOutDone_ + i + 1) / static_cast<float>(fade);
                else      g = static_cast<float>(fade - fadeInLeft_ + i + 1) / static_cast<float>(fade);
                g = std::clamp(g, 0.0f, 1.0f);
                out[static_cast<size_t>(pos + i) * 2] += tmp[static_cast<size_t>(i) * 2] * g;
                out[static_cast<size_t>(pos + i) * 2 + 1] += tmp[static_cast<size_t>(i) * 2 + 1] * g;
            }
        };
        if (pendingSync_) {
            const int n = std::min(fade - fadeOutDone_, numFrames);
            renderRamped(n, true);
            fadeOutDone_ += n; pos += n;
            if (fadeOutDone_ >= fade) {           // the old note is silent now: start the new one in phase
                StartNote(pendNote_, pendVel_, false, true);
                pendingSync_ = false; fadeOutDone_ = 0; fadeInLeft_ = fade;
            }
        }
        if (!pendingSync_ && pos < numFrames) {
            const int rest = numFrames - pos;
            if (fadeInLeft_ > 0) {
                const int n = std::min(fadeInLeft_, rest);
                renderRamped(n, false);
                fadeInLeft_ -= n; pos += n;
            }
            if (pos < numFrames) RenderPlain(out + static_cast<size_t>(pos) * 2, numFrames - pos, sampleRate);
        }
    }

    void RenderPlain(float* out, int numFrames, int sampleRate) {
        bool others = false;
        for (int i = 1; i < kMaxUnison; ++i) if (sub_[i].IsActive() || fm_[i].IsActive() || wt_[i].IsActive() || smp_[i].IsActive()) { others = true; break; }
        if (unison_ == 1 && !others && patch_.drive <= 0.0f) {
            sub_[0].Render(out, numFrames, sampleRate);      // the original direct path
            fm_[0].Render(out, numFrames, sampleRate);
            wt_[0].Render(out, numFrames, sampleRate);
            smp_[0].Render(out, numFrames, sampleRate);
        } else {
            const float g = 1.0f + 5.0f * patch_.drive;       // drive: tanh with make-up gain
            const float mk = 1.0f / (1.0f + 0.5f * (g - 1.0f));
            for (int done = 0; done < numFrames; done += kScratchFrames) {
                const int n = std::min(kScratchFrames, numFrames - done);
                float* dst = out + static_cast<size_t>(done) * 2;
                for (int i = 0; i < kMaxUnison; ++i) {
                    if (!(sub_[i].IsActive() || fm_[i].IsActive() || wt_[i].IsActive() || smp_[i].IsActive())) continue;
                    std::fill(scratch_.begin(), scratch_.begin() + static_cast<size_t>(n) * 2, 0.0f);
                    sub_[i].Render(scratch_.data(), n, sampleRate);
                    fm_[i].Render(scratch_.data(), n, sampleRate);
                    wt_[i].Render(scratch_.data(), n, sampleRate);
                    smp_[i].Render(scratch_.data(), n, sampleRate);
                    const float p = std::clamp(pan_[i], -1.0f, 1.0f);
                    const float ang = (p + 1.0f) * 0.78539816f;             // 0..pi/2, equal power
                    const float gl = std::cos(ang) * 1.41421356f * gain_[i], gr = std::sin(ang) * 1.41421356f * gain_[i];
                    for (int k = 0; k < n; ++k) {
                        float x = scratch_[static_cast<size_t>(k) * 2];
                        if (patch_.drive > 0.0f) x = std::tanh(x * g) * mk;
                        dst[static_cast<size_t>(k) * 2 + 0] += x * gl;
                        dst[static_cast<size_t>(k) * 2 + 1] += x * gr;
                    }
                }
            }
        }
        layer_.Render(out, numFrames, sampleRate);
    }

public:
    bool IsActive() const {
        if (layer_.IsActive()) return true;
        for (int i = 0; i < kMaxUnison; ++i) if (sub_[i].IsActive() || fm_[i].IsActive() || wt_[i].IsActive() || smp_[i].IsActive()) return true;
        return false;
    }

private:
    static constexpr int kScratchFrames = 512;
    SynthPatch patch_{};
    int unison_ = 1;
    std::array<SubtractiveSynth, kMaxUnison> sub_;
    std::array<FMSynth, kMaxUnison> fm_;
    std::array<WavetableSynth, kMaxUnison> wt_;
    std::array<SampleSynth, kMaxUnison> smp_;   // v44.53
    SamplePtr sample_;
    SubLayer layer_;
    std::array<float, kMaxUnison> pan_{}, gain_{};
    std::array<float, kScratchFrames * 2> scratch_{};
    bool pendingSync_ = false; int pendNote_ = 0; float pendVel_ = 1.0f;   // PHASE SYNC state
    int fadeOutDone_ = 0, fadeInLeft_ = 0;
    std::vector<float> syncTmp_;
};

} // namespace t8
