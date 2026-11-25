#pragma once
// Mouse panel for the project-wide MASTER FX bus (delay / reverb / distortion /
// sidechain). Writes the FxSettings fields directly with absolute-drag knobs.
// Per-track sends and insert FX live in the mixer / context block.
#include "app/AppState.h"

namespace t8::desktop {

class FxPanel {
public:
    void Draw(AppState& app);
};

} // namespace t8::desktop
