#pragma once
// Mac keyboard shortcuts for the desktop app. Called once per frame after ImGui::NewFrame(); does nothing while a
// text field has focus or a popup is open, so typing a name never triggers play/stop.
//
//   Space              play / stop
//   Cmd+Z              undo
//   Cmd+S              save the project
//   Cmd+C / Cmd+V      copy / paste the selected track          (Shift = the whole pattern)
//   [  and  ]          previous / next pattern
//   1 .. 9, 0          select track 1..10
//   Left / Right       (with Cmd) previous / next track
#include "app/AppState.h"

namespace t8::desktop {

void HandleShortcuts(AppState& app);

} // namespace t8::desktop
