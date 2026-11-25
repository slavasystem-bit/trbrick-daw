#include "UserDirs.h"
#include "Recorder.h"
#include "WavFile.h"
#include <chrono>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>

namespace t8 {

void Recorder::Init(int sampleRate) {
    sampleRate_ = sampleRate;
    ring_.assign(static_cast<size_t>(kRingCapacityFrames) * 2, 0.0f);
    shuttingDown_.store(false);
    drainThread_ = std::thread(&Recorder::DrainLoop, this);
}

void Recorder::Shutdown() {
    shuttingDown_.store(true);
    if (drainThread_.joinable()) drainThread_.join();
}

void Recorder::Push(const float* interleavedStereo, int numFrames) {
    if (!recording_.load(std::memory_order_relaxed)) return;
    int w = ringWrite_.load(std::memory_order_relaxed);
    int r = ringRead_.load(std::memory_order_acquire);
    for (int i = 0; i < numFrames; ++i) {
        int next = w + 1;
        if (next >= kRingCapacityFrames) next = 0;
        if (next == r) break; // ring full (consumer lagging) -- drop the rest of this tick rather than block
        ring_[static_cast<size_t>(w) * 2]     = interleavedStereo[i * 2];
        ring_[static_cast<size_t>(w) * 2 + 1] = interleavedStereo[i * 2 + 1];
        w = next;
    }
    ringWrite_.store(w, std::memory_order_release);
}

void Recorder::DrainRingIntoCaptured() {
    int w = ringWrite_.load(std::memory_order_acquire);
    int r = ringRead_.load(std::memory_order_relaxed);
    while (r != w) {
        captured_.push_back(ring_[static_cast<size_t>(r) * 2]);
        captured_.push_back(ring_[static_cast<size_t>(r) * 2 + 1]);
        r = r + 1;
        if (r >= kRingCapacityFrames) r = 0;
    }
    ringRead_.store(r, std::memory_order_release);
    capturedFrames_.store(static_cast<int64_t>(captured_.size() / 2), std::memory_order_relaxed);
}

void Recorder::DrainLoop() {
    while (!shuttingDown_.load(std::memory_order_relaxed)) {
        if (recording_.load(std::memory_order_relaxed)) {
            DrainRingIntoCaptured();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

double Recorder::ElapsedSeconds() const {
    if (sampleRate_ <= 0) return 0.0;
    return static_cast<double>(capturedFrames_.load(std::memory_order_relaxed)) / sampleRate_;
}

std::string Recorder::Toggle() {
    if (!recording_.load(std::memory_order_relaxed)) {
        captured_.clear();
        captured_.reserve(static_cast<size_t>(sampleRate_) * 2 * 5); // headstart for ~5s, avoids early reallocs
        ringWrite_.store(0, std::memory_order_relaxed);
        ringRead_.store(0, std::memory_order_relaxed);
        capturedFrames_.store(0, std::memory_order_relaxed);
        recording_.store(true, std::memory_order_relaxed);
        return "RECORDING";
    }

    // Stop: one more drain pass (on THIS thread -- the caller, never the
    // audio thread) so the last ~20ms isn't lost to the background
    // thread's sleep interval, then write the whole thing out.
    recording_.store(false, std::memory_order_relaxed);
    DrainRingIntoCaptured();

    double secs = ElapsedSeconds();
    const std::string path = userdirs::RecordingPath("", "wav");   // "<project> 2026-10-26 14.32.wav" in Documents/TR-Brick/Recordings
    bool ok = WriteWavPCM16(path, captured_, 2, sampleRate_);
    if (ok) userdirs::SetLastOutput(path);
    captured_.clear();
    captured_.shrink_to_fit();

    char buf[192];
    if (ok) {
        std::snprintf(buf, sizeof(buf), "SAVED %.1fs -> %s", secs, userdirs::DisplayPath(path).c_str());
    } else {
        std::snprintf(buf, sizeof(buf), "RECORDING FAILED TO SAVE (%.1fs lost)", secs);
    }
    return buf;
}

} // namespace t8
