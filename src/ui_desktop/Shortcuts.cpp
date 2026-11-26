#include "ui_desktop/Shortcuts.h"
#include "core/Persistence.h"
#include "ui_desktop/MixerSync.h"
#include "imgui.h"
#include "imgui_internal.h"   // SetKeyOwner (take a key for the frame)
#include <algorithm>
#include <mutex>

namespace t8::desktop {

void HandleShortcuts(AppState& app) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        app.pitchBend.store(0, std::memory_order_relaxed);   // a push never sticks when a box or a menu takes the keys
        return;
    }

    const bool cmd = io.KeyCtrl || io.KeySuper;     // ImGui maps Cmd to Ctrl on macOS
    const bool shift = io.KeyShift;

    // v44.57: PITCH (a turntable pitch fader). P switches the mode; in it the arrow keys are the fader: up / down = faster / slower by
    // 0.1% (Shift 1%), left / right = a push (pitch bend) while held, Return = back to 0, R = range +-8% / +-16%, Esc or P = leave.
    // The keys are taken for the frame, so the grid, the piano roll and the lists do not move at the same time. Cmd+arrows stay as they were.
    if (!cmd && ImGui::IsKeyPressed(ImGuiKey_P, false)) {
        app.pitchMode = !app.pitchMode;
        app.pitchBend.store(0, std::memory_order_relaxed);
        app.SetStatus(app.pitchMode ? "PITCH: UP / DOWN = FASTER / SLOWER, LEFT / RIGHT = PUSH (HOLD), RETURN = 0%, R = RANGE, P = LEAVE" : "PITCH: ARROWS ARE BACK TO NORMAL");
        return;
    }
    if (app.pitchMode && !cmd) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { app.pitchMode = false; app.pitchBend.store(0, std::memory_order_relaxed); return; }
        const float stepPct = shift ? 1.0f : 0.1f;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) app.PitchStep(stepPct);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) app.PitchStep(-stepPct);
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) { app.PitchSet(0.0f); app.SetStatus("PITCH 0.0%"); }
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) { app.PitchSetRange16(!app.pitchRange16); app.SetStatus(app.pitchRange16 ? "PITCH RANGE +-16%" : "PITCH RANGE +-8%"); }
        const bool l = ImGui::IsKeyDown(ImGuiKey_LeftArrow), r = ImGui::IsKeyDown(ImGuiKey_RightArrow);
        app.pitchBend.store(l == r ? 0 : (r ? 1 : -1), std::memory_order_relaxed);
        for (ImGuiKey k : {ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_R})
            ImGui::SetKeyOwner(k, ImGuiKeyOwner_Any, ImGuiInputFlags_LockThisFrame);
    } else {
        app.pitchBend.store(0, std::memory_order_relaxed);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F1, false) || (!cmd && shift && ImGui::IsKeyPressed(ImGuiKey_Slash, false))) {   // "?" / F1
        app.helpOpen = !app.helpOpen;
        return;
    }
    if (app.helpOpen && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { app.helpOpen = false; return; }
    // v44.45: Esc closes what was opened last: CHORD LAB over LIVE, MIX LAB back to LIVE (popups and text boxes take Esc first)
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        if (app.chordLabOpen && (app.activeTab == 2 || app.activeTab == 1)) { app.chordLabOpen = false; return; }
        if (app.activeTab == 3) { app.uiTabRequest = 2; return; }
    }
    if (!cmd && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        app.TransportToggle();   // v44.10: the one player (brick loop / track), see AppState::TransportToggle
        return;
    }
    if (!cmd && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {   // v44.38: Tab goes between LIVE and TRACK
        app.uiTabRequest = app.activeTab == 1 ? 2 : 1;
        return;
    }
    if (!cmd && ImGui::IsKeyPressed(ImGuiKey_Home, false) && app.activeTab == 1) {   // v44.28: to the first brick (restarts there when it plays)
        app.TrackToStart(); SelectBrickForEdit(app, 0);
        return;
    }
    if (!cmd && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) && app.activeTab == 1 && app.project.songChainLength > 0) {   // Return: play from the selected brick
        const int b = app.BrickForTransport();
        app.PlayFromBrick(b >= 0 ? b : std::clamp(app.selBrick, 0, app.project.songChainLength - 1));
        return;
    }
    if (cmd && (ImGui::IsKeyPressed(ImGuiKey_Y, false) || (shift && ImGui::IsKeyPressed(ImGuiKey_Z, false)))) {   // v44.15: Shift+Cmd+Z (Mac) / Cmd+Y
        SceneLock lock(app.sequencer.PatternMutex(), "KEYS");
        app.SetStatus(app.Redo() ? app.lastUndoLabel : "NOTHING TO REDO");
        return;
    }
    if (cmd && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        SceneLock lock(app.sequencer.PatternMutex(), "KEYS");
        app.SetStatus(app.Undo() ? app.lastUndoLabel : "NOTHING TO UNDO");
        return;
    }
    if (cmd && ImGui::IsKeyPressed(ImGuiKey_S, false)) {   // U8b: Cmd+S saves the project file, Shift+Cmd+S asks for a new name
        if (shift) { app.docNameMode = AppState::NameMode::SaveAs; app.docNameOpen = true; }
        else app.SaveDocument();
        return;
    }
    if (cmd && ImGui::IsKeyPressed(ImGuiKey_N, false)) { app.RequestDoc(AppState::DocAction::New); return; }
    if (cmd && ImGui::IsKeyPressed(ImGuiKey_O, false)) { app.docOpenDialog = true; return; }
    if (cmd && ImGui::IsKeyPressed(ImGuiKey_Q, false)) { app.RequestDoc(AppState::DocAction::Quit); return; }
    if (cmd && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        if (shift) { app.CopyPattern(); app.SetStatus("SCENE COPIED"); }
        else app.CopyTrack(std::clamp(app.seqCursorTrack, 0, kNumTracks - 1));
        return;
    }
    if (cmd && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
        SceneLock lock(app.sequencer.PatternMutex(), "KEYS");
        if (shift) app.SetStatus(app.PastePattern() ? "SCENE PASTED" : "CLIPBOARD EMPTY");
        else app.PasteTrack(std::clamp(app.seqCursorTrack, 0, kNumTracks - 1));
        return;
    }
    if (!cmd && (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false) || ImGui::IsKeyPressed(ImGuiKey_RightBracket, false))) {
        const int dir = ImGui::IsKeyPressed(ImGuiKey_RightBracket, false) ? 1 : -1;
        SceneLock lock(app.sequencer.PatternMutex(), "KEYS");
        StepScene(app, dir, false);
        app.SetStatus(app.activeTab == 1 && app.project.songChainLength > 0 ? "BRICK " + std::to_string(app.selBrick + 1) : "SCENE " + std::to_string(app.project.currentPattern + 1));
        return;
    }
    if (cmd && (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false))) {
        const int dir = ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) ? 1 : -1;
        app.seqCursorTrack = (std::clamp(app.seqCursorTrack, 0, kNumTracks - 1) + dir + kNumTracks) % kNumTracks;
        return;
    }
    if (!cmd) {
        static const ImGuiKey kDigits[10] = {ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4, ImGuiKey_5,
                                             ImGuiKey_6, ImGuiKey_7, ImGuiKey_8, ImGuiKey_9, ImGuiKey_0};
        for (int i = 0; i < 10 && i < kNumTracks; ++i)
            if (ImGui::IsKeyPressed(kDigits[i], false)) { app.seqCursorTrack = i; return; }
    }
}

} // namespace t8::desktop
