#pragma once
// v44.50: the voice of the SAMPLER track (the 11th instrument). A sample is held as an immutable SampleData shared between the
// project's cache (UI thread) and the voice (audio thread); the voice plays it ONE-SHOT: at the pitch of the step's note
// (patch.root plays it unchanged), from patch.start, for patch.decay of the rest, with a short attack and a click-free end.
// A new hit cuts the previous one (mono) unless patch.poly, which allows 4 hits at once. Cubic interpolation.
#include "TempoName.h"
#include "Types.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace t8 {

struct SampleData {
    std::vector<float> data;   // interleaved when channels == 2
    int channels = 1;
    int sampleRate = kSampleRate;
    std::string name;
    int frames() const { return channels > 0 ? static_cast<int>(data.size()) / channels : 0; }
};
using SamplePtr = std::shared_ptr<const SampleData>;

class SamplerVoice {
public:
    static constexpr int kVoices = 4;
    static constexpr double kLoopFadeSec = 0.002;   // v44.52.1: the crossfade at the seam of a LOOP

    void SetSample(const SamplePtr& s) { sample_ = s; Stop(); }
    const SamplePtr& Sample() const { return sample_; }
    void SetPatch(const SamplerPatch& p) {
        if (patch_.mode == 2 && p.mode != 2) for (Voice& v : voices_) if (v.active && v.loop) v.killStep = 0.01f;   // leaving LOOP: the loop fades out
        patch_ = p;
    }
    // v44.52: the project tempo (bpm), for SLICES / LOOP that fit whole bars.
    void SetTempo(double bpm) { bpm_ = bpm; }
    const SamplerPatch& Patch() const { return patch_; }
    // v44.53 (the SAMPLE engine of a synth): a note in LOOP mode starts the loop from its beginning (key-synced) instead of doing nothing;
    // the pitch of the sound can be modulated live (glide, LFO: a factor on the playing speed) and detuned (unison copies, in cents).
    void SetKeyedLoop(bool on) { keyed_ = on; }
    void SetPitchMod(double mul) { pitchMul_ = mul; }
    void SetDetune(double cents) { detuneCents_ = cents; }

    void Stop() { for (Voice& v : voices_) v.active = false; }
    // Fades every hit out in a few ms (transport stop, panic): a long sample must not keep playing after STOP.
    void FadeOutAll(int engineRate) { for (Voice& v : voices_) if (v.active) v.killStep = 1.0f / std::max(8.0f, 0.004f * static_cast<float>(engineRate)); }
    bool IsActive() const { for (const Voice& v : voices_) if (v.active) return true; return false; }

