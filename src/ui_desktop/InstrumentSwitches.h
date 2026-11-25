#pragma once
// v44.48: the instrument switches of the LIVE panel and of WHO PLAYS on TRACK, drawn from one place.
//   bar   = mute (click, or drag over several)       S under the bar = solo       group buttons on the right: ALL DRUMS, DRUMS, HATS,
//   DROP, MUTE ALL, ALL ON.
// Everything reads and writes through AppState::Sw*(brick, ...): brick >= 0 is the mask of that brick of the track, -1 is the flags of
// the edited scene (the LIVE jam). The sequencer's M and S buttons use the same stores, so none of these places can disagree.
#include "app/AppState.h"
#include "BrickGlyphs.h"
#include "InfoLine.h"
#include "LiveWidgets.h"
#include "TrackStyle.h"
#include "Visual.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>

namespace t8::desktop::instrument_switches {

constexpr float kBarW = 17.0f, kBarGap = 3.0f, kSoloH = 13.0f;   // v44.50: 11 bars in the width the ten had

inline float StripWidth() { return kNumTracks * (kBarW + kBarGap) - kBarGap; }

// The S button under a bar. `dim`: the instrument has nothing to play. Returns true when it was clicked.
inline bool SoloCell(const char* id, ImVec2 p0, bool on, bool dim) {
    ImGui::SetCursorScreenPos(p0);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(kBarW, kSoloH));
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + kBarW, p0.y + kSoloH);
    const ImU32 fill = on ? vis::kAccent : (hov ? IM_COL32(52, 56, 70, 255) : IM_COL32(38, 42, 53, 255));
    dl->AddRectFilled(p0, p1, fill, 3.0f);
    if (!on) dl->AddRect(p0, p1, IM_COL32(52, 57, 72, 255), 3.0f);
    const ImU32 tc = on ? IM_COL32(26, 28, 36, 255) : (dim ? IM_COL32(90, 94, 110, 255) : vis::kTextDim);
    const ImVec2 ts = ImGui::CalcTextSize("S");
    dl->AddText(ImVec2(p0.x + (kBarW - ts.x) * 0.5f, p0.y + (kSoloH - ts.y) * 0.5f), tc, "S");
    return clicked;
}

// v44.54: a switch waiting for the next bar line (NEXT BAR) pulses with the same white outline as a pending STUTTER.
inline void PendingOutline(ImVec2 p0, ImVec2 p1, float rounding = 3.0f) {
    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 10.0f);
    ImGui::GetWindowDrawList()->AddRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), IM_COL32(255, 255, 255, static_cast<int>(90 + 140 * pulse)), rounding, 0, 2.0f);
}

// A dashed outline (an instrument that is off because another one is soloed).
inline void DashedRect(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 c) {
    for (float y = p0.y; y < p1.y; y += 6.0f) { dl->AddLine(ImVec2(p0.x, y), ImVec2(p0.x, std::min(y + 3.0f, p1.y)), c); dl->AddLine(ImVec2(p1.x, y), ImVec2(p1.x, std::min(y + 3.0f, p1.y)), c); }
    for (float x = p0.x; x < p1.x; x += 6.0f) { dl->AddLine(ImVec2(x, p0.y), ImVec2(std::min(x + 3.0f, p1.x), p0.y), c); dl->AddLine(ImVec2(x, p1.y), ImVec2(std::min(x + 3.0f, p1.x), p1.y), c); }
}

// One instrument bar. State: not `has` = an empty slot (outline), `silenced` = off because another instrument is soloed (dim, dashed),
// `muted` = off by hand (dim), otherwise lit in the colour of the instrument.
inline void DrawBar(ImDrawList* dl, int t, ImVec2 p0, ImVec2 p1, bool has, bool muted, bool silenced) {
    const ImU32 tc = TrackColor(t);
    if (!has) {
        dl->AddRect(p0, p1, IM_COL32(60, 64, 82, 255), 3.0f);
        DrawInstrumentGlyph(dl, t, ImVec2((p0.x + p1.x) * 0.5f, p0.y + 11.0f), 14.0f, IM_COL32(80, 84, 100, 255));
        return;
    }
    if (silenced || muted) {
        dl->AddRectFilled(p0, p1, ColorAlpha(tc, silenced ? 26 : 38), 3.0f);
        if (silenced) DashedRect(dl, p0, p1, ColorAlpha(tc, 110));   // switched off by a solo, not by hand
        else dl->AddRect(p0, p1, ColorAlpha(tc, 100), 3.0f);
        DrawInstrumentGlyph(dl, t, ImVec2((p0.x + p1.x) * 0.5f, p0.y + 11.0f), 14.0f, ColorAlpha(tc, 170));
        return;
    }
    dl->AddRectFilled(p0, p1, ColorAlpha(tc, 235), 3.0f);
    DrawInstrumentGlyph(dl, t, ImVec2((p0.x + p1.x) * 0.5f, p0.y + 11.0f), 14.0f, IM_COL32(18, 20, 26, 235));
}

