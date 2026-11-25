#pragma once
// A rough "how much is going on" number for a scene as a chain step plays it (v44.5), 0..1. It only drives the energy line in the
// chain panel: the share of the weighted tracks that play (kick and bass count most), each scaled by how busy it is, with the tracks
// muted by the step's mute mask left out. A drop with everything playing is near 1, a break without kick and bass is low.
#include "Types.h"
#include <algorithm>

namespace t8 {

inline float SceneEnergy(const Pattern& p, uint16_t muteMask) {
    static const float kWeight[kNumTracks] = {3.0f, 1.5f, 1.2f, 1.5f, 0.8f, 1.0f, 3.0f, 1.5f, 1.5f, 1.5f, 1.2f};
    float sum = 0.0f, tot = 0.0f;
    for (int i = 0; i < kNumTracks; ++i) {
        tot += kWeight[i];
        if ((muteMask >> i) & 1u) continue;
        const Track& t = p.tracks[static_cast<size_t>(i)];
        const int len = std::clamp(t.useOwnLength ? t.length : p.masterLength, 1, kMaxSteps);
        int n = 0;
        for (int k = 0; k < len; ++k) if (t.steps[static_cast<size_t>(k)].active) ++n;
        if (n == 0) continue;
        const float density = std::min(1.0f, 3.0f * static_cast<float>(n) / static_cast<float>(len));
        sum += kWeight[i] * (0.5f + 0.5f * density);          // a track that plays at all counts half, a busy one fully
    }
    return tot > 0.0f ? std::clamp(sum / tot, 0.0f, 1.0f) : 0.0f;
}

} // namespace t8
