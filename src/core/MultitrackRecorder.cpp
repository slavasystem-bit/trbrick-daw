#include "UserDirs.h"
#include "MultitrackRecorder.h"
#include "WavFile.h"
#include <algorithm>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>

namespace t8 {

void MultitrackRecorder::Init(int sampleRate) {
    sampleRate_ = sampleRate;
    ringFrames_ = std::max(4096, sampleRate * 4);
    for (int g = 0; g < kNumStemGroups; ++g) {
        ring_[g].assign(static_cast<size_t>(ringFrames_) * 2, 0.0f);
        ringWrite_[g].store(0, std::memory_order_relaxed);
        ringRead_[g].store(0, std::memory_order_relaxed);
    }
    shuttingDown_.store(false);
    drainThread_ = std::thread(&MultitrackRecorder::DrainLoop, this);
}

void MultitrackRecorder::Shutdown() {
    shuttingDown_.store(true);
    if (drainThread_.joinable()) drainThread_.join();
}

void MultitrackRecorder::Push(int numFrames, const float* const* stemBlocks) {
    if (!armed_.load(std::memory_order_acquire)) return;

    int startOffset = 0; // frames to skip at the front of THIS block, see below
    if (!started_.load(std::memory_order_relaxed)) {
        // Still armed but waiting for the first sound anywhere across the
        // 7 stems -- scan this block for the first frame where ANY stem
        // has a nonzero sample on either channel, and start capturing
        // from exactly that frame forward. Everything before it (an
        // arbitrary amount of silence depending on how long the take sat
        // armed before anything actually played) is simply never written
        // to any ring, so every stem's captured audio begins at the same
        // instant -- not just "all roughly silent for a while", but
        // sample-identical in length from here on, by construction.
        int onset = -1;
        for (int i = 0; i < numFrames; ++i) {
            for (int g = 0; g < kNumStemGroups; ++g) {
                float l = stemBlocks[g][static_cast<size_t>(i) * 2];
                float r = stemBlocks[g][static_cast<size_t>(i) * 2 + 1];
                if (l != 0.0f || r != 0.0f) { onset = i; break; }
            }
            if (onset >= 0) break;
        }
        if (onset < 0) return; // whole block still silent -- keep waiting
        started_.store(true, std::memory_order_relaxed);
        startOffset = onset;
    }

    int framesToWrite = numFrames - startOffset;
    int written = framesToWrite;
    for (int g = 0; g < kNumStemGroups; ++g) {
        const float* block = stemBlocks[g] + static_cast<size_t>(startOffset) * 2;
        int w = ringWrite_[g].load(std::memory_order_relaxed);
        int r = ringRead_[g].load(std::memory_order_acquire);
        for (int i = 0; i < framesToWrite; ++i) {
            int next = w + 1;
            if (next >= ringFrames_) next = 0;
            if (next == r) { if (g == 0) written = i; break; } // ring full (disk lagging) -- drop the rest of this tick rather than block
            ring_[g][static_cast<size_t>(w) * 2]     = block[static_cast<size_t>(i) * 2];
            ring_[g][static_cast<size_t>(w) * 2 + 1] = block[static_cast<size_t>(i) * 2 + 1];
            w = next;
        }
        ringWrite_[g].store(w, std::memory_order_release);
    }
    if (written < framesToWrite) dropped_.fetch_add(static_cast<uint64_t>(framesToWrite - written), std::memory_order_relaxed);
}

void MultitrackRecorder::DrainToFiles() {
    // Stem 0 decides how much is taken this pass, the others take the same amount (they are pushed in lockstep), so the
    // files stay exactly the same length even when a Push() lands between the loads below.
    int avail = ringWrite_[0].load(std::memory_order_acquire) - ringRead_[0].load(std::memory_order_relaxed);
    if (avail < 0) avail += ringFrames_;
    for (int g = 1; g < kNumStemGroups; ++g) {
        int a = ringWrite_[g].load(std::memory_order_acquire) - ringRead_[g].load(std::memory_order_relaxed);
        if (a < 0) a += ringFrames_;
        avail = std::min(avail, a);
    }
    for (int g = 0; g < kNumStemGroups; ++g) {
        int r = ringRead_[g].load(std::memory_order_relaxed);
        int left = avail;
        while (left > 0) {
            const int piece = std::min(left, ringFrames_ - r);
            if (wav_[g].IsOpen()) wav_[g].Write(&ring_[g][static_cast<size_t>(r) * 2], static_cast<size_t>(piece));
            left -= piece;
            r += piece;
            if (r >= ringFrames_) r = 0;
        }
        ringRead_[g].store(r, std::memory_order_release);
    }
    framesSinceFlush_ += static_cast<uint64_t>(avail);
    writtenFrames_.store(static_cast<int64_t>(wav_[0].Frames()), std::memory_order_relaxed);
    if (framesSinceFlush_ >= static_cast<uint64_t>(sampleRate_)) {   // ~once a second: a crash keeps all but the last second
        framesSinceFlush_ = 0;
        for (auto& w : wav_) w.Flush();
    }
}

void MultitrackRecorder::FinishTake() {
    bool ok = true, full = false, anyOpen = false;
    for (auto& w : wav_) {
        anyOpen = anyOpen || w.IsOpen();
        full = full || w.Full();
        const bool failed = w.Failed();
        if (!w.Close() || failed) ok = false;
    }
    const uint64_t frames = wav_[0].Frames();
    const double secs = static_cast<double>(frames) / std::max(1, sampleRate_);
    const double lost = static_cast<double>(dropped_.load()) / std::max(1, sampleRate_);
    char buf[256];
    std::string kept = folder_;
    if (!anyOpen) {
        std::snprintf(buf, sizeof(buf), "STEMS FAILED: CANNOT WRITE %s", folder_.empty() ? "THE RECORDINGS FOLDER" : shownFolder_.c_str());
        kept.clear();
    } else if (frames == 0) {   // armed, but nothing ever played: leave nothing behind
        for (int g = 0; g < kNumStemGroups; ++g) std::remove(wav_[g].Path().c_str());
        ::rmdir(folder_.c_str());
        std::snprintf(buf, sizeof(buf), "MULTITRACK RECORDING STOPPED (NOTHING CAPTURED)");
        kept.clear();
    } else if (ok) {
        int n = std::snprintf(buf, sizeof(buf), "SAVED %d STEMS (%.1fs) -> %s", kNumStemGroups, secs, shownFolder_.c_str());
        if (full && n > 0) n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " (STOPPED AT THE 4 GB WAV LIMIT)");
        if (lost > 0.0005 && n > 0) std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " (%.2fs MISSING: DISK TOO SLOW)", lost);
    } else {
        std::snprintf(buf, sizeof(buf), "STEMS: DISK ERROR, KEPT %.1fs -> %s", secs, shownFolder_.c_str());
    }
    std::lock_guard<std::mutex> lk(msgMutex_);
    resultMsg_ = buf;
    resultPath_ = kept;
    hasResult_ = true;
}

