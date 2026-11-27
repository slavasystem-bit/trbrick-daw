#pragma once
// C3 part 2 (CODE-2): the values of one mixer channel in ONE place.
//
// A scene's Track (Types.h) keeps these values (what gets saved and undone) and Mixer::TrackState (Mixer.h) keeps
// the copy the audio thread reads. Both now inherit them from ChannelMix, so the two copies cannot drift apart in
// what they hold, and copying a whole channel is one assignment. kChannelFields below is the list every place
// walks that has to name the values one by one: saving and loading (key in the .t8proj file), repairing a damaged
// file (range and default), comparing and copying parts of a channel (MATCH LEVEL, MIX LAB), and clamping a value
// set from the UI. A new channel value = one member here + one row in kChannelFields; nothing else to remember.
//
// Not here on purpose: mute / solo (per scene, but with their own brick rules, see AppState::SetMuteScene), the
// drum voice (drumTune / drumDecay) and the DSP state that lives only in Mixer::TrackState.
#include "TrackEq.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace t8 {

struct ChannelMix {
    float volume = 0.8f;  // 0..1
    float pan    = 0.0f;  // -1..1

    // Effects sends -- how much of this track's POST-fader signal feeds the shared delay / reverb buses (see FxSettings
    // and AudioEngine::Render). 0 = fully dry. Per scene like volume / pan, so a build-up scene can carry more reverb.
    float delaySend  = 0.0f; // 0..1
    float reverbSend = 0.0f; // 0..1

    // Per-track INSERT effects -- applied to this track's own signal pre-fader (before volume / pan), unlike the sends
    // above. See TrackInsertFx in Effects.h for the DSP. All 0 (off) by default.
    float insertDrive  = 0.0f; // 0..1, per-track saturation (same curve as the master-bus DRIVE)
    float insertEqTilt = 0.0f; // -1..1, one-knob tilt EQ: negative = darker, positive = brighter, 0 = flat
    float insertChorus = 0.0f; // 0..1, short modulated-delay chorus mix

    // Autopan -- its own LFO, added to `pan` in Mixer::MixTrack (clamped back to -1..1). 0 depth = no movement.
    float autopanRate  = 1.0f; // 0.05..8 Hz, LFO speed
    float autopanDepth = 0.0f; // 0..1, how far the pan swings each way

    // Stereo image (v33, see Stereo.h): `width` scales the side signal (0 = mono, 1 = as is, 2 = double); `spread` adds
    // a decorrelated, mono-compatible side signal built from the (high-passed) mid.
    float width  = 1.0f;       // 0..2
    float spread = 0.0f;       // 0..1

    // Channel EQ (v29, see TrackEq.h): HPF + low shelf + mid bell + high shelf. Flat / off by default.
    float eqHpfHz    = 20.0f;   // 20 = off .. 500
    float eqHpfSlope = 0.0f;    // 0 = 12 dB/oct, 1 = 24 dB/oct
    float eqLowDb    = 0.0f;    // -15..+15
    float eqLowHz    = 120.0f;  // 30..500
    float eqMidDb    = 0.0f;    // -15..+15
    float eqMidHz    = 1000.0f; // 80..10000
    float eqMidQ     = 1.0f;    // 0.3..6
    float eqHighDb   = 0.0f;    // -15..+15
    float eqHighHz   = 6000.0f; // 1500..16000
};

// Which part of the strip a value belongs to, so a caller can copy or compare a part (MIX LAB: level, pan and EQ).
enum ChannelPart : uint8_t {
    kChLevel = 1, kChPan = 2, kChSends = 4, kChInserts = 8, kChStereo = 16, kChEq = 32,
    kChAll = kChLevel | kChPan | kChSends | kChInserts | kChStereo | kChEq,
};

struct ChannelField {
    const char* key;              // the key in the .t8proj file (never rename: old files use it)
    float ChannelMix::*member;
    float lo, hi, def;            // range a value is kept in, and what a damaged (non-finite) value becomes
    uint8_t part;                 // ChannelPart
    bool onOff;                   // only 0 or 1 (the HPF slope switch)
};

