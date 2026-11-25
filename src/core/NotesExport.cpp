#include "NotesExport.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <functional>
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace t8 {

std::string TrackerNoteName(int midiNote) {
    static const char* names[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
    int n = ((midiNote % 12) + 12) % 12;
    int octave = midiNote / 12 - 1;
    return std::string(names[n]) + std::to_string(octave);
}

std::string DrumAbbrev(DrumSlot slot) {
    switch (slot) {
        case DrumSlot::Kick:      return "k";
        case DrumSlot::Snare:     return "s";
        case DrumSlot::Clap:      return "c";
        case DrumSlot::ClosedHat: return "hc";
        case DrumSlot::OpenHat:   return "ho";
        case DrumSlot::Perc:      return "p";
    }
    return "?";
}

namespace {

constexpr int kStepsPerRow = 16;
// Fixed-width row-label column so every step-number header and note row --
// across every chunk and every track in the file -- lines its numbered
// columns up under each other. Widest label that appears is the shared
// DRUMS grid's "HAT CLOSED" row label (10 chars); 11 leaves it at least
// one space of breathing room before the first cell rather than butting
// up against it (a range label like "[17-32]", 7 chars, has plenty).
constexpr size_t kPrefixWidth = 11;

std::string PadRight(const std::string& s, size_t width) {
    return s.size() >= width ? s : s + std::string(width - s.size(), ' ');
}

// One track's cell content for step `idx`, or "---" if that step doesn't
// sound. A Chord step can carry up to 4 simultaneous notes, joined with
// "+" (in Step::chordNotes' own ascending order) -- Synth 1/2/3 steps only
// ever carry one. A Drum step has no pitch at all -- it just hits its
// track's fixed slot -- so it prints that slot's short abbreviation
// (DrumAbbrev) instead of a note name whenever it's active.
std::string CellFor(const Track& t, int idx) {
    const Step& s = t.steps[static_cast<size_t>(idx)];
    if (t.type == TrackType::Drum) {
        if (!s.active) return "---";
        return DrumAbbrev(static_cast<DrumSlot>(t.slotIndex));
    }
    if (t.type == TrackType::Chord) {
        if (!s.active || s.chordSize == 0) return "---";
        std::string out;
        for (int i = 0; i < s.chordSize && i < kMaxChordVoices; ++i) {
            if (i > 0) out += "+";
            out += TrackerNoteName(s.chordNotes[static_cast<size_t>(i)]);
        }
        return out;
    }
    if (!s.active) return "---";
    return TrackerNoteName(s.note);
}

int EffectiveLength(const Track& t, const Pattern& p) {
    int len = t.useOwnLength ? t.length : p.masterLength;
    return std::clamp(len, 1, kMaxSteps);
}

using CellFn = std::function<std::string(int)>;

// Appends one track's note grid -- a label line, then a step-number header
// and a note row per 16-step chunk (a track longer than 16 steps wraps
// into consecutive chunks rather than overflowing one line or silently
// truncating). `len`/`cellFn` are passed separately from a concrete Track
// so this same renderer also serves the dynamically-sized MIDI-capture
// export below, which has no Track/Step array to read from.
void AppendSingleTrackGrid(std::ostringstream& out, const std::string& label, int len,
                            bool ownLengthNote, const CellFn& cellFn, size_t cellWidth) {
    out << label << "  (" << len << (ownLengthNote ? " steps, own length)\n" : " steps)\n");
    for (int chunkStart = 0; chunkStart < len; chunkStart += kStepsPerRow) {
        int chunkEnd = std::min(chunkStart + kStepsPerRow, len);

        // Left-align step numbers (PadRight) to match the note cells' own
        // left alignment below -- right-aligning them (PadLeft) looked fine
        // at the narrow Synth cellWidth=4, but at Chord's wide cellWidth=15
        // it pushed each number to the far right of a mostly-empty column,
        // reading as sparse/misaligned against the note text underneath.
        out << PadRight("STEP", kPrefixWidth);
        for (int i = chunkStart; i < chunkEnd; ++i) {
            out << PadRight(std::to_string(i + 1), cellWidth) << " ";
        }
        out << "\n";

        std::string rangeLabel = "[" + std::to_string(chunkStart + 1) + "-" + std::to_string(chunkEnd) + "]";
        out << PadRight(rangeLabel, kPrefixWidth);
        for (int i = chunkStart; i < chunkEnd; ++i) {
            out << PadRight(cellFn(i), cellWidth) << " ";
        }
        out << "\n";
    }
    out << "\n";
}

// One row in a shared-header multi-track grid (see AppendMultiTrackGrid).
struct GridRow {
    std::string label;
    int len;
    CellFn cellFn;
};

// Appends a grid shared by several rows under ONE step-number header --
// used to unify the 6 drum tracks into a single grid instead of 6 separate
// per-track blocks: a drum row has no pitch, so its label (KICK/SNARE/...)
// already identifies it, making a repeated header/block per track pure
// repetition. Sized to the widest row's length; a shorter row shows "..."
// (rather than "---", which means "in range but silent") past its own
// length once another row in the group runs longer.
void AppendMultiTrackGrid(std::ostringstream& out, const std::string& groupLabel,
                           const std::vector<GridRow>& rows, size_t cellWidth) {
    int maxLen = 1;
    for (const auto& r : rows) maxLen = std::max(maxLen, r.len);
    out << groupLabel << "  (" << maxLen << " steps)\n";
    for (int chunkStart = 0; chunkStart < maxLen; chunkStart += kStepsPerRow) {
        int chunkEnd = std::min(chunkStart + kStepsPerRow, maxLen);

        out << PadRight("STEP", kPrefixWidth);
        for (int i = chunkStart; i < chunkEnd; ++i) {
            out << PadRight(std::to_string(i + 1), cellWidth) << " ";
        }
        out << "\n";

        for (const auto& r : rows) {
            out << PadRight(r.label, kPrefixWidth);
            for (int i = chunkStart; i < chunkEnd; ++i) {
                std::string cell = (i < r.len) ? r.cellFn(i) : "...";
                out << PadRight(cell, cellWidth) << " ";
            }
            out << "\n";
        }
    }
    out << "\n";
}

} // namespace

bool ExportPatternNotesText(const Pattern& pattern, const std::string& path) {
    // Split the containing directory off `path` and create it if missing --
    // same mkdir-if-needed convention as Recorder::Toggle's recordings/
    // folder (see Recorder.cpp), so a fresh install doesn't need the folder
    // pre-created.
    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        std::string dir = path.substr(0, slash);
        struct stat st{};
        if (::stat(dir.c_str(), &st) != 0) ::mkdir(dir.c_str(), 0755);
    }

    std::ostringstream out;
    out << "TR-Brick DAW -- Note Export\n";
    out << "Pattern: " << pattern.name << "   BPM " << static_cast<int>(pattern.bpm)
        << "   Master length " << pattern.masterLength << " steps\n";
    out << "Melodic notes are pitch-class + octave (C-4, C#4, D-4, ...); drum hits are "
           "k=kick s=snare c=clap hc=hat closed ho=hat open p=perc -- \"---\" means that step is silent.\n";
    out << "A track longer than 16 steps wraps into consecutive 16-step rows below; in the DRUMS "
           "grid, \"...\" marks a drum whose own (shorter) length has already ended for that step.\n\n";

    // All 6 drum tracks share ONE grid (Kick/Snare/Clap/Closed Hat/Open
    // Hat/Perc, same order as pattern.tracks[0..5]/DrumSlot) -- a drum row
    // has no pitch, just a fixed slot, so its label alone already
    // identifies it and a separate header per track would just repeat.
    static const char* kDrumLabels[kNumDrumTracks] = {
        "KICK      ", "SNARE     ", "CLAP      ", "HAT CLOSED", "HAT OPEN  ", "PERC      "
    };
    std::vector<GridRow> drumRows;
    for (int i = 0; i < kNumDrumTracks; ++i) {
        const Track* t = &pattern.tracks[i];
        drumRows.push_back(GridRow{kDrumLabels[i], EffectiveLength(*t, pattern),
                                    [t](int idx) { return CellFor(*t, idx); }});
    }
    AppendMultiTrackGrid(out, "DRUMS     ", drumRows, 4);

    // Then the 4 melodic tracks (Synth 1/2/3 + Chord), each still its own
    // block. Chord's cells can run up to 4 notes joined with "+" (see
    // CellFor), hence its wider column.
    static const char* kSynthLabels[kNumSynthTracks] = {"SYNTH 1   ", "SYNTH 2   ", "SYNTH 3   "};
    for (int s = 0; s < kNumSynthTracks; ++s) {
        const Track* t = &pattern.tracks[kNumDrumTracks + s];
        AppendSingleTrackGrid(out, kSynthLabels[s], EffectiveLength(*t, pattern), t->useOwnLength,
                               [t](int idx) { return CellFor(*t, idx); }, 4);
    }
    {
        const Track* t = &pattern.tracks[kNumDrumTracks + kNumSynthTracks];
        AppendSingleTrackGrid(out, "CHORD     ", EffectiveLength(*t, pattern), t->useOwnLength,
                               [t](int idx) { return CellFor(*t, idx); }, 15);
    }

    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return false;
    f << out.str();
    return f.good();
}

bool ExportMidiCaptureText(const MidiCapture& capture, float bpm, const std::string& path) {
    const auto& events = capture.Events();
    if (events.empty()) return false;

    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        std::string dir = path.substr(0, slash);
        struct stat st{};
        if (::stat(dir.c_str(), &st) != 0) ::mkdir(dir.c_str(), 0755);
    }

    // Quantize every captured event's sample-accurate timestamp to a
    // 16th-note step at `bpm`, same convention TrackerNoteName's caller
    // (CellFor) and the sequencer itself use -- but with NO kMaxSteps
    // ceiling: the grid grows to fit however long the performance actually
    // ran, which is the entire point of capturing into a dynamic event
    // list instead of a fixed-size Pattern buffer.
    double secondsPerStep = 60.0 / std::max(20.0, static_cast<double>(bpm)) / 4.0;
    int totalSteps = 0;
    for (const auto& e : events) {
        int idx = static_cast<int>(std::lround(e.timeSeconds / secondsPerStep));
        totalSteps = std::max(totalSteps, idx + 1);
    }

    std::vector<std::string> drumCells[kNumDrumTracks];
    std::vector<std::string> synthCells[kNumSynthTracks];
    std::vector<std::string> chordCells;
    for (int i = 0; i < kNumDrumTracks; ++i) drumCells[i].assign(static_cast<size_t>(totalSteps), "---");
    for (int i = 0; i < kNumSynthTracks; ++i) synthCells[i].assign(static_cast<size_t>(totalSteps), "---");
    chordCells.assign(static_cast<size_t>(totalSteps), "---");

    for (const auto& e : events) {
        int idx = static_cast<int>(std::lround(e.timeSeconds / secondsPerStep));
        if (idx < 0 || idx >= totalSteps) continue;
        size_t sidx = static_cast<size_t>(idx);
        if (e.isDrum) {
            drumCells[static_cast<int>(e.drumSlot)][sidx] = DrumAbbrev(e.drumSlot);
        } else if (e.isChord) {
            std::string cell;
            for (int i = 0; i < e.noteCount && i < kMaxChordVoices; ++i) {
                if (i > 0) cell += "+";
                cell += TrackerNoteName(e.notes[i]);
            }
            if (!cell.empty()) chordCells[sidx] = cell;
        } else if (e.synthIndex >= 0 && e.synthIndex < kNumSynthTracks && e.noteCount > 0) {
            synthCells[e.synthIndex][sidx] = TrackerNoteName(e.notes[0]);
        }
    }

    std::ostringstream out;
    out << "TR-Brick DAW -- Live MIDI Capture\n";
    out << "BPM " << static_cast<int>(bpm) << "   Captured length " << totalSteps << " steps\n";
    out << "This is everything that actually played during the captured performance -- "
           "programmed steps, LIVE BREAK fills, ratchets, and live NOTE PLAY notes alike -- "
           "quantized to 16th-note steps, same layout as EXPORT NOTES.\n";
    out << "Melodic notes are pitch-class + octave (C-4, C#4, ...); drum hits are "
           "k=kick s=snare c=clap hc=hat closed ho=hat open p=perc -- \"---\" means that step is silent.\n\n";

    static const char* kDrumLabels[kNumDrumTracks] = {
        "KICK      ", "SNARE     ", "CLAP      ", "HAT CLOSED", "HAT OPEN  ", "PERC      "
    };
    std::vector<GridRow> drumRows;
    for (int i = 0; i < kNumDrumTracks; ++i) {
        drumRows.push_back(GridRow{kDrumLabels[i], totalSteps,
                                    [&drumCells, i](int idx) { return drumCells[i][static_cast<size_t>(idx)]; }});
    }
    AppendMultiTrackGrid(out, "DRUMS     ", drumRows, 4);

    static const char* kSynthLabels[kNumSynthTracks] = {"SYNTH 1   ", "SYNTH 2   ", "SYNTH 3   "};
    for (int s = 0; s < kNumSynthTracks; ++s) {
        AppendSingleTrackGrid(out, kSynthLabels[s], totalSteps, false,
                               [&synthCells, s](int idx) { return synthCells[s][static_cast<size_t>(idx)]; }, 4);
    }
    AppendSingleTrackGrid(out, "CHORD     ", totalSteps, false,
                           [&chordCells](int idx) { return chordCells[static_cast<size_t>(idx)]; }, 15);

    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return false;
    f << out.str();
    return f.good();
}

} // namespace t8


