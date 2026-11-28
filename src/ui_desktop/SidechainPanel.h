#pragma once
// MIX LAB > SIDECHAIN: the editor of the sidechain pumps (PumpSettings in core/Types.h). Up to 4 pumps side by side: each has
// a SOURCE (an instrument's hits or a steady beat), ATTACK / HOLD / RELEASE, a curve, and below it one cell per instrument with
// how deep that instrument is turned down. On the right: what the pumps do to the selected instrument over two beats, quick
// start presets and how to use it. Approved mockup: mockups/sidechain/sidechain_mockup.png (2026-11-27).
#include "app/AppState.h"

namespace t8::desktop {

class SidechainPanel {
public:
    void Draw(AppState& app, float width, float height);

private:
    void DrawPump(AppState& app, std::array<PumpSettings, kNumPumps>& np, int p, float w, float h);
    void DrawCells(AppState& app, std::array<PumpSettings, kNumPumps>& np, int shown, float nameW, float colW);
    void DrawCurve(AppState& app, const std::array<PumpSettings, kNumPumps>& np, float w, float h);
    void DrawPresets(AppState& app, std::array<PumpSettings, kNumPumps>& np, float w);
    int shownExtra_ = 0;   // pumps revealed with + ADD PUMP that are still empty
    int sel_ = -1;         // the instrument shown on the right (-1 = the first one any pump reaches)
};

// The pumps differ (anything the person can change): commit + undo only then.
bool PumpsDiffer(const std::array<PumpSettings, kNumPumps>& a, const std::array<PumpSettings, kNumPumps>& b);
// A pump is in use when it is on or reaches any instrument.
bool PumpInUse(const PumpSettings& p);
const char* PumpSourceName(int source);

} // namespace t8::desktop
