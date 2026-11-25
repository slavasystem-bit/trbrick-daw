#pragma once
// v44.24 (S2): the bottom area of the TRACK screen has three views of the selected brick - NOTES (the editor), SOUND (the sound of an
// instrument: for the whole track or for this brick only) and MIX. This draws the strips above the view: the mode switch with the
// instrument chips, and for SOUND the scope row. The SOUND editor itself is the same instrument block as on LIVE (ContextPanel).
#include "app/AppState.h"

namespace t8::desktop {

class SoundPanel {
public:
    static constexpr float kRowH = 26.0f;
    // Row 1: NOTES | SOUND | MIX; in SOUND also the ten instrument chips (a dot on a chip = the brick has its own sound for it).
    void DrawModeRow(AppState& app);
    // Row 2 (SOUND): which instrument, WHOLE TRACK / THIS BRICK ONLY, the drum kit's scope, APPLY TO ALL BRICKS.
    void DrawScopeRow(AppState& app);
    // One line under the mode row in MIX.
    void DrawMixNote(AppState& app);
};

} // namespace t8::desktop
