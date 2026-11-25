#pragma once
// Shared "right-click a step" detail popup -- per-step detail: ratchet, probability, accent,
// variant, preview. One popup body, called from both SequencerPanel.cpp (every
// track type) and PianoRollPanel.cpp (Synth/Chord notes also get LENGTH/
// SLIDE here, which only make sense for a pitched note, not a drum hit).
// Same mutation shape as every other panel in this UI: PushUndo() once per
// actual change, then a direct field write under the caller's existing
// PatternMutex lock -- there's no separate "live" copy of Step fields to
// dual-write (Sequencer::FireTick reads Pattern::tracks[].steps[] directly).
#include "app/AppState.h"
#include "imgui.h"
#include "ValueField.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace t8::desktop {

namespace step_popup_detail {
constexpr uint8_t kProbSteps[5] = {100, 75, 50, 25, 0};
inline int ProbIndex(uint8_t p) {
    for (int i = 0; i < 5; ++i) if (kProbSteps[i] == p) return i;
    return 0;
}
} // namespace step_popup_detail

// Opens (if `open` is true, meaning the caller just right-clicked this
// exact cell) and draws the popup; call every frame regardless of `open`
// so ImGui's own popup-open state machine (BeginPopup returning false once
// dismissed) works normally. Returns nothing -- all effects are direct
// AppState/Pattern mutations.
// rowNote >= 0: the piano-roll pitch that was right-clicked (for a chord step the octave shift then moves only that note).
inline void DrawStepEditPopup(const char* popupId, bool open, AppState& app, int trackIndex, int stepIndex, int rowNote = -1) {
    if (open) ImGui::OpenPopup(popupId);
    if (!ImGui::BeginPopup(popupId)) return;

    Track& t = app.StepTrack(app.CurrentPattern(), trackIndex);
    Step& s = t.steps[stepIndex];

    char header[48];
    std::snprintf(header, sizeof(header), "%s - STEP %d", t.name.c_str(), stepIndex + 1);
    ImGui::TextDisabled("%s", header);
    ImGui::Separator();

    if (!s.active) {
        ImGui::TextDisabled("(step is off -- click it to activate)");
        ImGui::EndPopup();
        return;
    }

    // OCTAVE -- melodic steps only: move the note (a chord step: the clicked note, else the whole chord) up / down 12 semitones.
    if (t.type != TrackType::Drum) {
        auto shift = [&](int semis) {
            auto fits = [&](int n) { return n + semis >= 0 && n + semis <= 127; };
            if (IsNoteTrackType(t.type)) {
                if (!fits(s.note)) return;
                app.PushUndo();
                s.note = static_cast<int8_t>(s.note + semis);
                return;
            }
            const int cnt = std::clamp(static_cast<int>(s.chordSize), 0, kMaxChordVoices);
            int only = -1;
            if (rowNote >= 0) for (int i = 0; i < cnt; ++i) if (s.chordNotes[i] == rowNote) only = i;
            for (int i = 0; i < cnt; ++i) if ((only < 0 || i == only) && !fits(s.chordNotes[i])) return;
            app.PushUndo();
            for (int i = 0; i < cnt; ++i) if (only < 0 || i == only) s.chordNotes[i] = static_cast<int8_t>(s.chordNotes[i] + semis);
            std::sort(s.chordNotes.begin(), s.chordNotes.begin() + cnt);
        };
        ImGui::TextDisabled("OCTAVE");
        ImGui::SameLine();
        if (ImGui::Button("UP  +12")) shift(12);
        ImGui::SameLine();
        if (ImGui::Button("DOWN  -12")) shift(-12);
    }

    // ACCENT -- single on/off.
    bool accent = StepIsAccent(s);
    if (Check("ACCENT", &accent) && accent != StepIsAccent(s)) {
        app.PushUndo();
        s.vel = 0;
        s.accent = accent;
        if (accent) s.ghost = false;
    }

    // GHOST -- a quiet in-between hit (mutually exclusive with ACCENT).
    ImGui::SameLine();
    bool ghost = StepIsGhost(s);
    if (Check("GHOST", &ghost) && ghost != StepIsGhost(s)) {
        app.PushUndo();
        s.vel = 0;
        s.ghost = ghost;
        if (ghost) s.accent = false;
    }
    if (s.ghost && s.accent) s.accent = false;

    // VELOCITY -- the exact value (the lanes of the editor change it for many steps at once)
    {
        ImGui::TextDisabled("VELOCITY");
        int v = std::clamp(static_cast<int>(std::lround(StepVelocity(s) * 127.0f)), 1, 127);
        {
            float vf = static_cast<float>(v);
            ValueSpec sp; sp.lo = 1.0f; sp.hi = 127.0f; sp.step = 0.5f; sp.wheel = 1.0f; sp.def = 100.0f; sp.integer = true;
            const ValueResult r = ValueField("##vel", &vf, sp, 170.0f);
            if (r.started) app.PushUndo();
            if (r.changed) { s.vel = static_cast<uint8_t>(vf); s.accent = false; s.ghost = false; }
            if (r.cancelled) app.DropLastUndo();
        }
        ImGui::SameLine();
        if (MiniButton("AUTO") && s.vel != 0) { app.PushUndo(); s.vel = 0; }
    }

    // NUDGE -- microtiming in percent of a step. Drum hits can also be pulled ahead of the grid;
    // synth and chord notes can only be laid back (the sequencer clamps them to >= 0).
    {
        ImGui::TextDisabled("NUDGE (%% of a step)");
        int nudge = s.nudge;
        const int lo = t.type == TrackType::Drum ? -50 : 0;
        {
            float nf = static_cast<float>(nudge);
            ValueSpec sp; sp.lo = static_cast<float>(lo); sp.hi = 50.0f; sp.step = 0.4f; sp.wheel = 1.0f; sp.def = 0.0f; sp.integer = true; sp.fmt = "%+.0f"; sp.unit = "%";
            const ValueResult r = ValueField("##nudge", &nf, sp, 170.0f);
            if (r.started) app.PushUndo();
            if (r.changed) s.nudge = static_cast<int8_t>(std::clamp(static_cast<int>(nf), lo, 50));
            if (r.cancelled) app.DropLastUndo();
        }
        ImGui::SameLine();
        if (MiniButton("0") && s.nudge != 0) { app.PushUndo(); s.nudge = 0; }
    }

    // RATCHET -- 1 (off) .. 4 evenly-spaced retriggers, same range/meaning
    // (ratchet).
    ImGui::TextDisabled("RATCHET");
    for (int r = 1; r <= 4; ++r) {
        if (r > 1) ImGui::SameLine();
        ImGui::PushID(r);
        bool sel = (s.ratchet == r);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.95f, 0.64f, 0.24f, 1.0f));
        char buf[8];
        std::snprintf(buf, sizeof(buf), "x%d", r);
        if (ImGui::Button(buf, ImVec2(36, 0)) && s.ratchet != r) {
            app.PushUndo();
            s.ratchet = static_cast<uint8_t>(r);
        }
        if (sel) ImGui::PopStyleColor();
        ImGui::PopID();
    }

    // PROBABILITY -- fixed 5-value ladder, same as X+L2+R2+Left/Right.
    ImGui::TextDisabled("PROBABILITY");
    for (int i = 0; i < 5; ++i) {
        if (i > 0) ImGui::SameLine();
        ImGui::PushID(i + 100);
        bool sel = (s.probability == step_popup_detail::kProbSteps[i]);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.95f, 0.64f, 0.24f, 1.0f));
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d%%", step_popup_detail::kProbSteps[i]);
        if (ImGui::Button(buf, ImVec2(44, 0)) && !sel) {
            app.PushUndo();
            s.probability = step_popup_detail::kProbSteps[i];
        }
        if (sel) ImGui::PopStyleColor();
        ImGui::PopID();
    }

    if (t.type == TrackType::Drum) {
        // VARIANT -- which alt sample take (kick_2.wav, ...) this hit
        // plays. Only meaningful if the kit
        // actually loaded more than one take for this slot.
        int count = app.SceneKitVariantCount(app.project.currentPattern, t.slotIndex);
        if (count > 1) {
            ImGui::TextDisabled("VARIANT");
            for (int v = 0; v < count; ++v) {
                if (v > 0) ImGui::SameLine();
                ImGui::PushID(v + 200);
                bool sel = (StepVariant(t, s) == v);
                if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.95f, 0.64f, 0.24f, 1.0f));
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%d", v + 1);
                if (ImGui::Button(buf, ImVec2(28, 0)) && !sel) {
                    app.PushUndo();
                    s.variant = static_cast<uint8_t>(v);
                }
                if (sel) ImGui::PopStyleColor();
                ImGui::PopID();
            }
        }
    } else {
        // Synth/Chord note: LENGTH (tied/held notes) and SLIDE (303-style
        // glide from the previous note) -- meaningless for a drum hit, so
        // only shown for pitched tracks.
        ImGui::TextDisabled("NOTE LENGTH (steps)");
        int trackLen = t.useOwnLength ? t.length : app.CurrentPattern().masterLength;
        int maxLen = std::clamp(trackLen - stepIndex, 1, kMaxSteps);
        int len = std::clamp(static_cast<int>(s.lengthSteps), 1, maxLen);
        // 1..8 one by one, then the long holds a chord pad wants (12/16/24/32).
        static const int kChoices[] = {1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 24, 32};
        int shown = 0;
        for (int l : kChoices) {
            if (l > maxLen) break;
            if (shown > 0 && shown != 8) ImGui::SameLine();
            ++shown;
            ImGui::PushID(l + 300);
            bool sel = (len == l);
            if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.95f, 0.64f, 0.24f, 1.0f));
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d", l);
            if (ImGui::Button(buf, ImVec2(l >= 10 ? 34 : 28, 0)) && s.lengthSteps != l) {
                app.PushUndo();
                s.lengthSteps = static_cast<uint8_t>(l);
            }
            if (sel) ImGui::PopStyleColor();
            ImGui::PopID();
        }
        bool slide = s.slide;
        if (Check("TIE / SLIDE  (legato: no retrigger from previous note)", &slide) && slide != s.slide) {
            app.PushUndo();
            s.slide = slide;
        }
    }

    ImGui::EndPopup();
}

} // namespace t8::desktop
