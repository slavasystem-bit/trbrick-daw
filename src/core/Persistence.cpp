// Simple key=value text format, grouped by [section] headers:
//
//   [project]
//   name=My Song
//   kitFolder=kits/909
//   currentPattern=0
//   randomizerKeyRoot=0
//   randomizerScale=1
//   masterVolume=0.4
//   (randomizerKeyRoot/randomizerScale/masterVolume are all optional on
//   read for backward compat with older saves -- randomizerKeyRoot/Scale
//   default to C/Dorian, masterVolume defaults to 0.4, same as a fresh
//   Project. masterVolume was added after a user reported it silently
//   resetting on every relaunch; see Mixer::masterVolume's comment.)
//
//   [synth0]  / [synth1] / [synth2]     -- one of these per synth patch
//   waveform=Saw
//   cutoffHz=900
//   ...
//   lfoTarget=Off            (Off/Cutoff/Pitch/Volume -- Phase 3 automation
//   lfoRateHz=2               LFO, see SynthPatch in Types.h; all 4 keys
//   lfoDepth=0.3               optional on read, defaulting to Off/no
//   lfoShape=Sine               modulation for saves from before this existed)
//   engineType=SUB           (Phase 5/5b: SUB/FM/WT, see SynthEngineType in
//   fmAlgorithm=0              Types.h. Optional on read -- missing keys
//   fmOp0Ratio=1               default to SUB/algorithm 0/ratio 1.0/level
//   fmOp0Level=0.5             0.5/wavetableBank 0/wtPosition 0.0 for every
//   fmOp1Ratio=1               saved patch from before FM/Wavetable existed,
//   fmOp1Level=0.5             same backward-compat convention as the LFO
//   fmOp2Ratio=1               keys above. fmOpN Ratio/Level and
//   fmOp2Level=0.5             wavetableBank/wtPosition all exist regardless
//   fmOp3Ratio=1               of engineType -- the other engines simply
//   fmOp3Level=0.5             ignore whichever fields aren't theirs, so
//   wavetableBank=0            switching a patch between engines never
//   wtPosition=0               loses any engine's settings.)
//
//   [chordpatch]                        -- Phase 6: the dedicated Chord
//   (same fields as [synthN] above --      track's own patch, same
//   this is a SynthPatch under a           SynthPatch struct/format as
//   different section name, nothing        synthN. Optional on read --
//   chord-specific about the format        missing on an older save just
//   itself)                                leaves it at SynthPatch's
//                                           defaults.
//
//   [pattern0] .. [pattern15]           -- one per pattern slot
//   name=PTN
//   masterLength=16
//   bpm=120
//   swing=0
//
//   [pattern0.track0] .. [pattern0.track9]  -- Phase 6: track9 is the
//   type=Drum                                  dedicated Chord track
//   slotIndex=0                                (type=Chord)
//   name=Kick
//   length=16
//   useOwnLength=0
//   volume=0.85
//   pan=0
//   mute=0
//   solo=0
//   drumTune=0
//   drumDecay=1
//   delaySend=0
//   reverbSend=0
//   insertDrive=0            (per-track insert FX -- see TrackInsertFx in
//   insertEqTilt=0            Effects.h. Optional on read, same backward
//   insertChorus=0            compat convention as delaySend/reverbSend --
//                              missing keys default to 0, fully bypassed.)
//   autopanRate=1             (4th insert-style stage, but applied to pan
//   autopanDepth=0             directly in Mixer::MixTrack -- see Track::
//                              autopanRate/autopanDepth in Types.h. Same
//                              optional-key convention, depth=0 default.)
//   eqHpfHz/eqHpfSlope/eqLowDb/eqLowHz/eqMidDb/eqMidHz/eqMidQ/eqHighDb/eqHighHz  (v29 channel EQ, only
//                              written when not flat; optional on read)
//   step000=active,accent,variant,note,lengthSteps,slide,ratchet[,chordSize,n0,n1,n2,n3,probability[,nudge,ghost[,vel[,n4,n5]]]]
//   (one per step, 0..length-1; the bracketed chordSize+4 notes+probability
//   are ONLY written when at least one of them is non-default -- see
//   WriteTrack -- so a project with no chords in it and every step still
//   at 100% probability produces byte-for-byte the same step lines as
//   before probability existed. ratchet is optional on read for backward
//   compat with older saves -- missing/short lines default it to 1, i.e.
//   no ratchet, default chordSize to 0, i.e. not a chord step even on a
//   Chord track, and default probability to 100, i.e. always fires.
//   delaySend/reverbSend are likewise optional keys -- missing on an
//   older save just leaves them at their struct default of 0, fully dry.)
//
//   [fx]                                  -- project-wide, not per-pattern
//   delayTimeSec=0.25       (derived/informational -- see delayDivisionIndex)
//   delayDivisionIndex=6
//   delayFeedback=0.35
//   delayLevel=0
//   reverbSize=0.6
//   reverbLevel=0
//   distortionDrive=0
//   sidechainEnabled=0
//   sidechainAmount=0.5
//   sidechainRelease=0.2
//   (also optional on read for backward compat with saves from before
//   effects/sidechain/tempo-synced-delay existed -- missing keys leave
//   FxSettings at its defaults. delayTimeSec on read is immediately
//   overwritten by the next audio block's tempo sync anyway -- see
//   Sequencer::RenderAndAdvance -- so it's saved mostly for readability/
//   debugging a project file by hand, not as the actual source of truth.)
//
//   [songchain]                    -- song/chain mode's programmed sequence,
//   length=3                          see ChainStep in Types.h. Optional on
//   step0=0,4                         read for backward compat with saves
//   step1=2,2                         from before this existed (missing ->
//   step2=1,1                         length 0, no chain programmed).
//   (stepN=patternIndex,repeats[,chordScene] -- chordScene -1 = none; only `length` of the kMaxChainSteps=64
//   slots are read/written; whether song mode is currently ON is NOT saved
//   here, see Sequencer::SetSongModeActive, same as play/stop state.)
//
// Robustness (format 2): "format=2" under [project], and a final "[end]" line.
//   Saving goes through <path>.tmp + fsync + rename (previous file kept as
//   <path>.bak); loading a file with format>=2 but no [end] (cut short) or with
//   an unparsable value falls back to <path>.bak. Each successful load also
//   copies the file to backups/<name>-<timestamp>.t8proj (newest 10 kept).
//
// This is intentionally not JSON/XML to avoid pulling in a parser
// dependency -- it's simple enough to hand-write robustly.
#include "Persistence.h"
#include "FileSafety.h"
#include "SoundParts.h"
#include "SamplerText.h"
#include "UserDirs.h"
#include "FMSynth.h"
#include "TrackEq.h"
#include "Wavetables.h"
#include "Scales.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <unordered_map>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <exception>
#include <cmath>
#include <vector>
#include <string>

