#pragma once
// Stage C4b: the controls of a SAMPLE fill, shared by the brick's FILL page and the scene's fill window:
//
//   SAMPLE [<] [name v] [>] [HEAR]   FIT [STRETCH] [AS IS]   [DRUMS: MUTE | DRUMS: PLAY ON]
//
// The name opens the SAMPLES browser for this fill (a library file is copied into samples/). < > step through the project's own
// copies. Returns true when the rule was changed here (the caller pushes Undo and stores it); a sample chosen in the browser is stored
// by AppState::SetFillSample itself.
#include "SampleRow.h"
#include "LiveWidgets.h"
#include "InfoLine.h"
#include "app/AppState.h"
#include "imgui.h"
#include <algorithm>
#include <string>
#include <vector>

namespace t8::desktop {

// `where` >= 0: the own fill of that brick; < 0: the fill of scene (-1 - where) (as AppState::FillRuleAt).
inline bool FillSampleControls(AppState& app, FillRule& r, int where, float rowWidth = 300.0f) {
    bool ch = false;
    const std::string cur = r.Sample();
    const std::vector<std::string>& names = app.SampleNames();
    SampleRowSpec spec;
    spec.id = "##fillSmp";
    spec.caption = "SAMPLE";
    spec.captionW = 52.0f;
    spec.width = rowWidth;
    spec.label = cur.empty() ? std::string("CHOOSE A SAMPLE...") : cur;
    spec.dot = !cur.empty() && app.SampleMissing(cur) ? IM_COL32(230, 80, 70, 255) : 0;
    spec.canStep = names.size() > 1 || (names.size() == 1 && names[0] != cur);
    spec.canHear = !cur.empty() && !app.SampleMissing(cur);
    spec.nameHint = "The sample this fill plays: from this project's samples, or from MY LIBRARY (it is copied into the project).";
    spec.stepHint = "< > = the neighbour sample of this project, at once";
    const SampleRowHit hit = SampleRow(spec);
    if (hit.open) { app.libraryBrowserRequest = AppState::LibTarget::Fill; app.libraryBrowserBrick = where; }
    if (hit.step != 0 && !names.empty()) {
        auto it = std::find(names.begin(), names.end(), cur);
        int idx = it == names.end() ? (hit.step > 0 ? -1 : 0) : static_cast<int>(it - names.begin());
        idx = (idx + hit.step + static_cast<int>(names.size())) % static_cast<int>(names.size());
        if (names[static_cast<size_t>(idx)].size() < static_cast<size_t>(FillRule::kSampleMax)) { r.SetSample(names[static_cast<size_t>(idx)]); ch = true; }
    }
    if (hit.hear) if (const SamplePtr s = app.SampleFor(cur)) app.engine.AuditionSample(s, AppState::kAuditionGain);
    ImGui::SameLine(0, 10);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("FIT");
    ImGui::SameLine(0, 4);
    if (ToggleButton("STRETCH##fFit", r.sampleFit == 0, ImVec2(0, 0), ToggleTab())) { r.sampleFit = 0; ch = true; }
    if (ImGui::IsItemHovered()) Hint("STRETCH: the whole sample fits the fill exactly (it gets faster or slower, and higher or lower with it).");
    ImGui::SameLine(0, 4);
    if (ToggleButton("AS IS##fFit", r.sampleFit == 1, ImVec2(0, 0), ToggleTab())) { r.sampleFit = 1; ch = true; }
    if (ImGui::IsItemHovered()) Hint("AS IS: the sample plays at its own speed; a longer one is cut where the fill ends.");
    ImGui::SameLine(0, 10);
    if (ToggleButton(r.drumsMute ? "DRUMS: MUTE##fDr" : "DRUMS: PLAY ON##fDr", !r.drumsMute, ImVec2(0, 0), ToggleTab())) { r.drumsMute = !r.drumsMute; ch = true; }
    if (ImGui::IsItemHovered()) Hint("DRUMS: MUTE = the drums are silent while the sample plays; PLAY ON = they keep playing under it. Click to switch.");
    return ch;
}

} // namespace t8::desktop
