#pragma once
// The LIVE screen's context block: everything about the SELECTED instrument,
// laid out so nothing needs a sub-tab, collapse or scroll. What it shows
// follows the selection in the Sequencer:
//
//   DRUM   KIT (browser button + SAMPLE card) | OUTPUT (volume, pan) | tabs VOICE / EQ / FX  (same skeleton as a synth)
//   SYNTH  PRESET picker + ENGINE button | ENGINE params | ENVELOPE | LFO | VOICE
//          SENDS | INSERT FX | LENGTH
//   CHORD  two views of the same block: CHORDS (progression picker, SIMPLE /
//          COMPLEX, STACK / ARPEGGIO, arp style + rate, chord length, the four
//          CUSTOM chords) and SOUND (identical to a synth's block)
//
// Every value is a ParamCell: click or drag it with the mouse, or select it
// with LEFT / RIGHT and change it with UP / DOWN (hold SHIFT for bigger
// jumps) -- the d-pad convention for this screen. Synth patch edits use the
// same SynthParams.h list and live-engine push everywhere.
#include <functional>
#include "PresetBrowser.h"
#include "app/AppState.h"
#include "ParamCell.h"
#include <map>
#include <string>
#include <vector>

namespace t8::desktop {

struct DpadKeys {
    bool up = false, down = false, left = false, right = false; // already auto-repeated
    bool coarse = false;                                         // shift held
};

class ContextPanel {
public:
    void Draw(AppState& app, const DpadKeys& keys);
    void ShowChordSound() { chordTab_ = 1; }   // v44.46: CHORD LAB's SOUND view draws the chord instrument's SOUND tabs

private:
    // ---- per-frame cell bookkeeping (see ParamCell.h) ----
    int cellCounter_ = 0;      // index handed to the next cell this frame
    int lastCellCount_ = 0;    // cells drawn last frame (bounds LEFT / RIGHT)
    int selCell_ = 0;          // selected cell
    bool pendingUndo_ = false; // push one undo before the next real change
    DpadKeys keys_;
    ImVec2 cellSize_{60.0f, 68.0f};
    float accum_[96] = {};

    // ---- chord / preset UI state ----
    char presetName_[48] = "";
    int chordTab_ = 0;         // 0 = CHORDS, 1 = SOUND
    int drumTab_ = 0;          // DRUM tabs: 0 = VOICE, 1 = EQ, 2 = FX, 3 = AUTO
    int eqDrag_ = -1;          // EQ curve node being dragged (0 HPF, 1 low, 2 mid, 3 high), -1 = none
    int synthTab_ = 0;         // SOUND tabs: 0 = OSC, 1 = FILTER, 2 = ENV + LFO, 3 = EQ, 4 = FX, 5 = AUTO
    int autoParam_ = 0;        // v44.39: the knob the AUTO tab draws (an AutoParam)
    int presetIdx_[kNumSynthTracks + 1] = {-1, -1, -1, -1}; // last preset applied per slot (-1 = custom)

    // cell helpers
    template <class Apply>
    void FloatCell(AppState& app, const char* id, const char* cap, float cur, float lo, float hi,
                   float step, const char* text, Apply&& apply, bool dim = false);
    int DiscreteCell(const char* id, const char* cap, const char* text, bool dim = false);
    void PatchCell(AppState& app, int ti, int paramIndex);

    // layout helpers
    void BlockBegin(const char* title);
    void BlockEnd(bool separator = true);

    // content
    void DrawHeader(AppState& app, int ti);
    void DrawDrumVoice(AppState& app, int ti);
    void DrawDrum(AppState& app, int ti);
    void DrawKitBlock(AppState& app, int ti, float width);
    std::string DrumVariantLabel(AppState& app, int ti, int v, int nVar);   // C4b: kick.wav, kick_2.wav ... of the edited scene's kit
    void HearDrum(AppState& app, const Track& t, const std::shared_ptr<const SampleBuffer>& buf);
    std::map<std::string, std::vector<std::string>> drumVarNames_;
    std::vector<SamplePtr> hearKeep_;   // C4b: the last few HEAR samples stay alive here (the audio thread may hold the one that plays)
    void DrawSampler(AppState& app, int ti);        // v44.50: the 11th instrument -- SAMPLE block + OUTPUT | VOICE / EQ / FX / AUTO
    void DrawSamplerVoice(AppState& app, int ti);
    void DrawSampleBlock(AppState& app, int ti, float width);
    // v44.52: ONE editor for the sample of a drum track and of the sampler -- the same card, the same knobs, the same words.
    // `ed` is the track's SampleEdit (the sampler's patch is copied in and out by the caller through onChange).
    void DrawWaveCard(AppState& app, const char* id, const float* data, int frames, int channels, int sampleRate, const SampleEdit& ed,
                      float width, float height, ImU32 tc, int slices, const char* emptyMsg, bool* clicked,
                      const uint16_t* marks = nullptr, int markCount = 0);   // marks: HITS slices (v44.52.1)
    void DrawSampleEditBlocks(AppState& app, SampleEdit& ed, const std::function<void()>& onChange, bool drum, bool lastSeparator, bool hideAttack = false);
    void DrawSound(AppState& app, int ti);
    void DrawChords(AppState& app, int ti);
    void DrawAuto(AppState& app, int ti, float availW);   // v44.39: curves of the instrument's knobs over the scene
    void DrawEq(AppState& app, int ti, float availW);   // channel EQ: response curve + cells
    void DrawCommonRow(AppState& app, int ti);   // SENDS | INSERT FX | LENGTH
    void DrawPresetBlock(AppState& app, int ti, float width);
    void ApplyPresetToTrack(AppState& app, int ti, int idx);   // PRESET block + browser share this
    // v44.50: the waveform of the sample shown on the sampler's card, reduced to min / max pairs once per sample (never per frame)
    const void* wavePtr_ = nullptr;
    int waveFrames_ = 0;
    double hearOffAt_ = 0.0;   // v44.53: HEAR of a SAMPLE synth: when the note is released (ImGui time), and where
    int hearTgt_ = -1;
    void StopHear(AppState& app) { if (hearOffAt_ <= 0.0) return; if (hearTgt_ >= kNumSynthTracks) app.engine.ChordNoteOff(); else if (hearTgt_ >= 0) app.engine.SynthNoteOff(hearTgt_); hearOffAt_ = 0.0; }
    float hitSens_ = 0.5f;   // v44.52.1: the sensitivity of FIND HITS (not saved: the marks are)
    std::vector<float> wavePeaks_;   // 2 floats (min, max) per column
    PresetBrowser browser_;
};

} // namespace t8::desktop
