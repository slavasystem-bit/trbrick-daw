#pragma once
// Visual identity for TR-Brick DAW: a dark, modern DAW look (real
// anti-aliased type, flat panels, one accent color).
#include "imgui.h"

namespace t8::desktop {

// Applies the color/spacing/rounding theme to the CURRENT ImGui context's
// style. Call once, right after ImGui::CreateContext().
void ApplyDarkDawTheme(ImGuiStyle& style);

// Loads the two bundled TTFs (assets/fonts/Inter.ttf for UI text,
// assets/fonts/JetBrainsMono.ttf for tabular numeric readouts -- BPM,
// step counts, dB/percent values) at a few sizes into the given font
// atlas, and returns them via the out-params so callers can push/pop them
// around specific widgets (ImGui::PushFont/PopFont). Paths are resolved
// relative to the current working directory, same convention as kits/ and
// every other asset this project loads (see AppBootstrap.h's ChdirToProjectRoot)
// -- NOT baked in at build time, so moving the project folder doesn't
// break it, the same bug class the kit-loading code already had to fix
// once. Falls back to ImGui's built-in default font (with a stderr
// warning) if a file is missing, rather than failing to start.
struct DesktopFonts {
    ImFont* uiRegular = nullptr;   // Inter, ~16px -- body text, labels, buttons
    ImFont* uiSemibold = nullptr;  // Inter, ~20px -- panel headers (size-based
                                    // emphasis stands in for a missing bold
                                    // weight; see assets/fonts/LICENSE_FONTS.txt)
    ImFont* monoRegular = nullptr; // JetBrains Mono, ~16px -- BPM/step/dB readouts
    ImFont* monoLarge = nullptr;   // JetBrains Mono, ~28px -- the big transport BPM display
};
DesktopFonts LoadDesktopFonts(ImGuiIO& io);

} // namespace t8::desktop
