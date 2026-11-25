#pragma once
// Startup/runtime glue for the app: project-root discovery and the SDL audio callback.
#include "app/AppState.h"
#include <SDL.h>
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
inline void AudioCallback(void* userdata, Uint8* stream, int len) {
    auto* app = static_cast<AppState*>(userdata);
    int numFrames = len / static_cast<int>(sizeof(float) * 2);
    const Uint64 t0 = SDL_GetPerformanceCounter();
    app->engine.GetMixer().audioBlocks.fetch_add(1, std::memory_order_relaxed);
    {   // v44.57: PITCH - the whole output plays a little faster / slower, like a turntable (1.0 = a plain copy)
        const int rate = app->engine.SampleRate();
        app->varispeed.Process(reinterpret_cast<float*>(stream), numFrames, app->PitchTargetSpeed(), rate,
                               [&](float* dst, int frames) { app->sequencer.RenderAndAdvance(dst, frames, rate); });
    }
    {   // load meter: time spent / time available for this buffer
        const double used = static_cast<double>(SDL_GetPerformanceCounter() - t0) / static_cast<double>(SDL_GetPerformanceFrequency());
        const double budget = static_cast<double>(numFrames) / std::max(1, app->engine.SampleRate());
        const float load = static_cast<float>(used / budget);
        Mixer& mx = app->engine.GetMixer();
        mx.audioLoad.store(mx.audioLoad.load(std::memory_order_relaxed) * 0.9f + load * 0.1f, std::memory_order_relaxed);
        mx.audioLoadPeak.store(std::max(load, mx.audioLoadPeak.load(std::memory_order_relaxed) * 0.995f), std::memory_order_relaxed);
        // N1: a dropout is logged with how long it lasted and how much of it the audio thread spent waiting for the
        // scene lock (= the screen or a loader held the sound), so the header can tell "the screen" from "too much DSP".
        const unsigned waitUs = mx.audioLockWaitUs.exchange(0, std::memory_order_relaxed);
        const unsigned usedUs = static_cast<unsigned>(std::min(used * 1e6, 4.0e9));
        if (usedUs > mx.audioLongestUs.load(std::memory_order_relaxed)) {
            mx.audioLongestUs.store(usedUs, std::memory_order_relaxed);
            mx.audioLongestWaitUs.store(waitUs, std::memory_order_relaxed);
        }
        if (load > 1.0f) {
            const unsigned n = mx.audioXruns.fetch_add(1, std::memory_order_relaxed);
            Mixer::Dropout& d = mx.dropouts[n % Mixer::kDropoutLog];
            d.lenUs.store(usedUs, std::memory_order_relaxed);
            d.waitUs.store(waitUs, std::memory_order_relaxed);
            d.atMs.store(SDL_GetTicks(), std::memory_order_relaxed);
        }
    }
    // Live recording (see Recorder.h) taps the exact same final mixed
    // buffer SDL is about to play -- Push() is a no-op unless a recording
    // is actually in progress, and does no allocation/locking/disk I/O
    // itself (audio-thread-safe by construction).
    app->recorder.Push(reinterpret_cast<float*>(stream), numFrames);
}

} // namespace t8
