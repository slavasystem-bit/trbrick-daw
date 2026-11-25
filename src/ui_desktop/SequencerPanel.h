#pragma once
// Mouse-driven step sequencer grid. Edits go through AppState::ToggleStep and
// the MixerSync.h helpers, so there is only ever one copy of the pattern.
#include "app/AppState.h"
#include "LiveLayout.h"

namespace t8::desktop {

class SequencerPanel {
public:
    // Draws the whole panel (transport-adjacent pattern/bpm/swing/length
    // readout, then the scrollable step grid) and handles every mouse
    // interaction with it -- click a step to toggle it, click-and-drag
    // across steps to paint a run of them to the same on/off state, click
    // a track's M/S buttons to mute/solo. Call once per frame from inside
    // an open ImGui window (main_desktop.cpp hosts it in its own dockable
    // "Sequencer" window).
    void Draw(AppState& app, const LiveLayout& layout);

private:
    // Drag-paint state: while the mouse stays down, every NEW step cell
    // the cursor moves over during that same drag is set (not toggled) to
    // whatever on/off value the FIRST cell of the drag ended up at -- the
    // standard "paint a run of steps" gesture every grid-based sequencer
    // UI uses, rather than naively toggling every cell the cursor happens
    // to cross (which would flicker steps on and off again if the drag
    // backtracks over a cell it already passed).
    bool dragPainting_ = false;
    bool dragPaintValue_ = false;
    int dragLastTrack_ = -1;
    int dragLastStep_ = -1;

    // Drag-to-move: pressing on an ACTIVE step does not erase it at once; if
    // the mouse leaves the cell while held, the step is carried to the cell
    // it is released on (Ctrl = copy), otherwise a plain click erases it.
    bool moveCandidate_ = false;
    bool moveActive_ = false;
    int moveSrcTrack_ = -1;
    int moveSrcStep_ = -1;
    // Drag-to-resize of a note's length (grip at the right end of its bar).
    bool resizing_ = false;
    bool resizeUndone_ = false;   // PushUndo() taken for this drag already
    int resizeTrack_ = -1;
    int resizeStep_ = -1;
    int hoverTrack_ = -1;   // cell under the mouse this frame (-1 = none)
    int hoverStep_ = -1;

    // Which cell the ratchet/probability/accent/variant/length/slide
    // popup (StepEditPopup.h) is currently showing -- set on right-click,
    // -1/-1 until the first one. popupOpenRequest_ is a one-frame pulse
    // ("the user just right-clicked, (re)open the popup now") consumed at
    // the end of Draw() every frame, separate from popupTrack_/popupStep_
    // which stay put so the popup keeps rendering the right cell for as
    // long as ImGui itself still has it open.
    int popupTrack_ = -1;
    int popupStep_ = -1;
    bool popupOpenRequest_ = false;
};

} // namespace t8::desktop
