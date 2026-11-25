#pragma once
// Shared mutators for editing a SELECTED TRACK's own voice (SynthPatch for
// Synth/Chord tracks, drumTune/drumDecay for Drum tracks) from a mouse
// panel -- the desktop counterpart to MixerSync.h, same reasoning: there's
// exactly one copy of this data (Pattern::synthPatches[0..2]/chordPatch of the edited scene for
// melodic tracks, Track::drumTune/drumDecay for drum tracks), and both the
// keyboard-driven SynthView.cpp and this desktop UI need to read/write the
// SAME copy and push it to the SAME live engine voice, or the two drift
// apart the moment either side edits something the other doesn't know
// about. Factored out here rather than duplicated into TrackDetailPanel.cpp
// so there's exactly one place that knows "how a global track index maps
// onto a synth slot / the chord slot / the engine voice to push to".
#include "app/AppState.h"
#include "app/SynthParams.h"

namespace t8::desktop {

// Global track index -> true for the one dedicated Chord track (index
// kNumDrumTracks + kNumSynthTracks, i.e. 9), false for everything else.
// Matches the fixed Kick..Perc,Synth1..3,Chord ordering every panel in
// this UI already assumes (see SequencerPanel.cpp/MixerPanel.cpp).
inline bool IsChordTrack(int trackIndex) {
    return trackIndex == kNumDrumTracks + kNumSynthTracks;
}

// Synth1..3 -> 0..2 (Project::synthPatches' index / AppState::prSynthIndex's
// convention). Meaningless (and never called) for a Drum or Chord track.
inline int SynthSlotForTrack(int trackIndex) {
    return trackIndex - kNumDrumTracks;
}

// Read-only access to whichever SynthPatch a track index resolves to --
// mirrors SynthView.cpp's own file-local CurrentPatch() helper exactly
// (same ternary), just keyed by global track index instead of
// AppState::prSynthIndex/synthViewChordSlot.
inline const SynthPatch& TrackPatch(const AppState& app, int trackIndex) {
    return IsChordTrack(trackIndex) ? app.project.patterns[static_cast<size_t>(app.project.currentPattern)].chordPatch
                                     : app.project.patterns[static_cast<size_t>(app.project.currentPattern)].synthPatches[static_cast<size_t>(SynthSlotForTrack(trackIndex))];
}

// Applies one AdjustPatchParam step (see SynthParams.h -- `dir` is +1/-1,
// not an absolute value; a relative-encoder model, used instead of a
// second, absolute-value parameter model, since AdjustPatchParam already
// correctly handles all 3 synth engines -- Subtractive/FM/Wavetable --
// each with their own param layout) to whichever track's patch, writes the
// result back to Project, and pushes it to the matching live engine voice
// (engine.Chord() for the Chord track, engine.Synth(slot) otherwise) --
// same two-step "mutate the persisted copy, then push to the live voice"
// SynthView.cpp's own AdjustPatchParam call site already does.
inline void AdjustTrackPatchParam(AppState& app, int trackIndex, int paramIndex, int dir, bool pushUndo = true) {
    if (pushUndo) app.PushSoundUndo();   // v44.5.4: the patch belongs to the edited scene; the sound undo entry remembers which
    const bool chord = IsChordTrack(trackIndex);
    const int slot = chord ? 0 : SynthSlotForTrack(trackIndex);
    SynthPatch p = app.EditedPatch(chord, slot);
    AdjustPatchParam(p, paramIndex, dir);
    app.EditedPatch(chord, slot) = p;
    app.PushEditedPatchToEngine(chord, slot);   // only when the edited scene is the one playing
}

// Drum-voice shaping -- plain fields directly on Track, read straight from
// the current pattern at trigger time (see Sequencer.cpp), so unlike the
// synth path above there's no separate "live engine voice" copy to push to
// -- writing Track::drumTune/drumDecay IS the live edit.
inline void SetDrumTune(AppState& app, int trackIndex, float semitones) {
    app.PushUndo();
    app.CurrentPattern().tracks[trackIndex].drumTune = std::clamp(semitones, -24.0f, 24.0f);
}
inline void SetDrumDecay(AppState& app, int trackIndex, float decay) {
    app.PushUndo();
    app.CurrentPattern().tracks[trackIndex].drumDecay = std::clamp(decay, 0.25f, 2.0f);
}

} // namespace t8::desktop