namespace t8 {

namespace {
// v44.21: a lane as text, "pos:value,pos:value,..." (empty = flat); the reader accepts nothing it cannot make sense of.
std::string LaneToString(const Lane& l) {
    std::string out;
    for (int i = 0; i < l.n; ++i) { if (i) out += ","; out += std::to_string(static_cast<int>(l.pos[i])) + ":" + std::to_string(static_cast<int>(l.val[i])); }
    return out;
}
// v44.38 / v44.39: parameter rows as text, "track,param,lane;track,param,lane" (a row stays even while its lane is flat)
std::string ParamsToString(const StepParams& sp) {
    std::string out;
    for (int k = 0; k < sp.n; ++k) out += (k ? ";" : "") + std::to_string(static_cast<int>(sp.row[k].track)) + "," + std::to_string(static_cast<int>(sp.row[k].param)) + "," + LaneToString(sp.row[k].lane);
    return out;
}
Lane LaneFromString(const std::string& text, int lo, int hi);
StepParams ParamsFromString(const std::string& val) {
    std::istringstream ss(val); std::string part; StepParams sp;
    while (std::getline(ss, part, ';') && sp.n < kMaxParamRows) {
        const size_t c1 = part.find(','), c2 = c1 == std::string::npos ? c1 : part.find(',', c1 + 1);
        if (c2 == std::string::npos) continue;
        const int k = sp.Add(std::atoi(part.substr(0, c1).c_str()), std::atoi(part.substr(c1 + 1, c2 - c1 - 1).c_str()));
        if (k >= 0) sp.row[k].lane = LaneFromString(part.substr(c2 + 1), -100, 100);
    }
    return NormalizeParams(sp);
}
Lane LaneFromString(const std::string& text, int lo, int hi) {
    Lane l;
    std::istringstream ss(text); std::string tok;
    while (std::getline(ss, tok, ',') && l.n < kMaxLanePts) {
        const size_t c = tok.find(':');
        if (c == std::string::npos) return Lane{};
        l.pos[l.n] = static_cast<uint8_t>(std::clamp(std::atoi(tok.substr(0, c).c_str()), 0, kLanePosMax));
        l.val[l.n] = static_cast<int8_t>(std::clamp(std::atoi(tok.substr(c + 1).c_str()), lo, hi));
        ++l.n;
    }
    l.Normalize(lo, hi);
    return l;
}


Waveform WaveformFromName(const std::string& s) {
    if (s == "Sine") return Waveform::Sine;
    if (s == "Tri") return Waveform::Triangle;
    if (s == "Saw") return Waveform::Saw;
    if (s == "Sqr") return Waveform::Square;
    return Waveform::Saw;
}

LfoTarget LfoTargetFromName(const std::string& s) {
    return LfoTargetFromText(s);
}

// `section` is the full "[...]" header text minus the brackets -- e.g.
// "synth0" for the 3 melodic synths, or "chordpatch" for the dedicated
// Chord track's patch (Phase 6) -- same SynthPatch struct/field format
// either way, just a different section name, so ApplySynthPreset-style
// code never needed its own copy of this writer.
void WriteSynth(std::ostream& f, const std::string& section, const SynthPatch& p) {
    f << "[" << section << "]\n";
    f << "waveform=" << WaveformName(p.waveform) << "\n";
    f << "cutoffHz=" << p.cutoffHz << "\n";
    f << "resonance=" << p.resonance << "\n";
    f << "envAmount=" << p.envAmount << "\n";
    f << "attack=" << p.attack << "\n";
    f << "decay=" << p.decay << "\n";
    f << "sustain=" << p.sustain << "\n";
    f << "release=" << p.release << "\n";
    f << "filterDecay=" << p.filterDecay << "\n";
    f << "glideTime=" << p.glideTime << "\n";
    f << "volume=" << p.volume << "\n";
    f << "lfoTarget=" << LfoTargetName(p.lfoTarget) << "\n";
    f << "lfoRateHz=" << p.lfoRateHz << "\n";
    f << "lfoDepth=" << p.lfoDepth << "\n";
    f << "lfoShape=" << WaveformName(p.lfoShape) << "\n";
    const char* engineTypeStr = "SUB";
    if (p.engineType == SynthEngineType::FM) engineTypeStr = "FM";
    else if (p.engineType == SynthEngineType::Wavetable) engineTypeStr = "WT";
    else if (p.engineType == SynthEngineType::Sample) engineTypeStr = "SMP";
    f << "engineType=" << engineTypeStr << "\n";
    f << "fmAlgorithm=" << p.fmAlgorithm << "\n";
    for (int i = 0; i < 4; ++i) {
        f << "fmOp" << i << "Ratio=" << p.fmOps[i].ratio << "\n";
        f << "fmOp" << i << "Level=" << p.fmOps[i].level << "\n";
    }
    f << "wavetableBank=" << p.wavetableBank << "\n";
    if (p.fmIndexScale != 8.0f) f << "fmIndexScale=" << p.fmIndexScale << "\n";   // v44, only when changed
    if (p.fmModEnv > 0.0f) f << "fmModEnv=" << p.fmModEnv << "\n";   // v44, only when used
    f << "wtPosition=" << p.wtPosition << "\n";
    // v26 thickening (optional on read)
    f << "unisonVoices=" << p.unisonVoices << "\n";
    f << "unisonDetune=" << p.unisonDetune << "\n";
    f << "unisonSpread=" << p.unisonSpread << "\n";
    f << "subLevel=" << p.subLevel << "\n";
    f << "subOctave=" << p.subOctave << "\n";
    f << "noiseLevel=" << p.noiseLevel << "\n";
    f << "drive=" << p.drive << "\n";
    if (p.phaseSync) f << "phaseSync=1\n";   // v30, only when on: files stay readable by older builds
    f << "osc2Wave=" << WaveformName(p.osc2Wave) << "\n";
    f << "osc2Level=" << p.osc2Level << "\n";
    f << "osc2Semi=" << p.osc2Semi << "\n";
    f << "osc2Detune=" << p.osc2Detune << "\n";
    f << "filter24=" << (p.filter24 ? 1 : 0) << "\n";
    f << "acid=" << (p.acid ? 1 : 0) << "\n";
    f << "drift=" << p.drift << "\n";
    f << "keytrack=" << p.keytrack << "\n";
    if (p.engineType == SynthEngineType::Sample || p.sample.HasFile() || p.sample.mode != 0 || p.sample.markCount > 0)   // v44.53, only when used: older files stay byte for byte the same
        f << "sample=" << SamplerToString(ToSamplerPatch(p.sample)) << "\n";
    f << "\n";
}

// One step as the comma list after "stepN=" / "eorigN=" (also used for the original of EUCLID ON).
void WriteStepFields(std::ostream& f, const Step& s, bool isChord) {
    f << (s.active ? 1 : 0) << "," << (s.accent ? 1 : 0) << ","
      << static_cast<int>(s.variant) << "," << static_cast<int>(s.note) << ","
      << static_cast<int>(s.lengthSteps) << "," << (s.slide ? 1 : 0) << ","
      << static_cast<int>(s.ratchet);
    // Phase 6 chordSize+notes AND probability are both optional
    // trailing fields, positionally fixed (chordSize, n0..n3,
    // probability -- 6 fields together) so old saves' chord data keeps
    // landing at the same indices it always has. Written together
    // whenever EITHER is non-default, so a step that only has a
    // non-default probability (no chord data) still gets chordSize=0/
    // the default chordNotes as harmless placeholders ahead of it --
    // simpler than a sparse/independent suffix, and a save with
    // neither (the overwhelming common case) is still byte-for-byte
    // identical to what this app wrote before probability existed.
    // v39 appends nudge and ghost after probability (written, with the placeholders before them, only when used).
    if ((isChord && s.chordSize > 0) || s.probability != 100 || s.nudge != 0 || s.ghost || s.vel != 0) {
        f << "," << static_cast<int>(s.chordSize);
        for (int n = 0; n < 4; ++n) f << "," << static_cast<int>(s.chordNotes[n]);
        f << "," << static_cast<int>(s.probability);
        // nudge, ghost, vel and (v44.43) the 5th / 6th chord tone are positional suffixes: each one forces the placeholders before it.
        const bool big = s.chordSize > 4;
        const bool wantVel = s.vel != 0 || big;
        const bool wantNg = s.nudge != 0 || s.ghost || wantVel;
        if (wantNg) f << "," << static_cast<int>(s.nudge) << "," << (s.ghost ? 1 : 0);
        if (wantVel) f << "," << static_cast<int>(s.vel);   // v40
        if (big) f << "," << static_cast<int>(s.chordNotes[4]) << "," << static_cast<int>(s.chordNotes[5]);
    }
}

void WriteTrackBody(std::ostream& f, const Track& t) {
    const char* typeStr = "Drum";
    if (t.type == TrackType::Synth) typeStr = "Synth";
    else if (t.type == TrackType::Chord) typeStr = "Chord";
    else if (t.type == TrackType::Sampler) typeStr = "Sampler";
    f << "type=" << typeStr << "\n";
    f << "slotIndex=" << t.slotIndex << "\n";
    f << "name=" << t.name << "\n";
    f << "length=" << t.length << "\n";
    f << "useOwnLength=" << (t.useOwnLength ? 1 : 0) << "\n";
    f << "volume=" << t.volume << "\n";
    f << "pan=" << t.pan << "\n";
    if (t.swing >= 0.0f) f << "trackSwing=" << t.swing << "\n";
    if (!t.kitFolder.empty()) f << "trackKit=" << t.kitFolder << "\n";   // v44.26: this drum has a kit of its own
    if (t.defaultVariant != 0) f << "defaultVariant=" << static_cast<int>(t.defaultVariant) << "\n";   // v43.1
    if (!t.grooveOn) f << "grooveOn=0\n";   // v42   // v39, only when it overrides the pattern swing
    f << "mute=" << (t.mute ? 1 : 0) << "\n";
    f << "solo=" << (t.solo ? 1 : 0) << "\n";
    f << "drumTune=" << t.drumTune << "\n";
    f << "drumDecay=" << t.drumDecay << "\n";
    if (!t.edit.IsDefault())   // v44.52, only when used
        f << "sedit=" << t.edit.start << "|" << t.edit.length << "|" << t.edit.attackMs << "|" << static_cast<int>(t.edit.fineCents) << "|" << static_cast<int>(t.edit.gainDb) << "|" << (t.edit.reverse ? 1 : 0) << "\n";
    f << "delaySend=" << t.delaySend << "\n";
    f << "reverbSend=" << t.reverbSend << "\n";
    f << "insertDrive=" << t.insertDrive << "\n";
    f << "insertEqTilt=" << t.insertEqTilt << "\n";
    f << "insertChorus=" << t.insertChorus << "\n";
    f << "autopanRate=" << t.autopanRate << "\n";
    f << "autopanDepth=" << t.autopanDepth << "\n";
    f << "width=" << t.width << "\n";
    f << "spread=" << t.spread << "\n";
    // channel EQ (v29): only written when something is not flat, so old files stay byte-identical
    if (t.eqHpfHz > 20.5f || t.eqLowDb != 0 || t.eqMidDb != 0 || t.eqHighDb != 0) {
        f << "eqHpfHz=" << t.eqHpfHz << "\n" << "eqHpfSlope=" << t.eqHpfSlope << "\n"
          << "eqLowDb=" << t.eqLowDb << "\n" << "eqLowHz=" << t.eqLowHz << "\n"
          << "eqMidDb=" << t.eqMidDb << "\n" << "eqMidHz=" << t.eqMidHz << "\n" << "eqMidQ=" << t.eqMidQ << "\n"
          << "eqHighDb=" << t.eqHighDb << "\n" << "eqHighHz=" << t.eqHighHz << "\n";
    }
    if (t.euclidOn && t.type != TrackType::Chord) {   // v44.55, only when on: older files stay byte for byte the same
        f << "euclid=" << static_cast<int>(t.euclidHits) << "," << static_cast<int>(t.euclidSteps) << "," << static_cast<int>(t.euclidRotate) << "\n";
        for (const auto& o : t.euclidOrig) { f << "eorig" << static_cast<int>(o.first) << "="; WriteStepFields(f, o.second, false); f << "\n"; }
    }
    for (int i = 0; i < kMaxSteps; ++i) {
        const Step& s = t.steps[i];
        if (!s.active) continue; // sparse: only write steps that are on
        f << "step" << i << "=";
        WriteStepFields(f, s, t.type == TrackType::Chord);
        f << "\n";
    }
    f << "\n";
}

void WriteFxBlock(std::ostream& f, const Project& project) {
    f << "[fx]\n";
    f << "delayTimeSec=" << project.fx.delayTimeSec << "\n";
    f << "delayDivisionIndex=" << project.fx.delayDivisionIndex << "\n";
    f << "delayFeedback=" << project.fx.delayFeedback << "\n";
    f << "delayLevel=" << project.fx.delayLevel << "\n";
    f << "delayTone=" << project.fx.delayTone << "\n";
    f << "delayPingPong=" << (project.fx.delayPingPong ? 1 : 0) << "\n";
    f << "reverbSize=" << project.fx.reverbSize << "\n";
    f << "reverbDamp=" << project.fx.reverbDamp << "\n";
    f << "reverbPreMs=" << project.fx.reverbPreMs << "\n";
    f << "reverbLevel=" << project.fx.reverbLevel << "\n";
    f << "distortionDrive=" << project.fx.distortionDrive << "\n";
    f << "kickMode=" << project.fx.kick.mode << "\n";
    f << "kickHz=" << project.fx.kick.pitchHz << "\n";
    f << "kickKeyLock=" << (project.fx.kick.keyLock ? 1 : 0) << "\n";
    f << "kickSweep=" << project.fx.kick.sweep << "\n";
    f << "kickSweepMs=" << project.fx.kick.sweepMs << "\n";
    f << "kickDecayMs=" << project.fx.kick.decayMs << "\n";
    f << "kickClick=" << project.fx.kick.click << "\n";
    f << "kickDrive=" << project.fx.kick.drive << "\n";
    f << "kickLevel=" << project.fx.kick.level << "\n";
    f << "sidechainEnabled=" << (project.fx.sidechainEnabled ? 1 : 0) << "\n";
    f << "sidechainAmount=" << project.fx.sidechainAmount << "\n";
    f << "sidechainRelease=" << project.fx.sidechainRelease << "\n";
    {   // master compressor + finished-track chain (optional keys on read)
        const MasterChainSettings& m = project.fx.master;
        f << "mcChainOn=" << (m.chainOn ? 1 : 0) << "\n";
        f << "mcChainPreset=" << m.chainPreset << "\n";
        f << "mcHpfHz=" << m.hpfHz << "\n";
        f << "mcLowDb=" << m.lowDb << "\n";
        f << "mcMidDb=" << m.midDb << "\n";
        f << "mcHighDb=" << m.highDb << "\n";
        f << "mcWidth=" << m.width << "\n";
        f << "mcSaturation=" << m.saturation << "\n";
        f << "mcLimiterOn=" << (m.limiterOn ? 1 : 0) << "\n";
        f << "mcCeilingDb=" << m.ceilingDb << "\n";
        f << "mcMonoBassHz=" << m.monoBassHz << "\n";
        f << "mcCompScHpfHz=" << m.compScHpfHz << "\n";
        f << "mcCompOn=" << (m.compOn ? 1 : 0) << "\n";
        f << "mcCompPreset=" << m.compPreset << "\n";
        f << "mcCompThreshDb=" << m.compThreshDb << "\n";
        f << "mcCompRatio=" << m.compRatio << "\n";
        f << "mcCompAttackMs=" << m.compAttackMs << "\n";
        f << "mcCompReleaseMs=" << m.compReleaseMs << "\n";
        f << "mcCompMakeupDb=" << m.compMakeupDb << "\n";
        f << "mcCompMix=" << m.compMix << "\n\n";
    }
}

void WriteTrack(std::ostream& f, int patIdx, int trackIdx, const Track& t) {
    f << "[pattern" << patIdx << ".track" << trackIdx << "]\n";
    WriteTrackBody(f, t);
}

} // namespace

// Text dump of everything that shapes ONE instrument's sound (for pasting into a bug report / a question): the synth
// patch or the drum voice + kick synth, the track's mixer / EQ / sends / pattern steps, and the effects it runs through.
std::string DescribeInstrument(const Project& project, const Pattern& pat, int trackIndex) {
    trackIndex = std::clamp(trackIndex, 0, kNumTracks - 1);
    const Track& t = pat.tracks[trackIndex];
    std::ostringstream o;
    const char* kind = t.type == TrackType::Drum ? "DRUM" : t.type == TrackType::Chord ? "CHORD" : t.type == TrackType::Sampler ? "SAMPLER" : "SYNTH";
    o << "TR-Brick DAW instrument parameters\n";
    o << "instrument=" << t.name << " (" << kind << ", track " << trackIndex << ", slot " << t.slotIndex << ")\n";
    o << "pattern=" << pat.name << " bpm=" << pat.bpm << " swing=" << pat.swing << " masterLength=" << pat.masterLength << "\n";
    o << "key=" << project.randomizerKeyRoot << " scale=" << project.randomizerScale << " kitFolder=" << (pat.kitFolder.empty() ? project.kitFolder : pat.kitFolder)
      << " masterVolume=" << project.masterVolume << "\n\n";
    if (t.type == TrackType::Synth && t.slotIndex >= 0 && t.slotIndex < kNumSynthTracks)
        WriteSynth(o, "synth" + std::to_string(t.slotIndex), pat.synthPatches[static_cast<size_t>(t.slotIndex)]);
    else if (t.type == TrackType::Chord)
        WriteSynth(o, "chordpatch", pat.chordPatch);
    else if (t.type == TrackType::Sampler)
        o << "sampler=" << SamplerToString(pat.samplerPatch) << "\n";
    o << "[track]\n";
    WriteTrackBody(o, t);
    WriteFxBlock(o, project);
    return o.str();
}

std::string SoundPartText(const Pattern& pat, int part) {
    std::ostringstream o;
    if (part == kPartKit) { o << pat.kitFolder; return o.str(); }
    if (part < 0 || part >= kNumTracks) return o.str();
    Track t = pat.tracks[static_cast<size_t>(part)];
    t.steps.fill(Step{});
    t.length = 16; t.useOwnLength = false; t.swing = -1.0f; t.grooveOn = true; t.mute = false; t.solo = false;
    t.euclidOn = false; t.euclidOrig.clear();   // v44.55: the sound of a track does not carry the Euclid state
    WriteTrackBody(o, t);
    if (part == static_cast<int>(DrumSlot::Kick)) {
        const KickSynthSettings& k = pat.kick;
        o << "kick=" << k.mode << "," << k.pitchHz << "," << k.keyLock << "," << k.sweep << "," << k.sweepMs << "," << k.decayMs << "," << k.click << "," << k.drive << "," << k.level << "\n";
    } else if (part >= kNumDrumTracks && part < kChordTrackIndex) WriteSynth(o, "s", pat.synthPatches[static_cast<size_t>(part - kNumDrumTracks)]);
    else if (part == kChordTrackIndex) WriteSynth(o, "c", pat.chordPatch);
    else if (part == kSamplerTrackIndex) o << "sampler=" << SamplerToString(pat.samplerPatch) << "\n";
    return o.str();
}

// Crash-safe save: the project is written to "<path>.tmp", checked, flushed
// to disk, and only then swapped in with rename() (atomic on POSIX). The file
// being replaced is kept as "<path>.bak". A power cut or crash mid-write can
// therefore never leave a half-written project at <path>; LoadProject() falls
// back to the .bak if it ever finds an incomplete file (no [end] marker).
// The whole file as text. SaveProject writes it to disk; ProjectToString lets the program ask "is this still what is saved?"
// (the Edited mark) without touching the disk.
static void WriteProject(std::ostream& f, const Project& project) {
    f << "[project]\n";
    f << "format=" << kProjectFormatVersion << "\n";
    f << "name=" << project.name << "\n";
    f << "kitFolder=" << project.kitFolder << "\n";
    // v44.22: the TRACK tab may be editing a brick slot (>= kNumLiveScenes); the file remembers the LIVE scene to open on
    f << "currentPattern=" << (project.currentPattern >= kNumLiveScenes ? std::clamp(project.liveScene, 0, kNumLiveScenes - 1) : project.currentPattern) << "\n";
    if (project.trackSoundMask != 0) f << "trackSound=" << project.trackSoundMask << "\n";   // v44.23: the sound of the track is [pattern80]
    {   // v44.58: linked scenes (only written when there are any)
        bool any = false; for (uint8_t g : project.sceneLink) if (g) any = true;
        if (any) { f << "sceneLink="; for (int i = 0; i < kNumLiveScenes; ++i) f << (i ? "," : "") << static_cast<int>(project.sceneLink[static_cast<size_t>(i)]); f << "\n"; }
    }
    f << "chordsFollowKey=" << (project.chordsFollowKey ? 1 : 0) << "\n";
    f << "chordBankOn=" << (project.chordBankOn ? 1 : 0) << "\n";
    f << "chordSceneEdit=" << project.chordSceneEdit << "\n";
    f << "randomizerKeyRoot=" << project.randomizerKeyRoot << "\n";
    f << "randomizerScale=" << project.randomizerScale << "\n";
    f << "rndOctaves=" << static_cast<int>(project.rndOctaves[0]) << "," << static_cast<int>(project.rndOctaves[1]) << "," << static_cast<int>(project.rndOctaves[2]) << "\n";   // v44.3
    for (int i = 0; i < 3; ++i) {   // v44.4
        const RhythmRnd& r = project.rndRhythm[static_cast<size_t>(i)];
        f << "rndRhythm" << i << "=" << static_cast<int>(r.minNotes) << "," << static_cast<int>(r.maxNotes) << "," << static_cast<int>(r.pitch) << ","
          << static_cast<int>(r.length) << "," << (r.onTheOne ? 1 : 0) << "," << (r.sameEveryBar ? 1 : 0) << "\n";
    }
    f << "masterVolume=" << project.masterVolume << "\n\n";

    WriteFxBlock(f, project);

    f << "[songchain]\n";
    f << "length=" << project.songChainLength << "\n";
    for (int i = 0; i < project.songChainLength; ++i) {
        f << "step" << i << "=" << project.songChain[i].patternIndex << "," << project.songChain[i].repeats
          << "," << project.songChain[i].chordScene << "\n";
        const ChainStep& cs = project.songChain[i];   // v44.5 overlays, written only when something is set
        if (cs.muteMask != 0 || cs.fillMode != FillMode::Scene || cs.riser >= 0 || cs.trimSteps != 0)
            f << "arr" << i << "=" << cs.muteMask << "," << static_cast<int>(cs.fillMode) << "," << static_cast<int>(cs.fill.when) << "," << static_cast<int>(cs.fill.kind) << ","
              << static_cast<int>(cs.fill.beats) << "," << static_cast<int>(cs.fill.everyBars) << "," << static_cast<int>(cs.fill.breakIndex) << ","
              << static_cast<int>(cs.riser) << "," << static_cast<int>(cs.riserBars) << "," << static_cast<int>(cs.trimSteps) << "\n";
        if (cs.fill.shiftBeats != 0) f << "fsh" << i << "=" << static_cast<int>(cs.fill.shiftBeats) << "\n";   // v44.25: a fill moved earlier inside the brick
        if (cs.automation.Any())   // v44.21: the MIX lane and the DJ filter lane, each as "pos:value,pos:value,..."
            f << "dyn" << i << "=" << LaneToString(cs.automation.mix) << ";" << LaneToString(cs.automation.filter) << "\n";
        if (cs.levels.Any()) {   // v44.13 / v44.21: transition and fill levels, then one lane per instrument
            f << "lv2" << i << "=" << static_cast<int>(cs.levels.transitionDb) << "," << static_cast<int>(cs.levels.fillDb);
            for (int t = 0; t < kArrTracks; ++t) f << ";" << LaneToString(cs.levels.track[t]);
            f << "\n";
        }
        if (cs.params.n > 0) f << "prm" << i << "=" << ParamsToString(cs.params) << "\n";   // v44.38
    }
    f << "\n";

    // v44.5.4: the sound is per scene ([patternN.synthM] / [patternN.chordpatch] below); these are the base sound of the file (scene 1's)
    for (int i = 0; i < kNumSynthTracks; ++i) WriteSynth(f, "synth" + std::to_string(i), project.patterns[0].synthPatches[static_cast<size_t>(i)]);
    WriteSynth(f, "chordpatch", project.patterns[0].chordPatch); // Phase 6

    std::array<bool, kNumSlotsTotal> slotUsed{};   // the 16 live scenes always; a brick slot only while a brick points at it; the track's sound slot when it has a sound
    slotUsed[static_cast<size_t>(kTrackSoundSlot)] = project.trackSoundMask != 0;
    for (int p = 0; p < kNumLiveScenes; ++p) slotUsed[static_cast<size_t>(p)] = true;
    for (int i = 0; i < project.songChainLength; ++i) slotUsed[static_cast<size_t>(std::clamp(project.songChain[static_cast<size_t>(i)].patternIndex, 0, kNumPatterns - 1))] = true;
    for (int p = 0; p < kNumSlotsTotal; ++p) {
        if (!slotUsed[static_cast<size_t>(p)]) continue;
        const Pattern& pat = project.patterns[p];
        f << "[pattern" << p << "]\n";
        f << "name=" << pat.name << "\n";
        f << "masterLength=" << pat.masterLength << "\n";
        f << "bpm=" << pat.bpm << "\n";
        f << "swing=" << pat.swing << "\n";
        if (pat.stepsPerBeat != 4) f << "stepsPerBeat=" << pat.stepsPerBeat << "\n";   // v43
        if (pat.role != SceneRole::None) f << "role=" << static_cast<int>(pat.role) << "\n";   // v44.5
        if (pat.fill.when != FillWhen::Off)
            f << "fill=" << static_cast<int>(pat.fill.when) << "," << static_cast<int>(pat.fill.kind) << "," << static_cast<int>(pat.fill.beats) << ","
              << static_cast<int>(pat.fill.everyBars) << "," << static_cast<int>(pat.fill.breakIndex) << "\n";
        if (pat.grooveAmount > 0.0f || !pat.groove.IsFlat() || !pat.groove.name.empty()) {   // v42, only when used
            f << "grooveName=" << pat.groove.name << "\ngrooveAmount=" << pat.grooveAmount << "\ngroove=";
            for (int g = 0; g < 16; ++g) f << (g ? "," : "") << static_cast<int>(pat.groove.steps[g].nudge) << ":" << static_cast<int>(pat.groove.steps[g].vel);
            f << "\n";
        }
        if (!pat.kitFolder.empty()) f << "kitFolder=" << pat.kitFolder << "\n";   // v44.5.4, only when the scene has its own kit
        if (!pat.samplerPatch.file.empty() || pat.samplerPatch != SamplerPatch{}) f << "sampler=" << SamplerToString(pat.samplerPatch) << "\n";   // v44.50
        if (pat.params.n > 0) f << "params=" << ParamsToString(pat.params) << "\n";   // v44.39: the parameter rows of a LIVE scene
        if (pat.ownSound != 0) f << "ownSound=" << pat.ownSound << "\n";   // v44.24, a brick with an own sound for some instruments
        {   // v44.5.4: the built-in kick of the scene
            const KickSynthSettings& k = pat.kick;
            f << "kickSynth=" << k.mode << "," << k.pitchHz << "," << (k.keyLock ? 1 : 0) << "," << k.sweep << "," << k.sweepMs << "," << k.decayMs << ","
              << k.click << "," << k.drive << "," << k.level << "\n";
        }
        if (pat.chordsKey >= 0) f << "chordsKey=" << static_cast<int>(pat.chordsKey) << "\nchordsScale=" << static_cast<int>(pat.chordsScale) << "\n";
        f << "\n";
        for (int i = 0; i < kNumSynthTracks; ++i) WriteSynth(f, "pattern" + std::to_string(p) + ".synth" + std::to_string(i), pat.synthPatches[static_cast<size_t>(i)]);
        WriteSynth(f, "pattern" + std::to_string(p) + ".chordpatch", pat.chordPatch);
        for (int t = 0; t < kNumTracks; ++t) WriteTrack(f, p, t, pat.tracks[t]);
    }
    for (int c = 0; c < kNumChordScenes; ++c) {        // chord bank (optional on read)
        const ChordScene& cs = project.chordScenes[c];
        bool any = false;
        for (const Step& st : cs.track.steps) if (st.active) { any = true; break; }
        if (!any && cs.name.empty() && cs.key < 0 && cs.track.length == 16 && !cs.track.useOwnLength) continue;
        f << "[chordscene" << c << "]\n";
        f << "sceneName=" << cs.name << "\n";
        f << "sceneKey=" << static_cast<int>(cs.key) << "\n";
        f << "sceneScale=" << static_cast<int>(cs.scale) << "\n";
        WriteTrackBody(f, cs.track);
    }
    f << "[end]\n";   // completeness marker (format >= 2): a file without it was cut short
}

std::string ProjectToString(const Project& project) {
    std::ostringstream os;
    WriteProject(os, project);
    return os.str();
}

bool SaveProject(const Project& project, const std::string& path, bool keepHistory) {
    const std::string tmpPath = path + ".tmp";
    std::ofstream f(tmpPath, std::ios::trunc);
    if (!f) {
        std::cerr << "[persist] cannot open for write: " << tmpPath << "\n";
        return false;
    }
    WriteProject(f, project);
    f.flush();
    bool ok = f.good();
    f.close();
    ok = ok && !f.fail();
    if (!ok) {
        std::cerr << "[persist] write failed (disk full?): " << tmpPath << "\n";
        std::remove(tmpPath.c_str());
        return false;
    }
    // Force the bytes to the device before the swap so the rename can never
    // publish a file whose contents are still only in the OS cache.
    FlushFileToDisk(tmpPath);
    struct stat st;
    if (keepHistory && ::stat(path.c_str(), &st) == 0) {
        // First save of this file in this process: keep a dated copy of what
        // was there before. The app starts every session blank and autosaves
        // over project.t8proj, so without this yesterday's work would be gone
        // after two autosaves (the .bak below only holds the previous save).
        static std::vector<std::string> backedUpThisSession;
        if (std::find(backedUpThisSession.begin(), backedUpThisSession.end(), path) == backedUpThisSession.end()) {
            backedUpThisSession.push_back(path);
            BackupProjectFile(path);
        }
        // The previous save becomes <path>.bak as a second name of the same file (hard link), so <path> itself is never
        // missing, not even for a moment: the rename below swaps the new contents in atomically.
        const std::string bak = path + ".bak";
        std::remove(bak.c_str());
        if (::link(path.c_str(), bak.c_str()) != 0) {   // a disk without hard links: copy instead
            std::string prev;
            std::ifstream in(path, std::ios::binary);
            if (in) { std::ostringstream os; os << in.rdbuf(); prev = os.str(); WriteFileAtomic(bak, prev); }
        }
    }
    if (std::rename(tmpPath.c_str(), path.c_str()) != 0) {
        std::cerr << "[persist] rename failed: " << tmpPath << " -> " << path << "\n";
        std::remove(tmpPath.c_str());   // <path> still holds the previous version
        return false;
    }
    FlushDirOf(path);
    return true;
}

namespace {
// ---- Load-time sanity pass (DATA-3) ---------------------------------------------------------------------------------------
// The reader trusts many numbers it parses (lengths, slot indices, levels). A damaged or hand-edited file must never make the
// engine read past an array or feed NaN into the audio, so after parsing every value that indexes something or reaches the
// DSP is forced into its legal range. Each value that had to change is counted; LoadProject reports the count to the user.
// Values a normal save writes are always inside these ranges, so a healthy file is left exactly as it was.
struct Sanitizer {
    int fixed = 0;
    void F(float& v, float lo, float hi, float def) {
        if (!std::isfinite(v)) { v = def; ++fixed; return; }
        if (v < lo) { v = lo; ++fixed; } else if (v > hi) { v = hi; ++fixed; }
    }
    void I(int& v, int lo, int hi) {
        if (v < lo) { v = lo; ++fixed; } else if (v > hi) { v = hi; ++fixed; }
    }
    void Note(int8_t& n) { if (n < 0) { n = 0; ++fixed; } }   // int8_t never exceeds 127
    void Patch(SynthPatch& p) {
        const SynthPatch d;
        F(p.cutoffHz, 20.0f, 20000.0f, d.cutoffHz);
        F(p.resonance, 0.0f, 0.99f, d.resonance);
        F(p.envAmount, -1.0f, 1.0f, d.envAmount);
        F(p.attack, 0.0f, 10.0f, d.attack);
        F(p.decay, 0.0f, 10.0f, d.decay);
        F(p.sustain, 0.0f, 1.0f, d.sustain);
        F(p.release, 0.0f, 10.0f, d.release);
        F(p.filterDecay, 0.0f, 10.0f, d.filterDecay);
        F(p.glideTime, 0.0f, 2.0f, d.glideTime);
        F(p.volume, 0.0f, 2.0f, d.volume);
        F(p.lfoRateHz, 0.01f, 20.0f, d.lfoRateHz);
        F(p.lfoDepth, 0.0f, 1.0f, d.lfoDepth);
        I(p.fmAlgorithm, 0, kNumFmAlgorithms - 1);
        for (auto& op : p.fmOps) { F(op.ratio, 0.25f, 16.0f, 1.0f); F(op.level, 0.0f, 1.0f, 0.5f); }
        I(p.wavetableBank, 0, kNumWavetableBanks - 1);
        I(p.unisonVoices, 1, kMaxUnisonVoices);
    }
    void TrackAt(Track& t, TrackType type, int slot) {
        if (t.type != type) { t.type = type; ++fixed; }
        if (t.slotIndex != slot) { t.slotIndex = slot; ++fixed; }
        I(t.length, 1, kMaxSteps);
        for (Step& s : t.steps) {
            Note(s.note);
            for (auto& n : s.chordNotes) Note(n);
            if (s.lengthSteps < 1) { s.lengthSteps = 1; ++fixed; }
        }
        if (t.euclidSteps < 1 || t.euclidSteps > kMaxSteps) { t.euclidSteps = static_cast<uint8_t>(std::clamp<int>(t.euclidSteps, 1, kMaxSteps)); ++fixed; }
        if (t.euclidHits > t.euclidSteps) { t.euclidHits = t.euclidSteps; ++fixed; }
        if (!std::isfinite(t.swing)) { t.swing = -1.0f; ++fixed; }
        F(t.volume, 0.0f, 1.0f, 0.8f);
        F(t.pan, -1.0f, 1.0f, 0.0f);
        F(t.drumTune, -24.0f, 24.0f, 0.0f);
        F(t.drumDecay, 0.25f, 2.0f, 1.0f);
        F(t.delaySend, 0.0f, 1.0f, 0.0f);
        F(t.reverbSend, 0.0f, 1.0f, 0.0f);
        F(t.insertDrive, 0.0f, 1.0f, 0.0f);
        F(t.insertEqTilt, -1.0f, 1.0f, 0.0f);
        F(t.insertChorus, 0.0f, 1.0f, 0.0f);
        F(t.autopanRate, 0.05f, 8.0f, 1.0f);
        F(t.autopanDepth, 0.0f, 1.0f, 0.0f);
    }
    void PatternAt(Pattern& p) {
        I(p.masterLength, 1, kMaxSteps);
        F(p.bpm, 20.0f, 300.0f, 120.0f);
        F(p.swing, 0.0f, 0.75f, 0.0f);
        for (auto& sp : p.synthPatches) Patch(sp);
        Patch(p.chordPatch);
        for (int i = 0; i < kNumTracks; ++i) {
            Track& t = p.tracks[static_cast<size_t>(i)];
            if (i < kNumDrumTracks) TrackAt(t, TrackType::Drum, i);
            else if (i < kChordTrackIndex) TrackAt(t, TrackType::Synth, i - kNumDrumTracks);
            else if (i == kChordTrackIndex) TrackAt(t, TrackType::Chord, 0);
            else TrackAt(t, TrackType::Sampler, i - kSamplerTrackIndex);
        }
    }
    void ProjectAll(Project& pj) {
        for (int s = 0; s < kNumSlotsTotal; ++s) PatternAt(pj.patterns[static_cast<size_t>(s)]);
        for (auto& sp : pj.baseSynthPatches) Patch(sp);
        Patch(pj.baseChordPatch);
        for (auto& cs : pj.chordScenes) TrackAt(cs.track, TrackType::Chord, 0);
        I(pj.currentPattern, 0, kNumPatterns - 1);
        I(pj.liveScene, 0, kNumLiveScenes - 1);
        I(pj.chordSceneEdit, 0, kNumChordScenes - 1);
        I(pj.randomizerKeyRoot, 0, kNumKeys - 1);
        I(pj.randomizerScale, 0, kNumScales - 1);
        I(pj.songChainLength, 0, kMaxChainSteps);
        F(pj.masterVolume, 0.0f, 1.2f, 0.4f);
        FxSettings& fx = pj.fx;
        const FxSettings d;
        F(fx.delayTimeSec, 0.02f, 2.0f, d.delayTimeSec);
        I(fx.delayDivisionIndex, 0, kNumDelayDivisions - 1);
        F(fx.delayFeedback, 0.0f, 0.95f, d.delayFeedback);
        F(fx.delayLevel, 0.0f, 1.0f, d.delayLevel);
        F(fx.reverbSize, 0.0f, 0.97f, d.reverbSize);
        F(fx.reverbLevel, 0.0f, 1.0f, d.reverbLevel);
        F(fx.distortionDrive, 0.0f, 1.0f, d.distortionDrive);
        F(fx.sidechainAmount, 0.0f, 1.0f, d.sidechainAmount);
        F(fx.sidechainRelease, 0.02f, 1.0f, d.sidechainRelease);
        I(fx.master.chainPreset, -1, kNumMasterPresets - 1);
        I(fx.master.compPreset, -1, kNumCompPresets - 1);
    }
};

// Filled by LoadProjectOnce for LoadProject: values the sanity pass had to change, and the format of a file from a newer version.
int g_onceFixed = 0;
int g_onceNewerFormat = 0;

// One attempt at reading `path`. Returns false (with `problem` set) for an
// unreadable, malformed or incomplete file -- never throws.
bool LoadProjectOnce(Project& project, const std::string& path, std::string& problem) {
    g_onceFixed = 0;
    g_onceNewerFormat = 0;
    std::ifstream f(path);
    if (!f) {
        problem = "cannot open";
        return false;
    }

    // Reset to defaults first so a load never leaves stale state around.
    project = Project{};

    std::string line;
    std::string section;
    int curPattern = -1, curTrack = -1, curSynth = -1, curChordScene = -1;
    [[maybe_unused]] bool curChordPatch = false; // Phase 6 (tracked for symmetry, never read): "[chordpatch]" section
    SynthPatch* curPatchPtr = nullptr;   // v44.5.4: the patch a [synthN] / [chordpatch] / [patternP.synthN] / [patternP.chordpatch] section fills
    std::array<bool, kNumSlotsTotal> ownSound{}, ownKick{};   // scenes whose file carries their own sound (older files do not)

    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return std::string();
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    };

