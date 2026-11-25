#include "PresetBrowser.h"
#include "Visual.h"
#include "BrowserCore.h"
#include "SynthSync.h"
#include "LiveWidgets.h"
#include "core/SynthPresets.h"
#include "core/CustomSynthPresets.h"
#include "imgui.h"
#include "core/UserDirs.h"
#include "InfoLine.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sys/stat.h>

namespace t8::desktop {

namespace {
const char* const kTagNames[] = {"UNI", "SUB", "OSC2", "24dB", "ACID", "LFO", "FM", "WT"};
constexpr int kNumTags = 8;
constexpr int kNumCats = 2 + kNumSynthCategories + 1;   // ALL FAV <builtin categories> MY


std::string Lower(std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }

const char* CatName(int c) {
    if (c == 0) return "ALL";
    if (c == 1) return "FAVORITES";
    if (c == kNumCats - 1) return "MY PRESETS";
    return kSynthCategories[c - 2];
}
}

unsigned PresetBrowser::CategoryColor(const std::string& c) {
    if (c == "BASS") return vis::kAccent;
    if (c == "LEAD") return vis::kTeal;
    if (c == "PAD") return vis::kViolet;
    if (c == "KEYS") return IM_COL32(122, 170, 255, 255);
    if (c == "PLUCK") return IM_COL32(154, 220, 120, 255);
    if (c == "FX") return vis::kRed;
    return vis::kText;
}

std::vector<std::string> PresetBrowser::TagsOf(const SynthPatch& p) { return PresetTags(p); }

void PresetBrowser::LoadFavorites() {
    if (favsLoaded_) return;
    favsLoaded_ = true;
    std::ifstream f(userdirs::FavoritesDir() + "/favorites.txt");
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) favs_.insert(line);
    }
}

void PresetBrowser::SaveFavorites() const {
    mkdir(userdirs::FavoritesDir().c_str(), 0755);
    std::ofstream f(userdirs::FavoritesDir() + "/favorites.txt", std::ios::trunc);
    for (const auto& k : favs_) f << k << "\n";
}

void PresetBrowser::Refresh(AppState& app) {
    LoadFavorites();
    const auto& builtins = AllSynthPresets();
    const auto& customs = app.CustomSynthPresetNamesCached();
    items_.clear();
    for (size_t i = 0; i < builtins.size(); ++i) {
        Item it; it.idx = static_cast<int>(i); it.name = builtins[i].name; it.category = builtins[i].category;
        it.tags = TagsOf(builtins[i].patch); it.favKey = it.name;
        items_.push_back(std::move(it));
    }
    for (size_t i = 0; i < customs.size(); ++i) {
        Item it; it.idx = static_cast<int>(builtins.size() + i); it.name = customs[i]; it.category = "MY"; it.custom = true;
        SynthPatch p; if (LoadCustomSynthPreset(customs[i], p)) it.tags = TagsOf(p);
        it.favKey = "my:" + it.name;
        items_.push_back(std::move(it));
    }
    customSeen_ = customs;
    itemsValid_ = true;
}

const PresetBrowser::Item* PresetBrowser::Find(AppState& app, int idx) {
    if (!itemsValid_ || customSeen_ != app.CustomSynthPresetNamesCached()) Refresh(app);
    for (const auto& it : items_) if (it.idx == idx) return &it;
    return nullptr;
}

std::vector<int> PresetBrowser::Visible(bool useSearch) const {
    std::vector<int> out;
    const std::string q = useSearch ? Lower(search_) : std::string();
    for (size_t i = 0; i < items_.size(); ++i) {
        const Item& it = items_[i];
        if (cat_ == 1 && !IsFav(it)) continue;
        if (cat_ >= 2 && it.category != (cat_ == kNumCats - 1 ? "MY" : kSynthCategories[cat_ - 2])) continue;
        if (useSearch) {
            bool ok = true;
            for (int t = 0; t < kNumTags; ++t) {
                if (!(tagMask_ & (1u << t))) continue;
                if (std::find(it.tags.begin(), it.tags.end(), kTagNames[t]) == it.tags.end()) { ok = false; break; }
            }
            if (!ok) continue;
            if (!q.empty()) {
                std::string hay = Lower(it.name + " " + it.category);
                for (const auto& t : it.tags) hay += " " + Lower(t);
                if (hay.find(q) == std::string::npos) continue;
            }
        }
        out.push_back(static_cast<int>(i));
    }
    return out;
}

