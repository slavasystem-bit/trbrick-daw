#pragma once
// Stage C4b (g2): MOVE UNUSED TO TRASH... in the SAMPLES browser (THIS PROJECT). Approved mockup docs/mockups/v45_samples_unified.png,
// section 2a: the copies in samples/ beside the project that nothing plays go to the Trash, after a confirmation. A file is kept when
//   - anything in memory names it: any scene or brick slot (also the ones not in use), any Undo / Redo step;
//   - any saved file that could open it names it: every project file in the project's folder (they share samples/), their .bak, the
//     dated copies in the backups folder, the recovery copies, the user's synth presets (.t8syn).
// The saved files are only searched for the file name as text (a name inside another name also keeps it): a file is rather kept than
// lost. Reading those files and moving to the Trash run on the loader thread, never under the scene lock.
//
// The Trash: macOS ~/.Trash (the file can be dragged back from there); elsewhere the freedesktop Trash (~/.local/share/Trash, with
// the .trashinfo file so "Restore" works). A file on another disk than the Trash is not moved (rename only, never copy + delete).
#include "core/Persistence.h"
#include "core/UserDirs.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace t8::cleanup {

inline bool EndsWith(const std::string& s, const std::string& suf) { return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0; }

// Every saved file that may name a sample of the samples/ folder beside `projectPath`.
inline std::vector<std::string> FilesThatMayNameSamples(const std::string& projectPath) {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    auto addDir = [&](const std::string& dir, const char* ext1, const char* ext2) {
        std::error_code ec;
        if (dir.empty() || !fs::is_directory(dir, ec)) return;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file(ec)) continue;
            const std::string n = e.path().filename().string();
            if (EndsWith(n, ext1) || (ext2 && EndsWith(n, ext2))) out.push_back(e.path().string());
        }
    };
    const fs::path p(projectPath);
    const std::string folder = p.has_parent_path() ? p.parent_path().string() : std::string(".");
    addDir(folder, ".t8proj", ".t8proj.bak");                                        // every project sharing this samples/ folder
    addDir(ProjectBackupDir(projectPath), ".t8proj", nullptr);                          // the dated copies
    addDir(fs::path(userdirs::RecoveryPathFor(projectPath)).parent_path().string(), ".t8proj", nullptr);   // unsaved work
    addDir(userdirs::SynthPresetsDir(), ".t8syn", nullptr);                          // a preset on the SAMPLE engine names its file
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// The candidates whose name appears in any of `files` (read as text). A file that cannot be read keeps everything (*unreadable).
inline std::set<std::string> NamedInFiles(const std::vector<std::string>& candidates, const std::vector<std::string>& files, bool* unreadable = nullptr) {
    std::set<std::string> named;
    for (const std::string& f : files) {
        std::ifstream in(f, std::ios::binary);
        if (!in) { if (unreadable) *unreadable = true; continue; }
        std::ostringstream ss; ss << in.rdbuf();
        const std::string text = ss.str();
        for (const std::string& c : candidates) if (!named.count(c) && text.find(c) != std::string::npos) named.insert(c);
    }
    return named;
}

// Where the Trash is: macOS ~/.Trash; elsewhere $XDG_DATA_HOME/Trash or ~/.local/share/Trash ("files" + "info" inside). Empty = none.
inline std::string TrashRoot() {
    const char* home = std::getenv("HOME");
#ifdef __APPLE__
    return home && *home ? std::string(home) + "/.Trash" : std::string();
#else
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) return std::string(x) + "/Trash";
    return home && *home ? std::string(home) + "/.local/share/Trash" : std::string();
#endif
}

// Moves `path` into the Trash at `root` (TrashRoot() when empty). macOS layout: the file goes straight into root; freedesktop layout
// (freedesktop = true): root/files plus root/info/<name>.trashinfo. A name already in the Trash gets " 2", " 3" ... before the
// extension. Only a rename: a file on another disk stays where it is (returns false, `err` says why).
inline bool MoveToTrash(const std::string& path, std::string& err, std::string root = std::string(),
#ifdef __APPLE__
                        bool freedesktop = false
#else
                        bool freedesktop = true
#endif
) {
    namespace fs = std::filesystem;
    if (root.empty()) root = TrashRoot();
    if (root.empty()) { err = "no Trash folder"; return false; }
    std::error_code ec;
    const std::string filesDir = freedesktop ? root + "/files" : root;
    fs::create_directories(filesDir, ec);
    if (freedesktop) fs::create_directories(root + "/info", ec);
    const fs::path src(path);
    const std::string stem = src.stem().string(), ext = src.extension().string();
    std::string name = src.filename().string();
    for (int k = 2; k < 1000 && (fs::exists(filesDir + "/" + name, ec) || (freedesktop && fs::exists(root + "/info/" + name + ".trashinfo", ec))); ++k)
        name = stem + " " + std::to_string(k) + ext;
    const std::string dest = filesDir + "/" + name;
    if (freedesktop) {   // the info file first, as the spec asks (a Trash entry without its info file is not restorable)
        char stamp[32] = {};
        const std::time_t now = std::time(nullptr);
        std::tm tmv{};
        localtime_r(&now, &tmv);
        std::strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S", &tmv);
        std::ofstream info(root + "/info/" + name + ".trashinfo", std::ios::trunc);
        info << "[Trash Info]\nPath=" << fs::absolute(src, ec).string() << "\nDeletionDate=" << stamp << "\n";
        if (!info) { err = "cannot write the Trash info file"; return false; }
    }
    if (std::rename(path.c_str(), dest.c_str()) != 0) {
        err = errno == EXDEV ? "it is on another disk than the Trash" : std::strerror(errno);
        if (freedesktop) std::remove((root + "/info/" + name + ".trashinfo").c_str());
        return false;
    }
    return true;
}

} // namespace t8::cleanup