    // The fields after "stepN=" / "eorigN=" (see WriteStepFields); fields missing in older files keep their defaults.
    auto parseStepFields = [](Step& s, const std::string& val) {
        std::istringstream ss(val);
        std::string tok;
        // Defaults here matter: a save from before "ratchet"
        // existed only has 6 fields, so fields[6] never gets
        // overwritten by the loop below and stays at its
        // default of 1 (no ratchet) -- same backward-compat
        // trick already used for every other field here. Phase
        // 6 adds fields[7..11] (chordSize + 4 chordNotes) the
        // same way -- any save from before Chord tracks
        // existed (or any non-Chord track's own step line,
        // which never writes these -- see WriteTrack above)
        // just leaves chordSize at its default of 0, meaning
        // "not a chord step" regardless of what `active` says.
        // fields[12] (probability) is the newest addition --
        // same trick again, defaults to 100 (always fires).
        // fields[13] (nudge) and fields[14] (ghost) are v39.
        int fields[18] = {0, 0, 0, 60, 1, 0, 1, 0, 60, 64, 67, 71, 100, 0, 0, 0, 0, 0};
        int fi = 0;
        while (std::getline(ss, tok, ',') && fi < 18) { fields[fi++] = std::atoi(tok.c_str()); }
        s.active = fields[0] != 0;
        s.accent = fields[1] != 0;
        s.variant = static_cast<uint8_t>(fields[2]);
        s.note = static_cast<int8_t>(fields[3]);
        s.lengthSteps = static_cast<uint8_t>(std::max(1, fields[4]));
        s.slide = fields[5] != 0;
        s.ratchet = static_cast<uint8_t>(std::clamp(fields[6], 1, 4));
        s.chordSize = static_cast<uint8_t>(std::clamp(fields[7], 0, kMaxChordVoices));
        for (int n = 0; n < 4; ++n) s.chordNotes[n] = static_cast<int8_t>(fields[8 + n]);
        s.probability = static_cast<uint8_t>(std::clamp(fields[12], 0, 100));
        s.nudge = static_cast<int8_t>(std::clamp(fields[13], -50, 50));
        s.ghost = fields[14] != 0 && !s.accent;
        s.vel = static_cast<uint8_t>(std::clamp(fields[15], 0, 127));   // v40
        s.chordNotes[4] = static_cast<int8_t>(fields[16]); s.chordNotes[5] = static_cast<int8_t>(fields[17]);   // v44.43
    };

