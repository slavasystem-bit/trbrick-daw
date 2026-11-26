#include "BrickGlyphs.h"
#include "MixerPanel.h"
#include "Visual.h"
#include "MixerSync.h"
#include "LiveWidgets.h"
#include "ValueField.h"
#include "TrackStyle.h"
#include "imgui.h"
#include "InfoLine.h"
#include <algorithm>
#include <cstdio>
#include <mutex>

namespace t8::desktop {

void MixerPanel::Draw(AppState& app, const LiveLayout& layout) {
    std::lock_guard<SceneMutex> lock(app.sequencer.PatternMutex());
    Pattern& pat = app.CurrentPattern();

    ImGui::PushID("MixerPanel");
    ImVec2 origin = ImGui::GetCursorPos();
    float innerW = ImGui::GetContentRegionAvail().x;
    const float kValW = 40.0f, kPanW = std::min(150.0f, innerW * 0.26f), kGap = 8.0f;
    float volW = innerW - kPanW - kValW * 2 - kGap * 3;
    const float rowsTop = origin.y + layout.headerH;

    // ---- header: column titles ------------------------------------------
    ImGui::SetCursorPos(ImVec2(origin.x, origin.y + (layout.headerH - ImGui::GetFrameHeight()) * 0.5f));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("MIXER");
    {
        // A Synth / Chord track is selected but the roll was closed: offer a
        // one-click way back to it.
        TrackType selType = pat.tracks[std::clamp(app.seqCursorTrack, 0, kNumTracks - 1)].type;
        if (selType != TrackType::Drum) {
            ImGui::SameLine(0, 10);
            if (ImGui::Button("PIANO ROLL")) app.deskRollClosed = false;
            if (ImGui::IsItemHovered()) Hint("Show the piano roll for the selected track");
        } else {
            ImGui::SameLine(0, 10);
            if (ImGui::Button("LANES")) app.deskLanesClosed = false;
            if (ImGui::IsItemHovered()) Hint("Velocity and nudge of every hit of the selected drum track");
        }
    }
    ImGui::SameLine(origin.x + 190.0f);
    ImGui::TextDisabled("VOLUME");
    ImGui::SameLine(origin.x + volW + kValW + kGap * 2);
    ImGui::TextDisabled("PAN");

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 win = ImGui::GetWindowPos();

    for (int i = 0; i < kNumTracks; ++i) {
        auto& mt = app.EditMixer().tracks[i];
        Track& t = pat.tracks[i];
        float ry = rowsTop + i * layout.rowH;
        ImGui::PushID(i);

        if (app.seqCursorTrack == i) {
            ImVec2 a(win.x + origin.x - 2.0f, win.y + ry);
            dl->AddRectFilled(a, ImVec2(a.x + innerW + 4.0f, a.y + layout.rowH - 1.0f), vis::kAccentGlow, 4.0f);
        }

        // volume
        float x = origin.x;
        ImGui::SetCursorPos(ImVec2(x, ry + 3.0f));
        float vol = mt.volume;
        {
            ValueSpec sp = BarSpec(0.0f, 1.0f, 0.8f, volW); sp.height = layout.rowH - 6.0f; sp.meter = mt.peak.load(); sp.color = TrackColor(i); sp.scale = 100.0f;
            const ValueResult r = ValueField("vol", &vol, sp, volW);
            if (r.started) app.PushUndo();
            if (r.changed) { mt.volume = std::clamp(vol, 0.0f, 1.0f); t.volume = mt.volume; }
            if (r.cancelled) app.DropLastUndo();
        }
        x += volW + kGap;
        ImGui::SetCursorPos(ImVec2(x, ry));
        ImGui::AlignTextToFramePadding();
        ImGui::SetCursorPosY(ry + (layout.rowH - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::Text("%3.0f%%", mt.volume * 100.0f);
        x += kValW + kGap;

        // pan
        ImGui::SetCursorPos(ImVec2(x, ry + 3.0f));
        float pan = mt.pan;
        {
            ValueSpec sp = BarSpec(-1.0f, 1.0f, 0.0f, kPanW); sp.height = layout.rowH - 6.0f; sp.fromCenter = true; sp.color = TrackColor(i); sp.scale = 100.0f;
            const ValueResult r = ValueField("pan", &pan, sp, kPanW);
            if (r.started) app.PushUndo();
            if (r.changed) { mt.pan = std::clamp(pan, -1.0f, 1.0f); t.pan = mt.pan; }
            if (r.cancelled) app.DropLastUndo();
        }
        x += kPanW + kGap;
        ImGui::SetCursorPos(ImVec2(x, ry));
        ImGui::SetCursorPosY(ry + (layout.rowH - ImGui::GetTextLineHeight()) * 0.5f);
        if (std::abs(mt.pan) < 0.01f) ImGui::TextDisabled("C");
        else ImGui::TextDisabled("%c%d", mt.pan < 0 ? 'L' : 'R', static_cast<int>(std::abs(mt.pan) * 100.0f + 0.5f));
        ImGui::PopID();
    }

    DrawMasterStrip(app, layout, rowsTop + kNumTracks * layout.rowH + 4.0f);
    ImGui::PopID();
}

void DrawMasterStrip(AppState& app, const LiveLayout& layout, float y) {
    auto& mixer = app.engine.GetMixer();
    ImGui::PushID("MasterStrip");
    float x0 = ImGui::GetCursorPosX();
    float innerW = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 win = ImGui::GetWindowPos();
    float lineH = layout.footerH - 4.0f;

    ImGui::SetCursorPos(ImVec2(x0, y));
    dl->AddLine(ImVec2(win.x + x0, win.y + y - 3.0f), ImVec2(win.x + x0 + innerW, win.y + y - 3.0f),
                vis::kLine, 1.0f);
    ImGui::SetCursorPos(ImVec2(x0, y + (lineH - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f), "MASTER");

    const float kLabelW = 70.0f, kValW = 40.0f, kMeterW = 90.0f, kGap = 8.0f;
    float volW = innerW - kLabelW - kValW - kMeterW - kGap * 3 - 150.0f;
    ImGui::SetCursorPos(ImVec2(x0 + kLabelW, y + 1.0f));
    float mv = mixer.masterVolume;
    {
        ValueSpec sp = BarSpec(0.0f, 1.2f, 0.4f, volW); sp.height = lineH - 2.0f; sp.scale = 100.0f;
        const ValueResult r = ValueField("master", &mv, sp, volW);
        if (r.changed) SetMasterVolume(app, mv);
    }
    ImGui::SetCursorPos(ImVec2(x0 + kLabelW + volW + kGap, y + (lineH - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::Text("%3.0f%%", mixer.masterVolume * 100.0f);

    // stereo meters: two thin bars stacked
    float mx = x0 + kLabelW + volW + kGap * 2 + kValW;
    for (int ch = 0; ch < 2; ++ch) {
        float peak = ch == 0 ? mixer.masterPeakL.load() : mixer.masterPeakR.load();
        float frac = std::clamp(peak / 1.3f, 0.0f, 1.0f);
        ImVec2 a(win.x + mx, win.y + y + 4.0f + ch * 8.0f);
        ImVec2 b(a.x + kMeterW, a.y + 5.0f);
        dl->AddRectFilled(a, b, vis::kInset, 2.0f);
        ImU32 mc = peak > 1.0f ? vis::kRed : peak > 0.75f ? vis::kAccent : vis::kTeal;
        dl->AddRectFilled(a, ImVec2(a.x + kMeterW * frac, b.y), mc, 2.0f);
    }
    bool clipping = mixer.masterClipHold.load() > 0.0f;
    {   // loudness readout: short-term LUFS and the true peak (click = reset the integrated value and the peak)
        LoudnessMeter& lm = app.engine.Loudness();
        const float st = lm.ShortTerm(), tp = lm.TruePeakDb();
        const auto& mcs = app.project.fx.master;
        const bool hot = clipping || (mcs.limiterOn && tp > mcs.ceilingDb + 0.3f);
        char lb[48];
        if (st > -100.0f) std::snprintf(lb, sizeof(lb), "%s%.1f LUFS  TP %.1f", clipping ? "CLIP  " : "", st, tp);
        else std::snprintf(lb, sizeof(lb), clipping ? "CLIP" : "- LUFS");
        ImGui::SetCursorPos(ImVec2(mx + kMeterW + kGap, y + (lineH - ImGui::GetTextLineHeight()) * 0.5f));
        ImGui::TextColored(hot ? ImVec4(0.91f, 0.30f, 0.30f, 1.0f) : ImVec4(0.45f, 0.47f, 0.54f, 1.0f), "%s", lb);
        if (ImGui::IsItemHovered()) {
            const float il = lm.Integrated();
            Hint("Master loudness (ITU BS.1770): short-term %.1f LUFS, integrated %s%.1f, momentary %.1f.\n"
                              "TP = true peak in dBTP since the last reset (limiter ceiling %.1f).\nClick to reset the integrated value and the peak.",
                              st, il > -100.0f ? "" : "(none yet) ", il > -100.0f ? il : 0.0f, lm.Momentary(), mcs.ceilingDb);
        }
        if (ImGui::IsItemClicked()) lm.RequestReset();
    }
    ImGui::PopID();
}

void DrawTrackVolumeStrip(AppState& app, const LiveLayout& layout, float y, int ti) {
    ti = std::clamp(ti, 0, kNumTracks - 1);
    auto& mt = app.EditMixer().tracks[ti];
    Track& t = app.CurrentPattern().tracks[ti];
    ImGui::PushID("TrackVolStrip");
    float x0 = ImGui::GetCursorPosX();
    float innerW = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 win = ImGui::GetWindowPos();
    float lineH = layout.footerH - 4.0f;

    dl->AddLine(ImVec2(win.x + x0, win.y + y - 3.0f), ImVec2(win.x + x0 + innerW, win.y + y - 3.0f), vis::kLine, 1.0f);
    DrawInstrumentGlyph(dl, ti, ImVec2(win.x + x0 + 8.0f, win.y + y + lineH * 0.5f), 14.0f, TrackColor(ti));   // v44.37
    ImGui::SetCursorPos(ImVec2(x0 + 18.0f, y + (lineH - ImGui::GetTextLineHeight()) * 0.5f));
    std::string nm = TrackDisplayName(t);
    while (nm.size() > 1 && ImGui::CalcTextSize(nm.c_str()).x > 50.0f) nm.pop_back();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TrackColor(ti)), "%s", nm.c_str());

    const float kLabelW = 70.0f, kValW = 40.0f, kGap = 8.0f;
    float volW = innerW - kLabelW - kValW - kGap - 8.0f;
    ImGui::SetCursorPos(ImVec2(x0 + kLabelW, y + 1.0f));
    float vol = mt.volume;
    {
        ValueSpec sp = BarSpec(0.0f, 1.0f, 0.8f, volW); sp.height = lineH - 2.0f; sp.meter = mt.peak.load(); sp.color = TrackColor(ti); sp.scale = 100.0f;
        const ValueResult r = ValueField("trackvol", &vol, sp, volW);
        if (r.started) app.PushUndo();
        if (r.changed) { mt.volume = std::clamp(vol, 0.0f, 1.0f); t.volume = mt.volume; }
        if (r.cancelled) app.DropLastUndo();
    }
    if (ImGui::IsItemHovered()) Hint("Volume of the track you are editing (same fader as in the MIXER)");
    ImGui::SetCursorPos(ImVec2(x0 + kLabelW + volW + kGap, y + (lineH - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::Text("%3.0f%%", mt.volume * 100.0f);
    ImGui::PopID();
}

} // namespace t8::desktop
