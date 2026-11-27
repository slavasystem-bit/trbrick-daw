// The one app screen, drawn once per frame: shortcuts, the menu bar, the LIVE / TRACK tabs with their blocks, the tools (MIX LAB,
// CHORD LAB, MASTER FX), the dialogs, the info line and the help screen. main.cpp calls it between ImGui::NewFrame() and
// ImGui::Render(); the UI test (tools/uitest_main.cpp, TEST-5) calls the very same function with no window, so every frame the
// test draws is a frame of the real program.
#include "ui_desktop/Shell.h"
#include "ui_desktop/Shortcuts.h"
#include "ui_desktop/MixerSync.h"
#include "ui_desktop/InfoLine.h"
#include "ui_desktop/HelpScreen.h"
#include "ui_desktop/DocumentDialogs.h"
#include "ui_desktop/LiveLayout.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace t8::desktop {

void DrawShellFrame(AppState& app, ShellPanels& p, const DesktopFonts& fonts) {
    ImGuiIO& io = ImGui::GetIO();
    HandleShortcuts(app);
    app.TickKeyFollow();
    app.TickTransport();
    app.RefreshChordLive();

    ImGui::PushFont(fonts.uiRegular);

    // ---- the one app screen: a borderless shell filling the client
    // area. LIVE is the whole performance surface. ----------------------
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
    ImGuiWindowFlags shellFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_MenuBar;
    ImGui::Begin("##Shell", nullptr, shellFlags);
    ImGui::PopStyleVar();
    p.menuBar.Draw(app);   // U8c: File / Edit / View / Library / Settings

    // v44.10: the scene editor (grid | mixer or piano roll, the instrument block, RANDOM / KIT + LIVE) is drawn the same on the TRACK
    // screen (under the bricks) and on the LIVE screen (under the scene pads); `aboveH` is what the screen already used above it.
    // v44.12: STATIC layout. Nothing in here changes size when the author clicks something (no fold-away blocks, no toggles); the
    // only thing that follows the window is the row height of the grid, inside fixed limits. ctxMin = 0 / bottomH = 0 leave
    // the instrument block / the RANDOM-KIT + LIVE block out (TRACK).
    // the instrument block (everything about the selected instrument): under the editor on LIVE, as the SOUND view on TRACK
    auto drawContextBlock = [&](float ctxH) {
        DpadKeys keys;
        {
            bool blocked = io.WantTextInput || io.KeyCtrl || io.KeySuper || app.rollKeysFrame >= ImGui::GetFrameCount() - 1 ||
                ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
            if (!blocked) {
                keys.up = ImGui::IsKeyPressed(ImGuiKey_UpArrow, true);
                keys.down = ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
                keys.left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true);
                keys.right = ImGui::IsKeyPressed(ImGuiKey_RightArrow, true);
                keys.coarse = io.KeyShift;
            }
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
        ImGui::BeginChild("ContextChild", ImVec2(0, ctxH), ImGuiChildFlags_Borders, ImGuiWindowFlags_None);   // scrolls when the window is short
        p.contextPanel.Draw(app, keys);
        ImGui::EndChild();
        ImGui::PopStyleVar();
    };

    auto drawSceneEditor = [&](float availY, float aboveH, float sp, float minRow, float maxRow, float ctxMin, float bottomH) {
        app.detailOpen = true;   // the instrument block is always there (the fold-away "start here" card is gone)
        const float gaps = (2.0f + (ctxMin > 0.0f ? 1.0f : 0.0f) + (bottomH > 0.0f ? 1.0f : 0.0f)) * sp;
        LiveLayout layout;
        float midMax = availY - aboveH - bottomH - ctxMin - gaps;
        float overhead = layout.padY * 2 + layout.headerH + layout.footerH + 4.0f;
        layout.rowH = std::clamp((midMax - overhead) / kNumTracks, minRow, maxRow);
        float midH = overhead + layout.rowH * kNumTracks;
        float ctxH = ctxMin > 0.0f ? std::max(ctxMin, availY - aboveH - bottomH - midH - gaps) : 0.0f;
        const ImVec2 avail(ImGui::GetContentRegionAvail().x, availY);
        const ImGuiWindowFlags fixedFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        // 3. sequencer | mixer-or-piano-roll (same height, row-aligned)
        bool showRoll = false;
        {
            SceneLock lock(app.sequencer.PatternMutex(), "LAYOUT");
            int sel = std::clamp(app.seqCursorTrack, 0, kNumTracks - 1);
            if (sel != p.lastSelTrack) { p.lastSelTrack = sel; app.deskRollClosed = false; app.deskLanesClosed = false; }
            showRoll = app.CurrentPattern().tracks[sel].type != TrackType::Drum && !app.deskRollClosed;
        }
        float seqW = std::min(660.0f, avail.x * 0.45f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(layout.padX, layout.padY));
        ImGui::BeginChild("SeqChild", ImVec2(seqW, midH), ImGuiChildFlags_Borders, fixedFlags);
        p.seqPanel.Draw(app, layout);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("MixerOrRollChild", ImVec2(0, midH), ImGuiChildFlags_Borders, fixedFlags);
        bool showLanes = false;
        {
            SceneLock lock(app.sequencer.PatternMutex(), "LAYOUT");
            showLanes = app.CurrentPattern().tracks[std::clamp(app.seqCursorTrack, 0, kNumTracks - 1)].type == TrackType::Drum && !app.deskLanesClosed;
        }
        if (app.grooveViewOpen) p.groovePanel.Draw(app, layout);   // v44.3: the groove window replaces the mixer on any track
        else if (showRoll) p.pianoRollPanel.Draw(app, layout);
        else if (showLanes) p.lanePanel.Draw(app, layout);
        else p.mixPanel.Draw(app, layout);
        ImGui::EndChild();
        ImGui::PopStyleVar();

        // 4. context block for the selected instrument (LIVE only)
        if (ctxH > 0.0f) drawContextBlock(ctxH);

        // 5. RANDOM / KIT + LIVE, pinned to the bottom (LIVE only)
        if (bottomH > 0.0f) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
            ImGui::BeginChild("BottomChild", ImVec2(0, bottomH), ImGuiChildFlags_Borders, fixedFlags);
            p.bottomPanel.Draw(app);
            ImGui::EndChild();
            ImGui::PopStyleVar();
        }
    };

    {   // v44.57: the PITCH strip at the right end of the tab row (drawn first, the tab bar then starts where it did)
        const ImVec2 here = ImGui::GetCursorScreenPos();
        const float pitchLeft = p.headerPanel.DrawPitch(app, ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - 8.0f);
        ImGui::SetCursorScreenPos(here);
        p.headerPanel.DrawDropouts(app, pitchLeft - 24.0f);   // N1: always visible (the header row has no room at 1440 px)
        ImGui::SetCursorScreenPos(here);
    }
    if (ImGui::BeginTabBar("##MainTabs", ImGuiTabBarFlags_None)) {
        const int tabReq = app.uiTabRequest;   // a request made during this frame (THROW ONTO THE TRACK) waits for the next one
        const ImGuiTabItemFlags trackFlags = tabReq == 1 ? ImGuiTabItemFlags_SetSelected : 0;
        const ImGuiTabItemFlags liveFlags = tabReq == 2 ? ImGuiTabItemFlags_SetSelected : 0;
        const float headerH = 42.0f, liveChainH = 104.0f, liveCtxH = 214.0f, liveBottomH = 132.0f, trackInspH = BricksPanel::InspectorHeight();
        const ImGuiWindowFlags fixedFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        // LIVE (the program opens here): jam. Scene pads + chord bank, the editor, the instrument block, RANDOM / KIT + LIVE.
        // "where am I": LIVE is amber, TRACK is teal (tab, line under it and the label of the editor)
        auto tabHue = [](const ImVec4& bg, const ImVec4& line) {
            ImGui::PushStyleColor(ImGuiCol_TabSelected, bg); ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, bg); ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, line);
        };
        // v44.51: the CHORD LAB child, shared by LIVE and TRACK (`rest` = the height it gets)
        auto drawChordLabChild = [&](float rest) {
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.30f, 0.72f, 0.65f, 1.0f));
                ImGui::BeginChild("ChordLabChild", ImVec2(0, std::max(250.0f, rest)), ImGuiChildFlags_Borders, fixedFlags);
                ImGui::PopStyleColor();
                p.chordLabPanel.SetSoundBody([&](AppState& a) {   // the SOUND view: the instrument block of the chord track, drawn as everywhere else
                    DpadKeys keys;
                    const bool blocked = io.WantTextInput || io.KeyCtrl || io.KeySuper || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
                    if (!blocked) {
                        keys.up = ImGui::IsKeyPressed(ImGuiKey_UpArrow, true); keys.down = ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
                        keys.left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true); keys.right = ImGui::IsKeyPressed(ImGuiKey_RightArrow, true);
                        keys.coarse = io.KeyShift;
                    }
                    p.contextPanel.ShowChordSound();
                    p.contextPanel.Draw(a, keys);
                });
                p.chordLabPanel.Draw(app, ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
                ImGui::EndChild();
                ImGui::PopStyleVar();
        };
        // C1 (UI-1): MIX LAB, CHORD LAB and MASTER FX open the same way: in the room under the header of LIVE or TRACK, with one
        // strip on top (name, what it does, key, x). The tab under them stays selected; Esc, the same key or x closes.
        auto toolColor = [](AppState::Tool t) {
            return t == AppState::Tool::MixLab ? ImVec4(0.70f, 0.55f, 0.95f, 1.0f) : t == AppState::Tool::ChordLab ? ImVec4(0.30f, 0.72f, 0.65f, 1.0f) : ImVec4(0.95f, 0.64f, 0.24f, 1.0f);
        };
        auto drawTool = [&](float rest) {
            const AppState::Tool t = app.tool;
            const char* name = t == AppState::Tool::MixLab ? "MIX LAB" : t == AppState::Tool::ChordLab ? "CHORD LAB" : "MASTER FX";
            const char* what = t == AppState::Tool::MixLab ? "where instruments fight for the same frequencies"
                             : t == AppState::Tool::ChordLab ? "the chords of the key: hear them, try them, build a loop"
                             : "project-wide delay, reverb, distortion, sidechain and the master chain";
            const char* key = t == AppState::Tool::MixLab ? "M" : t == AppState::Tool::ChordLab ? "C" : "F";
            const float stripH = 30.0f;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 4));
            ImGui::PushStyleColor(ImGuiCol_Border, toolColor(t));
            ImGui::BeginChild("ToolStripChild", ImVec2(0, stripH), ImGuiChildFlags_Borders, fixedFlags);
            ImGui::PopStyleColor();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(toolColor(t), "%s", name);
            ImGui::SameLine(0, 12);
            ImGui::TextDisabled("%s", what);
            const float xw = 26.0f, right = ImGui::GetWindowContentRegionMax().x;
            ImGui::SameLine(right - xw - 120.0f);
            ImGui::TextDisabled("%s or Esc closes", key);
            ImGui::SameLine(right - xw);
            if (ImGui::Button("x##toolClose", ImVec2(xw, 0))) app.OpenTool(AppState::Tool::None);
            if (ImGui::IsItemHovered()) Hint(t == AppState::Tool::ChordLab ? "Close CHORD LAB (key C or Esc). A draft stays in its chord slot." : "Close (the same key or Esc).");
            ImGui::EndChild();
            ImGui::PopStyleVar();
            const float bodyH = std::max(200.0f, rest - stripH - ImGui::GetStyle().ItemSpacing.y);
            if (t == AppState::Tool::MixLab) {
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
                ImGui::BeginChild("MixLabChild", ImVec2(0, bodyH), ImGuiChildFlags_None, fixedFlags);
                p.mixLabPanel.Draw(app, ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
                ImGui::EndChild();
                ImGui::PopStyleVar();
            } else if (t == AppState::Tool::ChordLab) {
                drawChordLabChild(bodyH);
            } else {
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));
                ImGui::BeginChild("MasterFxChild", ImVec2(0, bodyH), ImGuiChildFlags_Borders, ImGuiWindowFlags_None);   // scrolls on a short window
                p.headerPanel.DrawMasterFx(app);
                ImGui::EndChild();
                ImGui::PopStyleVar();
            }
        };
        // v44.51: a pulsing dot on the tab whose music is playing (LIVE = the jam, TRACK = the bricks). The tab bar also says it in words.
        auto tabPlayDot = [&](int tab) {
            const AppState::PlaySource ps = app.PlayingSource();
            if (ps == AppState::PlaySource::None || (tab == 2) != (ps == AppState::PlaySource::Live)) return;
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const float pulse = 0.65f + 0.35f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.0f);
            const ImU32 col = tab == 2 ? IM_COL32(246, 163, 61, static_cast<int>(255 * pulse)) : IM_COL32(94, 224, 201, static_cast<int>(255 * pulse));
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(b.x - 12.0f, (a.y + b.y) * 0.5f), 3.6f, col);
        };
        tabHue(ImVec4(0.23f, 0.17f, 0.08f, 1.0f), ImVec4(0.95f, 0.64f, 0.24f, 1.0f));
        const bool liveOpen = ImGui::BeginTabItem("LIVE   ###tabLive", nullptr, liveFlags);
        tabPlayDot(2);
        ImGui::PopStyleColor(3);
        if (liveOpen) {
            ShowTab(app, 2);
            ImGui::PushFont(nullptr, 14.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 4));
            const float sp = ImGui::GetStyle().ItemSpacing.y;
            const float availY = ImGui::GetContentRegionAvail().y - kInfoLineHeight - 4.0f;   // the info line (U2) sits under everything
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 4));
            ImGui::BeginChild("HeaderChild", ImVec2(0, headerH), ImGuiChildFlags_Borders, fixedFlags);
            p.headerPanel.Draw(app);
            ImGui::EndChild();
            ImGui::PopStyleVar();
            if (app.tool != AppState::Tool::None) {
                drawTool(availY - headerH - sp);   // C1: a tool takes the room under the header
            } else {
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
                ImGui::BeginChild("ChainChild", ImVec2(0, liveChainH), ImGuiChildFlags_Borders, fixedFlags);
                p.chainPanel.Draw(app);
                ImGui::EndChild();
                ImGui::PopStyleVar();
                drawSceneEditor(availY, headerH + liveChainH, sp, 18.0f, 34.0f, liveCtxH, liveBottomH);
            }
            ImGui::PopStyleVar(2);
            ImGui::PopFont();
            ImGui::EndTabItem();
        }
        // TRACK: build the finished track from the bricks: header, the strip, the inspector, the scene editor. No RANDOM / KIT, no LIVE block.
        tabHue(ImVec4(0.07f, 0.20f, 0.19f, 1.0f), ImVec4(0.37f, 0.88f, 0.79f, 1.0f));
        const bool trackOpen = ImGui::BeginTabItem("TRACK   ###tabTrack", nullptr, trackFlags);
        tabPlayDot(1);
        ImGui::PopStyleColor(3);
        if (trackOpen) {
            ShowTab(app, 1);
            ImGui::PushFont(nullptr, 14.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 4));
            const float sp = ImGui::GetStyle().ItemSpacing.y;
            const float availY = ImGui::GetContentRegionAvail().y - kInfoLineHeight - 4.0f;   // the info line (U2) sits under everything
            // the strip has a fixed height (the editor under it wants about 60 + 10 rows of 26 px); it only gives way on a short window
            const float modeRowH = 34.0f;   // v44.24: NOTES | SOUND | MIX
            // v44.45: the room the editor needs is worked out from its own layout (10 rows of 20 + header + footer + padding); before, a
            // fixed 236 was too little and the bottom of the sequencer ran into the info line
            const bool emptyTrack = app.project.songChainLength == 0;
            const float editorNeed = LiveLayout{}.padY * 2 + LiveLayout{}.headerH + LiveLayout{}.footerH + 4.0f + kNumTracks * 18.0f + 2 * sp;   // v44.50: 11 rows of 18 (the sampler took 20 px from the strip above, which made its rows unreadable)
            const float stripH = emptyTrack ? std::clamp(availY - headerH - 190.0f - 2 * sp, 250.0f, 420.0f) : std::clamp(availY - headerH - trackInspH - modeRowH - 5 * sp - editorNeed, 170.0f, 420.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 4));
            ImGui::BeginChild("HeaderChildT", ImVec2(0, headerH), ImGuiChildFlags_Borders, fixedFlags);
            p.headerPanel.Draw(app);
            ImGui::EndChild();
            ImGui::PopStyleVar();
            if (app.tool != AppState::Tool::None) {   // C1: a tool takes the room under the header, over the brick that is open
                drawTool(availY - headerH - sp);
                p.chainPanel.DrawEditors(app);
                p.bottomPanel.DrawBrowsers(app);
                ImGui::PopStyleVar(2);
                ImGui::PopFont();
                ImGui::EndTabItem();
            } else {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
            ImGui::BeginChild("BricksChild", ImVec2(0, stripH), ImGuiChildFlags_Borders, fixedFlags);
            p.bricksPanel.Draw(app, p.chainPanel, true);
            ImGui::EndChild();
            ImGui::PopStyleVar();
            if (emptyTrack) {   // v44.45: no bricks, nothing to edit here: the LIVE scene is edited on LIVE only
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));
                ImGui::BeginChild("EmptyTrackChild", ImVec2(0, std::max(120.0f, availY - headerH - stripH - 2 * sp)), ImGuiChildFlags_Borders, fixedFlags);
                ImGui::TextColored(ImVec4(0.37f, 0.88f, 0.79f, 1.0f), "THE TRACK IS EMPTY");
                ImGui::Dummy(ImVec2(1, 4));
                ImGui::TextDisabled("A track is made of bricks. Make a loop on LIVE and throw it onto the track, or add a scene with + on the strip.");
                ImGui::Dummy(ImVec2(1, 10));
                if (ImGui::Button("GO TO LIVE", ImVec2(130, 30))) app.uiTabRequest = 2;
                if (ImGui::IsItemHovered()) Hint("Back to the jam. Scenes are edited on LIVE.");
                ImGui::SameLine(0, 8);
                if (ImGui::Button("THROW THE LIVE SCENE ONTO THE TRACK", ImVec2(300, 30))) {
                    SceneLock lock(app.sequencer.PatternMutex(), "LAYOUT");
                    app.ThrowIntoChain();
                }
                if (ImGui::IsItemHovered()) Hint("Makes the scene you are playing the first brick of the track.");
                ImGui::EndChild();
                ImGui::PopStyleVar();
                p.chainPanel.DrawEditors(app);
                p.bottomPanel.DrawBrowsers(app);
                ImGui::PopStyleVar(2);
                ImGui::PopFont();
                ImGui::EndTabItem();
            } else {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 5));
            ImGui::BeginChild("InspectorChild", ImVec2(0, trackInspH), ImGuiChildFlags_Borders, fixedFlags);
            p.bricksPanel.DrawInspector(app, p.chainPanel);
            ImGui::EndChild();
            ImGui::PopStyleVar();
            p.chainPanel.DrawEditors(app);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 4));
            ImGui::BeginChild("ModeRowChild", ImVec2(0, modeRowH), ImGuiChildFlags_Borders, fixedFlags);
            p.soundPanel.DrawModeRow(app);
            ImGui::EndChild();
            ImGui::PopStyleVar();
            const float aboveEditor = headerH + stripH + trackInspH + modeRowH + 2 * sp;
            const int mode = std::clamp(app.trackEditMode, 0, 2);
            if (mode == 0) {
                drawSceneEditor(availY, aboveEditor, sp, 20.0f, 30.0f, 0.0f, 0.0f);
            } else {
                const float rowH = 34.0f;
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 4));
                ImGui::BeginChild("ScopeRowChild", ImVec2(0, rowH), ImGuiChildFlags_Borders, fixedFlags);
                if (mode == 1) p.soundPanel.DrawScopeRow(app); else p.soundPanel.DrawMixNote(app);
                ImGui::EndChild();
                ImGui::PopStyleVar();
                const float restH = std::max(120.0f, availY - aboveEditor - rowH - sp);
                if (mode == 1) {
                    drawContextBlock(restH);
                } else {
                    LiveLayout layout;
                    const float overhead = layout.padY * 2 + layout.headerH + layout.footerH + 4.0f;
                    layout.rowH = std::clamp((restH - overhead) / kNumTracks, 18.0f, 34.0f);
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(layout.padX, layout.padY));
                    ImGui::BeginChild("MixViewChild", ImVec2(0, restH), ImGuiChildFlags_Borders, fixedFlags);
                    p.mixPanel.Draw(app, layout);
                    ImGui::EndChild();
                    ImGui::PopStyleVar();
                }
            }
            p.bottomPanel.DrawBrowsers(app);   // v44.26: the kit browser opened from the KIT picker of a drum on TRACK (it used to exist on LIVE only)
            ImGui::PopStyleVar(2);
            ImGui::PopFont();
            ImGui::EndTabItem();
            }
            }
        }
        // C1 (UI-1): the three tools, one look, the key next to the name; lit while open (one at a time)
        if (app.tool != AppState::Tool::MixLab) p.mixLabPanel.Leave(app);
        if (app.tool != AppState::Tool::ChordLab) p.chordLabPanel.Leave(app);
        auto toolButton = [&](const char* label, const char* key, AppState::Tool t, bool dot, const char* hint) {
            const bool on = app.tool == t;
            const ImVec4 c = toolColor(t);
            ImGui::PushStyleColor(ImGuiCol_Tab, on ? ImVec4(c.x * 0.30f, c.y * 0.30f, c.z * 0.30f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_Tab));
            ImGui::PushStyleColor(ImGuiCol_Text, on ? c : ImGui::GetStyleColorVec4(ImGuiCol_Text));
            char lb[48];
            std::snprintf(lb, sizeof lb, "%s         ###tool%s", label, key);   // the spaces leave room for the key box
            const bool hit = ImGui::TabItemButton(lb, ImGuiTabItemFlags_NoTooltip);
            ImGui::PopStyleColor(2);
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 ks = ImGui::CalcTextSize(key);
            const float kx = a.x + ImGui::GetStyle().FramePadding.x + ImGui::CalcTextSize(label).x + (dot ? 26.0f : 12.0f), ky = (a.y + b.y) * 0.5f;
            dl->AddRect(ImVec2(kx - 4.0f, ky - ks.y * 0.5f - 1.0f), ImVec2(kx + ks.x + 4.0f, ky + ks.y * 0.5f + 1.0f), vis::kLineHi, 3.0f);
            dl->AddText(ImVec2(kx, ky - ks.y * 0.5f), vis::kTextDim, key);
            if (dot) dl->AddCircleFilled(ImVec2(kx - 13.0f, ky), 3.5f, IM_COL32(94, 224, 201, 255));
            if (on) dl->AddLine(ImVec2(a.x + 2.0f, a.y + 1.0f), ImVec2(b.x - 2.0f, a.y + 1.0f), ImGui::GetColorU32(c), 2.0f);
            if (hit) app.ToggleTool(t);
            if (ImGui::IsItemHovered()) Hint("%s  Key %s; the same key or Esc closes.", hint, key);
        };
        toolButton("MIX LAB", "M", AppState::Tool::MixLab, false, "MIX LAB: where the instruments of the played scene fight for the same frequencies, and what to turn to make room.");
        toolButton("CHORD LAB", "C", AppState::Tool::ChordLab, false, "CHORD LAB: jam with the chords of the key over the running scene (LIVE) or the open brick (TRACK).");
        toolButton("MASTER FX", "F", AppState::Tool::MasterFx, app.project.fx.master.chainOn, "MASTER FX: project-wide delay / reverb / distortion / sidechain and the master chain (the dot: the chain is on).");
        {   // v44.51: what plays, in words, at the right end of the tab bar; when the tab on screen is not the one that plays, a click moves the music here
            const AppState::PlaySource ps = app.PlayingSource();
            if (ps != AppState::PlaySource::None) {
                char lb[96];
                const bool live = ps == AppState::PlaySource::Live;
                if (live) std::snprintf(lb, sizeof(lb), "PLAYING: LIVE %02d", app.sequencer.PlayPatternIndex() + 1);
                else std::snprintf(lb, sizeof(lb), "PLAYING: TRACK %d/%d", app.sequencer.SongChainPos() + 1, std::max(1, app.project.songChainLength));
                const bool elsewhere = app.PlayingElsewhere();
                if (elsewhere) std::snprintf(lb + std::strlen(lb), sizeof(lb) - std::strlen(lb), "  -  CLICK: PLAY %s", live ? "TRACK" : "LIVE");
                ImGui::PushStyleColor(ImGuiCol_Text, live ? ImVec4(0.96f, 0.64f, 0.24f, 1.0f) : ImVec4(0.37f, 0.88f, 0.79f, 1.0f));
                if (ImGui::TabItemButton((std::string(lb) + "###tabNow").c_str(), ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip) && elsewhere) {
                    // what the shown tab holds starts; the beat does not jump
                    app.PlayHere();
                }
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) Hint(elsewhere ? "The music you hear is %s. The tab on screen only shows and edits. Click: play what this tab holds instead."
                                                           : "The music you hear is %s.", live ? "the LIVE jam (a scene)" : "the TRACK (bricks)");
            }
        }
        ImGui::EndTabBar();
        if (app.uiTabRequest == tabReq) app.uiTabRequest = 0;
    }

    DrawDocumentDialogs(app);   // U8b: Save changes? / name / Open / recovery
    {   // the info line: what is under the mouse, and how to use it (U2). Drawn into the bottom strip of the shell.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        const ImVec2 a(wp.x + 6.0f, wp.y + ws.y - 4.0f - kInfoLineHeight), b(wp.x + ws.x - 6.0f, wp.y + ws.y - 4.0f);
        ImGui::PushFont(nullptr, 14.0f);
        DrawInfoLine(dl, a, b, ImGui::GetTime());
        ImGui::PopFont();
    }
    ImGui::End(); // ##Shell
    DrawHelpScreen(app);

    ImGui::PopFont();

    // U2: one cursor rule for the whole program -- the hand on everything that can be pressed, unless the control chose its
    // own cursor (resize arrows on a number, the text cursor in a field). Menus and lists keep the arrow, like macOS menus.
    {
        ImGuiContext* g = ImGui::GetCurrentContext();
        if (ImGui::GetMouseCursor() == ImGuiMouseCursor_Arrow && g->HoveredId != 0 && !g->HoveredIdIsDisabled && !io.WantTextInput &&
            !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

}

} // namespace t8::desktop
