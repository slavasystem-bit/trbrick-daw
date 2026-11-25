#pragma once
// Groove library (v42): built-in grooves, user grooves in "grooves/*.t8groove", capture from a track, and BAKE.
// A groove is 16 steps of {nudge %, velocity offset}; see Groove in Types.h. The pattern carries a COPY
// (Pattern::groove), so a project never depends on the library staying the same.
#include "Types.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace t8 {

inline Groove MakeGroove(const char* name, const int (&nudge)[16], const int (&vel)[16]) {
    Groove g; g.name = name;
    for (int i = 0; i < 16; ++i) {
        g.steps[i].nudge = static_cast<int8_t>(std::clamp(nudge[i], -50, 50));
        g.steps[i].vel = static_cast<int8_t>(std::clamp(vel[i], -127, 127));
    }
    return g;
}

inline const std::vector<Groove>& BuiltinGrooves() {
    static const std::vector<Groove> all = [] {
        std::vector<Groove> v;
        // electro: tight and slightly rushing downbeats, the 16th "a" a touch late, strong beat accents
        { const int n[16] = {0,2,-2,6, 0,2,-2,6, 0,2,-2,6, 0,2,-2,6}; const int a[16] = {14,-10,4,-4, 12,-10,4,-4, 14,-10,4,-4, 12,-10,4,-4}; v.push_back(MakeGroove("ELECTRO PUSH", n, a)); }
        // techno: dead straight, the beat and the offbeat 8th carry it, the 16ths sit low
        { const int n[16] = {0,3,0,3, 0,3,0,3, 0,3,0,3, 0,3,0,3}; const int a[16] = {10,-18,6,-12, 6,-18,6,-12, 10,-18,6,-12, 6,-18,6,-12}; v.push_back(MakeGroove("TECHNO DRIVE", n, a)); }
        // breaks: a shuffle on the odd 16ths, lazy snare
        { const int n[16] = {0,16,0,8, 3,16,0,16, 0,16,0,8, 3,16,0,16}; const int a[16] = {8,-22,0,-14, 10,-20,0,-16, 6,-22,0,-14, 10,-20,0,-16}; v.push_back(MakeGroove("BREAKS SHUFFLE", n, a)); }
        // acid: accents on the offbeat 8ths, "a" late
        { const int n[16] = {0,0,-2,5, 0,0,-2,5, 0,0,-2,5, 0,0,-2,5}; const int a[16] = {8,-10,12,-6, 6,-10,12,-6, 8,-10,12,-6, 6,-10,12,-6}; v.push_back(MakeGroove("ACID BOUNCE", n, a)); }
        { const int n[16] = {0,16,0,16, 0,16,0,16, 0,16,0,16, 0,16,0,16}; const int a[16] = {0,-8,0,-8, 0,-8,0,-8, 0,-8,0,-8, 0,-8,0,-8}; v.push_back(MakeGroove("MPC 16TH 58", n, a)); }
        { const int n[16] = {0,33,0,33, 0,33,0,33, 0,33,0,33, 0,33,0,33}; const int a[16] = {0,-10,0,-10, 0,-10,0,-10, 0,-10,0,-10, 0,-10,0,-10}; v.push_back(MakeGroove("MPC 16TH 66 TRIPLET", n, a)); }
        { const int n[16] = {0,6,3,6, 10,6,3,6, 0,6,3,6, 10,6,3,6}; const int a[16] = {6,-10,0,-8, 8,-10,0,-8, 6,-10,0,-8, 8,-10,0,-8}; v.push_back(MakeGroove("LAID BACK", n, a)); }
        // drums pull ahead (negative nudges work on drum tracks only; melodic tracks stay on the grid)
        { const int n[16] = {0,-3,-4,-5, 0,-3,-4,-5, 0,-3,-4,-5, 0,-3,-4,-5}; const int a[16] = {10,-6,4,-6, 10,-6,4,-6, 10,-6,4,-6, 10,-6,4,-6}; v.push_back(MakeGroove("PUSHING", n, a)); }
        { const int n[16] = {0,18,-6,12, 8,-10,16,6, -4,20,-8,14, 10,-12,18,4}; const int a[16] = {10,-16,-4,-12, 8,-18,2,-10, 12,-14,-6,-16, 6,-20,0,-8}; v.push_back(MakeGroove("DRUNK", n, a)); }
        { const int n[16] = {0,2,-1,3, -2,1,-3,2, 0,-2,1,-3, 2,-1,3,-2}; const int a[16] = {4,-4,2,-6, 6,-3,3,-5, 4,-5,1,-6, 5,-2,4,-4}; v.push_back(MakeGroove("HUMAN TIGHT", n, a)); }
        { const int n[16] = {0}; const int a[16] = {16,-8,6,-8, 12,-8,6,-8, 16,-8,6,-8, 12,-8,6,-8}; v.push_back(MakeGroove("FOUR FLOOR ACCENT", n, a)); }
        return v;
    }();
    return all;
}

