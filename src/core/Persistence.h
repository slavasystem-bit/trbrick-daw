#pragma once
// Save/load a Project as a small human-readable text format (easy to diff,
// back up, or hand-edit on the SD card). See Persistence.cpp for the
// format description.
#include "Project.h"
#include <string>
#include <vector>

namespace t8 {

// File format version written as "format=N" under [project]. 1 = files from
// before the key existed; 2 adds the trailing [end] completeness marker.
constexpr int kProjectFormatVersion = 13;   // 13: sidechain pumps (pump*N= in [fx]; an older program would drop them and play its one Kick -> Bass sidechain off, so it keeps the file and saves a copy); 12: C2, the PLAY layer of the chord track (chordPlay=); 11: C4b (e), a kit may live in the project's own folder (kitFolder / trackKit = "samples/kits/<kit>"; an older program would look for it beside itself and lose the drums, so it keeps the file and saves a copy); 10: C4b, a fill may last half a beat (fill beats 0; an older program would read 1 beat, so it keeps the file and saves a copy); 9: C4b, the SAMPLE fill (fillSample=, fsmN=); 8: v44.59, keys added in v44.51-v44.58 (sceneLink, euclid, sedit, sample, keytrack ...); every new key bumps it (selftest checks); 3: v44.5.4, the sound (patches, kick, kit) is saved per scene; 4: v44.21, dynamics lanes with free points (dyn / lv2); 5: v44.22, every brick has its own scene slot (pattern16..79); 7: v44.50, the sampler is the 11th track (sampler= per scene; the kit part of ownSound moved from bit 10 to bit 11); 6: v44.24, a brick can have its own sound for some instruments (ownSound), the sound of the track holds whole channels

// Crash-safe: writes <path>.tmp, fsyncs, renames over <path>, keeps the old
// file as <path>.bak.
// keepHistory = false writes a plain copy (the recovery file of the open project): no dated backup, no .bak.
bool SaveProject(const Project& project, const std::string& path, bool keepHistory = true);
// The text SaveProject would write (used to tell whether the open project differs from the saved one).
std::string ProjectToString(const Project& project);
// Never throws. A malformed / cut-short file falls back to <path>.bak (see
// LastLoadNote() for a human-readable "RECOVERED ..." message). On success a
// dated copy is also written to backups/ (BackupProjectFile).
bool LoadProject(Project& project, const std::string& path);
const std::string& LastLoadNote();
// After LoadProject: the format number of a file written by a NEWER version (0 = not newer). Such a file may hold data this
// version does not know; saving over it would drop that data, so the app saves a copy instead (DATA-1).
int LastLoadNewerFormat();
// After LoadProject: how many damaged values (out of range, NaN) the load had to put back into range (DATA-3).
int LastLoadFixedValues();
// Writes `bytes` to <dest>.tmp, flushes to the disk, renames over `dest` (never half a file).
bool WriteFileAtomic(const std::string& dest, const std::string& bytes);
// Plain-text dump of one instrument's sound (synth patch / drum voice + kick synth, track mixer / EQ / sends / steps, the
// effects) -- the COPY PARAMS button puts it on the clipboard so it can be pasted into a question.
std::string DescribeInstrument(const Project& project, const Pattern& pattern, int trackIndex);
// The text of one part of a scene's sound (see SoundParts.h): two scenes sound the same in that part when the texts are equal.
std::string SoundPartText(const Pattern& pattern, int part);
bool BackupProjectFile(const std::string& path, int keep = 10);
// All backups/*.t8proj next to `path`, newest first.
std::vector<std::string> ListProjectBackups(const std::string& path);
// The folder of the dated copies of `projectPath` (shared by the projects of one service folder; legacy: backups/ beside it).
std::string ProjectBackupDir(const std::string& projectPath);
// "2026-11-25  14:03:12" for a backup file name (what the Versions list shows).
std::string BackupLabel(const std::string& backupPath);

} // namespace t8
