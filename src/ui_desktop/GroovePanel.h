#pragma once
// v44.3: the GROOVE window of the whole pattern. It opens from the GROOVE button in the header and takes the place of the mixer /
// piano roll / lanes on whatever track is selected, so the drawing is always visible. The mechanic is the classic one: CHOOSE a
// groove from the list (or take the feel of the selected track), set the AMOUNT, then press BAKE - only then the groove is
// written into the steps of the tracks that follow it. The velocity (top) and timing (bottom) of the 16 positions are drawn with
// the mouse. X CLOSE brings the mixer back.
#include "app/AppState.h"
#include "LiveLayout.h"

namespace t8::desktop {

class GroovePanel {
public:
    void Draw(AppState& app, const LiveLayout& layout);

private:
    char grooveName_[48] = "";
    bool drag_ = false;
    bool dragUndone_ = false;
};

} // namespace t8::desktop
