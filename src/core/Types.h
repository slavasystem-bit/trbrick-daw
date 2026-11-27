#pragma once
// Core data types shared across the engine: steps, tracks, patterns, project.
// Kept as plain structs (no virtual dispatch) so the audio thread can copy
// small chunks of this cheaply and predictably.

#include "ArrangeTypes.h"
#include <algorithm>
#include <cctype>
#include <array>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>

namespace t8 {

constexpr int kMaxSteps        = 64;   // "up to 64 steps" per the spec
constexpr int kNumDrumTracks   = 6;    // Kick, Snare, Clap, Closed HH, Open HH, Perc
constexpr int kNumSynthTracks  = 3;
// Phase 6: one dedicated, genuinely POLYPHONIC track for chords (up to 4
// simultaneous notes -- see Step::chordSize/chordNotes below and
// ChordVoice.h) -- separate from, and untouched by, the 3 monophonic synth
// tracks above. Kept to exactly 1 for now (see ChordVoice's voice-pool
// size) rather than generalizing to N chord tracks -- one simultaneous
// chord/pad voice covers the overwhelming majority of groovebox use, and
// keeping this a single fixed slot (like the drum slots) avoids a second
// dynamic-count UI surface on top of the 3 synth tracks'.
constexpr int kNumChordTracks  = 1;
// v44.50: the SAMPLER is the 11th track, placed LAST so every older index (0..9) and every older project stays as it was.
constexpr int kNumSamplerTracks = 1;
constexpr int kNumTracks       = kNumDrumTracks + kNumSynthTracks + kNumChordTracks + kNumSamplerTracks; // 11
constexpr int kChordTrackIndex = kNumDrumTracks + kNumSynthTracks;   // 9
constexpr int kSamplerTrackIndex = kChordTrackIndex + 1;             // 10
// v44.22 (S1a): two kinds of scene slots share one array. The first kNumLiveScenes are the LIVE scenes (the jam sketch pad,
// like a TR-8 pattern bank). The next kNumBrickSlots are hidden slots, one per brick of the track: THROW copies the scene
// into a free brick slot, so a brick keeps its own notes and later edits of the LIVE scene do not change it.
constexpr int kNumLiveScenes   = 16;
constexpr int kNumBrickSlots   = 64;   // == kMaxChainSteps (static_assert below the chain definition)
constexpr int kNumPatterns     = kNumLiveScenes + kNumBrickSlots;   // every slot: live scenes + brick slots
// v44.23 (S1b): one more hidden slot after them holds the SOUND OF THE TRACK (only its sound parts matter: synth / chord patches, kick, kit,
// drum voices). It is never played and never listed by the loops over kNumPatterns.
constexpr int kTrackSoundSlot  = kNumPatterns;
constexpr int kNumSlotsTotal   = kNumPatterns + 1;
constexpr int kSampleRate      = 44100;
constexpr int kMaxKitVariants  = 8;    // alt takes per drum slot (kick_2.wav, kick_3.wav, ...)

// Chord = Phase 6's dedicated polyphonic track (see kNumChordTracks above
// and Step::chordSize/chordNotes) -- a genuine 3rd track type alongside
// Drum/Synth, not a special slotIndex on TrackType::Synth, so the existing
// per-synth-track code (Piano Roll's single-note editing, the 3 melodic
// randomizers, SynthNoteOn/Off's kNumSynthTracks-sized arrays) stays
// completely untouched and doesn't need to reason about multi-note steps.
enum class TrackType : uint8_t { Drum = 0, Synth = 1, Chord = 2, Sampler = 3 };
// v44.50: tracks whose steps carry ONE pitch (Step::note): the three synths and the sampler. The piano roll, note editing and the note
// field of the step editor treat them alike.
inline bool IsNoteTrackType(TrackType t) { return t == TrackType::Synth || t == TrackType::Sampler; }

// C2 part 3b: ChordOutputMode / ArpStyle (the CHORDS tab's baked STACK / ARPEGGIO) are gone: see ChordPlay below.

enum class DrumSlot : uint8_t {
    Kick = 0, Snare = 1, Clap = 2, ClosedHat = 3, OpenHat = 4, Perc = 5
};

inline const char* DrumSlotName(DrumSlot s) {
    switch (s) {
        case DrumSlot::Kick:      return "Kick";
        case DrumSlot::Snare:     return "Snare";
        case DrumSlot::Clap:      return "Clap";
        case DrumSlot::ClosedHat: return "ClosedHH";
        case DrumSlot::OpenHat:   return "OpenHH";
        case DrumSlot::Perc:      return "Perc";
    }
    return "?";
}
// Short filename stem used to find sample files in a kit folder, e.g. "kick.wav" / "kick_2.wav"
inline const char* DrumSlotFileStem(DrumSlot s) {
    switch (s) {
        case DrumSlot::Kick:      return "kick";
        case DrumSlot::Snare:     return "snare";
        case DrumSlot::Clap:      return "clap";
        case DrumSlot::ClosedHat: return "chh";
        case DrumSlot::OpenHat:   return "ohh";
        case DrumSlot::Perc:      return "perc";
    }
    return "?";
}

enum class Waveform : uint8_t { Sine = 0, Triangle = 1, Saw = 2, Square = 3 };
inline const char* WaveformName(Waveform w) {
    switch (w) {
        case Waveform::Sine:     return "Sine";
        case Waveform::Triangle: return "Tri";
        case Waveform::Saw:      return "Saw";
        case Waveform::Square:   return "Sqr";
    }
    return "?";
}

// One step in a track's pattern. Drum tracks use {active, accent, variant};
// synth tracks additionally use {note, lengthSteps, slide}.
constexpr uint8_t kAutoVariant = 255;   // Step::variant meaning "use the track's default sample"

// Chord tones one step can carry (v44.43: six, was four) and the size of the ChordVoice pool.
constexpr int kMaxChordVoices = 6;

struct Step {
    bool    active   = false;
    bool    accent   = false;   // louder / brighter hit, TR-8 style
    uint8_t variant  = 255;     // which alt sample to play (drum tracks) - index into kit variants; 255 = AUTO: the track's defaultVariant
    int8_t  note     = 60;      // MIDI note number (synth tracks), 60 = C4
    uint8_t lengthSteps = 1;    // note length in steps (synth tracks, for tied/held notes)
    bool    slide    = false;   // glide into this note from the previous one (synth tracks, 303-style)
    // Ratchet / step-repeat: 1 = a normal single hit (the default, and the
    // only value that existed before this field). 2/3/4 = retrigger this
    // step that many times, evenly spaced within its own duration, without
    // spilling into the next step -- the classic TR-8/Elektron "roll" for
    // quick live fills. See Sequencer::FireTick/RenderAndAdvance for the
    // sample-accurate sub-step scheduling this drives.
    uint8_t ratchet  = 1;        // 1..4

