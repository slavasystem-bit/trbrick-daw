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
constexpr int kProjectFormatVersion = 7;   // 3: v44.5.4, the sound (patches, kick, kit) is saved per scene; 4: v44.21, dynamics lanes with free points (dyn / lv2); 5: v44.22, every brick has its own scene slot (pattern16..79); 7: v44.50, the sampler is the 11th track (sampler= per scene; the kit part of ownSound moved from bit 10 to bit 11); 6: v44.24, a brick can have its own sound for some instruments (ownSound), the sound of the track holds whole channels

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
// Plain-text dump of one instrument's sound (synth patch / drum voice + kick synth, track mixer / EQ / sends / steps, the
// effects) -- the COPY PARAMS button puts it on the clipboard so it can be pasted into a question.
std::string DescribeInstrument(const Project& project, const Pattern& pattern, int trackIndex);
// The text of one part of a scene's sound (see SoundParts.h): two scenes sound the same in that part when the texts are equal.
std::string SoundPartText(const Pattern& pattern, int part);
bool BackupProjectFile(const std::string& path, int keep = 10);
// All backups/*.t8proj next to `path`, newest first.
std::vector<std::string> ListProjectBackups(const std::string& path);

} // namespace t8