    auto parseTrackKV = [&](Track& t, const std::string& key, const std::string& val) {
        if (key == "type") {
            t.defaultVariant = 0;   // older files have no defaultVariant key: sample 1, as they always played
            if (val == "Drum") t.type = TrackType::Drum;
            else if (val == "Chord") t.type = TrackType::Chord;
            else if (val == "Sampler") t.type = TrackType::Sampler;
            else t.type = TrackType::Synth;
        }
        else if (key == "slotIndex") t.slotIndex = std::atoi(val.c_str());
        else if (key == "name") t.name = val;
        else if (key == "length") t.length = std::atoi(val.c_str());
        else if (key == "useOwnLength") t.useOwnLength = std::atoi(val.c_str()) != 0;
        else if (key == "trackKit") t.kitFolder = val;
        else if (key == "defaultVariant") t.defaultVariant = static_cast<uint8_t>(std::clamp(std::atoi(val.c_str()), 0, 15));
        else if (key == "grooveOn") t.grooveOn = std::atoi(val.c_str()) != 0;
        else if (key == "trackSwing") t.swing = std::stof(val) < 0.0f ? -1.0f : std::clamp(std::stof(val), 0.0f, 0.75f);
        else if (key == "volume") t.volume = std::stof(val);
        else if (key == "pan") t.pan = std::stof(val);
        else if (key == "mute") t.mute = std::atoi(val.c_str()) != 0;
        else if (key == "solo") t.solo = std::atoi(val.c_str()) != 0;
        else if (key == "drumTune") t.drumTune = std::stof(val);
        else if (key == "drumDecay") t.drumDecay = std::stof(val);
        else if (key == "sedit") {   // v44.52
            float st = 0, ln = 1, at = 0; int fc = 0, gd = 0, rv = 0;
            if (std::sscanf(val.c_str(), "%f|%f|%f|%d|%d|%d", &st, &ln, &at, &fc, &gd, &rv) == 6) {
                t.edit.start = std::clamp(st, 0.0f, 0.95f); t.edit.length = std::clamp(ln, 0.02f, 1.0f); t.edit.attackMs = std::clamp(at, 0.0f, 200.0f);
                t.edit.fineCents = static_cast<int8_t>(std::clamp(fc, -100, 100)); t.edit.gainDb = static_cast<int8_t>(std::clamp(gd, -24, 12)); t.edit.reverse = rv != 0;
            }
        }
        else if (key == "delaySend") t.delaySend = std::stof(val);
        else if (key == "reverbSend") t.reverbSend = std::stof(val);
        else if (key == "insertDrive") t.insertDrive = std::stof(val);
        else if (key == "insertEqTilt") t.insertEqTilt = std::stof(val);
        else if (key == "insertChorus") t.insertChorus = std::stof(val);
        else if (key == "autopanRate") t.autopanRate = std::stof(val);
        else if (key == "autopanDepth") t.autopanDepth = std::stof(val);
        else if (key == "width") t.width = std::clamp(std::stof(val), 0.0f, 2.0f);
        else if (key == "spread") t.spread = std::clamp(std::stof(val), 0.0f, 1.0f);
        else if (key == "eqHpfHz") t.eqHpfHz = std::clamp(std::stof(val), 20.0f, 500.0f);
        else if (key == "eqHpfSlope") t.eqHpfSlope = std::stof(val) >= 0.5f ? 1.0f : 0.0f;
        else if (key == "eqLowDb") t.eqLowDb = std::clamp(std::stof(val), kEqMinDb, kEqMaxDb);
        else if (key == "eqLowHz") t.eqLowHz = std::clamp(std::stof(val), 30.0f, 500.0f);
        else if (key == "eqMidDb") t.eqMidDb = std::clamp(std::stof(val), kEqMinDb, kEqMaxDb);
        else if (key == "eqMidHz") t.eqMidHz = std::clamp(std::stof(val), 80.0f, 10000.0f);
        else if (key == "eqMidQ") t.eqMidQ = std::clamp(std::stof(val), 0.3f, 6.0f);
        else if (key == "eqHighDb") t.eqHighDb = std::clamp(std::stof(val), kEqMinDb, kEqMaxDb);
        else if (key == "eqHighHz") t.eqHighHz = std::clamp(std::stof(val), 1500.0f, 16000.0f);
        else if (key.rfind("step", 0) == 0) {
            int idx = std::atoi(key.substr(4).c_str());
            if (idx >= 0 && idx < kMaxSteps) parseStepFields(t.steps[idx], val);
        }
        else if (key == "euclid") {   // v44.55
            int h = 3, st = 8, ro = 0;
            if (std::sscanf(val.c_str(), "%d,%d,%d", &h, &st, &ro) == 3) {
                t.euclidOn = true; t.euclidOrig.clear(); t.euclidSteps = static_cast<uint8_t>(std::clamp(st, 1, kMaxSteps)); t.euclidHits = static_cast<uint8_t>(std::clamp(h, 0, static_cast<int>(t.euclidSteps)));
                t.euclidRotate = static_cast<int8_t>(std::clamp(ro, -(kMaxSteps - 1), kMaxSteps - 1));
            }
        }
        else if (key.rfind("eorig", 0) == 0) {
            int idx = std::atoi(key.substr(5).c_str());
            if (idx >= 0 && idx < kMaxSteps) { Step st; parseStepFields(st, val); t.euclidOrig.emplace_back(static_cast<uint8_t>(idx), st); }
        }
    };