    // Phase 6: Chord track only (TrackType::Chord) -- ignored by Drum/Synth
    // steps exactly the way `note` is already ignored by Drum steps above.
    // chordSize=0 means "nothing to play" even if `active` is true (the
    // Sequencer's Chord branch checks this, not `active` alone, so an
    // accidentally-toggled-on empty chord step stays silent instead of
    // firing a phantom note) -- 1..6 means that many of chordNotes[] sound
    // together, using the SAME shared `active`/`accent`/`lengthSteps`/
    // `slide`/`ratchet` fields above for timing/feel, just like a normal
    // synth step. chordNotes defaults to a plain C major triad (root
    // position) purely so a step manually toggled on from the Sequencer
    // tab (which seeds chordSize=3 on first toggle -- see SequencerView.cpp)
    // has a sensible sound rather than silence; the Chords tab overwrites
    // both fields wholesale when applying a real progression.
    uint8_t chordSize = 0;                       // 0..kMaxChordVoices (v44.43: six voices, was four)
    std::array<int8_t, kMaxChordVoices> chordNotes{60, 64, 67, 71, 0, 0}; // MIDI notes, ascending

    // Step probability: the odds this step actually fires when its turn
    // comes up, checked once per trigger against a fresh random roll (see
    // Sequencer::FireTick). 100 = the old, only-ever-existing behavior
    // (always fires if active) -- every pre-existing pattern/save loads
    // with every step still firing every time, byte-for-byte the same as
    // before this field existed. Restricted to the 5 values a live
    // performer actually reaches for (100/75/50/25/0) rather than a free
    // 0-100 range, so it stays a single quick D-pad cycle to dial in
    // rather than a fiddly percentage; 0 is a legitimate value (step is
    // toggled on but never actually plays until raised back up -- useful
    // as a "silenced but still there" placeholder while composing).
    uint8_t probability = 100;   // one of 100/75/50/25/0

    // v39 microtiming: the hit is moved by nudge percent of a step (+ = later / laid back, - = earlier / pushed).
    // Negative values work on drum tracks only (the sequencer looks one tick ahead for them); synth and chord
    // tracks are clamped to >= 0. Stored in whole percent, -50..+50.
    int8_t nudge = 0;
    // v39 ghost note: a quiet in-between hit (velocity ~0.45, so the velocity-tone of the drums and synths makes it
    // darker as well). Mutually exclusive with accent.
    bool   ghost = false;
    // v40 free velocity 1..127 (the velocity lane of the drum / piano-roll editor). 0 = not set: the step then plays the
    // classic way (accent 1.0, ghost 0.45, else 0.8). When set it wins over both flags.
    uint8_t vel = 0;
};

constexpr float kDefaultVelocity = 0.8f;
constexpr float kGhostVelocityValue = 0.45f;
inline float StepVelocity(const Step& s) {
    if (s.vel > 0) return static_cast<float>(s.vel) / 127.0f;
    return s.accent ? 1.0f : (s.ghost ? kGhostVelocityValue : kDefaultVelocity);
}
// What the grid shows: a free velocity counts as accent when it is at the top and as ghost when it is very soft.
inline bool StepIsAccent(const Step& s) { return s.vel > 0 ? s.vel >= 124 : s.accent; }
inline bool StepIsGhost(const Step& s) { return s.vel > 0 ? s.vel <= 60 : (s.ghost && !s.accent); }

// v44.52: how the sample of an instrument is played. The same six settings, the same card and the same ranges on a drum track and on the
// sampler (the sampler's patch carries them under its own names).  Defaults leave a drum sample exactly as recorded.
struct SampleEdit {
    float start = 0.0f;       // 0..0.95: where in the sample the sound starts (fraction of its length)
    float length = 1.0f;      // 0.02..1: how much of the rest plays (1 = to the end), faded out at the end
    float attackMs = 0.0f;    // 0..200; 0 = the sample's own start (drums keep their 0.6 ms click guard)
    int8_t fineCents = 0;     // -100..100
    int8_t gainDb = 0;        // -24..12
    bool reverse = false;
    bool IsDefault() const { return start == 0.0f && length == 1.0f && attackMs == 0.0f && fineCents == 0 && gainDb == 0 && !reverse; }
    bool operator==(const SampleEdit& o) const { return start == o.start && length == o.length && attackMs == o.attackMs && fineCents == o.fineCents && gainDb == o.gainDb && reverse == o.reverse; }
    bool operator!=(const SampleEdit& o) const { return !(*this == o); }
};

struct Track {
    TrackType type = TrackType::Drum;
    std::string name;
    int slotIndex = 0; // DrumSlot index if type==Drum, synth index (0-2) if type==Synth

    std::array<Step, kMaxSteps> steps{};
    int length = 16;          // 1..kMaxSteps, this track's own pattern length
    bool useOwnLength = false; // if false, follow the pattern's master length instead

    // v44.55: EUCLID ON - the melody (or beat) of this track is kept in `euclidOrig` (only its active steps, with their step index) and the
    // steps of the track are DERIVED from it: the notes in order are spread over `euclidHits` hits evenly placed on `euclidSteps` steps
    // (rotated by `euclidRotate`), cycling through the notes when there are more hits than notes. Turning it off puts the original back.
    // See core/Euclid.h (EuclidMelodyApply) and AppState::EuclidMelodyOn / EuclidMelodyOff.
    bool euclidOn = false;
    uint8_t euclidHits = 3, euclidSteps = 8;
    int8_t euclidRotate = 0;
    std::vector<std::pair<uint8_t, Step>> euclidOrig;

    // v43.1: the sample variant (0-based) that steps without their own choice play -- new steps, genre beats, randomizers.
    // The default kit (acid_v2) starts with kick 1, snare 2, hat 1, perc 3.
    uint8_t defaultVariant = 0;
    // v44.26: a drum instrument can have a kit of its own (a folder like "kits/808"); empty = it plays from the kit of the scene.
    std::string kitFolder;

    // v39: this track's own swing (0..0.75) instead of the pattern's; < 0 = follow the pattern swing.
    float swing = -1.0f;
    bool  grooveOn = true;   // v42: this track follows the pattern's groove

    float volume = 0.8f;  // 0..1
    float pan    = 0.0f;  // -1..1
    bool  mute   = false;
    bool  solo   = false;

    // Drum-track-only voice shaping (ignored for synth tracks).
    float drumTune  = 0.0f; // semitones, -24..+24
    float drumDecay = 1.0f; // 0.25..2.0, see DrumVoiceParams::decay
    SampleEdit edit;        // v44.52: START / LENGTH / ATTACK / FINE / GAIN / REVERSE of the sample (same as the sampler's)

    // Effects sends -- how much of this track's signal feeds the shared
    // delay/reverb buses (see FxSettings below and AudioEngine::Render).
    // 0 = fully dry. Per-track like volume/pan, so different sends per
    // pattern bank are possible (e.g. more reverb on a build-up pattern).
    float delaySend  = 0.0f; // 0..1
    float reverbSend = 0.0f; // 0..1

