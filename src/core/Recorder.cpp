#include "UserDirs.h"
#include "Recorder.h"
#include <chrono>
#include <cstdio>

namespace t8 {

void Recorder::Init(int sampleRate) {
    sampleRate_ = sampleRate;
    ringFrames_ = std::max(4096, sampleRate * 4);
    ring_.assign(static_cast<size_t>(ringFrames_) * 2, 0.0f);
    shuttingDown_.store(false);
    drainThread_ = std::thread(&Recorder::DrainLoop, this);
}

void Recorder::Shutdown() {
    shuttingDown_.store(true);
    if (drainThread_.joinable()) drainThread_.join();
}

void Recorder::Push(const float* interleavedStereo, int numFrames) {
    if (!recording_.load(std::memory_order_acquire)) return;
    int w = ringWrite_.load(std::memory_order_relaxed);
    const int r = ringRead_.load(std::memory_order_acquire);
    int i = 0;
    for (; i < numFrames; ++i) {
        int next = w + 1;
        if (next >= ringFrames_) next = 0;
        if (next == r) break; // ring full (disk lagging) -- drop the rest of this tick rather than block
        ring_[static_cast<size_t>(w) * 2]     = interleavedStereo[i * 2];
        ring_[static_cast<size_t>(w) * 2 + 1] = interleavedStereo[i * 2 + 1];
        w = next;
    }
    ringWrite_.store(w, std::memory_order_release);
    if (i < numFrames) dropped_.fetch_add(static_cast<uint64_t>(numFrames - i), std::memory_order_relaxed);
}

void Recorder::DrainToFile() {
    const int w = ringWrite_.load(std::memory_order_acquire);
    int r = ringRead_.load(std::memory_order_relaxed);
    while (r != w) {   // at most two contiguous pieces of the ring
        const int end = w > r ? w : ringFrames_;
        if (wav_.IsOpen()) wav_.Write(&ring_[static_cast<size_t>(r) * 2], static_cast<size_t>(end - r));
        framesSinceFlush_ += static_cast<uint64_t>(end - r);
        r = end >= ringFrames_ ? 0 : end;
    }
    ringRead_.store(r, std::memory_order_release);
    writtenFrames_.store(static_cast<int64_t>(wav_.Frames()), std::memory_order_relaxed);
    if (framesSinceFlush_ >= static_cast<uint64_t>(sampleRate_)) { framesSinceFlush_ = 0; wav_.Flush(); }   // ~once a second
}

void Recorder::FinishTake() {
    const bool full = wav_.Full(), failedWhileWriting = wav_.Failed();
    const bool ok = wav_.Close() && !failedWhileWriting;
    const double secs = static_cast<double>(wav_.Frames()) / std::max(1, sampleRate_);
    const double lost = static_cast<double>(dropped_.load()) / std::max(1, sampleRate_);
    char buf[256];
    if (ok) {
        int n = std::snprintf(buf, sizeof(buf), "SAVED %.1fs -> %s", secs, shownPath_.c_str());
        if (full && n > 0) n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " (STOPPED AT THE 4 GB WAV LIMIT)");
        if (lost > 0.0005 && n > 0) std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " (%.2fs MISSING: DISK TOO SLOW)", lost);
    } else if (wav_.Frames() > 0) {
        std::snprintf(buf, sizeof(buf), "RECORDING: DISK ERROR, KEPT %.1fs -> %s", secs, shownPath_.c_str());
    } else {
        std::snprintf(buf, sizeof(buf), "RECORDING FAILED: CANNOT WRITE %s", shownPath_.c_str());
    }
    std::lock_guard<std::mutex> lk(msgMutex_);
    resultMsg_ = buf;
    resultPath_ = wav_.Frames() > 0 || ok ? wav_.Path() : std::string();
    hasResult_ = true;
}

void Recorder::DrainLoop() {
    for (;;) {
        const bool quitting = shuttingDown_.load(std::memory_order_relaxed);
        State st = state_.load(std::memory_order_acquire);
        if (st == State::Starting) {
            std::string path;
            { std::lock_guard<std::mutex> lk(msgMutex_); path = pendingPath_; shownPath_ = pendingShown_; }
            framesSinceFlush_ = 0;
            if (wav_.Open(path, 2, sampleRate_)) {
                state_.store(State::Recording, std::memory_order_release);   // a stop that is already waiting is seen just below
            } else {
                recording_.store(false, std::memory_order_release);
                FinishTake();
                state_.store(State::Idle, std::memory_order_release);
            }
            st = state_.load(std::memory_order_acquire);
        }
        if (st == State::Recording && quitting) { recording_.store(false, std::memory_order_release); st = State::Stopping; }
        if (st == State::Recording && !recording_.load(std::memory_order_acquire)) st = State::Stopping;   // stopped while the file was opening
        if (st == State::Recording) {
            DrainToFile();
            if (wav_.Full() || wav_.Failed()) { recording_.store(false, std::memory_order_release); st = State::Stopping; }
        }
        if (st == State::Stopping) {
            // Push() may be in the middle of one last block that saw recording_ still on: give it a moment, then take everything.
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            DrainToFile();
            FinishTake();
            state_.store(State::Idle, std::memory_order_release);
        }
        if (quitting && state_.load(std::memory_order_acquire) == State::Idle) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

double Recorder::ElapsedSeconds() const {
    if (sampleRate_ <= 0) return 0.0;
    return static_cast<double>(writtenFrames_.load(std::memory_order_relaxed)) / sampleRate_;
}

std::string Recorder::Toggle(const std::string& path) {
    if (!drainThread_.joinable()) return "RECORDING IS NOT AVAILABLE";
    if (!recording_.load(std::memory_order_relaxed)) {
        if (state_.load(std::memory_order_acquire) != State::Idle) return "STILL SAVING THE LAST RECORDING";
        {
            std::lock_guard<std::mutex> lk(msgMutex_);
            pendingPath_ = path.empty() ? userdirs::RecordingPath("", "wav") : path;   // "<project> 2026-10-26 14.32.wav" in Documents/TR-Brick/Recordings
            pendingShown_ = userdirs::DisplayPath(pendingPath_);                         // UI thread: userdirs is not thread-safe
        }
        // Idle: the drain thread does not touch the ring, so the read side can be moved up to the write side here.
        ringRead_.store(ringWrite_.load(std::memory_order_acquire), std::memory_order_release);
        dropped_.store(0, std::memory_order_relaxed);
        writtenFrames_.store(0, std::memory_order_relaxed);
        // The flag first, then the state: the drain thread acts on Starting and must already see the take as running (else it
        // would open, see "stopped", close an empty take and leave REC on with nobody draining).
        recording_.store(true, std::memory_order_release);   // the ring fills from this moment, the file opens on the drain thread
        state_.store(State::Starting, std::memory_order_release);
        return "RECORDING";
    }
    // The drain thread sees recording_ off and closes the take (if the file is still opening, it opens it first).
    recording_.store(false, std::memory_order_release);
    return "SAVING RECORDING...";
}

bool Recorder::TakeResult(std::string& message, std::string& path) {
    std::lock_guard<std::mutex> lk(msgMutex_);
    if (!hasResult_) return false;
    message = resultMsg_; path = resultPath_;
    hasResult_ = false;
    return true;
}

void Recorder::WaitIdleForTest() {
    for (int i = 0; i < 500 && state_.load(std::memory_order_acquire) != State::Idle; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

} // namespace t8
