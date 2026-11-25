#pragma once
// v44.52.1: finds the hits (onsets) of a sample for the sampler's SLICES: an energy envelope in short steps, a hit is a steep rise of
// that envelope over what was there just before. No tempo is assumed, so it works on a break, a chopped vocal or a plucked riff alike.
// `sensitivity` 0..1: 1 also finds soft hits, 0 only the loud ones. At most `maxCount` hits are returned (the strongest ones),
// sorted by position, as frame numbers inside [from, to).
#include <algorithm>
#include <cmath>
#include <vector>

namespace t8 {

inline std::vector<int> DetectOnsets(const float* data, int frames, int channels, int sampleRate, int from, int to, float sensitivity, int maxCount = 32) {
    std::vector<int> out;
    channels = std::max(1, channels);
    from = std::clamp(from, 0, std::max(0, frames)); to = std::clamp(to, from, std::max(0, frames));
    const int hop = std::max(32, sampleRate / 345);          // ~2.9 ms
    const int win = hop * 4;
    if (!data || to - from < win * 4) return out;
    const int n = (to - from - win) / hop;
    std::vector<double> env(static_cast<size_t>(n), 0.0);
    double mx = 0.0;
    for (int k = 0; k < n; ++k) {
        double e = 0.0; const int s0 = from + k * hop;
        float prev = 0.0f;
        for (int i = 0; i < win; ++i) {
            float v = 0.0f; for (int c = 0; c < channels; ++c) v += data[static_cast<size_t>(s0 + i) * static_cast<size_t>(channels) + static_cast<size_t>(c)];
            v /= static_cast<float>(channels);
            const float d = v - prev; prev = v;   // the first difference leans on the bright, sudden part of a sound (a hit), not on a low sustain
            e += static_cast<double>(d) * d + 0.1 * static_cast<double>(v) * v;
        }
        env[static_cast<size_t>(k)] = e / win; mx = std::max(mx, env[static_cast<size_t>(k)]);
    }
    if (mx <= 1e-12) return out;
    const double thr = 2.0 * std::pow(6.0, 1.0 - std::clamp(static_cast<double>(sensitivity), 0.0, 1.0));   // rise needed: x2 (sensitive) .. x12
    const int back = 8, around = 6, minGap = std::max(1, static_cast<int>(0.05 * sampleRate / hop));
    struct Hit { int k; double strength; };
    std::vector<Hit> hits;
    if (env[0] >= mx * 0.1) hits.push_back({0, 1e9});   // the sample starts with a hit
    for (int k = 1; k < n; ++k) {
        double prevAvg = 0.0; int cnt = 0;
        for (int j = std::max(0, k - back); j < k; ++j) { prevAvg += env[static_cast<size_t>(j)]; ++cnt; }
        prevAvg = prevAvg / std::max(1, cnt) + mx * 1e-4;
        const double r = env[static_cast<size_t>(k)] / prevAvg;
        if (r < thr || env[static_cast<size_t>(k)] < mx * 0.003) continue;
        bool peak = true;
        for (int j = std::max(1, k - around); j <= std::min(n - 1, k + around) && peak; ++j) {
            if (j == k) continue;
            double pa = 0.0; int pc = 0; for (int q = std::max(0, j - back); q < j; ++q) { pa += env[static_cast<size_t>(q)]; ++pc; }
            pa = pa / std::max(1, pc) + mx * 1e-4;
            const double rj = env[static_cast<size_t>(j)] / pa;
            if (rj > r || (rj == r && j < k)) peak = false;
        }
        if (!peak) continue;
        if (!hits.empty() && k - hits.back().k < minGap) { if (r > hits.back().strength) hits.back() = {k, r}; continue; }
        hits.push_back({k, r});
    }
    if (static_cast<int>(hits.size()) > maxCount) {
        std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.strength > b.strength; });
        hits.resize(static_cast<size_t>(maxCount));
        std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.k < b.k; });
    }
    const int pre = std::max(1, sampleRate / 1000);   // 1 ms before the rise: the attack is not cut
    for (const Hit& h : hits) {
        // the window that first shows the rise starts a little before the hit: the hit is the first sudden step inside it
        const int s0 = from + h.k * hop, e0 = std::min(to - 2, s0 + win + hop);
        float peak = 0.0f; std::vector<float> d;
        for (int i = s0; i < e0; ++i) {
            float a = 0.0f, b = 0.0f; for (int c = 0; c < channels; ++c) { a += data[static_cast<size_t>(i) * static_cast<size_t>(channels) + static_cast<size_t>(c)]; b += data[static_cast<size_t>(i + 1) * static_cast<size_t>(channels) + static_cast<size_t>(c)]; }
            d.push_back(std::fabs(b - a) / static_cast<float>(channels)); peak = std::max(peak, d.back());
        }
        int at = 0; for (size_t i = 0; i < d.size(); ++i) if (d[i] >= peak * 0.25f) { at = static_cast<int>(i); break; }
        out.push_back(std::clamp(s0 + at - pre, from, to - 1));
    }
    return out;
}

} // namespace t8