    // Per-track INSERT effects -- applied to this track's own signal
    // pre-fader (before volume/pan), unlike delaySend/reverbSend above
    // which tap the POST-fader signal into the shared aux buses. See
    // TrackInsertFx in Effects.h for the DSP. All default to 0 (off) so
    // existing projects sound unchanged until a user dials one in.
    float insertDrive  = 0.0f; // 0..1, per-track saturation (same curve as the master-bus DRIVE)
    float insertEqTilt = 0.0f; // -1..1, one-knob tilt EQ: negative=darker, positive=brighter, 0=flat
    float insertChorus = 0.0f; // 0..1, short modulated-delay chorus mix

    // Autopan -- a 4th insert stage, own LFO rather than reusing the
    // chorus LFO above (independent rate makes more sense for a stereo
    // sweep than sharing a ~10-15ms chorus-tuned modulator). Applied to
    // this track's `pan` in Mixer::MixTrack (added to the static pan
    // value, clamped back to -1..1), not inside TrackInsertFx, since pan
    // is already a per-block Mixer-level concept and doesn't need its own
    // buffer stage. Both 0 by default (0 depth = no audible movement
    // regardless of rate) so existing projects are unaffected.
    float autopanRate  = 1.0f; // 0.05..8Hz, LFO speed
    float autopanDepth = 0.0f; // 0..1, how far the pan swings each way

    // Stereo image (v33, see Stereo.h): `width` scales the side signal (0 = mono, 1 = as is, 2 = double);
    // `spread` adds a decorrelated, mono-compatible side signal built from the (high-passed) mid, which
    // turns a mono source (hats, percussion, a plain synth voice) into a stereo one without touching the mono sum.
    float width  = 1.0f;       // 0..2
    float spread = 0.0f;       // 0..1

    // Channel EQ (v29, see TrackEq.h): HPF + low shelf + mid bell + high shelf.
    // Defaults are flat / off, so existing projects sound identical.
    float eqHpfHz   = 20.0f;   // 20 = off .. 500
    float eqHpfSlope = 0.0f;   // 0 = 12 dB/oct, 1 = 24 dB/oct
    float eqLowDb   = 0.0f;    // -15..+15
    float eqLowHz   = 120.0f;  // 30..500
    float eqMidDb   = 0.0f;    // -15..+15
    float eqMidHz   = 1000.0f; // 80..10000
    float eqMidQ    = 1.0f;    // 0.3..6
    float eqHighDb  = 0.0f;    // -15..+15
    float eqHighHz  = 6000.0f; // 1500..16000
};

// Project-wide (not per-pattern) effects settings -- one shared delay
// engine and one shared reverb engine for the whole set, plus a master-bus
// distortion/drive stage. Individual tracks control how much of their own
// signal reaches the delay/reverb via Track::delaySend/reverbSend above;
// these are the "how the effect itself sounds" knobs, deliberately global
// rather than per-pattern so dialing in a good delay/reverb once holds
// across the whole set instead of needing to be redone per pattern bank.
// Tempo-synced delay time divisions (FX tab's DELAY TIME row) -- fraction
// of a WHOLE NOTE (4 beats/one 4-4 bar), the standard way delay divisions
// are expressed. Straight divisions down to 1/16, plus 1/3, 1/6 and 1/12
// for a triplet feel and 3/4 for a dotted-half-ish stretch -- covers what
// a live performer would actually reach for without an overwhelming list.
// Longest first so DELAY TIME's Left/Right cycling reads top-to-bottom as
// "slower -> faster", matching how the row's value visibly shortens.
struct DelayDivision { const char* label; float wholeNoteFraction; };
constexpr DelayDivision kDelayDivisions[] = {
    {"1/1", 1.0f}, {"3/4", 0.75f}, {"1/2", 0.5f}, {"1/3", 1.0f / 3.0f},
    {"1/4", 0.25f}, {"1/6", 1.0f / 6.0f}, {"1/8", 0.125f}, {"1/12", 1.0f / 12.0f}, {"1/16", 0.0625f},
};
constexpr int kNumDelayDivisions = 9;
// Index 6 ("1/8") is the default -- at the default 120bpm this comes out
// to 0.25s, exactly matching FxSettings::delayTimeSec's old fixed default,
// so a fresh project sounds identical to before this became tempo-synced.
constexpr int kDefaultDelayDivisionIndex = 6;

// Converts a kDelayDivisions index + the current tempo into actual
// seconds for DelayEffect::Process to use. Clamped well inside
// DelayEffect's ~2.0s hard buffer ceiling (see Effects.cpp) so even an
// extreme combination (1/1 at a very low bpm) degrades to a long-but-safe
// echo instead of silently over-running the delay line.
inline float DelayDivisionSeconds(int index, float bpm) {
    index = std::clamp(index, 0, kNumDelayDivisions - 1);
    float safeBpm = std::max(20.0f, bpm);
    float secondsPerBeat = 60.0f / safeBpm;
    float seconds = kDelayDivisions[index].wholeNoteFraction * 4.0f * secondsPerBeat;
    return std::clamp(seconds, 0.02f, 1.9f);
}

// Repeater/stutter density (live performance; see Mixer::repeaterDensityIndex).
// Expressed as a fraction of ONE STEP (a 16th note, same "step" every other
// per-step feature in this app means) rather than a whole note like the
// delay divisions above -- a repeater's captured window is meant to be
// short (glitchy), so anchoring to the smallest existing musical unit
// keeps the numbers intuitive: "1" repeats a whole step, "1/16" repeats a
// sixteenth of one step, a very fast micro-stutter.
struct RepeaterDensity { const char* label; float stepFraction; };
constexpr RepeaterDensity kRepeaterDensities[] = {
    {"1", 1.0f}, {"1/2", 0.5f}, {"1/4", 0.25f}, {"1/8", 0.125f}, {"1/16", 0.0625f},
    // Longer capture windows (beat-repeat of 2 / 4 / 6 / 8 whole steps) --
    // appended AFTER the original five so the existing index order is unchanged.
    {"2", 2.0f}, {"4", 4.0f}, {"6", 6.0f}, {"8", 8.0f},
};
constexpr int kNumRepeaterDensities = 9;
// Order the desktop LIVE picker lists them in (shortest window first).
constexpr int kRepeaterDensityDisplayOrder[kNumRepeaterDensities] = {4, 3, 2, 1, 0, 5, 6, 7, 8};

// Master bus (see MasterChain.h, Limiter.h): the "finished track" chain
// (HPF -> 3-band tone -> tape-style saturation -> compressor -> stereo width -> mono bass),
// then master volume, the optional master DRIVE and the always-present look-ahead true-peak limiter.
struct MasterChainSettings {
    bool  chainOn   = false;   // HPF / tone / saturation / width / limiter
    int   chainPreset = -1;    // last preset applied (kMasterPresetNames), -1 = custom
    float hpfHz     = 20.0f;   // 20 = off
    float lowDb     = 0.0f;    // low shelf  @ 120 Hz, -9..+9
    float midDb     = 0.0f;    // bell       @ 1.5 kHz, -9..+9
    float highDb    = 0.0f;    // high shelf @ 9 kHz,  -9..+9
    float width     = 1.0f;    // 0 = mono, 1 = as is, 2 = extra wide
    float saturation= 0.0f;    // 0..1 soft tape / tube saturation
    bool  limiterOn = true;    // look-ahead true-peak limiter at the ceiling (independent of chainOn)
    float ceilingDb = -1.0f;   // -6..0 dBTP
    float monoBassHz = 120.0f; // everything below this is summed to the centre (0 = off); independent of chainOn

