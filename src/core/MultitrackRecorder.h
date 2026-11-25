#pragma once
// Live multitrack recording -- captures the SAME live performance the
// plain Recorder (see Recorder.h) captures, whatever actually plays
// (programmed pattern steps, LIVE BREAK fills, ratchets, live NOTE PLAY
// input, all of it), but as 7 separate stem buses instead of one mixed-
// down stereo file, so the result drops straight into Ableton Live (or
// any DAW) as individually mixable/masterable tracks.
//
// The 10 tracks collapse into 7 stems: Kick on its own, Snare+Clap+Perc
// together (the "rest of the percussion"), Closed+Open Hat together, then
// Synth 1/2/3 and Chord each on their own -- same grouping the user asked
// for, chosen because a drum kit's hats and the snare/clap/perc group are
// usually treated as one mix decision anyway, while Kick (sidechain/
// punch) and the 3 melodic voices + Chord each want independent control.
//
// Wired the same way Recorder is, for the same audio-thread-safety
// reasons (see Recorder.h's own comment): AudioEngine::Render feeds
// Push() every block with each stem's already-mixed (volume/pan/mute
// applied) signal -- BEFORE the shared delay/reverb sends and the master
// bus's filter/repeater/drive/soft-clip, so each stem is clean and
// independently remixable/masterable, not baked together with other
// stems' sends or the final master-bus treatment. Push() itself is
// allocation-free and lock-free, same as Recorder::Push, so it costs
// nothing extra on the audio thread when not recording.
#include "Types.h"
#include "WavStream.h"
#include <mutex>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace t8 {

constexpr int kNumStemGroups = 8;   // v44.50: the sampler has a stem of its own

// Which of the 7 stems each of the 10 tracks (Types.h's order: 6 drums,
// then 3 synths, then Chord) feeds into.
constexpr int kStemGroupForTrack[kNumTracks] = {
    0, // Kick
    1, // Snare      -> group 1 (Snare+Clap+Perc)
    1, // Clap       -> group 1
    2, // Closed Hat -> group 2 (Hats)
    2, // Open Hat   -> group 2
    1, // Perc       -> group 1
    3, // Synth 1
    4, // Synth 2
    5, // Synth 3
    6, // Chord
    7, // Sampler
};

// Display names (for status text) and the filenames each stem gets inside
// the take's folder -- numbered so they sort/import into Ableton in a
// sensible, stable order rather than alphabetically scrambling.
constexpr const char* kStemGroupNames[kNumStemGroups] = {
    "KICK", "SNARE+CLAP+PERC", "HATS", "SYNTH 1", "SYNTH 2", "SYNTH 3", "CHORD", "SAMPLER"
};
constexpr const char* kStemGroupFileNames[kNumStemGroups] = {
    "01_kick.wav", "02_snare_clap_perc.wav", "03_hats.wav",
    "04_synth1.wav", "05_synth2.wav", "06_synth3.wav", "07_chord.wav", "08_sampler.wav",
};

// N1 (audit v44.58 LIVE-1 / LIVE-3 / LIVE-7): the stems are written into
// their WAV files while they are recorded (WavStream.h), into a folder
// "Recordings/<project> <date> stems/". Before, all of them grew in memory
// (about 10 GB for a one-hour set) and STOP built a ZIP on the UI thread
// while the sound waited; the ZIP format also could not hold more than 4 GB.
// Only the drain thread touches the files and the read side of the rings.
class MultitrackRecorder {
public:
    ~MultitrackRecorder() { Shutdown(); }

    // Starts the background drain thread. Call once at startup (see
    // AudioEngine::Init) -- cheap and safe even if never used this
    // session.
    void Init(int sampleRate);
    void Shutdown();

