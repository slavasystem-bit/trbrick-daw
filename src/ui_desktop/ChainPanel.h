#pragma once
// The LIVE screen's second row: a full-width Scenes + Chain block, sized
// like the transport header above it.
//
//   SCENES  [01][02] ... [16]        <- the 16 pattern banks (click = switch,
//                                       right-click = duplicate / add to chain)
//   CHAIN   [slot][slot] ... [slot]  <- up to 16 visible song-chain slots, each
//                                       showing its pattern number and a -/+
//                                       loop-count stepper
//                                                     [CHAIN: ON] [CLEAR]
//
// This is a mouse front-end onto the song chain:
// Project::songChain / songChainLength (persisted) and
// Sequencer::SetSongModeActive / SongChainPos (ephemeral transport state).
// Song mode always loops back to the first slot after the last one, so there
// is no separate loop switch. The chain holds up to kMaxChainSteps (64)
// slots; a PAGE button cycles through the 16-slot pages when it is longer
// than 16.
#include "app/AppState.h"

namespace t8::desktop {

class ChainPanel {
public:
    void Draw(AppState& app);
    // v44.9: the TRACK tab (BricksPanel) opens the step editor and draws its windows through these
    void DrawEditors(AppState& app);

private:
    // Scene-pad right-click menu (same deferred-open trick as before: the
    // popup must be opened from the same ImGui ID scope it is drawn in).
    int padMenuPattern_ = -1;
    bool padMenuOpenRequest_ = false;
    // Chain-slot right-click menu.
    int slotMenuIndex_ = -1;
    bool slotMenuOpenRequest_ = false;
    // Chord-bank slot right-click menu.
    int chordMenuSlot_ = -1;
    bool chordMenuOpenRequest_ = false;
    char chordRenameBuf_[16] = {0};
    // v44.5: the CHAIN STEP editor (mutes, fill, transition, trim) and the scene's fill window
    int sceneFillIndex_ = -1;
    bool sceneFillOpenRequest_ = false;
    void DrawSceneFillEditor(AppState& app);
};

} // namespace t8::desktop
