#pragma once
// "Fill the chain from the roles" (v44.6): the roles of the scenes (INTRO / BUILD / DROP / BREAK / OUTRO) say what a chain step
// usually does - an intro opens up from dark and quiet, a build thins out from below while it rises, a break darkens and takes
// the low end out, an outro fades away - and which transition leads into it (a reverse crash into a drop, a downlifter into a
// break). This writes those habits into the steps' overlays. Everything stays editable in the CHAIN STEP window afterwards.
#include "Project.h"
#include "ArrangeTypes.h"

namespace t8 {

struct AutoArrangeResult { int lanes = 0, mutes = 0, transitions = 0; };

// replace = false: only steps that have nothing yet (no level / filter lanes, no mutes, no transition) are filled.
inline AutoArrangeResult AutoArrangeChain(Project& proj, bool replace) {
    AutoArrangeResult res;
    const int n = std::clamp(proj.songChainLength, 0, kMaxChainSteps);
    for (int i = 0; i < n; ++i) {
        ChainStep& cs = proj.songChain[static_cast<size_t>(i)];
        const SceneRole role = proj.patterns[static_cast<size_t>(std::clamp(cs.patternIndex, 0, kNumPatterns - 1))].role;
        if (role == SceneRole::None) continue;
        const StepAuto a = RoleAuto(role, i == n - 1);
        if ((replace || !cs.automation.Any()) && !(a == cs.automation)) { cs.automation = a; ++res.lanes; }
        const uint16_t m = RoleMutes(role);
        if (m != 0 && (replace || cs.muteMask == 0) && cs.muteMask != m) { cs.muteMask = m; ++res.mutes; }
        const SceneRole prev = proj.patterns[static_cast<size_t>(std::clamp(proj.songChain[static_cast<size_t>((i + n - 1) % n)].patternIndex, 0, kNumPatterns - 1))].role;
        uint8_t bars = 2;
        const int riser = n > 1 ? RoleTransition(prev, role, &bars) : -1;
        if (riser >= 0 && (replace || cs.riser < 0) && (cs.riser != riser || cs.riserBars != bars)) { cs.riser = static_cast<int8_t>(riser); cs.riserBars = bars; ++res.transitions; }
    }
    return res;
}

} // namespace t8
