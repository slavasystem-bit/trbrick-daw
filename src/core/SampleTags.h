#pragma once
// Stage C4b (v1): genre and tags of a library sample, guessed from the words of its folders and file name, the way the category and
// the tempo already are (SampleLibrary). Approved mockup docs/mockups/v45_samples_unified.png, section 2: GENRE uses the words of the
// STYLES browser, TAGS a few sound words. The user's own corrections come later (a file beside the library); the samples and folders
// are never changed. Pure functions, used by the SAMPLES browser and the tests.
#include <array>
#include <cctype>
#include <string>
#include <vector>

namespace t8 {

constexpr int kNumSampleGenres = 9;
inline const char* const kSampleGenres[kNumSampleGenres] = {"HOUSE", "TECHNO", "ELECTRO", "BREAKS", "HIP HOP", "DISCO", "DNB", "MINIMAL", "GARAGE"};
constexpr int kNumSampleTags = 5;
inline const char* const kSampleTags[kNumSampleTags] = {"DUSTY", "PUNCHY", "DARK", "LO-FI", "LIVE"};

namespace sampletags {

// The path in lower case with every run of non-letters / non-digits as one space, camelCase and letters / digits split
// ("BL_HipHop_95bpm" -> " bl hip hop 95 bpm "), padded with spaces so a word can be found as " word ".
inline std::string Words(const std::string& rel) {
    std::string out = " ";
    char prev = ' ';
    for (size_t i = 0; i < rel.size(); ++i) {
        const char c0 = rel[i];
        const unsigned char c = static_cast<unsigned char>(c0);
        const unsigned char next = i + 1 < rel.size() ? static_cast<unsigned char>(rel[i + 1]) : ' ';
        if (std::isalnum(c)) {
            if (std::isupper(c) && std::islower(static_cast<unsigned char>(prev)) && std::islower(next)) out += ' ';   // camelCase: "HipHop" -> "hip hop" ("DnB" stays one word)
            else if (std::isdigit(c) != std::isdigit(static_cast<unsigned char>(prev)) && std::isalnum(static_cast<unsigned char>(prev))) out += ' ';   // "95bpm" -> "95 bpm"
            out += static_cast<char>(std::tolower(c));
        } else if (out.back() != ' ') out += ' ';
        prev = c0;
    }
    if (out.back() != ' ') out += ' ';
    return out;
}
inline bool Has(const std::string& words, const char* w) { return words.find(std::string(" ") + w + " ") != std::string::npos; }

} // namespace sampletags

// The genre of a library path, or "" when no word names one. When several are named, the one named first in the path wins
// (a file in "House/Disco Edits" is HOUSE: its folder comes first).
inline std::string GuessSampleGenre(const std::string& rel) {
    const std::string w = sampletags::Words(rel);
    struct Rule { const char* genre; std::vector<const char*> words; };
    static const std::vector<Rule> rules = {
        {"HOUSE",   {"house", "deephouse", "techhouse"}},
        {"TECHNO",  {"techno"}},
        {"ELECTRO", {"electro"}},
        {"BREAKS",  {"break", "breaks", "breakbeat", "breakbeats", "amen"}},
        {"HIP HOP", {"hiphop", "boombap", "trap"}},
        {"DISCO",   {"disco", "nudisco"}},
        {"DNB",     {"dnb", "jungle", "drumnbass"}},
        {"MINIMAL", {"minimal"}},
        {"GARAGE",  {"garage", "ukg"}},
    };
    // two-word names ("hip hop", "drum and bass", "boom bap", "deep house") are joined first
    std::string j = w;
    auto join = [&](const char* a, const char* b, const char* to) {
        const std::string pat = std::string(" ") + a + " " + b + " ";
        for (size_t p = j.find(pat); p != std::string::npos; p = j.find(pat)) j.replace(p, pat.size(), std::string(" ") + to + " ");
    };
    join("hip", "hop", "hiphop"); join("boom", "bap", "boombap"); join("nu", "disco", "nudisco"); join("deep", "house", "deephouse");
    join("tech", "house", "techhouse"); join("drum", "n", "drumnbass");
    { const std::string pat = " drum and bass "; for (size_t p = j.find(pat); p != std::string::npos; p = j.find(pat)) j.replace(p, pat.size(), " dnb "); }
    size_t best = std::string::npos; std::string genre;
    for (const Rule& r : rules) for (const char* word : r.words) {
        const size_t p = j.find(std::string(" ") + word + " ");
        if (p != std::string::npos && p < best) { best = p; genre = r.genre; }
    }
    return genre;
}

// The tags of a library path (in kSampleTags order).
inline std::vector<std::string> GuessSampleTags(const std::string& rel) {
    using sampletags::Has;
    std::string w = sampletags::Words(rel);
    { const std::string pat = " lo fi "; for (size_t p = w.find(pat); p != std::string::npos; p = w.find(pat)) w.replace(p, pat.size(), " lofi "); }
    std::vector<std::string> out;
    if (Has(w, "dusty") || Has(w, "dust") || Has(w, "vinyl") || Has(w, "crackle")) out.push_back("DUSTY");
    if (Has(w, "punchy") || Has(w, "punch")) out.push_back("PUNCHY");
    if (Has(w, "dark")) out.push_back("DARK");
    if (Has(w, "lofi")) out.push_back("LO-FI");
    if (Has(w, "live")) out.push_back("LIVE");
    return out;
}

} // namespace t8
