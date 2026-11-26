#pragma once
// Group editing of the notes of one Synth / Chord track (v44.30, U6): select, transpose, move in time, copy, delete, change the length of
// several notes together. A note is (column, pitch): on a Synth track one step holds one note, on a Chord track a step holds up to four.
// Everything works from a SNAPSHOT of the steps taken when the gesture began, so a drag can go back and forth and never loses a note.
#include "core/Types.h"
#include <algorithm>
#include <array>
#include <vector>

namespace t8 {

struct NoteRef {
    int col = 0;
    int pitch = 60;
    bool operator==(const NoteRef& o) const { return col == o.col && pitch == o.pitch; }
};

using StepSnapshot = std::array<Step, kMaxSteps>;

inline bool NoteIsOn(const Track& t, int col, int pitch) {
    if (col < 0 || col >= kMaxSteps) return false;
    const Step& s = t.steps[static_cast<size_t>(col)];
    if (!s.active) return false;
    if (IsNoteTrackType(t.type)) return s.note == pitch;
    for (int i = 0; i < s.chordSize; ++i) if (s.chordNotes[static_cast<size_t>(i)] == pitch) return true;
    return false;
}

// Every note of the track inside its length.
inline std::vector<NoteRef> AllNotes(const Track& t, int trackLen) {
    std::vector<NoteRef> out;
    for (int c = 0; c < trackLen && c < kMaxSteps; ++c) {
        const Step& s = t.steps[static_cast<size_t>(c)];
        if (!s.active) continue;
        if (IsNoteTrackType(t.type)) out.push_back({c, s.note});
        else for (int i = 0; i < s.chordSize; ++i) out.push_back({c, s.chordNotes[static_cast<size_t>(i)]});
    }
    return out;
}

// Keeps only the references that still point at a note (after an undo or an edit from outside).
inline void PruneNotes(const Track& t, std::vector<NoteRef>& sel) {
    sel.erase(std::remove_if(sel.begin(), sel.end(), [&](const NoteRef& n) { return !NoteIsOn(t, n.col, n.pitch); }), sel.end());
}

// The largest move that keeps every selected note inside 0..trackLen-1 and pitch 0..127.
inline void ClampNoteDelta(const std::vector<NoteRef>& sel, int trackLen, int* dCol, int* dPitch) {
    if (sel.empty()) { *dCol = 0; *dPitch = 0; return; }
    int minC = kMaxSteps, maxC = 0, minP = 128, maxP = 0;
    for (const NoteRef& n : sel) { minC = std::min(minC, n.col); maxC = std::max(maxC, n.col); minP = std::min(minP, n.pitch); maxP = std::max(maxP, n.pitch); }
    *dCol = std::clamp(*dCol, -minC, std::max(0, trackLen - 1 - maxC));
    *dPitch = std::clamp(*dPitch, -minP, 127 - maxP);
}

// Who gets the arrow keys while notes are selected in the piano roll. The roll keeps them after a click or Cmd+A in it
// ("focus"), so the selected notes still move when the mouse drifts off the roll; a click anywhere else gives them back to
// the instrument block. Nothing moves while a text box is typed in or a mouse gesture (move / band / length drag) is running.
inline bool RollOwnsKeys(bool hovered, bool focused, bool typing, bool mouseGesture) {
    return (hovered || focused) && !typing && !mouseGesture;
}

inline void RemoveNoteFromStep(Track& t, int col, int pitch) {
    Step& s = t.steps[static_cast<size_t>(col)];
    if (IsNoteTrackType(t.type)) { if (s.active && s.note == pitch) s.active = false; return; }
    int idx = -1;
    for (int i = 0; i < s.chordSize; ++i) if (s.chordNotes[static_cast<size_t>(i)] == pitch) { idx = i; break; }
    if (idx < 0) return;
    for (int i = idx; i < s.chordSize - 1; ++i) s.chordNotes[static_cast<size_t>(i)] = s.chordNotes[static_cast<size_t>(i + 1)];
    s.chordSize--;
    if (s.chordSize == 0) s.active = false;
}

inline void RemoveNotes(Track& t, const std::vector<NoteRef>& sel) {
    for (const NoteRef& n : sel) if (n.col >= 0 && n.col < kMaxSteps) RemoveNoteFromStep(t, n.col, n.pitch);
}

// Puts the selected notes (as they are in `snap`) dCol steps later and dPitch semitones higher. `copy` leaves the originals where they
// were. The delta is clamped so nothing leaves the grid. Returns the new positions (the new selection). The whole step is carried
// along (length, probability, ratchet, nudge, slide ...), so a moved note keeps everything it had.
inline std::vector<NoteRef> MoveNotes(Track& t, const StepSnapshot& snap, const std::vector<NoteRef>& sel, int dCol, int dPitch, bool copy, int trackLen) {
    ClampNoteDelta(sel, trackLen, &dCol, &dPitch);
    t.steps = snap;
    if (!copy) RemoveNotes(t, sel);
    std::vector<NoteRef> out;
    out.reserve(sel.size());
    for (const NoteRef& n : sel) {
        const int tc = n.col + dCol, tp = n.pitch + dPitch;
        const Step& src = snap[static_cast<size_t>(n.col)];
        Step& d = t.steps[static_cast<size_t>(tc)];
        if (IsNoteTrackType(t.type)) {
            d = src; d.note = static_cast<int8_t>(tp); d.active = true;
        } else {
            if (!d.active || d.chordSize == 0) { d = src; d.chordSize = 0; }
            bool have = false;
            for (int i = 0; i < d.chordSize; ++i) if (d.chordNotes[static_cast<size_t>(i)] == tp) have = true;
            if (!have && d.chordSize < kMaxChordVoices) {
                d.chordNotes[static_cast<size_t>(d.chordSize)] = static_cast<int8_t>(tp);
                d.chordSize++;
                std::sort(d.chordNotes.begin(), d.chordNotes.begin() + d.chordSize);
            }
            d.active = d.chordSize > 0;
        }
        out.push_back({tc, tp});
    }
    return out;
}

// Length (in steps) of the note's step; the same for every note of a chord step.
inline int NoteLength(const Track& t, int col) { return std::max<int>(1, t.steps[static_cast<size_t>(col)].lengthSteps); }

// Sets the length of every selected note's step to its length in `snap` + delta (at least 1, never past the end of the track).
inline void ResizeNotes(Track& t, const StepSnapshot& snap, const std::vector<NoteRef>& sel, int delta, int trackLen) {
    for (const NoteRef& n : sel) {
        const int orig = std::max<int>(1, snap[static_cast<size_t>(n.col)].lengthSteps);
        t.steps[static_cast<size_t>(n.col)].lengthSteps = static_cast<uint8_t>(std::clamp(orig + delta, 1, std::max(1, trackLen - n.col)));
    }
}

// Cmd+D: the selection again right after itself (shifted by its own span); false when it does not fit.
inline bool DuplicateNotes(Track& t, std::vector<NoteRef>& sel, int trackLen) {
    if (sel.empty()) return false;
    int minC = kMaxSteps, maxEnd = 0;
    for (const NoteRef& n : sel) { minC = std::min(minC, n.col); maxEnd = std::max(maxEnd, n.col + NoteLength(t, n.col)); }
    const int span = std::max(1, maxEnd - minC);
    int maxC = 0; for (const NoteRef& n : sel) maxC = std::max(maxC, n.col);
    if (maxC + span >= trackLen) return false;
    const StepSnapshot snap = t.steps;
    sel = MoveNotes(t, snap, sel, span, 0, true, trackLen);
    return true;
}

} // namespace t8
