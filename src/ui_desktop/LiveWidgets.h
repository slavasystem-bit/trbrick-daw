#pragma once
// Small hand-drawn widgets shared by the LIVE screen: name prompt, toggle buttons, name button.
// (The fader that used to live here is ValueField with BarSpec, see ValueField.h.)
#include "imgui.h"
#include "Visual.h"
#include <algorithm>

namespace t8::desktop {

// Small modal-ish "type a name" popup. Open it with ImGui::OpenPopup(popupId)
// in the same ID scope; call this every frame right after. Returns true on
// the frame the user confirms (Enter or SAVE) with a non-empty name.
inline bool NamePrompt(const char* popupId, char* buf, size_t bufSize, const char* title, const char* confirmLabel = "SAVE") {
    bool confirmed = false;
    if (ImGui::BeginPopup(popupId)) {
        ImGui::TextDisabled("%s", title);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(220);
        bool enter = ImGui::InputText("##name", buf, bufSize, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine(0, 6);
        bool click = ImGui::Button(confirmLabel);
        if ((enter || click) && buf[0] != '\0') { confirmed = true; ImGui::CloseCurrentPopup(); }
        else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();   // Esc = cancel, like every window that asks
        ImGui::EndPopup();
    }
    return confirmed;
}

// A button with a persistent ON look. Every toggle in the LIVE screen goes
// through this so a lit button (mute, solo, stutter, chain, tabs ...) keeps
// its colour while it is hovered or pressed -- pushing only ImGuiCol_Button
// lets the hover/active colours replace it, which reads as "went dark".
struct ToggleColors {
    ImVec4 on = ImVec4(0.95f, 0.64f, 0.24f, 1.0f);   // amber
    ImVec4 off = ImVec4(0.15f, 0.16f, 0.20f, 1.0f);
    ImVec4 textOn = ImVec4(0.10f, 0.10f, 0.10f, 1.0f);
    ImVec4 textOff = ImVec4(0.91f, 0.91f, 0.93f, 1.0f);
};
inline ImVec4 ToggleLighten(ImVec4 c, float a) {
    return ImVec4(std::min(1.0f, c.x + a), std::min(1.0f, c.y + a), std::min(1.0f, c.z + a), c.w);
}
inline bool ToggleButton(const char* label, bool on, ImVec2 size = ImVec2(0, 0), const ToggleColors& tc = ToggleColors()) {
    ImVec4 base = on ? tc.on : tc.off;
    ImGui::PushStyleColor(ImGuiCol_Button, base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ToggleLighten(base, on ? 0.07f : 0.07f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ToggleLighten(base, on ? -0.06f : 0.14f));
    ImGui::PushStyleColor(ImGuiCol_Text, on ? tc.textOn : tc.textOff);
    bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}
inline ToggleColors ToggleRed() { ToggleColors c; c.on = ImVec4(0.82f, 0.29f, 0.29f, 1.0f); c.textOn = ImVec4(1, 1, 1, 1); return c; }
inline ToggleColors ToggleTeal() { ToggleColors c; c.on = ImVec4(0.37f, 0.88f, 0.79f, 1.0f); c.textOn = ImVec4(0.06f, 0.09f, 0.09f, 1.0f); return c; }
inline ToggleColors ToggleTab() {   // dark amber "selected tab" look
    ToggleColors c; c.on = ImVec4(0.36f, 0.25f, 0.09f, 1.0f); c.textOn = ImVec4(1.0f, 0.86f, 0.62f, 1.0f); return c;
}


// the wide "current name" button of the KIT / BEAT rows (opens a browser window), same look as the PRESET button
inline bool NameButton(const char* id, float w, const std::string& label, ImU32 dot) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + w, p0.y + h);
    dl->AddRectFilled(p0, p1, hov ? vis::kCellHover : vis::kCell, 4.0f);
    dl->AddRect(p0, p1, hov ? vis::kAccent : vis::kLineHi, 4.0f);
    dl->AddCircleFilled(ImVec2(p0.x + 10, p0.y + h * 0.5f), 3.5f, dot);
    ImGui::PushClipRect(ImVec2(p0.x + 20, p0.y), ImVec2(p1.x - 16, p1.y), true);
    dl->AddText(ImVec2(p0.x + 20, p0.y + (h - ImGui::GetTextLineHeight()) * 0.5f), vis::kText, label.c_str());
    ImGui::PopClipRect();
    dl->AddTriangleFilled(ImVec2(p1.x - 14, p0.y + h * 0.5f - 2), ImVec2(p1.x - 6, p0.y + h * 0.5f - 2), ImVec2(p1.x - 10, p0.y + h * 0.5f + 3), vis::kTextDim);
    return clicked;
}

} // namespace t8::desktop
