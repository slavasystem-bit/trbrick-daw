#include "BrickGlyphs.h"
#include "ValueField.h"
#include "LaneWidget.h"
#include "ContextPanel.h"
#include "Visual.h"
#include "Scope.h"
#include "LiveWidgets.h"
#include "SampleRow.h"
#include "TrackStyle.h"
#include "MixerSync.h"
#include "SynthSync.h"
#include "ChordEditor.h"
#include "core/SynthVoice.h"
#include "core/Persistence.h"
#include "core/ChordVoice.h"
#include "core/TrackEq.h"
#include "imgui.h"
#include "InfoLine.h"
#include "core/Scales.h"
#include "core/Chords.h"
#include "core/ChordProgressions.h"
#include "core/SynthPresets.h"
#include "core/Wavetables.h"
#include "app/SynthParams.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace t8::desktop {

namespace {

constexpr ImVec4 kAmberV(0.95f, 0.64f, 0.24f, 1.0f);

// ---- patch-param presentation ------------------------------------------

bool StartsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// Shorter captions so ~19 cells (an FM patch) still fit one row.
std::string ShortName(const char* name) {
    std::string n = name;
    static const struct { const char* from; const char* to; } kMap[] = {
        {"FILT DECAY", "F.DECAY"}, {"RESONANCE", "RESO"}, {"WAVEFORM", "WAVE"}, {"WAVETABLE", "TABLE"},
        {"POSITION", "POS"}, {"ALGORITHM", "ALGO"}, {"LFO TARGET", "TARGET"}, {"LFO RATE", "RATE"},
        {"LFO DEPTH", "DEPTH"}, {"LFO SHAPE", "SHAPE"}, {"OP1 RATIO", "1 RATIO"}, {"OP1 LEVEL", "1 LEVEL"},
        {"OP2 RATIO", "2 RATIO"}, {"OP2 LEVEL", "2 LEVEL"}, {"OP3 RATIO", "3 RATIO"}, {"OP3 LEVEL", "3 LEVEL"},
        {"OP4 RATIO", "4 RATIO"}, {"OP4 LEVEL", "4 LEVEL"},
        {"OSC2 WAVE", "WAVE 2"}, {"OSC2 SEMI", "SEMI 2"}, {"OSC2 DETUNE", "FINE 2"}, {"OSC2 LEVEL", "LEVEL 2"},
        {"KEYTRACK", "KEYTRK"}, {"FILTER", "FILTER"}, {"UNI DETUNE", "DETUNE"}, {"UNI SPREAD", "SPREAD"}, {"SUB LEVEL", "SUB"}, {"SUB OCT", "SUB OCT"}, {"PHASE SYNC", "PH.SYNC"},
    };
    for (const auto& m : kMap) if (n == m.from) return m.to;
    return n;
}

// 0..1 fill for the continuous params; -1 for the discrete / enum ones.
float PatchNorm(const SynthPatch& p, const std::string& name) {
    if (name == "ATTACK") return std::sqrt(std::clamp(p.attack / 2.0f, 0.0f, 1.0f));
    if (name == "DECAY") return std::sqrt(std::clamp(p.decay / 2.0f, 0.0f, 1.0f));
    if (name == "RELEASE") return std::sqrt(std::clamp(p.release / 2.0f, 0.0f, 1.0f));
    if (name == "FILT DECAY") return std::sqrt(std::clamp(p.filterDecay / 2.0f, 0.0f, 1.0f));
    if (name == "SUSTAIN") return p.sustain;
    if (name == "GLIDE") return p.glideTime;
    if (name == "VOLUME") return p.volume / 2.0f;
    if (name == "LFO RATE") return (p.lfoRateHz - 0.05f) / 9.95f;
    if (name == "LFO DEPTH") return p.lfoDepth;
    if (name == "CUTOFF") return std::log(std::max(100.0f, p.cutoffHz) / 100.0f) / std::log(180.0f);
    if (name == "RESONANCE") return p.resonance / 0.99f;
    if (name == "ENV AMT") return p.envAmount;
    if (name == "POSITION") return p.wtPosition;
    if (name == "UNISON") return (p.unisonVoices - 1) / static_cast<float>(kMaxUnisonVoices - 1);
    if (name == "UNI DETUNE") return p.unisonDetune;
    if (name == "UNI SPREAD") return p.unisonSpread;
    if (name == "SUB LEVEL") return p.subLevel;
    if (name == "NOISE") return p.noiseLevel;
    if (name == "DRIVE") return p.drive;
    if (name == "DRIFT") return p.drift;
    if (name == "OSC2 LEVEL") return p.osc2Level;
    if (name == "OSC2 SEMI") return (p.osc2Semi + 24) / 48.0f;
    if (name == "OSC2 DETUNE") return (p.osc2Detune + 50) / 100.0f;
    if (name == "KEYTRACK") return p.keytrack;
    if (name == "MOD ENV") return p.fmModEnv;
    if (name == "MOD RANGE") return (p.fmIndexScale - 0.25f) / 7.75f;
    if (StartsWith(name, "OP") && name.size() >= 9) {
        int op = name[2] - '1';
        if (op >= 0 && op < 4) {
            if (name.find("RATIO") != std::string::npos) return std::sqrt(std::clamp((p.fmOps[op].ratio - 0.25f) / 15.75f, 0.0f, 1.0f));
            return p.fmOps[op].level;
        }
    }
    return -1.0f; // WAVEFORM, LFO TARGET, LFO SHAPE, ALGORITHM, WAVETABLE ...
}

void DrawWaveIcon(ImDrawList* dl, ImVec2 a, ImVec2 b, const SynthPatch& p, ImU32 col) {
    const int n = 28;
    std::vector<ImVec2> pts;
    float w = b.x - a.x, h = b.y - a.y, mid = a.y + h * 0.5f;
    for (int i = 0; i <= n; ++i) {
        float t = static_cast<float>(i) / n * 2.0f; // two cycles
        float ph = t - std::floor(t);
        float y = 0.0f;
        if (p.engineType == SynthEngineType::FM) {
            y = 0.65f * std::sin(ph * 6.2831853f) + 0.35f * std::sin(ph * 6.2831853f * 3.0f + 1.0f);
        } else if (p.engineType == SynthEngineType::Sample) {   // v44.53: a hit that dies away
            y = (1.0f - ph) * (0.8f * std::sin(ph * 6.2831853f * 4.0f) + 0.2f * std::sin(ph * 6.2831853f * 9.0f + 0.5f));
        } else if (p.engineType == SynthEngineType::Wavetable) {
            y = 0.55f * std::sin(ph * 6.2831853f) + 0.30f * std::sin(ph * 6.2831853f * 2.0f) + 0.15f * std::sin(ph * 6.2831853f * 5.0f);
        } else {
            switch (p.waveform) {
                case Waveform::Sine: y = std::sin(ph * 6.2831853f); break;
                case Waveform::Triangle: y = ph < 0.5f ? (ph * 4.0f - 1.0f) : (3.0f - ph * 4.0f); break;
                case Waveform::Saw: y = 1.0f - 2.0f * ph; break;
                case Waveform::Square: y = ph < 0.5f ? 1.0f : -1.0f; break;
            }
        }
        pts.push_back(ImVec2(a.x + w * i / n, mid - y * h * 0.42f));
    }
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, 0, 2.0f);
}

} // namespace

// ===========================================================================
// cell + layout helpers
// ===========================================================================

template <class Apply>
void ContextPanel::FloatCell(AppState& app, const char* id, const char* cap, float cur, float lo, float hi,
                             float step, const char* text, Apply&& apply, bool dim) {
    int idx = cellCounter_++;
    bool sel = (idx == selCell_);
    float norm = hi > lo ? (cur - lo) / (hi - lo) : 0.0f;
    ParamCellResult r = ParamCell(id, cap, text, norm, sel, &accum_[std::min(idx, 95)], 3.0f, cellSize_, dim);
    int steps = r.steps;
    int key = 0;
    if (sel) {
        int mul = keys_.coarse ? 4 : 1;
        if (keys_.up) key += mul;
        if (keys_.down) key -= mul;
    }
    if (r.clicked) selCell_ = idx;
    if (r.activated || key != 0) pendingUndo_ = true;
    steps += key;
    if (steps != 0) {
        float nv = std::clamp(cur + steps * step, lo, hi);
        if (nv != cur) {
            if (pendingUndo_) { app.PushUndo(); pendingUndo_ = false; }
            apply(nv);
        }
    }
}

int ContextPanel::DiscreteCell(const char* id, const char* cap, const char* text, bool dim) {
    int idx = cellCounter_++;
    bool sel = (idx == selCell_);
    ParamCellResult r = ParamCell(id, cap, text, -1.0f, sel, &accum_[std::min(idx, 95)], 9.0f, cellSize_, dim);
    int steps = r.steps;
    if (r.clicked) selCell_ = idx;
    if (sel) {
        if (keys_.up) steps += 1;
        if (keys_.down) steps -= 1;
    }
    return steps;
}

void ContextPanel::PatchCell(AppState& app, int ti, int paramIndex) {
    const SynthPatch& patch = TrackPatch(app, ti);
    if (paramIndex >= NumPatchParams(patch.engineType)) return;
    std::string name = PatchParamName(patch.engineType, paramIndex);
    std::string cap = ShortName(name.c_str());
    std::string val = FormatPatchParam(patch, paramIndex);
    float norm = PatchNorm(patch, name);
    int idx = cellCounter_++;
    bool sel = (idx == selCell_);
    char id[24];
    std::snprintf(id, sizeof(id), "p%d", paramIndex);
    ParamCellResult r = ParamCell(id, cap.c_str(), val.c_str(), norm, sel, &accum_[std::min(idx, 95)],
                                  norm < 0.0f ? 9.0f : 3.0f, cellSize_);
    int steps = r.steps;
    if (r.clicked) selCell_ = idx;
    if (sel) {
        int mul = (keys_.coarse && norm >= 0.0f) ? 5 : 1;
        if (keys_.up) steps += mul;
        if (keys_.down) steps -= mul;
    }
    int dir = steps > 0 ? 1 : -1;
    if (steps != 0) app.PushSoundUndo();
    for (int k = 0; k < std::abs(steps); ++k) AdjustTrackPatchParam(app, ti, paramIndex, dir, /*pushUndo=*/false);
    if (steps != 0) presetIdx_[IsChordTrack(ti) ? kNumSynthTracks : SynthSlotForTrack(ti)] = -1;
}

void ContextPanel::BlockBegin(const char* title) {
    ImGui::BeginGroup();
    ImGui::TextColored(kAmberV, "%s", title);
}

void ContextPanel::BlockEnd(bool separator) {
    ImGui::EndGroup();
    if (separator) {
        ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(mx.x + 9.0f, mn.y + 2.0f), ImVec2(mx.x + 9.0f, mx.y),
                                            vis::kLine, 1.0f);
    }
    ImGui::SameLine(0, 18.0f);
}

// ===========================================================================
// header strip
// ===========================================================================

void ContextPanel::DrawHeader(AppState& app, int ti) {
    Pattern& pat = app.CurrentPattern();
    Track& t = pat.tracks[ti];
    const char* typeLabel = t.type == TrackType::Drum ? "DRUM" : t.type == TrackType::Chord ? "CHORD" : t.type == TrackType::Sampler ? "WAV" : "SYNTH";
    ImU32 dot = TrackColor(ti);
    ImVec2 p = ImGui::GetCursorScreenPos();
    DrawInstrumentGlyph(ImGui::GetWindowDrawList(), ti, ImVec2(p.x + 9.0f, p.y + ImGui::GetFrameHeight() * 0.5f), 16.0f, dot);   // v44.37: the pictogram instead of the dot
    ImGui::Dummy(ImVec2(22.0f, 1.0f));
    ImGui::SameLine(0, 0);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", TrackDisplayName(t).c_str());
    ImGui::SameLine(0, 8);
    ImGui::TextDisabled("%s", typeLabel);

    if (t.type == TrackType::Chord) {
        ImGui::SameLine(0, 20);
        for (int tab = 0; tab < 2; ++tab) {
            if (tab > 0) ImGui::SameLine(0, 4);
            bool on = (chordTab_ == tab);
            if (ToggleButton(tab == 0 ? "CHORDS" : "SOUND", on, ImVec2(76, 0), ToggleTab())) { chordTab_ = tab; selCell_ = 0; }
        }
    }

    ImGui::SameLine(0, 14);
    if (ImGui::Button("COPY PARAMS")) {
        ImGui::SetClipboardText(DescribeInstrument(app.project, pat, ti).c_str());
        app.SetStatus("PARAMETERS OF " + TrackDisplayName(t) + " COPIED");
    }
    if (ImGui::IsItemHovered()) Hint("Copy every sound parameter of this instrument (and its steps and effects) as text,\nto paste into a question.");

    const char* hint = "LEFT / RIGHT  select      UP / DOWN  change      SHIFT  bigger steps";
    float hw = ImGui::CalcTextSize(hint).x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + 16.0f, ImGui::GetWindowContentRegionMax().x - hw));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", hint);
}

// ===========================================================================
// shared bottom row: SENDS | INSERT FX | LENGTH
// ===========================================================================

