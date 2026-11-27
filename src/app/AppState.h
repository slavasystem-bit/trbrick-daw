#pragma once
// All the application state shared by the desktop panels (ui_desktop/*) and
// the keyboard shortcuts: project, engine, sequencer, cursors, clipboards,
// undo and the generator helpers (randomizers, kit/pattern swaps).
#include "core/Types.h"
#include "core/ChordLab.h"
#include "core/StarterGroove.h"
#include "core/Project.h"
#include "core/AudioEngine.h"
#include "core/Sequencer.h"
#include "core/Recorder.h"
#include "core/VariSpeed.h"
#include "core/Scales.h"
#include "core/Grooves.h"
#include "core/Euclid.h"
#include "core/DrumPatterns.h"
#include "core/Styles.h"
#include "core/CustomPatterns.h"
#include "core/ChordSpec.h"
#include "core/SynthPresets.h"
#include "core/CustomSynthPresets.h"
#include "core/Persistence.h"
#include "core/SoundParts.h"
#include "core/Chords.h"
#include "core/ChordProgressions.h"
#include "core/ChordVoice.h"
#include "core/NotesExport.h"
#include "core/UserDirs.h"
#include "ProjectFiles.h"
#include "KitFolders.h"
#include <filesystem>
#include <fstream>
#include <set>
#include "FilePicker.h"
#include "AsyncLoader.h"
#include "SampleCleanup.h"
#include "SampleLibrary.h"
#include "core/SampleTagStore.h"
#include "core/SampleSetStore.h"
#include <atomic>
#include <memory>
#include "Onsets.h"
#include <functional>
#include <cstdio>
#include <mutex>
#include <vector>
#include <sys/stat.h>
#include <cctype>
#include <array>
#include <deque>
#include <map>
#include <random>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <chrono>

namespace t8 {

struct AppState {
    Project project;
    AudioEngine engine;
    Sequencer sequencer;
    // Live-performance recording (see Recorder.h) -- separate from
    // project/pattern data entirely (not saved into .t8proj), started via
    // Recorder::Init(sampleRate) once in main() after engine.Init().
    Recorder recorder;

    std::string projectPath = "project.t8proj";
    std::string statusMessage;
    float statusMessageTimer = 0.0f;

    // Set by File > Quit; main_desktop checks it once per frame and
    // leaves the loop, which runs the same save-on-quit path as closing the window.
    bool quitRequested = false;
    bool audioRestartRequested = false;   // v44.3: Settings > Restart Audio (also done automatically after sleep / device changes)
    // N1 (LIVE-5 / LIVE-6): Settings > Audio. The choice is saved in ui_settings.cfg; main.cpp opens the device with it and fills
    // the "opened" fields. An empty output name = the system default.
    int audioBufferChoice = 128;              // 128 / 256 / 512 frames asked of the device
    std::string audioOutputChoice;            // the chosen output by name ("" = system default)
    std::vector<std::string> audioOutputs;    // outputs plugged in now (refreshed when the dialog opens and when devices change)
    bool audioOutputsWanted = false;          // the dialog asks main.cpp to read the list again
    std::string audioOpenedName;              // what is actually playing ("" = system default)
    int audioOpenedRate = 0, audioOpenedFrames = 0;
    bool audioOnFallback = false;             // the chosen output is not plugged in: the sound went to the system default

    // Sequencer cursor
    int seqCursorTrack = 0;
    int seqCursorStep = 0;   // 0..63 absolute step index; page = seqCursorStep / 16

    // Desktop LIVE screen: which 16-step page the Sequencer and Piano Roll
    // both show (0 = steps 1-16, 1 = 17-32, ...). Shared so the two panels
    // always look at the same bar.
    int deskStepPage = 0;
    // Desktop LIVE screen: the Piano Roll normally replaces the Mixer
    // whenever a Synth/Chord track is selected. Set when the user closes it
    // with the MIXER button (mixer comes back while a synth stays selected);
    // cleared again when another track is selected or via the mixer's
    // PIANO ROLL button.
    bool deskRollClosed = false;
    bool deskLanesClosed = false;   // drum track selected: false = the LANES editor shows, true = the mixer

    // Piano roll: which synth track it edits (0..2)
    int prSynthIndex = 0;
    int prPatchParam = 0;   // selected sound parameter row (shared by the piano roll and the context block)

    // Context block: true = the CHORD track's patch is being edited instead of SYNTH 1..3
    bool synthViewChordSlot = false;

    // Chord generator state (progression browse + settings)
    // Chord generator: which named progression template is browsed (CUSTOM is a UI-level entry in front of the list)
    int chdProgressionIndex = 0;
    // CUSTOM hand-built progression: four scale degrees (default I-IV-V-I)
    static constexpr int kNumCustomChordSlots = 4;
    std::array<int, kNumCustomChordSlots> chdCustomDegrees = {1, 4, 5, 1};
    // Desktop LIVE: which of the 4 custom chords is "switched on" as a live
    // override of the chord track (-1 = off, the programmed chords play).
    int chdLiveSlot = -1;
    // Rebuilds the live-override chord and pushes it to the sequencer. Cheap;
    // called once per frame. The chord keeps the key / scale it was last
    // (re)built in -- changing the project KEY / SCALE does NOT retune it
    // unless FOLLOW KEY is on; `relock` (a new slot, degree or voicing) takes
    // the current key. Takes the pattern lock itself.
    int chdLiveKey_ = -1, chdLiveScale_ = -1;
    void RefreshChordLive(bool relock = false) {
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        if (chdLiveSlot < 0 || chdLiveSlot >= kNumCustomChordSlots) { sequencer.ClearChordLive(); chdLiveKey_ = -1; return; }
        if (relock || project.chordsFollowKey || chdLiveKey_ < 0) {
            chdLiveKey_ = project.randomizerKeyRoot;
            chdLiveScale_ = project.randomizerScale;
        }
        Chord c = BuildChord(chdLiveKey_, chdLiveScale_, chdCustomDegrees[chdLiveSlot], chdComplex, 3);
        int8_t notes[kMaxChordVoices];
        int n = std::min(static_cast<int>(c.notes.size()), kMaxChordVoices);
        for (int i = 0; i < n; ++i) notes[i] = static_cast<int8_t>(c.notes[i]);
        sequencer.SetChordLive(chdLiveSlot, notes, n);
    }
    bool chdCustomSelected = true;   // the browse cursor is on CUSTOM (the hand-built 4-chord loop)
    bool chdComplex = false;                       // SIMPLE (triads) vs COMPLEX (7th chords)
    ChordOutputMode chdOutputMode = ChordOutputMode::Stack;
    ArpStyle chdArpStyle = ArpStyle::Up;
    int chdArpRateSteps = 1;                        // 1/2/4 steps per arpeggiated note
    // 0 = AUTO (divide the pattern's masterLength evenly across however
    // many chords are in the applied progression), else a fixed step count
    // per chord (4/8/16) regardless of progression length or pattern length.
    int chdChordLengthSteps = 0;

    int menuKitIndex = 0;   // kit currently browsed to

    // Browse positions for the RANDOM / KIT / LIVE blocks. 0 = "RANDOM (ANY)" in each list;
    // rndGenreIndex continues past the built-in genres into custom patterns saved in patterns/.
    int rndGenreIndex = 0;
    // LIVE BREAK row: which fill is armed for the hold-Y live trigger below.
    // Same "0 = RANDOM" convention as rndGenreIndex.
    int rndBreakIndex = 0; // 0 = "RANDOM", 1..kNumDrumBreaks = a specific named break
    // RISER row (live performance, sixth revision) -- same "0 = RANDOM"
    // convention as rndBreakIndex above, browsing RiserPlayer's loaded
    // samples instead of AllDrumBreaks(). See TriggerRiser()/BrowseRiser
    // below and RandomView.cpp's kRowRiser.
    int rndRiserIndex = 0; // 0 = "RANDOM", 1..RiserPlayer::Count() = a specific named riser
    // RANDOM BASS/SYNTH2/SYNTH3 rows: Left/Right toggles each part's own
    // note-density mode independently (0=SIMPLE, 1=COMPLEX -- see
    // RandomizeMelodic). Deliberately per-part, not one shared mode, since
    // a live performer might want a sparse bass under a busy lead or vice
    // versa. Session-only like the rest of this tab's browse state (not
    // persisted to the project file) -- resets to SIMPLE on restart.
    int rndDensityMode[kNumSynthTracks] = {0, 0, 0};   // 0 SIMPLE, 1 COMPLEX, 2 RHYTHM (v44.4)
    // v26: melody randomizers (BASS / SYNTH 2 / SYNTH 3) put chord tones of the chord that is sounding on the strong beats
    bool rndChordAware = true;

    // A suffix queued by a generator ("- C DORIAN", "CHORDS IN F IONIAN!") that the next SetStatus appends once,
    // so every caller's own message gets the key/scale information without each one being edited.
    std::string statusSuffix;
    // N1 (LIVE-9): a literal goes through here, so a switch applied on the audio thread returns before any std::string is built.
    void SetStatus(const char* msg) { if (SwitchApplying()) return; SetStatus(std::string(msg)); }
    void SetStatus(const std::string& msg) { if (SwitchApplying()) return; statusMessage = msg + statusSuffix; statusSuffix.clear(); statusMessageTimer = statusMessage.size() > msg.size() ? 3.5f : 2.0f; }

    // N1: slow file work (samples, kits, folder scans) runs here, off the scene lock; main() starts it, the tests leave it off
    // (then every Post() runs at once, as the old code did). See AsyncLoader.h.
    AsyncLoader loader;
    // A result that arrives late must not land in the wrong place (review of N1 part 3):
    //  - docGen_ changes when another project is adopted (New / Open / Restore): results asked for in the old one are dropped;
    //  - each target (a scene's sampler / synth sample, a drum's kit, a brick's transition) has a request token, bumped by every
    //    request and every direct choice; a result applies only if its token is still the newest AND the target still holds what
    //    it held when the file was asked for (so a newer choice made any other way, an undo, a preset, is not overwritten).
    unsigned docGen_ = 0;
    std::map<std::string, unsigned> reqToken_;
    unsigned BumpRequest(const std::string& key) { return ++reqToken_[key]; }
    bool RequestIsNewest(const std::string& key, unsigned tok) const { auto it = reqToken_.find(key); return it != reqToken_.end() && it->second == tok; }
    static std::string SampleKey(int scene, int tgt) { return "smp:" + std::to_string(scene) + ":" + std::to_string(tgt); }

    // ---- Cached folder listings (kits/, projects/, patterns/) -------------
    // ListKitFolders()/ListProjectFiles()/ListCustomPatternNames() (see
    // app/KitFolders.h, app/ProjectFiles.h, core/CustomPatterns.h) are cached:
    // scanned lazily on first use and again after InvalidateFolderCaches(), which
    // the main loop calls every few seconds (so files added in Finder show up) and
    // the save-a-new-file actions call for their own list.
    std::vector<std::string> cachedKitFolders;
    std::vector<std::string> cachedProjectFiles;
    std::vector<std::string> cachedCustomPatternNames;
    std::vector<std::string> cachedCustomSynthPresetNames;
    bool kitFoldersCacheValid = false;
    bool projectFilesCacheValid = false;
    bool customPatternNamesCacheValid = false;
    bool customSynthPresetNamesCacheValid = false;

    const std::vector<std::string>& KitFolders() {
        if (!kitFoldersCacheValid) { cachedKitFolders = ListKitFolders(); kitFoldersCacheValid = true; }
        return cachedKitFolders;
    }
    const std::vector<std::string>& ProjectFilesCached() {
        // Scan the open project's own directory too (it may live outside projects/).
        if (!projectFilesCacheValid) { cachedProjectFiles = ListProjectFiles(DirNameOf(projectPath)); projectFilesCacheValid = true; }
        return cachedProjectFiles;
    }
    const std::vector<std::string>& CustomPatternNamesCached() {
        if (!customPatternNamesCacheValid) { cachedCustomPatternNames = ListCustomPatternNames(); customPatternNamesCacheValid = true; }
        return cachedCustomPatternNames;
    }
    const std::vector<std::string>& CustomSynthPresetNamesCached() {
        if (!customSynthPresetNamesCacheValid) { cachedCustomSynthPresetNames = ListCustomSynthPresetNames(); customSynthPresetNamesCacheValid = true; }
        return cachedCustomSynthPresetNames;
    }
    // N1 (LIVE-10): the periodic rescan (so files added in Finder show up) reads the folders on the loader thread and swaps the
    // lists in on the next frame; until then the old lists stay. One scan at a time.
    bool folderScanRunning_ = false;
    unsigned folderCacheGen_ = 0;   // bumped by InvalidateFolderCaches: a scan that started before it is not swapped in
    void RescanFoldersInBackground() {
        if (!loader.Running()) { InvalidateFolderCaches(); return; }
        if (folderScanRunning_) return;
        folderScanRunning_ = true;
        const unsigned gen = folderCacheGen_;
        struct Lists { std::vector<std::string> kits, projects, patterns, presets; };
        auto out = std::make_shared<Lists>();
        const std::string projDir = DirNameOf(projectPath);
        loader.Post([out, projDir] {
            out->kits = ListKitFolders(); out->projects = ListProjectFiles(projDir);
            out->patterns = ListCustomPatternNames(); out->presets = ListCustomSynthPresetNames();
        }, [this, out, gen] {
            folderScanRunning_ = false;
            if (gen != folderCacheGen_) return;   // the caches were invalidated meanwhile (a file was saved): the accessors rescan
            cachedKitFolders = std::move(out->kits); kitFoldersCacheValid = true;
            cachedProjectFiles = std::move(out->projects); projectFilesCacheValid = true;
            cachedCustomPatternNames = std::move(out->patterns); customPatternNamesCacheValid = true;
            cachedCustomSynthPresetNames = std::move(out->presets); customSynthPresetNamesCacheValid = true;
        });
    }
    // Cheap: only clears the flags; the rescan happens on the next accessor call.
    void InvalidateFolderCaches() {
        ++folderCacheGen_;
        kitFoldersCacheValid = false;
        projectFilesCacheValid = false;
        customPatternNamesCacheValid = false;
        customSynthPresetNamesCacheValid = false;
    }

    Pattern& CurrentPattern() { return project.patterns[project.currentPattern]; }

    // The EDITED pattern (CurrentPattern) and the PLAYED pattern differ on the
    // desktop LIVE screen while a chain is running. The audio-thread mixer
    // always mirrors the PLAYED pattern; UI code that edits the other one
    // must not push those values into it.
    bool EditIsPlaying() const { return sequencer.PlayPatternIndex() == project.currentPattern; }
    // Push a pattern's mixer values to the audio thread -- only if that
    // pattern is the one being played.
    void SyncMixer(const Pattern& pat) {
        if (EditIsPlaying()) { engine.GetMixer().SyncFromPattern(pat); sequencer.ApplySceneSound(project.currentPattern); }
    }
    // The mixer state the UI should show / write for the edited pattern: the
    // real one when it is also the played pattern, otherwise a shadow copy
    // refreshed from the pattern (writes to it are discarded; the pattern's
    // own Track fields are the persistent copy, which UI code writes too).
    Mixer& EditMixer() {
        if (EditIsPlaying()) return engine.GetMixer();
        shadowMixer_.SyncFromPattern(CurrentPattern());
        return shadowMixer_;
    }
    Mixer shadowMixer_;

    // ---- Undo -------------------------------------------------------
    // Two kinds of entry share one stack:
    //   Pattern -- a snapshot of ONE scene's step/track data (steps, volume/
    //     pan, bpm/swing/length). Tagged with that scene's index: Undo only
    //     takes entries for the scene currently being edited, so switching
    //     scenes never lets one scene's snapshot overwrite another's.
    //   Sound   -- a snapshot of the project-wide sound state: the 3 synth
    //     patches, the chord patch, master FX / master volume and the song
    //     chain. Synth/FX/chain edits are NOT part of any Pattern.
    // mute / solo belong to the scene but are not part of an undo step: Undo leaves them as they are.
    // Call PushUndo() right before a pattern edit commits and PushSoundUndo()
    // before a sound / FX / chain edit; PushSoundUndo coalesces a continuous
    // knob drag (changes less than 0.8 s apart) into ONE undo step.
    struct UndoEntry {
        enum class Kind { Pattern, Sound, Link } kind = Kind::Pattern;
        // Kind::Link (v44.59, DATA-6): the LINK groups of the LIVE scenes and the mute / solo of every LIVE scene (linking overwrites them)
        std::array<uint8_t, kNumLiveScenes> links{};
        std::array<uint16_t, kNumLiveScenes> muteBits{}, soloBits{};
        int patternIndex = 0;                 // Kind::Pattern: which scene
        Pattern pattern;                      // Kind::Pattern
        std::array<SynthPatch, kNumSynthTracks> synthPatches{};   // Kind::Sound ... (the patches of scene `patternIndex`)
        SynthPatch chordPatch;
        KickSynthSettings kick;
        FxSettings fx;
        float masterVolume = 0.4f;
        std::array<ChainStep, kMaxChainSteps> songChain{};
        int songChainLength = 0;
        bool hasChordScene = false;           // Kind::Pattern while the chord bank is on: the edited chord scene too
        bool hasBank = false;                 // v44.44: the step also switched the chord bank / the edited slot (BANK ON / OFF, the start of a CHORD LAB draft)
        bool bankOn = false;
        int bankEdit = 0;
        int chordSceneIndex = 0;
        ChordScene chordScene;
        // v44.23: a sound step that also moves the sound of the track and of some brick slots (TAKE SOUND TO TRACK, the first throw)
        bool hasTrackSound = false;
        int trackSoundMask = 0;
        Pattern trackSound;
        std::vector<std::pair<int, Pattern>> bulk;   // brick slots whose SOUND parts are restored (notes untouched)
        uint64_t serial = 0;                  // C2: when the step was pushed (UndoMark / UndoBackTo)
    };
    static constexpr size_t kMaxUndo = 256;
    std::vector<UndoEntry> undoStack;
    std::string lastUndoLabel = "UNDO";       // what the last Undo() reverted (for the status line)
    double lastSoundUndoPush_ = -100.0;

    static double UndoNowSeconds() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    uint64_t undoSerial_ = 0;
    // Every push onto undoStack ends here: stamp the new step, then drop the oldest steps over kMaxUndo.
    void TrimUndo() {
        if (!undoStack.empty()) undoStack.back().serial = ++undoSerial_;
        while (undoStack.size() > kMaxUndo) undoStack.erase(undoStack.begin());
    }
    // C2 (UNDO ALL in CHORD LAB): a marker for "now". Steps pushed after it (edits, redos) have a larger serial.
    // The next sound edit starts a fresh step, so it never merges into a step made before the marker.
    uint64_t UndoMark() { lastSoundUndoPush_ = -100.0; return undoSerial_; }
    // Number of undo steps after `mark` that Undo() would take now (the steps of other scenes wait, as in Undo()).
    int UndoStepsSince(uint64_t mark) const {
        int n = 0;
        for (const UndoEntry& e : undoStack)
            if (e.serial > mark && !(e.kind == UndoEntry::Kind::Pattern && e.patternIndex != project.currentPattern)) ++n;
        return n;
    }
    // Undo every step pushed after `mark`, stopping at the first step from before it. Returns how many were undone.
    int UndoBackTo(uint64_t mark) {
        int n = 0;
        for (;;) {
            const int i = FindApplicable(undoStack);
            if (i < 0 || undoStack[static_cast<size_t>(i)].serial <= mark) break;
            if (!Undo()) break;
            ++n;
        }
        return n;
    }
    void PushUndo() {
        if (SwitchApplying()) return;   // v44.54: a queued LIVE switch is applied on the audio thread; its undo step was pushed at the click
        sequencer.CheckSceneLock("PushUndo");
        redoStack.clear();
        UndoEntry e;
        e.kind = UndoEntry::Kind::Pattern;
        e.patternIndex = project.currentPattern;
        e.pattern = CurrentPattern();
        if (project.chordBankOn) {
            e.hasChordScene = true;
            e.chordSceneIndex = std::clamp(project.chordSceneEdit, 0, kNumChordScenes - 1);
            e.chordScene = project.chordScenes[e.chordSceneIndex];
        }
        undoStack.push_back(std::move(e));
        TrimUndo();
    }
    // v44.44: like PushUndo, and the step also remembers whether the chord bank is on, which slot is edited, and the content of
    // `slot` (the one about to be filled). Undo puts all of it back, so BANK ON / OFF and the start of a CHORD LAB draft are one step.
    void PushUndoBank(int slot) {
        sequencer.CheckSceneLock("PushUndoBank");
        redoStack.clear();
        UndoEntry e;
        e.kind = UndoEntry::Kind::Pattern;
        e.patternIndex = project.currentPattern;
        e.pattern = CurrentPattern();
        e.hasChordScene = true;
        e.chordSceneIndex = std::clamp(slot, 0, kNumChordScenes - 1);
        e.chordScene = project.chordScenes[static_cast<size_t>(e.chordSceneIndex)];
        e.hasBank = true; e.bankOn = project.chordBankOn; e.bankEdit = project.chordSceneEdit;
        undoStack.push_back(std::move(e));
        TrimUndo();
    }
    // withTrack: the step also remembers the sound of the track and the sound parts of the brick slots in `slots` (never merged with the
    // step before it).
    void PushSoundUndo(bool withTrack = false, const std::vector<int>& slots = {}) {
        if (SwitchApplying()) return;
        sequencer.CheckSceneLock("PushSoundUndo");
        redoStack.clear();
        double now = UndoNowSeconds();
        bool coalesce = !withTrack && !undoStack.empty() && undoStack.back().kind == UndoEntry::Kind::Sound &&
                        undoStack.back().patternIndex == project.currentPattern && (now - lastSoundUndoPush_) < 0.8;
        lastSoundUndoPush_ = now;
        if (coalesce) return;
        UndoEntry e;
        e.kind = UndoEntry::Kind::Sound;
        e.patternIndex = project.currentPattern;
        e.synthPatches = CurrentPattern().synthPatches;
        e.chordPatch = CurrentPattern().chordPatch;
        e.kick = CurrentPattern().kick;
        e.fx = project.fx;
        e.masterVolume = project.masterVolume;
        e.songChain = project.songChain;
        e.songChainLength = project.songChainLength;
        if (withTrack) {
            e.hasTrackSound = true;
            e.trackSound = project.patterns[static_cast<size_t>(kTrackSoundSlot)];
            e.trackSoundMask = project.trackSoundMask;
            for (int sl : slots) if (sl >= 0 && sl < kNumPatterns) e.bulk.emplace_back(sl, project.patterns[static_cast<size_t>(sl)]);
        }
        undoStack.push_back(std::move(e));
        TrimUndo();
    }
    // Redo (v44.15): Undo moves the undone entry's *opposite* (the state it replaced) to redoStack; any new edit clears it.
    std::vector<UndoEntry> redoStack;
    // Takes back the step a cancelled gesture pushed (ValueResult::cancelled); nothing is restored from it.
    void DropLastUndo() { if (!undoStack.empty()) undoStack.pop_back(); lastSoundUndoPush_ = -100.0; }
    void ClearUndo() { undoStack.clear(); redoStack.clear(); lastSoundUndoPush_ = -100.0; }

    // v44.59 (DATA-6): one undo step for a LINK change: the groups and the mute / solo of every LIVE scene. Applies whatever scene is open.
    UndoEntry CaptureLinks() const {
        UndoEntry e;
        e.kind = UndoEntry::Kind::Link;
        e.patternIndex = project.currentPattern;
        e.links = project.sceneLink;
        for (int s = 0; s < kNumLiveScenes; ++s)
            for (int t = 0; t < kNumTracks; ++t) {
                const Track& tr = project.patterns[static_cast<size_t>(s)].tracks[static_cast<size_t>(t)];
                if (tr.mute) e.muteBits[static_cast<size_t>(s)] |= static_cast<uint16_t>(1u << t);
                if (tr.solo) e.soloBits[static_cast<size_t>(s)] |= static_cast<uint16_t>(1u << t);
            }
        return e;
    }
    void PushLinkUndo() {
        if (SwitchApplying()) return;
        redoStack.clear();
        undoStack.push_back(CaptureLinks());
        TrimUndo();
    }
    // A snapshot of the CURRENT state in the same shape (kind, scene, chord scene) as `like`.
    UndoEntry CaptureLike(const UndoEntry& like) {
        if (like.kind == UndoEntry::Kind::Link) return CaptureLinks();
        UndoEntry c;
        c.kind = like.kind;
        c.patternIndex = like.patternIndex;
        Pattern& p = project.patterns[static_cast<size_t>(like.patternIndex)];
        if (like.kind == UndoEntry::Kind::Pattern) {
            c.pattern = p;
            c.hasChordScene = like.hasChordScene;
            c.chordSceneIndex = like.chordSceneIndex;
            if (like.hasChordScene) c.chordScene = project.chordScenes[static_cast<size_t>(like.chordSceneIndex)];
            c.hasBank = like.hasBank; c.bankOn = project.chordBankOn; c.bankEdit = project.chordSceneEdit;
        } else {
            c.synthPatches = p.synthPatches;
            c.chordPatch = p.chordPatch;
            c.kick = p.kick;
            c.fx = project.fx;
            c.masterVolume = project.masterVolume;
            c.songChain = project.songChain;
            c.songChainLength = project.songChainLength;
            if (like.hasTrackSound) {
                c.hasTrackSound = true;
                c.trackSound = project.patterns[static_cast<size_t>(kTrackSoundSlot)];
                c.trackSoundMask = project.trackSoundMask;
                for (const auto& b : like.bulk) c.bulk.emplace_back(b.first, project.patterns[static_cast<size_t>(b.first)]);
            }
        }
        return c;
    }
    // Writes the snapshot back. A Pattern entry must belong to the scene being edited (the caller checks).
    std::string ApplyEntry(const UndoEntry& e) {
        const int cur = project.currentPattern;
        if (e.kind == UndoEntry::Kind::Link) {
            project.sceneLink = e.links;
            for (int s = 0; s < kNumLiveScenes; ++s)
                for (int t = 0; t < kNumTracks; ++t) {
                    Track& tr = project.patterns[static_cast<size_t>(s)].tracks[static_cast<size_t>(t)];
                    tr.mute = (e.muteBits[static_cast<size_t>(s)] >> t) & 1u;
                    tr.solo = (e.soloBits[static_cast<size_t>(s)] >> t) & 1u;
                }
            const int pl = sequencer.PlayPatternIndex();
            if (pl >= 0 && pl < kNumLiveScenes)
                for (int t = 0; t < kNumTracks; ++t) {
                    engine.GetMixer().tracks[static_cast<size_t>(t)].mute = project.patterns[static_cast<size_t>(pl)].tracks[static_cast<size_t>(t)].mute;
                    engine.GetMixer().tracks[static_cast<size_t>(t)].solo = project.patterns[static_cast<size_t>(pl)].tracks[static_cast<size_t>(t)].solo;
                }
            return "LINK";
        }
        if (e.kind == UndoEntry::Kind::Pattern) {
            bool keepMute[kNumTracks], keepSolo[kNumTracks];
            for (int t = 0; t < kNumTracks; ++t) { keepMute[t] = CurrentPattern().tracks[t].mute; keepSolo[t] = CurrentPattern().tracks[t].solo; }
            CurrentPattern() = e.pattern;
            if (e.hasChordScene) project.chordScenes[static_cast<size_t>(e.chordSceneIndex)] = e.chordScene;
            if (e.hasBank) {
                project.chordBankOn = e.bankOn;
                project.chordSceneEdit = std::clamp(e.bankEdit, 0, kNumChordScenes - 1);
                sequencer.SetChordSceneNow(project.chordBankOn ? project.chordSceneEdit : 0);
                RefreshChordLive(true);
            }
            for (int t = 0; t < kNumTracks; ++t) { CurrentPattern().tracks[t].mute = keepMute[t]; CurrentPattern().tracks[t].solo = keepSolo[t]; }
            if (EditIsPlaying()) { engine.GetMixer().SyncFromPattern(CurrentPattern()); sequencer.ApplySceneSound(project.currentPattern); }
            kitsDirty_ = true;   // the snapshot may carry another kit
            return "SCENE " + std::to_string(cur + 1);
        }
        project.patterns[static_cast<size_t>(e.patternIndex)].synthPatches = e.synthPatches;
        project.patterns[static_cast<size_t>(e.patternIndex)].chordPatch = e.chordPatch;
        project.patterns[static_cast<size_t>(e.patternIndex)].kick = e.kick;
        project.fx = e.fx;
        project.masterVolume = e.masterVolume;
        project.songChain = e.songChain;
        project.songChainLength = e.songChainLength;
        if (e.hasTrackSound) {
            CopyAllParts(project.patterns[static_cast<size_t>(kTrackSoundSlot)], e.trackSound);
            project.trackSoundMask = e.trackSoundMask;
            for (const auto& b : e.bulk) {
                Pattern& bp = project.patterns[static_cast<size_t>(b.first)];
                CopyAllParts(bp, b.second);
                bp.ownSound = b.second.ownSound;
                if (b.first == sequencer.PlayPatternIndex()) { sequencer.ApplySceneSound(b.first); engine.GetMixer().SyncFromPattern(bp); }
            }
            kitsDirty_ = true;
        }
        if (e.patternIndex == sequencer.PlayPatternIndex()) {
            sequencer.ApplySceneSound(e.patternIndex);
            engine.GetMixer().kickSynth = project.patterns[static_cast<size_t>(e.patternIndex)].kick;
        }
        engine.GetMixer().SyncFxFromProject(project);
        lastSoundUndoPush_ = -100.0;   // the next sound edit starts a fresh step
        return "SOUND / FX / TRACK";
    }
    // Last entry of `stack` that can be applied now (a Pattern entry only while its scene is the edited one); -1 if none.
    int FindApplicable(const std::vector<UndoEntry>& stack) const {
        for (int i = static_cast<int>(stack.size()) - 1; i >= 0; --i) {
            const UndoEntry& e = stack[static_cast<size_t>(i)];
            if (e.kind == UndoEntry::Kind::Pattern && e.patternIndex != project.currentPattern) continue;   // another scene's history (a Link step applies anywhere)
            return i;
        }
        return -1;
    }
    bool Undo() {
        const int i = FindApplicable(undoStack);
        if (i < 0) return false;
        UndoEntry e = std::move(undoStack[static_cast<size_t>(i)]);
        undoStack.erase(undoStack.begin() + i);
        UndoEntry opposite = CaptureLike(e);
        lastUndoLabel = "UNDO: " + ApplyEntry(e);
        redoStack.push_back(std::move(opposite));
        if (redoStack.size() > kMaxUndo) redoStack.erase(redoStack.begin());
        return true;
    }
    bool Redo() {
        const int i = FindApplicable(redoStack);
        if (i < 0) return false;
        UndoEntry e = std::move(redoStack[static_cast<size_t>(i)]);
        redoStack.erase(redoStack.begin() + i);
        UndoEntry opposite = CaptureLike(e);
        lastUndoLabel = "REDO: " + ApplyEntry(e);
        undoStack.push_back(std::move(opposite));   // not PushUndo(): the rest of the redo history stays
        TrimUndo();
        return true;
    }

    // ---- Pattern copy/paste -------------------------------------------
    // Cmd+Shift+C / Cmd+Shift+V. Deliberately whole-pattern (all tracks + bpm/
    // swing/length), not per-track -- the use case is "make a variation of this
    // scene in a new slot", which needs the whole thing.
    bool hasPatternClipboard = false;
    Pattern patternClipboard;

    void CopyPattern() {
        patternClipboard = CurrentPattern();
        hasPatternClipboard = true;
    }
    bool PastePattern() {
        if (!hasPatternClipboard) return false;
        PushUndo();
        CurrentPattern() = patternClipboard;
        SyncMixer(CurrentPattern());
        return true;
    }

    // ---- Per-track copy/paste -----------------------------------------
    // Copy one track (steps + per-track settings) and paste it over another
    // track of the same kind -- the single-track counterpart to whole-pattern
    // copy/paste above (Cmd+C / Cmd+V, or the track's right-click menu). Works
    // across scenes (switch the scene between copy and paste) AND across different tracks
    // (switch which track is selected between copy and paste) -- e.g.
    // duplicate a bassline from bank 1 onto bank 3, or copy the kick's
    // groove/level onto the snare. Restricted to same TrackType
    // (drum-onto-drum or synth-onto-synth): a drum step's variant/accent
    // fields and a synth step's note/slide/length fields aren't
    // interchangeable, so pasting onto a mismatched track type fails with
    // a status message instead of silently producing garbage. The
    // destination's own identity (name/type/slotIndex) is deliberately
    // kept, not overwritten -- only the groove + sound-shaping settings
    // transfer, so pasting Kick's pattern onto Snare still plays as Snare.
    bool hasTrackClipboard = false;
    Track trackClipboard;

    void CopyTrack(int trackIndex) {
        if (trackIndex < 0 || trackIndex >= kNumTracks) return;
        trackClipboard = CurrentPattern().tracks[trackIndex];
        hasTrackClipboard = true;
        SetStatus(trackClipboard.name + " TRACK COPIED");
    }
    void PasteTrack(int trackIndex) {
        if (trackIndex < 0 || trackIndex >= kNumTracks) return;
        if (!hasTrackClipboard) {
            SetStatus("CLIPBOARD EMPTY");
            return;
        }
        Track& dest = CurrentPattern().tracks[trackIndex];
        if (dest.type != trackClipboard.type) {
            SetStatus("CAN'T PASTE: TYPE MISMATCH");
            return;
        }
        PushUndo();
        std::string keepName = dest.name;
        TrackType keepType = dest.type;
        int keepSlot = dest.slotIndex;
        const bool keepMute = dest.mute, keepSolo = dest.solo;   // v44.5.1: mute / solo belong to the scene, a paste does not carry them
        dest = trackClipboard;
        dest.name = keepName;
        dest.type = keepType;
        dest.slotIndex = keepSlot;
        dest.mute = keepMute; dest.solo = keepSolo;
        SyncMixer(CurrentPattern());
        SetStatus(keepName + " TRACK PASTED");
    }

    // ---- Instruments between scenes (v44.5.1) ----------------------------------
    // Undo entry for ANY scene (PushUndo only knows the edited one). Undo takes it back while that scene is the edited one.
    void PushUndoForScene(int scene) {
        scene = std::clamp(scene, 0, kNumPatterns - 1);
        if (scene == project.currentPattern) { PushUndo(); return; }
        redoStack.clear();
        UndoEntry e;
        e.kind = UndoEntry::Kind::Pattern;
        e.patternIndex = scene;
        e.pattern = project.patterns[scene];
        undoStack.push_back(std::move(e));
        TrimUndo();
    }
    // True when the track holds any note (own steps, so the chord track of a scene counts too).
    bool TrackHasNotes(int scene, int trackIndex) const {
        if (scene < 0 || scene >= kNumPatterns || trackIndex < 0 || trackIndex >= kNumTracks) return false;
        for (const Step& st : project.patterns[scene].tracks[trackIndex].steps) if (st.active) return true;
        // v44.48: with the chord bank on, the chords are not in the scene but in the chord scenes (any of them can play), so the
        // chord track "has notes" when one of those does. Before, WHO PLAYS showed the chord bar as an empty slot you could not press.
        if (project.chordBankOn && trackIndex == kChordTrackIndex)
            for (const ChordScene& cs : project.chordScenes) for (const Step& st : cs.track.steps) if (st.active) return true;
        return false;
    }
    // Copies one instrument (notes, its own length, level / pan / sends / insert FX / EQ - everything of the track except
    // its identity and its mute / solo, which stay the destination's) from srcScene to dstScene. dstTrack = -1 means the same
    // track; another track must be of the same kind (drum onto drum, synth onto synth). Returns false with a status when it
    // cannot be done. Playing scenes pick the change up at once.
    bool CopyInstrument(int srcScene, int srcTrack, int dstScene, int dstTrack = -1) {
        if (dstTrack < 0) dstTrack = srcTrack;
        if (srcScene < 0 || srcScene >= kNumPatterns || dstScene < 0 || dstScene >= kNumPatterns ||
            srcTrack < 0 || srcTrack >= kNumTracks || dstTrack < 0 || dstTrack >= kNumTracks) return false;
        if (srcScene == dstScene && srcTrack == dstTrack) return false;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        const Track src = project.patterns[srcScene].tracks[srcTrack];
        Track& dst = project.patterns[dstScene].tracks[dstTrack];
        if (dst.type != src.type) { SetStatus("CAN'T COPY: TRACK TYPE MISMATCH"); return false; }
        PushUndoForScene(dstScene);
        const std::string keepName = dst.name; const TrackType keepType = dst.type; const int keepSlot = dst.slotIndex;
        const bool keepMute = dst.mute, keepSolo = dst.solo;
        dst = src;
        dst.name = keepName; dst.type = keepType; dst.slotIndex = keepSlot; dst.mute = keepMute; dst.solo = keepSolo;
        if (dstScene == sequencer.PlayPatternIndex()) engine.GetMixer().SyncFromPattern(project.patterns[dstScene]);
        std::string msg = keepName + (srcTrack == dstTrack ? "" : " <- " + src.name) + ": SCENE " + std::to_string(srcScene + 1) +
                          " -> SCENE " + std::to_string(dstScene + 1);
        if (!src.useOwnLength && project.patterns[srcScene].masterLength != project.patterns[dstScene].masterLength)
            msg += " (SCENE LENGTH " + std::to_string(project.patterns[srcScene].masterLength) + " -> " + std::to_string(project.patterns[dstScene].masterLength) + ")";
        SetStatus(msg);
        return true;
    }
    // v44.5.2: the same instrument usually wants the same level in every scene. Takes the track's level from srcScene and sets it
    // in the other scenes: onlyChain = only the scenes used in the chain, otherwise all 16; withMix = also pan, sends, insert FX
    // and EQ (everything the mixer strip holds), otherwise the fader only. Notes, mute and solo are not touched. Returns how many
    // scenes changed; each gets its own undo entry (Undo takes it back while that scene is edited).
    int MatchInstrumentLevel(int srcScene, int trackIndex, bool onlyChain, bool withMix) {
        if (srcScene < 0 || srcScene >= kNumPatterns || trackIndex < 0 || trackIndex >= kNumTracks) return 0;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        const Track src = project.patterns[srcScene].tracks[trackIndex];
        std::array<bool, kNumPatterns> inChain{};
        for (int i = 0; i < project.songChainLength; ++i) inChain[static_cast<size_t>(std::clamp(project.songChain[static_cast<size_t>(i)].patternIndex, 0, kNumPatterns - 1))] = true;
        auto same = [&](const Track& t) {
            if (t.volume != src.volume) return false;
            if (!withMix) return true;
            return t.pan == src.pan && t.delaySend == src.delaySend && t.reverbSend == src.reverbSend && t.insertDrive == src.insertDrive &&
                   t.insertEqTilt == src.insertEqTilt && t.insertChorus == src.insertChorus && t.autopanRate == src.autopanRate &&
                   t.autopanDepth == src.autopanDepth && t.width == src.width && t.spread == src.spread &&
                   t.eqHpfHz == src.eqHpfHz && t.eqHpfSlope == src.eqHpfSlope && t.eqLowDb == src.eqLowDb && t.eqLowHz == src.eqLowHz &&
                   t.eqMidDb == src.eqMidDb && t.eqMidHz == src.eqMidHz && t.eqMidQ == src.eqMidQ && t.eqHighDb == src.eqHighDb && t.eqHighHz == src.eqHighHz;
        };
        int changed = 0;
        for (int sc = 0; sc < kNumPatterns; ++sc) {
            if (sc == srcScene || !SlotInUse(sc) || (onlyChain && !inChain[static_cast<size_t>(sc)])) continue;
            Track& d = project.patterns[sc].tracks[trackIndex];
            if (same(d)) continue;
            PushUndoForScene(sc);
            d.volume = src.volume;
            if (withMix) {
                d.pan = src.pan; d.delaySend = src.delaySend; d.reverbSend = src.reverbSend; d.insertDrive = src.insertDrive;
                d.insertEqTilt = src.insertEqTilt; d.insertChorus = src.insertChorus; d.autopanRate = src.autopanRate; d.autopanDepth = src.autopanDepth;
                d.width = src.width; d.spread = src.spread;
                d.eqHpfHz = src.eqHpfHz; d.eqHpfSlope = src.eqHpfSlope; d.eqLowDb = src.eqLowDb; d.eqLowHz = src.eqLowHz;
                d.eqMidDb = src.eqMidDb; d.eqMidHz = src.eqMidHz; d.eqMidQ = src.eqMidQ; d.eqHighDb = src.eqHighDb; d.eqHighHz = src.eqHighHz;
            }
            if (sc == sequencer.PlayPatternIndex()) engine.GetMixer().SyncFromPattern(project.patterns[sc]);
            ++changed;
        }
        SetStatus(src.name + (withMix ? " MIX" : " LEVEL") + " MATCHED IN " + std::to_string(changed) + " SCENE" + (changed == 1 ? "" : "S"));
        return changed;
    }
    // v44.42 MIX LAB: the level, pan and channel EQ of one instrument from srcScene into every other scene in use (live scenes and
    // bricks; onlyChain = only the scenes the chain uses). Sends, insert FX, mute / solo and notes are NOT touched (unlike
    // MatchInstrumentLevel with withMix). Returns how many scenes changed; each gets its own undo entry.
    int MatchLevelPanEq(int srcScene, int trackIndex, bool onlyChain) {
        if (srcScene < 0 || srcScene >= kNumPatterns || trackIndex < 0 || trackIndex >= kNumTracks) return 0;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        const Track src = project.patterns[srcScene].tracks[trackIndex];
        std::array<bool, kNumPatterns> inChain{};
        for (int i = 0; i < project.songChainLength; ++i) inChain[static_cast<size_t>(std::clamp(project.songChain[static_cast<size_t>(i)].patternIndex, 0, kNumPatterns - 1))] = true;
        auto same = [&](const Track& t) {
            return t.volume == src.volume && t.pan == src.pan && t.eqHpfHz == src.eqHpfHz && t.eqHpfSlope == src.eqHpfSlope && t.eqLowDb == src.eqLowDb && t.eqLowHz == src.eqLowHz &&
                   t.eqMidDb == src.eqMidDb && t.eqMidHz == src.eqMidHz && t.eqMidQ == src.eqMidQ && t.eqHighDb == src.eqHighDb && t.eqHighHz == src.eqHighHz;
        };
        int changed = 0;
        for (int sc = 0; sc < kNumPatterns; ++sc) {
            if (sc == srcScene || !SlotInUse(sc) || (onlyChain && !inChain[static_cast<size_t>(sc)])) continue;
            Track& d = project.patterns[sc].tracks[trackIndex];
            if (same(d)) continue;
            PushUndoForScene(sc);
            d.volume = src.volume; d.pan = src.pan;
            d.eqHpfHz = src.eqHpfHz; d.eqHpfSlope = src.eqHpfSlope; d.eqLowDb = src.eqLowDb; d.eqLowHz = src.eqLowHz;
            d.eqMidDb = src.eqMidDb; d.eqMidHz = src.eqMidHz; d.eqMidQ = src.eqMidQ; d.eqHighDb = src.eqHighDb; d.eqHighHz = src.eqHighHz;
            if (sc == sequencer.PlayPatternIndex()) engine.GetMixer().SyncFromPattern(project.patterns[sc], true);
            ++changed;
        }
        return changed;
    }
    // Erases one instrument completely in a scene: every step of the track (also those beyond the visible length) is emptied.
    // The track's level, FX and own-length settings stay, so a new part can be played in at once. Undoable (in that scene).
    // With the chord bank on, the chords of the EDITED scene live in the bank, so that is what gets cleared there.
    bool ClearInstrument(int scene, int trackIndex) {
        if (scene < 0 || scene >= kNumPatterns || trackIndex < 0 || trackIndex >= kNumTracks) return false;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        PushUndoForScene(scene);
        Pattern& pat = project.patterns[scene];
        if (trackIndex == kChordTrackIndex && project.chordBankOn && scene == project.currentPattern) {
            for (Step& st : StepTrack(pat, trackIndex).steps) st = Step{};
        } else {
            for (Step& st : pat.tracks[trackIndex].steps) st = Step{};
        }
        if (scene == sequencer.PlayPatternIndex()) engine.GetMixer().SyncFromPattern(pat);
        SetStatus(pat.tracks[trackIndex].name + " ERASED IN SCENE " + std::to_string(scene + 1));
        return true;
    }

    // ---- Interface size (desktop) ---------------------------------------------------
    // 0 = AUTO: shrink to fit a small window, never enlarge; otherwise a fixed zoom.
    static constexpr int kNumUiZoom = 7;
    static constexpr float kUiZoomValues[kNumUiZoom] = {0.0f, 0.75f, 0.9f, 1.0f, 1.1f, 1.25f, 1.5f};
    int uiZoomIdx = 0;
    // v26: hides the CHAIN + CHORDS rows of the LIVE screen (CHAIN toggle button) to give the instrument editor more room.

    // ---- Chord bank ----------------------------------------------------------
    // With chordBankOn the chord track's STEPS live in project.chordScenes[chordSceneEdit]
    // (what the sequencer / piano roll / popups show and edit); volume, pan, sends and
    // mute stay on the pattern's own chord track. Everything that edits chord steps goes
    // through StepTrack().
    Track& StepTrack(Pattern& pat, int ti) {
        if (project.chordBankOn && ti == kChordTrackIndex)
            return project.chordScenes[std::clamp(project.chordSceneEdit, 0, kNumChordScenes - 1)].track;
        return pat.tracks[ti];
    }
    const Track& StepTrack(const Pattern& pat, int ti) const {
        if (project.chordBankOn && ti == kChordTrackIndex)
            return project.chordScenes[std::clamp(project.chordSceneEdit, 0, kNumChordScenes - 1)].track;
        return pat.tracks[ti];
    }
    ChordScene& EditChordScene() { return project.chordScenes[std::clamp(project.chordSceneEdit, 0, kNumChordScenes - 1)]; }
    static bool TrackHasSteps(const Track& t) { for (const Step& s : t.steps) if (s.active) return true; return false; }
    bool ChordSceneEmpty(int c) const { return !TrackHasSteps(project.chordScenes[std::clamp(c, 0, kNumChordScenes - 1)].track); }
    // Short label of a chord scene: the chords it plays, e.g. "Am F C G".
    static std::string ChordNameFromNotes(const Step& s) {
        if (s.chordSize <= 0) return "";
        {   // v44.43: the CHORD LAB naming (Am7, Adim, Cadd9 ...) when the notes make a known chord; the short rule below otherwise
            const LabChord lc = LabChordFromNotes(s.chordNotes.data(), s.chordSize);
            if (!lc.name.empty() && lc.name.find('?') == std::string::npos && lc.name.find('(') == std::string::npos) return lc.name;
        }
        int lo = 127; for (int i = 0; i < s.chordSize; ++i) lo = std::min(lo, static_cast<int>(s.chordNotes[i]));
        auto has = [&](int iv) { for (int i = 0; i < s.chordSize; ++i) if (((s.chordNotes[i] - lo) % 12 + 12) % 12 == iv) return true; return false; };
        std::string n = kKeyNames[((lo % 12) + 12) % 12];
        if (s.chordSize == 1) return n;
        if (has(3) && !has(4)) n += "m";
        else if (!has(3) && !has(4)) n += has(5) ? "sus4" : has(2) ? "sus2" : "5";
        return n;
    }
    std::string ChordSceneLabel(int c) const {
        const ChordScene& cs = project.chordScenes[std::clamp(c, 0, kNumChordScenes - 1)];
        if (!cs.name.empty()) return cs.name;
        LabSetSpelling(cs.key >= 0 ? cs.key : project.randomizerKeyRoot, cs.scale >= 0 ? cs.scale : project.randomizerScale);
        std::string out; std::string last; int shown = 0;
        for (int i = 0; i < kMaxSteps && shown < 4; ++i) {
            const Step& s = cs.track.steps[i];
            if (!s.active || s.chordSize == 0) continue;
            std::string nm = ChordNameFromNotes(s);
            if (nm == last) continue;
            last = nm; if (!out.empty()) out += " "; out += nm; ++shown;
        }
        return out.empty() ? std::string("-") : out;
    }
    // Switch the bank on (slot 0 is seeded from the chords this scene already has, so
    // nothing is lost) or off (the pattern's own chord track plays again).
    void SetChordBank(bool on) {
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        if (on == project.chordBankOn) return;
        {   // v44.44: the step remembers the bank switch itself (before, Undo left the bank as it was)
            bool anyContent = false;
            for (int c = 0; c < kNumChordScenes; ++c) if (!ChordSceneEmpty(c)) anyContent = true;
            PushUndoBank(on && !anyContent ? 0 : project.chordSceneEdit);
        }
        if (on) {
            bool anyContent = false;
            for (int c = 0; c < kNumChordScenes; ++c) if (!ChordSceneEmpty(c)) anyContent = true;
            if (!anyContent) {
                const Track& src = CurrentPattern().tracks[kChordTrackIndex];
                ChordScene& s0 = project.chordScenes[0];
                s0.track.steps = src.steps; s0.track.length = src.length; s0.track.useOwnLength = src.useOwnLength;
                s0.key = CurrentPattern().chordsKey; s0.scale = CurrentPattern().chordsScale;
                project.chordSceneEdit = 0;
            }
            project.chordBankOn = true;
            sequencer.SetChordSceneNow(project.chordSceneEdit);
            SetStatus("CHORD BANK ON");
        } else {
            project.chordBankOn = false;
            sequencer.SetChordSceneNow(0);   // releases the bank's chord; the pattern's own chords take over
            SetStatus("CHORD BANK OFF");
        }
        RefreshChordLive(true);
    }
    // Pad click: shows the scene for editing and queues it for playback (quantized like scenes).
    void LaunchChordScene(int c) {
        c = std::clamp(c, 0, kNumChordScenes - 1);
        if (!project.chordBankOn) SetChordBank(true);
        project.chordSceneEdit = c;
        sequencer.LaunchChordScene(c);
        RefreshChordLive(true);
    }
    void EditChordSceneOnly(int c) { project.chordSceneEdit = std::clamp(c, 0, kNumChordScenes - 1); RefreshChordLive(true); }
    void CopyPatternChordsToScene(int c) {
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        c = std::clamp(c, 0, kNumChordScenes - 1);
        PushUndo();
        const Track& src = CurrentPattern().tracks[kChordTrackIndex];
        ChordScene& cs = project.chordScenes[c];
        cs.track.steps = src.steps; cs.track.length = src.length; cs.track.useOwnLength = src.useOwnLength;
        cs.key = CurrentPattern().chordsKey; cs.scale = CurrentPattern().chordsScale;
        SetStatus("CHORD SCENE " + std::to_string(c + 1) + " <- SCENE " + std::to_string(project.currentPattern + 1) + " CHORDS");
    }
    // v44.43 CHORD LAB: ADD TO SCENE. The jam (chord bank slot `slot`) is written into the scene's OWN chord track, with the key / scale
    // it was built in, so the scene keeps the chords when the bank is off or the scene is copied. One undo step; the slot keeps its copy.
    // Returns false when the slot is empty.
    bool CommitChordSlotToScene(int slot, int scene) {
        slot = std::clamp(slot, 0, kNumChordScenes - 1);
        if (scene < 0 || scene >= kNumPatterns || ChordSceneEmpty(slot)) return false;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        PushUndoForScene(scene);
        const ChordScene& cs = project.chordScenes[static_cast<size_t>(slot)];
        Track& dst = project.patterns[static_cast<size_t>(scene)].tracks[kChordTrackIndex];
        dst.steps = cs.track.steps; dst.length = cs.track.length; dst.useOwnLength = cs.track.useOwnLength;
        project.patterns[static_cast<size_t>(scene)].chordsKey = cs.key;
        project.patterns[static_cast<size_t>(scene)].chordsScale = cs.scale;
        SetStatus("CHORDS OF SLOT " + std::to_string(slot + 1) + " ADDED TO SCENE " + std::to_string(scene + 1));
        return true;
    }
    // v44.44 CHORD LAB: the first change of a jam while the bank is off. One undo step: the scene's own chords are copied into the first
    // empty slot (the draft starts from what you hear), the bank goes on and that slot is edited and sounds. When every slot is in
    // use the slot that was edited last is used as it is. Returns the slot. The caller makes its change right after, in the same step.
    int StartChordDraft() {
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        if (project.chordBankOn) return std::clamp(project.chordSceneEdit, 0, kNumChordScenes - 1);
        int slot = -1;
        for (int c = 0; c < kNumChordScenes; ++c) if (ChordSceneEmpty(c)) { slot = c; break; }
        const bool fresh = slot >= 0;
        if (!fresh) slot = std::clamp(project.chordSceneEdit, 0, kNumChordScenes - 1);
        PushUndoBank(slot);
        if (fresh) {
            const Track& src = CurrentPattern().tracks[kChordTrackIndex];
            ChordScene& cs = project.chordScenes[static_cast<size_t>(slot)];
            cs = ChordScene{};
            cs.track.steps = src.steps; cs.track.length = src.length; cs.track.useOwnLength = src.useOwnLength;
            cs.key = CurrentPattern().chordsKey; cs.scale = CurrentPattern().chordsScale;
        }
        project.chordBankOn = true;
        project.chordSceneEdit = slot;
        sequencer.SetChordSceneNow(slot);
        RefreshChordLive(true);
        SetStatus(fresh ? "CHORD DRAFT STARTED IN SLOT " + std::to_string(slot + 1) + " (THE SCENE KEEPS ITS OWN CHORDS UNTIL ADD TO SCENE)"
                        : "ALL CHORD SLOTS ARE IN USE: JAMMING IN SLOT " + std::to_string(slot + 1));
        return slot;
    }
    void ClearChordScene(int c) {
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        c = std::clamp(c, 0, kNumChordScenes - 1);
        PushUndo();
        ChordScene fresh; project.chordScenes[c] = fresh;
        SetStatus("CHORD SCENE " + std::to_string(c + 1) + " CLEARED");
    }
    void DuplicateChordScene(int from, int to) {
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        from = std::clamp(from, 0, kNumChordScenes - 1); to = std::clamp(to, 0, kNumChordScenes - 1);
        if (from == to) return;
        PushUndo();
        project.chordScenes[to] = project.chordScenes[from];
        SetStatus("CHORD SCENE " + std::to_string(from + 1) + " COPIED TO " + std::to_string(to + 1));
    }

    // ---- Single-step toggle ------------------------------------------------
    // Toggles one step on/off and seeds sensible defaults on first activation
    // (a fresh synth step needs a note or it is silent, a fresh chord step
    // needs a chord). Calls PushUndo() itself.
    void ToggleStep(int trackIndex, int stepIndex) {
        if (trackIndex < 0 || trackIndex >= kNumTracks) return;
        if (stepIndex < 0 || stepIndex >= kMaxSteps) return;
        PushUndo();
        Pattern& pat = CurrentPattern();
        Track& t = StepTrack(pat, trackIndex);
        Step& s = t.steps[stepIndex];
        s.active = !s.active;
        if (s.active && IsNoteTrackType(t.type) && s.note == 0) s.note = 60;
        if (s.active && t.type == TrackType::Chord && s.chordSize == 0) {
            s.chordSize = 3;
            s.chordNotes = {60, 64, 67, 71, 0, 0};
        }
    }

    // ---- Category clear (all undoable) ------------------------------------
    void ClearAllDrums() {
        PushUndo();
        Pattern& pat = CurrentPattern();
        for (int slot = 0; slot < kNumDrumTracks; ++slot) {
            for (auto& s : pat.tracks[slot].steps) s = Step{};
        }
    }
    // Clears just ONE synth track (0=bass, 1=synth 2, 2=synth 3) -- the
    // per-track counterpart to ClearAllSynths above. On the RANDOM tab this
    // is bound to B on the same row A randomizes that synth from, so
    // "randomize this part" and "clear this part" live on one row instead
    // of needing 3 more dedicated rows.
    void ClearSynth(int synthIndex) {
        if (synthIndex < 0 || synthIndex >= kNumSynthTracks) return;
        PushUndo();
        Pattern& pat = CurrentPattern();
        for (auto& s : pat.tracks[kNumDrumTracks + synthIndex].steps) s = Step{};
        SyncMixer(pat);
    }
    // Clears the dedicated Chord track (see Types.h's TrackType::Chord) --
    // the Phase 6 counterpart to ClearSynth above. Bound to B on the
    // PERFORM tab's RANDOM CHORD row, same "A=randomize this part,
    // B=clear this part" convention as RANDOM BASS/SYNTH 2/SYNTH 3.
    void ClearChordTrack() {
        PushUndo();
        Pattern& pat = CurrentPattern();
        for (auto& s : StepTrack(pat, kChordTrackIndex).steps) s = Step{};
        SyncMixer(pat);
    }
    // ---- First sixty seconds (v44.8) ------------------------------------------------------------------------------------
    // True when the edited scene has no note at all (the chord bank's scene counts when it is on).
    bool CurrentPatternIsEmpty() {
        Pattern& pat = CurrentPattern();
        for (int t = 0; t < kNumTracks; ++t) for (const Step& st : StepTrack(pat, t).steps) if (st.active) return false;
        return true;
    }
    // Puts the slow starter groove (core/StarterGroove.h) into the edited scene; one undo step.
    void LoadStarterGroove() {
        std::lock_guard<SceneMutex> outer(sequencer.PatternMutex());   // v44.44: the undo copy is taken under the lock too
        PushUndo();
        {
            std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
            Pattern& pat = CurrentPattern();
            ApplyStarterGroove(pat, StepTrack(pat, kChordTrackIndex));
            SyncMixer(pat);
        }
        SetStatus("STARTER GROOVE LOADED - PRESS SPACE TO PLAY");
    }
    // ---- Brick slots (v44.22, S1a) -----------------------------------------------------------------------------------------
    // Slots 0..kNumLiveScenes-1 are the LIVE scenes. Slots kNumLiveScenes.. are hidden: one per brick, filled by THROW with a copy of
    // the scene, so the brick has its own notes. A brick slot is "in use" while some brick of the chain points at it (REPEAT SAME
    // shares one slot between bricks); an unreferenced slot is free and is not saved.
    static bool IsBrickSlot(int slot) { return slot >= kNumLiveScenes && slot < kNumPatterns; }
    bool SlotInUse(int slot) const {
        if (slot < 0 || slot >= kNumPatterns) return false;
        if (slot < kNumLiveScenes) return true;
        for (int i = 0; i < project.songChainLength; ++i) if (project.songChain[static_cast<size_t>(i)].patternIndex == slot) return true;
        return false;
    }
    // First brick slot no brick points at, or -1.
    int FreeBrickSlot() const {
        for (int s = kNumLiveScenes; s < kNumPatterns; ++s) if (!SlotInUse(s)) return s;
        return -1;
    }
    // ---- The sound of the track (v44.23, S1b) ------------------------------------------------------------------------------
    // One sound for the whole track lives in the hidden slot kTrackSoundSlot; Project::trackSoundMask says which parts of it exist (bits
    // kSoundSynth0.. below). A brick thrown from LIVE gets the sound of the track for every part that is defined, so jamming with
    // another sound on LIVE does not change the track; a part the track has not defined yet is taken from the thrown scene (the
    // first brick sets the sound of the track). TAKE SOUND TO TRACK replaces it on purpose.
    Pattern& TrackSound() { return project.patterns[static_cast<size_t>(kTrackSoundSlot)]; }
    // Copies scene `src` (any slot) into a free brick slot and returns the slot (-1 = none free). The copy is a complete scene: notes,
    // length, level / FX of its tracks, mute / solo flags and its sound at this moment. With fromLive the sound follows the rule
    // above, every part (the first brick sets the sound of the track) and the copy follows the track in everything. The caller has
    // pushed the undo step. Not undoable by itself: the chain edit that makes the slot used is.
    int CopySceneToBrickSlot(int src, bool fromLive = false, uint16_t* muteMaskOut = nullptr, StepParams* paramsOut = nullptr) {
        const int dst = FreeBrickSlot();
        if (dst < 0 || src < 0 || src >= kNumPatterns) return -1;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        project.patterns[static_cast<size_t>(dst)] = project.patterns[static_cast<size_t>(src)];
        // v44.38: a brick has one mute (its mask): what the scene had muted / soloed becomes WHO PLAYS, the scene flags are cleared
        if (muteMaskOut) *muteMaskOut = SceneMuteMask(project.patterns[static_cast<size_t>(dst)]);
        ClearSceneMuteFlags(project.patterns[static_cast<size_t>(dst)]);
        // v44.39: the parameter rows of a LIVE scene go with it into the brick, where they live in the brick's own settings
        if (paramsOut) *paramsOut = project.patterns[static_cast<size_t>(dst)].params;
        project.patterns[static_cast<size_t>(dst)].params = StepParams{};
        dropActiveScene[static_cast<size_t>(dst)] = false;
        if (fromLive) {
            // v44.27: a thrown brick sounds exactly like the scene you were hearing. The first brick sets the sound of the track; later
            // ones follow the track in every part where the scene sounds the same, and keep the scene's own sound (a dot on the
            // instrument) in the parts where it differs - before, those parts were replaced by the track's and the brick sounded
            // different from the same scene on LIVE.
            Pattern& d = project.patterns[static_cast<size_t>(dst)];
            d.ownSound = 0;
            if (project.trackSoundMask != 0) {
                const Pattern& hold = TrackSound();
                for (int part = 0; part < kNumParts; ++part)
                    if (SoundPartText(d, part) != SoundPartText(hold, part)) d.ownSound |= static_cast<uint16_t>(1u << part);
            } else { CopyAllParts(TrackSound(), project.patterns[static_cast<size_t>(src)]); project.trackSoundMask = kSoundAll; }
        }
        SyncSceneKits();
        return dst;
    }
    // The one-click gesture: a COPY of the edited scene becomes the next brick of the chain, and the chain row is shown.
    bool ThrowIntoChain() {
        if (project.songChainLength >= kMaxChainSteps) { SetStatus("TRACK FULL (MAX " + std::to_string(kMaxChainSteps) + ")"); return false; }
        if (FreeBrickSlot() < 0) { SetStatus("NO FREE BRICK SLOT"); return false; }   // cannot happen below kMaxChainSteps
        const int liveShown = AppState::IsBrickSlot(project.currentPattern) ? project.liveScene : project.currentPattern;
        PushSoundUndo(true);
        uint16_t mm = 0; StepParams spr;
        const int slot = CopySceneToBrickSlot(liveShown, true, &mm, &spr);
        project.songChain[static_cast<size_t>(project.songChainLength)] = ChainStep{slot, 1, project.chordBankOn ? sequencer.ChordSceneCur() : -1};   // v44.43: with the bank on the brick plays the chord scene you were hearing
        project.songChain[static_cast<size_t>(project.songChainLength)].muteMask = mm;
        project.songChain[static_cast<size_t>(project.songChainLength)].params = spr;
        project.songChainLength++;
        SetStatus("SCENE " + std::to_string(liveShown + 1) + " THROWN ONTO THE TRACK (BRICK " + std::to_string(project.songChainLength) + ")");
        return true;
    }
    // The distinct brick slots of the chain, optionally without `except`.
    std::vector<int> ChainSlots(int except = -1) const {
        std::vector<int> slots;
        for (int i = 0; i < project.songChainLength; ++i) {
            const int sl = project.songChain[static_cast<size_t>(i)].patternIndex;
            if (sl != except && sl >= 0 && sl < kNumPatterns && std::find(slots.begin(), slots.end(), sl) == slots.end()) slots.push_back(sl);
        }
        return slots;
    }
    // A slot's sound changed under the engine: the played scene hears it at once.
    void TouchSlotSound(int sl) {
        if (sl == sequencer.PlayPatternIndex()) { sequencer.ApplySceneSound(sl); engine.GetMixer().SyncFromPattern(project.patterns[static_cast<size_t>(sl)]); }
        kitsDirty_ = true;
    }
    // TAKE SOUND TO TRACK: the sound of `scene` (every instrument and the kit) becomes the sound of the whole track and every brick gets
    // it, own sounds included (the author asked for it). One undo step brings back the track's sound, the bricks' sounds and their marks.
    // Notes and brick settings are not touched. Returns how many bricks were changed, -1 on a bad scene.
    int TakeSceneSoundToTrack(int scene) {
        if (scene < 0 || scene >= kNumPatterns) return -1;
        const std::vector<int> slots = ChainSlots(scene);
        PushSoundUndo(true, slots);
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        const Pattern src = project.patterns[static_cast<size_t>(scene)];
        CopyAllParts(TrackSound(), src);
        project.trackSoundMask = kSoundAll;
        for (int sl : slots) { Pattern& bp = project.patterns[static_cast<size_t>(sl)]; CopyAllParts(bp, src); bp.ownSound = 0; TouchSlotSound(sl); }
        SyncSceneKits();
        SetStatus("SOUND OF SCENE " + std::to_string(scene + 1) + " IS NOW THE SOUND OF THE TRACK (" + std::to_string(slots.size()) + " BRICK" + (slots.size() == 1 ? "" : "S") + ")");
        return static_cast<int>(slots.size());
    }
    // UPDATE FROM LIVE: the notes (and timing, groove) of brick idx are replaced by the current version of the LIVE scene. The brick keeps
    // its sound (all parts and the own-sound marks), its role and fill and its mute / solo flags. Bricks that share the slot change together.
    bool UpdateBrickFromLive(int idx) {
        if (idx < 0 || idx >= project.songChainLength) return false;
        const int slot = project.songChain[static_cast<size_t>(idx)].patternIndex;
        const int live = AppState::IsBrickSlot(project.currentPattern) ? project.liveScene : project.currentPattern;
        if (!IsBrickSlot(slot) || live < 0 || live >= kNumLiveScenes) return false;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        PushUndoForScene(slot);
        Pattern& d = project.patterns[static_cast<size_t>(slot)];
        const Pattern old = d;
        d = project.patterns[static_cast<size_t>(live)];
        CopyAllParts(d, old);
        d.ownSound = old.ownSound;
        d.role = old.role; d.fill = old.fill;
        if (project.patterns[static_cast<size_t>(live)].params.n > 0) project.songChain[static_cast<size_t>(idx)].params = project.patterns[static_cast<size_t>(live)].params;   // v44.39
        d.params = StepParams{};
        for (int t = 0; t < kNumTracks; ++t) { d.tracks[static_cast<size_t>(t)].mute = old.tracks[static_cast<size_t>(t)].mute; d.tracks[static_cast<size_t>(t)].solo = old.tracks[static_cast<size_t>(t)].solo; }
        TouchSlotSound(slot);
        sequencer.RefreshArrangement();
        SetStatus("BRICK " + std::to_string(idx + 1) + " UPDATED FROM LIVE SCENE " + std::to_string(live + 1) + " (SOUND KEPT)");
        return true;
    }

    // ---- Sound of an instrument on TRACK (v44.24, S2) -------------------------------------------------------------------
    // A brick follows the sound of the track in every part unless the part is marked OWN in its slot (Pattern::ownSound). An edit of a
    // part that follows the track is an edit of the TRACK: SyncBrickSound (run every few frames on the TRACK tab) sees that the edited
    // brick's part differs from the track's and gives it to the track and to every other brick that follows. Undo needs nothing special:
    // it puts the edited brick back, and the next sync puts the track and the others back with it.
    bool PartIsOwn(int slot, int part) const { return slot >= 0 && slot < kNumPatterns && part >= 0 && part < kNumParts && ((project.patterns[static_cast<size_t>(slot)].ownSound >> part) & 1u) != 0; }
    // Which parts of a slot differ from the sound of the track: those that follow it are edits to hand to the track.
    int SyncBrickSound() {
        const int slot = project.currentPattern;
        if (!IsBrickSlot(slot) || !SlotInUse(slot) || project.trackSoundMask == 0) return 0;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        Pattern& cur = project.patterns[static_cast<size_t>(slot)];
        Pattern& hold = TrackSound();
        int moved = 0;
        std::vector<int> others;
        for (int part = 0; part < kNumParts; ++part) {
            if ((cur.ownSound >> part) & 1u) continue;
            if (SoundPartText(cur, part) == SoundPartText(hold, part)) continue;
            if (others.empty() && moved == 0) others = ChainSlots(slot);
            CopyPart(hold, cur, part);
            for (int sl : others) if (!PartIsOwn(sl, part)) CopyPart(project.patterns[static_cast<size_t>(sl)], cur, part);
            ++moved;
        }
        if (moved) for (int sl : others) TouchSlotSound(sl);
        return moved;
    }
    // THIS BRICK ONLY: the part becomes the brick's own (it sounds as the track's until it is edited). One undo step.
    bool MakeBrickSoundOwn(int slot, int part) {
        if (!IsBrickSlot(slot) || part < 0 || part >= kNumParts || PartIsOwn(slot, part)) return false;
        PushUndoForScene(slot);
        project.patterns[static_cast<size_t>(slot)].ownSound |= static_cast<uint16_t>(1u << part);
        return true;
    }
    // WHOLE TRACK / Follow track: the brick gives up its own sound for the part and takes the track's. One undo step.
    bool FollowTrackSound(int slot, int part) {
        if (!IsBrickSlot(slot) || part < 0 || part >= kNumParts || !PartIsOwn(slot, part)) return false;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        PushUndoForScene(slot);
        Pattern& bp = project.patterns[static_cast<size_t>(slot)];
        CopyPart(bp, TrackSound(), part);
        bp.ownSound &= static_cast<uint16_t>(~(1u << part));
        TouchSlotSound(slot);
        return true;
    }
    // Apply to all bricks: the sound of this brick in the part becomes the sound of the track and of every brick, own sounds included.
    // One undo step (restores the track, the bricks and their marks). Returns how many other bricks changed.
    int ApplySoundToAllBricks(int slot, int part) {
        if (!IsBrickSlot(slot) || part < 0 || part >= kNumParts) return -1;
        const std::vector<int> others = ChainSlots(slot);
        std::vector<int> all = others; all.push_back(slot);
        PushSoundUndo(true, all);
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        const Pattern src = project.patterns[static_cast<size_t>(slot)];
        CopyPart(TrackSound(), src, part);
        project.trackSoundMask = kSoundAll;
        for (int sl : others) { Pattern& bp = project.patterns[static_cast<size_t>(sl)]; CopyPart(bp, src, part); bp.ownSound &= static_cast<uint16_t>(~(1u << part)); TouchSlotSound(sl); }
        project.patterns[static_cast<size_t>(slot)].ownSound &= static_cast<uint16_t>(~(1u << part));
        return static_cast<int>(others.size());
    }

    // ---- Bricks (v44.9): the chain as one strip of bricks ---------------------------------------------------------------
    // Which tab the shell should select next frame (0 = none, 1 = TRACK, 2 = LIVE / the scene editor). Set by the UI, cleared by main.
    int trackEditMode = 0;  // v44.24: the bottom of TRACK: 0 = NOTES, 1 = SOUND, 2 = MIX
    int activeTab = 2;      // v44.22: the tab that is shown (1 = TRACK, 2 = LIVE); main sets it every frame. TRACK edits the notes of a brick, LIVE those of a live scene
    int tabBeforeLab = 2;   // v44.41: the tab MIX LAB (3) was opened from, so LIVE / TRACK treat the way back like a normal change
    int uiTabRequest = 2;   // v44.12: the program opens on LIVE (jam first; THROW ONTO THE TRACK moves you to TRACK)
    // Moves brick `from` so it ends up at index `to` (the others shift); one undo step.
    bool MoveChainStep(int from, int to) {
        const int n = project.songChainLength;
        if (from < 0 || from >= n) return false;
        to = std::clamp(to, 0, n - 1);
        if (from == to) return false;
        PushSoundUndo();
        ChainStep tmp = project.songChain[static_cast<size_t>(from)];
        if (from < to) { for (int i = from; i < to; ++i) project.songChain[static_cast<size_t>(i)] = project.songChain[static_cast<size_t>(i + 1)]; }
        else           { for (int i = from; i > to; --i) project.songChain[static_cast<size_t>(i)] = project.songChain[static_cast<size_t>(i - 1)]; }
        project.songChain[static_cast<size_t>(to)] = tmp;
        return true;
    }
    bool RemoveChainStep(int idx) {
        if (idx < 0 || idx >= project.songChainLength) return false;
        PushSoundUndo();
        for (int i = idx; i < project.songChainLength - 1; ++i) project.songChain[static_cast<size_t>(i)] = project.songChain[static_cast<size_t>(i + 1)];
        project.songChainLength--;
        return true;
    }
    // A copy of brick idx right after it (with all its settings).
    bool DuplicateChainStep(int idx) {
        if (idx < 0 || idx >= project.songChainLength || project.songChainLength >= kMaxChainSteps) return false;
        PushSoundUndo();
        for (int i = project.songChainLength; i > idx + 1; --i) project.songChain[static_cast<size_t>(i)] = project.songChain[static_cast<size_t>(i - 1)];
        project.songChain[static_cast<size_t>(idx + 1)] = project.songChain[static_cast<size_t>(idx)];
        project.songChainLength++;
        return true;
    }
    // Appends a COPY of scene `pattern` as a new brick (x1).
    bool AddChainStep(int pattern) {
        if (project.songChainLength >= kMaxChainSteps) { SetStatus("TRACK FULL (MAX " + std::to_string(kMaxChainSteps) + ")"); return false; }
        if (FreeBrickSlot() < 0) { SetStatus("NO FREE BRICK SLOT"); return false; }
        PushSoundUndo(true);
        uint16_t mm = 0; StepParams spr;
        const int slot = CopySceneToBrickSlot(std::clamp(pattern, 0, kNumPatterns - 1), true, &mm, &spr);
        project.songChain[static_cast<size_t>(project.songChainLength)] = ChainStep{slot, 1};
        project.songChain[static_cast<size_t>(project.songChainLength)].muteMask = mm;
        project.songChain[static_cast<size_t>(project.songChainLength)].params = spr;
        project.songChainLength++;
        return true;
    }
    // ---- Player (v44.10, one player v44.28) ----------------------------------------------------------------------------------
    // One transport for the whole program, like Logic / Live: PLAY (Space) starts at the selected brick. LOOP on keeps the selected
    // brick (or the selected range of bricks) going round; LOOP off plays the bricks in order and stops at the end of the track.
    // Home goes to the first brick (and restarts there when it is playing). On LIVE, PLAY plays the scene you are on.
    bool loopOn = true;        // LOOP: round the selected brick / range
    bool loopAll = false;      // v44.35: with LOOP on, round the WHOLE track (transitions between all bricks), whatever brick or range is selected / edited
    int selBrick = -1;         // the selected brick of the TRACK strip (the editor shows its scene); the first of a range
    int rollCopyFrame = -10, rollPasteFrame = -10;   // C1 (UI-8): frames in which Cmd+C / Cmd+V belong to the piano roll (notes selected / copied, mouse over it)
    int rollKeysFrame = -10;   // U6: the last ImGui frame in which the piano roll owned the arrow keys (selected notes + mouse over the roll)
    int selBrickHi = -1;       // the last brick of a selected range (U6), -1 = just selBrick
    void LoopRange(int* lo, int* hi) const {
        const int n = std::max(0, project.songChainLength);
        int a = std::clamp(selBrick, 0, std::max(0, n - 1)), b = selBrickHi >= 0 ? std::clamp(selBrickHi, 0, std::max(0, n - 1)) : a;
        if (b < a) std::swap(a, b);
        *lo = a; *hi = b;
    }
    void ApplyTransportFlags() {
        if (loopOn && loopAll) {   // v44.35: the whole track goes round; the sequencer clamps the high end to the last brick, so it follows added / removed bricks
            sequencer.SetSongLoopStep(false);
            sequencer.SetSongLoopRange(0, kMaxChainSteps - 1);
            sequencer.SetSongRunOnce(false);
            return;
        }
        int lo = 0, hi = 0; LoopRange(&lo, &hi);
        const bool range = loopOn && hi > lo;
        sequencer.SetSongLoopStep(loopOn && !range);
        sequencer.SetSongLoopRange(range ? lo : -1, range ? hi : -1);
        sequencer.SetSongRunOnce(!loopOn);
    }
    // The brick the editor is on: the selected one while it still holds the edited scene, else the first brick with that scene.
    int BrickForTransport() const {
        const int n = std::clamp(project.songChainLength, 0, kMaxChainSteps);
        if (selBrick >= 0 && selBrick < n && project.songChain[static_cast<size_t>(selBrick)].patternIndex == project.currentPattern) return selBrick;
        for (int i = 0; i < n; ++i) if (project.songChain[static_cast<size_t>(i)].patternIndex == project.currentPattern) return i;
        return -1;
    }
    void SetLoop(bool on) { loopOn = on; ApplyTransportFlags(); }
    // v44.35: LOOP round the whole track. Switching it while playing does not jump: the brick that plays finishes, then the track goes on / wraps.
    void SetLoopAll(bool on) { loopAll = on; ApplyTransportFlags(); }
    // Starts (or restarts, also in the middle of playing) from brick i.
    void PlayFromBrick(int i) {
        if (project.songChainLength <= 0) { sequencer.Play(); return; }
        i = std::clamp(i, 0, project.songChainLength - 1);
        int lo = 0, hi = 0; LoopRange(&lo, &hi);
        selBrick = i;
        if (selBrickHi >= 0 && (i < lo || i > hi)) selBrickHi = -1;   // another brick than the selected range: the range is gone
        else if (selBrickHi >= 0) { selBrick = lo; selBrickHi = hi; }
        ApplyTransportFlags();
        sequencer.PlayChainFrom(i, loopOn && selBrickHi < 0 && !loopAll);
        if (selBrickHi >= 0) selBrick = lo;
    }
    // Home: the first brick. Stopped: it is only selected (PLAY then starts there); playing: playback restarts there.
    void TrackToStart() {
        if (project.songChainLength <= 0) { SetStatus("NO BRICKS YET"); return; }
        selBrick = 0; selBrickHi = -1;
        ApplyTransportFlags();
        if (sequencer.IsPlaying() && sequencer.IsSongModeActive()) sequencer.PlayChainFrom(0, loopOn && !loopAll);
    }
    // U6 (v44.30): a selected range of bricks. LOOP goes round the whole range; Delete / Duplicate act on all of it.
    bool HasBrickRange() const { return selBrickHi >= 0 && selBrickHi != selBrick; }
    void SelectBrickRange(int a, int b) {
        const int n = project.songChainLength;
        if (n <= 0) return;
        a = std::clamp(a, 0, n - 1); b = std::clamp(b, 0, n - 1);
        if (b < a) std::swap(a, b);
        selBrick = a; selBrickHi = b > a ? b : -1;
        ApplyTransportFlags();
        SetStatus(b > a ? "BRICKS " + std::to_string(a + 1) + "-" + std::to_string(b + 1) + " SELECTED" + (loopOn ? " - LOOP GOES ROUND THEM" : "")
                        : "BRICK " + std::to_string(a + 1));
    }
    void SelectAllBricks() { SelectBrickRange(0, project.songChainLength - 1); }
    void ClearBrickRange() { if (selBrickHi >= 0) { selBrickHi = -1; ApplyTransportFlags(); } }
    // One undo step for the whole range.
    bool RemoveBrickRange(int lo, int hi) {
        const int n = project.songChainLength;
        if (n <= 0) return false;
        lo = std::clamp(lo, 0, n - 1); hi = std::clamp(hi, lo, n - 1);
        lastSoundUndoPush_ = -1e9; PushSoundUndo();
        const int cnt = hi - lo + 1;
        for (int i = lo; i + cnt < n; ++i) project.songChain[static_cast<size_t>(i)] = project.songChain[static_cast<size_t>(i + cnt)];
        project.songChainLength = n - cnt;
        selBrickHi = -1;
        selBrick = std::min(lo, project.songChainLength - 1);
        ApplyTransportFlags();
        sequencer.RefreshArrangement();
        SetStatus(std::to_string(cnt) + (cnt == 1 ? " BRICK" : " BRICKS") + " REMOVED");
        return true;
    }
    // The range again right after itself (same scenes, all settings); the copy becomes the selection. One undo step.
    bool DuplicateBrickRange(int lo, int hi) {
        const int n = project.songChainLength;
        if (n <= 0) return false;
        lo = std::clamp(lo, 0, n - 1); hi = std::clamp(hi, lo, n - 1);
        const int cnt = hi - lo + 1;
        if (n + cnt > kMaxChainSteps) { SetStatus("TRACK FULL (MAX " + std::to_string(kMaxChainSteps) + ")"); return false; }
        lastSoundUndoPush_ = -1e9; PushSoundUndo();
        for (int i = n - 1; i > hi; --i) project.songChain[static_cast<size_t>(i + cnt)] = project.songChain[static_cast<size_t>(i)];
        for (int k = 0; k < cnt; ++k) project.songChain[static_cast<size_t>(hi + 1 + k)] = project.songChain[static_cast<size_t>(lo + k)];
        project.songChainLength = n + cnt;
        selBrick = hi + 1; selBrickHi = cnt > 1 ? hi + cnt : -1;
        ApplyTransportFlags();
        sequencer.RefreshArrangement();
        SetStatus(std::to_string(cnt) + (cnt == 1 ? " BRICK" : " BRICKS") + " DUPLICATED");
        return true;
    }
    // Space / the PLAY button.
    void TransportToggle() {
        if (sequencer.IsPlaying()) { sequencer.Stop(); TickTransport(); return; }
        if (activeTab == 2 || project.songChainLength <= 0) {   // v44.27: LIVE (or no bricks at all): the player plays the scene you are on
            if (sequencer.IsSongModeActive()) sequencer.SetSongModeActive(false);
            sequencer.Play();
            return;
        }
        const int b = BrickForTransport();
        int lo = 0, hi = 0; LoopRange(&lo, &hi);
        int start = b >= 0 ? b : (selBrick >= 0 ? lo : 0);
        if (selBrickHi >= 0 && loopOn && (start < lo || start > hi)) start = lo;
        PlayFromBrick(start);
    }
    // v44.27: playback leaves the chain and continues with the scene shown on LIVE (the beat keeps running, so the groove does not jump).
    // v44.51: no longer done by switching to the LIVE tab; only PlayHere() (the tab bar button) calls it.
    void LeaveTrackForLive() {
        if (!sequencer.IsSongModeActive()) return;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        sequencer.SetSongModeActive(false);
    }
    // v44.51: what plays right now, whatever tab is shown: the jam (a LIVE scene) or the track.
    enum class PlaySource { None, Live, Track };
    PlaySource PlayingSource() const { return !sequencer.IsPlaying() ? PlaySource::None : (sequencer.IsSongModeActive() ? PlaySource::Track : PlaySource::Live); }
    int ShownTab() const { return activeTab == 3 ? tabBeforeLab : activeTab; }   // MIX LAB shows no music of its own
    // True when the tab on screen is not the one that plays (then the tab bar offers PLAY HERE).
    bool PlayingElsewhere() const {
        const PlaySource ps = PlayingSource();
        if (ps == PlaySource::None) return false;
        const int tab = ShownTab();
        if (tab == 2) return ps == PlaySource::Track;
        if (tab == 1) return ps == PlaySource::Live && project.songChainLength > 0;
        return false;
    }
    // PLAY HERE: playback moves to what the shown tab holds (LIVE: the scene on screen; TRACK: the brick on screen).
    void PlayHere() {
        if (!sequencer.IsPlaying()) return;
        if (ShownTab() == 2) LeaveTrackForLive();
        else if (project.songChainLength > 0) { const int b = BrickForTransport(); PlayFromBrick(b >= 0 ? b : std::clamp(selBrick, 0, project.songChainLength - 1)); }
    }
    // Every frame: when playback has stopped (also by itself at the end of the track) song mode is released, so the editor and the
    // audio mixer are back on the same scene.
    void TickTransport() {
        if (!sequencer.IsPlaying() && sequencer.IsSongModeActive()) sequencer.SetSongModeActive(false);
        if (!sequencer.IsPlaying() && !queuedSwitches_.empty()) {   // v44.54: stopped with switches waiting for a bar line: they take effect now
            std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
            if (!queuedSwitches_.empty()) {
                const int sc = project.currentPattern;
                for (auto& q : queuedSwitches_) if (q.scene != sc) q.scene = -1;   // another scene is edited: its flags are not touched by the queue
                bool& applying = SwitchApplying(); applying = true;
                for (const auto& q : queuedSwitches_) if (q.scene == sc) DispatchSwitch(q.kind, q.arg);
                applying = false;
                queuedSwitches_.clear();
                sequencer.ArmBarHook(false);
            }
        }
    }
    // v44.29: everything a brick adds on top of its scene back to nothing: mutes, fill, transition, trim, dynamics and levels. One undo step.
    void ResetBrickSettings(int idx) {
        if (idx < 0 || idx >= project.songChainLength) return;
        lastSoundUndoPush_ = -1e9;   // a discrete action: never merged into the previous edit's undo step
        PushSoundUndo();
        ChainStep& cs = project.songChain[static_cast<size_t>(idx)];
        cs.muteMask = 0; cs.fillMode = FillMode::Scene; cs.fill = FillRule{}; cs.riser = -1; cs.riserBars = 2; cs.trimSteps = 0;
        cs.automation = StepAuto{}; cs.levels = StepLevels{}; cs.params = StepParams{};
        sequencer.RefreshArrangement();
        SetStatus("BRICK " + std::to_string(idx + 1) + " RESET TO ITS SCENE");
    }
    // A copy of brick idx's notes in a free brick slot, as a new brick right after it (so the copy can be changed without touching
    // the original). Returns the index of the new brick, or -1 (status says why). One undo step (the chain; the slot is free again).
    int DuplicateBrickAsScene(int idx) {
        if (idx < 0 || idx >= project.songChainLength) return -1;
        if (project.songChainLength >= kMaxChainSteps) { SetStatus("TRACK FULL (MAX " + std::to_string(kMaxChainSteps) + ")"); return -1; }
        const int src = std::clamp(project.songChain[static_cast<size_t>(idx)].patternIndex, 0, kNumPatterns - 1);
        if (FreeBrickSlot() < 0) { SetStatus("NO FREE BRICK SLOT"); return -1; }
        PushSoundUndo();
        const int dst = CopySceneToBrickSlot(src);
        for (int i = project.songChainLength; i > idx + 1; --i) project.songChain[static_cast<size_t>(i)] = project.songChain[static_cast<size_t>(i - 1)];
        ChainStep copy = project.songChain[static_cast<size_t>(idx)];
        copy.patternIndex = dst;
        project.songChain[static_cast<size_t>(idx + 1)] = copy;
        project.songChainLength++;
        SetStatus("BRICK " + std::to_string(idx + 1) + " COPIED WITH ITS OWN NOTES - A NEW BRICK");
        return idx + 1;
    }
    // How many times the scene of brick idx is played (1..99). A trimmed brick becomes a full-length one. Returns true when it changed.
    bool SetBrickRepeats(int idx, int repeats) {
        if (idx < 0 || idx >= project.songChainLength) return false;
        ChainStep& cs = project.songChain[static_cast<size_t>(idx)];
        repeats = std::clamp(repeats, 1, 99);
        if (cs.repeats == repeats && cs.trimSteps == 0) return false;
        PushSoundUndo();
        cs.repeats = repeats;
        cs.trimSteps = 0;
        return true;
    }
    // Sets the length of brick idx to about `bars` bars: at least one pass of the scene -> a whole number of passes (repeats);
    // shorter than the scene -> one trimmed pass (whole bars, or single steps with fine = true). Returns true when it changed.
    // No undo here (the drag takes one snapshot at its start).
    bool SetBrickBars(int idx, double bars, bool fine) {
        if (idx < 0 || idx >= project.songChainLength) return false;
        ChainStep& cs = project.songChain[static_cast<size_t>(idx)];
        const Pattern& pat = project.patterns[static_cast<size_t>(std::clamp(cs.patternIndex, 0, kNumPatterns - 1))];
        const double sceneBars = PatternBars(pat);
        const int oldRep = cs.repeats; const uint8_t oldTrim = cs.trimSteps;
        const int barSteps = 4 * ClampGrid(pat.stepsPerBeat);
        if (bars >= sceneBars - 1e-6) {
            cs.repeats = std::clamp(static_cast<int>(std::lround(bars / sceneBars)), 1, 99);
            cs.trimSteps = 0;
        } else {
            int steps = fine ? static_cast<int>(std::lround(bars * barSteps)) : static_cast<int>(std::lround(bars)) * barSteps;
            steps = std::clamp(steps, fine ? 1 : barSteps, std::max(1, pat.masterLength - 1));
            cs.repeats = 1;
            cs.trimSteps = static_cast<uint8_t>(std::min(steps, 255));
        }
        const bool changed = cs.repeats != oldRep || cs.trimSteps != oldTrim;
        if (changed) sequencer.RefreshArrangement();
        return changed;
    }
    // Sets the start / end level (dB, -48 = silence) of a brick's MIX lane (free points in between stay).
    void SetBrickLevel(int idx, bool endPoint, int db) {
        if (idx < 0 || idx >= project.songChainLength) return;
        Lane& l = project.songChain[static_cast<size_t>(idx)].automation.mix;
        if (endPoint) l.SetEnd(db, kAutoMinDb, kAutoMaxDb); else l.SetStart(db, kAutoMinDb, kAutoMaxDb);
    }
    // v44.13: the level of ONE instrument over a brick, start -> end (dB, -48 = silence, 0 = untouched). No undo (callers take it).
    void SetBrickTrackLevel(int idx, int track, bool endPoint, int db) {
        if (idx < 0 || idx >= project.songChainLength || track < 0 || track >= kNumTracks) return;
        Lane& l = project.songChain[static_cast<size_t>(idx)].levels.track[track];
        if (endPoint) l.SetEnd(db, kAutoMinDb, kAutoMaxDb); else l.SetStart(db, kAutoMinDb, kAutoMaxDb);
    }
    void SetBrickTrackLevels(int idx, int track, int startDb, int endDb) { SetBrickTrackLevel(idx, track, false, startDb); SetBrickTrackLevel(idx, track, true, endDb); }
    // v44.21: any lane of a brick by id (0..9 instrument, kLaneMix, kLaneFilter); nullptr for a bad brick or id.
    Lane* BrickLane(int idx, int laneId) {
        if (idx < 0 || idx >= project.songChainLength || laneId < 0 || laneId >= kNumLaneIdsAll) return nullptr;
        ChainStep& cs = project.songChain[static_cast<size_t>(idx)];
        if (laneId >= kLaneParam0) return laneId - kLaneParam0 < cs.params.n ? &cs.params.row[laneId - kLaneParam0].lane : nullptr;
        return laneId < kArrTracks ? &cs.levels.track[laneId] : laneId == kLaneMix ? &cs.automation.mix : &cs.automation.filter;
    }
    // v44.38: parameter rows. Adds a row (flat) for one parameter of one instrument on brick idx and returns its lane id (kLaneParam0 + row),
    // or -1 (status says why). One undo step.
    int AddBrickParamRow(int idx, int track, int param) {
        if (idx < 0 || idx >= project.songChainLength) return -1;
        StepParams& sp = project.songChain[static_cast<size_t>(idx)].params;
        if (sp.Find(track, param) < 0 && sp.n >= kMaxParamRows) { SetStatus("A BRICK HAS ROOM FOR " + std::to_string(kMaxParamRows) + " PARAMETER ROWS - REMOVE ONE FIRST"); return -1; }
        lastSoundUndoPush_ = -1e9;
        PushSoundUndo();
        const int k = sp.Add(track, param);
        if (k < 0) return -1;
        sequencer.RefreshArrangement();
        return kLaneParam0 + k;
    }
    void RemoveBrickParamRow(int idx, int row) {
        if (idx < 0 || idx >= project.songChainLength) return;
        lastSoundUndoPush_ = -1e9;
        PushSoundUndo();
        project.songChain[static_cast<size_t>(idx)].params.Remove(row);
        sequencer.RefreshArrangement();
    }
    // Replaces a whole lane (a shape from the menu, a straight line); no undo (callers take it).
    void SetBrickLane(int idx, int laneId, const Lane& shape) {
        if (Lane* l = BrickLane(idx, laneId)) { int lo, hi; LaneRange(laneId, &lo, &hi); *l = shape; l->Normalize(lo, hi); }
    }
    // The level of the transition that leads into brick idx and of the hits of its fill (dB).
    void SetBrickTransitionLevel(int idx, int db) { if (idx >= 0 && idx < project.songChainLength) project.songChain[static_cast<size_t>(idx)].levels.transitionDb = ClampLevelDb(db); }
    void SetBrickFillLevel(int idx, int db) { if (idx >= 0 && idx < project.songChainLength) project.songChain[static_cast<size_t>(idx)].levels.fillDb = ClampLevelDb(db); }
    // v44.38: the brick that holds scene slot `slot` (the edited one when it is the edited scene), or -1.
    int BrickOfSlot(int slot) const {
        if (!IsBrickSlot(slot)) return -1;
        if (slot == project.currentPattern) { const int b = BrickForTransport(); if (b >= 0) return b; }
        for (int i = 0; i < project.songChainLength; ++i) if (project.songChain[static_cast<size_t>(i)].patternIndex == slot) return i;
        return -1;
    }
    // Solo on a brick is a way of writing the mask: every other instrument off. Pressing it again brings back what was there before.
    bool BrickTrackSolo(int idx, int track) const {
        if (idx < 0 || idx >= project.songChainLength || track < 0 || track >= kNumTracks) return false;
        return project.songChain[static_cast<size_t>(idx)].muteMask == static_cast<uint16_t>(kMuteAllTracks & ~(1u << track));
    }
    bool BrickTrackMuted(int idx, int track) const {
        if (idx < 0 || idx >= project.songChainLength || track < 0 || track >= kNumTracks) return false;
        return ((project.songChain[static_cast<size_t>(idx)].muteMask >> track) & 1u) != 0;
    }
    void SetBrickTrackSolo(int idx, int track, bool solo) {
        if (idx < 0 || idx >= project.songChainLength || track < 0 || track >= kNumTracks) return;
        ChainStep& cs = project.songChain[static_cast<size_t>(idx)];
        const bool inSolo = (cs.muteMask & kMuteAllTracks) != 0 && __builtin_popcount(cs.muteMask & kMuteAllTracks) == kNumTracks - 1;
        if (solo) {
            if (!inSolo) { soloRestoreMask_ = cs.muteMask; soloRestoreBrick_ = idx; }
            cs.muteMask = static_cast<uint16_t>(kMuteAllTracks & ~(1u << track));
        } else if (BrickTrackSolo(idx, track)) {
            cs.muteMask = soloRestoreBrick_ == idx ? soloRestoreMask_ : static_cast<uint16_t>(0);
            soloRestoreBrick_ = -1;
        }
        sequencer.RefreshArrangement();
    }
    uint16_t soloRestoreMask_ = 0; int soloRestoreBrick_ = -1;
    // Switches the mute of one instrument on one brick (the cell in the "who plays" table).
    void SetBrickTrackMuted(int idx, int track, bool muted) {
        if (idx < 0 || idx >= project.songChainLength || track < 0 || track >= kNumTracks) return;
        ChainStep& cs = project.songChain[static_cast<size_t>(idx)];
        cs.muteMask = static_cast<uint16_t>(muted ? (cs.muteMask | (1u << track)) : (cs.muteMask & ~(1u << track)));
        sequencer.RefreshArrangement();
    }

    // v44.8: the instrument settings block below the grid starts folded for a newcomer (a "start here" card takes its place);
    // once opened it stays open (remembered in ui_settings.cfg).
    bool detailOpen = true;

    void ClearPattern() {
        PushUndo();
        Pattern& pat = CurrentPattern();
        for (auto& t : pat.tracks) {
            for (auto& s : t.steps) s = Step{};
        }
        if (project.chordBankOn) for (auto& s : StepTrack(pat, kChordTrackIndex).steps) s = Step{};
    }

    // ---- Randomizer -----------------------------------------------------
    // RANDOM tab: RANDOM DRUMS / RANDOM BASS / RANDOM SYNTH 2 / RANDOM
    // SYNTH 3. Separate actions (not one "randomize everything") so a live
    // performer can refresh just the rhythm section, or just one melodic
    // part, without disturbing the rest. All overwrite the current pattern
    // bank's steps for their track(s) and are undoable like any other
    // pattern edit.
    static std::mt19937& RngEngine() {
        static std::mt19937 rng{std::random_device{}()};
        return rng;
    }

    // Randomizes all 6 drum tracks in the current pattern. Each slot has a
    // hand-tuned density profile (strong beat vs. weak/off-beat hit chance)
    // so the result reads as "a plausible drum pattern" rather than pure
    // noise -- kick/snare favor the beat, hats fill in, perc is sparse.
    void RandomizeDrums() {
        PushUndo();
        std::mt19937& rng = RngEngine();
        std::uniform_real_distribution<float> chance(0.0f, 1.0f);
        Pattern& pat = CurrentPattern();
        int len = pat.masterLength;

        struct SlotProfile { int strongMod; int strongRemainder; float strongP; float weakP; float accentP; };
        static const SlotProfile profiles[kNumDrumTracks] = {
            /*Kick*/      {4, 0, 0.55f, 0.10f, 0.30f},
            /*Snare*/     {8, 4, 0.60f, 0.08f, 0.30f},
            /*Clap*/      {8, 4, 0.35f, 0.05f, 0.25f},
            /*ClosedHat*/ {2, 0, 0.55f, 0.20f, 0.15f},
            /*OpenHat*/   {4, 2, 0.20f, 0.08f, 0.15f},
            /*Perc*/      {4, 3, 0.18f, 0.10f, 0.20f},
        };

        for (int slot = 0; slot < kNumDrumTracks; ++slot) {
            Track& t = pat.tracks[slot];
            for (auto& s : t.steps) s = Step{};
            const SlotProfile& prof = profiles[slot];
            for (int i = 0; i < len; ++i) {
                bool strong = (i % prof.strongMod) == prof.strongRemainder;
                float p = strong ? prof.strongP : prof.weakP;
                if (chance(rng) < p) {
                    t.steps[i].active = true;
                    t.steps[i].accent = chance(rng) < prof.accentP;
                }
            }
        }
        SyncMixer(pat);
    }

    // Randomizes one synth track's notes, constrained to the RANDOM tab's
    // KEY/SCALE picker (project.randomizerKeyRoot/randomizerScale, C Dorian
    // by default) within [octaveLow, octaveHigh]. Used for all 3 synths:
    // synth 1 ("bass") gets octaves 1-2, synths 2-3 get the higher C3-C5
    // band appropriate for leads/stabs sitting above a bassline.
    //
    // `complex` selects the note-density profile (see rndDensityMode):
    // false ("SIMPLE") uses a lower per-step note chance and ties more
    // notes together into held notes, for a sparser result; true
    // ("COMPLEX") raises the note chance and ties fewer notes, so more
    // individual hits land -- literally "less notes" vs. "more notes" per
    // the requested behavior, not just a different feel.
    void RandomizeMelodic(int synthIndex, int octaveLow, int octaveHigh, bool complex) {
        RandomizeMelodicMask(synthIndex, OctaveMaskRange(octaveLow, octaveHigh), complex);
    }
    // v44.3: the octaves are a SET (bit n = octave n), chosen per part in the RANDOM block (project.rndOctaves).
    void RandomizeMelodicOctaves(int synthIndex, bool complex) {
        if (synthIndex < 0 || synthIndex >= kNumSynthTracks) return;
        RandomizeMelodicMask(synthIndex, project.rndOctaves[static_cast<size_t>(synthIndex)], complex);
    }
    void RandomizeMelodicMask(int synthIndex, unsigned octaveMask, bool complex) {
        PushUndo();
        std::mt19937& rng = RngEngine();
        std::uniform_real_distribution<float> chance(0.0f, 1.0f);
        Pattern& pat = CurrentPattern();
        int len = pat.masterLength;
        Track& t = pat.tracks[kNumDrumTracks + synthIndex];
        for (auto& s : t.steps) s = Step{};

        std::vector<int> scaleNotes = BuildScaleNotesMask(project.randomizerKeyRoot, project.randomizerScale, octaveMask);
        if (scaleNotes.empty()) return; // defensive -- shouldn't happen for any valid octave range
        const int nScale = static_cast<int>(scaleNotes.size());
        auto isRoot = [&](int n) { return (((n - project.randomizerKeyRoot) % 12) + 12) % 12 == 0; };

        // v26: a melodic WALK through the scale (mostly steps of 0-2 scale degrees, rare leaps, a pull back to
        // the root / a middle octave) instead of independent uniform picks, with more notes and accents on the
        // strong beats. Everything is still taken from the key/scale in the header, so it stays in key by
        // construction; slides are only used between neighbouring pitches (<= 2 semitones) so the glide itself
        // never passes through notes outside the scale.
        auto pickRootIdx = [&]() {
            std::vector<int> r; for (int i = 0; i < nScale; ++i) if (isRoot(scaleNotes[i])) r.push_back(i);
            if (r.empty()) return nScale / 2;
            return r[std::uniform_int_distribution<size_t>(0, r.size() - 1)(rng)];
        };
        float noteProb = complex ? 0.60f : 0.32f;
        float heldProb = complex ? 0.10f : 0.25f;

        // Chord awareness: the chord (of the edited chord scene / the scene's own chords) that sounds at a step.
        const Track& chordTr = StepTrack(pat, kChordTrackIndex);
        const int chordLen = std::clamp<int>(chordTr.useOwnLength ? chordTr.length : len, 1, kMaxSteps);
        bool anyChord = false;
        if (rndChordAware) for (int k = 0; k < chordLen; ++k) if (chordTr.steps[k].active && chordTr.steps[k].chordSize > 0) { anyChord = true; break; }
        auto chordAt = [&](int step, unsigned& pcMask, int& rootPc) { return anyChord && ChordSoundingAt(chordTr, chordLen, step, pcMask, rootPc); };
        // nearest scale index (to `from`) whose pitch class is in `mask`; -1 when the scale has none of them
        auto nearestIn = [&](int from, unsigned mask) {
            int best = -1, bd = 1 << 20;
            for (int q = 0; q < nScale; ++q) {
                if (!(mask & (1u << (scaleNotes[q] % 12)))) continue;
                int d = std::abs(q - from) * 4 + (chance(rng) < 0.5f ? 0 : 1);
                if (d < bd) { bd = d; best = q; }
            }
            return best;
        };
        int cur = pickRootIdx();
        int prevNote = -1; int prevEnd = -1;                       // prev note and the step where it ends
        int i = 0;
        while (i < len) {
            const bool strong = (i % 4 == 0), mid = (i % 2 == 0);
            float p = noteProb * (strong ? 1.55f : mid ? 1.0f : 0.7f);
            if (i == 0) p = 1.0f;                                  // the phrase starts on the one
            if (chance(rng) < std::min(0.95f, p)) {
                // step size in scale degrees
                float r = chance(rng); int d;
                if (r < 0.14f) d = 0;
                else if (r < 0.54f) d = (chance(rng) < 0.5f) ? 1 : -1;
                else if (r < 0.78f) d = (chance(rng) < 0.5f) ? 2 : -2;
                else if (r < 0.90f) d = (chance(rng) < 0.5f) ? 3 : -3;
                else d = (chance(rng) < 0.5f ? 1 : -1) * (4 + static_cast<int>(chance(rng) * 3.0f));
                int nxt = cur + d;
                if (nxt < 0 || nxt >= nScale) nxt = cur - d;               // bounce at the range edges
                nxt = std::clamp(nxt, 0, nScale - 1);
                // gravity: strong beats like the root, every so often jump back to it
                if (strong && chance(rng) < (i == 0 ? 0.7f : 0.22f)) nxt = pickRootIdx();
                unsigned cm; int crp;
                if (chordAt(i, cm, crp)) {
                    // strong beats land on a chord tone (the bass prefers the chord root), the rest walks freely
                    const bool bassPart = synthIndex == 0;
                    if (strong || (bassPart && mid && chance(rng) < 0.5f)) {
                        int hit = -1;
                        if (bassPart && crp >= 0 && chance(rng) < (i % 8 == 0 ? 0.85f : 0.55f)) hit = nearestIn(nxt, 1u << crp);
                        if (hit < 0) hit = nearestIn(nxt, cm);
                        if (hit >= 0) nxt = hit;
                    }
                }
                cur = nxt;
                int note = scaleNotes[cur];
                bool held = chance(rng) < heldProb && (i + 1) < len;
                Step& s = t.steps[i];
                s.active = true;
                s.note = static_cast<int8_t>(note);
                s.accent = chance(rng) < (strong ? 0.35f : 0.10f);
                s.lengthSteps = held ? 2 : 1;
                // slide only into the step right after a note, and only for a neighbouring pitch
                s.slide = prevEnd == i && prevNote >= 0 && std::abs(note - prevNote) <= 2 && note != prevNote && chance(rng) < 0.30f;
                prevNote = note; prevEnd = i + s.lengthSteps;
                i += held ? 2 : 1;
            } else {
                ++i;
            }
        }
        SyncMixer(pat);
        statusSuffix = std::string(anyChord ? "  ON CHORDS  " : "  ") + kKeyNames[std::clamp(project.randomizerKeyRoot, 0, kNumKeys - 1)] + " " +
                       kScaleNames[std::clamp(project.randomizerScale, 0, kNumScales - 1)];
        if (SceneChordsOutOfKey(pat)) {
            statusSuffix += std::string("  (CHORDS IN ") + kKeyNames[std::clamp<int>(ChordsSrcKey(pat), 0, kNumKeys - 1)] + " " +
                            kScaleNames[std::clamp<int>(ChordsSrcScale(pat), 0, kNumScales - 1)] + "!)";
        }
    }

    // The chord of `chordTr` that sounds at `step`: the pitch classes (bit mask) and the root. False in a gap or without chords.
    bool ChordSoundingAt(const Track& chordTr, int chordLen, int step, unsigned& pcMask, int& rootPc) const {
        pcMask = 0; rootPc = -1;
        const int j = step % std::max(1, chordLen);
        for (int k = j; k >= 0; --k) {
            const Step& cs = chordTr.steps[k];
            if (!cs.active || cs.chordSize <= 0) continue;
            if (j >= k + std::max<int>(1, cs.lengthSteps)) return false;      // gap after the chord
            int lo = 127;
            for (int q = 0; q < cs.chordSize; ++q) { pcMask |= 1u << (((cs.chordNotes[q] % 12) + 12) % 12); lo = std::min<int>(lo, cs.chordNotes[q]); }
            ChordSpec sp; rootPc = IdentifyChord(cs.chordNotes.data(), cs.chordSize, sp) ? ((sp.root % 12) + 12) % 12 : ((lo % 12) + 12) % 12;
            return true;
        }
        return false;
    }

    // v44.4: what the RANDOM button of a melodic part does: mode 0 SIMPLE, 1 COMPLEX (the melodic walk), 2 RHYTHM (a figure
    // with a chosen number of notes per bar, settings in project.rndRhythm).
    void RandomizeMelodicMode(int synthIndex, int mode) {
        if (mode >= 2) RandomizeRhythm(synthIndex);
        else RandomizeMelodicOctaves(synthIndex, mode == 1);
    }

    // RHYTHM randomizer (v44.4). Writes a rhythmic figure on one synth part: `minNotes..maxNotes` notes in every bar (a bar =
    // 4 beats of the pattern grid), on the beats more often than between them, optionally the first one always on the one.
    // The pitch comes from the header's key / scale and the part's octave set: only the root (mostly its lowest octave), root
    // and fifth, the sounding chord, or any note of the scale. Notes are SHORT (one step), HELD (to the next note) or MIX.
    // With SAME EVERY BAR one bar is drawn and repeated, otherwise every bar gets a figure of its own.
    void RandomizeRhythm(int synthIndex) {
        if (synthIndex < 0 || synthIndex >= kNumSynthTracks) return;
        PushUndo();
        std::mt19937& rng = RngEngine();
        std::uniform_real_distribution<float> chance(0.0f, 1.0f);
        Pattern& pat = CurrentPattern();
        Track& t = pat.tracks[kNumDrumTracks + synthIndex];
        const RhythmRnd cfg = NormalizeRhythm(project.rndRhythm[static_cast<size_t>(synthIndex)]);
        const int len = std::clamp(t.useOwnLength ? t.length : pat.masterLength, 1, kMaxSteps);
        const int beat = ClampGrid(pat.stepsPerBeat), bar = 4 * beat;
        for (auto& s : t.steps) s = Step{};

        const std::vector<int> scaleNotes = BuildScaleNotesMask(project.randomizerKeyRoot, project.randomizerScale, project.rndOctaves[static_cast<size_t>(synthIndex)]);
        if (scaleNotes.empty()) return;
        const int nScale = static_cast<int>(scaleNotes.size());
        const int keyRoot = project.randomizerKeyRoot;
        auto pcOf = [&](int idx) { return ((scaleNotes[static_cast<size_t>(idx)] % 12) + 12) % 12; };
        auto idxWithPc = [&](int pc) { std::vector<int> r; for (int i = 0; i < nScale; ++i) if (pcOf(i) == ((pc % 12) + 12) % 12) r.push_back(i); return r; };
        auto pickLowBiased = [&](const std::vector<int>& v) {      // mostly the lowest, now and then another octave
            if (v.size() == 1 || chance(rng) < 0.6f) return v.front();
            return v[std::uniform_int_distribution<size_t>(0, v.size() - 1)(rng)];
        };
        std::vector<int> roots = idxWithPc(keyRoot);
        if (roots.empty()) roots.push_back(nScale / 2);
        std::vector<int> fifths = idxWithPc(keyRoot + 7);
        if (fifths.empty()) fifths = roots;

        const Track& chordTr = StepTrack(pat, kChordTrackIndex);
        const int chordLen = std::clamp<int>(chordTr.useOwnLength ? chordTr.length : pat.masterLength, 1, kMaxSteps);
        bool anyChord = false;
        for (int k = 0; k < chordLen; ++k) if (chordTr.steps[k].active && chordTr.steps[k].chordSize > 0) { anyChord = true; break; }

        int walk = roots.front();
        auto pickNote = [&](int step) -> int {
            switch (cfg.pitch) {
            case RhythmPitch::Root: return scaleNotes[static_cast<size_t>(pickLowBiased(roots))];
            case RhythmPitch::RootFifth: return scaleNotes[static_cast<size_t>(chance(rng) < 0.7f ? pickLowBiased(roots) : pickLowBiased(fifths))];
            case RhythmPitch::Chord: {
                unsigned cm; int crp;
                if (anyChord && ChordSoundingAt(chordTr, chordLen, step, cm, crp)) {
                    if (synthIndex == 0 && crp >= 0) { const auto v = idxWithPc(crp); if (!v.empty()) return scaleNotes[static_cast<size_t>(pickLowBiased(v))]; }
                    std::vector<int> v; for (int i = 0; i < nScale; ++i) if (cm & (1u << pcOf(i))) v.push_back(i);
                    if (!v.empty()) return scaleNotes[static_cast<size_t>(v[std::uniform_int_distribution<size_t>(0, v.size() - 1)(rng)])];
                }
                return scaleNotes[static_cast<size_t>(pickLowBiased(roots))];
            }
            default: {
                const int d = std::uniform_int_distribution<int>(-2, 2)(rng);
                walk = std::clamp(walk + d, 0, nScale - 1);
                if (chance(rng) < 0.25f) walk = pickLowBiased(roots);
                return scaleNotes[static_cast<size_t>(walk)];
            }
            }
        };

        struct Hit { int pos; int length; bool accent; int note; };
        std::vector<Hit> tmpl;     // the first bar, for SAME EVERY BAR
        int total = 0;
        for (int start = 0, b = 0; start < len; start += bar, ++b) {
            const int barLen = std::min(bar, len - start);
            std::vector<Hit> hits;
            if (cfg.sameEveryBar && b > 0) {
                for (const Hit& h : tmpl) if (h.pos < barLen) hits.push_back(h);
            } else {
                const int count = std::uniform_int_distribution<int>(cfg.minNotes, cfg.maxNotes)(rng);
                const std::vector<int> pos = PickRhythmSteps(rng, bar, count, cfg.onTheOne);
                for (int p : pos) if (p < barLen) hits.push_back({p, 1, chance(rng) < ((p % beat == 0) ? 0.25f : 0.08f), 0});
            }
            // lengths run to the next note of the bar (or its end)
            for (size_t k = 0; k < hits.size(); ++k) {
                if (cfg.sameEveryBar && b > 0) { hits[k].length = std::min(hits[k].length, (k + 1 < hits.size() ? hits[k + 1].pos : barLen) - hits[k].pos); continue; }
                const int room = (k + 1 < hits.size() ? hits[k + 1].pos : barLen) - hits[k].pos;
                hits[k].length = cfg.length == RhythmLen::Short ? 1 : cfg.length == RhythmLen::Held ? room : std::uniform_int_distribution<int>(1, std::max(1, room))(rng);
            }
            for (size_t k = 0; k < hits.size(); ++k) {
                Hit& h = hits[k];
                const bool reuse = cfg.sameEveryBar && b > 0 && cfg.pitch != RhythmPitch::Chord;
                if (reuse) h.note = tmpl[std::min(k, tmpl.size() - 1)].note;
                else h.note = pickNote(start + h.pos);
                Step& s = t.steps[static_cast<size_t>(start + h.pos)];
                s.active = true; s.note = static_cast<int8_t>(h.note); s.accent = h.accent;
                s.lengthSteps = static_cast<uint8_t>(std::clamp(h.length, 1, 255));
                ++total;
            }
            if (b == 0) tmpl = hits;
        }
        SyncMixer(pat);
        static const char* kNames[3] = {"BASS", "SYNTH 2", "SYNTH 3"};
        SetStatus(std::string("RHYTHM ") + kNames[synthIndex] + ": " + std::to_string(total) + " NOTES  " + RhythmPitchName(cfg.pitch) + "  " +
                  kKeyNames[std::clamp(project.randomizerKeyRoot, 0, kNumKeys - 1)] + " " + kScaleNames[std::clamp(project.randomizerScale, 0, kNumScales - 1)]);
    }

    // ---- Euclid / rotate (v44.4) -----------------------------------------------------------------------------------
    // EUCLID: `hits` hits spread evenly over `steps` steps (repeated through the track length), `rotate` slides the figure.
    // The hits copy the first hit the track already has (sound, velocity, note ...); an empty track gets the default hit
    // (drums: the track's default variant, synths: the lowest key root of the part's octave set). Not for the chord track.
    // Returns the number of hits written (0 = nothing done). One undo step.
    // The hit an empty track gets (drums: the track's default variant, synths: the lowest key root of the part's octave set).
    Step DefaultEuclidHit(int ti, const Track& t) {
        Step hit{};
        if (t.type == TrackType::Synth) {
            const int part = std::clamp(ti - kNumDrumTracks, 0, kNumSynthTracks - 1);
            const std::vector<int> sn = BuildScaleNotesMask(project.randomizerKeyRoot, project.randomizerScale, project.rndOctaves[static_cast<size_t>(part)]);
            int note = 36;
            for (int n : sn) if ((((n - project.randomizerKeyRoot) % 12) + 12) % 12 == 0) { note = n; break; }
            hit.note = static_cast<int8_t>(note);
        }
        return hit;
    }
    int ApplyEuclid(int ti, int hits, int steps, int rotate) {
        if (ti < 0 || ti >= kNumTracks) return 0;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        Pattern& pat = CurrentPattern();
        Track& t = StepTrack(pat, ti);
        if (t.type == TrackType::Chord) { SetStatus("EUCLID: NOT FOR THE CHORD TRACK (ROTATE WORKS)"); return 0; }
        const int len = std::clamp(t.useOwnLength ? t.length : pat.masterLength, 1, kMaxSteps);
        Step hit; bool found = false;
        for (int i = 0; i < len; ++i) if (t.steps[static_cast<size_t>(i)].active) { hit = t.steps[static_cast<size_t>(i)]; found = true; break; }
        if (!found) hit = DefaultEuclidHit(ti, t);
        hit.nudge = 0; hit.slide = false;
        PushUndo();
        t.euclidOn = false; t.euclidOrig.clear();   // v44.55: this is a plain overwrite: the kept melody of EUCLID ON is gone
        const int n = ApplyEuclidToTrack(t, len, hits, steps, rotate, hit);
        SyncMixer(pat);
        SetStatus("EUCLID " + std::to_string(std::clamp(hits, 0, steps)) + "/" + std::to_string(steps) + (rotate ? " ROTATE " + std::to_string(rotate) : std::string()) + ": " + std::to_string(n) + " HITS");
        return n;
    }
    // v44.55: EUCLID ON. The melody (or beat) of the track is KEPT (its active steps, in time order) and the steps are derived from it:
    // the notes go in turn onto `hits` hits spread evenly over `steps` steps, rotated by `rotate`, and cycle when there are more hits than
    // notes. Calling it again while it is on only changes the figure (the kept melody is not re-read), so the controls can be turned live.
    // EuclidMelodyOff puts the original back; if the steps were edited by hand since, they are kept as they are (nothing is lost).
    // Not for the chord track. An empty track gets the default hit. Undo: one step to turn it on / off; a run of live changes is one step.
    std::vector<Step> EuclidMelodyOf(int ti, const Track& t) {
        std::vector<Step> melody; melody.reserve(t.euclidOrig.size());
        for (const auto& o : t.euclidOrig) melody.push_back(o.second);
        if (melody.empty()) melody.push_back(DefaultEuclidHit(ti, t));
        return melody;
    }
    int EuclidMelodyOn(int ti, int hits, int steps, int rotate) {
        if (ti < 0 || ti >= kNumTracks) return 0;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        Pattern& pat = CurrentPattern();
        Track& t = StepTrack(pat, ti);
        if (t.type == TrackType::Chord) { SetStatus("EUCLID: NOT FOR THE CHORD TRACK (ROTATE WORKS)"); return 0; }
        const int len = std::clamp(t.useOwnLength ? t.length : pat.masterLength, 1, kMaxSteps);
        steps = std::clamp(steps, 1, kMaxSteps);
        hits = std::clamp(hits, 0, steps);
        rotate = std::clamp(rotate, -(steps - 1), steps - 1);
        const double now = UndoNowSeconds();
        if (!t.euclidOn || ti != euclidUndoTrack_ || now - euclidUndoT_ > 0.8) PushUndo();
        euclidUndoT_ = now; euclidUndoTrack_ = ti;
        if (!t.euclidOn) {
            t.euclidOrig = EuclidCaptureSteps(t, len);   // an empty track keeps nothing: turning it off leaves it empty again
            t.euclidOn = true;
        }
        t.euclidHits = static_cast<uint8_t>(hits); t.euclidSteps = static_cast<uint8_t>(steps); t.euclidRotate = static_cast<int8_t>(rotate);
        std::vector<Step> melody = EuclidMelodyOf(ti, t);
        const int n = EuclidMelodyApply(t, len, melody, hits, steps, rotate);
        SyncMixer(pat);
        SetStatus("EUCLID ON " + std::to_string(hits) + "/" + std::to_string(steps) + (rotate ? " ROTATE " + std::to_string(rotate) : std::string()) + ": " +
                  std::to_string(melody.size()) + " NOTES ON " + std::to_string(n) + " HITS");
        return n;
    }
    void EuclidMelodyOff(int ti) {
        if (ti < 0 || ti >= kNumTracks) return;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        Pattern& pat = CurrentPattern();
        Track& t = StepTrack(pat, ti);
        if (!t.euclidOn) return;
        const int len = std::clamp(t.useOwnLength ? t.length : pat.masterLength, 1, kMaxSteps);
        const std::vector<Step> melody = EuclidMelodyOf(ti, t);
        const bool intact = EuclidDerivedIntact(t, len, melody);
        PushUndo();
        if (intact) {
            for (int i = 0; i < len; ++i) t.steps[static_cast<size_t>(i)] = Step{};
            for (const auto& o : t.euclidOrig) if (o.first < len) t.steps[o.first] = o.second;
        }
        t.euclidOn = false; t.euclidOrig.clear();
        euclidUndoTrack_ = -1;
        SyncMixer(pat);
        SetStatus(intact ? "EUCLID OFF: THE MELODY IS BACK" : "EUCLID OFF: YOUR EDITED STEPS ARE KEPT");
    }
    double euclidUndoT_ = -100.0; int euclidUndoTrack_ = -1;
    // ROTATE: the whole figure of a track slides `shift` steps round its length (any track, chord track too). One undo step.
    void RotateTrack(int ti, int shift) {
        if (ti < 0 || ti >= kNumTracks) return;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        Pattern& pat = CurrentPattern();
        Track& t = StepTrack(pat, ti);
        const int len = std::clamp(t.useOwnLength ? t.length : pat.masterLength, 1, kMaxSteps);
        if (((shift % len) + len) % len == 0) return;
        PushUndo();
        RotateSteps(t, len, shift);
        if (t.type == TrackType::Chord) RefreshChordLive(true);
        SyncMixer(pat);
        SetStatus(std::string("ROTATED ") + (shift > 0 ? "+" : "") + std::to_string(shift) + " STEPS");
    }

    // Moves every note of one pitched track (synth tracks and the chord track) to the nearest note of the
    // header's key / scale. Undoable. Returns how many notes moved.
    int SnapTrackToScale(int trackIndex) {
        if (trackIndex < kNumDrumTracks || trackIndex >= kNumTracks) return 0;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        PushUndo();
        Track& t = StepTrack(CurrentPattern(), trackIndex);
        const int key = project.randomizerKeyRoot, sc = project.randomizerScale;
        int moved = 0;
        for (auto& st : t.steps) {
            if (!st.active) continue;
            if (t.type == TrackType::Chord) {
                for (int k = 0; k < st.chordSize && k < kMaxChordVoices; ++k) {
                    int n = SnapToScale(st.chordNotes[k], key, sc);
                    if (n != st.chordNotes[k]) { st.chordNotes[k] = static_cast<int8_t>(n); ++moved; }
                }
            } else {
                int n = SnapToScale(st.note, key, sc);
                if (n != st.note) { st.note = static_cast<int8_t>(n); ++moved; }
            }
        }
        RefreshChordLive(true);
        SetStatus(std::string("SNAPPED ") + std::to_string(moved) + " NOTES TO " + kKeyNames[key] + " " + kScaleNames[sc]);
        return moved;
    }

    // ---- Key / scale change vs. written notes -------------------------------
    // Changing KEY / SCALE in the header leaves every written chord untouched.
    // These re-tune on request: the notes move by the key difference (shortest
    // way, -6..+5 semitones) from the key the scene was written in
    // (Pattern::chordsKey, unknown = no shift) and are then snapped to the
    // nearest note of the new scale. `includeMelody` also re-tunes the three
    // synth tracks. Returns how many notes changed.
    int AdjustSceneToScale(Pattern& pat, bool includeMelody) {
        const int newKey = std::clamp(project.randomizerKeyRoot, 0, kNumKeys - 1);
        const int newScale = std::clamp(project.randomizerScale, 0, kNumScales - 1);
        int delta = 0;
        if (pat.chordsKey >= 0) delta = ((newKey - pat.chordsKey + 6) % 12 + 12) % 12 - 6;
        int changed = 0;
        auto fix = [&](int n) {
            int m = std::clamp(SnapToScale(n + delta, newKey, newScale), 0, 127);
            if (m != n) ++changed;
            return m;
        };
        for (int ti = 0; ti < kNumTracks; ++ti) {
            Track& tr = pat.tracks[ti];
            if (tr.type == TrackType::Chord) {
                if (project.chordBankOn) continue;       // the bank's chords are adjusted per chord scene
                for (Step& s : tr.steps) {
                    if (!s.active) continue;
                    for (int i = 0; i < s.chordSize; ++i) s.chordNotes[i] = static_cast<int8_t>(fix(s.chordNotes[i]));
                    std::sort(s.chordNotes.begin(), s.chordNotes.begin() + s.chordSize);
                }
            } else if (includeMelody && tr.type == TrackType::Synth) {
                for (Step& s : tr.steps) if (s.active) s.note = static_cast<int8_t>(fix(s.note));
            }
        }
        if (!project.chordBankOn) {
            pat.chordsKey = static_cast<int8_t>(newKey);
            pat.chordsScale = static_cast<int8_t>(newScale);
        } else if (includeMelody) {
            pat.chordsKey = static_cast<int8_t>(newKey);
            pat.chordsScale = static_cast<int8_t>(newScale);
        }
        return changed;
    }
    // Chord-bank counterpart of the chord part of AdjustSceneToScale.
    int AdjustChordSceneToScale(ChordScene& cs) {
        const int newKey = std::clamp(project.randomizerKeyRoot, 0, kNumKeys - 1);
        const int newScale = std::clamp(project.randomizerScale, 0, kNumScales - 1);
        int delta = 0;
        if (cs.key >= 0) delta = ((newKey - cs.key + 6) % 12 + 12) % 12 - 6;
        int changed = 0;
        for (Step& s : cs.track.steps) {
            if (!s.active) continue;
            for (int i = 0; i < s.chordSize; ++i) {
                int n = s.chordNotes[i];
                int m = std::clamp(SnapToScale(n + delta, newKey, newScale), 0, 127);
                if (m != n) ++changed;
                s.chordNotes[i] = static_cast<int8_t>(m);
            }
            std::sort(s.chordNotes.begin(), s.chordNotes.begin() + s.chordSize);
        }
        cs.key = static_cast<int8_t>(newKey); cs.scale = static_cast<int8_t>(newScale);
        return changed;
    }
    // True when this scene's chords were written in another key / scale than
    // the project's current one (drives the "ADJUST" hint).
    int ChordsSrcKey(const Pattern& pat) const { return project.chordBankOn ? project.chordScenes[std::clamp(project.chordSceneEdit, 0, kNumChordScenes - 1)].key : pat.chordsKey; }
    int ChordsSrcScale(const Pattern& pat) const { return project.chordBankOn ? project.chordScenes[std::clamp(project.chordSceneEdit, 0, kNumChordScenes - 1)].scale : pat.chordsScale; }
    bool SceneChordsOutOfKey(const Pattern& pat) const {
        int k = pat.chordsKey, sc = pat.chordsScale;
        if (project.chordBankOn) { const ChordScene& cs = project.chordScenes[std::clamp(project.chordSceneEdit, 0, kNumChordScenes - 1)]; k = cs.key; sc = cs.scale; }
        return k >= 0 && (k != project.randomizerKeyRoot || sc != project.randomizerScale);
    }
    void AdjustChordsToScale(bool includeMelody) {
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        PushUndo();
        int n = AdjustSceneToScale(CurrentPattern(), includeMelody);
        if (project.chordBankOn) n += AdjustChordSceneToScale(EditChordScene());
        RefreshChordLive(true);
        SetStatus(std::string(includeMelody ? "NOTES" : "CHORDS") + " ADJUSTED TO " +
                  kKeyNames[project.randomizerKeyRoot] + " " + kScaleNames[project.randomizerScale] + " (" + std::to_string(n) + ")");
    }
    // Once per frame: with FOLLOW KEY on, a key / scale change re-tunes the
    // chord track of every scene at once (undo covers the scene being edited).
    int followSeenKey_ = -1, followSeenScale_ = -1;
    void TickKeyFollow() {
        const int k = project.randomizerKeyRoot, sc = project.randomizerScale;
        if (followSeenKey_ < 0) { followSeenKey_ = k; followSeenScale_ = sc; return; }
        if (k == followSeenKey_ && sc == followSeenScale_) return;
        followSeenKey_ = k; followSeenScale_ = sc;
        if (!project.chordsFollowKey) return;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        PushUndo();
        for (Pattern& p : project.patterns) AdjustSceneToScale(p, false);
        if (project.chordBankOn) for (ChordScene& cs : project.chordScenes) AdjustChordSceneToScale(cs);
        RefreshChordLive(true);
        SetStatus(std::string("CHORDS FOLLOW KEY: ") + kKeyNames[k] + " " + kScaleNames[sc]);
    }

    // ---- Chord progressions (Phase 6) --------------------------------------
    // Resolves a scale-degree sequence (see ChordProgressions.h) into actual
    // chords via core/Chords.h's BuildChord, using the SAME KEY/SCALE picker
    // the melodic randomizers above use (randomizerKeyRoot/randomizerScale)
    // -- so anything the CHORDS tab generates automatically harmonizes with
    // whatever RANDOM tab melodies you've already got, sharing one key
    // rather than drifting independently. Writes onto the dedicated Chord
    // track (see Types.h's TrackType::Chord), clearing it first the same
    // "wipe then rebuild" way ApplyGenrePattern/RandomizeMelodic do. Shared
    // by ApplyChordProgression (a named template) and RandomizeChordProgression
    // (a freshly generated one) below -- undoable (PATTERN-scoped, like
    // those two), but the CHORDS tab's own SETTINGS (chdComplex/
    // chdOutputMode/chdArpStyle/etc, all plain AppState fields) stay
    // deliberately out of Undo's coverage, same "settings vs. pattern data"
    // split the rest of this file already draws.
    // v44.49: `undo` = false when a caller (ApplyStyle) has already made the one undo step; `lenOverride` > 0 sets the length of one
    // chord in steps; `forceStack` writes held chords whatever the CHORDS tab's output mode is.
    void ApplyDegreeSequence(const std::vector<int>& degrees, const std::string& label, bool undo = true, int lenOverride = 0, bool forceStack = false) {
        if (degrees.empty()) return;
        if (undo) PushUndo();
        std::mt19937& rng = RngEngine();
        Pattern& pat = CurrentPattern();
        int len = pat.masterLength;
        Track& t = StepTrack(pat, kChordTrackIndex);
        for (auto& s : t.steps) s = Step{};
        if (project.chordBankOn) {
            EditChordScene().key = static_cast<int8_t>(project.randomizerKeyRoot);
            EditChordScene().scale = static_cast<int8_t>(project.randomizerScale);
        } else {
            pat.chordsKey = static_cast<int8_t>(project.randomizerKeyRoot);
            pat.chordsScale = static_cast<int8_t>(project.randomizerScale);
        }

        int numChords = static_cast<int>(degrees.size());
        int chordLen = lenOverride > 0 ? lenOverride : (chdChordLengthSteps > 0 ? chdChordLengthSteps : std::max(1, len / numChords));
        // C3 -- a warm pad/comping register, deliberately distinct from the
        // bass (octaves 1-2) and lead (C3-C5) bands the RANDOM tab's
        // melodic randomizers already use, so a chord doesn't automatically
        // collide registers with either.
        constexpr int kAnchorOctave = 3;

        int stepPos = 0;
        for (int ci = 0; ci < numChords && stepPos < len && stepPos < kMaxSteps; ++ci) {
            Chord chord = BuildChord(project.randomizerKeyRoot, project.randomizerScale,
                                      degrees[ci], chdComplex, kAnchorOctave);
            int span = std::min({chordLen, len - stepPos, kMaxSteps - stepPos});
            if (span <= 0) break;
            int n = std::min(static_cast<int>(chord.notes.size()), kMaxChordVoices);

            if (forceStack || chdOutputMode == ChordOutputMode::Stack) {
                // One held step carrying all `n` notes at once for the
                // chord's whole span -- a real chord (see ChordVoice.h).
                Step& s = t.steps[stepPos];
                s.active = true;
                s.chordSize = static_cast<uint8_t>(n);
                for (int i = 0; i < kMaxChordVoices; ++i) s.chordNotes[i] = static_cast<int8_t>(i < n ? chord.notes[i] : 60);
                s.lengthSteps = static_cast<uint8_t>(span);
            } else {
                // Arpeggio: a single-note hit every chdArpRateSteps steps
                // within this chord's span, cycling the chord's tones per
                // chdArpStyle -- chordSize=1 per step, so this plays back
                // through the exact same "1 note = a normal note" path as
                // a Stack chord with n==1, nothing arpeggio-specific needed
                // downstream in Sequencer/ChordVoice.
                int rate = std::max(1, chdArpRateSteps);
                int hit = 0;
                for (int off = 0; off < span; off += rate) {
                    int idx = 0;
                    if (n > 1) {
                        switch (chdArpStyle) {
                            case ArpStyle::Up:   idx = hit % n; break;
                            case ArpStyle::Down: idx = (n - 1) - (hit % n); break;
                            case ArpStyle::UpDown: {
                                int period = 2 * (n - 1);
                                int m = hit % period;
                                idx = (m < n) ? m : period - m;
                                break;
                            }
                            case ArpStyle::Random:
                                idx = std::uniform_int_distribution<int>(0, n - 1)(rng);
                                break;
                        }
                    }
                    Step& s = t.steps[stepPos + off];
                    s.active = true;
                    s.chordSize = 1;
                    s.chordNotes[0] = static_cast<int8_t>(chord.notes[idx]);
                    s.lengthSteps = static_cast<uint8_t>(std::min(rate, span - off));
                    ++hit;
                }
            }
            stepPos += span;
        }
        SyncMixer(pat);
        if (!label.empty()) SetStatus(label);
    }

    // progressionIndex indexes ChordProgressions.h's AllChordProgressions()
    // directly (no "RANDOM" pseudo-entry in this list -- see
    // RandomizeChordProgression below for the dedicated random generator).
    void ApplyChordProgression(int progressionIndex) {
        const auto& all = AllChordProgressions();
        if (progressionIndex < 0 || progressionIndex >= static_cast<int>(all.size())) return;
        const auto& prog = all[progressionIndex];
        ApplyDegreeSequence(prog.degrees, std::string("CHORDS: ") + prog.name);
    }

    // Generates a fresh scale-degree sequence rather than picking among the
    // named templates above -- weighted toward common tonal motion
    // (resolving toward the tonic, circle-of-fifths-ish root movement)
    // instead of a uniform 1..7 pick each time, so the result still reads
    // as "a progression" rather than scale-degree noise, while staying
    // genuinely different every press (~20% of each step is a wildcard
    // uniform-random degree, and the whole thing isn't just reshuffling
    // ChordProgressions.h's fixed list).
    void RandomizeChordProgression() {
        std::mt19937& rng = RngEngine();
        std::uniform_real_distribution<float> chance(0.0f, 1.0f);
        static const std::vector<std::vector<int>> kLikelyNext = {
            {4, 5, 6, 2}, // from degree 1
            {5, 4, 7},    // from degree 2
            {6, 4},       // from degree 3
            {5, 1, 2},    // from degree 4
            {1, 6, 4},    // from degree 5
            {4, 2, 5},    // from degree 6
            {1, 3},       // from degree 7
        };
        static const int kLengths[7] = {2, 3, 4, 4, 4, 6, 8}; // weighted toward 4
        int length = kLengths[std::uniform_int_distribution<int>(0, 6)(rng)];

        std::vector<int> degrees;
        int cur = (chance(rng) < 0.7f) ? 1 : std::uniform_int_distribution<int>(1, 7)(rng);
        degrees.push_back(cur);
        for (int i = 1; i < length; ++i) {
            const auto& candidates = kLikelyNext[cur - 1];
            if (chance(rng) < 0.8f && !candidates.empty()) {
                cur = candidates[std::uniform_int_distribution<size_t>(0, candidates.size() - 1)(rng)];
            } else {
                cur = std::uniform_int_distribution<int>(1, 7)(rng);
            }
            degrees.push_back(cur);
        }
        // A progression that just stops on degree 5 or 3 reads as
        // unfinished -- bias (not force) the last chord back to the tonic.
        if (chance(rng) < 0.6f) degrees.back() = 1;

        ApplyDegreeSequence(degrees, "CHORDS: RANDOM");
    }

    // ---- Typical patterns -------------------------------------------------
    // Applies a genre-shaped drum groove template (see DrumPatterns.h) to
    // all 6 drum tracks, plus that template's suggested swing -- unlike
    // RandomizeDrums (per-hit probabilistic noise), this drops in something
    // that already reads as "a real four-on-the-floor" etc. Tiles every 16
    // steps for patterns longer than 16, truncates cleanly for shorter ones.
    // genreIndex is 0-based into AllGenrePatterns(). Undoable (covers steps
    // + swing in one entry, same as pattern copy/paste).
    // v44.47: long patterns. A genre pattern may be several bars long (AMEN is 4, JUNGLE 2 ...; gp.bars). How many of those bars fit
    // depends on the grid, since a scene holds 64 steps at most: 4 bars of 1/16, 2 of 1/32 or of 1/16T, 5 of 1/8T.
    static int GenreBarsFit(const GenrePattern& gp, int spb) {
        spb = ClampGrid(spb);
        return std::max(1, std::min(gp.bars, kMaxSteps / (4 * spb)));
    }
    // Writes one drum slot of a genre pattern into a track: the templates are written in 16ths and mapped to the scene's grid, and
    // the `barsUsed` bars are repeated until the scene length `len` is filled.
    static void WriteGenreSlot(Track& t, const GenrePattern& gp, int slot, int spb, int len, int barsUsed) {
        for (auto& s : t.steps) s = Step{};
        const int period = barsUsed * 4 * spb, limit = barsUsed * 16;
        for (int hit : gp.hitSteps[static_cast<size_t>(slot)]) {
            if (hit >= limit) continue;
            for (int i = (hit * spb + 2) / 4; i < len; i += period) t.steps[static_cast<size_t>(i)].active = true;
        }
        for (int hit : gp.accentSteps[static_cast<size_t>(slot)]) {
            if (hit >= limit) continue;
            for (int i = (hit * spb + 2) / 4; i < len; i += period)
                if (t.steps[static_cast<size_t>(i)].active) t.steps[static_cast<size_t>(i)].accent = true;
        }
    }

    void ApplyGenrePattern(int genreIndex) {
        const auto& all = AllGenrePatterns();
        if (genreIndex < 0 || genreIndex >= static_cast<int>(all.size())) return;
        const GenrePattern& gp = all[genreIndex];

        PushUndo();
        Pattern& pat = CurrentPattern();
        const int spb = ClampGrid(pat.stepsPerBeat);      // templates are written in 16ths: map to the pattern's grid
        const int barsUsed = GenreBarsFit(gp, spb);
        const int typical = barsUsed * 4 * spb;
        const int oldLen = pat.masterLength;
        // v44.47: a pattern that is longer than the scene brings the scene up to its typical length (the other tracks repeat
        // what they have, as with LEN); a scene that is already as long or longer is never shortened.
        if (typical > oldLen) ResizeSceneNoUndo(pat, typical);
        const int len = pat.masterLength;
        for (int slot = 0; slot < kNumDrumTracks; ++slot) WriteGenreSlot(pat.tracks[static_cast<size_t>(slot)], gp, slot, spb, len, barsUsed);
        pat.swing = gp.swing;
        SyncMixer(pat);
        std::string msg = std::string(gp.name) + " BEAT";
        if (len != oldLen) msg += ", SCENE LENGTH " + std::to_string(oldLen) + " -> " + std::to_string(len) + " (" + std::to_string(barsUsed) + " BARS)";
        else if (gp.bars > 1) msg += " (" + std::to_string(barsUsed) + " BARS)";
        if (barsUsed < gp.bars) msg += ", ONLY THE FIRST " + std::to_string(barsUsed) + " OF " + std::to_string(gp.bars) + " BARS FIT THE " + GridLabel(spb) + " GRID";
        SetStatus(msg);
    }

    // ---- STYLE (v44.49) -------------------------------------------------------------------------------------------------------------
    // One press writes a whole style (core/Styles.h) into the EDITED scene as ONE undo step. Four parts, each switched on or off:
    //   BEAT  - the drum pattern with its swing, the groove (baked into the steps), the kit, and the scene length (it grows to the
    //           typical length of the pattern, never shrinks)
    //   SOUND - the presets of bass, synth 2, synth 3 and chord
    //   NOTES - the bass line and the chord progression, in the current KEY and SCALE (the key itself is never changed)
    //   TEMPO - the style's BPM (off by default: a style never changes the tempo of a set by surprise)
    int styleIndex = 0;
    bool styleBeat = true, styleSound = true, styleNotes = true, styleTempo = false;
    bool ApplyStyle(int index) {
        const auto& styles = AllStyles();
        if (index < 0 || index >= static_cast<int>(styles.size())) return false;
        if (!styleBeat && !styleSound && !styleNotes && !styleTempo) { SetStatus("STYLE: SWITCH ON AT LEAST ONE PART (BEAT / SOUND / NOTES / TEMPO)"); return false; }
        const StyleDef& sd = styles[static_cast<size_t>(index)];
        PushUndo();                                           // the one undo step for everything below
        Pattern& pat = CurrentPattern();
        const int spb = ClampGrid(pat.stepsPerBeat);
        std::string said;
        auto say = [&](const std::string& w) { if (!said.empty()) said += ", "; said += w; };
        if (styleBeat) {
            const int gi = FindGenrePatternIndex(sd.pattern);
            if (gi >= 0) {
                const GenrePattern& gp = AllGenrePatterns()[static_cast<size_t>(gi)];
                const int barsUsed = GenreBarsFit(gp, spb);
                const int typical = barsUsed * 4 * spb;
                if (typical > pat.masterLength) ResizeSceneNoUndo(pat, typical);
                for (int slot = 0; slot < kNumDrumTracks; ++slot) WriteGenreSlot(pat.tracks[static_cast<size_t>(slot)], gp, slot, spb, pat.masterLength, barsUsed);
                pat.swing = gp.swing;
                say("BEAT");
            }
            pat.kitFolder = sd.kit;                           // the whole scene plays this kit: the drums that had a kit of their own follow it
            for (int t = 0; t < kNumDrumTracks; ++t) pat.tracks[static_cast<size_t>(t)].kitFolder.clear();
            SyncSceneKits();
        }
        if (styleSound) {
            const auto& presets = AllSynthPresets();
            auto put = [&](const char* nm, SynthPatch& dst) { const int i = FindSynthPresetIndex(nm); if (i >= 0) dst = presets[static_cast<size_t>(i)].patch; };
            put(sd.bass, pat.synthPatches[0]); put(sd.syn2, pat.synthPatches[1]); put(sd.syn3, pat.synthPatches[2]); put(sd.chord, pat.chordPatch);
            if (EditIsPlaying()) { for (int i = 0; i < kNumSynthTracks; ++i) PushEditedPatchToEngine(false, i); PushEditedPatchToEngine(true, 0); }
            say("SOUND");
        }
        if (styleNotes) {
            // bass line: the one-bar line repeated over the scene, every note snapped into the scale
            Track& bt = pat.tracks[static_cast<size_t>(kNumDrumTracks)];
            for (auto& st : bt.steps) st = Step{};
            const int barLen = 4 * spb, len = std::clamp(pat.masterLength, 1, kMaxSteps);
            const int root = BuildRootNotes(project.randomizerKeyRoot, sd.bassOctave, sd.bassOctave).front();
            for (int bar0 = 0; bar0 < len; bar0 += barLen) {
                for (const StyleBassNote& n : sd.bassLine) {
                    const int pos = bar0 + (n.step * spb + 2) / 4;
                    if (pos >= len || pos >= kMaxSteps) continue;
                    Step& st = bt.steps[static_cast<size_t>(pos)];
                    st.active = true;
                    st.note = static_cast<int8_t>(std::clamp(SnapToScale(root + n.semis, project.randomizerKeyRoot, project.randomizerScale), 0, 127));
                    st.lengthSteps = static_cast<uint8_t>(std::clamp((n.len * spb + 2) / 4, 1, std::max(1, std::min(len - pos, 255))));
                    st.slide = n.slide;
                }
            }
            // chords: the progression repeated until the scene is full, one chord per `chordBeats` beats, held
            const int pi = FindProgressionIndex(sd.progression);
            if (pi >= 0) {
                const std::vector<int>& base = AllChordProgressions()[static_cast<size_t>(pi)].degrees;
                const int chordSteps = std::max(1, sd.chordBeats * spb);
                std::vector<int> degs;
                if (!base.empty()) for (int at = 0, k = 0; at < len; at += chordSteps, ++k) degs.push_back(base[static_cast<size_t>(k) % base.size()]);
                ApplyDegreeSequence(degs, "", false, chordSteps, true);
            }
            say("NOTES");
        }
        if (styleBeat && sd.groove[0] != '\0' && sd.grooveAmount > 0.0f) {   // last: it nudges the drums and the notes written above
            const int gi = FindBuiltinGrooveIndex(sd.groove);
            if (gi >= 0) { pat.groove = BuiltinGrooves()[static_cast<size_t>(gi)]; pat.grooveAmount = sd.grooveAmount; BakeGroove(pat); }
        }
        if (styleTempo) { pat.bpm = static_cast<float>(sd.bpm); say("TEMPO " + std::to_string(sd.bpm)); }
        SyncMixer(pat);
        styleIndex = index;
        SetStatus(std::string("STYLE ") + sd.name + ": " + said + " (ONE UNDO STEP)");
        return true;
    }

    // ---- Grid (v43, v44.2): 1/16, 1/32, 1/16 triplets, 1/8 triplets --------------------------------------------------
    // Switching the grid CONVERTS the pattern in time: every step moves to the nearest step of the new grid (step i of a grid
    // with `old` steps per beat sits at i/old beats, so it goes to round(i * new / old)), note lengths and nudges scale with
    // the step size and the length keeps the same number of bars. 1/16 <-> 1/32 is exact (odd 1/32 steps snap to a neighbour
    // when they are free). A triplet grid cannot hold every 16th: the downbeats and 8ths land exactly, the 16ths in between snap
    // to the nearest triplet step; steps that would land on the same cell go to the next free neighbour, else they are merged
    // (the count is shown in the status line). Before v44.2 the triplet grids only re-read the steps, which made the pattern
    // sound faster or slower. Undoable.
    int SetPatternResolution(int newSpb) {
        newSpb = ClampGrid(newSpb);
        Pattern& pat = CurrentPattern();
        const int oldSpb = ClampGrid(pat.stepsPerBeat);
        if (newSpb == oldSpb) return 0;
        PushUndo();
        const double ratio = static_cast<double>(newSpb) / oldSpb;
        int merged = 0;
        auto convertTrack = [&](Track& t) {
            const auto old = t.steps;
            for (auto& st : t.steps) st = Step{};
            for (int i = 0; i < kMaxSteps; ++i) {
                const Step& src = old[static_cast<size_t>(i)];
                const bool carries = src.active || src.chordSize > 0 || src.nudge != 0 || src.vel != 0 || src.ghost || src.accent;
                if (!carries) continue;
                const double exact = i * ratio;
                const int lo = static_cast<int>(std::floor(exact)), hi = lo + 1;
                const int near = (exact - lo < 0.5) ? lo : hi, other = (near == lo) ? hi : lo;
                int target = -1;
                for (int c : {near, other}) {
                    if (c < 0 || c >= kMaxSteps) continue;
                    const Step& d = t.steps[static_cast<size_t>(c)];
                    if (!d.active && d.chordSize == 0 && d.nudge == 0 && d.vel == 0 && !d.ghost && !d.accent) { target = c; break; }
                }
                if (target < 0) { if (src.active) ++merged; continue; }
                Step& dst = t.steps[static_cast<size_t>(target)];
                dst = src;
                dst.lengthSteps = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(src.lengthSteps * ratio)), 1, 255));
                dst.nudge = static_cast<int8_t>(std::clamp(static_cast<int>(std::lround(src.nudge * ratio)), -50, 50));
            }
            t.length = std::clamp(static_cast<int>(std::lround(static_cast<double>(t.length) * ratio)), 1, kMaxSteps);
        };
        for (Track& t : pat.tracks) convertTrack(t);
        pat.masterLength = std::clamp(static_cast<int>(std::lround(static_cast<double>(pat.masterLength) * ratio)), 1, kMaxSteps);
        pat.stepsPerBeat = newSpb;
        deskStepPage = 0; seqCursorStep = 0;
        SyncMixer(pat);
        SetStatus(std::string("GRID ") + GridLabel(newSpb) + (merged > 0 ? " (" + std::to_string(merged) + " NOTES MERGED)" : std::string()));
        return merged;
    }

    // ---- Feel (v44.2/v44.3): swing + groove --------------------------------------------------------------------------
    // v44.3: the groove works like before v42 again - CHOOSE one in the groove window (it replaces the mixer; shape drawn there),
    // set the AMOUNT, then BAKE: only then it is written into the steps of the tracks that follow it. Choosing alone changes
    // nothing you hear. The chosen groove is "staged" here (not saved in the project); a project written by an older version
    // may still carry a live groove layer in the pattern, which keeps playing until RESET or BAKE.
    Groove stagedGroove;
    float stagedAmount = 1.0f;
    bool helpOpen = false;             // the "?" screen with all gestures (U2)
    // C1 (UI-1): the three tools MIX LAB, CHORD LAB and MASTER FX open the same way: in the room under the header of LIVE or TRACK,
    // one at a time, keys M / C / F, the same key or Esc closes. The tab under them stays what it was (LIVE or TRACK).
    enum class Tool { None, MixLab, ChordLab, MasterFx };
    Tool tool = Tool::None;
    bool ToolOpen(Tool t) const { return tool == t; }
    bool ChordLabOpen() const { return tool == Tool::ChordLab; }
    void OpenTool(Tool t) { tool = t; }
    void CloseTool(Tool t) { if (tool == t) tool = Tool::None; }
    void ToggleTool(Tool t) { tool = tool == t ? Tool::None : t; }
    bool grooveViewOpen = false;       // the groove window is shown instead of the mixer / piano roll / lanes
    void StageGroove(const Groove& g) { stagedGroove = g; if (stagedAmount <= 0.0f) stagedAmount = 1.0f; }
    // Writes the staged groove (at the staged amount) into the steps of the tracks that follow it. Returns false when there is
    // nothing to bake. One undo step; afterwards the staged groove is cleared so a second press cannot add it twice by accident.
    bool BakeStagedGroove() {
        if (stagedAmount <= 0.0f || stagedGroove.IsFlat()) { SetStatus("PICK A GROOVE AND AN AMOUNT FIRST"); return false; }
        Pattern& pat = CurrentPattern();
        PushUndo();
        const Groove oldLayer = pat.groove; const float oldAmount = pat.grooveAmount;
        pat.groove = stagedGroove; pat.grooveAmount = stagedAmount;
        BakeGroove(pat);                                   // writes velocity + nudge into the steps, switches the layer off
        pat.groove = oldLayer; pat.grooveAmount = oldAmount;
        SetStatus("GROOVE " + (stagedGroove.name.empty() ? std::string("(EDITED)") : stagedGroove.name) + " BAKED INTO THE STEPS");
        stagedGroove = Groove{};
        SyncMixer(pat);
        return true;
    }
    // True while anything of the feel layer is switched on (pattern swing, a live groove layer from an old project, a chosen
    // but not yet baked groove, or a track with its own swing). The header's RESET button lights up with it.
    bool FeelIsActive(const Pattern& pat) const {
        if (pat.swing > 0.0f || (pat.grooveAmount > 0.0f && !pat.groove.IsFlat()) || !stagedGroove.IsFlat()) return true;
        for (const Track& t : pat.tracks) if (t.swing >= 0.0f && t.swing != pat.swing) return true;
        return false;
    }
    // One press back to a straight pattern: swing 0, every track follows the pattern swing again, no groove chosen, no live
    // groove layer. Which tracks follow the groove, and nudges / velocities already baked into steps, stay as they are.
    void ResetFeel() {
        Pattern& pat = CurrentPattern();
        PushUndo();
        pat.swing = 0.0f;
        for (Track& t : pat.tracks) t.swing = -1.0f;
        pat.groove = Groove{};
        pat.grooveAmount = 0.0f;
        stagedGroove = Groove{};
        SetStatus("SWING + GROOVE RESET");
    }

    // ---- Pattern length (v44.2) ----------------------------------------------------------------------------------
    // The LEN field. Growing the pattern repeats what was there: the existing steps are tiled over the new range (the
    // first 16 steps / the old length of the tracks that follow the master length), so 16 -> 64 gives four copies and the
    // pattern does not fall silent after the first bar. Only empty territory is filled (a track that already has notes
    // beyond the old length keeps them), tracks with their own length are left alone, and shrinking just hides the steps
    // behind the end like before. Undoable as one step.
    // The length change itself, without undo or status: used by SetPatternLength and by the typical beats that need more room
    // (v44.47). Growing tiles the tracks that follow the master length (see above); shrinking only moves the end.
    void ResizeSceneNoUndo(Pattern& pat, int newLen) {
        newLen = std::clamp(newLen, 1, kMaxSteps);
        const int oldLen = std::max(1, pat.masterLength);
        if (newLen > oldLen) {
            auto tile = [&](Track& t) {
                if (t.useOwnLength) return;
                for (int i = oldLen; i < newLen; ++i) {
                    const Step& d = t.steps[static_cast<size_t>(i)];
                    if (d.active || d.chordSize > 0 || d.nudge != 0 || d.vel != 0) return;   // already written there: leave this track as it is
                }
                for (int i = oldLen; i < newLen; ++i) t.steps[static_cast<size_t>(i)] = t.steps[static_cast<size_t>(i % oldLen)];
            };
            for (Track& t : pat.tracks) tile(t);
        }
        pat.masterLength = newLen;
    }

    void SetPatternLength(int newLen) {
        Pattern& pat = CurrentPattern();
        newLen = std::clamp(newLen, 1, kMaxSteps);
        const int oldLen = std::max(1, pat.masterLength);
        if (newLen == oldLen) return;
        PushUndo();
        ResizeSceneNoUndo(pat, newLen);
        SyncMixer(pat);
        SetStatus("LEN " + std::to_string(oldLen) + " -> " + std::to_string(newLen) + (newLen > oldLen ? " (REPEATED)" : ""));
    }

    // ---- Drum "swap" (live-performance feature) ----------------------------
    // Like ApplyGenrePattern above, but rewrites just ONE drum slot's steps
    // from a genre-pattern template's part for that slot -- every other
    // track, AND the pattern's swing, are left completely untouched. So
    // pressing "kick swap" while sitting on a hand-built four-on-the-floor
    // groove replaces only the kick hits, leaving the hats/snare/etc (and
    // the groove's feel) exactly as they were. lastSwapGenreIndex[]
    // remembers which genre was last used per slot: SwapDrumSlotRandom
    // never repeats the same one twice in a row, and SwapDrumSlotNext steps
    // forward deterministically from wherever random last left off (or from
    // the first genre pattern, the first time either is pressed).
    int lastSwapGenreIndex[kNumDrumTracks] = {-1, -1, -1, -1, -1, -1};

    // Shared by SwapDrumSlotFromGenre (below) and SwapDrumsGroupFromGenre
    // (the sixth revision's combined Snare+Clap+Perc swap pad) -- rewrites
    // one slot's steps WITHOUT pushing its own undo entry, so a group swap
    // covering multiple slots can wrap all of them in a single PushUndo()
    // and still revert cleanly in one Undo press.
    void RewriteDrumSlotFromGenreNoUndo(int slot, int genreIndex) {
        const auto& all = AllGenrePatterns();
        if (slot < 0 || slot >= kNumDrumTracks) return;
        if (genreIndex < 0 || genreIndex >= static_cast<int>(all.size())) return;
        const GenrePattern& gp = all[genreIndex];
        Pattern& pat = CurrentPattern();
        // v44.47: a swap never changes the scene length (only ApplyGenrePattern does): a long pattern shows as much of itself as
        // the scene holds.
        const int spb = ClampGrid(pat.stepsPerBeat);
        WriteGenreSlot(pat.tracks[static_cast<size_t>(slot)], gp, slot, spb, pat.masterLength, GenreBarsFit(gp, spb));
        // Deliberately NOT touching pat.swing here -- that's the whole
        // point of a per-slot swap over the full ApplyGenrePattern above.
        lastSwapGenreIndex[slot] = genreIndex;
    }

    void SwapDrumSlotFromGenre(int slot, int genreIndex) {
        if (slot < 0 || slot >= kNumDrumTracks) return;
        const auto& all = AllGenrePatterns();
        if (genreIndex < 0 || genreIndex >= static_cast<int>(all.size())) return;
        PushUndo();
        RewriteDrumSlotFromGenreNoUndo(slot, genreIndex);
        SyncMixer(CurrentPattern());
        SetStatus(std::string(DrumSlotName(static_cast<DrumSlot>(slot))) + " SWAPPED: " + all[genreIndex].name);
    }

    void SwapDrumSlotRandom(int slot) {
        if (slot < 0 || slot >= kNumDrumTracks) return;
        int n = kNumGenrePatterns;
        if (n <= 1) { SwapDrumSlotFromGenre(slot, 0); return; }
        std::uniform_int_distribution<int> pick(0, n - 1);
        int genreIndex = pick(RngEngine());
        if (genreIndex == lastSwapGenreIndex[slot]) genreIndex = (genreIndex + 1) % n; // never repeat
        SwapDrumSlotFromGenre(slot, genreIndex);
    }

    void SwapDrumSlotNext(int slot) {
        if (slot < 0 || slot >= kNumDrumTracks) return;
        int n = kNumGenrePatterns;
        int next = (lastSwapGenreIndex[slot] + 1 + n) % n; // -1 (never used) -> 0
        SwapDrumSlotFromGenre(slot, next);
    }

    // ---- Combined drum-group swap (sixth revision) --------------------------
    // ---- Mute groups (RANDOM tab DRUM MUTE / SYNTH MUTE rows) --------------
    // Drum voices mute in 3 fixed groups rather than individually, per
    // direct feedback: the RANDOM tab is meant to stay "light to touch"
    // mid-performance, and 6 separate per-voice mute chips was judged too
    // fiddly for that. Groups: KICK alone, DRUMS = snare+clap+perc, HATS =
    // closed+open hat. These reuse the SAME Track::mute flag the Sequencer
    // tab's B=mute and Mixer tab's A=mute already read/write (and that's
    // already saved/loaded by Persistence.cpp) -- no new persisted state,
    // just bulk-toggle logic over existing flags, and both the pattern
    // data AND the live Mixer copy are updated together (same two-copies
    // pattern every other mute toggle in this app already follows).
    static constexpr int kNumDrumMuteGroups = 3;
    static const std::array<std::vector<int>, kNumDrumMuteGroups>& DrumMuteGroups() {
        static const std::array<std::vector<int>, kNumDrumMuteGroups> groups = {{
            {static_cast<int>(DrumSlot::Kick)},
            {static_cast<int>(DrumSlot::Snare), static_cast<int>(DrumSlot::Clap), static_cast<int>(DrumSlot::Perc)},
            {static_cast<int>(DrumSlot::ClosedHat), static_cast<int>(DrumSlot::OpenHat)},
        }};
        return groups;
    }
    // A group reads as "muted" only when EVERY track in it is muted -- see
    // ToggleDrumMuteGroup for why (keeps a single A-press's direction
    // unambiguous even if the group's per-track mute states had drifted
    // apart via the Sequencer/Mixer tabs).
    // v44.5.1: mute and solo belong to the SCENE again. Each pattern keeps its own flags (they are saved with it); the audio
    // mixer mirrors the scene that is PLAYED, so a flag written to another scene is stored but not heard until that scene plays.
    // A scene change (launch, chain step, song mode on / off) loads the new scene's flags into the mixer.
    void SetMuteScene(int trackIndex, bool mute, int scene = -1) {
        if (trackIndex < 0 || trackIndex >= kNumTracks) return;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        if (scene < 0 || scene >= kNumPatterns) scene = project.currentPattern;
        if (const int b = BrickOfSlot(scene); b >= 0) { SetBrickTrackMuted(b, trackIndex, mute); return; }   // v44.38: a brick has one mute, WHO PLAYS
        const int grp = SceneGroupOf(scene);   // v44.58: linked scenes share mute and solo - the change is made in every scene of the group
        for (int sc = 0; sc < (grp > 0 ? kNumLiveScenes : 1); ++sc) {
            const int target = grp > 0 ? sc : scene;
            if (grp > 0 && project.sceneLink[static_cast<size_t>(sc)] != grp) continue;
            project.patterns[target].tracks[trackIndex].mute = mute;
            if (target == sequencer.PlayPatternIndex()) engine.GetMixer().tracks[trackIndex].mute = mute;
        }
    }
    void SetSoloScene(int trackIndex, bool solo, int scene = -1) {
        if (trackIndex < 0 || trackIndex >= kNumTracks) return;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        if (scene < 0 || scene >= kNumPatterns) scene = project.currentPattern;
        if (const int b = BrickOfSlot(scene); b >= 0) { SetBrickTrackSolo(b, trackIndex, solo); return; }   // v44.38: solo = every other instrument off, in WHO PLAYS
        const int grp = SceneGroupOf(scene);   // v44.58: see SetMuteScene
        for (int sc = 0; sc < (grp > 0 ? kNumLiveScenes : 1); ++sc) {
            const int target = grp > 0 ? sc : scene;
            if (grp > 0 && project.sceneLink[static_cast<size_t>(sc)] != grp) continue;
            project.patterns[target].tracks[trackIndex].solo = solo;
            if (target == sequencer.PlayPatternIndex()) engine.GetMixer().tracks[trackIndex].solo = solo;
        }
    }
    bool TrackIsMuted(int trackIndex) {
        if (const int b = BrickOfSlot(project.currentPattern); b >= 0) return BrickTrackMuted(b, trackIndex);   // v44.38
        return trackIndex >= 0 && trackIndex < kNumTracks && CurrentPattern().tracks[trackIndex].mute;
    }
    bool TrackIsSolo(int trackIndex) {
        if (const int b = BrickOfSlot(project.currentPattern); b >= 0) return BrickTrackSolo(b, trackIndex);
        return trackIndex >= 0 && trackIndex < kNumTracks && CurrentPattern().tracks[trackIndex].solo;
    }
    bool DrumMuteGroupIsMuted(int groupIdx) {
        const auto& groups = DrumMuteGroups();
        if (groupIdx < 0 || groupIdx >= kNumDrumMuteGroups) return false;
        for (int slot : groups[groupIdx]) {
            if (!TrackIsMuted(slot)) return false;   // v44.48: reads the brick mask on a brick, the scene flags otherwise
        }
        return true;
    }
    void ToggleDrumMuteGroup(int groupIdx) {
        const auto& groups = DrumMuteGroups();
        if (groupIdx < 0 || groupIdx >= kNumDrumMuteGroups) return;
        PushUndo();
        bool allMuted = DrumMuteGroupIsMuted(groupIdx);
        for (int slot : groups[groupIdx]) SetMuteScene(slot, !allMuted);
    }
    bool SynthIsMuted(int synthIdx) {
        if (synthIdx < 0 || synthIdx >= kNumSynthTracks) return false;
        return TrackIsMuted(kNumDrumTracks + synthIdx);
    }
    void ToggleSynthMute(int synthIdx) {
        if (synthIdx < 0 || synthIdx >= kNumSynthTracks) return;
        PushUndo();
        int ti = kNumDrumTracks + synthIdx;
        SetMuteScene(ti, !CurrentPattern().tracks[ti].mute);
    }

    // v44.3: the ALL DRUMS pad - kick, snare, clap, hats and perc muted together (reads as muted when every drum track is).
    bool AllDrumsMuted() {
        for (int i = 0; i < kNumDrumTracks; ++i) if (!TrackIsMuted(i)) return false;
        return true;
    }
    void ToggleAllDrumsMute() {
        PushUndo();
        const bool mute = !AllDrumsMuted();
        for (int i = 0; i < kNumDrumTracks; ++i) SetMuteScene(i, mute);
    }

    // v44.1: chord track mute and DROP (the CHORD pad left the LIVE grid in v44.3; the chord track still has its M button).
    bool ChordIsMuted() { return TrackIsMuted(kChordTrackIndex); }
    void ToggleChordMute() {
        PushUndo();
        SetMuteScene(kChordTrackIndex, !CurrentPattern().tracks[kChordTrackIndex].mute);
    }
    // DROP = everything except the kick and the bass is muted (a breakdown that keeps the low end moving). Press again
    // and every muted track goes back to what it was before the drop. Kick and bass are never touched by it.
    // v44.5.1: per scene - it mutes the EDITED scene only and each scene remembers its own "before the drop".
    std::array<bool, kNumPatterns> dropActiveScene{};
    std::array<std::array<bool, kNumTracks>, kNumPatterns> dropPrevMuteScene{};
    static bool IsDropKeeper(int trackIndex) { return trackIndex == static_cast<int>(DrumSlot::Kick) || trackIndex == kNumDrumTracks; }
    // Lit only while the drop really is in force: if the performer unmutes something by hand the flag drops out.
    bool DropIsActive() {
        const int sc = project.currentPattern;
        if (!dropActiveScene[static_cast<size_t>(sc)]) return false;
        for (int i = 0; i < kNumTracks; ++i) {
            // v44.48: lit only while the kick and the bass play and the rest is off (MUTE ALL after a DROP ends the drop)
            const bool bad = IsDropKeeper(i) ? TrackIsMuted(i) : !TrackIsMuted(i);
            if (bad) { dropActiveScene[static_cast<size_t>(sc)] = false; return false; }
        }
        return true;
    }
    void ToggleDrop() {
        PushUndo();
        const int sc = project.currentPattern;
        auto& prev = dropPrevMuteScene[static_cast<size_t>(sc)];
        if (!DropIsActive()) {
            for (int i = 0; i < kNumTracks; ++i) prev[static_cast<size_t>(i)] = CurrentPattern().tracks[i].mute;
            for (int i = 0; i < kNumTracks; ++i) if (!IsDropKeeper(i)) SetMuteScene(i, true);
            dropActiveScene[static_cast<size_t>(sc)] = true;
            SetStatus("DROP: ONLY KICK + BASS (THIS SCENE)");
        } else {
            for (int i = 0; i < kNumTracks; ++i) if (!IsDropKeeper(i)) SetMuteScene(i, prev[static_cast<size_t>(i)]);
            dropActiveScene[static_cast<size_t>(sc)] = false;
            SetStatus("DROP OFF: TRACKS BACK");
        }
    }
    // ---- v44.48: ONE set of instrument switches for the LIVE panel and for WHO PLAYS ----------------------------------------------
    // `brick` >= 0 works on the mask of that brick of the track (mute = a bit, solo = every other instrument off); -1 works on the
    // flags of the edited scene (the LIVE jam). The bars, the S buttons and the group buttons of both screens go through these, so
    // they cannot disagree with each other or with the M and S of the sequencer (which use SetMuteScene / SetSoloScene).
    bool SwMuted(int brick, int t) const {
        if (t < 0 || t >= kNumTracks) return false;
        return brick >= 0 ? BrickTrackMuted(brick, t) : project.patterns[project.currentPattern].tracks[static_cast<size_t>(t)].mute;
    }
    bool SwSolo(int brick, int t) const {
        if (t < 0 || t >= kNumTracks) return false;
        return brick >= 0 ? BrickTrackSolo(brick, t) : project.patterns[project.currentPattern].tracks[static_cast<size_t>(t)].solo;
    }
    bool SwAnySolo(int brick) const { for (int t = 0; t < kNumTracks; ++t) if (SwSolo(brick, t)) return true; return false; }
    // off because somebody else is soloed (as opposed to muted by hand)
    bool SwSilenced(int brick, int t) const { return SwAnySolo(brick) && !SwSolo(brick, t); }
    uint16_t SwMask(int brick) const {
        if (brick >= 0) return brick < project.songChainLength ? project.songChain[static_cast<size_t>(brick)].muteMask : static_cast<uint16_t>(0);
        uint16_t m = 0;
        for (int t = 0; t < kNumTracks; ++t) if (project.patterns[project.currentPattern].tracks[static_cast<size_t>(t)].mute) m |= static_cast<uint16_t>(1u << t);
        return m;
    }
    void SwUndo(int brick) { if (brick >= 0) PushSoundUndo(); else PushUndo(); }
    void SwToggleMute(int brick, int t) {
        if (t < 0 || t >= kNumTracks) return;
        SwUndo(brick);
        if (brick >= 0) SetBrickTrackMuted(brick, t, !BrickTrackMuted(brick, t)); else SetMuteScene(t, !SwMuted(brick, t));
    }
    void SwToggleSolo(int brick, int t) {
        if (t < 0 || t >= kNumTracks) return;
        SwUndo(brick);
        if (brick >= 0) SetBrickTrackSolo(brick, t, !BrickTrackSolo(brick, t)); else SetSoloScene(t, !SwSolo(brick, t));
    }
    // A group of instruments (bits of the mask): muted when every one of them is; pressing it mutes them all, or brings them all back.
    bool SwGroupMuted(int brick, uint16_t bits) const { return bits != 0 && (SwMask(brick) & bits) == bits; }
    void SwToggleGroup(int brick, uint16_t bits) {
        SwUndo(brick);
        const bool mute = !SwGroupMuted(brick, bits);
        if (brick >= 0) {
            ChainStep& cs = project.songChain[static_cast<size_t>(brick)];
            cs.muteMask = static_cast<uint16_t>(mute ? (cs.muteMask | bits) : (cs.muteMask & ~bits));
            sequencer.RefreshArrangement();
        } else {
            for (int t = 0; t < kNumTracks; ++t) if ((bits >> t) & 1u) SetMuteScene(t, mute);
        }
    }
    bool SwDropOn(int brick) { return brick >= 0 ? SwMask(brick) == kMuteAllButKickBass : DropIsActive(); }
    void SwToggleDrop(int brick) {
        if (brick < 0) { ToggleDrop(); return; }
        PushSoundUndo();
        ChainStep& cs = project.songChain[static_cast<size_t>(brick)];
        if (cs.muteMask != kMuteAllButKickBass) { dropRestoreMask_ = cs.muteMask; dropRestoreBrick_ = brick; cs.muteMask = kMuteAllButKickBass; SetStatus("DROP: ONLY KICK + BASS (THIS BRICK)"); }
        else { cs.muteMask = dropRestoreBrick_ == brick ? dropRestoreMask_ : static_cast<uint16_t>(0); dropRestoreBrick_ = -1; SetStatus("DROP OFF: INSTRUMENTS BACK"); }
        sequencer.RefreshArrangement();
    }
    uint16_t dropRestoreMask_ = 0; int dropRestoreBrick_ = -1;
    // v44.51: DROP + CHORD, and any "only these play" group: `keep` is the mask of the instruments that stay on, the rest is muted.
    // Lit while the mutes are exactly that; pressing it again puts the mutes back as they were before.
    static constexpr uint16_t kKeepKickBassChord = static_cast<uint16_t>((1u << 0) | (1u << 6) | (1u << kChordTrackIndex));
    bool SwKeepOn(int brick, uint16_t keep) const { return !SwAnySolo(brick) && SwMask(brick) == static_cast<uint16_t>(kMuteAllTracks & ~keep); }
    void SwToggleKeep(int brick, uint16_t keep) {
        const uint16_t target = static_cast<uint16_t>(kMuteAllTracks & ~keep);
        if (brick >= 0) {
            PushSoundUndo();
            ChainStep& cs = project.songChain[static_cast<size_t>(brick)];
            if (cs.muteMask != target) { keepRestoreMask_ = cs.muteMask; keepRestoreBrick_ = brick; cs.muteMask = target; SetStatus("DROP + CHORD: ONLY KICK, BASS AND CHORDS (THIS BRICK)"); }
            else { cs.muteMask = keepRestoreBrick_ == brick ? keepRestoreMask_ : static_cast<uint16_t>(0); keepRestoreBrick_ = -1; SetStatus("DROP + CHORD OFF: INSTRUMENTS BACK"); }
            sequencer.RefreshArrangement();
            return;
        }
        PushUndo();
        const int sc = project.currentPattern;
        if (!SwKeepOn(-1, keep)) {
            for (int i = 0; i < kNumTracks; ++i) keepPrevMuteScene_[static_cast<size_t>(sc)][static_cast<size_t>(i)] = CurrentPattern().tracks[static_cast<size_t>(i)].mute;
            keepPrevValid_[static_cast<size_t>(sc)] = true;
            for (int i = 0; i < kNumTracks; ++i) { SetSoloScene(i, false); SetMuteScene(i, ((keep >> i) & 1u) == 0); }
            dropActiveScene[static_cast<size_t>(sc)] = false;
            SetStatus("DROP + CHORD: ONLY KICK, BASS AND CHORDS (THIS SCENE)");
        } else {
            for (int i = 0; i < kNumTracks; ++i) SetMuteScene(i, keepPrevValid_[static_cast<size_t>(sc)] ? keepPrevMuteScene_[static_cast<size_t>(sc)][static_cast<size_t>(i)] : false);
            keepPrevValid_[static_cast<size_t>(sc)] = false;
            SetStatus("DROP + CHORD OFF: INSTRUMENTS BACK");
        }
    }
    uint16_t keepRestoreMask_ = 0; int keepRestoreBrick_ = -1;
    std::array<std::array<bool, kNumTracks>, kNumPatterns> keepPrevMuteScene_{};
    std::array<bool, kNumPatterns> keepPrevValid_{};
    // ALL ON: every mute and every solo of the scene (or the brick) off in one press.
    void SwAllOn(int brick) {
        SwUndo(brick);
        if (brick >= 0) { project.songChain[static_cast<size_t>(brick)].muteMask = 0; sequencer.RefreshArrangement(); }
        else {
            for (int t = 0; t < kNumTracks; ++t) { SetMuteScene(t, false); SetSoloScene(t, false); }
            dropActiveScene[static_cast<size_t>(project.currentPattern)] = false;
        }
        SetStatus("ALL ON: NO MUTES, NO SOLO");
    }


    // ---- v44.58: LINKED scenes, EDIT mode and the armed scene -----------------------------------------------------------------------
    // LINK: scenes with the same project.sceneLink group number share mute and solo. A change in one is made in all of them
    // (SetMuteScene / SetSoloScene), and the sequencer carries the flags over when playback moves from one to another
    // (Sequencer::CarryLinkedFlags), so playing across linked scenes never changes what is muted. Unlinked scenes keep their own, as before.
    int SceneGroupOf(int s) const { return s >= 0 && s < kNumLiveScenes ? static_cast<int>(project.sceneLink[static_cast<size_t>(s)]) : 0; }
    bool ScenesLinked(int a, int b) const { if (a == b) return true; const int g = SceneGroupOf(a); return g > 0 && g == SceneGroupOf(b); }
    int LinkedCountOf(int s) const {
        const int g = SceneGroupOf(s); if (g == 0) return 1;
        int n = 0; for (uint8_t v : project.sceneLink) if (v == g) ++n;
        return n;
    }
    void NormalizeLinks() {   // a group of one is not a group
        for (int g = 1; g < 256; ++g) { int n = 0; for (uint8_t v : project.sceneLink) if (v == g) ++n; if (n == 1) for (uint8_t& v : project.sceneLink) if (v == g) v = 0; }
    }
    // `scene` joins the group of `ref` (a new group when `ref` stands alone); it takes the mute and solo of `ref`, which are the ones heard
    void LinkScenes(int scene, int ref) {
        if (scene < 0 || ref < 0 || scene >= kNumLiveScenes || ref >= kNumLiveScenes || scene == ref) return;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        int g = SceneGroupOf(ref);
        if (g == 0) { std::array<bool, 256> used{}; for (uint8_t v : project.sceneLink) used[v] = true; for (g = 1; g < 255 && used[static_cast<size_t>(g)]; ++g) {} project.sceneLink[static_cast<size_t>(ref)] = static_cast<uint8_t>(g); }
        project.sceneLink[static_cast<size_t>(scene)] = static_cast<uint8_t>(g);
        for (int t = 0; t < kNumTracks; ++t) {
            Track& d = project.patterns[static_cast<size_t>(scene)].tracks[static_cast<size_t>(t)];
            const Track& r = project.patterns[static_cast<size_t>(ref)].tracks[static_cast<size_t>(t)];
            d.mute = r.mute; d.solo = r.solo;
            if (scene == sequencer.PlayPatternIndex()) { engine.GetMixer().tracks[static_cast<size_t>(t)].mute = r.mute; engine.GetMixer().tracks[static_cast<size_t>(t)].solo = r.solo; }
        }
        NormalizeLinks();
    }
    void UnlinkScene(int scene) {
        if (scene < 0 || scene >= kNumLiveScenes) return;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        project.sceneLink[static_cast<size_t>(scene)] = 0;
        NormalizeLinks();
    }
    // v44.59 (DATA-6): every LINK change is one undo step that also brings back the mute / solo it overwrote.
    // The right-click menu of a pad: join / leave the group of the open scene. On the open scene itself: leave.
    void ToggleSceneLink(int scene) {
        const int ref = project.currentPattern;
        if (scene < 0 || scene >= kNumLiveScenes || ref >= kNumLiveScenes) return;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        PushLinkUndo();
        if (scene == ref || ScenesLinked(scene, ref)) {
            UnlinkScene(scene);
            SetStatus("SCENE " + std::to_string(scene + 1) + " IS NOT LINKED ANY MORE - CMD+Z UNDOES");
        } else {
            LinkScenes(scene, ref);
            SetStatus("SCENES " + std::to_string(ref + 1) + " + " + std::to_string(scene + 1) + " LINKED: SAME MUTE AND SOLO - CMD+Z UNDOES");
        }
    }
    // Leave the group, as one undo step (the LINK button's UNLINK, the menu's "Leave its link group").
    void UnlinkSceneUndoable(int scene) {
        if (SceneGroupOf(scene) == 0) return;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        PushLinkUndo();
        UnlinkScene(scene);
        SetStatus("SCENE " + std::to_string(scene + 1) + " IS NOT LINKED ANY MORE - CMD+Z UNDOES");
    }
    // The pads selected with Shift+click (range) / Cmd+click (one by one); bit i = LIVE scene i. The LINK button acts on them.
    uint16_t sceneSel = 0;
    int sceneSelAnchor = -1;
    int SceneSelCount() const { int n = 0; for (int i = 0; i < kNumLiveScenes; ++i) if ((sceneSel >> i) & 1u) ++n; return n; }
    // "01-03" for a run, "01+05+07" otherwise
    std::string SceneSelLabel() const {
        int first = -1, last = -1; bool run = true;
        for (int i = 0; i < kNumLiveScenes; ++i) if ((sceneSel >> i) & 1u) { if (first < 0) first = i; else if (i != last + 1) run = false; last = i; }
        if (first < 0) return "";
        char b[96];
        if (run) { std::snprintf(b, sizeof(b), "%02d-%02d", first + 1, last + 1); return b; }
        std::string r;
        for (int i = 0; i < kNumLiveScenes; ++i) if ((sceneSel >> i) & 1u) { std::snprintf(b, sizeof(b), "%s%02d", r.empty() ? "" : "+", i + 1); r += b; }
        return r;
    }
    // Shift+click: the range from the anchor (else the open scene) to `i`; Cmd+click: add / remove one; a plain click: nothing selected.
    void SelectScenePad(int i, bool range, bool toggle) {
        if (i < 0 || i >= kNumLiveScenes) return;
        if (range) {
            int a = sceneSelAnchor >= 0 ? sceneSelAnchor : std::clamp(project.currentPattern, 0, kNumLiveScenes - 1);
            if (a >= kNumLiveScenes) a = i;
            sceneSel = 0;
            for (int k = std::min(a, i); k <= std::max(a, i); ++k) sceneSel |= static_cast<uint16_t>(1u << k);
            sceneSelAnchor = a;
        } else if (toggle) {
            if (sceneSel == 0 && project.currentPattern < kNumLiveScenes && project.currentPattern != i) sceneSel |= static_cast<uint16_t>(1u << project.currentPattern);
            sceneSel ^= static_cast<uint16_t>(1u << i);
            sceneSelAnchor = i;
        } else { sceneSel = 0; sceneSelAnchor = i; }
    }
    // The LINK button with two or more pads selected: they become one group (with the group of the open scene when it is among them,
    // else of the first). The mute / solo come from the open scene when it is selected (the one you hear), else from the first selected.
    // One undo step. Returns how many scenes are in the group.
    int LinkSelectedScenes() {
        if (SceneSelCount() < 2) return 0;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        int ref = -1;
        if (project.currentPattern < kNumLiveScenes && ((sceneSel >> project.currentPattern) & 1u)) ref = project.currentPattern;
        else for (int i = 0; i < kNumLiveScenes; ++i) if ((sceneSel >> i) & 1u) { ref = i; break; }
        PushLinkUndo();
        for (int i = 0; i < kNumLiveScenes; ++i) if (i != ref && ((sceneSel >> i) & 1u)) LinkScenes(i, ref);
        const std::string what = SceneSelLabel();
        sceneSel = 0;
        SetStatus("LINKED " + what + " (MUTE / SOLO FROM " + (ref < 9 ? "0" : "") + std::to_string(ref + 1) + ") - CMD+Z UNDOES");
        return LinkedCountOf(ref);
    }
    // "01+02+03": the group a scene is in, for the display ("" when it stands alone)
    std::string LinkLabel(int scene) const {
        const int g = SceneGroupOf(scene); if (g == 0) return "";
        std::string r;
        for (int i = 0; i < kNumLiveScenes; ++i) if (project.sceneLink[static_cast<size_t>(i)] == g) { char b[8]; std::snprintf(b, sizeof(b), "%s%02d", r.empty() ? "" : "+", i + 1); r += b; }
        return r;
    }
    // CLICK: LAUNCH | OPEN (v44.59, replaces the EDIT and LINK modes of v44.58). OPEN: on LIVE, a click on a pad only opens the scene in the
    // editor; the player keeps playing the scene it plays. GO launches the open scene (quantized by LAUNCH, like any launch). A double click on
    // a pad always opens (OpenScenePad). Stopped, or in song mode, there is nothing to hold apart. Remembered in ui_settings.cfg (clickopen=).
    bool clickOpens = false;
    bool SceneIsHeldApart() const { return sequencer.IsPlaying() && !sequencer.IsSongModeActive() && sequencer.PlayPatternIndex() != project.currentPattern && project.currentPattern < kNumLiveScenes; }
    void SelectSceneForEdit(int scene) {
        if (scene < 0 || scene >= kNumLiveScenes) return;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        if (!sequencer.IsPlaying() || sequencer.IsSongModeActive()) { sequencer.LaunchScene(scene); return; }
        sequencer.PinPlayToPlaying();
        project.currentPattern = scene;
        if (sequencer.PlayPatternIndex() == scene) sequencer.ReleasePin();   // the playing scene itself: the editor and the player are one again
        if (EditIsPlaying()) { engine.GetMixer().SyncFromPattern(CurrentPattern()); sequencer.ApplySceneSound(project.currentPattern); }
    }
    void GoToEditedScene() { sequencer.LaunchScene(project.currentPattern); }
    // A double click on a pad: open it without launching. With LAUNCH on BEAT / BAR the first click only queued the launch: it is cancelled.
    // (With LAUNCH = NOW the first click has already started the scene; CLICK: OPEN is the way to prepare a scene silently.)
    void OpenScenePad(int scene) {
        if (scene < 0 || scene >= kNumLiveScenes) return;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        if (sequencer.PendingScene() == scene) sequencer.ClearPendingScene();
        SelectSceneForEdit(scene);
    }
    // "Duplicate as new scene" (pad menu): copies scene `src` into the free slot `dst` and opens the copy in the editor WITHOUT launching it.
    // The music keeps playing what it plays; GO (or a click on the pad) launches the copy on the grid as usual. Before, the copy took over
    // playback at once, off the grid. Stopped or in song mode only the edited scene changes, as before. One undo step empties the slot again.
    void DuplicateSceneAsNew(int src, int dst) {
        if (src < 0 || src >= kNumLiveScenes || dst < 0 || dst >= kNumLiveScenes || src == dst) return;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        PushUndoForScene(dst);
        project.patterns[dst] = project.patterns[src];
        if (sequencer.IsPlaying() && !sequencer.IsSongModeActive()) { SelectSceneForEdit(dst); return; }
        project.currentPattern = dst;
        if (EditIsPlaying()) { engine.GetMixer().SyncFromPattern(CurrentPattern()); sequencer.ApplySceneSound(dst); }
    }
    // What one frame of mouse input on a SCENES pad means (ChainPanel passes ImGui's facts; the selftest replays real click sequences).
    // released = the pad's Button returned true (it fires on EVERY release, also the one that ends a double click);
    // doubleClicked = the second press of a double click landed on the pad; clickCount = io.MouseClickedLastCount[0] (2 on that second
    // release). A double click is: release 1 -> Click, press 2 -> DoubleClick, release 2 -> None (the release that ends a double click
    // must not launch again, or LAUNCH = BEAT / BAR would queue the scene the double click just cancelled).
    enum class PadGesture { None, Click, DoubleClick, SelectRange, SelectAdd };
    static PadGesture ScenePadGesture(bool released, bool doubleClicked, int clickCount, bool shift, bool cmd) {
        if (shift || cmd) return (released && clickCount < 2) ? (shift ? PadGesture::SelectRange : PadGesture::SelectAdd) : PadGesture::None;
        if (doubleClicked) return PadGesture::DoubleClick;
        if (released && clickCount < 2) return PadGesture::Click;
        return PadGesture::None;
    }
    // A gesture on pad `scene` outside song mode (song mode: ChainPanel keeps its own LaunchOrSwitchScene / SwitchPattern).
    void ScenePadInput(int scene, PadGesture g) {
        switch (g) {
            case PadGesture::SelectRange: SelectScenePad(scene, true, false); break;
            case PadGesture::SelectAdd:   SelectScenePad(scene, false, true); break;
            case PadGesture::Click:
                SelectScenePad(scene, false, false);
                if (clickOpens) SelectSceneForEdit(scene); else LaunchScenePad(scene);
                break;
            case PadGesture::DoubleClick: OpenScenePad(scene); break;
            case PadGesture::None: break;
        }
    }
    // A launch click while a scene is held apart: the editor first goes back to the scene that plays (the player is not moved), then the
    // clicked scene is launched as usual.
    void LaunchScenePad(int scene) {
        if (scene < 0 || scene >= kNumLiveScenes) return;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        if (SceneIsHeldApart()) {
            project.currentPattern = sequencer.PlayPatternIndex();
            sequencer.ReleasePin();
        }
        sequencer.LaunchScene(scene);
    }

    // The scene a click on the LIVE switches means. While a launch waits for its boundary it is the scene that is coming, so a kick muted now
    // is muted from the first beat of that scene (and the playing scene is not touched); otherwise the edited scene. -1: the edited scene.
    int ArmedScene() const {
        const int pend = sequencer.PendingScene();
        if (pend < 0 || pend >= kNumLiveScenes || !sequencer.IsPlaying() || sequencer.IsSongModeActive()) return -1;
        if (project.currentPattern >= kNumLiveScenes || pend == project.currentPattern) return -1;
        return pend;
    }
    // While alive, "the edited scene" is the armed scene and the player stays where it is: the LIVE switches then read and write that scene.
    struct ArmScope {
        AppState& a; std::unique_lock<SceneMutex> lock; bool on = false; int cur = 0, pin = -1;
        ArmScope(AppState& app, int scene) : a(app), lock(app.sequencer.PatternMutex()) {
            if (scene >= 0) { on = true; cur = a.project.currentPattern; pin = a.sequencer.PinRaw(); a.sequencer.SetPinRaw(a.sequencer.PlayPatternIndex()); a.project.currentPattern = scene; }
        }
        ~ArmScope() { if (on) { a.project.currentPattern = cur; a.sequencer.SetPinRaw(pin); } }
        ArmScope(const ArmScope&) = delete; ArmScope& operator=(const ArmScope&) = delete;
    };

    // ---- v44.57: PITCH - a turntable pitch fader for the whole output -----------------------------------------------------------------
    // Plays everything (the sequencer, synths, loops, effect tails) a little faster or slower, tempo and pitch together, like a record on
    // a Technics deck - so the project can be played along with vinyl. A stage after the engine (core/VariSpeed.h); exports and the
    // project file are not touched (the fader is not saved: like a deck, it starts at 0). Range +-8% or +-16%, step 0.1%.
    // `pitchMode` (key P): the arrow keys drive it - up / down faster / slower (Shift = 1%), left / right a push (pitch bend, held), Return = 0.
    VariSpeed varispeed;                       // audio thread only
    std::atomic<float> pitchPct{0.0f};         // percent: + faster and higher
    std::atomic<int> pitchBend{0};             // -1 / 0 / +1: a push while a key is held
    bool pitchRange16 = false;                 // false: +-8%, true: +-16%  (saved in ui_settings.cfg)
    bool pitchMode = false;
    static constexpr float kPitchBendPct = 4.0f;
    float PitchLimit() const { return pitchRange16 ? 16.0f : 8.0f; }
    // the speed the audio callback aims at (1.0 = as rendered)
    double PitchTargetSpeed() const {
        const double base = 1.0 + static_cast<double>(pitchPct.load(std::memory_order_relaxed)) / 100.0;
        return base * (1.0 + kPitchBendPct / 100.0 * pitchBend.load(std::memory_order_relaxed));
    }
    void PitchSet(float pct) {
        const float lim = PitchLimit();
        pct = std::round(std::clamp(pct, -lim, lim) * 10.0f) / 10.0f;
        pitchPct.store(std::fabs(pct) < 0.05f ? 0.0f : pct, std::memory_order_relaxed);
    }
    void PitchStep(float deltaPct) { PitchSet(pitchPct.load(std::memory_order_relaxed) + deltaPct); }
    void PitchSetRange16(bool wide) { pitchRange16 = wide; PitchSet(pitchPct.load(std::memory_order_relaxed)); }   // a narrower range pulls the fader in
    // what the tempo is now, with the fader (for the display)
    double PitchedBpm(double bpm) const { return bpm * (1.0 + static_cast<double>(pitchPct.load(std::memory_order_relaxed)) / 100.0); }

    // ---- v44.54: NEXT BAR - the LIVE switches queue like a fill ---------------------------------------------------------------------
    // With `quantizeSwitches` on (the NEXT BAR button of the LIVE panel) a click on a bar, an S or a group button does not act at once:
    // it waits and takes effect exactly on the next bar line (the same moment a FILL starts), applied by the sequencer's bar hook on
    // the audio thread. A second click on a waiting button cancels it. Stopped, or editing a scene other than the one that plays, a
    // click acts at once (there is no bar to wait for). Not queued: STUTTER, scenes (LAUNCH has its own quantize), WHO PLAYS.
    // The undo step is pushed at the click; while the queue is applied PushUndo / PushSoundUndo / SetStatus do nothing (they are not
    // audio-thread safe) - see SwitchApplying().
    bool quantizeSwitches = false;
    struct QueuedSwitch { enum Kind : uint8_t { Mute, Solo, Group, Drop, Keep } kind; uint16_t arg; int scene; };
    std::vector<QueuedSwitch> queuedSwitches_;   // under the pattern lock; capacity reserved on the first use, so the audio thread never allocates
    bool switchHookSet_ = false;
    static bool& SwitchApplying() { static thread_local bool f = false; return f; }
    static constexpr size_t kMaxQueuedSwitches = 64;
    void DispatchSwitch(QueuedSwitch::Kind kind, uint16_t arg) {
        switch (kind) {
            case QueuedSwitch::Mute: SwToggleMute(-1, arg); break;
            case QueuedSwitch::Solo: SwToggleSolo(-1, arg); break;
            case QueuedSwitch::Group: SwToggleGroup(-1, arg); break;
            case QueuedSwitch::Drop: SwToggleDrop(-1); break;
            case QueuedSwitch::Keep: SwToggleKeep(-1, arg); break;
        }
    }
    bool SwitchesCanQueue() {
        return quantizeSwitches && sequencer.IsPlaying() && !sequencer.IsSongModeActive() &&
               BrickOfSlot(project.currentPattern) < 0 && project.currentPattern == sequencer.PlayPatternIndex();
    }
    // Is this switch waiting for the bar line? (for the pulsing outline)
    bool SwQueued(QueuedSwitch::Kind kind, uint16_t arg = 0) {
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        for (const auto& q : queuedSwitches_) if (q.kind == kind && q.arg == arg && q.scene == project.currentPattern) return true;
        return false;
    }
    int SwQueuedCount() {
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        int n = 0;
        for (const auto& q : queuedSwitches_) if (q.scene == project.currentPattern) ++n;
        return n;
    }
    // The one entry point of the LIVE panel (brick = -1) for mute / solo / group / drop / keep.
    void LiveSwitch(QueuedSwitch::Kind kind, uint16_t arg = 0) {
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        if (!SwitchesCanQueue()) { DispatchSwitch(kind, arg); return; }
        for (size_t i = 0; i < queuedSwitches_.size(); ++i) {
            const auto& q = queuedSwitches_[i];
            if (q.kind == kind && q.arg == arg && q.scene == project.currentPattern) {   // a second click cancels the waiting one
                queuedSwitches_.erase(queuedSwitches_.begin() + static_cast<std::ptrdiff_t>(i));
                if (queuedSwitches_.empty()) sequencer.ArmBarHook(false);
                SetStatus("NEXT BAR: CANCELLED");
                return;
            }
        }
        if (queuedSwitches_.size() >= kMaxQueuedSwitches) { SetStatus("NEXT BAR: QUEUE FULL"); return; }
        if (queuedSwitches_.capacity() < kMaxQueuedSwitches) queuedSwitches_.reserve(kMaxQueuedSwitches);
        PushUndo();
        queuedSwitches_.push_back(QueuedSwitch{kind, arg, project.currentPattern});
        if (!switchHookSet_) { sequencer.SetBarHook([this]() { ApplyQueuedSwitches(); }); switchHookSet_ = true; }
        sequencer.ArmBarHook(true);
        SetStatus("QUEUED - TAKES EFFECT ON THE NEXT BAR (CLICK AGAIN TO CANCEL)");
    }
    // Runs under the pattern lock: on the audio thread at the bar line (the bar hook), or on the UI thread when the transport stops.
    // Switches meant for a scene that is not the played one any more (a scene launch landed on the same bar line) are dropped.
    void ApplyQueuedSwitches() {
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        sequencer.ArmBarHook(false);
        bool& applying = SwitchApplying();
        const bool was = applying;
        applying = true;
        for (const auto& q : queuedSwitches_)
            if (q.scene == project.currentPattern && q.scene == sequencer.PlayPatternIndex()) DispatchSwitch(q.kind, q.arg);
        queuedSwitches_.clear();
        applying = was;
    }

    // the KIT button in a drum track's panel asks the bottom panel (which owns the browser window) to open it
    bool kitBrowserRequest = false;
    bool kitAuditionRequest = false;   // v44.2: NOT set by LoadKitFolder any more (picking a kit live must stay silent); a caller may set it to play the short kick / hat / snare phrase
    // Loads a drum kit folder into the engine and makes it the project's kit (the kit picker, the arrows and the browser all use this).
    int LoadKitFolder(const std::string& kitPath) {
        // v44.5.4: the kit belongs to the EDITED scene (a scene that never picked one follows the project kit)
        PushUndo();
        BumpRequest("kit:" + std::to_string(project.currentPattern) + ":" + std::to_string(std::clamp(seqCursorTrack, 0, kNumTracks - 1)));   // N1: a dropped file still being read for this drum is not used
        {   // v44.26: when the selected drum has a kit of its own, the picker changes that kit, not the kit of the scene
            Track& st = CurrentPattern().tracks[static_cast<size_t>(std::clamp(seqCursorTrack, 0, kNumTracks - 1))];
            if (st.type == TrackType::Drum && !st.kitFolder.empty()) st.kitFolder = kitPath; else CurrentPattern().kitFolder = kitPath;
        }
        SyncSceneKits();
        const auto& kits = KitFolders();
        for (size_t i = 0; i < kits.size(); ++i) if (kits[i] == kitPath) menuKitIndex = static_cast<int>(i);
        if (KitPending(kitPath)) {   // N1: read on the loader thread; the count is said when it arrives
            kitStatusFolder_ = kitPath;
            SetStatus("KIT " + kitPath + " (LOADING) - SCENE " + std::to_string(project.currentPattern + 1));
            return 0;
        }
        const int loaded = KitSlotsLoaded(KitBank(kitPath));
        SetStatus("KIT " + kitPath + " (" + std::to_string(loaded) + "/6 loaded) - SCENE " + std::to_string(project.currentPattern + 1));
        return loaded;
    }
    // "Use this kit in every scene": the project kit becomes `kitPath` and no scene keeps an own kit.
    void SetKitAllScenes(const std::string& kitPath) {
        for (int i = 0; i < kNumPatterns; ++i) if (SlotInUse(i)) PushUndoForScene(i);
        project.kitFolder = kitPath;
        for (Pattern& p : project.patterns) p.kitFolder.clear();
        SyncSceneKits();
        SetStatus("KIT " + kitPath + " IN ALL SCENES");
    }

    // ---- Sound of a scene (v44.5.4) -----------------------------------------------------------------------------------------------
    // A scene carries its own synth / chord patches, built-in kick and drum kit (Pattern::synthPatches, chordPatch, kick, kitFolder).
    // The engine holds the sound of the PLAYED scene (Sequencer::ApplySceneSound loads it at every scene change); the editor always
    // shows and edits the sound of the EDITED scene, and pushes it to the engine only when that scene is also the played one.
    SynthPatch& EditedPatch(bool chord, int synthIdx) { return chord ? CurrentPattern().chordPatch : CurrentPattern().synthPatches[static_cast<size_t>(std::clamp(synthIdx, 0, kNumSynthTracks - 1))]; }
    void PushEditedPatchToEngine(bool chord, int synthIdx) {
        if (!EditIsPlaying()) return;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        if (chord) engine.Chord().SetPatch(CurrentPattern().chordPatch);
        else engine.Synth(std::clamp(synthIdx, 0, kNumSynthTracks - 1)).SetPatch(CurrentPattern().synthPatches[static_cast<size_t>(std::clamp(synthIdx, 0, kNumSynthTracks - 1))]);
    }
    // The edited scene's kick settings; call PushEditedKick() after changing them.
    KickSynthSettings& EditedKick() { return CurrentPattern().kick; }
    void PushEditedKick() { if (EditIsPlaying()) engine.GetMixer().kickSynth = CurrentPattern().kick; }

    // Kits: banks are built on the UI thread (disk), cached by folder, and handed to the sequencer per scene.
    std::map<std::string, DrumKit::BankPtr> kitCache_;
    std::vector<DrumKit::BankPtr> kitRetired_;      // banks dropped from the cache stay alive a while, so the audio thread never frees one
    std::array<std::string, kNumPatterns> kitSyncedFolder_{};
    std::array<std::array<std::string, kNumDrumTracks>, kNumPatterns> kitSyncedTrack_{};   // v44.26: the own kits of the drums, as last handed to the sequencer
    std::map<std::string, DrumKit::BankPtr> kitMixed_;                                    // v44.26: banks put together from a scene kit and drums with a kit of their own
    std::array<bool, kNumPatterns> kitSyncedInUse_{};   // v44.33: which slots had a brick pointing at them when the banks were last handed over
    std::string kitSyncedDefault_, kitSyncedDir_;
    bool kitsDirty_ = true;
    const std::string& SceneKitFolder(int scene) const {
        scene = std::clamp(scene, 0, kNumPatterns - 1);
        const std::string& f = project.patterns[static_cast<size_t>(scene)].kitFolder;
        return f.empty() ? project.kitFolder : f;
    }
    // v44.26: the kit a drum instrument plays from: its own folder when it has one, else the kit of the scene.
    const std::string& TrackKitFolder(int scene, int track) const {
        scene = std::clamp(scene, 0, kNumPatterns - 1);
        if (track >= 0 && track < kNumDrumTracks) { const std::string& f = project.patterns[static_cast<size_t>(scene)].tracks[static_cast<size_t>(track)].kitFolder; if (!f.empty()) return f; }
        return SceneKitFolder(scene);
    }
    // N1 (LIVE-2): with the loader running, a kit that is not cached yet is read on the loader thread; meanwhile this returns null
    // and KitPending() is true. When it arrives the kits are synced again (kitsDirty_).
    std::set<std::string> kitPending_;
    std::string kitStatusFolder_;   // LoadKitFolder waits for this one to say how many samples it has
    // C4b (e): where a kit folder is on disk. A project kit ("samples/kits/x") is beside the project file, so the same name means
    // another folder in another project: the caches below are keyed by this path, never by the name the project stores.
    std::string KitDiskPath(const std::string& folder) const { return IsProjectKit(folder) ? DirNameOf(projectPath) + "/" + folder : folder; }
    DrumKit::BankPtr KitBank(const std::string& folderName) {
        const std::string folder = KitDiskPath(folderName);
        auto it = kitCache_.find(folder);
        if (it != kitCache_.end()) return it->second;
        if (kitPending_.count(folder)) return nullptr;
        if (!loader.Running()) {
            DrumKit::BankPtr b = engine.GetKit()->Build(folder, engine.SampleRate());
            kitCache_[folder] = b;
            return b;
        }
        kitPending_.insert(folder);
        auto out = std::make_shared<DrumKit::BankPtr>();
        const DrumKit* kit = engine.GetKit();   // Build() is const and only reads files
        const int rate = engine.SampleRate();
        loader.Post([out, kit, folder, rate] { *out = kit->Build(folder, rate); }, [this, out, folder, folderName] {
            kitPending_.erase(folder);
            kitCache_[folder] = *out;
            kitsDirty_ = true;
            if (folderName == kitStatusFolder_) {
                kitStatusFolder_.clear();
                SetStatus("KIT " + folderName + " (" + std::to_string(KitSlotsLoaded(*out)) + "/6 loaded) - SCENE " + std::to_string(project.currentPattern + 1));
            }
        });
        return nullptr;
    }
    bool KitPending(const std::string& folder) const { return kitPending_.count(KitDiskPath(folder)) != 0; }
    static int KitSlotsLoaded(const DrumKit::BankPtr& b) {
        int n = 0;
        if (b) for (const auto& v : b->variants) if (!v.empty()) ++n;
        return n;
    }
    int LoadedKitSlots() { return KitSlotsLoaded(engine.GetKit()->CurrentBank()); }
    // How many sample variants a drum slot has in the kit of `scene` (the engine's kit is the played scene's, not always this one).
    int SceneKitVariantCount(int scene, int slot) {
        if (slot < 0 || slot >= kNumDrumTracks) return 0;
        DrumKit::BankPtr b = KitBank(TrackKitFolder(scene, slot));
        return b ? static_cast<int>(b->variants[static_cast<size_t>(slot)].size()) : 0;
    }
    // v44.52: the sample a drum slot plays in the kit of `scene` (for the waveform card); null when there is none.
    std::shared_ptr<const SampleBuffer> SceneKitVariantBuffer(int scene, int slot, int variant) {
        if (slot < 0 || slot >= kNumDrumTracks) return nullptr;
        DrumKit::BankPtr b = KitBank(TrackKitFolder(scene, slot));
        if (!b) return nullptr;
        const auto& v = b->variants[static_cast<size_t>(slot)];
        return variant >= 0 && variant < static_cast<int>(v.size()) ? v[static_cast<size_t>(variant)] : nullptr;
    }
    // A kit folder whose files changed on disk (a dropped sample): forget its bank so the next sync reads it again.
    void InvalidateKit(const std::string& folderName) {
        auto it = kitCache_.find(KitDiskPath(folderName));
        if (it != kitCache_.end()) { kitRetired_.push_back(it->second); kitCache_.erase(it); }
        kitsDirty_ = true;
    }
    // Cheap per-frame check: when any scene's kit (or the project kit) changed, builds the missing banks and hands the set to the
    // sequencer, which swaps the kit of the played scene at once.
    void SyncSceneKits(bool force = false) {
        SyncSceneSamples();   // v44.50: the sampler's samples follow the same cheap per-frame check
        auto trackKitsChanged = [&](int i) {
            for (int t = 0; t < kNumDrumTracks; ++t) if (kitSyncedTrack_[static_cast<size_t>(i)][static_cast<size_t>(t)] != project.patterns[static_cast<size_t>(i)].tracks[static_cast<size_t>(t)].kitFolder) return true;
            return false;
        };
        bool changed = force || kitsDirty_ || kitSyncedDefault_ != project.kitFolder || kitSyncedDir_ != DirNameOf(projectPath);   // C4b (e): project kits are read beside the project file
        for (int i = 0; i < kNumPatterns && !changed; ++i) if (kitSyncedFolder_[static_cast<size_t>(i)] != project.patterns[static_cast<size_t>(i)].kitFolder || trackKitsChanged(i) || kitSyncedInUse_[static_cast<size_t>(i)] != SlotInUse(i)) changed = true;
        if (!changed) return;
        {   // N1: every bank this needs must be in the cache first; while one is still being read, the sequencer keeps what it has
            bool waiting = false;
            auto need = [&](const std::string& f) { if (!KitBank(f) && KitPending(f)) waiting = true; };
            need(project.kitFolder);
            for (int i = 0; i < kNumPatterns; ++i) {
                if (!SlotInUse(i)) continue;
                const Pattern& pat = project.patterns[static_cast<size_t>(i)];
                if (!pat.kitFolder.empty()) need(pat.kitFolder);
                for (int t = 0; t < kNumDrumTracks; ++t) if (!pat.tracks[static_cast<size_t>(t)].kitFolder.empty()) need(pat.tracks[static_cast<size_t>(t)].kitFolder);
            }
            if (waiting) { kitsDirty_ = true; return; }
        }
        std::array<DrumKit::BankPtr, kNumPatterns> banks{};
        DrumKit::BankPtr def = KitBank(project.kitFolder);
        std::map<std::string, DrumKit::BankPtr> mixedNow;
        for (int i = 0; i < kNumPatterns; ++i) {
            const Pattern& pat = project.patterns[static_cast<size_t>(i)];
            const std::string& own = pat.kitFolder;
            bool anyTrackKit = false;
            for (int t = 0; t < kNumDrumTracks; ++t) { kitSyncedTrack_[static_cast<size_t>(i)][static_cast<size_t>(t)] = pat.tracks[static_cast<size_t>(t)].kitFolder; if (!pat.tracks[static_cast<size_t>(t)].kitFolder.empty()) anyTrackKit = true; }
            kitSyncedFolder_[static_cast<size_t>(i)] = own;
            kitSyncedInUse_[static_cast<size_t>(i)] = SlotInUse(i);   // v44.33: a brick thrown from a scene with its own kit was never given that kit (its slot was not in the chain yet when the kit was synced)
            if (!SlotInUse(i)) continue;
            if (!anyTrackKit) { if (!own.empty()) banks[static_cast<size_t>(i)] = KitBank(own); continue; }
            // v44.26: some drums of the scene have a kit of their own: a bank put together from the kit of the scene and those drums
            std::string key = own.empty() ? project.kitFolder : own;
            for (int t = 0; t < kNumDrumTracks; ++t) key += "|" + pat.tracks[static_cast<size_t>(t)].kitFolder;
            auto it = kitMixed_.find(key);
            if (it == kitMixed_.end() || force || kitsDirty_) {   // a dirty kit may have changed on disk: put it together again
                DrumKit::BankPtr base = KitBank(own.empty() ? project.kitFolder : own);
                auto nb = std::make_shared<DrumKit::Bank>(base ? *base : DrumKit::Bank{});
                for (int t = 0; t < kNumDrumTracks; ++t) {
                    const std::string& tf = pat.tracks[static_cast<size_t>(t)].kitFolder;
                    if (tf.empty()) continue;
                    DrumKit::BankPtr tb = KitBank(tf);
                    if (tb && !tb->variants[static_cast<size_t>(t)].empty()) { nb->variants[static_cast<size_t>(t)] = tb->variants[static_cast<size_t>(t)]; nb->names[static_cast<size_t>(t)] = tb->names[static_cast<size_t>(t)]; }
                }
                if (it != kitMixed_.end()) kitRetired_.push_back(it->second);
                kitMixed_[key] = nb;
                it = kitMixed_.find(key);
            }
            mixedNow[key] = it->second;
            banks[static_cast<size_t>(i)] = it->second;
        }
        for (auto it = kitMixed_.begin(); it != kitMixed_.end();) { if (mixedNow.count(it->first)) ++it; else { kitRetired_.push_back(it->second); it = kitMixed_.erase(it); } }
        kitSyncedDefault_ = project.kitFolder;
        kitSyncedDir_ = DirNameOf(projectPath);
        kitsDirty_ = false;
        sequencer.SetSceneBanks(banks, def);
        // forget banks no scene uses any more (kept alive in kitRetired_ for a while)
        for (auto it = kitCache_.begin(); it != kitCache_.end();) {
            bool used = it->first == KitDiskPath(project.kitFolder);
            for (const Pattern& p : project.patterns) { if (!p.kitFolder.empty() && KitDiskPath(p.kitFolder) == it->first) used = true; for (int t = 0; t < kNumDrumTracks; ++t) if (!p.tracks[static_cast<size_t>(t)].kitFolder.empty() && KitDiskPath(p.tracks[static_cast<size_t>(t)].kitFolder) == it->first) used = true; }
            if (used) ++it; else { kitRetired_.push_back(it->second); it = kitCache_.erase(it); }
        }
        while (kitRetired_.size() > 6) kitRetired_.erase(kitRetired_.begin());
    }
    // After a project is loaded / created / restored: the played scene's patches, kick and kit go to the engine.
    // ---- SAMPLER (v44.50) -------------------------------------------------------------------------------------------------------
    // The 11th instrument plays one sample per scene (Pattern::samplerPatch.file = a name inside <project folder>/samples/). Files are
    // read on the UI thread into immutable SampleData buffers, kept in a cache for the whole session (so the audio thread never frees
    // one) and handed to the sequencer per scene, like the kit banks. A WAV dropped on the window with the sampler selected is COPIED
    // into samples/ (the original is never touched); SAVE AS copies the samples the project uses beside the new file.
    std::string SamplesDir() const { return DirNameOf(projectPath) + "/samples"; }
    std::map<std::string, SamplePtr> sampleCache_;
    std::array<std::string, kNumPatterns> sampleSyncedFile_{};
    std::array<std::array<std::string, kNumSynthTracks + 1>, kNumPatterns> synthSampleSyncedFile_{};   // v44.53: the SAMPLE engines of the synths (0..2) and the chord (3), as last handed over
    std::string sampleSyncedDir_;
    std::vector<std::string> fillSampleSynced_;   // C4b: the SAMPLE fill files as last handed over
    bool samplesDirty_ = true;
    std::vector<std::string> sampleNamesCache_;
    bool sampleNamesValid_ = false;
    static constexpr double kMaxSampleSeconds = 180.0;
    // The sample `file` of the open project's samples/ folder, loaded once (nullptr = missing or unreadable, remembered until a sync is forced).
    // N1 (LIVE-2): with the loader running, a file that is not cached yet is read on the loader thread (null meanwhile, SamplePending()).
    std::set<std::string> samplePending_;
    static SamplePtr ReadSampleFile(const std::string& path, const std::string& name, int rate) {   // any thread
        SampleBuffer buf;
        if (!(LoadWav(path, buf, rate) && !buf.empty() && buf.frames() >= 2 && buf.frames() <= kMaxSampleSeconds * rate)) return nullptr;
        auto sd = std::make_shared<SampleData>();
        sd->data = std::move(buf.data); sd->channels = std::clamp(buf.channels, 1, 2); sd->sampleRate = rate; sd->name = name;
        return sd;
    }
    SamplePtr SampleFor(const std::string& file) {
        if (file.empty()) return nullptr;
        const std::string path = SamplesDir() + "/" + file;
        auto it = sampleCache_.find(path);
        if (it != sampleCache_.end()) return it->second;
        if (samplePending_.count(path)) return nullptr;
        if (!loader.Running()) {
            SamplePtr result = ReadSampleFile(path, file, engine.SampleRate());
            sampleCache_[path] = result;
            return result;
        }
        samplePending_.insert(path);
        auto out = std::make_shared<SamplePtr>();
        const int rate = engine.SampleRate();
        loader.Post([out, path, file, rate] { *out = ReadSampleFile(path, file, rate); }, [this, out, path] {
            samplePending_.erase(path);
            sampleCache_[path] = *out;
            samplesDirty_ = true;
        });
        return nullptr;
    }
    bool SamplePending(const std::string& file) const { return !file.empty() && samplePending_.count(SamplesDir() + "/" + file) != 0; }
    bool SampleMissing(const std::string& file) { return !file.empty() && SampleFor(file) == nullptr && !SamplePending(file); }
    // Hands the samples of every scene in use to the sequencer when a file name (or the folder) changed. Cheap: names are compared.
    void SyncSceneSamples(bool force = false) {
        if (force) transDirty_ = true;
        SyncUserTransitions();   // v44.51
        const std::string dir = SamplesDir();
        bool changed = force || samplesDirty_ || dir != sampleSyncedDir_;
        if (!changed) for (int i = 0; i < kNumPatterns && !changed; ++i) {
            const std::string& f = project.patterns[static_cast<size_t>(i)].samplerPatch.file;
            if ((SlotInUse(i) ? f : std::string()) != sampleSyncedFile_[static_cast<size_t>(i)]) changed = true;
            for (int k = 0; k <= kNumSynthTracks && !changed; ++k)
                if (SynthSampleWanted(i, k) != synthSampleSyncedFile_[static_cast<size_t>(i)][static_cast<size_t>(k)]) changed = true;
        }
        const std::vector<std::string> fillWanted = FillSamplesWanted();   // C4b
        if (fillWanted != fillSampleSynced_) changed = true;
        if (!changed) return;
        if (force || !loader.Running())   // retry what was missing (N1: with the loader only on a forced sync, or a missing file would be read again every frame)
            for (auto it = sampleCache_.begin(); it != sampleCache_.end();) it = it->second ? std::next(it) : sampleCache_.erase(it);
        {   // N1: every file this needs must be in the cache first; while one is still being read, the sequencer keeps what it has
            bool waiting = false;
            for (int i = 0; i < kNumPatterns; ++i) {
                if (SlotInUse(i)) { const std::string& f = project.patterns[static_cast<size_t>(i)].samplerPatch.file; if (!SampleFor(f) && SamplePending(f)) waiting = true; }
                for (int k = 0; k <= kNumSynthTracks; ++k) { const std::string f = SynthSampleWanted(i, k); if (!SampleFor(f) && SamplePending(f)) waiting = true; }
            }
            for (const std::string& f : fillWanted) if (!SampleFor(f) && SamplePending(f)) waiting = true;
            if (waiting) { samplesDirty_ = true; return; }
        }
        std::array<SamplePtr, kNumPatterns> ptrs{};
        for (int i = 0; i < kNumPatterns; ++i) {
            sampleSyncedFile_[static_cast<size_t>(i)] = SlotInUse(i) ? project.patterns[static_cast<size_t>(i)].samplerPatch.file : std::string();
            if (!sampleSyncedFile_[static_cast<size_t>(i)].empty()) ptrs[static_cast<size_t>(i)] = SampleFor(sampleSyncedFile_[static_cast<size_t>(i)]);
        }
        std::array<std::array<SamplePtr, kNumSynthTracks + 1>, kNumPatterns> sptrs{};   // v44.53
        for (int i = 0; i < kNumPatterns; ++i) for (int k = 0; k <= kNumSynthTracks; ++k) {
            synthSampleSyncedFile_[static_cast<size_t>(i)][static_cast<size_t>(k)] = SynthSampleWanted(i, k);
            if (!synthSampleSyncedFile_[static_cast<size_t>(i)][static_cast<size_t>(k)].empty()) sptrs[static_cast<size_t>(i)][static_cast<size_t>(k)] = SampleFor(synthSampleSyncedFile_[static_cast<size_t>(i)][static_cast<size_t>(k)]);
        }
        sampleSyncedDir_ = dir; samplesDirty_ = false;
        sequencer.SetSceneSynthSamples(sptrs);
        sequencer.SetSceneSamples(ptrs);
        std::array<Sequencer::FillSampleEntry, Sequencer::kMaxFillSamples> fills{};   // C4b: the samples of the SAMPLE fills
        for (size_t j = 0; j < fillWanted.size() && j < fills.size(); ++j) {
            std::memcpy(fills[j].name, fillWanted[j].data(), std::min<size_t>(fillWanted[j].size(), FillRule::kSampleMax - 1));
            fills[j].sample = SampleFor(fillWanted[j]);
        }
        if (fillWanted.size() > static_cast<size_t>(Sequencer::kMaxFillSamples) && fillWanted != fillSampleSynced_)
            SetStatus("TOO MANY FILL SAMPLES: " + std::to_string(Sequencer::kMaxFillSamples) + " DIFFERENT FILES PLAY AT MOST");
        fillSampleSynced_ = fillWanted;
        sequencer.SetFillSamples(fills);
    }
    // C4b: the files the SAMPLE fills can play: the fill rule of every scene in use and the own fill rule of every chain step
    // (sorted, no repeats; at most Sequencer::kMaxFillSamples go to the sequencer, one more tells there are too many).
    std::vector<std::string> FillSamplesWanted() const {
        std::vector<std::string> v;
        auto add = [&v](const FillRule& r) { if (r.kind == FillKind::Sample && r.when != FillWhen::Off && r.sample[0]) v.push_back(r.Sample()); };
        for (int i = 0; i < kNumPatterns; ++i) if (SlotInUse(i)) add(project.patterns[static_cast<size_t>(i)].fill);
        const int n = std::clamp(project.songChainLength, 0, kMaxChainSteps);
        for (int i = 0; i < n; ++i) { const ChainStep& cs = project.songChain[static_cast<size_t>(i)]; if (cs.fillMode == FillMode::Own) add(cs.fill); }
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        if (v.size() > static_cast<size_t>(Sequencer::kMaxFillSamples) + 1) v.resize(static_cast<size_t>(Sequencer::kMaxFillSamples) + 1);   // one more = "too many"
        return v;
    }
    // v44.53: the file the SAMPLE engine of synth slot k (0..2, 3 = the chord) of scene i needs; empty when the scene is not in use or the slot is not on the SAMPLE engine.
    std::string SynthSampleWanted(int i, int k) const {
        if (!SlotInUse(i)) return std::string();
        const Pattern& pt = project.patterns[static_cast<size_t>(i)];
        const SynthPatch& sp = k < kNumSynthTracks ? pt.synthPatches[static_cast<size_t>(k)] : pt.chordPatch;
        return sp.engineType == SynthEngineType::Sample ? sp.sample.File() : std::string();
    }
    // C4b (g2): every sample file name anything in memory holds: each scene and brick slot (in use or not) and every Undo / Redo
    // step, whatever the engine type. MOVE UNUSED TO TRASH never touches these.
    std::set<std::string> SampleFilesNamedInMemory() const {
        std::set<std::string> out;
        auto patch = [&](const SynthPatch& sp) { const std::string f = sp.sample.File(); if (!f.empty()) out.insert(f); };
        auto pattern = [&](const Pattern& pt) {
            if (!pt.samplerPatch.file.empty()) out.insert(pt.samplerPatch.file);
            for (const SynthPatch& sp : pt.synthPatches) patch(sp);
            patch(pt.chordPatch);
            if (pt.fill.sample[0]) out.insert(pt.fill.Sample());   // C4b: the SAMPLE fill of the scene
        };
        auto chain = [&](const std::array<ChainStep, kMaxChainSteps>& ch) { for (const ChainStep& cs : ch) if (cs.fill.sample[0]) out.insert(cs.fill.Sample()); };
        for (const Pattern& pt : project.patterns) pattern(pt);
        chain(project.songChain);
        for (const SynthPatch& sp : project.baseSynthPatches) patch(sp);   // the sound a loaded file started with
        patch(project.baseChordPatch);
        pattern(patternClipboard);   // a copied scene can be pasted back
        for (const auto* st : {&undoStack, &redoStack}) for (const UndoEntry& e : *st) {
            pattern(e.pattern); pattern(e.trackSound);
            for (const auto& b : e.bulk) pattern(b.second);
            for (const SynthPatch& sp : e.synthPatches) patch(sp);
            patch(e.chordPatch);
            chain(e.songChain);
        }
        return out;
    }
    // C4b (g2) MOVE UNUSED TO TRASH, step 1: which copies in samples/ nothing names (memory, then the saved files on the loader
    // thread). done(list, kept, error) runs on the UI thread: `list` may go to the Trash, `kept` are named only by a saved file.
    // A saved file that cannot be read stops it (error), so nothing is moved on a guess.
    void PlanTrashUnusedSamples(std::function<void(std::vector<std::string>, int, std::string)> done) {
        std::set<std::string> mem;   // folded: the Mac's disk ignores upper / lower case
        for (const std::string& n : SampleFilesNamedInMemory()) mem.insert(cleanup::Fold(n));
        std::vector<std::string> cands;
        for (const std::string& n : SampleNames()) if (!mem.count(cleanup::Fold(n))) cands.push_back(n);
        trashPlanGen_ = docGen_; trashPlanDir_ = SamplesDir();   // the plan belongs to this project only (review of C4b g2)
        if (cands.empty()) { done({}, 0, std::string()); return; }
        struct Plan { std::vector<std::string> list; int kept = 0; std::string err; };
        auto r = std::make_shared<Plan>();
        const std::string pp = projectPath;
        const unsigned gen = docGen_;
        const std::string dir = trashPlanDir_;
        loader.Post([r, cands, pp] {
            bool unreadable = false, unlisted = false;
            const std::vector<std::string> files = cleanup::FilesThatMayNameSamples(pp, &unlisted);
            if (unlisted) { r->err = "a folder of saved projects could not be read"; return; }
            const std::set<std::string> named = cleanup::NamedInFiles(cands, files, &unreadable);
            if (unreadable) { r->err = "a saved project file could not be read"; return; }
            for (const std::string& c : cands) if (!named.count(c)) r->list.push_back(c);
            r->kept = static_cast<int>(named.size());
        }, [this, r, done, gen, dir] {
            if (gen != docGen_ || dir != SamplesDir()) { done({}, 0, "another project is open now"); return; }
            done(std::move(r->list), r->kept, r->err);
        });
    }
    // True while the last plan still belongs to the open project (the browser closes the question otherwise).
    bool TrashPlanStillValid() const { return trashPlanGen_ == docGen_ && trashPlanDir_ == SamplesDir(); }
    // Step 2, after the confirmation: refused when another project is open now or a sample is still being copied into samples/
    // (a copy could reuse one of these files); what memory names now is taken out again (case ignored), the rest is moved to the
    // Trash on the loader thread, except a file that is the same file on disk as one memory names. done(moved, error) on the UI thread.
    void TrashSampleFiles(std::vector<std::string> names, std::function<void(int, std::string)> done) {
        if (!TrashPlanStillValid()) { done(0, "another project is open now"); return; }
        if (loader.Busy() > 0) { done(0, "a file is still being copied or read, try again in a moment"); return; }
        std::set<std::string> mem; std::vector<std::string> memList;
        for (const std::string& n : SampleFilesNamedInMemory()) { mem.insert(cleanup::Fold(n)); memList.push_back(n); }
        names.erase(std::remove_if(names.begin(), names.end(), [&](const std::string& n) {
            return mem.count(cleanup::Fold(n)) > 0 || n.empty() || n.find('/') != std::string::npos || n.find('\\') != std::string::npos || n == "." || n == "..";
        }), names.end());
        struct Moved { int n = 0; std::string err; };
        auto r = std::make_shared<Moved>();
        const std::string dir = SamplesDir();
        loader.Post([r, names, dir, memList] {
            for (const std::string& n : names) {
                if (cleanup::SameFileAsAny(dir, n, memList)) continue;   // another spelling of a name that plays
                std::string e;
                if (cleanup::MoveToTrash(dir + "/" + n, e)) ++r->n;
                else if (r->err.empty()) r->err = n + ": " + e;
            }
        }, [this, r, done] { sampleNamesValid_ = false; done(r->n, r->err); });
    }
    unsigned trashPlanGen_ = ~0u;
    std::string trashPlanDir_;
    // The sampler of the EDITED scene changed (a knob): the engine follows when that scene is the played one.
    void PushEditedSamplerToEngine() { if (EditIsPlaying()) sequencer.ApplySceneSound(project.currentPattern); }
    // v44.52.1: SLICES mode: writes the slice notes (ROOT, ROOT+1 ...) on the sampler's steps so that the loop plays through in order
    // at the project tempo. A slice lasts (the played part / FIT speed / slices) seconds; its step is that many steps after the
    // previous one. The track gets its own length (the loop's length in steps, at most 64). Returns the message for the info line.
    // v44.53: a "sample target" is the sampler track (kTargetSampler) or the SAMPLE engine of a synth slot 0..2 / the chord (kNumSynthTracks): the editor and
    // these functions work on a SamplerPatch copy of it (GetSampleTarget) and write it back (PutSampleTarget).
    static constexpr int kTargetSampler = -1;
    SamplerPatch GetSampleTarget(int tgt) {
        if (tgt < 0) return CurrentPattern().samplerPatch;
        return ToSamplerPatch(EditedPatch(tgt >= kNumSynthTracks, tgt).sample);
    }
    void PutSampleTarget(int tgt, const SamplerPatch& sp) {
        if (tgt < 0) { CurrentPattern().samplerPatch = sp; PushEditedSamplerToEngine(); return; }
        FromSamplerPatch(EditedPatch(tgt >= kNumSynthTracks, tgt).sample, sp);
        PushEditedPatchToEngine(tgt >= kNumSynthTracks, tgt);
    }
    // The target a track index edits (-1 for tracks without a sample).
    static int SampleTargetOfTrack(int track) {
        if (track == kSamplerTrackIndex) return kTargetSampler;
        if (track == kChordTrackIndex) return kNumSynthTracks;
        if (track >= kNumDrumTracks && track < kNumDrumTracks + kNumSynthTracks) return track - kNumDrumTracks;
        return -2;
    }
    std::string LayoutSamplerSlices(int tgt = kTargetSampler) {
        Pattern& pat = CurrentPattern();
        SamplerPatch sp = GetSampleTarget(tgt);
        if (tgt >= kNumSynthTracks) return "SLICES: THE CHORD TRACK PLAYS CHORDS - LAY OUT THE SLICES ON A SYNTH OR ON THE SAMPLER";
        const int destTrack = tgt < 0 ? kSamplerTrackIndex : kNumDrumTracks + tgt;
        const SamplePtr smp = SampleFor(sp.file);
        if (sp.mode != 1 || !smp) return "SLICES: PICK A SAMPLE AND SET MODE TO SLICES FIRST";
        SamplerVoice probe; probe.SetSample(smp); probe.SetPatch(sp);
        double a = 0, b = 0; probe.Region(a, b);
        double st[33]; const int n = probe.SliceStarts(st, a, b);
        const double rate = std::max(0.01, SamplerVoice::FitRate(*smp, sp, pat.bpm, a, b));
        const double sr = std::max(1, smp->sampleRate);
        const double stepSec = 60.0 / std::max(20.0f, pat.bpm) / ClampGrid(pat.stepsPerBeat);
        const double outSec = (b - a) / sr / rate;
        const int total = std::max(1, static_cast<int>(std::lround(outSec / stepSec)));
        PushUndo();
        Track& t = pat.tracks[static_cast<size_t>(destTrack)];
        t.steps.fill(Step{});
        int placed = 0, dropped = 0;
        std::array<int, 33> atOf{}; std::array<bool, 33> okOf{};
        for (int i = 0; i < n; ++i) {   // a slice sits where it starts in the loop (equal parts: evenly; HITS: where the hit really is)
            const int at = static_cast<int>(std::lround((st[i] - a) / sr / rate / stepSec));
            atOf[static_cast<size_t>(i)] = at;
            if (at >= kMaxSteps || at >= total || t.steps[static_cast<size_t>(at)].active) { ++dropped; continue; }
            Step& stp = t.steps[static_cast<size_t>(at)];
            stp.active = true; stp.note = static_cast<int8_t>(std::clamp(sp.root + i, 0, 127)); stp.lengthSteps = 1;
            okOf[static_cast<size_t>(i)] = true;
            ++placed;
        }
        if (tgt >= 0) {   // a synth releases at the end of its note: a slice must be held as long as it lasts (up to the next slice)
            for (int i = 0; i < n; ++i) if (okOf[static_cast<size_t>(i)]) {
                int nextAt = total;
                for (int j = i + 1; j < n; ++j) if (okOf[static_cast<size_t>(j)]) { nextAt = atOf[static_cast<size_t>(j)]; break; }
                t.steps[static_cast<size_t>(atOf[static_cast<size_t>(i)])].lengthSteps = static_cast<uint8_t>(std::clamp(nextAt - atOf[static_cast<size_t>(i)], 1, 64));
            }
        }
        const int len = std::clamp(total, 1, kMaxSteps);
        t.useOwnLength = len != pat.masterLength;
        t.length = len;
        char m[200];
        if (dropped > 0) std::snprintf(m, sizeof(m), "SLICES: %d OF %d PLACED, TRACK LENGTH %d STEPS (%d DID NOT FIT: A LOOP OVER 64 STEPS, OR TWO HITS ON ONE STEP)", placed, n, len, dropped);
        else std::snprintf(m, sizeof(m), "SLICES: %d NOTES PLACED WHERE THEY SOUND IN THE LOOP, TRACK LENGTH %d STEPS", placed, len);
        return m;
    }
    // v44.52.1: FIND HITS: the marks of the HITS slices from the sample itself (sensitivity 0..1). Returns the message for the info line.
    std::string FindSliceHits(float sensitivity, int tgt = kTargetSampler) {
        SamplerPatch sp = GetSampleTarget(tgt);
        const SamplePtr smp = SampleFor(sp.file);
        if (!smp) return "HITS: PICK A SAMPLE FIRST";
        SamplerVoice probe; probe.SetSample(smp); probe.SetPatch(sp);
        double a = 0, b = 0; probe.Region(a, b);
        std::vector<int> hits = DetectOnsets(smp->data.data(), smp->frames(), smp->channels, smp->sampleRate, static_cast<int>(a), static_cast<int>(b), sensitivity, 32);
        if (hits.empty() || hits.front() > static_cast<int>(a) + smp->sampleRate / 20) hits.insert(hits.begin(), static_cast<int>(a));   // the first slice starts with the played part
        if (hits.size() > 32) hits.resize(32);
        PushUndo();
        sp.slices = 0; sp.markCount = 0;
        const double frames = std::max(1, smp->frames());
        int prev = -1;
        for (int h : hits) { const int v = std::clamp(static_cast<int>(std::lround(h / frames * 65535.0)), 0, 65535); if (v > prev) { sp.marks[sp.markCount++] = static_cast<uint16_t>(v); prev = v; } }
        PutSampleTarget(tgt, sp);
        return "HITS: " + std::to_string(sp.markCount) + " SLICES FOUND (SENS higher finds softer hits; click the waveform to add a mark, right-click to remove one)";
    }
    // Marks by hand: `frac` = position in the whole sample (0..1).
    void AddSliceMark(float frac, int tgt = kTargetSampler) {
        SamplerPatch sp = GetSampleTarget(tgt);
        if (sp.markCount >= 32) { SetStatus("HITS: 32 MARKS IS THE MOST"); return; }
        const int v = std::clamp(static_cast<int>(std::lround(frac * 65535.0f)), 0, 65535);
        for (int i = 0; i < sp.markCount; ++i) if (std::abs(static_cast<int>(sp.marks[static_cast<size_t>(i)]) - v) < 40) return;   // already there
        PushUndo();
        int at = sp.markCount;
        while (at > 0 && sp.marks[static_cast<size_t>(at - 1)] > v) { sp.marks[static_cast<size_t>(at)] = sp.marks[static_cast<size_t>(at - 1)]; --at; }
        sp.marks[static_cast<size_t>(at)] = static_cast<uint16_t>(v); ++sp.markCount; sp.slices = 0;
        PutSampleTarget(tgt, sp);
    }
    void RemoveNearestSliceMark(float frac, float tol, int tgt = kTargetSampler) {
        SamplerPatch sp = GetSampleTarget(tgt);
        int best = -1; float bd = tol;
        for (int i = 0; i < sp.markCount; ++i) { const float d = std::fabs(sp.marks[static_cast<size_t>(i)] / 65535.0f - frac); if (d <= bd) { bd = d; best = i; } }
        if (best < 0) return;
        PushUndo();
        for (int i = best; i + 1 < sp.markCount; ++i) sp.marks[static_cast<size_t>(i)] = sp.marks[static_cast<size_t>(i + 1)];
        --sp.markCount;
        PutSampleTarget(tgt, sp);
    }
    // C4b (d): drag a HITS mark. No Undo here (the caller pushed one when the drag began). The marks stay sorted; returns the
    // mark's new index (it may pass a neighbour), or -1. A mark never lands on another (40 / 65535 apart at least).
    int MoveSliceMark(int idx, float frac, int tgt = kTargetSampler) {
        SamplerPatch sp = GetSampleTarget(tgt);
        if (idx < 0 || idx >= sp.markCount) return -1;
        const int v = std::clamp(static_cast<int>(std::lround(frac * 65535.0f)), 0, 65535);
        for (int i = 0; i < sp.markCount; ++i) if (i != idx && std::abs(static_cast<int>(sp.marks[static_cast<size_t>(i)]) - v) < 40) return idx;   // would sit on another
        for (int i = idx; i + 1 < sp.markCount; ++i) sp.marks[static_cast<size_t>(i)] = sp.marks[static_cast<size_t>(i + 1)];
        int n = sp.markCount - 1, at = n;
        while (at > 0 && sp.marks[static_cast<size_t>(at - 1)] > v) { sp.marks[static_cast<size_t>(at)] = sp.marks[static_cast<size_t>(at - 1)]; --at; }
        sp.marks[static_cast<size_t>(at)] = static_cast<uint16_t>(v);
        sp.markCount = static_cast<uint8_t>(n + 1);
        PutSampleTarget(tgt, sp);
        return at;
    }
    void ClearSliceMarks(int tgt = kTargetSampler) {
        SamplerPatch sp = GetSampleTarget(tgt);
        if (sp.markCount == 0) return;
        PushUndo();
        sp.markCount = 0;
        PutSampleTarget(tgt, sp);
    }
    // Names of the sample files (.wav, .aif, .aiff, .aifc) in samples/ (for the picker).
    const std::vector<std::string>& SampleNames() {
        if (!sampleNamesValid_) {
            sampleNamesCache_.clear();
            std::error_code ec;
            for (const auto& f : std::filesystem::directory_iterator(SamplesDir(), ec)) {
                if (!f.is_regular_file()) continue;
                if (IsSampleFileName(f.path().string())) sampleNamesCache_.push_back(f.path().filename().string());   // v44.56: AIFF too
            }
            std::sort(sampleNamesCache_.begin(), sampleNamesCache_.end());
            sampleNamesValid_ = true;
        }
        return sampleNamesCache_;
    }
    // Picks a file that is already in samples/ for the sampler of the edited scene.
    bool SetSamplerFile(const std::string& file, int tgt = kTargetSampler) {
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        if (SampleFor(file) == nullptr && !SamplePending(file)) { SetStatus("CANNOT READ " + file); return false; }
        BumpRequest(SampleKey(project.currentPattern, tgt));   // N1: a file still being read for this target is not used any more
        CancelLibraryAudition();
        PushUndo();
        if (tgt < 0) CurrentPattern().samplerPatch.file = file;
        else EditedPatch(tgt >= kNumSynthTracks, tgt).sample.SetFile(file);
        engine.SamplerFadeOut(); engine.AuditionStop();   // v44.52.1: what was sounding (HEAR, a hit) stops when another sample is chosen (before the new one is loaded: a running LOOP restarts with it)
        SyncSceneSamples(true);
        SetStatus(std::string(tgt < 0 ? "SAMPLER: " : "SAMPLE: ") + file);
        return true;
    }
    // A WAV or AIFF dropped on the window with the sampler selected: copied into samples/ (as it is: the program reads both), then it is the sample of the edited scene.
    // N1 (LIVE-2): the file is read and copied on the loader thread; the scene that was edited when it was asked for gets it then
    // (the decoded sound goes straight into the cache, so it is not read a second time).
    struct SamplerCopy { bool ok = false; std::string error, name; SamplePtr sound; int frames = 0; };
    static SamplerCopy CopySamplerFile(const std::string& wavPath, const std::string& dir, int rate) {   // any thread
        namespace fs = std::filesystem;
        SamplerCopy r;
        SampleBuffer probe;
        if (!LoadWav(wavPath, probe, rate) || probe.empty()) { const std::string why = UnreadableReason(wavPath); r.error = why.empty() ? "CANNOT READ THAT FILE (WAV OR AIFF: PCM 8 / 16 / 24 / 32 BIT OR FLOAT; NO COMPRESSED AIFF)" : why; return r; }
        if (probe.frames() > kMaxSampleSeconds * rate) { r.error = "SAMPLE TOO LONG (MAX 3 MINUTES)"; return r; }
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) { r.error = "CANNOT CREATE " + dir; return r; }
        std::string stem = SanitizeFileNamePart(fs::path(wavPath).stem().string(), "sample");
        if (stem.size() > 80) stem.resize(80);   // v44.53: a synth keeps the name in a fixed field (SynthSample::kFileMax)
        const std::string sampleExt = IsSampleFileName(wavPath) ? SampleFileExt(wavPath) : std::string(".wav");   // v44.56: an AIFF keeps its own extension in samples/
        std::string name = stem + sampleExt;
        const auto srcSize = fs::file_size(wavPath, ec);
        bool copy = true;
        for (int n = 2; n < 1000; ++n) {
            const fs::path target = fs::path(dir) / name;
            if (!fs::exists(target, ec)) break;
            if (fs::equivalent(target, wavPath, ec) || fs::file_size(target, ec) == srcSize) { copy = false; break; }   // the same file again
            name = stem + "_" + std::to_string(n) + sampleExt;
        }
        if (copy) {
            fs::copy_file(wavPath, fs::path(dir) / name, fs::copy_options::overwrite_existing, ec);
            if (ec) { r.error = "CANNOT COPY THE FILE INTO " + dir; return r; }
        }
        r.frames = probe.frames();
        if (probe.frames() >= 2) {
            auto sd = std::make_shared<SampleData>();
            sd->data = std::move(probe.data); sd->channels = std::clamp(probe.channels, 1, 2); sd->sampleRate = rate; sd->name = name;
            r.sound = sd;
        }
        r.ok = true; r.name = name;
        return r;
    }
    // `then` runs after the sample is in place (UI thread), e.g. to set LOOP mode for a loop from the library.
    std::string SampleFileOf(int scene, int tgt) const {
        const Pattern& pt = project.patterns[static_cast<size_t>(std::clamp(scene, 0, kNumSlotsTotal - 1))];
        if (tgt < 0) return pt.samplerPatch.file;
        return (tgt >= kNumSynthTracks ? pt.chordPatch : pt.synthPatches[static_cast<size_t>(std::clamp(tgt, 0, kNumSynthTracks - 1))]).sample.File();
    }
    // `then(scene)` runs after the sample is in place (UI thread), e.g. to set LOOP mode for a loop from the library.
    bool AssignSampleToSampler(const std::string& wavPath, int tgt = kTargetSampler, std::function<void(int)> then = nullptr) {
        auto res = std::make_shared<SamplerCopy>();
        const std::string dir = SamplesDir();
        const int rate = engine.SampleRate(), scene = project.currentPattern;
        const std::string key = SampleKey(scene, tgt), prev = SampleFileOf(scene, tgt);
        const unsigned tok = BumpRequest(key), gen = docGen_;
        loader.Post([res, wavPath, dir, rate] { *res = CopySamplerFile(wavPath, dir, rate); }, [this, res, tgt, scene, dir, then, rate, key, prev, tok, gen] {
            if (!res->ok) { SetStatus(res->error); statusMessageTimer = 6.0f; return; }
            if (gen != docGen_ || dir != SamplesDir()) { SetStatus("SAMPLE NOT SET: ANOTHER PROJECT IS OPEN NOW"); return; }
            if (!RequestIsNewest(key, tok) || SampleFileOf(scene, tgt) != prev) { SetStatus("SAMPLE NOT SET: ANOTHER ONE WAS CHOSEN MEANWHILE"); return; }
            {
            std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
            if (res->sound && dir == SamplesDir()) sampleCache_[dir + "/" + res->name] = res->sound;
            Pattern& pt = project.patterns[static_cast<size_t>(std::clamp(scene, 0, kNumSlotsTotal - 1))];
            PushUndoForScene(scene);
            if (tgt < 0) pt.samplerPatch.file = res->name;
            else (tgt >= kNumSynthTracks ? pt.chordPatch : pt.synthPatches[static_cast<size_t>(std::clamp(tgt, 0, kNumSynthTracks - 1))]).sample.SetFile(res->name);
            sampleNamesValid_ = false; samplesDirty_ = true;
            CancelLibraryAudition();
            engine.SamplerFadeOut(); engine.AuditionStop();   // v44.52.1: the old sample stops sounding
            SyncSceneSamples(true);
            SetStatus(std::string(tgt < 0 ? "SAMPLER: " : "SAMPLE: ") + res->name + " (" + std::to_string(static_cast<long long>(res->frames) * 1000 / std::max(1, rate)) + " MS, COPIED INTO samples/)");
            }
            if (then) then(scene);
        });
        if (loader.Running()) { SetStatus("READING " + std::filesystem::path(wavPath).filename().string() + "..."); return true; }
        return res->ok;   // no loader: both parts have run already
    }
    // ---- C4b: the SAMPLE fill ----------------------------------------------------------------------------------------------------
    // `where` >= 0: the own fill of that brick; < 0: the fill of scene (-1 - where). Null when out of range.
    FillRule* FillRuleAt(int where) {
        if (where >= 0) return where < std::clamp(project.songChainLength, 0, kMaxChainSteps) ? &project.songChain[static_cast<size_t>(where)].fill : nullptr;
        const int sc = -1 - where;
        return sc >= 0 && sc < kNumSlotsTotal ? &project.patterns[static_cast<size_t>(sc)].fill : nullptr;
    }
    // A file already in samples/ becomes the sample of that fill (its kind turns to SAMPLE; one Undo step).
    static std::string FillKey(int where) { return "fill:" + std::to_string(where); }
    bool SetFillSample(int where, const std::string& file) {
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        BumpRequest(FillKey(where));   // a library file still being copied for this fill is not used any more
        FillRule* r = FillRuleAt(where);
        if (!r || file.empty() || file.size() >= static_cast<size_t>(FillRule::kSampleMax) || file.find('/') != std::string::npos) { SetStatus("THAT SAMPLE CANNOT BE USED FOR A FILL"); return false; }
        if (where >= 0) PushSoundUndo(); else PushUndoForScene(-1 - where);
        r = FillRuleAt(where);
        if (where >= 0) { ChainStep& cs = project.songChain[static_cast<size_t>(where)]; if (cs.fillMode != FillMode::Own) { cs.fillMode = FillMode::Own; if (r->when == FillWhen::Off) r->when = FillWhen::LastLoop; } }
        else if (r->when == FillWhen::Off) r->when = FillWhen::LastLoop;
        r->kind = FillKind::Sample; r->SetSample(file);
        *r = NormalizeFill(*r);
        engine.AuditionStop();
        SyncSceneSamples(true);
        sequencer.RefreshArrangement();
        SetStatus("FILL SAMPLE: " + file);
        return true;
    }
    // A file from the library (or Finder) is copied into samples/ first, on the loader thread, then set as above -- only if it is still
    // wanted: the same project, no newer choice for this fill, the fill still has the sample it had, and (a brick) the track was not
    // changed meanwhile (a brick removed or moved would put it on another brick). Review of C4b part 5, as AssignSampleToSampler.
    struct FillTargetState { int len = 0, pattern = -1; FillMode mode = FillMode::Scene; FillKind kind = FillKind::Break; std::string sample; };
    FillTargetState FillTargetNow(int where) {
        FillTargetState st; st.len = project.songChainLength;
        if (const FillRule* r = FillRuleAt(where)) { st.kind = r->kind; st.sample = r->Sample(); }
        if (where >= 0 && where < std::clamp(project.songChainLength, 0, kMaxChainSteps)) { st.pattern = project.songChain[static_cast<size_t>(where)].patternIndex; st.mode = project.songChain[static_cast<size_t>(where)].fillMode; }
        return st;
    }
    static bool SameFillTarget(const FillTargetState& a, const FillTargetState& b) { return a.len == b.len && a.pattern == b.pattern && a.mode == b.mode && a.kind == b.kind && a.sample == b.sample; }
    bool AssignSampleToFill(const std::string& wavPath, int where) {
        if (!FillRuleAt(where)) { SetStatus("FILL NOT SET: THAT BRICK DOES NOT EXIST"); return false; }
        auto res = std::make_shared<SamplerCopy>();
        const std::string dir = SamplesDir(), key = FillKey(where);
        const int rate = engine.SampleRate();
        const unsigned gen = docGen_, tok = BumpRequest(key);
        const FillTargetState before = FillTargetNow(where);
        loader.Post([res, wavPath, dir, rate] { *res = CopySamplerFile(wavPath, dir, rate); }, [this, res, dir, where, gen, key, tok, before] {
            if (!res->ok) { SetStatus(res->error); statusMessageTimer = 6.0f; return; }
            if (gen != docGen_ || dir != SamplesDir()) { SetStatus("FILL NOT SET: ANOTHER PROJECT IS OPEN NOW"); return; }
            std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
            if (res->sound) sampleCache_[dir + "/" + res->name] = res->sound;   // the copy is there either way
            sampleNamesValid_ = false; samplesDirty_ = true;
            if (!RequestIsNewest(key, tok)) { SetStatus("FILL NOT SET: ANOTHER SAMPLE WAS CHOSEN MEANWHILE"); return; }
            if (!SameFillTarget(before, FillTargetNow(where))) { SetStatus(where >= 0 ? "FILL NOT SET: THE BRICK CHANGED MEANWHILE" : "FILL NOT SET: THE SCENE'S FILL CHANGED MEANWHILE"); return; }
            SetFillSample(where, res->name);
        });
        if (loader.Running()) { SetStatus("READING " + std::filesystem::path(wavPath).filename().string() + "..."); return true; }
        return res->ok;
    }
    // ---- SAMPLE LIBRARY (v44.51) -----------------------------------------------------------------------------------------------
    // The user's own folder of samples (see core/SampleLibrary.h). Only listed and auditioned here; a chosen file is COPIED into the
    // project (the sampler's samples/, or the kit of the drum scene) by the same code that handles a dropped WAV.
    enum class LibTarget { None, Drum, Sampler, Transition, Fill };   // Fill (C4b): libraryBrowserBrick = the brick, or -1 - scene for a scene's fill
    LibTarget libraryBrowserRequest = LibTarget::None;   // set by a button, taken by BottomPanel::DrawBrowsers
    int libraryBrowserBrick = -1;                        // Transition: the brick whose transition is chosen
    std::vector<LibSample> libSamples_;
    bool libValid_ = false, libTruncated_ = false;
    std::string libRoot_;
    std::map<std::string, SamplePtr> libAudition_;
    std::deque<SamplePtr> libAuditionKeep_;              // the last few stay alive: the audio thread may still hold the one that plays
    const std::vector<LibSample>& LibraryList() {
        if (!libValid_) { libRoot_ = SamplesRoot(); libSamples_ = ScanSampleLibrary(libRoot_, &libTruncated_); libValid_ = true; }
        return libSamples_;
    }
    const std::string& LibraryRoot() { LibraryList(); return libRoot_; }
    // C4b: the listing for the SAMPLES browser without reading the disk under the scene lock: null until it is there. The first
    // call starts the scan on the loader thread; a scan older than a RescanLibrary (or a new library folder) is dropped.
    unsigned libScanGen_ = 0;
    bool libScanRunning_ = false;
    // ---- C4b (v2): the user's own genre / tags of library samples (Library/sample_tags.txt; the samples are never changed) ----
    // Read once on the loader thread (null until then); every change is written in one go on the loader thread, in order.
    SampleTagMap sampleTags_;
    bool sampleTagsValid_ = false, sampleTagsLoading_ = false;
    std::string sampleTagsPathOverride_;   // tests
    unsigned sampleTagsGen_ = 0;           // changes made (UI thread)
    std::shared_ptr<std::atomic<unsigned>> sampleTagsSaved_ = std::make_shared<std::atomic<unsigned>>(0u);   // the last change on disk
    std::string SampleTagsPath() const { return sampleTagsPathOverride_.empty() ? userdirs::LibraryDir() + "/sample_tags.txt" : sampleTagsPathOverride_; }
    static SampleTagMap ReadSampleTags(const std::string& path) {   // any thread; a missing file = no corrections
        std::ifstream in(path, std::ios::binary);
        if (!in) return {};
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return sampletags::ParseStore(text);
    }
    const SampleTagMap* SampleTagsIfReady() {
        if (sampleTagsValid_) return &sampleTags_;
        const std::string path = SampleTagsPath();
        if (!loader.Running()) { sampleTags_ = ReadSampleTags(path); sampleTagsValid_ = true; return &sampleTags_; }
        if (!sampleTagsLoading_) {
            sampleTagsLoading_ = true;
            auto out = std::make_shared<SampleTagMap>();
            loader.Post([out, path] { *out = ReadSampleTags(path); }, [this, out] { sampleTagsLoading_ = false; if (!sampleTagsValid_) { sampleTags_ = std::move(*out); sampleTagsValid_ = true; } });
        }
        return nullptr;
    }
    // `o` = the new genre / tags of library file `rel`; null = back to the guess. Ignored until the file was read.
    bool SetSampleTagOverride(const std::string& rel, const SampleTagOverride* o) {
        if (!sampleTagsValid_ || rel.empty() || rel.find('\t') != std::string::npos || rel.find('\n') != std::string::npos) return false;
        if (o) sampleTags_[rel] = *o; else sampleTags_.erase(rel);
        const unsigned gen = ++sampleTagsGen_;
        const std::string text = sampletags::SerializeStore(sampleTags_), path = SampleTagsPath();
        auto ok = std::make_shared<bool>(false);
        auto saved = sampleTagsSaved_;
        loader.Post([ok, text, path, gen, saved] {
            *ok = WriteSampleTagsFile(path, text);
            if (*ok) saved->store(gen);   // the loader runs one job at a time, in order, so this only grows
        }, [this, ok] { if (!*ok) { SetStatus("CANNOT SAVE THE SAMPLE TAGS (Library/sample_tags.txt)"); statusMessageTimer = 6.0f; } });
        return true;
    }
    // Review of PR 35: AsyncLoader::Stop() drops jobs that have not started, so at quit (after loader.Stop()) the last change
    // is written here if its write never ran. Returns false only when a needed write failed.
    bool FlushSampleTags() {
        if (!sampleTagsValid_ || sampleTagsSaved_->load() == sampleTagsGen_) return true;
        if (!WriteSampleTagsFile(SampleTagsPath(), sampletags::SerializeStore(sampleTags_))) return false;
        sampleTagsSaved_->store(sampleTagsGen_);
        return true;
    }
    // ---- C4b (v3): performance sets of the SAMPLES browser (Library/Favorites/sample_sets.txt), the same way as the own tags:
    // read once on the loader thread, every change written whole on the loader thread in order, the last one again at quit.
    SampleSetList sampleSets_;
    bool sampleSetsValid_ = false, sampleSetsLoading_ = false;
    std::string sampleSetsPathOverride_;   // tests
    unsigned sampleSetsGen_ = 0;
    std::shared_ptr<std::atomic<unsigned>> sampleSetsSaved_ = std::make_shared<std::atomic<unsigned>>(0u);
    std::string SampleSetsPath() const { return sampleSetsPathOverride_.empty() ? userdirs::FavoritesDir() + "/sample_sets.txt" : sampleSetsPathOverride_; }
    static SampleSetList ReadSampleSets(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return {};
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return samplesets::ParseStore(text);
    }
    const SampleSetList* SampleSetsIfReady() {
        if (sampleSetsValid_) return &sampleSets_;
        const std::string path = SampleSetsPath();
        if (!loader.Running()) { sampleSets_ = ReadSampleSets(path); sampleSetsValid_ = true; return &sampleSets_; }
        if (!sampleSetsLoading_) {
            sampleSetsLoading_ = true;
            auto out = std::make_shared<SampleSetList>();
            loader.Post([out, path] { *out = ReadSampleSets(path); }, [this, out] { sampleSetsLoading_ = false; if (!sampleSetsValid_) { sampleSets_ = std::move(*out); sampleSetsValid_ = true; } });
        }
        return nullptr;
    }
    void SaveSampleSets() {
        const unsigned gen = ++sampleSetsGen_;
        const std::string text = samplesets::SerializeStore(sampleSets_), path = SampleSetsPath();
        auto ok = std::make_shared<bool>(false);
        auto saved = sampleSetsSaved_;
        loader.Post([ok, text, path, gen, saved] {
            *ok = WriteSampleTagsFile(path, text);
            if (*ok) saved->store(gen);
        }, [this, ok] { if (!*ok) { SetStatus("CANNOT SAVE THE SAMPLE SETS (Library/Favorites/sample_sets.txt)"); statusMessageTimer = 6.0f; } });
    }
    // All refused (false) until the file was read, so a slow read is never overwritten.
    // A new set named `base` (or "base 2" ... when taken) holding `firstKey` (if not empty). Returns its index, -1 when refused.
    int NewSampleSet(const std::string& base, const std::string& firstKey) {
        const std::string clean = samplesets::CleanName(base);
        if (!sampleSetsValid_ || clean.empty() || sampleSets_.size() >= samplesets::kMaxSets) return -1;
        SampleSet set; set.name = samplesets::CleanName(samplesets::FreeName(sampleSets_, clean));
        if (set.name.empty() || samplesets::Find(sampleSets_, set.name) >= 0) return -1;
        if (!firstKey.empty()) set.keys.push_back(firstKey);
        sampleSets_.push_back(std::move(set));
        SaveSampleSets();
        return static_cast<int>(sampleSets_.size()) - 1;
    }
    bool AddToSampleSet(int set, const std::string& key) {
        if (!sampleSetsValid_ || set < 0 || set >= static_cast<int>(sampleSets_.size()) || key.empty() || key.find_first_of("\t\r\n") != std::string::npos) return false;
        auto& keys = sampleSets_[static_cast<size_t>(set)].keys;
        if (std::find(keys.begin(), keys.end(), key) != keys.end() || keys.size() >= samplesets::kMaxEntries) return false;
        keys.push_back(key);
        SaveSampleSets();
        return true;
    }
    bool RemoveFromSampleSet(int set, const std::string& key) {
        if (!sampleSetsValid_ || set < 0 || set >= static_cast<int>(sampleSets_.size())) return false;
        auto& keys = sampleSets_[static_cast<size_t>(set)].keys;
        const auto it = std::find(keys.begin(), keys.end(), key);
        if (it == keys.end()) return false;
        keys.erase(it);
        SaveSampleSets();
        return true;
    }
    bool RenameSampleSet(int set, const std::string& name) {
        const std::string clean = samplesets::CleanName(name);
        if (!sampleSetsValid_ || set < 0 || set >= static_cast<int>(sampleSets_.size()) || clean.empty()) return false;
        const int other = samplesets::Find(sampleSets_, clean);
        if (other >= 0) return other == set;   // the same name again: nothing to do; another set's name: refused
        sampleSets_[static_cast<size_t>(set)].name = clean;
        SaveSampleSets();
        return true;
    }
    bool DeleteSampleSet(int set) {   // only the list goes; the samples are not touched
        if (!sampleSetsValid_ || set < 0 || set >= static_cast<int>(sampleSets_.size())) return false;
        sampleSets_.erase(sampleSets_.begin() + set);
        SaveSampleSets();
        return true;
    }
    bool FlushSampleSets() {   // at quit, after loader.Stop() (see FlushSampleTags)
        if (!sampleSetsValid_ || sampleSetsSaved_->load() == sampleSetsGen_) return true;
        if (!WriteSampleTagsFile(SampleSetsPath(), samplesets::SerializeStore(sampleSets_))) return false;
        sampleSetsSaved_->store(sampleSetsGen_);
        return true;
    }
    static bool WriteSampleTagsFile(const std::string& path, const std::string& text) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        return WriteFileAtomic(path, text);
    }
    const std::vector<LibSample>* LibraryListIfReady() {
        if (libValid_) return &libSamples_;
        if (!loader.Running()) return &LibraryList();
        if (!libScanRunning_) {
            libScanRunning_ = true;
            const unsigned gen = libScanGen_;
            struct Scan { std::string root; std::vector<LibSample> list; bool truncated = false; };
            auto out = std::make_shared<Scan>();
            loader.Post([out] { out->root = SamplesRoot(); out->list = ScanSampleLibrary(out->root, &out->truncated); }, [this, out, gen] {
                libScanRunning_ = false;
                if (gen != libScanGen_ || libValid_) return;
                libRoot_ = std::move(out->root); libSamples_ = std::move(out->list); libTruncated_ = out->truncated; libValid_ = true;
            });
        }
        return nullptr;
    }
    void RescanLibrary() {
        ++libScanGen_;
        libValid_ = false; libAudition_.clear();
        const int kits = SyncLibraryKits(SamplesRoot(), "kits");
        if (kits > 0) InvalidateFolderCaches();
        const size_t n = LibraryList().size();
        size_t nLocked = 0; for (const LibSample& ls : libSamples_) if (ls.locked) ++nLocked;
        SetStatus("LIBRARY: " + std::to_string(n) + " SAMPLES" + (nLocked > 0 ? " (" + std::to_string(nLocked) + " PROTECTED ABLETON FILES: ONLY LIVE CAN READ THEM)" : std::string()) + (kits > 0 ? ", " + std::to_string(kits) + " NEW KIT" + (kits == 1 ? "" : "S") + " COPIED INTO kits/" : std::string()) + (libTruncated_ ? " (LIST CUT AT 20000)" : ""));
    }
    std::string LibraryPath(const std::string& rel) { return LibraryRoot() + "/" + rel; }
    // Plays a library file once through the master bus (nothing else is touched).
    // v44.56: the sentence for the info line when a file cannot be read (the reason when it is known: a protected Ableton pack sample)
    static std::string CannotReadText(const std::string& path, const std::string& label) {
        const std::string why = UnreadableReason(path);
        return why.empty() ? "CANNOT READ THAT FILE: " + label : why + ": " + label;
    }
    static constexpr float kAuditionGain = 0.4f;   // v44.52.1: the library audition is quieter than before (0.85): a loud file no longer startles
    // N1 (LIVE-2): a file not heard before is read on the loader thread and starts when it arrives (only the newest request plays).
    std::string libAuditionWanted_;
    void CancelLibraryAudition() { libAuditionWanted_.clear(); }   // a file still being read for HEAR does not start when it arrives
    bool AuditionLibrary(const std::string& rel) {
        const std::string path = LibraryPath(rel);
        auto it = libAudition_.find(path);
        if (it != libAudition_.end()) { libAuditionWanted_.clear(); return PlayLibraryAudition(rel, path, it->second); }   // a file still being read is not wanted any more
        libAuditionWanted_ = path;
        auto out = std::make_shared<SamplePtr>();
        const int rate = engine.SampleRate();
        auto playedFlag = std::make_shared<bool>(false);
        loader.Post([out, path, rel, rate] {
            SampleBuffer buf;
            if (LoadWav(path, buf, rate) && !buf.empty() && buf.frames() >= 2) {
                auto sd = std::make_shared<SampleData>();
                sd->data = std::move(buf.data); sd->channels = std::clamp(buf.channels, 1, 2); sd->sampleRate = rate; sd->name = rel;
                *out = sd;
            }
        }, [this, out, path, rel, playedFlag] {
            if (libAudition_.size() > 40) libAudition_.clear();
            libAudition_[path] = *out;
            if (path == libAuditionWanted_) { libAuditionWanted_.clear(); *playedFlag = PlayLibraryAudition(rel, path, *out); }
        });
        return loader.Running() ? true : *playedFlag;   // no loader: both parts have run already
    }
    bool PlayLibraryAudition(const std::string& rel, const std::string& path, const SamplePtr& smp) {
        if (!smp) { SetStatus(CannotReadText(path, rel)); statusMessageTimer = 6.0f; return false; }
        libAuditionKeep_.push_back(smp);
        while (libAuditionKeep_.size() > 16) libAuditionKeep_.pop_front();
        double speed = 1.0;   // v44.52.1: a loop with a tempo in its name is heard at the project tempo (varispeed, like the sampler)
        for (const LibSample& ls : LibraryList()) if (ls.rel == rel) { if (ls.bpm > 0.0) speed = CurrentPattern().bpm / ls.bpm; break; }
        engine.AuditionSample(smp, kAuditionGain, speed);
        return true;
    }
    // v44.52.1: a LOOP from the library went to the sampler: play it as a loop, tempo fit AUTO (the tempo comes from the name when it has one).
    // N1: the same for the scene that asked for the file (the edited scene may be another one by the time the file arrives).
    void SetSamplerLoopFromLibraryInScene(const LibSample& ls, int tgt, int scene) {
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        const int was = project.currentPattern;
        project.currentPattern = std::clamp(scene, 0, kNumSlotsTotal - 1);
        SetSamplerLoopFromLibrary(ls, tgt);
        project.currentPattern = was;
    }
    void SetSamplerLoopFromLibrary(const LibSample& ls, int tgt = kTargetSampler) {
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        SamplerPatch sp = GetSampleTarget(tgt);
        sp.mode = 2; sp.fitBars = 0; sp.start = 0.0f; sp.decay = 1.0f;
        PutSampleTarget(tgt, sp);
        char m[120];
        if (ls.bpm > 0.0) std::snprintf(m, sizeof(m), "SAMPLER: LOOP, %.0f BPM IN THE NAME, PLAYS AT %.0f BPM (MODE: SLICES CUTS IT INTO PARTS)", ls.bpm, static_cast<double>(CurrentPattern().bpm));
        else std::snprintf(m, sizeof(m), "SAMPLER: LOOP, TEMPO FIT AUTO (MODE: SLICES CUTS IT INTO PARTS)");
        SetStatus(m);
    }
    // ---- the user's own transitions (v44.51) -----------------------------------------------------------------------------------
    // A library file chosen for a transition is COPIED into <project>/samples/transitions/tNN_name.wav; NN (01..40) is its slot, so the
    // riser index a brick stores (RiserPlayer::kFirstUser + NN - 1) stays valid however the folder changes. Loading a project (or SAVE AS
    // into another folder) reads that folder into the engine; nothing is stored in the project file itself.
    std::string TransitionsDir() const { return SamplesDir() + "/transitions"; }
    std::string transSyncedDir_ = "?";
    bool transDirty_ = true;
    // N1 (LIVE-2): the folder is read on the loader thread; `then` runs (UI thread) once the engine has the new list.
    bool transLoading_ = false;
    void ReloadUserTransitions(std::function<void()> then = nullptr) {
        const std::string dir = TransitionsDir();
        const int rate = engine.SampleRate();
        auto list = std::make_shared<std::vector<UserTransition>>();
        transSyncedDir_ = dir; transDirty_ = false; transLoading_ = true;
        loader.Post([list, dir, rate] { *list = LoadUserTransitionFolder(dir, rate); }, [this, list, then] {
            transLoading_ = false;
            { std::lock_guard<SceneMutex> lock(sequencer.PatternMutex()); engine.ReplaceUserTransitions(std::move(*list)); }
            if (then) then();
        });
    }
    void SyncUserTransitions() { if (!transLoading_ && (transDirty_ || TransitionsDir() != transSyncedDir_)) ReloadUserTransitions(); }
    // The library file `rel` becomes the transition before brick `brick` (>= 1).
    // N1 (LIVE-2): the file is read and copied on the loader thread; the brick gets it once the engine has the new transitions.
    struct TransitionCopy { bool ok = false; std::string error; int slot = -1; };
    static TransitionCopy CopyTransitionFile(const std::string& src, const std::string& rel, const std::string& dir, int rate) {   // any thread
        namespace fs = std::filesystem;
        TransitionCopy r;
        SampleBuffer probe;
        if (!LoadWav(src, probe, rate) || probe.empty() || probe.frames() < 2) { r.error = CannotReadText(src, rel); return r; }
        const double secs = static_cast<double>(probe.frames()) / std::max(1, rate);
        if (secs > 30.0) { r.error = "A TRANSITION CAN BE 30 SECONDS AT MOST"; return r; }
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) { r.error = "CANNOT CREATE " + dir; return r; }
        std::string stem = SanitizeFileNamePart(fs::path(src).stem().string(), "transition");
        for (char& c : stem) if (c == ' ') c = '_';
        const auto srcSize = fs::file_size(src, ec);
        int slot = -1;
        std::array<bool, RiserPlayer::kNumUser> used{};
        for (const auto& f : fs::directory_iterator(dir, ec)) {
            if (!f.is_regular_file(ec)) continue;
            const int sl = TransitionSlotOfStem(f.path().stem().string());
            if (sl < 0) continue;
            used[static_cast<size_t>(sl)] = true;
            if (slot < 0 && (fs::file_size(f.path(), ec) == srcSize || IsAiffFileName(src)) && f.path().stem().string().size() > 4 && f.path().stem().string().substr(4) == stem) slot = sl;   // the same file again: same slot
        }
        if (slot < 0) {
            for (int i = 0; i < RiserPlayer::kNumUser; ++i) if (!used[static_cast<size_t>(i)]) { slot = i; break; }
            if (slot < 0) { r.error = "40 OWN TRANSITIONS IN THIS PROJECT ALREADY"; return r; }
            char nn[8]; std::snprintf(nn, sizeof(nn), "t%02d_", slot + 1);
            if (!ImportAudioAsWav(src, (fs::path(dir) / (std::string(nn) + stem + ".wav")).string())) { r.error = "CANNOT COPY THE FILE INTO " + dir; return r; }   // v44.56: an AIFF becomes a .wav here (the transitions folder holds WAVs)
        }
        r.ok = true; r.slot = slot;
        return r;
    }
    bool AssignLibraryTransition(int brick, const std::string& rel) {
        if (brick < 1 || brick >= project.songChainLength) { SetStatus("A TRANSITION GOES BEFORE BRICK 2 OR LATER"); statusMessageTimer = 6.0f; return false; }
        auto res = std::make_shared<TransitionCopy>();
        const std::string src = LibraryPath(rel), dir = TransitionsDir();
        const int rate = engine.SampleRate();
        const std::string key = "tr:" + std::to_string(brick);
        const int8_t prev = project.songChain[static_cast<size_t>(brick)].riser;
        const unsigned tok = BumpRequest(key), gen = docGen_;
        auto stillWanted = [this, brick, key, prev, tok, gen, dir] {
            return gen == docGen_ && dir == TransitionsDir() && brick < project.songChainLength && RequestIsNewest(key, tok) && project.songChain[static_cast<size_t>(brick)].riser == prev;
        };
        loader.Post([res, src, rel, dir, rate] { *res = CopyTransitionFile(src, rel, dir, rate); }, [this, res, brick, stillWanted] {
            if (!res->ok) { SetStatus(res->error); statusMessageTimer = 6.0f; return; }
            if (!stillWanted()) { SetStatus("TRANSITION NOT SET: THE PROJECT OR THE BRICK CHANGED MEANWHILE"); return; }
            ReloadUserTransitions([this, res, brick, stillWanted] {
                std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
                if (!stillWanted()) { SetStatus("TRANSITION NOT SET: THE PROJECT OR THE BRICK CHANGED MEANWHILE"); return; }
                PushSoundUndo();
                ChainStep& cs = project.songChain[static_cast<size_t>(brick)];
                cs.riser = static_cast<int8_t>(RiserPlayer::kFirstUser + res->slot);
                if (cs.riserBars < 1) cs.riserBars = 2;
                sequencer.RefreshArrangement();
                SetStatus("TRANSITION BEFORE BRICK " + std::to_string(brick + 1) + ": " + engine.Risers().NameAt(cs.riser) + " (COPIED INTO samples/transitions)");
            });
        });
        if (loader.Running()) { SetStatus("READING " + rel + "..."); return true; }
        return res->ok;   // no loader: both parts have run already
    }
    // Chooses where the library browser sends the picked file: the edited drum track, the sampler, or a transition.
    int libSampleTarget = kTargetSampler;   // v44.53: for LibTarget::Sampler: the sampler track, or the SAMPLE engine of a synth slot / the chord (see GetSampleTarget)
    void OpenLibrary(LibTarget t, int brick = -1, int sampleTarget = kTargetSampler) { libraryBrowserRequest = t; libraryBrowserBrick = brick; libSampleTarget = sampleTarget; }
    // The native file window for the sampler (click on the sample card): the chosen WAV goes the same way as a dropped one.
    FilePicker samplePicker;
    int samplePickTarget = kTargetSampler;
    bool RequestSamplePick(int tgt = kTargetSampler) {
        samplePickTarget = tgt;
        if (samplePicker.Busy()) { SetStatus("THE FILE WINDOW IS ALREADY OPEN"); return false; }
        if (!samplePicker.StartWav()) { SetStatus("NO FILE WINDOW ON THIS SYSTEM - DROP THE .WAV OR .AIF ON THE WINDOW"); statusMessageTimer = 6.0f; return false; }
        SetStatus("CHOOSE A .WAV OR .AIF IN THE FILE WINDOW");
        return true;
    }
    void PollSamplePick() {
        std::string path;
        if (samplePicker.Poll(path)) AssignSampleToSampler(path, samplePickTarget);
        if (libraryFolderPicker.Poll(path)) { SetSamplesRoot(path); RescanLibrary(); }   // v44.51: the library folder chosen in the Library menu
    }
    FilePicker libraryFolderPicker;
    bool RequestLibraryFolder() {
        if (libraryFolderPicker.Busy()) { SetStatus("THE FILE WINDOW IS ALREADY OPEN"); return false; }
        if (!libraryFolderPicker.StartFolder()) { SetStatus("NO FILE WINDOW ON THIS SYSTEM - SET TRB_SAMPLES_DIR OR PUT THE SAMPLES IN THE DEFAULT SAMPLES FOLDER"); statusMessageTimer = 6.0f; return false; }
        SetStatus("CHOOSE YOUR SAMPLES FOLDER IN THE FILE WINDOW");
        return true;
    }
    // SAVE AS into another folder: the samples the project uses are copied beside the new file (nothing is moved or deleted).
    void CopySamplesTo(const std::string& newProjectPath) {
        namespace fs = std::filesystem;
        const std::string from = SamplesDir(), to = DirNameOf(newProjectPath) + "/samples";
        std::error_code ec;
        if (fs::equivalent(from, to, ec)) return;
        std::set<std::string> files;
        for (int i = 0; i < kNumSlotsTotal; ++i) {
            const Pattern& pt = project.patterns[static_cast<size_t>(i)];
            if (!pt.samplerPatch.file.empty()) files.insert(pt.samplerPatch.file);
            for (int k = 0; k <= kNumSynthTracks; ++k) { const SynthPatch& sp = k < kNumSynthTracks ? pt.synthPatches[static_cast<size_t>(k)] : pt.chordPatch; if (sp.sample.HasFile()) files.insert(sp.sample.File()); }   // v44.53
        }
        const fs::path tfrom = fs::path(from) / "transitions";   // v44.51: the user's own transitions travel with the project
        if (fs::is_directory(tfrom, ec)) {
            fs::create_directories(fs::path(to) / "transitions", ec);
            for (const auto& f : fs::directory_iterator(tfrom, ec)) if (f.is_regular_file(ec)) fs::copy_file(f.path(), fs::path(to) / "transitions" / f.path().filename(), fs::copy_options::skip_existing, ec);
        }
        {   // C4b (e): the kits inside the project's folder that the project plays (samples/kits/<kit>)
            std::set<std::string> kits;
            auto addKit = [&](const std::string& k) { if (IsProjectKit(k)) kits.insert(k.substr(8)); };   // "kits/<kit>" under samples/
            addKit(project.kitFolder);
            for (const Pattern& pt : project.patterns) { addKit(pt.kitFolder); for (int t = 0; t < kNumDrumTracks; ++t) addKit(pt.tracks[static_cast<size_t>(t)].kitFolder); }
            for (const std::string& k : kits) {
                const fs::path src = fs::path(from) / k, dst = fs::path(to) / k;
                if (!fs::is_directory(src, ec)) continue;
                fs::create_directories(dst, ec);
                for (const auto& f : fs::directory_iterator(src, ec)) if (f.is_regular_file(ec)) fs::copy_file(f.path(), dst / f.path().filename(), fs::copy_options::skip_existing, ec);
            }
            if (!kits.empty()) kitsDirty_ = true;   // read again from the new folder
        }
        if (files.empty()) return;
        fs::create_directories(to, ec);
        for (const std::string& f : files) fs::copy_file(fs::path(from) / f, fs::path(to) / f, fs::copy_options::skip_existing, ec);
    }

    // Program start: hands the engine the sound of the project that was just opened (synth / chord patches, samples of the
    // sampler and of SAMPLE engines, kits, mixer). The sequencer must know the engine BEFORE RefreshSound: before, main.cpp
    // refreshed first, ApplySceneSound returned early (no engine yet) and the auto-opened last project played the default synth
    // instead of its samples until the scene's sound was touched.
    void StartSound() {
        sequencer.Init(&engine, &project);
        RefreshSound();
        engine.GetMixer().SyncFromPattern(CurrentPattern());
        engine.GetMixer().SyncFxFromProject(project);
    }
    void RefreshSound() {
        SyncSceneKits(true);
        SyncSceneSamples(true);
        sequencer.ApplySceneSound(sequencer.PlayPatternIndex());
    }

    // Copying sound between scenes. `what` bits: kSoundSynth0..2, kSoundChord, kSoundKick, kSoundDrums (tune / decay / default sample of
    // the six drum tracks), kSoundKit. dst = a scene, or kToAll / kToChain.
    static constexpr int kSoundSynth0 = 1, kSoundSynth1 = 2, kSoundSynth2 = 4, kSoundChord = 8, kSoundKick = 16, kSoundDrums = 32, kSoundKit = 64, kSoundSampler = 128;
    static constexpr int kSoundAll = 255, kToAll = -1, kToChain = -2;
    // The sound parts named by `what` (see the bits above) from scene a to scene b; nothing else of b changes.
    static void CopySoundParts(Pattern& b, const Pattern& a, int what) {
        for (int i = 0; i < kNumSynthTracks; ++i) if (what & (kSoundSynth0 << i)) b.synthPatches[static_cast<size_t>(i)] = a.synthPatches[static_cast<size_t>(i)];
        if (what & kSoundChord) b.chordPatch = a.chordPatch;
        if (what & kSoundKick) b.kick = a.kick;
        if (what & kSoundDrums) for (int i = 0; i < kNumDrumTracks; ++i) {
            b.tracks[static_cast<size_t>(i)].drumTune = a.tracks[static_cast<size_t>(i)].drumTune;
            b.tracks[static_cast<size_t>(i)].drumDecay = a.tracks[static_cast<size_t>(i)].drumDecay;
            b.tracks[static_cast<size_t>(i)].edit = a.tracks[static_cast<size_t>(i)].edit;   // v44.52
            b.tracks[static_cast<size_t>(i)].defaultVariant = a.tracks[static_cast<size_t>(i)].defaultVariant;
        }
        if (what & kSoundKit) b.kitFolder = a.kitFolder;
        if (what & kSoundSampler) b.samplerPatch = a.samplerPatch;   // v44.50
    }
    bool CopySceneSound(int src, int dst, int what) {
        if (src < 0 || src >= kNumPatterns || dst < 0 || dst >= kNumPatterns || src == dst || what == 0) return false;
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        const Pattern& a = project.patterns[static_cast<size_t>(src)];
        Pattern& b = project.patterns[static_cast<size_t>(dst)];
        PushUndoForScene(dst);
        CopySoundParts(b, a, what);
        if (dst == sequencer.PlayPatternIndex()) sequencer.ApplySceneSound(dst);
        return true;
    }
    // Returns how many scenes received it.
    int CopySoundToScenes(int src, int dstOrMode, int what) {
        int n = 0;
        std::array<bool, kNumPatterns> inChain{};
        for (int i = 0; i < project.songChainLength; ++i) inChain[static_cast<size_t>(std::clamp(project.songChain[static_cast<size_t>(i)].patternIndex, 0, kNumPatterns - 1))] = true;
        for (int sc = 0; sc < kNumPatterns; ++sc) {
            if (sc == src || !SlotInUse(sc)) continue;
            if (dstOrMode == kToChain && !inChain[static_cast<size_t>(sc)]) continue;
            if (dstOrMode >= 0 && sc != dstOrMode) continue;
            if (CopySceneSound(src, sc, what)) ++n;
        }
        SyncSceneKits();
        SetStatus("SOUND COPIED FROM SCENE " + std::to_string(src + 1) + " TO " + std::to_string(n) + " SCENE" + (n == 1 ? "" : "S"));
        return n;
    }

    // ---- Custom drum patterns (RANDOM tab TYPICAL PATTERN row) -----------
    // The user-saved counterpart to ApplyGenrePattern above -- see
    // core/CustomPatterns.h for the on-disk format and why it's a
    // deliberately smaller capture than a full project/pattern save.
    void ApplyCustomPattern(const std::string& name) {
        CustomPatternData data;
        if (!LoadCustomDrumPattern(name, data)) {
            SetStatus("LOAD FAILED: " + name);
            return;
        }
        PushUndo();
        Pattern& pat = CurrentPattern();
        int len = pat.masterLength;
        // a saved beat remembers the grid it was written on (older files: 1/16) and is mapped onto this pattern's grid
        const int fromGrid = ClampGrid(data.grid), toGrid = ClampGrid(pat.stepsPerBeat);
        auto mapStep = [&](int s) { return (s * toGrid + fromGrid / 2) / fromGrid; };
        int stride = std::max(1, (data.storedLength * toGrid + fromGrid / 2) / fromGrid);
        for (int slot = 0; slot < kNumDrumTracks; ++slot) {
            Track& t = pat.tracks[slot];
            for (auto& s : t.steps) s = Step{};
            for (int hit : data.hitSteps[slot]) {
                for (int i = mapStep(hit); i >= 0 && i < len; i += stride) t.steps[i].active = true;
            }
            for (int hit : data.accentSteps[slot]) {
                for (int i = mapStep(hit); i >= 0 && i < len; i += stride) {
                    if (t.steps[i].active) t.steps[i].accent = true;
                }
            }
            // per-hit variant / ratchet / probability (newer beat files)
            for (const auto& d : data.details[slot]) {
                for (int i = mapStep(d.step); i >= 0 && i < len; i += stride) {
                    if (!t.steps[i].active) continue;
                    t.steps[i].variant = static_cast<uint8_t>(d.variant);
                    t.steps[i].ratchet = static_cast<uint8_t>(d.ratchet);
                    t.steps[i].probability = static_cast<uint8_t>(d.probability);
                }
            }
        }
        pat.swing = data.swing;
        SyncMixer(pat);
        SetStatus(name + " BEAT (SAVED)");
    }
    void SaveCurrentAsCustomPattern(const std::string& name) {
        // Nothing to save if the 6 drum tracks are empty (a silent "beat" would
        // only clutter the list).
        bool any = false;
        const Pattern& cp = CurrentPattern();
        for (int slot = 0; slot < kNumDrumTracks && !any; ++slot)
            for (int i = 0; i < cp.masterLength; ++i) if (cp.tracks[slot].steps[i].active) { any = true; break; }
        if (!any) { SetStatus("NOTHING TO SAVE: NO DRUM HITS IN THIS SCENE"); return; }
        bool existed = CustomDrumPatternExists(name);
        bool ok = SaveCustomDrumPattern(name, cp);
        // So the just-saved pattern shows up in TYPICAL PATTERN's browse
        // list immediately, without needing a tab round-trip to pick up a
        // fresh disk scan -- see the cached-listings comment above.
        if (ok) customPatternNamesCacheValid = false;
        SetStatus(ok ? ((existed ? "UPDATED BEAT: " : "SAVED BEAT: ") + name) : ("SAVE FAILED: " + name));
    }
    void RenameCustomPattern(const std::string& oldName, const std::string& newName) {
        bool ok = RenameCustomDrumPattern(oldName, newName);
        if (ok) customPatternNamesCacheValid = false;
        SetStatus(ok ? ("RENAMED BEAT: " + oldName + " -> " + SanitizePatternFileName(newName))
                     : ("RENAME FAILED (NAME TAKEN?): " + newName));
    }
    void DeleteCustomPattern(const std::string& name) {
        bool ok = DeleteCustomDrumPattern(name);
        if (ok) customPatternNamesCacheValid = false;
        SetStatus(ok ? ("DELETED BEAT: " + name) : ("DELETE FAILED: " + name));
    }

    // ---- Synth presets (SYNTH tab PRESET row, Phase 4) --------------------
    // presetIndex is 0-based into the combined list the desktop PresetBrowser shows:
    // 0..AllSynthPresets().size()-1 = a built-in SynthPresets.cpp preset (v44: ~56), everything past that = a custom
    // one from CustomSynthPresetNamesCached(). Applies to whichever slot
    // is currently selected -- prSynthIndex (one of the 3 melodic synths)
    // normally, or project.chordPatch/engine.Chord() when synthViewChordSlot
    // is true (the SYNTH tab's CHORD slot, a later addition -- presets are
    // just plain SynthPatch data, so the same built-in/custom list applies
    // to the Chord track's patch with no format changes needed). Updates
    // both the PROJECT copy (what gets saved) and the live engine voice,
    // same two-copies convention as every other patch edit on this tab (see
    // SynthView::HandleInput's Up/Down handlers). Undoable as a Sound step.
    void ApplySynthPreset(int presetIndex) {
        const auto& builtins = AllSynthPresets();
        SynthPatch patch;
        std::string label;
        if (presetIndex >= 0 && presetIndex < static_cast<int>(builtins.size())) {
            patch = builtins[presetIndex].patch;
            label = builtins[presetIndex].name;
        } else {
            const auto& customNames = CustomSynthPresetNamesCached();
            int customI = presetIndex - static_cast<int>(builtins.size());
            if (customI < 0 || customI >= static_cast<int>(customNames.size())) return;
            if (!LoadCustomSynthPreset(customNames[customI], patch)) {
                SetStatus("LOAD FAILED: " + customNames[customI]);
                return;
            }
            label = customNames[customI] + " (SAVED)";
        }
        PushSoundUndo();
        if (synthViewChordSlot) {
            CurrentPattern().chordPatch = patch;
            PushEditedPatchToEngine(true, 0);
            SetStatus("CHORD: " + label);
        } else {
            CurrentPattern().synthPatches[static_cast<size_t>(prSynthIndex)] = patch;
            PushEditedPatchToEngine(false, prSynthIndex);
            SetStatus("SYNTH " + std::to_string(prSynthIndex + 1) + ": " + label);
        }
    }
    // The user-saved counterpart above dispatches to -- saves whichever
    // slot is currently selected (prSynthIndex, or the Chord patch when
    // synthViewChordSlot is true) as a new named preset. See
    // core/CustomSynthPresets.h for the on-disk format.
    void SaveCurrentSynthAsPreset(const std::string& name) {
        const SynthPatch& patch = synthViewChordSlot ? CurrentPattern().chordPatch : CurrentPattern().synthPatches[static_cast<size_t>(prSynthIndex)];
        bool ok = SaveCustomSynthPreset(name, patch);
        // So the just-saved preset shows up in the PRESET row's browse
        // list immediately, without needing a tab round-trip -- see the
        // cached-listings comment above.
        if (ok) customSynthPresetNamesCacheValid = false;
        SetStatus(ok ? ("SAVED PRESET: " + name) : ("SAVE FAILED: " + name));
    }

    // Replaces the open project with `incoming` (already read): stops what sounds, re-syncs the engine, clears the undo history.
    void AdoptProject(Project&& incoming) {
        ++docGen_;   // N1: files still being read for the old project are not put into this one
        {   // the audio thread reads the project: swap it under the lock, held only for the swap (no files are read here)
            std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
            sequencer.Panic();   // nothing from the old project may keep sounding
            project = std::move(incoming);
            sequencer.SetSongModeActive(false);
            sequencer.SetChordSceneNow(project.chordSceneEdit);
            chdLiveSlot = -1;
            engine.GetMixer().SyncFromPattern(CurrentPattern());
            engine.GetMixer().SyncFxFromProject(project);
        }
        RefreshSound();   // may decode samples from the disk: outside the lock, like every other caller (Н1 owns its own locking)
        ClearUndo();
    }
    // Opens an existing project file. Does NOT save the open one (the caller asked "Save / Don't Save / Cancel" first, see
    // RequestDoc). Returns false if the file could not be read; the open project is then untouched.
    bool LoadProjectFile(const std::string& path) {
        // Load into a scratch copy: a failed load must never leave the open project blanked (LoadProject resets its target).
        Project incoming;
        if (!LoadProject(incoming, path)) {
            SetStatus("LOAD FAILED: " + path);
            NoteLoadFailed(path);
            return false;
        }
        projectPath = path;   // first: the project's own kits and samples are read beside this file (C4b e)
        AdoptProject(std::move(incoming));
        docNewerFormat = LastLoadNewerFormat();
        MarkSaved();
        if (KitPending(project.kitFolder)) {   // N1: the kit is still being read; its own message comes when it is there
            kitStatusFolder_ = project.kitFolder;
            SetStatus(LastLoadNote().empty() ? "LOADED " + path + " (KIT LOADING)" : LastLoadNote());
            return true;
        }
        int loaded = loader.Running() ? KitSlotsLoaded(KitBank(project.kitFolder)) : LoadedKitSlots();
        SetStatus(LastLoadNote().empty() ? "LOADED " + path + " (" + std::to_string(loaded) + "/6 drum samples)"
                                         : LastLoadNote());
        NoteLoadResult(path);
        return true;
    }
    // A .wav or .aif dropped on the window (or taken in the SAMPLES browser) is ADDED to the selected DRUM track as its next sample
    // (C4b e): the kit is first copied into the project's own folder (samples/kits/<kit>, once; a kit already there is used as it
    // is), the file goes in as the next free take (snare_3.wav ...; nothing is replaced or deleted), and the drum then plays it.
    // The factory kits beside the program are never touched, and the kit travels with the project (SAVE AS copies it). One Undo
    // step brings the previous kit and sample back. An AIFF is written into the kit as a .wav.
    // N1 (LIVE-2): the file is read, the kit folder written and the new bank built on the loader thread; the scene / drum that was
    // selected when it was asked for gets it then.
    struct SlotCopy { bool ok = false; std::string error, target; int variant = 0; DrumKit::BankPtr bank; };
    static SlotCopy CopySlotFile(const std::string& wavPath, const std::string& kitName, const std::string& kitDisk, const std::string& projDir,
                                 int slotIndex, int rate, const DrumKit* kit) {   // any thread
        namespace fs = std::filesystem;
        SlotCopy r;
        SampleBuffer probe;
        if (!LoadWav(wavPath, probe, rate) || probe.empty()) { const std::string why = UnreadableReason(wavPath); r.error = why.empty() ? "CANNOT READ THAT FILE (WAV OR AIFF)" : why; return r; }
        std::error_code ec;
        std::string target = kitName, disk = kitDisk;
        if (!IsProjectKit(kitName)) {   // a factory (or older private) kit: the project gets its own copy of it
            std::string base;
            for (char c : KitShortName(kitName)) if (c != '/' && c != '\\' && c != ':') base += c;
            if (base.empty() || base == "." || base == "..") base = "kit";
            target = kProjectKitPrefix + base;
            disk = projDir + "/" + target;
            fs::create_directories(disk, ec);
            if (ec || !fs::is_directory(disk, ec)) { r.error = "CANNOT CREATE " + target; return r; }
            for (const auto& f : fs::directory_iterator(kitDisk, ec))   // missing files only: a copy made before may hold added takes
                if (f.is_regular_file() && f.path().extension() == ".wav") fs::copy_file(f.path(), fs::path(disk) / f.path().filename(), fs::copy_options::skip_existing, ec);
        }
        // the next take after the highest one there, so the takes other scenes play keep their numbers
        const std::string stem = DrumSlotFileStem(static_cast<DrumSlot>(slotIndex));
        int highest = fs::exists(fs::path(disk) / (stem + ".wav"), ec) ? 1 : 0;
        for (int v = 2; v <= kMaxKitVariants; ++v) if (fs::exists(fs::path(disk) / (stem + "_" + std::to_string(v) + ".wav"), ec)) highest = v;
        if (highest >= kMaxKitVariants) { r.error = "THIS DRUM HAS " + std::to_string(kMaxKitVariants) + " SAMPLES IN KIT " + KitShortName(target) + " ALREADY: PICK ANOTHER KIT FIRST"; return r; }
        const std::string file = highest == 0 ? stem + ".wav" : stem + "_" + std::to_string(highest + 1) + ".wav";
        if (!ImportAudioAsWav(wavPath, (fs::path(disk) / file).string())) { r.error = "CANNOT COPY THE FILE INTO " + target; return r; }
        r.bank = kit->Build(disk, rate);
        const auto& names = r.bank->names[static_cast<size_t>(slotIndex)];
        const auto at = std::find(names.begin(), names.end(), file);
        if (at == names.end()) { r.error = "CANNOT READ THE COPY IN " + target; return r; }
        r.variant = static_cast<int>(at - names.begin());
        r.ok = true; r.target = target;
        return r;
    }
    bool AssignSampleToSlot(const std::string& wavPath) {
        namespace fs = std::filesystem;
        const int trackIdx = std::clamp(seqCursorTrack, 0, kNumTracks - 1), scene = project.currentPattern;
        const Track& tr = CurrentPattern().tracks[static_cast<size_t>(trackIdx)];
        if (tr.type != TrackType::Drum) { SetStatus("SELECT A DRUM TRACK OR THE SAMPLER FIRST, THEN DROP THE FILE"); statusMessageTimer = 6.0f; return false; }
        const std::string sceneKit = TrackKitFolder(project.currentPattern, tr.slotIndex);   // v44.26: a drum with a kit of its own gets the sample in that kit
        const std::string sceneKitDisk = KitDiskPath(sceneKit), projDir = DirNameOf(projectPath);
        const bool ownKit = !tr.kitFolder.empty();
        const std::string trackName = tr.name;
        auto res = std::make_shared<SlotCopy>();
        const int slotIndex = tr.slotIndex, rate = engine.SampleRate();
        const DrumKit* kit = engine.GetKit();   // Build() is const and only reads files
        const std::string key = "kit:" + std::to_string(scene) + ":" + std::to_string(trackIdx);
        const std::string prev = ownKit ? tr.kitFolder : CurrentPattern().kitFolder;
        const unsigned tok = BumpRequest(key), gen = docGen_;
        auto kitOf = [this, scene, trackIdx, ownKit]() -> const std::string& {
            const Pattern& pt = project.patterns[static_cast<size_t>(std::clamp(scene, 0, kNumSlotsTotal - 1))];
            return ownKit ? pt.tracks[static_cast<size_t>(trackIdx)].kitFolder : pt.kitFolder;
        };
        loader.Post([res, wavPath, sceneKit, sceneKitDisk, projDir, slotIndex, rate, kit] { *res = CopySlotFile(wavPath, sceneKit, sceneKitDisk, projDir, slotIndex, rate, kit); },
                    [this, res, wavPath, scene, trackIdx, ownKit, trackName, key, prev, tok, gen, kitOf, projDir] {
            if (!res->ok) { SetStatus(res->error); statusMessageTimer = 6.0f; return; }
            if (gen != docGen_) { SetStatus("SAMPLE NOT SET: ANOTHER PROJECT IS OPEN NOW (KIT WRITTEN: " + res->target + ")"); return; }
            if (projDir != DirNameOf(projectPath)) { SetStatus("SAMPLE NOT SET: THE PROJECT WAS SAVED IN ANOTHER FOLDER MEANWHILE - ADD IT AGAIN"); statusMessageTimer = 6.0f; InvalidateFolderCaches(); return; }
            if (!RequestIsNewest(key, tok) || kitOf() != prev) { SetStatus("SAMPLE NOT SET: THE KIT WAS CHANGED MEANWHILE (KIT WRITTEN: " + res->target + ")"); InvalidateFolderCaches(); return; }
            {
                std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
                Pattern& pt = project.patterns[static_cast<size_t>(std::clamp(scene, 0, kNumSlotsTotal - 1))];
                PushUndoForScene(scene);   // one step: the kit and the sample the drum played before
                if (ownKit) pt.tracks[static_cast<size_t>(trackIdx)].kitFolder = res->target; else pt.kitFolder = res->target;
                Track& t = pt.tracks[static_cast<size_t>(trackIdx)];
                for (Step& st : t.steps) st.variant = kAutoVariant;   // every step plays the new sample, as a pick in the SAMPLE row does
                t.defaultVariant = static_cast<uint8_t>(res->variant);
            }
            InvalidateKit(res->target);
            kitCache_[KitDiskPath(res->target)] = res->bank;   // built on the loader thread with the new file: no second read
            SyncSceneKits();
            InvalidateFolderCaches();
            SetStatus(std::string("SAMPLE ADDED: ") + fs::path(wavPath).filename().string() + " -> " + trackName + " (SAMPLE " + std::to_string(res->variant + 1)
                      + " OF KIT " + KitShortName(res->target) + " IN THE PROJECT)");
        });
        if (loader.Running()) { SetStatus("READING " + fs::path(wavPath).filename().string() + "..."); return true; }
        return res->ok;   // no loader: both parts have run already
    }
    // Opens a dated copy from backups/ as the CURRENT project file's content:
    // the open file's path is kept, so the next save writes the restored state
    // there (the file it replaces is itself backed up on that save).
    // DATA-5: every write is checked; a failed one is said and the document stays "Edited" (never shown as saved when it is not).
    bool RestoreBackupFile(const std::string& backupPath) {
        Project loaded;
        if (!LoadProject(loaded, backupPath)) { SetStatus("RESTORE FAILED: " + backupPath); statusMessageTimer = 8.0f; return false; }
        const int newer = LastLoadNewerFormat();
        const bool untitled = userdirs::IsUntitledPath(projectPath);
        // keep what is open now (dated backup + .bak); never over a file from a newer version
        if (!untitled && !docNewerFormat && FileExistsNow(projectPath) && !SaveSnapshot(projectPath)) {
            SetStatus("RESTORE STOPPED: COULD NOT SAVE THE OPEN PROJECT FIRST (DISK FULL?)"); statusMessageTimer = 8.0f; return false;
        }
        AdoptProject(std::move(loaded));
        docNewerFormat = newer;
        if (newer) { MarkUnsaved(); SetStatus("RESTORED A VERSION FROM A NEWER TR-BRICK: CMD+S SAVES IT AS A COPY"); statusMessageTimer = 8.0f; return true; }
        if (!SaveSnapshot(projectPath)) { MarkUnsaved(); SetStatus("RESTORED, BUT SAVING FAILED (DISK FULL?): THE PROJECT IS NOT SAVED"); statusMessageTimer = 8.0f; return true; }
        MarkSaved();
        SetStatus("RESTORED THE VERSION OF " + BackupLabel(backupPath));
        return true;
    }
    // DATA-2: a project that could not be opened (the damaged file is kept as <path>.corrupt) can be brought back from its newest
    // dated copy: the copy becomes the project file again.
    bool RestoreFailedFromBackup() {
        const std::string path = docNoticeFailedPath;
        if (path.empty()) return false;
        // Copies of THIS file (same folder) first, then copies from before v44.59 that only share the name; newest first in each.
        std::vector<std::string> copies, legacy;
        for (const std::string& c : ListProjectBackups(path)) (IsOwnBackupName(c) ? copies : legacy).push_back(c);
        copies.insert(copies.end(), legacy.begin(), legacy.end());
        Project loaded;
        std::string used;
        for (const std::string& c : copies) if (LoadProject(loaded, c)) { used = c; break; }   // a damaged copy: try the next older one
        if (used.empty()) { SetStatus(copies.empty() ? std::string("NO DATED COPY TO RESTORE") : "RESTORE FAILED: NO COPY COULD BE OPENED"); statusMessageTimer = 8.0f; return false; }
        const int newer = LastLoadNewerFormat();
        AdoptProject(std::move(loaded));
        projectPath = path;
        projectFilesCacheValid = false;
        docNewerFormat = newer;
        docNoticeFailedPath.clear();
        if (newer || !SaveSnapshot(path)) { MarkUnsaved(); SetStatus(newer ? "RESTORED: CMD+S SAVES IT AS A COPY" : "RESTORED, BUT SAVING FAILED (DISK FULL?)"); statusMessageTimer = 8.0f; return true; }
        MarkSaved();
        SetStatus("RESTORED THE VERSION OF " + BackupLabel(used));
        return true;
    }
    // "<stem>~<8 hex>-YYYYMMDD-HHMMSS.t8proj" (a v44.59 copy, tied to the project's folder) vs the older "<stem>-YYYYMMDD-HHMMSS".
    static bool IsOwnBackupName(const std::string& p) {
        const size_t n = p.size(), at = n - 7 - 15 - 1 - 8 - 1;
        if (n < 7 + 15 + 1 + 8 + 1 + 1 || p[at] != '~' || p[n - 7 - 15 - 1] != '-') return false;
        for (size_t i = at + 1; i < at + 9; ++i) if (!std::isxdigit(static_cast<unsigned char>(p[i]))) return false;
        return true;
    }
    // ---- Document (v44.18, U8b): the classic model of Logic / GarageBand ----------------------------------------------------
    // Cmd+S saves into the project file and only then; while there are unsaved changes the title says "Edited" and a recovery copy
    // is written every few seconds into the service folder (never over the file). A project that was never saved is "Untitled"
    // and lives in the service folder until Save gives it a name. New / Open / Revert / Quit ask "Save / Don't Save / Cancel"
    // when something is unsaved (RequestDoc). "Edited" is not a flag set by every edit but a comparison of the open project
    // with the saved one, so undoing back to the saved state clears it, and nothing can forget to set it.
    enum class DocAction { None, Quit, New, Open, Revert };
    enum class NameMode { SaveAs, Rename, Duplicate };
    bool edited = false;                // updated by TickDocument (twice a second) and by RequestDoc
    DocAction docPending = DocAction::None;
    std::string docPendingPath;
    bool docAskUnsaved = false;         // the "Save / Don't Save / Cancel" dialog
    bool docNameOpen = false;           // the name dialog (Save As / Rename / Duplicate / first Save)
    NameMode docNameMode = NameMode::SaveAs;
    bool docNameResume = false;         // after the name dialog saved: go on with docPending
    bool docOpenDialog = false;         // the Open list
    std::string docRecoverPath;         // a recovery copy was found at start: the dialog asks what to do with it
    // DATA-1: the open file was written by a NEWER version (its format number; 0 = no). It may hold data this version does not know,
    // so Cmd+S never writes over it: it asks for a name and saves a copy. The file itself stays as it is.
    int docNewerFormat = 0;
    // DATA-2: what the user must be told about the last load (could not open / opened from the backup / newer version / repaired
    // values). Shown once as a dialog (DocumentDialogs); docNoticeFailedPath = the project that could not be opened at all.
    std::string docNoticeTitle, docNoticeText, docNoticeFailedPath;
    bool docNoticeCanRestore = false;   // the failed project has dated copies (set once by NoteLoadFailed, not looked up every frame)
    size_t savedHash_ = 0;
    double editedClock_ = 0.0;

    static bool FileExistsNow(const std::string& p) { struct stat st{}; return ::stat(p.c_str(), &st) == 0; }
    // The notice after a load that worked: only when there is something to tell.
    void NoteLoadResult(const std::string& path) {
        const std::string name = userdirs::ProjectDisplayName(path);
        docNoticeFailedPath.clear();
        if (LastLoadNote().rfind("RECOVERED FROM BACKUP", 0) == 0) {
            docNoticeTitle = "\"" + name + "\" was damaged and was opened from its last good save";
            docNoticeText = "The damaged file is kept next to it as \"" + name + ".t8proj.corrupt\". Check the project before you play it.";
        } else if (LastLoadNewerFormat()) {
            docNoticeTitle = "\"" + name + "\" was saved by a newer TR-Brick";
            docNoticeText = "It may hold things this version does not know. To keep them, Cmd+S does not write over the file:\nit asks for a name and saves a copy. Open the file in the newer version to keep working on it there.";
        } else if (LastLoadFixedValues()) {
            docNoticeTitle = "\"" + name + "\" had damaged values";
            docNoticeText = std::to_string(LastLoadFixedValues()) + " values were outside their range and were put back to safe values. Check the project before you play it.";
        } else { docNoticeTitle.clear(); docNoticeText.clear(); return; }
        if (LastLoadFixedValues() && LastLoadNote().rfind("RECOVERED FROM BACKUP", 0) == 0) docNoticeText += "\n" + std::to_string(LastLoadFixedValues()) + " damaged values were also put back to safe values.";
    }
    // The notice after a load that failed: the file is kept as <path>.corrupt; the dialog offers the newest dated copy.
    void NoteLoadFailed(const std::string& path) {
        const std::string name = userdirs::ProjectDisplayName(path);
        docNoticeFailedPath = path;
        docNoticeTitle = "\"" + name + "\" could not be opened";
        const std::vector<std::string> copies = ListProjectBackups(path);
        docNoticeCanRestore = !copies.empty();
        docNoticeText = "The file is damaged. It is kept as \"" + name + ".t8proj.corrupt\" and nothing was written over it.";
        docNoticeText += copies.empty() ? std::string("\nThere is no dated copy of it to go back to.")
                                        : "\nThe newest dated copy is from " + BackupLabel(copies.front()) + ".";
    }
    // Saves a copy of the open project taken under a short lock; the file is written outside the lock (LIVE-4: a save never holds
    // the sound waiting on the disk).
    bool SaveSnapshot(const std::string& path, bool keepHistory = true) {
        Project snap;
        { std::lock_guard<SceneMutex> lock(sequencer.PatternMutex()); snap = project; }
        return SaveProject(snap, path, keepHistory);
    }

    bool IsUntitled() const { return userdirs::IsUntitledPath(projectPath); }
    // What the open project looks like as text, without the scene being looked at (that is not an edit).
    // LIVE-4: the project is copied under a short lock (a memory copy) and turned into text outside it; before, the whole text was
    // built while the sound waited, twice a second.
    size_t DocHash() {
        Project snap;
        { std::lock_guard<SceneMutex> lock(sequencer.PatternMutex()); snap = project; }
        snap.currentPattern = 0; snap.chordSceneEdit = 0;
        return std::hash<std::string>{}(ProjectToString(snap));
    }
    void MarkSaved() { savedHash_ = DocHash(); edited = false; }
    void MarkUnsaved() { savedHash_ = DocHash() ^ 0x9e3779b97f4a7c15ULL; edited = true; }   // a document that must be saved as it is (recovered, loaded Untitled)
    bool IsEditedNow() { edited = DocHash() != savedHash_; return edited; }
    void TickDocument(double dt) {
        editedClock_ += dt;
        // LIVE-4: while the project is already Edited the check only looks for an undo back to the saved state, every 2 s is enough
        if (editedClock_ < (edited ? 2.0 : 0.5)) return;
        editedClock_ = 0.0;
        edited = DocHash() != savedHash_;
    }
    // The copy that protects unsaved work: the Untitled file itself, or Recovery/<name>.t8proj for a named project.
    void WriteRecovery() {
        SaveSnapshot(IsUntitled() ? projectPath : userdirs::RecoveryPathFor(projectPath), false);   // LIVE-4: written outside the lock
    }
    void DropRecovery() {
        if (IsUntitled()) { std::remove(projectPath.c_str()); return; }
        std::remove(userdirs::RecoveryPathFor(projectPath).c_str());
    }
    // Cmd+S. An Untitled project needs a name first: returns false and opens the name dialog.
    bool SaveDocument() {
        if (IsUntitled()) { docNameMode = NameMode::SaveAs; docNameOpen = true; return false; }
        if (docNewerFormat) {   // DATA-1: never over a file from a newer version: save a copy under a new name
            docNameMode = NameMode::SaveAs; docNameOpen = true;
            SetStatus("THIS FILE IS FROM A NEWER TR-BRICK: SAVE A COPY UNDER A NEW NAME, THE FILE STAYS AS IT IS"); statusMessageTimer = 8.0f;
            return false;
        }
        if (!SaveSnapshot(projectPath)) { SetStatus("SAVE FAILED: " + projectPath); statusMessageTimer = 8.0f; return false; }
        std::remove(userdirs::RecoveryPathFor(projectPath).c_str());
        MarkSaved();
        SetStatus("SAVED " + userdirs::ProjectDisplayName(projectPath));
        return true;
    }
    // SAVE AS: writes what is open right now to a NEW file named after `name` (never overwriting another project --
    // UniqueProjectPathForName adds a suffix) and makes that file the open one. The file we were editing stays exactly as it
    // was last saved; an Untitled project leaves the service folder for Projects.
    bool SaveProjectAs(const std::string& name) {
        std::string clean = name;
        size_t a = clean.find_first_not_of(' ');
        if (a == std::string::npos) { SetStatus("SAVE AS: TYPE A NAME"); return false; }
        clean = clean.substr(a, clean.find_last_not_of(' ') - a + 1);
        std::string path = UniqueProjectPathForName(clean);
        std::string oldName = project.name;
        { std::lock_guard<SceneMutex> lock(sequencer.PatternMutex()); project.name = clean; }
        if (!SaveSnapshot(path)) {
            { std::lock_guard<SceneMutex> lock(sequencer.PatternMutex()); project.name = oldName; }
            SetStatus("SAVE AS FAILED: " + path); statusMessageTimer = 8.0f;
            return false;
        }
        docNewerFormat = 0;   // the copy is this version's own file
        DropRecovery();   // the old document's copy (the Untitled file, or Recovery/<old>.t8proj)
        CopySamplesTo(path);   // v44.50: the samples of the sampler go with the project (C4b e: and its own kits)
        projectPath = path;
        projectFilesCacheValid = false;
        MarkSaved();
        SetStatus("SAVED AS " + path);
        return true;
    }
    // RENAME: the project file gets the new name (the content stays as it is; unsaved changes stay unsaved).
    bool RenameDocument(const std::string& name) {
        if (IsUntitled()) return SaveProjectAs(name);
        const std::string clean = SanitizeFileNamePart(name, "");
        if (clean.empty()) { SetStatus("RENAME: TYPE A NAME"); return false; }
        if (clean == userdirs::ProjectDisplayName(projectPath)) return true;
        const bool wasEdited = IsEditedNow();
        const std::string path = UniqueProjectPathForName(clean);
        if (std::rename(projectPath.c_str(), path.c_str()) != 0) { SetStatus("RENAME FAILED"); return false; }
        std::rename((projectPath + ".bak").c_str(), (path + ".bak").c_str());
        std::remove(userdirs::RecoveryPathFor(projectPath).c_str());
        projectPath = path;
        project.name = userdirs::ProjectDisplayName(path);
        projectFilesCacheValid = false;
        if (!wasEdited) { if (!docNewerFormat) SaveSnapshot(path); MarkSaved(); }   // a newer-version file is only renamed, never rewritten
        SetStatus("RENAMED TO " + project.name);
        return true;
    }
    bool DuplicateDocument() {
        const std::string base = IsUntitled() ? std::string("Untitled") : userdirs::ProjectDisplayName(projectPath);
        return SaveProjectAs(base + " copy");
    }
    // Cmd+N: a new Untitled project. The open one has been dealt with by the caller (RequestDoc).
    void NewDocument() {
        std::lock_guard<SceneMutex> lock(sequencer.PatternMutex());
        const std::string keepKitFolder = IsProjectKit(project.kitFolder) ? Project{}.kitFolder : project.kitFolder;   // C4b (e): a kit inside the old project's folder stays there
        Project fresh;
        fresh.name = "Untitled";
        fresh.kitFolder = keepKitFolder;
        fresh.currentPattern = 0;
        AdoptProject(std::move(fresh));
        projectPath = userdirs::UntitledPath();
        std::remove(projectPath.c_str());
        projectFilesCacheValid = false;
        docNewerFormat = 0;
        MarkSaved();
        SetStatus("NEW PROJECT");
    }
    void RevertToSaved() {
        if (IsUntitled()) return;
        std::remove(userdirs::RecoveryPathFor(projectPath).c_str());
        const std::string p = projectPath;
        if (LoadProjectFile(p)) SetStatus("REVERTED TO THE LAST SAVED VERSION");
    }
    // Throw away the unsaved work of the open document (Don't Save).
    void DiscardChanges() { DropRecovery(); }
    void PerformDoc(DocAction a, const std::string& path) {
        switch (a) {
            case DocAction::Quit: quitRequested = true; break;
            case DocAction::New: NewDocument(); break;
            case DocAction::Open: LoadProjectFile(path); break;
            case DocAction::Revert: RevertToSaved(); break;
            case DocAction::None: break;
        }
    }
    // The one door for everything that would leave the open document: runs at once when nothing is unsaved, else asks first.
    void RequestDoc(DocAction a, const std::string& path = "") {
        if (docAskUnsaved || docNameOpen) return;   // a question is already open
        if (!IsEditedNow()) {
            if (a == DocAction::Quit || a == DocAction::New) DropRecovery();   // nothing worth keeping (a pristine Untitled leaves no file)
            PerformDoc(a, path);
            return;
        }
        docPending = a;
        docPendingPath = path;
        docAskUnsaved = true;
    }
    // The answers of the unsaved dialog.
    void DocAnswerSave() {
        docAskUnsaved = false;
        if (IsUntitled()) { docNameMode = NameMode::SaveAs; docNameResume = true; docNameOpen = true; return; }
        if (SaveDocument()) { const DocAction a = docPending; docPending = DocAction::None; PerformDoc(a, docPendingPath); }
        else docPending = DocAction::None;
    }
    void DocAnswerDiscard() {
        docAskUnsaved = false;
        DiscardChanges();
        const DocAction a = docPending; docPending = DocAction::None;
        PerformDoc(a, docPendingPath);
    }
    void DocAnswerCancel() { docAskUnsaved = false; docPending = DocAction::None; }
    // The name dialog confirmed `name` for the mode it was opened in.
    void DocNameConfirm(const std::string& name) {
        bool ok = false;
        switch (docNameMode) {
            case NameMode::SaveAs: ok = SaveProjectAs(name); break;
            case NameMode::Rename: ok = RenameDocument(name); break;
            case NameMode::Duplicate: ok = SaveProjectAs(name); break;
        }
        if (!ok) return;   // the dialog stays open with the status line saying why
        docNameOpen = false;
        if (docNameResume) { docNameResume = false; const DocAction a = docPending; docPending = DocAction::None; PerformDoc(a, docPendingPath); }
    }
    void DocNameCancel() { docNameOpen = false; docNameResume = false; docPending = DocAction::None; }
    // After a crash: put the recovery copy back (the open file stays the saved state, so the document reads "Edited").
    void RestoreRecovery() {
        Project incoming;
        if (!docRecoverPath.empty() && LoadProject(incoming, docRecoverPath)) {
            AdoptProject(std::move(incoming));
            MarkUnsaved();
            SetStatus("RESTORED THE UNSAVED CHANGES");
        }
        docRecoverPath.clear();
    }
    void DiscardRecovery() { if (!docRecoverPath.empty()) std::remove(docRecoverPath.c_str()); docRecoverPath.clear(); }

    // ---- Live break (see DrumBreaks.h / Sequencer::SetLiveBreak) --------
    // Arms the currently-selected (or random) LIVE BREAK to start at the
    // next bar boundary (launch-quantized; it plays its one-bar arrangement and reverts on its own). If the transport isn't
    // already running, this also starts it (same as clicking a clip in
    // Ableton Live's Session View starts the transport) -- otherwise
    // "press to trigger a break" while stopped would silently do nothing,
    // since a break with no playing pattern underneath it has nothing to
    // launch into.
    void ArmLiveBreak() {
        const auto& all = AllDrumBreaks();
        int idx = rndBreakIndex;
        if (idx <= 0) { // "RANDOM" -- pick a fresh one for this trigger
            std::uniform_int_distribution<int> dist(0, static_cast<int>(all.size()) - 1);
            idx = dist(RngEngine());
        } else {
            idx = std::min(idx - 1, static_cast<int>(all.size()) - 1);
        }
        sequencer.SetLiveBreak(idx); // arm -- starts at the next bar boundary
        if (!sequencer.IsPlaying()) {
            sequencer.Play(); // tick 0 IS a bar boundary, so it starts right away
        }
        SetStatus(std::string("LIVE BREAK QUEUED: ") + all[idx].name);
    }

    // Cancels a pending (not yet started) or in-progress break immediately.
    // NOT used by the hold-Y live trigger above (which always runs to
    // completion on its own once it starts) -- only by RandomView's LIVE
    // BREAK row B=CANCEL, for backing out of a break you queued by mistake.
    void DeactivateLiveBreak() {
        sequencer.ClearLiveBreak();
    }

    // ---- Riser one-shot trigger (live performance, sixth revision) --------
    // Bar-synced while the transport is running (see Sequencer::
    // SetPendingRiser): a riser is a one-off layered sweep dropped in on
    // top of whatever's already playing (see RiserPlayer::Render, which
    // just adds into the master bus every block), not a pattern
    // substitution -- but for it to actually "fit into the bars" alongside
    // the drum pattern rather than starting wherever the pad happened to
    // get hit, the START needs to land on a downbeat, exactly like
    // ArmLiveBreak's launch-quantization above. With the transport stopped
    // there's no bar clock to quantize against (FireTick, which promotes a
    // pending riser, only runs while playing), so this falls back to firing
    // immediately -- matches how a riser already behaved with playback
    // stopped, and avoids a press silently doing nothing.
    // Uses rndRiserIndex's "0 = RANDOM" convention exactly like
    // rndBreakIndex/ArmLiveBreak; the random case never repeats the
    // immediately-previous riser, same never-repeat convention as the drum
    // swap actions (see lastSwapGenreIndex above).
    int lastRiserIndex = -1;
    // Which bar line is the drop: index into kRiserDropBars (1 = next bar, 2 / 4 / 8 bars ahead).
    static constexpr int kRiserDropBars[4] = {1, 2, 4, 8};
    int riserDropIdx = 0;

    void TriggerRiser() {
        int n = engine.Risers().Count();
        if (n <= 0) { SetStatus("NO RISERS LOADED"); return; }
        int idx = rndRiserIndex;
        if (idx <= 0) { // "RANDOM" -- pick a fresh one for this trigger
            if (n == 1) {
                idx = 0;
            } else {
                std::uniform_int_distribution<int> dist(0, n - 1);
                idx = dist(RngEngine());
                if (idx == lastRiserIndex) idx = (idx + 1) % n; // never repeat
            }
        } else {
            idx = std::min(idx - 1, n - 1);
        }
        for (int guard = 0; guard < n && engine.Risers().NameAt(idx).empty(); ++guard) idx = (idx + 1) % n;   // v44.51: a missing stock file or an empty user slot is never picked
        lastRiserIndex = idx;
        if (sequencer.IsPlaying()) {
            const int bars = kRiserDropBars[std::clamp(riserDropIdx, 0, 3)];
            sequencer.SetPendingRiser(idx, bars);   // back-timed: its peak lands on the drop
            SetStatus(std::string("RISER ") + engine.Risers().NameAt(idx) + " -> DROP IN " + std::to_string(bars) + (bars == 1 ? " BAR" : " BARS"));
        } else {
            // Stopped: no bar clock, so just play it from the start at native speed.
            engine.Risers().TriggerAligned(idx, 0.0, 0.0, 1.0);
            SetStatus(std::string("RISER: ") + engine.Risers().NameAt(idx));
        }
    }

    // Cancels a riser that's armed (queued for the next bar) but hasn't
    // actually started playing yet -- PERFORM tab's RISER row B press, see
    // RandomView.cpp. Symmetrical with DeactivateLiveBreak above; a no-op
    // if nothing is currently pending.
    void CancelPendingRiser() {
        sequencer.ClearPendingRiser();
    }

    // ---- Note export (plain-text, tracker-style grid) ---------------------
    // Menu tab's EXPORT NOTES row (see MenuView.cpp) -- writes the CURRENT
    // pattern's 6 drum tracks (as short k/s/c/hc/ho/p codes, see
    // NotesExport.h's DrumAbbrev) followed by its 4 melodic tracks (Synth
    // 1/2/3 + Chord, as human-readable "C-4"/"C#4"/"---" note names) out as
    // one grid, timestamped the same way Recorder's WAV takes are (see
    // Recorder::Toggle) so repeated exports never collide or silently
    // overwrite each other. This is a read-only dump for looking at/
    // sharing a pattern's content outside the app -- see Persistence.h for
    // the actual project save/load format.
    void ExportCurrentPatternNotes() {
        std::string path = userdirs::RecordingPath(" notes", "txt");   // v44.14: Documents/TR-Brick/Recordings, "<project> <date> notes.txt"
        Pattern exportPat = CurrentPattern();
        if (project.chordBankOn) {   // the chord track of the export = the chord scene being edited
            Track& ct = exportPat.tracks[kChordTrackIndex];
            const Track& sc = EditChordScene().track;
            ct.steps = sc.steps; ct.length = sc.length; ct.useOwnLength = sc.useOwnLength;
        }
        if (ExportPatternNotesText(exportPat, path)) {
            userdirs::SetLastOutput(path);
            SetStatus("EXPORTED NOTES -> " + userdirs::DisplayPath(path));
        } else {
            SetStatus("EXPORT FAILED: " + path);
        }
    }

    // ---- Live MIDI capture (record a live performance, then export it as
    // the same tracker-style grid EXPORT NOTES writes) ----------------------
    // Captures whatever ACTUALLY plays through the audio engine while
    // armed -- programmed pattern steps, LIVE BREAK fills, ratchet
    // retriggers, AND live NOTE PLAY keyboard notes alike -- the same
    // "whatever really sounds" scope Recorder has for audio, achieved the
    // same way Recorder achieves it: AudioEngine::Capture() (see
    // MidiCapture.h) is hooked directly into TriggerDrum/SynthNoteOn/
    // ChordNoteOn, the one shared choke point every sound in the app
    // already passes through, and its clock is advanced sample-accurately
    // inside AudioEngine::Render -- so it is sample-accurate and not
    // subject to wall-clock jitter. AppState just starts/stops it
    // and exports whatever it collected; it holds no capture state of its
    // own beyond the bpm snapshot needed to quantize the export.
    bool midiCaptureActive = false;
    float midiCaptureBpm_ = 120.0f; // pattern bpm snapshotted at Start(), used to quantize the export grid

    void StartMidiCapture() {
        midiCaptureBpm_ = CurrentPattern().bpm;
        engine.Capture().Start();
        midiCaptureActive = true;
        SetStatus("MIDI CAPTURE STARTED -- PLAY OR RUN THE SEQUENCER");
    }

    // Writes whatever got captured out via ExportMidiCaptureText -- unlike
    // the old Pattern-buffer approach, the grid grows to fit however long
    // the performance actually ran instead of being bounded to kMaxSteps.
    void StopMidiCaptureAndExport() {
        midiCaptureActive = false;
        engine.Capture().Stop();
        size_t eventCount = engine.Capture().Events().size();
        if (eventCount == 0) {
            SetStatus("MIDI CAPTURE STOPPED (NOTHING CAPTURED)");
            return;
        }
        std::string midPath = userdirs::RecordingPath(" capture", "mid");
        std::string path = midPath.substr(0, midPath.size() - 4) + ".txt";   // the same name for the text grid
        bool midOk = ExportMidiCaptureSmf(engine.Capture(), midiCaptureBpm_, midPath);
        if (ExportMidiCaptureText(engine.Capture(), midiCaptureBpm_, path)) {
            userdirs::SetLastOutput(midOk ? midPath : path);
            SetStatus("CAPTURED " + std::to_string(eventCount) + " EVENTS -> " + userdirs::DisplayPath(midOk ? midPath : path));
        } else {
            SetStatus("CAPTURE EXPORT FAILED: " + path);
        }
    }

    void ToggleMidiCapture() {
        if (midiCaptureActive) StopMidiCaptureAndExport(); else StartMidiCapture();
    }

    // ---- Master repeater ------------------------------------------------------
    // Thin wrapper around Mixer::repeaterWantOn with a status message.
    // Deliberately no PushUndo: like the master HPF/LPF it is a momentary
    // performance gesture, not an undoable pattern edit.
    void ToggleRepeater() {
        Mixer& mx = engine.GetMixer();
        mx.repeaterWantOn = !mx.repeaterWantOn;
        // while playing the change lands on the next step / beat (see Sequencer::ApplyRepeaterRequest)
        SetStatus(mx.repeaterWantOn ? (sequencer.IsPlaying() ? "STUTTER ARMED" : "STUTTER ON") : "STUTTER OFF");
    }


    // ---- Master filter sweep (0..1) -------------------------------------------
    void SetMasterHpfAmount(float value01) {
        engine.GetMixer().masterHpfAmount = std::clamp(value01, 0.0f, 1.0f);
    }
    void SetMasterLpfAmount(float value01) {
        engine.GetMixer().masterLpfAmount = std::clamp(value01, 0.0f, 1.0f);
    }



};

} // namespace t8
