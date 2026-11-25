#pragma once
// v44.52.1: the tempo written in a file name ("Drum and Bass Down 170 bpm.wav", "loop_90BPM.wav", "BPM 128 break.wav", "amen-174-bpm.wav"),
// 0 when there is none. Used for the library list, the audition at the project tempo and the AUTO tempo fit of the sampler.
#include <cctype>
#include <cstdlib>
#include <string>

namespace t8 {

inline double TempoFromName(const std::string& name) {
    std::string n; for (char c : name) n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (size_t at = n.find("bpm"); at != std::string::npos; at = n.find("bpm", at + 1)) {
        auto number = [&](long i, int dir) -> double {   // the digits next to "bpm", skipping spaces, '_' and '-'
            while (i >= 0 && i < static_cast<long>(n.size()) && (n[static_cast<size_t>(i)] == ' ' || n[static_cast<size_t>(i)] == '_' || n[static_cast<size_t>(i)] == '-')) i += dir;
            std::string d;
            while (i >= 0 && i < static_cast<long>(n.size()) && std::isdigit(static_cast<unsigned char>(n[static_cast<size_t>(i)]))) { if (dir < 0) d.insert(d.begin(), n[static_cast<size_t>(i)]); else d += n[static_cast<size_t>(i)]; i += dir; }
            return d.empty() || d.size() > 3 ? 0.0 : std::atof(d.c_str());
        };
        for (double v : {number(static_cast<long>(at) - 1, -1), number(static_cast<long>(at) + 3, 1)}) if (v >= 50.0 && v <= 300.0) return v;
    }
    return 0.0;
}

} // namespace t8
