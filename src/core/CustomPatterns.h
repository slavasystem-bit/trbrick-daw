#pragma once
// User-saved drum pattern templates for the RANDOM tab's TYPICAL PATTERN
// row -- the custom counterpart to DrumPatterns.h's 27 built-in
// GenrePatterns, except these are written to/read from disk under
// "patterns/" instead of being compiled in, so a live performer can build
// a groove by hand and save it for later (B on TYPICAL PATTERN, see
// RandomView.cpp) instead of being limited to the built-in set.
//
// Deliberately a much smaller format than a full Project/Pattern save
// (Persistence.h): just which of the 6 drum slots hit/accent on which
// step, plus the swing and the step-length it was captured at, tiled the
// same way GenrePattern is (see AppState::ApplyGenrePattern /
// ApplyCustomPattern) -- no per-variant/ratchet/note data, matching what
// GenrePattern itself captures. If you need the full fidelity of what you
// played (including ratchets/sample variants), save the whole PROJECT
// instead; this is meant for "the groove shape", not a perfect copy.
// (Newer files also keep each hit's sample variant, ratchet and probability
// in an optional `detail=` line per track; older files load unchanged.)
#include "Types.h"
#include "UserDirs.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace t8 {

// Per-hit extras (sample variant, ratchet count, probability). Optional: files
// written before beats kept these simply have none (defaults apply).
struct CustomPatternHitDetail {
    int step = 0;
    int variant = 0;
    int ratchet = 1;
    int probability = 100;
};

struct CustomPatternData {
    int storedLength = 16;
    int grid = 4;           // v43: steps per beat the beat was written on (older files: 1/16)
    float swing = 0.0f;
    std::array<std::vector<int>, kNumDrumTracks> hitSteps;
    std::array<std::vector<int>, kNumDrumTracks> accentSteps;
    std::array<std::vector<CustomPatternHitDetail>, kNumDrumTracks> details;
};

// See app/ProjectFiles.h's SanitizeFileNamePart -- same idea, kept as its
// own copy here so core/ doesn't need to depend on ui/.
inline std::string SanitizePatternFileName(const std::string& raw) {
    std::string out;
    for (char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == ' ') out += c;
    }
    size_t a = out.find_first_not_of(' ');
    size_t b = out.find_last_not_of(' ');
    if (a == std::string::npos) return "PATTERN";
    return out.substr(a, b - a + 1);
}