void ContextPanel::DrawCommonRow(AppState& app, int ti) {
    Pattern& pat = app.CurrentPattern();
    Track& t = app.StepTrack(pat, ti);      // chord bank: the chord scene's own length
    auto& mt = app.EditMixer().tracks[ti];
    char buf[24];
    cellSize_.x = 68.0f; // "PAN DEPTH" needs the extra width

    BlockBegin("SENDS");
    std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(mt.reverbSend * 100 + 0.5f));
    FloatCell(app, "rev", "REVERB", mt.reverbSend, 0.0f, 1.0f, 1.0f / 64.0f, buf,
              [&](float v) { SetTrackFloatField(app, ti, &Mixer::TrackState::reverbSend, &Track::reverbSend, v, 0.0f, 1.0f); });
    ImGui::SameLine(0, 6);
    std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(mt.delaySend * 100 + 0.5f));
    FloatCell(app, "del", "DELAY", mt.delaySend, 0.0f, 1.0f, 1.0f / 64.0f, buf,
              [&](float v) { SetTrackFloatField(app, ti, &Mixer::TrackState::delaySend, &Track::delaySend, v, 0.0f, 1.0f); });
    BlockEnd();

    BlockBegin("STEREO");
    std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(mt.width * 100 + 0.5f));
    FloatCell(app, "wid", "WIDTH", mt.width, 0.0f, 2.0f, 2.0f / 64.0f, buf,
              [&](float v) { SetTrackFloatField(app, ti, &Mixer::TrackState::width, &Track::width, v, 0.0f, 2.0f); });
    ImGui::SameLine(0, 6);
    std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(mt.spread * 100 + 0.5f));
    FloatCell(app, "spr", "SPREAD", mt.spread, 0.0f, 1.0f, 1.0f / 64.0f, buf,
              [&](float v) { SetTrackFloatField(app, ti, &Mixer::TrackState::spread, &Track::spread, v, 0.0f, 1.0f); });
    BlockEnd();

    BlockBegin("INSERT FX");
    std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(mt.insertDrive * 100 + 0.5f));
    FloatCell(app, "drv", "DRIVE", mt.insertDrive, 0.0f, 1.0f, 1.0f / 64.0f, buf,
              [&](float v) { SetTrackFloatField(app, ti, &Mixer::TrackState::insertDrive, &Track::insertDrive, v, 0.0f, 1.0f); });
    ImGui::SameLine(0, 6);
    std::snprintf(buf, sizeof(buf), "%+d%%", static_cast<int>(std::lround(mt.insertEqTilt * 100)));
    FloatCell(app, "eq", "EQ TILT", mt.insertEqTilt, -1.0f, 1.0f, 2.0f / 64.0f, buf,
              [&](float v) { SetTrackFloatField(app, ti, &Mixer::TrackState::insertEqTilt, &Track::insertEqTilt, v, -1.0f, 1.0f); });
    ImGui::SameLine(0, 6);
    std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(mt.insertChorus * 100 + 0.5f));
    FloatCell(app, "cho", "CHORUS", mt.insertChorus, 0.0f, 1.0f, 1.0f / 64.0f, buf,
              [&](float v) { SetTrackFloatField(app, ti, &Mixer::TrackState::insertChorus, &Track::insertChorus, v, 0.0f, 1.0f); });
    ImGui::SameLine(0, 6);
    std::snprintf(buf, sizeof(buf), "%.2fHZ", mt.autopanRate);
    FloatCell(app, "apr", "PAN RATE", mt.autopanRate, 0.05f, 8.0f, 0.1f, buf,
              [&](float v) { SetTrackFloatField(app, ti, &Mixer::TrackState::autopanRate, &Track::autopanRate, v, 0.05f, 8.0f); });
    ImGui::SameLine(0, 6);
    std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(mt.autopanDepth * 100 + 0.5f));
    FloatCell(app, "apd", "PAN DEPTH", mt.autopanDepth, 0.0f, 1.0f, 1.0f / 64.0f, buf,
              [&](float v) { SetTrackFloatField(app, ti, &Mixer::TrackState::autopanDepth, &Track::autopanDepth, v, 0.0f, 1.0f); });
    BlockEnd();

    BlockBegin("LENGTH / FEEL");
    int polyStep = DiscreteCell("poly", "POLY", t.useOwnLength ? "ON" : "OFF");
    if (polyStep != 0) {
        app.PushUndo();
        t.useOwnLength = !t.useOwnLength;
        if (t.useOwnLength) t.length = pat.masterLength;
    }
    ImGui::SameLine(0, 6);
    int trackLen = t.useOwnLength ? t.length : pat.masterLength;
    std::snprintf(buf, sizeof(buf), "%d", trackLen);
    FloatCell(app, "len", "STEPS", static_cast<float>(trackLen), 1.0f, static_cast<float>(kMaxSteps), 1.0f, buf,
              [&](float v) { t.useOwnLength = true; t.length = std::clamp(static_cast<int>(v + 0.5f), 1, kMaxSteps); });
    // v39: this track's own swing; the lowest position means "follow the scene swing" (shown as SCENE)
    ImGui::SameLine(0, 6);
    if (t.swing < 0.0f) std::snprintf(buf, sizeof(buf), "SCENE");
    else std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(t.swing * 100.0f + 0.5f));
    FloatCell(app, "tsw", "SWING", t.swing < 0.0f ? -0.01f : t.swing, -0.01f, 0.75f, 0.01f, buf,
              [&](float v) { t.swing = v < 0.0f ? -1.0f : std::clamp(std::round(v * 100.0f) / 100.0f, 0.0f, 0.75f); });
    BlockEnd(false);
}

// ===========================================================================
// channel EQ (v29): HPF + low shelf + mid bell + high shelf, with a response curve
// ===========================================================================

void ContextPanel::DrawEq(AppState& app, int ti, float availW) {
    using namespace t8;
    auto& mt = app.EditMixer().tracks[ti];
    const TrackEqParams ep = TrackEqOf(mt);
    const double sr = std::max(8000, app.engine.SampleRate());
    char buf[32];

    auto setF = [&](float Mixer::TrackState::*mf, float Track::*tf, float v, float lo, float hi) {
        SetTrackFloatField(app, ti, mf, tf, v, lo, hi);
    };
    auto fmtHz = [](char* b, size_t n, float f) {
        if (f >= 1000.0f) std::snprintf(b, n, "%.1fK", f / 1000.0f); else std::snprintf(b, n, "%.0fHZ", f);
    };

    // ---- response curve (drag the dots; wheel = mid Q; double-click a dot = flatten it) ----
    const float cw = 250.0f;
    const float ch = std::max(64.0f, cellSize_.y + 8.0f);
    BlockBegin("EQ");
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 p1(p0.x + cw, p0.y + ch);
    ImGui::InvisibleButton("##eqcurve", ImVec2(cw, ch));
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, vis::kInset, 5.0f);
    const float lo20 = std::log10(20.0f), span = std::log10(20000.0f) - lo20;
    auto fx = [&](float f) { return p0.x + (std::log10(std::clamp(f, 20.0f, 20000.0f)) - lo20) / span * cw; };
    auto gy = [&](float db) { return p0.y + ch * 0.5f - std::clamp(db, -kEqMaxDb, kEqMaxDb) / kEqMaxDb * (ch * 0.5f - 4.0f); };
    for (float f : {50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f})
        dl->AddLine(ImVec2(fx(f), p0.y + 1), ImVec2(fx(f), p1.y - 1), (f == 100.0f || f == 1000.0f || f == 10000.0f) ? vis::kLine : IM_COL32(40, 42, 54, 255), 1.0f);
    for (float d : {-10.0f, 10.0f}) dl->AddLine(ImVec2(p0.x + 1, gy(d)), ImVec2(p1.x - 1, gy(d)), IM_COL32(40, 42, 54, 255), 1.0f);
    dl->AddLine(ImVec2(p0.x + 1, gy(0)), ImVec2(p1.x - 1, gy(0)), vis::kLineHi, 1.0f);
    {
        ImVec2 pts[121];
        for (int i = 0; i <= 120; ++i) {
            float f = std::pow(10.0f, lo20 + span * i / 120.0f);
            pts[i] = ImVec2(p0.x + cw * i / 120.0f, gy(static_cast<float>(TrackEqResponseDb(ep, sr, f))));
        }
        dl->PushClipRect(p0, p1, true);
        dl->AddPolyline(pts, 121, ep.Flat() ? vis::kTextDim : vis::kAccent, ImDrawFlags_None, 2.0f);
        dl->PopClipRect();
    }
    struct Node { float f; bool on; };
    const Node nodes[4] = {{ep.hpfHz, ep.HpfOn()}, {ep.lowHz, ep.lowDb != 0.0f}, {ep.midHz, ep.midDb != 0.0f}, {ep.highHz, ep.highDb != 0.0f}};
    ImVec2 npos[4];
    for (int n = 0; n < 4; ++n) {
        float f = nodes[n].f;
        npos[n] = ImVec2(fx(f), gy(static_cast<float>(TrackEqResponseDb(ep, sr, f))));
        if (n == 0 && !nodes[0].on) npos[0] = ImVec2(fx(20.0f) + 6.0f, gy(0));
        if (n != 0 && !nodes[n].on) npos[n].y = gy(0);
    }
    ImVec2 mp = ImGui::GetIO().MousePos;
    auto nearestNode = [&]() { int best = 0; float bd = 1e9f; for (int n = 0; n < 4; ++n) { float d = std::fabs(mp.x - npos[n].x) + 0.35f * std::fabs(mp.y - npos[n].y); if (d < bd) { bd = d; best = n; } } return best; };
    const int hoverNode = hov ? nearestNode() : -1;
    static const ImU32 kNodeCol[4] = {IM_COL32(240, 110, 110, 255), IM_COL32(95, 220, 200, 255), IM_COL32(242, 163, 61, 255), IM_COL32(150, 170, 255, 255)};
    for (int n = 0; n < 4; ++n) {
        float r = (n == hoverNode || n == eqDrag_) ? 6.0f : 4.5f;
        if (nodes[n].on) dl->AddCircleFilled(npos[n], r, kNodeCol[n]); else dl->AddCircle(npos[n], r, kNodeCol[n], 0, 1.5f);
    }
    dl->AddText(nullptr, 10.0f, ImVec2(p0.x + 3, p0.y + 1), vis::kTextDim, "+15");
    dl->AddText(nullptr, 10.0f, ImVec2(p0.x + 3, p1.y - 12), vis::kTextDim, "-15 dB");
    dl->AddText(nullptr, 10.0f, ImVec2(fx(100.0f) + 2, p1.y - 12), vis::kTextDim, "100");
    dl->AddText(nullptr, 10.0f, ImVec2(fx(1000.0f) + 2, p1.y - 12), vis::kTextDim, "1k");
    dl->AddText(nullptr, 10.0f, ImVec2(fx(10000.0f) + 2, p1.y - 12), vis::kTextDim, "10k");
    if (ImGui::IsItemActivated()) { eqDrag_ = nearestNode(); app.PushUndo(); }
    if (ImGui::IsItemActive() && eqDrag_ >= 0 && !ImGui::IsMouseDoubleClicked(0)) {
        float f = std::pow(10.0f, lo20 + span * std::clamp((mp.x - p0.x) / cw, 0.0f, 1.0f));
        float db = std::clamp((p0.y + ch * 0.5f - mp.y) / (ch * 0.5f - 4.0f) * kEqMaxDb, -kEqMaxDb, kEqMaxDb);
        db = std::round(db * 2.0f) / 2.0f;
        switch (eqDrag_) {
            case 0: setF(&Mixer::TrackState::eqHpfHz, &Track::eqHpfHz, f < 22.0f ? 20.0f : std::clamp(f, 20.0f, 500.0f), 20.0f, 500.0f); break;
            case 1: setF(&Mixer::TrackState::eqLowHz, &Track::eqLowHz, f, 30.0f, 500.0f); setF(&Mixer::TrackState::eqLowDb, &Track::eqLowDb, db, -kEqMaxDb, kEqMaxDb); break;
            case 2: setF(&Mixer::TrackState::eqMidHz, &Track::eqMidHz, f, 80.0f, 10000.0f); setF(&Mixer::TrackState::eqMidDb, &Track::eqMidDb, db, -kEqMaxDb, kEqMaxDb); break;
            default: setF(&Mixer::TrackState::eqHighHz, &Track::eqHighHz, f, 1500.0f, 16000.0f); setF(&Mixer::TrackState::eqHighDb, &Track::eqHighDb, db, -kEqMaxDb, kEqMaxDb); break;
        }
    }
    if (!ImGui::IsMouseDown(0)) eqDrag_ = -1;
    if (hov && ImGui::IsMouseDoubleClicked(0)) {   // flatten the node under the cursor
        int n = nearestNode();
        app.PushUndo();
        if (n == 0) setF(&Mixer::TrackState::eqHpfHz, &Track::eqHpfHz, 20.0f, 20.0f, 500.0f);
        else if (n == 1) setF(&Mixer::TrackState::eqLowDb, &Track::eqLowDb, 0.0f, -kEqMaxDb, kEqMaxDb);
        else if (n == 2) setF(&Mixer::TrackState::eqMidDb, &Track::eqMidDb, 0.0f, -kEqMaxDb, kEqMaxDb);
        else setF(&Mixer::TrackState::eqHighDb, &Track::eqHighDb, 0.0f, -kEqMaxDb, kEqMaxDb);
        eqDrag_ = -1;
    }
    if (hov && ImGui::GetIO().MouseWheel != 0.0f) {   // wheel = mid band Q (width)
        app.PushUndo();
        setF(&Mixer::TrackState::eqMidQ, &Track::eqMidQ, mt.eqMidQ * std::pow(1.12f, ImGui::GetIO().MouseWheel), 0.3f, 6.0f);
    }
    if (hov && !ImGui::IsMouseDown(0))
        Hint("Drag a dot: frequency (left / right) and gain (up / down)\nRed = low cut, teal = low shelf, orange = mid bell, blue = high shelf\nMouse wheel = width (Q) of the mid bell  -  double-click a dot = flatten it");
    BlockEnd(true);

    // ---- cells ----
    int cells = 10;
    cellSize_.x = std::clamp((availW - cw - 5 * 19.0f - 6.0f * cells) / cells, 40.0f, 62.0f);
    auto freqCell = [&](const char* id, const char* cap, float cur, float lo, float hi, float Mixer::TrackState::*mf, float Track::*tf, bool offAtMin) {
        if (offAtMin && cur <= 20.5f) std::snprintf(buf, sizeof(buf), "OFF"); else fmtHz(buf, sizeof(buf), cur);
        FloatCell(app, id, cap, std::log2(cur), std::log2(lo), std::log2(hi), 1.0f / 12.0f, buf,
                  [&, mf, tf, lo, hi, offAtMin](float v) { float f = std::exp2(v); if (offAtMin && f < 22.0f) f = 20.0f; SetTrackFloatField(app, ti, mf, tf, f, lo, hi); });
    };
    auto gainCell = [&](const char* id, float cur, float Mixer::TrackState::*mf, float Track::*tf) {
        std::snprintf(buf, sizeof(buf), "%+.1fDB", cur);
        FloatCell(app, id, "GAIN", cur, -kEqMaxDb, kEqMaxDb, 0.5f, buf,
                  [&, mf, tf](float v) { SetTrackFloatField(app, ti, mf, tf, std::round(v * 2.0f) / 2.0f, -kEqMaxDb, kEqMaxDb); });
    };

    BlockBegin("LOW CUT");
    freqCell("hpf", "FREQ", mt.eqHpfHz, 20.0f, 500.0f, &Mixer::TrackState::eqHpfHz, &Track::eqHpfHz, true);
    ImGui::SameLine(0, 6);
    int sl = DiscreteCell("hps", "SLOPE", mt.eqHpfSlope >= 0.5f ? "24 DB" : "12 DB", !ep.HpfOn());
    if (sl != 0) { app.PushUndo(); setF(&Mixer::TrackState::eqHpfSlope, &Track::eqHpfSlope, mt.eqHpfSlope >= 0.5f ? 0.0f : 1.0f, 0.0f, 1.0f); }
    BlockEnd(true);

    BlockBegin("LOW SHELF");
    gainCell("lg", mt.eqLowDb, &Mixer::TrackState::eqLowDb, &Track::eqLowDb);
    ImGui::SameLine(0, 6);
    freqCell("lf", "FREQ", mt.eqLowHz, 30.0f, 500.0f, &Mixer::TrackState::eqLowHz, &Track::eqLowHz, false);
    BlockEnd(true);

    BlockBegin("MID BELL");
    gainCell("mg", mt.eqMidDb, &Mixer::TrackState::eqMidDb, &Track::eqMidDb);
    ImGui::SameLine(0, 6);
    freqCell("mf", "FREQ", mt.eqMidHz, 80.0f, 10000.0f, &Mixer::TrackState::eqMidHz, &Track::eqMidHz, false);
    ImGui::SameLine(0, 6);
    std::snprintf(buf, sizeof(buf), "%.1f", mt.eqMidQ);
    FloatCell(app, "mq", "Q", std::log2(mt.eqMidQ), std::log2(0.3f), std::log2(6.0f), 1.0f / 12.0f, buf,
              [&](float v) { SetTrackFloatField(app, ti, &Mixer::TrackState::eqMidQ, &Track::eqMidQ, std::exp2(v), 0.3f, 6.0f); });
    BlockEnd(true);

    BlockBegin("HIGH SHELF");
    gainCell("hg", mt.eqHighDb, &Mixer::TrackState::eqHighDb, &Track::eqHighDb);
    ImGui::SameLine(0, 6);
    freqCell("hf", "FREQ", mt.eqHighHz, 1500.0f, 16000.0f, &Mixer::TrackState::eqHighHz, &Track::eqHighHz, false);
    BlockEnd(false);
    ImGui::NewLine();
    if (ImGui::Button("FLAT##eqflat")) {
        app.PushUndo();
        setF(&Mixer::TrackState::eqHpfHz, &Track::eqHpfHz, 20.0f, 20.0f, 500.0f);
        setF(&Mixer::TrackState::eqLowDb, &Track::eqLowDb, 0.0f, -kEqMaxDb, kEqMaxDb);
        setF(&Mixer::TrackState::eqMidDb, &Track::eqMidDb, 0.0f, -kEqMaxDb, kEqMaxDb);
        setF(&Mixer::TrackState::eqHighDb, &Track::eqHighDb, 0.0f, -kEqMaxDb, kEqMaxDb);
    }
    if (ImGui::IsItemHovered()) Hint("Reset every band of this track's EQ (frequencies stay where they are)");
    ImGui::SameLine(0, 10);
    ImGui::TextDisabled("%s", ti >= kNumDrumTracks ? "Tip: a bass usually wants a 24 dB low cut at 30-40 Hz and a small dip around 200-300 Hz." : "Tip: cut the lows of hats / claps / perc (100-300 Hz) so only the kick and the bass own the bottom.");
}

