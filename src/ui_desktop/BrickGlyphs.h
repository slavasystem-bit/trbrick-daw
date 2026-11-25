#pragma once
// One set of pictograms for the whole program (v44.34), drawn with ImDrawList like StepGlyphs.h: no font, no image files, they stay sharp at
// any scale. A glyph is drawn in a 24 x 24 box and scaled to the tile. The words go to the info line (Hint), not onto the buttons.
//
//   GlyphTile("##id", Glyph::Copy, nullptr, false, vis::kAccent)      a square tile with only the picture
//   GlyphTile("##id", Glyph::Repeat, "x2", true, vis::kAccent)        picture + a short value
//   DrawGlyph(dl, Glyph::Fill, centre, 16.0f, colour)                 the picture alone (tabs, chips)
//
// The colours follow one rule everywhere: amber = what you play with (repeats, trim), green = who plays, blue = level, yellow = fill,
// red = transition, the track colours = instruments (TrackColor).
#include "Visual.h"
#include "core/Types.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>

namespace t8::desktop {

enum class Glyph { Repeat, Trim, Level, Fill, Trans, Flat, FadeIn, FadeOut, Copy, FromLive, Twice, Reset, Remove, Presets, Bricks, Mix, Filter, Param };

namespace colors {
constexpr ImU32 kPlay  = IM_COL32(95, 211, 138, 255);    // PLAY / WHO PLAYS
constexpr ImU32 kLevel = IM_COL32(90, 169, 240, 255);    // LEVEL page
constexpr ImU32 kFill  = IM_COL32(242, 201, 76, 255);    // FILL page
constexpr ImU32 kTrans = IM_COL32(239, 106, 106, 255);   // TRANSITION page
constexpr ImU32 kParam = IM_COL32(178, 140, 242, 255);   // PARAM page (a knob of an instrument)
} // namespace colors

namespace glyph_detail {
struct Pen {
    ImDrawList* dl; ImVec2 o; float s; ImU32 col; float w;
    ImVec2 P(float x, float y) const { return ImVec2(o.x + x * s, o.y + y * s); }
    void Line(float x0, float y0, float x1, float y1) const { dl->AddLine(P(x0, y0), P(x1, y1), col, w); }
    void Poly(std::initializer_list<ImVec2> pts) const {
        for (const ImVec2& p : pts) dl->PathLineTo(P(p.x, p.y));
        dl->PathStroke(col, ImDrawFlags_None, w);
    }
    void Rect(float x0, float y0, float x1, float y1, float r = 1.5f) const { dl->AddRect(P(x0, y0), P(x1, y1), col, r * s, 0, w); }
    void LineW(float x0, float y0, float x1, float y1, float k) const { dl->AddLine(P(x0, y0), P(x1, y1), col, w * k); }
    void Circle(float cx, float cy, float r) const { dl->AddCircle(P(cx, cy), r * s, col, 24, w); }
    void Ellipse(float cx, float cy, float rx, float ry) const {
        for (int k = 0; k < 24; ++k) { const float a = 6.2831853f * k / 24.0f; dl->PathLineTo(P(cx + rx * std::cos(a), cy + ry * std::sin(a))); }
        dl->PathStroke(col, ImDrawFlags_Closed, w);
    }
    void Dot(float x, float y, float r) const { dl->AddCircleFilled(P(x, y), r * s, col, 12); }
    void Arc(float cx, float cy, float r, float a0, float a1) const { dl->PathArcTo(P(cx, cy), r * s, a0, a1, 18); dl->PathStroke(col, ImDrawFlags_None, w); }
    // a small chevron at `tip`, pointing along (dx, dy)
    void Head(float tx, float ty, float dx, float dy, float len = 3.4f) const {
        const float l = std::sqrt(dx * dx + dy * dy); if (l < 1e-4f) return;
        dx /= l; dy /= l;
        const float px = -dy, py = dx;
        Poly({ImVec2(tx - dx * len + px * len * 0.8f, ty - dy * len + py * len * 0.8f), ImVec2(tx, ty), ImVec2(tx - dx * len - px * len * 0.8f, ty - dy * len - py * len * 0.8f)});
    }
};
} // namespace glyph_detail

// `centre` is the middle of the picture, `size` its width and height in pixels.
inline void DrawGlyph(ImDrawList* dl, Glyph g, ImVec2 centre, float size, ImU32 col) {
    const float s = size / 24.0f;
    glyph_detail::Pen p{dl, ImVec2(centre.x - size * 0.5f, centre.y - size * 0.5f), s, col, std::max(1.3f, size * 0.085f)};
    switch (g) {
    case Glyph::Repeat:   // two arrows chasing each other
        p.Poly({ImVec2(4, 11), ImVec2(4, 8), ImVec2(7, 5), ImVec2(19, 5)}); p.Head(20, 5, 1, 0);
        p.Poly({ImVec2(20, 13), ImVec2(20, 16), ImVec2(17, 19), ImVec2(5, 19)}); p.Head(4, 19, -1, 0);
        break;
    case Glyph::Trim:     // two walls and an arrow between them: only this much plays
        p.Line(4, 5, 4, 19); p.Line(20, 5, 20, 19);
        p.Line(8, 12, 16, 12); p.Head(8, 12, -1, 0, 3.0f); p.Head(16, 12, 1, 0, 3.0f);
        break;
    case Glyph::Level:    // a ramp with its two ends
        p.Line(5, 18, 19, 6); p.Dot(5, 18, 2.2f); p.Dot(19, 6, 2.2f);
        break;
    case Glyph::Fill:     // drum hits getting busier
        p.Line(3.5f, 18, 3.5f, 12); p.Line(7.5f, 18, 7.5f, 9); p.Line(11.5f, 18, 11.5f, 5); p.Line(15.5f, 18, 15.5f, 9); p.Line(19.5f, 18, 19.5f, 12);
        break;
    case Glyph::Trans:    // a riser building up to the brick
        p.Poly({ImVec2(3, 19), ImVec2(21, 19), ImVec2(21, 5), ImVec2(3, 19)});
        break;
    case Glyph::Flat:     p.Line(3, 12, 21, 12); break;
    case Glyph::FadeIn:   p.Line(3, 19, 21, 5); break;
    case Glyph::FadeOut:  p.Line(3, 5, 21, 19); break;
    case Glyph::Copy:     // a sheet behind a sheet
        p.Poly({ImVec2(16, 8), ImVec2(16, 5), ImVec2(15, 4), ImVec2(5, 4), ImVec2(4, 5), ImVec2(4, 15), ImVec2(5, 16), ImVec2(8, 16)});
        p.Rect(8, 8, 20, 20, 2.0f);
        break;
    case Glyph::FromLive: // an arrow going round to the dot in the middle: fetch the newest version
        p.Arc(12, 12, 8, -0.6f, 5.4f); p.Head(12 + 8 * std::cos(5.4f), 12 + 8 * std::sin(5.4f), -std::sin(5.4f), std::cos(5.4f)); p.Dot(12, 12, 1.9f);
        break;
    case Glyph::Twice:    // two equal blocks
        p.Rect(3, 7, 11, 17); p.Rect(13, 7, 21, 17); p.Line(9, 3, 15, 3);
        break;
    case Glyph::Reset:    // an arrow going back
        p.Arc(12, 12, 8, -2.5f, 3.1f); p.Head(12 + 8 * std::cos(-2.5f), 12 + 8 * std::sin(-2.5f), std::sin(-2.5f), -std::cos(-2.5f));
        break;
    case Glyph::Remove:   // a bin
        p.Line(4, 7, 20, 7); p.Poly({ImVec2(9, 7), ImVec2(9, 4), ImVec2(15, 4), ImVec2(15, 7)});
        p.Poly({ImVec2(6, 7), ImVec2(7, 20), ImVec2(17, 20), ImVec2(18, 7)}); p.Line(10, 11, 10, 17); p.Line(14, 11, 14, 17);
        break;
    case Glyph::Presets:  // a short list
        p.Line(4, 7, 20, 7); p.Line(4, 12, 20, 12); p.Line(4, 17, 14, 17);
        break;
    case Glyph::Bricks:   // a row of bricks
        p.Rect(2, 8, 8, 16, 1.2f); p.Rect(9, 8, 15, 16, 1.2f); p.Rect(16, 8, 22, 16, 1.2f);
        break;
    case Glyph::Mix:      // three faders
        p.Line(6, 4, 6, 20); p.Line(12, 4, 12, 20); p.Line(18, 4, 18, 20); p.Dot(6, 15, 2.4f); p.Dot(12, 8, 2.4f); p.Dot(18, 12, 2.4f);
        break;
    case Glyph::Filter:   // a low-pass curve
        p.Poly({ImVec2(2, 7), ImVec2(11, 7), ImVec2(14, 9), ImVec2(17, 16), ImVec2(22, 19)});
        break;
    case Glyph::Param:    // a knob with its pointer and a few tick marks
        p.Circle(12, 13, 6.5f); p.Line(12, 13, 15.5f, 8.5f);
        p.Line(4, 19, 2.6f, 20.4f); p.Line(20, 19, 21.4f, 20.4f); p.Line(5.2f, 6.6f, 3.8f, 5.2f); p.Line(18.8f, 6.6f, 20.2f, 5.2f);
        break;
    }
}

// The eleven instruments (global track index: Kick, Snare, Clap, C.Hat, O.Hat, Perc, Bass, Syn 2, Syn 3, Chord, Sampler), same 24 x 24 box.
inline const char* InstrumentName(int t) {
    static const char* k[kNumTracks] = {"KICK", "SNARE", "CLAP", "C.HAT", "O.HAT", "PERC", "BASS", "SYN 2", "SYN 3", "CHORD", "SMPL"};
    return k[std::clamp(t, 0, kNumTracks - 1)];
}
inline void DrawInstrumentGlyph(ImDrawList* dl, int track, ImVec2 centre, float size, ImU32 col) {
    const float s = size / 24.0f;
    glyph_detail::Pen p{dl, ImVec2(centre.x - size * 0.5f, centre.y - size * 0.5f), s, col, std::max(1.2f, size * 0.09f)};
    switch (std::clamp(track, 0, kNumTracks - 1)) {
    case 0:   // kick: the front of a bass drum
        p.Circle(12, 11, 8); p.Circle(12, 11, 3.2f); p.Line(8, 21, 16, 21);
        break;
    case 1:   // snare: a drum from the side with the wires
        p.Ellipse(12, 8, 8, 3); p.Poly({ImVec2(4, 8), ImVec2(4, 16), ImVec2(6, 18.2f), ImVec2(12, 19.2f), ImVec2(18, 18.2f), ImVec2(20, 16), ImVec2(20, 8)});
        p.Poly({ImVec2(7, 14), ImVec2(9, 16), ImVec2(11, 14), ImVec2(13, 16), ImVec2(15, 14), ImVec2(17, 16)});
        break;
    case 2:   // clap: two hands and the burst
        p.Line(12, 2, 12, 6); p.Line(4.5f, 5, 7, 8); p.Line(19.5f, 5, 17, 8);
        p.LineW(7.5f, 12, 8.5f, 20.5f, 2.6f); p.LineW(16.5f, 12, 15.5f, 20.5f, 2.6f);
        break;
    case 3:   // closed hat: two plates on a stand
        p.Ellipse(12, 10, 9, 2.3f); p.Ellipse(12, 13.6f, 9, 2.3f); p.Line(12, 16, 12, 22);
        break;
    case 4:   // open hat: the top plate lifted
        p.Ellipse(12, 5, 9, 2.3f); p.Ellipse(12, 13.6f, 9, 2.3f); p.Line(12, 16, 12, 22); p.Line(8, 9, 7, 11); p.Line(16, 9, 17, 11);
        break;
    case 5:   // perc: a conga
        p.Ellipse(12, 6, 6, 2.2f); p.Poly({ImVec2(6, 6), ImVec2(8, 20), ImVec2(16, 20), ImVec2(18, 6)}); p.Line(7, 11, 17, 11);
        break;
    case 6:   // bass: one long sub wave on a floor
        for (int k = 0; k <= 16; ++k) { const float t = k / 16.0f; dl->PathLineTo(p.P(2 + 20 * t, 12 - 7 * std::sin(6.2831853f * t))); }
        dl->PathStroke(col, ImDrawFlags_None, p.w);
        p.Line(3, 21, 21, 21);
        break;
    case 7:   // synth 2: a saw
        p.Poly({ImVec2(3, 17), ImVec2(9, 7), ImVec2(9, 17), ImVec2(15, 7), ImVec2(15, 17), ImVec2(21, 7)});
        break;
    case 8:   // synth 3: a square
        p.Poly({ImVec2(3, 17), ImVec2(3, 8), ImVec2(9, 8), ImVec2(9, 17), ImVec2(15, 17), ImVec2(15, 8), ImVec2(21, 8), ImVec2(21, 17)});
        break;
    case 10:  // sampler: a recorded waveform (a tall burst that dies away) in a frame
        p.Line(2, 12, 22, 12);
        p.Line(5, 10, 5, 14); p.Line(8, 6, 8, 18); p.Line(11, 3, 11, 21); p.Line(14, 7, 14, 17); p.Line(17, 9, 17, 15); p.Line(20, 11, 20, 13);
        break;
    default:  // chord: three note heads on one stem
        p.Ellipse(9, 19, 3, 2.2f); p.Ellipse(9, 13, 3, 2.2f); p.Ellipse(9, 7, 3, 2.2f); p.Line(12, 5, 12, 19);
        break;
    }
}

// A tile with a picture and, if given, a short text next to it. Returns true when clicked. `on` = the lit (chosen) state.
// `width` 0 = as wide as the content needs. The caller adds the Hint() for the info line.
inline bool GlyphTile(const char* id, Glyph g, const char* text, bool on, ImU32 accent, bool enabled = true, float width = 0.0f, float height = 30.0f,
                      int instrument = -1, ImU32 idleCol = 0) {   // instrument >= 0: draw that instrument's pictogram instead of g; idleCol: its colour while not lit
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = 8.0f, icon = 16.0f;
    float w = width;
    if (w <= 0.0f) w = text && *text ? pad * 2 + icon + 5.0f + ImGui::CalcTextSize(text).x : height;
    const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + height);
    ImGui::InvisibleButton(id, ImVec2(w, height));
    const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled), down = ImGui::IsItemActive();
    const float alpha = enabled ? 1.0f : 0.4f;
    auto A = [&](ImU32 c) { return (c & 0x00FFFFFFu) | (static_cast<ImU32>(((c >> 24) & 0xFF) * alpha) << 24); };
    const ImU32 bg = on ? (accent & 0x00FFFFFFu) | (36u << 24) : (down && enabled ? vis::kCellHover : (hov && enabled ? vis::kCellHover : vis::kCell));
    dl->AddRectFilled(a, b, A(bg), 5.0f);
    dl->AddRect(a, b, A(on ? accent : (hov ? vis::kLineHi : vis::kLine)), 5.0f, 0, on ? 1.4f : 1.0f);
    const ImU32 fg = A(on ? accent : (hov ? vis::kText : (idleCol ? idleCol : vis::kTextDim)));
    auto draw = [&](ImVec2 c, float sz) { if (instrument >= 0) DrawInstrumentGlyph(dl, instrument, c, sz, fg); else DrawGlyph(dl, g, c, sz, fg); };
    if (text && *text) {
        draw(ImVec2(a.x + pad + icon * 0.5f, (a.y + b.y) * 0.5f), icon);
        const ImVec2 ts = ImGui::CalcTextSize(text);
        dl->AddText(ImVec2(a.x + pad + icon + 5.0f, (a.y + b.y) * 0.5f - ts.y * 0.5f), A(on ? accent : vis::kText), text);
    } else {
        draw(ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), icon + 2.0f);
    }
    return enabled && ImGui::IsItemClicked(ImGuiMouseButton_Left);
}

} // namespace t8::desktop
