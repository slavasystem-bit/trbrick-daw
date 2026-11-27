#pragma once
// Shared custom ImGui knob widgets -- ImGui has no built-in knob, so these
// are hand-built from ImGui::InvisibleButton + ImDrawList, originally
// written for TrackDetailPanel.cpp and now reused by FxPanel.cpp/
// PianoRollPanel.cpp too (extracted here rather than copy-pasted a third
// time).
//   - KnobAbsolute: the value's full range is KNOWN (e.g. 0..1 send
//     level, -24..24 semitones) -- shows a real proportional fill bar;
//     since C3 it is a ValueField (box look) with all its gestures.
// KnobColumn wraps a caption+knob+value trio in one BeginGroup/EndGroup so
// callers can SameLine() BETWEEN whole knob columns without the classic
// ImGui gotcha where a Text() call right after SameLine() only keeps that
// one line indented, not subsequent ones in the same column.
#include "imgui.h"
#include "InfoLine.h"
#include "Visual.h"
#include "ValueField.h"
#include <algorithm>

namespace t8::desktop {

// C3: a ValueField in the box look, so it follows every rule of Interaction.h (drag from where it is, Shift finer, Option+click
// = `def`, wheel or Up / Down over it = a step, double-click = type the number). Returns the new value; the caller pushes the undo.
// `scale`: what the tip and the typed number are in (100 = a 0..1 value in percent, 1000 = seconds in ms).
inline float KnobAbsolute(const char* id, float value, float minV, float maxV, const char* tooltipFmt, ImVec2 size, float def, float scale = 1.0f) {
    ValueSpec sp = BoxSpec(minV, maxV, def, size.y);
    sp.scale = scale;   // typed and shown as the knob's text shows it: a 0..1 value as percent (100), seconds as ms (1000)
    sp.fmt = scale >= 100.0f || maxV - minV >= 20.0f ? "%.0f" : "%.2f";
    float v = value;
    ValueField(id, &v, sp, size.x);
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ValueTip(tooltipFmt, v * scale);
    return v;
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
