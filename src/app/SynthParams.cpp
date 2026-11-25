#include "SynthParams.h"
#include "core/FMSynth.h"
#include "core/Wavetables.h"
#include <algorithm>
#include <cstdio>

namespace t8 {

namespace {
// Engine-specific row names/count. Subtractive keeps its original 5
// waveform/filter rows; FM gets ALGORITHM + 4 ratio/level operator pairs;
// Wavetable (Phase 5b) gets bank select + scan position, then reuses the
// same 4 filter rows Subtractive has (both engines share one post-
// oscillator filter block -- see WavetableSynth's class doc comment).
constexpr int kNumSubtractiveRows = 11;
const char* kSubtractiveRowNames[kNumSubtractiveRows] = {
    "WAVEFORM", "CUTOFF", "RESONANCE", "ENV AMT", "FILT DECAY",
    "OSC2 WAVE", "OSC2 SEMI", "OSC2 DETUNE", "OSC2 LEVEL", "FILTER", "KEYTRACK"      // v26
};
constexpr int kNumFmRows = 11;
const char* kFmRowNames[kNumFmRows] = {
    "ALGORITHM",
    "OP1 RATIO", "OP1 LEVEL", "OP2 RATIO", "OP2 LEVEL",
    "OP3 RATIO", "OP3 LEVEL", "OP4 RATIO", "OP4 LEVEL",
    "MOD ENV", "MOD RANGE"                                    // v44
};
constexpr int kNumWavetableRows = 6;
const char* kWavetableRowNames[kNumWavetableRows] = {
    "WAVETABLE", "POSITION", "CUTOFF", "RESONANCE", "ENV AMT", "FILT DECAY"
};

// v44.53: the SAMPLE engine has the filter rows of the subtractive engine (CUTOFF .. FILT DECAY, FILTER, KEYTRACK) and nothing else here: the
// sample itself (file, mode, root, start, length ...) is edited by the SAMPLE tab, not by rows.
constexpr int kNumSampleRows = 6;
const char* kSampleRowNames[kNumSampleRows] = { "CUTOFF", "RESONANCE", "ENV AMT", "FILT DECAY", "FILTER", "KEYTRACK" };
constexpr int kSampleToSubtractive[kNumSampleRows] = { 1, 2, 3, 4, 9, 10 };   // the same fields and steps as the subtractive rows

// Tail rows shared by all engines -- same 10 params/order as the
// original (pre-Phase-5) kPatchParamNames tail.
constexpr int kNumSharedRows = 19;
const char* kSharedRowNames[kNumSharedRows] = {
    "ATTACK", "DECAY", "SUSTAIN", "RELEASE", "GLIDE", "VOLUME",
    "LFO TARGET", "LFO RATE", "LFO DEPTH", "LFO SHAPE",
    "UNISON", "UNI DETUNE", "UNI SPREAD", "SUB LEVEL", "SUB OCT", "NOISE", "DRIVE",   // v26 (all engines)
    "PHASE SYNC",                                                                      // v30
    "DRIFT"                                                                            // v37
};

int EngineRowCount(SynthEngineType e) {
    switch (e) {
        case SynthEngineType::FM:        return kNumFmRows;
        case SynthEngineType::Wavetable: return kNumWavetableRows;
        case SynthEngineType::Sample:    return kNumSampleRows;
        case SynthEngineType::Subtractive: default: return kNumSubtractiveRows;
    }
}

void AdjustShared(SynthPatch& p, int tailIdx, int dir) {
    switch (tailIdx) {
        case 0: p.attack = std::clamp(p.attack + dir * 0.005f, 0.001f, 2.0f); break;
        case 1: p.decay = std::clamp(p.decay + dir * 0.02f, 0.01f, 2.0f); break;
        case 2: p.sustain = std::clamp(p.sustain + dir * 0.05f, 0.0f, 1.0f); break;
        case 3: p.release = std::clamp(p.release + dir * 0.02f, 0.01f, 2.0f); break;
        case 4: p.glideTime = std::clamp(p.glideTime + dir * 0.01f, 0.0f, 1.0f); break;
        case 5: p.volume = std::clamp(p.volume + dir * 0.05f, 0.0f, 2.0f); break;
        case 6: { int t = (static_cast<int>(p.lfoTarget) + dir + kNumLfoTargets) % kNumLfoTargets;
                   p.lfoTarget = static_cast<LfoTarget>(t); break; }
        case 7: p.lfoRateHz = std::clamp(p.lfoRateHz + dir * 0.1f, 0.05f, 10.0f); break;
        case 8: p.lfoDepth = std::clamp(p.lfoDepth + dir * 0.05f, 0.0f, 1.0f); break;
        case 9: { int w = (static_cast<int>(p.lfoShape) + dir + 4) % 4; p.lfoShape = static_cast<Waveform>(w); break; }
        case 10: p.unisonVoices = std::clamp(p.unisonVoices + dir, 1, kMaxUnisonVoices); break;
        case 11: p.unisonDetune = std::clamp(p.unisonDetune + dir * 0.05f, 0.0f, 1.0f); break;
        case 12: p.unisonSpread = std::clamp(p.unisonSpread + dir * 0.05f, 0.0f, 1.0f); break;
        case 13: p.subLevel = std::clamp(p.subLevel + dir * 0.05f, 0.0f, 1.0f); break;
        case 14: p.subOctave = p.subOctave >= 2 ? 1 : 2; break;
        case 15: p.noiseLevel = std::clamp(p.noiseLevel + dir * 0.05f, 0.0f, 1.0f); break;
        case 16: p.drive = std::clamp(p.drive + dir * 0.05f, 0.0f, 1.0f); break;
        case 17: p.phaseSync = !p.phaseSync; break;
        case 18: p.drift = std::clamp(p.drift + dir * 0.05f, 0.0f, 1.0f); break;
    }
}

std::string FormatShared(const SynthPatch& p, int tailIdx) {
    char buf[32];
    switch (tailIdx) {
        case 0: std::snprintf(buf, sizeof(buf), "%dMS", static_cast<int>(p.attack * 1000)); return buf;
        case 1: std::snprintf(buf, sizeof(buf), "%dMS", static_cast<int>(p.decay * 1000)); return buf;
        case 2: std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(p.sustain * 100)); return buf;
        case 3: std::snprintf(buf, sizeof(buf), "%dMS", static_cast<int>(p.release * 1000)); return buf;
        case 4: std::snprintf(buf, sizeof(buf), "%dMS", static_cast<int>(p.glideTime * 1000)); return buf;
        case 5: std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(p.volume * 100)); return buf;
        case 6: return LfoTargetName(p.lfoTarget);
        case 7: std::snprintf(buf, sizeof(buf), "%.1fHZ", p.lfoRateHz); return buf;
        case 8: std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(p.lfoDepth * 100)); return buf;
        case 9: return WaveformName(p.lfoShape);
        case 10: std::snprintf(buf, sizeof(buf), "%dx", p.unisonVoices); return buf;
        case 11: std::snprintf(buf, sizeof(buf), "%dC", static_cast<int>(p.unisonDetune * 40.0f + 0.5f)); return buf;
        case 12: std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(p.unisonSpread * 100)); return buf;
        case 13: std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(p.subLevel * 100)); return buf;
        case 14: return p.subOctave >= 2 ? "-2 OCT" : "-1 OCT";
        case 15: std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(p.noiseLevel * 100)); return buf;
        case 16: std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(p.drive * 100)); return buf;
        case 17: return p.phaseSync ? "ON" : "OFF";
        case 18: std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(p.drift * 100 + 0.5f)); return buf;
    }
    return "?";
}

