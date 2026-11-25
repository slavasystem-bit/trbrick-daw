#pragma once
// v44.1: one-line character of every shipped drum kit, for the kit browser (category + tags). Kits a user drops into
// kits/ are not listed here and show up under MY KITS without tags.
#include <string>
#include <vector>

namespace t8 {

struct KitInfo { const char* name; const char* category; std::vector<std::string> tags; };

inline const KitInfo* FindKitInfo(const std::string& folderName) {
    static const std::vector<KitInfo> kits = {
        {"606", "CLASSIC", {"SHORT", "BRIGHT"}},
        {"707", "CLASSIC", {"SHORT", "BRIGHT"}},
        {"808", "CLASSIC", {"SUB", "PUNCHY", "LONG"}},
        {"808perc", "CLASSIC", {"SUB", "PUNCHY"}},
        {"909", "CLASSIC", {"PUNCHY", "DRIVEN"}},
        {"cr78", "CLASSIC", {"SOFT", "VINTAGE"}},
        {"linn", "CLASSIC", {"CRUSHED", "VINTAGE"}},
        {"acid", "ELECTRONIC", {"DRIVEN", "PUNCHY"}},
        {"acid_v2", "ELECTRONIC", {"DRIVEN", "PUNCHY"}},
        {"electro", "ELECTRONIC", {"TIGHT", "BRIGHT"}},
        {"techno", "ELECTRONIC", {"PUNCHY", "TIGHT"}},
        {"minimal", "ELECTRONIC", {"SHORT", "CLICKY"}},
        {"gabber", "HARD", {"DISTORTED", "LONG"}},
        {"industrial", "HARD", {"DISTORTED", "METAL"}},
        {"breaks", "GROOVE", {"DRY", "PUNCHY"}},
        {"dub", "GROOVE", {"LONG", "SPRING"}},
        {"funk", "GROOVE", {"DRY", "SOFT"}},
        {"lofi", "GROOVE", {"CRUSHED", "DUSTY"}},
        {"trap808", "GROOVE", {"SUB", "LONG"}},
    };
    for (const auto& k : kits) if (folderName == k.name) return &k;
    return nullptr;
}

} // namespace t8
