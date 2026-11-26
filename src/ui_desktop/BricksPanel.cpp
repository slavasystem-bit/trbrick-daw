#include "BricksPanel.h"
#include "ChainPanel.h"
#include "Visual.h"
#include "MixerSync.h"
#include "LiveWidgets.h"
#include "SampleRow.h"
#include "FillSampleRow.h"
#include "ValueField.h"
#include "BrickGlyphs.h"
#include "InstrumentSwitches.h"
#include "ParamText.h"
#include "TrackStyle.h"
#include "Interaction.h"
#include "DynPresets.h"
#include "core/ArrangeAuto.h"
#include "core/DrumBreaks.h"
#include "imgui.h"
#include "InfoLine.h"
#include <algorithm>
#include <sstream>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace t8::desktop {

namespace {
// A dB field of the brick dynamics (the lowest value reads "off"). started -> one undo step (coalesced by PushSoundUndo).
bool DbField(AppState& app, const char* id, int* db, float width, bool enabled = true) {
    float f = static_cast<float>(*db);
    const ValueResult r = ValueField(id, &f, DbSpec(kAutoMinDb, kAutoMaxDb), width, enabled);
    if (r.started) app.PushSoundUndo();
    if (r.changed) *db = static_cast<int>(f);
    if (r.cancelled) app.DropLastUndo();
    return r.changed;
}
} // namespace

namespace {

constexpr ImU32 kNoRole = IM_COL32(124, 132, 156, 255);
constexpr ImU32 kDim = IM_COL32(140, 146, 166, 255);
constexpr ImU32 kFillCol = IM_COL32(240, 160, 60, 255);
constexpr ImU32 kTransCol = IM_COL32(160, 130, 240, 255);
constexpr float kLaneRowMin = 30.0f;   // smallest height of one lane row under the bricks (MIX / FILTER / instrument)
constexpr ImU32 kAmberHpf = IM_COL32(240, 170, 70, 200);
constexpr ImU32 kVioletLpf = IM_COL32(174, 126, 224, 200);

ImU32 RoleCol(SceneRole r) {
    switch (r) {
    case SceneRole::Intro: return IM_COL32(100, 156, 224, 255);
    case SceneRole::Build: return IM_COL32(232, 202, 74, 255);
    case SceneRole::Drop:  return IM_COL32(238, 94, 70, 255);
    case SceneRole::Break: return IM_COL32(84, 204, 166, 255);
    case SceneRole::Outro: return IM_COL32(174, 126, 224, 255);
    default: return kNoRole;
    }
}
ImU32 WithAlpha(ImU32 c, int a) { return (c & 0x00FFFFFFu) | (static_cast<ImU32>(std::clamp(a, 0, 255)) << 24); }

bool SceneHasNotes(const Pattern& p) {
    for (const Track& t : p.tracks) for (const Step& s : t.steps) if (s.active) return true;
    return false;
}

struct Geo { float x0 = 0, x1 = 0; };

// How tall / opaque something at `db` is drawn: 0 dB = full, the lowest level = a sliver.
float LevelFrac(int db) { return db <= kAutoMinDb ? 0.08f : std::clamp(1.0f + static_cast<float>(std::min(db, 0)) / 36.0f, 0.2f, 1.0f); }
const char* TrackNameShort(int t) {
    static const char* k[kNumTracks] = {"KICK", "SNARE", "CLAP", "C.HAT", "O.HAT", "PERC", "BASS", "SYN 2", "SYN 3", "CHORD", "SMPL"};
    return k[std::clamp(t, 0, kNumTracks - 1)];
}
std::string DbText(int db) { return db <= kAutoMinDb ? std::string("off") : (std::to_string(db) + " dB"); }

} // namespace

// Changes the zoom and keeps the bar under `anchorScreenX` where it is on the screen.
void BricksPanel::ZoomTo(float newPpb, float anchorScreenX) {
    newPpb = std::clamp(newPpb, 3.0f, 240.0f);
    const float anchorBar = (anchorScreenX - childLeftX_ + scrollX_ - 4.0f) / std::max(1.0f, ppbCur_);
    ppbUser_ = newPpb;
    ScrollTo(anchorBar * newPpb + 4.0f - (anchorScreenX - childLeftX_));
}

