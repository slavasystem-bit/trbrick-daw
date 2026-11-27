#pragma once
// The whole saved unit of work: 16 pattern slots (banks) + the 3 synth
// patches (project-wide, like an instrument's patch memory) + which kit
// folder is loaded. Patterns hold note/step data only.
#include "Types.h"
#include "RhythmRandom.h"
#include <algorithm>
#include <array>
#include <memory>
#include <string>

namespace t8 {

// v44.22: all scene slots (16 live + 64 brick slots) live on the heap, so a Project stays small enough to sit on the stack and to
// be copied around; the array never moves or grows after construction, so references into it stay valid. Copy only (no move), so a
// Project is never left without its slots.
class PatternBank {
public:
    PatternBank() : p_(new Pattern[kNumSlotsTotal]()) {}
    PatternBank(const PatternBank& o) : PatternBank() { *this = o; }
    PatternBank& operator=(const PatternBank& o) { if (this != &o) std::copy(o.begin(), o.end(), begin()); return *this; }
    Pattern& operator[](size_t i) { return p_[i]; }
    const Pattern& operator[](size_t i) const { return p_[i]; }
    Pattern* begin() { return p_.get(); }
    Pattern* end() { return p_.get() + kNumSlotsTotal; }
    const Pattern* begin() const { return p_.get(); }
    const Pattern* end() const { return p_.get() + kNumSlotsTotal; }
    static constexpr size_t size() { return static_cast<size_t>(kNumSlotsTotal); }
private:
    std::unique_ptr<Pattern[]> p_;
};

// LIVE-5: everything of a project except its scene slots, so the screen can copy a project in short pieces (CopyProjectInPieces).
struct ProjectHead {
    std::string name = "New Project";
    std::string kitFolder = "kits/acid_v2";   // the kit of every scene whose own Pattern::kitFolder is empty
    // v44.5.4: the sound lives in each Pattern (Pattern::synthPatches / chordPatch / kick / kitFolder). These are only the
    // BASE sound: what a file without per-scene sounds is upgraded from, and what the project starts with. The editor and the
    // engine never read them.
    std::array<SynthPatch, kNumSynthTracks> baseSynthPatches{};
    // Phase 6: the dedicated Chord track's own patch -- same SynthPatch
    // struct as the 3 melodic synths (full engine choice + filter/ADSR/LFO),
    // kept as a separate field rather than a 4th slot in synthPatches above
    // so the melodic randomizers/Piano Roll (both hard-scoped to
    // kNumSynthTracks) never need to reason about it. Edited via the
    // CHORDS tab's PATCH mode (see ChordsView.cpp), which reuses the exact
    // same SynthParams.cpp row-list machinery as the Synth tab.
    SynthPatch baseChordPatch;
    FxSettings fx; // project-wide delay/reverb/distortion settings
    // Master bus volume (Mixer tab's MASTER column) -- project-wide like fx
    // above, not per-pattern. Mirrored into the live Mixer::masterVolume on
    // every edit and on load/new/switch (see Mixer::SyncFxFromProject) so
    // it round-trips through Save/Load the same way fx does; default
    // matches Mixer::masterVolume's own default.
    float masterVolume = 0.4f;
    int currentPattern = 0;
    // v44.22: the LIVE scene (0..kNumLiveScenes-1) the LIVE tab shows. currentPattern may point at a brick slot while the
    // TRACK tab edits a brick; liveScene remembers where to come back to.
    int liveScene = 0;
    // v44.58: LINKED scenes. sceneLink[i] = 0: scene i stands alone; n > 0: scenes with the same n are one group and share mute and solo
    // (a change in one is made in all, and moving between them leaves the mixer as it is). Saved with the project.
    std::array<uint8_t, kNumLiveScenes> sceneLink{};
    // v44.23: which parts of the track's sound are defined (bits kSoundSynth0.. of AppState); the sound itself is patterns[kTrackSoundSlot].
    int trackSoundMask = 0;

    // RANDOM tab's KEY/SCALE picker -- project-wide (like fx above), applies
    // to all 3 melodic randomizers (bass + synth 2/3). 0=C by default;
    // scale index into Scales.h's kScaleNames, 1=Dorian by default.
    // FOLLOW KEY: when on, changing the project key / scale re-tunes the written
    // chords automatically (off by default: chords stay exactly as written).
    bool chordsFollowKey = false;
    // Chord bank: independent chord scenes launched separately from the pattern
    // scenes (see ChordScene in Types.h). Off = the chord track of each pattern
    // plays as before. chordSceneEdit = the slot shown / edited in the UI; the
    // slot that is PLAYING is the sequencer's (it follows quantized launches).
    bool chordBankOn = false;
    int chordSceneEdit = 0;
    std::array<ChordScene, kNumChordScenes> chordScenes{};
    int randomizerKeyRoot = 0;
    int randomizerScale = 1;
    // v44.3: which octaves the melodic randomizers may use, per part (BASS, SYNTH 2, SYNTH 3): bit n = octave n (C0..B7).
    // Defaults are the old fixed ranges: bass 1-2, synths 3-5. Saved with the project.
    std::array<uint8_t, 3> rndOctaves{{0x06, 0x38, 0x38}};
    // v44.4: settings of the RHYTHM randomizer per part (BASS, SYNTH 2, SYNTH 3): notes per bar, which notes, how long they hold.
    std::array<RhythmRnd, 3> rndRhythm{{
        RhythmRnd{1, 3, RhythmPitch::Root, RhythmLen::Mix, true, true},
        RhythmRnd{2, 5, RhythmPitch::Scale, RhythmLen::Mix, true, true},
        RhythmRnd{2, 5, RhythmPitch::Scale, RhythmLen::Mix, true, true}}};

    // Song/chain mode -- see ChainStep in Types.h. songChainLength is how
    // many of the kMaxChainSteps entries below are actually programmed
    // (0 = no chain, song mode has nothing to play). Whether song mode is
    // currently ACTIVE (driving playback right now) is separate, ephemeral
    // state owned by Sequencer, not saved here -- same as Pattern step data
    // (persisted) vs. Sequencer::playing_ (not persisted, always starts
    // stopped).
    std::array<ChainStep, kMaxChainSteps> songChain{};
    int songChainLength = 0;

    // Loads/saves a simple human-readable key=value text format (see
    // Persistence.cpp for the format description). Returns false on failure.
};

struct Project : ProjectHead {
    PatternBank patterns;   // the scene slots (16 live + the brick slots)
};

} // namespace t8
