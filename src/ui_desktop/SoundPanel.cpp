#include "BrickGlyphs.h"
#include "SoundPanel.h"
#include "ValueField.h"
#include "LiveWidgets.h"
#include "TrackStyle.h"
#include "Visual.h"
#include "InfoLine.h"
#include "imgui.h"
#include <algorithm>
#include <cstdio>

namespace t8::desktop {

namespace {
// The brick the TRACK screen is on, or -1 (no bricks: the editor shows a scene).
int EditedBrickSlot(const AppState& app) {
    const int s = app.project.currentPattern;
    return AppState::IsBrickSlot(s) && app.SlotInUse(s) ? s : -1;
}
const char* const kPartLong[kNumTracks] = {"KICK", "SNARE", "CLAP", "CLOSED HAT", "OPEN HAT", "PERC", "BASS", "SYNTH 2", "SYNTH 3", "CHORD", "SAMPLER"};
}

void SoundPanel::DrawModeRow(AppState& app) {
    const int slot = EditedBrickSlot(app);
    ImGui::AlignTextToFramePadding();
    if (slot >= 0) {
        const int b = app.BrickForTransport();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(vis::kTeal), "BRICK %d", (b >= 0 ? b : std::clamp(app.selBrick, 0, app.project.songChainLength - 1)) + 1);
    } else {
        ImGui::TextDisabled("NO BRICK");
    }
    ImGui::SameLine(0, 10);
    static const char* const kModes[3] = {"NOTES", "SOUND", "MIX"};
    int mode = std::clamp(app.trackEditMode, 0, 2);
    if (Segmented("##trkmode", &mode, kModes, 3)) app.trackEditMode = mode;
    if (ImGui::IsItemHovered())
        Hint("What the bottom of the screen edits for the selected brick:\nNOTES = its notes.  SOUND = the sound of an instrument (for the whole track, or for this brick only).  MIX = levels, pan, sends, EQ and FX of the instruments.");
    if (mode != 1) return;
    ImGui::SameLine(0, 16);
    const int sel = std::clamp(app.seqCursorTrack, 0, kNumTracks - 1);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int t = 0; t < kNumTracks; ++t) {
        if (t) ImGui::SameLine(0, 3);
        const ImU32 col = TrackColor(t);
        const bool on = sel == t;
        ImGui::PushID(t);
        // v44.37: the chip is the instrument's pictogram (the name is in the info line); lit = the selected one, in its own colour
        if (GlyphTile("##part", Glyph::Level, nullptr, on, col, true, 36.0f, kMinTarget + 3.0f, t, col)) app.seqCursorTrack = t;
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        dl->AddRectFilled(ImVec2(a.x + 4, b.y - 4), ImVec2(b.x - 4, b.y - 2), col, 1.0f);   // the instrument's colour under the picture
        if (slot >= 0 && app.PartIsOwn(slot, t)) {   // the brick has its own sound for this instrument
            dl->AddCircleFilled(ImVec2(b.x - 6, a.y + 6), 3.5f, IM_COL32(255, 255, 255, 255));
            dl->AddCircle(ImVec2(b.x - 6, a.y + 6), 3.5f, IM_COL32(150, 80, 220, 255), 0, 1.5f);
        }
        if (ImGui::IsItemHovered()) Hint("%s: its sound%s", kPartLong[t], slot >= 0 && app.PartIsOwn(slot, t) ? " (THIS BRICK has its own)" : "");
        ImGui::PopID();
    }
}