void AdjustSubtractive(SynthPatch& p, int localIdx, int dir) {
    switch (localIdx) {
        case 0: { int w = (static_cast<int>(p.waveform) + dir + 4) % 4; p.waveform = static_cast<Waveform>(w); break; }
        case 1: p.cutoffHz = std::clamp(p.cutoffHz + dir * 100.0f, 100.0f, 18000.0f); break;
        case 2: p.resonance = std::clamp(p.resonance + dir * 0.02f, 0.0f, 0.99f); break;
        case 3: p.envAmount = std::clamp(p.envAmount + dir * 0.05f, 0.0f, 1.0f); break;
        case 4: p.filterDecay = std::clamp(p.filterDecay + dir * 0.02f, 0.01f, 2.0f); break;
        case 5: { int w = (static_cast<int>(p.osc2Wave) + dir + 4) % 4; p.osc2Wave = static_cast<Waveform>(w); break; }
        case 6: p.osc2Semi = std::clamp(p.osc2Semi + dir, -24, 24); break;
        case 7: p.osc2Detune = std::clamp(p.osc2Detune + dir, -50, 50); break;
        case 8: p.osc2Level = std::clamp(p.osc2Level + dir * 0.05f, 0.0f, 1.0f); break;
        case 9: {   // 12 dB -> 24 dB -> ACID -> 12 dB (dir < 0 goes back)
            int m = p.acid ? 2 : (p.filter24 ? 1 : 0);
            m = (m + (dir >= 0 ? 1 : 2)) % 3;
            p.acid = m == 2; p.filter24 = m == 1; break;
        }
        case 10: p.keytrack = std::clamp(p.keytrack + dir * 0.05f, 0.0f, 1.0f); break;
    }
}

