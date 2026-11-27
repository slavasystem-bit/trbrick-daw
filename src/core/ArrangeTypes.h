#pragma once
// Arrangement overlays (v44.5): what a CHAIN STEP can do on top of the scene it plays, and what a scene carries as its own habits.
//  - ROLE of a scene (INTRO / BUILD / DROP / BREAK / OUTRO): a tag, used for colours and the energy line.
//  - FILL rule: a drum fill that plays over the last beats of a loop, without touching the steps of the scene.
//  - MUTE MASK of a chain step: the tracks that are silent while the step plays (a break = kick and bass out).
//  - TRANSITION: a riser / impact / downlifter that lands on the first beat of the step.
//  - TRIM: the loop of the step is shorter than the scene.
// Everything here is plain data plus pure functions (testable without the audio engine).
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace t8 {

enum class SceneRole : uint8_t { None = 0, Intro = 1, Build = 2, Drop = 3, Break = 4, Outro = 5 };
constexpr int kNumRoles = 6;
inline const char* RoleName(SceneRole r) {
    switch (r) { case SceneRole::Intro: return "INTRO"; case SceneRole::Build: return "BUILD"; case SceneRole::Drop: return "DROP"; case SceneRole::Break: return "BREAK"; case SceneRole::Outro: return "OUTRO"; default: return "-"; }
}

// When the fill plays. LAST LOOP = only on the final pass of the chain step (in free play every loop), EVERY LOOP = at the end of
// every pass, EVERY N BARS = at the end of every N-bar block counted from the start of the step (4 bars = "every 64 steps" on 1/16).
enum class FillWhen : uint8_t { Off = 0, LastLoop = 1, EveryLoop = 2, EveryBars = 3 };
constexpr int kNumFillWhen = 4;
inline const char* FillWhenName(FillWhen w) {
    switch (w) { case FillWhen::LastLoop: return "LAST LOOP"; case FillWhen::EveryLoop: return "EVERY LOOP"; case FillWhen::EveryBars: return "EVERY N BARS"; default: return "OFF"; }
}
// What the fill plays: a drum break from the library (its last beats), a hi-hat roll that speeds up, a snare roll that speeds up,
// a drum drop-out (the drums fall silent), or one of those at random every time (RANDOM never picks SAMPLE).
// C4b: SAMPLE plays a sample of the project's samples/ over the window, started on its first step and ended exactly on the step
// after the window (the next brick's beat 1): STRETCH fits the sample to the window (speed and pitch together, like FIT TO of the
// sampler), AS IS plays it at its own speed and cuts it at the end with a short fade.
enum class FillKind : uint8_t { Break = 0, HatRoll = 1, SnareBuild = 2, DropOut = 3, Random = 4, Sample = 5 };
constexpr int kNumFillKinds = 6;
inline const char* FillKindName(FillKind k) {
    switch (k) { case FillKind::Break: return "BREAK"; case FillKind::HatRoll: return "HAT ROLL"; case FillKind::SnareBuild: return "SNARE BUILD"; case FillKind::DropOut: return "DROP OUT"; case FillKind::Sample: return "SAMPLE"; default: return "RANDOM"; }
}
// Length of the fill in beats. 0 = HALF A BEAT (C4b: promised with FILL: SAMPLE); an older program reads 0 as 1 beat.
constexpr int kFillHalfBeat = 0;
constexpr int kFillBeatChoices[5] = {kFillHalfBeat, 1, 2, 4, 8};
inline double FillBeatsOf(int beats) { return beats == kFillHalfBeat ? 0.5 : static_cast<double>(beats); }
inline std::string FillLenLabel(int beats, bool upper = false) {   // "1/2 beat", "1 beat", "4 beats"
    if (beats == kFillHalfBeat) return upper ? "1/2 BEAT" : "1/2 beat";
    return std::to_string(beats) + (upper ? (beats == 1 ? " BEAT" : " BEATS") : (beats == 1 ? " beat" : " beats"));
}
// Steps the fill lasts on this grid. Half a beat is at least one step (on an odd grid it is rounded down).
inline int FillStepsOf(int beats, int stepsPerBeat) { stepsPerBeat = std::max(1, stepsPerBeat); return beats == kFillHalfBeat ? std::max(1, stepsPerBeat / 2) : std::max(1, beats) * stepsPerBeat; }
constexpr int kFillBarChoices[4] = {2, 4, 8, 16};         // N of EVERY N BARS

struct FillRule {
    FillWhen when = FillWhen::Off;
    FillKind kind = FillKind::Break;
    uint8_t beats = 2;        // 1, 2, 4 or 8; 0 = half a beat (kFillHalfBeat)
    uint8_t everyBars = 4;    // 2, 4, 8 or 16 (EveryBars only)
    uint8_t breakIndex = 0;   // 0 = a random break each time, otherwise 1 + index into the drum break library (Break kind)
    uint8_t shiftBeats = 0;   // v44.25: the fill is moved this many beats EARLIER than the end of the pass / of the N-bar block (drag on the lane)
    // C4b, SAMPLE kind: the file in the project's samples/ (plain array, so the rule stays plain data), how it fits, the drums
    static constexpr int kSampleMax = 96;
    char sample[kSampleMax] = {};
    uint8_t sampleFit = 0;    // 0 = STRETCH to the length, 1 = AS IS (cut at the end)
    bool drumsMute = true;    // the drums are silent while the sample plays (false = they play on)
    void SetSample(const std::string& f) { std::memset(sample, 0, sizeof(sample)); std::memcpy(sample, f.data(), std::min<size_t>(f.size(), kSampleMax - 1)); }
    std::string Sample() const { size_t n = 0; while (n < static_cast<size_t>(kSampleMax) && sample[n]) ++n; return std::string(sample, n); }
    bool operator==(const FillRule& o) const {
        return when == o.when && kind == o.kind && beats == o.beats && everyBars == o.everyBars && breakIndex == o.breakIndex && shiftBeats == o.shiftBeats
            && std::memcmp(sample, o.sample, sizeof(sample)) == 0 && sampleFit == o.sampleFit && drumsMute == o.drumsMute;
    }
};
inline FillRule NormalizeFill(FillRule r) {
    if (static_cast<int>(r.when) >= kNumFillWhen) r.when = FillWhen::Off;
    if (static_cast<int>(r.kind) >= kNumFillKinds) r.kind = FillKind::Break;
    auto snap = [](int v, const auto& c) { int best = c[0]; for (int x : c) if (std::abs(x - v) < std::abs(best - v)) best = x; return best; };
    r.beats = static_cast<uint8_t>(snap(r.beats, kFillBeatChoices));   // 0 stays HALF A BEAT
    r.everyBars = static_cast<uint8_t>(snap(r.everyBars, kFillBarChoices));
    r.shiftBeats = std::min<uint8_t>(r.shiftBeats, 64);
    r.sample[FillRule::kSampleMax - 1] = 0;
    r.sampleFit = std::min<uint8_t>(r.sampleFit, 1);
    return r;
}
// A chain step uses the fill of its scene, none at all, or a rule of its own.
enum class FillMode : uint8_t { Scene = 0, Off = 1, Own = 2 };

// ---- mute masks: bit i = track i (0 kick, 1 snare, 2 clap, 3 closed hat, 4 open hat, 5 perc, 6 bass, 7 synth 2, 8 synth 3, 9 chord, 10 sampler)
constexpr uint16_t kMuteAllTracks = 0x07FF;
constexpr uint16_t kMuteKickBass = (1u << 0) | (1u << 6);
constexpr uint16_t kMuteDrums = 0x003F;
constexpr uint16_t kMuteBass = (1u << 6);
constexpr uint16_t kMuteAllButKickBass = kMuteAllTracks & ~kMuteKickBass;
constexpr uint16_t kMuteAllButChord = kMuteAllTracks & ~(1u << 9);

// What a window of a fill looks like at one tick.
struct FillState { bool active = false; int pos = 0; int total = 0; };

// Is the fill sounding at this tick? `loopTick` 0..loopLen-1 is the position in the current pass; `loopIndex` / `loopsTotal` say
// which pass of the chain step this is (loopsTotal 0 = free play, no chain step: LAST LOOP then acts like EVERY LOOP and the
// N-bar blocks are counted on `absTick`, the free-running tick counter). `stepsPerBeat` = the grid; a bar = 4 beats.
inline FillState EvalFill(const FillRule& r, int stepsPerBeat, int loopTick, int loopLen, int loopIndex, int loopsTotal, int absTick) {
    FillState fs;
    if (r.when == FillWhen::Off) return fs;
    stepsPerBeat = std::max(1, stepsPerBeat);
    loopLen = std::max(1, loopLen);
    int total = FillStepsOf(r.beats, stepsPerBeat);
    switch (r.when) {
    case FillWhen::LastLoop:
        if (loopsTotal > 0 && loopIndex != loopsTotal - 1) return fs;
        [[fallthrough]];
    case FillWhen::EveryLoop:
        total = std::min(total, loopLen);
        {
            const int end = loopLen - std::min(static_cast<int>(r.shiftBeats) * stepsPerBeat, loopLen - total);   // v44.25: moved earlier by shiftBeats
            if (loopTick >= end - total && loopTick < end) { fs.active = true; fs.pos = loopTick - (end - total); fs.total = total; }
        }
        return fs;
    case FillWhen::EveryBars: {
        const int block = std::max(1, static_cast<int>(r.everyBars)) * 4 * stepsPerBeat;
        total = std::min(total, block);
        const int at = loopsTotal > 0 ? loopIndex * loopLen + loopTick : absTick;
        const int inBlock = ((at % block) + block) % block;
        const int end = block - std::min(static_cast<int>(r.shiftBeats) * stepsPerBeat, block - total);
        if (inBlock >= end - total && inBlock < end) { fs.active = true; fs.pos = inBlock - (end - total); fs.total = total; }
        return fs;
    }
    default: return fs;
    }
}

// ---- Lanes: a value over the length of a chain step, drawn with free points (v44.21) ---------------------------------------------
// A lane is a polyline over everything the step plays (all its repeats). The first point is always at the start (position 0), the
// last at the end (255); up to kMaxLanePts - 2 points can sit in between, anywhere. n == 0 is a flat lane: value 0 everywhere,
// which every lane treats as "untouched" (0 dB, no filter). Values between points are interpolated linearly (for levels that is
// linear in dB, so a fade sounds even). Plain data, no UI, no engine: testable on its own.
constexpr int kMaxLanePts = 8;
constexpr int kLanePosMax = 255;
struct Lane {
    uint8_t n = 0;
    uint8_t pos[kMaxLanePts] = {};
    int8_t val[kMaxLanePts] = {};

    bool Any() const { for (int i = 0; i < n; ++i) if (val[i] != 0) return true; return false; }
    int Start() const { return n ? val[0] : 0; }
    int End() const { return n ? val[n - 1] : 0; }
    // The value at `progress` 0..1 (0 for a flat lane).
    float At(double progress) const {
        if (n < 2) return 0.0f;
        const float x = static_cast<float>(std::clamp(progress, 0.0, 1.0)) * static_cast<float>(kLanePosMax);
        for (int i = 1; i < n; ++i) {
            if (x <= static_cast<float>(pos[i]) || i == n - 1) {
                const float x0 = static_cast<float>(pos[i - 1]), x1 = static_cast<float>(pos[i]);
                const float t = x1 > x0 ? std::clamp((x - x0) / (x1 - x0), 0.0f, 1.0f) : 1.0f;
                return static_cast<float>(val[i - 1]) + (static_cast<float>(val[i]) - static_cast<float>(val[i - 1])) * t;
            }
        }
        return static_cast<float>(val[n - 1]);
    }
    // Two points, start and end: what every lane was before free points.
    static Lane Line(int a, int b, int lo, int hi) {
        Lane l;
        a = std::clamp(a, lo, hi); b = std::clamp(b, lo, hi);
        if (a == 0 && b == 0) return l;
        l.n = 2; l.pos[0] = 0; l.pos[1] = kLanePosMax; l.val[0] = static_cast<int8_t>(a); l.val[1] = static_cast<int8_t>(b);
        return l;
    }
    // Makes a flat lane editable (two points at 0) without changing what it sounds like.
    void Materialize() { if (n == 0) { n = 2; pos[0] = 0; pos[1] = kLanePosMax; val[0] = val[1] = 0; } }
    void SetStart(int v, int lo, int hi) { Materialize(); val[0] = static_cast<int8_t>(std::clamp(v, lo, hi)); Normalize(lo, hi); }
    void SetEnd(int v, int lo, int hi) { Materialize(); val[n - 1] = static_cast<int8_t>(std::clamp(v, lo, hi)); Normalize(lo, hi); }
    // Adds a point (sorted by position); -1 when the lane is full or another point is too close. Position 0 and 255 belong to the ends.
    int AddPoint(int p, int v, int lo, int hi) {
        Materialize();
        p = std::clamp(p, 1, kLanePosMax - 1);
        if (n >= kMaxLanePts) return -1;
        int at = 1;
        while (at < n - 1 && pos[at] < p) ++at;
        if (std::abs(static_cast<int>(pos[at - 1]) - p) < kMinPointGap || std::abs(static_cast<int>(pos[at]) - p) < kMinPointGap) return -1;
        for (int i = n; i > at; --i) { pos[i] = pos[i - 1]; val[i] = val[i - 1]; }
        pos[at] = static_cast<uint8_t>(p); val[at] = static_cast<int8_t>(std::clamp(v, lo, hi));
        ++n;
        return at;
    }
    // Moves point i: the ends only change value; an inner point also moves sideways, but never past its neighbours.
    // `keepFlatPoints` (v44.25): during a mouse drag an all-zero lane must stay editable (otherwise grabbing an end of a flat lane
    // flattened it again on the first frame and the drag died); the caller calls Normalize() when the mouse is released.
    void MovePoint(int i, int p, int v, int lo, int hi, bool keepFlatPoints = false) {
        if (i < 0 || i >= n) return;
        val[i] = static_cast<int8_t>(std::clamp(v, lo, hi));
        if (i > 0 && i < n - 1) pos[i] = static_cast<uint8_t>(std::clamp(p, static_cast<int>(pos[i - 1]) + kMinPointGap, static_cast<int>(pos[i + 1]) - kMinPointGap));
        if (keepFlatPoints) { if (n >= 2) { pos[0] = 0; pos[n - 1] = kLanePosMax; } }
        else Normalize(lo, hi);
    }
    // Removes an inner point (the ends cannot be removed). A lane that becomes flat is stored as n == 0.
    bool RemovePoint(int i) {
        if (i <= 0 || i >= n - 1) return false;
        for (int k = i; k + 1 < n; ++k) { pos[k] = pos[k + 1]; val[k] = val[k + 1]; }
        --n;
        if (!Any()) *this = Lane{};
        return true;
    }
    // Clamps values, puts the ends at 0 / 255, keeps positions in order, and turns an all-zero lane into a flat one.
    void Normalize(int lo, int hi) {
        if (n < 2) { *this = Lane{}; return; }
        if (n > kMaxLanePts) n = kMaxLanePts;
        pos[0] = 0; pos[n - 1] = kLanePosMax;
        for (int i = 0; i < n; ++i) val[i] = static_cast<int8_t>(std::clamp<int>(val[i], lo, hi));
        for (int i = 1; i < n - 1; ++i) pos[i] = static_cast<uint8_t>(std::clamp(static_cast<int>(pos[i]), static_cast<int>(pos[i - 1]) + 1, kLanePosMax - 1));
        if (!Any()) *this = Lane{};
    }
    bool operator==(const Lane& o) const {
        if (n != o.n) return false;
        for (int i = 0; i < n; ++i) if (pos[i] != o.pos[i] || val[i] != o.val[i]) return false;
        return true;
    }
    static constexpr int kMinPointGap = 4;   // of 255: a point cannot sit on top of its neighbour
};

// ---- Dynamics of a chain step (v44.6, free points v44.21) ---------------------------------------------------------------------
// A chain step can move the whole mix over its length: the MIX lane (dB, -48..+6) and ONE filter lane, the DJ filter (-100..+100:
// to the left it closes the sound like a low-pass, to the right it thins it like a high-pass, 0 = open). Both are lanes of free points;
// the two ends are what the step always had. Everything 0 = untouched.
constexpr int kAutoMinDb = -48, kAutoMaxDb = 6;
constexpr int kFilterMin = -100, kFilterMax = 100;
struct StepAuto {
    Lane mix;      // dB
    Lane filter;   // DJ filter, -100 (LPF closed) .. +100 (HPF thinned)
    bool Any() const { return mix.Any() || filter.Any(); }
    bool operator==(const StepAuto& o) const { return mix == o.mix && filter == o.filter; }
    // The two-ended form: mix start/end dB, filter start/end.
    static StepAuto Ends(int mixStart, int mixEnd, int filterStart = 0, int filterEnd = 0) {
        StepAuto a; a.mix = Lane::Line(mixStart, mixEnd, kAutoMinDb, kAutoMaxDb); a.filter = Lane::Line(filterStart, filterEnd, kFilterMin, kFilterMax); return a;
    }
};
inline StepAuto NormalizeAuto(StepAuto a) { a.mix.Normalize(kAutoMinDb, kAutoMaxDb); a.filter.Normalize(kFilterMin, kFilterMax); return a; }
// What an older project's two filters (HPF 0..100, LPF 0..100, each start -> end) become: at every end the stronger one wins
// (HPF = positive, LPF = negative). Both at once cannot be said with one knob.
inline int DjFilterFromHpfLpf(int hpf, int lpf) { hpf = std::clamp(hpf, 0, 100); lpf = std::clamp(lpf, 0, 100); return hpf >= lpf ? hpf : -lpf; }
// The values at `progress` (0 = first step of the chain step, 1 = its end): a linear gain (the level is interpolated in dB, so a
// fade sounds even), and the two filter amounts 0..1 the master filter takes (only one of them is above 0).
struct AutoValues { float gain = 1.0f; float hpf = 0.0f; float lpf = 0.0f; };
inline float DbToGainLane(float db) { return db <= static_cast<float>(kAutoMinDb) ? 0.0f : std::pow(10.0f, db / 20.0f); }
inline AutoValues StepAutoAt(const StepAuto& a, double progress) {
    AutoValues v;
    if (!a.Any()) return v;
    v.gain = a.mix.Any() ? DbToGainLane(a.mix.At(progress)) : 1.0f;
    const float f = a.filter.At(progress) / 100.0f;
    if (f > 0.0f) v.hpf = std::min(f, 1.0f); else v.lpf = std::min(-f, 1.0f);
    return v;
}

// ---- Levels of the parts of a chain step (v44.13) ---------------------------------------------------------------------------------
// The dynamics above move the whole mix. These move the parts: every instrument has its own level lane (dB, the same range and the same
// linear-in-dB line as the mix level; 0 = untouched, the lowest value = silent), the transition that leads INTO the step has one level,
// and so do the hits of the step's fill. All 0 = nothing changes.
constexpr int kArrTracks = 11;   // = kNumTracks (checked in Types.h)
struct StepLevels {
    Lane track[kArrTracks];
    int8_t transitionDb = 0;   // the riser / impact that lands on the first beat of this step
    int8_t fillDb = 0;         // the drum hits of this step's fill
    bool TrackAny(int t) const { return track[t].Any(); }
    bool Any() const {
        for (int t = 0; t < kArrTracks; ++t) if (TrackAny(t)) return true;
        return transitionDb != 0 || fillDb != 0;
    }
    bool operator==(const StepLevels& o) const {
        for (int t = 0; t < kArrTracks; ++t) if (!(track[t] == o.track[t])) return false;
        return transitionDb == o.transitionDb && fillDb == o.fillDb;
    }
};
inline int8_t ClampLevelDb(int db) { return static_cast<int8_t>(std::clamp(db, kAutoMinDb, kAutoMaxDb)); }
inline StepLevels NormalizeLevels(StepLevels l) {
    for (int t = 0; t < kArrTracks; ++t) l.track[t].Normalize(kAutoMinDb, kAutoMaxDb);
    l.transitionDb = ClampLevelDb(l.transitionDb); l.fillDb = ClampLevelDb(l.fillDb);
    return l;
}
inline float LevelDbToGain(int db) { return db <= kAutoMinDb ? 0.0f : std::pow(10.0f, static_cast<float>(db) / 20.0f); }
// The gain of instrument `t` at `progress` (0..1) of the step; the dB value is interpolated, so a fade sounds even.
inline float TrackLevelAt(const StepLevels& l, int t, double progress) {
    if (t < 0 || t >= kArrTracks || !l.TrackAny(t)) return 1.0f;
    return DbToGainLane(l.track[t].At(progress));
}

// Lane ids used by the bricks strip and the inspector: 0..9 = the level of one instrument, then the MIX lane and the DJ filter.
constexpr int kLaneMix = kArrTracks, kLaneFilter = kArrTracks + 1, kNumLaneIds = kArrTracks + 2;

// ---- Parameter rows (v44.38) ------------------------------------------------------------------------------------------------------
// A brick can move one parameter of one instrument over its length: PAN, the REVERB and DELAY sends of any instrument, TUNE and DECAY of
// the six drums, CUTOFF and RESONANCE of bass / synth 2 / synth 3 / chord. The lane holds an OFFSET from the value the sound has
// (-100..+100, 0 = untouched), so nothing in the sound itself is overwritten and the same brick works on any sound. What 100 means:
// PAN 1.0 (full side), sends +1.0, TUNE 12 semitones, DECAY a factor of 2, CUTOFF 3 octaves, RESONANCE +0.5.
enum class AutoParam : uint8_t { Pan = 0, Reverb = 1, Delay = 2, Tune = 3, Decay = 4, Cutoff = 5, Resonance = 6 };
constexpr int kNumAutoParams = 7;
inline const char* AutoParamName(int p) {
    static const char* const n[kNumAutoParams] = {"PAN", "REVERB", "DELAY", "TUNE", "DECAY", "CUTOFF", "RESONANCE"};
    return p >= 0 && p < kNumAutoParams ? n[p] : "-";
}
inline const char* AutoParamShort(int p) {
    static const char* const n[kNumAutoParams] = {"PAN", "REV", "DLY", "TUN", "DEC", "CUT", "RES"};
    return p >= 0 && p < kNumAutoParams ? n[p] : "-";
}
// Which parameters an instrument has: tracks 0..5 are the drums, 6..9 bass, synth 2, synth 3 and chord, 10 the sampler.
inline bool AutoParamValid(int track, int param) {
    if (track < 0 || track >= kArrTracks || param < 0 || param >= kNumAutoParams) return false;
    if (param <= static_cast<int>(AutoParam::Delay)) return true;
    if (track == kArrTracks - 1) return false;   // the last track is the sampler;   // v44.50: the sampler has pan and the two sends; its other knobs are not on the automation list yet
    return param <= static_cast<int>(AutoParam::Decay) ? track < 6 : track >= 6;
}
// What one lane value (-100..100) means in the unit of the parameter (see above).
inline float AutoParamScale(int param) {
    switch (param) { case 3: return 12.0f; case 4: return 1.0f; case 5: return 3.0f; case 6: return 0.5f; default: return 1.0f; }
}
constexpr int kMaxParamRows = 6;
struct ParamRow {
    uint8_t track = 0, param = 0;
    Lane lane;
    bool operator==(const ParamRow& o) const { return track == o.track && param == o.param && lane == o.lane; }
};
struct StepParams {
    uint8_t n = 0;
    ParamRow row[kMaxParamRows];
    bool Any() const { for (int i = 0; i < n; ++i) if (row[i].lane.Any()) return true; return false; }
    int Find(int track, int param) const { for (int i = 0; i < n; ++i) if (row[i].track == track && row[i].param == param) return i; return -1; }
    // A new (flat) row, or the one that already exists; -1 when the pair is not valid or all rows are taken.
    int Add(int track, int param) {
        if (!AutoParamValid(track, param)) return -1;
        const int f = Find(track, param);
        if (f >= 0) return f;
        if (n >= kMaxParamRows) return -1;
        row[n] = ParamRow{}; row[n].track = static_cast<uint8_t>(track); row[n].param = static_cast<uint8_t>(param);
        return n++;
    }
    void Remove(int i) { if (i < 0 || i >= n) return; for (int k = i; k + 1 < n; ++k) row[k] = row[k + 1]; row[--n] = ParamRow{}; }
    // The offset in lane units (-100..100) of one parameter at `progress` of the brick (0 when there is no row).
    float OffsetAt(int track, int param, double progress) const { const int i = Find(track, param); return i >= 0 ? row[i].lane.At(progress) : 0.0f; }
    bool operator==(const StepParams& o) const { if (n != o.n) return false; for (int i = 0; i < n; ++i) if (!(row[i] == o.row[i])) return false; return true; }
};
inline StepParams NormalizeParams(StepParams p) {
    StepParams out;
    for (int i = 0; i < p.n && i < kMaxParamRows; ++i) {
        const int k = out.Add(p.row[i].track, p.row[i].param);
        if (k >= 0) { out.row[k].lane = p.row[i].lane; out.row[k].lane.Normalize(-100, 100); }
    }
    return out;
}
// Lane ids 0..11 are above; the parameter rows follow (kLaneParam0 + row).
constexpr int kLaneParam0 = kNumLaneIds, kNumLaneIdsAll = kNumLaneIds + kMaxParamRows;
inline void LaneRange(int laneId, int* lo, int* hi) {
    if (laneId == kLaneFilter || laneId >= kLaneParam0) { *lo = kFilterMin; *hi = kFilterMax; } else { *lo = kAutoMinDb; *hi = kAutoMaxDb; }
}

// What a role usually does with the dynamics of its step, and the transition into it. Used by "fill the empty steps from the roles".
inline StepAuto RoleAuto(SceneRole r, bool lastStepOfChain) {
    StepAuto a;
    switch (r) {
        case SceneRole::Intro: a = StepAuto::Ends(-8, -3, -60, 0); break;     // opens up from dark and quiet
        case SceneRole::Build: a = StepAuto::Ends(-3, 0, 0, 55); break;       // thins out from below while it rises
        case SceneRole::Break: a = StepAuto::Ends(0, 0, -30, -30); break;     // darker, same level
        case SceneRole::Outro: a = StepAuto::Ends(-2, -30, 0, 40); break;     // fades away
        default: break;                                                       // DROP / none: flat, full
    }
    if (lastStepOfChain && r == SceneRole::Outro) a.mix.SetEnd(kAutoMinDb, kAutoMinDb, kAutoMaxDb);   // the very end is silence
    return a;
}
// Mute mask a role suggests (only the BREAK takes the low end out); 0 = leave the step's mutes alone.
inline uint16_t RoleMutes(SceneRole r) { return r == SceneRole::Break ? kMuteKickBass : 0; }
// Transition sound that should lead INTO a step of role `to` coming from role `from`: indices of the four synthesized transition sounds
// (10 DOWNLIFTER, 11 IMPACT, 12 SUB DROP, 13 REVERSE CRASH). -1 = none. bars = how many bars before the step it starts.
inline int RoleTransition(SceneRole from, SceneRole to, uint8_t* bars) {
    if (to == SceneRole::Drop && (from == SceneRole::Build || from == SceneRole::Break || from == SceneRole::Intro)) { if (bars) *bars = from == SceneRole::Build ? 2 : 1; return 13; }
    if (to == SceneRole::Break && (from == SceneRole::Drop || from == SceneRole::Build)) { if (bars) *bars = 1; return 10; }
    if (to == SceneRole::Build && from == SceneRole::Break) { if (bars) *bars = 1; return 11; }
    return -1;
}

} // namespace t8