// ===========================================================================
// DRUM
// ===========================================================================

// The left column of a drum track, built like the synth's PRESET block (same width): the KIT row (the arrows load the
// neighbour kit, the name opens the kit browser), then the SAMPLE row every instrument has (stage C4b): < > = the neighbour
// variant of this drum in the kit (kick, kick_2 ...), the name = the list of them, HEAR plays it; under it the waveform.
void ContextPanel::DrawKitBlock(AppState& app, int ti, float width) {
    Pattern& pat = app.CurrentPattern();
    Track& t = pat.tracks[ti];
    const int scene = app.project.currentPattern;
    {   // the block is named after the drum (KICK, SNARE ...): KIT and SAMPLE are the captions of its two rows
        std::string title = TrackDisplayName(t);
        for (char& c : title) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        BlockBegin(title.c_str());
    }
    constexpr float capW = 50.0f;
    const auto& kits = app.KitFolders();
    if (kits.empty()) {
        ImGui::TextDisabled("(no kit folders)");
    } else {
        const int n = static_cast<int>(kits.size());
        int cur = std::clamp(app.menuKitIndex, 0, n - 1);
        for (int i = 0; i < n; ++i) if (kits[static_cast<size_t>(i)] == app.TrackKitFolder(scene, ti)) cur = i;
        std::string name = kits[static_cast<size_t>(cur)];
        if (name.rfind("kits/", 0) == 0) name = name.substr(5);
        SampleRowSpec ks;
        ks.id = "##kitRow"; ks.caption = "KIT"; ks.captionW = capW; ks.width = width; ks.label = name; ks.dot = vis::kAccent;
        ks.canStep = n > 1; ks.showHear = false;
        ks.nameHint = "The kit: all six drums at once (like a synth's PRESET). Click = the kit browser (categories, search, favorites).";
        ks.stepHint = "< > = the neighbour kit, at once and silently (try kits while it plays)";
        const SampleRowHit kh = SampleRow(ks);
        if (kh.step != 0) app.LoadKitFolder(kits[static_cast<size_t>(((cur + kh.step) % n + n) % n)]);
        if (kh.open) app.kitBrowserRequest = true;
    }

    // SAMPLE row: the default sample of this track (steps set to AUTO follow it)
    const DrumSlot slot = static_cast<DrumSlot>(t.slotIndex);
    const int nVar = std::max(1, app.SceneKitVariantCount(scene, static_cast<int>(slot)));
    int cur = -1; bool mixed = false;
    for (const Step& st : t.steps) {
        if (!st.active) continue;
        const int ev = StepVariant(t, st);
        if (cur < 0) cur = ev; else if (cur != ev) mixed = true;
    }
    if (cur < 0) cur = t.defaultVariant;
    cur = std::clamp(cur, 0, nVar - 1);
    auto pickVariant = [&](int nv) {
        app.PushUndo();
        for (Step& st : t.steps) st.variant = kAutoVariant;   // every step follows the track default again
        t.defaultVariant = static_cast<uint8_t>(nv);
        cur = nv; mixed = false;
        app.SetStatus(TrackDisplayName(t) + " SAMPLE " + std::to_string(nv + 1) + " / " + std::to_string(nVar) + ": " + DrumVariantLabel(app, ti, nv, nVar));
    };
    const std::shared_ptr<const SampleBuffer> buf = app.SceneKitVariantBuffer(scene, static_cast<int>(slot), cur);
    ImGui::Dummy(ImVec2(1, 2));
    SampleRowSpec ss;
    ss.id = "##drumSmpRow"; ss.caption = "SAMPLE"; ss.captionW = capW; ss.width = width;
    const bool kitLoading = !buf && app.KitPending(app.TrackKitFolder(scene, ti));   // N1: the kit is read on the loader thread
    ss.label = kitLoading ? std::string("(loading...)")
             : !buf ? std::string(slot == DrumSlot::Kick ? "(synth kick)" : "(none in this kit)")
             : mixed ? std::string("MIXED (steps choose)") : DrumVariantLabel(app, ti, cur, nVar);
    ss.dot = TrackColor(ti);
    ss.canStep = nVar > 1; ss.canHear = buf != nullptr;
    ss.nameHint = mixed ? "Some steps play their own sample (right-click on a step). < > or a pick from the list makes every step follow this track again."
                        : "The sample of this drum (steps set to AUTO follow it). Click = the SAMPLES browser: this kit, your library, the other kits.";
    ss.stepHint = "< > = the neighbour sample of this drum in the kit (kick, kick_2 ...), at once and silently";
    ss.hearHint = "Play this drum once, as it sounds here (TUNE, DECAY, START / LENGTH)";
    ss.noHearHint = slot == DrumSlot::Kick ? "This kit has no kick sample: the kick is the built-in KICK SYNTH." : "This kit has no sample for this drum.";
    const SampleRowHit sh = SampleRow(ss);
    if (sh.step != 0 && nVar > 1) pickVariant(((cur + sh.step) % nVar + nVar) % nVar);
    if (sh.hear && buf) HearDrum(app, t, buf);
    if (sh.open) app.OpenLibrary(AppState::LibTarget::Drum);   // C4b: the SAMPLES browser, on the samples of this drum in its kit
    {   // v44.52: the waveform of that sample, with the START / LENGTH of the SAMPLE knobs (the same card the sampler has)
        ImGui::Dummy(ImVec2(1, 3));
        WaveGesture g; g.lines = buf != nullptr;
        DrawWaveCard(app, "##drumWave", buf ? buf->data.data() : nullptr, buf ? buf->frames() : 0, buf ? buf->channels : 1, buf ? buf->sampleRate : kSampleRate, t.edit,
                     width, 40.0f, TrackColor(ti), 0, slot == DrumSlot::Kick ? "NO SAMPLE (SYNTH KICK)" : "NO SAMPLE IN THIS KIT", nullptr, nullptr, 0, &g);
        if (g.begin) app.PushUndo();   // C4b (d): one Undo per drag of START / the end
        if (g.edit) { t.edit.start = g.start; t.edit.length = g.length; }
    }
    BlockEnd();
}

// C4b: the file name of variant `v` of a drum in the kit of the edited scene (kick.wav, kick_2.wav ...). The names are filled
// by DrumKit::Build on the loader thread, so nothing here touches the disk (this runs under the scene lock).
std::string ContextPanel::DrumVariantLabel(AppState& app, int ti, int v, int nVar) {
    const int slot = std::clamp(ti, 0, kNumDrumTracks - 1);
    const DrumKit::BankPtr b = app.KitBank(app.TrackKitFolder(app.project.currentPattern, slot));
    if (b && v >= 0 && v < static_cast<int>(b->names[static_cast<size_t>(slot)].size())) return b->names[static_cast<size_t>(slot)][static_cast<size_t>(v)];
    return "SAMPLE " + std::to_string(v + 1) + " / " + std::to_string(nVar);
}

// C4b: HEAR on a drum. When the engine holds this very sample (the edited scene's kit is the sounding one), the hit goes through
// the drum voice, so TUNE, DECAY and START / LENGTH are heard; otherwise the sample is played as it is, quietly (like the library).
void ContextPanel::HearDrum(AppState& app, const Track& t, const std::shared_ptr<const SampleBuffer>& buf) {
    const int slot = std::clamp(t.slotIndex, 0, kNumDrumTracks - 1);
    const DrumKit::BankPtr bank = app.engine.GetKit()->CurrentBank();
    bool sounding = false;
    if (bank) for (const auto& b : bank->variants[static_cast<size_t>(slot)]) if (b == buf) sounding = true;
    if (sounding) {
        int vi = 0;
        for (size_t k = 0; k < bank->variants[static_cast<size_t>(slot)].size(); ++k) if (bank->variants[static_cast<size_t>(slot)][k] == buf) vi = static_cast<int>(k);
        DrumVoiceParams p;
        p.tune = t.drumTune; p.decay = t.drumDecay; p.edit = t.edit; p.level = 0.9f;
        p.keyRoot = app.project.randomizerKeyRoot;
        app.engine.TriggerDrum(static_cast<DrumSlot>(slot), 0.9f, vi, p);
        return;
    }
    auto sd = std::make_shared<SampleData>();
    sd->data = buf->data; sd->channels = std::clamp(buf->channels, 1, 2); sd->sampleRate = buf->sampleRate; sd->name = DrumSlotFileStem(static_cast<DrumSlot>(slot));
    SamplePtr smp = sd;
    hearKeep_.push_back(smp);   // the audio thread may still hold the one that plays: never let it drop the last reference there
    while (hearKeep_.size() > 16) hearKeep_.erase(hearKeep_.begin());
    app.engine.AuditionSample(smp, AppState::kAuditionGain);
}

