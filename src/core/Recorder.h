#pragma once
// Live WAV recording of the master audio output, for bouncing a
// performance to a file (live-performance feature). AudioCallback
// (AppBootstrap.h) calls Push() every audio-thread tick with exactly what it
// just rendered; a dedicated background thread drains a lock-free ring
// buffer, so the audio thread itself never does a heap allocation, a mutex
// wait, or (especially) disk I/O.
//
// N1 (audit v44.58 LIVE-1 / LIVE-3 / LIVE-7): the drain thread now writes the
// take straight into the WAV file while it is recorded (WavStream.h) instead
// of growing an in-memory buffer that STOP wrote out on the UI thread. So:
// STOP costs the UI thread nothing (the file is closed on the drain thread and
// the result arrives through TakeResult()), a one-hour set needs a few MB of
// memory instead of 1.3 GB, a crash leaves the file up to the last second,
// and only the drain thread ever touches the file and the read side of the
// ring (the old final drain on the UI thread raced with the drain thread).
//
// MP3/etc conversion is deliberately NOT done here -- no encoder library
// is vendored in this build (see README's recording section for why), so
// this ships WAV-only for now; convert externally if you need another
// format.
#include "WavStream.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace t8 {

class Recorder {
public:
    ~Recorder() { Shutdown(); }

    // Starts the background drain thread. Call once at startup, after
    // AudioEngine::Init (needs the sample rate) -- cheap and safe to call
    // even if recording is never used this session.
    void Init(int sampleRate);
    // Finishes a take that is still running (the file is closed properly),
    // then stops and joins the background thread. Safe to call even if
    // Init() was never called, and safe to call more than once.
    void Shutdown();

    // Audio-thread-safe: allocation-free, lock-free, bounded work only.
    // Called every AudioCallback tick with exactly what was just rendered
    // (interleaved stereo float, `numFrames` frames). A no-op (besides one
    // atomic load) whenever not currently recording.
    void Push(const float* interleavedStereo, int numFrames);

    // Starts or stops a recording -- what the REC button calls (UI thread).
    // Returns at once with a short message for SetStatus ("RECORDING",
    // "SAVING RECORDING..."); the final "SAVED ..." comes from TakeResult().
    // `path` empty = a new stamped name in Documents/TR-Brick/Recordings.
    std::string Toggle(const std::string& path = std::string());

    // UI thread, once per frame: true once when a take has been closed
    // (or failed); `message` for SetStatus, `path` of the file ("" when
    // nothing was written) for the "last file" menu.
    bool TakeResult(std::string& message, std::string& path);

    // True from the start press until the stop press.
    bool IsRecording() const { return recording_.load(std::memory_order_relaxed); }
    // True while the last take is still being closed (or the drain thread has not opened the new one yet).
    bool IsBusy() const { return state_.load(std::memory_order_acquire) != State::Idle && !IsRecording(); }
    double ElapsedSeconds() const;
    // Frames that did not fit into the ring (the disk was too slow) in the current / last take.
    uint64_t DroppedFrames() const { return dropped_.load(std::memory_order_relaxed); }

    // Tests: drain once on the calling thread instead of waiting for the background pass.
    void WaitIdleForTest();

private:
    enum class State { Idle, Starting, Recording, Stopping };
    void DrainLoop();
    void DrainToFile();   // drain thread only
    void FinishTake();    // drain thread only

    int sampleRate_ = 44100;
    std::atomic<bool> recording_{false};   // what Push() looks at
    std::atomic<State> state_{State::Idle};
    std::atomic<bool> shuttingDown_{false};
    std::thread drainThread_;

    // Lock-free single-producer/single-consumer ring buffer: AudioCallback
    // (producer, audio thread) writes via Push(); drainThread_ (consumer)
    // reads. Four seconds deep, so a slow disk write does not lose audio;
    // if the consumer still falls that far behind, newest samples are
    // dropped (counted in dropped_) rather than blocking the audio thread.
    int ringFrames_ = 0;
    std::vector<float> ring_;        // ringFrames_ * 2 (stereo), sized in Init()
    std::atomic<int> ringWrite_{0};  // frame index, producer-owned
    std::atomic<int> ringRead_{0};   // frame index, consumer-owned (reset by Toggle while Idle)
    std::atomic<uint64_t> dropped_{0};

    WavStreamWriter wav_;                    // drain thread only
    uint64_t framesSinceFlush_ = 0;          // drain thread only
    std::atomic<int64_t> writtenFrames_{0};  // for ElapsedSeconds(), any thread

    std::mutex msgMutex_;                    // UI <-> drain thread (never the audio thread)
    std::string pendingPath_, pendingShown_; // set by Toggle before Starting (the shown form made on the UI thread)
    std::string shownPath_;                  // drain thread only
    std::string resultMsg_, resultPath_;
    bool hasResult_ = false;
};

} // namespace t8
