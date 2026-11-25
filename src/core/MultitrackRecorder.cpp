#include "UserDirs.h"
#include "MultitrackRecorder.h"
#include "WavFile.h"
#include "ZipWriter.h"
#include <chrono>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>

namespace t8 {

void MultitrackRecorder::Init(int sampleRate) {
    sampleRate_ = sampleRate;
    for (int g = 0; g < kNumStemGroups; ++g) {
        ring_[g].assign(static_cast<size_t>(kRingCapacityFrames) * 2, 0.0f);
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
    if (!armed_.load(std::memory_order_relaxed)) return;

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
    for (int g = 0; g < kNumStemGroups; ++g) {
        const float* block = stemBlocks[g] + static_cast<size_t>(startOffset) * 2;
        int w = ringWrite_[g].load(std::memory_order_relaxed);
        int r = ringRead_[g].load(std::memory_order_acquire);
        for (int i = 0; i < framesToWrite; ++i) {
            int next = w + 1;
            if (next >= kRingCapacityFrames) next = 0;
            if (next == r) break; // ring full (consumer lagging) -- drop the rest of this tick rather than block
            ring_[g][static_cast<size_t>(w) * 2]     = block[static_cast<size_t>(i) * 2];
            ring_[g][static_cast<size_t>(w) * 2 + 1] = block[static_cast<size_t>(i) * 2 + 1];
            w = next;
        }
        ringWrite_[g].store(w, std::memory_order_release);
    }
}

void MultitrackRecorder::DrainRingIntoCaptured() {
    for (int g = 0; g < kNumStemGroups; ++g) {
        int w = ringWrite_[g].load(std::memory_order_acquire);
        int r = ringRead_[g].load(std::memory_order_relaxed);
        while (r != w) {
            captured_[g].push_back(ring_[g][static_cast<size_t>(r) * 2]);
            captured_[g].push_back(ring_[g][static_cast<size_t>(r) * 2 + 1]);
            r = r + 1;
            if (r >= kRingCapacityFrames) r = 0;
        }
        ringRead_[g].store(r, std::memory_order_release);
    }
    // All 7 stems are pushed in lockstep (same numFrames each call), so
    // stem 0's frame count stands in for all of them.
    capturedFrames_.store(static_cast<int64_t>(captured_[0].size() / 2), std::memory_order_relaxed);
}

void MultitrackRecorder::DrainLoop() {
    while (!shuttingDown_.load(std::memory_order_relaxed)) {
        if (armed_.load(std::memory_order_relaxed)) {
            DrainRingIntoCaptured();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

double MultitrackRecorder::ElapsedSeconds() const {
    if (sampleRate_ <= 0) return 0.0;
    return static_cast<double>(capturedFrames_.load(std::memory_order_relaxed)) / sampleRate_;
}

std::string MultitrackRecorder::Toggle() {
    if (!armed_.load(std::memory_order_relaxed)) {
        for (int g = 0; g < kNumStemGroups; ++g) {
            captured_[g].clear();
            captured_[g].reserve(static_cast<size_t>(sampleRate_) * 2 * 5); // headstart for ~5s, avoids early reallocs
            ringWrite_[g].store(0, std::memory_order_relaxed);
            ringRead_[g].store(0, std::memory_order_relaxed);
        }
        capturedFrames_.store(0, std::memory_order_relaxed);
        started_.store(false, std::memory_order_relaxed);
        armed_.store(true, std::memory_order_relaxed);
        return "STEMS ARMED -- WAITING FOR SOUND";
    }

    // Stop: one more drain pass (on THIS thread -- the caller, never the
    // audio thread) so the last ~20ms isn't lost to the background
    // thread's sleep interval, then write everything out.
    armed_.store(false, std::memory_order_relaxed);
    DrainRingIntoCaptured();

    double secs = ElapsedSeconds();
    if (secs <= 0.0) {
        return "MULTITRACK RECORDING STOPPED (NOTHING CAPTURED)";
    }

    const std::string zipPath = userdirs::RecordingPath(" stems", "zip");

    std::vector<ZipEntry> entries;
    entries.reserve(kNumStemGroups);
    for (int g = 0; g < kNumStemGroups; ++g) {
        entries.push_back(ZipEntry{kStemGroupFileNames[g], BuildWavPCM16Bytes(captured_[g], 2, sampleRate_)});
    }
    bool ok = WriteStoreZip(zipPath, entries);
    if (ok) userdirs::SetLastOutput(zipPath);

    for (auto& c : captured_) { c.clear(); c.shrink_to_fit(); }

    char buf[192];
    if (ok) {
        std::snprintf(buf, sizeof(buf), "SAVED %d STEMS (%.1fs) -> %s", kNumStemGroups, secs, userdirs::DisplayPath(zipPath).c_str());
    } else {
        std::snprintf(buf, sizeof(buf), "MULTITRACK EXPORT FAILED (%.1fs lost)", secs);
    }
    return buf;
}

} // namespace t8
