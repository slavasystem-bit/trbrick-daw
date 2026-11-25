#pragma once
// Stage C4b ("samples in one place"): the one row every place that plays a sample uses, so the drums, the SAMPLER, a synth or the
// chord on ENGINE > SAMPLE and a transition all look and work the same:
//
//   [CAPTION] [<] [name v] [>] [HEAR]
//
//   < >   the neighbour sample in the same place, at once and silently (kit variants, the project's samples, the transitions)
//   name  opens the list of what can go here (the caller draws that popup)
//   HEAR  plays it once
//
// The row only draws and reports; what "neighbour", "list" and "hear" mean is decided by the caller.
#include "imgui.h"
#include "InfoLine.h"
#include "LiveWidgets.h"
#include "Visual.h"
#include <algorithm>
#include <string>

namespace t8::desktop {

struct SampleRowHit {
    int step = 0;        // -1 = <, +1 = >
    bool open = false;   // the name was clicked: open the list
    bool hear = false;   // HEAR
};

struct SampleRowSpec {
    const char* id = "##smpRow";          // unique within the window (the buttons get it as a suffix)
    const char* caption = nullptr;        // e.g. "SAMPLE" before the arrows; null = none
    float captionW = 0.0f;                // room for the caption (so rows above / below line up)
    float width = 260.0f;                 // the whole row, caption included
    std::string label;                    // the name shown on the button
    ImU32 dot = 0;                        // colour of the dot on the name button (the track's colour; red = missing file)
    bool canStep = true;                  // false = the arrows are greyed (fewer than two to step through)
    bool canHear = true;                  // false = HEAR is greyed (nothing to play)
    bool showHear = true;                 // false = no HEAR button at all (e.g. the KIT row)
    const char* nameHint = nullptr;       // info-line text for the name button
    const char* stepHint = "< > = the neighbour sample in the same place, at once and silently";
    const char* hearHint = "Play it once";
    const char* noHearHint = "There is no sample to hear yet.";
};

constexpr float kSampleRowArrowW = 26.0f;
constexpr float kSampleRowHearW = 48.0f;
constexpr float kSampleRowGap = 4.0f;

inline SampleRowHit SampleRow(const SampleRowSpec& s) {
    SampleRowHit hit;
    ImGui::PushID(s.id);
    if (s.caption) {
        ImGui::AlignTextToFramePadding();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(s.captionW, ImGui::GetFrameHeight()));
        ImGui::GetWindowDrawList()->AddText(ImVec2(at.x, at.y + (ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f), vis::kTextDim, s.caption);
        ImGui::SameLine(0, kSampleRowGap);
    }
    const float capW = s.caption ? s.captionW + kSampleRowGap : 0.0f;
    const float hearW = s.showHear ? kSampleRowHearW + kSampleRowGap : 0.0f;
    const float nameW = std::max(40.0f, s.width - capW - kSampleRowArrowW * 2.0f - kSampleRowGap * 2.0f - hearW);

    ImGui::BeginDisabled(!s.canStep);
    if (ImGui::Button("<", ImVec2(kSampleRowArrowW, 0))) hit.step = -1;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && s.stepHint) Hint("%s", s.stepHint);
    ImGui::SameLine(0, kSampleRowGap);
    if (NameButton("##name", nameW, s.label, s.dot ? s.dot : vis::kAccent)) hit.open = true;
    if (ImGui::IsItemHovered() && s.nameHint) Hint("%s", s.nameHint);
    ImGui::SameLine(0, kSampleRowGap);
    ImGui::BeginDisabled(!s.canStep);
    if (ImGui::Button(">", ImVec2(kSampleRowArrowW, 0))) hit.step = 1;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && s.stepHint) Hint("%s", s.stepHint);
    if (s.showHear) {
        ImGui::SameLine(0, kSampleRowGap);
        ImGui::BeginDisabled(!s.canHear);
        if (ImGui::Button("HEAR", ImVec2(kSampleRowHearW, 0))) hit.hear = true;
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) Hint("%s", s.canHear ? s.hearHint : s.noHearHint);
    }
    ImGui::PopID();
    return hit;
}

} // namespace t8::desktop