inline const char* StateWord(bool has, bool muted, bool silenced, bool solo) {
    if (!has) return "nothing to play here";
    if (solo) return "SOLO";
    if (silenced) return "off (another instrument is soloed)";
    return muted ? "OFF" : "ON";
}

// The six group buttons in two columns of three, top left at `origin`. Returns the size they take.
inline ImVec2 DrawGroups(AppState& app, int brick, ImVec2 origin, float bw, float bh, float gap, float fontSize = 11.0f) {
    struct G { const char* label; uint16_t bits; const char* tip; };
    static const G kGroups[6] = {
        {"ALL DRUMS", kMuteDrums, "ALL DRUMS: kick, snare, clap, both hats and perc off or on together."},
        {"DRUMS", static_cast<uint16_t>((1u << 1) | (1u << 2) | (1u << 5)), "DRUMS: snare + clap + perc off or on together (the kick and the hats keep going)."},
        {"HATS", static_cast<uint16_t>((1u << 3) | (1u << 4)), "HATS: closed + open hat off or on together."},
        {"DROP", 0, "DROP: everything off except kick and bass. Press again to bring the others back as they were."},
        {"MUTE ALL", kMuteAllTracks, "MUTE ALL: every instrument off. Press again to bring them all back."},
        {"DROP+CHORD", 0, "DROP+CHORD: everything off except kick, bass and chords. Press again to bring the others back as they were.\n(Every mute and solo off in one press: the M and S buttons, or press MUTE ALL twice.)"},
    };
    ImGui::PushFont(nullptr, fontSize);   // the longest word, ALL DRUMS, has to fit in the button
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.0f, 2.0f));
    for (int i = 0; i < 6; ++i) {
        const int col = i % 2, row = i / 2;
        ImGui::SetCursorScreenPos(ImVec2(origin.x + col * (bw + gap), origin.y + row * (bh + gap)));
        ImGui::PushID(i);
        const G& g = kGroups[i];
        bool hit = false;
        if (i == 3) hit = ToggleButton(g.label, app.SwDropOn(brick), ImVec2(bw, bh));
        else if (i == 5) {   // the longest label: a little smaller so it fits the same button
            ImGui::PushFont(nullptr, fontSize * 0.82f);
            hit = ToggleButton(g.label, app.SwKeepOn(brick, app.kKeepKickBassChord), ImVec2(bw, bh));
            ImGui::PopFont();
        }
        else hit = ToggleButton(g.label, app.SwGroupMuted(brick, g.bits), ImVec2(bw, bh), ToggleRed());
        using QS = AppState::QueuedSwitch;
        const QS::Kind qk = i == 3 ? QS::Drop : (i == 5 ? QS::Keep : QS::Group);
        const uint16_t qa = i == 5 ? app.kKeepKickBassChord : (i == 3 ? static_cast<uint16_t>(0) : g.bits);
        if (brick < 0 && app.SwQueued(qk, qa)) PendingOutline(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), 4.0f);   // v44.54: waiting for the next bar
        if (hit) {
            if (brick < 0) app.LiveSwitch(qk, qa);   // v44.54: NEXT BAR may queue it; WHO PLAYS (brick >= 0) is always immediate
            else if (i == 3) app.SwToggleDrop(brick);
            else if (i == 5) app.SwToggleKeep(brick, app.kKeepKickBassChord);
            else app.SwToggleGroup(brick, g.bits);
        }
        if (ImGui::IsItemHovered()) Hint("%s", g.tip);
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    ImGui::PopFont();
    return ImVec2(2 * bw + gap, 3 * bh + 2 * gap);
}

} // namespace t8::desktop::instrument_switches