    int fileFormat = 1;       // files from before the format key are format 1
    bool sawEnd = false;
    try {
    while (std::getline(f, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty()) continue;
        if (trimmed.front() == '[') {
            section = trimmed.substr(1, trimmed.size() - 2);
            if (section == "end") sawEnd = true;
            curPattern = curTrack = curSynth = curChordScene = -1;
            curChordPatch = false;
            curPatchPtr = nullptr;
            if (section.rfind("pattern", 0) == 0 && section.find('.') != std::string::npos) {
                size_t dot = section.find('.');
                curPattern = std::atoi(section.substr(7, dot - 7).c_str());
                std::string trackPart = section.substr(dot + 1); // "trackN", "synthN" or "chordpatch"
                if (trackPart.rfind("synth", 0) == 0 || trackPart == "chordpatch") {
                    if (curPattern >= 0 && curPattern < kNumSlotsTotal) {
                        Pattern& pp = project.patterns[static_cast<size_t>(curPattern)];
                        if (trackPart == "chordpatch") { curPatchPtr = &pp.chordPatch; ownSound[static_cast<size_t>(curPattern)] = true; }
                        else {
                            const int si = std::atoi(trackPart.substr(5).c_str());
                            if (si >= 0 && si < kNumSynthTracks) { curPatchPtr = &pp.synthPatches[static_cast<size_t>(si)]; ownSound[static_cast<size_t>(curPattern)] = true; }
                        }
                    }
                } else {
                    curTrack = std::atoi(trackPart.substr(5).c_str());
                }
            } else if (section.rfind("pattern", 0) == 0) {
                curPattern = std::atoi(section.substr(7).c_str());
            } else if (section.rfind("chordscene", 0) == 0) {
                curChordScene = std::atoi(section.substr(10).c_str());
            } else if (section == "chordpatch") {
                curChordPatch = true;
                curPatchPtr = &project.baseChordPatch;
            } else if (section.rfind("synth", 0) == 0) {
                curSynth = std::atoi(section.substr(5).c_str());
                if (curSynth >= 0 && curSynth < kNumSynthTracks) curPatchPtr = &project.baseSynthPatches[static_cast<size_t>(curSynth)];
            }
            continue;
        }
        size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(trimmed.substr(0, eq));
        std::string val = trim(trimmed.substr(eq + 1));

        if (section == "project") {
            if (key == "format") fileFormat = std::atoi(val.c_str());
            else if (key == "name") project.name = val;
            else if (key == "kitFolder") project.kitFolder = val;
            else if (key == "currentPattern") project.currentPattern = std::atoi(val.c_str());
            else if (key == "trackSound") { project.trackSoundMask = std::clamp(std::atoi(val.c_str()), 0, 255); }
            else if (key == "sceneLink") {   // v44.58
                size_t pos = 0; int i = 0;
                while (i < kNumLiveScenes && pos <= val.size()) {
                    const size_t c = val.find(',', pos);
                    project.sceneLink[static_cast<size_t>(i++)] = static_cast<uint8_t>(std::clamp(std::atoi(val.substr(pos, c == std::string::npos ? std::string::npos : c - pos).c_str()), 0, 255));
                    if (c == std::string::npos) break;
                    pos = c + 1;
                }
            }
            else if (key == "chordsFollowKey") project.chordsFollowKey = std::atoi(val.c_str()) != 0;
            else if (key == "chordBankOn") project.chordBankOn = std::atoi(val.c_str()) != 0;
            else if (key == "chordSceneEdit") project.chordSceneEdit = std::clamp(std::atoi(val.c_str()), 0, kNumChordScenes - 1);
            else if (key == "randomizerKeyRoot") project.randomizerKeyRoot = std::atoi(val.c_str());
            else if (key == "randomizerScale") project.randomizerScale = std::atoi(val.c_str());
            else if (key == "rndOctaves") {
                int a = 0x06, b = 0x38, c = 0x38;
                if (std::sscanf(val.c_str(), "%d,%d,%d", &a, &b, &c) == 3) {
                    project.rndOctaves = {static_cast<uint8_t>(a & 0xFF), static_cast<uint8_t>(b & 0xFF), static_cast<uint8_t>(c & 0xFF)};
                    for (auto& m : project.rndOctaves) if (m == 0) m = 0x08;   // never an empty set
                }
            }
            else if (key.size() == 10 && key.compare(0, 9, "rndRhythm") == 0 && key[9] >= '0' && key[9] <= '2') {
                int a = 1, b = 3, c = 0, d = 2, e = 1, g = 1;
                if (std::sscanf(val.c_str(), "%d,%d,%d,%d,%d,%d", &a, &b, &c, &d, &e, &g) == 6) {
                    RhythmRnd r; r.minNotes = static_cast<uint8_t>(std::clamp(a, 1, kRhythmMaxNotes)); r.maxNotes = static_cast<uint8_t>(std::clamp(b, 1, kRhythmMaxNotes));
                    r.pitch = static_cast<RhythmPitch>(std::clamp(c, 0, kRhythmPitchCount - 1)); r.length = static_cast<RhythmLen>(std::clamp(d, 0, kRhythmLenCount - 1));
                    r.onTheOne = e != 0; r.sameEveryBar = g != 0;
                    project.rndRhythm[static_cast<size_t>(key[9] - '0')] = NormalizeRhythm(r);
                }
            }
            else if (key == "masterVolume") project.masterVolume = std::stof(val);
        } else if (section == "fx") {
            FxSettings& fx = project.fx;
            if (key == "delayTimeSec") fx.delayTimeSec = std::stof(val);
            else if (key == "delayDivisionIndex") fx.delayDivisionIndex = std::atoi(val.c_str());
            else if (key == "delayFeedback") fx.delayFeedback = std::stof(val);
            else if (key == "delayLevel") fx.delayLevel = std::stof(val);
            else if (key == "delayTone") fx.delayTone = std::clamp(std::stof(val), 0.0f, 1.0f);
            else if (key == "delayPingPong") fx.delayPingPong = std::atoi(val.c_str()) != 0;
            else if (key == "reverbSize") fx.reverbSize = std::stof(val);
            else if (key == "reverbDamp") fx.reverbDamp = std::clamp(std::stof(val), 0.0f, 1.0f);
            else if (key == "reverbPreMs") fx.reverbPreMs = std::clamp(std::stof(val), 0.0f, 120.0f);
            else if (key == "reverbLevel") fx.reverbLevel = std::stof(val);
            else if (key == "distortionDrive") fx.distortionDrive = std::stof(val);
            else if (key == "kickMode") fx.kick.mode = std::clamp(static_cast<int>(std::stof(val)), 0, 2);
            else if (key == "kickHz") fx.kick.pitchHz = std::clamp(std::stof(val), 30.0f, 100.0f);
            else if (key == "kickKeyLock") fx.kick.keyLock = std::stof(val) != 0.0f;
            else if (key == "kickSweep") fx.kick.sweep = std::clamp(std::stof(val), 1.0f, 10.0f);
            else if (key == "kickSweepMs") fx.kick.sweepMs = std::clamp(std::stof(val), 3.0f, 200.0f);
            else if (key == "kickDecayMs") fx.kick.decayMs = std::clamp(std::stof(val), 60.0f, 1500.0f);
            else if (key == "kickClick") fx.kick.click = std::clamp(std::stof(val), 0.0f, 1.0f);
            else if (key == "kickDrive") fx.kick.drive = std::clamp(std::stof(val), 0.0f, 1.0f);
            else if (key == "kickLevel") fx.kick.level = std::clamp(std::stof(val), 0.0f, 1.0f);
            else if (key == "sidechainEnabled") fx.sidechainEnabled = std::atoi(val.c_str()) != 0;
            else if (key == "sidechainAmount") fx.sidechainAmount = std::stof(val);
            else if (key == "sidechainRelease") fx.sidechainRelease = std::stof(val);
            else if (key.rfind("mc", 0) == 0) {
                MasterChainSettings& m = fx.master;
                float v = std::stof(val);
                if (key == "mcChainOn") m.chainOn = v != 0.0f;
                else if (key == "mcChainPreset") m.chainPreset = static_cast<int>(v);
                else if (key == "mcHpfHz") m.hpfHz = std::clamp(v, 20.0f, 400.0f);
                else if (key == "mcLowDb") m.lowDb = std::clamp(v, -9.0f, 9.0f);
                else if (key == "mcMidDb") m.midDb = std::clamp(v, -9.0f, 9.0f);
                else if (key == "mcHighDb") m.highDb = std::clamp(v, -9.0f, 9.0f);
                else if (key == "mcWidth") m.width = std::clamp(v, 0.0f, 2.0f);
                else if (key == "mcSaturation") m.saturation = std::clamp(v, 0.0f, 1.0f);
                else if (key == "mcLimiterOn") m.limiterOn = v != 0.0f;
                else if (key == "mcCeilingDb") m.ceilingDb = std::clamp(v, -6.0f, 0.0f);
                else if (key == "mcMonoBassHz") m.monoBassHz = std::clamp(v, 0.0f, 300.0f);
                else if (key == "mcCompScHpfHz") m.compScHpfHz = std::clamp(v, 20.0f, 300.0f);
                else if (key == "mcCompOn") m.compOn = v != 0.0f;
                else if (key == "mcCompPreset") m.compPreset = static_cast<int>(v);
                else if (key == "mcCompThreshDb") m.compThreshDb = std::clamp(v, -40.0f, 0.0f);
                else if (key == "mcCompRatio") m.compRatio = std::clamp(v, 1.0f, 10.0f);
                else if (key == "mcCompAttackMs") m.compAttackMs = std::clamp(v, 1.0f, 100.0f);
                else if (key == "mcCompReleaseMs") m.compReleaseMs = std::clamp(v, 20.0f, 800.0f);
                else if (key == "mcCompMakeupDb") m.compMakeupDb = std::clamp(v, 0.0f, 12.0f);
                else if (key == "mcCompMix") m.compMix = std::clamp(v, 0.0f, 1.0f);
            }
        } else if (section == "songchain") {
            if (key == "length") {
                project.songChainLength = std::clamp(std::atoi(val.c_str()), 0, kMaxChainSteps);
            } else if (key.rfind("step", 0) == 0) {
                int idx = std::atoi(key.substr(4).c_str());
                if (idx >= 0 && idx < kMaxChainSteps) {
                    std::istringstream ss(val);
                    std::string tok;
                    ChainStep cs;
                    if (std::getline(ss, tok, ',')) cs.patternIndex = std::clamp(std::atoi(tok.c_str()), 0, kNumPatterns - 1);
                    if (std::getline(ss, tok, ',')) cs.repeats = std::clamp(std::atoi(tok.c_str()), 1, 99);
                    if (std::getline(ss, tok, ',')) cs.chordScene = std::clamp(std::atoi(tok.c_str()), -1, kNumChordScenes - 1);
                    project.songChain[idx] = cs;
                }
            } else if (key.rfind("lvl", 0) == 0) {   // v44.13 levels of chain step N (start / end per instrument); newer files have lv2 instead
                const int idx = std::atoi(key.substr(3).c_str());
                if (idx >= 0 && idx < kMaxChainSteps) {
                    std::istringstream ss(val); std::string tok; std::vector<int> v;
                    while (std::getline(ss, tok, ',')) v.push_back(std::atoi(tok.c_str()));
                    StepLevels l;
                    if (v.size() >= 2) { l.transitionDb = ClampLevelDb(v[0]); l.fillDb = ClampLevelDb(v[1]); }
                    for (int t = 0; t < kArrTracks; ++t) {
                        const size_t k = 2 + static_cast<size_t>(t) * 2;
                        if (k + 1 < v.size()) l.track[t] = Lane::Line(v[k], v[k + 1], kAutoMinDb, kAutoMaxDb);
                    }
                    project.songChain[idx].levels = NormalizeLevels(l);
                }
            } else if (key.rfind("lv2", 0) == 0) {   // v44.21 levels with free points: "transition,fill;lane0;...;lane9"
                const int idx = std::atoi(key.substr(3).c_str());
                if (idx >= 0 && idx < kMaxChainSteps) {
                    std::istringstream ss(val); std::string part; StepLevels l; int k = 0;
                    while (std::getline(ss, part, ';') && k <= kArrTracks) {
                        if (k == 0) { int tr = 0, fl = 0; if (std::sscanf(part.c_str(), "%d,%d", &tr, &fl) == 2) { l.transitionDb = ClampLevelDb(tr); l.fillDb = ClampLevelDb(fl); } }
                        else l.track[k - 1] = LaneFromString(part, kAutoMinDb, kAutoMaxDb);
                        ++k;
                    }
                    project.songChain[idx].levels = NormalizeLevels(l);
                }
            } else if (key.rfind("prm", 0) == 0) {   // v44.38 parameter rows of chain step N
                const int idx = std::atoi(key.substr(3).c_str());
                if (idx >= 0 && idx < kMaxChainSteps) project.songChain[idx].params = ParamsFromString(val);
            } else if (key.rfind("dyn", 0) == 0) {   // v44.21 MIX lane and DJ filter lane of chain step N
                const int idx = std::atoi(key.substr(3).c_str());
                if (idx >= 0 && idx < kMaxChainSteps) {
                    const size_t semi = val.find(';');
                    StepAuto au;
                    au.mix = LaneFromString(val.substr(0, semi), kAutoMinDb, kAutoMaxDb);
                    if (semi != std::string::npos) au.filter = LaneFromString(val.substr(semi + 1), kFilterMin, kFilterMax);
                    project.songChain[idx].automation = NormalizeAuto(au);
                }
            } else if (key.rfind("fsh", 0) == 0) {   // v44.25 the own fill of chain step N is moved earlier by that many beats
                const int idx = std::atoi(key.substr(3).c_str());
                if (idx >= 0 && idx < kMaxChainSteps) project.songChain[idx].fill.shiftBeats = static_cast<uint8_t>(std::clamp(std::atoi(val.c_str()), 0, 64));
            } else if (key.rfind("arr", 0) == 0) {   // v44.5 overlays of chain step N (written right after its stepN line)
                const int idx = std::atoi(key.substr(3).c_str());
                int m = 0, fm = 0, fw = 0, fk = 0, fb = 2, fe = 4, fi = 0, rs = -1, rb = 2, tr = 0, a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0;
                const int nf = std::sscanf(val.c_str(), "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", &m, &fm, &fw, &fk, &fb, &fe, &fi, &rs, &rb, &tr, &a0, &a1, &a2, &a3, &a4, &a5);
                if (idx >= 0 && idx < kMaxChainSteps && (nf == 10 || nf == 16)) {   // 10 = before v44.6 (no dynamics)
                    if (nf == 16) {
                        // before v44.21 the filters were two (HPF, LPF); at each end the stronger one becomes the one DJ filter
                        StepAuto au = StepAuto::Ends(a0, a1, DjFilterFromHpfLpf(a2, a4), DjFilterFromHpfLpf(a3, a5));
                        project.songChain[idx].automation = NormalizeAuto(au);
                    }
                    ChainStep& cs = project.songChain[idx];
                    cs.muteMask = static_cast<uint16_t>(m & kMuteAllTracks);
                    cs.fillMode = static_cast<FillMode>(std::clamp(fm, 0, 2));
                    FillRule r; r.when = static_cast<FillWhen>(std::clamp(fw, 0, kNumFillWhen - 1)); r.kind = static_cast<FillKind>(std::clamp(fk, 0, kNumFillKinds - 1));
                    r.beats = static_cast<uint8_t>(std::clamp(fb, 1, 8)); r.everyBars = static_cast<uint8_t>(std::clamp(fe, 1, 16)); r.breakIndex = static_cast<uint8_t>(std::clamp(fi, 0, 255));
                    cs.fill = NormalizeFill(r);
                    cs.riser = static_cast<int8_t>(std::clamp(rs, -1, 127));
                    cs.riserBars = static_cast<uint8_t>(std::clamp(rb, 1, 16));
                    cs.trimSteps = static_cast<uint8_t>(std::clamp(tr, 0, kMaxSteps));
                }
            }
        } else if (curChordScene >= 0 && curChordScene < kNumChordScenes) {
            ChordScene& cs = project.chordScenes[curChordScene];
            if (key == "sceneName") cs.name = val;
            else if (key == "sceneKey") cs.key = static_cast<int8_t>(std::clamp(std::atoi(val.c_str()), -1, kNumKeys - 1));
            else if (key == "sceneScale") cs.scale = static_cast<int8_t>(std::clamp(std::atoi(val.c_str()), -1, kNumScales - 1));
            else { parseTrackKV(cs.track, key, val); cs.track.type = TrackType::Chord; cs.track.slotIndex = 0; }
        } else if (curPatchPtr) {
            // Phase 6: "[chordpatch]" and "[synthN]" are both just a
            // SynthPatch under a different section name -- same field
            // list/parsing either way (see WriteSynth's comment above).
            SynthPatch& sp = *curPatchPtr;
            if (key == "waveform") sp.waveform = WaveformFromName(val);
            else if (key == "cutoffHz") sp.cutoffHz = std::stof(val);
            else if (key == "resonance") sp.resonance = std::stof(val);
            else if (key == "envAmount") sp.envAmount = std::stof(val);
            else if (key == "attack") sp.attack = std::stof(val);
            else if (key == "decay") sp.decay = std::stof(val);
            else if (key == "sustain") sp.sustain = std::stof(val);
            else if (key == "release") sp.release = std::stof(val);
            else if (key == "filterDecay") sp.filterDecay = std::stof(val);
            else if (key == "glideTime") sp.glideTime = std::stof(val);
            else if (key == "volume") sp.volume = std::stof(val);
            else if (key == "lfoTarget") sp.lfoTarget = LfoTargetFromName(val);
            else if (key == "lfoRateHz") sp.lfoRateHz = std::stof(val);
            else if (key == "lfoDepth") sp.lfoDepth = std::stof(val);
            else if (key == "lfoShape") sp.lfoShape = WaveformFromName(val);
            // Phase 5 fields -- optional on read: any save from before FM
            // existed simply lacks these keys, and SynthPatch's own
            // in-struct defaults (Subtractive, algorithm 0, ratio 1.0,
            // level 0.5) already leave old patches behaving exactly as
            // they did before, since `project = Project{}` above resets
            // every patch to defaults before any keys are parsed.
            else if (key == "engineType") {
                if (val == "FM") sp.engineType = SynthEngineType::FM;
                else if (val == "WT") sp.engineType = SynthEngineType::Wavetable;
                else if (val == "SMP") sp.engineType = SynthEngineType::Sample;
                else sp.engineType = SynthEngineType::Subtractive;
            }
            else if (key == "fmAlgorithm") sp.fmAlgorithm = std::clamp(std::atoi(val.c_str()), 0, kNumFmAlgorithms - 1);
            else if (key.rfind("fmOp", 0) == 0 && key.size() > 4) {
                int opIdx = key[4] - '0';
                if (opIdx >= 0 && opIdx < 4) {
                    if (key.substr(5) == "Ratio") sp.fmOps[opIdx].ratio = std::clamp(std::stof(val), 0.25f, 16.0f);
                    else if (key.substr(5) == "Level") sp.fmOps[opIdx].level = std::clamp(std::stof(val), 0.0f, 1.0f);
                }
            }
            // Phase 5b fields -- same optional-on-read convention as the
            // FM keys above: missing on an older save leaves wavetableBank/
            // wtPosition at SynthPatch's own struct defaults (0 / 0.0).
            else if (key == "wavetableBank") sp.wavetableBank = std::clamp(std::atoi(val.c_str()), 0, kNumWavetableBanks - 1);
            else if (key == "fmIndexScale") sp.fmIndexScale = std::clamp(std::stof(val), 0.25f, 8.0f);
            else if (key == "fmModEnv") sp.fmModEnv = std::clamp(std::stof(val), 0.0f, 1.0f);
            else if (key == "wtPosition") sp.wtPosition = std::clamp(std::stof(val), 0.0f, 1.0f);
            else if (key == "unisonVoices") sp.unisonVoices = std::clamp(std::atoi(val.c_str()), 1, kMaxUnisonVoices);
            else if (key == "unisonDetune") sp.unisonDetune = std::clamp(std::stof(val), 0.0f, 1.0f);
            else if (key == "unisonSpread") sp.unisonSpread = std::clamp(std::stof(val), 0.0f, 1.0f);
            else if (key == "subLevel") sp.subLevel = std::clamp(std::stof(val), 0.0f, 1.0f);
            else if (key == "subOctave") sp.subOctave = std::clamp(std::atoi(val.c_str()), 1, 2);
            else if (key == "noiseLevel") sp.noiseLevel = std::clamp(std::stof(val), 0.0f, 1.0f);
            else if (key == "phaseSync") sp.phaseSync = std::atoi(val.c_str()) != 0;
            else if (key == "drive") sp.drive = std::clamp(std::stof(val), 0.0f, 1.0f);
            else if (key == "osc2Wave") sp.osc2Wave = WaveformFromName(val);
            else if (key == "osc2Level") sp.osc2Level = std::clamp(std::stof(val), 0.0f, 1.0f);
            else if (key == "osc2Semi") sp.osc2Semi = std::clamp(std::atoi(val.c_str()), -24, 24);
            else if (key == "osc2Detune") sp.osc2Detune = std::clamp(std::atoi(val.c_str()), -50, 50);
            else if (key == "filter24") sp.filter24 = std::atoi(val.c_str()) != 0;
            else if (key == "acid") sp.acid = std::atoi(val.c_str()) != 0;
            else if (key == "drift") sp.drift = std::clamp(std::stof(val), 0.0f, 1.0f);
            else if (key == "sample") FromSamplerPatch(sp.sample, SamplerFromString(val));   // v44.53
            else if (key == "keytrack") sp.keytrack = std::clamp(std::stof(val), 0.0f, 1.0f);
        } else if (curPattern >= 0 && curPattern < kNumSlotsTotal && curTrack < 0) {
            Pattern& pat = project.patterns[curPattern];
            if (key == "name") pat.name = val;
            else if (key == "masterLength") pat.masterLength = std::atoi(val.c_str());
            else if (key == "bpm") pat.bpm = std::stof(val);
            else if (key == "swing") pat.swing = std::stof(val);
            else if (key == "stepsPerBeat") pat.stepsPerBeat = ClampGrid(std::atoi(val.c_str()));
            else if (key == "kitFolder") pat.kitFolder = val;
            else if (key == "sampler") pat.samplerPatch = SamplerFromString(val);   // v44.50
            else if (key == "params") pat.params = ParamsFromString(val);
            else if (key == "ownSound") {
                uint16_t v = static_cast<uint16_t>(std::clamp(std::atoi(val.c_str()), 0, static_cast<int>(kAllPartsMask)));
                if (fileFormat < 7 && (v & 0x400u)) v = static_cast<uint16_t>((v & ~0x400u) | (1u << kPartKit));   // v44.50: bit 10 was the kit; it is the sampler now, the kit moved to bit 11
                pat.ownSound = v;
            }
            else if (key == "kickSynth") {
                int m = 0, kl = 1; float hz = 52, sw = 4, swm = 40, dm = 380, ck = 0.3f, dr = 0.25f, lv = 0.85f;
                if (std::sscanf(val.c_str(), "%d,%f,%d,%f,%f,%f,%f,%f,%f", &m, &hz, &kl, &sw, &swm, &dm, &ck, &dr, &lv) == 9) {
                    KickSynthSettings& k = pat.kick;
                    k.mode = std::clamp(m, 0, 2); k.pitchHz = std::clamp(hz, 30.0f, 100.0f); k.keyLock = kl != 0; k.sweep = std::clamp(sw, 1.0f, 10.0f);
                    k.sweepMs = std::clamp(swm, 3.0f, 200.0f); k.decayMs = std::clamp(dm, 60.0f, 1500.0f); k.click = std::clamp(ck, 0.0f, 1.0f);
                    k.drive = std::clamp(dr, 0.0f, 1.0f); k.level = std::clamp(lv, 0.0f, 1.0f);
                    ownKick[static_cast<size_t>(curPattern)] = true;
                }
            }
            else if (key == "role") pat.role = static_cast<SceneRole>(std::clamp(std::atoi(val.c_str()), 0, kNumRoles - 1));
            else if (key == "fill") {
                int a = 0, b = 0, c = 2, d = 4, e = 0;
                if (std::sscanf(val.c_str(), "%d,%d,%d,%d,%d", &a, &b, &c, &d, &e) == 5) {
                    FillRule r; r.when = static_cast<FillWhen>(std::clamp(a, 0, kNumFillWhen - 1)); r.kind = static_cast<FillKind>(std::clamp(b, 0, kNumFillKinds - 1));
                    r.beats = static_cast<uint8_t>(std::clamp(c, 1, 8)); r.everyBars = static_cast<uint8_t>(std::clamp(d, 1, 16)); r.breakIndex = static_cast<uint8_t>(std::clamp(e, 0, 255));
                    pat.fill = NormalizeFill(r);
                }
            }
            else if (key == "grooveName") pat.groove.name = val;
            else if (key == "grooveAmount") pat.grooveAmount = std::clamp(std::stof(val), 0.0f, 1.0f);
            else if (key == "groove") {
                std::istringstream gs(val); std::string tok; int gi = 0;
                while (gi < 16 && std::getline(gs, tok, ',')) {
                    const size_t c = tok.find(':');
                    if (c != std::string::npos) {
                        pat.groove.steps[gi].nudge = static_cast<int8_t>(std::clamp(std::atoi(tok.c_str()), -50, 50));
                        pat.groove.steps[gi].vel = static_cast<int8_t>(std::clamp(std::atoi(tok.c_str() + c + 1), -127, 127));
                    }
                    ++gi;
                }
            }
            else if (key == "chordsKey") pat.chordsKey = static_cast<int8_t>(std::clamp(std::atoi(val.c_str()), -1, kNumKeys - 1));
            else if (key == "chordsScale") pat.chordsScale = static_cast<int8_t>(std::clamp(std::atoi(val.c_str()), -1, kNumScales - 1));
        } else if (curPattern >= 0 && curPattern < kNumSlotsTotal &&
                   curTrack >= 0 && curTrack < kNumTracks) {
            Track& t = project.patterns[curPattern].tracks[curTrack];
            parseTrackKV(t, key, val);
        }
    }
    } catch (const std::exception& e) {
        problem = std::string("corrupt value (") + e.what() + ")";
        return false;
    }
    if (fileFormat >= 2 && !sawEnd) { problem = "file is incomplete (no end marker)"; return false; }
    // v44.5.4: a file from before the sound belonged to the scene: every scene starts from the one project-wide sound
    for (int pi = 0; pi < kNumSlotsTotal; ++pi) {
        Pattern& pp = project.patterns[static_cast<size_t>(pi)];
        if (!ownSound[static_cast<size_t>(pi)]) { pp.synthPatches = project.baseSynthPatches; pp.chordPatch = project.baseChordPatch; }
        if (!ownKick[static_cast<size_t>(pi)]) pp.kick = project.fx.kick;
    }
    // v44.22 (S1a): a file from before the bricks had their own notes: every brick pointed at a LIVE scene. Each distinct scene used by
    // the chain gets one hidden brick slot (a copy), so the bricks keep playing exactly what they played; bricks that shared a
    // scene still share (REPEAT SAME). The live scenes stay as they were.
    // A brick of the track must have a scene slot of its own (v44.22). Files from before that (format < 5) and files whose first
    // brick was the v44.9 starter brick (DATA-7: it pointed at LIVE scene 1 itself, so editing the track changed the jam scene)
    // have chain steps that point at a LIVE scene: each such scene is copied into a free brick slot and the steps point at the
    // copy. The copy holds the same notes and sound, so the file plays exactly as before; the LIVE scene is no longer shared.
    {
        std::array<bool, kNumPatterns> used{};
        for (int i = 0; i < project.songChainLength; ++i) {
            const int pi = project.songChain[static_cast<size_t>(i)].patternIndex;
            if (pi >= kNumLiveScenes && pi < kNumPatterns) used[static_cast<size_t>(pi)] = true;
        }
        std::array<int, kNumLiveScenes> slotOf{};
        slotOf.fill(-1);
        for (int i = 0; i < project.songChainLength; ++i) {
            ChainStep& cs = project.songChain[static_cast<size_t>(i)];
            const int pi = cs.patternIndex;
            if (pi < 0 || pi >= kNumLiveScenes) continue;
            if (slotOf[static_cast<size_t>(pi)] < 0) {
                int free = -1;
                for (int sl = kNumLiveScenes; sl < kNumPatterns; ++sl) if (!used[static_cast<size_t>(sl)]) { free = sl; break; }
                if (free < 0) continue;   // cannot happen: there is a brick slot for every chain step
                used[static_cast<size_t>(free)] = true;
                slotOf[static_cast<size_t>(pi)] = free;
                Pattern& brick = project.patterns[static_cast<size_t>(free)];
                brick = project.patterns[static_cast<size_t>(pi)];
                if (fileFormat >= 6) {   // the brick's sound follows the track where it sounds the same (as a thrown brick does)
                    Pattern& ts = project.patterns[static_cast<size_t>(kTrackSoundSlot)];
                    brick.ownSound = 0;
                    if (project.trackSoundMask == 0) { CopyAllParts(ts, brick); project.trackSoundMask = 255; }
                    else for (int part = 0; part < kNumParts; ++part) if (SoundPartText(brick, part) != SoundPartText(ts, part)) brick.ownSound |= static_cast<uint16_t>(1u << part);
                }
            }
            const Pattern& brick = project.patterns[static_cast<size_t>(slotOf[static_cast<size_t>(pi)])];
            if (fileFormat >= 5 && !cs.params.Any() && brick.params.Any()) cs.params = brick.params;   // the scene's rows go with it, as when thrown
            cs.patternIndex = slotOf[static_cast<size_t>(pi)];
        }
        if (fileFormat >= 5) for (int sl : slotOf) if (sl >= 0) project.patterns[static_cast<size_t>(sl)].params = StepParams{};
    }
    // v44.24: a file from before bricks could have their own sound: the first brick's sound becomes the sound of the track (the
    // v44.23 track sound held only the voices, not the channels), and every other brick that sounds different in a part keeps that
    // part as its own, so nothing changes in what the file plays.
    if (fileFormat < 6 && project.songChainLength > 0) {
        const int firstSlot = std::clamp(project.songChain[0].patternIndex, 0, kNumPatterns - 1);
        Pattern& ts = project.patterns[static_cast<size_t>(kTrackSoundSlot)];
        CopyAllParts(ts, project.patterns[static_cast<size_t>(firstSlot)]);
        project.trackSoundMask = 127;
        std::array<bool, kNumPatterns> done{};
        for (int i = 0; i < project.songChainLength; ++i) {
            const int sl = std::clamp(project.songChain[static_cast<size_t>(i)].patternIndex, 0, kNumPatterns - 1);
            if (done[static_cast<size_t>(sl)]) continue;
            done[static_cast<size_t>(sl)] = true;
            Pattern& bp = project.patterns[static_cast<size_t>(sl)];
            bp.ownSound = 0;
            for (int part = 0; part < kNumParts; ++part) if (SoundPartText(bp, part) != SoundPartText(ts, part)) bp.ownSound |= static_cast<uint16_t>(1u << part);
        }
    }
    // v44.38: a brick has ONE mute, its muteMask; the mute / solo flags a brick's scene still carries (from a file written before) are
    // folded into every brick that uses the scene and cleared, so the file plays as it did.
    {
        std::array<bool, kNumPatterns> folded{};
        for (int i = 0; i < project.songChainLength; ++i) {
            ChainStep& cs = project.songChain[static_cast<size_t>(i)];
            const int sl = cs.patternIndex;
            if (sl < kNumLiveScenes || sl >= kNumPatterns) continue;
            cs.muteMask = static_cast<uint16_t>((cs.muteMask | SceneMuteMask(project.patterns[static_cast<size_t>(sl)])) & kMuteAllTracks);
            folded[static_cast<size_t>(sl)] = true;
        }
        for (int sl = kNumLiveScenes; sl < kNumPatterns; ++sl) if (folded[static_cast<size_t>(sl)]) ClearSceneMuteFlags(project.patterns[static_cast<size_t>(sl)]);
    }
    project.currentPattern = std::clamp(project.currentPattern, 0, kNumLiveScenes - 1);
    project.liveScene = project.currentPattern;
    Sanitizer san;
    san.ProjectAll(project);
    g_onceFixed = san.fixed;
    if (san.fixed > 0) std::cerr << "[persist] " << path << ": " << san.fixed << " damaged values put back into range\n";
    g_onceNewerFormat = fileFormat > kProjectFormatVersion ? fileFormat : 0;
    if (g_onceNewerFormat) {
        std::cerr << "[persist] " << path << " was written by a newer version (format " << fileFormat
                  << "); loading what is understood\n";
    }
    return true;
}
} // namespace

