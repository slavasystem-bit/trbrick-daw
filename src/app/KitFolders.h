#pragma once
// Shared by MenuView (KIT row) and RandomView (its own KIT row, added so a
// live performer can switch kits without a trip to the Menu tab) -- moved
// here rather than staying a MenuView.cpp-local static function once a
// second view needed the exact same listing.
#include <algorithm>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace t8 {

// Every subdirectory of "kits/" is treated as a selectable kit -- no
// requirement that it actually contains valid samples here; LoadFromFolder
// reports back how many of the 6 required slots it found when you commit.
inline std::vector<std::string> ListKitFolders() {
    std::vector<std::string> out;
    DIR* d = opendir("kits");
    if (d) {
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            std::string name = ent->d_name;
            if (name == "." || name == ".." || name == "risers") continue;   // risers/ holds the one-shots, not a drum kit
            std::string full = "kits/" + name;
            struct stat st{};
            if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) out.push_back(full);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace t8
