#pragma once
// The mouse / keyboard contract that EVERY control follows (docs/INTERACTION_MODEL.md, section 0). One place for the
// modifier meaning and the numbers, so a field in the header and a field in the mixer cannot drift apart.
//
//   drag (horizontal or vertical)  change the value        Shift while dragging = fine (x0.1)
//   Option(Alt) + click            reset to the default    (also: double-click on a slider-like control is NOT reset)
//   double-click on a number       type the value          Enter / click elsewhere applies, Esc cancels
//   mouse wheel over the control   one step                Shift = fine; not with Cmd (Cmd+wheel zooms views)
//   Esc during a drag              puts the value back
//   hover                          lighter field + the matching cursor
#include "imgui.h"

namespace t8::desktop::interaction {

constexpr float kDragThresholdPx = 3.0f;   // a click that moves less than this is a click, not a drag
constexpr float kFineFactor = 0.1f;        // Shift while dragging / wheel

inline bool IsFine()  { return ImGui::GetIO().KeyShift; }
inline bool IsReset() { return ImGui::GetIO().KeyAlt; }   // Option on macOS
inline bool IsCopy()  { return ImGui::GetIO().KeyAlt; }   // Option(Alt) while dragging = copy instead of move (v44.45: was Ctrl, which on a Mac is Cmd)
// The words for the hints: the key is Option on a Mac and Alt elsewhere.
inline const char* OptionName() {
#ifdef __APPLE__
    return "Option";
#else
    return "Alt";
#endif
}
inline bool IsCmd()   { const ImGuiIO& io = ImGui::GetIO(); return io.KeyCtrl || io.KeySuper; }   // ImGui maps Cmd to Ctrl on macOS

// Windows that ask something (modals, browsers) all answer the same keys: Return = the main button, Esc = cancel / close.
// Esc only counts for the window that has the focus, so Esc in a rename box inside a dialog closes the box, not the dialog.
inline bool KeyConfirm() { return ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false); }
inline bool KeyCancel()  { return ImGui::IsKeyPressed(ImGuiKey_Escape, false) && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows); }

inline void ValueCursor() { ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW); }

} // namespace t8::desktop::interaction
