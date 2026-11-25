#pragma once
// Phase 6: a small polyphonic voice pool for the dedicated Chord track --
// up to kMaxChordVoices independent SynthVoice instances sharing ONE
// SynthPatch (so the Chord track gets the exact same 3-engine choice +
// filter/ADSR/LFO every other synth voice already has), fired/released
// together as a unit. This is the ONE place in the engine that plays more
// than one note at once from a single track -- confined entirely to this
// class rather than retrofitting the 3 monophonic synth tracks' NoteOn/
// NoteOff/Step model (which the Piano Roll, the melodic randomizers, and
// Sequencer's synthNoteOffScheduled_ bookkeeping all assume is exactly
// one note per track). Same header-only style as SynthVoice.h, which this
// wraps -- each internal voice ends up rendering via SynthVoice::Render's
// existing `+=` accumulate convention, so summing kMaxChordVoices of them
// into one buffer is just calling Render() on each in turn.
#include "SynthVoice.h"
#include "Types.h"
#include <algorithm>
#include <array>
#include <cstdint>

namespace t8 {

// kMaxChordVoices lives in Types.h (a Step carries that many chord tones).

class ChordVoice {
public:
    // Six chord tones x unison copies: a chord uses at most this many copies per tone.
    static constexpr int kMaxChordUnison = 8;
    void SetPatch(const SynthPatch& p) {
        patch_ = p;
        SynthPatch q = p;
        q.unisonVoices = std::min(q.unisonVoices, kMaxChordUnison);
        for (auto& v : voices_) v.SetPatch(q);
    }
    const SynthPatch& Patch() const { return patch_; }
    void SetMod(float cutoffOct, float resAdd) { for (auto& v : voices_) v.SetMod(cutoffOct, resAdd); }   // v44.38
    // v44.53: the sample / tempo of the SAMPLE engine, for every chord tone
    void SetSample(const SamplePtr& s) { for (auto& v : voices_) v.SetSample(s); }
    const SamplePtr& Sample() const { return voices_[0].Sample(); }
    void SetTempo(double bpm) { for (auto& v : voices_) v.SetTempo(bpm); }

    // Fires up to kMaxChordVoices simultaneous notes -- `count` (clamped
    // to 0..kMaxChordVoices) of `notes[]` sound together, one per voice,
    // all sharing this same velocity/slide (a chord's tones don't
    // meaningfully differ in accent/glide from each other in this engine).
    void NoteOn(const int8_t* notes, int count, float velocity, bool slide) {
        count = std::clamp(count, 0, kMaxChordVoices);
        for (int i = 0; i < count; ++i) {
            voices_[i].NoteOn(notes[i], velocity, slide);
        }
    }
    // Releases every voice in the pool -- same "always release everything,
    // even voices that might already be idle" simplicity as SynthVoice::
    // NoteOff, cheap because each engine's own NoteOff is a no-op when
    // already idle.
    void NoteOff() {
        for (auto& v : voices_) v.NoteOff();
    }
    void Render(float* out, int numFrames, int sampleRate) {
        for (auto& v : voices_) v.Render(out, numFrames, sampleRate);
    }
    bool IsActive() const {
        for (const auto& v : voices_) {
            if (v.IsActive()) return true;
        }
        return false;
    }

private:
    SynthPatch patch_{};
    std::array<SynthVoice, kMaxChordVoices> voices_;
};

} // namespace t8
