#include <chrono>
#include "Sequencer.h"
#include <algorithm>
#include <cmath>

namespace t8 {

int TrackSampleVariant(const Track& t) {
    int count[256] = {0};
    int active = 0;
    for (const Step& st : t.steps) if (st.active) { ++count[StepVariant(t, st)]; ++active; }
    if (!active) return t.defaultVariant;   // nothing written yet: the track's default sample
    int best = 0;
    for (int v = 1; v < 256; ++v) if (count[v] > count[best]) best = v;
    return best;
}


void Sequencer::Init(AudioEngine* engine, Project* project) {
    engine_ = engine;
    project_ = project;
    globalTick_ = 0;
    chordBaseTick_ = 0;
    samplesIntoStep_ = 0.0;
    synthNoteOffScheduled_.fill(false);
    chordNoteOffScheduled_ = false;
    chordHeldCount_ = 0;
    pendingRatchets_.clear();
    prefire_.fill(Prefire{});
}

void Sequencer::Play() {
    std::lock_guard<SceneMutex> lock(patternMutex_);
    playing_ = true;
    globalTick_ = 0;
    chordBaseTick_ = 0;
    samplesIntoStep_ = 0.0;
    pendingRatchets_.clear();
    prefire_.fill(Prefire{});
    fillWas_ = false; fillActiveUi_.store(false); riserArmedFor_ = -1;
    ApplyStepOverlay();
    engine_->SamplerSetTempo(project_->patterns[PlayPatternIndex()].bpm);
    engine_->SamplerLoopRestart();   // v44.52: a sampler LOOP starts with the first step
    FireTick(0); // sound step 1 immediately instead of waiting a full step
}

void Sequencer::Stop() {
    std::lock_guard<SceneMutex> lock(patternMutex_);
    playing_ = false;
    pinnedPlay_.store(-1);
    pendingScene_.store(-1);
    pendingChordScene_.store(-1);
    pendingRiserArmed_.store(false);
    riserTicksLeft_.store(-1);
    globalTick_ = 0;
    chordBaseTick_ = 0;
    samplesIntoStep_ = 0.0;
    pendingRatchets_.clear();
    prefire_.fill(Prefire{});
    for (int s = 0; s < kNumSynthTracks; ++s) {
        engine_->SynthNoteOff(s);
        synthNoteOffScheduled_[s] = false;
    }
    engine_->ChordNoteOff();
    engine_->ChordAuditionOff();
    engine_->SamplerFadeOut();
    chordNoteOffScheduled_ = false;
    chordHeldCount_ = 0;
    fillWas_ = false; fillActiveUi_.store(false);
    ApplyStepOverlay();   // not playing: the arrangement mutes let go
}

void Sequencer::Panic() {
    std::lock_guard<SceneMutex> lock(patternMutex_);
    pendingRatchets_.clear();
    for (int s = 0; s < kNumSynthTracks; ++s) {
        engine_->SynthNoteOff(s);
        synthNoteOffScheduled_[s] = false;
    }
    engine_->ChordNoteOff();
    engine_->SamplerFadeOut();
    chordNoteOffScheduled_ = false;
    chordHeldCount_ = 0;
}

void Sequencer::PlayChainFrom(int pos, bool loopBrick) {
    std::lock_guard<SceneMutex> lock(patternMutex_);
    if (!project_ || project_->songChainLength <= 0) { if (!playing_) Play(); return; }
    if (playing_) Stop();   // a clean restart: held notes are released, the clock goes back to the first step
    songLoopStep_.store(loopBrick);
    SetSongModeActive(true, pos);
    Play();
}

void Sequencer::TogglePlay() {
    if (playing_) Stop(); else Play();
}

double Sequencer::SamplesPerStep(int sampleRate) const {
    const Pattern& p = project_->patterns[PlayPatternIndex()];
    float bpm = std::max(20.0f, p.bpm);
    // one step = a beat / stepsPerBeat (1/16 notes by default, 1/32 or triplets on the other grids)
    double secondsPerStep = 60.0 / bpm / ClampGrid(p.stepsPerBeat);
    return secondsPerStep * sampleRate;
}

// The length of a 16th note, whatever the grid: the stutter / beat repeat is counted in these.
double Sequencer::SamplesPerSixteenth(int sampleRate) const {
    const Pattern& p = project_->patterns[PlayPatternIndex()];
    return 60.0 / std::max(20.0f, p.bpm) / 4.0 * sampleRate;
}

double Sequencer::BeatsUntilLaunch() const {
    if (!project_ || pendingScene_.load() < 0) return 0.0;
    const double stepLen = std::max(1.0, SamplesPerStep(engine_ ? engine_->SampleRate() : kSampleRate));
    const double frac = std::clamp(samplesIntoStep_ / stepLen, 0.0, 1.0);
    return std::max(0.0, (static_cast<double>(TicksUntilLaunch()) - frac) / static_cast<double>(std::max(1, StepsPerBeat())));
}
double Sequencer::LaunchQuantumBeats() const {
    if (!project_) return 1.0;
    const int spb = std::max(1, StepsPerBeat());
    const int q = launchQuant_.load() == 1 ? spb : std::max(1, project_->patterns[static_cast<size_t>(PlayPatternIndex())].masterLength);
    return static_cast<double>(q) / static_cast<double>(spb);
}
int Sequencer::StepsPerBeat() const { return ClampGrid(project_->patterns[PlayPatternIndex()].stepsPerBeat); }
int Sequencer::StepsPerBar() const { return 4 * StepsPerBeat(); }

// Where inside its own tick a hit of this step sounds, as a fraction of a step (v39).
//  * swing: on every odd tick the hit is delayed by swing/2 of a step (the track's own swing if it has one, else the
//    pattern's). Every tick keeps the same length, so a bar lasts exactly as long as it should whatever the swing.
//  * nudge: the step's own microtiming, +-50 % of a step.
// Negative results (a nudge to the front) are only honoured for drum tracks -- the lookahead in FireTick schedules those
// hits inside the previous tick. Synth and chord tracks are clamped to 0 (their note-offs and legato logic are tick based).
double Sequencer::HitOffset(const Track& t, const Pattern& p, int tick, const Step& s) const {
    return StepHitOffset(t, p, tick, s);
}

// x = when the hit sounds inside the current tick (fraction of a step). x <= 0 -> right now.
void Sequencer::ScheduleHit(double x, const RatchetEvent& ev) {
    if (x <= 0.0) { FireRatchetEvent(ev); return; }
    RatchetEvent e = ev;
    e.fraction = static_cast<float>(std::min(x, 0.999));
    pendingRatchets_.push_back(e);
}

int Sequencer::StepForTrack(int trackIndex) const {
    const Pattern& p = project_->patterns[PlayPatternIndex()];
    const Track& t = StepTrack(p, trackIndex);
    int len = t.useOwnLength ? t.length : p.masterLength;
    len = std::max(1, len);
    if (t.type == TrackType::Chord) return ChordStepIndex(globalTick_, len);   // chord bank: counts from its own launch
    return globalTick_ % len;
}

// Stutter / beat-repeat requests: a press waits for the next step boundary (the
// next beat for windows of 4+ steps), so the captured window - and with it the
// loop - lies on the grid. `immediate` = sequencer stopped, nothing to align to.
void Sequencer::ApplyRepeaterRequest(bool immediate, int tick) {
    Mixer& mx = engine_->GetMixer();
    if (mx.repeaterOn == mx.repeaterWantOn && mx.repeaterDensityIndex == mx.repeaterWantDensity) return;
    if (!immediate) {
        int idx = std::clamp(mx.repeaterWantDensity, 0, kNumRepeaterDensities - 1);
        int gran = std::clamp(static_cast<int>(std::lround(kRepeaterDensities[idx].stepFraction)), 1, 4);
        gran = std::max(1, static_cast<int>(std::lround(gran * StepsPerBeat() / 4.0)));   // counted in 16ths, ticks differ on other grids
        if (tick % gran != 0) return;
    }
    mx.repeaterDensityIndex = mx.repeaterWantDensity;
    mx.repeaterOn = mx.repeaterWantOn;
}

// Switches the playing chord scene. On a tick boundary the old chord is simply due for
// its release right now (FireTick's note-off logic below decides between releasing and
// tying it into the new scene's first step); outside a tick it is released at once.
void Sequencer::ApplyChordScene(int c, bool onBoundary) {
    chordSceneCur_ = c;
    if (onBoundary) {
        chordBaseTick_ = globalTick_;
        if (chordNoteOffScheduled_) chordNoteOffTick_ = 0;
    } else {
        chordBaseTick_ = playing_.load() ? globalTick_ + 1 : 0;
        if (chordNoteOffScheduled_ || chordHeldCount_ > 0) {
            engine_->ChordNoteOff();
            chordNoteOffScheduled_ = false;
            chordHeldCount_ = 0;
        }
    }
}

void Sequencer::FireTick(int tick) {
    std::lock_guard<SceneMutex> lock(patternMutex_);
    ApplyRepeaterRequest(false, tick);
    // Queued chord-scene launch (see LaunchChordScene)
    {
        int pc = pendingChordScene_.load();
        if (pc >= 0) {
            int q = launchQuant_.load() == 1 ? StepsPerBeat() : std::max(1, PlayPattern().masterLength);
            if (tick % q == 0) {
                pendingChordScene_.store(-1);
                ApplyChordScene(pc, true);
            }
        }
    }
    // Queued scene launch (see LaunchScene): applied exactly on the boundary.
    {
        int ps = pendingScene_.load();
        if (ps >= 0) {
            int q = launchQuant_.load() == 1 ? StepsPerBeat() : std::max(1, PlayPattern().masterLength);
            if (tick % q == 0) {
                pendingScene_.store(-1);
                ApplyScene(ps);
            }
        }
    }
    // v44.54: the NEXT BAR queue of the LIVE switches: applied exactly on the bar line, after a scene launch that lands on the same line
    // (the app drops switches that were meant for another scene).
    if (barHookArmed_.load() && (tick % std::max(1, StepsPerBar())) == 0) {
        barHookArmed_.store(false);
        if (barHook_) barHook_();
    }
    Pattern& p = PlayPattern();

    // Any ratchet retriggers left over from the previous step should
    // already have all fired (RenderAndAdvance fires them strictly before
    // advancing the tick) -- clear defensively and rebuild fresh for the
    // step that's starting now.
    pendingRatchets_.clear();

    // TIE detection: a step that (a) has SLIDE set, or (b) is as long as its
    // whole track loop (the "endless pad" case: one chord/note spanning the
    // loop) and lands exactly when the previous note is due to be released
    // must NOT release+retrigger -- that is what made loop restarts audible
    // (release stage + new attack + filter-envelope restart). For such a
    // step the pending note-off is skipped and the note-on goes in as a
    // legato "slide" (engines keep their envelopes running when the gate is
    // still open, see SubtractiveSynth::NoteOn).
    bool synthTie[kNumSynthTracks] = {};
    bool chordTie = false;
    for (int i = kNumDrumTracks; i < kNumTracks; ++i) {
        const Track& tt = StepTrack(p, i);
        if (tt.type == TrackType::Sampler) continue;   // v44.50: a one-shot has no gate to tie
        int tlen = std::max(1, tt.useOwnLength ? tt.length : p.masterLength);
        const Step& st = tt.steps[tt.type == TrackType::Chord ? ChordStepIndex(tick, tlen) : tick % tlen];
        if (!st.active || st.probability < 100) continue;
        bool full = static_cast<int>(st.lengthSteps) >= tlen;
        if (!(st.slide || full)) continue;
        if (tt.type == TrackType::Chord) {
            int cc = st.chordSize;
            if (chordLiveSlot_ >= 0 && chordLiveCount_ > 0) cc = chordLiveCount_;
            if (cc > 0 && cc == chordHeldCount_ && st.ratchet <= 1) chordTie = true;
        } else if (tt.type != TrackType::Drum && st.ratchet <= 1) {
            if (tt.slotIndex >= 0 && tt.slotIndex < kNumSynthTracks) synthTie[tt.slotIndex] = true;
        }
    }

    // Note-offs count down in steps (not compared against the absolute tick), so a
    // tick reset - chain step, scene change, restart - can never leave a note
    // whose off time "was in the past" hanging forever.
    for (int s = 0; s < kNumSynthTracks; ++s)
        if (synthNoteOffScheduled_[s] && synthNoteOffTick_[s] > 0) --synthNoteOffTick_[s];
    if (chordNoteOffScheduled_ && chordNoteOffTick_ > 0) --chordNoteOffTick_;

    // Fire any scheduled synth note-offs first so a note-on landing on the
    // same tick (from a new step) takes over cleanly.
    for (int s = 0; s < kNumSynthTracks; ++s) {
        if (synthNoteOffScheduled_[s] && synthNoteOffTick_[s] <= 0) {
            if (synthTie[s]) continue;     // tied: keep gate open, step below re-schedules the off
            engine_->SynthNoteOff(s);
            synthNoteOffScheduled_[s] = false;
        }
    }
    if (chordNoteOffScheduled_ && chordNoteOffTick_ <= 0) {
        if (!chordTie) {
            engine_->ChordNoteOff();
            chordNoteOffScheduled_ = false;
            chordHeldCount_ = 0;
        }
    }

    // v44.5 transitions of the chain: the NEXT chain step may carry a riser / impact / downlifter that lands on its first beat.
    // It is armed once, when no more than `riserBars` bars of the playing step are left, through the same back-timed riser as the live RISER pad.
    if (songModeActive_ && project_ && project_->songChainLength > 0 && !pendingRiserArmed_.load()) {
        const int n = project_->songChainLength;
        const int pos = std::max(0, songChainPos_) % n;
        const ChainStep& nx = project_->songChain[static_cast<size_t>((pos + 1) % n)];
        if (nx.riser >= 0 && riserArmedFor_ != pos) {
            const int loopLen = std::max(1, LoopSteps());
            const int ticksLeft = (std::max(1, songChainRepeatsLeft_) - 1) * loopLen + (loopLen - tick % loopLen);
            const int bar = std::max(1, StepsPerBar());
            if (ticksLeft <= std::max(1, static_cast<int>(nx.riserBars)) * bar) {
                const int ahead = std::clamp((ticksLeft + bar - 1) / bar, 1, std::max(1, static_cast<int>(nx.riserBars)));
                SetPendingRiser(nx.riser, ahead, LevelDbToGain(nx.levels.transitionDb));
                riserArmedFor_ = pos;
            }
        }
    }

    // Riser bar-sync -- see Sequencer.h's SetPendingRiser. Simple one-shot
    // promotion: fire straight into RiserPlayer the moment we land on a bar
    // boundary, no ongoing "active" window to track here the way the live
    // break below needs (RiserPlayer itself owns however long the one-shot
    // plays for, release tail included).
    if (pendingRiserArmed_.load()) {
        const int idx = pendingRiserIndex_.load();
        const float tgain = pendingRiserGain_.load();
        const int sr = engine_->SampleRate();
        const double spt = SamplesPerStep(sr);
        RiserPlayer& rp = engine_->Risers();
        const double hit = rp.HitFrame(idx);
        int left = riserTicksLeft_.load();
        if (left < 0) {
            const int bar = StepsPerBar();
            left = bar - (tick % bar);                                // ticks to the next bar line (1..bar)
            const double minLead = 0.5 * sr;
            while (left * spt < minLead) left += bar;                  // never a drop that is already on top of us
            left += bar * (std::max(1, pendingRiserBars_.load()) - 1);
        }
        const double timeLeft = left * spt;                           // frames from this tick to the drop
        const double delay = timeLeft - hit;
        if (hit <= 0.0 || delay < spt) {
            pendingRiserArmed_.store(false);
            riserTicksLeft_.store(-1);
            if (delay >= 0.0) {
                rp.TriggerAligned(idx, delay, 0.0, 1.0, tgain);       // wait, then play at natural speed
            } else {
                const double ratio = hit / std::max(1.0, timeLeft);
                if (ratio <= 1.2) rp.TriggerAligned(idx, 0.0, 0.0, ratio, tgain);      // slightly faster
                else rp.TriggerAligned(idx, 0.0, hit - timeLeft, 1.0, tgain);         // enter part-way through
            }
        } else {
            riserTicksLeft_.store(left - 1);                          // value for the next tick
        }
    }

    // Live break: launch-quantized (Ableton Session View style), one-shot.
    // See Sequencer.h. SetLiveBreak() only ARMS it (liveBreakPending_) --
    // it does NOT cut into whatever bar is currently playing. Here, a
    // pending break is promoted to active only once we land exactly on a
    // bar boundary (tick % 16 == 0), which becomes the start of a fixed
    // one-bar (16-tick) window; every DRUM track's trigger for ticks
    // inside that window comes from the break template instead of its
    // real pattern steps. Once 16 ticks have elapsed, the break clears
    // itself automatically and every subsequent tick falls straight
    // through to the real pattern again, with no further action needed
    // from whatever triggered it. Synth tracks always fall through to the
    // normal path below, so bass/leads keep playing uninterrupted under a
    // drum fill, exactly like a "FILL" button on a real groovebox.
    if (liveBreakPending_.load() && !liveBreakActive_.load() && (tick % StepsPerBar()) == 0) {
        liveBreakPending_.store(false);
        liveBreakActive_.store(true);
        liveBreakStartTick_ = tick;
    }
    bool useBreak = false;
    int breakPos = 0; // 0..15, the break's OWN step index, always bar-
                       // aligned since it only ever starts on tick % 16
                       // == 0 -- a break always plays its full authored
                       // arrangement from its own first step.
    const DrumBreak* brk = nullptr;
    float breakLevel = 1.0f;
    if (liveBreakActive_.load()) {
        int p16 = tick - liveBreakStartTick_;
        if (p16 >= 0 && p16 < StepsPerBar()) {
            useBreak = true;
            breakPos = p16;
            const auto& allBreaks = AllDrumBreaks();
            int bi = std::clamp(liveBreakIndex_.load(), 0, static_cast<int>(allBreaks.size()) - 1);
            brk = &allBreaks[bi];
            breakLevel = BreakTrimLinear(bi);
        } else {
            liveBreakActive_.store(false); // one bar played -- auto-revert
        }
    }

    // v44.5 fill: a window over the last beats of a pass (rule of the scene / the chain step). It never touches the steps of the scene.
    // A live break wins over it. A Break fill plays the end of a library break through the same path as the live break.
    FillState fs;
    if (!useBreak) {
        FillRule rule;
        fs = FillAt(tick, &rule);
        if (fs.active && !fillWas_) {                     // a new window starts: settle the kind and the break once
            FillKind k = rule.kind;
            if (k == FillKind::Random) k = static_cast<FillKind>(std::uniform_int_distribution<int>(0, 3)(rng_));
            fillKindCur_.store(static_cast<int>(k));
            const int nb = static_cast<int>(AllDrumBreaks().size());
            fillBreakCur_ = rule.breakIndex > 0 ? std::clamp(static_cast<int>(rule.breakIndex) - 1, 0, std::max(0, nb - 1)) : std::uniform_int_distribution<int>(0, std::max(0, nb - 1))(rng_);
        }
        fillWas_ = fs.active;
    } else {
        fillWas_ = false;
    }
    fillActiveUi_.store(fs.active);
    const FillKind fillKind = static_cast<FillKind>(fillKindCur_.load());
    float fillGain = 1.0f;   // v44.13: the level of the drum hits of the chain step's fill
    if (fs.active) if (const ChainStep* fcs = PlayingChainStep()) fillGain = LevelDbToGain(fcs->levels.fillDb);
    if (fs.active && fillKind == FillKind::Break) {
        const auto& allBreaks = AllDrumBreaks();
        if (!allBreaks.empty()) {
            const int bi = std::clamp(fillBreakCur_, 0, static_cast<int>(allBreaks.size()) - 1);
            const int bs = std::max(1, StepsPerBar());
            useBreak = true; brk = &allBreaks[bi]; breakLevel = BreakTrimLinear(bi) * fillGain;
            breakPos = fs.total <= bs ? bs - fs.total + fs.pos : fs.pos % bs;   // the LAST beats of the break bar
        }
    }

    for (int i = 0; i < kNumTracks; ++i) {
        Track& t = const_cast<Track&>(StepTrack(p, i));

        if (fs.active && !useBreak && i < kNumDrumTracks) {
            if (fillKind == FillKind::DropOut) continue;                       // the drums fall silent for the window
            const bool hatRoll = fillKind == FillKind::HatRoll && t.slotIndex == static_cast<int>(DrumSlot::ClosedHat);
            const bool snareRoll = fillKind == FillKind::SnareBuild && t.slotIndex == static_cast<int>(DrumSlot::Snare);
            if (hatRoll || snareRoll) {
                // a roll that speeds up: 1, 2, 3, 4 hits per step towards the end, getting louder
                const float f = (static_cast<float>(fs.pos) + 0.5f) / static_cast<float>(std::max(1, fs.total));
                const int ratchet = std::clamp(1 + static_cast<int>(f * 3.99f), 1, 4);
                DrumVoiceParams params;
                params.tune = t.drumTune; params.edit = t.edit; params.decay = t.drumDecay; ApplyDrumMods(params, t.slotIndex); params.level = 1.0f;
                params.keyRoot = project_ ? project_->randomizerKeyRoot : -1;
                RatchetEvent ev;
                ev.isDrum = true;
                ev.drumSlot = static_cast<DrumSlot>(t.slotIndex);
                ev.drumVariant = static_cast<uint8_t>(TrackSampleVariant(t));
                ev.drumParams = params;
                ev.velocity = ((snareRoll ? 0.40f : 0.45f) + (snareRoll ? 0.60f : 0.50f) * f) * fillGain;
                for (int r = 0; r < ratchet; ++r) ScheduleHit(static_cast<double>(r) / ratchet, ev);
                continue;
            }
        }

        if (useBreak && i < kNumDrumTracks) {
            int pos = breakPos;
            bool hit = false, accent = false;
            // the break is written in 16ths; on another grid its steps land on the nearest step of that grid
            const int barSteps = StepsPerBar();
            auto tickOf = [&](int s) { return (s * barSteps + 8) / 16; };
            for (int s : brk->hitSteps[i]) if (tickOf(s) == pos) { hit = true; break; }
            if (hit) for (int s : brk->accentSteps[i]) if (tickOf(s) == pos) { accent = true; break; }
            if (!hit) continue;
            DrumVoiceParams params;
            params.tune = t.drumTune; params.edit = t.edit;
            params.decay = t.drumDecay; ApplyDrumMods(params, t.slotIndex);
            params.level = breakLevel;
            params.keyRoot = project_ ? project_->randomizerKeyRoot : -1;
            engine_->TriggerDrum(static_cast<DrumSlot>(t.slotIndex), accent ? 1.0f : 0.8f, TrackSampleVariant(t), params);   // the track's own sample, not always the first
            continue; // break hits don't carry per-step ratchet data
        }

        int len = t.useOwnLength ? t.length : p.masterLength;
        len = std::max(1, len);
        int idx = t.type == TrackType::Chord ? ChordStepIndex(tick, len) : tick % len;
        Step& step = t.steps[idx];

        // A drum hit that sits ahead of its own tick (negative nudge) was already handled by the lookahead at the end
        // of the previous FireTick: its probability roll is stored, the early hits have sounded.
        Prefire& pf = prefire_[i];
        const bool pre = t.type == TrackType::Drum && pf.tick == tick && pf.pattern == PlayPatternIndex();
        const bool preRolled = pre && pf.play;
        if (pre) pf = Prefire{};
        if (pre && !preRolled) continue;
        if (!pre) {
            if (!step.active) continue;
            if (step.probability < 100) {
                std::uniform_int_distribution<int> roll(1, 100);
                if (roll(rng_) > static_cast<int>(step.probability)) continue; // silently skipped this pass
            }
        }

        // ghost = quiet in-between hit; accent wins if both are somehow set
        float velocity = StepPlayVelocity(t, p, tick, step);
        int ratchet = std::clamp(static_cast<int>(step.ratchet), 1, 4);
        const double off = HitOffset(t, p, tick, step);   // where inside this tick the main hit sounds (steps)
        // hit k of a ratchet (k = 0 is the main hit) sounds at off + k*(1-off)/ratchet: the roll still ends on the next step
        auto hitPos = [&](int k) { return off + static_cast<double>(k) * (1.0 - off) / static_cast<double>(ratchet); };

        if (t.type == TrackType::Drum) {
            DrumVoiceParams params;
            params.tune = t.drumTune; params.edit = t.edit;
            params.decay = t.drumDecay; ApplyDrumMods(params, t.slotIndex);
            params.level = 1.0f; // track-level volume is applied later by the mixer
            params.keyRoot = project_ ? project_->randomizerKeyRoot : -1;
            RatchetEvent ev;
            ev.isDrum = true;
            ev.drumSlot = static_cast<DrumSlot>(t.slotIndex);
            ev.drumVariant = static_cast<uint8_t>(StepVariant(t, step));
            ev.drumParams = params;
            ev.velocity = velocity;
            for (int r = 0; r < ratchet; ++r) {
                const double x = hitPos(r);
                if (pre && x < 0.0) continue;   // the lookahead sounded this one already
                ScheduleHit(x, ev);
            }
        } else if (t.type == TrackType::Chord) {
            // chordSize==0 means "nothing to play" even though `active` is
            // true (see Step::chordSize's comment in Types.h) -- an empty
            // chord step stays silent rather than firing a phantom NoteOn.
            std::array<int8_t, 6> cNotes = step.chordNotes;
            int cCount = step.chordSize;
            if (chordLiveSlot_ >= 0 && chordLiveCount_ > 0) { cNotes = chordLiveNotes_; cCount = chordLiveCount_; }
            if (cCount == 0) continue;
            chordHeldCount_ = cCount;
            int lenSteps = std::max(1, static_cast<int>(step.lengthSteps));
            chordNoteOffTick_ = lenSteps;   // steps left until the release
            chordNoteOffScheduled_ = true;
            RatchetEvent ev;
            ev.isDrum = false;
            ev.isChord = true;
            ev.chordNotes = cNotes;
            ev.chordCount = cCount;
            ev.velocity = velocity;
            for (int r = 0; r < ratchet; ++r) {
                // A tied step goes in as slide=true so the engines keep the running envelopes (no re-attack / filter restart).
                ev.slide = r == 0 && (step.slide || chordTie);
                ScheduleHit(hitPos(r), ev);
            }
        } else if (t.type == TrackType::Sampler) {
            // v44.50: ONE-SHOT: the note picks the pitch, the sample plays on its own (no note-off); a ratchet re-strikes it
            RatchetEvent ev;
            ev.isDrum = false;
            ev.isSampler = true;
            ev.synthNote = step.note;
            ev.velocity = velocity;
            for (int r = 0; r < ratchet; ++r) ScheduleHit(hitPos(r), ev);
        } else {
            int synthIdx = t.slotIndex;
            const bool tied = step.slide || (synthIdx >= 0 && synthIdx < kNumSynthTracks && synthTie[synthIdx]);
            int lenSteps = std::max(1, static_cast<int>(step.lengthSteps));
            synthNoteOffTick_[synthIdx] = lenSteps;   // steps left until the release
            synthNoteOffScheduled_[synthIdx] = true;
            RatchetEvent ev;
            ev.isDrum = false;
            ev.synthIndex = synthIdx;
            ev.synthNote = step.note;
            ev.velocity = velocity;
            for (int r = 0; r < ratchet; ++r) {
                ev.slide = r == 0 && tied;
                ScheduleHit(hitPos(r), ev);
            }
        }
    }

    LookaheadEarlyHits(tick);
}

// Drum hits with a negative nudge sound BEFORE their tick. At the end of FireTick(T) the steps of tick T+1 are examined and
// every such hit (the main hit and the ratchets that fall before the grid line) is scheduled inside the tick that is
// running now; FireTick(T+1) finds the Prefire mark and skips them. Where the next tick is not certain to be the
// next step of this pattern (song chain wrap, queued scene, live break) nothing is scheduled ahead, and the hit simply
// sounds on the grid line instead of being lost.
void Sequencer::LookaheadEarlyHits(int tick) {
    if (!project_) return;
    if (pendingScene_.load() >= 0 || liveBreakPending_.load() || liveBreakActive_.load()) return;
    Pattern& p = PlayPattern();
    const int nt = tick + 1;
    if (songModeActive_ && project_->songChainLength > 0 && (nt % std::max(1, LoopSteps())) == 0) return;
    if (FillAt(nt).active || FillAt(tick).active) return;   // v44.5: a fill window plays its own drum events
    const int pidx = PlayPatternIndex();
    for (int i = 0; i < kNumDrumTracks; ++i) {
        Track& t = const_cast<Track&>(StepTrack(p, i));
        const int len = std::max(1, t.useOwnLength ? t.length : p.masterLength);
        const Step& step = t.steps[nt % len];
        if (!step.active) continue;
        const double off = HitOffset(t, p, nt, step);
        if (off >= 0.0) continue;
        bool play = true;
        if (step.probability < 100) {
            std::uniform_int_distribution<int> roll(1, 100);
            play = roll(rng_) <= static_cast<int>(step.probability);
        }
        prefire_[i] = Prefire{nt, pidx, play};
        if (!play) continue;
        DrumVoiceParams params;
        params.tune = t.drumTune; params.edit = t.edit;
        params.decay = t.drumDecay; ApplyDrumMods(params, t.slotIndex);
        params.level = 1.0f;
        params.keyRoot = project_->randomizerKeyRoot;
        RatchetEvent ev;
        ev.isDrum = true;
        ev.drumSlot = static_cast<DrumSlot>(t.slotIndex);
        ev.drumVariant = static_cast<uint8_t>(StepVariant(t, step));
        ev.drumParams = params;
        ev.velocity = StepPlayVelocity(t, p, nt, step);
        const int ratchet = std::clamp(static_cast<int>(step.ratchet), 1, 4);
        for (int r = 0; r < ratchet; ++r) {
            const double x = off + static_cast<double>(r) * (1.0 - off) / ratchet;   // in steps from the start of tick nt
            if (x >= 0.0) break;
            ev.fraction = static_cast<float>(1.0 + x);                                // in the running tick
            pendingRatchets_.push_back(ev);
        }
    }
}

int Sequencer::LoopSteps() const {
    if (!project_) return 1;
    const Pattern& p = project_->patterns[PlayPatternIndex()];
    const ChainStep* cs = PlayingChainStep();
    return cs ? ChainStepLoopSteps(p, *cs) : std::max(1, p.masterLength);
}

FillState Sequencer::FillAt(int tick, FillRule* ruleOut) const {
    if (!project_) return FillState{};
    const Pattern& p = project_->patterns[PlayPatternIndex()];
    const ChainStep* cs = PlayingChainStep();
    FillRule rule = p.fill;
    int loopLen = std::max(1, p.masterLength), loopsTotal = 0, loopIndex = 0;
    if (cs) {
        loopLen = ChainStepLoopSteps(p, *cs);
        if (cs->fillMode == FillMode::Off) rule = FillRule{};
        else if (cs->fillMode == FillMode::Own) rule = cs->fill;
        loopsTotal = std::max(1, cs->repeats);
        loopIndex = std::clamp(loopsTotal - std::max(1, songChainRepeatsLeft_), 0, loopsTotal - 1);
    }
    if (ruleOut) *ruleOut = rule;
    return EvalFill(rule, StepsPerBeat(), ((tick % loopLen) + loopLen) % loopLen, loopLen, loopIndex, loopsTotal, tick);
}

void Sequencer::ApplyStepOverlay() {
    unsigned m = 0;
    if (playing_.load()) if (const ChainStep* cs = PlayingChainStep()) m = cs->muteMask & kMuteAllTracks;
    engine_->GetMixer().arrMuteMask.store(m);
}

double Sequencer::ChainStepProgress() const {
    if (!playing_.load()) return -1.0;
    const ChainStep* cs = PlayingChainStep();
    if (!cs) return -1.0;
    const int loopLen = std::max(1, LoopSteps());
    const int reps = std::max(1, cs->repeats);
    const int pass = std::clamp(reps - std::max(1, songChainRepeatsLeft_), 0, reps - 1);
    const double stepLen = std::max(1.0, SamplesPerStep(engine_ ? engine_->SampleRate() : kSampleRate));
    const double inPass = static_cast<double>(((globalTick_ % loopLen) + loopLen) % loopLen) + std::clamp(samplesIntoStep_ / stepLen, 0.0, 1.0);
    return std::clamp((static_cast<double>(pass) * loopLen + inPass) / (static_cast<double>(reps) * loopLen), 0.0, 1.0);
}

void Sequencer::UpdateAutomation(int sampleRate) {
    (void)sampleRate;
    AutoValues v;
    const double prog = ChainStepProgress();
    if (prog >= 0.0) if (const ChainStep* cs = PlayingChainStep()) v = StepAutoAt(cs->automation, prog);
    Mixer& mx = engine_->GetMixer();
    mx.arrGain.store(v.gain, std::memory_order_relaxed);
    mx.arrHpf.store(v.hpf, std::memory_order_relaxed);
    mx.arrLpf.store(v.lpf, std::memory_order_relaxed);
    const ChainStep* pcs = prog >= 0.0 ? PlayingChainStep() : nullptr;   // v44.13: the level of every instrument
    for (int t = 0; t < kNumTracks; ++t) mx.arrTrackGain[t].v.store(pcs ? TrackLevelAt(pcs->levels, t, prog) : 1.0f, std::memory_order_relaxed);
    // v44.38: parameter rows (an offset per instrument and parameter, -1..1; 0 for everything the brick does not move)
    float off[kNumTracks][kNumAutoParams] = {};
    const StepParams* rows = pcs ? &pcs->params : nullptr;
    double rowProg = prog;
    if (!rows && project_ && playing_.load() && !songModeActive_) {   // v44.39: on LIVE the rows of the scene go round with the scene
        rows = &project_->patterns[static_cast<size_t>(PlayPatternIndex())].params;
        rowProg = std::max(0.0, SceneProgress());
    }
    if (rows) for (int i = 0; i < rows->n; ++i) {
        const ParamRow& r = rows->row[i];
        if (AutoParamValid(r.track, r.param)) off[r.track][r.param] = std::clamp(r.lane.At(rowProg) / 100.0f, -1.0f, 1.0f);
    }
    for (int t = 0; t < kNumTracks; ++t) for (int k = 0; k < kNumAutoParams; ++k) mx.arrParam[t].v[k].store(off[t][k], std::memory_order_relaxed);
}

double Sequencer::SceneProgress() const {
    if (!project_ || !playing_.load() || songModeActive_) return -1.0;
    const int loopLen = std::max(1, LoopSteps());
    const double stepLen = std::max(1.0, SamplesPerStep(engine_ ? engine_->SampleRate() : kSampleRate));
    return std::clamp((static_cast<double>(((globalTick_ % loopLen) + loopLen) % loopLen) + std::clamp(samplesIntoStep_ / stepLen, 0.0, 1.0)) / static_cast<double>(loopLen), 0.0, 1.0);
}

// v44.38: tune and decay of a drum hit follow the parameter rows of the playing brick (0 offset = the track's own values, exactly).
void Sequencer::ApplyDrumMods(DrumVoiceParams& p, int slot) const {
    if (!engine_ || slot < 0 || slot >= kNumTracks) return;
    const Mixer& mx = engine_->GetMixer();
    const float tu = mx.ParamOff(slot, AutoParam::Tune), de = mx.ParamOff(slot, AutoParam::Decay);
    if (tu != 0.0f) p.tune = std::clamp(p.tune + tu * AutoParamScale(3), -24.0f, 24.0f);
    if (de != 0.0f) p.decay = std::clamp(p.decay * std::exp2(de * AutoParamScale(4)), 0.25f, 2.0f);
}

void Sequencer::AdvanceSongChain() {
    // Caller (RenderAndAdvance) holds patternMutex_.
    if (!project_ || project_->songChainLength <= 0) return;
    songChainRepeatsLeft_--;
    if (songChainRepeatsLeft_ > 0) return; // this chain step still has loops left -- stay put
    if (songLoopStep_.load()) {   // v44.10: loop this one brick
        songChainPos_ = std::clamp(songChainPos_, 0, project_->songChainLength - 1);
        songChainRepeatsLeft_ = std::max(1, project_->songChain[static_cast<size_t>(songChainPos_)].repeats);
        riserArmedFor_ = -1;
        return;
    }
    int nextPos = (songChainPos_ + 1) % project_->songChainLength;
    const int loopLo = songLoopLo_.load();
    if (loopLo >= 0) {   // v44.28: LOOP over a range of bricks: after the last one of the range back to the first
        const int n = project_->songChainLength;
        const int lo = std::clamp(loopLo, 0, n - 1), hi = std::clamp(songLoopHi_.load(), lo, n - 1);
        nextPos = (songChainPos_ >= lo && songChainPos_ < hi) ? songChainPos_ + 1 : lo;
    } else if (songRunOnce_.load() && songChainPos_ + 1 >= project_->songChainLength) { songEnded_ = true; return; }   // v44.10: the track is over
    songChainPos_ = nextPos;
    const ChainStep& step = project_->songChain[songChainPos_];
    songChainRepeatsLeft_ = std::max(1, step.repeats);
    riserArmedFor_ = -1;
    ApplyStepOverlay();
    songPlayPattern_ = std::clamp(step.patternIndex, 0, kNumPatterns - 1);
    if (songFollowsEdit_) project_->currentPattern = songPlayPattern_;
    engine_->GetMixer().SyncFromPattern(project_->patterns[songPlayPattern_]);
    ApplySceneSound(songPlayPattern_);
    // The chain slot can carry a chord scene: the chords move with the arrangement.
    if (project_->chordBankOn && step.chordScene >= 0 && step.chordScene < kNumChordScenes && step.chordScene != chordSceneCur_) {
        pendingChordScene_.store(-1);
        ApplyChordScene(step.chordScene, true);
    }
}

void Sequencer::FireRatchetEvent(const RatchetEvent& ev) {
    std::lock_guard<SceneMutex> lock(patternMutex_);
    if (ev.isDrum) {
        engine_->TriggerDrum(ev.drumSlot, ev.velocity, ev.drumVariant, ev.drumParams);
    } else if (ev.isSampler) {
        engine_->SamplerNoteOn(ev.synthNote, ev.velocity);
    } else if (ev.isChord) {
        // Re-strike the same chord tones -- retriggers all voices' envelopes
        // together, same "audibly distinct hits, not one sustained note"
        // reasoning as the synth branch below.
        engine_->ChordNoteOn(ev.chordNotes.data(), ev.chordCount, ev.velocity, ev.slide);
    } else {
        // Re-strike the same note -- retriggers the voice's envelope, which
        // is what makes consecutive hits from the same mono synth voice
        // audibly distinct rather than one sustained note.
        engine_->SynthNoteOn(ev.synthIndex, ev.synthNote, ev.velocity, ev.slide);
    }
}

void Sequencer::RenderAndAdvance(float* out, int numFrames, int sampleRate) {
    // Held for the whole callback: this is what makes it safe for the UI
    // thread to edit pattern/mixer state concurrently (see PatternMutex()).
    // N1: when the lock is busy, measure how long the audio thread waits for it (shown in the header's XRUN hint).
    patternMutex_.MarkAudioThread();
    if (!patternMutex_.try_lock()) {
        const auto t0 = std::chrono::steady_clock::now();
        patternMutex_.lock();
        const auto waited = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        engine_->GetMixer().audioLockWaitUs.fetch_add(static_cast<unsigned>(std::max<long long>(0, waited)), std::memory_order_relaxed);
    }
    std::lock_guard<SceneMutex> lock(patternMutex_, std::adopt_lock);

    // Tempo-synced delay (see Types.h's DelayDivisionSeconds/
    // FxSettings::delayDivisionIndex): recompute the actual delay time
    // from the CURRENT pattern's bpm every callback, playing or not, so
    // it's always in sync -- a bpm tweak or pattern switch takes effect
    // immediately rather than needing some other trigger to notice. Same
    // two-copies convention as every other FX setting: Project::fx is the
    // copy that gets saved, Mixer::fx is the live copy AudioEngine::Render
    // actually reads (see FxView.cpp for where user edits update the
    // Project copy first).
    {
        float bpm = project_->patterns[PlayPatternIndex()].bpm;
        float seconds = DelayDivisionSeconds(project_->fx.delayDivisionIndex, bpm);
        project_->fx.delayTimeSec = seconds;
        engine_->GetMixer().fx.delayTimeSec = seconds;
        if (bpm != lastSamplerBpm_) { lastSamplerBpm_ = bpm; engine_->SamplerSetTempo(bpm); }   // v44.52: loops / slices follow the tempo
    }

    if (!playing_) {
        ApplyRepeaterRequest(true);
        UpdateAutomation(sampleRate);
        engine_->Render(out, numFrames, SamplesPerSixteenth(sampleRate));
        return;
    }

    int framesRendered = 0;
    while (framesRendered < numFrames) {
        // every tick is exactly one step long: swing and nudge move the hits inside it (see HitOffset)
        const double stepLen = SamplesPerStep(sampleRate);

        // Stop the chunk at whichever comes first: the next pending
        // ratchet retrigger within this step, or the step boundary itself.
        // With no ratchets active (the common case) this is exactly the
        // old behaviour -- nextEventSamples just equals stepLen.
        double nextEventSamples = stepLen;
        for (const auto& ev : pendingRatchets_) {
            double t = static_cast<double>(ev.fraction) * stepLen;
            if (t > samplesIntoStep_ && t < nextEventSamples) nextEventSamples = t;
        }

        double samplesLeftToEvent = nextEventSamples - samplesIntoStep_;
        int framesToNextBoundary = static_cast<int>(std::ceil(samplesLeftToEvent));
        int chunk = std::min(numFrames - framesRendered, std::max(1, framesToNextBoundary));

        UpdateAutomation(sampleRate);
        engine_->Render(out + static_cast<size_t>(framesRendered) * 2, chunk, SamplesPerSixteenth(sampleRate));   // the step length sizes the stutter window
        framesRendered += chunk;
        samplesIntoStep_ += chunk;

        // Fire every pending ratchet whose target time has now been
        // reached -- a loop rather than a single check, since more than
        // one track can ratchet at the same fraction within a step.
        for (size_t i = 0; i < pendingRatchets_.size(); ) {
            double t = static_cast<double>(pendingRatchets_[i].fraction) * stepLen;
            if (t <= samplesIntoStep_ + 0.5) {
                FireRatchetEvent(pendingRatchets_[i]);
                pendingRatchets_.erase(pendingRatchets_.begin() + static_cast<long>(i));
            } else {
                ++i;
            }
        }

        if (samplesIntoStep_ >= stepLen - 0.5) {
            samplesIntoStep_ -= stepLen;
            if (samplesIntoStep_ < 0) samplesIntoStep_ = 0;
            // Capture the pattern that's about to finish its loop BEFORE
            // advancing the tick/chain -- AdvanceSongChain may switch
            // project_->currentPattern to a different pattern with a
            // different masterLength, so this must be resolved from the
            // OLD pattern, not whatever's current after the switch.
            int finishedPatternLen = std::max(1, LoopSteps());   // v44.5: a trimmed chain step loops sooner
            globalTick_++;
            // Only touch anything here if there's an actual chain
            // programmed -- songModeActive_ with an empty chain must be a
            // total no-op (in particular, must NOT reset globalTick_ on
            // every master-length boundary "just in case": that would
            // silently break polymeter tracks, whose whole point is
            // drifting out of lockstep with the master length rather than
            // resetting alongside it).
            if (songModeActive_ && project_ && project_->songChainLength > 0 &&
                (globalTick_ % finishedPatternLen) == 0) {
                AdvanceSongChain();
                if (songEnded_) {   // v44.10: the last brick has finished and the track plays once
                    songEnded_ = false;
                    Stop();
                    songChainPos_ = 0;
                    if (project_->songChainLength > 0) songChainRepeatsLeft_ = std::max(1, project_->songChain[0].repeats);
                    if (framesRendered < numFrames) engine_->Render(out + static_cast<size_t>(framesRendered) * 2, numFrames - framesRendered, SamplesPerSixteenth(sampleRate));
                    return;
                }
                // Realign to the new pattern's own step 0 -- the whole
                // point of a chain-step boundary is a clean cut, unlike a
                // manual mid-bar scene switch which deliberately keeps
                // playing from wherever the tick counter already was.
                globalTick_ = 0;
                chordBaseTick_ = 0;
            }
            FireTick(globalTick_);
        }
    }
}

} // namespace t8
