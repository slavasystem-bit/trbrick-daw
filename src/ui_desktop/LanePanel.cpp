#include "BrickGlyphs.h"
#include "LanePanel.h"
#include "LiveWidgets.h"
#include "ValueField.h"
#include "EuclidWidget.h"
#include "core/Grooves.h"
#include "MixerPanel.h"
#include "StepGlyphs.h"
#include "TrackStyle.h"
#include "Visual.h"
#include "imgui.h"
#include "InfoLine.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <cstdio>
#include <mutex>

namespace t8::desktop {

void LanePanel::Draw(AppState& app, const LiveLayout& layout) {
    std::lock_guard<SceneMutex> lock(app.sequencer.PatternMutex());
    const int ti = std::clamp(app.seqCursorTrack, 0, kNumTracks - 1);
    Pattern& pat = app.CurrentPattern();
    Track& t = app.StepTrack(pat, ti);

    ImGui::PushID("LanePanel");
    const ImVec2 origin = ImGui::GetCursorPos();
    const float innerW = ImGui::GetContentRegionAvail().x;
    const float rowsTop = origin.y + layout.headerH;
    const float bodyH = layout.rowH * kNumTracks;

    // ---- header -----------------------------------------------------------
    ImGui::SetCursorPos(ImVec2(origin.x, origin.y + (layout.headerH - ImGui::GetFrameHeight()) * 0.5f));
    if (ImGui::Button("X  MIXER")) app.deskLanesClosed = true;
    if (ImGui::IsItemHovered()) Hint("Close the lanes and bring the mixer back");
    ImGui::SameLine(0, 8);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f), "LANES");
    ImGui::SameLine(0, 6);
    {   // v44.37: the instrument's pictogram before its name
        const ImVec2 p = ImGui::GetCursorScreenPos();
        DrawInstrumentGlyph(ImGui::GetWindowDrawList(), ti, ImVec2(p.x + 8.0f, p.y + ImGui::GetFrameHeight() * 0.5f), 14.0f, TrackColor(ti));
        ImGui::Dummy(ImVec2(18.0f, 1.0f));
        ImGui::SameLine(0, 0);
    }
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TrackColor(ti)), "%.8s", TrackDisplayName(t).c_str());
    ImGui::SameLine(0, 14);
    if (ToggleButton("VELOCITY", !swingTab_ && kind_ == LaneKind::Velocity, ImVec2(0, 0), ToggleTab())) { kind_ = LaneKind::Velocity; swingTab_ = false; }
    if (ImGui::IsItemHovered()) Hint("How hard each hit plays (1-127). Draw across the steps; right-click a step = default.\nSoft hits are also darker (velocity tone).");
    ImGui::SameLine(0, 4);
    if (ToggleButton("NUDGE", !swingTab_ && kind_ == LaneKind::Nudge, ImVec2(0, 0), ToggleTab())) { kind_ = LaneKind::Nudge; swingTab_ = false; }
    if (ImGui::IsItemHovered()) Hint("Microtiming of each hit, -50 ... +50 %% of a step.\nBelow the middle line = ahead of the grid (rushing), above = laid back.");
    ImGui::SameLine(0, 4);
    if (ToggleButton("SWING", swingTab_, ImVec2(0, 0), ToggleTab())) { swingTab_ = true; }
    if (ImGui::IsItemHovered()) Hint("Swing of the whole pattern and of this track on its own, with a preview of where the hits land.");
    if (!swingTab_) {
    ImGui::SameLine(0, 14);
    if (ImGui::Button("HUMANIZE")) HumanizeLane(app, ti, kind_, humanizePct_);
    if (ImGui::IsItemHovered()) Hint("Random jitter of the lane on every hit of this track (%.0f %% of the range). Undoable.", humanizePct_);
    ImGui::SameLine(0, 4);
    {
        ValueSpec sp; sp.lo = 2.0f; sp.hi = 60.0f; sp.step = 0.5f; sp.wheel = 1.0f; sp.def = 15.0f; sp.integer = true; sp.unit = "%";
        ValueField("##hum", &humanizePct_, sp, 54.0f);
        if (ImGui::IsItemHovered()) Hint("HUMANIZE amount, 2-60 %% of the range. Option+click = 15.");
    }
    ImGui::SameLine(0, 8);
    if (ImGui::Button("RESET")) ResetLane(app, ti, kind_);
    if (ImGui::IsItemHovered()) Hint("Set the whole lane of this track back to default (velocity: from ACCENT / GHOST, nudge: 0)");
    ImGui::SameLine(0, 14);
    DrawEuclidTools(app, ti);
    }

    // ---- geometry: the same 16-step page as the sequencer ------------------------
    const int gridSpb = ClampGrid(pat.stepsPerBeat);
    const int kPageSteps = PageStepsFor(gridSpb), kGroup = GroupStepsFor(gridSpb);
    const float kLabelW = 6.0f, kCellGap = 2.0f, kGroupGap = 6.0f;
    const float cellW = std::max(8.0f, CellWidthFor(innerW - 6.0f, kLabelW, kCellGap, kGroupGap, kPageSteps, kGroup));
    const int base = std::clamp(app.deskStepPage, 0, std::max(0, (kMaxSteps / kPageSteps) - 1)) * kPageSteps;
    const PageGeom geom{origin.x + kLabelW, cellW, kCellGap, kGroupGap, base, kPageSteps, kGroup};
    const int trackLen = t.useOwnLength ? t.length : pat.masterLength;
    const int playStep = app.sequencer.IsPlaying() && app.EditIsPlaying() ? app.sequencer.StepForTrack(ti) : -1;
    const float totalW = geom.X(kPageSteps - 1) + cellW - geom.x0;

    // step numbers
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 win = ImGui::GetWindowPos();
    for (int c = 0; c < kPageSteps && !swingTab_; ++c) {
        char b[6]; std::snprintf(b, sizeof(b), "%d", base + c + 1);
        const bool beat = c % gridSpb == 0;
        ImVec2 ts = ImGui::CalcTextSize(b);
        dl->AddText(ImVec2(win.x + geom.X(c) + (cellW - ts.x) * 0.5f - ImGui::GetScrollX(), win.y + rowsTop - 2.0f - ImGui::GetScrollY()),
                    beat ? IM_COL32(190, 194, 208, 230) : IM_COL32(110, 114, 130, 200), b);
    }

    if (swingTab_) DrawSwingTab(app, ti, rowsTop + 4.0f, bodyH + 10.0f, innerW, geom, trackLen);
    else DrawStepLane(app, ti, kind_, state_, geom, rowsTop + 14.0f, bodyH - 18.0f, totalW, trackLen, playStep);

    DrawTrackVolumeStrip(app, layout, rowsTop + kNumTracks * layout.rowH + 4.0f, ti);
    ImGui::PopID();
}