namespace t8 {

namespace {

void PutVlq(std::vector<uint8_t>& v, uint32_t value) {
    uint8_t buf[5];
    int n = 0;
    buf[n++] = static_cast<uint8_t>(value & 0x7F);
    while ((value >>= 7) > 0) buf[n++] = static_cast<uint8_t>((value & 0x7F) | 0x80);
    while (n > 0) v.push_back(buf[--n]);
}
void Put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24)); v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));  v.push_back(static_cast<uint8_t>(x));
}
void Put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x >> 8)); v.push_back(static_cast<uint8_t>(x));
}

struct MidiEv { uint32_t tick; int order; uint8_t status, d1, d2; };

void AppendTrack(std::vector<uint8_t>& file, std::vector<MidiEv> evs, const char* name, bool tempo, uint32_t microsPerQuarter) {
    std::stable_sort(evs.begin(), evs.end(), [](const MidiEv& a, const MidiEv& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        return a.order < b.order;   // note-offs before note-ons at the same tick
    });
    std::vector<uint8_t> t;
    PutVlq(t, 0); t.push_back(0xFF); t.push_back(0x03);
    PutVlq(t, static_cast<uint32_t>(std::strlen(name)));
    for (const char* c = name; *c; ++c) t.push_back(static_cast<uint8_t>(*c));
    if (tempo) {
        PutVlq(t, 0); t.push_back(0xFF); t.push_back(0x51); t.push_back(0x03);
        t.push_back(static_cast<uint8_t>(microsPerQuarter >> 16));
        t.push_back(static_cast<uint8_t>(microsPerQuarter >> 8));
        t.push_back(static_cast<uint8_t>(microsPerQuarter));
    }
    uint32_t last = 0;
    for (const auto& e : evs) {
        PutVlq(t, e.tick - last);
        last = e.tick;
        t.push_back(e.status); t.push_back(e.d1); t.push_back(e.d2);
    }
    PutVlq(t, 0); t.push_back(0xFF); t.push_back(0x2F); t.push_back(0x00);
    file.push_back('M'); file.push_back('T'); file.push_back('r'); file.push_back('k');
    Put32(file, static_cast<uint32_t>(t.size()));
    file.insert(file.end(), t.begin(), t.end());
}

} // namespace