void BricksPanel::Draw(AppState& app, ChainPanel& /*chain*/, bool showDetails) {
    SceneLock lock(app.sequencer.PatternMutex(), "BRICKS");
    Project& proj = app.project;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::PushID("BricksPanel");
    const ImVec4 amber(0.95f, 0.64f, 0.24f, 1.0f);
    const bool songOn = app.sequencer.IsSongModeActive();
    const bool playing = app.sequencer.IsPlaying();
    const int playingPos = songOn && playing ? app.sequencer.SongChainPos() : -1;
    const int n = std::clamp(proj.songChainLength, 0, kMaxChainSteps);
    // The selected brick is the one holding the scene in the editor (the editor and the strip are one screen).
    sel_ = app.BrickForTransport();
    auto selectBrick = [&](int idx) {
        if (idx < 0 || idx >= proj.songChainLength) return;
        sel_ = idx; app.selBrick = idx; anchor_ = idx;
        if (app.selBrickHi >= 0) app.ClearBrickRange();   // a plain selection ends a range
        if (proj.currentPattern != proj.songChain[static_cast<size_t>(idx)].patternIndex) SwitchPattern(app, proj.songChain[static_cast<size_t>(idx)].patternIndex);
    };

    // ---- totals ---------------------------------------------------------------------------------------------------------
    double totalBars = 0.0, totalSec = 0.0;
    std::vector<double> startBar(static_cast<size_t>(n) + 1, 0.0), barsOf(static_cast<size_t>(n), 0.0);
    for (int i = 0; i < n; ++i) {
        const ChainStep& cs = proj.songChain[static_cast<size_t>(i)];
        const Pattern& pat = proj.patterns[static_cast<size_t>(std::clamp(cs.patternIndex, 0, kNumPatterns - 1))];
        barsOf[static_cast<size_t>(i)] = ChainStepBars(pat, cs);
        startBar[static_cast<size_t>(i)] = totalBars;
        totalBars += barsOf[static_cast<size_t>(i)];
        totalSec += ChainStepSeconds(pat, cs);
    }
    startBar[static_cast<size_t>(n)] = totalBars;

    // ---- top bar: the player and the brick actions -------------------------------------------------------------------------
    ImGui::TextColored(amber, "TRACK");
    ImGui::SameLine(0, 10);
    if (n > 0) ImGui::TextDisabled("%d brick%s  %.4g bar%s  %d:%02d", n, n == 1 ? "" : "s", totalBars, totalBars == 1.0 ? "" : "s", static_cast<int>(totalSec) / 60, static_cast<int>(totalSec) % 60);
    else ImGui::TextDisabled("no bricks yet");
    ImGui::SameLine(0, 16);
    ImGui::SameLine(0, 16);
    // v44.28: the one player (PLAY is in the header, Space): LOOP keeps the selected brick / bricks going round, |< goes to the first brick (Home)
    ImGui::BeginDisabled(n == 0);
    if (ToggleButton("LOOP", app.loopOn, ImVec2(52, 0), ToggleTeal())) app.SetLoop(!app.loopOn);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) Hint("LOOP on: PLAY (Space) keeps the selected brick (or the selected bricks) going round; with ALL on, the whole track goes round.\nLOOP off: PLAY runs the bricks in order from the selected one and stops at the end of the track.\nClick another brick while it plays: you edit it, the music does not jump. Double-click a brick to play from it.");
    ImGui::SameLine(0, 4);
    ImGui::BeginDisabled(n < 2 || !app.loopOn);
    if (ToggleButton("ALL", app.loopAll && app.loopOn, ImVec2(40, 0), ToggleTeal())) app.SetLoopAll(!app.loopAll);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) Hint("ALL: with LOOP on, the WHOLE track goes round, with the transitions between the bricks, however you select or edit bricks.\nClick a brick to edit it: the music does not jump and the loop stays the whole track. Double-click a brick to start playing from it.");
    ImGui::SameLine(0, 4);
    ImGui::BeginDisabled(n == 0);
    if (ImGui::Button("|<##home", ImVec2(30, 0))) { app.TrackToStart(); SelectBrickForEdit(app, 0); }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) Hint("Go to the first brick (Home). When it plays, it restarts there.");
    ImGui::SameLine(0, 16);
    ImGui::BeginDisabled(n == 0);
    if (ImGui::Button("FILL FROM ROLES")) {
        app.PushSoundUndo();
        const AutoArrangeResult r = AutoArrangeChain(proj, false);
        app.sequencer.RefreshArrangement();
        app.SetStatus("ROLES: " + std::to_string(r.lanes) + " DYNAMICS, " + std::to_string(r.mutes) + " MUTES, " + std::to_string(r.transitions) + " TRANSITIONS");
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) Hint("Bricks with nothing set yet get what the role of their scene suggests:\ndynamics, break mutes, risers between roles. Right-click a brick > Role to set it.");
    // zoom
    ImGui::SameLine(0, 16);
    if (ImGui::Button("-##zoom", ImVec2(24, 0))) ZoomTo(ppbCur_ / 1.4f, childLeftX_ + viewW_ * 0.5f);
    if (ImGui::IsItemHovered()) Hint("Zoom out  (Ctrl / Cmd + mouse wheel over the strip)");
    ImGui::SameLine(0, 3);
    if (ImGui::Button("+##zoom", ImVec2(24, 0))) ZoomTo(ppbCur_ * 1.4f, childLeftX_ + viewW_ * 0.5f);
    if (ImGui::IsItemHovered()) Hint("Zoom in  (Ctrl / Cmd + mouse wheel over the strip)");
    ImGui::SameLine(0, 3);
    if (ImGui::Button("FIT")) { ppbUser_ = 0.0f; ScrollTo(0.0f); }
    if (ImGui::IsItemHovered()) Hint("Show the whole track in the window");
    ImGui::SameLine(0, 3);
    ImGui::BeginDisabled(sel_ < 0);
    if (ImGui::Button("BRICK##zoom") && sel_ >= 0 && sel_ < n) {   // zoom so that the selected brick fills about 60% of the window
        const float b = static_cast<float>(barsOf[static_cast<size_t>(sel_)]);
        const float ppbNew = std::clamp(viewW_ * 0.6f / std::max(0.25f, b), 3.0f, 240.0f);
        ppbUser_ = ppbNew;
        ScrollTo(static_cast<float>(startBar[static_cast<size_t>(sel_)]) * ppbNew + 4.0f - viewW_ * 0.2f);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) Hint("Zoom to the selected brick");

    // overview of a long track: the whole track as a thin strip, the window as a frame; click or drag to go there
    if (contentW_ > viewW_ + 1.0f && n > 0 && totalBars > 0.0) {
        ImGui::SameLine(0, 12);
        const float ow = std::clamp(ImGui::GetContentRegionAvail().x - 8.0f, 60.0f, 260.0f);
        const float oh = ImGui::GetFrameHeight();
        ImGui::InvisibleButton("##overview", ImVec2(ow, oh));
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImDrawList* odl = ImGui::GetWindowDrawList();
        odl->AddRectFilled(a, b, IM_COL32(24, 26, 33, 255), 3.0f);
        for (int i = 0; i < n; ++i) {
            const ChainStep& cs = proj.songChain[static_cast<size_t>(i)];
            const SceneRole role = proj.patterns[static_cast<size_t>(std::clamp(cs.patternIndex, 0, kNumPatterns - 1))].role;
            const float x0 = a.x + (b.x - a.x) * static_cast<float>(startBar[static_cast<size_t>(i)] / totalBars);
            const float x1 = a.x + (b.x - a.x) * static_cast<float>(startBar[static_cast<size_t>(i + 1)] / totalBars);
            odl->AddRectFilled(ImVec2(x0 + 0.5f, a.y + 3), ImVec2(std::max(x0 + 1.0f, x1 - 0.5f), b.y - 3), WithAlpha(RoleCol(role), i == sel_ ? 255 : 170), 1.5f);
        }
        const float tot = std::max(1.0f, contentW_);
        const float vx0 = a.x + (b.x - a.x) * (scrollX_ / tot), vx1 = a.x + (b.x - a.x) * std::min(1.0f, (scrollX_ + viewW_) / tot);
        odl->AddRect(ImVec2(vx0, a.y + 1), ImVec2(std::max(vx0 + 4.0f, vx1), b.y - 1), vis::kAccentHi, 3.0f, 0, 1.6f);
        if (app.sequencer.IsPlaying() && playingPos >= 0 && playingPos < n) {
            const double bar = startBar[static_cast<size_t>(playingPos)] + std::clamp(app.sequencer.ChainStepProgress(), 0.0, 1.0) * barsOf[static_cast<size_t>(playingPos)];
            const float px = a.x + (b.x - a.x) * static_cast<float>(bar / totalBars);
            odl->AddLine(ImVec2(px, a.y + 1), ImVec2(px, b.y - 1), vis::kTeal, 1.6f);
        }
        if (ImGui::IsItemActive()) {
            const float frac = std::clamp((ImGui::GetIO().MousePos.x - a.x) / std::max(1.0f, b.x - a.x), 0.0f, 1.0f);
            ScrollTo(frac * tot - viewW_ * 0.5f);
        }
        if (ImGui::IsItemHovered()) Hint("The whole track. Click or drag to move the window.");
    }

    // ---- geometry ---------------------------------------------------------------------------------------------------------
    const bool det = showDetails;
    bool hasParamRows = false;   // v44.38: a fourth lane row (the chosen parameter) appears once a brick has a parameter row
    for (int i = 0; i < n; ++i) if (proj.songChain[static_cast<size_t>(i)].params.n > 0) hasParamRows = true;
    const int laneRows = hasParamRows ? 4 : 3;
    const float labelW = 34.0f, rulerH = 16.0f, gap = 4.0f, addW = 56.0f, laneH = 28.0f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float H = std::max(70.0f, avail.y);
    const float fitW = std::max(40.0f, avail.x - labelW - 14.0f);
    const float fitPpb = (fitW - addW - 16.0f) / static_cast<float>(std::max(totalBars, 4.0));
    const float ppb = ppbUser_ > 0.0f ? std::clamp(ppbUser_, 3.0f, 240.0f) : std::clamp(fitPpb, 3.0f, 72.0f);
    ppbCur_ = ppb;
    // the sideways scroll bar only takes room when the track is wider than the window
    const float scrollH = (static_cast<float>(totalBars) * ppb + addW + 24.0f > fitW + 2.0f) ? 16.0f : 0.0f;
    const float brickH = det ? 36.0f : std::clamp(H - rulerH - laneH - 2.0f - scrollH - 4.0f, 34.0f, 56.0f);
    float rowH = 0.0f, matrixH = 0.0f, dynH = 0.0f;
    float laneMin = kLaneRowMin;   // v44.50: on a short window the lane rows give up room (30 -> 20 each) so the eleven instrument rows stay readable
    if (det) {
        const float rem = H - rulerH - brickH - laneH - 2.0f - gap * 2.0f - scrollH - 4.0f;
        // v44.50: the eleven instrument rows come first (12 px each at least); the lane rows under them (MIX, FILTER, the instrument, a
        // parameter) take what is left, 20-30 px each, and on a short window only the first of them or two are drawn
        const float matrixMin = 12.0f * static_cast<float>(kNumTracks);
        const int shown = std::clamp(static_cast<int>((rem - matrixMin) / 20.0f), 1, laneRows);
        laneMin = std::clamp((rem - matrixMin) / static_cast<float>(shown), 20.0f, kLaneRowMin);
        rowH = std::clamp((rem - laneMin * static_cast<float>(shown)) / static_cast<float>(kNumTracks), 5.0f, 20.0f);
        matrixH = rowH * kNumTracks;
        dynH = std::max(laneMin, rem - matrixH);
    }
    const float yBricks = rulerH, yLane = rulerH + brickH + 2.0f, yMatrix = yLane + laneH + gap, yDyn = yMatrix + matrixH + gap;
    const float contentBottom = det ? yDyn + dynH : yLane + laneH;

    // ---- labels (fixed on the left) ---------------------------------------------------------------------------------
    static const char* kTrk[kNumTracks] = {"KICK", "SNARE", "CLAP", "C.HAT", "O.HAT", "PERC", "BASS", "SYN 2", "SYN 3", "CHORD", "SMPL"};
    ImGui::BeginChild("##BrickLabels", ImVec2(labelW, H), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoInputs);
    {   // v44.36: the rows are named by pictograms (the name and what the row does are in the info line)
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const float cx = o.x + (labelW - 6.0f) * 0.5f;
        auto over = [&](float y0, float y1) { return ImGui::IsMouseHoveringRect(ImVec2(o.x, o.y + y0), ImVec2(o.x + labelW - 4.0f, o.y + y1), false); };
        auto icon = [&](float y0, float y1, Glyph g, ImU32 col, const char* tip) {
            const float sz = std::clamp(y1 - y0 - 2.0f, 6.0f, 16.0f);
            const bool hov = over(y0, y1);
            DrawGlyph(dl, g, ImVec2(cx, (y0 + y1) * 0.5f + o.y), sz, hov ? vis::kText : col);
            if (hov) Hint("%s", tip);
        };
        icon(yBricks, yBricks + brickH, Glyph::Bricks, kDim, "BRICKS: the track as bricks, in playing order. Click to select, drag the right edge to change the length, drag a brick to move it.");
        icon(yLane + 1.0f, yLane + laneH * 0.5f, Glyph::Fill, kFillCol, "FILLS: where each brick has a drum fill. Drag its left edge to change the length.");
        icon(yLane + laneH * 0.5f, yLane + laneH, Glyph::Trans, kTransCol, "TRANSITIONS: risers and impacts on the border of two bricks. Drag the left edge to change the length.");
        if (det) {
            for (int t = 0; t < kNumTracks; ++t) {
                const float y0 = yMatrix + t * rowH, y1 = y0 + rowH;
                const float sz = std::clamp(rowH - 1.0f, 6.0f, 15.0f);
                const bool hov = over(y0, y1);
                DrawInstrumentGlyph(dl, t, ImVec2(cx, o.y + (y0 + y1) * 0.5f), sz, hov ? vis::kText : TrackColor(t));
                if (hov) Hint("%s: the row of who plays it on each brick. Click or drag on the row to switch it off / on.", kTrk[t]);
            }
            {   // v44.21: the lane rows share one look: MIX, the DJ filter and the instrument you have selected
                const int rows = std::clamp(static_cast<int>(dynH / laneMin), 1, laneRows);
                const float rh = dynH / static_cast<float>(rows);
                const int ids[4] = {kLaneMix, std::clamp(app.seqCursorTrack, 0, kNumTracks - 1), kLaneFilter, kLaneParam0};   // v44.51: the instrument level is the second row, so a short window still shows it
                for (int r = 0; r < rows; ++r) {
                    const int id = ids[r];
                    const float y0 = yDyn + r * rh, y1 = y0 + rh;
                    const bool hov = over(y0, y1);
                    const float sz = std::clamp(rh - 8.0f, 8.0f, 16.0f);
                    const ImVec2 c(cx, o.y + (y0 + y1) * 0.5f);
                    if (id == kLaneMix) DrawGlyph(dl, Glyph::Mix, c, sz, hov ? vis::kText : vis::kTeal);
                    else if (id == kLaneFilter) DrawGlyph(dl, Glyph::Filter, c, sz, hov ? vis::kText : kAmberHpf);
                    else if (id == kLaneParam0) {   // v44.38: the chosen parameter: the instrument and a short name of the knob
                        const ImU32 pc = TrackColor(std::clamp(parTrack_, 0, kNumTracks - 1));
                        DrawInstrumentGlyph(dl, std::clamp(parTrack_, 0, kNumTracks - 1), ImVec2(c.x, c.y - 5.0f), std::min(sz, 11.0f), hov ? vis::kText : pc);
                        const char* sn = AutoParamShort(parParam_); const float tw = ImGui::CalcTextSize(sn).x * (8.5f / ImGui::GetFontSize());
                        dl->AddText(nullptr, 8.5f, ImVec2(c.x - tw * 0.5f, c.y + 3.0f), hov ? vis::kText : pc, sn);
                    }
                    else DrawInstrumentGlyph(dl, id, c, sz, hov ? vis::kText : TrackColor(id));
                    if (hov && id == kLaneParam0) Hint("%s %s: the parameter row you chose on the PARAM page, over each brick (drag the dots). Click an empty cell to start one on that brick.", kTrk[std::clamp(parTrack_, 0, kNumTracks - 1)], AutoParamName(parParam_));
                    else if (hov) Hint(id == kLaneMix ? "MIX: the level of the whole brick from its start to its end (drag the dots)." : id == kLaneFilter ? "FILTER: the DJ filter over each brick (below the middle closes the sound, above thins it)."
                                                    : "%s: the level of the selected instrument over each brick (drag the dots). Choose the instrument on the LEVEL page below (or right-click its bar in WHO PLAYS).", kTrk[std::clamp(id, 0, kNumTracks - 1)]);   // audit v44.43: id is 10 / 11 on the MIX / FILTER rows, the argument is evaluated either way
                }
            }
        }
    }
    ImGui::EndChild();
    ImGui::SameLine(0, 0);

    // ---- the strip (scrolls sideways when the track is long) ---------------------------------------------------
    if (pendingFrames_ > 0) { ImGui::SetNextWindowScroll(ImVec2(pendingScroll_, -1.0f)); if (--pendingFrames_ == 0) pendingScroll_ = -1.0f; }
    ImGui::BeginChild("##BrickScroll", ImVec2(0, H), false, ImGuiWindowFlags_HorizontalScrollbar);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 o = ImGui::GetCursorScreenPos();   // content origin (follows the scroll)
        const float X0 = o.x + 4.0f;
        const float contentW = static_cast<float>(totalBars) * ppb + addW + 24.0f;
        const float yTop = o.y;
        const bool winHovered = ImGui::IsWindowHovered();
        const ImVec2 mouse = io.MousePos;
        scrollX_ = ImGui::GetScrollX(); viewW_ = ImGui::GetWindowWidth(); contentW_ = contentW; childLeftX_ = ImGui::GetWindowPos().x;
        if (winHovered && io.MouseWheel != 0.0f && drag_ == Drag::None) {   // wheel: sideways; Ctrl / Cmd + wheel: zoom around the mouse
            if (io.KeyCtrl) ZoomTo(ppb * std::pow(1.18f, io.MouseWheel), mouse.x);
            else ScrollTo(scrollX_ - io.MouseWheel * 80.0f);
        }
        if (winHovered && io.MouseWheelH != 0.0f && drag_ == Drag::None) ScrollTo(scrollX_ - io.MouseWheelH * 80.0f);

        // ruler: bar numbers
        {
            int stepBars = 1;
            for (int sb : {1, 2, 4, 8, 16, 32, 64}) { stepBars = sb; if (ppb * sb >= 38.0f) break; }
            for (int b = 0; b <= static_cast<int>(std::ceil(totalBars)) + 1; b += stepBars) {
                const float x = X0 + b * ppb;
                dl->AddLine(ImVec2(x, yTop + rulerH - 5), ImVec2(x, yTop + rulerH - 1), kDim);
                char bb[16]; std::snprintf(bb, sizeof bb, "%d", b + 1);
                dl->AddText(nullptr, 11.0f, ImVec2(x + 2, yTop + 2), kDim, bb);
            }
            // a click on the ruler above a brick plays from that brick (also in the middle of playing)
            if (winHovered && mouse.y >= yTop && mouse.y < yTop + rulerH) {
                int hit = -1;
                for (int i = 0; i < n; ++i) {
                    const float bx0 = X0 + static_cast<float>(startBar[static_cast<size_t>(i)]) * ppb, bx1 = X0 + static_cast<float>(startBar[static_cast<size_t>(i + 1)]) * ppb;
                    if (mouse.x >= bx0 && mouse.x < bx1) { hit = i; break; }
                }
                if (hit >= 0) {
                    const float bx0 = X0 + static_cast<float>(startBar[static_cast<size_t>(hit)]) * ppb;
                    dl->AddTriangleFilled(ImVec2(bx0 + 2, yTop + 3), ImVec2(bx0 + 2, yTop + 13), ImVec2(bx0 + 10, yTop + 8), vis::kAccentHi);
                    Hint("Play from brick %d", hit + 1);
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && drag_ == Drag::None) { selectBrick(hit); app.PlayFromBrick(hit); }
                }
            }
        }

        std::vector<Geo> geo(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            geo[static_cast<size_t>(i)].x0 = X0 + static_cast<float>(startBar[static_cast<size_t>(i)]) * ppb;
            geo[static_cast<size_t>(i)].x1 = X0 + static_cast<float>(startBar[static_cast<size_t>(i + 1)]) * ppb;
        }

        // ---- bricks -----------------------------------------------------------------------------------------------------
        for (int i = 0; i < n; ++i) {
            ChainStep& cs = proj.songChain[static_cast<size_t>(i)];
            const int pi = std::clamp(cs.patternIndex, 0, kNumPatterns - 1);
            const Pattern& pat = proj.patterns[static_cast<size_t>(pi)];
            const float x0 = geo[static_cast<size_t>(i)].x0, x1 = geo[static_cast<size_t>(i)].x1, w = x1 - x0;
            const ImVec2 p0(x0 + 1.0f, yTop + yBricks), p1(x1 - 1.0f, yTop + yBricks + brickH);
            const ImU32 rc = RoleCol(pat.role);
            int rLo = 0, rHi = 0; app.LoopRange(&rLo, &rHi);
            const bool inRange = app.HasBrickRange() && i >= rLo && i <= rHi;
            const bool isPlaying = playingPos == i, isSel = sel_ == i || inRange;
            ImGui::PushID(i);

            ImGui::SetCursorScreenPos(ImVec2(x0 + 1.0f, p0.y));
            ImGui::InvisibleButton("##brick", ImVec2(std::max(4.0f, w - 11.0f), brickH));
            const bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                if (io.KeyShift || t8::desktop::interaction::IsCmd()) {   // U6: Shift / Cmd + click = the range from the last selected brick to this one
                    const int from = anchor_ >= 0 && anchor_ < n ? anchor_ : std::clamp(app.selBrick, 0, n - 1);
                    app.SelectBrickRange(from, i); anchor_ = from;
                    const int lo = app.selBrick;
                    if (proj.currentPattern != proj.songChain[static_cast<size_t>(lo)].patternIndex) SwitchPattern(app, proj.songChain[static_cast<size_t>(lo)].patternIndex);
                    sel_ = lo;
                } else selectBrick(i);   // v44.51: a click only opens the brick for editing; the loop keeps going round the brick that plays (double-click, the ruler or the menu play from here)
            }
            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) app.PlayFromBrick(i);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { selectBrick(i); menuBrick_ = i; menuOpen_ = true; }
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                ImGui::SetDragDropPayload("T8_BRICK", &i, sizeof(int));
                ImGui::Text("Brick %d - drop on another brick to put it there", i + 1);
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("T8_BRICK")) { const int from = *static_cast<const int*>(pl->Data); if (app.MoveChainStep(from, i)) selectBrick(i); }
                ImGui::EndDragDropTarget();
            }
            if (hovered && drag_ == Drag::None && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const int rep = std::max(1, cs.repeats);
                Hint("Brick %d - %.4g bars (x%d%s)\nClick: edit its notes below   Shift+click: a range (LOOP goes round it)   Double-click: play from here   Drag: reorder   Right-click: more\nDrag over empty space or Cmd+A: select bricks   Drag the right edge: longer / shorter (Shift: by one step)",
                                  i + 1, barsOf[static_cast<size_t>(i)], rep, cs.trimSteps ? ", trimmed" : "");
            }

            dl->AddRectFilled(p0, p1, WithAlpha(rc, isPlaying ? 95 : 62), 6.0f);
            dl->AddRect(p0, p1, isPlaying ? vis::kTeal : (isSel ? vis::kAccentHi : WithAlpha(rc, 230)), 6.0f, 0, isPlaying || isSel ? 2.5f : 1.6f);
            dl->PushClipRect(p0, p1, true);
            char num[16]; std::snprintf(num, sizeof num, "%02d", i + 1);
            dl->AddText(nullptr, brickH >= 50.0f ? 17.0f : 15.0f, ImVec2(p0.x + 7, p0.y + 3), vis::kText, num);
            float topLeftEnd = p0.x + 7 + ImGui::GetFont()->CalcTextSizeA(brickH >= 50.0f ? 17.0f : 15.0f, 1.0e6f, 0.0f, num).x;
            if (w > 50.0f) {   // how many times the scene plays
                char rp[16]; if (cs.trimSteps) std::snprintf(rp, sizeof rp, "trim"); else std::snprintf(rp, sizeof rp, "x%d", std::max(1, cs.repeats));
                const float nx = p0.x + 7 + ImGui::GetFont()->CalcTextSizeA(brickH >= 50.0f ? 17.0f : 15.0f, 1.0e6f, 0.0f, num).x + 6.0f;
                dl->AddText(nullptr, 12.0f, ImVec2(nx, p0.y + 5), vis::kAccentHi, rp);
                topLeftEnd = nx + ImGui::GetFont()->CalcTextSizeA(12.0f, 1.0e6f, 0.0f, rp).x;
            }
            if (w > 62.0f) dl->AddText(nullptr, 11.0f, ImVec2(p0.x + 7, p0.y + (brickH >= 50.0f ? 26.0f : 21.0f)), WithAlpha(rc, 255), pat.role == SceneRole::None ? "no role" : RoleName(pat.role));
            if (w > 96.0f && brickH >= 56.0f) { char bs[32]; std::snprintf(bs, sizeof bs, "%.4g bars", barsOf[static_cast<size_t>(i)]); dl->AddText(nullptr, 11.0f, ImVec2(p0.x + 7, p0.y + 41), kDim, bs); }
            {   // what the brick adds to its scene
                char fl[12]; int k = 0;
                if (cs.muteMask) fl[k++] = 'M';
                if (cs.fillMode != FillMode::Off && (cs.fillMode == FillMode::Own ? cs.fill.when != FillWhen::Off : pat.fill.when != FillWhen::Off)) fl[k++] = 'F';
                if (cs.riser >= 0) fl[k++] = 'T';
                if (cs.trimSteps) fl[k++] = 'L';
                if (cs.automation.Any()) fl[k++] = 'D';
                fl[k] = 0;
                if (k > 0 && p1.x - ImGui::CalcTextSize(fl).x - 8 > topLeftEnd + 6.0f) dl->AddText(nullptr, 11.0f, ImVec2(p1.x - ImGui::CalcTextSize(fl).x - 8, p0.y + 6), IM_COL32(240, 160, 60, 235), fl);
            }
            dl->PopClipRect();

            // right edge = length
            ImGui::SetCursorScreenPos(ImVec2(x1 - 10.0f, p0.y));
            ImGui::InvisibleButton("##edge", ImVec2(10.0f, brickH));
            const bool edgeHot = ImGui::IsItemHovered() || (drag_ == Drag::Resize && dragBrick_ == i);
            if (edgeHot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            dl->AddRectFilled(ImVec2(p1.x - 6, p0.y + 14), ImVec2(p1.x - 3, p1.y - 14), edgeHot ? vis::kAccentHi : IM_COL32(255, 255, 255, 90), 1.5f);
            if (ImGui::IsItemActivated()) { app.PushSoundUndo(); drag_ = Drag::Resize; dragBrick_ = i; selectBrick(i); }
            if (ImGui::IsItemActive() && drag_ == Drag::Resize && dragBrick_ == i) app.SetBrickBars(i, (mouse.x - x0) / ppb, io.KeyShift);
            if (ImGui::IsItemDeactivated() && drag_ == Drag::Resize) drag_ = Drag::None;
            ImGui::PopID();
        }

        // "+" brick
        {
            const float ax = X0 + static_cast<float>(totalBars) * ppb + 8.0f;
            const ImVec2 p0(ax, yTop + yBricks), p1(ax + addW - 12.0f, yTop + yBricks + brickH);
            ImGui::SetCursorScreenPos(p0);
            if (ImGui::InvisibleButton("##add", ImVec2(p1.x - p0.x, brickH))) addOpen_ = true;
            const bool hv = ImGui::IsItemHovered();
            dl->AddRect(p0, p1, hv ? vis::kAccentHi : vis::kLineHi, 6.0f, 0, 1.4f);
            dl->AddText(nullptr, 26.0f, ImVec2(p0.x + (p1.x - p0.x) * 0.5f - 7, p0.y + brickH * 0.5f - 14), hv ? vis::kAccentHi : kDim, "+");
            if (hv) Hint("Add a scene to the end of the track");
            if (n == 0) dl->AddText(nullptr, 14.0f, ImVec2(p1.x + 14, p0.y + 10), kDim, "Make a loop on LIVE and press THROW ONTO THE TRACK, or add a scene with +.");
        }


        // ---- fills and transitions lane (v44.10.2) -------------------------------------------------------------------------------
        {
            const RiserPlayer& rp = app.engine.Risers();
            const float fy = yTop + yLane + 1.0f, ty = fy + 14.0f, rh = 12.0f;
            const float laneR = X0 + contentW - addW;
            dl->AddRectFilled(ImVec2(X0 - 2, yTop + yLane), ImVec2(laneR, yTop + yLane + laneH), IM_COL32(255, 255, 255, 5), 3.0f);
            {   // bar lines, so a block can be seen to start and end exactly on a bar
                int sb = 1; for (int c : {1, 2, 4, 8, 16, 32, 64}) { sb = c; if (ppb * c >= 14.0f) break; }
                for (int b = 0; b <= static_cast<int>(std::ceil(totalBars)); b += sb) dl->AddLine(ImVec2(X0 + b * ppb, yTop + yLane), ImVec2(X0 + b * ppb, yTop + yLane + laneH), IM_COL32(255, 255, 255, 12));
            }
            auto snapChoice = [](double v, const int (&c)[4]) { int best = c[0]; for (int x : c) if (std::fabs(x - v) < std::fabs(best - v)) best = x; return best; };
            static const int kTransBars[4] = {1, 2, 4, 8};
            static const char* kKindShort[kNumFillKinds] = {"BREAK", "HATS", "SNARE", "DROP OUT", "RANDOM", "SAMPLE"};

            for (int i = 0; i < n; ++i) {
                ChainStep& cs = proj.songChain[static_cast<size_t>(i)];
                const int pi = std::clamp(cs.patternIndex, 0, kNumPatterns - 1);
                const Pattern& pat = proj.patterns[static_cast<size_t>(pi)];
                const float bx0 = geo[static_cast<size_t>(i)].x0, bx1 = geo[static_cast<size_t>(i)].x1;
                ImGui::PushID(7000 + i);

                // ---- FILL row: the blocks are where the fill really plays (last beats of the pass / of every N bars)
                {
                    const FillRule fr = cs.fillMode == FillMode::Off ? FillRule{} : (cs.fillMode == FillMode::Own ? cs.fill : pat.fill);
                    const double brickBars = barsOf[static_cast<size_t>(i)];
                    const double passBars = std::max(0.0625, static_cast<double>(ChainStepLoopSteps(pat, cs)) / (4.0 * ClampGrid(pat.stepsPerBeat)));
                    const double fillBars = std::max(1, static_cast<int>(fr.beats)) / 4.0;
                    std::vector<std::pair<double, double>> blocks;   // (start bar inside the brick, length in bars)
                    const double shBars = fr.shiftBeats / 4.0;   // v44.25: the block sits this much earlier than the end of the pass / block
                    if (fr.when == FillWhen::LastLoop) { const double l = std::min(fillBars, passBars); blocks.push_back({brickBars - l - std::min(shBars, passBars - l), l}); }
                    else if (fr.when == FillWhen::EveryLoop) { const double l = std::min(fillBars, passBars); for (int k = 1; k <= std::max(1, cs.repeats) && k <= 64; ++k) blocks.push_back({k * passBars - l - std::min(shBars, passBars - l), l}); }
                    else if (fr.when == FillWhen::EveryBars) { const double blk = std::max(1, static_cast<int>(fr.everyBars)); const double l = std::min(fillBars, blk); for (int k = 1; k * blk <= brickBars + 1e-6 && k <= 64; ++k) blocks.push_back({k * blk - l - std::min(shBars, blk - l), l}); }
                    ImGui::SetCursorScreenPos(ImVec2(bx0 + 1.0f, fy));
                    ImGui::InvisibleButton("##fill", ImVec2(std::max(4.0f, bx1 - bx0 - 2.0f), rh));
                    const bool hov = ImGui::IsItemHovered() && (drag_ == Drag::None || (drag_ == Drag::FillLen && dragBrick_ == i));
                    int hovBlock = -1; bool onEdge = false;
                    for (size_t b = 0; b < blocks.size(); ++b) {
                        const float xa = bx0 + static_cast<float>(blocks[b].first) * ppb, xb = xa + std::max(4.0f, static_cast<float>(blocks[b].second) * ppb);
                        if (hov && mouse.x >= xa - 2 && mouse.x <= xb + 2) { hovBlock = static_cast<int>(b); onEdge = mouse.x <= xa + std::min(7.0f, (xb - xa) * 0.4f); }
                    }
                    const bool own = cs.fillMode == FillMode::Own;
                    for (size_t b = 0; b < blocks.size(); ++b) {
                        const float xa = bx0 + static_cast<float>(blocks[b].first) * ppb, xb = xa + std::max(4.0f, static_cast<float>(blocks[b].second) * ppb);
                        const bool hb = hovBlock == static_cast<int>(b) || (drag_ == Drag::FillLen && dragBrick_ == i);
                        dl->AddRectFilled(ImVec2(xa, fy), ImVec2(xb, fy + rh), WithAlpha(kFillCol, static_cast<int>((own ? (hb ? 255 : 215) : (hb ? 200 : 120)) * LevelFrac(cs.levels.fillDb))), 3.0f);
                        if (!own) dl->AddRect(ImVec2(xa, fy), ImVec2(xb, fy + rh), WithAlpha(kFillCol, 230), 3.0f);
                        if (hb) dl->AddRectFilled(ImVec2(xa, fy), ImVec2(xa + 3.0f, fy + rh), IM_COL32(255, 255, 255, 230), 2.0f);
                        if (xb - xa > 42.0f) { dl->PushClipRect(ImVec2(xa, fy), ImVec2(xb, fy + rh), true); dl->AddText(nullptr, 10.0f, ImVec2(xa + 5, fy + 1.5f), IM_COL32(30, 22, 10, 255), kKindShort[static_cast<int>(fr.kind)]); dl->PopClipRect(); }
                    }
                    if (fr.when == FillWhen::Off && hov) {   // ghost: what a click adds
                        const double l = std::min(0.5, brickBars); const float xa = bx1 - static_cast<float>(l) * ppb;
                        dl->AddRect(ImVec2(xa, fy), ImVec2(bx1 - 1.0f, fy + rh), WithAlpha(kFillCol, 200), 3.0f);
                        dl->AddText(nullptr, 10.0f, ImVec2(std::max(bx0 + 4.0f, xa - 60.0f), fy + 1.5f), WithAlpha(kFillCol, 230), "+ fill");
                    }
                    if (ImGui::IsItemActivated()) {
                        selectBrick(i);
                        if (hovBlock >= 0 && onEdge) { app.PushSoundUndo(); drag_ = Drag::FillLen; dragBrick_ = i; dragFill_ = fr; dragRightX_ = bx0 + static_cast<float>(blocks[static_cast<size_t>(hovBlock)].first + blocks[static_cast<size_t>(hovBlock)].second) * ppb; }
                        else if (hovBlock >= 0) {   // v44.25: the body of a block: drag = move it (inside the brick, or onto another brick), click = remove it
                            app.PushSoundUndo(); drag_ = Drag::FillMove; dragBrick_ = i; dragX0_ = mouse.x; dragMoved_ = false; dragTarget_ = i; dragFill_ = fr; dragMode0_ = cs.fillMode; dragFill0_ = cs.fill;
                        }
                        else if (fr.when == FillWhen::Off) { app.PushSoundUndo(); cs.fillMode = FillMode::Own; cs.fill = FillRule{FillWhen::LastLoop, FillKind::Break, 2, 4, 0}; app.sequencer.RefreshArrangement(); }
                    }
                    if (ImGui::IsItemActive() && drag_ == Drag::FillMove && dragBrick_ == i) {
                        const float dx = mouse.x - dragX0_;
                        if (!dragMoved_ && std::fabs(dx) > 3.0f) dragMoved_ = true;
                        if (dragMoved_) {
                            dragTarget_ = -1;
                            for (int j = 0; j < n; ++j) if (mouse.x >= geo[static_cast<size_t>(j)].x0 && mouse.x < geo[static_cast<size_t>(j)].x1) { dragTarget_ = j; break; }
                            if (dragTarget_ == i) {   // inside the same brick: earlier / later by whole beats
                                const double periodBeats = (dragFill_.when == FillWhen::EveryBars ? std::max(1, static_cast<int>(dragFill_.everyBars)) : passBars) * 4.0;
                                const int maxShift = std::max(0, static_cast<int>(std::floor(periodBeats + 1e-6)) - static_cast<int>(dragFill_.beats));
                                const int ns = std::clamp(static_cast<int>(dragFill_.shiftBeats) - static_cast<int>(std::lround(dx / ppb * 4.0)), 0, maxShift);
                                FillRule nr = dragFill_; nr.shiftBeats = static_cast<uint8_t>(ns);
                                if (cs.fillMode != FillMode::Own || !(cs.fill == nr)) { cs.fillMode = FillMode::Own; cs.fill = NormalizeFill(nr); app.sequencer.RefreshArrangement(); }
                                ValueTip(ns == 0 ? "at the end" : "%d beat%s before the end", ns, ns == 1 ? "" : "s");
                            } else {
                                if (cs.fillMode != dragMode0_ || !(cs.fill == dragFill0_)) { cs.fillMode = dragMode0_; cs.fill = dragFill0_; app.sequencer.RefreshArrangement(); }
                                if (dragTarget_ >= 0) {
                                    const float tx0 = geo[static_cast<size_t>(dragTarget_)].x0, tx1 = geo[static_cast<size_t>(dragTarget_)].x1;
                                    dl->AddRectFilled(ImVec2(tx0 + 1.0f, fy), ImVec2(tx1 - 1.0f, fy + rh), WithAlpha(kFillCol, 70), 3.0f);
                                    dl->AddRect(ImVec2(tx0 + 1.0f, fy), ImVec2(tx1 - 1.0f, fy + rh), WithAlpha(kFillCol, 255), 3.0f, 0, 1.6f);
                                    ValueTip("move the fill to brick %d", dragTarget_ + 1);
                                } else ValueTip("drop it on a brick");
                            }
                        }
                    }
                    if (ImGui::IsItemDeactivated() && drag_ == Drag::FillMove) {
                        if (!dragMoved_) { cs.fillMode = FillMode::Off; app.sequencer.RefreshArrangement(); app.SetStatus("FILL REMOVED (UNDO BRINGS IT BACK)"); }
                        else if (dragTarget_ >= 0 && dragTarget_ != i) {
                            ChainStep& d = proj.songChain[static_cast<size_t>(dragTarget_)];
                            FillRule nr = dragFill_; nr.shiftBeats = 0;
                            d.fillMode = FillMode::Own; d.fill = NormalizeFill(nr); d.levels.fillDb = cs.levels.fillDb;
                            cs.fillMode = FillMode::Off; cs.fill = dragFill0_; cs.levels.fillDb = 0;
                            app.sequencer.RefreshArrangement(); selectBrick(dragTarget_);
                            app.SetStatus("FILL MOVED TO BRICK " + std::to_string(dragTarget_ + 1));
                        } else if (cs.fillMode != dragMode0_ || !(cs.fill == dragFill0_)) {
                            if (dragTarget_ < 0) { cs.fillMode = dragMode0_; cs.fill = dragFill0_; app.sequencer.RefreshArrangement(); }
                        }
                        drag_ = Drag::None;
                    }
                    if (ImGui::IsItemActive() && drag_ == Drag::FillLen && dragBrick_ == i) {
                        const int nb = snapChoice((dragRightX_ - mouse.x) / ppb * 4.0, kFillBeatChoices);
                        FillRule nr = dragFill_; nr.beats = static_cast<uint8_t>(nb);
                        if (cs.fillMode != FillMode::Own || !(cs.fill == nr)) { cs.fillMode = FillMode::Own; cs.fill = NormalizeFill(nr); app.sequencer.RefreshArrangement(); }
                        ValueTip("%d beat%s (%g bar%s)", nb, nb == 1 ? "" : "s", nb / 4.0, nb == 4 ? "" : "s");
                    }
                    if (ImGui::IsItemDeactivated() && drag_ == Drag::FillLen) drag_ = Drag::None;
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { selectBrick(i); fxMenuBrick_ = i; fxMenuKind_ = 0; fxMenuOpen_ = true; }
                    if (onEdge) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    else if (hovBlock >= 0 || drag_ == Drag::FillMove) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (hov && drag_ == Drag::None) {
                        if (hovBlock >= 0) Hint("FILL on brick %d: %s, %d beat%s, %s\\n%s\\nDrag the block: move it (inside the brick or onto another brick).  Click: remove it.  Drag the left edge: 1 / 2 / 4 / 8 beats.  Right-click: kind, break, when, length, none.",
                                                             i + 1, FillKindName(fr.kind), fr.beats, fr.beats == 1 ? "" : "s", FillWhenName(fr.when),
                                                             own ? "This brick has its own fill." : (cs.fillMode == FillMode::Scene ? "Taken from the scene's fill (dragging gives the brick its own)." : ""));
                        else if (fr.when == FillWhen::Off) Hint("Click: add a fill at the end of brick %d (2 beats).  Right-click: more.", i + 1);
                    }
                }

                // ---- TRANSITION row: a riser that LANDS exactly on the first beat of this brick (i >= 1) and starts up to N bars before it
                if (i >= 1) {
                    const double prevBars = barsOf[static_cast<size_t>(i - 1)];
                    const bool has = cs.riser >= 0;
                    const double winBars = has ? std::min(static_cast<double>(cs.riserBars), prevBars) : 0.0;
                    const float prevW = bx0 - geo[static_cast<size_t>(i - 1)].x0;
                    const float zoneW = std::clamp(std::max(static_cast<float>(winBars) * ppb + 8.0f, std::min(ppb * 2.0f, prevW * 0.5f)), 10.0f, prevW);
                    ImGui::SetCursorScreenPos(ImVec2(bx0 - zoneW, ty));
                    ImGui::InvisibleButton("##trans", ImVec2(zoneW, rh));
                    const bool hov = ImGui::IsItemHovered() && (drag_ == Drag::None || (drag_ == Drag::TransLen && dragBrick_ == i));
                    const float xa = bx0 - static_cast<float>(winBars) * ppb;
                    const bool onEdge = has && hov && mouse.x <= xa + 7.0f && mouse.x >= xa - 4.0f;
                    if (has) {
                        const bool hb = hov || (drag_ == Drag::TransLen && dragBrick_ == i);
                        const ImU32 c0 = WithAlpha(kTransCol, 25), c1 = WithAlpha(kTransCol, static_cast<int>((hb ? 255 : 220) * LevelFrac(cs.levels.transitionDb)));
                        dl->AddRectFilledMultiColor(ImVec2(xa, ty), ImVec2(bx0, ty + rh), c0, c1, c1, c0);
                        dl->AddRect(ImVec2(xa, ty), ImVec2(bx0, ty + rh), WithAlpha(kTransCol, 230), 3.0f);
                        dl->AddLine(ImVec2(bx0, ty - 2), ImVec2(bx0, ty + rh + 2), IM_COL32(255, 255, 255, 235), 2.0f);   // the landing: the first beat of the brick
                        if (hb) dl->AddRectFilled(ImVec2(xa, ty), ImVec2(xa + 3.0f, ty + rh), IM_COL32(255, 255, 255, 230), 2.0f);
                        const std::string nm = rp.NameAt(cs.riser).empty() ? std::string("?") : rp.NameAt(cs.riser);
                        if (bx0 - xa > 54.0f) { dl->PushClipRect(ImVec2(xa, ty), ImVec2(bx0, ty + rh), true); dl->AddText(nullptr, 10.0f, ImVec2(xa + 5, ty + 1.5f), IM_COL32(236, 232, 255, 255), nm.c_str()); dl->PopClipRect(); }
                    } else if (!hov) {   // v44.51: always show a quiet "+" so the row is findable without hovering
                        const float gw = std::min(ppb * 2.0f, zoneW);
                        dl->AddRect(ImVec2(bx0 - gw, ty + 1), ImVec2(bx0, ty + rh - 1), WithAlpha(kTransCol, 70), 3.0f);
                        if (gw > 40.0f) dl->AddText(nullptr, 10.0f, ImVec2(bx0 - gw + 4, ty + 1.5f), WithAlpha(kTransCol, 110), "+ transition");
                        else dl->AddText(nullptr, 10.0f, ImVec2(bx0 - gw + 3, ty + 1.5f), WithAlpha(kTransCol, 110), "+");
                    } else {
                        dl->AddRect(ImVec2(bx0 - std::min(ppb * 2.0f, zoneW), ty), ImVec2(bx0, ty + rh), WithAlpha(kTransCol, 200), 3.0f);
                        dl->AddText(nullptr, 10.0f, ImVec2(bx0 - std::min(ppb * 2.0f, zoneW) + 4, ty + 1.5f), WithAlpha(kTransCol, 240), "+ transition");
                    }
                    if (ImGui::IsItemActivated()) {
                        selectBrick(i);
                        if (onEdge) { app.PushSoundUndo(); drag_ = Drag::TransLen; dragBrick_ = i; dragRightX_ = bx0; }
                        else if (has && hov && mouse.x >= xa) { app.PushSoundUndo(); drag_ = Drag::TransMove; dragBrick_ = i; dragX0_ = mouse.x; dragMoved_ = false; dragTarget_ = i; }   // v44.25: drag = to another brick, click = remove
                        else if (!has) {
                            app.PushSoundUndo();
                            uint8_t b = 2;
                            int r = RoleTransition(proj.patterns[static_cast<size_t>(std::clamp(proj.songChain[static_cast<size_t>(i - 1)].patternIndex, 0, kNumPatterns - 1))].role, pat.role, &b);
                            if (r < 0 || r >= rp.Count() || rp.NameAt(r).empty()) { r = -1; for (int q = 0; q < rp.Count(); ++q) if (!rp.NameAt(q).empty()) { r = q; break; } b = 2; }
                            if (r >= 0) { cs.riser = static_cast<int8_t>(r); cs.riserBars = static_cast<uint8_t>(snapChoice(b, kTransBars)); app.sequencer.RefreshArrangement(); }
                            else { app.DropLastUndo(); app.SetStatus("NO TRANSITION SOUND FOUND: ADD RISERS TO THE RISER FOLDER OR RIGHT-CLICK FOR OTHERS"); }
                        }
                    }
                    if (ImGui::IsItemActive() && drag_ == Drag::TransLen && dragBrick_ == i) {
                        const int nb = snapChoice((dragRightX_ - mouse.x) / ppb, kTransBars);
                        if (cs.riserBars != nb) { cs.riserBars = static_cast<uint8_t>(nb); app.sequencer.RefreshArrangement(); }
                        Hint("%d bar%s before the brick%s", nb, nb == 1 ? "" : "s", nb > prevBars + 1e-6 ? "\\n(the brick before is shorter: it starts at the beginning of that one)" : "");
                    }
                    if (ImGui::IsItemDeactivated() && drag_ == Drag::TransLen) drag_ = Drag::None;
                    if (ImGui::IsItemActive() && drag_ == Drag::TransMove && dragBrick_ == i) {
                        if (!dragMoved_ && std::fabs(mouse.x - dragX0_) > 3.0f) dragMoved_ = true;
                        if (dragMoved_) {   // the transition lands on a first beat: it can only be dropped on a boundary, the nearest one
                            float best = 1e9f; dragTarget_ = i;
                            for (int j = 1; j < n; ++j) { const float d = std::fabs(mouse.x - geo[static_cast<size_t>(j)].x0); if (d < best) { best = d; dragTarget_ = j; } }
                            const float tx = geo[static_cast<size_t>(dragTarget_)].x0;
                            const float tw = static_cast<float>(std::min(static_cast<double>(cs.riserBars), barsOf[static_cast<size_t>(dragTarget_ - 1)])) * ppb;
                            dl->AddRectFilled(ImVec2(tx - tw, ty), ImVec2(tx, ty + rh), WithAlpha(kTransCol, dragTarget_ == i ? 60 : 130), 3.0f);
                            dl->AddRect(ImVec2(tx - tw, ty), ImVec2(tx, ty + rh), WithAlpha(kTransCol, 255), 3.0f, 0, 1.6f);
                            if (dragTarget_ == i) ValueTip("stays before brick %d", i + 1); else ValueTip("move the transition before brick %d", dragTarget_ + 1);
                        }
                    }
                    if (ImGui::IsItemDeactivated() && drag_ == Drag::TransMove) {
                        if (!dragMoved_) { cs.riser = -1; app.sequencer.RefreshArrangement(); app.SetStatus("TRANSITION REMOVED (UNDO BRINGS IT BACK)"); }
                        else if (dragTarget_ >= 1 && dragTarget_ != i) {
                            ChainStep& d = proj.songChain[static_cast<size_t>(dragTarget_)];
                            d.riser = cs.riser; d.riserBars = cs.riserBars; d.levels.transitionDb = cs.levels.transitionDb;
                            cs.riser = -1; cs.levels.transitionDb = 0;
                            app.sequencer.RefreshArrangement(); selectBrick(dragTarget_);
                            app.SetStatus("TRANSITION MOVED BEFORE BRICK " + std::to_string(dragTarget_ + 1));
                        }
                        drag_ = Drag::None;
                    }
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { selectBrick(i); fxMenuBrick_ = i; fxMenuKind_ = 1; fxMenuOpen_ = true; }
                    if (onEdge) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    else if ((has && hov && mouse.x >= xa) || drag_ == Drag::TransMove) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (hov && drag_ == Drag::None) {
                        if (has) Hint("TRANSITION into brick %d: %s.\\nIt starts up to %d bar%s before the brick (on a bar line) and its peak lands exactly on the first beat (the white line).\\nDrag the block: onto another brick's start.  Click: remove it.  Drag the left edge: 1 / 2 / 4 / 8 bars.  Right-click: sound, length, none.",
                                                   i + 1, rp.NameAt(cs.riser).c_str(), cs.riserBars, cs.riserBars == 1 ? "" : "s");
                        else Hint("Click: add a transition (riser / impact) that lands on the first beat of brick %d.  Right-click: more.", i + 1);
                    }
                }
                ImGui::PopID();
            }
        }

        if (det) {
        // ---- who plays ---------------------------------------------------------------------------------------------------
        auto cellAt = [&](ImVec2 m, int* bi, int* ti) {
            if (m.y < yTop + yMatrix || m.y >= yTop + yMatrix + matrixH) return false;
            *ti = std::clamp(static_cast<int>((m.y - (yTop + yMatrix)) / rowH), 0, kNumTracks - 1);
            for (int i = 0; i < n; ++i) if (m.x >= geo[static_cast<size_t>(i)].x0 && m.x < geo[static_cast<size_t>(i)].x1) { *bi = i; return true; }
            return false;
        };
        for (int t = 0; t < kNumTracks; ++t) {
            const float y = yTop + yMatrix + t * rowH;
            if ((t & 1) == 0) dl->AddRectFilled(ImVec2(X0 - 2, y), ImVec2(X0 + contentW - addW, y + rowH), IM_COL32(255, 255, 255, 6));
            for (int i = 0; i < n; ++i) {
                const ChainStep& cs = proj.songChain[static_cast<size_t>(i)];
                const int pi = std::clamp(cs.patternIndex, 0, kNumPatterns - 1);
                const Pattern& pat = proj.patterns[static_cast<size_t>(pi)];
                const bool has = app.TrackHasNotes(pi, t), muted = (cs.muteMask >> t) & 1u, sceneMuted = pat.tracks[static_cast<size_t>(t)].mute;
                const ImU32 rc = RoleCol(pat.role);
                const ImVec2 a(geo[static_cast<size_t>(i)].x0 + 2, y + 2), b(geo[static_cast<size_t>(i)].x1 - 2, y + rowH - 2);
                if (!has) { dl->AddRectFilled(a, b, IM_COL32(25, 28, 37, 255), 3.0f); continue; }
                if (muted) { dl->AddRectFilled(a, b, IM_COL32(36, 40, 54, 255), 3.0f); dl->AddRect(a, b, WithAlpha(rc, 70), 3.0f); }
                else if (cs.levels.TrackAny(t)) {   // v44.13: the bar rises / falls like the level of the instrument over the brick
                    const float h = b.y - a.y, fs = LevelFrac(cs.levels.track[t].Start()), fe = LevelFrac(cs.levels.track[t].End());
                    dl->AddRectFilled(a, b, IM_COL32(36, 40, 54, 255), 3.0f);
                    const ImVec2 q[4] = {ImVec2(a.x, b.y - h * fs), ImVec2(b.x, b.y - h * fe), ImVec2(b.x, b.y), ImVec2(a.x, b.y)};
                    dl->AddConvexPolyFilled(q, 4, WithAlpha(rc, sceneMuted ? 80 : 225));
                }
                else dl->AddRectFilled(a, b, WithAlpha(rc, sceneMuted ? 80 : 215), 3.0f);
                if ((pat.ownSound >> t) & 1u) {   // v44.24: this brick has its own sound for the instrument
                    const ImVec2 c(b.x - 6.0f, a.y + 5.0f);
                    dl->AddCircleFilled(c, 3.2f, IM_COL32(255, 255, 255, 255));
                    dl->AddCircle(c, 3.2f, IM_COL32(150, 80, 220, 255), 0, 1.4f);
                }
            }
        }
        {   // click / drag to switch cells
            int bi = -1, ti = -1;
            const bool over = winHovered && cellAt(mouse, &bi, &ti);
            if (drag_ == Drag::None && over && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {   // v44.13: the level shapes of this instrument on this brick
                selectBrick(bi); lvlMenuBrick_ = bi; lvlMenuTrack_ = ti; lvlMenuOpen_ = true;
            }
            if (drag_ == Drag::None && over && io.KeyShift && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemActive()) {
                const ChainStep& cs = proj.songChain[static_cast<size_t>(bi)];
                const bool endHalf = mouse.x > 0.5f * (geo[static_cast<size_t>(bi)].x0 + geo[static_cast<size_t>(bi)].x1);
                app.PushSoundUndo();
                drag_ = Drag::TrackLevel; dragBrick_ = bi; dragTrack_ = ti; dragEnd_ = endHalf; dragY0_ = mouse.y;
                dragDb0_ = endHalf ? cs.levels.track[ti].End() : cs.levels.track[ti].Start(); selectBrick(bi);
                app.seqCursorTrack = ti;
            } else if (drag_ == Drag::None && over && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemActive()) {
                const ChainStep& cs = proj.songChain[static_cast<size_t>(bi)];
                if (app.TrackHasNotes(std::clamp(cs.patternIndex, 0, kNumPatterns - 1), ti)) {
                    paintMuted_ = !(((cs.muteMask >> ti) & 1u) != 0);
                    app.PushSoundUndo();
                    app.SetBrickTrackMuted(bi, ti, paintMuted_);
                    drag_ = Drag::Paint; selectBrick(bi);
                }
            } else if (drag_ == Drag::TrackLevel) {
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag_ = Drag::None;
                else {
                    int db = dragDb0_ + static_cast<int>(std::lround((dragY0_ - mouse.y) * 0.6f));
                    db = db <= kAutoMinDb + 1 ? kAutoMinDb : std::min(db, 0);
                    app.SetBrickTrackLevel(dragBrick_, dragTrack_, dragEnd_, db);
                    ValueTip("%s  %s of the brick: %s", TrackNameShort(dragTrack_), dragEnd_ ? "end" : "start", DbText(db).c_str());
                }
            } else if (drag_ == Drag::Paint) {
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag_ = Drag::None;
                else if (over) {
                    const ChainStep& cs = proj.songChain[static_cast<size_t>(bi)];
                    if (app.TrackHasNotes(std::clamp(cs.patternIndex, 0, kNumPatterns - 1), ti) && (((cs.muteMask >> ti) & 1u) != 0) != paintMuted_) app.SetBrickTrackMuted(bi, ti, paintMuted_);
                }
            }
            if (over && drag_ == Drag::None && n > 0) {
                const ChainStep& cs = proj.songChain[static_cast<size_t>(bi)];
                if (cs.levels.TrackAny(ti)) Hint("%s on brick %d: level %s -> %s\nClick: off / on.  Shift + drag up / down: level (left half = start, right half = end).  Right-click: shapes.", TrackNameShort(ti), bi + 1, DbText(cs.levels.track[ti].Start()).c_str(), DbText(cs.levels.track[ti].End()).c_str());
                else Hint("Click or drag: switch this instrument off / on for the brick.\nShift + drag up / down: its level (left half = start, right half = end).  Right-click: fades and shapes.\nGrey = the scene has no notes for it.  A white dot = this brick has its own sound for the instrument (SOUND).");
            }
        }

        // ---- dynamics: the lane rows (v44.21) --------------------------------------------------------------------------------
        // MIX, the DJ filter and the selected instrument are drawn and edited the same way: a line of free points over each brick.
        // Click on empty space = add a point, drag = move it (sideways too, except the two ends), double-click an inner point =
        // remove it, Option + click = back to 0, Shift = finer, Esc while dragging = cancel, right-click = ready-made shapes.
        {
            const int rows = std::clamp(static_cast<int>(dynH / laneMin), 1, laneRows);
            const float rh = dynH / static_cast<float>(rows);
            const int ids[4] = {kLaneMix, std::clamp(app.seqCursorTrack, 0, kNumTracks - 1), kLaneFilter, kLaneParam0};   // v44.51: the instrument level is the second row, so a short window still shows it
            const float xr = X0 + contentW - addW;
            auto bip = [](int id) { return id == kLaneFilter || id == kLaneParam0; };   // -100..100 around a middle line
            auto laneOf = [&](int brick, int id) -> Lane* {   // v44.38: the parameter row is looked up by the chosen instrument and parameter
                if (id != kLaneParam0) return app.BrickLane(brick, id);
                if (brick < 0 || brick >= n) return nullptr;
                StepParams& sp = proj.songChain[static_cast<size_t>(brick)].params;
                const int k = sp.Find(parTrack_, parParam_);
                return k >= 0 ? &sp.row[k].lane : nullptr;
            };
            auto laneY = [&](int id, float v, float top, float h) {   // value -> screen y inside a row (top, height h)
                const float pad = 4.0f, span = h - 2.0f * pad;
                if (bip(id)) return top + pad + span * 0.5f * (1.0f - std::clamp(v, -100.0f, 100.0f) / 100.0f);
                // v44.51: 0 .. -36 dB over the upper 80 % of the row, -36 .. -48 over the next 10 %, the last tenth = off: the bottom of the row is
                // really silence and the "off" zone is wide enough to hit (a point dragged to the bottom used to stay at about -30 dB).
                if (v <= static_cast<float>(kAutoMinDb)) return top + pad + span;
                if (v >= 0.0f) return top + pad;
                const float f = v > -36.0f ? 0.8f * (-v / 36.0f) : 0.8f + 0.1f * ((-v - 36.0f) / 12.0f);
                return top + pad + span * std::min(f, 1.0f);
            };
            auto laneVal = [&](int id, float y, float top, float h) {   // screen y -> value (the same mapping backwards)
                const float pad = 4.0f, span = std::max(1.0f, h - 2.0f * pad);
                if (bip(id)) { int v = static_cast<int>(std::lround(std::clamp((top + pad + span * 0.5f - y) / (span * 0.5f), -1.0f, 1.0f) * 100.0f)); return std::abs(v) <= 3 ? 0 : v; }
                const float t = (y - top - pad) / span;
                if (t >= 0.9f) return kAutoMinDb;   // the bottom tenth of the row is "off"
                if (t <= 0.8f) return -static_cast<int>(std::lround(std::clamp(t / 0.8f, 0.0f, 1.0f) * 36.0f));
                return -static_cast<int>(std::lround(36.0f + (t - 0.8f) / 0.1f * 11.0f));
            };
            auto valText = [&](int id, int v) { return id == kLaneParam0 ? ParamValueText(parParam_, v) : id == kLaneFilter ? (v == 0 ? std::string("open") : (v < 0 ? "low-pass " : "high-pass ") + std::to_string(std::abs(v))) : DbText(v); };
            // ---- a drag in progress: follow the mouse, Esc cancels --------------------------------------------------------
            if (drag_ == Drag::Point) {
                Lane* ln = laneOf(dragBrick_, dragLane_);
                if (!ln) drag_ = Drag::None;
                else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { *ln = dragLane0_; app.DropLastUndo(); drag_ = Drag::None; }
                else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { int lo, hi; LaneRange(dragLane_, &lo, &hi); ln->Normalize(lo, hi); drag_ = Drag::None; }   // an all-zero lane goes back to flat on release
                else {
                    int lo, hi; LaneRange(dragLane_, &lo, &hi);
                    const int r = dragRow_;
                    const float top = yTop + yDyn + r * rh;
                    float y = mouse.y;
                    if (io.KeyShift) y = dragY0_ + (mouse.y - dragY0_) * 0.2f;   // Shift: five times finer
                    const int v = laneVal(dragLane_, y + dragYOff_, top, rh);
                    const float gx0 = geo[static_cast<size_t>(dragBrick_)].x0 + 4.0f, gx1 = geo[static_cast<size_t>(dragBrick_)].x1 - 4.0f;
                    const int p = static_cast<int>(std::lround(std::clamp((mouse.x - gx0) / std::max(1.0f, gx1 - gx0), 0.0f, 1.0f) * kLanePosMax));
                    ln->MovePoint(dragPt_, p, v, lo, hi, true);   // v44.25: stays editable while the mouse is down (even when every point is 0)
                    ValueTip("%s", valText(dragLane_, ln->n ? ln->val[std::min<int>(dragPt_, ln->n - 1)] : 0).c_str());
                }
            }
            for (int r = 0; r < rows; ++r) {
                const int id = ids[r];
                const float top = yTop + yDyn + r * rh;
                dl->AddRectFilled(ImVec2(X0 - 2, top + 1), ImVec2(xr, top + rh - 1), IM_COL32(24, 26, 33, 255), 4.0f);
                if (bip(id)) dl->AddLine(ImVec2(X0 - 2, laneY(id, 0, top, rh)), ImVec2(xr, laneY(id, 0, top, rh)), IM_COL32(255, 255, 255, 30));
                else for (int db : {0, -12, -24}) dl->AddLine(ImVec2(X0 - 2, laneY(id, static_cast<float>(db), top, rh)), ImVec2(xr, laneY(id, static_cast<float>(db), top, rh)), IM_COL32(255, 255, 255, db == 0 ? 28 : 12));
                for (int i = 0; i < n; ++i) {
                    ChainStep& cs = proj.songChain[static_cast<size_t>(i)];
                    Lane* ln = laneOf(i, id);
                    if (!ln && id == kLaneParam0) {   // v44.38: this brick has no row for the chosen parameter: a click in its cell starts one
                        const float gx0 = geo[static_cast<size_t>(i)].x0, gx1 = geo[static_cast<size_t>(i)].x1;
                        dl->AddLine(ImVec2(gx0, top + 1), ImVec2(gx0, top + rh - 1), IM_COL32(255, 255, 255, 22));
                        const bool inC = winHovered && drag_ == Drag::None && mouse.x >= gx0 + 1.0f && mouse.x < gx1 - 1.0f && mouse.y >= top && mouse.y < top + rh;
                        dl->AddText(nullptr, 10.0f, ImVec2((gx0 + gx1) * 0.5f - 3.0f, top + rh * 0.5f - 5.0f), WithAlpha(TrackColor(parTrack_), inC ? 200 : 60), "+");
                        if (inC) {
                            Hint("Brick %d has no %s %s row yet. Click to start one, then drag the dots.", i + 1, kTrk[std::clamp(parTrack_, 0, kNumTracks - 1)], AutoParamName(parParam_));
                            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemActive()) { selectBrick(i); app.AddBrickParamRow(i, parTrack_, parParam_); }
                        }
                        continue;
                    }
                    if (!ln) continue;
                    const float x0 = geo[static_cast<size_t>(i)].x0, x1 = geo[static_cast<size_t>(i)].x1;
                    const float gx0 = x0 + 4.0f, gx1 = x1 - 4.0f;
                    const ImU32 rc = RoleCol(proj.patterns[static_cast<size_t>(std::clamp(cs.patternIndex, 0, kNumPatterns - 1))].role);
                    const ImU32 lineCol = id == kLaneFilter ? kAmberHpf : (id == kLaneParam0 ? TrackColor(std::clamp(parTrack_, 0, kNumTracks - 1)) : (id < kArrTracks ? rc : vis::kTeal));
                    dl->AddLine(ImVec2(x0, top + 1), ImVec2(x0, top + rh - 1), IM_COL32(255, 255, 255, 22));
                    auto px = [&](int k) { return gx0 + (gx1 - gx0) * static_cast<float>(ln->pos[k]) / static_cast<float>(kLanePosMax); };
                    auto py = [&](int k) { return laneY(id, static_cast<float>(ln->val[k]), top, rh); };
                    const bool flat = ln->n < 2;
                    const float zeroY = laneY(id, 0.0f, top, rh);
                    if (flat) dl->AddLine(ImVec2(gx0, zeroY), ImVec2(gx1, zeroY), WithAlpha(lineCol, 90), 1.4f);
                    for (int k = 0; k + 1 < ln->n; ++k) {
                        const ImVec2 pa(px(k), py(k)), pb(px(k + 1), py(k + 1));
                        ImU32 col = lineCol;
                        if (id == kLaneFilter) col = (ln->val[k] + ln->val[k + 1]) < 0 ? kVioletLpf : kAmberHpf;   // amber = high-pass, violet = low-pass
                        if (!bip(id)) {
                            const float bot = top + rh - 4.0f;
                            const ImVec2 q[4] = {pa, pb, ImVec2(pb.x, bot), ImVec2(pa.x, bot)};
                            dl->AddConvexPolyFilled(q, 4, WithAlpha(id < kArrTracks ? rc : vis::kTeal, 38));
                        }
                        dl->AddLine(pa, pb, col, 2.2f);
                    }
                    // the points: the ends of a flat lane are shown as ghosts, so the first click has something to grab
                    const int shown = flat ? 2 : ln->n;
                    int hotPt = -1;
                    for (int k = 0; k < shown; ++k) {
                        const ImVec2 c = flat ? ImVec2(k ? gx1 : gx0, zeroY) : ImVec2(px(k), py(k));
                        const bool dragged = drag_ == Drag::Point && dragBrick_ == i && dragLane_ == id && dragPt_ == k;
                        const bool near = winHovered && drag_ == Drag::None && std::fabs(mouse.x - c.x) < 9 && std::fabs(mouse.y - c.y) < 9;
                        if (near) hotPt = k;
                        const bool big = dragged || near;
                        dl->AddCircleFilled(c, big ? 5.5f : 4.0f, big ? vis::kAccentHi : lineCol);
                        dl->AddCircle(c, big ? 5.5f : 4.0f, IM_COL32(20, 22, 28, 255), 0, 1.3f);
                    }
                    // ---- gestures --------------------------------------------------------------------------------------------
                    const bool inCell = winHovered && drag_ == Drag::None && mouse.x >= x0 + 1.0f && mouse.x < x1 - 1.0f && mouse.y >= top && mouse.y < top + rh && !ImGui::IsPopupOpen("DynMenu");
                    if (inCell) {
                        int lo, hi; LaneRange(id, &lo, &hi);
                        if (id == kLaneParam0) Hint("%s %s of brick %d (%s).\nClick on empty space = add a point, drag the points.  Shift: finer.  Double-click an inner point: remove it.  Right-click: the PARAM page.\n%s.", kTrk[std::clamp(parTrack_, 0, kNumTracks - 1)], AutoParamName(parParam_), i + 1, hotPt >= 0 ? valText(id, ln->val[hotPt]).c_str() : "0 = the sound as it is", ParamHundred(parParam_));
                        else if (hotPt >= 0) Hint("%s of brick %d, point %d (%s).\nDrag: move it.  Shift: finer.  Option + click: back to 0.  Double-click an inner point: remove it.  Esc while dragging: cancel.", id == kLaneMix ? "MIX" : id == kLaneFilter ? "FILTER" : TrackNameShort(id), i + 1, hotPt + 1, valText(id, flat ? 0 : ln->val[hotPt]).c_str());
                        else Hint("%s of brick %d: click on empty space = add a point, drag the points.\n%s Right-click: ready-made shapes.", id == kLaneMix ? "MIX (dB)" : id == kLaneFilter ? "FILTER: up = high-pass thins the low end, down = low-pass darkens the sound, the middle line = open" : TrackNameShort(id), i + 1,
                                  id == kLaneFilter ? "" : "All the way down = silence.");
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                            selectBrick(i);
                            if (id == kLaneParam0) page_ = 3;   // v44.38: the PARAM page has the shapes and the numbers
                            else if (id < kArrTracks) { app.seqCursorTrack = id; lvlMenuBrick_ = i; lvlMenuTrack_ = id; lvlMenuOpen_ = true; }   // the shapes of this instrument
                            else { dynMenuBrick_ = i; dynMenuOpen_ = true; }                                                              // the shapes of the whole brick
                        }
                        else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemActive()) {
                            selectBrick(i);
                            if (id < kArrTracks) app.seqCursorTrack = id;
                            const float ymid = hotPt >= 0 ? (flat ? zeroY : py(hotPt)) : mouse.y;
                            if (hotPt >= 0 && io.KeyAlt) { app.PushSoundUndo(); ln->Materialize(); ln->MovePoint(hotPt, ln->pos[hotPt], 0, lo, hi); }
                            else if (hotPt >= 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !flat && hotPt > 0 && hotPt < ln->n - 1 && ImGui::GetTime() - lastAddT_ > 0.5) {
                                app.PushSoundUndo(); ln->RemovePoint(hotPt);
                            } else if (hotPt >= 0) {
                                app.PushSoundUndo(); ln->Materialize();
                                drag_ = Drag::Point; dragBrick_ = i; dragLane_ = id; dragPt_ = hotPt; dragRow_ = r; dragLane0_ = *ln; dragY0_ = mouse.y; dragYOff_ = ymid - mouse.y;
                            } else {
                                app.PushSoundUndo();
                                const Lane before = *ln;
                                const int p = static_cast<int>(std::lround(std::clamp((mouse.x - gx0) / std::max(1.0f, gx1 - gx0), 0.0f, 1.0f) * kLanePosMax));
                                const int at = ln->AddPoint(p, laneVal(id, mouse.y, top, rh), lo, hi);
                                if (at < 0) { *ln = before; app.DropLastUndo(); app.SetStatus(before.n >= kMaxLanePts ? "LANE FULL (6 POINTS)" : "TOO CLOSE TO ANOTHER POINT"); }
                                else { drag_ = Drag::Point; dragBrick_ = i; dragLane_ = id; dragPt_ = at; dragRow_ = r; dragLane0_ = before; dragY0_ = mouse.y; dragYOff_ = 0.0f; lastAddT_ = ImGui::GetTime(); }
                            }
                        }
                    }
                }
            }
            // the name of the shape over each column (first row), as before
            for (int i = 0; i < n; ++i) {
                const float x0 = geo[static_cast<size_t>(i)].x0, x1 = geo[static_cast<size_t>(i)].x1;
                const char* pn = DynPresetName(proj.songChain[static_cast<size_t>(i)].automation);
                if (pn && x1 - x0 > 60.0f && rh >= 22.0f) dl->AddText(nullptr, 10.0f, ImVec2(x0 + 6.0f, yTop + yDyn + 2.0f), IM_COL32(220, 224, 236, 170), pn);
            }
        }

        }   // det

        // ---- rubber band (U6): press on empty space of the brick row and drag over bricks = select that range ---------------------------
        {
            const float bandY0 = yTop + yBricks - 3.0f, bandY1 = yTop + yBricks + brickH + 3.0f;
            if (!marquee_ && drag_ == Drag::None && winHovered && n > 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered() &&
                !ImGui::IsAnyItemActive() && mouse.y >= bandY0 && mouse.y <= bandY1) {
                marquee_ = true; marqueeX0_ = mouse.x - o.x; marqueeLo_ = marqueeHi_ = -2;
            }
            if (marquee_) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    const float ax = o.x + std::min(marqueeX0_, mouse.x - o.x), bx = o.x + std::max(marqueeX0_, mouse.x - o.x);
                    if (std::fabs(mouse.x - o.x - marqueeX0_) > 3.0f) {
                        int lo = -1, hi = -1;
                        for (int i = 0; i < n; ++i) if (geo[static_cast<size_t>(i)].x1 > ax && geo[static_cast<size_t>(i)].x0 < bx) { if (lo < 0) lo = i; hi = i; }
                        if (lo != marqueeLo_ || hi != marqueeHi_) {
                            marqueeLo_ = lo; marqueeHi_ = hi;
                            if (lo >= 0) { app.SelectBrickRange(lo, hi); anchor_ = lo; sel_ = lo; }
                        }
                        dl->AddRectFilled(ImVec2(ax, bandY0 - 4.0f), ImVec2(bx, bandY1 + 4.0f), WithAlpha(vis::kAccentHi, 38), 3.0f);
                        dl->AddRect(ImVec2(ax, bandY0 - 4.0f), ImVec2(bx, bandY1 + 4.0f), vis::kAccentHi, 3.0f, 0, 1.5f);
                    }
                } else {
                    if (marqueeLo_ == -2) app.ClearBrickRange();   // a plain click on empty space: no range any more
                    else if (marqueeLo_ >= 0 && proj.currentPattern != proj.songChain[static_cast<size_t>(marqueeLo_)].patternIndex)
                        SwitchPattern(app, proj.songChain[static_cast<size_t>(marqueeLo_)].patternIndex);
                    marquee_ = false;
                }
            }
        }

        // ---- playhead --------------------------------------------------------------------------------------------------------
        if (playingPos >= 0 && playingPos < n) {
            const double prog = app.sequencer.ChainStepProgress();
            const double bar = startBar[static_cast<size_t>(playingPos)] + std::clamp(prog, 0.0, 1.0) * barsOf[static_cast<size_t>(playingPos)];
            const float x = X0 + static_cast<float>(bar) * ppb;
            dl->AddLine(ImVec2(x, yTop + 2), ImVec2(x, yTop + contentBottom), vis::kTeal, 2.0f);
        }

        // keep the scroll region as wide as the track
        ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + contentBottom));
        ImGui::Dummy(ImVec2(contentW, 1.0f));

        // ---- Cmd+A selects all bricks, Esc ends the range, Cmd+D duplicates the range -----------------------------------------------
        if (winHovered && !io.WantTextInput && n > 0) {
            if (t8::desktop::interaction::IsCmd() && ImGui::IsKeyPressed(ImGuiKey_A, false)) { app.SelectAllBricks(); anchor_ = 0; SwitchPattern(app, proj.songChain[0].patternIndex); }
            else if (t8::desktop::interaction::IsCmd() && ImGui::IsKeyPressed(ImGuiKey_D, false) && sel_ >= 0) { int lo = 0, hi = 0; app.LoopRange(&lo, &hi); app.DuplicateBrickRange(lo, hi); }
            else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && app.HasBrickRange()) app.ClearBrickRange();
        }
        // ---- Delete removes the selected brick (or all bricks of the selected range) -----------------------------------------------------------------------------------
        if (app.HasBrickRange() && winHovered && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete)) { int lo = 0, hi = 0; app.LoopRange(&lo, &hi); app.RemoveBrickRange(lo, hi); }
        else if (sel_ >= 0 && sel_ < n && winHovered && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete)) { app.RemoveChainStep(sel_); app.selBrick = std::min(sel_, proj.songChainLength - 1); }

        // ---- menus --------------------------------------------------------------------------------------------------------------------
        if (menuOpen_) { ImGui::OpenPopup("BrickMenu"); menuOpen_ = false; }
        if (ImGui::BeginPopup("BrickMenu")) {
            const int i = menuBrick_;
            if (i >= 0 && i < proj.songChainLength) {
                const int pi = std::clamp(proj.songChain[static_cast<size_t>(i)].patternIndex, 0, kNumPatterns - 1);
                ImGui::TextDisabled("BRICK %d", i + 1);
                ImGui::Separator();
                if (ImGui::MenuItem("Play from here  (double-click)")) app.PlayFromBrick(i);
                if (ImGui::MenuItem("Copy brick  (its own notes)")) { const int nb = app.DuplicateBrickAsScene(i); if (nb >= 0) selectBrick(nb); }
                if (ImGui::BeginMenu("Role of this scene")) {
                    for (int r = 0; r <= static_cast<int>(SceneRole::Outro); ++r) {
                        const SceneRole role = static_cast<SceneRole>(r);
                        if (ImGui::MenuItem(r == 0 ? "- none" : RoleName(role), nullptr, proj.patterns[static_cast<size_t>(pi)].role == role)) { app.PushUndoForScene(pi); proj.patterns[static_cast<size_t>(pi)].role = role; }
                    }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Duplicate")) { if (app.DuplicateChainStep(i)) selectBrick(i + 1); else app.SetStatus("TRACK FULL"); }
                if (ImGui::MenuItem("Move left", nullptr, false, i > 0)) { app.MoveChainStep(i, i - 1); selectBrick(i - 1); }
                if (ImGui::MenuItem("Move right", nullptr, false, i < proj.songChainLength - 1)) { app.MoveChainStep(i, i + 1); selectBrick(i + 1); }
                if (ImGui::MenuItem("Remove  (Delete)")) { app.RemoveChainStep(i); app.selBrick = std::min(i, proj.songChainLength - 1); }
                if (ImGui::MenuItem("Reset brick  (mutes, fill, transition, trim, dynamics, levels)")) app.ResetBrickSettings(i);
                ImGui::Separator();
                if (ImGui::MenuItem("Replace ALL bricks from the roles")) {
                    app.PushSoundUndo();
                    const AutoArrangeResult r = AutoArrangeChain(proj, true);
                    app.sequencer.RefreshArrangement();
                    app.SetStatus("ROLES: " + std::to_string(r.lanes) + " DYNAMICS, " + std::to_string(r.mutes) + " MUTES, " + std::to_string(r.transitions) + " TRANSITIONS");
                }
            }
            ImGui::EndPopup();
        }

        if (fxMenuOpen_) { ImGui::OpenPopup("FxMenu"); fxMenuOpen_ = false; }
        if (ImGui::BeginPopup("FxMenu")) {
            const int i = fxMenuBrick_;
            if (i >= 0 && i < proj.songChainLength) {
                ChainStep& cs = proj.songChain[static_cast<size_t>(i)];
                const Pattern& pat = proj.patterns[static_cast<size_t>(std::clamp(cs.patternIndex, 0, kNumPatterns - 1))];
                if (fxMenuKind_ == 0) {
                    ImGui::TextDisabled("FILL  -  BRICK %d", i + 1);
                    ImGui::Separator();
                    FillRule cur = cs.fillMode == FillMode::Off ? FillRule{} : (cs.fillMode == FillMode::Own ? cs.fill : pat.fill);
                    if (ImGui::MenuItem("Use the scene's fill", nullptr, cs.fillMode == FillMode::Scene)) { app.PushSoundUndo(); cs.fillMode = FillMode::Scene; app.sequencer.RefreshArrangement(); }
                    if (ImGui::MenuItem("No fill on this brick", nullptr, cs.fillMode == FillMode::Off)) { app.PushSoundUndo(); cs.fillMode = FillMode::Off; app.sequencer.RefreshArrangement(); }
                    ImGui::Separator();
                    ImGui::TextDisabled("Own fill for this brick:");
                    bool changed = false; FillRule nr = cur;
                    if (nr.when == FillWhen::Off) nr = FillRule{FillWhen::LastLoop, FillKind::Break, 2, 4, 0};
                    if (ImGui::BeginMenu("Break (the BREAK kind)")) {   // v44.25: pick one break instead of a random one each time
                        const auto& brk = AllDrumBreaks();
                        if (ImGui::MenuItem("RANDOM (a different break each time)", nullptr, cs.fillMode == FillMode::Own && cs.fill.kind == FillKind::Break && cs.fill.breakIndex == 0)) { nr.kind = FillKind::Break; nr.breakIndex = 0; changed = true; }
                        for (size_t b = 0; b < brk.size(); ++b) if (ImGui::MenuItem(brk[b].name, nullptr, cs.fillMode == FillMode::Own && cs.fill.kind == FillKind::Break && cs.fill.breakIndex == b + 1)) { nr.kind = FillKind::Break; nr.breakIndex = static_cast<uint8_t>(b + 1); changed = true; }
                        ImGui::EndMenu();
                    }
                    if (ImGui::BeginMenu("Kind")) { for (int k = 0; k < kNumFillKinds; ++k) if (ImGui::MenuItem(FillKindName(static_cast<FillKind>(k)), nullptr, cs.fillMode == FillMode::Own && cs.fill.kind == static_cast<FillKind>(k))) { nr.kind = static_cast<FillKind>(k); changed = true; } ImGui::EndMenu(); }
                    if (ImGui::BeginMenu("When")) {
                        if (ImGui::MenuItem("LAST LOOP (the last pass of the brick)", nullptr, cs.fillMode == FillMode::Own && cs.fill.when == FillWhen::LastLoop)) { nr.when = FillWhen::LastLoop; changed = true; }
                        if (ImGui::MenuItem("EVERY LOOP (end of every pass)", nullptr, cs.fillMode == FillMode::Own && cs.fill.when == FillWhen::EveryLoop)) { nr.when = FillWhen::EveryLoop; changed = true; }
                        for (int b : kFillBarChoices) { char lb[40]; std::snprintf(lb, sizeof lb, "EVERY %d BARS", b); if (ImGui::MenuItem(lb, nullptr, cs.fillMode == FillMode::Own && cs.fill.when == FillWhen::EveryBars && cs.fill.everyBars == b)) { nr.when = FillWhen::EveryBars; nr.everyBars = static_cast<uint8_t>(b); changed = true; } }
                        ImGui::EndMenu();
                    }
                    if (ImGui::BeginMenu("Length")) { for (int b : kFillBeatChoices) { char lb[24]; std::snprintf(lb, sizeof lb, "%d beat%s", b, b == 1 ? "" : "s"); if (ImGui::MenuItem(lb, nullptr, cs.fillMode == FillMode::Own && cs.fill.beats == b)) { nr.beats = static_cast<uint8_t>(b); changed = true; } } ImGui::EndMenu(); }
                    if (changed) { app.PushSoundUndo(); cs.fillMode = FillMode::Own; cs.fill = NormalizeFill(nr); app.sequencer.RefreshArrangement(); }
                    ImGui::Separator();
                    int fl = cs.levels.fillDb;
                    ImGui::TextUnformatted("Fill level"); ImGui::SameLine(0, 6);
                    if (DbField(app, "##fl", &fl, 90)) app.SetBrickFillLevel(i, fl);
                } else {
                    const RiserPlayer& rp = app.engine.Risers();
                    ImGui::TextDisabled("TRANSITION INTO BRICK %d", i + 1);
                    ImGui::Separator();
                    if (ImGui::MenuItem("None", nullptr, cs.riser < 0)) { app.PushSoundUndo(); cs.riser = -1; app.sequencer.RefreshArrangement(); }
                    if (ImGui::BeginMenu("Sound")) {
                        for (int r = 0; r < rp.Count(); ++r) { if (rp.NameAt(r).empty()) continue; if (ImGui::MenuItem(rp.NameAt(r).c_str(), nullptr, cs.riser == r)) { app.PushSoundUndo(); cs.riser = static_cast<int8_t>(r); app.sequencer.RefreshArrangement(); } }
                        ImGui::Separator();
                        if (ImGui::MenuItem("My samples...")) app.OpenLibrary(AppState::LibTarget::Transition, i);   // v44.51: one of your own files from the sample library
                        ImGui::EndMenu();
                    }
                    {
                        int tl = cs.levels.transitionDb;
                        ImGui::TextUnformatted("Level"); ImGui::SameLine(0, 6);
                        if (DbField(app, "##tl", &tl, 90)) app.SetBrickTransitionLevel(i, tl);
                    }
                    if (ImGui::BeginMenu("Starts")) { for (int b : {1, 2, 4, 8}) { char lb[24]; std::snprintf(lb, sizeof lb, "%d bar%s before", b, b == 1 ? "" : "s"); if (ImGui::MenuItem(lb, nullptr, cs.riserBars == b)) { app.PushSoundUndo(); cs.riserBars = static_cast<uint8_t>(b); app.sequencer.RefreshArrangement(); } } ImGui::EndMenu(); }
                }
            }
            ImGui::EndPopup();
        }
        if (lvlMenuOpen_) { ImGui::OpenPopup("LvlMenu"); lvlMenuOpen_ = false; }
        if (ImGui::BeginPopup("LvlMenu")) {
            const int i = lvlMenuBrick_, t = lvlMenuTrack_;
            if (i >= 0 && i < proj.songChainLength && t >= 0 && t < kNumTracks) {
                ChainStep& cs = proj.songChain[static_cast<size_t>(i)];
                ImGui::TextDisabled("%s  -  BRICK %d", TrackNameShort(t), i + 1);
                ImGui::TextDisabled("Level of this instrument from the start of the brick to its end.");
                ImGui::Separator();
                int s0 = cs.levels.track[t].Start(), e0 = cs.levels.track[t].End();
                bool ch = false;
                ImGui::TextUnformatted("START"); ImGui::SameLine(0, 6);
                if (DbField(app, "##ls", &s0, 100)) ch = true;
                ImGui::SameLine(0, 10); ImGui::TextUnformatted("END"); ImGui::SameLine(0, 6);
                if (DbField(app, "##le", &e0, 100)) ch = true;
                if (ch) app.SetBrickTrackLevels(i, t, s0, e0);
                ImGui::Separator();
                struct Shape { const char* name; int s, e; };
                static const Shape shapes[] = {{"FLAT (untouched)", 0, 0}, {"FADE IN", kAutoMinDb, 0}, {"FADE OUT", 0, kAutoMinDb}, {"SWELL", -18, 0}, {"DIP", 0, -18}, {"QUIETER  (-6 dB)", -6, -6}, {"MUCH QUIETER  (-12 dB)", -12, -12}};
                for (const Shape& sh : shapes) if (ImGui::MenuItem(sh.name)) { app.PushSoundUndo(); app.SetBrickTrackLevels(i, t, sh.s, sh.e); }
                ImGui::Separator();
                if (ImGui::MenuItem("Same shape on ALL bricks of this scene")) {
                    app.PushSoundUndo();
                    for (int k = 0; k < proj.songChainLength; ++k) if (proj.songChain[static_cast<size_t>(k)].patternIndex == cs.patternIndex) app.SetBrickTrackLevels(k, t, s0, e0);
                }
            }
            ImGui::EndPopup();
        }
        if (dynMenuOpen_) { ImGui::OpenPopup("DynMenu"); dynMenuOpen_ = false; }
        if (ImGui::BeginPopup("DynMenu")) {
            const int i = dynMenuBrick_;
            if (i >= 0 && i < proj.songChainLength) {
                ChainStep& cs = proj.songChain[static_cast<size_t>(i)];
                ImGui::TextDisabled("DYNAMICS  -  BRICK %d", i + 1);
                ImGui::TextDisabled("Level and filters move from the start of the brick to its end.");
                ImGui::Separator();
                size_t np = 0; const DynPreset* dp = DynPresets(&np);
                for (size_t k = 0; k < np; ++k) {
                    const bool cur = dp[k].a == cs.automation;
                    if (ImGui::MenuItem(dp[k].label, nullptr, cur)) { app.PushSoundUndo(); cs.automation = dp[k].a; }
                    if (ImGui::IsItemHovered()) Hint("%s", dp[k].tip);
                }
            }
            ImGui::EndPopup();
        }
        if (addOpen_) { ImGui::OpenPopup("AddScene"); addOpen_ = false; }
        if (ImGui::BeginPopup("AddScene")) {
            ImGui::TextDisabled("ADD A COPY OF A SCENE TO THE END");
            const int liveShown = AppState::IsBrickSlot(proj.currentPattern) ? proj.liveScene : proj.currentPattern;
            char cur[48]; std::snprintf(cur, sizeof cur, "THE LIVE SCENE YOU WERE ON (%02d)", liveShown + 1);
            if (ImGui::Button(cur, ImVec2(-1, 0))) { if (app.AddChainStep(liveShown)) selectBrick(proj.songChainLength - 1); ImGui::CloseCurrentPopup(); }
            ImGui::Separator();
            for (int p = 0; p < kNumLiveScenes; ++p) {
                const bool has = SceneHasNotes(proj.patterns[static_cast<size_t>(p)]);
                char lb[16]; std::snprintf(lb, sizeof lb, "%02d##s%d", p + 1, p);
                ImGui::PushStyleColor(ImGuiCol_Text, has ? ImVec4(0.93f, 0.93f, 0.95f, 1.0f) : ImVec4(0.5f, 0.52f, 0.6f, 1.0f));
                if (ImGui::Button(lb, ImVec2(46, 28))) { if (app.AddChainStep(p)) selectBrick(proj.songChainLength - 1); ImGui::CloseCurrentPopup(); }
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) Hint(has ? "Scene %d" : "Scene %d (empty)", p + 1);
                if (p % 4 != 3) ImGui::SameLine(0, 4);
            }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();

    ImGui::PopID();
}

