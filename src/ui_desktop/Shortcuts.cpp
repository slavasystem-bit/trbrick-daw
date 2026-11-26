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

    // v44.57 PITCH, C1 (UI-4): no hidden keyboard mode any more. Option+Up / Down = faster / slower by 0.1% (Shift 1%),
    // Option+Left / Right = a push (pitch bend) while held, at any time and on any screen; Return always plays from the brick.
    // The keys are taken for the frame, so the grid, the piano roll and the lists do not move at the same time.
    const bool opt = io.KeyAlt && !cmd;
    if (opt) {
        const float stepPct = shift ? 1.0f : 0.1f;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) app.PitchStep(stepPct);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) app.PitchStep(-stepPct);
        const bool l = ImGui::IsKeyDown(ImGuiKey_LeftArrow), r = ImGui::IsKeyDown(ImGuiKey_RightArrow);
        app.pitchBend.store(l == r ? 0 : (r ? 1 : -1), std::memory_order_relaxed);
        for (ImGuiKey k : {ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_LeftArrow, ImGuiKey_RightArrow})
            ImGui::SetKeyOwner(k, ImGuiKeyOwner_Any, ImGuiInputFlags_LockThisFrame);
    } else {
        app.pitchBend.store(0, std::memory_order_relaxed);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F1, false) || (!cmd && shift && ImGui::IsKeyPressed(ImGuiKey_Slash, false))) {   // "?" / F1
        app.helpOpen = !app.helpOpen;
        return;
    }
    if (app.helpOpen && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { app.helpOpen = false; return; }
    // C1 (UI-1): Esc closes the open tool (MIX LAB, CHORD LAB, MASTER FX), then the PITCH fader strip (popups and text boxes take Esc first)
    // A drag in progress (a knob, a fader, a lane point) takes Esc first: it puts the value back (review of C1 part 2)
    const bool dragging = ImGui::IsAnyItemActive();
    if (!dragging && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (app.tool != AppState::Tool::None) { app.OpenTool(AppState::Tool::None); return; }
        if (app.pitchMode) { app.pitchMode = false; return; }
    }
    // C1 (UI-1): M / C / F open MIX LAB / CHORD LAB / MASTER FX in the room under the header; the same key closes it
    if (!cmd && !io.KeyAlt && !shift && !dragging) {
        if (ImGui::IsKeyPressed(ImGuiKey_M, false)) { app.ToggleTool(AppState::Tool::MixLab); return; }
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) { app.ToggleTool(AppState::Tool::ChordLab); return; }
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) { app.ToggleTool(AppState::Tool::MasterFx); return; }
    }
    if (!cmd && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        app.TransportToggle();   // v44.10: the one player (brick loop / track), see AppState::TransportToggle
        return;
    }
    if (!cmd && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {   // v44.38: Tab goes between LIVE and TRACK (C1: and closes the open tool)
        app.OpenTool(AppState::Tool::None);
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
    const int frame = ImGui::GetFrameCount();   // C1 (UI-8): over the piano roll with notes selected / copied, Cmd+C / Cmd+V are for the notes
    if (cmd && !shift && app.rollCopyFrame >= frame - 1 && ImGui::IsKeyPressed(ImGuiKey_C, false)) return;
    if (cmd && !shift && app.rollPasteFrame >= frame - 1 && ImGui::IsKeyPressed(ImGuiKey_V, false)) return;
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
