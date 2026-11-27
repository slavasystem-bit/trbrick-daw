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

// C4b (e): a kit may also live in the project's own samples folder: "samples/kits/<name>" (relative to the project file's folder,
// so it travels with the project). Factory kits stay "kits/<name>" beside the program.
constexpr const char* kProjectKitPrefix = "samples/kits/";
inline bool IsProjectKit(const std::string& folder) { return folder.rfind(kProjectKitPrefix, 0) == 0; }
// The kit's name for the screen: "909" for "kits/909" and for "samples/kits/909".
inline std::string KitShortName(const std::string& folder) {
    if (IsProjectKit(folder)) return folder.substr(13);
    if (folder.rfind("kits/", 0) == 0) return folder.substr(5);
    const size_t s = folder.find_last_of('/');
    return s == std::string::npos ? folder : folder.substr(s + 1);
}

} // namespace t8