int PresetBrowser::Step(AppState& app, int curIdx, int dir) {
    if (!itemsValid_ || customSeen_ != app.CustomSynthPresetNamesCached()) Refresh(app);
    auto vis = Visible(false);
    if (vis.empty()) return -1;
    int pos = -1;
    for (size_t i = 0; i < vis.size(); ++i) if (items_[vis[i]].idx == curIdx) { pos = static_cast<int>(i); break; }
    const int n = static_cast<int>(vis.size());
    pos = pos < 0 ? (dir > 0 ? 0 : n - 1) : ((pos + dir) % n + n) % n;
    return items_[vis[pos]].idx;
}

void PresetBrowser::Open(AppState& app, int ti, int curIdx) {
    Refresh(app);
    ti_ = ti; wantOpen_ = true; search_[0] = '\0'; tagMask_ = 0; focusSearch_ = true;
    if (cat_ == 1 && Visible(false).empty()) cat_ = 0;
    // start on the category of the current preset when browsing ALL
    sel_ = -1;
    auto vis = Visible(true);
    for (size_t i = 0; i < vis.size(); ++i) if (items_[vis[i]].idx == curIdx) { sel_ = static_cast<int>(i); break; }
    if (sel_ < 0 && !vis.empty()) sel_ = 0;
    scrollToSel_ = true;
}

void PresetBrowser::Audition(AppState& app, const Item& it) {
    StopAudition(app);
    if (ti_ < 0) return;
    const bool chord = IsChordTrack(ti_);
    if (chord) {
        const int8_t notes[3] = {static_cast<int8_t>(it.category == "BASS" ? 36 : 57), static_cast<int8_t>(it.category == "BASS" ? 43 : 60), static_cast<int8_t>(it.category == "BASS" ? 48 : 64)};
        app.engine.ChordNoteOn(notes, 3, 0.85f, false);
        auditionChord_ = true;
    } else {
        const int slot = SynthSlotForTrack(ti_);
        app.engine.SynthNoteOn(slot, it.category == "BASS" ? 36 : (it.category == "PAD" ? 57 : 60), 0.9f, false);
        auditionChord_ = false; auditionSlot_ = slot;
    }
    offAt_ = ImGui::GetTime() + (it.category == "PAD" || it.category == "FX" ? 1.4 : (it.category == "PLUCK" ? 0.5 : 0.8));
}

void PresetBrowser::StopAudition(AppState& app) {
    if (offAt_ <= 0.0) return;
    if (auditionChord_) app.engine.ChordNoteOff(); else app.engine.SynthNoteOff(auditionSlot_);
    offAt_ = 0.0;
}