    bool  compOn    = false;   // the compressor works on its own, chain on or off
    int   compPreset= -1;      // GLUE / PUNCH / SOFT, -1 = custom
    float compThreshDb = -16.0f; // -40..0
    float compRatio = 2.0f;    // 1..10
    float compAttackMs = 25.0f;  // 1..100
    float compReleaseMs = 180.0f;// 20..800
    float compMakeupDb = 1.5f; // 0..12
    float compMix   = 1.0f;    // 0..1, below 1 = parallel compression
    float compScHpfHz = 80.0f; // high-pass in the detector only, so the bass does not trigger the pumping (20 = off)
};
constexpr int kNumCompPresets = 3;
constexpr int kNumMasterPresets = 4;
extern const char* const kCompPresetNames[kNumCompPresets];     // GLUE, PUNCH, SOFT
extern const char* const kMasterPresetNames[kNumMasterPresets];  // CLEAN, WARM, LOUD, LO-FI
void ApplyCompPreset(MasterChainSettings& m, int preset);
void ApplyMasterPreset(MasterChainSettings& m, int preset);

// Built-in kick (v34): a sine with an exponential pitch sweep and a short click, instead of (or on top of) the kit's
// sample. Project-wide like the rest of FxSettings.
struct KickSynthSettings {
    int   mode = 0;           // 0 = SAMPLE (kit kick only), 1 = SYNTH (built-in kick only), 2 = LAYER (both)
    float pitchHz = 52.0f;    // fundamental the sweep settles on, 35..90 Hz (ignored while keyLock is on)
    bool  keyLock = true;     // tune the fundamental to the project key's root note (40..80 Hz range)
    float sweep = 4.0f;       // start pitch = fundamental x sweep (1..10)
    float sweepMs = 40.0f;    // how fast the pitch falls, 5..150 ms
    float decayMs = 380.0f;   // time to -60 dB, 80..1200 ms
    float click = 0.30f;      // 0..1 noise transient
    float drive = 0.25f;      // 0..1 saturation of the body
    float level = 0.85f;      // 0..1, layer / synth level
};

struct FxSettings {
    // The actual value DelayEffect::Process reads, and what gets written
    // to the save file -- but no longer hand-tweaked directly. Continuously
    // recomputed from delayDivisionIndex (below) and the CURRENT pattern's
    // bpm every audio block (see Sequencer::RenderAndAdvance), so the
    // delay always tracks tempo changes/pattern switches automatically
    // instead of drifting out of sync with the music. An old save's raw
    // delayTimeSec is honored for exactly one block before being
    // overwritten by that sync, which is fine -- delayDivisionIndex's own
    // default reproduces the old fixed default at the old fixed bpm
    // anyway (see kDefaultDelayDivisionIndex above).
    float delayTimeSec   = 0.25f;
    // Which musical division DELAY TIME is set to -- see kDelayDivisions.
    // This, not delayTimeSec, is what the FX tab's DELAY TIME row actually
    // edits (Left/Right or, in this row's case, Up/Down cycles through
    // kDelayDivisions -- see FxView.cpp).
    int   delayDivisionIndex = kDefaultDelayDivisionIndex;
    float delayFeedback  = 0.35f;  // 0..0.9, how many repeats decay out
    float delayLevel     = 0.35f;  // 0..1, master return gain (0 = bypassed even if tracks send to it)
    float delayTone      = 0.7f;   // v37: 0..1, brightness of the repeats (0 dark, 1 bright)
    bool  delayPingPong  = false;  // v37: repeats bounce left / right
    float reverbSize     = 0.5f;   // 0..0.97, room size / decay length
    float reverbLevel    = 0.35f;  // 0..1, master return gain (0 = bypassed)
    float reverbDamp     = 0.5f;   // v37: 0..1, how fast the highs die away in the tail (0 bright, 1 dark)
    float reverbPreMs    = 12.0f;  // v37: 0..120 ms pre-delay
    float distortionDrive= 0.0f;   // 0..1, master-bus saturation amount

    // Kick -> Bass sidechain ducking (FX tab's MASTER FX mode). Every Kick
    // hit instantly ducks the bass synth (synth track 0) down to
    // (1 - sidechainAmount) of its normal level, then releases linearly
    // back to full volume over sidechainRelease seconds -- the classic
    // "pumping" effect a four-on-the-floor kick gives a bassline. See
    // AudioEngine::sidechainGain_ for the actual envelope; this struct only
    // holds the user-facing knobs (project-wide, like the rest of
    // FxSettings, not per-pattern).
    bool  sidechainEnabled = false;
    float sidechainAmount  = 0.5f;  // 0..1, how deep the duck goes (0 = no effect even if enabled)
    float sidechainRelease = 0.2f;  // 0.02..1.0s, time to recover back to full volume

    MasterChainSettings master;     // master compressor + "finished track" chain
    KickSynthSettings kick;         // built-in kick synth (Kick track)
};

// The sample variant a drum step really plays.
inline int StepVariant(const Track& t, const Step& s) { return s.variant == kAutoVariant ? t.defaultVariant : s.variant; }

// v42 groove: a bar of 16 steps, each with a timing push (percent of a step, +- 50) and a velocity offset (in 1/127 units).
// It is a live layer on the pattern (Pattern::groove + grooveAmount): the sequencer adds amount * groove to every step of the
// tracks that have `grooveOn`, so the amount can be moved while playing and the steps themselves stay untouched
// (BAKE writes it into the steps). It repeats every 16 ticks, whatever the pattern length.
struct GrooveStep { int8_t nudge = 0; int8_t vel = 0; };
struct Groove {
    std::string name;
    std::array<GrooveStep, 16> steps{};
    bool IsFlat() const { for (const auto& g : steps) if (g.nudge != 0 || g.vel != 0) return false; return true; }
};

constexpr int kGridChoices[4] = {3, 4, 6, 8};          // valid Pattern::stepsPerBeat values
inline const char* GridLabel(int spb) { return spb == 8 ? "1/32" : spb == 6 ? "1/16T" : spb == 3 ? "1/8T" : "1/16"; }
inline int ClampGrid(int spb) { for (int g : kGridChoices) if (g == spb) return spb; return 4; }

// ---- Synth patch (moved above Pattern in v44.5.4: every scene carries its own sound) ----
// What an LFO (see SynthPatch below) modulates, if anything. Off is the
// default/fast-path -- SubtractiveSynth::Render skips the modulation math
// entirely when a patch's lfoTarget is Off, so patches that don't use
// this cost nothing extra. WtPosition (Phase 5b) is meaningful only for
// the Wavetable engine -- SubtractiveSynth/FMSynth simply never match it
// in their own target switches, so it's a safe no-op on those two engines
// (same convention FMSynth already established for Cutoff being a no-op
// there: shared field, per-engine relevance).
enum class LfoTarget : uint8_t { Off = 0, Cutoff = 1, Pitch = 2, Volume = 3, WtPosition = 4 };
inline const char* LfoTargetName(LfoTarget t) {
    switch (t) {
        case LfoTarget::Off:        return "OFF";
        case LfoTarget::Cutoff:     return "CUTOFF";
        case LfoTarget::Pitch:      return "PITCH";
        case LfoTarget::Volume:     return "VOLUME";
        case LfoTarget::WtPosition: return "WT POS";
    }
    return "?";
}
constexpr int kNumLfoTargets = 5;
// The one reader of what LfoTargetName writes (project files and preset files). v44.44: before, each reader had its own list
// and neither knew "WT POS", so an LFO on the wavetable position came back as OFF after a save. Case does not matter.
inline LfoTarget LfoTargetFromText(const std::string& s) {
    auto same = [](const std::string& a, const char* b) {
        size_t i = 0; for (; i < a.size() && b[i]; ++i) if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i]))) return false;
        return i == a.size() && b[i] == 0;
    };
    for (int i = 0; i < kNumLfoTargets; ++i) if (same(s, LfoTargetName(static_cast<LfoTarget>(i)))) return static_cast<LfoTarget>(i);
    return LfoTarget::Off;
}

