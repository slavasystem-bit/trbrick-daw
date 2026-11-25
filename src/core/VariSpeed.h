#pragma once
// v44.57: PITCH, a turntable-style speed control for the WHOLE output.
//
// The engine always renders at its own steady clock; this stage sits between the engine and the sound card and
// plays what the engine rendered a little faster or slower (band-limited sinc interpolation), exactly like a
// record on a deck whose pitch fader is moved: tempo and pitch move together, and everything follows (drums,
// synths, loops, samples, delay and reverb tails). Speed 1.0 is a plain copy, bit for bit.
//
// Audio-thread safe: no allocation after construction, no locks. The engine is asked for exactly the frames the
// interpolation needs, so the stage adds no delay at speed 1.0 and about half a millisecond of look-ahead while
// the speed is not 1.0.
#include "core/Resample.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace t8 {

class VariSpeed {
public:
    static constexpr int kCapacityFrames = 1 << 16;   // the most we ever hold: far above any block * 1.5
    static constexpr int kSubBlock = 2048;            // positions are planned this many output frames at a time

    VariSpeed() : table_(32, 256, 0.88), buf_(static_cast<size_t>(kCapacityFrames) * 2, 0.0f) {}

    void Reset() { count_ = 0; p_ = 0.0; cur_ = 1.0; }
    double CurrentSpeed() const { return cur_; }

    // out: n interleaved stereo frames. target: the wanted speed (1.0 = as rendered; 1.05 = 5% faster and higher).
    // render(float* dst, int frames): fills `frames` interleaved stereo frames with the next engine audio.
    template <class RenderFn>
    void Process(float* out, int n, double target, int sampleRate, RenderFn&& render) {
        target = std::clamp(target, 0.5, 1.5);
        const double k = 1.0 - std::exp(-1.0 / (0.020 * std::max(8000, sampleRate)));   // ~20 ms: a fader has weight
        const int lead = table_.Lead();
        int done = 0;
        while (done < n) {
            const int m = std::min(kSubBlock, n - done);
            // 1. plan where each output frame reads from
            bool allExact = true;
            for (int i = 0; i < m; ++i) {
                bool exact = false;
                double step = 1.0;
                cur_ += (target - cur_) * k;
                if (target == 1.0 && std::fabs(cur_ - 1.0) < 1e-5) {
                    // heading home: land on a whole frame again so the plain copy (no filter) takes over
                    cur_ = 1.0;
                    const double fl = std::floor(p_), frac = p_ - fl;
                    if (frac == 0.0) exact = true;
                    else if (frac < 0.5) { if (frac <= 0.002) step = 1.0 - frac; else step = 1.0 - 0.002; }
                    else { if (1.0 - frac <= 0.002) step = 2.0 - frac; else step = 1.0 + 0.002; }
                } else {
                    step = cur_;
                }
                pos_[i] = p_;
                ex_[i] = exact ? 1 : 0;
                if (!exact) allExact = false;
                p_ += step;
            }
            // 2. make sure the engine has rendered everything those reads touch
            const int need = static_cast<int>(std::floor(pos_[m - 1])) + (allExact ? 1 : lead + 2);   // plain copies need no look-ahead
            if (need > kCapacityFrames) { Reset(); std::memset(out + static_cast<size_t>(done) * 2, 0, sizeof(float) * m * 2); done += m; continue; }
            if (need > count_) {
                render(buf_.data() + static_cast<size_t>(count_) * 2, need - count_);
                count_ = need;
            }
            // 3. read
            float* o = out + static_cast<size_t>(done) * 2;
            for (int i = 0; i < m; ++i) {
                const int base = static_cast<int>(pos_[i]);
                if (ex_[i]) {
                    o[2 * i] = buf_[static_cast<size_t>(base) * 2];
                    o[2 * i + 1] = buf_[static_cast<size_t>(base) * 2 + 1];
                } else {
                    const double frac = pos_[i] - base;
                    o[2 * i] = table_.At(buf_.data(), count_, 2, 0, base, frac);
                    o[2 * i + 1] = table_.At(buf_.data(), count_, 2, 1, base, frac);
                }
            }
            // 4. drop what no later read can touch (keep the history the kernel looks back on)
            const int drop = static_cast<int>(std::floor(p_)) - lead - 1;
            if (drop > 0 && drop <= count_) {
                std::memmove(buf_.data(), buf_.data() + static_cast<size_t>(drop) * 2, sizeof(float) * static_cast<size_t>(count_ - drop) * 2);
                count_ -= drop;
                p_ -= drop;
            }
            done += m;
        }
    }

private:
    SincTable table_;
    std::vector<float> buf_;
    int count_ = 0;            // frames held in buf_
    double p_ = 0.0;           // read position (frames) of the next output frame
    double cur_ = 1.0;         // the speed now (follows the target with some weight)
    double pos_[kSubBlock];
    unsigned char ex_[kSubBlock];
};

} // namespace t8