namespace {
std::string g_lastLoadNote;
int g_lastNewerFormat = 0, g_lastFixed = 0;
// What the user must hear about a file that did load (newer version, repaired values); empty when nothing.
std::string LoadWarnings() {
    std::string note;
    if (g_lastNewerFormat) note = "THIS FILE IS FROM A NEWER TR-BRICK: CMD+S SAVES A COPY, THE FILE STAYS AS IT IS";
    if (g_lastFixed) note += (note.empty() ? "" : " | ") + std::string("REPAIRED ") + std::to_string(g_lastFixed) + " DAMAGED VALUES IN THE FILE";
    return note;
}
} // namespace
const std::string& LastLoadNote() { return g_lastLoadNote; }
int LastLoadNewerFormat() { return g_lastNewerFormat; }
int LastLoadFixedValues() { return g_lastFixed; }

bool LoadProject(Project& project, const std::string& path) {
    g_lastLoadNote.clear();
    g_lastNewerFormat = g_lastFixed = 0;
    std::string problem;
    if (LoadProjectOnce(project, path, problem)) {
        g_lastNewerFormat = g_onceNewerFormat; g_lastFixed = g_onceFixed;
        g_lastLoadNote = LoadWarnings();
        BackupProjectFile(path);   // keep a dated copy of whatever we just opened
        return true;
    }
    std::cerr << "[persist] " << path << ": " << problem << "\n";
    // Fall back to the previous good save, if there is one.
    std::string bak = path + ".bak";
    std::string problem2;
    if (LoadProjectOnce(project, bak, problem2)) {
        g_lastNewerFormat = g_onceNewerFormat; g_lastFixed = g_onceFixed;
        g_lastLoadNote = "RECOVERED FROM BACKUP (" + problem + ")";
        if (!LoadWarnings().empty()) g_lastLoadNote += " | " + LoadWarnings();
        // Keep the damaged file for inspection and put the good copy in its place.
        std::rename(path.c_str(), (path + ".corrupt").c_str());
        {
            std::string good;
            std::ifstream in(bak, std::ios::binary);
            if (in) { std::ostringstream os; os << in.rdbuf(); good = os.str(); }
            if (!in || !WriteFileAtomic(path, good)) std::cerr << "[persist] could not put " << bak << " back in place of " << path << "\n";
        }
        std::cerr << "[persist] recovered from " << bak << "\n";
        return true;
    }
    project = Project{};
    // Keep the unreadable file for inspection instead of letting the next
    // autosave overwrite it.
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) std::rename(path.c_str(), (path + ".corrupt").c_str());
    return false;
}