    // The part of the sample that plays (START and LENGTH), in source frames.
    void Region(double& a, double& b) const {
        const int n = sample_ ? sample_->frames() : 0;
        a = std::clamp(static_cast<double>(patch_.start), 0.0, 0.95) * n;
        const double len = std::max(2.0, (n - a) * std::clamp(static_cast<double>(patch_.decay), 0.02, 1.0));
        b = std::min(static_cast<double>(n), a + len);
    }
    static double NameBpm(const std::string& name) { return TempoFromName(name); }   // see TempoName.h
    // How much faster (> 1) or slower the played part [a, b] runs so that it lasts a whole number of bars at the project tempo; 1 = native speed.
    // AUTO: with a tempo in the file name the speed is exactly project / file tempo (and the bars are counted from it); without one the
    // whole number of bars (1, 2, 4, 8, 16) that needs the smallest change of speed (a guess: 170 BPM at 120 BPM sits right between two).
    // `barsOut` / `fileBpmOut` (optional) report what AUTO decided, for the screen.
    static double FitRate(const SampleData& smp, const SamplerPatch& p, double bpm, double a, double b, int* barsOut = nullptr, double* fileBpmOut = nullptr) {
        if (barsOut) *barsOut = 0;
        if (fileBpmOut) *fileBpmOut = 0.0;
        if (p.mode == 0 || p.fitBars < 0 || bpm <= 0.0 || smp.sampleRate <= 0) return 1.0;
        const double sec = (b - a) / smp.sampleRate, barSec = 240.0 / bpm;
        if (sec <= 0.0) return 1.0;
        int bars = p.fitBars;
        if (bars == 0) {
            const double fb = NameBpm(smp.name);
            if (fb > 0.0) {
                if (fileBpmOut) *fileBpmOut = fb;
                if (barsOut) *barsOut = std::max(1, static_cast<int>(std::lround(sec / (240.0 / fb))));
                return bpm / fb;
            }
            double best = 1e9; bars = 1;
            for (int c : {1, 2, 4, 8, 16}) { const double d = std::fabs(std::log(sec / (c * barSec))); if (d < best) { best = d; bars = c; } }
        }
        if (barsOut) *barsOut = bars;
        return sec / (bars * barSec);
    }
    double FitFactor(double a, double b) const { return sample_ ? FitRate(*sample_, patch_, bpm_, a, b) : 1.0; }
    // v44.52.1: where the slices of the played part [a, b] start: starts[0..n-1], starts[n] = b. Equal parts (4 / 8 / 16 / 32) or, for
    // HITS (slices == 0), the marks that lie inside the part. Returns n (0 = no slices).
    int SliceStarts(double* starts, double a, double b) const {
        if (!sample_ || sample_->frames() < 2) return 0;
        const double frames = sample_->frames();
        int n = 0;
        if (patch_.slices == 0) {
            for (int i = 0; i < patch_.markCount && n < 32; ++i) { const double m = patch_.marks[static_cast<size_t>(i)] / 65535.0 * frames; if (m >= a && m < b - 1.0) starts[n++] = m; }
            if (n == 0) starts[n++] = a;   // no marks yet: the whole part is one slice
        } else {
            n = patch_.slices == 4 || patch_.slices == 16 || patch_.slices == 32 ? patch_.slices : 8;
            for (int i = 0; i < n; ++i) starts[i] = a + (b - a) * i / n;
        }
        starts[n] = b;
        return n;
    }
    void NoteOn(int note, float velocity, int engineRate) {
        if (!sample_ || sample_->frames() < 2) return;
        double a0, b0; Region(a0, b0);
        if (patch_.mode == 2) {   // LOOP plays by itself (LoopRestart): a note does nothing, except for a synth (keyed): it starts the loop over, at the note's pitch
            if (!keyed_) return;
            for (Voice& v : voices_) if (v.active && v.loop) v.killStep = 1.0f / std::max(8.0f, 0.003f * static_cast<float>(engineRate));
            StartVoice(a0, b0, (note - patch_.root) / 12.0, velocity, engineRate, true, FitFactor(a0, b0));
            return;
        }
        double a = a0, b = b0;
        if (patch_.mode == 1) {   // SLICES: ROOT plays the first slice, ROOT + 1 the second ...
            double st[33]; const int n = SliceStarts(st, a0, b0);
            const int idx = note - patch_.root;
            if (idx < 0 || idx >= n) return;
            a = st[idx]; b = st[idx + 1];
        }
        StartVoice(a, b, patch_.mode == 1 ? 0.0 : (note - patch_.root) / 12.0, velocity, engineRate, false, FitFactor(a0, b0));
    }
    // LOOP: starts (or restarts) the loop at its beginning; called when the transport starts and when the scene's sample changes.
    void LoopRestart(int engineRate) {
        for (Voice& v : voices_) if (v.active && v.loop) v.active = false;
        if (!sample_ || sample_->frames() < 2 || patch_.mode != 2) return;
        double a, b; Region(a, b);
        StartVoice(a, b, 0.0, 0.9f, engineRate, true, FitFactor(a, b));
    }
    // HEAR on a LOOP sample: one pass at the project tempo.
    void LoopPreview(int engineRate) {
        if (!sample_ || sample_->frames() < 2) return;
        double a, b; Region(a, b);
        StartVoice(a, b, 0.0, 0.9f, engineRate, false, FitFactor(a, b));
    }
    bool LoopRunning() const { for (const Voice& v : voices_) if (v.active && v.loop) return true; return false; }