void ContextPanel::DrawDrum(AppState& app, int ti) {
    Pattern& pat = app.CurrentPattern();
    Track& t = pat.tracks[ti];
    (void)t;
    auto& mt = app.EditMixer().tracks[ti];
    char buf[24];

    // ---- left: KIT + SAMPLE, then OUTPUT (the same two blocks a synth has: PRESET + OUTPUT) ----
    cellSize_.x = 62.0f;
    DrawKitBlock(app, ti, 262.0f);
    BlockBegin("OUTPUT");
    std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(mt.volume * 100 + 0.5f));
    FloatCell(app, "tvol", "VOLUME", mt.volume, 0.0f, 1.0f, 1.0f / 64.0f, buf,
              [&](float v) { SetTrackFloatField(app, ti, &Mixer::TrackState::volume, &Track::volume, v, 0.0f, 1.0f); });
    ImGui::SameLine(0, 6);
    if (std::fabs(mt.pan) < 0.015f) std::snprintf(buf, sizeof(buf), "C");
    else std::snprintf(buf, sizeof(buf), "%c%d", mt.pan < 0 ? 'L' : 'R', static_cast<int>(std::fabs(mt.pan) * 100 + 0.5f));
    FloatCell(app, "tpan", "PAN", mt.pan, -1.0f, 1.0f, 1.0f / 32.0f, buf,
              [&](float v) { SetTrackFloatField(app, ti, &Mixer::TrackState::pan, &Track::pan, std::fabs(v) < 0.02f ? 0.0f : v, -1.0f, 1.0f); });
    BlockEnd(true);

    // ---- right: tabs (same place, same size as the synth's) ----
    static const char* kTabs[4] = {"VOICE", "EQ", "FX", "AUTO"};
    const float availW = ImGui::GetContentRegionAvail().x;
    ImGui::BeginGroup();
    for (int tb = 0; tb < 4; ++tb) {
        if (tb > 0) ImGui::SameLine(0, 4);
        if (ToggleButton(kTabs[tb], drumTab_ == tb, ImVec2(78.0f, 0), ToggleTab())) { drumTab_ = tb; selCell_ = 0; }
    }
    ImGui::Dummy(ImVec2(1, 2));
    if (drumTab_ == 0) DrawDrumVoice(app, ti);
    else if (drumTab_ == 1) DrawEq(app, ti, availW);
    else if (drumTab_ == 3) DrawAuto(app, ti, availW);
    else { cellSize_.x = 68.0f; DrawCommonRow(app, ti); ImGui::NewLine(); }
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(1, 4));
}

void ContextPanel::DrawDrumVoice(AppState& app, int ti) {
    Pattern& pat = app.CurrentPattern();
    Track& t = pat.tracks[ti];
    char buf[24];
    if (t.slotIndex == static_cast<int>(DrumSlot::Kick)) {   // v44.52: VOICE + SAMPLE + TUNE / LEVEL + KICK SYNTH in one row: the cells give a little width
        const float avail = ImGui::GetContentRegionAvail().x;
        cellSize_.x = std::clamp((avail - 4.0f * 27.0f - 6.0f * 11.0f) / 15.0f, 48.0f, 62.0f);
    }

    BlockBegin("VOICE");
    std::snprintf(buf, sizeof(buf), "%+.1fST", t.drumTune);
    FloatCell(app, "tune", "TUNE", t.drumTune, -24.0f, 24.0f, 0.5f, buf, [&](float v) { t.drumTune = v; });
    ImGui::SameLine(0, 6);
    std::snprintf(buf, sizeof(buf), "%.2fX", t.drumDecay);
    FloatCell(app, "decay", "DECAY", t.drumDecay, 0.25f, 2.0f, 0.05f, buf, [&](float v) { t.drumDecay = v; });
    BlockEnd(true);

    DrawSampleEditBlocks(app, t.edit, [] {}, true, t.slotIndex == static_cast<int>(DrumSlot::Kick));   // v44.52: the same SAMPLE knobs as the sampler
    if (t.slotIndex == static_cast<int>(DrumSlot::Kick)) {
        // built-in kick synth (v44.5.4: of the edited scene): sine + pitch sweep + click, tuned to the project key
        KickSynthSettings& ks = app.EditedKick();
        auto push = [&] { app.PushEditedKick(); };
        auto setF = [&](float& field, float v) { field = v; push(); };
        static const char* kModes[3] = {"SAMPLE", "SYNTH", "LAYER"};
        BlockBegin("KICK SYNTH");
        int ms = DiscreteCell("kmode", "SOURCE", kModes[std::clamp(ks.mode, 0, 2)]);
        if (ms != 0) { app.PushUndo(); ks.mode = ((ks.mode + (ms > 0 ? 1 : -1)) % 3 + 3) % 3; push(); app.SetStatus(std::string("KICK SOURCE ") + kModes[ks.mode]); }
        const bool off = ks.mode == 0;
        ImGui::SameLine(0, 6);
        int kl = DiscreteCell("klock", "KEY LOCK", ks.keyLock ? "ON" : "OFF", off);
        if (kl != 0 && !off) { app.PushUndo(); ks.keyLock = !ks.keyLock; push(); }
        ImGui::SameLine(0, 6);
        if (ks.keyLock) std::snprintf(buf, sizeof(buf), "%s %.0fHZ", kKeyNames[std::clamp(app.project.randomizerKeyRoot, 0, kNumKeys - 1)], KickKeyFrequency(app.project.randomizerKeyRoot));
        else std::snprintf(buf, sizeof(buf), "%.0fHZ", ks.pitchHz);
        FloatCell(app, "kpitch", "PITCH", ks.pitchHz, 30.0f, 100.0f, 1.0f, buf, [&](float v) { setF(ks.pitchHz, v); }, off || ks.keyLock);
        ImGui::SameLine(0, 6);
        std::snprintf(buf, sizeof(buf), "%.1fX", ks.sweep);
        FloatCell(app, "ksweep", "SWEEP", ks.sweep, 1.0f, 10.0f, 0.1f, buf, [&](float v) { setF(ks.sweep, v); }, off);
        ImGui::SameLine(0, 6);
        std::snprintf(buf, sizeof(buf), "%.0fMS", ks.sweepMs);
        FloatCell(app, "ksweept", "SWEEP TIME", ks.sweepMs, 5.0f, 150.0f, 1.0f, buf, [&](float v) { setF(ks.sweepMs, v); }, off);
        ImGui::SameLine(0, 6);
        std::snprintf(buf, sizeof(buf), "%.0fMS", ks.decayMs);
        FloatCell(app, "kdecay", "LENGTH", ks.decayMs, 80.0f, 1200.0f, 10.0f, buf, [&](float v) { setF(ks.decayMs, v); }, off);
        ImGui::SameLine(0, 6);
        std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(ks.click * 100 + 0.5f));
        FloatCell(app, "kclick", "CLICK", ks.click, 0.0f, 1.0f, 1.0f / 32.0f, buf, [&](float v) { setF(ks.click, v); }, off);
        ImGui::SameLine(0, 6);
        std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(ks.drive * 100 + 0.5f));
        FloatCell(app, "kdrive", "DRIVE", ks.drive, 0.0f, 1.0f, 1.0f / 32.0f, buf, [&](float v) { setF(ks.drive, v); }, off);
        ImGui::SameLine(0, 6);
        std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(ks.level * 100 + 0.5f));
        FloatCell(app, "klevel", "LEVEL", ks.level, 0.0f, 1.0f, 1.0f / 32.0f, buf, [&](float v) { setF(ks.level, v); }, off);
        BlockEnd(false);
    }
}


// ===========================================================================
// SAMPLER (v44.50)
// ===========================================================================

// The card on the left: the sample's name (click = the list of samples/), HEAR, the waveform with the START and END markers.
void ContextPanel::DrawSampleBlock(AppState& app, int ti, float width) {
    const int tgt = AppState::SampleTargetOfTrack(ti);   // v44.53: the sampler track, or the SAMPLE engine of a synth / the chord track
    if (tgt < -1) return;
    const SamplerPatch sp = app.GetSampleTarget(tgt);
    const bool synthTgt = tgt >= 0;
    BlockBegin("SAMPLE");
    const auto& names = app.SampleNames();
    const std::string label = sp.file.empty() ? std::string("(choose a sample)") : sp.file;
    int cur = -1;
    for (size_t i = 0; i < names.size(); ++i) if (names[i] == sp.file) cur = static_cast<int>(i);
    const int n = static_cast<int>(names.size());
    const SamplePtr smp = app.SampleFor(sp.file);
    // C4b: the same SAMPLE row as the drums and the transitions: [<] [name v] [>] [HEAR]
    SampleRowSpec ss;
    ss.id = "##smpRow"; ss.width = width; ss.label = label;
    ss.dot = app.SampleMissing(sp.file) ? IM_COL32(230, 90, 80, 255) : TrackColor(ti);
    ss.canStep = n >= 2; ss.canHear = smp != nullptr;
    std::string nameHint;
    if (app.SampleMissing(sp.file)) nameHint = sp.file + " is not in the samples folder next to the project any more. Put it back there, or pick another sample.";
    else nameHint = synthTgt ? "The sample this sound plays: a note plays it at its pitch (ROOT = unchanged). ENV + LFO, FILTER, EQ, FX and AUTO shape it like any synth sound. Click = the SAMPLES browser (this project, your library, Finder)."
                             : "The sample this scene plays. Click = the SAMPLES browser (this project, your library, Finder). Drop a .wav or .aif on the window to add one.";
    ss.nameHint = nameHint.c_str();
    ss.stepHint = "< > = the neighbour sample of this project (samples/ beside it), at once and silently";
    ss.hearHint = synthTgt ? "Play the note ROOT through this synth (its envelope, filter and LFO), once." : sp.mode == 2 ? "Play the loop once at the project tempo." : "Play the sample once at its ROOT note.";
    const SampleRowHit sh = SampleRow(ss);
    if (sh.step != 0 && n > 0) {
        const int base = cur >= 0 ? cur : (sh.step > 0 ? -1 : 0);   // no current one: > takes the first, < the last
        app.SetSamplerFile(names[static_cast<size_t>(((base + sh.step) % n + n) % n)], tgt);
    }
    if (sh.hear && smp) {
        const bool live = app.sequencer.IsPlaying() && app.EditIsPlaying();   // the engine holds the sound of the PLAYED scene; the edited one may not be playing
        if (!synthTgt) {
            if (!live) app.engine.SamplerSet(smp, sp);
            app.engine.SamplerSetTempo(app.CurrentPattern().bpm);
            if (sp.mode == 2) app.engine.SamplerLoopPreview();   // a LOOP is heard once, at the project tempo
            else app.engine.SamplerNoteOn(sp.root, 0.9f);
        } else {   // v44.53: the note ROOT through the synth (envelope, filter, LFO ...), released after what the sample lasts
            const bool chord = tgt >= kNumSynthTracks;
            if (!live) { app.engine.SynthSampleSet(tgt, smp); app.engine.SynthPatchSet(tgt, app.EditedPatch(chord, tgt)); }
            app.engine.SamplerSetTempo(app.CurrentPattern().bpm);
            SamplerVoice probe; probe.SetSample(smp); probe.SetPatch(sp);
            double ra, rb; probe.Region(ra, rb);
            const double rate = std::max(0.05, SamplerVoice::FitRate(*smp, sp, app.CurrentPattern().bpm, ra, rb));
            const double secs = (rb - ra) / std::max(1, smp->sampleRate) / rate;
            if (hearOffAt_ > 0.0) StopHear(app);
            const int8_t note = sp.root;
            if (chord) app.engine.ChordNoteOn(&note, 1, 0.9f, false); else app.engine.SynthNoteOn(tgt, sp.root, 0.9f, false);
            hearOffAt_ = ImGui::GetTime() + std::clamp(sp.mode == 1 ? secs / std::max(1, static_cast<int>(sp.slices ? sp.slices : std::max<int>(1, sp.markCount))) : secs, 0.2, 8.0);
            hearTgt_ = tgt;
        }
    }
    if (sh.open) app.OpenLibrary(AppState::LibTarget::Sampler, -1, tgt);   // C4b: the SAMPLES browser

    // waveform card (v44.52: the same card the drum tracks have)
    ImGui::Dummy(ImVec2(1, 3));
    {
        SampleEdit ed; ed.start = sp.start; ed.length = sp.decay;
        bool clicked = false;
        const bool marksMode = sp.mode == 1 && sp.slices == 0 && smp;   // v44.52.1: on HITS slices a click on the waveform edits the marks
        const char* msg = sp.file.empty() ? "CLICK TO CHOOSE A SAMPLE" : "FILE NOT FOUND - CLICK TO CHOOSE";
        WaveGesture g; g.lines = smp != nullptr; g.marks = marksMode;
        DrawWaveCard(app, "##smpWave", smp ? smp->data.data() : nullptr, smp ? smp->frames() : 0, smp ? smp->channels : 1, smp ? smp->sampleRate : kSampleRate, ed,
                     width, synthTgt ? 40.0f : std::max(40.0f, cellSize_.y - 8.0f), TrackColor(ti), sp.mode == 1 ? sp.slices : 0, msg, &clicked,
                     marksMode ? sp.marks.data() : nullptr, marksMode ? sp.markCount : 0, &g);
        if (clicked) app.OpenLibrary(AppState::LibTarget::Sampler, -1, tgt);   // C4b: the same browser as the name (an empty card)
        if (g.begin) app.PushUndo();   // one Undo per drag
        if (g.edit) { SamplerPatch np = app.GetSampleTarget(tgt); np.start = g.start; np.decay = g.length; app.PutSampleTarget(tgt, np); }
        if (g.moveMark >= 0) waveDragMark_ = app.MoveSliceMark(g.moveMark, g.moveFrac, tgt);   // the mark may pass a neighbour: follow its new index
        if (g.addMark) app.AddSliceMark(g.frac, tgt);
        if (g.delMark) app.RemoveNearestSliceMark(g.frac, 10.0f / std::max(1.0f, width - 12.0f), tgt);
        if (g.menu) { waveMenuFrac_ = g.frac; waveMenuTgt_ = tgt; ImGui::OpenPopup("##waveMarks"); }
        if (ImGui::BeginPopup("##waveMarks")) {
            const float tol = 10.0f / std::max(1.0f, width - 12.0f);
            const SamplerPatch cp = app.GetSampleTarget(waveMenuTgt_);
            bool near = false; for (int i = 0; i < cp.markCount; ++i) if (std::fabs(cp.marks[static_cast<size_t>(i)] / 65535.0f - waveMenuFrac_) <= tol) near = true;
            if (ImGui::MenuItem("ADD A MARK HERE", nullptr, false, !near && cp.markCount < 32)) app.AddSliceMark(waveMenuFrac_, waveMenuTgt_);
            if (ImGui::MenuItem("DELETE THIS MARK", nullptr, false, near)) app.RemoveNearestSliceMark(waveMenuFrac_, tol, waveMenuTgt_);
            if (ImGui::MenuItem("CLEAR ALL MARKS", nullptr, false, cp.markCount > 0)) app.ClearSliceMarks(waveMenuTgt_);
            ImGui::EndPopup();
        }
    }
    BlockEnd();
}

