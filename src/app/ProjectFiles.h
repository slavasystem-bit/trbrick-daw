#pragma once
// Shared by MenuView (PROJECT row) and AppState (NEW PROJECT's text-entry
// confirm, see AppState::SaveProjectAs) -- moved here once a second
// call site needed the exact same project-file listing/naming logic that
// used to live as MenuView.cpp-local static functions.
#include "core/UserDirs.h"
#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace t8 {

// Returns the directory portion of a path ("a/b/c.t8proj" -> "a/b"), or
// "." if the path has no directory component (a bare filename). Used to
// make sure ListProjectFiles below always includes wherever the
// CURRENTLY OPEN project file actually lives, even when that's neither
// "." nor "projects/" -- see ListProjectFiles' comment for why this
// matters.
inline std::string DirNameOf(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// Finds every "*.t8proj" file in the projects folder (Documents/TR-Brick/Projects, see core/UserDirs.h) and -- if given -- in
// extraDir, the directory of the project that is currently open (it may live elsewhere when opened with --project).
// Small local folders: a rescan is cheap.
inline std::vector<std::string> ListProjectFiles(const std::string& extraDir = "") {
    std::vector<std::string> out;
    auto scan = [&](const std::string& dir) {
        DIR* d = opendir(dir.c_str());
        if (!d) return;
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            std::string name = ent->d_name;
            const std::string ext = ".t8proj";
            if (name.size() > ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0) out.push_back(dir + "/" + name);
        }
        closedir(d);
    };
    const std::string projects = t8::userdirs::ProjectsDir();
    scan(projects);
    if (!extraDir.empty() && extraDir != "." && extraDir != projects) scan(extraDir);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// File name the user typed: letters of any alphabet (Ukrainian included), digits, space and - _ . ( ) are kept, separators and
// control characters dropped; `fallback` when nothing is left (see core/UserDirs.h). Also used for custom drum pattern names.
inline std::string SanitizeFileNamePart(const std::string& raw, const char* fallback) {
    return t8::userdirs::SanitizeName(raw, fallback);
}

// Builds "<projects folder>/<sanitized name>.t8proj", disambiguating with " (2)", " (3)", ... if a file by that name already exists --
// so typing the same name twice (e.g. two takes named "JAM") never silently clobbers the first one.
inline std::string UniqueProjectPathForName(const std::string& rawName) {
    const std::string dir = t8::userdirs::ProjectsDir();
    mkdir(dir.c_str(), 0755); // fine if it already exists
    std::string base = SanitizeFileNamePart(rawName, "Untitled");
    std::string path = dir + "/" + base + ".t8proj";
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return path;
    for (int n = 2; n < 1000; ++n) {
        path = dir + "/" + base + " (" + std::to_string(n) + ").t8proj";
        if (stat(path.c_str(), &st) != 0) return path;
    }
    return dir + "/" + base + ".t8proj"; // give up disambiguating past 999 takes
}

} // namespace t8