bool ExportMidiCaptureSmf(const MidiCapture& capture, float bpm, const std::string& path) {
    const auto& events = capture.Events();
    if (events.empty()) return false;
    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        std::string dir = path.substr(0, slash);
        struct stat st{};
        if (::stat(dir.c_str(), &st) != 0) ::mkdir(dir.c_str(), 0755);
    }
    constexpr int kPpq = 480;
    const double safeBpm = std::max(20.0, static_cast<double>(bpm));
    auto toTick = [&](double seconds) { return static_cast<uint32_t>(std::max(0.0, std::lround(seconds * safeBpm / 60.0 * kPpq) * 1.0)); };
    const uint32_t noteLen = kPpq / 4; // one 16th-note step

    static const uint8_t kDrumNote[kNumDrumTracks] = {36, 38, 39, 42, 46, 56};
    std::vector<MidiEv> drums, synths[kNumSynthTracks], chord;
    for (const auto& e : events) {
        uint32_t on = toTick(e.timeSeconds);
        if (e.isDrum) {
            uint8_t n = kDrumNote[static_cast<int>(e.drumSlot) % kNumDrumTracks];
            drums.push_back({on, 1, 0x99, n, 100});
            drums.push_back({on + noteLen, 0, 0x89, n, 0});
        } else if (e.isChord) {
            for (int i = 0; i < e.noteCount && i < kMaxChordVoices; ++i) {
                uint8_t n = static_cast<uint8_t>(std::clamp<int>(e.notes[i], 0, 127));
                chord.push_back({on, 1, 0x94, n, 90});
                chord.push_back({on + noteLen, 0, 0x84, n, 0});
            }
        } else if (e.synthIndex >= 0 && e.synthIndex < kNumSynthTracks && e.noteCount > 0) {
            uint8_t ch = static_cast<uint8_t>(e.synthIndex);
            uint8_t n = static_cast<uint8_t>(std::clamp<int>(e.notes[0], 0, 127));
            synths[e.synthIndex].push_back({on, 1, static_cast<uint8_t>(0x90 | ch), n, 100});
            synths[e.synthIndex].push_back({on + noteLen, 0, static_cast<uint8_t>(0x80 | ch), n, 0});
        }
    }

    std::vector<uint8_t> file;
    file.push_back('M'); file.push_back('T'); file.push_back('h'); file.push_back('d');
    Put32(file, 6);
    Put16(file, 1);
    Put16(file, static_cast<uint16_t>(3 + kNumSynthTracks)); // tempo + drums + synths + chord
    Put16(file, kPpq);
    AppendTrack(file, {}, "TR-Brick capture", true, static_cast<uint32_t>(60000000.0 / safeBpm));
    AppendTrack(file, drums, "Drums", false, 0);
    static const char* kSynthTrackNames[kNumSynthTracks] = {"Bass", "Synth 2", "Synth 3"};
    for (int i = 0; i < kNumSynthTracks; ++i) AppendTrack(file, synths[i], kSynthTrackNames[i], false, 0);
    AppendTrack(file, chord, "Chord", false, 0);

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    return f.good();
}

} // namespace t8