// Dated safety copies: <backups>/<name>~<folder>-YYYYMMDD-HHMMSS.t8proj. <folder> is a short code of the folder the project
// lives in, so two projects with the same name in different folders never push out each other's copies (DATA-4). Taken on
// every successful load and before the first save of a session, but only when the file differs from the newest copy (opening
// the same project ten times no longer leaves ten identical copies). Kept: the newest `keep`, plus the newest copy of each of
// the 14 days before them, so yesterday's version survives a busy day. Since v44.14 the folder is the program's own service
// folder (userdirs::BackupsDir()); in legacy mode it is "backups" next to the project file. Copies from before v44.59 have no
// <folder> part; they are still listed (never pruned) for the project with that name.
namespace {
std::string BackupDirFor(const std::string& projectPath) {
    const std::string svc = userdirs::BackupsDir();
    if (!svc.empty()) return svc;
    size_t slash = projectPath.find_last_of("/\\");
    return (slash == std::string::npos ? std::string() : projectPath.substr(0, slash + 1)) + "backups";
}
std::string StemOf(const std::string& projectPath) {
    std::string base = projectPath;
    size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    size_t dot = base.rfind('.');
    return dot == std::string::npos ? base : base.substr(0, dot);
}
// 8 hex digits naming the folder of `projectPath` (FNV-1a of its absolute path).
// The absolute path of a folder (as given when it cannot be resolved).
std::string RealPathOf(std::string dir) {
    if (dir.empty()) dir = "/";
    if (char* real = ::realpath(dir.c_str(), nullptr)) { dir = real; std::free(real); }
    return dir;
}
std::string FolderOf(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}
std::string FolderCodeOf(const std::string& projectPath) {
    const std::string dir = RealPathOf(FolderOf(projectPath));
    uint32_t h = 2166136261u;
    for (unsigned char c : dir) { h ^= c; h *= 16777619u; }
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", h);
    return buf;
}
bool IsStamp(const std::string& ts) {   // "YYYYMMDD-HHMMSS"
    if (ts.size() != 15) return false;
    for (size_t i = 0; i < ts.size(); ++i) {
        if (i == 8) { if (ts[i] != '-') return false; }
        else if (ts[i] < '0' || ts[i] > '9') return false;
    }
    return true;
}
// "<stem>~<code>-YYYYMMDD-HHMMSS.t8proj" exactly (so "a" does not claim the backups of "a-b")
bool IsBackupOf(const std::string& file, const std::string& stem, const std::string& code) {
    const std::string head = stem + "~" + code + "-";
    if (file.size() != head.size() + 15 + 7 || file.compare(0, head.size(), head) != 0) return false;
    if (file.compare(file.size() - 7, 7, ".t8proj") != 0) return false;
    return IsStamp(file.substr(head.size(), 15));
}
// A copy from before v44.59: "<stem>-YYYYMMDD-HHMMSS.t8proj".
bool IsLegacyBackupOf(const std::string& file, const std::string& stem) {
    if (file.size() != stem.size() + 1 + 15 + 7 || file.compare(0, stem.size() + 1, stem + "-") != 0) return false;
    if (file.compare(file.size() - 7, 7, ".t8proj") != 0) return false;
    return IsStamp(file.substr(stem.size() + 1, 15));
}
std::string StampOf(const std::string& file) { return file.size() >= 22 ? file.substr(file.size() - 22, 15) : std::string(); }
bool ReadWhole(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream os;
    os << in.rdbuf();
    out = os.str();
    return !in.bad();
}
} // namespace

