#pragma once
// The one place where the program explains itself (U2, v44.17): an info line at the bottom of the window. Whatever is under
// the mouse says what it does there; the gestures that work on it come after it (like Quick Help in Logic, Info View in
// Live). No pop-up tooltips: a tooltip hides what is next to it and appears late.
//
//   if (ImGui::IsItemHovered()) Hint("What this does.");      // printf-style; call it where a SetTooltip would have been
//   ValueTip("%d bar%s", n, n == 1 ? "" : "s");               // only for the number that follows a drag (a value bubble)
//
// ValueField reports its own gestures (HintGestures) so a field with no text still explains how to use it.
#include "imgui.h"
#include "Visual.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace t8::desktop {
namespace info_detail {
struct State {
    std::string text, gestures;
    int textFrame = -1, gestFrame = -1;
    std::string heldText, heldGest;
    double heldAt = -100.0;
};
inline State& St() { static State s; return s; }
inline std::string Flatten(const char* in) {   // line breaks become spaces; the info line wraps by itself
    std::string o;
    for (const char* p = in; *p; ++p) {
        if (*p == '\n') { if (!o.empty() && o.back() != ' ') o += "  "; }
        else o += *p;
    }
    return o;
}
} // namespace info_detail

inline void Hint(const char* fmt, ...) {
    char buf[768];
    va_list ap; va_start(ap, fmt); std::vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    info_detail::State& s = info_detail::St();
    s.text = info_detail::Flatten(buf);
    s.textFrame = ImGui::GetFrameCount();
}
inline void HintGestures(const char* gestures) {
    info_detail::State& s = info_detail::St();
    s.gestures = gestures;
    s.gestFrame = ImGui::GetFrameCount();
}
// A short value bubble that follows the mouse while a drag is going (the one thing a tooltip is still good for).
inline void ValueTip(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); ImGui::SetTooltipV(fmt, ap); va_end(ap);
}

constexpr float kInfoLineHeight = 40.0f;

// Draws the line into the rectangle [a, b]. Call once per frame, after everything else has had its chance to call Hint().
inline void DrawInfoLine(ImDrawList* dl, ImVec2 a, ImVec2 b, double now) {
    info_detail::State& s = info_detail::St();
    const int f = ImGui::GetFrameCount();
    // a hint set by a window drawn after this call (a browser) is picked up one frame later: accept the last two frames
    std::string text = s.textFrame >= f - 1 ? s.text : std::string();
    std::string gest = s.gestFrame >= f - 1 ? s.gestures : std::string();
    if (!text.empty() || !gest.empty()) { s.heldText = text; s.heldGest = gest; s.heldAt = now; }
    else if (now - s.heldAt < 0.25) { text = s.heldText; gest = s.heldGest; }   // no flicker while the mouse crosses a gap
    const bool idle = text.empty() && gest.empty();
    if (idle) text = "Point at anything to see what it does here.   ?  all gestures   Space  play / stop   Cmd+Z  undo   Shift+Cmd+Z  redo";

    dl->AddRectFilled(a, b, vis::kInset, 4.0f);
    dl->AddRect(a, b, vis::kLine, 4.0f);
    const float pad = 10.0f, wrapW = (b.x - a.x) - pad * 2;
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize(), lh = ImGui::GetTextLineHeight();
    const float maxH = (b.y - a.y) - 6.0f;
    // shorten what does not fit in the room (never spill out of the line)
    while (!text.empty() && ImGui::CalcTextSize(text.c_str(), nullptr, false, wrapW).y > maxH + 0.5f) {
        text.resize(text.size() > 12 ? text.size() - 10 : 0);
        text += "...";
        if (text.size() <= 3) break;
    }
    const float th = text.empty() ? 0.0f : ImGui::CalcTextSize(text.c_str(), nullptr, false, wrapW).y;
    const bool twoLines = !gest.empty() && th + lh <= maxH + 0.5f && !idle;
    const float total = th + (twoLines ? lh : 0.0f);
    float y = a.y + ((b.y - a.y) - total) * 0.5f;
    if (!text.empty()) dl->AddText(font, fs, ImVec2(a.x + pad, y), idle ? vis::kTextDim : vis::kText, text.c_str(), nullptr, wrapW);
    if (twoLines) dl->AddText(font, fs, ImVec2(a.x + pad, y + th), vis::kAccent, gest.c_str(), nullptr, wrapW);
}

} // namespace t8::desktop
