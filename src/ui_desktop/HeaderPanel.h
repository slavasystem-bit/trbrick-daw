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
    void DrawMasterFx(AppState& app);   // C1: the MASTER FX tool body (main draws it under the header)
    // The PITCH strip (v44.57), right-aligned at screen x `rightX`; call just before the main tab bar and put the cursor back.
    // Returns the screen x where the strip starts (N1: the dropout readout goes left of it).
    float DrawPitch(AppState& app, float rightX);
    // N1: DSP load and dropouts (XRUN), right-aligned to rightX in the tab row.
    void DrawDropouts(AppState& app, float rightX);

private:
    FxPanel fxPanel_;
    // N1 (LIVE-5): share of the last second in which the screen (or a loader) held the scene lock, see DrawDropouts
    float heldShare_ = 0.0f;
    unsigned lastShareMs_ = 0;
    unsigned long long lastHeldTotalUs_ = 0;
};

} // namespace t8::desktop