// Writes `bytes` to <dest>.tmp, flushes it to the disk and renames it over `dest`: a reader sees the old file or the new one,
// never half of one.
bool WriteFileAtomic(const std::string& dest, const std::string& bytes) {
    const std::string tmp = dest + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) { out.close(); std::remove(tmp.c_str()); return false; }
    }
    FlushFileToDisk(tmp);
    if (std::rename(tmp.c_str(), dest.c_str()) != 0) { std::remove(tmp.c_str()); return false; }
    FlushDirOf(dest);
    return true;
}

bool BackupProjectFile(const std::string& path, int keep) {
    std::string bytes;
    if (!ReadWhole(path, bytes)) return false;
    const std::string stem = StemOf(path);
    const std::string code = FolderCodeOf(path);
    const std::string bdir = BackupDirFor(path);
    // A file opened from the backups folder itself (the Versions list, or by hand): no copy of the copy, they would only pile up.
    if (RealPathOf(FolderOf(path)) == RealPathOf(bdir)) return true;
    ::mkdir(bdir.c_str(), 0755);
    std::vector<std::string> mine;
    if (DIR* d = ::opendir(bdir.c_str())) {
        while (dirent* e = ::readdir(d)) {
            std::string n = e->d_name;
            if (IsBackupOf(n, stem, code)) mine.push_back(n);
        }
        ::closedir(d);
    }
    std::sort(mine.begin(), mine.end());   // by name = by time
    // Nothing new since the newest copy: no copy (it would only push out an older, different one).
    if (!mine.empty()) {
        std::string newest;
        if (ReadWhole(bdir + "/" + mine.back(), newest) && newest == bytes) return true;
    }
    std::time_t now = std::time(nullptr);
    std::string name;
    for (int tries = 0; tries < 120; ++tries, ++now) {   // two copies in one second: the second takes the next free second
        char stamp[32];
        std::tm tmv{};
        localtime_r(&now, &tmv);
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
        name = stem + "~" + code + "-" + stamp + ".t8proj";
        struct stat st;
        if (::stat((bdir + "/" + name).c_str(), &st) != 0) break;
    }
    if (!WriteFileAtomic(bdir + "/" + name, bytes)) return false;
    if (std::find(mine.begin(), mine.end(), name) == mine.end()) mine.push_back(name);
    std::sort(mine.begin(), mine.end());
    // prune: keep the newest `keep`; of the older ones keep the newest copy of each day, for up to 14 days
    const int n = static_cast<int>(mine.size());
    std::vector<std::string> days;
    for (int i = n - 1 - keep; i >= 0; --i) {
        const std::string day = mine[static_cast<size_t>(i)].substr(mine[static_cast<size_t>(i)].size() - 22, 8);
        const bool newDay = std::find(days.begin(), days.end(), day) == days.end();
        if (newDay && days.size() < 14) { days.push_back(day); continue; }
        std::remove((bdir + "/" + mine[static_cast<size_t>(i)]).c_str());
    }
    return true;
}

std::vector<std::string> ListProjectBackups(const std::string& path) {
    const std::string stem = StemOf(path);
    const std::string code = FolderCodeOf(path);
    const std::string bdir = BackupDirFor(path);
    std::vector<std::string> out;
    if (DIR* d = ::opendir(bdir.c_str())) {
        while (dirent* e = ::readdir(d)) {
            std::string n = e->d_name;
            if (IsBackupOf(n, stem, code) || IsLegacyBackupOf(n, stem)) out.push_back(bdir + "/" + n);
        }
        ::closedir(d);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return StampOf(a) > StampOf(b);   // newest first (timestamp suffix YYYYMMDD-HHMMSS.t8proj)
    });
    return out;
}

std::string BackupLabel(const std::string& backupPath) {
    const std::string ts = StampOf(backupPath);
    if (!IsStamp(ts)) return backupPath;
    return ts.substr(0, 4) + "-" + ts.substr(4, 2) + "-" + ts.substr(6, 2) + "  " + ts.substr(9, 2) + ":" + ts.substr(11, 2) + ":" + ts.substr(13, 2);
}

} // namespace t8
