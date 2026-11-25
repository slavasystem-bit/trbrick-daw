#pragma once
// Shared synth-patch parameter list, used by both the Piano Roll tab's
// X+Y sound-design overlay and the dedicated SYNTH tab -- one canonical
// list of parameters/adjust-step-sizes/formatting so the two surfaces
// can never drift out of sync with each other.
//
// Phase 5/5b: row COUNT and NAMES now depend on SynthPatch::engineType --
// row 0 is always ENGINE TYPE (cycles Subtractive -> FM -> Wavetable ->
// back to Subtractive), followed by engine-specific rows (5 for
// Subtractive: WAVEFORM/CUTOFF/RESONANCE/ENV AMT/FILT DECAY; 9 for FM:
// ALGORITHM + 4x OP RATIO/LEVEL pairs; 6 for Wavetable: WAVETABLE bank/
// POSITION + the same 4 filter fields Subtractive has), followed by 10
// rows shared by all three engines (ATTACK..LFO SHAPE). That's 16 total
// rows for a Subtractive patch, 20 for an FM patch, 17 for a Wavetable
// patch. Callers MUST use NumPatchParams()/PatchParamName() instead of a
// fixed constant/array now, and must clamp any stored row-index UI state
// (e.g. AppState::prPatchParam) whenever the engine type changes, since
// switching to a shorter-row-count engine can leave a previously valid
// index out of range -- see SynthView.cpp/PianoRollView.cpp.
#include "core/Types.h"
#include <string>

namespace t8 {

int NumPatchParams(SynthEngineType engineType);
const char* PatchParamName(SynthEngineType engineType, int index);

void AdjustPatchParam(SynthPatch& p, int index, int dir);
std::string FormatPatchParam(const SynthPatch& p, int index);

} // namespace t8
