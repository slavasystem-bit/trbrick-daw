#pragma once
// Startup/runtime glue for the app: project-root discovery and the SDL audio callback.
#include "app/AppState.h"
#include <SDL.h>
#include <cstdint>
#include <mutex>
#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif
#include <string>
#include <algorithm>
#include <cstdio>
#include <climits>
#include <sys/stat.h>
#include <unistd.h>

namespace t8 {

inline bool FileExists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

inline bool IsDir(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

inline std::string CurrentDir() {
    char buf[1024];
    return getcwd(buf, sizeof(buf)) ? std::string(buf) : std::string("?");
}

// The recurring "0/6 drum samples loaded" / silent-drums bug has always
// come down to one thing: every relative path in this app (kits/808,
// project.t8proj, the Menu tab's projects/ folder, and now also
// assets/fonts/ for the desktop build) is resolved against the CURRENT
// DIRECTORY, not the binary's location -- so launching from inside
// build-mac/ (easy muscle-memory mistake: "cd build-mac &&
// ./trbrick_daw" instead of "./build-mac/trbrick_daw" from the project
// root) silently breaks it. Rather than relying on everyone remembering
// to cd correctly every time, chdir() into the actual project root
// (wherever "kits/" lives) before resolving anything else, so relative
// paths work no matter where the binary was launched from.
inline void ChdirToProjectRoot(const char* argv0) {
    if (IsDir("kits")) return; // already in the right place 

    char resolved[PATH_MAX];
    if (!realpath(argv0, resolved)) return;
    std::string dir = resolved;
    for (int hop = 0; hop < 4; ++hop) {
        size_t slash = dir.find_last_of('/');
        if (slash == std::string::npos) return;
        dir = dir.substr(0, slash);
        if (IsDir(dir + "/kits")) {
            if (::chdir(dir.c_str()) != 0) {
                std::fprintf(stderr, "NOTE: found project root at %s but chdir() failed; "
                                      "relative paths may still be wrong.\n", dir.c_str());
            }
            return;
        }
    }
}

// SDL audio callback: renders straight from the sequencer, which in turn
// drives the audio engine in sample-accurate sub-chunks.
// N1 (audit v44.58 LIVE-11): flush denormals to zero on the audio thread. Reverb / delay feedback, filters and the dynamics decay
// into subnormal numbers after a stop or a mute, and on Intel Macs those are very slow (CPU spikes, crackle in the quiet part).
inline void EnableFlushToZero() {
#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64)
    _mm_setcsr(_mm_getcsr() | 0x8040);   // FTZ (bit 15) + DAZ (bit 6)
#elif defined(__aarch64__)
    uint64_t fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    __asm__ __volatile__("msr fpcr, %0" :: "r"(fpcr | (uint64_t(1) << 24)));   // FZ
#endif
}

// SDL audio callback: renders straight from the sequencer, which in turn
// drives the audio engine in sample-accurate sub-chunks.
inline void AudioCallback(void* userdata, Uint8* stream, int len) {
    auto* app = static_cast<AppState*>(userdata);
    int numFrames = len / static_cast<int>(sizeof(float) * 2);
    const Uint64 t0 = SDL_GetPerformanceCounter();
    EnableFlushToZero();   // cheap; per call, because SDL may run the callback on a new thread after a restart
    Mixer& mx = app->engine.GetMixer();
    mx.audioBlocks.fetch_add(1, std::memory_order_relaxed);
    float* out = reinterpret_cast<float*>(stream);
    const int rate = std::max(1, app->engine.SampleRate());
    const double budget = static_cast<double>(numFrames) / rate;
    // N1 (LIVE-5): wait for the scene lock at most two buffers (at least 4 ms: short holds by the screen are absorbed by the
    // device's own buffering, as before). Longer = play a fade to silence now instead of blocking the device.
    unsigned waitUs = 0;
    const unsigned maxWaitUs = static_cast<unsigned>(std::max(4000.0, 2.0e6 * budget));
    if (app->sequencer.LockForAudio(maxWaitUs, waitUs)) {
        std::lock_guard<SceneMutex> lock(app->sequencer.PatternMutex(), std::adopt_lock);
        // v44.57: PITCH - the whole output plays a little faster / slower, like a turntable (1.0 = a plain copy)
        app->varispeed.Process(out, numFrames, app->PitchTargetSpeed(), rate,
                               [&](float* dst, int frames) { app->sequencer.RenderAndAdvance(dst, frames, rate); });
    } else {
        const int fade = std::min(numFrames, 64);
        for (int i = 0; i < numFrames; ++i) {
            const float g = i < fade ? 1.0f - static_cast<float>(i + 1) / fade : 0.0f;
            out[i * 2] = mx.lastOutL * g; out[i * 2 + 1] = mx.lastOutR * g;
        }
        mx.audioSkippedBlocks.fetch_add(1, std::memory_order_relaxed);
    }
    if (numFrames > 0) { mx.lastOutL = out[(numFrames - 1) * 2]; mx.lastOutR = out[(numFrames - 1) * 2 + 1]; }
    mx.audioLockWaitUs.fetch_add(waitUs, std::memory_order_relaxed);
    {   // load meter: time spent / time available for this buffer
        const double used = static_cast<double>(SDL_GetPerformanceCounter() - t0) / static_cast<double>(SDL_GetPerformanceFrequency());
        const float load = static_cast<float>(used / budget);
        mx.audioLoad.store(mx.audioLoad.load(std::memory_order_relaxed) * 0.9f + load * 0.1f, std::memory_order_relaxed);
        mx.audioLoadPeak.store(std::max(load, mx.audioLoadPeak.load(std::memory_order_relaxed) * 0.995f), std::memory_order_relaxed);
        // N1: a dropout is logged with how long it lasted and how much of it the audio thread spent waiting for the
        // scene lock (= the screen or a loader held the sound), so the header can tell "the screen" from "too much DSP".
        // Late buffers in a row are one dropout (a long stall now arrives as many short skipped buffers); it is logged when
        // a buffer is on time again.
        const unsigned waitedUs = mx.audioLockWaitUs.exchange(0, std::memory_order_relaxed);
        mx.audioBufferFrames.store(numFrames, std::memory_order_relaxed);
        const unsigned usedUs = static_cast<unsigned>(std::min(used * 1e6, 4.0e9));
        if (load > 1.0f) {
            mx.runUs += usedUs; mx.runWaitUs += waitedUs;
        } else if (mx.runUs > 0) {
            const unsigned n = mx.audioXruns.fetch_add(1, std::memory_order_relaxed);
            Mixer::Dropout& d = mx.dropouts[n % Mixer::kDropoutLog];
            d.lenUs.store(mx.runUs, std::memory_order_relaxed);
            d.waitUs.store(mx.runWaitUs, std::memory_order_relaxed);
            d.atMs.store(SDL_GetTicks(), std::memory_order_relaxed);
            if (mx.runUs > mx.audioLongestUs.load(std::memory_order_relaxed)) {
                mx.audioLongestUs.store(mx.runUs, std::memory_order_relaxed);
                mx.audioLongestWaitUs.store(mx.runWaitUs, std::memory_order_relaxed);
            }
            mx.runUs = 0; mx.runWaitUs = 0;
        }
    }
    // Live recording (see Recorder.h) taps the exact same final mixed
    // buffer SDL is about to play -- Push() is a no-op unless a recording
    // is actually in progress, and does no allocation/locking/disk I/O
    // itself (audio-thread-safe by construction).
    app->recorder.Push(out, numFrames);
}

} // namespace t8