// v44.52: the waveform card shared by the sampler and the drum tracks: the part that plays (START .. LENGTH) is lit, SLICES are
// marked, the white line is START and the coloured line the end. `data` null = nothing to show (emptyMsg is written instead).
void ContextPanel::DrawWaveCard(AppState& app, const char* id, const float* data, int frames, int channels, int sampleRate, const SampleEdit& ed,
                                float width, float bh, ImU32 tc, int slices, const char* emptyMsg, bool* clicked, const uint16_t* marks, int markCount, WaveGesture* gest) {
    (void)app;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(width, bh), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hov = ImGui::IsItemHovered();
    const bool hasWave = data && frames >= 2;
    // C4b (d): with a sample the card is edited by hand; only an empty card opens the browser (the name always does)
    if (clicked && !hasWave && ImGui::IsItemClicked(ImGuiMouseButton_Left)) *clicked = true;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + width, p0.y + bh);
    dl->AddRectFilled(p0, p1, hov && clicked ? vis::kCellHover : vis::kCell, 5.0f);
    dl->AddRect(p0, p1, ColorAlpha(tc, 160), 5.0f);
    if (!data || frames < 2) {
        const char* msg = emptyMsg ? emptyMsg : "NO SAMPLE";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f), hov ? vis::kText : vis::kTextDim, msg);
        return;
    }
    const int ch = std::max(1, channels);
    const int cols = std::max(8, static_cast<int>(width) - 12);
    if (wavePtr_ != data || waveFrames_ != frames || static_cast<int>(wavePeaks_.size()) != cols * 2) {   // once per sample (or per card width), not per frame
        wavePtr_ = data; waveFrames_ = frames;
        wavePeaks_.assign(static_cast<size_t>(cols) * 2, 0.0f);
        for (int c = 0; c < cols; ++c) {
            const int f0 = static_cast<int>(static_cast<int64_t>(frames) * c / cols), f1 = std::max(f0 + 1, static_cast<int>(static_cast<int64_t>(frames) * (c + 1) / cols));
            float lo = 0.0f, hi = 0.0f;
            for (int f = f0; f < f1 && f < frames; ++f) {
                float v = 0.0f;
                for (int k = 0; k < ch; ++k) v += data[static_cast<size_t>(f) * static_cast<size_t>(ch) + static_cast<size_t>(k)];
                v /= static_cast<float>(ch);
                lo = std::min(lo, v); hi = std::max(hi, v);
            }
            wavePeaks_[static_cast<size_t>(c) * 2] = lo; wavePeaks_[static_cast<size_t>(c) * 2 + 1] = hi;
        }
    }
    const float x0 = p0.x + 6.0f, mid = (p0.y + p1.y) * 0.5f, amp = bh * 0.5f - 6.0f;
    const float st = std::clamp(ed.start, 0.0f, 0.95f);
    const float sx = x0 + st * (width - 12.0f);
    const float ex = x0 + std::min(1.0f, st + (1.0f - st) * std::clamp(ed.length, 0.02f, 1.0f)) * (width - 12.0f);
    dl->AddRectFilled(ImVec2(sx, p0.y + 2.0f), ImVec2(ex, p1.y - 2.0f), ColorAlpha(tc, 30));   // the part that plays
    for (int c = 0; c < cols; ++c) {
        const float x = x0 + static_cast<float>(c);
        const bool in = x >= sx && x <= ex;
        dl->AddLine(ImVec2(x, mid - wavePeaks_[static_cast<size_t>(c) * 2 + 1] * amp), ImVec2(x, mid - wavePeaks_[static_cast<size_t>(c) * 2] * amp - 1.0f), in ? tc : ColorAlpha(tc, 70));
    }
    if (slices > 1) {   // equal slices of the played part, numbered
        for (int i = 1; i < slices; ++i) {
            const float x = sx + (ex - sx) * static_cast<float>(i) / static_cast<float>(slices);
            dl->AddLine(ImVec2(x, p0.y + 3.0f), ImVec2(x, p1.y - 3.0f), IM_COL32(255, 255, 255, 90), 1.0f);
        }
        if (slices <= 16) for (int i = 0; i < slices; ++i) {
            char nb[8]; std::snprintf(nb, sizeof(nb), "%d", i + 1);
            const float x = sx + (ex - sx) * (static_cast<float>(i) + 0.5f) / static_cast<float>(slices);
            dl->AddText(ImVec2(x - ImGui::CalcTextSize(nb).x * 0.5f, p1.y - ImGui::GetTextLineHeight() - 2.0f), vis::kTextDim, nb);
        }
    }
    if (marks && markCount > 0) {   // v44.52.1: the marks of HITS slices (those outside the played part are dim)
        for (int i = 0; i < markCount; ++i) {
            const float x = x0 + marks[i] / 65535.0f * (width - 12.0f);
            const bool in = x >= sx - 0.5f && x < ex;
            dl->AddLine(ImVec2(x, p0.y + 3.0f), ImVec2(x, p1.y - 3.0f), in ? IM_COL32(255, 255, 255, 170) : IM_COL32(255, 255, 255, 50), 1.2f);
            if (in && markCount <= 24) { char nb[8]; std::snprintf(nb, sizeof(nb), "%d", i + 1); dl->AddText(ImVec2(x + 3.0f, p1.y - ImGui::GetTextLineHeight() - 2.0f), vis::kTextDim, nb); }
        }
    }
    // ---- C4b (d): gestures (the caller applies them) ----
    const float span = std::max(1.0f, width - 12.0f);
    const float mx = ImGui::GetIO().MousePos.x;
    const float mfrac = std::clamp((mx - x0) / span, 0.0f, 1.0f);
    auto nearMark = [&](float x) { int best = -1; float bd = 6.0f; for (int i = 0; i < markCount; ++i) { const float d = std::fabs(x0 + marks[i] / 65535.0f * span - x); if (d <= bd) { bd = d; best = i; } } return best; };
    int hoverWhat = 0;   // 1 START, 2 end, 3 a mark
    if (gest && hov && waveDrag_ == 0) {
        if (gest->lines && std::fabs(mx - sx) <= 5.0f) hoverWhat = 1;
        else if (gest->lines && std::fabs(mx - ex) <= 5.0f) hoverWhat = 2;
        else if (gest->marks && marks && nearMark(mx) >= 0) hoverWhat = 3;
    }
    if (gest) {
        if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            waveDragMoved_ = false;
            waveDrag_ = hoverWhat ? hoverWhat : 4;
            waveDragMark_ = hoverWhat == 3 ? nearMark(mx) : -1;
        }
        if (ImGui::IsItemActive() && waveDrag_ != 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
            if (!waveDragMoved_ && waveDrag_ != 4) gest->begin = true;   // Undo only when something really moves
            waveDragMoved_ = true;
            const float endAbs = std::min(1.0f, st + (1.0f - st) * std::clamp(ed.length, 0.02f, 1.0f));
            if (waveDrag_ == 1) {   // START moves, the end stays where it is
                const float ns = std::clamp(mfrac, 0.0f, std::min(0.95f, endAbs - 0.01f));
                gest->edit = true; gest->start = ns; gest->length = std::clamp((endAbs - ns) / std::max(0.05f, 1.0f - ns), 0.02f, 1.0f);
            } else if (waveDrag_ == 2) {
                gest->edit = true; gest->start = ed.start; gest->length = std::clamp((mfrac - st) / std::max(0.05f, 1.0f - st), 0.02f, 1.0f);
            } else if (waveDrag_ == 3 && waveDragMark_ >= 0 && waveDragMark_ < markCount) {
                gest->moveMark = waveDragMark_; gest->moveFrac = mfrac;
            }
        }
        if (ImGui::IsItemDeactivated() && waveDrag_ != 0) {
            if (waveDrag_ == 4 && !waveDragMoved_ && gest->marks) { gest->addMark = true; gest->frac = mfrac; }   // a click on nothing
            waveDrag_ = 0; waveDragMark_ = -1;
        }
        if (gest->marks && hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && nearMark(mx) >= 0) { gest->delMark = true; gest->addMark = false; gest->frac = mfrac; }
        if (gest->marks && hov && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) { gest->menu = true; gest->frac = mfrac; }
        const int shown = waveDrag_ == 1 || waveDrag_ == 2 || waveDrag_ == 3 ? waveDrag_ : hoverWhat;
        if (shown) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    const bool hotStart = gest && (waveDrag_ == 1 || hoverWhat == 1), hotEnd = gest && (waveDrag_ == 2 || hoverWhat == 2);
    dl->AddLine(ImVec2(sx, p0.y + 3.0f), ImVec2(sx, p1.y - 3.0f), IM_COL32(255, 255, 255, 200), hotStart ? 3.0f : 1.5f);
    dl->AddLine(ImVec2(ex, p0.y + 3.0f), ImVec2(ex, p1.y - 3.0f), ColorAlpha(tc, 255), hotEnd ? 3.0f : 1.5f);
    const double secs = static_cast<double>(frames) / std::max(1, sampleRate);
    char info[48]; std::snprintf(info, sizeof(info), "%.2f S  %s%s", secs, ch == 2 ? "STEREO" : "MONO", ed.reverse ? "  REV" : "");
    dl->AddText(ImVec2(p1.x - ImGui::CalcTextSize(info).x - 8.0f, p0.y + 3.0f), vis::kTextDim, info);
    if (hov && waveDrag_ == 0) {
        if (gest && gest->marks) Hint("HITS slices: click = add a mark, drag a mark = move it, double-click a mark = delete it, right-click = menu.\nDrag the white line = START, the coloured line = the end. Each mark starts a slice; slice 1 is played by ROOT, slice 2 by ROOT + 1 ...");
        else if (gest && gest->lines) Hint("Drag the white line = START, the coloured line = the end (LENGTH): the same as the START and LENGTH knobs. Numbers mark SLICES.");
        else Hint("The sample of this track. The white line is START, the coloured line the end set by LENGTH. Numbers mark SLICES.");
    }
}

