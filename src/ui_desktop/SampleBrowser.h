#pragma once
// Stage C4b part 2: the one SAMPLES browser. Every place that plays a sample opens it (the name of the SAMPLE row): the drums, the
// SAMPLER, a synth or the chord on ENGINE > SAMPLE, a transition before a brick. The title says where the file goes
// ("SAMPLE FOR: KICK"). Approved mockup: docs/mockups/v45_samples_unified.png, section 2.
//
//   left    SOURCE: THIS PROJECT, MY LIBRARY, KITS (drums), BUILT-IN (transitions), FROM FINDER... (sampler / synth)
//           LIBRARY FOLDERS (a tree with counts, MY LIBRARY only), MY COLLECTIONS: FAVORITES, RECENT (the last 20 taken)
//   top     search, TYPE chips, LIST / GRID (remembered; samples open as LIST)
//   middle  the files: columns star / NAME / FOLDER / TYPE / BPM, a click on a column title sorts
//   right   the selected file: waveform (when it is in memory), where it lives, HEAR, AUTO, USE FOR ...
//
// Same keys as every browser: click = select (silent), Space / HEAR = listen, AUTO = listen to every selection, Up / Down = move,
// Left / Right = close / open the folder, Return / double-click = take, Esc = clear the search, then close.
//
// File work never happens here while drawing (the scene lock is held): the kit folders are listed and files are read for HEAR on
// the loader thread (AsyncLoader), and a late result is dropped when the browser was opened again meanwhile.
#include "imgui.h"
#include "app/AppState.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace t8::desktop {

class SampleBrowser {
public:
    enum class Source { Project, Library, Kits, BuiltIn, Favorites, Recent };
    struct Item {
        Source src = Source::Library;
        std::string key;      // library: rel path; project: file name in samples/; kits: full path; built-in / project transition: riser index
        std::string name;     // shown
        std::string folder;   // shown in FOLDER (library: the folder path below the root; kits: the kit)
        std::string type;     // KICK ... OTHER, or empty
        double bpm = 0.0;
        int riser = -1;       // transitions: the riser index
        int variant = -1;     // drums, THIS PROJECT: the variant of this drum in the kit
        std::string favKey;   // stable key for favourites and recent ("lib:<rel>", "kit:<path>", "prj:<name>", "riser:<name>")
    };

    // target: Drum = the selected drum track (seqCursorTrack), Sampler = app.libSampleTarget, Transition = the brick `brick`.
    void Open(AppState& app, AppState::LibTarget target, int brick);
    bool IsOpen() const { return wantOpen_ || open_; }
    void Draw(AppState& app);   // under the scene lock, once per frame

    // Pure helpers, used by the tests.
    static std::vector<std::string> FolderChain(const std::string& rel) {   // "A/B/c.wav" -> {"A", "A/B"}
        std::vector<std::string> out;
        for (size_t p = rel.find('/'); p != std::string::npos; p = rel.find('/', p + 1)) out.push_back(rel.substr(0, p));
        return out;
    }
    // column 0 name, 1 folder, 2 type, 3 bpm; ties by name (case ignored); stable
    static void SortItems(std::vector<Item>& items, int column, bool descending) {
        auto lower = [](std::string x) { for (auto& c : x) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return x; };
        auto key = [&](const Item& a, const Item& b) -> int {
            switch (column) {
                case 1: { const int c = lower(a.folder).compare(lower(b.folder)); if (c) return c; break; }
                case 2: { const int c = a.type.compare(b.type); if (c) return c; break; }
                case 3: if (a.bpm != b.bpm) return a.bpm < b.bpm ? -1 : 1; break;
                default: break;
            }
            return lower(a.name).compare(lower(b.name));
        };
        std::stable_sort(items.begin(), items.end(), [&](const Item& a, const Item& b) { const int c = key(a, b); return descending ? c > 0 : c < 0; });
    }

private:
    void Rebuild(AppState& app);
    void ScanKits(AppState& app);
    void Hear(AppState& app, const Item& it);
    void Take(AppState& app, const Item& it);
    void PushRecent(const std::string& favKey);
    void LoadLists();
    void SaveFavorites() const;
    void SaveRecent() const;
    bool Visible(const Item& it) const;
    std::string Who(AppState& app) const;
    void DrawWave(AppState& app, const Item& it, float w, float h);

    AppState::LibTarget target_ = AppState::LibTarget::None;
    int brick_ = -1, sampleTarget_ = AppState::kTargetSampler, drumTrack_ = 0, scene_ = 0;
    bool wantOpen_ = false, open_ = false;
    Source src_ = Source::Project;
    std::map<int, Source> lastSrc_;            // per target kind
    std::string folder_;                       // MY LIBRARY: the folder filter ("" = all)
    std::set<std::string> openFolders_;        // expanded in the tree
    std::map<std::string, int> folderCounts_;  // files below each library folder (counted once per library listing, not per frame)
    size_t folderCountsOf_ = static_cast<size_t>(-1);
    int type_ = -1;                            // -1 = ALL, else kLibCategories index
    char search_[48] = "";
    bool list_ = true;                         // LIST / GRID
    int sortCol_ = 0; bool sortDesc_ = false;
    std::vector<Item> items_;                  // of the current source (rebuilt when the source changes)
    std::vector<int> vis_;                     // indices into items_ shown, in order
    int sel_ = -1;
    bool scrollToSel_ = false, focusSearch_ = false, autoHear_ = false, dirty_ = true;
    bool waitKit_ = false, waitLib_ = false;   // the last rebuild found the kit / the library still being read
    std::set<std::string> favs_;
    std::vector<std::string> recent_;          // newest first, at most 20
    // KITS: the files of the target drum in every kit folder, listed on the loader thread
    unsigned kitScanToken_ = 0;
    bool kitScanDone_ = false;
    std::vector<Item> kitItems_;
    std::vector<SamplePtr> keep_;              // the last HEAR samples stay alive here (the audio thread may hold the one that plays)
    std::map<std::string, SamplePtr> kitAudition_;   // kit files read for HEAR, by path
    std::string kitWanted_;                    // the kit file whose HEAR is still being read (only the newest plays)
    // waveform of the selected item, reduced once per sample
    const void* wavePtr_ = nullptr;
    std::vector<float> peaks_;
};

} // namespace t8::desktop