// ---- INSPECTOR (v44.11; redrawn in v44.34 with pictograms) ---------------------------------------------------------------------------
// Left to right: the brick and its role | PLAY (repeats, trim) | WHO PLAYS (ten bars, one per instrument) | three coloured pages
// LEVEL / FILL / TRANSITION, one open at a time | the brick's actions. Pictures instead of words; the words are in the info line.
float BricksPanel::InspectorHeight() { return 104.0f; }

void BricksPanel::DrawInspector(AppState& app, ChainPanel& chain) {
    (void)chain;
    SceneLock lock(app.sequencer.PatternMutex(), "BRICK INSPECTOR");
    Project& proj = app.project;
    const ImVec4 amber(0.95f, 0.64f, 0.24f, 1.0f);
    ImGui::PushID("Inspector");
    const int n = std::clamp(proj.songChainLength, 0, kMaxChainSteps);
    const int i = app.BrickForTransport();
    if (n == 0 || i < 0 || i >= n) {
        ImGui::TextColored(amber, "BRICK");
        ImGui::SameLine(0, 10);
        ImGui::TextDisabled(n == 0 ? "No bricks yet. Make a loop on LIVE and press THROW ONTO THE TRACK, or add a scene with + on the strip - the settings of the brick appear here." : "Click a brick above to see and change what it does.");
        ImGui::PopID();
        return;
    }
    auto selectBrick = [&](int idx) {
        if (idx < 0 || idx >= proj.songChainLength) return;
        app.selBrick = idx;
        if (proj.currentPattern != proj.songChain[static_cast<size_t>(idx)].patternIndex) SwitchPattern(app, proj.songChain[static_cast<size_t>(idx)].patternIndex);
    };
    ChainStep& cs = proj.songChain[static_cast<size_t>(i)];
    const int pi = std::clamp(cs.patternIndex, 0, kNumPatterns - 1);
    Pattern& pat = proj.patterns[static_cast<size_t>(pi)];
    const int bar = 4 * ClampGrid(pat.stepsPerBeat), sceneLen = std::max(1, pat.masterLength);
    auto refresh = [&] { app.sequencer.RefreshArrangement(); };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 org = ImGui::GetCursorScreenPos();
    const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
    const float fh = ImGui::GetFrameHeight();
    constexpr float kRow = 18.0f;                       // the row of controls under the small captions
    auto at = [&](float x, float y) { ImGui::SetCursorPos(ImVec2(x0 + x, y0 + y)); };
    auto cap = [&](float x, const char* t, ImU32 c) { dl->AddText(ImGui::GetFont(), 10.0f, ImVec2(org.x + x, org.y + 1.0f), c, t); };
    auto sep = [&](float x) { dl->AddLine(ImVec2(org.x + x, org.y), ImVec2(org.x + x, org.y + 88.0f), vis::kLine); };
    auto capUnder = [&](const char* t) {   // a tiny caption centred under the item just drawn
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax(), ts = ImGui::CalcTextSize(t);
        const float sc = 9.5f / ImGui::GetFontSize();
        dl->AddText(ImGui::GetFont(), 9.5f, ImVec2((a.x + b.x) * 0.5f - ts.x * sc * 0.5f, b.y + 1.0f), vis::kTextDim, t);
    };

    // ---- 1. the brick: title, length, its five actions (pictures), role -------------------------------------------------------------
    constexpr float kActW = 26.0f, kActGap = 3.0f;
    const float xPlay = 5 * kActW + 4 * kActGap + 22.0f;   // the columns start after the left column
    at(0, 0);
    ImGui::TextColored(amber, "BRICK %d", i + 1);
    ImGui::SameLine(0, 8);
    ImGui::TextDisabled("%.4g bar%s  %.1f s", ChainStepBars(pat, cs), ChainStepBars(pat, cs) == 1.0 ? "" : "s", ChainStepSeconds(pat, cs));
    at(0, 22);
    {
        constexpr float tw = kActW, gap = kActGap;
        if (GlyphTile("##copy", Glyph::Copy, nullptr, false, vis::kAccent, true, tw, tw)) { const int nb = app.DuplicateBrickAsScene(i); if (nb >= 0) selectBrick(nb); }
        if (ImGui::IsItemHovered()) Hint("COPY BRICK: a copy of this brick right after it, with its own notes:\na variation you can change without touching the original.");
        ImGui::SameLine(0, gap);
        {   // v44.23: bring the latest version of the LIVE scene into this brick (notes), keeping its sound and settings
            const int live = AppState::IsBrickSlot(proj.currentPattern) ? proj.liveScene : proj.currentPattern;
            const bool liveEmpty = !SceneHasNotes(proj.patterns[static_cast<size_t>(std::clamp(live, 0, kNumLiveScenes - 1))]);
            if (GlyphTile("##fromlive", Glyph::FromLive, nullptr, false, vis::kAccent, !liveEmpty, tw, tw)) app.UpdateBrickFromLive(i);
            if (ImGui::IsItemHovered())
                Hint(liveEmpty ? "UPDATE FROM LIVE: the LIVE scene you were on is empty." : "UPDATE FROM LIVE: replaces this brick's notes with the current version of LIVE scene %d.\nThe brick keeps its sound, role, fill and mutes. Undoable. (A brick that shares its notes with REPEAT SAME changes together.)", live + 1);
        }
        ImGui::SameLine(0, gap);
        if (GlyphTile("##same", Glyph::Twice, nullptr, false, vis::kAccent, true, tw, tw)) { if (app.DuplicateChainStep(i)) selectBrick(i + 1); else app.SetStatus("TRACK FULL"); }
        if (ImGui::IsItemHovered()) Hint("REPEAT SAME: another brick that plays the SAME notes right after this one (they share them: changing one changes both).");
        ImGui::SameLine(0, gap);
        if (GlyphTile("##reset", Glyph::Reset, nullptr, false, vis::kAccent, true, tw, tw)) app.ResetBrickSettings(i);
        if (ImGui::IsItemHovered()) Hint("RESET: everything this brick adds on top of its scene goes back to nothing: mutes, fill, transition, trim, dynamics and levels.\nThe notes and the sound are not touched. Undoable.");
        ImGui::SameLine(0, gap);
        if (GlyphTile("##remove", Glyph::Remove, nullptr, false, vis::kRed, true, tw, tw)) { app.RemoveChainStep(i); app.selBrick = std::min(i, proj.songChainLength - 1); }
        if (ImGui::IsItemHovered()) Hint("REMOVE: take this brick out of the track (the scene stays; Delete does the same).");
    }
    at(0, 56);
    {
        ImGui::SetNextItemWidth(5 * kActW + 4 * kActGap);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(RoleCol(pat.role)));
        const bool open = ChoiceBegin("##role", pat.role == SceneRole::None ? "no role" : RoleName(pat.role));
        ImGui::PopStyleColor();
        if (open) {
            for (int r = 0; r <= static_cast<int>(SceneRole::Outro); ++r) {
                const SceneRole role = static_cast<SceneRole>(r);
                if (ImGui::Selectable(r == 0 ? "none" : RoleName(role), pat.role == role)) { app.PushUndoForScene(pi); pat.role = role; }
            }
            ChoiceEnd();
        }
        if (ImGui::IsItemHovered()) Hint("What this scene is in the track (intro, build, drop, break, outro): the colour of the brick.\nFILL FROM ROLES uses it to suggest dynamics, mutes and transitions.");
    }
    sep(xPlay - 12.0f);

    // ---- 2. PLAY: repeats and trim -------------------------------------------------------------------------------------------------
    cap(xPlay, "PLAY", colors::kPlay);
    DrawGlyph(dl, Glyph::Repeat, ImVec2(org.x + xPlay + 9, org.y + kRow + fh * 0.5f), 16.0f, vis::kAccent);
    at(xPlay + 22, kRow);
    {
        float rep = static_cast<float>(cs.repeats);
        char rb[16]; std::snprintf(rb, sizeof rb, "x%d", cs.repeats);
        ValueSpec sp; sp.lo = 1.0f; sp.hi = 99.0f; sp.step = 0.12f; sp.wheel = 1.0f; sp.def = 1.0f; sp.integer = true;
        sp.text = cs.trimSteps ? "trim" : rb;
        const ValueResult r = ValueField("##rep", &rep, sp, 56);
        if (r.changed) app.SetBrickRepeats(i, static_cast<int>(rep + 0.5f));   // takes its own (coalesced) undo step
        if (ImGui::IsItemHovered()) Hint("REPEATS: how many times the scene plays in this brick (1-99). The same as dragging the right edge of the brick.");
    }
    DrawGlyph(dl, Glyph::Trim, ImVec2(org.x + xPlay + 88 + 9, org.y + kRow + fh * 0.5f), 16.0f, vis::kAccent);
    at(xPlay + 88 + 22, kRow);
    {
        const int spb = ClampGrid(pat.stepsPerBeat);
        const int cur = cs.trimSteps == 0 ? sceneLen : std::min<int>(cs.trimSteps, sceneLen);
        auto trimText = [&](int steps, char* out, size_t n) {
            if (steps % bar == 0) std::snprintf(out, n, "%d BAR%s", steps / bar, steps / bar == 1 ? "" : "S");
            else { const double bt = steps / static_cast<double>(spb); std::snprintf(out, n, "%g BEAT%s", bt, bt == 1.0 ? "" : "S"); }
        };
        char cl[24]; if (cs.trimSteps == 0) std::snprintf(cl, sizeof cl, "FULL"); else trimText(cur, cl, sizeof cl);
        std::vector<int> opts;   // 1-3 beats, then whole bars, only the ones shorter than the scene
        for (int bt : {1, 2, 3}) opts.push_back(bt * spb);
        for (int nb : {1, 2, 4, 8}) opts.push_back(nb * bar);
        opts.erase(std::remove_if(opts.begin(), opts.end(), [&](int st) { return st >= sceneLen || st > 255; }), opts.end());
        opts.erase(std::unique(opts.begin(), opts.end()), opts.end());
        ImGui::BeginDisabled(opts.empty() && cs.trimSteps == 0);
        ImGui::SetNextItemWidth(84);
        if (ChoiceBegin("##trim", cl)) {
            if (ImGui::Selectable("FULL SCENE", cs.trimSteps == 0)) { app.PushSoundUndo(); cs.trimSteps = 0; refresh(); }
            for (int st : opts) {
                char lb[24]; trimText(st, lb, sizeof lb);
                if (ImGui::Selectable(lb, cs.trimSteps == st)) { app.PushSoundUndo(); cs.trimSteps = static_cast<uint8_t>(st); refresh(); }
            }
            ChoiceEnd();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) Hint(opts.empty() && cs.trimSteps == 0 ? "TRIM: this scene is only %d steps long, there is nothing shorter to cut it to." : "TRIM: FULL = the whole scene plays in every pass. Pick a shorter length to play only the first beats or bars of the scene (the scene itself is not changed).", sceneLen);
    }
    const float xWho = xPlay + 202.0f + 12.0f;
    sep(xPlay + 202.0f);

    // ---- 3. WHO PLAYS: one bar per instrument (click / drag = off and on, right-click = the instrument the LEVEL page works on) -----
    cap(xWho, "WHO PLAYS", colors::kPlay);
    const int laneSel = lanePick_ == kLaneMix || lanePick_ == kLaneFilter ? -1 : std::clamp(app.seqCursorTrack, 0, kNumTracks - 1);
    {
        constexpr float bw = 17.0f, bg = 3.0f, bh = 44.0f;   // v44.50: 11 bars in the width the ten had
        const float barsW = kNumTracks * (bw + bg) - bg;
        at(xWho, kRow);
        ImGui::InvisibleButton("##who", ImVec2(barsW, bh));
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 a = ImGui::GetItemRectMin(), mouse = ImGui::GetIO().MousePos;
        const int hot = hovered ? std::clamp(static_cast<int>((mouse.x - a.x) / (bw + bg)), 0, kNumTracks - 1) : -1;
        auto has = [&](int t) { return app.TrackHasNotes(pi, t); };
        if (ImGui::IsItemActivated() && hot >= 0 && has(hot)) {
            paintMuted_ = !(((cs.muteMask >> hot) & 1u) != 0);
            app.PushSoundUndo(); app.SetBrickTrackMuted(i, hot, paintMuted_);
            whoPaint_ = true;
        }
        if (whoPaint_) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) whoPaint_ = false;
            else {
                const int t = std::clamp(static_cast<int>((mouse.x - a.x) / (bw + bg)), 0, kNumTracks - 1);
                if (has(t) && (((cs.muteMask >> t) & 1u) != 0) != paintMuted_) app.SetBrickTrackMuted(i, t, paintMuted_);
            }
        }
        if (hot >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { app.seqCursorTrack = hot; lanePick_ = -1; lanePickAt_ = hot; page_ = 0; }
        for (int t = 0; t < kNumTracks; ++t) {
            const ImVec2 p0(a.x + t * (bw + bg), a.y), p1(p0.x + bw, a.y + bh);
            const ImU32 tc = TrackColor(t);
            const bool muted = (cs.muteMask >> t) & 1u, sceneMuted = pat.tracks[static_cast<size_t>(t)].mute;
            if (!has(t)) {   // nothing to play: an empty slot
                dl->AddRect(p0, p1, IM_COL32(60, 64, 82, 255), 3.0f);
            } else if (muted) {
                dl->AddRectFilled(p0, p1, WithAlpha(tc, 34), 3.0f);
                if (app.SwSilenced(i, t)) instrument_switches::DashedRect(dl, p0, p1, WithAlpha(tc, 110));   // v44.48: off because another instrument is soloed
                else dl->AddRect(p0, p1, WithAlpha(tc, 90), 3.0f);
            } else if (cs.levels.TrackAny(t)) {   // the bar rises / falls like the level of the instrument over the brick
                const float h = p1.y - p0.y, fs = LevelFrac(cs.levels.track[t].Start()), fe = LevelFrac(cs.levels.track[t].End());
                dl->AddRectFilled(p0, p1, WithAlpha(tc, 40), 3.0f);
                const ImVec2 q[4] = {ImVec2(p0.x, p1.y - h * fs), ImVec2(p1.x, p1.y - h * fe), ImVec2(p1.x, p1.y), ImVec2(p0.x, p1.y)};
                dl->AddConvexPolyFilled(q, 4, WithAlpha(tc, sceneMuted ? 110 : 235));
            } else dl->AddRectFilled(p0, p1, WithAlpha(tc, sceneMuted ? 110 : 235), 3.0f);
            {   // the instrument on the bar: dark on a lit bar, in its own colour on a dim one
                const bool lit = has(t) && !muted;
                DrawInstrumentGlyph(dl, t, ImVec2((p0.x + p1.x) * 0.5f, p0.y + 11.0f), 14.0f, lit ? IM_COL32(18, 20, 26, 235) : (has(t) ? WithAlpha(tc, 170) : IM_COL32(80, 84, 100, 255)));
            }
            if (has(t) && ((pat.ownSound >> t) & 1u)) { dl->AddCircleFilled(ImVec2(p1.x - 3.0f, p1.y - 4.0f), 2.4f, IM_COL32(255, 255, 255, 255)); }
            if (t == laneSel) dl->AddRect(ImVec2(p0.x - 2, p0.y - 2), ImVec2(p1.x + 2, p1.y + 2), vis::kText, 4.0f, 0, 1.5f);
            else if (t == hot) dl->AddRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), vis::kLineHi, 3.0f);
        }
        if (hovered && hot >= 0) {
            const bool m = (cs.muteMask >> hot) & 1u;
            if (!has(hot)) Hint("%s: the scene has no notes for it on this brick.", TrackNameShort(hot));
            else Hint("%s on brick %d: %s.  Click or drag: off / on.  Right-click: this is the instrument the LEVEL page changes.%s", TrackNameShort(hot), i + 1, m ? "OFF" : (cs.levels.TrackAny(hot) ? "ON, with a level shape" : "ON"),
                      ((pat.ownSound >> hot) & 1u) ? "\nThe white dot: this brick has its own sound for it." : "");
        }
        {   // v44.48: S under every bar (solo = every other instrument off) and the group buttons on the right, the same widgets as on LIVE
            using namespace instrument_switches;
            for (int t = 0; t < kNumTracks; ++t) {
                ImGui::PushID(t);
                if (SoloCell("##whosolo", ImVec2(a.x + t * (bw + bg), a.y + bh + 3.0f), app.SwSolo(i, t), !has(t))) app.SwToggleSolo(i, t);
                if (ImGui::IsItemHovered()) Hint("%s solo on brick %d: only this instrument plays. Press again to bring the others back. The same switch as S in the sequencer.", TrackNameShort(t), i + 1);
                ImGui::PopID();
            }
            DrawGroups(app, i, ImVec2(a.x + barsW + 8.0f, a.y), 56.0f, 18.0f, 3.0f, 10.0f);
        }
        at(xWho + barsW + 8.0f + 2 * 56.0f + 3.0f + 6.0f, kRow + 7.0f);
        if (GlyphTile("##whopre", Glyph::Presets, nullptr, false, colors::kPlay, true, 28.0f, 30.0f)) ImGui::OpenPopup("##whomenu");
        if (ImGui::IsItemHovered()) Hint("Quick choices for WHO PLAYS: everyone, no kick + bass, no drums, kick + bass only ...");
        if (ImGui::BeginPopup("##whomenu")) {
            static const struct { const char* label; uint16_t mask; } kPre[] = {
                {"EVERYONE", 0}, {"NO KICK + BASS", kMuteKickBass}, {"NO DRUMS", kMuteDrums}, {"NO BASS", kMuteBass},
                {"KICK + BASS ONLY", kMuteAllButKickBass}, {"CHORD ONLY", kMuteAllButChord}};
            for (const auto& pr : kPre) if (ImGui::Selectable(pr.label, pr.mask == cs.muteMask)) { app.PushSoundUndo(); cs.muteMask = pr.mask; refresh(); }
            ImGui::EndPopup();
        }
    }
    const float xTabs = xWho + 10 * 22.0f - 3.0f + 8.0f + 2 * 56.0f + 3.0f + 6.0f + 28.0f + 10.0f + 10.0f;   // v44.48: the group buttons stand between the bars and the preset tile
    sep(xTabs - 10.0f);

    // ---- 4. three coloured pages: LEVEL / FILL / TRANSITION --------------------------------------------------------------------------
    const bool hasLevel = cs.automation.Any() || [&] { for (int k = 0; k < kNumTracks; ++k) if (cs.levels.TrackAny(k)) return true; return false; }();
    const bool hasFill = cs.fillMode == FillMode::Own || (cs.fillMode == FillMode::Scene && pat.fill.when != FillWhen::Off);
    const bool hasTrans = (i > 0 && cs.riser >= 0) || (i + 1 < n && proj.songChain[static_cast<size_t>(i + 1)].riser >= 0);   // a transition on either border of this brick
    page_ = std::clamp(page_, 0, 3);
    const bool hasParam = cs.params.Any();
    {
        struct Tab { const char* name; Glyph g; ImU32 col; bool dot; const char* tip; };
        const Tab tabs[4] = {
            {"LEVEL", Glyph::Level, colors::kLevel, hasLevel, "LEVEL: how loud an instrument (or the whole mix, or the DJ filter) is at the start and at the end of the brick."},
            {"FILL", Glyph::Fill, colors::kFill, hasFill, "FILL: the drum fill at the end of the brick."},
            {"TRANS", Glyph::Trans, colors::kTrans, hasTrans, "TRANSITION: a riser or impact on the border of two bricks: into this brick (IN) or out of it into the next one (OUT)."},
            {"PARAM", Glyph::Param, colors::kParam, hasParam, "PARAM: one knob of one instrument (pan, reverb / delay send, tune, decay, cutoff, resonance) moving over the brick."}};
        for (int k = 0; k < 4; ++k) {
            at(xTabs, 1.0f + k * 23.0f);
            ImGui::PushID(k);
            ImGui::InvisibleButton("##tab", ImVec2(92, 21));
            const bool hov = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) page_ = k;
            if (hov) Hint("%s", tabs[k].tip);
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const bool on = page_ == k;
            dl->AddRectFilled(a, b, on ? tabs[k].col : (hov ? vis::kCellHover : vis::kCell), 5.0f);
            if (!on) dl->AddRect(a, b, hov ? vis::kLineHi : vis::kLine, 5.0f);
            const ImU32 fg = on ? IM_COL32(16, 18, 22, 255) : tabs[k].col;
            DrawGlyph(dl, tabs[k].g, ImVec2(a.x + 14, (a.y + b.y) * 0.5f), 13.0f, fg);
            const ImVec2 ts = ImGui::CalcTextSize(tabs[k].name);
            dl->AddText(ImVec2(a.x + 28, (a.y + b.y) * 0.5f - ts.y * 0.5f), fg, tabs[k].name);
            if (tabs[k].dot) dl->AddCircleFilled(ImVec2(b.x - 9, (a.y + b.y) * 0.5f), 3.0f, on ? IM_COL32(16, 18, 22, 255) : tabs[k].col);
            ImGui::PopID();
        }
    }
    const float px = xTabs + 106.0f;
    at(px, kRow);

    if (page_ == 0) {
        // LEVEL: lane, its picture, the two ends, three shapes, the dynamics presets
        const int t = std::clamp(app.seqCursorTrack, 0, kNumTracks - 1);
        if (lanePickAt_ != t) { lanePick_ = -1; lanePickAt_ = t; }   // another instrument was chosen somewhere else: follow it
        const int lane = lanePick_ == kLaneMix || lanePick_ == kLaneFilter ? lanePick_ : t;
        ImGui::SetNextItemWidth(84);
        if (ChoiceBegin("##ltrack", lane == kLaneMix ? "MIX" : lane == kLaneFilter ? "FILTER" : TrackNameShort(t))) {
            if (ImGui::Selectable(cs.automation.mix.Any() ? "MIX   *" : "MIX", lane == kLaneMix)) lanePick_ = kLaneMix;
            if (ImGui::Selectable(cs.automation.filter.Any() ? "FILTER   *" : "FILTER", lane == kLaneFilter)) lanePick_ = kLaneFilter;
            ImGui::Separator();
            for (int k = 0; k < kNumTracks; ++k) {
                char lb[40]; std::snprintf(lb, sizeof lb, "%s%s", TrackNameShort(k), cs.levels.TrackAny(k) ? "   *" : "");
                if (ImGui::Selectable(lb, lane == k)) { lanePick_ = -1; app.seqCursorTrack = k; lanePickAt_ = k; }
            }
            ChoiceEnd();
        }
        if (ImGui::IsItemHovered()) Hint("Which lane the two fields change: the MIX of the whole brick, its DJ filter, or one instrument. A star = it already has a shape on this brick.\nThe same lanes are drawn under the bricks, where the free points are set.");
        ImGui::SameLine(0, 8);

        int s0 = 0, e0 = 0;
        const bool isFilter = lane == kLaneFilter;
        if (isFilter) { s0 = cs.automation.filter.Start(); e0 = cs.automation.filter.End(); }
        else if (lane == kLaneMix) { s0 = cs.automation.mix.Start(); e0 = cs.automation.mix.End(); }
        else { s0 = cs.levels.track[t].Start(); e0 = cs.levels.track[t].End(); }
        {   // the picture: a line from the start level to the end level
            const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + 90.0f, a.y + fh);
            ImGui::Dummy(ImVec2(90.0f, fh));
            dl->AddRectFilled(a, b, vis::kInset, 4.0f); dl->AddRect(a, b, vis::kLine, 4.0f);
            auto yOf = [&](int v) {
                const float f = isFilter ? (static_cast<float>(v - kFilterMin) / static_cast<float>(kFilterMax - kFilterMin)) : (LevelFrac(v) - 0.08f) / 0.92f;
                return b.y - 4.0f - std::clamp(f, 0.0f, 1.0f) * (b.y - a.y - 8.0f);
            };
            const ImVec2 p0(a.x + 8, yOf(s0)), p1(b.x - 8, yOf(e0));
            dl->AddLine(p0, p1, colors::kLevel, 2.0f); dl->AddCircleFilled(p0, 3.2f, colors::kLevel); dl->AddCircleFilled(p1, 3.2f, colors::kLevel);
        }
        ImGui::SameLine(0, 10);
        bool ch = false;
        if (isFilter) {
            int fs = s0, fe = e0;
            auto fld = [&](const char* id, int* v) {
                float f = static_cast<float>(*v);
                ValueSpec sp; sp.lo = static_cast<float>(kFilterMin); sp.hi = static_cast<float>(kFilterMax); sp.step = 0.5f; sp.wheel = 1.0f; sp.def = 0.0f; sp.integer = true; sp.unit = "";
                const ValueResult r = ValueField(id, &f, sp, 64);
                if (r.started) app.PushSoundUndo();
                if (r.changed) { *v = static_cast<int>(f); ch = true; }
                if (r.cancelled) app.DropLastUndo();
            };
            fld("##fst", &fs); capUnder("START");
            if (ImGui::IsItemHovered()) Hint("DJ filter at the start of the brick: negative closes the sound (low-pass), positive thins it from below (high-pass), 0 = open.");
            ImGui::SameLine(0, 6); fld("##fen", &fe); capUnder("END");
            if (ImGui::IsItemHovered()) Hint("DJ filter at the end of the brick: negative closes the sound (low-pass), positive thins it from below (high-pass), 0 = open.");
            if (ch) { StepAuto a = cs.automation; a.filter.SetStart(fs, kFilterMin, kFilterMax); a.filter.SetEnd(fe, kFilterMin, kFilterMax); cs.automation = NormalizeAuto(a); app.sequencer.RefreshArrangement(); }
        } else {
            const bool mix = lane == kLaneMix;
            int ns = s0, ne = e0;
            if (DbField(app, mix ? "##mst" : "##lst", &ns, 64)) ch = true;
            capUnder("START");
            if (ImGui::IsItemHovered()) Hint("The level at the start of the brick (the lowest value is off).");
            ImGui::SameLine(0, 6);
            if (DbField(app, mix ? "##men" : "##len", &ne, 64)) ch = true;
            capUnder("END");
            if (ImGui::IsItemHovered()) Hint("The level at the end of the brick (the lowest value is off).");
            if (ch) {
                if (mix) { StepAuto a = cs.automation; a.mix.SetStart(ns, kAutoMinDb, kAutoMaxDb); a.mix.SetEnd(ne, kAutoMinDb, kAutoMaxDb); cs.automation = NormalizeAuto(a); app.sequencer.RefreshArrangement(); }
                else app.SetBrickTrackLevels(i, t, ns, ne);
            }
        }
        ImGui::SameLine(0, 10);
        {   // shapes: flat / fade in / fade out
            const bool flat = s0 == e0, fin = !isFilter && s0 <= kAutoMinDb && e0 > kAutoMinDb, fout = !isFilter && e0 <= kAutoMinDb && s0 > kAutoMinDb;
            if (GlyphTile("##shflat", Glyph::Flat, nullptr, flat, colors::kLevel, true, fh + 6, fh)) {
                app.PushSoundUndo();
                if (isFilter) { cs.automation.filter = Lane{}; app.sequencer.RefreshArrangement(); }
                else if (lane == kLaneMix) { cs.automation.mix = Lane{}; app.sequencer.RefreshArrangement(); }
                else app.SetBrickTrackLevels(i, t, 0, 0);
            }
            if (ImGui::IsItemHovered()) Hint("FLAT: no movement over the brick.");
            if (!isFilter) {
                ImGui::SameLine(0, 3);
                if (GlyphTile("##shin", Glyph::FadeIn, nullptr, fin, colors::kLevel, true, fh + 6, fh)) {
                    app.PushSoundUndo();
                    if (lane == kLaneMix) { cs.automation.mix = Lane::Line(kAutoMinDb, 0, kAutoMinDb, kAutoMaxDb); app.sequencer.RefreshArrangement(); }
                    else app.SetBrickTrackLevels(i, t, kAutoMinDb, 0);
                }
                if (ImGui::IsItemHovered()) Hint("FADE IN: from silence to full level over the brick.");
                ImGui::SameLine(0, 3);
                if (GlyphTile("##shout", Glyph::FadeOut, nullptr, fout, colors::kLevel, true, fh + 6, fh)) {
                    app.PushSoundUndo();
                    if (lane == kLaneMix) { cs.automation.mix = Lane::Line(0, kAutoMinDb, kAutoMinDb, kAutoMaxDb); app.sequencer.RefreshArrangement(); }
                    else app.SetBrickTrackLevels(i, t, 0, kAutoMinDb);
                }
                if (ImGui::IsItemHovered()) Hint("FADE OUT: from full level to silence over the brick.");
            }
        }
        ImGui::SameLine(0, 10);
        {   // dynamics presets (level and filter together)
            const char* nm = DynPresetName(cs.automation);
            ImGui::SetNextItemWidth(104);
            if (ChoiceBegin("##dyn", nm ? nm : "DYNAMICS")) {
                size_t np = 0; const DynPreset* dp = DynPresets(&np);
                for (size_t k = 0; k < np; ++k) {
                    if (ImGui::Selectable(dp[k].label, dp[k].a == cs.automation)) { app.PushSoundUndo(); cs.automation = dp[k].a; }
                    if (ImGui::IsItemHovered()) Hint("%s", dp[k].tip);
                }
                ChoiceEnd();
            }
            if (ImGui::IsItemHovered()) Hint("DYNAMICS: ready-made movements of level and filter from the start of the brick to its end.\nThe level dots can also be dragged in the DYNAMICS block under the bricks.");
        }
    } else if (page_ == 1) {
        // FILL: kind, when, length, level, a picture of where it lands
        const auto& brkList = AllDrumBreaks();
        auto kindText = [&](const FillRule& r) { std::string t = FillKindName(r.kind); if (r.kind == FillKind::Break && r.breakIndex > 0) t = brkList[std::min<size_t>(r.breakIndex - 1, brkList.size() - 1)].name; if (r.kind == FillKind::Sample && r.sample[0]) t = "SAMPLE: " + r.Sample(); return t; };
        std::string cur = cs.fillMode == FillMode::Off ? "NO FILL" : (cs.fillMode == FillMode::Own ? "OWN: " + kindText(cs.fill) : "SCENE'S: " + kindText(pat.fill));
        if (cs.fillMode == FillMode::Scene && pat.fill.when == FillWhen::Off) cur = "NO FILL (scene)";
        ImGui::SetNextItemWidth(160);
        if (ChoiceBegin("##fill", cur.c_str())) {
            if (ImGui::Selectable("Use the scene's fill", cs.fillMode == FillMode::Scene)) { app.PushSoundUndo(); cs.fillMode = FillMode::Scene; refresh(); }
            if (ImGui::Selectable("No fill on this brick", cs.fillMode == FillMode::Off)) { app.PushSoundUndo(); cs.fillMode = FillMode::Off; refresh(); }
            ImGui::Separator();
            ImGui::TextDisabled("Own fill:");
            for (int k = 0; k < kNumFillKinds; ++k) {
                const FillKind kd = static_cast<FillKind>(k);
                const int nBrk = kd == FillKind::Break ? static_cast<int>(brkList.size()) : 0;
                for (int b = -1; b < nBrk; ++b) {   // v44.25: BREAK is RANDOM or one chosen break from the library
                    const std::string lbl = kd == FillKind::Break ? (b < 0 ? std::string("BREAK: RANDOM") : std::string("BREAK: ") + brkList[static_cast<size_t>(b)].name) : std::string(FillKindName(kd));
                    const bool on = cs.fillMode == FillMode::Own && cs.fill.kind == kd && (kd != FillKind::Break || cs.fill.breakIndex == b + 1);
                    if (ImGui::Selectable(lbl.c_str(), on)) {
                        app.PushSoundUndo();
                        FillRule nr = cs.fillMode == FillMode::Own ? cs.fill : (pat.fill.when != FillWhen::Off ? pat.fill : FillRule{FillWhen::LastLoop, FillKind::Break, 2, 4, 0});
                        nr.kind = kd; if (kd == FillKind::Break) nr.breakIndex = static_cast<uint8_t>(b + 1);
                        cs.fillMode = FillMode::Own; cs.fill = NormalizeFill(nr); refresh();
                    }
                }
            }
            ChoiceEnd();
        }
        if (ImGui::IsItemHovered()) Hint("The drum fill at the end of the brick. The fills lane under the bricks shows where it plays\nand its left edge can be dragged.");
        if (cs.fillMode == FillMode::Own) {
            bool ch = false; FillRule nr = cs.fill;
            ImGui::SameLine(0, 4);
            char wl[24]; if (nr.when == FillWhen::LastLoop) std::snprintf(wl, sizeof wl, "LAST LOOP"); else if (nr.when == FillWhen::EveryLoop) std::snprintf(wl, sizeof wl, "EVERY LOOP"); else std::snprintf(wl, sizeof wl, "EVERY %d BARS", nr.everyBars);
            ImGui::SetNextItemWidth(108);
            if (ChoiceBegin("##fwhen", wl)) {
                if (ImGui::Selectable("LAST LOOP", nr.when == FillWhen::LastLoop)) { nr.when = FillWhen::LastLoop; ch = true; }
                if (ImGui::Selectable("EVERY LOOP", nr.when == FillWhen::EveryLoop)) { nr.when = FillWhen::EveryLoop; ch = true; }
                for (int b : kFillBarChoices) { char lb[24]; std::snprintf(lb, sizeof lb, "EVERY %d BARS", b); if (ImGui::Selectable(lb, nr.when == FillWhen::EveryBars && nr.everyBars == b)) { nr.when = FillWhen::EveryBars; nr.everyBars = static_cast<uint8_t>(b); ch = true; } }
                ChoiceEnd();
            }
            if (ImGui::IsItemHovered()) Hint("WHEN: on the last loop of the brick, on every loop, or every few bars.");
            ImGui::SameLine(0, 4);
            char ll[16]; std::snprintf(ll, sizeof ll, "%d BEAT%s", nr.beats, nr.beats == 1 ? "" : "S");
            ImGui::SetNextItemWidth(80);
            if (ChoiceBegin("##flen", ll)) {
                for (int b : kFillBeatChoices) { char lb[16]; std::snprintf(lb, sizeof lb, "%d BEAT%s", b, b == 1 ? "" : "S"); if (ImGui::Selectable(lb, nr.beats == b)) { nr.beats = static_cast<uint8_t>(b); ch = true; } }
                ChoiceEnd();
            }
            if (ImGui::IsItemHovered()) Hint("LENGTH: how many beats at the end the fill takes.");
            if (ch) { app.PushSoundUndo(); cs.fill = NormalizeFill(nr); refresh(); }
        }
        ImGui::SameLine(0, 10);
        int fl = cs.levels.fillDb;
        if (DbField(app, "##lfill", &fl, 64)) app.SetBrickFillLevel(i, fl);
        capUnder("LEVEL");
        if (ImGui::IsItemHovered()) Hint("How loud the drum hits of this brick's fill are.");
        ImGui::SameLine(0, 10);
        {   // where it lands: the brick as a bar, the fill as its tail
            const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + 96.0f, a.y + fh);
            ImGui::Dummy(ImVec2(96.0f, fh));
            dl->AddRectFilled(a, b, vis::kInset, 4.0f); dl->AddRect(a, b, vis::kLine, 4.0f);
            const FillRule& fr = cs.fillMode == FillMode::Own ? cs.fill : pat.fill;
            if (hasFill) {
                const float lenBeats = std::max(1.0f, static_cast<float>(ChainStepBars(pat, cs)) * 4.0f);
                const float w = std::clamp(static_cast<float>(fr.beats) / lenBeats, 0.06f, 1.0f) * (b.x - a.x - 4.0f);
                dl->AddRectFilled(ImVec2(b.x - 2.0f - w, a.y + 3.0f), ImVec2(b.x - 2.0f, b.y - 3.0f), WithAlpha(colors::kFill, 120), 2.0f);
                dl->AddLine(ImVec2(b.x - 2.0f - w, a.y + 3.0f), ImVec2(b.x - 2.0f - w, b.y - 3.0f), colors::kFill, 2.0f);
            }
        }
        // C4b: a SAMPLE fill gets a second row: which sample, how it fits, whether the drums go on
        if (cs.fillMode == FillMode::Own && cs.fill.kind == FillKind::Sample) {
            at(px, kRow + fh + 16.0f);   // under the LEVEL caption
            FillRule nr = cs.fill;
            if (FillSampleControls(app, nr, i, 280.0f)) { app.PushSoundUndo(); cs.fill = NormalizeFill(nr); refresh(); }
        } else if (cs.fillMode == FillMode::Scene && pat.fill.when != FillWhen::Off && pat.fill.kind == FillKind::Sample) {
            at(px, kRow + fh + 16.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("SAMPLE: %s  (the scene's fill: change it in the scene, or choose an own fill here)", pat.fill.sample[0] ? pat.fill.Sample().c_str() : "none chosen");
        }
    } else if (page_ == 2) {
        // TRANSITION: a riser / impact on the border of two bricks. IN = the one that leads into this brick (from the brick before it),
        // OUT = the one that leads out of it into the next brick (it is the IN of the next brick, shown here because that is where you look for it).
        const RiserPlayer& rp = app.engine.Risers();
        const bool canIn = i > 0, canOut = i + 1 < n;
        if (!canIn && !canOut) {   // a track of one brick has no border
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("A transition sits on the border of two bricks. Add another brick to the track.");
            if (ImGui::IsItemHovered()) Hint("TRANSITION: a riser or impact between two bricks. This track has only one brick.");
        } else {
            if (!canIn) transOut_ = true; else if (!canOut) transOut_ = false;
            const int target = transOut_ ? i + 1 : i;
            ChainStep& tc = proj.songChain[static_cast<size_t>(target)];
            const Pattern& tpat = proj.patterns[static_cast<size_t>(std::clamp(tc.patternIndex, 0, kNumPatterns - 1))];
            int io = transOut_ ? 1 : 0;
            static const char* const kIO[2] = {"IN", "OUT"};
            ImGui::BeginDisabled(!(canIn && canOut));
            if (Segmented("##tio", &io, kIO, 2, 34.0f)) transOut_ = io == 1;
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                Hint(canIn && canOut ? "IN: the transition that leads into this brick, from the one before it.  OUT: the one that leads out of it into the next brick."
                                     : (canOut ? "The first brick has nothing before it, so only the transition out of it (into brick 2) is possible."
                                               : "The last brick has nothing after it, so only the transition into it is possible."));
            ImGui::SameLine(0, 8);
            // C4b: the same row as every other sample: [<] [name v] [>] [HEAR]
            std::string cur = tc.riser < 0 ? std::string("NONE") : (rp.NameAt(tc.riser).empty() ? std::string("?") : rp.NameAt(tc.riser));
            std::vector<int> valid;   // the transitions that exist (a missing stock file or an empty user slot is skipped)
            for (int r = 0; r < rp.Count(); ++r) if (!rp.NameAt(r).empty()) valid.push_back(r);
            char rowHint[256];
            std::snprintf(rowHint, sizeof rowHint, "A riser or impact on the border between brick %d and brick %d; its peak lands on the first beat of brick %d. Click = the SAMPLES browser, right-click = No transition.", target, target + 1, target + 1);
            SampleRowSpec ts;
            ts.id = "##transRow"; ts.width = 262.0f; ts.label = cur; ts.dot = colors::kTrans;
            ts.canStep = valid.size() >= 2 || (valid.size() == 1 && tc.riser != valid[0]);
            ts.canHear = tc.riser >= 0 && !rp.NameAt(tc.riser).empty();
            ts.nameHint = rowHint;
            ts.stepHint = "< > = the neighbour transition, at once and silently";
            ts.hearHint = "Play this transition once, at its LEVEL";
            ts.noHearHint = "No transition is chosen here.";
            const SampleRowHit th = SampleRow(ts);
            if (th.step != 0 && !valid.empty()) {
                const int nv = static_cast<int>(valid.size());
                int at = -1;
                for (int k = 0; k < nv; ++k) if (valid[static_cast<size_t>(k)] == tc.riser) at = k;
                const int base = at >= 0 ? at : (th.step > 0 ? -1 : 0);   // NONE: > takes the first, < the last
                app.PushSoundUndo(); tc.riser = static_cast<int8_t>(valid[static_cast<size_t>(((base + th.step) % nv + nv) % nv)]); refresh();
                app.SetStatus("TRANSITION BEFORE BRICK " + std::to_string(target + 1) + ": " + rp.NameAt(tc.riser));
            }
            if (th.hear && ts.canHear) app.engine.Risers().TriggerAligned(tc.riser, 0.0, 0.0, 1.0, LevelDbToGain(tc.levels.transitionDb));   // under the scene lock, like the sequencer
            if (th.open) app.OpenLibrary(AppState::LibTarget::Transition, target);   // C4b: the SAMPLES browser (this project, built-in, your library)
            if (th.menu) ImGui::OpenPopup("##transMenu");   // right-click on the name = menu
            if (ImGui::BeginPopup("##transMenu")) {
                if (ImGui::Selectable("No transition", false, tc.riser < 0 ? ImGuiSelectableFlags_Disabled : 0)) { app.PushSoundUndo(); tc.riser = -1; refresh(); app.SetStatus("NO TRANSITION BEFORE BRICK " + std::to_string(target + 1)); }
                ImGui::EndPopup();
            }
            if (tc.riser >= 0) {
                ImGui::SameLine(0, 6);
                static const char* const kTBars[4] = {"1", "2", "4", "8"};
                int bi = tc.riserBars == 2 ? 1 : tc.riserBars == 4 ? 2 : tc.riserBars == 8 ? 3 : 0;
                if (Segmented("##tbars", &bi, kTBars, 4, 26.0f)) { app.PushSoundUndo(); tc.riserBars = static_cast<uint8_t>(1 << bi); refresh(); }
                if (ImGui::IsItemHovered()) Hint("How many bars before the border the transition starts building.");
                capUnder("BARS BEFORE");
            }
            ImGui::SameLine(0, 10);
            int tl = tc.levels.transitionDb;
            if (DbField(app, "##ltrans", &tl, 64)) app.SetBrickTransitionLevel(target, tl);
            capUnder("LEVEL");
            if (ImGui::IsItemHovered()) Hint("How loud this transition is.");
            ImGui::SameLine(0, 10);
            if (ImGui::GetContentRegionAvail().x >= 96.0f) {   // the lead-in: a ramp that ends where the next brick begins (C4b: only when it fits; the lane under the bricks shows it too)
                const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + 96.0f, a.y + fh);
                ImGui::Dummy(ImVec2(96.0f, fh));
                dl->AddRectFilled(a, b, vis::kInset, 4.0f); dl->AddRect(a, b, vis::kLine, 4.0f);
                if (tc.riser >= 0) {
                    const float rb = static_cast<float>(std::max<int>(1, tc.riserBars)), bb = std::max(1.0f, static_cast<float>(ChainStepBars(tpat, tc)));
                    const float xs = a.x + 2.0f + (b.x - a.x - 4.0f) * (rb / (rb + bb));   // where the brick starts
                    dl->AddRectFilled(ImVec2(xs, a.y + 3.0f), ImVec2(b.x - 2.0f, b.y - 3.0f), WithAlpha(vis::kTextDim, 40), 2.0f);
                    const ImVec2 tri[3] = {ImVec2(a.x + 2.0f, b.y - 3.0f), ImVec2(xs, b.y - 3.0f), ImVec2(xs, a.y + 3.0f)};
                    dl->AddConvexPolyFilled(tri, 3, WithAlpha(colors::kTrans, 150));
                }
            }
        }
    } else {
        // PARAM (v44.38): one knob of one instrument over the brick. The lane is an OFFSET from the value the sound has (0 = untouched).
        if (!AutoParamValid(parTrack_, parParam_)) { parTrack_ = 6; parParam_ = 5; }
        const int rowK = cs.params.Find(parTrack_, parParam_);
        char pl[48]; std::snprintf(pl, sizeof pl, "%s  %s%s", TrackNameShort(parTrack_), AutoParamName(parParam_), rowK >= 0 && cs.params.row[rowK].lane.Any() ? "  *" : "");
        ImGui::SetNextItemWidth(186);
        if (ChoiceBegin("##ppick", pl)) {
            if (ImGui::IsWindowAppearing()) { parSearch_[0] = 0; ImGui::SetKeyboardFocusHere(); }
            ImGui::SetNextItemWidth(176);
            ImGui::InputTextWithHint("##psearch", "search: bass cut, hat pan ...", parSearch_, sizeof parSearch_);
            auto lower = [](std::string x) { for (char& c : x) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return x; };
            const std::string q = lower(parSearch_);
            ImGui::BeginChild("##plist", ImVec2(176, 190));
            for (int pass = 0; pass < 2; ++pass) {   // the rows this brick already has first, then everything the instruments offer
                if (pass == 0 && cs.params.n == 0) continue;
                if (pass == 0) ImGui::TextDisabled("ON THIS BRICK");
                else { ImGui::Separator(); ImGui::TextDisabled("ALL"); }
                for (int t = 0; t < kNumTracks; ++t) for (int pr = 0; pr < kNumAutoParams; ++pr) {
                    if (!AutoParamValid(t, pr)) continue;
                    const bool has = cs.params.Find(t, pr) >= 0;
                    if (pass == 0 && !has) continue;
                    char lb[48]; std::snprintf(lb, sizeof lb, "%s  %s", TrackNameShort(t), AutoParamName(pr));
                    {   // every word of the search must be in "instrument parameter" (so "bass cut" and "cut bass" both find BASS CUTOFF)
                        const std::string hay = lower(lb);
                        std::istringstream words(q); std::string w; bool okAll = true;
                        while (words >> w) if (hay.find(w) == std::string::npos) { okAll = false; break; }
                        if (!okAll) continue;
                    }
                    ImGui::PushID(pass * 1000 + t * 10 + pr);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(TrackColor(t)));
                    if (ImGui::Selectable(lb, parTrack_ == t && parParam_ == pr)) { parTrack_ = t; parParam_ = pr; app.seqCursorTrack = t; ImGui::CloseCurrentPopup(); }
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            ChoiceEnd();
        }
        if (ImGui::IsItemHovered()) Hint("Which knob moves over this brick: type to search (instrument and parameter). A star = it already moves on this brick. The same row is drawn under the bricks.");
        ImGui::SameLine(0, 8);
        const Lane* cl = rowK >= 0 ? &cs.params.row[rowK].lane : nullptr;
        int s0 = cl ? cl->Start() : 0, e0 = cl ? cl->End() : 0;
        {   // the picture: a line from the start value to the end value around the middle (0 = the sound as it is)
            const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + 90.0f, a.y + fh);
            ImGui::Dummy(ImVec2(90.0f, fh));
            dl->AddRectFilled(a, b, vis::kInset, 4.0f); dl->AddRect(a, b, vis::kLine, 4.0f);
            dl->AddLine(ImVec2(a.x + 4, (a.y + b.y) * 0.5f), ImVec2(b.x - 4, (a.y + b.y) * 0.5f), IM_COL32(255, 255, 255, 30));
            auto yOf = [&](int v) { return (a.y + b.y) * 0.5f - std::clamp(v / 100.0f, -1.0f, 1.0f) * (b.y - a.y - 8.0f) * 0.5f; };
            const ImVec2 p0(a.x + 8, yOf(s0)), p1(b.x - 8, yOf(e0));
            dl->AddLine(p0, p1, colors::kParam, 2.0f); dl->AddCircleFilled(p0, 3.2f, colors::kParam); dl->AddCircleFilled(p1, 3.2f, colors::kParam);
        }
        ImGui::SameLine(0, 10);
        bool pch = false; int ns = s0, ne = e0;
        auto pfld = [&](const char* id, int* v) {
            float f = static_cast<float>(*v);
            ValueSpec sp; sp.lo = -100.0f; sp.hi = 100.0f; sp.step = 0.5f; sp.wheel = 1.0f; sp.def = 0.0f; sp.integer = true; sp.unit = "";
            const ValueResult r = ValueField(id, &f, sp, 64);
            if (r.started) app.PushSoundUndo();
            if (r.changed) { *v = static_cast<int>(f); pch = true; }
            if (r.cancelled) app.DropLastUndo();
        };
        pfld("##pst", &ns); capUnder(ParamValueText(parParam_, ns).c_str());
        if (ImGui::IsItemHovered()) Hint("%s of %s at the START of the brick, as a change from the sound's own value (0 = untouched). %s.", AutoParamName(parParam_), TrackNameShort(parTrack_), ParamHundred(parParam_));
        ImGui::SameLine(0, 6); pfld("##pen", &ne); capUnder(ParamValueText(parParam_, ne).c_str());
        if (ImGui::IsItemHovered()) Hint("%s of %s at the END of the brick, as a change from the sound's own value (0 = untouched). %s.", AutoParamName(parParam_), TrackNameShort(parTrack_), ParamHundred(parParam_));
        if (pch) {
            const int k = cs.params.Add(parTrack_, parParam_);
            if (k < 0) app.SetStatus("A BRICK HAS ROOM FOR " + std::to_string(kMaxParamRows) + " PARAMETER ROWS - REMOVE ONE FIRST");
            else { Lane& ln = cs.params.row[k].lane; ln.SetStart(ns, -100, 100); ln.SetEnd(ne, -100, 100); refresh(); }
        }
        ImGui::SameLine(0, 10);
        {   // shapes: flat / up / down, and removing the row
            auto setLane = [&](const Lane& shape) {
                app.PushSoundUndo();
                const int k = cs.params.Add(parTrack_, parParam_);
                if (k < 0) { app.DropLastUndo(); app.SetStatus("A BRICK HAS ROOM FOR " + std::to_string(kMaxParamRows) + " PARAMETER ROWS - REMOVE ONE FIRST"); return; }
                cs.params.row[k].lane = shape; cs.params.row[k].lane.Normalize(-100, 100); refresh();
            };
            if (GlyphTile("##pflat", Glyph::Flat, nullptr, s0 == e0, colors::kParam, true, fh + 6, fh)) setLane(Lane{});
            if (ImGui::IsItemHovered()) Hint("FLAT: the knob does not move over the brick (the row stays, ready to be drawn).");
            ImGui::SameLine(0, 3);
            if (GlyphTile("##pup", Glyph::FadeIn, nullptr, s0 == 0 && e0 > 0, colors::kParam, true, fh + 6, fh)) setLane(Lane::Line(0, 100, -100, 100));
            if (ImGui::IsItemHovered()) Hint("UP: from the sound's own value to the most the knob can add over the brick.");
            ImGui::SameLine(0, 3);
            if (GlyphTile("##pdown", Glyph::FadeOut, nullptr, s0 == 0 && e0 < 0, colors::kParam, true, fh + 6, fh)) setLane(Lane::Line(0, -100, -100, 100));
            if (ImGui::IsItemHovered()) Hint("DOWN: from the sound's own value to the most the knob can take away over the brick.");
            ImGui::SameLine(0, 10);
            if (GlyphTile("##prem", Glyph::Remove, nullptr, false, colors::kTrans, rowK >= 0, fh + 6, fh)) app.RemoveBrickParamRow(i, rowK);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) Hint(rowK >= 0 ? "Remove this row from the brick (the sound goes back to its own value)." : "This brick has no row for this knob.");
        }
        ImGui::SameLine(0, 12);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%d of %d rows", cs.params.n, kMaxParamRows);
    }

    ImGui::PopID();
}

} // namespace t8::desktop