std::string FormatSubtractive(const SynthPatch& p, int localIdx) {
    char buf[32];
    switch (localIdx) {
        case 0: return WaveformName(p.waveform);
        case 1: std::snprintf(buf, sizeof(buf), "%dHZ", static_cast<int>(p.cutoffHz)); return buf;
        case 2: std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(p.resonance * 100)); return buf;
        case 3: std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(p.envAmount * 100)); return buf;
        case 4: std::snprintf(buf, sizeof(buf), "%dMS", static_cast<int>(p.filterDecay * 1000)); return buf;
        case 5: return WaveformName(p.osc2Wave);
        case 6: std::snprintf(buf, sizeof(buf), "%+dST", p.osc2Semi); return buf;
        case 7: std::snprintf(buf, sizeof(buf), "%+dC", p.osc2Detune); return buf;
        case 8: std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(p.osc2Level * 100)); return buf;
        case 9: return p.acid ? "ACID" : (p.filter24 ? "24 DB" : "12 DB");
        case 10: std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(p.keytrack * 100)); return buf;
    }
    return "?";
}

void AdjustFm(SynthPatch& p, int localIdx, int dir) {
    if (localIdx == 0) {
        p.fmAlgorithm = (p.fmAlgorithm + dir + kNumFmAlgorithms) % kNumFmAlgorithms;
        return;
    }
    if (localIdx == 10) { p.fmIndexScale = std::clamp(p.fmIndexScale + dir * 0.25f, 0.25f, 8.0f); return; }
    if (localIdx == 9) { p.fmModEnv = std::clamp(p.fmModEnv + dir * 0.05f, 0.0f, 1.0f); return; }
    int opIdx = (localIdx - 1) / 2;
    bool isRatio = ((localIdx - 1) % 2) == 0;
    if (opIdx < 0 || opIdx >= 4) return;
    FMOperator& op = p.fmOps[opIdx];
    if (isRatio) op.ratio = std::clamp(op.ratio + dir * 0.25f, 0.25f, 16.0f);
    else op.level = std::clamp(op.level + dir * 0.05f, 0.0f, 1.0f);
}

std::string FormatFm(const SynthPatch& p, int localIdx) {
    char buf[32];
    if (localIdx == 0) return kFmAlgorithmNames[p.fmAlgorithm];
    if (localIdx == 10) { std::snprintf(buf, sizeof(buf), "%.2f", p.fmIndexScale); return buf; }
    if (localIdx == 9) { std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(p.fmModEnv * 100)); return buf; }
    int opIdx = (localIdx - 1) / 2;
    bool isRatio = ((localIdx - 1) % 2) == 0;
    if (opIdx < 0 || opIdx >= 4) return "?";
    const FMOperator& op = p.fmOps[opIdx];
    if (isRatio) { std::snprintf(buf, sizeof(buf), "%.2fX", op.ratio); return buf; }
    std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(op.level * 100)); return buf;
}

// Wavetable-specific rows 0-1 (WAVETABLE bank, POSITION); rows 2-5 reuse
// the exact same filter fields/step-sizes as AdjustSubtractive's
// CUTOFF/RESONANCE/ENV AMT/FILT DECAY (rows 1-4 there), since both
// engines share one filter block.
void AdjustWavetable(SynthPatch& p, int localIdx, int dir) {
    switch (localIdx) {
        case 0: p.wavetableBank = (p.wavetableBank + dir + kNumWavetableBanks) % kNumWavetableBanks; break;
        case 1: p.wtPosition = std::clamp(p.wtPosition + dir * 0.02f, 0.0f, 1.0f); break;
        case 2: p.cutoffHz = std::clamp(p.cutoffHz + dir * 100.0f, 100.0f, 18000.0f); break;
        case 3: p.resonance = std::clamp(p.resonance + dir * 0.02f, 0.0f, 0.99f); break;
        case 4: p.envAmount = std::clamp(p.envAmount + dir * 0.05f, 0.0f, 1.0f); break;
        case 5: p.filterDecay = std::clamp(p.filterDecay + dir * 0.02f, 0.01f, 2.0f); break;
    }
}

