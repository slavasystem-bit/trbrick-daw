#include "ValueField.h"
#include "MenuBar.h"
#include "InfoLine.h"
#include "Interaction.h"
#include "LiveWidgets.h"
#include "imgui.h"
#include "core/UserDirs.h"
#include "app/AudioSettings.h"
#include "Visual.h"
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <sys/stat.h>

namespace t8::desktop {

namespace {
const ImVec4 kAmber(0.95f, 0.64f, 0.24f, 1.0f);


// MenuItem with a shortcut text ("Cmd+S"), a hint for the info line and an enabled flag. Returns true when chosen.
bool Item(const char* label, const char* key, const char* hint, bool enabled = true, bool checked = false) {
    std::string sc;
    if (key && key[0]) {
        sc = key;   // "^S" -> Cmd+S, "Shift+^S" -> Shift+Cmd+S
        for (size_t at; (at = sc.find('^')) != std::string::npos;) sc.replace(at, 1, MenuBar::CmdPrefix());
    }
    const bool hit = ImGui::MenuItem(label, sc.empty() ? nullptr : sc.c_str(), checked, enabled);
    if (hint && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) Hint("%s", hint);
    return hit;
}
} // namespace

const char* MenuBar::CmdPrefix() {
#ifdef __APPLE__
    return "Cmd+";
#else
    return "Ctrl+";
#endif
}

bool MenuBar::AnyCaptureActive(AppState& app) {
    return app.engine.Multitrack().IsRecording() || app.midiCaptureActive;
}

void MenuBar::Draw(AppState& app) {
    using DA = AppState::DocAction;
    ImGui::PushFont(nullptr, 14.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 5));
    if (ImGui::BeginMenuBar()) {
        const bool untitled = app.IsUntitled();
        const bool cap = AnyCaptureActive(app);

        // ---- File -------------------------------------------------------------------------------------------------------
        if (cap) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.38f, 0.38f, 1.0f));
        const bool fileOpen = ImGui::BeginMenu("File");
        if (cap) ImGui::PopStyleColor();
        if (fileOpen) {
            if (Item("New", "^N", "A new Untitled project. If something is unsaved you are asked first.")) app.RequestDoc(DA::New);
            if (Item("Open...", "^O", "Choose a project from the Projects folder.")) app.docOpenDialog = true;
            if (ImGui::BeginMenu("Open Recent")) {
                std::vector<std::pair<long long, std::string>> byTime;
                for (const std::string& p : app.ProjectFilesCached()) {
                    struct stat st{};
                    byTime.push_back({::stat(p.c_str(), &st) == 0 ? static_cast<long long>(st.st_mtime) : 0LL, p});
                }
                std::sort(byTime.begin(), byTime.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                if (byTime.empty()) ImGui::MenuItem("(no saved projects yet)", nullptr, false, false);
                int shown = 0;
                for (const auto& it : byTime) {
                    if (++shown > 10) break;
                    const bool active = it.second == app.projectPath;
                    if (ImGui::MenuItem(userdirs::ProjectDisplayName(it.second).c_str(), nullptr, active, !active)) app.RequestDoc(DA::Open, it.second);
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) Hint("%s", it.second.c_str());
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (Item("Save", "^S", "Save into the project file. A project without a name asks for one.")) app.SaveDocument();
            if (Item("Save As...", "Shift+^S", "Write what you have now to a new project file and go on in it. The old file stays as it was saved.")) {
                app.docNameMode = AppState::NameMode::SaveAs; app.docNameOpen = true;
            }
            if (Item("Rename...", nullptr, "Give the project file another name.", !untitled)) {
                app.docNameMode = AppState::NameMode::Rename; app.docNameOpen = true;
            }
            if (Item("Duplicate...", nullptr, "Make a copy of the project under another name and go on in the copy.")) {
                app.docNameMode = AppState::NameMode::Duplicate; app.docNameOpen = true;
            }
            if (Item("Revert to Saved", nullptr, "Throw away the changes and go back to the last saved version.", !untitled && app.edited)) app.RequestDoc(DA::Revert);
            if (Item("Browse Versions...", nullptr, "Dated copies the program made by itself: look at them and bring one back.", !untitled)) openVersions_ = true;
            ImGui::Separator();
            if (ImGui::BeginMenu("Export")) {
                const bool stems = app.engine.Multitrack().IsRecording();
                if (Item("Record Stems", nullptr, "Record Kick / Snare+Clap+Perc / Hats / Synth 1-3 / Chord as separate WAVs in one folder (written while you play) - for mixing in another program. The master recording is the REC button.",
                         true, stems)) app.SetStatus(app.engine.Multitrack().Toggle());
                const bool capNow = app.midiCaptureActive;
                if (Item("Record MIDI", nullptr, "Log every note / drum hit that plays (steps, fills, live input). Stop to get a .mid file and a text grid in Recordings.",
                         true, capNow)) app.ToggleMidiCapture();
                if (Item("Notes as Text", nullptr, "Write the edited scene's synth / chord notes as a text grid (Recordings folder).")) app.ExportCurrentPatternNotes();
                ImGui::Separator();
                if (Item("Show Recordings", nullptr, "Open the Recordings folder in Finder.")) userdirs::Reveal(userdirs::RecordingsDir());
                if (Item("Show Last File", nullptr, userdirs::LastOutput().empty() ? "Nothing was written in this session yet." : userdirs::DisplayPath(userdirs::LastOutput()).c_str(),
                         !userdirs::LastOutput().empty())) userdirs::Reveal(userdirs::LastOutput());
                ImGui::EndMenu();
            }
            if (Item("Show in Finder", nullptr, untitled ? "Not saved yet: Save gives it a name and a place in the Projects folder." : "Reveal the open project file in Finder.", !untitled))
                userdirs::Reveal(app.projectPath);
            ImGui::Separator();
            if (Item("Quit TR-Brick", "^Q", "If something is unsaved you are asked first.")) app.RequestDoc(DA::Quit);
            ImGui::EndMenu();
        }

        // ---- Edit -------------------------------------------------------------------------------------------------------
        if (ImGui::BeginMenu("Edit")) {
            const bool canUndo = app.FindApplicable(app.undoStack) >= 0;
            const bool canRedo = app.FindApplicable(app.redoStack) >= 0;
            if (Item("Undo", "^Z", "Take back the last change.", canUndo)) {
                SceneLock lock(app.sequencer.PatternMutex(), "MENU");
                app.SetStatus(app.Undo() ? app.lastUndoLabel : "NOTHING TO UNDO");
            }
            if (Item("Redo", "Shift+^Z", "Bring back what Undo took away.", canRedo)) {
                SceneLock lock(app.sequencer.PatternMutex(), "MENU");
                app.SetStatus(app.Redo() ? app.lastUndoLabel : "NOTHING TO REDO");
            }
            ImGui::Separator();
            const int tr = std::clamp(app.seqCursorTrack, 0, kNumTracks - 1);
            if (Item("Copy Track", "^C", "Copy the track under the cursor (the digits 1-0 or Cmd+arrows move the cursor).")) app.CopyTrack(tr);
            if (Item("Paste Track", "^V", "Paste it onto the track under the cursor (same kind of track only).", app.hasTrackClipboard)) {
                SceneLock lock(app.sequencer.PatternMutex(), "MENU");
                app.PasteTrack(tr);
            }
            if (Item("Copy Scene", "Shift+^C", "Copy the whole edited scene.")) { app.CopyPattern(); app.SetStatus("SCENE COPIED"); }
            if (Item("Paste Scene", "Shift+^V", "Replace the edited scene with the copied one (undoable).", app.hasPatternClipboard)) {
                SceneLock lock(app.sequencer.PatternMutex(), "MENU");
                app.SetStatus(app.PastePattern() ? "SCENE PASTED" : "CLIPBOARD EMPTY");
            }
            ImGui::EndMenu();
        }

        // ---- View -------------------------------------------------------------------------------------------------------
        if (ImGui::BeginMenu("View")) {
            // C1 (UI-1): the two screens, then the three tools that open the same way under the header (same key or Esc closes)
            if (Item("LIVE", "Tab", "The jam screen.", true, app.ShownTab() == 2)) { app.uiTabRequest = 2; app.OpenTool(AppState::Tool::None); }
            if (Item("TRACK", "Tab", "Build the finished track from the bricks.", true, app.ShownTab() == 1)) { app.uiTabRequest = 1; app.OpenTool(AppState::Tool::None); }
            ImGui::Separator();
            if (Item("MIX LAB", "M", "Where the instruments fight for the same frequencies, and what to turn to make room.", true, app.ToolOpen(AppState::Tool::MixLab))) app.ToggleTool(AppState::Tool::MixLab);
            if (Item("CHORD LAB", "C", "Jam with chords over the running scene (on LIVE) or over the open brick (on TRACK).", true, app.ToolOpen(AppState::Tool::ChordLab))) app.ToggleTool(AppState::Tool::ChordLab);
            if (Item("MASTER FX", "F", "Project-wide delay / reverb / distortion / sidechain and the master chain.", true, app.ToolOpen(AppState::Tool::MasterFx))) app.ToggleTool(AppState::Tool::MasterFx);
            ImGui::Separator();
            if (ImGui::BeginMenu("Interface Size")) {
                for (int z = 0; z < AppState::kNumUiZoom; ++z) {
                    char zl[24];
                    if (z == 0) std::snprintf(zl, sizeof(zl), "Auto");
                    else std::snprintf(zl, sizeof(zl), "%d%%", static_cast<int>(AppState::kUiZoomValues[z] * 100.0f + 0.5f));
                    if (ImGui::MenuItem(zl, nullptr, app.uiZoomIdx == z)) app.uiZoomIdx = z;
                    if (z == 0 && ImGui::IsItemHovered()) Hint("Auto shrinks the interface to fit a small window.");
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (Item("Gestures and Shortcuts", "F1", "All the mouse gestures and shortcuts on one screen.", true, app.helpOpen)) app.helpOpen = !app.helpOpen;
            ImGui::EndMenu();
        }

        // ---- Library ----------------------------------------------------------------------------------------------------
        if (ImGui::BeginMenu("Library")) {
            if (Item("My Beats...", nullptr, "Save the drums of the edited scene as a beat, apply, rename or delete saved beats. They also appear in the BEAT picker of RANDOM / KIT.")) openBeats_ = true;
            ImGui::Separator();
            if (Item("Rescan Samples", nullptr, "Read your sample library again (and copy new folders from Kits/ into the kit list). The library is only ever read, never changed.")) app.RescanLibrary();
            if (Item("Choose Samples Folder...", nullptr, "The folder with your own samples, sub-folders by instrument (Kicks, Snares, Hats, Fills ...). WAV and AIFF (.aif) files are listed. A folder Kits/<name>/ with kick.wav, snare.wav ... becomes a drum kit.")) app.RequestLibraryFolder();
            if (Item("Show Samples Folder in Finder", nullptr, "Where the program looks for your own samples.")) userdirs::Reveal(app.LibraryRoot());
            ImGui::Separator();
            if (Item("Show Library in Finder", nullptr, "The folder with your beats, synth presets and favourites. Copy it to take them to another computer.")) userdirs::Reveal(userdirs::LibraryDir());
            ImGui::EndMenu();
        }

        // ---- Settings ---------------------------------------------------------------------------------------------------
        if (ImGui::BeginMenu("Settings")) {
            if (Item("Restart Audio", nullptr, "Reopen the sound card and clear the effects. Use it if the sequence runs but nothing is heard (after the laptop slept or the headphones / speakers changed). Happens automatically in most cases."))
                app.audioRestartRequested = true;
            if (Item("Audio...", nullptr, "Which output the sound goes to (speakers, headphones, a sound card) and the buffer size. Remembered between launches.")) openAudio_ = true;
            if (Item("Show Data Folder in Finder", nullptr, "Projects, Recordings and the Library live here.")) userdirs::Reveal(userdirs::Root());
            ImGui::EndMenu();
        }

        if (cap) {   // a running capture is never forgotten
            ImGui::SameLine(0, 18);
            ImGui::TextColored(ImVec4(0.95f, 0.38f, 0.38f, 1.0f), "%s", app.engine.Multitrack().IsRecording() ? "RECORDING STEMS" : "CAPTURING MIDI");
        }
        ImGui::EndMenuBar();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopFont();

    // The dialogs open from here, outside the menus, so their popup ids are the shell's own.
    if (openVersions_) { ImGui::OpenPopup("VersionsDialog"); openVersions_ = false; backups_ = ListProjectBackups(app.projectPath); restoreArmed_.clear(); }
    if (openBeats_) { ImGui::OpenPopup("BeatsDialog"); openBeats_ = false; deleteArmed_.clear(); }
    if (openAudio_) { ImGui::OpenPopup("AudioDialog"); openAudio_ = false; app.audioOutputsWanted = true; }
    DrawVersionsDialog(app);
    DrawBeatsDialog(app);
    DrawAudioDialog(app);
}

// N1 (LIVE-5 / LIVE-6): Settings > Audio. A change is saved at once and restarts the sound for a moment (the same path as
// Settings > Restart Audio); main.cpp opens the device and reports what it really got.
void MenuBar::DrawAudioDialog(AppState& app) {
    ImGui::PushFont(nullptr, 14.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.38f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("AudioDialog", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar)) {
        const char* kDefaultName = "System default (follows the Mac)";
        ImGui::TextColored(kAmber, "Audio");
        ImGui::TextDisabled("Remembered between launches. Changing anything restarts the sound for a moment (a running recording gets a short gap).");
        ImGui::Spacing();

        ImGui::TextUnformatted("OUTPUT");
        ImGui::SetNextItemWidth(360);
        const std::string& chosen = app.audioOutputChoice;
        const bool chosenPlugged = std::find(app.audioOutputs.begin(), app.audioOutputs.end(), chosen) != app.audioOutputs.end();
        const std::string preview = chosen.empty() ? std::string(kDefaultName) : chosenPlugged ? chosen : chosen + "  (not plugged in)";
        if (ChoiceBegin("##audioout", preview.c_str())) {
            std::string pick = chosen;
            if (ImGui::Selectable(kDefaultName, chosen.empty())) pick.clear();
            if (!chosen.empty() && !chosenPlugged) ImGui::Selectable((chosen + "  (not plugged in)").c_str(), true);
            for (const std::string& n : app.audioOutputs) if (ImGui::Selectable(n.c_str(), n == chosen)) pick = n;
            ChoiceEnd();
            if (pick != chosen) { app.audioOutputChoice = pick; app.audioRestartRequested = true; }
        }
        ImGui::TextDisabled("If the chosen output is unplugged, the sound goes to the system default and comes back when it returns.");
        ImGui::Spacing();

        ImGui::TextUnformatted("BUFFER");
        const int rate = app.audioOpenedRate > 0 ? app.audioOpenedRate : app.engine.SampleRate();
        char labs[3][40];
        const char* labPtr[3];
        int bufIdx = 0;
        for (int i = 0; i < 3; ++i) {
            const int b = audiosettings::kBufferChoices[i];
            std::snprintf(labs[i], sizeof(labs[i]), "%d  (%.1f ms)", b, audiosettings::BufferMs(b, rate));
            labPtr[i] = labs[i];
            if (app.audioBufferChoice == b) bufIdx = i;
        }
        if (Segmented("##audiobuf", &bufIdx, labPtr, 3, 118.0f)) {
            app.audioBufferChoice = audiosettings::kBufferChoices[bufIdx];
            app.audioRestartRequested = true;
        }
        ImGui::TextDisabled("Smaller = pads answer faster.  Bigger = safer from crackles on a busy computer.");
        ImGui::TextDisabled("If DSP/XRUN shows dropouts, take 256 or 512.");
        ImGui::Spacing();

        Mixer& mx = app.engine.GetMixer();
        if (app.audioOpenedRate <= 0) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(vis::kRed), "Now: no output device found. The program tries again every few seconds.");
        } else {
            if (app.audioOnFallback)
                ImGui::TextColored(kAmber, "\"%s\" is not plugged in or could not be opened: playing on the system output.", app.audioOutputChoice.c_str());
            const int frames = mx.audioBufferFrames.load(std::memory_order_relaxed) > 0 ? mx.audioBufferFrames.load(std::memory_order_relaxed) : app.audioOpenedFrames;
            const unsigned drops = mx.audioXruns.load(std::memory_order_relaxed);
            char dropText[48];
            if (drops == 0) std::snprintf(dropText, sizeof(dropText), "No dropouts this session.");
            else std::snprintf(dropText, sizeof(dropText), "%u dropout%s this session.", drops, drops == 1 ? "" : "s");
            const ImU32 col = drops == 0 ? vis::kTeal : vis::kAccent;
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col), "Now: %s, %d Hz, %d frames = %.1f ms. %s",
                               app.audioOpenedName.empty() ? "system output" : app.audioOpenedName.c_str(),
                               app.audioOpenedRate, frames, audiosettings::BufferMs(frames, app.audioOpenedRate), dropText);
        }
        ImGui::Spacing();
        if (ImGui::Button("CLOSE", ImVec2(120, 0)) || t8::desktop::interaction::KeyCancel()) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
}

