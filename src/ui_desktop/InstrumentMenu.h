#pragma once
// v44.5.1: the menu for moving an instrument (one of the ten tracks) between scenes and for erasing it in a scene.
// Used by the track's right-click menu in the sequencer grid and by the scene pad menu in the chain panel.
#include "app/AppState.h"
#include "imgui.h"
#include <cstdio>
#include <string>

namespace t8::desktop {

// v44.5.4: "copy this part of the scene's sound to ..." : all scenes, the scenes of the track, or one scene.
inline void DrawSoundTargets(AppState& app, int scene, int what) {
    if (ImGui::MenuItem("All other scenes")) app.CopySoundToScenes(scene, AppState::kToAll, what);
    if (ImGui::MenuItem("Scenes of the track", nullptr, false, app.project.songChainLength > 0)) app.CopySoundToScenes(scene, AppState::kToChain, what);
    ImGui::Separator();
    for (int s = 0; s < kNumLiveScenes; ++s) {
        if (s == scene) continue;
        std::string label = "Scene " + std::to_string(s + 1);
        if (ImGui::MenuItem(label.c_str())) app.CopySoundToScenes(scene, s, what);
    }
}

// The sound items of a whole scene (scene pad menu).
inline void DrawSceneSoundItems(AppState& app, int scene) {
    if (ImGui::BeginMenu("Copy everything (synths, chord, kick, drum voices, kit) to")) { DrawSoundTargets(app, scene, AppState::kSoundAll); ImGui::EndMenu(); }
    if (ImGui::BeginMenu("Copy the synth + chord + sampler patches to")) { DrawSoundTargets(app, scene, AppState::kSoundSynth0 | AppState::kSoundSynth1 | AppState::kSoundSynth2 | AppState::kSoundChord | AppState::kSoundSampler); ImGui::EndMenu(); }
    if (ImGui::BeginMenu("Copy the drum sound (kit, kick synth, tune / decay) to")) { DrawSoundTargets(app, scene, AppState::kSoundKit | AppState::kSoundKick | AppState::kSoundDrums); ImGui::EndMenu(); }
    if (ImGui::BeginMenu("Copy only the kit to")) { DrawSoundTargets(app, scene, AppState::kSoundKit); ImGui::EndMenu(); }
}

// The items for ONE instrument of ONE scene: copy it to another scene, copy it to another track of the same kind, erase it.
inline void DrawInstrumentItems(AppState& app, int scene, int track) {
    Project& proj = app.project;
    if (scene < 0 || scene >= kNumPatterns || track < 0 || track >= kNumTracks) return;
    const Track& src = proj.patterns[scene].tracks[track];
    if (ImGui::BeginMenu("Copy to scene")) {
        for (int s = 0; s < kNumLiveScenes; ++s) {
            if (s == scene) continue;
            std::string label = "Scene " + std::to_string(s + 1) + (app.TrackHasNotes(s, track) ? "  (has notes - replaces them)" : "  (empty)");
            if (ImGui::MenuItem(label.c_str())) app.CopyInstrument(scene, track, s);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Copy to another track of this scene")) {
        bool any = false;
        for (int t = 0; t < kNumTracks; ++t) {
            if (t == track || proj.patterns[scene].tracks[t].type != src.type) continue;
            any = true;
            std::string label = proj.patterns[scene].tracks[t].name + (app.TrackHasNotes(scene, t) ? "  (replaces its notes)" : "  (empty)");
            if (ImGui::MenuItem(label.c_str())) app.CopyInstrument(scene, track, scene, t);
        }
        if (!any) ImGui::TextDisabled("no other track of this kind");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Copy to the edited scene", scene != proj.currentPattern)) {
        if (ImGui::MenuItem("Same track")) app.CopyInstrument(scene, track, proj.currentPattern);
        ImGui::EndMenu();
    }
    {   // the sound of this instrument (synth / chord patch, drum voice + kick synth) is the scene's own; this copies it elsewhere
        int what = 0;
        if (src.type == TrackType::Synth) what = AppState::kSoundSynth0 << src.slotIndex;
        else if (src.type == TrackType::Chord) what = AppState::kSoundChord;
        else if (src.type == TrackType::Sampler) what = AppState::kSoundSampler;
        else what = AppState::kSoundDrums | (src.slotIndex == static_cast<int>(DrumSlot::Kick) ? AppState::kSoundKick : 0);
        if (ImGui::BeginMenu(src.type == TrackType::Drum ? "Copy the drum voices to scenes" : "Copy this patch to scenes")) { DrawSoundTargets(app, scene, what); ImGui::EndMenu(); }
    }
    if (ImGui::BeginMenu("Match level in other scenes")) {
        char b[64];
        std::snprintf(b, sizeof(b), "Fader %.0f%% -> all scenes", src.volume * 100.0f);
        if (ImGui::MenuItem(b)) app.MatchInstrumentLevel(scene, track, false, false);
        std::snprintf(b, sizeof(b), "Fader %.0f%% -> scenes of the track", src.volume * 100.0f);
        if (ImGui::MenuItem(b)) app.MatchInstrumentLevel(scene, track, true, false);
        ImGui::Separator();
        if (ImGui::MenuItem("Whole strip (fader, pan, sends, FX, EQ) -> all scenes")) app.MatchInstrumentLevel(scene, track, false, true);
        if (ImGui::MenuItem("Whole strip -> scenes of the track")) app.MatchInstrumentLevel(scene, track, true, true);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Erase this instrument in the scene", nullptr, false, app.TrackHasNotes(scene, track))) app.ClearInstrument(scene, track);
}

} // namespace t8::desktop
