#pragma once
// Live MIDI capture -- the note-event counterpart to Recorder (which taps
// AudioEngine::Render's audio output). This is deliberately NOT wired to
// any single input source: it is
// wired into AudioEngine's three trigger methods (TriggerDrum/SynthNoteOn/
// ChordNoteOn), the exact same central choke point every sound in the app
// already passes through (programmed pattern steps via Sequencer::FireTick,
// LIVE BREAK fills, ratchet retriggers via FireRatchetEvent, AND live
// note input). So capturing
// "whatever is being played in performance mode" falls out for free from
// hooking that one shared point, exactly mirroring how Recorder captures
// whatever audio actually plays regardless of what triggered it.
//
// Timing is sample-accurate, not wall-clock: Advance() is called once per
// AudioEngine::Render block with that block's actual frame count, so an
// event logged from inside Sequencer::RenderAndAdvance's per-chunk Render
// call lands at exactly the elapsed-audio-time position it played at --
// immune to UI-thread scheduling jitter, unlike the original
// std::chrono::steady_clock-based v1 implementation.
#include "Types.h"
#include <cstdint>
#include <vector>

namespace t8 {

// One captured note/chord/drum-hit event. Exactly one of isDrum / isChord /
// (synthIndex >= 0) is meaningful per event.
struct CapturedEvent {
    double timeSeconds = 0.0;
    bool isDrum = false;
    DrumSlot drumSlot = DrumSlot::Kick;
    bool isChord = false;
    int synthIndex = -1;      // 0..kNumSynthTracks-1 for a synth note
    int8_t notes[kMaxChordVoices] = {};
    int noteCount = 0;        // 1 for a synth note, up to kMaxChordVoices for a chord voicing
};

class MidiCapture {
public:
    // N1 (LIVE-9): the Log* calls run on the audio thread, so the room is made here (UI thread): about 2 hours of a busy pattern.
    // When it is full, further events are dropped (counted) instead of growing the vector on the audio thread.
    static constexpr size_t kCapacity = 200000;
    void Start() {
        events_.clear();
        events_.reserve(kCapacity);
        dropped_ = 0;
        elapsedSeconds_ = 0.0;
        active_ = true;
    }
    size_t Dropped() const { return dropped_; }
    void Stop() { active_ = false; }
    bool IsActive() const { return active_; }
    double ElapsedSeconds() const { return elapsedSeconds_; }

    // Called once per AudioEngine::Render block (any time, active or not,
    // same as Recorder::Push) so the internal clock always matches the
    // audio actually rendered.
    void Advance(int numFrames, int sampleRate) {
        if (!active_ || sampleRate <= 0) return;
        elapsedSeconds_ += static_cast<double>(numFrames) / static_cast<double>(sampleRate);
    }

    void LogDrum(DrumSlot slot) {
        if (!active_) return;
        CapturedEvent e;
        e.timeSeconds = elapsedSeconds_;
        e.isDrum = true;
        e.drumSlot = slot;
        Add(e);
    }

    void LogSynth(int synthIndex, int8_t note) {
        if (!active_) return;
        CapturedEvent e;
        e.timeSeconds = elapsedSeconds_;
        e.synthIndex = synthIndex;
        e.noteCount = 1;
        e.notes[0] = note;
        Add(e);
    }

    void LogChord(const int8_t* notes, int count) {
        if (!active_) return;
        CapturedEvent e;
        e.timeSeconds = elapsedSeconds_;
        e.isChord = true;
        e.noteCount = count < 0 ? 0 : (count > kMaxChordVoices ? kMaxChordVoices : count);   // v44.44: was 4, a six-note chord lost two notes
        for (int i = 0; i < e.noteCount; ++i) e.notes[i] = notes[i];
        Add(e);
    }

    const std::vector<CapturedEvent>& Events() const { return events_; }

private:
    void Add(const CapturedEvent& e) { if (events_.size() < events_.capacity()) events_.push_back(e); else ++dropped_; }
    size_t dropped_ = 0;
    bool active_ = false;
    double elapsedSeconds_ = 0.0;
    std::vector<CapturedEvent> events_;
};

} // namespace t8
