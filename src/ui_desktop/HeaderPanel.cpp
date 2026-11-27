#include "HeaderPanel.h"
#include "LiveWidgets.h"
#include "ValueField.h"
#include "MenuBar.h"
#include "MixerSync.h"
#include "Scope.h"
#include "imgui.h"
#include "InfoLine.h"
#include "Visual.h"
#include "core/Scales.h"
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace t8::desktop {

namespace {
void Label(const char* text) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", text);
    ImGui::SameLine();
}
void Gap(float w = 10.0f) { ImGui::SameLine(0, w); }
} // namespace

// C1 (UI-1): MASTER FX is a tool like MIX LAB and CHORD LAB: main draws it in the room under the header (key F, Esc closes).
// It used to be a popover from a "MASTER FX v" tab button that Esc did not close. The document lives in the menu bar (U8c).
void HeaderPanel::DrawMasterFx(AppState& app) {
    ImGui::PushFont(nullptr, 14.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 4));
    fxPanel_.Draw(app);
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
}

// v44.57: PITCH, a turntable pitch fader for the whole output. Lives at the right end of the tab row (the header row is full at 1440 px).
// `rightX`: screen x the strip ends at. Drawn before the tab bar, so the caller puts the cursor back afterwards.
float HeaderPanel::DrawPitch(AppState& app, float rightX) {
    double bpm;
    {
        SceneLock lock(app.sequencer.PatternMutex(), "PITCH");
        bpm = app.CurrentPattern().bpm;
    }
    const float stripW = app.pitchMode ? 452.0f : 118.0f;
    ImGui::SetCursorScreenPos(ImVec2(rightX - stripW, ImGui::GetCursorScreenPos().y - 1.0f));
    ImGui::PushID("PitchStrip");
    Label("PITCH");
    {
        const float pct = app.pitchPct.load(std::memory_order_relaxed);
        const int bend = app.pitchBend.load(std::memory_order_relaxed);
        char pb[24];
        if (std::fabs(pct) < 0.05f) std::snprintf(pb, sizeof(pb), "0.0%%"); else std::snprintf(pb, sizeof(pb), "%+.1f%%", pct);
        ToggleColors pc;   // lit blue-white while the arrows drive it (the "pitch light" of a deck), amber would read as a warning
        pc.on = ImVec4(0.55f, 0.78f, 1.0f, 1.0f); pc.textOn = ImVec4(0.05f, 0.08f, 0.12f, 1.0f);
        if (ToggleButton(pb, app.pitchMode, ImVec2(62, 0), pc)) app.pitchMode = !app.pitchMode;
        if (ImGui::IsItemHovered() && (ImGui::IsMouseDoubleClicked(0) || (ImGui::GetIO().KeyAlt && ImGui::IsMouseClicked(0)))) { app.PitchSet(0.0f); app.pitchMode = false; }
        if (ImGui::IsItemHovered())   // C1 (UI-4): the keys work at any time, there is no hidden keyboard mode any more
            Hint("PITCH: plays EVERYTHING faster or slower, tempo and pitch together, like the pitch fader of a Technics deck - to play along with a record. Now %+.1f%% = %.1f BPM.\nAt any time: Option+Up / Down = faster / slower by 0.1%% (Shift 1%%), Option+Left / Right = a push while held. Double click or Option+click = 0%%.\nClick: shows the fader and the range. Exports are not affected.",
                 pct, app.PitchedBpm(bpm));
        if (app.pitchMode) {
            // C3 (UI-3): the fader is a ValueField bar like every other fader: it moves from where it is (a click does not jump), Shift =
            // finer, Option+click = 0 %, double click = type a value, Esc during a drag puts it back. Centre = 0 % with a detent.
            ImGui::SameLine(0, 6);
            const float lim = app.PitchLimit();
            const float fw = 150.0f, fh = ImGui::GetFrameHeight();
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            float v = app.pitchPct.load(std::memory_order_relaxed);
            ValueSpec sp = BarSpec(-lim, lim, 0.0f, fw);
            sp.fromCenter = true; sp.notch = kPitchNotchPct; sp.color = IM_COL32(140, 198, 255, 255); sp.fmt = "%.1f"; sp.unit = " %";
            const ValueResult r = ValueField("##pitchFader", &v, sp, fw);
            if (r.changed) app.PitchSet(v);
            const bool hov = ImGui::IsItemHovered();
            {   // ticks every quarter of the range over the bar, the zero one longer
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const float cy = p0.y + fh * 0.5f, x0 = p0.x, x1 = p0.x + fw, xm = (x0 + x1) * 0.5f;
                for (int tk = -4; tk <= 4; ++tk) {
                    const float tx = xm + (x1 - x0) * 0.5f * tk / 4.0f;
                    dl->AddLine(ImVec2(tx, cy - (tk == 0 ? 9.0f : 6.0f)), ImVec2(tx, cy - 5.0f), IM_COL32(150, 160, 175, tk == 0 ? 255 : 120), 1.0f);
                }
            }
            if (hov) Hint("PITCH fader: drag (Shift = finer), Option+click = 0%%, double click = type. Range +-%.0f%%, 0.1%% steps.", lim);

            ImGui::SameLine(0, 6);
            static const char* const kRangeNames[2] = {"8%", "16%"};
            int rg = app.pitchRange16 ? 1 : 0;
            if (Segmented("##pitchRange", &rg, kRangeNames, 2)) app.PitchSetRange16(rg == 1);
            if (ImGui::IsItemHovered()) Hint("PITCH range: +-8%% (fine) or +-16%% (like the 16%% switch of a deck).");

            ImGui::SameLine(0, 6);
            ImGui::AlignTextToFramePadding();
            if (bend != 0) ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1.0f), "PUSH %+.0f%%", bend * AppState::kPitchBendPct);
            else ImGui::TextDisabled("= %.1f BPM", app.PitchedBpm(bpm));
        }
    }
    ImGui::PopID();
    return rightX - stripW;
}

