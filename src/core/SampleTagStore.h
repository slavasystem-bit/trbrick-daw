#pragma once
// Stage C4b (v2): the user's own genre and tags for library samples, kept in one small text file in the Library
// (Documents/TR-Brick/Library/sample_tags.txt). The samples and folders are never changed. One line per corrected file:
//
//   # TR-Brick sample tags 1
//   Drum Loops/BL_HipHop_95bpm.wav<TAB>HIP HOP<TAB>DUSTY,LIVE
//
// The path is relative to the library folder (as LibSample::rel). An empty genre means "no genre"; an empty tag list "no tags".
// A file that is not listed keeps the guess. Unknown genres / tags and damaged lines are skipped, so an old or hand-edited file
// never stops the browser. Written in one go (WriteFileAtomic), so a crash never leaves half a file.
#include "SampleTags.h"
#include <map>
#include <sstream>
#include <string>

namespace t8 {

struct SampleTagOverride {
    std::string genre;    // "" = none, else one of kSampleGenres
    unsigned tags = 0;    // bit i = kSampleTags[i]
    bool operator==(const SampleTagOverride& o) const { return genre == o.genre && tags == o.tags; }
};
using SampleTagMap = std::map<std::string, SampleTagOverride>;

namespace sampletags {

constexpr const char* kStoreHeader = "# TR-Brick sample tags 1";

inline int GenreIndex(const std::string& g) { for (int i = 0; i < kNumSampleGenres; ++i) if (g == kSampleGenres[i]) return i; return -1; }

// A path below the library: not absolute, no ".." part. (The path is only a key matched against the library list, never opened.)
inline bool SafeRel(const std::string& rel) {
    if (rel.empty() || rel[0] == '/') return false;
    size_t a = 0;
    while (a <= rel.size()) {
        size_t b = rel.find('/', a); if (b == std::string::npos) b = rel.size();
        if (rel.compare(a, b - a, "..") == 0 && b - a == 2) return false;
        a = b + 1;
    }
    return true;
}

inline SampleTagMap ParseStore(const std::string& text, int* skipped = nullptr) {
    SampleTagMap out;
    int bad = 0;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t t1 = line.find('\t'), t2 = t1 == std::string::npos ? std::string::npos : line.find('\t', t1 + 1);
        if (t1 == 0 || t2 == std::string::npos) { ++bad; continue; }
        const std::string rel = line.substr(0, t1), genre = line.substr(t1 + 1, t2 - t1 - 1), tags = line.substr(t2 + 1);
        if (!SafeRel(rel)) { ++bad; continue; }
        SampleTagOverride o;
        if (!genre.empty()) { if (GenreIndex(genre) < 0) ++bad; else o.genre = genre; }
        size_t a = 0;
        while (a <= tags.size()) {
            size_t b = tags.find(',', a); if (b == std::string::npos) b = tags.size();
            const std::string w = tags.substr(a, b - a);
            if (!w.empty()) { int k = -1; for (int i = 0; i < kNumSampleTags; ++i) if (w == kSampleTags[i]) k = i; if (k >= 0) o.tags |= 1u << k; else ++bad; }
            a = b + 1;
        }
        out[rel] = o;
    }
    if (skipped) *skipped = bad;
    return out;
}

inline std::string SerializeStore(const SampleTagMap& m) {
    std::string s = std::string(kStoreHeader) + "\n";
    for (const auto& [rel, o] : m) {
        if (rel.empty() || rel.find('\t') != std::string::npos || rel.find('\n') != std::string::npos) continue;
        s += rel; s += '\t'; s += o.genre; s += '\t';
        bool first = true;
        for (int i = 0; i < kNumSampleTags; ++i) if (o.tags & (1u << i)) { if (!first) s += ','; s += kSampleTags[i]; first = false; }
        s += '\n';
    }
    return s;
}

} // namespace sampletags
} // namespace t8
