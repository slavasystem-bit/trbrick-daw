#pragma once
// Shared custom ImGui knob widgets -- ImGui has no built-in knob, so these
// are hand-built from ImGui::InvisibleButton + ImDrawList, originally
// written for TrackDetailPanel.cpp and now reused by FxPanel.cpp/
// PianoRollPanel.cpp too (extracted here rather than copy-pasted a third
// time). Two shapes, matching the two kinds of parameter this UI edits:
//   - KnobAbsolute: the value's full range is KNOWN (e.g. 0..1 send
//     level, -24..24 semitones) -- shows a real proportional fill bar,
//     drag sets an absolute value directly.
//   - KnobRelative: the parameter is only reachable through a +1/-1 step
//     function (AdjustPatchParam-style -- different ranges per field and
//     per synth engine, no single absolute range to show as a fill), so
//     this renders a plain draggable pad and reports discrete steps for
//     the caller to apply; `accumPx` is a per-knob drag accumulator the
//     caller owns (one float per knob) so a slow drag doesn't lose
//     fractional movement between frames.
// KnobColumn wraps a caption+knob+value trio in one BeginGroup/EndGroup so
// callers can SameLine() BETWEEN whole knob columns without the classic
// ImGui gotcha where a Text() call right after SameLine() only keeps that
// one line indented, not subsequent ones in the same column.
#include "imgui.h"
#include "InfoLine.h"
#include "Visual.h"
#include <algorithm>

namespace t8::desktop {

inline float KnobAbsolute(const char* id, float value, float minV, float maxV, const char* tooltipFmt, ImVec2 size) {
    ImGui::PushID(id);
    ImGui::InvisibleButton("##k", size);
    bool dragging = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetItemRectMin();
    ImVec2 p1 = ImGui::GetItemRectMax();
    float norm = (maxV > minV) ? (value - minV) / (maxV - minV) : 0.0f;
    norm = std::clamp(norm, 0.0f, 1.0f);
    dl->AddRectFilled(p0, p1, vis::kCell, 4.0f);
    float fillH = (p1.y - p0.y) * norm;
    dl->AddRectFilled(ImVec2(p0.x, p1.y - fillH), p1,
                       dragging ? vis::kAccentHi : vis::kAccent, 4.0f);
    dl->AddRect(p0, p1, vis::kLineHi, 4.0f);

    float newValue = value;
    if (dragging) {
        float dy = -ImGui::GetIO().MouseDelta.y;
        newValue = std::clamp(value + (dy / size.y) * (maxV - minV), minV, maxV);
    }
    if (dragging || ImGui::IsItemHovered()) {
        ValueTip(tooltipFmt, newValue);
    }
    ImGui::PopID();
    return newValue;
}

inline bool KnobRelative(const char* id, float* accumPx, int* stepsOut, ImVec2 size) {
    ImGui::PushID(id);
    ImGui::InvisibleButton("##k", size);
    bool dragging = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetItemRectMin();
    ImVec2 p1 = ImGui::GetItemRectMax();
    dl->AddRectFilled(p0, p1, dragging ? IM_COL32(40, 36, 28, 255) : vis::kCell, 4.0f);
    dl->AddRect(p0, p1, dragging ? vis::kAccent : vis::kLineHi, 4.0f);
    float midY = (p0.y + p1.y) * 0.5f;
    dl->AddLine(ImVec2(p0.x + 6, midY - 4), ImVec2(p1.x - 6, midY - 4), vis::kLineHi, 1.0f);
    dl->AddLine(ImVec2(p0.x + 6, midY + 4), ImVec2(p1.x - 6, midY + 4), vis::kLineHi, 1.0f);

    *stepsOut = 0;
    constexpr float kPxPerStep = 6.0f;
    if (dragging) {
        *accumPx -= ImGui::GetIO().MouseDelta.y;
        while (*accumPx >= kPxPerStep) { (*stepsOut)++; *accumPx -= kPxPerStep; }
        while (*accumPx <= -kPxPerStep) { (*stepsOut)--; *accumPx += kPxPerStep; }
    } else {
        *accumPx = 0.0f;
    }
    ImGui::PopID();
    return dragging;
}

template <typename KnobFn>
void KnobColumn(const char* label, const char* valueText, KnobFn&& drawKnob) {
    ImGui::BeginGroup();
    ImGui::TextDisabled("%s", label);
    drawKnob();
    ImGui::TextUnformatted(valueText);
    ImGui::EndGroup();
}

} // namespace t8::desktop
