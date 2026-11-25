#pragma once
// v44.24 (S2): the SOUND of a scene as twelve parts, so the track can have one sound for each instrument and a brick can take its own
// for some of them. Part t (0..kNumTracks-1) is the instrument of track t: its channel strip (level, pan, sends, insert FX, EQ, drum voice
// tune / decay / sample) and, where the instrument has one, its synth patch or the built-in kick. Part kPartKit is the drum kit folder
// of the scene. Notes, the track's own length and swing, and its mute / solo are not part of the sound.
#include "Types.h"

namespace t8 {

// v44.50: the kit part keeps its place at the END (bit 11); before the sampler existed it was bit 10 (files of format < 7 are remapped on load).
constexpr int kPartKit = kNumTracks;        // 11
constexpr int kNumParts = kNumTracks + 1;   // 12
constexpr uint16_t kAllPartsMask = static_cast<uint16_t>((1u << kNumParts) - 1u);

// Copies the channel of track `s` onto track `d` without touching what is the destination's own: name / type / slot, the notes and the
// timing of the track, mute and solo.
inline void CopyTrackChannel(Track& d, const Track& s) {
    const std::string name = d.name; const TrackType type = d.type; const int slot = d.slotIndex;
    const int length = d.length; const bool own = d.useOwnLength; const float swing = d.swing; const bool groove = d.grooveOn;
    const bool mute = d.mute, solo = d.solo;
    std::array<Step, kMaxSteps> steps = d.steps;
    d = s;
    d.name = name; d.type = type; d.slotIndex = slot; d.length = length; d.useOwnLength = own; d.swing = swing; d.grooveOn = groove;
    d.mute = mute; d.solo = solo; d.steps = steps;
}

// One part of the sound from scene `s` into scene `d`.
inline void CopyPart(Pattern& d, const Pattern& s, int part) {
    if (part == kPartKit) { d.kitFolder = s.kitFolder; return; }
    if (part < 0 || part >= kNumTracks) return;
    CopyTrackChannel(d.tracks[static_cast<size_t>(part)], s.tracks[static_cast<size_t>(part)]);
    if (part == static_cast<int>(DrumSlot::Kick)) d.kick = s.kick;
    else if (part >= kNumDrumTracks && part < kChordTrackIndex) d.synthPatches[static_cast<size_t>(part - kNumDrumTracks)] = s.synthPatches[static_cast<size_t>(part - kNumDrumTracks)];
    else if (part == kChordTrackIndex) d.chordPatch = s.chordPatch;
    else if (part == kSamplerTrackIndex) d.samplerPatch = s.samplerPatch;
}
// Every part (not the own-sound marks).
inline void CopyAllParts(Pattern& d, const Pattern& s) { for (int p = 0; p < kNumParts; ++p) CopyPart(d, s, p); }

} // namespace t8
