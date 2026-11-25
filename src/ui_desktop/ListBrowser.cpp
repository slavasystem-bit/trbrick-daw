#include "ListBrowser.h"
#include "core/UserDirs.h"
#include "InfoLine.h"
#include "Visual.h"
#include "BrowserCore.h"
#include "LiveWidgets.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sys/stat.h>

namespace t8::desktop {

namespace {
std::string Lower(std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
ImU32 PaletteColor(int i) {
    static const ImU32 pal[6] = {vis::kAccent, vis::kTeal, vis::kViolet, IM_COL32(122, 170, 255, 255), IM_COL32(154, 220, 120, 255), vis::kRed};
    return pal[((i % 6) + 6) % 6];
}
}

const std::string& ListBrowser::CatName(int c) const {
    static const std::string all = "ALL", fav = "FAVORITES";
    if (c == 0) return all;
    if (c == 1) return fav;
    return cfg_.categories[static_cast<size_t>(c - 2)];
}

void ListBrowser::LoadFavorites() {
    favs_.clear();
    std::ifstream f(cfg_.favFile);
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) favs_.insert(line);
    }
}

void ListBrowser::SaveFavorites() const {
    mkdir(userdirs::FavoritesDir().c_str(), 0755);
    std::ofstream f(cfg_.favFile, std::ios::trunc);
    for (const auto& k : favs_) f << k << "\n";
}

std::vector<int> ListBrowser::Visible() const {
    std::vector<int> out;
    const std::string q = Lower(search_);
    for (size_t i = 0; i < items_.size(); ++i) {
        const Item& it = items_[i];
        if (cat_ == 1 && !IsFav(it)) continue;
        if (cat_ >= 2 && it.category != cfg_.categories[static_cast<size_t>(cat_ - 2)]) continue;
        bool ok = true;
        for (size_t t = 0; t < cfg_.tags.size() && t < 31; ++t) {
            if (!(tagMask_ & (1u << t))) continue;
            if (std::find(it.tags.begin(), it.tags.end(), cfg_.tags[t]) == it.tags.end()) { ok = false; break; }
        }
        if (!ok) continue;
        if (!q.empty()) {
            std::string hay = Lower(it.name + " " + it.category);
            for (const auto& t : it.tags) hay += " " + Lower(t);
            if (hay.find(q) == std::string::npos) continue;
        }
        out.push_back(static_cast<int>(i));
    }
    return out;
}

void ListBrowser::Open(const Config& cfg, std::vector<Item> items, int curId) {
    const bool sameKind = cfg.title == cfg_.title;
    cfg_ = cfg; items_ = std::move(items);
    LoadFavorites();
    if (!sameKind || cat_ >= NumCats()) cat_ = 0;
    if (!cfg_.startCategory.empty()) for (size_t c = 0; c < cfg_.categories.size(); ++c) if (cfg_.categories[c] == cfg_.startCategory) cat_ = 2 + static_cast<int>(c);
    wantOpen_ = true; search_[0] = '\0'; tagMask_ = 0; focusSearch_ = true;
    sel_ = -1;
    auto vis = Visible();
    for (size_t i = 0; i < vis.size(); ++i) if (items_[static_cast<size_t>(vis[i])].id == curId) { sel_ = static_cast<int>(i); break; }
    if (sel_ < 0 && !vis.empty()) sel_ = 0;
    scrollToSel_ = true;
}

