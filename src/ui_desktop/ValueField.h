#pragma once
// ValueField -- the ONE number control of the program (v44.15, U1a). Replaces ImGui::DragInt / DragFloat / SliderInt /
// SliderFloat / the -/+ button pairs, so BPM, LEN, SWING, REPEATS, and later every value in the mixer, FX and synth panels
// behave the same (see Interaction.h for the contract). It looks like a quiet field with a thin position bar along the bottom.
//
//   ValueSpec sp; sp.lo = 20; sp.hi = 300; sp.step = 0.5f; sp.def = 120;
//   ValueResult r = ValueField("##bpm", &bpm, sp, 64);
//   if (r.started) app.PushUndo();     // once per gesture, never per frame
//   if (r.changed) pat.bpm = bpm;
//   if (r.cancelled) app.DropLastUndo();
//
// started   : the first frame a gesture really changes the value (a plain click does not start anything)
// changed   : the value is different this frame
// finished  : the gesture ended (mouse released, Enter, wheel notch, reset) -- the moment to apply a "pending" change
// cancelled : Esc during a drag that had already started; the value is back to what it was (changed is set too) -- the
//             caller takes back the undo step that `started` pushed (AppState::DropLastUndo)
//             (Esc while typing just closes the field: nothing started, nothing to take back)
#include "imgui.h"
#include "Interaction.h"
#include "InfoLine.h"
#include "LiveWidgets.h"
#include "Visual.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace t8::desktop {

// C3 (review of #54): the PITCH fader's notch at 0 % stays as narrow as the old fader's (+-0.2 %), so +0.3 % can be set.
inline constexpr float kPitchNotchPct = 0.2f;

struct ValueSpec {
    float lo = 0.0f, hi = 1.0f;
    float step = 0.01f;         // value change per pixel of drag
    float wheel = 0.0f;         // value change per wheel notch (0 = same as 10 px of drag, at least 1 for integers)
    float def = 0.0f;           // Option+click
    bool integer = false;       // rounds to whole numbers
    const char* fmt = "%.0f";   // one float argument
    const char* unit = "";      // appended: "%", " s", "x"
    const char* offText = nullptr;   // shown instead of the number at lo ("off")
    const char* text = nullptr;      // shown instead of the number, whatever the value ("trim"); drag and typing still work
    float scale = 1.0f;         // shown / typed value = value * scale (a 0..1 volume typed as percent: scale 100)
    // A field that drags a 0..1 position but means a unit (Hz on a log scale, ms on a square-root one): typing goes through these
    // instead of `scale` (toTyped: value -> the number shown when typing starts; fromTyped: the typed number -> value).
    float (*toTyped)(float) = nullptr;
    float (*fromTyped)(float) = nullptr;
    // Bar look (faders, pan): a track with a thumb instead of a text field; the number is drawn by the caller or typed.
    bool bar = false;
    bool fromCenter = false;    // bar: fills from the middle, snaps to the middle (pan)
    float notch = -1.0f;        // fromCenter: the half-width of the snap to the middle, in value units (< 0 = 3 % of the range)
    float meter = -1.0f;        // bar: level meter along the bottom edge (0..1.3), < 0 = none
    ImU32 color = vis::kAccent; // bar: fill colour (the track's colour)
    float height = 0.0f;        // 0 = the frame height
    // Box look (the MASTER FX knobs, C3): a square that fills from the bottom; drag up / down; the number is drawn by the caller.
    bool box = false;
};

// A box knob of `size` (C3): the whole height is the whole range, like the old MASTER FX knobs.
inline ValueSpec BoxSpec(float lo, float hi, float def, float height) {
    ValueSpec s; s.lo = lo; s.hi = hi; s.def = def; s.box = true; s.height = height;
    s.step = height > 1.0f ? (hi - lo) / height : 0.01f;
    s.wheel = (hi - lo) / 50.0f;
    return s;
}

// A fader: drag = 1:1 along the bar, so the whole width is the whole range.
inline ValueSpec BarSpec(float lo, float hi, float def, float width) {
    ValueSpec s; s.lo = lo; s.hi = hi; s.def = def; s.bar = true;
    s.step = width > 1.0f ? (hi - lo) / width : 0.01f;
    s.wheel = (hi - lo) / 50.0f;
    return s;
}
// A level in dB (brick dynamics); the lowest value reads "off".
inline ValueSpec DbSpec(int lo, int hi) {
    ValueSpec s; s.lo = static_cast<float>(lo); s.hi = static_cast<float>(hi); s.step = 0.25f; s.wheel = 1.0f; s.def = 0.0f;
    s.integer = true; s.unit = " dB"; s.offText = "off";
    return s;
}