// Phase 5: which DSP engine a given synth patch plays through. Lives ON
// SynthPatch (not a separate parallel array) so per-synth-slot engine
// choice reuses the existing "one patch per synth slot, project-wide"
// persistence/UI model -- see SynthVoice for the dispatch wrapper that
// actually reads this at NoteOn/Render time.
enum class SynthEngineType : uint8_t { Subtractive = 0, FM = 1, Wavetable = 2, Sample = 3 };   // v44.53: SAMPLE plays a sample of the project at the pitch of the note
constexpr int kNumSynthEngineTypes = 4;
inline const char* SynthEngineTypeName(SynthEngineType t) {
    switch (t) {
        case SynthEngineType::Subtractive: return "SUBTRACTIVE";
        case SynthEngineType::FM:          return "FM";
        case SynthEngineType::Wavetable:   return "WAVETABLE";
        case SynthEngineType::Sample:      return "SAMPLE";
    }
    return "?";
}

// One FM operator: a pure-sine phase-modulation oscillator. `ratio`
// multiplies the voice's base frequency (so ratio=1 tracks the played
// note, ratio=2 is an octave up, ratio=0.5 an octave down, etc. -- the
// classic DX7-style "coarse" control, here left continuous rather than
// integer-snapped for a wider timbral range). `level` is 0..1 and means
// different things depending on the operator's role in the currently
// selected algorithm (see FMSynth): output-volume weight for a carrier,
// or phase-modulation-index scale for a modulator. See FMSynth.h for the
// 4 fixed algorithms and how ratio/level are actually consumed.
struct FMOperator {
    float ratio = 1.0f;  // 0.25..16.0 -- wide enough for classic high-ratio
                          // bell/e-piano modulator ratios (e.g. 11, 14)
                          // without reaching all the way to a full DX7's
                          // 31.99 ceiling, which this simplified engine
                          // doesn't need.
    float level = 0.5f;  // 0..1
};

// v44.53: what the SAMPLE engine of a synth / chord track plays and how. Plain data (no std::string) so that a SynthPatch stays trivially
// copyable: the audio thread copies and compares patches (memcmp). The file is a name inside <project folder>/samples/; empty = none.
// The meaning of the fields is the one of SamplerPatch (see below); ATTACK / sustain / release come from the synth's own envelope.
struct SynthSample {
    static constexpr int kFileMax = 96;
    char file[kFileMax] = {};
    std::array<uint16_t, 32> marks{};   // HITS marks in 1/65535 of the whole sample, ascending
    float start = 0.0f;                 // 0..0.95
    float decay = 1.0f;                 // LENGTH: 0.02..1
    int8_t root = 60;
    int8_t tuneCents = 0;
    int8_t gainDb = 0;
    int8_t fitBars = -1;                // -1 off, 0 AUTO, 1 / 2 / 4 / 8 / 16
    uint8_t mode = 0;                   // 0 ONE-SHOT, 1 SLICES, 2 LOOP (a note starts the loop from its beginning)
    uint8_t slices = 8;                 // 4 / 8 / 16 / 32, 0 = HITS
    uint8_t markCount = 0;
    uint8_t reverse = 0;
    // Writes the name (cut to what fits, the rest of the array cleared so that memcmp of two patches stays meaningful).
    void SetFile(const std::string& f) {
        std::memset(file, 0, sizeof(file));
        std::memcpy(file, f.data(), std::min<size_t>(f.size(), kFileMax - 1));
    }
    std::string File() const { size_t n = 0; while (n < kFileMax && file[n]) ++n; return std::string(file, n); }
    bool HasFile() const { return file[0] != 0; }
};

struct SynthPatch {
    Waveform waveform = Waveform::Saw;
    float cutoffHz   = 900.0f;   // filter cutoff
    float resonance  = 0.15f;    // 0..0.99
    float envAmount  = 0.5f;     // filter envelope amount, 0..1
    float attack     = 0.005f;   // seconds
    float decay      = 0.15f;    // seconds
    float sustain    = 0.6f;     // 0..1
    float release    = 0.08f;    // seconds
    float filterDecay= 0.2f;     // seconds, separate decay for the filter envelope (acid-style)
    float glideTime  = 0.0f;     // seconds, portamento
    float volume     = 0.50f;    // matches the Mixer tab's default synth-channel fader level

    // Live automation LFO (Phase 3) -- one simple, free-running LFO per
    // synth voice, modulating a single selectable parameter continuously
    // during playback. Deliberately just 4 knobs (target/rate/depth/
    // shape), not a step-sequenced automation lane -- see
    // SubtractiveSynth::Render for how each target is actually applied.
    LfoTarget lfoTarget = LfoTarget::Off;
    float lfoRateHz = 2.0f;             // 0.05..20 Hz
    float lfoDepth  = 0.3f;             // 0..1, how strongly it modulates the target
    Waveform lfoShape = Waveform::Sine; // reuses the oscillator's own Waveform enum/shapes