void LanePanel::DrawSwingTab(AppState& app, int ti, float top, float height, float innerW, const PageGeom& geom, int trackLen) {
    Pattern& pat = app.CurrentPattern();
    Track& t = app.StepTrack(pat, ti);
    const ImVec2 win = ImGui::GetWindowPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float x0 = geom.x0, w = innerW - 2.0f * x0 + 10.0f;
    const ImU32 col = TrackColor(ti);

    auto mpc = [](float sw) { return 50.0f + 25.0f * sw; };   // MPC-style percentage (50 = straight, 66.7 = triplet)

    // ---- pattern swing ----
    ImGui::SetCursorPos(ImVec2(x0, top));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f), "SCENE SWING");
    ImGui::SameLine(0, 10);
    {
        float pct = static_cast<float>(std::lround(pat.swing * 100.0f));
        ValueSpec sp; sp.lo = 0.0f; sp.hi = 75.0f; sp.step = 0.5f; sp.wheel = 1.0f; sp.def = 0.0f; sp.integer = true; sp.unit = "%";
        const ValueResult r = ValueField("##patswing", &pct, sp, std::max(120.0f, w - 330.0f));
        if (r.started) app.PushUndo();
        if (r.changed) pat.swing = std::clamp(pct, 0.0f, 75.0f) / 100.0f;
        if (r.cancelled) app.DropLastUndo();
    }
    ImGui::SameLine(0, 8);
    ImGui::TextDisabled("= MPC %.0f%%", mpc(pat.swing));
    ImGui::SetCursorPosX(x0);
    static const struct { const char* label; float v; } kQuick[] = {{"STRAIGHT", 0.0f}, {"LIGHT 20", 0.20f}, {"MPC 58", 0.32f}, {"MPC 62", 0.48f}, {"TRIPLET", 0.667f}};
    for (int i = 0; i < 5; ++i) {
        if (i) ImGui::SameLine(0, 4);
        if (ImGui::Button(kQuick[i].label)) { app.PushUndo(); pat.swing = kQuick[i].v; }
    }
    if (ImGui::IsItemHovered()) Hint("Every 2nd 16th note is delayed by half of this swing, in steps: 67 %% = a triplet feel.");

    // ---- track swing ----
    ImGui::Dummy(ImVec2(1, 6));
    ImGui::SetCursorPosX(x0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col), "SWING OF %.8s", TrackDisplayName(t).c_str());
    ImGui::SameLine(0, 10);
    const bool own = t.swing >= 0.0f;
    if (ToggleButton(own ? "OWN" : "SCENE", own, ImVec2(70, 0), ToggleTab())) { app.PushUndo(); t.swing = own ? -1.0f : pat.swing; }
    if (ImGui::IsItemHovered()) Hint("SCENE = this track follows the scene swing.\nOWN = its own swing (a straight kick under swung hats, or the other way round).");
    ImGui::SameLine(0, 10);
    ImGui::BeginDisabled(!own);
    {
        float tp = static_cast<float>(own ? std::lround(t.swing * 100.0f) : std::lround(pat.swing * 100.0f));
        ValueSpec sp; sp.lo = 0.0f; sp.hi = 75.0f; sp.step = 0.5f; sp.wheel = 1.0f; sp.def = 0.0f; sp.integer = true; sp.unit = "%";
        const ValueResult r = ValueField("##trkswing", &tp, sp, std::max(120.0f, w - 330.0f), own);
        if (r.started) app.PushUndo();
        if (r.changed) t.swing = std::clamp(tp, 0.0f, 75.0f) / 100.0f;
        if (r.cancelled) app.DropLastUndo();
    }
    ImGui::EndDisabled();

    if (pat.stepsPerBeat == 3 || pat.stepsPerBeat == 6) {
        ImGui::SetCursorPosX(x0);
        ImGui::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f), "Swing is not used on the triplet grids (they have their own shuffle).");
    }

    // ---- preview: where the hits of this track land on the 16-step page ----
    const float pTop = ImGui::GetCursorPosY() + 14.0f;
    const float pH = std::max(90.0f, top + height - pTop - 44.0f);
    const float L = win.x + x0, R = L + w, T = win.y + pTop, B = T + pH;
    dl->AddRectFilled(ImVec2(L - 4.0f, T), ImVec2(R, B), vis::kInset, 4.0f);
    const float cx0 = L + geom.cellW * 0.5f;
    const float pitch = (geom.X(geom.pageSteps - 1) - geom.X(0)) / static_cast<float>(geom.pageSteps - 1);
    const float sw = std::clamp(t.swing >= 0.0f ? t.swing : pat.swing, 0.0f, 0.75f);

    for (int c = 0; c < geom.pageSteps; ++c) {
        const float gx = cx0 + (geom.X(c) - geom.X(0));
        const bool beat = c % std::max(1, ClampGrid(pat.stepsPerBeat)) == 0;
        dl->AddLine(ImVec2(gx, T + 16.0f), ImVec2(gx, B - 6.0f), beat ? vis::kLineHi : vis::kCellHover, 1.0f);
        char b[16]; std::snprintf(b, sizeof(b), "%d", geom.base + c + 1);
        ImVec2 ts = ImGui::CalcTextSize(b);
        dl->AddText(ImVec2(gx - ts.x * 0.5f, T + 2.0f), beat ? IM_COL32(190, 194, 208, 230) : IM_COL32(110, 114, 130, 200), b);
        const int stp = geom.base + c;
        if (stp >= kMaxSteps || stp >= trackLen) continue;
        const Step& s = t.steps[stp];
        if (!s.active) continue;
        const double off = StepHitOffset(t, pat, stp, s);
        const float hx = gx + static_cast<float>(off) * pitch;
        const float vel = StepVelocity(s);
        const float r = 4.0f + 7.0f * vel;
        const float cy = (T + 16.0f + B - 6.0f) * 0.5f;
        if (std::fabs(off) > 0.004) {
            dl->AddLine(ImVec2(gx, cy), ImVec2(hx, cy), IM_COL32(255, 255, 255, 110), 1.5f);
            dl->AddCircle(ImVec2(gx, cy), r, IM_COL32(255, 255, 255, 70), 16, 1.0f);   // where it would be on the grid
        }
        dl->AddCircleFilled(ImVec2(hx, cy), r, ColorAlpha(col, 235), 16);
    }
    ImGui::SetCursorPos(ImVec2(x0, pTop + pH + 6.0f));
    ImGui::TextDisabled("Filled = where the hit sounds (swing %d%% + nudge); ring = the straight grid. Size = velocity.", static_cast<int>(std::lround(sw * 100.0f)));
}

} // namespace t8::desktop