struct ValueResult {
    bool started = false, changed = false, finished = false, cancelled = false;
};

namespace valuefield_detail {
struct Edit { ImGuiID id = 0; char buf[32] = {0}; bool focus = false; };
inline Edit& E() { static Edit e; return e; }
inline float Snap(float v, const ValueSpec& s) {
    v = std::clamp(v, s.lo, s.hi);
    if (s.fromCenter) { const float mid = (s.lo + s.hi) * 0.5f; if (std::fabs(v - mid) < (s.notch >= 0.0f ? s.notch : (s.hi - s.lo) * 0.03f)) v = mid; }   // a notch at the middle
    return s.integer ? std::round(v) : v;
}
// The pure rules (no ImGui), shared with the self-test.
inline float DragValue(float startValue, float accumPx, bool fine, const ValueSpec& s) {
    return Snap(startValue + accumPx * s.step * (fine ? interaction::kFineFactor : 1.0f), s);
}
inline float WheelValue(float value, int notches, bool fine, const ValueSpec& s) {
    float w = s.wheel > 0.0f ? s.wheel : std::max(s.integer ? 1.0f : 0.0f, 10.0f * s.step);
    if (fine && !s.integer) w *= interaction::kFineFactor;
    return Snap(value + notches * w, s);
}
inline bool ParseTyped(const char* text, const ValueSpec& s, float* out) {
    char* end = nullptr;
    const float typed = std::strtof(text, &end);
    const float v = s.fromTyped ? s.fromTyped(typed) : typed / (s.scale != 0.0f ? s.scale : 1.0f);
    if (end == text) return false;
    while (*end == ' ' || *end == '%') ++end;   // "50%" and "50 " are fine
    if (*end != '\0' && s.unit[0] == '\0') return false;
    *out = Snap(v, s);
    return true;
}
} // namespace valuefield_detail