void PresetBrowser::Draw(AppState& app, int curIdx, const ApplyFn& apply) {
    if (offAt_ > 0.0 && ImGui::GetTime() >= offAt_) StopAudition(app);
    if (wantOpen_) { ImGui::OpenPopup("##presetBrowser"); wantOpen_ = false; open_ = true; }
    if (!open_) return;
    if (!itemsValid_ || customSeen_ != app.CustomSynthPresetNamesCached()) Refresh(app);

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 sz(std::min(940.0f, io.DisplaySize.x - 60.0f), std::min(600.0f, io.DisplaySize.y - 60.0f));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
    bool keepOpen = true;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
    if (!ImGui::BeginPopupModal("##presetBrowser", &keepOpen, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar)) {
        ImGui::PopStyleVar();
        open_ = false; StopAudition(app);
        return;
    }
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool chord = IsChordTrack(ti_);
    std::string title = chord ? "CHORD SOUND PRESETS" : ("SYNTH " + std::to_string(SynthSlotForTrack(ti_) + 1) + " PRESETS");

    // ---- top bar: title, search, tag chips ----
    ImGui::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f), "%s", title.c_str());
    ImGui::SameLine(0, 18);
    if (focusSearch_) { ImGui::SetKeyboardFocusHere(); focusSearch_ = false; }
    ImGui::SetNextItemWidth(250.0f);
    const bool searchChanged = ImGui::InputTextWithHint("##pbSearch", "search name, category, tag ...", search_, sizeof(search_));
    const bool searchActive = ImGui::IsItemActive();
    // typing anywhere in the window goes to the search (no need to click the field first)
    bool wantHear = false;   // the HEAR button, Space (while the search is empty) or AUTO-HEAR after a selection
    if (browser::HearKey(searchActive, search_)) wantHear = true;
    const bool typed = browser::CaptureTyping(search_, sizeof(search_), searchActive);
    ImGui::SameLine(0, 14);
    for (int t = 0; t < kNumTags; ++t) {
        if (t > 0) ImGui::SameLine(0, 4);
        char lab[24]; std::snprintf(lab, sizeof lab, "%s##tag%d", kTagNames[t], t);
        if (ToggleButton(lab, (tagMask_ >> t) & 1u, ImVec2(0, 0), ToggleTab())) { tagMask_ ^= (1u << t); scrollToSel_ = true; }
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - 36.0f);
    if (ImGui::Button("X##pbClose", ImVec2(22, 0))) keepOpen = false;
    if (ImGui::IsItemHovered()) Hint("Close (Esc)");
    ImGui::Dummy(ImVec2(1, 2));

    // category counts
    int counts[kNumCats] = {};
    for (const auto& it : items_) {
        counts[0]++;
        if (IsFav(it)) counts[1]++;
        if (it.custom) counts[kNumCats - 1]++;
        else for (int c = 0; c < kNumSynthCategories; ++c) if (it.category == kSynthCategories[c]) counts[2 + c]++;
    }

    const float footerH = 26.0f;
    const float bodyH = ImGui::GetContentRegionAvail().y - footerH;
    // ---- left: categories ----
    ImGui::BeginChild("##pbCats", ImVec2(150.0f, bodyH), false, ImGuiWindowFlags_NoScrollbar);
    dl = ImGui::GetWindowDrawList();   // a child clips through its own draw list
    for (int c = 0; c < kNumCats; ++c) {
        if (c == 2 || c == kNumCats - 1) ImGui::Dummy(ImVec2(1, 5));
        ImGui::PushID(c);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x, h = 26.0f;
        if (ImGui::InvisibleButton("##cat", ImVec2(w, h))) { cat_ = c; sel_ = 0; scrollToSel_ = true; }
        const bool hov = ImGui::IsItemHovered();
        const ImVec2 p1(p0.x + w, p0.y + h);
        if (cat_ == c) { dl->AddRectFilled(p0, p1, IM_COL32(242, 163, 61, 38), 4.0f); dl->AddRect(p0, p1, vis::kAccentDim, 4.0f); }
        else if (hov) dl->AddRectFilled(p0, p1, vis::kCellHover, 4.0f);
        const std::string cname = (c >= 2 && c < kNumCats - 1) ? kSynthCategories[c - 2] : "";
        const ImU32 dot = c == 1 ? IM_COL32(255, 214, 10, 255) : (c == 0 ? vis::kTextDim : (c == kNumCats - 1 ? vis::kText : CategoryColor(cname)));
        dl->AddCircleFilled(ImVec2(p0.x + 12, p0.y + h * 0.5f), 4.0f, dot);
        dl->AddText(ImVec2(p0.x + 24, p0.y + 5), cat_ == c ? vis::kText : vis::kTextDim, CatName(c));
        char cnt[12]; std::snprintf(cnt, sizeof cnt, "%d", counts[c]);
        dl->AddText(ImVec2(p1.x - 8 - ImGui::CalcTextSize(cnt).x, p0.y + 5), vis::kTextDim, cnt);
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine(0, 10);

    // ---- right: the card grid ----
    std::vector<int> visIdx = Visible(true);
    const int nVis = static_cast<int>(visIdx.size());
    if (nVis == 0) sel_ = -1; else sel_ = std::clamp(sel_, 0, nVis - 1);
    if (searchChanged || typed) { sel_ = nVis > 0 ? 0 : -1; scrollToSel_ = true; }

    const float cw = 170.0f, ch = 64.0f, gap = 8.0f;
    ImGui::BeginChild("##pbGrid", ImVec2(0, bodyH), false);
    dl = ImGui::GetWindowDrawList();
    const float availW = ImGui::GetContentRegionAvail().x;
    const int cols = std::max(1, static_cast<int>((availW + gap) / (cw + gap)));

    // keyboard: arrows browse (apply + audition), Enter picks, Esc clears the search, then closes
    bool pick = false, moved = false;
    if (nVis > 0 && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        const int d = browser::NavDelta(cols, searchActive, search_);
        if (d != 0) { int ns = std::clamp((sel_ < 0 ? 0 : sel_) + d, 0, nVis - 1); if (ns != sel_ || sel_ < 0) { sel_ = ns; moved = true; scrollToSel_ = true; } }
        if (interaction::KeyConfirm()) pick = true;
    }
    if (moved && sel_ >= 0) { apply(items_[visIdx[sel_]].idx); if (autoHear_) wantHear = true; }

    const ImVec2 origin = ImGui::GetCursorPos();
    bool favToggled = false;
    for (int i = 0; i < nVis; ++i) {
        const Item& it = items_[visIdx[i]];
        const int col = i % cols, row = i / cols;
        ImGui::SetCursorPos(ImVec2(origin.x + col * (cw + gap), origin.y + row * (ch + gap)));
        ImGui::PushID(it.idx);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 p1(p0.x + cw, p0.y + ch);
        ImGui::InvisibleButton("##card", ImVec2(cw, ch));
        const bool hov = ImGui::IsItemHovered();
        const ImVec2 starC(p1.x - 14, p0.y + 14);
        const ImVec2 mp = ImGui::GetIO().MousePos;
        const bool onStar = hov && std::fabs(mp.x - starC.x) < 11 && std::fabs(mp.y - starC.y) < 11;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            if (onStar) { if (!favs_.erase(it.favKey)) favs_.insert(it.favKey); favToggled = true; }
            else { sel_ = i; apply(it.idx); if (autoHear_) wantHear = true; }
        }
        if (hov && !onStar && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) pick = true;
        const bool isCur = it.idx == curIdx, isSel = i == sel_;
        dl->AddRectFilled(p0, p1, hov ? vis::kCellHover : vis::kCell, 6.0f);
        dl->AddRect(p0, p1, isCur ? vis::kAccent : (isSel ? vis::kLineHi : vis::kLine), 6.0f, 0, isCur ? 2.0f : 1.0f);
        if (isSel) dl->AddRect(ImVec2(p0.x - 2, p0.y - 2), ImVec2(p1.x + 2, p1.y + 2), vis::kAccentDim, 8.0f);
        dl->AddCircleFilled(ImVec2(p0.x + 11, p0.y + 15), 3.5f, CategoryColor(it.category));
        ImGui::PushClipRect(ImVec2(p0.x + 20, p0.y), ImVec2(p1.x - 28, p1.y), true);
        dl->AddText(ImVec2(p0.x + 20, p0.y + 7), vis::kText, it.name.c_str());
        ImGui::PopClipRect();
        const bool fav = IsFav(it);
        // star: outline when off, filled when on
        browser::DrawStar(dl, starC, fav, onStar, vis::kText);
        // tags along the bottom
        browser::DrawTags(dl, p0, p1, it.tags, vis::kTextDim);
        if (hov) {
            std::string tip = it.name + "  -  " + it.category;
            if (!it.tags.empty()) { tip += "\n"; for (size_t k = 0; k < it.tags.size(); ++k) tip += (k ? "  " : "") + it.tags[k]; }
            tip += onStar ? "\nclick: toggle favorite" : "\nclick: select   Space: hear   double-click / Enter: pick";
            Hint("%s", tip.c_str());
        }
        if (isSel && scrollToSel_) { ImGui::SetScrollHereY(0.5f); }
        ImGui::PopID();
    }
    if (nVis > 0) { ImGui::SetCursorPos(ImVec2(origin.x, origin.y + ((nVis + cols - 1) / cols) * (ch + gap))); ImGui::Dummy(ImVec2(1, 1)); }
    else {
        ImGui::SetCursorPos(ImVec2(origin.x + 20, origin.y + 30));
        ImGui::TextDisabled("%s", cat_ == 1 && search_[0] == '\0' && !tagMask_ ? "No favorites yet - click the star on any card."
                                : (cat_ == kNumCats - 1 && search_[0] == '\0' && !tagMask_ ? "Nothing saved yet - tweak a sound and press SAVE in the PRESET block."
                                : "Nothing matches - clear the search or the tag chips."));
    }
    scrollToSel_ = false;
    ImGui::EndChild();
    if (favToggled) SaveFavorites();

    // ---- footer ----
    ImGui::Dummy(ImVec2(1, 2));
    ImGui::TextDisabled("%d of %d   |   click = select (silent)   arrows = browse   Enter / double-click = pick   Esc = %s   star = favorite",
                        nVis, counts[0], search_[0] ? "clear search" : "close");
    // v44.2: listen without picking (selecting is silent); AUTO plays every preset you click or browse to
    ImGui::SameLine(ImGui::GetWindowWidth() - 14.0f - 130.0f);
    if (ImGui::Button("HEAR##pbHear", ImVec2(60, 0))) wantHear = true;
    if (ImGui::IsItemHovered()) Hint("Play the selected preset without changing anything (Space)");
    ImGui::SameLine(0, 4);
    if (ToggleButton("AUTO##pbAuto", autoHear_, ImVec2(60, 0), ToggleTab())) autoHear_ = !autoHear_;
    if (ImGui::IsItemHovered()) Hint("AUTO-HEAR: play a note for every preset you click or browse to.\nOff by default, so a live set stays silent while you pick.");
    if (wantHear && sel_ >= 0 && sel_ < nVis) Audition(app, items_[visIdx[sel_]]);

    // Esc: first clears a search, then closes
    if (browser::EscapeClosesOrClears(search_, &sel_, &scrollToSel_)) keepOpen = false;
    if (pick) {
        if (sel_ >= 0 && sel_ < nVis && items_[visIdx[sel_]].idx != curIdx) apply(items_[visIdx[sel_]].idx);
        keepOpen = false;
    }
    if (!keepOpen) { ImGui::CloseCurrentPopup(); open_ = false; StopAudition(app); }
    ImGui::EndPopup();
}

} // namespace t8::desktop
