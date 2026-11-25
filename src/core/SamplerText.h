#pragma once
// v44.50 / v44.53: the one-line text form of a sampler's settings ("file|root|start|attackMs|decay|poly|reverse[|tune|gain|mode|slices|fit[|marks]]"),
// used by the project file (the sampler track and, since v44.53, the SAMPLE engine of a synth) and by the synth presets.
#include "Types.h"
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace t8 {

// v44.50: "file|root|start|attackMs|decay|poly|reverse" (the file name never contains a '|': it is sanitised when a sample is copied in).
inline std::string SamplerToString(const SamplerPatch& s) {
    std::ostringstream o;
    o << s.file << "|" << static_cast<int>(s.root) << "|" << s.start << "|" << s.attackMs << "|" << s.decay << "|" << (s.poly ? 1 : 0) << "|" << (s.reverse ? 1 : 0);
    if (s.tuneCents != 0 || s.gainDb != 0 || s.mode != 0 || s.slices != 8 || s.fitBars != -1 || s.markCount > 0)   // v44.51 / v44.52, only when used: older files stay byte for byte the same
        o << "|" << static_cast<int>(s.tuneCents) << "|" << static_cast<int>(s.gainDb) << "|" << static_cast<int>(s.mode) << "|" << static_cast<int>(s.slices) << "|" << static_cast<int>(s.fitBars);
    if (s.markCount > 0) { o << "|"; for (int i = 0; i < s.markCount; ++i) o << (i ? "," : "") << s.marks[static_cast<size_t>(i)]; }   // v44.52.1
    return o.str();
}
inline SamplerPatch SamplerFromString(const std::string& v) {
    SamplerPatch s;
    std::vector<std::string> f;
    size_t pos = 0;
    for (;;) { const size_t q = v.find('|', pos); f.push_back(v.substr(pos, q == std::string::npos ? q : q - pos)); if (q == std::string::npos) break; pos = q + 1; }
    if (f.size() < 7) { s.file = f.empty() ? std::string() : f[0]; return s; }
    s.file = f[0];
    s.root = static_cast<int8_t>(std::clamp(std::atoi(f[1].c_str()), 0, 127));
    s.start = std::clamp(static_cast<float>(std::atof(f[2].c_str())), 0.0f, 0.95f);
    s.attackMs = std::clamp(static_cast<float>(std::atof(f[3].c_str())), 0.0f, 200.0f);
    s.decay = std::clamp(static_cast<float>(std::atof(f[4].c_str())), 0.02f, 1.0f);
    s.poly = std::atoi(f[5].c_str()) != 0;
    s.reverse = std::atoi(f[6].c_str()) != 0;
    if (f.size() >= 9) { s.tuneCents = static_cast<int8_t>(std::clamp(std::atoi(f[7].c_str()), -100, 100)); s.gainDb = static_cast<int8_t>(std::clamp(std::atoi(f[8].c_str()), -24, 12)); }
    if (f.size() >= 12) {
        s.mode = static_cast<uint8_t>(std::clamp(std::atoi(f[9].c_str()), 0, 2));
        const int sl = std::atoi(f[10].c_str()); s.slices = static_cast<uint8_t>(sl == 0 || sl == 4 || sl == 8 || sl == 16 || sl == 32 ? sl : 8);
        const int fb = std::atoi(f[11].c_str()); s.fitBars = static_cast<int8_t>(fb == -1 || fb == 0 || fb == 1 || fb == 2 || fb == 4 || fb == 8 || fb == 16 ? fb : -1);
    }
    if (f.size() >= 13 && !f[12].empty()) {   // v44.52.1: the marks of the HITS slices
        size_t at = 0; int cnt = 0; int prev = -1;
        while (at < f[12].size() && cnt < 32) {
            const size_t q = f[12].find(',', at);
            const int v = std::clamp(std::atoi(f[12].substr(at, q == std::string::npos ? q : q - at).c_str()), 0, 65535);
            if (v > prev) { s.marks[static_cast<size_t>(cnt++)] = static_cast<uint16_t>(v); prev = v; }
            if (q == std::string::npos) break;
            at = q + 1;
        }
        s.markCount = static_cast<uint8_t>(cnt);
    }
    return s;
}


} // namespace t8
