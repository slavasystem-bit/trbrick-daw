#pragma once
// Chord editor: build / change the chords of the edited chord scene (or of the scene's own chord track when
// the bank is off) by hand -- root, quality, inversion, length, order -- without going through a progression.
// Opened from the CHAIN panel (double-click or right-click a chord slot) and from CHORDS > CUSTOM CHORDS.
#include "core/ChordLab.h"
#include "app/AppState.h"
#include "core/ChordSpec.h"
#include "core/Chords.h"
#include "core/Scales.h"
#include "Visual.h"
#include "ValueField.h"
#include "imgui.h"
#include "InfoLine.h"
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace t8::desktop {

struct ChordEditorState { bool open = false; int sel = 0; bool sevenths = false; };
inline ChordEditorState& ChordEditor() { static ChordEditorState s; return s; }

inline void OpenChordEditor(AppState& app, int scene) {
    if (scene >= 0) {
        if (!app.project.chordBankOn) app.SetChordBank(true);
        app.EditChordSceneOnly(scene);
    }
    ChordEditorState& st = ChordEditor();
    st.open = true; st.sel = 0;
}

namespace chordeditor_detail {
inline std::string NoteLabel(int n) {
    return std::string(kKeyNames[((n % 12) + 12) % 12]) + std::to_string(n / 12 - 1);
}
inline std::string ChordLabel(const Step& s) {
    ChordSpec sp;
    if (IdentifyChord(s.chordNotes.data(), s.chordSize, sp)) {
        std::string n = std::string(LabPcName(sp.root)) + ChordQuality(sp.quality).suffix;
        return n;
    }
    return AppState::ChordNameFromNotes(s);
}
}

inline void DrawChordEditor(AppState& app) {
    using namespace chordeditor_detail;
    ChordEditorState& st = ChordEditor();
    if (!st.open) return;
    SceneLock lock(app.sequencer.PatternMutex(), "CHORD EDITOR");
    LabSetSpelling(app.project.randomizerKeyRoot, app.project.randomizerScale);   // v44.46: the same chord is spelled the same way everywhere
    Pattern& pat = app.CurrentPattern();
    Track& tr = app.StepTrack(pat, kChordTrackIndex);
    Project& proj = app.project;
    const bool bank = proj.chordBankOn;
    const int len = std::clamp(tr.useOwnLength ? static_cast<int>(tr.length) : pat.masterLength, 1, kMaxSteps);
    st.sel = std::clamp(st.sel, 0, len - 1);
    const int key = std::clamp(proj.randomizerKeyRoot, 0, kNumKeys - 1), scale = std::clamp(proj.randomizerScale, 0, kNumScales - 1);

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(std::min(780.0f, io.DisplaySize.x - 30.0f), std::min(520.0f, io.DisplaySize.y - 30.0f)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.45f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(520, 360), ImVec2(4000, 3000));
    bool open = st.open;
    if (!ImGui::Begin("CHORD EDITOR", &open, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); st.open = open; return; }

    // --- which chord track is being edited ----------------------------------
    if (bank) ImGui::TextColored(ImVec4(0.72f, 0.55f, 1.0f, 1.0f), "CHORD SCENE %d  %s", proj.chordSceneEdit + 1, app.ChordSceneLabel(proj.chordSceneEdit).c_str());
    else ImGui::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f), "SCENE %d CHORDS (chord bank is off)", proj.currentPattern + 1);
    ImGui::SameLine(0, 16);
    ImGui::TextDisabled("key %s %s   %d steps", kKeyNames[key], kScaleNames[scale], len);
    if (bank) {
        ImGui::SameLine(0, 16);
        for (int c = 0; c < kNumChordScenes; ++c) {
            ImGui::SameLine(0, 3);
            char b[8]; std::snprintf(b, sizeof b, "%d", c + 1);
            const bool cur = c == proj.chordSceneEdit;
            if (cur) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.22f, 0.46f, 1.0f));
            if (MiniButton(b)) { app.EditChordSceneOnly(c); st.sel = 0; }
            if (cur) ImGui::PopStyleColor();
        }
    }

    auto coverOf = [&](int i) {
        int c = -1;
        for (int j = 0; j <= i && j < len; ++j) {
            const Step& s = tr.steps[j];
            if (s.active && s.chordSize > 0 && i < j + std::max<int>(1, s.lengthSteps)) c = j;
        }
        return c;
    };
    auto nextStart = [&](int after) {
        for (int j = after + 1; j < len; ++j) if (tr.steps[j].active && tr.steps[j].chordSize > 0) return j;
        return len;
    };
    auto countChords = [&]() { int n = 0; for (int j = 0; j < len; ++j) if (tr.steps[j].active && tr.steps[j].chordSize > 0) ++n; return n; };
    auto markKey = [&]() {
        if (bank) { ChordScene& cs = app.EditChordScene(); if (cs.key < 0) { cs.key = static_cast<int8_t>(key); cs.scale = static_cast<int8_t>(scale); } }
        else if (pat.chordsKey < 0) { pat.chordsKey = static_cast<int8_t>(key); pat.chordsScale = static_cast<int8_t>(scale); }
    };
    auto writeChord = [&](int start, const std::vector<int>& notes, int lenHint) {
        app.PushUndo();
        const int prev = coverOf(start);
        const bool had = prev == start;
        if (prev >= 0 && prev != start) tr.steps[prev].lengthSteps = static_cast<uint8_t>(start - prev);   // the earlier chord ends where this starts
        Step& s = tr.steps[start];
        if (!had) s = Step{};
        s.active = true;
        s.chordSize = static_cast<uint8_t>(std::min<size_t>(notes.size(), kMaxChordVoices));
        for (int i = 0; i < kMaxChordVoices; ++i) s.chordNotes[i] = i < s.chordSize ? static_cast<int8_t>(notes[i]) : static_cast<int8_t>(0);
        if (!had) {
            int room = nextStart(start) - start;
            int want = lenHint > 0 ? lenHint : (countChords() <= 1 ? len : room);
            s.lengthSteps = static_cast<uint8_t>(std::clamp(want, 1, std::max(1, room)));
        }
        markKey();
        app.RefreshChordLive(true);
    };

    // --- timeline ------------------------------------------------------------
    ImGui::Dummy(ImVec2(1, 2));
    ImVec2 a = ImGui::GetCursorScreenPos();
    const float W = std::max(200.0f, ImGui::GetContentRegionAvail().x), H = 52.0f;
    ImGui::InvisibleButton("##chtimeline", ImVec2(W, H));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cw = W / len;
    dl->AddRectFilled(a, ImVec2(a.x + W, a.y + H), vis::kInset, 4.0f);
    for (int i = 0; i < len; ++i) {
        float x = a.x + cw * i;
        if (i % 4 == 0) dl->AddLine(ImVec2(x, a.y + 2), ImVec2(x, a.y + H - 2), i % 16 == 0 ? vis::kLineHi : vis::kLine, 1.0f);
    }
    const int bsSel = coverOf(st.sel);
    for (int j = 0; j < len; ++j) {
        const Step& s = tr.steps[j];
        if (!s.active || s.chordSize <= 0) continue;
        int span = std::clamp<int>(s.lengthSteps, 1, len - j);
        ImVec2 p0(a.x + cw * j + 1.0f, a.y + 4.0f), p1(a.x + cw * (j + span) - 1.0f, a.y + H - 4.0f);
        dl->AddRectFilled(p0, p1, j == bsSel ? IM_COL32(110, 82, 170, 255) : IM_COL32(70, 54, 112, 255), 4.0f);
        dl->AddRect(p0, p1, j == bsSel ? vis::kAccent : IM_COL32(150, 120, 210, 255), 4.0f, 0, j == bsSel ? 2.0f : 1.0f);
        std::string lb = ChordLabel(s);
        while (lb.size() > 1 && ImGui::CalcTextSize(lb.c_str()).x > p1.x - p0.x - 6.0f) lb.pop_back();
        dl->AddText(ImVec2(p0.x + 4.0f, p0.y + 3.0f), vis::kText, lb.c_str());
        char sn[16]; std::snprintf(sn, sizeof sn, "%d", j + 1);
        if (p1.x - p0.x > 24.0f) dl->AddText(ImVec2(p0.x + 4.0f, p1.y - 15.0f), IM_COL32(190, 176, 230, 200), sn);
    }
    if (bsSel < 0) dl->AddRect(ImVec2(a.x + cw * st.sel, a.y + 3.0f), ImVec2(a.x + cw * (st.sel + 1), a.y + H - 3.0f), vis::kAccent, 3.0f, 0, 2.0f);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        int step = std::clamp(static_cast<int>((io.MousePos.x - a.x) / cw), 0, len - 1);
        int c = coverOf(step);
        st.sel = c >= 0 ? c : step;
    }
    if (ImGui::IsItemHovered()) Hint("Click a chord to edit it, or an empty step to put a chord there.");

    // --- the selected chord --------------------------------------------------
    const bool haveChord = bsSel >= 0;
    ChordSpec sp; sp.root = BuildScaleNotes(key, 0, 3, 3).empty() ? 48 : 48 + key; sp.quality = 0; sp.inversion = 0;
    bool known = false;
    std::vector<int> curNotes;
    if (haveChord) {
        const Step& s = tr.steps[bsSel];
        known = IdentifyChord(s.chordNotes.data(), s.chordSize, sp);
        if (!known) { int lo = 127; for (int i = 0; i < s.chordSize; ++i) lo = std::min<int>(lo, s.chordNotes[i]); sp.root = lo; sp.quality = 0; sp.inversion = 0; }
        for (int i = 0; i < s.chordSize; ++i) curNotes.push_back(s.chordNotes[i]);
    } else {
        // new chord: the tonic triad / seventh of the key as the starting point
        Chord c = BuildChord(key, scale, 1, st.sevenths, 3);
        std::vector<int8_t> nn(c.notes.begin(), c.notes.end());
        if (!IdentifyChord(nn.data(), static_cast<int>(nn.size()), sp)) { sp.root = 48 + key; sp.quality = 0; }
        sp.inversion = 0;
    }
    ImGui::Dummy(ImVec2(1, 4));
    if (haveChord) {
        std::string nm = known ? std::string(LabPcName(sp.root)) + ChordQuality(sp.quality).suffix : std::string("custom");
        ImGui::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f), "STEP %d   %s", bsSel + 1, nm.c_str());
        ImGui::SameLine(0, 12);
        std::string ns; for (int n : curNotes) { ns += NoteLabel(n); ns += " "; }
        ImGui::TextDisabled("%s  (%d step%s)", ns.c_str(), tr.steps[bsSel].lengthSteps, tr.steps[bsSel].lengthSteps == 1 ? "" : "s");
    } else {
        ImGui::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f), "STEP %d   (no chord)", st.sel + 1);
        ImGui::SameLine(0, 12); ImGui::TextDisabled("pick a degree, root or quality below to put one here");
    }
    const int start = haveChord ? bsSel : st.sel;
    auto applySpec = [&](ChordSpec ns) { writeChord(start, BuildChordNotes(ns), 0); };

    // degrees of the header's key / scale
    ImGui::TextDisabled("DEGREE");
    ImGui::SameLine(0, 6);
    Check("7ths", &st.sevenths);
    static const char* const kRoman[7] = {"I", "II", "III", "IV", "V", "VI", "VII"};
    for (int d = 1; d <= 7; ++d) {
        Chord c = BuildChord(key, scale, d, st.sevenths, 3);
        std::vector<int8_t> nn(c.notes.begin(), c.notes.end());
        ChordSpec cs; bool ok = IdentifyChord(nn.data(), static_cast<int>(nn.size()), cs);
        std::string lb = std::string(kRoman[d - 1]) + " " + (ok ? std::string(LabPcName(cs.root)) + ChordQuality(cs.quality).suffix : std::string("?"));
        if (d > 1) ImGui::SameLine(0, 3);
        if (ImGui::Button((lb + "##deg" + std::to_string(d)).c_str())) {
            std::vector<int> v(c.notes.begin(), c.notes.end());
            writeChord(start, v, 0);
        }
    }

    // root
    ImGui::TextDisabled("ROOT  ");
    for (int pc = 0; pc < 12; ++pc) {
        ImGui::SameLine(0, 3);
        const bool cur = (((sp.root % 12) + 12) % 12) == pc && (haveChord || true);
        if (cur) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.36f, 0.10f, 1.0f));
        ImGui::PushID(100 + pc);
        if (ImGui::Button(kKeyNames[pc], ImVec2(36, 0))) {
            ChordSpec ns = sp; ns.root = (sp.root / 12) * 12 + pc; applySpec(ns);
        }
        ImGui::PopID();
        if (cur) ImGui::PopStyleColor();
    }
    ImGui::SameLine(0, 8);
    if (ImGui::Button("OCT -")) { ChordSpec ns = sp; ns.root = std::clamp(sp.root - 12, 24, 84); applySpec(ns); }
    ImGui::SameLine(0, 3);
    if (ImGui::Button("OCT +")) { ChordSpec ns = sp; ns.root = std::clamp(sp.root + 12, 24, 84); applySpec(ns); }

    // quality
    ImGui::TextDisabled("TYPE  ");
    for (int q = 0; q < kNumChordQualities; ++q) {
        if (q % 8 != 0) ImGui::SameLine(0, 3); else if (q > 0) ImGui::Dummy(ImVec2(0, 0)), ImGui::SameLine(0, 0), ImGui::TextUnformatted("       "), ImGui::SameLine(0, 3);
        const bool cur = known && sp.quality == q;
        if (cur) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.22f, 0.46f, 1.0f));
        if (ImGui::Button((std::string(ChordQuality(q).label) + "##q" + std::to_string(q)).c_str())) { ChordSpec ns = sp; ns.quality = q; ns.inversion = std::min(sp.inversion, ChordQuality(q).n - 1); applySpec(ns); }
        if (cur) ImGui::PopStyleColor();
    }

    // inversion / length / order
    ImGui::TextDisabled("INVERSION");
    for (int i = 0; i < 4; ++i) {
        ImGui::SameLine(0, 3);
        const int nmax = ChordQuality(sp.quality).n;
        const bool cur = known && sp.inversion == i;
        if (cur) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.22f, 0.46f, 1.0f));
        ImGui::BeginDisabled(i >= nmax);
        if (ImGui::Button((i == 0 ? "ROOT##inv0" : (std::string(std::to_string(i)) + "##inv" + std::to_string(i)).c_str()))) { ChordSpec ns = sp; ns.inversion = i; applySpec(ns); }
        ImGui::EndDisabled();
        if (cur) ImGui::PopStyleColor();
    }
    ImGui::SameLine(0, 16);
    ImGui::BeginDisabled(!haveChord);
    ImGui::TextDisabled("LENGTH");
    ImGui::SameLine(0, 4);
    {   // one field instead of - / + / x2 / /2: drag, scroll or type the chord's length in steps (up to the next chord)
        const int room = haveChord ? std::max(1, nextStart(bsSel) - bsSel) : 1;
        float lf = haveChord ? static_cast<float>(tr.steps[bsSel].lengthSteps) : 1.0f;
        ValueSpec sp; sp.lo = 1.0f; sp.hi = static_cast<float>(std::max(2, room)); sp.step = 0.15f; sp.wheel = 1.0f; sp.def = 1.0f; sp.integer = true;
        const ValueResult r = ValueField("##chordlen", &lf, sp, 56.0f, haveChord);
        if (haveChord) {
            if (r.started) app.PushUndo();
            if (r.changed) tr.steps[bsSel].lengthSteps = static_cast<uint8_t>(std::clamp(static_cast<int>(lf), 1, room));
            if (r.cancelled) app.DropLastUndo();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) Hint("Length of the selected chord in steps (up to the next chord).");
    }
    ImGui::EndDisabled();

    ImGui::Dummy(ImVec2(1, 4));
    ImGui::BeginDisabled(!haveChord);
    if (ImGui::Button("+ NEXT CHORD") && haveChord) {
        const Step& s = tr.steps[bsSel];
        int at = bsSel + std::max<int>(1, s.lengthSteps);
        if (at < len) { writeChord(at, curNotes, std::max<int>(1, s.lengthSteps)); st.sel = at; }
    }
    if (ImGui::IsItemHovered()) Hint("Add a copy of this chord right after it (then change its root / type).");
    ImGui::SameLine(0, 6);
    if (ImGui::Button("DELETE CHORD") && haveChord) { app.PushUndo(); tr.steps[bsSel] = Step{}; app.RefreshChordLive(true); }
    ImGui::SameLine(0, 6);
    if (ImGui::Button("HEAR")) {}
    if (haveChord) {
        if (ImGui::IsItemActivated() && !app.sequencer.IsPlaying()) {
            int8_t nn[kMaxChordVoices]; int n = std::min<int>(kMaxChordVoices, static_cast<int>(curNotes.size()));
            for (int i = 0; i < n; ++i) nn[i] = static_cast<int8_t>(curNotes[i]);
            app.engine.ChordNoteOn(nn, n, 0.85f, false);
        }
        if (ImGui::IsItemDeactivated() && !app.sequencer.IsPlaying()) app.engine.ChordNoteOff();
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0, 16);
    if (ImGui::Button("CLEAR ALL")) {
        app.PushUndo();
        for (int j = 0; j < kMaxSteps; ++j) tr.steps[j] = Step{};
        app.RefreshChordLive(true);
    }
    if (bank) {
        ImGui::SameLine(0, 16);
        static char nameBuf[16] = ""; static int nameFor = -1;
        if (nameFor != proj.chordSceneEdit) { std::snprintf(nameBuf, sizeof nameBuf, "%s", proj.chordScenes[proj.chordSceneEdit].name.c_str()); nameFor = proj.chordSceneEdit; }
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::InputTextWithHint("##chname", "scene name", nameBuf, sizeof nameBuf, ImGuiInputTextFlags_EnterReturnsTrue)) {
            app.PushUndo(); proj.chordScenes[proj.chordSceneEdit].name = nameBuf;
        }
    }
    ImGui::End();
    st.open = open;
}

} // namespace t8::desktop
