#pragma once
// LANES: the editor that replaces the mixer on the LIVE screen while a DRUM track is selected (v40).
// A tall bar lane for the selected track on the 16-step page shared with the sequencer: draw the VELOCITY of every
// hit, or its NUDGE (microtiming). X MIXER brings the mixer back. The same lane widget (StepLane.h) sits under the
// piano roll for synth / chord tracks. The SWING tab shows the swing of the pattern / track. (The groove has its own window since v44.3: GroovePanel.)
#include "app/AppState.h"
#include "LiveLayout.h"
#include "StepLane.h"

namespace t8::desktop {

class LanePanel {
public:
    void Draw(AppState& app, const LiveLayout& layout);

private:
    LaneKind kind_ = LaneKind::Velocity;
    bool swingTab_ = false;   // swing of the pattern / of this track + a timing preview
    void DrawSwingTab(AppState& app, int ti, float top, float height, float innerW, const PageGeom& geom, int trackLen);
    LaneState state_;
    float humanizePct_ = 15.0f;
};

} // namespace t8::desktop