void MenuBar::DrawVersionsDialog(AppState& app) {
    ImGui::PushFont(nullptr, 14.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.38f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("VersionsDialog", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextColored(kAmber, "Versions of \"%s\"", userdirs::ProjectDisplayName(app.projectPath).c_str());
        ImGui::TextDisabled("Dated copies the program makes when a project is opened and before the first save of a session.");
        ImGui::BeginChild("##verlist", ImVec2(460, 170), ImGuiChildFlags_Borders);
        if (backups_.empty()) ImGui::TextDisabled("(no versions yet - they are made automatically)");
        for (const std::string& b : backups_) {
            const bool armed = restoreArmed_ == b;
            if (ImGui::Selectable((BackupLabel(b) + "##" + b).c_str(), armed)) restoreArmed_ = armed ? std::string() : b;
        }
        ImGui::EndChild();
        if (!restoreArmed_.empty()) {
            ImGui::TextColored(kAmber, "Replace the open project with the version of %s?", BackupLabel(restoreArmed_).c_str());
            ImGui::TextDisabled("The project file becomes this version. Changes you have not saved are lost.");
            if (ImGui::Button("RESTORE", ImVec2(120, 0))) {
                app.RestoreBackupFile(restoreArmed_);   // takes the scene lock only for the short swaps (LIVE-4)
                restoreArmed_.clear();
                backups_ = ListProjectBackups(app.projectPath);
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("CLOSE", ImVec2(120, 0)) || t8::desktop::interaction::KeyCancel()) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
}

void MenuBar::DrawBeatsDialog(AppState& app) {
    ImGui::PushFont(nullptr, 14.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));
    const float kW = 470.0f;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.38f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("BeatsDialog", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextColored(kAmber, "My Beats");
        ImGui::TextDisabled("Save the drums of the edited scene, reuse them anywhere. Same name = update that beat.");
        ImGui::SetNextItemWidth(250);
        const bool beatEnter = ImGui::InputTextWithHint("##newbeat", "beat name", newBeat_, sizeof(newBeat_), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine(0, 6);
        if ((ImGui::Button("SAVE BEAT", ImVec2(96, 0)) || beatEnter) && newBeat_[0] != '\0') {
            SceneLock lock(app.sequencer.PatternMutex(), "MENU");
            app.SaveCurrentAsCustomPattern(newBeat_);
            newBeat_[0] = '\0';
        }
        if (ImGui::IsItemHovered()) Hint("Saves the drum tracks of the scene you are editing into the Library.");

        const auto beats = app.CustomPatternNamesCached();   // copy: the list changes when we rename / delete below
        ImGui::BeginChild("##beatlist", ImVec2(kW, 150), ImGuiChildFlags_Borders);
        if (beats.empty()) ImGui::TextDisabled("(no saved beats yet)");
        std::string applyName, renameName, deleteName;
        for (const std::string& n : beats) {
            ImGui::PushID(n.c_str());
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(n.c_str());
            ImGui::SameLine(kW - 200.0f);
            if (MiniButton("APPLY")) applyName = n;
            if (ImGui::IsItemHovered()) Hint("Replace the drum tracks of the edited scene with this beat (undoable)");
            ImGui::SameLine(0, 4);
            if (MiniButton("RENAME")) renameName = n;
            ImGui::SameLine(0, 4);
            const bool armed = deleteArmed_ == n;
            if (MiniButton(armed ? "SURE?" : "DELETE")) { if (armed) deleteName = n; else deleteArmed_ = n; }
            ImGui::PopID();
        }
        ImGui::EndChild();
        if (!applyName.empty()) {
            SceneLock lock(app.sequencer.PatternMutex(), "MENU");
            app.ApplyCustomPattern(applyName);
        }
        if (!renameName.empty()) {
            renameTarget_ = renameName;
            std::snprintf(renameBuf_, sizeof(renameBuf_), "%s", renameName.c_str());
            ImGui::OpenPopup("BeatRename");
        }
        if (!deleteName.empty()) { app.DeleteCustomPattern(deleteName); deleteArmed_.clear(); }
        if (NamePrompt("BeatRename", renameBuf_, sizeof(renameBuf_), "RENAME BEAT", "RENAME")) app.RenameCustomPattern(renameTarget_, renameBuf_);

        ImGui::TextDisabled("%s", userdirs::Tilde(userdirs::BeatsDir()).c_str());
        ImGui::SameLine(kW - 120.0f);
        if (ImGui::Button("CLOSE", ImVec2(120, 0)) || t8::desktop::interaction::KeyCancel()) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
}

} // namespace t8::desktop