// N1: audio load and dropouts, right-aligned to `rightX` in the tab row (left of PITCH). It used to sit at the end of the
// header row, where there is no room at 1440 px, so it was usually hidden. Reads atomics only (no scene lock).
void HeaderPanel::DrawDropouts(AppState& app, float rightX) {
    Mixer& mx = app.engine.GetMixer();
    SceneMutex& sm = app.sequencer.PatternMutex();
    const float load = mx.audioLoad.load(), peak = mx.audioLoadPeak.load();
    const unsigned xr = mx.audioXruns.load();
    const unsigned longestMs = (mx.audioLongestUs.load() + 500) / 1000;
    char b[64];
    if (xr) std::snprintf(b, sizeof(b), "DSP %.0f%%   XRUN %u (%u ms)", load * 100.0f, xr, longestMs);
    else std::snprintf(b, sizeof(b), "DSP %.0f%%   XRUN 0", load * 100.0f);
    ImGui::SetCursorScreenPos(ImVec2(rightX - ImGui::CalcTextSize(b).x, ImGui::GetCursorScreenPos().y));
    ImGui::AlignTextToFramePadding();
    const ImVec4 col = ImGui::ColorConvertU32ToFloat4(xr ? vis::kRed : (load > 0.6f ? vis::kAccent : vis::kTextDim));   // theme colours (Visual.h)
    ImGui::TextColored(col, "%s", b);
    {   // N1 (LIVE-5): share of the last second in which other threads (mostly the screen) held the scene lock
        const Uint32 nowMs = SDL_GetTicks();
        if (lastShareMs_ == 0 || nowMs - lastShareMs_ >= 1000) {
            const unsigned long long total = sm.HeldTotalUs();
            if (lastShareMs_ != 0)
                heldShare_ = std::min(1.0f, static_cast<float>(static_cast<double>(total - lastHeldTotalUs_) / (1000.0 * (nowMs - lastShareMs_))));
            lastShareMs_ = nowMs; lastHeldTotalUs_ = total;
        }
    }
    if (ImGui::IsItemHovered()) {
        // the last few dropouts, newest first: "12s ago 250.0 ms (screen 248.0)"
        std::string recent;
        const Uint32 now = SDL_GetTicks();
        for (unsigned k = 0; k < std::min<unsigned>(xr, 5); ++k) {
            const Mixer::Dropout& d = mx.dropouts[(xr - 1 - k) % Mixer::kDropoutLog];
            char e[64];
            const unsigned ago = (now - d.atMs.load()) / 1000;
            std::snprintf(e, sizeof(e), "%s%us ago %.1f ms (screen %.1f)", k ? ", " : "", ago, d.lenUs.load() / 1000.0, d.waitUs.load() / 1000.0);
            recent += e;
        }
        const double bufMs = 1000.0 * mx.audioBufferFrames.load() / std::max(1, app.engine.SampleRate());   // the buffer the device really uses
        const char* who = sm.MaxHoldWho();
        Hint("Audio thread load %.0f%% (recent peak %.0f%%). XRUN = audible dropouts: a buffer (%.1f ms) was not ready in time.\n"
             "Longest %.1f ms, of which the screen held the sound %.1f ms. The screen held the sound at most %.1f ms%s%s%s, "
             "and %.0f%% of the last second.%s%s\nClick to reset.",
             load * 100.0f, peak * 100.0f, bufMs, mx.audioLongestUs.load() / 1000.0, mx.audioLongestWaitUs.load() / 1000.0,
             sm.MaxHoldUs() / 1000.0, who[0] ? " (" : "", who, who[0] ? ")" : "", heldShare_ * 100.0f,
             recent.empty() ? "" : "\nLast: ", recent.c_str());
    }
    if (ImGui::IsItemClicked()) { mx.ResetDropouts(); sm.ResetMaxHold(); }
}

