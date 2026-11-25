#pragma once
// Live WAV recording of the master audio output, for bouncing a
// performance to a file (live-performance feature). AudioCallback
// (AppBootstrap.h) calls Push() every audio-thread tick with exactly what it
// just rendered; a dedicated background thread drains a small lock-free
// ring buffer into a growing in-RAM buffer, so the audio thread itself
// never does a heap allocation, a mutex wait, or (especially) disk I/O --
// this codebase has a documented, previously-diagnosed bug where blocking
// I/O on the audio thread caused audible dropouts on slow storage, so recording is built from the
// ground up to avoid that exact failure mode rather than risk repeating it.
//
// MP3/etc conversion is deliberately NOT done here -- no encoder library
// is vendored in this build (see README's recording section for why), so
// this ships WAV-only for now; convert externally if you need another
// format.
#include <atomic>
#include <cstdint>
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
    // Stops and joins the background thread. Safe to call even if Init()
    // was never called, and safe to call more than once (e.g. once
    // explicitly at shutdown, then again from the destructor).
    void Shutdown();

    // Audio-thread-safe: allocation-free, lock-free, bounded work only.
    // Called every AudioCallback tick with exactly what was just rendered
    // (interleaved stereo float, `numFrames` frames). A no-op (besides one
    // atomic load) whenever not currently recording.
    void Push(const float* interleavedStereo, int numFrames);

    // Starts or stops a recording -- what a pad/action calls. Returns a
    // short human-readable message to show via SetStatus, e.g.
    // "RECORDING" or "SAVED 12.4s -> recordings/take_1723….wav". Safe to
    // call from the main/UI thread only (does file I/O on stop).
    std::string Toggle();

    bool IsRecording() const { return recording_.load(std::memory_order_relaxed); }
    double ElapsedSeconds() const;

private:
    void DrainLoop();
    void DrainRingIntoCaptured(); // caller's thread does the (cheap) copy

    int sampleRate_ = 44100;
    std::atomic<bool> recording_{false};
    std::atomic<bool> shuttingDown_{false};
    std::thread drainThread_;

    // Lock-free single-producer/single-consumer ring buffer: AudioCallback
    // (producer, audio thread) writes via Push(); drainThread_ (consumer,
    // background thread) reads. Fixed capacity (~1s of stereo audio) --
    // if the consumer ever fell a full second behind (it drains every
    // ~20ms, so this shouldn't happen in practice), newest samples are
    // dropped rather than blocking the audio thread.
    static constexpr int kRingCapacityFrames = 44100; // ~1s @ 44.1kHz
    std::vector<float> ring_;        // kRingCapacityFrames * 2 (stereo), sized in Init()
    std::atomic<int> ringWrite_{0};  // frame index, producer-owned
    std::atomic<int> ringRead_{0};   // frame index, consumer-owned

    std::vector<float> captured_;            // grows only on the background thread (or Toggle()'s final drain)
    std::atomic<int64_t> capturedFrames_{0}; // for ElapsedSeconds(), safe to read from any thread
};

} // namespace t8