void MultitrackRecorder::DrainLoop() {
    for (;;) {
        const bool quitting = shuttingDown_.load(std::memory_order_relaxed);
        State st = state_.load(std::memory_order_acquire);
        if (st == State::Starting) {
            { std::lock_guard<std::mutex> lk(msgMutex_); folder_ = pendingFolder_; shownFolder_ = pendingShown_; }
            framesSinceFlush_ = 0;
            bool opened = !folder_.empty();
            for (int g = 0; g < kNumStemGroups && opened; ++g)
                opened = wav_[g].Open(folder_ + "/" + kStemGroupFileNames[g], 2, sampleRate_);
            if (!opened) {
                for (auto& w : wav_) { w.Close(); std::remove(w.Path().c_str()); }
                if (!folder_.empty()) ::rmdir(folder_.c_str());
                armed_.store(false, std::memory_order_release);
                FinishTake();
                state_.store(State::Idle, std::memory_order_release);
            } else {
                state_.store(State::Recording, std::memory_order_release);
            }
            st = state_.load(std::memory_order_acquire);
        }
        if (st == State::Recording) {
            bool stop = quitting || !armed_.load(std::memory_order_acquire);
            if (!stop) {
                DrainToFiles();
                for (auto& w : wav_) stop = stop || w.Full() || w.Failed();
            }
            if (stop) {
                armed_.store(false, std::memory_order_release);
                // Push() may be in the middle of one last block that saw armed_ still on: give it a moment, then take everything.
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
                DrainToFiles();
                FinishTake();
                state_.store(State::Idle, std::memory_order_release);
            }
        }
        if (quitting && state_.load(std::memory_order_acquire) == State::Idle) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

double MultitrackRecorder::ElapsedSeconds() const {
    if (sampleRate_ <= 0) return 0.0;
    return static_cast<double>(writtenFrames_.load(std::memory_order_relaxed)) / sampleRate_;
}

std::string MultitrackRecorder::Toggle(const std::string& folder) {
    if (!drainThread_.joinable()) return "STEM RECORDING IS NOT AVAILABLE";
    if (!armed_.load(std::memory_order_relaxed)) {
        if (state_.load(std::memory_order_acquire) != State::Idle) return "STILL SAVING THE LAST STEMS";
        {
            std::lock_guard<std::mutex> lk(msgMutex_);
            pendingFolder_ = folder.empty() ? userdirs::RecordingFolder(" stems") : folder;   // "<project> 2026-10-26 14.32 stems"
            pendingShown_ = userdirs::DisplayPath(pendingFolder_);                          // UI thread: userdirs is not thread-safe
        }
        // Idle: the drain thread does not touch the rings, so the read sides can be moved up to the write sides here.
        for (int g = 0; g < kNumStemGroups; ++g) ringRead_[g].store(ringWrite_[g].load(std::memory_order_acquire), std::memory_order_release);
        dropped_.store(0, std::memory_order_relaxed);
        writtenFrames_.store(0, std::memory_order_relaxed);
        started_.store(false, std::memory_order_relaxed);
        // The flag first, then the state (see Recorder::Toggle): the drain thread must see the take as armed when it opens the files.
        armed_.store(true, std::memory_order_release);
        state_.store(State::Starting, std::memory_order_release);
        return "STEMS ARMED -- WAITING FOR SOUND";
    }
    armed_.store(false, std::memory_order_release);   // the drain thread closes the files
    return "SAVING STEMS...";
}

bool MultitrackRecorder::TakeResult(std::string& message, std::string& path) {
    std::lock_guard<std::mutex> lk(msgMutex_);
    if (!hasResult_) return false;
    message = resultMsg_; path = resultPath_;
    hasResult_ = false;
    return true;
}

void MultitrackRecorder::WaitIdleForTest() {
    for (int i = 0; i < 500 && state_.load(std::memory_order_acquire) != State::Idle; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

} // namespace t8