    // Phase 5: which engine this patch plays through, and the FM-specific
    // parameters (ignored by SubtractiveSynth, and vice versa for the
    // fields above being ignored by FMSynth -- both engines share the
    // ADSR/glide/volume/LFO fields above so switching engineType on an
    // existing patch keeps its envelope/LFO feel intact). Defaults keep
    // every pre-Phase-5 patch (presets, saved projects) unchanged.
    SynthEngineType engineType = SynthEngineType::Subtractive;
    int fmAlgorithm = 0;              // 0..3, see FMSynth::kNumFmAlgorithms
    std::array<FMOperator, 4> fmOps{};
    float fmIndexScale = 8.0f;        // v44: modulation range in CYCLES per unit of modulator level (the phase is added in cycles, so 8 = up to 50 rad: the original, very dense
                                      // range, kept for old patches; 0.25..2 gives the usual FM bell / piano / bass indices)
    float fmModEnv = 0.0f;            // v44: 0..1 how much the modulation depth follows the amp envelope (0 = constant, as before; 1 = the sound gets
                                      // darker as the note fades -- what makes FM bells and electric pianos ring instead of buzzing)

    // Phase 5b: Wavetable-engine-specific parameters (ignored by the other
    // two engines). `wavetableBank` selects which built-in single-cycle
    // wavetable set to scan through (see Wavetables.h), `wtPosition` is
    // where in that bank to read from (0=first frame, 1=last frame,
    // continuously interpolated in between -- the classic wavetable
    // "scan" knob). WavetableSynth reuses cutoffHz/resonance/envAmount/
    // filterDecay above for its own post-oscillator filter -- same filter
    // block as SubtractiveSynth, just fed by a wavetable lookup instead of
    // a formulaic waveform -- so those fields do double duty across both
    // engines rather than needing a separate copy.
    int wavetableBank = 0;     // 0..kNumWavetableBanks-1
    float wtPosition = 0.0f;   // 0..1

    // v26 "thickening" -- every default means OFF, so older patches / projects sound exactly as before.
    // Shared by all three engines (applied around the engine in SynthVoice):
    int   unisonVoices  = 1;      // 1..kMaxUnisonVoices detuned copies of the voice
    float unisonDetune  = 0.25f;  // 0..1  (1 = the outer copies are +-40 cents)
    float unisonSpread  = 0.7f;   // 0..1  stereo width of the copies
    float subLevel      = 0.0f;   // 0..1  sine / square oscillator below the note
    int   subOctave     = 1;      // 1 or 2 octaves below
    float noiseLevel    = 0.0f;   // 0..1
    float drive         = 0.0f;   // 0..1  soft saturation after the filter
    bool  phaseSync     = false;  // v30: restart the oscillator phases on every new note (with a ~1.5 ms declick duck)
    // Subtractive engine only:
    Waveform osc2Wave   = Waveform::Saw;
    float osc2Level     = 0.0f;   // 0..1  second oscillator (0 = off)
    int   osc2Semi      = 0;      // -24..+24
    int   osc2Detune    = 7;      // cents -50..+50
    bool  filter24      = false;  // false = 12 dB / oct, true = 24 dB / oct
    bool  acid          = false;  // v37: ACID filter (diode-style, accent opens the filter, linear 60 ms slide); overrides filter24
    float drift         = 0.0f;   // v37: 0..1 slow analog wander of pitch and cutoff (all engines' pitch is untouched, see SubtractiveSynth)
    float keytrack      = 0.0f;   // 0..1  cutoff follows the played note
    // v44.53: the SAMPLE engine (engineType == Sample): the sample and how it plays. Ignored by the other engines.
    SynthSample sample;
};

// v44.50: the sound of the SAMPLER track. One sample per scene (a file name inside the project's samples/ folder; empty = none).
// ONE-SHOT plays the sample at the pitch of the step's note (`root` plays it unchanged); later modes add slices and loops.
struct SamplerPatch {
    std::string file;            // name inside <project folder>/samples/, empty = no sample
    int8_t root = 60;            // MIDI note that plays the sample at its original pitch
    float start = 0.0f;          // 0..0.95, where in the sample the sound starts (fraction of its length)
    float attackMs = 1.0f;       // 0..200
    float decay = 1.0f;          // 0.02..1: fraction of the sample that plays (1 = to the end), faded out at the end
    bool poly = false;           // false: a new hit cuts the previous one (mono); true: up to 4 hits sound together
    bool reverse = false;
    int8_t tuneCents = 0;        // v44.51: -100..100 (fine tune, on top of the note)
    int8_t gainDb = 0;           // v44.51: -24..12 (library samples come at any level)
    // v44.52: ONE-SHOT (a note plays the sample at its pitch), SLICES (the played part is cut into `slices` equal parts: ROOT plays the
    // first, ROOT+1 the second ...; no transposing) and LOOP (the sample plays by itself with the scene, over whole bars).
    uint8_t mode = 0;            // 0 ONE-SHOT, 1 SLICES, 2 LOOP
    uint8_t slices = 8;          // 4, 8, 16 or 32 equal parts, or 0 = HITS: the parts run between the marks below (v44.52.1)
    // v44.52.1: marks of the HITS slices, in 1/65535 of the whole sample, ascending; slice i runs from mark i to mark i + 1 (the last to the
    // end of the played part). Found by the program (FIND HITS) and corrected by hand on the waveform.
    uint8_t markCount = 0;
    std::array<uint16_t, 32> marks{};
    int8_t fitBars = -1;         // -1 = native speed, 0 = AUTO (the nearest whole number of bars at the project tempo), 1 / 2 / 4 / 8 / 16 bars
    bool operator==(const SamplerPatch& o) const {
        return file == o.file && root == o.root && start == o.start && attackMs == o.attackMs && decay == o.decay && poly == o.poly && reverse == o.reverse && tuneCents == o.tuneCents && gainDb == o.gainDb
            && mode == o.mode && slices == o.slices && fitBars == o.fitBars && markCount == o.markCount && std::equal(marks.begin(), marks.begin() + markCount, o.marks.begin());
    }
    bool operator!=(const SamplerPatch& o) const { return !(*this == o); }
};

// v44.53: the SAMPLE engine of a synth keeps its settings in a SynthSample (plain data); the editor and the voice work with a SamplerPatch.
// ATTACK is the synth's own envelope there (attackMs 0), the voice is mono (a new note always restarts the sample).
inline void FillSamplerPatch(SamplerPatch& p, const SynthSample& s) {   // everything but the file name (no allocation: for the audio thread)
    p.root = s.root; p.start = s.start; p.attackMs = 0.0f; p.decay = s.decay; p.poly = false; p.reverse = s.reverse != 0;
    p.tuneCents = s.tuneCents; p.gainDb = s.gainDb; p.mode = s.mode; p.slices = s.slices; p.markCount = s.markCount; p.marks = s.marks; p.fitBars = s.fitBars;
}
inline SamplerPatch ToSamplerPatch(const SynthSample& s) { SamplerPatch p; FillSamplerPatch(p, s); p.file = s.File(); return p; }
inline void FromSamplerPatch(SynthSample& s, const SamplerPatch& p) {
    s.SetFile(p.file); s.root = p.root; s.start = p.start; s.decay = p.decay; s.reverse = p.reverse ? 1 : 0;
    s.tuneCents = p.tuneCents; s.gainDb = p.gainDb; s.mode = p.mode; s.slices = p.slices; s.markCount = p.markCount; s.marks = p.marks; s.fitBars = p.fitBars;
}