std::string FormatWavetable(const SynthPatch& p, int localIdx) {
    char buf[32];
    switch (localIdx) {
        case 0: return kWavetableBankNames[std::clamp(p.wavetableBank, 0, kNumWavetableBanks - 1)];
        case 1: std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(p.wtPosition * 100)); return buf;
        case 2: std::snprintf(buf, sizeof(buf), "%dHZ", static_cast<int>(p.cutoffHz)); return buf;
        case 3: std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(p.resonance * 100)); return buf;
        case 4: std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(p.envAmount * 100)); return buf;
        case 5: std::snprintf(buf, sizeof(buf), "%dMS", static_cast<int>(p.filterDecay * 1000)); return buf;
    }
    return "?";
}

} // namespace

int NumPatchParams(SynthEngineType engineType) {
    return 1 + EngineRowCount(engineType) + kNumSharedRows;
}

const char* PatchParamName(SynthEngineType engineType, int index) {
    if (index == 0) return "ENGINE";
    int engineRows = EngineRowCount(engineType);
    if (index - 1 < engineRows) {
        int local = index - 1;
        switch (engineType) {
            case SynthEngineType::FM:        return kFmRowNames[local];
            case SynthEngineType::Wavetable: return kWavetableRowNames[local];
            case SynthEngineType::Sample:    return kSampleRowNames[local];
            case SynthEngineType::Subtractive: default: return kSubtractiveRowNames[local];
        }
    }
    int tailIdx = index - 1 - engineRows;
    if (tailIdx >= 0 && tailIdx < kNumSharedRows) return kSharedRowNames[tailIdx];
    return "?";
}

void AdjustPatchParam(SynthPatch& p, int index, int dir) {
    if (index == 0) {
        // ENGINE TYPE row: cycles through all kNumSynthEngineTypes engines
        // (wraparound, same convention as WAVEFORM/LFO SHAPE cycling
        // elsewhere) -- Subtractive -> FM -> Wavetable -> back to
        // Subtractive on repeated Up presses, Down runs it in reverse.
        int e = (static_cast<int>(p.engineType) + dir + kNumSynthEngineTypes) % kNumSynthEngineTypes;
        const bool toSample = static_cast<SynthEngineType>(e) == SynthEngineType::Sample && p.engineType != SynthEngineType::Sample;
        p.engineType = static_cast<SynthEngineType>(e);
        if (toSample) {   // v44.53: a sample wants an open filter and a gate that holds it: the envelope / filter of a synth sound would choke it
            p.cutoffHz = 18000.0f; p.envAmount = 0.0f; p.resonance = std::min(p.resonance, 0.1f); p.keytrack = 0.0f;
            p.sustain = 1.0f; p.attack = std::min(p.attack, 0.002f); p.release = std::max(p.release, 0.25f);
        }
        return;
    }
    int engineRows = EngineRowCount(p.engineType);
    if (index - 1 < engineRows) {
        int local = index - 1;
        switch (p.engineType) {
            case SynthEngineType::FM:        AdjustFm(p, local, dir); break;
            case SynthEngineType::Wavetable: AdjustWavetable(p, local, dir); break;
            case SynthEngineType::Sample:    AdjustSubtractive(p, kSampleToSubtractive[local], dir); break;
            case SynthEngineType::Subtractive: default: AdjustSubtractive(p, local, dir); break;
        }
        return;
    }
    int tailIdx = index - 1 - engineRows;
    AdjustShared(p, tailIdx, dir);
}

std::string FormatPatchParam(const SynthPatch& p, int index) {
    if (index == 0) return SynthEngineTypeName(p.engineType);
    int engineRows = EngineRowCount(p.engineType);
    if (index - 1 < engineRows) {
        int local = index - 1;
        switch (p.engineType) {
            case SynthEngineType::FM:        return FormatFm(p, local);
            case SynthEngineType::Wavetable: return FormatWavetable(p, local);
            case SynthEngineType::Sample:    return FormatSubtractive(p, kSampleToSubtractive[local]);
            case SynthEngineType::Subtractive: default: return FormatSubtractive(p, local);
        }
    }
    int tailIdx = index - 1 - engineRows;
    return FormatShared(p, tailIdx);
}

} // namespace t8
