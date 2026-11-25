#pragma once
// Phase 5b: built-in single-cycle wavetable banks for WavetableSynth. Each
// bank is a small ordered set of "frames" (single-cycle waveforms) that
// SynthPatch::wtPosition scans/interpolates across continuously -- the
// classic PPG/Serum-style wavetable idea: instead of one fixed oscillator
// shape (like SubtractiveSynth's Waveform enum), the timbre itself morphs
// as you sweep one knob (or automate it with the LFO's WtPosition target).
//
// All 4 banks are procedurally generated at first use (no external audio
// files/assets -- same "hand-authored, no external resources" convention
// as Font5x7's bitmap font) via additive harmonic synthesis and simple
// waveshaping, computed once into a static table and reused for the life
// of the process.
#include <array>
#include <cstddef>

namespace t8 {

constexpr int kNumWavetableBanks = 4;
constexpr int kWtFrames = 8;      // frames per bank, scanned by wtPosition
constexpr int kWtFrameSize = 2048; // samples per single-cycle frame

extern const char* kWavetableBankNames[kNumWavetableBanks];

using WavetableBank = std::array<std::array<float, kWtFrameSize>, kWtFrames>;

// Lazily computed on first call, then reused (function-local static ->
// thread-safe one-time init per C++11, and this project already renders
// audio from a single locked-around thread anyway -- see AudioEngine).
const std::array<WavetableBank, kNumWavetableBanks>& AllWavetables();

} // namespace t8
