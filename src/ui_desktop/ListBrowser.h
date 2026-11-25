#pragma once
// v44.1: the same big browser window as the synth PresetBrowser, for any short list of named things (drum kits,
// typical drum patterns): a category list with counts on the left (ALL, FAVORITES, then the caller's categories),
// a search field and tag chips on top, a scrolling grid of cards. The caller owns the data (rebuilt on Open) and
// decides what a click and a pick do:
//   click / arrow key   onClick(item)  - e.g. load a kit (silent since v44.2; only when liveSelect is on for arrows)
//   HEAR / Space / AUTO onHear(item)   - plays the selected card without picking it
//   Enter / double-click onPick(item)  - then the window closes
// Esc clears the search, then closes. A star on a card toggles a favourite (saved in Config::favFile).
#include "imgui.h"
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace t8::desktop {

class ListBrowser {
public:
    struct Item {
        int id = 0;
        std::string name;
        std::string category;           // one of Config::categories (or empty = only under ALL)
        std::vector<std::string> tags;  // shown on the card, filterable when listed in Config::tags
        std::string favKey;             // stable key for the favourites file
    };
    struct Config {
        std::string title;
        std::vector<std::string> categories;
        std::vector<std::string> tags;  // tag chips on top (may be empty)
        std::string favFile;
        std::string startCategory;      // v44.51: the category shown first when the browser of this title opens (empty = keep the last one)
        std::string footer;             // hint line, e.g. "click = load and hear   Enter = pick"
        bool liveSelect = false;        // arrow keys call onClick for the new card (like a click)
        // v44.49: an optional strip under the cards (height detailH) the caller draws for the selected card (STYLE: what APPLY does).
        float detailH = 0.0f;
        std::function<void(int id, float width)> detail;
    };
    using Fn = std::function<void(const Item&)>;

    void Open(const Config& cfg, std::vector<Item> items, int curId);
    bool IsOpen() const { return wantOpen_ || open_; }
    // onHear (optional) plays the selected card without picking it: HEAR button, Space, or automatically with AUTO-HEAR.
    void Draw(int curId, const Fn& onClick, const Fn& onPick, const Fn& onHear = nullptr);

private:
    void LoadFavorites();
    void SaveFavorites() const;
    bool IsFav(const Item& it) const { return favs_.count(it.favKey) > 0; }
    std::vector<int> Visible() const;
    int NumCats() const { return 2 + static_cast<int>(cfg_.categories.size()); }
    const std::string& CatName(int c) const;

    Config cfg_;
    std::vector<Item> items_;
    std::set<std::string> favs_;
    bool wantOpen_ = false, open_ = false;
    int cat_ = 0;                // 0 ALL, 1 FAV, 2.. cfg_.categories
    char search_[40] = "";
    unsigned tagMask_ = 0;
    int sel_ = -1;
    bool scrollToSel_ = false;
    bool focusSearch_ = false;
    bool autoHear_ = false;      // v44.2: selecting is silent unless this is on (live sets must not make noise while browsing)
};

} // namespace t8::desktop