// Every "*.t8pat" file directly under "patterns/", name with the
// extension stripped, alphabetically sorted -- same rescan-on-every-call
// convention as ListProjectFiles/ListKitFolders (cheap, small folder,
// stays in sync with anything added/removed by hand on the SD card).
inline std::vector<std::string> ListCustomPatternNames() {
    std::vector<std::string> out;
    DIR* d = opendir(t8::userdirs::BeatsDir().c_str());
    if (d) {
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            std::string name = ent->d_name;
            const std::string ext = ".t8pat";
            if (name.size() > ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0) {
                out.push_back(name.substr(0, name.size() - ext.size()));
            }
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Captures every active step (0..pat.masterLength-1) of the 6 drum tracks
// in `pat`, plus its swing, as a new (or overwritten, if the same name is
// saved again) "patterns/<name>.t8pat". Returns false only on a disk
// write failure.
inline bool SaveCustomDrumPattern(const std::string& rawName, const Pattern& pat) {
    mkdir(t8::userdirs::BeatsDir().c_str(), 0755); // fine if it already exists
    std::string path = (t8::userdirs::BeatsDir() + "/") + SanitizePatternFileName(rawName) + ".t8pat";
    std::string tmpPath = path + ".tmp";   // written aside, renamed in only when complete
    std::ofstream f(tmpPath, std::ios::trunc);
    if (!f) return false;

    f << "[pattern]\n";
    f << "storedLength=" << pat.masterLength << "\n";
    if (pat.stepsPerBeat != 4) f << "grid=" << pat.stepsPerBeat << "\n";
    f << "swing=" << pat.swing << "\n\n";
    for (int slot = 0; slot < kNumDrumTracks; ++slot) {
        f << "[track" << slot << "]\n";
        std::string hits, accents, detail;
        for (int i = 0; i < pat.masterLength; ++i) {
            const Step& s = pat.tracks[slot].steps[i];
            if (!s.active) continue;
            if (!hits.empty()) hits += ",";
            hits += std::to_string(i);
            if (s.accent) {
                if (!accents.empty()) accents += ",";
                accents += std::to_string(i);
            }
            if (s.variant != kAutoVariant || s.ratchet > 1 || s.probability < 100) {
                if (!detail.empty()) detail += ",";
                detail += std::to_string(i) + "/" + std::to_string(StepVariant(pat.tracks[slot], s)) + "/" +
                          std::to_string(static_cast<int>(s.ratchet)) + "/" + std::to_string(static_cast<int>(s.probability));
            }
        }
        f << "hits=" << hits << "\n";
        f << "accents=" << accents << "\n";
        if (!detail.empty()) f << "detail=" << detail << "\n";
        f << "\n";
    }
    f.flush();
    bool ok = f.good();
    f.close();
    if (!ok) { std::remove(tmpPath.c_str()); return false; }
    if (std::rename(tmpPath.c_str(), path.c_str()) != 0) { std::remove(tmpPath.c_str()); return false; }
    return true;
}

inline bool CustomDrumPatternExists(const std::string& rawName) {
    struct stat st;
    return ::stat(((t8::userdirs::BeatsDir() + "/") + SanitizePatternFileName(rawName) + ".t8pat").c_str(), &st) == 0;
}

// Removes "patterns/<name>.t8pat" (the file the BEAT list shows).
inline bool DeleteCustomDrumPattern(const std::string& name) {
    return std::remove(((t8::userdirs::BeatsDir() + "/") + name + ".t8pat").c_str()) == 0;
}

// Renames a saved beat; refuses to overwrite an existing one. Returns false if
// the source is missing, the target exists, or the rename fails.
inline bool RenameCustomDrumPattern(const std::string& oldName, const std::string& rawNew) {
    std::string target = SanitizePatternFileName(rawNew);
    if (target == oldName) return true;
    if (CustomDrumPatternExists(target)) return false;
    return std::rename(((t8::userdirs::BeatsDir() + "/") + oldName + ".t8pat").c_str(), ((t8::userdirs::BeatsDir() + "/") + target + ".t8pat").c_str()) == 0;
}

// Reads back "patterns/<name>.t8pat". Returns false if the file doesn't
// exist / can't be opened (e.g. it was deleted from the SD card between
// the TYPICAL PATTERN row being browsed and A being pressed).
inline bool LoadCustomDrumPattern(const std::string& name, CustomPatternData& out) {
    std::string path = (t8::userdirs::BeatsDir() + "/") + name + ".t8pat";
    std::ifstream f(path);
    if (!f) return false;
    out = CustomPatternData{};

    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return std::string();
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    };
    auto parseList = [](const std::string& val) {
        std::vector<int> vals;
        std::istringstream ss(val);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            if (!tok.empty()) vals.push_back(std::atoi(tok.c_str()));
        }
        return vals;
    };

    std::string line, section;
    int curTrack = -1;
    while (std::getline(f, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty()) continue;
        if (trimmed.front() == '[') {
            section = trimmed.substr(1, trimmed.size() - 2);
            curTrack = -1;
            if (section.rfind("track", 0) == 0) curTrack = std::atoi(section.substr(5).c_str());
            continue;
        }
        size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(trimmed.substr(0, eq));
        std::string val = trim(trimmed.substr(eq + 1));

        if (section == "pattern") {
            if (key == "storedLength") out.storedLength = std::max(1, std::atoi(val.c_str()));
            else if (key == "grid") out.grid = std::atoi(val.c_str());
            else if (key == "swing") out.swing = static_cast<float>(std::atof(val.c_str()));
        } else if (curTrack >= 0 && curTrack < kNumDrumTracks) {
            if (key == "hits") out.hitSteps[curTrack] = parseList(val);
            else if (key == "accents") out.accentSteps[curTrack] = parseList(val);
            else if (key == "detail") {
                std::istringstream ds(val);
                std::string tok;
                while (std::getline(ds, tok, ',')) {
                    CustomPatternHitDetail d;
                    int f4[4] = {0, 0, 1, 100};
                    std::istringstream ts(tok);
                    std::string part;
                    int fi = 0;
                    while (std::getline(ts, part, '/') && fi < 4) f4[fi++] = std::atoi(part.c_str());
                    d.step = f4[0]; d.variant = f4[1]; d.ratchet = std::clamp(f4[2], 1, 4); d.probability = std::clamp(f4[3], 0, 100);
                    out.details[curTrack].push_back(d);
                }
            }
        }
    }
    return true;
}

} // namespace t8