// v44.52: the knobs that edit a sample, identical on every instrument that plays one (drum tracks and the sampler).
void ContextPanel::DrawSampleEditBlocks(AppState& app, SampleEdit& ed, const std::function<void()>& onChange, bool drum, bool lastSeparator, bool hideAttack) {
    char buf[24];
    (void)drum;
    BlockBegin("SAMPLE");
    std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(ed.start * 100 + 0.5f));
    FloatCell(app, "sstart", "START", ed.start, 0.0f, 0.95f, 0.01f, buf, [&](float v) { ed.start = v; onChange(); });
    if (ImGui::IsItemHovered()) Hint("Where in the sample the sound starts. Cut a silence or a pre-hit off the front.");
    ImGui::SameLine(0, 6);
    std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(ed.length * 100 + 0.5f));
    FloatCell(app, "sdecay", "LENGTH", ed.length, 0.02f, 1.0f, 0.02f, buf, [&](float v) { ed.length = v; onChange(); });
    if (ImGui::IsItemHovered()) Hint("How much of the sample, counted from START, plays before it fades out. 100% = to the end.");
    if (!hideAttack) {
        ImGui::SameLine(0, 6);
        std::snprintf(buf, sizeof(buf), "%.0fMS", ed.attackMs);
        FloatCell(app, "sattack", "ATTACK", ed.attackMs, 0.0f, 200.0f, 1.0f, buf, [&](float v) { ed.attackMs = v; onChange(); });
        if (ImGui::IsItemHovered()) Hint("A fade-in. 0 = the sample's own start (a tiny click guard on drums). Longer = a softer, swelling start.");
    }
    BlockEnd(true);

    BlockBegin("TUNE / LEVEL");
    std::snprintf(buf, sizeof(buf), "%+d CT", static_cast<int>(ed.fineCents));
    FloatCell(app, "stune", "FINE", static_cast<float>(ed.fineCents), -100.0f, 100.0f, 1.0f, buf, [&](float v) { ed.fineCents = static_cast<int8_t>(std::lround(v)); onChange(); });
    if (ImGui::IsItemHovered()) Hint("Fine tune in cents (100 = a semitone), on top of TUNE / ROOT.");
    ImGui::SameLine(0, 6);
    std::snprintf(buf, sizeof(buf), "%+d DB", static_cast<int>(ed.gainDb));
    FloatCell(app, "sgain", "GAIN", static_cast<float>(ed.gainDb), -24.0f, 12.0f, 1.0f, buf, [&](float v) { ed.gainDb = static_cast<int8_t>(std::lround(v)); onChange(); });
    if (ImGui::IsItemHovered()) Hint("Makes this sample quieter or louder before the track's VOLUME (a quiet recording, a hot one).");
    ImGui::SameLine(0, 6);
    int rv = DiscreteCell("srev", "DIRECT.", ed.reverse ? "REVERSE" : "FORWARD");
    if (rv != 0) { app.PushUndo(); ed.reverse = !ed.reverse; onChange(); }
    if (ImGui::IsItemHovered()) Hint("REVERSE plays the sample backwards.");
    BlockEnd(lastSeparator);
}

void ContextPanel::DrawSampler(AppState& app, int ti) {
    auto& mt = app.EditMixer().tracks[ti];
    char buf[24];
    cellSize_.x = 62.0f;
    DrawSampleBlock(app, ti, 262.0f);
    BlockBegin("OUTPUT");
    std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(mt.volume * 100 + 0.5f));
    FloatCell(app, "tvol", "VOLUME", mt.volume, 0.0f, 1.0f, 1.0f / 64.0f, buf,
              [&](float v) { SetTrackFloatField(app, ti, &Mixer::TrackState::volume, &Track::volume, v, 0.0f, 1.0f); });
    ImGui::SameLine(0, 6);
    if (std::fabs(mt.pan) < 0.015f) std::snprintf(buf, sizeof(buf), "C");
    else std::snprintf(buf, sizeof(buf), "%c%d", mt.pan < 0 ? 'L' : 'R', static_cast<int>(std::fabs(mt.pan) * 100 + 0.5f));
    FloatCell(app, "tpan", "PAN", mt.pan, -1.0f, 1.0f, 1.0f / 32.0f, buf,
              [&](float v) { SetTrackFloatField(app, ti, &Mixer::TrackState::pan, &Track::pan, std::fabs(v) < 0.02f ? 0.0f : v, -1.0f, 1.0f); });
    BlockEnd(true);

    static const char* kTabs[4] = {"VOICE", "EQ", "FX", "AUTO"};
    const float availW = ImGui::GetContentRegionAvail().x;
    ImGui::BeginGroup();
    for (int tb = 0; tb < 4; ++tb) {
        if (tb > 0) ImGui::SameLine(0, 4);
        if (ToggleButton(kTabs[tb], drumTab_ == tb, ImVec2(78.0f, 0), ToggleTab())) { drumTab_ = tb; selCell_ = 0; }
    }
    ImGui::Dummy(ImVec2(1, 2));
    if (drumTab_ == 0) DrawSamplerVoice(app, ti);
    else if (drumTab_ == 1) DrawEq(app, ti, availW);
    else if (drumTab_ == 3) DrawAuto(app, ti, availW);
    else { cellSize_.x = 68.0f; DrawCommonRow(app, ti); ImGui::NewLine(); }
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(1, 4));
}

void ContextPanel::DrawSamplerVoice(AppState& app, int ti) {
    const int tgt = AppState::SampleTargetOfTrack(ti);   // v44.53: the same editor for the sampler track and for the SAMPLE engine of a synth / the chord
    if (tgt < -1) return;
    const bool synthTgt = tgt >= 0;
    SamplerPatch sp = app.GetSampleTarget(tgt);   // a copy: edited here, written back by push()
    char buf[24];
    auto push = [&] { app.PutSampleTarget(tgt, sp); };

    BlockBegin("PLAY");
    static const char* kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static const char* kModes[3] = {"ONE-SHOT", "SLICES", "LOOP"};
    int md = DiscreteCell("smode", "MODE", kModes[std::clamp<int>(sp.mode, 0, 2)]);
    if (md != 0) {
        app.PushUndo();
        sp.mode = static_cast<uint8_t>(((sp.mode + (md > 0 ? 1 : -1)) % 3 + 3) % 3);
        if (sp.mode == 2 && sp.fitBars < 0) sp.fitBars = 0;   // a loop is fitted to the tempo from the start (AUTO bars)
        push();
        app.SetStatus(sp.mode == 0 ? "SAMPLER: ONE-SHOT - EVERY STEP PLAYS THE SAMPLE FROM START"
                      : sp.mode == 1 ? "SAMPLER: SLICES - NOTE = SLICE NUMBER (ROOT = THE FIRST)" : "SAMPLER: LOOP - PLAYS WITH THE SCENE AT THE PROJECT TEMPO, STEPS ARE NOT USED");
    }
    if (ImGui::IsItemHovered()) Hint("ONE-SHOT: each step plays the sample, pitched by its note.\nSLICES: the sample is cut into equal parts, the step's note picks the part (ROOT = the first).\nLOOP: the sample plays by itself with the scene, fitted to whole bars of the project tempo. Steps are not used.");
    ImGui::SameLine(0, 6);
    const int rootMidi = sp.root;
    std::snprintf(buf, sizeof(buf), "%s%d", kNoteNames[((rootMidi % 12) + 12) % 12], rootMidi / 12 - 1);
    FloatCell(app, "sroot", "ROOT", static_cast<float>(sp.root), 24.0f, 96.0f, 1.0f, buf, [&](float v) { sp.root = static_cast<int8_t>(std::lround(v)); push(); }, sp.mode == 2);
    if (ImGui::IsItemHovered()) Hint(sp.mode == 1 ? "The note of the first slice; the next notes up play the next slices."
                                                  : "The note that plays the sample at its own pitch. A step on another note plays it higher or lower, like a keyboard.");
    if (sp.mode == 1) {
        ImGui::SameLine(0, 6);
        static const int kSl[5] = {4, 8, 16, 32, 0};   // 0 = HITS: the parts run between marks found in the sample
        int cur = 1; for (int i = 0; i < 5; ++i) if (kSl[i] == sp.slices) cur = i;
        if (kSl[cur] == 0) std::snprintf(buf, sizeof(buf), "HITS %d", static_cast<int>(sp.markCount)); else std::snprintf(buf, sizeof(buf), "%d", kSl[cur]);
        int sd = DiscreteCell("sslices", "SLICES", buf);
        if (sd != 0) { app.PushUndo(); sp.slices = static_cast<uint8_t>(kSl[((cur + (sd > 0 ? 1 : -1)) % 5 + 5) % 5]); push(); if (sp.slices == 0 && sp.markCount == 0) { app.SetStatus(app.FindSliceHits(hitSens_, tgt)); sp = app.GetSampleTarget(tgt); } }
        if (ImGui::IsItemHovered()) Hint("Cuts the part that plays (START / LENGTH) into this many equal slices, or HITS: slices that start where the sample has a hit (found by FIND, corrected by hand on the waveform). Slice 1 is played by ROOT, slice 2 by ROOT + 1 ...");
        if (sp.slices == 0) {
            ImGui::SameLine(0, 6);
            int fd = DiscreteCell("sfind", "FIND", "> HITS", !app.SampleFor(sp.file));
            if (fd != 0) { app.SetStatus(app.FindSliceHits(hitSens_, tgt)); sp = app.GetSampleTarget(tgt); }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) Hint("Finds the hits of the sample and puts a mark on each. It replaces the marks you have now (Undo brings them back). Then fine-tune on the waveform: click = add a mark, right-click = remove the nearest one.");
            ImGui::SameLine(0, 6);
            std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(hitSens_ * 100 + 0.5f));
            FloatCell(app, "ssens", "SENS", hitSens_, 0.0f, 1.0f, 0.05f, buf, [&](float v) { hitSens_ = v; });
            if (ImGui::IsItemHovered()) Hint("How soft a hit may be and still get a mark. Higher finds more (also ghost notes), lower only the loud hits. Press FIND to search again.");
        }
    }
    if (sp.mode == 1) {
        ImGui::SameLine(0, 6);
        int lay = DiscreteCell("slay", "LAY OUT", "> STEPS", !app.SampleFor(sp.file) || tgt >= kNumSynthTracks);
        if (lay != 0) { app.SetStatus(app.LayoutSamplerSlices(tgt)); sp = app.GetSampleTarget(tgt); }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) Hint("Writes the slice notes on the steps so that the whole loop plays through in order at the project tempo (each slice lasts as many steps as it does at this tempo). The track gets the loop's length. Undo brings the old steps back.");
    }
    if (sp.mode != 0) {
        ImGui::SameLine(0, 6);
        static const int kBars[6] = {-1, 0, 1, 2, 4, 8};   // -1 = off (native speed), 0 = auto
        int cur = 0; for (int i = 0; i < 6; ++i) if (kBars[i] == sp.fitBars) cur = i;
        if (sp.fitBars > 8) cur = 5;
        int autoBars = 0; double fileBpm = 0.0, rate = 1.0;
        const SamplePtr fitSmp = app.SampleFor(sp.file);
        if (fitSmp && sp.fitBars == 0) { double ra, rb; SamplerVoice probe; probe.SetSample(fitSmp); probe.SetPatch(sp); probe.Region(ra, rb); rate = SamplerVoice::FitRate(*fitSmp, sp, app.CurrentPattern().bpm, ra, rb, &autoBars, &fileBpm); }
        if (sp.fitBars < 0) std::snprintf(buf, sizeof(buf), "OFF");
        else if (sp.fitBars == 0) { if (autoBars > 0) std::snprintf(buf, sizeof(buf), "AUTO %dB", autoBars); else std::snprintf(buf, sizeof(buf), "AUTO"); }
        else std::snprintf(buf, sizeof(buf), "%d BAR%s", sp.fitBars, sp.fitBars > 1 ? "S" : "");
        int bd = DiscreteCell("sbars", sp.mode == 2 ? "BARS" : "FIT TO", buf);
        if (bd != 0) { app.PushUndo(); sp.fitBars = static_cast<int8_t>(kBars[((cur + (bd > 0 ? 1 : -1)) % 6 + 6) % 6]); push(); }
        if (ImGui::IsItemHovered()) {
            std::string now;
            if (sp.fitBars == 0 && fileBpm > 0.0) { char t[160]; std::snprintf(t, sizeof(t), "\nNow: the file name says %.0f BPM, so it plays at %.0f BPM (speed x%.2f), %d bar%s.", fileBpm, static_cast<double>(app.CurrentPattern().bpm), rate, autoBars, autoBars > 1 ? "s" : ""); now = t; }
            else if (sp.fitBars == 0 && autoBars > 0) { char t[160]; std::snprintf(t, sizeof(t), "\nNow: AUTO guessed %d bar%s (speed x%.2f). If that is wrong, pick the number of bars yourself.", autoBars, autoBars > 1 ? "s" : "", rate); now = t; }
            Hint("How many bars the played part lasts at the project tempo. The sample is sped up or slowed down (its pitch follows: no time-stretch) to fit.\nAUTO reads a tempo from the file name (\"170 bpm\"), otherwise picks the nearest of 1 / 2 / 4 / 8 / 16 bars. OFF = the sample's own speed.\nSLICES: each slice lasts (bars x 4 x steps per beat / slices) steps - put the notes that far apart.%s", now.c_str());
        }
    }
    BlockEnd(true);

    SampleEdit ed;
    ed.start = sp.start; ed.length = sp.decay; ed.attackMs = sp.attackMs; ed.fineCents = sp.tuneCents; ed.gainDb = sp.gainDb; ed.reverse = sp.reverse;
    DrawSampleEditBlocks(app, ed, [&] {
        sp.start = ed.start; sp.decay = ed.length; sp.attackMs = ed.attackMs; sp.tuneCents = ed.fineCents; sp.gainDb = ed.gainDb; sp.reverse = ed.reverse;
        push();
    }, false, !synthTgt, synthTgt);   // a synth's attack is its own envelope (ENV + LFO tab)
    if (synthTgt) return;

    BlockBegin("VOICES");
    int pv = DiscreteCell("spoly", "VOICES", sp.poly ? "POLY" : "MONO", sp.mode != 0);
    if (pv != 0 && sp.mode == 0) { app.PushUndo(); sp.poly = !sp.poly; push(); app.SetStatus(sp.poly ? "SAMPLER: HITS RING OVER EACH OTHER (4 VOICES)" : "SAMPLER: A NEW HIT CUTS THE PREVIOUS ONE"); }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) Hint("MONO: a new hit cuts the previous one (a short, clean cut). POLY: up to four hits ring together. Only for ONE-SHOT.");
    BlockEnd(false);
}

// ===========================================================================
// SYNTH / CHORD SOUND
// ===========================================================================