void HeaderPanel::Draw(AppState& app) {
    SceneLock lock(app.sequencer.PatternMutex(), "HEADER");
    Pattern& pat = app.CurrentPattern();

    ImGui::PushID("HeaderPanel");
    // Vertically centre the single row inside the fixed-height child.
    float h = ImGui::GetWindowHeight();
    ImGui::SetCursorPosY(std::max(0.0f, (h - ImGui::GetFrameHeight()) * 0.5f));

    // ---- PLAY -----------------------------------------------------------
    bool playing = app.sequencer.IsPlaying();
    if (ToggleButton(playing ? "STOP" : "PLAY", playing, ImVec2(64, 0), ToggleTeal())) app.TransportToggle();

    // ---- REC ------------------------------------------------------------
    ImGui::SameLine();
    bool rec = app.recorder.IsRecording();
    ToggleColors recCol = ToggleRed();
    recCol.off = ImVec4(0.22f, 0.12f, 0.12f, 1.0f);
    char recBuf[32];
    if (rec) std::snprintf(recBuf, sizeof(recBuf), "REC %.0fs", app.recorder.ElapsedSeconds());
    else std::snprintf(recBuf, sizeof(recBuf), "REC");
    if (ToggleButton(recBuf, rec, ImVec2(rec ? 78.0f : 56.0f, 0), recCol)) app.SetStatus(app.recorder.Toggle());
    {   // v44.40: an outline and a dot, so REC is not mistaken for the (now red) kick
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImDrawList* rdl = ImGui::GetWindowDrawList();
        rdl->AddRect(a, b, IM_COL32(255, 107, 94, rec ? 255 : 150), 4.0f, 0, 1.5f);
        if (!rec) rdl->AddCircleFilled(ImVec2(a.x + 9.0f, (a.y + b.y) * 0.5f), 3.2f, IM_COL32(255, 107, 94, 230));
    }
    if (ImGui::IsItemHovered()) {
        Hint(rec ? "Stop recording the live set" : "Record the whole live set to a WAV file");
    }

    // ---- BPM ------------------------------------------------------------
    Gap(18);
    Label("BPM");
    {
        float bpm = pat.bpm;
        ValueSpec sp; sp.lo = 20.0f; sp.hi = 300.0f; sp.step = 0.5f; sp.wheel = 1.0f; sp.def = 120.0f; sp.integer = false; sp.fmt = "%.0f";
        const ValueResult r = ValueField("##bpm", &bpm, sp, 64);
        if (r.started) app.PushUndo();
        if (r.changed) pat.bpm = std::clamp(bpm, 20.0f, 300.0f);
        if (r.cancelled) app.DropLastUndo();
        if (ImGui::IsItemHovered()) Hint("Tempo of this scene (20-300 BPM). Option+click = 120.");
    }

    // ---- PATTERN --------------------------------------------------------
    Gap();
    const bool onTrack = app.activeTab == 1 && app.project.songChainLength > 0;
    Label(onTrack ? "BRICK" : "SCENE");
    if (ImGui::ArrowButton("##ptnPrev", ImGuiDir_Left)) StepScene(app, -1, true);
    if (ImGui::IsItemHovered()) Hint(onTrack ? "Previous brick (its own notes open in the editor)." : "Previous scene.");
    ImGui::SameLine(0, 4);
    ImGui::AlignTextToFramePadding();
    if (onTrack) {
        const int b = app.BrickForTransport();
        ImGui::Text("%02d/%d", (b >= 0 ? b : std::clamp(app.selBrick, 0, app.project.songChainLength - 1)) + 1, app.project.songChainLength);
    } else {
        ImGui::Text("%02d/%d", (AppState::IsBrickSlot(app.project.currentPattern) ? app.project.liveScene : app.project.currentPattern) + 1, kNumLiveScenes);
    }
    ImGui::SameLine(0, 4);
    if (ImGui::ArrowButton("##ptnNext", ImGuiDir_Right)) StepScene(app, 1, true);
    if (ImGui::IsItemHovered()) Hint(onTrack ? "Next brick (its own notes open in the editor)." : "Next scene.");

    // ---- LAUNCH quantize (when a scene change takes effect) ----------------
    // C1 (UI-9): only on LIVE; TRACK plays bricks in order, LAUNCH is about clicking scenes
    if (app.ShownTab() != 1) {
        Gap(8);
        Label("LAUNCH");
        static const char* const kLaunchNames[3] = {"NOW", "BEAT", "BAR"};
        int lq = app.sequencer.LaunchQuantize();
        if (Segmented("##launchq", &lq, kLaunchNames, 3)) app.sequencer.SetLaunchQuantize(lq);
        if (ImGui::IsItemHovered()) Hint("When a scene click takes effect while playing:\nNOW = immediately, BEAT = next beat, BAR = next bar");
    }

    // ---- KEY / SCALE (shared with the RANDOM / CHORDS generators) -------
    Gap();
    Label("KEY");
    int key = std::clamp(app.project.randomizerKeyRoot, 0, kNumKeys - 1);
    ImGui::SetNextItemWidth(58);
    if (ChoiceBegin("##key", kKeyNames[key])) {
        for (int k = 0; k < kNumKeys; ++k) {
            if (ImGui::Selectable(kKeyNames[k], k == key)) app.project.randomizerKeyRoot = k;
        }
        ChoiceEnd();
    }
    ImGui::SameLine(0, 8);
    int scale = std::clamp(app.project.randomizerScale, 0, kNumScales - 1);
    ImGui::SetNextItemWidth(150);
    if (ChoiceBegin("##scale", kScaleNames[scale], ImGuiComboFlags_HeightLargest)) {   // all 13 scales visible, no scrolling
        for (int k = 0; k < kNumScales; ++k) {
            const int s = kScaleMenuOrder[k];
            if (k == 2 || k == 7 || k == 9 || k == 12) ImGui::Separator();   // major + minor | modes | minors | pentatonics + blues | chromatic
            if (ImGui::Selectable(kScaleNames[s], s == scale)) app.project.randomizerScale = s;
        }
        ChoiceEnd();
    }
    if (ImGui::IsItemHovered()) Hint("Key / scale used by the randomizers and the chord generator.\nWritten chords are NOT changed (see CHORDS > ADJUST CHORDS TO SCALE / FOLLOW KEY).");
    if (app.SceneChordsOutOfKey(pat)) {
        ImGui::SameLine(0, 6);
        ImGui::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f), "!");
        if (ImGui::IsItemHovered())
            Hint("This scene's chords were written in %s %s.\nThey stay as they are -- use CHORDS > ADJUST CHORDS TO SCALE to move them.",
                              kKeyNames[std::clamp<int>(app.ChordsSrcKey(pat), 0, kNumKeys - 1)], kScaleNames[std::clamp<int>(app.ChordsSrcScale(pat), 0, kNumScales - 1)]);
    }

    // ---- LENGTH / SWING -------------------------------------------------
    Gap();
    Label("LEN");
    // v44.2: the value is applied when the drag / typing ends, so growing the pattern repeats it once from the old length
    // (dragging through 17, 18, 19 ... must not tile from every intermediate length)
    static bool lenEditing = false;
    static int lenEditVal = 16;
    {
        float lenF = static_cast<float>(lenEditing ? lenEditVal : pat.masterLength);
        ValueSpec sp; sp.lo = 1.0f; sp.hi = static_cast<float>(kMaxSteps); sp.step = 0.2f; sp.wheel = 1.0f; sp.def = 16.0f; sp.integer = true;
        const ValueResult r = ValueField("##len", &lenF, sp, 54);
        if (r.changed) { lenEditing = true; lenEditVal = std::clamp(static_cast<int>(lenF + 0.5f), 1, kMaxSteps); }
        if (r.finished && lenEditing) { lenEditing = false; app.SetPatternLength(lenEditVal); }   // SetPatternLength takes the undo step
    }
    if (ImGui::IsItemHovered()) {
        Pattern lp = pat; lp.masterLength = lenEditing ? lenEditVal : pat.masterLength;
        Hint("Scene length in steps. Longer REPEATS what is already there (16 -> 64 = four copies); tracks with their own length stay. Option+click = 16. Undoable.  Now: %.4g bars = %.1f s at %.0f BPM (a bar = %d steps).",
                          PatternBars(lp), PatternSeconds(lp), pat.bpm, 4 * ClampGrid(pat.stepsPerBeat));
    }
    {   // v44.4: the length in bars next to the field ("1b", "2.5b": a triplet grid does not always end on a bar line)
        Pattern lp = pat; lp.masterLength = lenEditing ? lenEditVal : pat.masterLength;
        ImGui::SameLine(0, 3);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%.4gb", PatternBars(lp));
    }
    Gap(10);
    Label("SWING");
    {
        float swingPct = std::round(pat.swing * 100.0f);
        ValueSpec sp; sp.lo = 0.0f; sp.hi = 75.0f; sp.step = 0.5f; sp.wheel = 1.0f; sp.def = 0.0f; sp.integer = true; sp.unit = "%";
        const ValueResult r = ValueField("##swing", &swingPct, sp, 58);
        if (r.started) app.PushUndo();
        if (r.changed) pat.swing = std::clamp(swingPct, 0.0f, 75.0f) / 100.0f;
        if (r.cancelled) app.DropLastUndo();
    }
    // ---- GROOVE (a property of the whole pattern, so it sits next to the swing) + RESET -----------------
    // v44.3: the button opens the groove window in place of the mixer (any track selected); choose there, then BAKE.
    Gap(8);
    {
        const bool staged = !app.stagedGroove.IsFlat();
        if (ToggleButton(staged && !app.grooveViewOpen ? "GROOVE *" : "GROOVE", app.grooveViewOpen, ImVec2(0, 0), ToggleTab())) app.grooveViewOpen = !app.grooveViewOpen;
        if (ImGui::IsItemHovered()) Hint("Groove of the whole scene: opens the groove window in place of the mixer.\nChoose a groove and an amount there, then BAKE writes it into the steps.%s",
                                                       staged ? "\n(* a groove is chosen but not baked yet)" : "");
        ImGui::SameLine(0, 4);
        const bool feel = app.FeelIsActive(pat);
        ImGui::PushStyleColor(ImGuiCol_Text, feel ? ImVec4(0.95f, 0.64f, 0.24f, 1.0f) : ImVec4(0.45f, 0.47f, 0.55f, 1.0f));
        if (ImGui::Button("RESET##feel")) app.ResetFeel();
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) Hint("Straight again: swing 0, every track back on the scene swing, no groove chosen.\nAlways available; Undo brings the feel back. (Grooves already baked into steps stay.)");
    }
    Gap(10);
    Label("GRID");
    ImGui::SetNextItemWidth(66);
    if (ChoiceBegin("##grid", GridLabel(ClampGrid(pat.stepsPerBeat)))) {
        for (int g : kGridChoices) {
            if (ImGui::Selectable(GridLabel(g), ClampGrid(pat.stepsPerBeat) == g)) app.SetPatternResolution(g);
        }
        ChoiceEnd();
    }
    if (ImGui::IsItemHovered()) Hint("Step grid of this scene: 1/16 (default), 1/32, 1/16 triplets, 1/8 triplets.\nSwitching the grid converts the scene (notes stay where they are in time).\nThe triplet grids move every note to the nearest step in time\n(tempo and feel stay put; 16ths between the triplet steps snap). The length keeps its number of bars. Undoable.");

    // C1 (UI-9): no MASTER button here any more: the chain switch is CHAIN ON / OFF in MASTER FX (key F), and the MASTER FX
    // button in the tab row shows a dot while the chain is on.
    Gap(10);
    // ---- UNDO -----------------------------------------------------------
    ImGui::SameLine();
    if (ImGui::Button("UNDO")) app.SetStatus(app.Undo() ? app.lastUndoLabel : "NOTHING TO UNDO");

    // ---- live scope of the master bus (only if there is room) ----------
    {
        float rightReserve = 64.0f; // keeps the clip / status text clear
        // room left on this row (the cursor already moved to the next line, so measure from the last item)
        const float roomX = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - ImGui::GetItemRectMax().x;
        // full scope when there is plenty of room, a narrower one so the DSP readout still fits
        const float scopeW = roomX > 104.0f + rightReserve + 14.0f + 76.0f ? 104.0f : 60.0f;
        if (roomX > scopeW + rightReserve + 14.0f) {
            ImGui::SameLine(0, 14.0f);
            DrawScope(app.engine, ImVec2(scopeW, ImGui::GetFrameHeight()));
        }
    }
    // ---- loudness of the master bus: LUFS short-term + integrated, true peak, gain reduction ----
    {
        LoudnessMeter& lm = app.engine.Loudness();
        const MasterChainSettings& mc = app.project.fx.master;
        const float st = lm.ShortTerm(), integ = lm.Integrated(), tp = lm.TruePeakDb();
        const float gr = mc.compOn ? app.engine.GetMasterChain().GainReductionDb() : 0.0f;
        const float lgr = mc.limiterOn ? app.engine.GetLimiter().GainReductionDb() : 0.0f;
        char b[96];
        if (integ > -100.0f) std::snprintf(b, sizeof(b), "LUFS-S %.1f  INT %.1f  TP %.1f", std::max(st, -99.0f), integ, tp);
        else std::snprintf(b, sizeof(b), "LUFS-S %.1f  TP %.1f", std::max(st, -99.0f), tp);
        const float roomG = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - ImGui::GetItemRectMax().x;
        if (roomG > ImGui::CalcTextSize(b).x + 40.0f) {
            ImGui::SameLine(0, 10);
            ImGui::AlignTextToFramePadding();
            const bool hot = tp > mc.ceilingDb + 0.3f && mc.limiterOn;
            ImVec4 col = hot ? ImVec4(0.91f, 0.30f, 0.30f, 1.0f) : ImVec4(0.5f, 0.52f, 0.6f, 1.0f);
            ImGui::TextColored(col, "%s", b);
            if (ImGui::IsItemHovered())
                Hint("Loudness of the master output (ITU BS.1770 / EBU R128).\nS = short-term (3 s), INT = integrated since the last reset, TP = true peak (dBTP).\nComp GR %.1f dB, limiter GR %.1f dB. Streaming is about -14 LUFS; club masters are often -9..-7.\nClick to reset INT and TP.", gr, lgr);
            if (ImGui::IsItemClicked()) lm.RequestReset();
        }
    }

    // ---- right edge: clip + status -------------------------------------
    bool clipping = app.engine.GetMixer().masterClipHold.load() > 0.0f;
    std::string right;
    if (app.statusMessageTimer > 0.0f) right = app.statusMessage;
    {
        // Room between the last item and the right edge; a long message is shortened (never drawn over the buttons).
        const float lastRight = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
        const float clipW = clipping ? ImGui::CalcTextSize("CLIP  ").x : 0.0f;
        const float avail = ImGui::GetWindowContentRegionMax().x - lastRight - 14.0f - clipW;
        if (!right.empty()) {
            std::string r = right;
            while (!r.empty() && ImGui::CalcTextSize(r.c_str()).x > avail) r.pop_back();
            if (r.size() < right.size() && r.size() > 3) { r.resize(r.size() - 2); r += ".."; }
            right = r;
        }
        float rightW = clipW + (right.empty() ? 0.0f : ImGui::CalcTextSize(right.c_str()).x);
        if (rightW > 0.0f && (right.size() > 0 || clipping)) {
            float x = ImGui::GetWindowContentRegionMax().x - rightW;
            ImGui::SameLine();
            ImGui::SetCursorPosX(std::max(x, lastRight + 12.0f));
            ImGui::AlignTextToFramePadding();
            if (clipping) { ImGui::TextColored(ImVec4(0.91f, 0.30f, 0.30f, 1.0f), "CLIP"); ImGui::SameLine(); }
            if (!right.empty()) ImGui::TextDisabled("%s", right.c_str());
        }
    }

    // Needed so ImGui accepts the SetCursorPos() calls above as content.
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::PopID();
}

} // namespace t8::desktop
