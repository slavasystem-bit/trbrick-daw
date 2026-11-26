#pragma once
// Stage C4b (v3): performance sets of the SAMPLES browser ("Add to set"): named lists of samples the user gathers for a gig,
// kept in one small text file beside the favourites (Documents/TR-Brick/Library/Favorites/sample_sets.txt):
//
//   # TR-Brick sample sets 1
//   [Set 26 Nov]
//   - Solid Sounds/Drum Loops/funk_95.wav
//   - kit:kits/909/kick_2.wav
//
// The entries are the browser's favourite keys (a library path, "kit:...", "prj:...", "riser:..."), so a set may hold any kind
// of sample the favourites can. Sets keep their order; entries keep the order they were added in. Damaged lines, entries outside
// a valid set and repeats are skipped, so a hand-edited file never stops the browser. Written in one go (WriteFileAtomic).
#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace t8 {

struct SampleSet {
    std::string name;
    std::vector<std::string> keys;
    bool operator==(const SampleSet& o) const { return name == o.name && keys == o.keys; }
};
using SampleSetList = std::vector<SampleSet>;

namespace samplesets {

constexpr const char* kStoreHeader = "# TR-Brick sample sets 1";
constexpr size_t kNameMax = 40;     // characters (bytes) in a set name
constexpr size_t kMaxSets = 64;
constexpr size_t kMaxEntries = 2000;  // per set

// A name the file can hold: trimmed, no line breaks or brackets, not empty, at most kNameMax bytes (cut on a UTF-8 boundary).
inline std::string CleanName(const std::string& in) {
    std::string s;
    for (char c : in) if (c != '\n' && c != '\r' && c != '\t' && c != '[' && c != ']') s += c;
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (s.size() > kNameMax) {
        size_t n = kNameMax;
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;   // do not split a UTF-8 letter
        s.resize(n);
        while (!s.empty() && s.back() == ' ') s.pop_back();
    }
    return s;
}

inline int Find(const SampleSetList& l, const std::string& name) {
    for (size_t i = 0; i < l.size(); ++i) if (l[i].name == name) return static_cast<int>(i);
    return -1;
}

// "Set 26 Nov", or "Set 26 Nov 2", "Set 26 Nov 3" ... when that name is taken.
inline std::string FreeName(const SampleSetList& l, const std::string& base) {
    if (Find(l, base) < 0) return base;
    for (int k = 2; k < 1000; ++k) { const std::string n = base + " " + std::to_string(k); if (Find(l, n) < 0) return n; }
    return base + " +";
}

inline SampleSetList ParseStore(const std::string& text, int* skipped = nullptr) {
    SampleSetList out;
    int bad = 0;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line.front() == '[') {
            const std::string name = line.size() >= 3 && line.back() == ']' ? CleanName(line.substr(1, line.size() - 2)) : std::string();
            const bool ok = !name.empty() && Find(out, name) < 0 && out.size() < kMaxSets;
            if (!ok) ++bad;
            out.push_back(SampleSet{ok ? name : std::string(), {}});   // a bad set swallows its entries (removed below)
            continue;
        }
        if (line.size() < 3 || line.compare(0, 2, "- ") != 0 || out.empty() || out.back().name.empty()) { ++bad; continue; }
        const std::string key = line.substr(2);
        SampleSet& set = out.back();
        if (key.find('\t') != std::string::npos || std::find(set.keys.begin(), set.keys.end(), key) != set.keys.end() || set.keys.size() >= kMaxEntries) { ++bad; continue; }
        set.keys.push_back(key);
    }
    out.erase(std::remove_if(out.begin(), out.end(), [](const SampleSet& s) { return s.name.empty(); }), out.end());
    if (skipped) *skipped = bad;
    return out;
}

inline std::string SerializeStore(const SampleSetList& l) {
    std::string s = std::string(kStoreHeader) + "\n";
    for (const SampleSet& set : l) {
        const std::string name = CleanName(set.name);
        if (name.empty()) continue;
        s += "[" + name + "]\n";
        for (const std::string& k : set.keys) {
            if (k.empty() || k.find('\n') != std::string::npos || k.find('\r') != std::string::npos || k.find('\t') != std::string::npos) continue;
            s += "- "; s += k; s += '\n';
        }
    }
    return s;
}

} // namespace samplesets
} // namespace t8
