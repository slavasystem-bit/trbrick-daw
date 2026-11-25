#pragma once
// The LIVE screen's top transport header, one row: PLAY, REC, BPM, PATTERN,
// KEY, SCALE, LENGTH, SWING, GRID, GROOVE, CHAIN, UNDO (MASTER FX sits next to the
// LIVE tab; the document is in the menu bar since v44.19), plus status / clip readout at
// the right edge. Replaces the old ImGui main-menu-bar transport and the
// per-panel BPM/LEN/SWING drags that used to sit above the sequencer.
#include "app/AppState.h"
#include "FxPanel.h"

namespace t8::desktop {

class HeaderPanel {
public:
    void Draw(AppState& app);
    // The MASTER FX button + its popover; call inside the main tab bar, after the LIVE tab.
    void DrawMenus(AppState& app);
    // The PITCH strip (v44.57), right-aligned at screen x `rightX`; call just before the main tab bar and put the cursor back.
    void DrawPitch(AppState& app, float rightX);

private:
    FxPanel fxPanel_;
};

} // namespace t8::desktop