    // Adds `frames` of interleaved stereo to `out`.
    void Render(float* out, int frames, int engineRate) {
        const float fadeLen = std::max(16.0f, 0.004f * static_cast<float>(engineRate));
        for (Voice& v : voices_) {
            if (!v.active) continue;
            const SampleData& s = *v.sample;
            const int n = s.frames(), ch = s.channels;
            auto at = [&](int i, int c) -> float {
                i = std::clamp(i, 0, n - 1);
                return s.data[static_cast<size_t>(i) * ch + (ch == 2 ? c : 0)];
            };
            auto interp = [&](double pos, float& l, float& r) {   // Catmull-Rom between i0 and i0+1
                const int i0 = static_cast<int>(pos);
                const float f = static_cast<float>(pos - i0);
                const float c0 = at(i0 - 1, 0), c1 = at(i0, 0), c2 = at(i0 + 1, 0), c3 = at(i0 + 2, 0);
                l = c1 + 0.5f * f * (c2 - c0 + f * (2.0f * c0 - 5.0f * c1 + 4.0f * c2 - c3 + f * (3.0f * (c1 - c2) + c3 - c0)));
                if (ch == 2) {
                    const float d0 = at(i0 - 1, 1), d1 = at(i0, 1), d2 = at(i0 + 1, 1), d3 = at(i0 + 2, 1);
                    r = d1 + 0.5f * f * (d2 - d0 + f * (2.0f * d0 - 5.0f * d1 + 4.0f * d2 - d3 + f * (3.0f * (d1 - d2) + d3 - d0)));
                } else r = l;
            };
            double xf = 0.0, period = 0.0;   // a LOOP crosses the end into the beginning over xf source frames (no click at the seam)
            if (v.loop) {   // a LOOP follows START / LENGTH / TUNE / the project tempo live (once per block)
                Region(v.a, v.b);
                const double len = v.b - v.a;
                xf = std::min(kLoopFadeSec * s.sampleRate, len * 0.25);
                period = std::max(1.0, len - xf);
                // the period (not the whole length) is what must last whole bars: the speed is corrected by the part that overlaps
                const double ratio = std::pow(2.0, v.semi + (patch_.tuneCents + detuneCents_) / 1200.0) * (static_cast<double>(s.sampleRate) / std::max(1, engineRate)) * FitRate(s, patch_, bpm_, v.a, v.b) * (period / std::max(1.0, len));
                v.inc = patch_.reverse ? -ratio : ratio;
                if (v.pos < v.a || v.pos >= v.b) v.pos = patch_.reverse ? v.b - 1.0 : v.a;
            }
            for (int i = 0; i < frames; ++i) {
                if (v.loop) {
                    if (v.pos >= v.b) v.pos -= period;
                    else if (v.pos < v.a) v.pos += period;
                    if (v.pos < v.a || v.pos >= v.b) v.pos = patch_.reverse ? v.b - 1.0 : v.a;   // after a jump of START / LENGTH
                } else
                if (v.pos < v.a || v.pos >= v.b) { v.active = false; break; }
                float l, r;
                interp(v.pos, l, r);
                if (v.loop && xf > 1.0) {   // blend the last xf frames (or, reversed, the first) with the same frames one period away
                    const double into = v.inc > 0 ? v.pos - (v.b - xf) : (v.a + xf) - v.pos;
                    if (into > 0.0) {
                        const float t = static_cast<float>(std::min(1.0, into / xf));
                        float l2, r2; interp(v.inc > 0 ? v.pos - period : v.pos + period, l2, r2);
                        l = l * (1.0f - t) + l2 * t; r = r * (1.0f - t) + r2 * t;
                    }
                }
                // attack in, end out (the last few ms of the played part), quick kill when a mono hit is cut
                v.env = std::min(1.0f, v.env + v.attackStep);
                const double toEnd = v.inc > 0 ? (v.b - v.pos) : (v.pos - v.a);
                const float endFade = v.loop ? 1.0f : static_cast<float>(std::min(1.0, toEnd / std::max(1e-6, std::fabs(v.inc) * fadeLen)));
                if (v.killStep > 0.0f) { v.kill -= v.killStep; if (v.kill <= 0.0f) { v.active = false; break; } }
                const float g = v.gain * v.env * endFade * v.kill;
                out[static_cast<size_t>(i) * 2 + 0] += l * g;
                out[static_cast<size_t>(i) * 2 + 1] += r * g;
                v.pos += v.inc * pitchMul_;
            }
            if (!v.active) v.sample.reset();
        }
    }

private:
    void StartVoice(double a, double b, double semitones, float velocity, int engineRate, bool loop, double fit) {
        if (!patch_.poly && !loop) for (Voice& v : voices_) if (v.active && !v.loop) v.killStep = 1.0f / std::max(8.0f, 0.003f * static_cast<float>(engineRate));   // quick fade, not a click
        Voice* slot = nullptr;
        for (Voice& v : voices_) if (!v.active) { slot = &v; break; }
        if (!slot) {   // all busy: take the oldest that is not the loop
            for (Voice& v : voices_) if (!v.loop && (!slot || v.age > slot->age)) slot = &v;
            if (!slot) slot = &voices_[0];
        }
        for (Voice& v : voices_) if (v.active) ++v.age;
        const double ratio = std::pow(2.0, semitones + (patch_.tuneCents + detuneCents_) / 1200.0) * (static_cast<double>(sample_->sampleRate) / std::max(1, engineRate)) * fit;
        slot->semi = loop ? semitones : 0.0;
        slot->active = true; slot->age = 0; slot->killStep = 0.0f; slot->kill = 1.0f; slot->loop = loop;
        slot->a = a; slot->b = b;
        slot->inc = patch_.reverse ? -ratio : ratio;
        slot->pos = patch_.reverse ? b - 1.0 : a;
        slot->gain = std::clamp(velocity, 0.0f, 1.0f) * (patch_.gainDb == 0 ? 1.0f : std::pow(10.0f, static_cast<float>(patch_.gainDb) / 20.0f));
        slot->env = 0.0f;
        slot->attackStep = patch_.attackMs <= 0.05f ? 1.0f : 1.0f / std::max(1.0f, patch_.attackMs * 0.001f * static_cast<float>(engineRate));
        slot->sample = sample_;     // the voice keeps its own reference: a swap of the sample never frees what it is playing
    }
    struct Voice {
        bool loop = false;          // v44.52: a LOOP voice wraps at the end of the played part and follows START / LENGTH / the tempo live
        bool active = false;
        int age = 0;
        double pos = 0, inc = 1, a = 0, b = 0, semi = 0;   // semi: the pitch of a (keyed) LOOP voice in semitones, followed live with the tuning
        float gain = 1, env = 0, attackStep = 1, kill = 1, killStep = 0;
        SamplePtr sample;
    };
    SamplePtr sample_;
    SamplerPatch patch_;
    double bpm_ = 120.0;
    bool keyed_ = false;
    double pitchMul_ = 1.0, detuneCents_ = 0.0;
    std::array<Voice, kVoices> voices_;
};

} // namespace t8
