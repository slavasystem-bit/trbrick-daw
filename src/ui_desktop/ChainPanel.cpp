#include "ChainPanel.h"
#include "Visual.h"
#include "MixerSync.h"
#include "InstrumentMenu.h"
#include "LiveWidgets.h"
#include "ValueField.h"
#include "FillSampleRow.h"
#include "DynPresets.h"
#include "core/ArrangeEnergy.h"
#include "core/ArrangeAuto.h"
#include "imgui.h"
#include "InfoLine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

namespace t8::desktop {

namespace {
// C2 part 3b: a chord slot opens in CHORD LAB (the chord editor window is gone): the bank goes on, the slot is edited, the lab opens.
void OpenChordLabOn(AppState& app, int slot) {
    if (!app.project.chordBankOn) app.SetChordBank(true);
    app.EditChordSceneOnly(slot);
    app.OpenTool(AppState::Tool::ChordLab);
}
} // namespace


namespace {


ImU32 RoleColor(SceneRole r) {
    switch (r) {
    case SceneRole::Intro: return IM_COL32(100, 156, 224, 255);
    case SceneRole::Build: return IM_COL32(232, 202, 74, 255);
    case SceneRole::Drop:  return IM_COL32(238, 94, 70, 255);
    case SceneRole::Break: return IM_COL32(84, 204, 166, 255);
    case SceneRole::Outro: return IM_COL32(174, 126, 224, 255);
    default: return IM_COL32(0, 0, 0, 0);
    }
}

std::string FillSummary(const FillRule& r) {
    if (r.when == FillWhen::Off) return "no fill";
    char b[96];
    const std::string len = FillLenLabel(r.beats);
    if (r.when == FillWhen::EveryBars) std::snprintf(b, sizeof b, "%s, last %s of every %d bars", FillKindName(r.kind), len.c_str(), r.everyBars);
    else std::snprintf(b, sizeof b, "%s, last %s, %s", FillKindName(r.kind), len.c_str(), r.when == FillWhen::LastLoop ? "last pass" : "every pass");
    return b;
}

// The fill rule editor shared by the scene window and the chain step window. Returns true when something changed.
bool DrawFillRuleEditor(FillRule& r) {
    bool ch = false;
    ImGui::TextUnformatted("WHEN   ");
    for (int w = 0; w < kNumFillWhen; ++w) {
        ImGui::SameLine(0, 4);
        ImGui::PushID(w);
        if (ToggleButton(FillWhenName(static_cast<FillWhen>(w)), r.when == static_cast<FillWhen>(w), ImVec2(0, 0), ToggleTab())) { r.when = static_cast<FillWhen>(w); ch = true; }
        ImGui::PopID();
    }
    if (r.when == FillWhen::Off) return ch;
    if (r.when == FillWhen::EveryBars) {
        ImGui::TextUnformatted("EVERY  ");
        for (int b : kFillBarChoices) {
            ImGui::SameLine(0, 4);
            char lb[16]; std::snprintf(lb, sizeof lb, "%d BARS##eb%d", b, b);
            if (ToggleButton(lb, r.everyBars == b, ImVec2(0, 0), ToggleTab())) { r.everyBars = static_cast<uint8_t>(b); ch = true; }
        }
    }
    ImGui::TextUnformatted("LENGTH ");
    for (int b : kFillBeatChoices) {
        ImGui::SameLine(0, 4);
        char lb[24]; std::snprintf(lb, sizeof lb, "%s##fb%d", FillLenLabel(b, true).c_str(), b);
        if (ToggleButton(lb, r.beats == b, ImVec2(0, 0), ToggleTab())) { r.beats = static_cast<uint8_t>(b); ch = true; }
    }
    ImGui::TextUnformatted("TYPE   ");
    for (int k = 0; k < kNumFillKinds; ++k) {
        ImGui::SameLine(0, 4);
        ImGui::PushID(100 + k);
        if (ToggleButton(FillKindName(static_cast<FillKind>(k)), r.kind == static_cast<FillKind>(k), ImVec2(0, 0), ToggleTab())) { r.kind = static_cast<FillKind>(k); ch = true; }
        ImGui::PopID();
    }
    if (r.kind == FillKind::Break) {
        ImGui::TextUnformatted("BREAK  ");
        ImGui::SameLine(0, 4);
        ImGui::SetNextItemWidth(200);
        const auto& brk = AllDrumBreaks();
        const char* cur = r.breakIndex == 0 ? "RANDOM" : brk[std::min<size_t>(r.breakIndex - 1, brk.size() - 1)].name;
        if (ChoiceBegin("##brk", cur)) {
            if (ImGui::Selectable("RANDOM", r.breakIndex == 0)) { r.breakIndex = 0; ch = true; }
            for (size_t i = 0; i < brk.size(); ++i) if (ImGui::Selectable(brk[i].name, r.breakIndex == i + 1)) { r.breakIndex = static_cast<uint8_t>(i + 1); ch = true; }
            ChoiceEnd();
        }
    }
    return ch;
}

bool PatternHasContent(const Pattern& p) {
    for (const Track& t : p.tracks) {
        int len = t.useOwnLength ? t.length : p.masterLength;
        for (int i = 0; i < len; ++i) {
            if (t.steps[i].active) return true;
        }
    }
    return false;
}

void AddToChain(AppState& app, int patIdx) {   // v44.22: a COPY of the scene becomes the brick
    if (app.AddChainStep(patIdx)) app.SetStatus("SCENE " + std::to_string(patIdx + 1) + " COPIED ONTO THE TRACK (BRICK " + std::to_string(app.project.songChainLength) + ")");
}

void RemoveSlot(AppState& app, int idx) {
    Project& proj = app.project;
    if (idx < 0 || idx >= proj.songChainLength) return;
    app.PushSoundUndo();
    for (int i = idx; i < proj.songChainLength - 1; ++i) proj.songChain[i] = proj.songChain[i + 1];
    proj.songChainLength--;
}

bool InsertSlotCopy(AppState& app, int idx) {
    Project& proj = app.project;
    if (proj.songChainLength >= kMaxChainSteps || idx < 0 || idx >= proj.songChainLength) return false;
    app.PushSoundUndo();
    for (int i = proj.songChainLength; i > idx + 1; --i) proj.songChain[i] = proj.songChain[i - 1];
    proj.songChain[idx + 1] = proj.songChain[idx];
    proj.songChainLength++;
    return true;
}

int FindFreeSlotAfter(const Project& proj, int from) {
    for (int step = 1; step <= kNumLiveScenes; ++step) {
        int i = (from + step) % kNumLiveScenes;
        if (!PatternHasContent(proj.patterns[i])) return i;
    }
    return -1;
}

// Moves chain slot `from` so it ends up at index `to` (others shift).
void MoveSlot(AppState& app, int from, int to) {
    Project& proj = app.project;
    if (from < 0 || from >= proj.songChainLength) return;
    to = std::clamp(to, 0, proj.songChainLength - 1);
    if (from == to) return;
    app.PushSoundUndo();
    ChainStep tmp = proj.songChain[from];
    if (from < to) { for (int i = from; i < to; ++i) proj.songChain[i] = proj.songChain[i + 1]; }
    else           { for (int i = from; i > to; --i) proj.songChain[i] = proj.songChain[i - 1]; }
    proj.songChain[to] = tmp;
}

// Drag a scene pad onto another one: the two swap places (their content and
// every chain slot that refers to them). The edited scene follows its content.
void SwapScenes(AppState& app, int a, int b) {
    if (a == b || a < 0 || b < 0 || a >= kNumLiveScenes || b >= kNumLiveScenes) return;
    Project& proj = app.project;
    app.PushSoundUndo();   // (the swapped scene contents are not part of Undo; bricks keep their own copies, so the chain is untouched)
    std::swap(proj.patterns[a], proj.patterns[b]);
    if (proj.currentPattern == a) proj.currentPattern = b;
    else if (proj.currentPattern == b) proj.currentPattern = a;
    if (app.EditIsPlaying()) { app.engine.GetMixer().SyncFromPattern(app.CurrentPattern()); app.sequencer.ApplySceneSound(app.project.currentPattern); }
    app.SetStatus("SCENES " + std::to_string(a + 1) + " <-> " + std::to_string(b + 1) + " SWAPPED");
}

// "Delete scene": empties every step of the scene (bricks made from it keep their own copies).
void DeleteScene(AppState& app, int i) {
    if (i < 0 || i >= kNumLiveScenes) return;
    Project& proj = app.project;
    if (i == proj.currentPattern) app.PushUndo();
    for (Track& t : proj.patterns[i].tracks)
        for (Step& st : t.steps) st = Step{};
    app.SetStatus("SCENE " + std::to_string(i + 1) + " DELETED");
}

constexpr ImU32 kTeal = vis::kTeal;
constexpr ImU32 kAmber = vis::kAccent;

} // namespace

void ChainPanel::Draw(AppState& app) {
    SceneLock lock(app.sequencer.PatternMutex(), "TRACK STRIP");
    Project& proj = app.project;
    ImGui::PushID("ChainPanel");

    const float kLabelW = 58.0f, kCtrlW = 176.0f, kGap = 4.0f;
    const float kPadH = 28.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float gridW = avail.x - kLabelW - kCtrlW - 12.0f;
    float cellW = (gridW - kGap * 15) / 16.0f;
    float y0 = ImGui::GetCursorPosY();
    float x0 = ImGui::GetCursorPosX();
    bool songOn = app.sequencer.IsSongModeActive();

    // ---- SCENES row ------------------------------------------------------
    const bool coll = false;   // v44.12: the layout is static; the chord row is always there
    ImGui::SetCursorPos(ImVec2(x0, y0));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("SCENES");
    for (int i = 0; i < kNumLiveScenes; ++i) {
        ImGui::SetCursorPos(ImVec2(x0 + kLabelW + i * (cellW + kGap), y0));
        ImGui::PushID(i);
        bool active = (i == proj.currentPattern);
        bool hasContent = PatternHasContent(proj.patterns[i]);
        const bool selected = (app.sceneSel >> i) & 1u;   // v44.59: Shift+click / Cmd+click selection (the LINK button acts on it)

        ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImVec4(0.15f, 0.19f, 0.27f, 1.0f) : active ? ImVec4(0.23f, 0.17f, 0.08f, 1.0f) : ImVec4(0.12f, 0.13f, 0.16f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? ImVec4(0.19f, 0.24f, 0.33f, 1.0f) : active ? ImVec4(0.29f, 0.21f, 0.09f, 1.0f) : ImVec4(0.17f, 0.18f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, active ? ImVec4(0.95f, 0.64f, 0.24f, 1.0f) : selected ? ImVec4(0.50f, 0.66f, 1.0f, 1.0f) : ImVec4(0.18f, 0.19f, 0.24f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, active ? 2.0f : 1.0f);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%02d", i + 1);
        // v44.59: Shift+click = select a range, Cmd+click = select one more (the Mac rule); a plain click launches or opens (CLICK switch);
        // a double click always opens without launching. Linking is the LINK button / the right-click menu (one undo step each).
        const bool shiftDown = ImGui::GetIO().KeyShift, cmdDown = ImGui::GetIO().KeySuper || ImGui::GetIO().KeyCtrl;
        // ImGui's Button fires on every release, also on the one that ends a double click: ScenePadGesture drops that one (else
        // LAUNCH = BEAT / BAR would queue again the launch the double click just cancelled).
        const bool released = ImGui::Button(buf, ImVec2(cellW, kPadH));
        const bool dbl = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0);
        const AppState::PadGesture g = AppState::ScenePadGesture(released, dbl, ImGui::GetIO().MouseClickedLastCount[0], shiftDown, cmdDown);
        if (songOn && g == AppState::PadGesture::Click) { app.SelectScenePad(i, false, false); LaunchOrSwitchScene(app, i); }
        else if (songOn && g == AppState::PadGesture::DoubleClick) SwitchPattern(app, i);
        else app.ScenePadInput(i, g);
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            ImGui::SetDragDropPayload("T8_SCENE", &i, sizeof(int));
            ImGui::Text("Scene %02d", i + 1);
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("T8_SCENE")) {
                SwapScenes(app, *static_cast<const int*>(pl->Data), i);
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            Hint("Scene %02d%s%s\nClick: %s  -  Double-click: open without launching  -  Shift+click: select several (then LINK)  -  Drag onto another scene: swap  -  Right-click: menu",
                              i + 1, app.SceneGroupOf(i) ? "  -  linked " : "", app.LinkLabel(i).c_str(),
                              songOn ? "edit" : (app.clickOpens ? "open it (CLICK: OPEN); GO launches it" : (app.sequencer.LaunchQuantize() == 0 ? "launch" : "launch (see LAUNCH in the header)")));
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            padMenuPattern_ = i;
            padMenuOpenRequest_ = true;
        }
        if (app.sequencer.PendingScene() == i) {
            // queued launch: pulsing outline + countdown in steps
            ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 8.0f);
            ImGui::GetWindowDrawList()->AddRect(a, b, IM_COL32(255, 214, 140, static_cast<int>(120 + 135 * pulse)), 4.0f, 0, 2.5f);
            ImGui::GetWindowDrawList()->AddRectFilled(a, b, IM_COL32(242, 163, 61, 26), 4.0f);   // v44.40: a warm wash while it waits, the countdown is in beats
            char cd[16];
            std::snprintf(cd, sizeof(cd), "%.1f", app.sequencer.BeatsUntilLaunch());
            ImVec2 ts = ImGui::CalcTextSize(cd);
            ImGui::GetWindowDrawList()->AddText(ImVec2(b.x - ts.x - 3, a.y + 2), vis::kAccentHi, cd);
        }
        if (app.sequencer.IsPlaying() && i == app.sequencer.PlayPatternIndex()) {
            // playing marker (small triangle, teal) -- distinct from the amber "being edited" outline
            ImVec2 a = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddTriangleFilled(ImVec2(a.x + 4, a.y + 4), ImVec2(a.x + 4, a.y + 12), ImVec2(a.x + 11, a.y + 8), kTeal);
            const double sp = app.sequencer.SceneProgress();   // v44.40: how far through its loop the playing scene is
            if (sp >= 0.0) {
                const ImVec2 b = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x + 2, b.y - 5.0f), ImVec2(a.x + 2 + (b.x - a.x - 4) * static_cast<float>(sp), b.y - 2.0f), IM_COL32(95, 211, 138, 255), 1.5f);
            }
        }
        if (const int lg = app.SceneGroupOf(i); lg > 0) {   // v44.58: linked scenes: one coloured bar along the top, running through the gap to the next member
            static const ImU32 kLinkCol[6] = {IM_COL32(95, 211, 138, 255), IM_COL32(120, 170, 255, 255), IM_COL32(255, 150, 190, 255), IM_COL32(240, 205, 90, 255), IM_COL32(190, 140, 255, 255), IM_COL32(110, 220, 220, 255)};
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const bool nextSame = i + 1 < kNumLiveScenes && app.SceneGroupOf(i + 1) == lg;
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x, a.y), ImVec2(b.x + (nextSame ? kGap : 0.0f), a.y + 3.0f), kLinkCol[(lg - 1) % 6], 1.0f);
        }
        if (proj.patterns[i].role != SceneRole::None) {   // v44.5 role: a coloured bar along the top edge of the pad
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x + 4, a.y + 2), ImVec2(b.x - 4, a.y + 5), RoleColor(proj.patterns[i].role), 1.5f);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
                Hint("Scene %02d  -  %s%s%s\nClick: edit  -  Drag onto another scene: swap  -  Right-click: menu (role, fill)", i + 1, RoleName(proj.patterns[i].role),
                                  proj.patterns[i].fill.when != FillWhen::Off ? "  -  fill: " : "", proj.patterns[i].fill.when != FillWhen::Off ? FillSummary(proj.patterns[i].fill).c_str() : "");
        }
        if (hasContent) {   // v44.40: sixteen dots = what the scene plays across its loop (brighter = more instruments)
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const Pattern& sp = proj.patterns[static_cast<size_t>(i)];
            const int len = std::clamp(sp.masterLength, 1, kMaxSteps);
            const float dw = (b.x - a.x - 12.0f) / 16.0f;
            ImDrawList* ddl = ImGui::GetWindowDrawList();
            const bool pl = app.sequencer.IsPlaying() && i == app.sequencer.PlayPatternIndex();
            for (int k = 0; k < 16; ++k) {
                int cnt = 0;
                for (int st = k * len / 16; st < std::max(k * len / 16 + 1, (k + 1) * len / 16); ++st)
                    for (int t = 0; t < kNumTracks; ++t) if (sp.tracks[static_cast<size_t>(t)].steps[static_cast<size_t>(st)].active) ++cnt;
                const ImU32 c = cnt == 0 ? IM_COL32(35, 38, 48, 255) : (active || pl ? IM_COL32(230, 190, 120, static_cast<int>(110 + std::min(cnt, 4) * 36)) : IM_COL32(112, 120, 146, static_cast<int>(100 + std::min(cnt, 4) * 38)));
                ddl->AddRectFilled(ImVec2(a.x + 6.0f + k * dw, b.y - 8.0f), ImVec2(a.x + 6.0f + (k + 1) * dw - 1.0f, b.y - 6.0f), c, 1.0f);
            }
        }
        ImGui::PopID();
    }

    // ---- (v44.10) The chain itself is edited on the TRACK screen (BricksPanel); this panel keeps the scene pads and the chord bank. ----
    float rowY = y0 + kPadH + 6.0f;
    if (!coll) {
    // ---- CHORDS row: independent chord scenes (the "chord bank") ------------------
    {
        const float chordY = rowY;
        const float kChordH = 30.0f;
        const float sw = 2.0f * cellW + kGap;
        ImGui::SetCursorPos(ImVec2(x0, chordY + 7));
        ImGui::TextDisabled("CHORDS");
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const bool bankOn = proj.chordBankOn;
        for (int c = 0; c < kNumChordScenes; ++c) {
            ImGui::SetCursorPos(ImVec2(x0 + kLabelW + c * (sw + kGap), chordY));
            ImGui::PushID(2000 + c);
            const bool empty = app.ChordSceneEmpty(c);
            const bool editing = bankOn && proj.chordSceneEdit == c;
            const bool sounding = bankOn && app.sequencer.IsPlaying() && app.sequencer.ChordSceneCur() == c;
            const bool pending = bankOn && app.sequencer.PendingChordScene() == c;
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImVec2 p1(p0.x + sw, p0.y + kChordH);
            if (ImGui::InvisibleButton("##chordslot", ImVec2(sw, kChordH))) app.LaunchChordScene(c);
            const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { chordMenuSlot_ = c; chordMenuOpenRequest_ = true; }
            if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) OpenChordLabOn(app, c);
            if (hov && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
                Hint("Chord scene %d: %s\nClick: edit + launch%s  -  Double-click: CHORD LAB on this slot  -  Right-click: menu\nThe drums / bass scenes keep playing; chords change on the same boundary.",
                                  c + 1, empty ? "(empty)" : app.ChordSceneLabel(c).c_str(),
                                  app.sequencer.LaunchQuantize() == 0 ? "" : " (quantized, see LAUNCH)");
            ImU32 fill = sounding ? IM_COL32(44, 34, 70, 255) : (hov ? IM_COL32(34, 30, 52, 255) : IM_COL32(26, 24, 40, 255));
            dl->AddRectFilled(p0, p1, bankOn ? fill : IM_COL32(22, 22, 30, 255), 5.0f);
            ImU32 edge = editing ? kAmber : (sounding ? vis::kViolet : vis::kLine);
            dl->AddRect(p0, p1, bankOn ? edge : vis::kCellHover, 5.0f, 0, (editing || sounding) ? 2.0f : 1.0f);
            if (pending) {
                float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 8.0f);
                dl->AddRect(p0, p1, IM_COL32(220, 190, 255, static_cast<int>(120 + 135 * pulse)), 5.0f, 0, 2.5f);
                char cd[16]; std::snprintf(cd, sizeof(cd), "%d", app.sequencer.TicksUntilChordLaunch());
                ImVec2 ts = ImGui::CalcTextSize(cd);
                dl->AddText(ImVec2(p1.x - ts.x - 4, p0.y + 2), IM_COL32(235, 215, 255, 255), cd);
            }
            char num[16]; std::snprintf(num, sizeof(num), "%d", c + 1);
            dl->AddText(ImVec2(p0.x + 5, p0.y + 2), bankOn ? vis::kViolet : vis::kLineHi, num);
            std::string lbl = empty ? std::string("-") : app.ChordSceneLabel(c);
            while (lbl.size() > 1 && ImGui::CalcTextSize(lbl.c_str()).x > sw - 16.0f) lbl.pop_back();
            ImVec2 ts = ImGui::CalcTextSize(lbl.c_str());
            dl->AddText(ImVec2(p0.x + (sw - ts.x) * 0.5f, p0.y + (kChordH - ts.y) * 0.5f + 4.0f),
                        !bankOn ? vis::kLineHi : (empty ? IM_COL32(110, 114, 132, 255) : vis::kText), lbl.c_str());
            if (app.ChordLabOpen() && editing) {   // v44.43: the slot the CHORD LAB jam lives in
                dl->AddRect(p0, p1, vis::kTeal, 5.0f, 0, 2.0f);
                dl->AddText(ImVec2(p0.x + 20, p0.y + 2), vis::kTeal, "DRAFT");
            }
            if (sounding) dl->AddTriangleFilled(ImVec2(p0.x + 5, p0.y + 16), ImVec2(p0.x + 5, p0.y + 24), ImVec2(p0.x + 11, p0.y + 20), vis::kViolet);
            ImGui::PopID();
        }
    }

    }   // !coll

    // ---- right-hand controls -------------------------------------------
    float cx = x0 + kLabelW + 16 * (cellW + kGap) + 8.0f;
    {   // v44.12: THROW ONTO THE TRACK, the way from jamming to the structure of the track: the scene becomes a brick and TRACK opens
        const bool empty = app.CurrentPatternIsEmpty();
        ImGui::SetCursorPos(ImVec2(cx, y0));
        ImGui::BeginDisabled(empty);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.86f, 0.50f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.96f, 0.60f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.76f, 0.42f, 0.08f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.10f, 0.07f, 0.03f, 1.0f));
        if (ImGui::Button("THROW ONTO THE TRACK", ImVec2(kCtrlW, kPadH))) {
            if (app.ThrowIntoChain()) { app.selBrick = std::max(0, proj.songChainLength - 1); app.uiTabRequest = 1; }
        }
        ImGui::PopStyleColor(4);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            Hint(empty ? "Make a loop first (click the squares), then throw it into the track." : "Happy with this loop? It becomes the next brick of the track,\nand the TRACK screen opens to build the track from the bricks.");
    }
    if (app.sequencer.PendingScene() >= 0 && app.sequencer.IsPlaying()) {   // v44.40: what is waiting, and for how long
        const char* qn = app.sequencer.LaunchQuantize() == 1 ? "BEAT" : "BAR";
        const double beats = app.sequencer.BeatsUntilLaunch();
        const double q = app.sequencer.LaunchQuantumBeats();
        ImGui::SetCursorPos(ImVec2(cx - 360.0f, rowY + 34.0f));
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        const ImVec2 c(o.x + 9.0f, o.y + 9.0f);
        cdl->AddCircle(c, 7.0f, IM_COL32(58, 61, 73, 255), 24, 2.4f);
        const float f = static_cast<float>(std::clamp(1.0 - beats / std::max(0.25, q), 0.0, 1.0));
        cdl->PathArcTo(c, 7.0f, -1.5708f, -1.5708f + 6.2832f * f, 24);
        cdl->PathStroke(vis::kAccent, 0, 2.4f);
        char tb[96];
        std::snprintf(tb, sizeof(tb), "SCENE %02d launches in %.1f beats (%s)", app.sequencer.PendingScene() + 1, beats, qn);
        cdl->AddText(ImVec2(o.x + 24.0f, o.y + 2.0f), IM_COL32(190, 194, 208, 255), tb);
    }
    {
        ImGui::SetCursorPos(ImVec2(cx, rowY));
        if (ToggleButton(proj.chordBankOn ? "BANK: ON" : "BANK: OFF", proj.chordBankOn, ImVec2(kCtrlW - 92.0f, 30.0f), ToggleTeal()))
            app.SetChordBank(!proj.chordBankOn);
        if (ImGui::IsItemHovered())
            Hint("ON: the chord track plays the chord scenes (1-8) instead of each scene's own chords,\nso you can change the chords without touching the drums / bass - and the other way round.\nOFF: every scene plays its own chords.");
        ImGui::SameLine(0, 4);
        if (ToggleButton("CHORD LAB", app.ChordLabOpen(), ImVec2(88.0f, 30.0f), ToggleTab())) app.ToggleTool(AppState::Tool::ChordLab);   // C1: the same as key C
        if (ImGui::IsItemHovered())
            Hint("CHORD LAB: jam with chords over the running scene. A palette of the chords of the key (with borrowed ones), a fit against your bass / synth notes, a loop to build in, and the reasons.\nThe jam is a draft in a chord slot; ADD TO SCENE and THROW ONTO THE TRACK take it on.");
    }
    {   // v44.23: the sound of the track. A thrown brick keeps the sound the track has; this takes the sound of the scene you are on instead.
        ImGui::SetCursorPos(ImVec2(cx, rowY + 34.0f));
        const int shown = AppState::IsBrickSlot(proj.currentPattern) ? proj.liveScene : proj.currentPattern;
        if (MiniButton("TAKE SOUND TO TRACK", kCtrlW)) app.TakeSceneSoundToTrack(shown);
        if (ImGui::IsItemHovered())
            Hint("Makes the sound of this scene (synths, chord, kick, drum voices, kit) the sound of the WHOLE track: every brick switches to it.\nThe notes and the brick settings stay. Undoable.\nWithout this a thrown brick keeps the sound the track already has, so you can jam with other sounds here.");
    }

    {   // v44.59: the controls of the SCENES row (no separate PADS row): CLICK: LAUNCH | OPEN, GO, and the LINK action
        // Every item of the row is placed on the same top line (ry) and has the same height (bh); the texts are centred on it.
        // (ImGui's SameLine puts the next item on the top of the FIRST item of the line, here the CLICK label: the buttons after
        // LAUNCH used to land 2 px lower than LAUNCH, so the row looked uneven on the Mac.)
        const float ry = rowY + 36.0f, bh = 20.0f;
        const float textY = ry + std::floor((bh - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::SetCursorPos(ImVec2(x0 + kLabelW, textY));
        ImGui::TextDisabled("CLICK");
        ImGui::SameLine(0, 6);
        ImGui::SetCursorPosY(ry);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        ToggleColors ec; ec.on = ImVec4(0.37f, 0.88f, 0.79f, 1.0f); ec.textOn = ImVec4(0.06f, 0.09f, 0.09f, 1.0f);
        if (ToggleButton("LAUNCH", !app.clickOpens, ImVec2(60, bh), ec) && app.clickOpens) {
            app.clickOpens = false;
            app.SetStatus("CLICK: LAUNCH - A CLICK ON A PAD LAUNCHES IT; A DOUBLE CLICK OPENS IT WITHOUT LAUNCHING");
        }
        if (ImGui::IsItemHovered()) Hint("CLICK: LAUNCH - a click on a pad launches the scene (quantized by LAUNCH in the header). A double click opens it without launching.");
        ImGui::SameLine(0, 0);
        ImGui::SetCursorPosY(ry);
        if (ToggleButton("OPEN", app.clickOpens, ImVec2(52, bh), ec) && !app.clickOpens) {
            app.clickOpens = true;
            app.SetStatus("CLICK: OPEN - A CLICK ON A PAD ONLY OPENS THE SCENE; GO LAUNCHES IT");
        }
        if (ImGui::IsItemHovered()) Hint("CLICK: OPEN - while the music plays, a click on a pad only opens that scene in the editor: correct it, nothing is heard. GO launches it. The scene that plays keeps playing.");
        ImGui::PopStyleVar();
        ImGui::SameLine(0, 8);
        ImGui::SetCursorPosY(ry);
        const bool apart = app.SceneIsHeldApart();
        char goLbl[24];
        std::snprintf(goLbl, sizeof(goLbl), apart ? "GO %02d###go" : "GO###go", proj.currentPattern + 1);
        ImGui::BeginDisabled(!apart);
        ToggleColors gc; gc.on = ImVec4(0.95f, 0.64f, 0.24f, 1.0f); gc.textOn = ImVec4(0.10f, 0.07f, 0.03f, 1.0f);
        if (ToggleButton(goLbl, apart, ImVec2(54, bh), gc)) app.GoToEditedScene();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) Hint(apart ? "GO: launch the open scene (quantized by LAUNCH in the header)." : "GO: lights up when the open scene is not the one that plays.");
        ImGui::SameLine(0, 4);
        ImGui::SetCursorPosY(ry);
        // LINK acts on the selected pads; with none selected and the open scene linked it takes the open scene out of its group.
        const int nSel = app.SceneSelCount();
        const bool openLinked = proj.currentPattern < kNumLiveScenes && app.SceneGroupOf(proj.currentPattern) != 0;
        char linkLbl[64];
        if (nSel >= 2) std::snprintf(linkLbl, sizeof(linkLbl), "LINK %s###link", app.SceneSelLabel().c_str());
        else if (openLinked) std::snprintf(linkLbl, sizeof(linkLbl), "UNLINK %02d###link", proj.currentPattern + 1);
        else std::snprintf(linkLbl, sizeof(linkLbl), "LINK###link");
        ImGui::BeginDisabled(nSel < 2 && !openLinked);
        const float lw = std::max(52.0f, ImGui::CalcTextSize(linkLbl, nullptr, true).x + 14.0f);
        if (ToggleButton(linkLbl, nSel >= 2, ImVec2(lw, bh), ec)) {
            if (nSel >= 2) app.LinkSelectedScenes(); else app.UnlinkSceneUndoable(proj.currentPattern);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            Hint("LINK: linked scenes share mute and solo, so you can play across them without anything changing.\nSelect the scenes with Shift+click (a range) or Cmd+click (one by one), then LINK. With a linked scene open and nothing selected: UNLINK takes it out of its group.\nThe mute / solo come from the open scene. Cmd+Z undoes a link, mutes included.");
        ImGui::PopStyleVar();
        ImGui::SameLine(0, 10);
        ImGui::SetCursorPosY(textY);
        char info[160] = "";
        ImVec4 infoCol(0.55f, 0.85f, 0.65f, 1.0f);
        if (nSel >= 2) { std::snprintf(info, sizeof(info), "%d SCENES SELECTED - LINK makes them share mute / solo", nSel); infoCol = ImVec4(0.62f, 0.74f, 1.0f, 1.0f); }
        else if (apart) { std::snprintf(info, sizeof(info), "OPEN %02d  -  PLAYING %02d  -  GO launches %02d", proj.currentPattern + 1, app.sequencer.PlayPatternIndex() + 1, proj.currentPattern + 1); infoCol = ImVec4(0.95f, 0.78f, 0.45f, 1.0f); }
        else if (app.SceneGroupOf(proj.currentPattern)) std::snprintf(info, sizeof(info), "LINKED %s: SAME MUTE / SOLO", app.LinkLabel(proj.currentPattern).c_str());
        if (info[0]) ImGui::TextColored(infoCol, "%s", info);
    }

    // ---- popups (opened outside the per-widget PushID scopes) -----------
    if (padMenuOpenRequest_) { ImGui::OpenPopup("ScenePadMenu"); padMenuOpenRequest_ = false; }
    if (ImGui::BeginPopup("ScenePadMenu")) {
        int i = padMenuPattern_;
        if (i >= 0 && i < kNumLiveScenes) {
            ImGui::TextDisabled("SCENE %02d", i + 1);
            ImGui::Separator();
            if (i != proj.currentPattern && proj.currentPattern < kNumLiveScenes && ImGui::MenuItem(app.ScenesLinked(i, proj.currentPattern) ? "Unlink from the open scene" : "Link with the open scene (same mute / solo)")) app.ToggleSceneLink(i);
            if (app.SceneGroupOf(i) && ImGui::MenuItem("Leave its link group")) app.UnlinkSceneUndoable(i);
            if (ImGui::MenuItem("Duplicate as new scene")) {
                int dst = FindFreeSlotAfter(proj, i);
                if (dst < 0) {
                    app.SetStatus("ALL 16 SCENE SLOTS ARE IN USE");
                } else {
                    app.DuplicateSceneAsNew(i, dst);   // opens the copy; playback stays on the scene that plays
                    app.SetStatus("SCENE " + std::to_string(i + 1) + " DUPLICATED -> " + std::to_string(dst + 1));
                }
            }
            if (ImGui::MenuItem("Throw a copy onto the track")) AddToChain(app, i);
            if (ImGui::MenuItem("Take this scene's sound to the track")) app.TakeSceneSoundToTrack(i);
            ImGui::Separator();
            if (ImGui::BeginMenu("Role")) {
                for (int r = 0; r < kNumRoles; ++r) {
                    if (ImGui::MenuItem(r == 0 ? "- none" : RoleName(static_cast<SceneRole>(r)), nullptr, proj.patterns[i].role == static_cast<SceneRole>(r))) {
                        app.PushUndo(); proj.patterns[i].role = static_cast<SceneRole>(r);
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Fill...")) { sceneFillIndex_ = i; sceneFillOpenRequest_ = true; }
            ImGui::Separator();
            if (ImGui::BeginMenu("Sound of this scene")) { DrawSceneSoundItems(app, i); ImGui::EndMenu(); }
            if (ImGui::BeginMenu("Instruments of this scene")) {
                for (int t = 0; t < kNumTracks; ++t) {
                    const Track& tr = proj.patterns[i].tracks[t];
                    std::string lab = tr.name + (app.TrackHasNotes(i, t) ? "" : "  (empty)");
                    if (ImGui::BeginMenu(lab.c_str())) { DrawInstrumentItems(app, i, t); ImGui::EndMenu(); }
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Delete scene")) {
                if (ImGui::MenuItem("Yes - clear scene and remove it from the track")) DeleteScene(app, i);
                ImGui::EndMenu();
            }
        }
        ImGui::EndPopup();
    }
    if (chordMenuOpenRequest_) { ImGui::OpenPopup("ChordSlotMenu"); chordMenuOpenRequest_ = false; chordRenameBuf_[0] = 0; }
    if (ImGui::BeginPopup("ChordSlotMenu")) {
        int c = chordMenuSlot_;
        if (c >= 0 && c < kNumChordScenes) {
            ImGui::TextDisabled("CHORD SCENE %d", c + 1);
            if (ImGui::MenuItem("Open in CHORD LAB")) OpenChordLabOn(app, c);
            ImGui::Separator();
            if (ImGui::MenuItem("Edit only (do not launch)")) { if (!proj.chordBankOn) app.SetChordBank(true); app.EditChordSceneOnly(c); }
            if (ImGui::MenuItem("Copy this scene's chords here")) { if (!proj.chordBankOn) app.SetChordBank(true); app.CopyPatternChordsToScene(c); }
            if (ImGui::MenuItem("Duplicate to next slot", nullptr, false, c + 1 < kNumChordScenes)) app.DuplicateChordScene(c, c + 1);
            if (ImGui::MenuItem("Clear", nullptr, false, !app.ChordSceneEmpty(c))) app.ClearChordScene(c);
            ImGui::Separator();
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::InputTextWithHint("##rename", "name (Enter)", chordRenameBuf_, sizeof(chordRenameBuf_), ImGuiInputTextFlags_EnterReturnsTrue)) {
                app.PushUndo();
                proj.chordScenes[c].name = chordRenameBuf_;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    if (slotMenuOpenRequest_) { ImGui::OpenPopup("ChainSlotMenu"); slotMenuOpenRequest_ = false; }
    if (ImGui::BeginPopup("ChainSlotMenu")) {
        int idx = slotMenuIndex_;
        if (idx >= 0 && idx < proj.songChainLength) {
            ImGui::TextDisabled("SCENE SLOT %d", idx + 1);
            ImGui::Separator();
            if (ImGui::BeginMenu("Change scene")) {
                for (int p = 0; p < kNumLiveScenes; ++p) {
                    char lbl[16];
                    std::snprintf(lbl, sizeof(lbl), "Scene %02d", p + 1);
                    if (ImGui::MenuItem(lbl, nullptr, proj.songChain[idx].patternIndex == p)) { app.PushSoundUndo(); proj.songChain[idx].patternIndex = p; }
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Chord scene")) {
                if (ImGui::MenuItem("- keep the chords as they are", nullptr, proj.songChain[idx].chordScene < 0)) { app.PushSoundUndo(); proj.songChain[idx].chordScene = -1; }
                for (int c = 0; c < kNumChordScenes; ++c) {
                    std::string lb = std::to_string(c + 1) + "  " + (app.ChordSceneEmpty(c) ? std::string("(empty)") : app.ChordSceneLabel(c));
                    if (ImGui::MenuItem(lb.c_str(), nullptr, proj.songChain[idx].chordScene == c)) {
                        app.PushSoundUndo(); proj.songChain[idx].chordScene = c;
                        if (!proj.chordBankOn) app.SetChordBank(true);
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Fill empty steps from the roles", nullptr, false, proj.songChainLength > 0)) {
                app.PushSoundUndo();
                const AutoArrangeResult r = AutoArrangeChain(proj, false);
                app.sequencer.RefreshArrangement();
                app.SetStatus("ROLES: " + std::to_string(r.lanes) + " DYNAMICS, " + std::to_string(r.mutes) + " MUTES, " + std::to_string(r.transitions) + " TRANSITIONS");
            }
            if (ImGui::IsItemHovered()) Hint("Every brick that has nothing set yet gets what its scene's role suggests:\ndynamics (intro opens up, build thins out, outro fades), break mutes, and risers / impacts between roles.\nSteps you have already set are left alone.");
            if (ImGui::MenuItem("Replace all from the roles", nullptr, false, proj.songChainLength > 0)) {
                app.PushSoundUndo();
                const AutoArrangeResult r = AutoArrangeChain(proj, true);
                app.sequencer.RefreshArrangement();
                app.SetStatus("ROLES: " + std::to_string(r.lanes) + " DYNAMICS, " + std::to_string(r.mutes) + " MUTES, " + std::to_string(r.transitions) + " TRANSITIONS");
            }
            if (ImGui::IsItemHovered()) Hint("Overwrites the dynamics, mutes and transitions of ALL bricks with what the roles suggest (undo restores them).");
            ImGui::Separator();
            if (ImGui::MenuItem("Duplicate slot")) {
                if (!InsertSlotCopy(app, idx)) app.SetStatus("TRACK FULL");
            }
            if (ImGui::MenuItem("Move left", nullptr, false, idx > 0)) MoveSlot(app, idx, idx - 1);
            if (ImGui::MenuItem("Move right", nullptr, false, idx < proj.songChainLength - 1)) MoveSlot(app, idx, idx + 1);
            if (ImGui::MenuItem("Remove slot")) RemoveSlot(app, idx);
        }
        ImGui::EndPopup();
    }

    DrawSceneFillEditor(app);
    ImGui::PopID();
}


void ChainPanel::DrawEditors(AppState& app) {
    ImGui::PushID("ChainPanel");
    DrawSceneFillEditor(app);
    ImGui::PopID();
}

// The fill a scene usually ends with (v44.5): a rule on the pattern, played in the song and while the scene loops on its own.
void ChainPanel::DrawSceneFillEditor(AppState& app) {
    Project& proj = app.project;
    if (sceneFillOpenRequest_) { ImGui::OpenPopup("SceneFillEditor"); sceneFillOpenRequest_ = false; }
    if (!ImGui::BeginPopup("SceneFillEditor")) return;
    const int i = sceneFillIndex_;
    if (i < 0 || i >= kNumPatterns) { ImGui::EndPopup(); return; }
    Pattern& pat = proj.patterns[static_cast<size_t>(i)];
    ImGui::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f), "SCENE %02d  -  FILL", i + 1);
    ImGui::TextDisabled("A drum fill over the last beats; the steps of the scene are not touched.\nA brick can switch it off or use a rule of its own.");
    FillRule r = pat.fill;
    bool fch = DrawFillRuleEditor(r);
    if (r.when != FillWhen::Off && r.kind == FillKind::Sample && FillSampleControls(app, r, -1 - i, 300.0f)) fch = true;   // C4b
    if (fch) { app.PushUndo(); pat.fill = NormalizeFill(r); }
    ImGui::Separator();
    ImGui::TextDisabled("%s", FillSummary(pat.fill).c_str());
    if (ImGui::Button("CLOSE")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace t8::desktop
