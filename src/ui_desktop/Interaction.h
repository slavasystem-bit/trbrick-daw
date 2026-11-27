#pragma once
// The mouse / keyboard contract that EVERY control follows (docs/INTERACTION_MODEL.md, section 0). One place for the
// modifier meaning and the numbers, so a field in the header and a field in the mixer cannot drift apart.
//
//   drag (horizontal or vertical)  change the value        Shift while dragging = fine (x0.1)
//   Option(Alt) + click            reset to the default    (also: double-click on a slider-like control is NOT reset)
//   double-click on a number       type the value          Enter / click elsewhere applies, Esc cancels
//   mouse wheel over the control   one step                Shift = fine; not with Cmd (Cmd+wheel zooms views)
//   Esc during a drag              puts the value back
//   mouse over the control + Up / Down   one step (repeats while held)   Shift = fine (C3, the person's ask 27.11.2026)
//   hover                          lighter field + the matching cursor
#include "imgui.h"
#include "imgui_internal.h"   // SetKeyOwner / IsKeyPressed with an owner: the control under the mouse takes Up / Down

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

// ---- Up / Down over a value (C3) ------------------------------------------------------------------------------------------
// A value control under the mouse notes itself (NoteValueHover). At the start of the next frame, before anything reads the
// keys, DrawShellFrame calls ClaimArrowsForHoveredValue: that control owns Up / Down for the frame, so the grid, the piano roll
// and the instrument block's selected cell do not move at the same time. The control then reads its steps with ArrowSteps.
// Not with Option (PITCH) or Cmd, and not while a text field is typed in.
struct HoverKeys { ImGuiID noted = 0; int frame = -10; ImGuiID claimed = 0; };
inline HoverKeys& HoverKeysState() { static HoverKeys h; return h; }
inline void NoteValueHover(ImGuiID id) { HoverKeys& h = HoverKeysState(); h.noted = id; h.frame = ImGui::GetFrameCount(); }
inline void ClaimArrowsForHoveredValue() {
    HoverKeys& h = HoverKeysState();
    const ImGuiIO& io = ImGui::GetIO();
    h.claimed = 0;
    if (h.noted == 0 || h.frame < ImGui::GetFrameCount() - 1 || io.KeyAlt || IsCmd() || io.WantTextInput) return;
    h.claimed = h.noted;
    ImGui::SetKeyOwner(ImGuiKey_UpArrow, h.claimed, ImGuiInputFlags_LockThisFrame);
    ImGui::SetKeyOwner(ImGuiKey_DownArrow, h.claimed, ImGuiInputFlags_LockThisFrame);
}
// +1 per Up, -1 per Down this frame (auto-repeat), for the control that owns the arrows; 0 for every other control.
inline int ArrowSteps(ImGuiID id) {
    if (id == 0 || HoverKeysState().claimed != id) return 0;
    int n = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, ImGuiInputFlags_Repeat, id)) ++n;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, ImGuiInputFlags_Repeat, id)) --n;
    return n;
}

} // namespace t8::desktop::interaction