void ContextPanel::ApplyPresetToTrack(AppState& app, int ti, int idx) {
    const int slot = IsChordTrack(ti) ? kNumSynthTracks : SynthSlotForTrack(ti);
    const int total = static_cast<int>(AllSynthPresets().size() + app.CustomSynthPresetNamesCached().size());
    if (idx < 0 || idx >= total) return;
    bool oldChord = app.synthViewChordSlot;
    int oldSynth = app.prSynthIndex;
    app.synthViewChordSlot = IsChordTrack(ti);
    if (!IsChordTrack(ti)) app.prSynthIndex = SynthSlotForTrack(ti);
    app.ApplySynthPreset(idx);
    app.synthViewChordSlot = oldChord;
    app.prSynthIndex = oldSynth;
    presetIdx_[slot] = idx;
}

void ContextPanel::DrawPresetBlock(AppState& app, int ti, float width) {
    int slot = IsChordTrack(ti) ? kNumSynthTracks : SynthSlotForTrack(ti);
    const SynthPatch& patch = TrackPatch(app, ti);
    const int total = static_cast<int>(AllSynthPresets().size() + app.CustomSynthPresetNamesCached().size());

    BlockBegin("PRESET");
    const float arrowW = 26.0f;
    int cur = presetIdx_[slot] < total ? presetIdx_[slot] : -1;
    // < > step through the category the browser was last left on; the wide button in the middle opens the browser
    if (ImGui::Button("<##presetPrev", ImVec2(arrowW, 0))) { int n = browser_.Step(app, cur, -1); if (n >= 0) ApplyPresetToTrack(app, ti, n); }
    ImGui::SameLine(0, 4);
    {
        const float bw = width - arrowW * 2 - 8.0f - 48.0f;
        const PresetBrowser::Item* it = cur >= 0 ? browser_.Find(app, cur) : nullptr;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float bh = ImGui::GetFrameHeight();
        if (ImGui::InvisibleButton("##presetOpen", ImVec2(bw, bh))) browser_.Open(app, ti, cur);
        const bool hov = ImGui::IsItemHovered();
        ImDrawList* bdl = ImGui::GetWindowDrawList();
        const ImVec2 p1(p0.x + bw, p0.y + bh);
        bdl->AddRectFilled(p0, p1, hov ? vis::kCellHover : vis::kCell, 4.0f);
        bdl->AddRect(p0, p1, hov ? vis::kAccent : vis::kLineHi, 4.0f);
        bdl->AddCircleFilled(ImVec2(p0.x + 10, p0.y + bh * 0.5f), 3.5f, it ? PresetBrowser::CategoryColor(it->category) : vis::kTextDim);
        ImGui::PushClipRect(ImVec2(p0.x + 20, p0.y), ImVec2(p1.x - 16, p1.y), true);
        bdl->AddText(ImVec2(p0.x + 20, p0.y + (bh - ImGui::GetTextLineHeight()) * 0.5f), it ? vis::kText : vis::kTextDim, it ? it->name.c_str() : "- custom -");
        ImGui::PopClipRect();
        bdl->AddTriangleFilled(ImVec2(p1.x - 14, p0.y + bh * 0.5f - 2), ImVec2(p1.x - 6, p0.y + bh * 0.5f - 2), ImVec2(p1.x - 10, p0.y + bh * 0.5f + 3), vis::kTextDim);
        if (hov) Hint("Open the preset browser (categories, search, favorites)");
    }
    ImGui::SameLine(0, 4);
    if (ImGui::Button(">##presetNext", ImVec2(arrowW, 0))) { int n = browser_.Step(app, cur, +1); if (n >= 0) ApplyPresetToTrack(app, ti, n); }
    ImGui::SameLine(0, 4);
    if (ImGui::Button("SAVE##presetSave", ImVec2(44, 0))) { presetName_[0] = '\0'; ImGui::OpenPopup("PresetSaveName"); }
    if (ImGui::IsItemHovered()) Hint("Save the current sound as a named preset (appears under MY PRESETS)");
    if (NamePrompt("PresetSaveName", presetName_, sizeof(presetName_), "PRESET NAME")) {
        bool oldChord = app.synthViewChordSlot;
        int oldSynth = app.prSynthIndex;
        app.synthViewChordSlot = IsChordTrack(ti);
        if (!IsChordTrack(ti)) app.prSynthIndex = SynthSlotForTrack(ti);
        app.SaveCurrentSynthAsPreset(presetName_);
        app.synthViewChordSlot = oldChord;
        app.prSynthIndex = oldSynth;
    }

    // ENGINE button: waveform icon + engine name; click cycles the engine type
    ImGui::Dummy(ImVec2(1, 3));
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool sampleEng = patch.engineType == SynthEngineType::Sample;
    float bh = sampleEng ? 26.0f : cellSize_.y - 8.0f;   // v44.53: on SAMPLE the engine bar is one line, the room goes to the sample card below
    ImGui::InvisibleButton("##engine", ImVec2(width, bh));
    bool hov = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) { app.PushSoundUndo(); AdjustTrackPatchParam(app, ti, 0, 1, false); presetIdx_[slot] = -1; }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { app.PushSoundUndo(); AdjustTrackPatchParam(app, ti, 0, -1, false); presetIdx_[slot] = -1; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p1(p0.x + width, p0.y + bh);
    dl->AddRectFilled(p0, p1, hov ? vis::kCellHover : vis::kCell, 5.0f);
    dl->AddRect(p0, p1, IM_COL32(242, 163, 61, 160), 5.0f);
    DrawWaveIcon(dl, ImVec2(p0.x + 8, p0.y + 6), ImVec2(p0.x + 62, p1.y - 6), patch, vis::kAccent);
    dl->AddText(ImVec2(p0.x + 70, sampleEng ? p0.y + (bh - ImGui::GetTextLineHeight()) * 0.5f : p0.y + 7), vis::kText, SynthEngineTypeName(patch.engineType));
    const char* sub = patch.engineType == SynthEngineType::Subtractive ? WaveformName(patch.waveform)
                    : patch.engineType == SynthEngineType::Wavetable ? kWavetableBankNames[std::clamp(patch.wavetableBank, 0, kNumWavetableBanks - 1)]
                    : patch.engineType == SynthEngineType::Sample ? (patch.sample.HasFile() ? "your own file" : "no sample yet")
                    : "4-OP";
    if (sampleEng) dl->AddText(ImVec2(p0.x + 70 + ImGui::CalcTextSize("SAMPLE").x + 10.0f, p0.y + (bh - ImGui::GetTextLineHeight()) * 0.5f), vis::kTextDim, sub);
    else dl->AddText(ImVec2(p0.x + 70, p0.y + 7 + ImGui::GetTextLineHeight() + 1), vis::kTextDim, sub);
    if (hov) Hint("Engine: click = next, right-click = previous\n(Subtractive / FM / Wavetable / Sample: plays a file of your own at the pitch of the note)");
    if (patch.engineType == SynthEngineType::Sample) { ImGui::Dummy(ImVec2(1, 3)); DrawSampleBlock(app, ti, width); }   // v44.53: the sample of this sound (file, HEAR, MINE, waveform)
    BlockEnd();
}

void ContextPanel::DrawSound(AppState& app, int ti) {
    const SynthPatch& patch = TrackPatch(app, ti);
    int total = NumPatchParams(patch.engineType);

    // Split the patch rows into the four tab groups by name (row 0 = ENGINE
    // type, handled by the engine button in the PRESET block).
    std::vector<int> src, osc2, thick, filt, env, lfo, voice;
    for (int idx = 1; idx < total; ++idx) {
        std::string n = PatchParamName(patch.engineType, idx);
        if (n == "ATTACK" || n == "DECAY" || n == "SUSTAIN" || n == "RELEASE") env.push_back(idx);
        else if (StartsWith(n, "LFO")) lfo.push_back(idx);
        else if (n == "GLIDE" || n == "VOLUME") voice.push_back(idx);
        else if (StartsWith(n, "OSC2")) osc2.push_back(idx);
        else if (n == "CUTOFF" || n == "RESONANCE" || n == "ENV AMT" || n == "FILT DECAY" || n == "FILTER" || n == "KEYTRACK") filt.push_back(idx);
        else if (patch.engineType == SynthEngineType::Sample && (n == "PHASE SYNC" || n == "DRIFT")) continue;   // v44.53: nothing to align or wander in a recording
        else if (n == "UNISON" || StartsWith(n, "UNI ") || StartsWith(n, "SUB ") || n == "NOISE" || n == "DRIVE" || n == "PHASE SYNC" || n == "DRIFT") thick.push_back(idx);
        else src.push_back(idx);
    }

    // ---- left: PRESET (+ engine button) and the two values you always want in reach ----
    const float presetW = 262.0f;
    cellSize_.x = 62.0f;
    DrawPresetBlock(app, ti, presetW);
    if (!voice.empty()) {
        BlockBegin("OUTPUT");
        for (size_t i = 0; i < voice.size(); ++i) { if (i > 0) ImGui::SameLine(0, 6); PatchCell(app, ti, voice[i]); }
        BlockEnd(true);
    }

    // ---- right: tabs ----
    static const char* kTabs[6] = {"OSC", "FILTER", "ENV + LFO", "EQ", "FX", "AUTO"};
    const bool sampleEngine = patch.engineType == SynthEngineType::Sample;   // v44.53: the OSC tab becomes the SAMPLE tab
    float availW = ImGui::GetContentRegionAvail().x;
    ImGui::BeginGroup();
    for (int tb = 0; tb < 6; ++tb) {
        if (tb > 0) ImGui::SameLine(0, 4);
        bool on = (synthTab_ == tb);
        if (ToggleButton(tb == 0 && sampleEngine ? "SAMPLE" : kTabs[tb], on, ImVec2(tb == 2 ? 96.0f : 78.0f, 0), ToggleTab())) { synthTab_ = tb; selCell_ = 0; }
    }
    ImGui::Dummy(ImVec2(1, 2));

    // cell width adapts to what the tab holds, so even an FM patch fits one row
    auto fit = [&](int nCells, int nGroups, float extra) {
        float cw = (availW - extra - nGroups * 19.0f - 6.0f * std::max(0, nCells - nGroups)) / std::max(1, nCells);
        cellSize_.x = std::clamp(cw, 40.0f, 70.0f);
    };
    auto group = [&](const char* title, const std::vector<int>& list, bool sep, float previewW = 0.0f) {
        if (list.empty()) return;
        BlockBegin(title);
        const float previewH = cellSize_.y > 10.0f ? cellSize_.y : 44.0f;
        if (std::string(title) == "ENVELOPE") DrawEnvelope(patch, ImVec2(previewW, previewH));
        else if (std::string(title) == "LFO") DrawLfoPreview(patch, ImVec2(previewW, previewH));
        for (size_t i = 0; i < list.size(); ++i) {
            if (i > 0 || previewW > 0.0f) ImGui::SameLine(0, 6);
            PatchCell(app, ti, list[i]);
        }
        BlockEnd(sep);
    };

    if (synthTab_ == 0 && sampleEngine) {
        cellSize_.x = 62.0f;
        DrawSamplerVoice(app, ti);
        ImGui::Dummy(ImVec2(1, 2));
        fit(static_cast<int>(thick.size()), 1, 0.0f);
        const float keepH = cellSize_.y; cellSize_.y = std::min(cellSize_.y, 34.0f);   // one panel height holds both rows
        group("UNISON / LAYERS", thick, false);
        cellSize_.y = keepH;
    } else if (synthTab_ == 0) {
        int groups = (src.empty() ? 0 : 1) + (osc2.empty() ? 0 : 1) + (thick.empty() ? 0 : 1);
        fit(static_cast<int>(src.size() + osc2.size() + thick.size()), groups, 0.0f);
        group("SOURCE", src, true);
        group("OSC 2", osc2, true);
        group("UNISON / LAYERS", thick, false);
        ImGui::NewLine();
        ImGui::TextDisabled("UNISON: detuned copies of the voice (up to %d, chords %d per note). SUB / NOISE / DRIVE work on every engine.",
                            kMaxUnisonVoices, ChordVoice::kMaxChordUnison);
    } else if (synthTab_ == 1) {
        if (filt.empty()) {
            ImGui::Dummy(ImVec2(1, 8));
            ImGui::TextDisabled("The FM engine has no filter - shape the tone with the operator levels on the OSC tab.");
        } else {
            fit(static_cast<int>(filt.size()), 1, 0.0f);
            cellSize_.x = std::max(cellSize_.x, 62.0f);
            group("FILTER", filt, false);
            ImGui::NewLine();
            ImGui::TextDisabled("CUTOFF / RESO shape the tone, ENV AMT + F.DECAY make it open and close on every note, FILTER 24 dB is steeper, ACID is the 303-style filter where accented steps open it further, KEYTRK makes high notes brighter.");
        }
    } else if (synthTab_ == 2) {
        fit(static_cast<int>(env.size() + lfo.size()), 2, 96.0f + 80.0f);
        group("ENVELOPE", env, true, 96.0f);
        group("LFO", lfo, false, 80.0f);
        ImGui::NewLine();
    } else if (synthTab_ == 3) {
        DrawEq(app, ti, availW);
    } else if (synthTab_ == 5) {
        DrawAuto(app, ti, availW);
    } else {
        cellSize_.x = 68.0f;
        DrawCommonRow(app, ti);
        ImGui::NewLine();
    }
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(1, 4));
}