// C2 (CHORD LAB v2, the PLAY layer): HOW the chords of a scene sound. The chord steps hold only WHAT sounds (the notes and how long);
// the sequencer plays them through these settings at playback, so the loop, a live chord and the lab all go through one performer.
// The default (STACK, gate 100 %, 1 octave, voices as written) plays exactly as before the layer existed.
enum class ChordPlayMode : uint8_t { Stack = 0, Arp = 1, Strum = 2, Rhythm = 3 };
constexpr int kNumChordPlayModes = 4;
inline const char* ChordPlayModeName(int m) {
    static const char* const k[kNumChordPlayModes] = {"STACK", "ARP", "STRUM", "RHYTHM"};
    return k[std::clamp(m, 0, kNumChordPlayModes - 1)];
}
constexpr int kNumChordPlayRates = 5;
inline const char* ChordPlayRateName(int r) {
    static const char* const k[kNumChordPlayRates] = {"1/32", "1/16", "1/8", "1/4", "1/2"};
    return k[std::clamp(r, 0, kNumChordPlayRates - 1)];
}
inline double ChordPlayRateBeats(int r) {   // a quarter note is one beat
    static const double k[kNumChordPlayRates] = {0.125, 0.25, 0.5, 1.0, 2.0};
    return k[std::clamp(r, 0, kNumChordPlayRates - 1)];
}
constexpr int kNumChordPlayOrders = 4;
inline const char* ChordPlayOrderName(int o) {
    static const char* const k[kNumChordPlayOrders] = {"UP", "DOWN", "UP-DN", "RND"};
    return k[std::clamp(o, 0, kNumChordPlayOrders - 1)];
}
struct ChordPlay {
    uint8_t mode = 0;      // ChordPlayMode
    uint8_t rate = 1;      // index into ChordPlayRate*: ARP / RHYTHM = one hit per rate, STRUM = the time from the first note to the last
    uint8_t gate = 100;    // 10..100 %: how long a note holds of its hit (STACK / STRUM: of the chord)
    uint8_t octaves = 1;   // 1..3: ARP climbs this many octaves; the other modes double the chord this many octaves up (6 voices at most)
    uint8_t order = 0;     // ARP and STRUM: UP / DOWN / UP-DN / RND
    bool smooth = false;   // SMOOTH VOICES: each chord takes the inversion nearest the chord before it
    bool IsDefault() const { return mode == 0 && gate == 100 && octaves == 1 && !smooth; }
    bool operator==(const ChordPlay& o) const { return mode == o.mode && rate == o.rate && gate == o.gate && octaves == o.octaves && order == o.order && smooth == o.smooth; }
    bool operator!=(const ChordPlay& o) const { return !(*this == o); }
    void Clamp() {
        mode = static_cast<uint8_t>(std::min<int>(mode, kNumChordPlayModes - 1));
        rate = static_cast<uint8_t>(std::min<int>(rate, kNumChordPlayRates - 1));
        gate = static_cast<uint8_t>(std::clamp<int>(gate, 10, 100));
        octaves = static_cast<uint8_t>(std::clamp<int>(octaves, 1, 3));
        order = static_cast<uint8_t>(std::min<int>(order, kNumChordPlayOrders - 1));
    }
};

struct Pattern {
    std::string name = "PTN";
    int masterLength = 16; // 1..kMaxSteps
    float bpm = 120.0f;
    float swing = 0.0f;    // 0..0.75, delays every 2nd 16th note
    StepParams params;     // v44.39: parameter rows of a LIVE scene (the same as a brick's), over one pass of the scene; a brick's own are in its ChainStep
    Groove groove;               // v42: the groove layer (timing + velocity per step of the bar)
    float grooveAmount = 0.0f;   // 0..1; 0 = no groove
    // v43 grid: steps per beat. 4 = 1/16 notes (the classic grid), 8 = 1/32, 6 = 1/16 triplets, 3 = 1/8 triplets.
    // A bar is 4 beats = 4 * stepsPerBeat steps. BPM always counts quarter notes.
    int stepsPerBeat = 4;
    // Key / scale the chord track (and, after an ADJUST, the melodic tracks) of
    // this scene were written in; -1 = unknown. Changing the project key does
    // NOT touch written chords -- "ADJUST TO SCALE" uses this as the source.
    int8_t chordsKey = -1;
    int8_t chordsScale = -1;
    ChordPlay chordPlay;         // C2: how the chord track of this scene is played (the PLAY layer)
    // v44.5 arrangement habits of the scene: its role (a tag) and the fill it usually ends with.
    SceneRole role = SceneRole::None;
    FillRule fill;
    // v44.5.4: the SOUND belongs to the scene - the three synth patches, the chord patch, the built-in kick and the drum kit
    // (kitFolder empty = the project's kit, Project::kitFolder). Drum tune / decay already live in the tracks.
    std::array<SynthPatch, kNumSynthTracks> synthPatches{};
    SynthPatch chordPatch;
    SamplerPatch samplerPatch;   // v44.50: the sample of the sampler track and how it plays
    KickSynthSettings kick;
    std::string kitFolder;
    // v44.24: for a brick slot, the parts of the sound (bit = part, see SoundParts.h) that are the brick's OWN: they do not follow the sound
    // of the track. 0 = the brick follows the track in everything.
    uint16_t ownSound = 0;
    std::array<Track, kNumTracks> tracks{};

    Pattern();
};

// Which of the 16 groove positions (sixteenth notes of the bar) a tick belongs to.
// ---- Lengths in musical time (v44.4) -------------------------------------------------------------------------------------
// A scene loops for `masterLength` steps; a step is a beat / stepsPerBeat, so on the triplet grids 64 steps are not a whole number
// of bars. BPM counts quarter notes. A chain slot plays the scene `repeats` times.
inline double PatternBeats(const Pattern& p) { return static_cast<double>(std::max(1, p.masterLength)) / ClampGrid(p.stepsPerBeat); }
inline double PatternBars(const Pattern& p) { return PatternBeats(p) / 4.0; }
inline double PatternSeconds(const Pattern& p) { return PatternBeats(p) * 60.0 / std::max(20.0f, p.bpm); }

inline size_t GroovePos(const Pattern& p, int tick) { return static_cast<size_t>(((tick < 0 ? 0 : tick) * 4) / std::max(1, p.stepsPerBeat)) & 15u; }

