#pragma once
// v44: the large synth-preset browser. Replaces the small drop-down: a modal window with a category list on
// the left (ALL / FAV / BASS / LEAD / PAD / KEYS / PLUCK / FX / MY, each with a count), a search field and tag
// chips on top and a scrolling grid of preset cards. A click applies the preset silently (v44.2: a live set must not make noise while
// browsing); HEAR / Space / AUTO-HEAR plays a short audition note (a bass is auditioned low, a pad as a chord), the arrow keys move through the grid, Enter / double click picks
// and closes, Esc clears the search and then closes. Stars mark favourites (presets/favorites.txt).
#include "app/AppState.h"
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace t8::desktop {

class PresetBrowser {
public:
    struct Item {
        int idx = 0;                 // index in the combined list (built-ins first, then saved presets)
        std::string name;
        std::string category;        // BASS LEAD PAD KEYS PLUCK FX or MY
        std::vector<std::string> tags;
        bool custom = false;
        std::string favKey;
    };
    using ApplyFn = std::function<void(int idx)>;

    // Open for the synth / chord track `ti`; `curIdx` is the preset currently on it (-1 = none).
    void Open(AppState& app, int ti, int curIdx);
    bool IsOpen() const { return wantOpen_ || open_; }
    int TrackIndex() const { return ti_; }
    // Call every frame (also while closed: it ends a running audition note).
    void Draw(AppState& app, int curIdx, const ApplyFn& apply);
    // Previous / next preset inside the category the browser was last left on (no search); -1 when empty.
    int Step(AppState& app, int curIdx, int dir);

    // Name + category of a preset for the small button in the PRESET block.
    const Item* Find(AppState& app, int idx);
    static unsigned CategoryColor(const std::string& category);
    static std::vector<std::string> TagsOf(const SynthPatch& p);

private:
    void Refresh(AppState& app);
    void LoadFavorites();
    void SaveFavorites() const;
    bool IsFav(const Item& it) const { return favs_.count(it.favKey) > 0; }
    std::vector<int> Visible(bool useSearch) const;     // indices into items_
    void Audition(AppState& app, const Item& it);
    void StopAudition(AppState& app);

    std::vector<Item> items_;
    std::vector<std::string> customSeen_;
    bool itemsValid_ = false;
    std::set<std::string> favs_;
    bool favsLoaded_ = false;

    bool wantOpen_ = false, open_ = false;
    int ti_ = -1;
    int cat_ = 0;                    // 0 ALL, 1 FAV, 2.. built-in categories, last = MY
    char search_[40] = "";
    unsigned tagMask_ = 0;
    int sel_ = -1;                   // selected card (index into the visible list)
    bool scrollToSel_ = false;
    bool focusSearch_ = false;
    bool autoHear_ = false;          // v44.2: selecting is silent unless AUTO-HEAR is on; HEAR / Space plays the selected card

    // audition
    double offAt_ = 0.0;
    bool auditionChord_ = false;
    int auditionSlot_ = 0;
};

} // namespace t8::desktop