void SoundPanel::DrawScopeRow(AppState& app) {
    const int slot = EditedBrickSlot(app);
    const int t = std::clamp(app.seqCursorTrack, 0, kNumTracks - 1);
    ImGui::AlignTextToFramePadding();
    if (slot < 0) {
        ImGui::TextDisabled("Throw a scene onto the track and select its brick: then the sound of each instrument is set here.");
        return;
    }
    const bool own = app.PartIsOwn(slot, t);
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        DrawInstrumentGlyph(ImGui::GetWindowDrawList(), t, ImVec2(p.x + 8.0f, p.y + ImGui::GetFrameHeight() * 0.5f), 15.0f, TrackColor(t));
        ImGui::Dummy(ImVec2(18.0f, 1.0f));
        ImGui::SameLine(0, 0);
    }
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TrackColor(t)), "%s", kPartLong[t]);
    ImGui::SameLine(0, 6);
    ImGui::TextDisabled("SOUND");
    ImGui::SameLine(0, 12);
    static const char* const kScope[2] = {"WHOLE TRACK", "THIS BRICK ONLY"};
    int scope = own ? 1 : 0;
    if (Segmented("##scope", &scope, kScope, 2)) {
        if (scope == 1) app.MakeBrickSoundOwn(slot, t); else app.FollowTrackSound(slot, t);
    }
    if (ImGui::IsItemHovered())
        Hint("WHOLE TRACK: what you change here changes this instrument in every brick that follows the track.\nTHIS BRICK ONLY: this brick gets its own copy of the sound (a dot on the instrument); changes to the track no longer reach it.\nGoing back to WHOLE TRACK gives the brick the track's sound again (Cmd+Z undoes it).");
    ImGui::SameLine(0, 10);
    ImGui::BeginDisabled(app.project.songChainLength < 2);
    if (MiniButton("APPLY TO ALL BRICKS")) app.ApplySoundToAllBricks(slot, t);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        Hint("The sound of %s in THIS brick becomes the sound of the track and of every brick, own sounds included. Cmd+Z undoes it.", kPartLong[t]);
    if (t < kNumDrumTracks) {   // the drum kit is one choice for all the drums of a brick
        ImGui::SameLine(0, 18);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("KIT");
        ImGui::SameLine(0, 6);
        static const char* const kKitScope[2] = {"TRACK'S KIT", "THIS BRICK'S KIT"};
        int ks = app.PartIsOwn(slot, kPartKit) ? 1 : 0;
        if (Segmented("##kitscope", &ks, kKitScope, 2)) {
            if (ks == 1) app.MakeBrickSoundOwn(slot, kPartKit); else app.FollowTrackSound(slot, kPartKit);
        }
        if (ImGui::IsItemHovered()) Hint("The drum kit (the folder of samples) of the brick: one choice for all the drums that do not have a kit of their own.\nTRACK'S KIT follows the kit of the track; THIS BRICK'S KIT keeps its own, e.g. a harder kit for the drop.");
        // v44.26: one drum can play from another kit than the rest
        ImGui::SameLine(0, 14);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("THIS DRUM");
        ImGui::SameLine(0, 6);
        static const char* const kDrumKit[2] = {"THE KIT", "OWN KIT"};
        Pattern& bp = app.project.patterns[static_cast<size_t>(slot)];
        int dk = bp.tracks[static_cast<size_t>(t)].kitFolder.empty() ? 0 : 1;
        if (Segmented("##drumkit", &dk, kDrumKit, 2)) {
            app.PushUndo();
            Track& tr = bp.tracks[static_cast<size_t>(t)];
            if (dk == 1) tr.kitFolder = app.SceneKitFolder(slot); else tr.kitFolder.clear();
            app.SyncSceneKits();
        }
        if (ImGui::IsItemHovered()) Hint("THE KIT: %s plays from the kit of the brick.\nOWN KIT: only %s plays from another kit - pick it with the kit arrows or the kit browser below (the rest of the drums stay in the kit of the brick).\nIt follows the WHOLE TRACK / THIS BRICK ONLY choice of this instrument.", kPartLong[t], kPartLong[t]);
    }
    if (own) {
        ImGui::SameLine(0, 14);
        ImGui::TextColored(ImVec4(0.78f, 0.55f, 0.95f, 1.0f), "own sound");
    }
}

void SoundPanel::DrawMixNote(AppState& app) {
    (void)app;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Level, pan, sends, EQ and FX of every instrument. They are set for the whole track; an instrument with THIS BRICK ONLY in SOUND keeps its own.");
}

} // namespace t8::desktop
