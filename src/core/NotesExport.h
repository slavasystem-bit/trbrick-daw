#pragma once
// Exports the current pattern's melodic note content as a plain-text,
// tracker-style note-name grid -- for reading a pattern's pitches away
// from the app (in a text editor, pasted into a chat, printed out), not
// for reloading (see Persistence.h for the actual save/load format).
#include "Types.h"
#include "MidiCapture.h"
#include <string>

namespace t8 {

// Converts a MIDI note number to a classic tracker-style 3-character note
// name: the pitch-class letter, then "-" for a natural or "#" for a
// sharp, then the octave digit -- e.g. 60 -> "C-4", 61 -> "C#4",
// 62 -> "D-4". Octave numbering matches PianoRollView::NoteName's existing
// on-screen convention (midiNote/12 - 1, so 60 = C4); this just formats it
// fixed-width for a grid instead of PianoRollView's "C4"/"C#4" label.
std::string TrackerNoteName(int midiNote);

// Short lowercase code for a drum slot's step cell -- k/s/c/hc/ho/p for
// Kick/Snare/Clap/Closed Hat/Open Hat/Perc. A drum step has no pitch (it
// just hits its track's fixed slot), so this stands in for
// TrackerNoteName in the drum rows of ExportPatternNotesText's grid below.
std::string DrumAbbrev(DrumSlot slot);

// Renders `pattern`'s 6 drum tracks as ONE shared grid (a single step-
// number header with all 6 drum rows -- Kick/Snare/Clap/Closed Hat/Open
// Hat/Perc, using DrumAbbrev -- stacked underneath it, rather than a
// separate header per track, since a drum row's label alone already
// identifies it) followed by its 4 melodic tracks (Synth 1/2/3 + Chord,
// each still its own block, using TrackerNoteName) as plain-text 16-step-
// wide grids, and writes it to `path`, creating the containing directory
// if needed. A track longer than 16 steps (its own `length` if
// useOwnLength, else the pattern's masterLength) wraps into consecutive
// 16-step rows rather than truncating or overflowing one line; in the
// shared drum grid, a row whose own length is shorter than the widest
// drum track in the group shows "..." (instead of "---") past its own
// length. Returns false only on a file-write failure.
bool ExportPatternNotesText(const Pattern& pattern, const std::string& path);

// Renders a live MIDI capture session (see AudioEngine::Capture()) in the
// same grid layout as ExportPatternNotesText -- one shared grid for the 6
// drum tracks, then a block per melodic track (Synth 1/2/3 + Chord) --
// but sized to however long the captured performance actually ran rather
// than being bounded to a single pattern's length: `capture`'s events are
// quantized to 16th-note steps at `bpm` and the grid grows to fit whatever
// step the last captured event landed on. Returns false if `capture` has
// no events, or on a file-write failure.
bool ExportMidiCaptureText(const MidiCapture& capture, float bpm, const std::string& path);

// Writes the same capture as a standard MIDI file (format 1, 480 PPQ): a
// tempo track, a drum track on channel 10 (GM drum map), and one channel per
// synth plus one for the chord track. The engine only records note-ONs, so
// every note is given a fixed one-step (16th) length. Returns false if there
// is nothing to write or the file cannot be created.
bool ExportMidiCaptureSmf(const MidiCapture& capture, float bpm, const std::string& path);

} // namespace t8