    // Audio-thread-safe: allocation-free, lock-free, bounded work only.
    // Called once per AudioEngine::Render block with `numFrames` frames
    // of interleaved stereo float for EACH of the 7 stems
    // (stemBlocks[0..6]). A no-op (one atomic load) when not armed.
    //
    // Follow-up round, per direct feedback ("arm the recording and then
    // start the rec when the first sound is played... make sure all
    // tracks... are the same length, cause it is hard to match them in
    // Ableton"): arming (Toggle()'s start press) no longer begins writing
    // samples immediately -- it only flips armed_ on, and every Push()
    // call scans its block for the first nonzero sample across ANY of the
    // 7 stems. Until that happens, nothing is written anywhere (no
    // arbitrary "how long you fiddled with menus before hitting play"
    // dead air baked into the files). The instant it IS found, every
    // stem starts being written from that exact same global sample
    // onward -- including stems that are themselves silent at that
    // moment, so all 7 stay sample-aligned and exactly the same length
    // from then until Toggle()'s stop, by construction, with no separate
    // trimming pass needed.
    void Push(int numFrames, const float* const* stemBlocks);

    // Starts or stops the multitrack take (UI thread). Starting ARMS it
    // (see Push's comment above -- actual sample capture begins on the
    // first sound). Returns at once with a short message for SetStatus; the
    // final "SAVED ..." comes from TakeResult() once the files are closed.
    // `folder` empty = a new stamped folder in Documents/TR-Brick/Recordings.
    std::string Toggle(const std::string& folder = std::string());

    // UI thread, once per frame: true once when a take has been closed;
    // `message` for SetStatus, `path` = the folder ("" if nothing was kept).
    bool TakeResult(std::string& message, std::string& path);

    // True from the moment Toggle() arms the take until the matching stop
    // -- covers BOTH the "waiting for the first sound" phase and actual
    // capture, since A should stop/cancel either way. See HasStarted()
    // to tell the two phases apart (e.g. for a different status string).
    bool IsRecording() const { return armed_.load(std::memory_order_relaxed); }
    // True once the first sound has actually been detected and real
    // sample capture is underway (as opposed to still armed-and-waiting).
    bool HasStarted() const { return started_.load(std::memory_order_relaxed); }
    double ElapsedSeconds() const;
    uint64_t DroppedFrames() const { return dropped_.load(std::memory_order_relaxed); }

    // Tests: wait until the last take is closed.
    void WaitIdleForTest();

private:
    enum class State { Idle, Starting, Recording };
    void DrainLoop();
    void DrainToFiles();   // drain thread only, all stems in one pass
    void FinishTake();     // drain thread only

    int sampleRate_ = 44100;
    std::atomic<bool> armed_{false};   // Toggle()'s start press -- may still be waiting for the first sound
    std::atomic<bool> started_{false}; // first sound detected -- real sample capture is underway
    std::atomic<State> state_{State::Idle};
    std::atomic<bool> shuttingDown_{false};
    std::thread drainThread_;

    // One lock-free single-producer/single-consumer ring per stem, four
    // seconds deep, drop-on-full like Recorder's (counted in dropped_).
    // All stems are pushed in lockstep (same numFrames) every Render()
    // block and drained together every pass, so they stay sample-aligned.
    int ringFrames_ = 0;
    std::array<std::vector<float>, kNumStemGroups> ring_;
    std::array<std::atomic<int>, kNumStemGroups> ringWrite_;
    std::array<std::atomic<int>, kNumStemGroups> ringRead_;
    std::atomic<uint64_t> dropped_{0};

    std::array<WavStreamWriter, kNumStemGroups> wav_;  // drain thread only
    std::string folder_, shownFolder_;                 // drain thread only (copied from pendingFolder_ / pendingShown_)
    uint64_t framesSinceFlush_ = 0;                    // drain thread only
    std::atomic<int64_t> writtenFrames_{0};            // frames written so far (stem 0), for ElapsedSeconds()

    std::mutex msgMutex_;                              // UI <-> drain thread (never the audio thread)
    std::string pendingFolder_, pendingShown_;
    std::string resultMsg_, resultPath_;
    bool hasResult_ = false;
};

} // namespace t8