// File order: the save writes the values in this order (level / pan near the top of a track, the rest after the
// drum voice, the EQ only when it is not flat -- see WriteTrackBody), so old files stay byte for byte the same.
inline constexpr ChannelField kChannelFields[] = {
    {"volume",       &ChannelMix::volume,       0.0f,     1.0f,     0.8f,    kChLevel,   false},
    {"pan",          &ChannelMix::pan,          -1.0f,    1.0f,     0.0f,    kChPan,     false},
    {"delaySend",    &ChannelMix::delaySend,    0.0f,     1.0f,     0.0f,    kChSends,   false},
    {"reverbSend",   &ChannelMix::reverbSend,   0.0f,     1.0f,     0.0f,    kChSends,   false},
    {"insertDrive",  &ChannelMix::insertDrive,  0.0f,     1.0f,     0.0f,    kChInserts, false},
    {"insertEqTilt", &ChannelMix::insertEqTilt, -1.0f,    1.0f,     0.0f,    kChInserts, false},
    {"insertChorus", &ChannelMix::insertChorus, 0.0f,     1.0f,     0.0f,    kChInserts, false},
    {"autopanRate",  &ChannelMix::autopanRate,  0.05f,    8.0f,     1.0f,    kChInserts, false},
    {"autopanDepth", &ChannelMix::autopanDepth, 0.0f,     1.0f,     0.0f,    kChInserts, false},
    {"width",        &ChannelMix::width,        0.0f,     2.0f,     1.0f,    kChStereo,  false},
    {"spread",       &ChannelMix::spread,       0.0f,     1.0f,     0.0f,    kChStereo,  false},
    {"eqHpfHz",      &ChannelMix::eqHpfHz,      20.0f,    500.0f,   20.0f,   kChEq,      false},
    {"eqHpfSlope",   &ChannelMix::eqHpfSlope,   0.0f,     1.0f,     0.0f,    kChEq,      true},
    {"eqLowDb",      &ChannelMix::eqLowDb,      kEqMinDb, kEqMaxDb, 0.0f,    kChEq,      false},
    {"eqLowHz",      &ChannelMix::eqLowHz,      30.0f,    500.0f,   120.0f,  kChEq,      false},
    {"eqMidDb",      &ChannelMix::eqMidDb,      kEqMinDb, kEqMaxDb, 0.0f,    kChEq,      false},
    {"eqMidHz",      &ChannelMix::eqMidHz,      80.0f,    10000.0f, 1000.0f, kChEq,      false},
    {"eqMidQ",       &ChannelMix::eqMidQ,       0.3f,     6.0f,     1.0f,    kChEq,      false},
    {"eqHighDb",     &ChannelMix::eqHighDb,     kEqMinDb, kEqMaxDb, 0.0f,    kChEq,      false},
    {"eqHighHz",     &ChannelMix::eqHighHz,     1500.0f,  16000.0f, 6000.0f, kChEq,      false},
};
inline constexpr int kNumChannelFields = static_cast<int>(sizeof(kChannelFields) / sizeof(kChannelFields[0]));
// Every float of ChannelMix has a row (a member added without a row fails here).
static_assert(sizeof(ChannelMix) == kNumChannelFields * sizeof(float), "ChannelMix: every value needs a row in kChannelFields");

inline const ChannelField* FindChannelField(float ChannelMix::*member) {
    for (const ChannelField& f : kChannelFields) if (f.member == member) return &f;
    return nullptr;
}
inline const ChannelField* FindChannelField(const char* key) {
    for (const ChannelField& f : kChannelFields) if (std::strcmp(f.key, key) == 0) return &f;
    return nullptr;
}

// A value for `member` the way the channel keeps it: in range, 0 / 1 for a switch, the default when not finite.
inline float ClampChannelValue(const ChannelField& f, float v) {
    if (!std::isfinite(v)) return f.def;
    v = std::clamp(v, f.lo, f.hi);
    if (f.onOff) v = v >= 0.5f ? 1.0f : 0.0f;
    return v;
}
inline float ClampChannelValue(float ChannelMix::*member, float v) {
    const ChannelField* f = FindChannelField(member);
    return f ? ClampChannelValue(*f, v) : v;
}

inline void CopyChannel(ChannelMix& to, const ChannelMix& from, uint8_t parts = kChAll) {
    if (parts == kChAll) { to = from; return; }
    for (const ChannelField& f : kChannelFields) if (f.part & parts) to.*f.member = from.*f.member;
}
inline bool SameChannel(const ChannelMix& a, const ChannelMix& b, uint8_t parts = kChAll) {
    for (const ChannelField& f : kChannelFields) if ((f.part & parts) && a.*f.member != b.*f.member) return false;
    return true;
}

} // namespace t8