void ListBrowser::Draw(int curId, const Fn& onClick, const Fn& onPick, const Fn& onHear) {
    if (wantOpen_) { ImGui::OpenPopup("##listBrowser"); wantOpen_ = false; open_ = true; }
    if (!open_) return;

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 sz(std::min(940.0f, io.DisplaySize.x - 60.0f), std::min(600.0f, io.DisplaySize.y - 60.0f));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
    bool keepOpen = true;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
    if (!ImGui::BeginPopupModal("##listBrowser", &keepOpen, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar)) {
        ImGui::PopStyleVar();
        open_ = false;
        return;
    }
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ---- top bar ----
    ImGui::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f), "%s", cfg_.title.c_str());
    ImGui::SameLine(0, 18);
    if (focusSearch_) { ImGui::SetKeyboardFocusHere(); focusSearch_ = false; }
    ImGui::SetNextItemWidth(250.0f);
    const bool searchChanged = ImGui::InputTextWithHint("##lbSearch", "search name, category, tag ...", search_, sizeof(search_));
    const bool searchActive = ImGui::IsItemActive();
    bool wantHear = false;   // the HEAR button, Space (while the search is empty) or AUTO-HEAR after a selection
    if (onHear && browser::HearKey(searchActive, search_)) wantHear = true;
    const bool typed = browser::CaptureTyping(search_, sizeof(search_), searchActive);
    ImGui::SameLine(0, 14);
    for (size_t t = 0; t < cfg_.tags.size() && t < 31; ++t) {
        if (t > 0) ImGui::SameLine(0, 4);
        char lab[48]; std::snprintf(lab, sizeof lab, "%s##tag%d", cfg_.tags[t].c_str(), static_cast<int>(t));
        if (ToggleButton(lab, (tagMask_ >> t) & 1u, ImVec2(0, 0), ToggleTab())) { tagMask_ ^= (1u << t); scrollToSel_ = true; }
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - 36.0f);
    if (ImGui::Button("X##lbClose", ImVec2(22, 0))) keepOpen = false;
    if (ImGui::IsItemHovered()) Hint("Close (Esc)");
    ImGui::Dummy(ImVec2(1, 2));

    const int nCats = NumCats();
    std::vector<int> counts(static_cast<size_t>(nCats), 0);
    for (const auto& it : items_) {
        counts[0]++;
        if (IsFav(it)) counts[1]++;
        for (int c = 2; c < nCats; ++c) if (it.category == cfg_.categories[static_cast<size_t>(c - 2)]) counts[static_cast<size_t>(c)]++;
    }

    const float footerH = 26.0f;
    const float detailH = cfg_.detail ? cfg_.detailH : 0.0f;
    const float bodyH = ImGui::GetContentRegionAvail().y - footerH - detailH;
    // ---- categories ----
    ImGui::BeginChild("##lbCats", ImVec2(150.0f, bodyH), false, ImGuiWindowFlags_NoScrollbar);
    dl = ImGui::GetWindowDrawList();
    for (int c = 0; c < nCats; ++c) {
        if (c == 2) ImGui::Dummy(ImVec2(1, 5));
        ImGui::PushID(c);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x, h = 26.0f;
        if (ImGui::InvisibleButton("##cat", ImVec2(w, h))) { cat_ = c; sel_ = 0; scrollToSel_ = true; }
        const bool hov = ImGui::IsItemHovered();
        const ImVec2 p1(p0.x + w, p0.y + h);
        if (cat_ == c) { dl->AddRectFilled(p0, p1, IM_COL32(242, 163, 61, 38), 4.0f); dl->AddRect(p0, p1, vis::kAccentDim, 4.0f); }
        else if (hov) dl->AddRectFilled(p0, p1, vis::kCellHover, 4.0f);
        const ImU32 dot = c == 1 ? IM_COL32(255, 214, 10, 255) : (c == 0 ? vis::kTextDim : PaletteColor(c - 2));
        dl->AddCircleFilled(ImVec2(p0.x + 12, p0.y + h * 0.5f), 4.0f, dot);
        dl->AddText(ImVec2(p0.x + 24, p0.y + 5), cat_ == c ? vis::kText : vis::kTextDim, CatName(c).c_str());
        char cnt[12]; std::snprintf(cnt, sizeof cnt, "%d", counts[static_cast<size_t>(c)]);
        dl->AddText(ImVec2(p1.x - 8 - ImGui::CalcTextSize(cnt).x, p0.y + 5), vis::kTextDim, cnt);
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine(0, 10);

    // ---- grid ----
    std::vector<int> visIdx = Visible();
    const int nVis = static_cast<int>(visIdx.size());
    if (nVis == 0) sel_ = -1; else sel_ = std::clamp(sel_, 0, nVis - 1);
    if (searchChanged || typed) { sel_ = nVis > 0 ? 0 : -1; scrollToSel_ = true; }

    const float cw = 170.0f, ch = 64.0f, gap = 8.0f;
    ImGui::BeginChild("##lbGrid", ImVec2(0, bodyH), false);
    dl = ImGui::GetWindowDrawList();
    const float availW = ImGui::GetContentRegionAvail().x;
    const int cols = std::max(1, static_cast<int>((availW + gap) / (cw + gap)));

    bool pick = false, moved = false;
    if (nVis > 0 && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        const int d = browser::NavDelta(cols, searchActive, search_);
        if (d != 0) { int ns = std::clamp((sel_ < 0 ? 0 : sel_) + d, 0, nVis - 1); if (ns != sel_ || sel_ < 0) { sel_ = ns; moved = true; scrollToSel_ = true; } }
        if (interaction::KeyConfirm()) pick = true;
    }
    if (moved && cfg_.liveSelect && sel_ >= 0) onClick(items_[static_cast<size_t>(visIdx[static_cast<size_t>(sel_)])]);
    if (moved && autoHear_) wantHear = true;

    const ImVec2 origin = ImGui::GetCursorPos();
    bool favToggled = false;
    for (int i = 0; i < nVis; ++i) {
        const Item& it = items_[static_cast<size_t>(visIdx[static_cast<size_t>(i)])];
        const int col = i % cols, row = i / cols;
        ImGui::SetCursorPos(ImVec2(origin.x + col * (cw + gap), origin.y + row * (ch + gap)));
        ImGui::PushID(it.id);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 p1(p0.x + cw, p0.y + ch);
        ImGui::InvisibleButton("##card", ImVec2(cw, ch));
        const bool hov = ImGui::IsItemHovered();
        const ImVec2 starC(p1.x - 14, p0.y + 14);
        const ImVec2 mp = io.MousePos;
        const bool onStar = hov && std::fabs(mp.x - starC.x) < 11 && std::fabs(mp.y - starC.y) < 11;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            if (onStar) { if (!favs_.erase(it.favKey)) favs_.insert(it.favKey); favToggled = true; }
            else { sel_ = i; onClick(it); if (autoHear_) wantHear = true; }
        }
        if (hov && !onStar && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) pick = true;
        const bool isCur = it.id == curId, isSel = i == sel_;
        dl->AddRectFilled(p0, p1, hov ? vis::kCellHover : vis::kCell, 6.0f);
        dl->AddRect(p0, p1, isCur ? vis::kAccent : (isSel ? vis::kLineHi : vis::kLine), 6.0f, 0, isCur ? 2.0f : 1.0f);
        if (isSel) dl->AddRect(ImVec2(p0.x - 2, p0.y - 2), ImVec2(p1.x + 2, p1.y + 2), vis::kAccentDim, 8.0f);
        int catIdx = -1;
        for (size_t c = 0; c < cfg_.categories.size(); ++c) if (cfg_.categories[c] == it.category) catIdx = static_cast<int>(c);
        dl->AddCircleFilled(ImVec2(p0.x + 11, p0.y + 15), 3.5f, catIdx >= 0 ? PaletteColor(catIdx) : vis::kTextDim);
        ImGui::PushClipRect(ImVec2(p0.x + 20, p0.y), ImVec2(p1.x - 28, p1.y), true);
        dl->AddText(ImVec2(p0.x + 20, p0.y + 7), vis::kText, it.name.c_str());
        ImGui::PopClipRect();
        const bool fav = IsFav(it);
        browser::DrawStar(dl, starC, fav, onStar, vis::kText);
        browser::DrawTags(dl, p0, p1, it.tags, vis::kTextDim);
        if (hov) {
            std::string tip = it.name;
            if (!it.category.empty()) tip += "  -  " + it.category;
            tip += onStar ? "\nclick: toggle favorite" : ("\n" + cfg_.footer);
            Hint("%s", tip.c_str());
        }
        if (isSel && scrollToSel_) ImGui::SetScrollHereY(0.5f);
        ImGui::PopID();
    }
    if (nVis > 0) { ImGui::SetCursorPos(ImVec2(origin.x, origin.y + ((nVis + cols - 1) / cols) * (ch + gap))); ImGui::Dummy(ImVec2(1, 1)); }
    else {
        ImGui::SetCursorPos(ImVec2(origin.x + 20, origin.y + 30));
        ImGui::TextDisabled("%s", cat_ == 1 && search_[0] == '\0' && !tagMask_ ? "No favorites yet - click the star on any card." : "Nothing matches - clear the search or the tag chips.");
    }
    scrollToSel_ = false;
    ImGui::EndChild();
    if (favToggled) SaveFavorites();
    if (detailH > 0.0f) {   // v44.49: the strip for the selected card
        ImGui::BeginChild("##lbDetail", ImVec2(0, detailH), true, ImGuiWindowFlags_NoScrollbar);
        const int selId = (sel_ >= 0 && sel_ < nVis) ? items_[static_cast<size_t>(visIdx[static_cast<size_t>(sel_)])].id : curId;
        cfg_.detail(selId, ImGui::GetContentRegionAvail().x);
        ImGui::EndChild();
    }

    ImGui::Dummy(ImVec2(1, 2));
    ImGui::TextDisabled("%d of %d   |   %s   |   Esc = %s   star = favorite", nVis, counts[0], cfg_.footer.c_str(), search_[0] ? "clear search" : "close");
    if (onHear) {   // v44.2: listen without picking (selecting is silent); AUTO plays every card you click or browse to
        ImGui::SameLine(ImGui::GetWindowWidth() - 14.0f - 130.0f);
        if (ImGui::Button("HEAR##lbHear", ImVec2(60, 0))) wantHear = true;
        if (ImGui::IsItemHovered()) Hint("Play the selected card without picking it (Space)");
        ImGui::SameLine(0, 4);
        if (ToggleButton("AUTO##lbAuto", autoHear_, ImVec2(60, 0), ToggleTab())) autoHear_ = !autoHear_;
        if (ImGui::IsItemHovered()) Hint("AUTO-HEAR: play every card you click or browse to.\nOff by default, so a live set stays silent while you pick.");
    }
    if (wantHear && onHear && sel_ >= 0 && sel_ < nVis) onHear(items_[static_cast<size_t>(visIdx[static_cast<size_t>(sel_)])]);

    if (browser::EscapeClosesOrClears(search_, &sel_, &scrollToSel_)) keepOpen = false;
    if (pick && sel_ >= 0 && sel_ < nVis) { onPick(items_[static_cast<size_t>(visIdx[static_cast<size_t>(sel_)])]); keepOpen = false; }
    else if (pick) keepOpen = false;
    if (!keepOpen) { ImGui::CloseCurrentPopup(); open_ = false; }
    ImGui::EndPopup();
}

} // namespace t8::desktop
