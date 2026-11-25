// t8_fuzz: random sequences of the operations the UI calls, with audio rendering in between (or on a second thread with --threads),
// then invariants and a save / load / save round trip. Run it under ASan / UBSan / TSan. Deterministic per seed.
//   t8_fuzz [--seeds N] [--ops N] [--threads] [--first SEED] [--verbose]
#include "app/AppState.h"
#include "core/Persistence.h"
#include "core/ChordProgressions.h"
#include "core/DrumPatterns.h"
#include "core/SynthPresets.h"
#include "core/WavFile.h"
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>
using namespace t8;

static std::string Slurp(const std::string& p) { std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

static const char* kOpNames[] = {"ToggleStep","RandomizeDrums","RandomizeMelodic","RandomizeRhythm","Euclid","Rotate","SetLen","SetRes","ChordProg","ChordRandom",
    "ChordBank","LaunchChordScene","CommitChordSlot","CopyChordsToSlot","ClearChordSlot","DupChordSlot","Throw","MoveBrick","RemoveBrick","DupBrick","BrickRepeats",
    "BrickBars","UpdateFromLive","DupBrickAsScene","TakeSound","MakeOwn","FollowTrack","ApplySoundAll","BrickMute","BrickSolo","PlayFromBrick","Transport","Loop",
    "LoopAll","ToStart","Undo","Redo","CopyPastePattern","CopyPasteTrack","CopyInstrument","MatchLevelPanEq","ClearInstrument","ClearPattern","Genre","Drop","Mutes",
    "AdjustToScale","SnapToScale","KeyScale","LaunchScene","SynthPreset","LiveChord","ChordSettings","EditScene","BrickLevels","ParamRow","MatchLevel","SelectBrick",
    "Starter","SaveLoad","Style","Sampler","SynthSample","NextBar","EuclidKeep",
    "Link","OpenGo","ArmScope","Pitch","RiserBreakRepeat","MasterFx","ReopenDamaged"};
constexpr int kNumOps = sizeof(kOpNames) / sizeof(kOpNames[0]);

static int Check(AppState& a, const char* op, unsigned seed, int n) {
    const Project& p = a.project; int bad = 0;
    auto fail = [&](const std::string& m) { std::cout << "INVARIANT seed " << seed << " op#" << n << " after " << op << ": " << m << "\n"; ++bad; };
    if (p.songChainLength < 0 || p.songChainLength > kMaxChainSteps) fail("chain length " + std::to_string(p.songChainLength));
    if (p.currentPattern < 0 || p.currentPattern >= kNumPatterns) fail("currentPattern " + std::to_string(p.currentPattern));
    if (p.chordSceneEdit < 0 || p.chordSceneEdit >= kNumChordScenes) fail("chordSceneEdit");
    for (int i = 0; i < p.songChainLength; ++i) {
        const ChainStep& cs = p.songChain[static_cast<size_t>(i)];
        if (cs.patternIndex < 0 || cs.patternIndex >= kNumPatterns) fail("brick " + std::to_string(i) + " patternIndex " + std::to_string(cs.patternIndex));
        if (cs.repeats < 1 || cs.repeats > 99) fail("brick repeats " + std::to_string(cs.repeats));
        if (cs.chordScene < -1 || cs.chordScene >= kNumChordScenes) fail("brick chordScene");
    }
    for (int s = 0; s < kNumPatterns; ++s) {
        const Pattern& pat = p.patterns[static_cast<size_t>(s)];
        if (pat.masterLength < 1 || pat.masterLength > kMaxSteps) fail("scene " + std::to_string(s) + " masterLength " + std::to_string(pat.masterLength));
        if (pat.stepsPerBeat < 1 || pat.stepsPerBeat > 16) fail("scene stepsPerBeat " + std::to_string(pat.stepsPerBeat));
        if (!(pat.bpm >= 20.0f && pat.bpm <= 400.0f)) fail("scene bpm " + std::to_string(pat.bpm));
        for (int t = 0; t < kNumTracks; ++t) {
            const Track& tr = pat.tracks[static_cast<size_t>(t)];
            if (tr.length < 1 || tr.length > kMaxSteps) fail("track length " + std::to_string(tr.length));
            if (!(tr.volume >= 0.0f && tr.volume <= 2.0f) || !(tr.pan >= -1.0f && tr.pan <= 1.0f)) fail("track volume / pan");
            for (const Step& st : tr.steps) {
                if (st.chordSize > kMaxChordVoices) fail("chordSize " + std::to_string(st.chordSize));
                if (st.lengthSteps < 1) fail("lengthSteps 0");
                if (st.ratchet < 1 || st.ratchet > 4) fail("ratchet");
            }
        }
    }
    return bad;
}

int main(int argc, char** argv) {
    int seeds = 20, ops = 400; bool threads = false, verbose = false; unsigned first = 1;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seeds") && i + 1 < argc) seeds = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--ops") && i + 1 < argc) ops = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--first") && i + 1 < argc) first = static_cast<unsigned>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--threads")) threads = true;
        else if (!std::strcmp(argv[i], "--verbose")) verbose = true;
    }
    std::filesystem::create_directories("selftest_tmp_fuzz/src");
    t8::userdirs::SetRootsForTest("selftest_tmp_fuzz/Documents", "selftest_tmp_fuzz/Service");   // backups of the loads never reach the real Documents
    {   // v44.50: two small WAVs for the sampler ops (a mono tone and a stereo noise burst)
        std::vector<float> tone(18000), noise(2 * 66000);
        for (size_t i = 0; i < tone.size(); ++i) tone[i] = 0.6f * std::sin(0.03f * static_cast<float>(i)) * (1.0f - static_cast<float>(i) / tone.size());
        std::mt19937 nr(5); for (float& v : noise) v = (static_cast<float>(nr() % 2000) / 1000.0f - 1.0f) * 0.4f;
        WriteWavPCM16("selftest_tmp_fuzz/src/fz_a.wav", tone, 1, kSampleRate);
        WriteWavPCM16("selftest_tmp_fuzz/src/fz_b.wav", noise, 2, kSampleRate);
    }
    int failures = 0, roundTrips = 0, roundTripDiffs = 0, nanBlocks = 0, lockMisses = 0;
    for (unsigned seed = first; seed < first + static_cast<unsigned>(seeds); ++seed) {
        auto ap = std::make_unique<AppState>(); AppState& a = *ap;
        a.engine.Init(kSampleRate); a.sequencer.Init(&a.engine, &a.project);
        a.projectPath = "selftest_tmp_fuzz/fuzz.t8proj";
        a.RefreshSound();
        a.LoadStarterGroove();
        std::mt19937 rng(seed);
        auto R = [&](int n) { return n <= 0 ? 0 : static_cast<int>(rng() % static_cast<unsigned>(n)); };
        std::atomic<bool> stop{false};
        std::atomic<int> nan{0};
        auto render = [&](int blocks) {
            std::vector<float> ob(256 * 2);
            for (int b = 0; b < blocks; ++b) {
                std::fill(ob.begin(), ob.end(), 0.0f);
                // v44.59 (TEST-3): through the PITCH stage, as the audio callback does
                a.varispeed.Process(ob.data(), 256, a.PitchTargetSpeed(), kSampleRate, [&](float* dst, int frames) { a.sequencer.RenderAndAdvance(dst, frames, kSampleRate); });
                for (float v : ob) if (!(v == v) || v > 8.0f || v < -8.0f) { nan.fetch_add(1); break; }
            }
        };
        std::thread audio;
        if (threads) audio = std::thread([&] { while (!stop.load()) render(4); });
        for (int n = 0; n < ops; ++n) {
            const int op = R(kNumOps);
            Project& p = a.project;
            const int nb = p.songChainLength;
            const int tr = R(kNumTracks), syn = R(kNumSynthTracks);
            if (verbose) std::cout << seed << ":" << n << " " << kOpNames[op] << std::endl;
            // the panels of the app hold the pattern lock for their whole Draw, so every operation the UI calls runs under it
            std::unique_lock<SceneMutex> uiLock(a.sequencer.PatternMutex());
            switch (op) {
            case 0: a.ToggleStep(tr, R(kMaxSteps)); break;
            case 1: a.RandomizeDrums(); break;
            case 2: a.RandomizeMelodicOctaves(syn, R(2)); break;
            case 3: a.RandomizeRhythm(syn); break;
            case 4: a.PushUndo(); a.ApplyEuclid(tr, R(17), 1 + R(32), R(8)); break;
            case 5: a.PushUndo(); a.RotateTrack(tr, R(9) - 4); break;
            case 6: { static const int L[] = {1, 8, 12, 16, 24, 32, 48, 64}; a.SetPatternLength(L[R(8)]); break; }
            case 7: { static const int S[] = {2, 3, 4, 6, 8}; a.SetPatternResolution(S[R(5)]); break; }
            case 8: a.ApplyChordProgression(R(kNumChordProgressions)); break;
            case 9: a.RandomizeChordProgression(); break;
            case 10: a.SetChordBank(R(2)); break;
            case 11: a.LaunchChordScene(R(kNumChordScenes)); break;
            case 12: a.CommitChordSlotToScene(R(kNumChordScenes), R(kNumLiveScenes)); break;
            case 13: a.CopyPatternChordsToScene(R(kNumChordScenes)); break;
            case 14: a.ClearChordScene(R(kNumChordScenes)); break;
            case 15: a.DuplicateChordScene(R(kNumChordScenes), R(kNumChordScenes)); break;
            case 16: a.ThrowIntoChain(); break;
            case 17: if (nb > 1) a.MoveChainStep(R(nb), R(nb)); break;
            case 18: if (nb > 0) a.RemoveChainStep(R(nb)); break;
            case 19: if (nb > 0) a.DuplicateChainStep(R(nb)); break;
            case 20: if (nb > 0) a.SetBrickRepeats(R(nb), 1 + R(8)); break;
            case 21: if (nb > 0) a.SetBrickBars(R(nb), 0.25 * (1 + R(16)), R(2)); break;
            case 22: if (nb > 0) a.UpdateBrickFromLive(R(nb)); break;
            case 23: if (nb > 0) a.DuplicateBrickAsScene(R(nb)); break;
            case 24: a.TakeSceneSoundToTrack(R(kNumLiveScenes)); break;
            case 25: if (nb > 0) a.MakeBrickSoundOwn(p.songChain[static_cast<size_t>(R(nb))].patternIndex, R(kNumParts)); break;
            case 26: if (nb > 0) a.FollowTrackSound(p.songChain[static_cast<size_t>(R(nb))].patternIndex, R(kNumParts)); break;
            case 27: if (nb > 0) a.ApplySoundToAllBricks(p.songChain[static_cast<size_t>(R(nb))].patternIndex, R(kNumParts)); break;
            case 28: if (nb > 0) a.SetBrickTrackMuted(R(nb), tr, R(2)); break;
            case 29: if (nb > 0) a.SetBrickTrackSolo(R(nb), tr, R(2)); break;
            case 30: if (nb > 0) a.PlayFromBrick(R(nb)); break;
            case 31: a.TransportToggle(); break;
            case 32: a.SetLoop(R(2)); break;
            case 33: a.SetLoopAll(R(2)); break;
            case 34: a.TrackToStart(); break;
            case 35: a.Undo(); break;
            case 36: a.Redo(); break;
            case 37: a.CopyPattern(); p.currentPattern = R(kNumLiveScenes); a.PastePattern(); break;
            case 38: a.CopyTrack(tr); a.PasteTrack(tr < kNumDrumTracks ? R(kNumDrumTracks) : tr); break;
            case 39: a.CopyInstrument(R(kNumLiveScenes), tr, R(kNumLiveScenes)); break;
            case 40: a.MatchLevelPanEq(R(kNumLiveScenes), tr, R(2)); break;
            case 41: a.ClearInstrument(R(kNumLiveScenes), tr); break;
            case 42: if (R(4) == 0) a.ClearPattern(); break;
            case 43: a.ApplyGenrePattern(R(kNumGenrePatterns)); break;
            case 44: a.ToggleDrop(); break;
            case 45: { const int k = R(4); if (k == 0) a.ToggleDrumMuteGroup(R(3)); else if (k == 1) a.ToggleSynthMute(syn); else if (k == 2) a.ToggleAllDrumsMute(); else a.ToggleChordMute(); break; }
            case 46: a.AdjustChordsToScale(R(2)); break;
            case 47: a.SnapTrackToScale(kNumDrumTracks + R(kNumSynthTracks + 1)); break;
            case 48: p.randomizerKeyRoot = R(12); p.randomizerScale = R(kNumScales); a.TickKeyFollow(); break;
            case 49: a.sequencer.SetLaunchQuantize(R(3)); a.sequencer.LaunchScene(R(kNumLiveScenes)); break;
            case 50: a.seqCursorTrack = kNumDrumTracks + syn; a.ApplySynthPreset(R(static_cast<int>(AllSynthPresets().size()))); break;
            case 51: a.chdLiveSlot = -1; a.RefreshChordLive(true); break;   // v44.44: the live chord is gone from the UI; the engine path stays off
            case 52: a.chdComplex = R(2); a.chdOutputMode = R(2) ? ChordOutputMode::Arpeggio : ChordOutputMode::Stack; a.chdArpRateSteps = 1 + R(4); a.chdChordLengthSteps = 1 << R(6); break;
            case 53: { std::lock_guard<SceneMutex> lk(a.sequencer.PatternMutex()); p.currentPattern = R(kNumLiveScenes); p.liveScene = p.currentPattern; break; }
            case 54: if (nb > 0) { a.PushSoundUndo(); const int b = R(nb); a.SetBrickLevel(b, R(2), -R(30)); a.SetBrickTrackLevels(b, tr, -R(48), -R(12)); a.SetBrickTransitionLevel(b, -R(20)); a.SetBrickFillLevel(b, -R(20)); } break;
            case 55: if (nb > 0) { const int b = R(nb); const int row = a.AddBrickParamRow(b, tr, R(6)); if (row >= 0 && R(3) == 0) a.RemoveBrickParamRow(b, 0); } break;
            case 56: a.MatchInstrumentLevel(R(kNumLiveScenes), tr, R(2), R(2)); break;
            case 57: a.selBrick = nb > 0 ? R(nb) : -1; if (nb > 1 && R(3) == 0) { a.SelectBrickRange(R(nb), R(nb)); if (R(2)) a.DuplicateBrickRange(std::min(a.selBrick, a.selBrickHi < 0 ? a.selBrick : a.selBrickHi), std::max(a.selBrick, a.selBrickHi)); } break;
            case 58: if (a.CurrentPatternIsEmpty()) a.LoadStarterGroove(); break;
            case 59: {
                std::lock_guard<SceneMutex> lk(a.sequencer.PatternMutex());
                const std::string f1 = "selftest_tmp_fuzz/a.t8proj", f2 = "selftest_tmp_fuzz/b.t8proj";
                if (!SaveProject(p, f1, false)) { std::cout << "SAVE FAILED seed " << seed << "\n"; ++failures; break; }
                auto q = std::make_unique<Project>();
                if (!LoadProject(*q, f1)) { std::cout << "LOAD FAILED seed " << seed << " op#" << n << "\n"; ++failures; break; }
                SaveProject(*q, f2, false);
                ++roundTrips;
                if (Slurp(f1) != Slurp(f2)) {
                    ++roundTripDiffs;
                    if (roundTripDiffs <= 3) { std::filesystem::copy_file(f1, "selftest_tmp_fuzz/diff_a_" + std::to_string(seed) + "_" + std::to_string(n) + ".t8proj", std::filesystem::copy_options::overwrite_existing);
                                               std::filesystem::copy_file(f2, "selftest_tmp_fuzz/diff_b_" + std::to_string(seed) + "_" + std::to_string(n) + ".t8proj", std::filesystem::copy_options::overwrite_existing); }
                    std::cout << "ROUND TRIP DIFFERS seed " << seed << " op#" << n << "\n";
                }
                break; }
            case 61: {   // v44.50: the sampler -- assign a WAV, pick one from samples/, a missing one, knobs, steps, mute
                SamplerPatch& sp = a.CurrentPattern().samplerPatch;
                switch (R(7)) {
                case 0: a.AssignSampleToSampler(R(2) ? "selftest_tmp_fuzz/src/fz_a.wav" : "selftest_tmp_fuzz/src/fz_b.wav"); break;
                case 1: { const auto& nm = a.SampleNames(); if (!nm.empty()) a.SetSamplerFile(nm[static_cast<size_t>(R(static_cast<int>(nm.size())))]); break; }
                case 2: sp.file = "gone.wav"; a.SyncSceneSamples(true); break;   // a file that is not there: silence, no crash
                case 3: sp.root = static_cast<int8_t>(24 + R(73)); sp.start = R(4) ? R(96) / 100.0f : 0.95f; sp.attackMs = static_cast<float>(R(201)); sp.decay = 0.02f + R(99) / 100.0f; sp.poly = R(2); sp.reverse = R(2); a.PushEditedSamplerToEngine(); break;
                case 4: for (int k = 0, c = 1 + R(6); k < c; ++k) a.ToggleStep(kSamplerTrackIndex, R(kMaxSteps)); break;
                case 5: a.SwToggleMute(-1, kSamplerTrackIndex); break;
                default: a.sequencer.ApplySceneSound(p.currentPattern); break;
                }
                break; }
            case 62: {   // v44.53: the SAMPLE engine of a synth / the chord: engine, files, modes, marks, layout, steps
                const int tgt = R(kNumSynthTracks + 1);
                const bool chord = tgt >= kNumSynthTracks;
                SynthPatch& sp = a.EditedPatch(chord, tgt);
                switch (R(9)) {
                case 0: sp.engineType = R(4) ? SynthEngineType::Sample : static_cast<SynthEngineType>(R(3)); a.PushEditedPatchToEngine(chord, tgt); a.SyncSceneSamples(true); break;
                case 1: sp.engineType = SynthEngineType::Sample; a.AssignSampleToSampler(R(2) ? "selftest_tmp_fuzz/src/fz_a.wav" : "selftest_tmp_fuzz/src/fz_b.wav", tgt); break;
                case 2: { sp.engineType = SynthEngineType::Sample; const auto& nm = a.SampleNames(); if (!nm.empty()) a.SetSamplerFile(nm[static_cast<size_t>(R(static_cast<int>(nm.size())))], tgt); break; }
                case 3: sp.engineType = SynthEngineType::Sample; sp.sample.SetFile(R(2) ? "gone.wav" : std::string(R(200), 'q')); a.SyncSceneSamples(true); break;   // missing / absurd names: silence, no crash
                case 4: { SamplerPatch q = a.GetSampleTarget(tgt); q.mode = static_cast<uint8_t>(R(3)); static const int SL[] = {0, 4, 8, 16, 32}; q.slices = static_cast<uint8_t>(SL[R(5)]); q.root = static_cast<int8_t>(24 + R(73)); q.start = R(4) ? R(96) / 100.0f : 0.95f; q.decay = 0.02f + R(99) / 100.0f; q.reverse = R(2); q.fitBars = static_cast<int8_t>(R(3) - 1); q.tuneCents = static_cast<int8_t>(R(201) - 100); a.PutSampleTarget(tgt, q); break; }
                case 5: a.SetStatus(R(2) ? a.FindSliceHits(R(101) / 100.0f, tgt) : a.LayoutSamplerSlices(tgt)); break;
                case 6: if (R(2)) a.AddSliceMark(R(1001) / 1000.0f, tgt); else a.RemoveNearestSliceMark(R(1001) / 1000.0f, 0.1f, tgt); break;
                case 7: for (int k = 0, c = 1 + R(6); k < c; ++k) a.ToggleStep(chord ? kChordTrackIndex : kNumDrumTracks + tgt, R(kMaxSteps)); break;
                default: a.sequencer.ApplySceneSound(p.currentPattern); break;
                }
                break; }
            case 63: {   // v44.54: NEXT BAR -- queued mute / solo / groups, the button on and off, scene launches and edits in between
                using QS = AppState::QueuedSwitch;
                if (R(5) == 0) a.quantizeSwitches = !a.quantizeSwitches;
                switch (R(7)) {
                case 0: case 1: a.LiveSwitch(QS::Mute, static_cast<uint16_t>(tr)); break;
                case 2: a.LiveSwitch(QS::Solo, static_cast<uint16_t>(tr)); break;
                case 3: a.LiveSwitch(QS::Group, static_cast<uint16_t>(R(2) ? kMuteDrums : kMuteAllTracks)); break;
                case 4: a.LiveSwitch(QS::Drop); break;
                case 5: a.LiveSwitch(QS::Keep, AppState::kKeepKickBassChord); break;
                default: a.sequencer.LaunchScene(R(kNumLiveScenes)); break;
                }
                break; }
            case 64: {   // v44.55: EUCLID ON / OFF on any track, live changes, hand edits, rotation and a scene length change in between
                switch (R(6)) {
                case 0: case 1: a.EuclidMelodyOn(tr, R(17), 1 + R(32), R(9) - 4); break;
                case 2: a.EuclidMelodyOff(tr); break;
                case 3: a.ToggleStep(tr, R(kMaxSteps)); break;
                case 4: a.RotateTrack(tr, R(9) - 4); break;
                default: a.SetPatternLength(R(2) ? 16 : 24); break;
                }
                break; }
            // ---- v44.59 (TEST-3): LINK, OPEN / GO, the armed scene, PITCH, risers / breaks / repeater, master FX, a damaged file ----
            case 65: {   // LINK on selected pads, UNLINK, the menu toggle, and their undo / redo
                const int s1 = R(kNumLiveScenes), s2 = R(kNumLiveScenes);
                auto linkSel = [&]() {   // after LINK every selected scene has the mute / solo of the reference and is in its group
                    const uint16_t sel = a.sceneSel;
                    if (a.SceneSelCount() < 2) { a.LinkSelectedScenes(); return; }
                    int ref = -1;
                    if (p.currentPattern < kNumLiveScenes && ((sel >> p.currentPattern) & 1u)) ref = p.currentPattern;
                    else for (int i = 0; i < kNumLiveScenes; ++i) if ((sel >> i) & 1u) { ref = i; break; }
                    a.LinkSelectedScenes();
                    for (int i = 0; i < kNumLiveScenes; ++i) {
                        if (!((sel >> i) & 1u)) continue;
                        bool same = a.ScenesLinked(i, ref);
                        for (int t = 0; t < kNumTracks && same; ++t)
                            same = p.patterns[static_cast<size_t>(i)].tracks[static_cast<size_t>(t)].mute == p.patterns[static_cast<size_t>(ref)].tracks[static_cast<size_t>(t)].mute &&
                                   p.patterns[static_cast<size_t>(i)].tracks[static_cast<size_t>(t)].solo == p.patterns[static_cast<size_t>(ref)].tracks[static_cast<size_t>(t)].solo;
                        if (!same) { std::cout << "INVARIANT seed " << seed << " op#" << n << " Link: scene " << i + 1 << " is not like scene " << ref + 1 << " after LINK\n"; ++failures; break; }
                    }
                };
                switch (R(6)) {
                case 0: case 1: a.SelectScenePad(s1, false, false); a.SelectScenePad(s2, true, false); linkSel(); break;
                case 2: a.SelectScenePad(s1, false, R(2) != 0); a.SelectScenePad(s2, false, true); linkSel(); break;
                case 3: a.UnlinkSceneUndoable(s1); break;
                case 4: if (p.currentPattern < kNumLiveScenes) a.ToggleSceneLink(s1); break;
                default: {   // undo of a LINK step brings back exactly the groups and flags from before it
                    if (!a.undoStack.empty() && a.undoStack.back().kind == AppState::UndoEntry::Kind::Link) {
                        const auto want = a.undoStack.back().links;
                        a.Undo();
                        if (p.sceneLink != want) { std::cout << "INVARIANT seed " << seed << " op#" << n << " Link: undo did not restore the groups\n"; ++failures; }
                    } else if (R(2)) a.Redo();
                    break; }
                }
                break; }
            case 66: {   // OPEN / GO: open a scene without launching, GO, a launch click while held apart, the CLICK switch
                if (R(6) == 0) a.clickOpens = !a.clickOpens;
                const int sc = R(kNumLiveScenes);
                const int playing = a.sequencer.PlayPatternIndex();
                const bool wasPlaying = a.sequencer.IsPlaying() && !a.sequencer.IsSongModeActive();
                switch (R(5)) {
                case 0: case 1:
                    a.OpenScenePad(sc);
                    if (wasPlaying && a.sequencer.PlayPatternIndex() != playing) { std::cout << "INVARIANT seed " << seed << " op#" << n << " OpenGo: opening a scene moved the player\n"; ++failures; }
                    if (a.sequencer.PendingScene() == sc) { std::cout << "INVARIANT seed " << seed << " op#" << n << " OpenGo: the opened scene is still queued\n"; ++failures; }
                    break;
                case 2: a.GoToEditedScene(); break;
                case 3: a.LaunchScenePad(sc); if (a.SceneIsHeldApart()) { std::cout << "INVARIANT seed " << seed << " op#" << n << " OpenGo: a launch click left the editor apart\n"; ++failures; } break;
                default: a.SelectSceneForEdit(sc); break;
                }
                break; }
            case 67: {   // the armed scene: with a launch waiting, the switches write the coming scene and never the playing one
                if (!a.sequencer.IsPlaying()) a.sequencer.Play();
                a.sequencer.SetLaunchQuantize(1 + R(2));
                const int target = R(kNumLiveScenes);
                a.sequencer.LaunchScene(target);
                const int armed = a.ArmedScene(), playing = a.sequencer.PlayPatternIndex();
                if (armed >= 0 && armed != playing && !a.ScenesLinked(armed, playing)) {
                    const bool before = p.patterns[static_cast<size_t>(playing)].tracks[static_cast<size_t>(tr)].mute;
                    { AppState::ArmScope arm(a, armed); a.LiveSwitch(AppState::QueuedSwitch::Mute, static_cast<uint16_t>(tr)); }
                    if (p.patterns[static_cast<size_t>(playing)].tracks[static_cast<size_t>(tr)].mute != before) { std::cout << "INVARIANT seed " << seed << " op#" << n << " ArmScope: the playing scene changed\n"; ++failures; }
                    if (a.sequencer.PlayPatternIndex() != playing) { std::cout << "INVARIANT seed " << seed << " op#" << n << " ArmScope: the player moved\n"; ++failures; }
                }
                break; }
            case 68:   // PITCH: the fader, steps, a push, the range
                switch (R(4)) {
                case 0: a.PitchSet(static_cast<float>(R(3201) - 1600) / 100.0f); break;
                case 1: a.PitchStep(R(2) ? 0.1f : -1.0f); break;
                case 2: a.pitchBend.store(R(3) - 1); break;
                default: a.PitchSetRange16(R(2) != 0); break;
                }
                break;
            case 69:   // performance gestures: a riser, a live break, the repeater
                switch (R(3)) {
                case 0: a.TriggerRiser(); break;
                case 1: a.ArmLiveBreak(); break;
                default: a.ToggleRepeater(); break;
                }
                break;
            case 70: {   // master FX: chain, EQ, comp, limiter values anywhere in their ranges
                MasterChainSettings& m = a.project.fx.master;
                m.chainOn = R(2); m.limiterOn = R(2); m.compOn = R(2);
                m.hpfHz = 20.0f + R(381); m.lowDb = R(19) - 9.0f; m.midDb = R(19) - 9.0f; m.highDb = R(19) - 9.0f; m.width = R(21) / 10.0f; m.saturation = R(11) / 10.0f;
                m.ceilingDb = -R(7) * 1.0f; m.compThreshDb = -R(41) * 1.0f; m.compRatio = 1.0f + R(10); m.compMix = R(11) / 10.0f;
                a.engine.GetMixer().SyncFxFromProject(a.project);
                break; }
            case 71: {   // open a damaged copy of the project: the load repairs it (or refuses it), the app adopts it and keeps playing
                const std::string f1 = "selftest_tmp_fuzz/dmg.t8proj";
                if (!SaveProject(p, f1, false)) break;
                std::string txt = Slurp(f1);
                static const char* kBad[] = {"500", "-7", "nan", "inf", "99999", "-1e30", "40", ""};
                for (int k = 0, c = 1 + R(6); k < c && !txt.empty(); ++k) {
                    size_t at = txt.find('=', static_cast<size_t>(R(static_cast<int>(txt.size()))));
                    if (at == std::string::npos) continue;
                    const size_t eol = txt.find('\n', at);
                    txt.replace(at + 1, (eol == std::string::npos ? txt.size() : eol) - at - 1, kBad[R(8)]);
                }
                { std::ofstream o(f1, std::ios::trunc); o << txt; }
                std::remove((f1 + ".bak").c_str());
                a.LoadProjectFile(f1);   // takes the lock itself; a refused file leaves the open project as it was
                a.projectPath = "selftest_tmp_fuzz/fuzz.t8proj";
                break; }
            case 60:   // v44.49: a style with a random set of parts, on whatever grid, length and key the scene has
                a.styleBeat = R(4) != 0; a.styleSound = R(2) != 0; a.styleNotes = R(4) != 0; a.styleTempo = R(4) == 0;
                a.ApplyStyle(R(static_cast<int>(AllStyles().size())));
                break;
            }
            a.TickTransport(); a.SyncBrickSound(); a.RefreshChordLive();
            uiLock.unlock();
            if (!threads) render(2 + R(6));
            failures += Check(a, kOpNames[op], seed, n);
            if (a.sequencer.SceneLockViolations() > 0) { std::cout << "SCENE LOCK MISSING seed " << seed << " op " << kOpNames[op] << "\n"; ++lockMisses; a.sequencer.ResetSceneLockViolations(); }
        }
        stop.store(true);
        if (audio.joinable()) audio.join();
        a.sequencer.Stop();
        if (nan.load()) { std::cout << "BAD AUDIO (NaN or > +18 dB) seed " << seed << ": " << nan.load() << " blocks\n"; nanBlocks += nan.load(); }
    }
    std::cout << "fuzz: " << seeds << " seeds x " << ops << " ops" << (threads ? " (audio thread)" : "") << ", invariant failures " << failures
              << ", round trips " << roundTrips << " (differ: " << roundTripDiffs << "), bad audio blocks " << nanBlocks << "\n";
    return (failures || roundTripDiffs || nanBlocks || lockMisses) ? 1 : 0;
}
