#include "GroovePanel.h"
#include "LiveWidgets.h"
#include "ValueField.h"
#include "core/Grooves.h"
#include "StepGlyphs.h"
#include "TrackStyle.h"
#include "Visual.h"
#include "imgui.h"
#include "InfoLine.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace t8::desktop {

void GroovePanel::Draw(AppState& app, const LiveLayout& layout) {
    SceneLock lock(app.sequencer.PatternMutex(), "GROOVE");
    Pattern& pat = app.CurrentPattern();
    const int ti = std::clamp(app.seqCursorTrack, 0, kNumTracks - 1);
    Track& cur = app.StepTrack(pat, ti);
    const ImVec4 amber(0.95f, 0.64f, 0.24f, 1.0f);

    ImGui::PushID("GroovePanel");
    const ImVec2 origin = ImGui::GetCursorPos();
    const float innerW = ImGui::GetContentRegionAvail().x;
    const float areaH = ImGui::GetContentRegionAvail().y;
    const ImVec2 win = ImGui::GetWindowPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float x0 = origin.x + 6.0f;

    static std::vector<Groove> customs;
    static double lastScan = -10.0;
    if (ImGui::GetTime() - lastScan > 2.0) { customs = ListCustomGrooves(); lastScan = ImGui::GetTime(); }

    // ---- title row ------------------------------------------------------------------------------
    ImGui::SetCursorPos(ImVec2(origin.x, origin.y + (layout.headerH - ImGui::GetFrameHeight()) * 0.5f));
    if (ImGui::Button("X  CLOSE")) app.grooveViewOpen = false;
    if (ImGui::IsItemHovered()) Hint("Close the groove window and bring the mixer back");
    ImGui::SameLine(0, 8);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(amber, "GROOVE");
    ImGui::SameLine(0, 8);
    ImGui::TextDisabled("of the whole pattern:  1 choose  -  2 amount  -  3 bake");

    // ---- row 1: 1 CHOOSE, 2 AMOUNT, 3 BAKE ------------------------------------------------------------
    ImGui::SetCursorPos(ImVec2(x0, origin.y + layout.headerH + 6.0f));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("1 CHOOSE");
    ImGui::SameLine(0, 6);
    ImGui::SetNextItemWidth(210.0f);
    const std::string curName = app.stagedGroove.name.empty() ? (app.stagedGroove.IsFlat() ? "NONE" : "(edited)") : app.stagedGroove.name;
    if (ChoiceBegin("##groovepick", curName.c_str(), ImGuiComboFlags_HeightLargest)) {
        if (ImGui::Selectable("NONE", app.stagedGroove.IsFlat() && app.stagedGroove.name.empty())) app.stagedGroove = Groove{};
        for (const Groove& g : BuiltinGrooves())
            if (ImGui::Selectable(g.name.c_str(), app.stagedGroove.name == g.name)) app.StageGroove(g);
        if (!customs.empty()) ImGui::Separator();
        for (const Groove& g : customs) {
            const std::string lbl = g.name + "  (SAVED)";
            if (ImGui::Selectable(lbl.c_str(), app.stagedGroove.name == g.name)) app.StageGroove(g);
        }
        ChoiceEnd();
    }
    if (ImGui::IsItemHovered()) Hint("Pick a groove: its shape appears below. Nothing changes in the pattern until you press BAKE.");
    ImGui::SameLine(0, 14);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("2 AMOUNT");
    ImGui::SameLine(0, 6);
    {
        float amt = static_cast<float>(std::lround(app.stagedAmount * 100.0f));
        ValueSpec sp; sp.lo = 0.0f; sp.hi = 100.0f; sp.step = 0.5f; sp.wheel = 1.0f; sp.def = 100.0f; sp.integer = true; sp.unit = "%";
        if (ValueField("##grooveamt", &amt, sp, std::max(90.0f, innerW - 540.0f)).changed) app.stagedAmount = std::clamp(amt, 0.0f, 100.0f) / 100.0f;
    }
    if (ImGui::IsItemHovered()) Hint("How much of the groove is written in when you press BAKE (100 %% = the full shape).");
    ImGui::SameLine(0, 14);
    const bool canBake = !app.stagedGroove.IsFlat() && app.stagedAmount > 0.0f;
    ImGui::PushStyleColor(ImGuiCol_Button, canBake ? ImVec4(0.78f, 0.47f, 0.12f, 1.0f) : ImVec4(0.20f, 0.21f, 0.27f, 1.0f));
    if (ImGui::Button("3 BAKE", ImVec2(76, 0))) app.BakeStagedGroove();
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) Hint("Write the groove (at this amount) into the velocity and nudge of the steps of the tracks marked below.\nThis is what makes it take effect. Undoable.");

    // ---- row 2: take a feel from a track ------------------------------------------------------------
    ImGui::SetCursorPosX(x0);
    if (ImGui::Button("SAVE FROM TRACK")) { std::snprintf(grooveName_, sizeof(grooveName_), "%s GROOVE", TrackDisplayName(cur).c_str()); ImGui::OpenPopup("GrooveSaveName"); }
    if (ImGui::IsItemHovered()) Hint("Capture the timing and velocity of the steps of the selected track (%s) as a new groove in the list.", TrackDisplayName(cur).c_str());
    if (NamePrompt("GrooveSaveName", grooveName_, sizeof(grooveName_), "GROOVE NAME")) {
        Groove g = GrooveFromTrack(cur, pat, cur.useOwnLength ? cur.length : pat.masterLength, SanitizeGrooveName(grooveName_));
        if (SaveCustomGroove(g)) { app.StageGroove(g); lastScan = -10.0; app.SetStatus("GROOVE SAVED: " + g.name); }
    }
    ImGui::SameLine(0, 4);
    if (ImGui::Button("USE TRACK'S FEEL")) app.StageGroove(GrooveFromTrack(cur, pat, cur.useOwnLength ? cur.length : pat.masterLength, "FROM " + TrackDisplayName(cur)));
    if (ImGui::IsItemHovered()) Hint("Choose the feel of the selected track (%s) as the groove (without saving it to the list). BAKE applies it.", TrackDisplayName(cur).c_str());

    // ---- row 3: which tracks the bake writes into ---------------------------------------------------
    ImGui::Dummy(ImVec2(1, 2));
    ImGui::SetCursorPosX(x0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("BAKE INTO");
    for (int i = 0; i < kNumTracks; ++i) {
        ImGui::SameLine(0, i == 0 ? 8 : 3);
        Track& tt = app.StepTrack(pat, i);
        std::string nm = TrackDisplayName(tt);
        std::string shortName = (nm.size() > 3 && std::isdigit(static_cast<unsigned char>(nm.back()))) ? nm.substr(0, 2) + nm.back() : nm.substr(0, 3);
        char lb[16]; std::snprintf(lb, sizeof(lb), "%s##g%d", shortName.c_str(), i);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(tt.grooveOn ? TrackColor(i) : IM_COL32(110, 114, 130, 255)));
        if (ToggleButton(lb, tt.grooveOn, ImVec2(36, 0), ToggleTab())) { app.PushUndo(); tt.grooveOn = !tt.grooveOn; }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) Hint("%s %s the groove", TrackDisplayName(tt).c_str(), tt.grooveOn ? "gets" : "does not get");
    }

    // ---- the groove itself: velocity (top) and timing (bottom), drawn with the mouse ------------------
    ImGui::Dummy(ImVec2(1, 6));
    const float cellW = std::max(8.0f, CellWidthFor(innerW - 12.0f, 0.0f, 2.0f, 6.0f, 16, 4));
    const PageGeom geom{x0, cellW, 2.0f, 6.0f, 0, 16, 4};
    const float pTop = ImGui::GetCursorPosY();
    const float pH = std::max(90.0f, origin.y + areaH - pTop - 40.0f);
    const float vH = pH * 0.58f, tH = pH - vH - 6.0f;
    const float L = win.x + x0 - 4.0f, R = win.x + geom.X(15) + cellW + 4.0f;
    const float vT = win.y + pTop - ImGui::GetScrollY(), vB = vT + vH, tT = vB + 6.0f, tB = tT + tH;
    dl->AddRectFilled(ImVec2(L, vT), ImVec2(R, vB), vis::kInset, 4.0f);
    dl->AddRectFilled(ImVec2(L, tT), ImVec2(R, tB), vis::kInset, 4.0f);
    dl->AddLine(ImVec2(L + 2, (vT + vB) * 0.5f), ImVec2(R - 2, (vT + vB) * 0.5f), vis::kLineHi, 1.0f);
    dl->AddLine(ImVec2(L + 2, (tT + tB) * 0.5f), ImVec2(R - 2, (tT + tB) * 0.5f), vis::kLineHi, 1.0f);
    // beat separators
    for (int c = 4; c < 16; c += 4) {
        const float xs = win.x + geom.X(c) - 3.0f;
        dl->AddLine(ImVec2(xs, vT), ImVec2(xs, vB), vis::kCellHover, 1.0f);
        dl->AddLine(ImVec2(xs, tT), ImVec2(xs, tB), vis::kCellHover, 1.0f);
    }
    dl->AddText(ImVec2(L + 4, vT + 1), IM_COL32(150, 154, 170, 200), "VELOCITY  (up = louder)");
    dl->AddText(ImVec2(L + 4, tT + 1), IM_COL32(150, 154, 170, 200), "TIMING  (up = later)");

    ImGui::SetCursorPos(ImVec2(x0 - 4.0f, pTop));
    ImGui::InvisibleButton("##groovearea", ImVec2(R - L, pH));
    const ImGuiIO& io = ImGui::GetIO();
    const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    auto colAt = [&](float mx) { return std::clamp(geom.ColAtX(mx - win.x), 0, 15); };
    Groove& g = app.stagedGroove;
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { drag_ = true; dragUndone_ = false; }
    if (drag_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const int c = colAt(io.MousePos.x);
        if (io.MousePos.y >= vT - 2 && io.MousePos.y <= vB + 2) {
            const float f = 1.0f - (io.MousePos.y - vT) / vH;                       // 0 bottom .. 1 top
            g.steps[c].vel = static_cast<int8_t>(std::clamp(static_cast<int>(std::lround((f - 0.5f) * 2.0f * 64.0f)), -64, 64));
            if (std::abs(g.steps[c].vel) <= 2) g.steps[c].vel = 0;
            dragUndone_ = true;
        } else if (io.MousePos.y >= tT - 2 && io.MousePos.y <= tB + 2) {
            const float f = 1.0f - (io.MousePos.y - tT) / tH;
            g.steps[c].nudge = static_cast<int8_t>(std::clamp(static_cast<int>(std::lround((f - 0.5f) * 2.0f * 50.0f)), -50, 50));
            if (std::abs(g.steps[c].nudge) <= 2) g.steps[c].nudge = 0;
            dragUndone_ = true;
        }
        if (g.name.size() && g.name.back() != '*') g.name += "*";
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag_ = false;
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { g.steps[colAt(io.MousePos.x)] = GrooveStep{}; }

    for (int c = 0; c < 16; ++c) {
        const float x = win.x + geom.X(c), xm = x + cellW * 0.5f;
        const GrooveStep& gs = g.steps[c];
        const float vmid = (vT + vB) * 0.5f, tmid = (tT + tB) * 0.5f;
        const float vy = vmid - (vH * 0.5f) * (gs.vel / 64.0f);
        dl->AddRectFilled(ImVec2(x, std::min(vy, vmid)), ImVec2(x + cellW, std::max(vy, vmid) + (gs.vel == 0 ? 1.5f : 0.0f)), ColorAlpha(IM_COL32(240, 163, 61, 255), gs.vel == 0 ? 90 : 235), 2.0f);
        const float ty = tmid - (tH * 0.5f) * (gs.nudge / 50.0f);
        dl->AddRectFilled(ImVec2(x, std::min(ty, tmid)), ImVec2(x + cellW, std::max(ty, tmid) + (gs.nudge == 0 ? 1.5f : 0.0f)), ColorAlpha(IM_COL32(95, 208, 240, 255), gs.nudge == 0 ? 90 : 235), 2.0f);
        char b[6]; std::snprintf(b, sizeof(b), "%d", c + 1);
        const ImVec2 ts = ImGui::CalcTextSize(b);
        dl->AddText(ImVec2(xm - ts.x * 0.5f, tB + 2.0f), (c % 4 == 0) ? IM_COL32(190, 194, 208, 230) : IM_COL32(110, 114, 130, 200), b);
    }
    ImGui::SetCursorPos(ImVec2(x0, pTop + pH + 20.0f));
    ImGui::TextDisabled("Drag in the top box = velocity, in the bottom box = timing. Right-click a step = flat. One bar of 16ths.");
    ImGui::PopID();
}

} // namespace t8::desktop