inline ValueResult ValueField(const char* id, float* value, const ValueSpec& sp, float width, bool enabled = true) {
    using namespace valuefield_detail;
    ValueResult r;
    ImGui::PushID(id);
    const ImGuiID wid = ImGui::GetID("##vf");
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float h = sp.height > 0.0f ? sp.height : ImGui::GetFrameHeight();
    const ImVec2 p1(p0.x + width, p0.y + h);
    ImGuiIO& io = ImGui::GetIO();
    Edit& ed = E();

    if (ed.id == wid) {   // ---- typing -------------------------------------------------------------------------------
        ImGui::SetNextItemWidth(width);
        if (ed.focus) { ImGui::SetKeyboardFocusHere(); ed.focus = false; }
        ImGui::InputText("##typed", ed.buf, sizeof ed.buf,
                         ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue);
        const bool esc = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (ImGui::IsItemDeactivated() || esc) {
            float v = *value;
            if (!esc && ParseTyped(ed.buf, sp, &v) && v != *value) { r.started = r.changed = true; *value = v; }
            r.finished = true;
            ed.id = 0;
        }
        ImGui::PopID();
        return r;
    }

    ImGui::InvisibleButton("##vf", ImVec2(width, h));
    const bool hovered = enabled && ImGui::IsItemHovered();
    const bool active = enabled && ImGui::IsItemActive();
    const ImGuiID kStart = ImGui::GetID("start"), kAcc = ImGui::GetID("acc"), kMoved = ImGui::GetID("moved"),
                  kSkip = ImGui::GetID("skip"), kBegan = ImGui::GetID("began");

    if (enabled) {
        if (ImGui::IsItemActivated()) {
            st->SetFloat(kStart, *value); st->SetFloat(kAcc, 0.0f); st->SetInt(kMoved, 0); st->SetInt(kSkip, 0); st->SetInt(kBegan, 0);
            if (interaction::IsReset()) {   // Option+click: back to the default, and the press does not start a drag
                st->SetInt(kSkip, 1);
                const float d = Snap(sp.def, sp);
                if (d != *value) { r.started = r.changed = true; *value = d; }
                r.finished = true;
            }
        }
        if (active && !st->GetInt(kSkip, 0)) {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {   // Esc: put it back and ignore the rest of this press
                const float s0 = st->GetFloat(kStart, *value);
                if (st->GetInt(kMoved, 0) && s0 != *value) { *value = s0; r.changed = true; }
                r.cancelled = st->GetInt(kBegan, 0) != 0;   // only then did `started` already push an undo step
                r.finished = true;
                st->SetInt(kSkip, 1);
            } else {
                float acc = st->GetFloat(kAcc, 0.0f) + (sp.bar ? io.MouseDelta.x : sp.box ? -io.MouseDelta.y : io.MouseDelta.x - io.MouseDelta.y);
                st->SetFloat(kAcc, acc);
                const ImVec2 dd = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
                if (st->GetInt(kMoved, 0) || std::sqrt(dd.x * dd.x + dd.y * dd.y) >= interaction::kDragThresholdPx) {
                    st->SetInt(kMoved, 1);
                    const float v = DragValue(st->GetFloat(kStart, *value), acc, interaction::IsFine(), sp);
                    if (v != *value) { r.started = !st->GetInt(kBegan, 0); st->SetInt(kBegan, 1); r.changed = true; *value = v; }
                }
            }
        }
        if (ImGui::IsItemDeactivated() && st->GetInt(kMoved, 0) && !r.finished) r.finished = true;
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !interaction::IsReset()) {
            ed.id = wid; ed.focus = true;
            std::snprintf(ed.buf, sizeof ed.buf, sp.fmt, static_cast<double>(sp.toTyped ? sp.toTyped(*value) : *value * sp.scale));
        }
        if (hovered && !active && io.MouseWheel != 0.0f && !interaction::IsCmd()) {   // one notch = one step
            ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
            const int n = io.MouseWheel > 0 ? 1 : -1;
            const float v = WheelValue(*value, n, interaction::IsFine(), sp);
            if (v != *value) { r.started = r.changed = r.finished = true; *value = v; }
        }
        if (hovered) interaction::NoteValueHover(wid);   // C3: the mouse over a value + Up / Down = one step (Shift = fine), like a wheel notch
        if (hovered && !active) {
            if (const int n = interaction::ArrowSteps(wid)) {
                const float v = WheelValue(*value, n, interaction::IsFine(), sp);
                if (v != *value) { r.started = r.changed = r.finished = true; *value = v; }
            }
        }
        if (hovered || active) {
            interaction::ValueCursor();
            HintGestures("Drag: change   Shift: finer   Wheel or Up / Down: step   Double-click: type   Option+click: default");
        }
    }

    // ---- drawing ------------------------------------------------------------------------------------------------
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (sp.bar) {
        const float trackH = 8.0f, cy = (p0.y + p1.y) * 0.5f;
        const ImVec2 t0(p0.x, cy - trackH * 0.5f), t1(p1.x, cy + trackH * 0.5f);
        dl->AddRectFilled(t0, t1, enabled ? vis::kCell : vis::kInset, 3.0f);
        const float norm = sp.hi > sp.lo ? std::clamp((*value - sp.lo) / (sp.hi - sp.lo), 0.0f, 1.0f) : 0.0f;
        const float xv = p0.x + width * norm;
        const ImU32 col = !enabled ? vis::kLineHi : (active || hovered) ? vis::kAccentHi : sp.color;
        if (sp.fromCenter) {
            const float xc = (p0.x + p1.x) * 0.5f;
            dl->AddRectFilled(ImVec2(std::min(xc, xv), t0.y), ImVec2(std::max(xc, xv), t1.y), col, 3.0f);
            dl->AddLine(ImVec2(xc, t0.y - 3), ImVec2(xc, t1.y + 3), IM_COL32(120, 126, 150, 255), 1.0f);
        } else {
            dl->AddRectFilled(t0, ImVec2(xv, t1.y), col, 3.0f);
        }
        dl->AddRect(t0, t1, vis::kLineHi, 3.0f);
        dl->AddRectFilled(ImVec2(xv - 3, cy - 8), ImVec2(xv + 3, cy + 8), enabled ? vis::kText : vis::kTextDim, 2.0f);
        if (sp.meter >= 0.0f) {
            const float m = std::clamp(sp.meter / 1.3f, 0.0f, 1.0f);
            const ImU32 mc = sp.meter > 1.0f ? vis::kRed : sp.meter > 0.75f ? vis::kAccent : vis::kTeal;
            const float my = t1.y + 3.0f;
            dl->AddRectFilled(ImVec2(p0.x, my), ImVec2(p1.x, my + 2.0f), vis::kInset);
            dl->AddRectFilled(ImVec2(p0.x, my), ImVec2(p0.x + width * m, my + 2.0f), mc);
        }
        ImGui::PopID();
        return r;
    }
    if (sp.box) {
        const float norm = sp.hi > sp.lo ? std::clamp((*value - sp.lo) / (sp.hi - sp.lo), 0.0f, 1.0f) : 0.0f;
        dl->AddRectFilled(p0, p1, enabled ? (hovered && !active ? vis::kCellHover : vis::kCell) : vis::kInset, 4.0f);
        const float fh = (p1.y - p0.y) * norm;
        if (fh > 0.5f) dl->AddRectFilled(ImVec2(p0.x, p1.y - fh), p1, !enabled ? vis::kLineHi : active ? vis::kAccentHi : vis::kAccent, 4.0f);
        dl->AddRect(p0, p1, hovered || active ? vis::kAccent : vis::kLineHi, 4.0f);
        ImGui::PopID();
        return r;
    }
    const ImU32 bg = !enabled ? vis::kInset : (active || hovered) ? vis::kCellHover : vis::kCell;
    dl->AddRectFilled(p0, p1, bg, 4.0f);
    if (sp.hi > sp.lo) {
        const float n = std::clamp((*value - sp.lo) / (sp.hi - sp.lo), 0.0f, 1.0f);
        dl->AddRectFilled(ImVec2(p0.x + 3, p1.y - 3), ImVec2(p0.x + 3 + (width - 6) * n, p1.y - 1),
                          enabled ? (active ? vis::kAccentHi : vis::kAccent) : vis::kLineHi, 1.0f);
    }
    dl->AddRect(p0, p1, active ? vis::kAccent : hovered ? vis::kLineHi : vis::kLine, 4.0f);
    char txt[48];
    if (sp.text) std::snprintf(txt, sizeof txt, "%s", sp.text);
    else if (sp.offText && *value <= sp.lo) std::snprintf(txt, sizeof txt, "%s", sp.offText);
    else { char num[32]; std::snprintf(num, sizeof num, sp.fmt, static_cast<double>(*value * sp.scale)); std::snprintf(txt, sizeof txt, "%s%s", num, sp.unit); }
    const ImVec2 ts = ImGui::CalcTextSize(txt);
    dl->AddText(ImVec2(p0.x + (width - ts.x) * 0.5f, p0.y + (h - 3 - ts.y) * 0.5f + 1), enabled ? vis::kText : vis::kTextDim, txt);
    ImGui::PopID();
    return r;
}