// ---- user grooves: grooves/<name>.t8groove ----------------------------------------------------------------------
inline std::string SanitizeGrooveName(const std::string& raw) {
    std::string out;
    for (char c : raw) if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == ' ') out += c;
    const size_t a = out.find_first_not_of(' '), b = out.find_last_not_of(' ');
    if (a == std::string::npos) return "GROOVE";
    return out.substr(a, b - a + 1);
}

inline bool SaveCustomGroove(const Groove& g) {
    mkdir("grooves", 0755);
    const std::string name = SanitizeGrooveName(g.name);
    const std::string path = "grooves/" + name + ".t8groove", tmp = path + ".tmp";
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return false;
    f << "name=" << name << "\n";
    for (int i = 0; i < 16; ++i) f << "g" << i << "=" << static_cast<int>(g.steps[i].nudge) << "," << static_cast<int>(g.steps[i].vel) << "\n";
    f.close();
    return !f.fail() && std::rename(tmp.c_str(), path.c_str()) == 0;
}

inline bool LoadCustomGroove(const std::string& path, Groove& out) {
    std::ifstream f(path);
    if (!f) return false;
    out = Groove{};
    std::string line;
    bool any = false;
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        if (key == "name") out.name = val;
        else if (key.size() >= 2 && key[0] == 'g') {
            const int i = std::atoi(key.c_str() + 1);
            if (i < 0 || i > 15) continue;
            const size_t c = val.find(',');
            if (c == std::string::npos) continue;
            out.steps[i].nudge = static_cast<int8_t>(std::clamp(std::atoi(val.c_str()), -50, 50));
            out.steps[i].vel = static_cast<int8_t>(std::clamp(std::atoi(val.c_str() + c + 1), -127, 127));
            any = true;
        }
    }
    return any;
}

inline std::vector<Groove> ListCustomGrooves() {
    std::vector<Groove> out;
    DIR* d = opendir("grooves");
    if (!d) return out;
    while (struct dirent* ent = readdir(d)) {
        const std::string n = ent->d_name, ext = ".t8groove";
        if (n.size() > ext.size() && n.compare(n.size() - ext.size(), ext.size(), ext) == 0) {
            Groove g;
            if (LoadCustomGroove("grooves/" + n, g)) { if (g.name.empty()) g.name = n.substr(0, n.size() - ext.size()); out.push_back(g); }
        }
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const Groove& a, const Groove& b) { return a.name < b.name; });
    return out;
}

// Capture the feel of one track: for every step position of the bar the average nudge and the velocity relative to the
// plain 0.8 of the hits that sit there (positions without a hit stay 0). "Play it, save the groove, use it on other tracks".
inline Groove GrooveFromTrack(const Track& t, const Pattern& pat, int trackLen, const std::string& name) {
    Groove g; g.name = name;
    int nSum[16] = {}, vSum[16] = {}, cnt[16] = {};
    for (int i = 0; i < std::min(trackLen, kMaxSteps); ++i) {
        const Step& s = t.steps[i];
        if (!s.active) continue;
        const int p = static_cast<int>(GroovePos(pat, i));
        nSum[p] += s.nudge;
        vSum[p] += static_cast<int>(std::lround((StepVelocity(s) - kDefaultVelocity) * 127.0f));
        ++cnt[p];
    }
    for (int p = 0; p < 16; ++p) if (cnt[p]) {
        g.steps[p].nudge = static_cast<int8_t>(std::clamp(static_cast<int>(std::lround(static_cast<float>(nSum[p]) / cnt[p])), -50, 50));
        g.steps[p].vel = static_cast<int8_t>(std::clamp(static_cast<int>(std::lround(static_cast<float>(vSum[p]) / cnt[p])), -127, 127));
    }
    return g;
}

// Write the groove (at its amount) into the steps of the tracks that follow it, then switch the groove layer off.
inline void BakeGroove(Pattern& p) {
    if (p.grooveAmount <= 0.0f) return;
    for (Track& t : p.tracks) {
        if (!t.grooveOn) continue;
        for (int i = 0; i < kMaxSteps; ++i) {
            Step& s = t.steps[i];
            if (!s.active) continue;
            const float v = StepPlayVelocity(t, p, i, s);
            const double off = static_cast<double>(s.nudge) / 100.0 + p.grooveAmount * p.groove.steps[GroovePos(p, i)].nudge / 100.0;
            s.vel = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(v * 127.0f)), 1, 127));
            s.accent = false; s.ghost = false;
            s.nudge = static_cast<int8_t>(std::clamp(static_cast<int>(std::lround(off * 100.0)), t.type == TrackType::Drum ? -50 : 0, 50));
        }
    }
    p.grooveAmount = 0.0f;
    p.groove = Groove{};
}

} // namespace t8
