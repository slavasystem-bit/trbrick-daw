#pragma once
// Where the user's own files live (v44.14, U8a). One visible folder in Documents for what the user makes (projects, recordings),
// one service folder for what the program keeps for itself (dated backups of projects). The program, the build and the factory
// kits stay wherever the program is installed; nothing here depends on the current directory.
//
//   ~/Documents/TR-Brick/Projects/     *.t8proj
//   ~/Documents/TR-Brick/Recordings/   "<project> 2026-10-26 14.32.wav", "... stems.zip", "... capture.mid", "... notes.txt"
//   ~/Library/Application Support/TR-Brick/Backups/   dated copies (Linux: ~/.local/share/TR-Brick/Backups)
//
// If the folders cannot be created (read-only home, sandbox) the old relative folders are used ("projects", "recordings",
// backups next to the project file), so the program never loses the ability to save.
#include <string>

namespace t8::userdirs {

// Test / packaging switches: TRB_DATA_DIR (instead of ~/Documents/TR-Brick), TRB_SERVICE_DIR (instead of Application Support).
// SetRootsForTest overrides both and re-runs EnsureDirs.
void SetRootsForTest(const std::string& dataRoot, const std::string& serviceRoot);

// Creates the folders (once). Returns false when it had to fall back to the legacy relative folders.
bool EnsureDirs();
bool InLegacyMode();

std::string Root();            // data root (visible)
std::string ProjectsDir();     // never ends with '/'
std::string RecordingsDir();
std::string ServiceDir();
// The library: what the user makes for reuse (saved beats, synth presets, favourites). Visible, next to the projects, so it
// survives program updates and can be copied to another computer. Kits stay with the program for now.
//   ~/Documents/TR-Brick/Library/Beats/       *.t8pat
//   ~/Documents/TR-Brick/Library/Presets/     *.t8syn
//   ~/Documents/TR-Brick/Library/Favorites/   favorites*.txt (the stars of the browsers)
std::string LibraryDir();
std::string BeatsDir();
std::string SynthPresetsDir();
std::string FavoritesDir();
std::string BackupsDir();      // empty in legacy mode: backups then go next to the project file

// The document that has not been saved under a name yet ("Untitled"): it lives in the service folder until Save gives it a name,
// so the Projects folder only ever holds projects the user saved. The recovery copy of a named project (written while it has
// unsaved changes, removed by Save) sits next to it.
std::string UntitledPath();                          // <service>/Untitled/Untitled.t8proj
bool IsUntitledPath(const std::string& path);
std::string RecoveryPathFor(const std::string& projectPath);   // <service>/Recovery/<project>.t8proj

// Name of the open project (file name without extension), used in the names of recordings and exports.
void SetProjectName(const std::string& name);
const std::string& ProjectName();
// "a/b/Нічна зміна.t8proj" -> "Нічна зміна"
std::string ProjectDisplayName(const std::string& path);

// A unique path "<dir>/<project> 2026-10-26 14.32<suffix>.<ext>" (adds " 2", " 3" ... when the file exists). The folder is created.
std::string StampedPath(const std::string& dir, const std::string& project, const std::string& suffix, const std::string& ext,
                        long long unixTime = 0);
// The same inside RecordingsDir() for the open project.
std::string RecordingPath(const std::string& suffix, const std::string& ext);
// N1: a new folder "<Recordings>/<project> 2026-10-26 14.32<suffix>" (" 2", " 3" ... when taken), created; "" if it cannot be made.
std::string RecordingFolder(const std::string& suffix);

// "Recordings/Name 2026-10-26 14.32.wav": the path as shown in the status line (relative to the data root, else unchanged).
std::string DisplayPath(const std::string& path);
// The last file a recording or export wrote ("" if none): the target of SHOW IN FINDER.
void SetLastOutput(const std::string& path);
const std::string& LastOutput();

// File names the user can type: keeps letters of any alphabet (UTF-8), digits, space, - _ . ( ); drops separators, control
// characters and a leading dot; falls back to `fallback` when nothing is left.
std::string SanitizeName(const std::string& raw, const char* fallback);

// "/Users/me/Documents/TR-Brick" -> "~/Documents/TR-Brick" (for display).
std::string Tilde(const std::string& path);

// Shows `path` in Finder (macOS: open -R) or the file manager (Linux: opens the folder). A folder is simply opened.
// Fire and forget; returns false if the helper could not be started.
bool Reveal(const std::string& path);

struct MigrationResult {
    bool ran = false;          // false: already done earlier (marker found) or nothing to look at
    int projects = 0, recordings = 0, backups = 0;
};
// One-time, non-destructive copy of what older versions kept next to the program in patterns/ and presets/ (beats, synth presets,
// favourites) into the Library folders. Has its own marker (Library/.migrated), so it also runs for people who migrated in v44.14.
struct LibraryMigration { bool ran = false; int beats = 0, presets = 0, favorites = 0; };
LibraryMigration MigrateLibrary(const std::string& legacyDir);
// One-time, non-destructive copy of what older versions kept next to the program (project.t8proj, projects/, recordings/,
// exports/, backups/) into the folders above. Nothing is deleted or overwritten; recordings get readable names.
MigrationResult MigrateLegacy(const std::string& legacyDir);

} // namespace t8::userdirs