// ---- Segmented: a few exclusive choices side by side (replaces a 2-4 item combo and radio buttons) ---------------------
inline bool Segmented(const char* id, int* index, const char* const* labels, int n, float segWidth = 0.0f) {
    bool changed = false;
    ImGui::PushID(id);
    ImGui::BeginGroup();   // one item for the whole row: IsItemHovered() / Hint() after the call cover every segment
    for (int i = 0; i < n; ++i) {
        if (i) ImGui::SameLine(0, 2);
        if (ToggleButton(labels[i], *index == i, ImVec2(segWidth, 0), ToggleTab())) { if (*index != i) { *index = i; changed = true; } }
    }
    ImGui::EndGroup();
    ImGui::PopID();
    return changed;
}

// ---- Check: an on/off option (replaces ImGui::Checkbox): a button that stays lit while it is on ---------------------------
inline bool Check(const char* label, bool* on, float width = 0.0f) {
    if (ToggleButton(label, *on, ImVec2(width, 0), ToggleTab())) { *on = !*on; return true; }
    return false;
}

// ---- Choice: one pick from a longer list -- the one combo look (width, hint and item order are the caller's) --------
// ChoiceBegin / ChoiceEnd are the only places in ui_desktop that touch ImGui::BeginCombo (a test counts them), so every list of
// the program opens the same way. Use Choice for a plain array; ChoiceBegin + Selectable + ChoiceEnd for items built on the fly
// (groups, separators, names from a folder). Up to four short exclusive options are a Segmented, not a Choice.
inline bool ChoiceBegin(const char* id, const char* preview, ImGuiComboFlags flags = 0) { return ImGui::BeginCombo(id, preview, flags); }
inline void ChoiceEnd() { ImGui::EndCombo(); }
inline bool Choice(const char* id, int* index, const char* const* items, int n, float width) {
    bool changed = false;
    ImGui::SetNextItemWidth(width);
    const int cur = std::clamp(*index, 0, n - 1);
    if (ChoiceBegin(id, items[cur])) {
        for (int i = 0; i < n; ++i)
            if (ImGui::Selectable(items[i], i == cur)) { if (i != *index) { *index = i; changed = true; } }
        ChoiceEnd();
    }
    return changed;
}

// ---- MiniButton: the small action button (replaces ImGui::SmallButton). Every target is at least kMinTarget high. -------
constexpr float kMinTarget = 21.0f;
inline bool MiniButton(const char* label, float width = 0.0f) { return ImGui::Button(label, ImVec2(width, kMinTarget)); }

} // namespace t8::desktop
