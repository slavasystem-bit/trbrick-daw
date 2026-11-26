#pragma once
// "?" / F1: every gesture of the program on one screen (the table of docs/INTERACTION_MODEL.md section 5, only what works today).
#include "app/AppState.h"
#include "imgui.h"
#include "Visual.h"

namespace t8::desktop {

inline void DrawHelpScreen(AppState& app) {
    if (!app.helpOpen) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(660.0f, 0.0f), ImGuiCond_Appearing);
    ImGui::PushFont(nullptr, 14.0f);
    if (ImGui::Begin("Gestures  -  the same everywhere##help", &app.helpOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) app.helpOpen = false;
        struct Row { const char* gesture; const char* meaning; };
        auto table = [&](const char* title, const Row* rows, int n) {
            ImGui::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f), "%s", title);
            if (ImGui::BeginTable(title, 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("g", ImGuiTableColumnFlags_WidthFixed, 190.0f);
                ImGui::TableSetupColumn("m", ImGuiTableColumnFlags_WidthStretch);
                for (int i = 0; i < n; ++i) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(rows[i].gesture);
                    ImGui::TableNextColumn(); ImGui::TextDisabled("%s", rows[i].meaning);
                }
                ImGui::EndTable();
            }
            ImGui::Spacing();
        };
        static const Row numbers[] = {
            {"Drag", "change the value (fields: any direction; faders: along the bar)"},
            {"Shift + drag", "finer (x0.1)"},
            {"Mouse wheel", "one step; Shift = finer"},
            {"Double-click", "type the number (Enter applies, Esc cancels)"},
            {"Option + click", "back to the default value"},
            {"Esc while dragging", "puts the value back"}};
        static const Row editing[] = {
            {"Click", "select / press"},
            {"Double-click a brick", "play from here"},
            {"Drag", "move; the edge of a brick or a fill changes its length"},
            {"Shift + click a scene pad", "link / unlink it with the edited scene (same mute and solo); EDIT: a click only opens, double click = launch"},
            {"Shift + drag a WHO PLAYS cell", "that instrument's level on the brick"},
            {"Click on a lane (MIX, FILTER, instrument)", "add a point; drag it, double-click an inner point = remove"},
            {"Right-click", "the menu of the thing under the mouse"},
            {"Ctrl / Cmd + wheel over the strip", "zoom"}};
        static const Row keys[] = {
            {"Space", "play / stop"},
            {"Home", "play the track from the first brick"},
            {"Cmd + Z    Shift + Cmd + Z", "undo    redo (also Cmd + Y)"},
            {"Cmd + S", "save the project"},
            {"Cmd + C / V", "copy / paste the selected track  (+ Shift: the whole scene); over the piano roll: the selected notes"},
            {"[  and  ]", "previous / next scene"},
            {"1 ... 0   and   Cmd + left / right", "select an instrument"},
            {"Tab", "LIVE <-> TRACK"},
            {"M   C   F", "MIX LAB   CHORD LAB   MASTER FX (the same key or Esc closes)"},
            {"Option + up / down", "PITCH faster / slower 0.1% (Shift 1%); Option + left / right = push while held"},
            {"?   or   F1", "this screen"}};
        table("NUMBERS AND FADERS", numbers, 6);
        table("EDITING", editing, 7);
        table("KEYS", keys, 11);
        ImGui::TextDisabled("Point at anything: the line at the bottom of the window says what it does.");
    }
    ImGui::End();
    ImGui::PopFont();
}

} // namespace t8::desktop
