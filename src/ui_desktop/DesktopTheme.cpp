#include "DesktopTheme.h"
#include <cstdio>

namespace t8::desktop {

namespace {
// Warm amber accent (nods to the classic orange step-LEDs on a real
// Roland TR-8/808-style groovebox -- the hardware this whole project is
// styled after) on a cool, slightly blue-leaning charcoal ground, rather
// than the generic near-black + lone neon accent a lot of app dark themes
// default to. One accent color used consistently for every "this is on /
// selected / primary action" state (step-on, active fader fill, the play
// button, a focused tab) -- a second, different hue is reserved
// specifically for record/clip (red) and a third for "currently playing"
// confirmation (teal), so those meanings stay visually distinct from the
// one general-purpose accent.
constexpr ImVec4 kBg        (0.082f, 0.086f, 0.110f, 1.00f);
constexpr ImVec4 kPanel     (0.110f, 0.118f, 0.149f, 1.00f);
constexpr ImVec4 kPanelAlt  (0.149f, 0.161f, 0.204f, 1.00f);
constexpr ImVec4 kPanelHover(0.188f, 0.204f, 0.255f, 1.00f);
constexpr ImVec4 kBorder    (0.204f, 0.220f, 0.275f, 1.00f);
constexpr ImVec4 kText      (0.910f, 0.914f, 0.933f, 1.00f);
constexpr ImVec4 kTextDim   (0.557f, 0.573f, 0.639f, 1.00f);
constexpr ImVec4 kAccent    (0.949f, 0.639f, 0.239f, 1.00f);
constexpr ImVec4 kAccentHov (1.000f, 0.729f, 0.388f, 1.00f);
constexpr ImVec4 kAccentDim (0.949f, 0.639f, 0.239f, 0.55f);
} // namespace

void ApplyDarkDawTheme(ImGuiStyle& style) {
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text]                  = kText;
    c[ImGuiCol_TextDisabled]          = kTextDim;
    c[ImGuiCol_WindowBg]              = kBg;
    c[ImGuiCol_ChildBg]               = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]               = kPanel;
    c[ImGuiCol_Border]                = kBorder;
    c[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]               = kPanelAlt;
    c[ImGuiCol_FrameBgHovered]        = kPanelHover;
    c[ImGuiCol_FrameBgActive]         = kPanelHover;
    c[ImGuiCol_TitleBg]               = kPanel;
    c[ImGuiCol_TitleBgActive]         = kPanel;
    c[ImGuiCol_TitleBgCollapsed]      = kPanel;
    c[ImGuiCol_MenuBarBg]             = kPanel;
    c[ImGuiCol_ScrollbarBg]           = kBg;
    c[ImGuiCol_ScrollbarGrab]         = kPanelAlt;
    c[ImGuiCol_ScrollbarGrabHovered]  = kPanelHover;
    c[ImGuiCol_ScrollbarGrabActive]   = kAccentDim;
    c[ImGuiCol_CheckMark]             = kAccent;
    c[ImGuiCol_SliderGrab]            = kAccent;
    c[ImGuiCol_SliderGrabActive]      = kAccentHov;
    c[ImGuiCol_Button]                = kPanelAlt;
    c[ImGuiCol_ButtonHovered]         = kPanelHover;
    c[ImGuiCol_ButtonActive]          = kAccentDim;
    c[ImGuiCol_Header]                = kPanelAlt;
    c[ImGuiCol_HeaderHovered]         = kPanelHover;
    c[ImGuiCol_HeaderActive]          = kAccentDim;
    c[ImGuiCol_Separator]             = kBorder;
    c[ImGuiCol_SeparatorHovered]      = kAccentDim;
    c[ImGuiCol_SeparatorActive]       = kAccent;
    c[ImGuiCol_ResizeGrip]            = kBorder;
    c[ImGuiCol_ResizeGripHovered]     = kAccentDim;
    c[ImGuiCol_ResizeGripActive]      = kAccent;
    c[ImGuiCol_Tab]                   = kPanel;
    c[ImGuiCol_TabHovered]            = kPanelHover;
    c[ImGuiCol_TabSelected]           = kPanelAlt;
    c[ImGuiCol_TabSelectedOverline]   = kAccent;
    c[ImGuiCol_TabDimmed]             = kPanel;
    c[ImGuiCol_TabDimmedSelected]     = kPanelAlt;
    c[ImGuiCol_DockingPreview]        = kAccentDim;
    c[ImGuiCol_DockingEmptyBg]        = kBg;
    c[ImGuiCol_PlotLines]             = kAccent;
    c[ImGuiCol_PlotLinesHovered]      = kAccentHov;
    c[ImGuiCol_PlotHistogram]         = kAccent;
    c[ImGuiCol_PlotHistogramHovered]  = kAccentHov;
    c[ImGuiCol_TextSelectedBg]        = kAccentDim;
    c[ImGuiCol_DragDropTarget]        = kAccent;
    c[ImGuiCol_NavCursor]             = kAccent;

    style.WindowRounding    = 8.0f;
    style.ChildRounding     = 6.0f;
    style.FrameRounding     = 5.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding      = 5.0f;
    style.TabRounding       = 5.0f;
    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.WindowPadding     = ImVec2(12, 12);
    style.FramePadding      = ImVec2(8, 5);
    style.ItemSpacing       = ImVec2(10, 8);
    style.ItemInnerSpacing  = ImVec2(6, 6);
    style.IndentSpacing     = 18.0f;
    style.ScrollbarSize     = 14.0f;
    style.GrabMinSize       = 10.0f;
}

namespace {
ImFont* LoadOne(ImGuiIO& io, const char* path, float size) {
    ImFont* f = io.Fonts->AddFontFromFileTTF(path, size);
    if (!f) {
        std::fprintf(stderr,
            "trbrick_daw_desktop: couldn't load font \"%s\" (%.0fpx) -- falling back to "
            "ImGui's built-in font. Check that you're running from the project root "
            "(same directory kits/ lives in) so assets/fonts/ resolves.\n",
            path, size);
    }
    return f;
}
} // namespace

DesktopFonts LoadDesktopFonts(ImGuiIO& io) {
    DesktopFonts fonts;
    fonts.uiRegular   = LoadOne(io, "assets/fonts/Inter.ttf", 17.0f);
    fonts.uiSemibold  = LoadOne(io, "assets/fonts/Inter.ttf", 21.0f);
    fonts.monoRegular = LoadOne(io, "assets/fonts/JetBrainsMono.ttf", 16.0f);
    fonts.monoLarge   = LoadOne(io, "assets/fonts/JetBrainsMono.ttf", 30.0f);
    // If uiRegular failed to load, nothing has been built into the atlas
    // yet (AddFontFromFileTTF returning null doesn't add a fallback by
    // itself) -- without at least one real font, io.Fonts->Build() has
    // nothing to build and the backend's font upload fails outright, so
    // explicitly fall back to ImGui's built-in pixel font rather than
    // leaving the atlas empty.
    if (!fonts.uiRegular) fonts.uiRegular = io.Fonts->AddFontDefault();
    return fonts;
}

} // namespace t8::desktop