// ===========================================================================
// AUTO (v44.39): curves of this instrument's knobs over the SCENE's loop -- the same parameter rows a brick has on its PARAM page.
// THROW ONTO THE TRACK moves them into the brick.
// ===========================================================================

void ContextPanel::DrawAuto(AppState& app, int ti, float availW) {
    Pattern& pat = app.CurrentPattern();
    if (!AutoParamValid(ti, autoParam_)) { autoParam_ = 0; for (int p = 0; p < kNumAutoParams; ++p) if (AutoParamValid(ti, p)) { autoParam_ = p; break; } }
    const ImU32 col = colors::kParam;
    const float fh = ImGui::GetFrameHeight();

    // the knobs of this instrument; a star = the scene already moves it
    for (int p = 0, shown = 0; p < kNumAutoParams; ++p) {
        if (!AutoParamValid(ti, p)) continue;
        const int k = pat.params.Find(ti, p);
        const bool moves = k >= 0 && pat.params.row[k].lane.Any();
        std::string lb = std::string(AutoParamName(p)) + (moves ? " *" : "");
        if (shown++ > 0) ImGui::SameLine(0, 4);
        if (ToggleButton(lb.c_str(), autoParam_ == p, ImVec2(0, 0), ToggleTab())) autoParam_ = p;
        if (ImGui::IsItemHovered()) Hint("%s of %s: pick the knob to draw.  A star = the scene already moves it.", AutoParamName(p), TrackDisplayName(pat.tracks[ti]).c_str());
    }
    ImGui::Dummy(ImVec2(1, 3));

    const float sideW = 196.0f;
    const float canvasW = std::max(200.0f, availW - sideW - 12.0f);
    const bool here = app.sequencer.IsPlaying() && app.sequencer.PlayPatternIndex() == app.project.currentPattern;
    const double prog = here ? app.sequencer.SceneProgress() : -1.0;
    ImGui::BeginGroup();
    LaneEditor(app, "##autolane", pat.params, ti, autoParam_, ImVec2(canvasW, 92.0f), col, prog);
    ImGui::EndGroup();
    ImGui::SameLine(0, 12);

    const int rowK = pat.params.Find(ti, autoParam_);
    const Lane* cl = rowK >= 0 ? &pat.params.row[rowK].lane : nullptr;
    const int s0 = cl ? cl->Start() : 0, e0 = cl ? cl->End() : 0;
    ImGui::BeginGroup();
    {
        auto setLane = [&](const Lane& shape) {
            app.PushSoundUndo();
            const int k = pat.params.Add(ti, autoParam_);
            if (k < 0) { app.DropLastUndo(); app.SetStatus("A SCENE HAS ROOM FOR " + std::to_string(kMaxParamRows) + " PARAMETER ROWS - REMOVE ONE FIRST"); return; }
            pat.params.row[k].lane = shape; pat.params.row[k].lane.Normalize(-100, 100);
        };
        if (GlyphTile("##aflat", Glyph::Flat, nullptr, s0 == e0, col, true, fh + 6, fh)) setLane(Lane{});
        if (ImGui::IsItemHovered()) Hint("FLAT: the knob does not move over the scene (the row stays, ready to be drawn).");
        ImGui::SameLine(0, 3);
        if (GlyphTile("##aup", Glyph::FadeIn, nullptr, s0 == 0 && e0 > 0, col, true, fh + 6, fh)) setLane(Lane::Line(0, 100, -100, 100));
        if (ImGui::IsItemHovered()) Hint("UP: from the sound's own value to the most the knob can add, once round the scene's loop.");
        ImGui::SameLine(0, 3);
        if (GlyphTile("##adown", Glyph::FadeOut, nullptr, s0 == 0 && e0 < 0, col, true, fh + 6, fh)) setLane(Lane::Line(0, -100, -100, 100));
        if (ImGui::IsItemHovered()) Hint("DOWN: from the sound's own value to the most the knob can take away, once round the scene's loop.");
        ImGui::SameLine(0, 10);
        if (GlyphTile("##arem", Glyph::Remove, nullptr, false, colors::kTrans, rowK >= 0, fh + 6, fh)) { app.PushSoundUndo(); pat.params.Remove(rowK); }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) Hint(rowK >= 0 ? "Remove this curve from the scene (the sound goes back to its own value)." : "This scene has no curve for this knob.");
    }
    ImGui::Dummy(ImVec2(1, 2));
    ImGui::TextDisabled("START  %s", ParamValueText(autoParam_, s0).c_str());
    ImGui::TextDisabled("END    %s", ParamValueText(autoParam_, e0).c_str());
    ImGui::TextDisabled("%d of %d curves in this scene", pat.params.n, kMaxParamRows);
    ImGui::EndGroup();
    if (ImGui::IsItemHovered()) Hint("The curves go round with the scene's loop. THROW ONTO THE TRACK moves them into the brick (its PARAM page). 0 = the sound as it is.");
    ImGui::Dummy(ImVec2(1, 2));
}

// ===========================================================================
// CHORDS
// ===========================================================================

void ContextPanel::DrawChords(AppState& app, int ti) {
    (void)ti;
    char buf[24];
    cellSize_.x = 68.0f;

    // ---- CHORDS: v44.46 the progression picker (PROGRESSION, CUSTOM, APPLY) is gone: START FROM in CHORD LAB does the same, with the
    // chords in front of you. What stays here is what CHORD LAB does not do: follow the key, a random progression, clear.
    BlockBegin("CHORDS");
    const float blockW = 330.0f;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.40f, 0.34f, 1.0f));
    if (ImGui::Button("OPEN CHORD LAB", ImVec2(blockW, 0))) app.OpenTool(AppState::Tool::ChordLab);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) Hint("Build a progression: the palette of the key, START FROM a typical progression, a loop of blocks. Esc closes it.");
    ImGui::TextDisabled("in %s %s  (change in the header)", kKeyNames[std::clamp(app.project.randomizerKeyRoot, 0, kNumKeys - 1)],
                        kScaleNames[std::clamp(app.project.randomizerScale, 0, kNumScales - 1)]);
    {
        // Written chords never change by themselves when the key / scale changes.
        const Pattern& cpat = app.CurrentPattern();
        if (app.SceneChordsOutOfKey(cpat)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.64f, 0.24f, 1.0f));
            ImGui::TextUnformatted("chords here were written in");
            ImGui::SameLine(0, 4);
            ImGui::Text("%s %s", kKeyNames[std::clamp<int>(app.ChordsSrcKey(cpat), 0, kNumKeys - 1)], kScaleNames[std::clamp<int>(app.ChordsSrcScale(cpat), 0, kNumScales - 1)]);
            ImGui::PopStyleColor();
        }
        bool follow = app.project.chordsFollowKey;
        if (ImGui::Button("ADJUST CHORDS TO SCALE", ImVec2(blockW * 0.58f, 0))) app.AdjustChordsToScale(false);
        if (ImGui::IsItemHovered()) Hint("Move the chords of this scene to the project key / scale\n(transpose by the key change, then snap to the scale). Undo restores them.");
        ImGui::SameLine(0, 4);
        if (ToggleButton("FOLLOW KEY", follow, ImVec2(blockW * 0.38f, 0))) app.project.chordsFollowKey = !follow;
        if (ImGui::IsItemHovered()) Hint("OFF (default): changing KEY / SCALE leaves written chords as they are.\nON: every scene's chords are re-tuned automatically when KEY / SCALE changes.");
        if (MiniButton("ADJUST MELODY TOO")) app.AdjustChordsToScale(true);
        if (ImGui::IsItemHovered()) Hint("Same, but also the three synth tracks of this scene");
    }
    if (ImGui::Button("RANDOM", ImVec2(88, 0))) app.RandomizeChordProgression();
    if (ImGui::IsItemHovered()) Hint("A random progression in the key, written with the VOICING settings on the right.");
    ImGui::SameLine(0, 4);
    if (ImGui::Button("CLEAR", ImVec2(70, 0))) { app.ClearChordTrack(); app.SetStatus("CHORD TRACK CLEARED"); }
    BlockEnd();

    // ---- VOICING (the five generator settings) ----
    BlockBegin("VOICING");
    int st = DiscreteCell("cx", "CHORDS", app.chdComplex ? "7TH" : "TRIAD");
    if (st != 0) { app.chdComplex = !app.chdComplex; app.RefreshChordLive(true); }
    ImGui::SameLine(0, 6);
    st = DiscreteCell("mode", "OUTPUT", app.chdOutputMode == ChordOutputMode::Arpeggio ? "ARP" : "STACK");
    if (st != 0) app.chdOutputMode = app.chdOutputMode == ChordOutputMode::Arpeggio ? ChordOutputMode::Stack : ChordOutputMode::Arpeggio;
    bool arp = app.chdOutputMode == ChordOutputMode::Arpeggio;
    ImGui::SameLine(0, 6);
    st = DiscreteCell("arpst", "ARP STYLE", ArpStyleName(app.chdArpStyle), !arp);
    if (st != 0) app.chdArpStyle = static_cast<ArpStyle>((static_cast<int>(app.chdArpStyle) + (st > 0 ? 1 : -1) + kNumArpStyles) % kNumArpStyles);
    ImGui::SameLine(0, 6);
    std::snprintf(buf, sizeof(buf), "%d STEP%s", app.chdArpRateSteps, app.chdArpRateSteps == 1 ? "" : "S");
    st = DiscreteCell("arprate", "ARP RATE", buf, !arp);
    if (st != 0) {
        static const int kRates[3] = {1, 2, 4};
        int c = 0;
        for (int i = 0; i < 3; ++i) if (kRates[i] == app.chdArpRateSteps) c = i;
        app.chdArpRateSteps = kRates[(c + (st > 0 ? 1 : -1) + 3) % 3];
    }
    ImGui::SameLine(0, 6);
    if (app.chdChordLengthSteps == 0) std::snprintf(buf, sizeof(buf), "AUTO");
    else std::snprintf(buf, sizeof(buf), "%d STEPS", app.chdChordLengthSteps);
    st = DiscreteCell("clen", "CHORD LEN", buf);
    if (st != 0) {
        static const int kLens[5] = {0, 4, 8, 16, 32};
        int c = 0;
        for (int i = 0; i < 5; ++i) if (kLens[i] == app.chdChordLengthSteps) c = i;
        app.chdChordLengthSteps = kLens[(c + (st > 0 ? 1 : -1) + 5) % 5];
    }
    ImGui::TextDisabled("settings are used by RANDOM and START FROM");
    BlockEnd();

    // ---- the chord editor (single chords by hand) ----
    BlockBegin("EDIT BY HAND");
    if (ImGui::Button("CHORD EDITOR...")) OpenChordEditor(app, app.project.chordBankOn ? app.project.chordSceneEdit : -1);
    if (ImGui::IsItemHovered()) Hint("Build or change single chords by hand: root, type, inversion, length, order.\nWorks on the chord scene you are editing (LIVE > CHORDS) or on this scene's own chords.");
    ImGui::SameLine(0, 8);
    ImGui::TextDisabled(app.project.chordBankOn ? "RANDOM writes into chord slot %d" : "RANDOM writes into this scene's chords", app.project.chordSceneEdit + 1);
    BlockEnd(false);
    ImGui::Dummy(ImVec2(1, 4));
    // (the chord track's sends / insert FX / length live in the SOUND view)
}

// ===========================================================================
// entry point
// ===========================================================================

void ContextPanel::Draw(AppState& app, const DpadKeys& keys) {
    SceneLock lock(app.sequencer.PatternMutex(), "INSTRUMENT PANEL");
    int ti = std::clamp(app.seqCursorTrack, 0, kNumTracks - 1);
    Track& t = app.CurrentPattern().tracks[ti];

    if (hearOffAt_ > 0.0 && ImGui::GetTime() >= hearOffAt_) StopHear(app);   // v44.53: HEAR of a SAMPLE synth lets go of its note
    keys_ = keys;
    cellCounter_ = 0;
    cellSize_ = ImVec2(60.0f, 60.0f);
    // LEFT / RIGHT move the selection through last frame's cell list.
    if (lastCellCount_ > 0) {
        if (keys_.left) selCell_ = std::max(0, selCell_ - 1);
        if (keys_.right) selCell_ = std::min(lastCellCount_ - 1, selCell_ + 1);
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !keys_.up && !keys_.down) pendingUndo_ = false;

    ImGui::PushID("ContextPanel");
    DrawHeader(app, ti);
    ImGui::Dummy(ImVec2(1, 2));

    if (t.type == TrackType::Drum) {
        DrawDrum(app, ti);
    } else if (t.type == TrackType::Sampler) {
        DrawSampler(app, ti);
    } else if (t.type == TrackType::Chord && chordTab_ == 0) {
        DrawChords(app, ti);
        // sends / insert FX / length for the chord track live in SOUND
    } else {
        DrawSound(app, ti);
    }

    lastCellCount_ = cellCounter_;
    selCell_ = std::clamp(selCell_, 0, std::max(0, cellCounter_ - 1));
    ImGui::PopID();

    // the preset browser belongs to the track it was opened for, not to the one selected now
    {
        const int bt = browser_.TrackIndex();
        const int bslot = bt >= 0 ? (IsChordTrack(bt) ? kNumSynthTracks : SynthSlotForTrack(bt)) : 0;
        browser_.Draw(app, bt >= 0 ? presetIdx_[bslot] : -1, [&](int idx) { if (bt >= 0) ApplyPresetToTrack(app, bt, idx); });
    }
}

} // namespace t8::desktop
