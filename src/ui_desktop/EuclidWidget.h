#pragma once
// EUCLID / ROTATE tools for the selected track (v44.4): one "EUCLID" button that opens a small window with HITS / STEPS / ROTATE,
// a preview of the figure, presets (3-3-2 ...) and quick ROTATE buttons for the whole track. Used by the lane panel (drums) and
// the piano roll (synths / chords).
#include "app/AppState.h"
#include "LiveWidgets.h"
#include "ValueField.h"
#include "imgui.h"
#include "InfoLine.h"
#include <algorithm>
#include <cstdio>

namespace t8::desktop {

inline void DrawEuclidTools(AppState& app, int ti) {
    static int hits = 3, steps = 8, rotate = 0;
    ImGui::PushID("euclidTools");
    const bool euclidOn = [&] { const Track& tt = app.StepTrack(app.CurrentPattern(), ti); return tt.euclidOn; }();   // v44.55: lit while the melody is spread
    char btnLabel[48];
    if (euclidOn) { const Track& tt = app.StepTrack(app.CurrentPattern(), ti); std::snprintf(btnLabel, sizeof btnLabel, "EUCLID %d/%d###euclidBtn", tt.euclidHits, tt.euclidSteps); }
    else std::snprintf(btnLabel, sizeof btnLabel, "EUCLID###euclidBtn");
    if (ToggleButton(btnLabel, euclidOn)) ImGui::OpenPopup("EuclidPopup");
    if (ImGui::IsItemHovered()) Hint("Spread N hits evenly over M steps (3 of 8 = the 3-3-2 pattern), rotate it, write it on this track.\nSPREAD MELODY: the notes of the track are spread over the hits and the original comes back when you turn it off.\nAlso: slide the whole track by steps. Undoable.");
    const ImVec2 bmin = ImGui::GetItemRectMin(), bmax = ImGui::GetItemRectMax();
    ImGui::SetNextWindowPos(ImVec2(bmin.x, bmax.y + 4.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopup("EuclidPopup")) {
        const Pattern& pat = app.CurrentPattern();
        const Track& t = app.StepTrack(pat, ti);
        const int len = std::clamp(t.useOwnLength ? t.length : pat.masterLength, 1, kMaxSteps);
        const bool chord = t.type == TrackType::Chord;
        ImGui::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f), "EUCLID  -  evenly spread hits");
        if (t.euclidOn) { hits = t.euclidHits; steps = t.euclidSteps; rotate = t.euclidRotate; }   // v44.55: show the figure this track really has
        steps = std::clamp(steps, 1, kMaxSteps);
        hits = std::clamp(hits, 0, steps);
        {
            auto field = [&](const char* label, int* v, int lo, int hi, int def) {
                float f = static_cast<float>(*v);
                ValueSpec sp; sp.lo = static_cast<float>(lo); sp.hi = static_cast<float>(hi); sp.step = 0.1f; sp.wheel = 1.0f; sp.def = static_cast<float>(def); sp.integer = true;
                ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(label); ImGui::SameLine(70);
                const ValueResult r = ValueField(label, &f, sp, 110.0f);
                if (r.changed) *v = static_cast<int>(f);
                return r.changed;
            };
            bool ch = field("HITS", &hits, 0, steps, 3);
            if (field("STEPS", &steps, 1, 32, 8)) { hits = std::min(hits, steps); ch = true; }
            rotate = std::clamp(rotate, -(steps - 1), steps - 1);
            if (field("ROTATE", &rotate, -(steps - 1), steps - 1, 0)) ch = true;
            if (ch && t.euclidOn) app.EuclidMelodyOn(ti, hits, steps, rotate);   // v44.55: the kept melody follows the controls live
        }
        // preview: the figure over `steps` steps, repeated through the track length (first 32 shown)
        const std::vector<bool> fig = RotatePattern(EuclidPattern(hits, steps), rotate);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float cw = 11.0f, ch = 16.0f;
        const int shown = std::min(len, 32);
        for (int i = 0; i < shown; ++i) {
            const bool on = fig[static_cast<size_t>(i % steps)];
            const float x = p0.x + i * (cw + 2.0f) + (i / 4) * 4.0f;
            dl->AddRectFilled(ImVec2(x, p0.y), ImVec2(x + cw, p0.y + ch), on ? IM_COL32(240, 160, 60, 255) : IM_COL32(52, 55, 68, 255), 2.0f);
            if (i % steps == 0 && i > 0) dl->AddLine(ImVec2(x - 2.0f, p0.y - 2.0f), ImVec2(x - 2.0f, p0.y + ch + 2.0f), IM_COL32(150, 154, 170, 200));
        }
        ImGui::Dummy(ImVec2(shown * (cw + 2.0f) + (shown / 4) * 4.0f, ch + 4.0f));
        ImGui::TextDisabled("%d hits over %d steps, repeated through %d steps", hits, steps, len);
        static const struct { const char* label; int h, s; } kPre[] = {{"3-3-2", 3, 8}, {"5/8", 5, 8}, {"5/16", 5, 16}, {"7/16", 7, 16}, {"9/16", 9, 16}, {"11/16", 11, 16}, {"4/16", 4, 16}};
        for (size_t k = 0; k < sizeof(kPre) / sizeof(kPre[0]); ++k) {
            if (k > 0) ImGui::SameLine(0, 4);
            if (ImGui::Button(kPre[k].label)) { hits = kPre[k].h; steps = kPre[k].s; rotate = 0; if (t.euclidOn) app.EuclidMelodyOn(ti, hits, steps, rotate); }
        }
        if (chord) ImGui::TextDisabled("The chord track cannot get EUCLID (use ROTATE below).");
        ImGui::BeginDisabled(chord);
        if (ToggleButton("SPREAD MELODY", t.euclidOn, ImVec2(190, 0))) {   // v44.55
            if (t.euclidOn) app.EuclidMelodyOff(ti); else app.EuclidMelodyOn(ti, hits, steps, rotate);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) Hint("ON: the notes of this track stay saved and are spread over the hits, one after another (they cycle when there are more hits than notes).\nTurn the controls live. OFF: the original melody comes back (steps you edited while it was on are kept as they are).");
        ImGui::SameLine(0, 6);
        ImGui::BeginDisabled(chord);
        if (ImGui::Button("APPLY ON THIS TRACK", ImVec2(180, 0))) app.ApplyEuclid(ti, hits, steps, rotate);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) Hint("Replace this track's steps with the figure for good (one note repeated: the first hit the track already has). Use SPREAD MELODY to spread the whole melody and be able to get it back.");
        ImGui::Separator();
        ImGui::TextUnformatted("ROTATE TRACK");
        ImGui::SameLine(0, 8);
        const int beat = ClampGrid(pat.stepsPerBeat);
        if (ImGui::Button("< 1")) app.RotateTrack(ti, -1);
        ImGui::SameLine(0, 2);
        if (ImGui::Button("1 >")) app.RotateTrack(ti, 1);
        ImGui::SameLine(0, 6);
        if (ImGui::Button("< BEAT")) app.RotateTrack(ti, -beat);
        ImGui::SameLine(0, 2);
        if (ImGui::Button("BEAT >")) app.RotateTrack(ti, beat);
        if (ImGui::IsItemHovered()) Hint("Slide the whole figure of this track round its length (a beat = %d steps).", beat);
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

} // namespace t8::desktop
