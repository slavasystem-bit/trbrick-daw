#pragma once
// v44.31 (U7): the parts every list browser shares, in one place, so the synth PresetBrowser and the ListBrowser (kits, beats) cannot drift apart:
//   typing anywhere = search      Space (empty search) = hear      arrows / PageUp / PageDown = move      Return = pick      Esc = clear search, then close
//   cards: a favourite star and the tag chips
#include "imgui.h"
#include "Interaction.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <string>

namespace t8::desktop::browser {

// Characters typed while the search field is not active go into the search (no need to click it first). Space on an empty search is the
// HEAR key and never becomes a character. Returns true when the search text changed.
inline bool CaptureTyping(char* search, size_t cap, bool searchActive) {
    ImGuiIO& io = ImGui::GetIO();
    bool typed = false;
    if (searchActive || !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return false;
    for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
        const ImWchar c = io.InputQueueCharacters[i];
        if (c == ' ' && search[0] == '\0') continue;
        const size_t len = std::strlen(search);
        if (c >= 32 && c < 127 && len + 1 < cap) { search[len] = static_cast<char>(c); search[len + 1] = '\0'; typed = true; }
    }
    io.InputQueueCharacters.resize(0);
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && search[0]) { search[std::strlen(search) - 1] = '\0'; typed = true; }
    return typed;
}

// Space (not while typing a search) = hear the selected card.
inline bool HearKey(bool searchActive, const char* search) {
    return !searchActive && search[0] == '\0' && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Space, false);
}

// How far the arrow keys move the selection in a grid of `cols` columns (0 = no key). Left / Right stay with the search field while it has text.
inline int NavDelta(int cols, bool searchActive, const char* search) {
    int d = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) d = cols;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) d = -cols;
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) d = cols * 3;
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) d = -cols * 3;
    if (!searchActive || search[0] == '\0') {
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) d = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) d = -1;
    }
    return d;
}

// Esc: first clears a search, then closes. Returns true when the browser should close.
inline bool EscapeClosesOrClears(char* search, int* sel, bool* scrollToSel) {
    if (!ImGui::IsKeyPressed(ImGuiKey_Escape, false)) return false;
    if (search[0] != '\0') { search[0] = '\0'; *sel = 0; *scrollToSel = true; return false; }
    return true;
}

// The favourite star of a card: an outline when off, filled when on.
inline void DrawStar(ImDrawList* dl, ImVec2 c, bool fav, bool hot, ImU32 hotCol) {
    const ImU32 sc = fav ? IM_COL32(255, 214, 10, 255) : (hot ? hotCol : IM_COL32(110, 114, 130, 255));
    ImVec2 pts[10];
    for (int k = 0; k < 10; ++k) { const float r = (k % 2 == 0) ? 7.0f : 3.0f; const float a = -1.5707963f + k * 0.62831853f; pts[k] = ImVec2(c.x + r * std::cos(a), c.y + r * std::sin(a)); }
    if (fav) dl->AddConvexPolyFilled(pts, 10, sc); else dl->AddPolyline(pts, 10, sc, ImDrawFlags_Closed, 1.2f);
}

// The tag chips along the bottom of a card.
inline void DrawTags(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const std::vector<std::string>& tags, ImU32 textCol) {
    float tx = p0.x + 10;
    for (const auto& t : tags) {
        const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
        if (tx + ts.x + 10 > p1.x - 6) break;
        dl->AddRectFilled(ImVec2(tx, p1.y - 22), ImVec2(tx + ts.x + 8, p1.y - 6), IM_COL32(255, 255, 255, 14), 3.0f);
        dl->AddText(ImVec2(tx + 4, p1.y - 22 + (16 - ts.y) * 0.5f), textCol, t.c_str());
        tx += ts.x + 12;
    }
}

} // namespace t8::desktop::browser
