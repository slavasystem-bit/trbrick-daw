#pragma once
// Small shared mutators for the one piece of state every mouse panel in
// TR-Brick DAW touches: a track's volume/pan/mute/solo. There are two
// copies of this data on purpose (Mixer::TrackState is what the audio
// thread actually reads every block; Track::volume/pan/mute/solo in the
// Pattern is what gets saved to the .t8proj file and restored on load),
// and both need to be kept in sync on every edit or the two diverge the
// next time something re-syncs from one side (pattern switch, project
// load -- see Mixer::SyncFromPattern). Factored out here, rather than
// copied into SequencerPanel.cpp AND MixerPanel.cpp separately, since
// BOTH panels edit mute/solo (quick per-track buttons on the Sequencer
// grid, the full set on the Mixer) and a mouse-driven UI makes those
// edits far more frequent than the keyboard ever did -- worth not having
// two copies of "did I remember to write both sides" to keep straight.
#include "app/AppState.h"
#include <mutex>

namespace t8::desktop {

inline void SetTrackVolume(AppState& app, int trackIndex, float volume01) {
    if (trackIndex < 0 || trackIndex >= kNumTracks) return;
    app.PushUndo();
    auto& mt = app.EditMixer().tracks[trackIndex];
    mt.volume = std::clamp(volume01, 0.0f, 1.0f);
    app.CurrentPattern().tracks[trackIndex].volume = mt.volume;
}

inline void SetTrackPan(AppState& app, int trackIndex, float pan) {
    if (trackIndex < 0 || trackIndex >= kNumTracks) return;
    app.PushUndo();
    auto& mt = app.EditMixer().tracks[trackIndex];
    mt.pan = std::clamp(pan, -1.0f, 1.0f);
    app.CurrentPattern().tracks[trackIndex].pan = mt.pan;
}

// v44.38: on TRACK (a brick) mute is WHO PLAYS and solo = every other instrument off (see AppState::SetMuteScene); on LIVE the flags of the scene.
// v44.5.1: mute / solo are per SCENE. The flags are written to the edited scene's pattern (the saved copy) and, when that
// scene is also the played one, to the audio-thread mixer. Another scene keeps its own flags.
inline void SetTrackMuteGlobal(AppState& app, int trackIndex, bool mute) { app.SetMuteScene(trackIndex, mute); }
inline void SetTrackSoloGlobal(AppState& app, int trackIndex, bool solo) { app.SetSoloScene(trackIndex, solo); }
inline void ToggleTrackMute(AppState& app, int trackIndex) {
    if (trackIndex < 0 || trackIndex >= kNumTracks) return;
    if (app.BrickOfSlot(app.project.currentPattern) >= 0) app.PushSoundUndo();   // v44.38: on a brick this is WHO PLAYS, and that is undoable
    app.SetMuteScene(trackIndex, !app.TrackIsMuted(trackIndex));
}

inline void ToggleTrackSolo(AppState& app, int trackIndex) {
    if (trackIndex < 0 || trackIndex >= kNumTracks) return;
    if (app.BrickOfSlot(app.project.currentPattern) >= 0) app.PushSoundUndo();
    app.SetSoloScene(trackIndex, !app.TrackIsSolo(trackIndex));
}

inline void SetMasterVolume(AppState& app, float volume) {
    app.PushSoundUndo();
    app.project.masterVolume = std::clamp(volume, 0.0f, 1.2f);
    app.engine.GetMixer().masterVolume = app.project.masterVolume;
}

// Per-track FX sends/drive -- same dual-write pattern as volume/pan above
// (Mixer::TrackState for the audio thread, Track for persistence), reachable as a live mouse knob.
inline void SetTrackReverbSend(AppState& app, int trackIndex, float send01) {
    if (trackIndex < 0 || trackIndex >= kNumTracks) return;
    app.PushUndo();
    auto& mt = app.EditMixer().tracks[trackIndex];
    mt.reverbSend = std::clamp(send01, 0.0f, 1.0f);
    app.CurrentPattern().tracks[trackIndex].reverbSend = mt.reverbSend;
}
inline void SetTrackDelaySend(AppState& app, int trackIndex, float send01) {
    if (trackIndex < 0 || trackIndex >= kNumTracks) return;
    app.PushUndo();
    auto& mt = app.EditMixer().tracks[trackIndex];
    mt.delaySend = std::clamp(send01, 0.0f, 1.0f);
    app.CurrentPattern().tracks[trackIndex].delaySend = mt.delaySend;
}
inline void SetTrackDrive(AppState& app, int trackIndex, float drive01) {
    if (trackIndex < 0 || trackIndex >= kNumTracks) return;
    app.PushUndo();
    auto& mt = app.EditMixer().tracks[trackIndex];
    mt.insertDrive = std::clamp(drive01, 0.0f, 1.0f);
    app.CurrentPattern().tracks[trackIndex].insertDrive = mt.insertDrive;
}

// Generic dual-write for any float field that exists under the same name on
// both Mixer::TrackState and Track (EQ tilt, chorus, autopan, ...). No undo
// push here -- the caller decides (one push per gesture, not per frame).
inline void SetTrackFloatField(AppState& app, int trackIndex,
                               float Mixer::TrackState::*mixerField, float Track::*trackField,
                               float value, float lo, float hi) {
    if (trackIndex < 0 || trackIndex >= kNumTracks) return;
    float v = std::clamp(value, lo, hi);
    app.engine.GetMixer().tracks[trackIndex].*mixerField = v;
    app.CurrentPattern().tracks[trackIndex].*trackField = v;
}

// Switches the live scene and re-syncs the audio-thread mixer copy
// from it -- the two steps are: set currentPattern, re-sync. Caller
// does not need to hold the pattern lock (taken here; it is recursive).
inline void SwitchPattern(AppState& app, int patternIndex) {
    std::lock_guard<SceneMutex> lock(app.sequencer.PatternMutex());
    app.project.currentPattern = std::clamp(patternIndex, 0, kNumPatterns - 1);
    // While a chain is playing, selecting a scene only changes what is being
    // EDITED -- playback keeps following the chain (see
    // Sequencer::PlayPatternIndex), and the audio mixer keeps mirroring the
    // played scene. Without a chain it is the classic live switch.
    if (app.EditIsPlaying()) { app.engine.GetMixer().SyncFromPattern(app.CurrentPattern()); app.sequencer.ApplySceneSound(app.project.currentPattern); }
}

// Scene selection from the UI: in song mode it only changes what is edited
// (SwitchPattern); otherwise it is a launch, quantized to the beat / bar chosen
// in the header (Sequencer::LaunchScene -- immediate when stopped or NOW).
inline void LaunchOrSwitchScene(AppState& app, int patternIndex) {
    if (app.sequencer.IsSongModeActive()) SwitchPattern(app, patternIndex);
    else app.sequencer.LaunchScene(patternIndex);
}

// v44.22 (S1a): the editor is on a LIVE scene on the LIVE tab and on the slot of a brick on the TRACK tab. main calls this when a tab
// is shown. LIVE: coming back from a brick, the scene that was open there is restored. TRACK: the selected brick (else the first) opens;
// with no brick the edited scene stays as it is.
inline void SelectBrickForEdit(AppState& app, int idx) {
    if (idx < 0 || idx >= app.project.songChainLength) return;
    app.selBrick = idx;
    const int slot = app.project.songChain[static_cast<size_t>(idx)].patternIndex;
    if (app.project.currentPattern != slot) SwitchPattern(app, slot);
}
inline void ShowTab(AppState& app, int tab) {
    int prevTab = app.activeTab;
    if (tab == 3) { if (prevTab != 3) app.tabBeforeLab = prevTab; app.activeTab = 3; return; }   // v44.41 MIX LAB: it only looks; the music and the scene stay as they were
    if (prevTab == 3) prevTab = app.tabBeforeLab;
    app.activeTab = tab;
    Project& pj = app.project;
    if (tab == 1) {   // v44.24: an edit of a part that follows the track is an edit of the track (every third frame is plenty)
        static int tick = 0;
        if (++tick % 3 == 0) app.SyncBrickSound();
    }
    if (tab == 2) {
        if (AppState::IsBrickSlot(pj.currentPattern)) SwitchPattern(app, std::clamp(pj.liveScene, 0, kNumLiveScenes - 1));
        else pj.liveScene = pj.currentPattern;
        // v44.51: a tab only SHOWS something; it never changes what is playing. The track keeps playing under LIVE until you press PLAY HERE
        // (the tab bar says what plays); a jam that was pinned while TRACK was open plays on as the edited scene again.
        if (prevTab != 2) app.sequencer.ReleasePin();
    } else if (pj.songChainLength > 0) {
        if (prevTab != 1) app.sequencer.PinPlayToCurrent();   // v44.51: LIVE -> TRACK while the jam plays: the jam keeps playing, the editor opens the brick
        if (!AppState::IsBrickSlot(pj.currentPattern)) pj.liveScene = std::clamp(pj.currentPattern, 0, kNumLiveScenes - 1);
        const int b = app.BrickForTransport();
        SelectBrickForEdit(app, b >= 0 ? b : std::clamp(app.selBrick, 0, pj.songChainLength - 1));
    }
}
// The scene arrows of the header and the [ ] keys: LIVE steps through the live scenes (launch = true quantizes like a pad click),
// TRACK through the bricks.
inline void StepScene(AppState& app, int dir, bool launch) {
    Project& pj = app.project;
    if (app.activeTab == 1 && pj.songChainLength > 0) {
        const int n = pj.songChainLength;
        int cur = app.BrickForTransport();
        if (cur < 0) cur = std::clamp(app.selBrick, 0, n - 1);
        SelectBrickForEdit(app, (cur + dir + n) % n);
        return;
    }
    const int cur = AppState::IsBrickSlot(pj.currentPattern) ? pj.liveScene : pj.currentPattern;
    const int next = (cur + dir + kNumLiveScenes) % kNumLiveScenes;
    if (launch) LaunchOrSwitchScene(app, next); else SwitchPattern(app, next);
}

// PERFORM panel's MUTE ALL -- a single live-performance "kill everything"
// gesture. Toggles based on
// aggregate state: if anything is currently unmuted, this mutes every
// track (the common "cut the music" moment); if every track is already
// muted, it un-mutes them all back to how they were -- a plain press-again
// undo rather than a separate button, since a live performer reaching for
// this wants one control, not two.
inline void ToggleMuteAllTracks(AppState& app) {
    app.PushUndo();
    bool anyUnmuted = false;
    for (int i = 0; i < kNumTracks; ++i) {
        if (!app.CurrentPattern().tracks[i].mute) { anyUnmuted = true; break; }
    }
    bool newMute = anyUnmuted; // mute everything if anything was still sounding, else un-mute all
    for (int i = 0; i < kNumTracks; ++i) SetTrackMuteGlobal(app, i, newMute);
    app.SetStatus(newMute ? "ALL TRACKS MUTED (THIS SCENE)" : "ALL TRACKS UNMUTED");
}

} // namespace t8::desktop
