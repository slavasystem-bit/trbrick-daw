#pragma once
// The whole app screen as one call (see Shell.cpp). The panels keep their own state between frames, so they live here, owned by
// whoever runs the frames: main.cpp (the program) or tools/uitest_main.cpp (the UI test).
#include "app/AppState.h"
#include "ui_desktop/DesktopTheme.h"
#include "ui_desktop/HeaderPanel.h"
#include "ui_desktop/MenuBar.h"
#include "ui_desktop/ChainPanel.h"
#include "ui_desktop/ChordLabPanel.h"
#include "ui_desktop/BricksPanel.h"
#include "ui_desktop/SequencerPanel.h"
#include "ui_desktop/MixerPanel.h"
#include "ui_desktop/MixLabPanel.h"
#include "ui_desktop/LanePanel.h"
#include "ui_desktop/GroovePanel.h"
#include "ui_desktop/PianoRollPanel.h"
#include "ui_desktop/ContextPanel.h"
#include "ui_desktop/SoundPanel.h"
#include "ui_desktop/BottomPanel.h"

namespace t8::desktop {

struct ShellPanels {
    HeaderPanel headerPanel;
    MenuBar menuBar;
    ChainPanel chainPanel;
    BricksPanel bricksPanel;
    SequencerPanel seqPanel;
    MixerPanel mixPanel;
    MixLabPanel mixLabPanel;
    ChordLabPanel chordLabPanel;
    LanePanel lanePanel;
    GroovePanel groovePanel;
    PianoRollPanel pianoRollPanel;
    ContextPanel contextPanel;
    SoundPanel soundPanel;
    BottomPanel bottomPanel;
    int lastSelTrack = -1;   // the piano roll / lanes reopen when another track is selected
};

// Draws one frame of the screen. Call between ImGui::NewFrame() and ImGui::Render().
void DrawShellFrame(AppState& app, ShellPanels& p, const DesktopFonts& fonts);

} // namespace t8::desktop