// Where inside its own step a hit sounds, as a fraction of a step (v39 / v41): the swing offset (odd steps, the track's own
// swing if it has one) plus the step's nudge. Negative values (pulled ahead) are honoured on drum tracks only. Shared by the
// sequencer and the editor previews so they can never disagree.
inline double StepHitOffset(const Track& t, const Pattern& p, int tick, const Step& s) {
    const float sw = std::clamp(t.swing >= 0.0f ? t.swing : p.swing, 0.0f, 0.75f);
    // swing delays every second 16th note: on the 1/16 grid that is every odd step (half of the swing, in steps), on the
    // 1/32 grid every odd pair of steps (twice as many steps long). Triplet grids have their own shuffle and take no swing.
    double off = 0.0;
    if (p.stepsPerBeat == 4) off = (tick & 1) ? sw * 0.5 : 0.0;
    else if (p.stepsPerBeat == 8) off = ((tick >> 1) & 1) ? static_cast<double>(sw) : 0.0;
    off += static_cast<double>(s.nudge) / 100.0;
    if (t.grooveOn && p.grooveAmount > 0.0f) off += static_cast<double>(p.grooveAmount) * p.groove.steps[GroovePos(p, tick)].nudge / 100.0;
    const double lo = t.type == TrackType::Drum ? -0.5 : 0.0;
    return std::clamp(off, lo, 0.95);
}

// The velocity a step really plays with: its own value plus the groove layer.
inline float StepPlayVelocity(const Track& t, const Pattern& p, int tick, const Step& s) {
    float v = StepVelocity(s);
    if (t.grooveOn && p.grooveAmount > 0.0f) v += p.grooveAmount * static_cast<float>(p.groove.steps[GroovePos(p, tick)].vel) / 127.0f;
    return std::clamp(v, 0.02f, 1.0f);
}

// ---- Chord bank -----------------------------------------------------------
// Independent chord "scenes": with Project::chordBankOn the chord track plays
// the steps of the SELECTED chord scene instead of the ones stored in the
// pattern, so chords can be launched (quantized) separately from the drum /
// bass scenes and the harmony keeps sounding while the groove changes.
// Only the steps / length live here; volume, pan, sends and mute of the chord
// track still come from the pattern's own chord track.
constexpr int kNumChordScenes = 8;
struct ChordScene {
    std::string name;
    Track track;                 // type Chord; steps + length / useOwnLength
    int8_t key = -1;             // key / scale this progression was written in (-1 unknown)
    int8_t scale = -1;
    ChordScene();
};

// ---- Song/chain mode ------------------------------------------------------
// An ordered sequence of pattern banks to auto-play in order, each held for
// a number of full loops before advancing to the next -- the classic
// grid-groovebox "song mode" arrangement feature, distinct from (and not
// replacing) manual scene switching, which stays available at all times. See Sequencer::SetSongModeActive/AdvanceSongChain
// for the playback side and ChainView for the editing UI. This struct
// (and Project::songChain/songChainLength below) is the persisted CHAIN
// DEFINITION; whether song mode is actively driving playback right now is
// separate, ephemeral transport state (see Sequencer), same relationship
// as a pattern's step data (persisted) vs. Sequencer::playing_ (not).
constexpr int kMaxChainSteps = 64;
static_assert(kNumBrickSlots == kMaxChainSteps, "one hidden slot per possible brick");
// Unison copies per synth voice (UNISON 1..16).
constexpr int kMaxUnisonVoices = 16;
struct ChainStep {
    // v44.38: constructors instead of aggregate initialisation: with the parameter rows the struct got big enough for GCC 13 (-O2) to
    // build ChainStep{1, 1, 2} wrongly (the fields after the second came out as zero). Same call sites, same meaning.
    ChainStep() = default;
    ChainStep(int pattern, int rep = 1, int chord = -1) : patternIndex(pattern), repeats(rep), chordScene(chord) {}
    int patternIndex = 0; // 0..kNumPatterns-1, which pattern bank to play
    int repeats = 1;      // 1..99, how many full loops of it before advancing
    int chordScene = -1;  // -1 = leave the chord bank alone, 0..7 = switch the chord bank to this scene when this step starts
    // v44.5: overlays on the scene, none of them changes its steps.
    uint16_t muteMask = 0;                  // bit i = track i is silent while this step plays (see ArrangeTypes.h)
    FillMode fillMode = FillMode::Scene;    // the scene's own fill / no fill / the rule below
    FillRule fill;                          // used when fillMode == Own
    int8_t riser = -1;                      // -1 = none, else the index of a riser / transition sound that lands on the first beat of this step
    uint8_t riserBars = 2;                  // how many bars before the step it starts building (1, 2, 4, 8)
    uint8_t trimSteps = 0;                  // 0 = the whole scene per pass; N = every pass is only N steps long
    StepAuto automation;                    // v44.6: level and filter sweeps over the step (see ArrangeTypes.h)
    StepLevels levels;                      // v44.13: level of every instrument, of the transition into the step and of its fill
    StepParams params;                      // v44.38: parameter rows (pan, sends, tune, decay, cutoff, resonance over the step)
};
static_assert(kArrTracks == kNumTracks, "StepLevels must have one lane per track");
// v44.38: ONE mute for a brick. The instruments a brick switches off are the bits of its muteMask (WHO PLAYS, the M and S buttons of the
// sequencer on TRACK, the keys). A scene's own mute / solo flags are the LIVE jam: when a scene becomes a brick they are folded into the
// mask (a solo = every other instrument off) and cleared on the brick's scene.
inline uint16_t SceneMuteMask(const Pattern& p) {
    uint16_t m = 0, solo = 0;
    for (int t = 0; t < kNumTracks; ++t) {
        if (p.tracks[static_cast<size_t>(t)].mute) m |= static_cast<uint16_t>(1u << t);
        if (p.tracks[static_cast<size_t>(t)].solo) solo |= static_cast<uint16_t>(1u << t);
    }
    if (solo) m |= static_cast<uint16_t>(kMuteAllTracks & ~solo);
    return m;
}
inline void ClearSceneMuteFlags(Pattern& p) { for (auto& t : p.tracks) { t.mute = false; t.solo = false; } }
// Steps in one pass of a chain step (the trim, if any, is never longer than the scene), and its length in musical time.
inline int ChainStepLoopSteps(const Pattern& p, const ChainStep& s) {
    const int len = std::max(1, p.masterLength);
    return s.trimSteps > 0 ? std::min<int>(len, s.trimSteps) : len;
}
inline double ChainStepBeats(const Pattern& p, const ChainStep& s) { return static_cast<double>(ChainStepLoopSteps(p, s)) * std::max(1, s.repeats) / ClampGrid(p.stepsPerBeat); }
inline double ChainStepBars(const Pattern& p, const ChainStep& s) { return ChainStepBeats(p, s) / 4.0; }
inline double ChainStepSeconds(const Pattern& p, const ChainStep& s) { return ChainStepBeats(p, s) * 60.0 / std::max(20.0f, p.bpm); }

struct DrumVoiceParams {
    float tune  = 0.0f;   // semitones, -24..+24
    float decay = 1.0f;   // 1.0 = the sample as recorded; below 1 shortens its tail, above 1 lifts the tail (0.25..2.0)
    float level = 0.9f;   // 0..1
    int   keyRoot = -1;   // project key root (0 = C ... 11 = B) for the kick synth's key lock, -1 = unknown
    SampleEdit edit;      // v44.52: where the sample starts / ends, attack, fine tune, gain, direction
};

} // namespace t8
