#include "MixLabPanel.h"
#include "Visual.h"
#include "TrackStyle.h"
#include "LiveWidgets.h"
#include "InfoLine.h"
#include "core/TrackEq.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

namespace t8::desktop {

namespace {

constexpr float kDbSpan = 62.0f;   // the map shows the loudest level down to this far below it

bool IsDrum(int t) { return t < kNumDrumTracks; }
bool InFilter(int filter, int t) { return filter == 0 || (filter == 1 && IsDrum(t)) || (filter == 2 && !IsDrum(t)); }

std::string HzLabel(float hz) {
    char b[24];
    if (hz >= 1000.0f) std::snprintf(b, sizeof b, "%.1fk", hz / 1000.0f); else std::snprintf(b, sizeof b, "%.0f", hz);
    return b;
}

// Smoothed copy of one instrument's bands (a third of an octave), for drawing.
void Smooth(const std::array<float, kMixBands>& in, std::array<float, kMixBands>& out) {
    for (int b = 0; b < kMixBands; ++b) {
        double s = 0.0; int n = 0;
        for (int k = -2; k <= 2; ++k) { const int bb = b + k; if (bb < 0 || bb >= kMixBands) continue; s += std::pow(10.0, in[static_cast<size_t>(bb)] / 10.0); ++n; }
        out[static_cast<size_t>(b)] = static_cast<float>(10.0 * std::log10(s / std::max(1, n) + 1e-12));
    }
}

void Chip(ImDrawList* dl, ImVec2 p, const char* text, ImU32 col, float* outW = nullptr) {
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float w = ts.x + 14.0f, h = ts.y + 4.0f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col, h * 0.5f);
    dl->AddText(ImVec2(p.x + 7.0f, p.y + 2.0f), IM_COL32(16, 18, 22, 255), text);
    if (outW) *outW = w;
}

void WrapText(ImDrawList* dl, ImVec2 p, float maxW, ImU32 col, const std::string& text, float lineH, int maxLines) {
    std::string line; int lines = 0; size_t i = 0;
    while (i < text.size() && lines < maxLines) {
        size_t j = text.find(' ', i); if (j == std::string::npos) j = text.size();
        const std::string word = text.substr(i, j - i);
        const std::string tryLine = line.empty() ? word : line + " " + word;
        if (!line.empty() && ImGui::CalcTextSize(tryLine.c_str()).x > maxW) {
            dl->AddText(ImVec2(p.x, p.y + lines * lineH), col, line.c_str()); ++lines; line = word;
        } else line = tryLine;
        i = j + 1;
    }
    if (!line.empty() && lines < maxLines) dl->AddText(ImVec2(p.x, p.y + lines * lineH), col, line.c_str());
}

}  // namespace

void MixLabPanel::StartAnalysis(AppState& app) { lab_.Start(app); }

void MixLabPanel::Rebuild(AppState& app) {
    const Pattern& pat = app.project.patterns[static_cast<size_t>(std::clamp(lab_.Scene(), 0, kNumPatterns - 1))];
    if (haveBase_) baseRep_ = BuildMixReport(base_, lab_.Priority(), &pat);
    if (haveCur_) curRep_ = BuildMixReport(cur_, lab_.Priority(), &pat);
}

void MixLabPanel::Refresh(AppState& app) {
    const int g = lab_.Generation();
    if (g == seenGen_) return;
    seenGen_ = g;
    MixSpectra sp; MixReport rep;
    if (!lab_.Get(sp, rep)) return;
    cur_ = sp; curRep_ = rep; haveCur_ = true;
    if (lab_.TriedFixes().empty() || !haveBase_) { base_ = sp; baseRep_ = rep; haveBase_ = true; }
    (void)app;
}

void MixLabPanel::ReleaseSolo(AppState& app) {
    if (!soloSaved_) { soloSpot_ = -1; return; }
    Mixer& mx = app.engine.GetMixer();
    for (int t = 0; t < kNumTracks; ++t) { mx.tracks[static_cast<size_t>(t)].mute = savedMute_[static_cast<size_t>(t)]; mx.tracks[static_cast<size_t>(t)].solo = savedSolo_[static_cast<size_t>(t)]; }
    soloSaved_ = false; soloSpot_ = -1;
}

void MixLabPanel::Leave(AppState& app) { ReleaseSolo(app); lastFrame_ = -10; }

void MixLabPanel::ShowSpot(AppState& app, int spot) {
    Mixer& mx = app.engine.GetMixer();
    if (soloSpot_ == spot) { ReleaseSolo(app); app.SetStatus("MIX LAB: BACK TO THE FULL MIX"); return; }
    if (!soloSaved_) { for (int t = 0; t < kNumTracks; ++t) { savedMute_[static_cast<size_t>(t)] = mx.tracks[static_cast<size_t>(t)].mute; savedSolo_[static_cast<size_t>(t)] = mx.tracks[static_cast<size_t>(t)].solo; } soloSaved_ = true; }
    const MixReport& rep = baseRep_;
    if (spot < 0 || spot >= static_cast<int>(rep.spots.size())) return;
    for (int t = 0; t < kNumTracks; ++t) { mx.tracks[static_cast<size_t>(t)].solo = false; mx.tracks[static_cast<size_t>(t)].mute = false; }
    for (int t : rep.spots[static_cast<size_t>(spot)].tracks) mx.tracks[static_cast<size_t>(t)].solo = true;
    soloSpot_ = spot;
    app.SetStatus("SHOWING ONLY THE INSTRUMENTS OF THIS OVERLAP");
}

void MixLabPanel::ApplySnaps(AppState& app, bool after) {
    std::lock_guard<SceneMutex> lock(app.sequencer.PatternMutex());
    const int sc = std::clamp(lab_.Scene(), 0, kNumPatterns - 1);
    Pattern& pat = app.project.patterns[static_cast<size_t>(sc)];
    auto& tr = lab_.TriedFixes();
    if (after) for (const auto& t : tr) t.after.To(pat.tracks[static_cast<size_t>(t.track)]);
    else for (auto it = tr.rbegin(); it != tr.rend(); ++it) it->before.To(pat.tracks[static_cast<size_t>(it->track)]);
    if (app.sequencer.PlayPatternIndex() == sc || app.project.currentPattern == sc) app.engine.GetMixer().SyncFromPattern(pat, true);
}

void MixLabPanel::SetAB(AppState& app, bool b) {
    if (lab_.TriedFixes().empty()) { lab_.showB = false; return; }
    if (b == lab_.showB) return;
    lab_.showB = b;
    ApplySnaps(app, b);
    app.SetStatus(b ? "B: WITH THE FIXES" : "A: THE MIX AS IT WAS");
}

void MixLabPanel::TryFix(AppState& app, int spot) {
    if (spot < 0 || spot >= static_cast<int>(baseRep_.spots.size())) return;
    const MixSpot& sp = baseRep_.spots[static_cast<size_t>(spot)];
    if (sp.fix.kind == MixFix::Kind::None) return;
    auto& tr = lab_.TriedFixes();
    for (const auto& t : tr) if (t.spot == spot) return;   // already tried
    ReleaseSolo(app);
    const int sc = std::clamp(lab_.Scene(), 0, kNumPatterns - 1);
    if (tr.empty()) app.PushUndoForScene(sc);   // one undo step for everything tried (and kept)
    chan_ = sp.fix.track;
    {
        std::lock_guard<SceneMutex> lock(app.sequencer.PatternMutex());
        Pattern& pat = app.project.patterns[static_cast<size_t>(sc)];
        Track& t = pat.tracks[static_cast<size_t>(sp.fix.track)];
        MixLab::Tried e; e.track = sp.fix.track; e.spot = spot; e.before = MixLab::Snap::Of(t);
        MixApplyFix(t, sp.fix);
        e.after = MixLab::Snap::Of(t);
        tr.push_back(e);
        if (app.sequencer.PlayPatternIndex() == sc || app.project.currentPattern == sc) app.engine.GetMixer().SyncFromPattern(pat, true);
    }
    lab_.showB = true;
    app.SetStatus("TRYING: " + sp.fix.text + "  (A / B SWITCHES; KEEP FIXES TO STAY)");
    StartAnalysis(app);   // the picture follows the changed sound
}

void MixLabPanel::UndoAll(AppState& app) {
    auto& tr = lab_.TriedFixes();
    if (tr.empty()) return;
    if (lab_.showB) ApplySnaps(app, false); else { /* already A: the snapshots were put back */ }
    tr.clear(); lab_.showB = false;
    cur_ = base_; curRep_ = baseRep_;
    app.SetStatus("MIX LAB: ALL TRIED FIXES TAKEN BACK");
}

void MixLabPanel::EditStrip(AppState& app, int t, const std::function<void(Track&)>& fn) {
    if (t < 0 || t >= kNumTracks) return;
    ReleaseSolo(app);
    const int sc = std::clamp(lab_.Scene(), 0, kNumPatterns - 1);
    auto& tr = lab_.TriedFixes();
    std::lock_guard<SceneMutex> lock(app.sequencer.PatternMutex());
    Pattern& pat = app.project.patterns[static_cast<size_t>(sc)];
    Track& tk = pat.tracks[static_cast<size_t>(t)];
    MixLab::Tried* e = nullptr;
    for (auto it = tr.rbegin(); it != tr.rend(); ++it) if (it->track == t) { e = &*it; break; }
    if (!e) {
        if (tr.empty()) app.PushUndoForScene(sc);
        MixLab::Tried n; n.track = t; n.spot = -1; n.before = MixLab::Snap::Of(tk); n.after = n.before;
        tr.push_back(n); e = &tr.back();
    }
    fn(tk);
    e->after = MixLab::Snap::Of(tk);
    if (app.sequencer.PlayPatternIndex() == sc || app.project.currentPattern == sc) app.engine.GetMixer().SyncFromPattern(pat, true);
    lab_.showB = true;
}

void MixLabPanel::ApplyAll(AppState& app, int onlyTrack) {
    std::vector<int> ts;
    if (onlyTrack >= 0) ts.push_back(onlyTrack);
    else for (const auto& e : lab_.TriedFixes()) if (std::find(ts.begin(), ts.end(), e.track) == ts.end()) ts.push_back(e.track);
    if (ts.empty()) { app.SetStatus("MIX LAB: CHANGE SOMETHING FIRST (A FIX, A FADER OR THE EQ), THEN APPLY"); return; }
    if (onlyTrack < 0 && !lab_.showB) { lab_.showB = true; ApplySnaps(app, true); }   // the changed sound is what gets copied
    int scenes = 0;
    for (int t : ts) scenes = std::max(scenes, app.MatchLevelPanEq(lab_.Scene(), t, applyChainOnly_));
    if (onlyTrack < 0) { lab_.TriedFixes().clear(); lab_.showB = false; base_ = cur_; baseRep_ = curRep_; reanalyse_ = true; }
    std::string names;
    for (size_t i = 0; i < ts.size() && i < 3; ++i) names += (i ? ", " : "") + std::string(MixTrackName(ts[i]));
    if (ts.size() > 3) names += " +" + std::to_string(ts.size() - 3);
    app.SetStatus("MIX LAB: " + names + " (LEVEL, PAN, EQ) COPIED TO " + std::to_string(scenes) + (applyChainOnly_ ? " OTHER SCENE(S) OF THE TRACK" : " OTHER SCENE(S) AND BRICK(S)"));
}

void MixLabPanel::Draw(AppState& app, float width, float height) {
    const int frame = ImGui::GetFrameCount();
    if (lastFrame_ != frame - 1 && lastFrame_ != frame && !lab_.Busy() && lab_.TriedFixes().empty()) StartAnalysis(app);   // opened: look at the scene as it is now
    lastFrame_ = frame;
    Refresh(app);
    if (reanalyse_ && !lab_.Busy() && !ImGui::IsMouseDown(0)) { reanalyse_ = false; StartAnalysis(app); }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushFont(nullptr, 14.0f);

    // ---- top row
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(0.93f, 0.93f, 0.95f, 1), "MIX LAB");
    ImGui::SameLine(0, 10);
    ImGui::TextColored(ImVec4(0.56f, 0.57f, 0.64f, 1), "where instruments fight for the same frequencies");
    if (ImGui::IsItemHovered()) Hint("MIX LAB renders the played scene once, off to the side, and measures every instrument after its fader and pan. Nothing is saved unless you press KEEP FIXES.");
    ImGui::SameLine(width - 560.0f);
    char sc[64];
    {   // v44.45: a brick lives in a hidden slot (18, 19 ...); the person knows it as BRICK n
        const int b = lab_.Scene() >= kNumLiveScenes ? app.BrickOfSlot(lab_.Scene()) : -1;
        if (b >= 0) std::snprintf(sc, sizeof sc, "BRICK %d", b + 1); else std::snprintf(sc, sizeof sc, "SCENE %02d", lab_.Scene() + 1);
    }
    ImGui::TextColored(ImVec4(0.56f, 0.57f, 0.64f, 1), "%s", lab_.Busy() ? "ANALYSING ..." : sc);
    ImGui::SameLine(0, 12);
    if (ToggleButton("ANALYSE AGAIN", false, ImVec2(118, 0)) && !lab_.Busy()) { ReleaseSolo(app); StartAnalysis(app); }
    if (ImGui::IsItemHovered()) Hint("Listen to the scene again with the sound as it is now (the played scene, or the edited one when stopped).");
    ImGui::SameLine(0, 10);
    static const char* kFilt[3] = {"ALL", "DRUMS", "MELODIC"};
    for (int i = 0; i < 3; ++i) { if (i) ImGui::SameLine(0, 4); if (ToggleButton(kFilt[i], filter_ == i, ImVec2(i == 2 ? 74 : 54, 0), ToggleTab())) filter_ = i; }
    if (ImGui::IsItemHovered()) Hint("Which instruments are drawn on the map.");
    ImGui::SameLine(0, 10);
    if (ToggleButton(masking_ ? "OVERLAPS ON" : "OVERLAPS OFF", masking_, ImVec2(112, 0), ToggleTeal())) masking_ = !masking_;
    if (ImGui::IsItemHovered()) Hint("Red zones on the map = the places where two or more instruments are about equally loud.");
    ImGui::Dummy(ImVec2(0, 2));

    if (!haveBase_) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddText(ImVec2(p.x + 20, p.y + 30), vis::kTextDim, lab_.Busy() ? "Listening to the scene ..." : "Press ANALYSE AGAIN to listen to the scene.");
        ImGui::PopFont();
        return;
    }

    if (chan_ < 0 || !ShownSpectra().present[static_cast<size_t>(chan_)]) {
        chan_ = -1;
        for (int i = 0; i < kNumTracks && chan_ < 0; ++i) { const int t = lab_.Priority()[static_cast<size_t>(i)]; if (ShownSpectra().present[static_cast<size_t>(t)]) chan_ = t; }
    }
    const float gap = 8.0f;
    const float topH = ImGui::GetContentRegionAvail().y;
    const float rightW = std::clamp(width * 0.30f, 340.0f, 520.0f);
    const float leftW = width - rightW - gap - 12.0f;
    const float mapH = std::max(230.0f, topH * 0.62f);
    const float lowerH = topH - mapH - gap;
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    ImGui::BeginGroup();
    ImGui::BeginChild("MixMap", ImVec2(leftW, mapH), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawMap(app, leftW, mapH);
    ImGui::EndChild();
    const float balW = leftW * 0.31f, prioW = leftW * 0.19f, chanW = leftW - balW - prioW - 2 * gap;
    ImGui::BeginChild("MixBal", ImVec2(balW, lowerH), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawBalance(app, balW, lowerH);
    ImGui::EndChild();
    ImGui::SameLine(0, gap);
    ImGui::BeginChild("MixPrio", ImVec2(prioW, lowerH), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawPriority(app, prioW, lowerH);
    ImGui::EndChild();
    ImGui::SameLine(0, gap);
    ImGui::BeginChild("MixChan", ImVec2(chanW, lowerH), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawChannel(app, chanW, lowerH);
    ImGui::EndChild();
    ImGui::EndGroup();

    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    const float abH = 142.0f;
    ImGui::BeginChild("MixCards", ImVec2(rightW, topH - abH - gap), ImGuiChildFlags_Borders);
    DrawCards(app, rightW, topH - abH - gap);
    ImGui::EndChild();
    ImGui::BeginChild("MixAB", ImVec2(rightW, abH), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawAB(app, rightW, abH);
    ImGui::EndChild();
    ImGui::EndGroup();
    (void)origin;
    ImGui::PopFont();
}

// ---------------------------------------------------------------------------------------------------------------------------
void MixLabPanel::DrawMap(AppState& app, float w, float h) {
    (void)app;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const MixSpectra& sp = ShownSpectra();
    const MixReport& rep = ShownReport();
    const float nameW = 74.0f, padR = 14.0f;
    const float x0 = wp.x + nameW + 6.0f, x1 = wp.x + w - padR;
    const float plotY0 = wp.y + 26.0f;
    int nPresent = 0; for (int t = 0; t < kNumTracks; ++t) if (sp.present[static_cast<size_t>(t)]) ++nPresent;
    const float rowH = std::clamp((h - 26.0f - 24.0f) * 0.40f / kNumTracks, 11.0f, 20.0f);
    const float stripsH = rowH * kNumTracks;
    const float plotY1 = wp.y + h - stripsH - 44.0f;
    const float pw = x1 - x0, ph = plotY1 - plotY0;
    auto xOfBand = [&](float b) { return x0 + pw * b / static_cast<float>(kMixBands); };
    auto xOfHz = [&](float hz) { return xOfBand(MixHzToBand(hz)); };

    // the loudest smoothed level sets the top of the map
    std::array<std::array<float, kMixBands>, kNumTracks> sm{};
    float ref = -120.0f;
    for (int t = 0; t < kNumTracks; ++t) if (sp.present[static_cast<size_t>(t)]) { Smooth(sp.db[static_cast<size_t>(t)], sm[static_cast<size_t>(t)]); for (float v : sm[static_cast<size_t>(t)]) ref = std::max(ref, v); }
    std::array<float, kMixBands> mixSm{}; Smooth(sp.mixDb, mixSm); for (float v : mixSm) ref = std::max(ref, v);
    auto yOf = [&](float db) { return plotY1 - ph * std::clamp((db - (ref - kDbSpan)) / kDbSpan, 0.0f, 1.0f); };

    dl->AddRectFilled(ImVec2(x0, plotY0), ImVec2(x1, plotY1), vis::kInset, 3.0f);
    // zone labels + grid
    for (int z = 0; z < kMixZones; ++z) {
        const float a = xOfHz(kMixZoneEdges[z]), b = xOfHz(kMixZoneEdges[z + 1]);
        if (z > 0) dl->AddLine(ImVec2(a, plotY0), ImVec2(a, plotY1 + stripsH + 8.0f), vis::kGrid, 1.0f);
        const ImVec2 ts = ImGui::CalcTextSize(kMixZoneNames[z]);
        dl->AddText(ImVec2((a + b) * 0.5f - ts.x * 0.5f, wp.y + 6.0f), vis::kTextDim, kMixZoneNames[z]);
    }
    // overlap zones
    if (masking_) {
        for (size_t i = 0; i < rep.spots.size(); ++i) {
            const MixSpot& s = rep.spots[i];
            const float a = xOfHz(s.loHz), b = std::max(a + 4.0f, xOfHz(s.hiHz));
            const bool sel = lab_.selectedSpot == static_cast<int>(i);
            dl->AddRectFilled(ImVec2(a, plotY0), ImVec2(b, plotY1 + stripsH + 8.0f), IM_COL32(232, 76, 76, sel ? 70 : 42));
            dl->AddRectFilled(ImVec2(a, plotY0), ImVec2(b, plotY0 + 3.0f), IM_COL32(232, 76, 76, 220));
        }
    }
    // curves: filled columns (soft) + a line
    for (int t = 0; t < kNumTracks; ++t) {
        if (!sp.present[static_cast<size_t>(t)] || !InFilter(filter_, t)) continue;
        const ImU32 c = TrackColor(t);
        for (int b = 0; b < kMixBands; ++b) {
            const float yy = yOf(sm[static_cast<size_t>(t)][static_cast<size_t>(b)]);
            dl->AddRectFilled(ImVec2(xOfBand(static_cast<float>(b)), yy), ImVec2(xOfBand(static_cast<float>(b + 1)) + 0.5f, plotY1), ColorAlpha(c, 16));
        }
        for (int b = 0; b + 1 < kMixBands; ++b)
            dl->AddLine(ImVec2(xOfBand(b + 0.5f), yOf(sm[static_cast<size_t>(t)][static_cast<size_t>(b)])), ImVec2(xOfBand(b + 1.5f), yOf(sm[static_cast<size_t>(t)][static_cast<size_t>(b + 1)])), ColorAlpha(c, 210), 1.6f);
    }
    // the whole mix: dashed
    for (int b = 0; b + 1 < kMixBands; b += 2)
        dl->AddLine(ImVec2(xOfBand(b + 0.5f), yOf(mixSm[static_cast<size_t>(b)])), ImVec2(xOfBand(b + 1.5f), yOf(mixSm[static_cast<size_t>(b + 1)])), IM_COL32(240, 240, 245, 200), 1.3f);
    dl->AddText(ImVec2(x1 - 96.0f, plotY0 + 6.0f), vis::kTextDim, "- - the whole mix");
    // numbers on the spots
    if (masking_) for (size_t i = 0; i < rep.spots.size(); ++i) {
        const MixSpot& s = rep.spots[i];
        const float cx = std::clamp(0.5f * (xOfHz(s.loHz) + xOfHz(s.hiHz)), x0 + 10.0f, x1 - 10.0f), cy = plotY0 + 20.0f;
        char nb[8]; std::snprintf(nb, sizeof nb, "%d", static_cast<int>(i) + 1);
        dl->AddCircleFilled(ImVec2(cx, cy), 8.5f, IM_COL32(232, 76, 76, 255));
        const ImVec2 ts = ImGui::CalcTextSize(nb);
        dl->AddText(ImVec2(cx - ts.x * 0.5f, cy - ts.y * 0.5f), IM_COL32(255, 255, 255, 255), nb);
    }

    // who lives where: one strip per instrument, brightness = level
    const float sy0 = plotY1 + 22.0f;
    dl->AddText(ImVec2(x0, plotY1 + 5.0f), vis::kTextDim, "WHO LIVES WHERE (BRIGHTER = LOUDER THERE)");
    int row = 0;
    for (int t = 0; t < kNumTracks; ++t) {
        const float ry = sy0 + row * rowH;
        const bool on = sp.present[static_cast<size_t>(t)] && InFilter(filter_, t);
        dl->AddRectFilled(ImVec2(x0, ry + 1), ImVec2(x1, ry + rowH - 1), IM_COL32(20, 22, 28, 255));
        const ImU32 c = TrackColor(t);
        dl->AddCircleFilled(ImVec2(wp.x + 12.0f, ry + rowH * 0.5f), 3.5f, on ? c : ColorAlpha(c, 70));
        dl->AddText(ImVec2(wp.x + 22.0f, ry + (rowH - 13.0f) * 0.5f), on ? c : vis::kTextDim, MixTrackName(t));
        if (on) for (int b = 0; b < kMixBands; ++b) {
            const float a = std::clamp((sm[static_cast<size_t>(t)][static_cast<size_t>(b)] - (ref - 46.0f)) / 46.0f, 0.0f, 1.0f);
            if (a < 0.03f) continue;
            dl->AddRectFilled(ImVec2(xOfBand(static_cast<float>(b)), ry + 1), ImVec2(xOfBand(static_cast<float>(b + 1)) + 0.5f, ry + rowH - 1), ColorAlpha(c, static_cast<int>(30 + 215 * a * a)));
        } else if (!sp.present[static_cast<size_t>(t)]) dl->AddText(ImVec2(x0 + 8.0f, ry + (rowH - 13.0f) * 0.5f), IM_COL32(110, 114, 130, 255), "silent in this scene");
        ImGui::SetCursorScreenPos(ImVec2(wp.x + 4.0f, ry));
        ImGui::PushID(100 + t);
        if (ImGui::InvisibleButton("##sel", ImVec2(nameW + 2.0f, rowH)) && sp.present[static_cast<size_t>(t)]) chan_ = t;
        if (ImGui::IsItemHovered() && sp.present[static_cast<size_t>(t)]) Hint("Click to edit %s in CHANNEL (level, pan, EQ).", MixTrackName(t));
        ImGui::PopID();
        if (chan_ == t) dl->AddRect(ImVec2(wp.x + 4.0f, ry), ImVec2(x1, ry + rowH), ColorAlpha(c, 190), 2.0f);
        ++row;
    }
    // frequency axis
    static const float kTicks[] = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
    for (float f : kTicks) {
        const std::string l = HzLabel(f); const ImVec2 ts = ImGui::CalcTextSize(l.c_str());
        dl->AddText(ImVec2(std::min(xOfHz(f) - ts.x * 0.5f, x1 - ts.x + 6.0f), sy0 + stripsH + 3.0f), vis::kTextDim, l.c_str());
    }
    // the mouse: Hz and the loudest ones
    ImGui::SetCursorScreenPos(ImVec2(x0, plotY0));
    ImGui::InvisibleButton("##mapHit", ImVec2(pw, sy0 + stripsH - plotY0));
    if (ImGui::IsItemHovered()) {
        const float mx = ImGui::GetIO().MousePos.x;
        const float bpos = std::clamp((mx - x0) / pw * kMixBands, 0.0f, kMixBands - 0.01f);
        const float hz = MixBandEdgeHz(bpos);
        dl->AddLine(ImVec2(mx, plotY0), ImVec2(mx, sy0 + stripsH), IM_COL32(255, 255, 255, 90), 1.0f);
        const int b = static_cast<int>(bpos);
        std::vector<std::pair<float, int>> lv;
        for (int t = 0; t < kNumTracks; ++t) if (sp.present[static_cast<size_t>(t)] && InFilter(filter_, t)) lv.push_back({sm[static_cast<size_t>(t)][static_cast<size_t>(b)], t});
        std::sort(lv.begin(), lv.end(), [](const auto& a, const auto& c) { return a.first > c.first; });
        std::string who;
        for (size_t i = 0; i < lv.size() && i < 3; ++i) { if (lv[i].first < ref - 50.0f) break; who += (i ? ", " : "") + std::string(MixTrackName(lv[i].second)); }
        Hint("%s Hz: %s. %s", HzLabel(hz).c_str(), who.empty() ? "nothing much here" : ("loudest here: " + who).c_str(),
             (lv.size() > 1 && lv[0].first - lv[1].first < 4.0f && lv[1].first > ref - 40.0f) ? "The first two are about equally loud - that is an overlap." : "Click an overlap zone to open its card.");
        if (ImGui::IsMouseClicked(0)) {
            lab_.selectedSpot = -1;
            for (size_t i = 0; i < rep.spots.size(); ++i) if (hz >= rep.spots[i].loHz && hz <= rep.spots[i].hiHz) lab_.selectedSpot = static_cast<int>(i);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
void MixLabPanel::DrawCards(AppState& app, float w, float h) {
    (void)h;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const MixReport& rep = baseRep_;
    if (rep.spots.empty()) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddText(ImVec2(p.x + 8, p.y + 10), vis::kTeal, "No instruments are fighting.");
        WrapText(dl, ImVec2(p.x + 8, p.y + 32), w - 36.0f, vis::kTextDim, "Nothing is about equally loud in the same place. Check the tonal balance below, or change the scene and press ANALYSE AGAIN.", 17.0f, 4);
        ImGui::Dummy(ImVec2(w - 20, 100));
        return;
    }
    const float cw = w - 22.0f, ch = 142.0f;
    for (size_t i = 0; i < rep.spots.size(); ++i) {
        const MixSpot& s = rep.spots[i];
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool sel = lab_.selectedSpot == static_cast<int>(i);
        bool tried = false; for (const auto& t : lab_.TriedFixes()) if (t.spot == static_cast<int>(i)) tried = true;
        dl->AddRectFilled(p, ImVec2(p.x + cw, p.y + ch), IM_COL32(30, 32, 40, 255), 5.0f);
        dl->AddRect(p, ImVec2(p.x + cw, p.y + ch), sel ? vis::kAccent : (tried ? vis::kTeal : vis::kLine), 5.0f, 0, sel ? 1.8f : 1.0f);
        char nb[8]; std::snprintf(nb, sizeof nb, "%d", static_cast<int>(i) + 1);
        dl->AddCircleFilled(ImVec2(p.x + 17, p.y + 18), 9.5f, tried ? vis::kTeal : IM_COL32(232, 76, 76, 255));
        const ImVec2 nts = ImGui::CalcTextSize(nb);
        dl->AddText(ImVec2(p.x + 17 - nts.x * 0.5f, p.y + 18 - nts.y * 0.5f), IM_COL32(16, 18, 22, 255), nb);
        dl->AddText(ImVec2(p.x + 34, p.y + 9), vis::kText, s.title.c_str());
        char rng[48]; std::snprintf(rng, sizeof rng, "%s-%s Hz", HzLabel(s.loHz).c_str(), HzLabel(s.hiHz).c_str());
        const float tw = ImGui::CalcTextSize(s.title.c_str()).x;
        dl->AddText(ImVec2(p.x + 42 + tw, p.y + 9), vis::kTextDim, rng);
        // chips of who is involved (right aligned)
        float cx = p.x + cw - 8.0f;
        for (int k = static_cast<int>(std::min<size_t>(s.tracks.size(), 3)) - 1; k >= 0; --k) {
            const char* nm = MixTrackName(s.tracks[static_cast<size_t>(k)]);
            const float cwid = ImGui::CalcTextSize(nm).x + 14.0f; cx -= cwid;
            Chip(dl, ImVec2(cx, p.y + 28.0f), nm, TrackColor(s.tracks[static_cast<size_t>(k)])); cx -= 4.0f;
        }
        WrapText(dl, ImVec2(p.x + 12, p.y + 52), cw - 24.0f, IM_COL32(205, 208, 220, 255), s.advice, 16.0f, 5);
        // buttons
        ImGui::SetCursorScreenPos(ImVec2(p.x + cw - 232.0f, p.y + ch - 32.0f));
        const bool soloing = soloSpot_ == static_cast<int>(i);
        if (ToggleButton(soloing ? "BACK TO MIX" : "SHOW IT", soloing, ImVec2(100, 24), ToggleTab())) { lab_.selectedSpot = static_cast<int>(i); ShowSpot(app, static_cast<int>(i)); }
        if (ImGui::IsItemHovered()) Hint("Hear only the instruments that collide here (a temporary solo - it is put back when you press again or leave the tab).");
        ImGui::SameLine(0, 6);
        if (tried) {
            if (ToggleButton("TRIED", true, ImVec2(120, 24), ToggleTeal())) {}
            if (ImGui::IsItemHovered()) Hint("This fix is on. Use A / B below to compare, KEEP FIXES to stay, UNDO ALL to take everything back.");
        } else {
            if (ToggleButton("TRY IT (A / B)", false, ImVec2(120, 24), ToggleTeal()) && s.fix.kind != MixFix::Kind::None) { lab_.selectedSpot = static_cast<int>(i); TryFix(app, static_cast<int>(i)); }
            if (ImGui::IsItemHovered()) Hint("Make this one change now and listen: %s. Nothing is lost - A / B and UNDO ALL put it back.", s.fix.text.c_str());
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + ch + 8.0f));
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 2));
}

// ---------------------------------------------------------------------------------------------------------------------------
void MixLabPanel::DrawBalance(AppState& app, float w, float h) {
    (void)app;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const MixReport& rep = ShownReport();
    dl->AddText(ImVec2(wp.x + 10, wp.y + 7), vis::kTextDim, "TONAL BALANCE  -  THE WHOLE MIX");
    const float x0 = wp.x + 14.0f, x1 = wp.x + w - 14.0f;
    const float top = wp.y + 52.0f, bot = wp.y + h - 100.0f, mid = 0.5f * (top + bot);
    // v44.45: the scale grows with the data (12 dB at least, then steps of 6 up to 36), so a bar is never cut off while its label says +25 dB
    float worst = 12.0f;
    for (int z = 0; z < kMixZones; ++z) worst = std::max(worst, std::fabs(rep.balance.deltaDb[static_cast<size_t>(z)]));
    const float range = std::min(36.0f, std::ceil(worst / 6.0f) * 6.0f);
    const float scale = (bot - top) * 0.5f / range;
    dl->AddRectFilled(ImVec2(x0, mid - kMixToleranceDb * scale), ImVec2(x1, mid + kMixToleranceDb * scale), IM_COL32(95, 225, 201, 22));
    dl->AddLine(ImVec2(x0, mid), ImVec2(x1, mid), vis::kLineHi, 1.0f);
    dl->AddText(ImVec2(x0, wp.y + 27.0f), vis::kTextDim, "MORE");
    { const char* g = "GREEN = TYPICAL +/-3 dB"; dl->AddText(ImVec2(x1 - ImGui::CalcTextSize(g).x, wp.y + 27.0f), IM_COL32(95, 225, 201, 190), g); }
    dl->AddText(ImVec2(x0, bot + 3), vis::kTextDim, "LESS ENERGY");
    { char sc2[32]; std::snprintf(sc2, sizeof sc2, "+/-%.0f dB", static_cast<double>(range)); const float sw = ImGui::CalcTextSize(sc2).x; dl->AddText(ImVec2(x1 - sw, bot + 3), vis::kTextDim, sc2); }
    const float cell = (x1 - x0) / kMixZones;
    for (int z = 0; z < kMixZones; ++z) {
        const float cx = x0 + cell * (z + 0.5f), bw = std::min(54.0f, cell * 0.55f);
        const float d = std::clamp(rep.balance.deltaDb[static_cast<size_t>(z)], -range, range);
        const bool off = rep.balance.off[static_cast<size_t>(z)];
        const ImU32 col = !off ? IM_COL32(95, 225, 201, 150) : (d > 0 ? IM_COL32(236, 128, 76, 235) : IM_COL32(86, 148, 232, 235));
        const float y = mid - d * scale;
        dl->AddRectFilled(ImVec2(cx - bw * 0.5f, std::min(y, mid)), ImVec2(cx + bw * 0.5f, std::max(y, mid) + 1.0f), col, 2.0f);
        if (lab_.selectedZone == z) dl->AddRect(ImVec2(cx - bw * 0.5f - 3, top - 2), ImVec2(cx + bw * 0.5f + 3, bot + 2), vis::kAccent, 3.0f);
        char lb[24]; if (z == 3) std::snprintf(lb, sizeof lb, "anchor"); else if (off) std::snprintf(lb, sizeof lb, "%+.0f dB", static_cast<double>(rep.balance.deltaDb[static_cast<size_t>(z)])); else std::snprintf(lb, sizeof lb, "ok");
        const ImVec2 ts = ImGui::CalcTextSize(lb);
        dl->AddText(ImVec2(cx - ts.x * 0.5f, d >= 0 ? std::max(y - 16.0f, top - 6.0f) : std::min(y + 3.0f, bot - 4.0f)), off ? vis::kText : vis::kTeal, lb);
        const ImVec2 zs = ImGui::CalcTextSize(kMixZoneNames[z]);
        dl->AddText(ImVec2(cx - zs.x * 0.5f, bot + 18.0f), vis::kTextDim, kMixZoneNames[z]);
        ImGui::SetCursorScreenPos(ImVec2(cx - cell * 0.5f, top));
        ImGui::PushID(z);
        if (ImGui::InvisibleButton("##bar", ImVec2(cell, bot - top))) lab_.selectedZone = lab_.selectedZone == z ? -1 : z;
        if (ImGui::IsItemHovered()) Hint("%s (%s-%s Hz): %+.1f dB against a typical track. Click to see who is responsible.", kMixZoneNames[z], HzLabel(kMixZoneEdges[z]).c_str(), HzLabel(kMixZoneEdges[z + 1]).c_str(), static_cast<double>(rep.balance.deltaDb[static_cast<size_t>(z)]));
        ImGui::PopID();
    }
    // the sentence under the bars
    std::string line = "Compared with where a typical finished track sits in each band. A guide, not a rule: a dark ambient track may be dark.";
    if (lab_.selectedZone >= 0) {
        const int z = lab_.selectedZone;
        std::vector<std::pair<float, int>> sh;
        for (int t = 0; t < kNumTracks; ++t) if (rep.balance.share[static_cast<size_t>(z)][static_cast<size_t>(t)] > 0.03f) sh.push_back({rep.balance.share[static_cast<size_t>(z)][static_cast<size_t>(t)], t});
        std::sort(sh.begin(), sh.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        line = std::string(kMixZoneNames[z]) + " is made by:";
        for (size_t i = 0; i < sh.size() && i < 4; ++i) { char b[48]; std::snprintf(b, sizeof b, " %s %.0f%%", MixTrackName(sh[i].second), static_cast<double>(sh[i].first * 100.0f)); line += (i ? "," : "") + std::string(b); }
        line += ".";
    }
    WrapText(dl, ImVec2(wp.x + 12, wp.y + h - 56.0f), w - 24.0f, vis::kTextDim, line, 15.0f, 3);
}

// ---------------------------------------------------------------------------------------------------------------------------
void MixLabPanel::DrawPriority(AppState& app, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    dl->AddText(ImVec2(wp.x + 10, wp.y + 7), vis::kTextDim, "WHO KEEPS THE SPACE");
    auto& pr = lab_.Priority();
    const float top = wp.y + 28.0f;
    const float rowH = std::clamp((h - 28.0f - 38.0f) / kNumTracks, 14.0f, 24.0f);
    int moveFrom = -1, moveTo = -1;
    for (int i = 0; i < kNumTracks; ++i) {
        const int t = pr[static_cast<size_t>(i)];
        const float ry = top + rowH * i;
        ImGui::SetCursorScreenPos(ImVec2(wp.x + 8.0f, ry));
        ImGui::PushID(i);
        ImGui::InvisibleButton("##row", ImVec2(w - 16.0f, rowH - 1.0f));
        const bool hov = ImGui::IsItemHovered();
        dl->AddRectFilled(ImVec2(wp.x + 8, ry), ImVec2(wp.x + w - 8, ry + rowH - 1), hov ? vis::kCellHover : vis::kCell, 3.0f);
        char nb[8]; std::snprintf(nb, sizeof nb, "%d", i + 1);
        dl->AddText(ImVec2(wp.x + 16, ry + (rowH - 14.0f) * 0.5f), vis::kTextDim, nb);
        dl->AddCircleFilled(ImVec2(wp.x + 48, ry + rowH * 0.5f), 4.0f, TrackColor(t));
        dl->AddText(ImVec2(wp.x + 60, ry + (rowH - 14.0f) * 0.5f), vis::kText, MixTrackName(t));
        for (int k = 0; k < 3; ++k) dl->AddLine(ImVec2(wp.x + w - 28, ry + rowH * 0.5f - 3 + k * 3), ImVec2(wp.x + w - 16, ry + rowH * 0.5f - 3 + k * 3), vis::kLineHi, 1.5f);
        if (hov) Hint("Drag to change. When two instruments collide, the one lower in the list is the one asked to make room.");
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            ImGui::SetDragDropPayload("MIXPRIO", &i, sizeof(int));
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TrackColor(t)), "%s", MixTrackName(t));
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("MIXPRIO")) { moveFrom = *static_cast<const int*>(pl->Data); moveTo = i; }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
    }
    if (moveFrom >= 0 && moveTo >= 0 && moveFrom != moveTo) {
        const int v = pr[static_cast<size_t>(moveFrom)];
        if (moveFrom < moveTo) for (int k = moveFrom; k < moveTo; ++k) pr[static_cast<size_t>(k)] = pr[static_cast<size_t>(k + 1)];
        else for (int k = moveFrom; k > moveTo; --k) pr[static_cast<size_t>(k)] = pr[static_cast<size_t>(k - 1)];
        pr[static_cast<size_t>(moveTo)] = v;
        Rebuild(app);
        app.SetStatus("MIX LAB: THE LIST CHANGED WHO MAKES ROOM");
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
void MixLabPanel::DrawAB(AppState& app, float w, float h) {
    (void)h;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    dl->AddText(ImVec2(wp.x + 10, wp.y + 7), vis::kTextDim, "AFTER THE CHANGES  (A / B)");
    const bool any = !lab_.TriedFixes().empty();
    ImGui::SetCursorScreenPos(ImVec2(wp.x + 10, wp.y + 28));
    const float bw = (w - 20.0f - 18.0f) / 4.0f;
    if (ToggleButton("A  now", any && !lab_.showB, ImVec2(bw, 24), ToggleTab())) SetAB(app, false);
    if (ImGui::IsItemHovered()) Hint("The mix as it was before the tried fixes. Switches instantly, so you can hear it.");
    ImGui::SameLine(0, 6);
    if (ToggleButton("B  with fixes", any && lab_.showB, ImVec2(bw, 24), ToggleTab())) SetAB(app, true);
    ImGui::SameLine(0, 6);
    if (ToggleButton("KEEP FIXES", false, ImVec2(bw, 24), ToggleTeal()) && any) {
        if (!lab_.showB) { lab_.showB = true; ApplySnaps(app, true); }
        lab_.TriedFixes().clear(); lab_.showB = false;
        base_ = cur_; baseRep_ = curRep_;
        app.SetStatus("MIX LAB: FIXES KEPT (UNDO BRINGS THEM BACK OUT)");
    }
    if (ImGui::IsItemHovered()) Hint("Stay with the changes. They are ordinary EQ / level / pan settings of those instruments - Undo takes them out again.");
    ImGui::SameLine(0, 6);
    if (ToggleButton("UNDO ALL", false, ImVec2(bw, 24), ToggleRed()) && any) UndoAll(app);
    if (ImGui::IsItemHovered()) Hint("Put every instrument back to how it was before you pressed TRY IT.");
    ImGui::SetCursorScreenPos(ImVec2(wp.x + 10, wp.y + 58));
    if (ToggleButton("APPLY TO ALL", false, ImVec2(w * 0.40f, 24), ToggleTeal())) ApplyAll(app, -1);
    if (ImGui::IsItemHovered()) Hint("Keep what you changed here and copy those instruments' LEVEL, PAN and EQ into every other scene and brick, so the whole track gets the same fix. Sends, FX and notes are not touched. Each scene gets its own Undo step.");
    ImGui::SameLine(0, 6);
    if (ToggleButton(applyChainOnly_ ? "ONLY THE TRACK" : "ALL SCENES + BRICKS", applyChainOnly_, ImVec2(w - 20.0f - w * 0.40f - 6.0f, 24), ToggleTab())) applyChainOnly_ = !applyChainOnly_;
    if (ImGui::IsItemHovered()) Hint("Where APPLY TO ALL writes: every scene and brick, or only the scenes the track actually plays.");
    std::string line;
    if (any) {
        char b[160];
        const float dl2 = curRep_.valid ? cur_.mixRmsDb - base_.mixRmsDb : 0.0f;
        std::snprintf(b, sizeof b, "Overlaps: %d -> %d.  Loudness of the mix: %+.1f dB.  Nothing is saved until you press KEEP FIXES.", static_cast<int>(baseRep_.spots.size()), static_cast<int>(curRep_.spots.size()), static_cast<double>(dl2));
        line = lab_.Busy() ? "Listening again ..." : b;
    } else line = "Change an instrument below (fader, EQ) or press TRY IT on a card. A / B switches before / after; APPLY TO ALL copies it everywhere.";
    WrapText(dl, ImVec2(wp.x + 10, wp.y + 90), w - 20.0f, vis::kTextDim, line, 15.0f, 3);
}


// ---------------------------------------------------------------------------------------------------------------------------
// CHANNEL (v44.42): the strip of one instrument -- level, pan and the four-band channel EQ -- drawn over what that instrument contains,
// with the overlap zones it is part of, so a cut lands where the fight is. Every edit goes into the analysed scene's track (one Undo step,
// A / B, KEEP / UNDO ALL like a fix); APPLY TO ALL copies it into the other scenes and bricks.
void MixLabPanel::DrawChannel(AppState& app, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const MixSpectra& sp = ShownSpectra();
    const MixReport& rep = ShownReport();
    const int sc = std::clamp(lab_.Scene(), 0, kNumPatterns - 1);
    if (chan_ < 0) { dl->AddText(ImVec2(wp.x + 10, wp.y + 8), vis::kTextDim, "CHANNEL"); return; }
    const Track& tk = app.project.patterns[static_cast<size_t>(sc)].tracks[static_cast<size_t>(chan_)];
    const ImU32 col = TrackColor(chan_);

    // header: CHANNEL  < name >     FLAT EQ   TO ALL SCENES
    dl->AddText(ImVec2(wp.x + 10, wp.y + 8), vis::kTextDim, "CHANNEL");
    auto step = [&](int dir) { int t = chan_; for (int k = 0; k < kNumTracks; ++k) { t = (t + dir + kNumTracks) % kNumTracks; if (sp.present[static_cast<size_t>(t)]) { chan_ = t; return; } } };
    ImGui::SetCursorScreenPos(ImVec2(wp.x + 70, wp.y + 4));
    ImGui::PushID("chan");
    if (ToggleButton("<", false, ImVec2(22, 22))) step(-1);
    ImGui::SameLine(0, 4);
    if (ToggleButton(">", false, ImVec2(22, 22))) step(1);
    ImGui::PopID();
    float cw0 = 0.0f; Chip(dl, ImVec2(wp.x + 126, wp.y + 5), MixTrackName(chan_), col, &cw0);
    const float bx = wp.x + w - 10.0f;
    ImGui::SetCursorScreenPos(ImVec2(bx - 112.0f - 6.0f - 64.0f, wp.y + 4));
    if (ToggleButton("FLAT EQ", false, ImVec2(64, 22))) EditStrip(app, chan_, [](Track& t) { t.eqHpfHz = 20.0f; t.eqLowDb = 0.0f; t.eqMidDb = 0.0f; t.eqHighDb = 0.0f; });
    if (ImGui::IsItemHovered()) Hint("Take the EQ of this instrument back to flat (it stays an A / B change until you keep it).");
    ImGui::SameLine(0, 6);
    if (ToggleButton("TO ALL SCENES", false, ImVec2(112, 22), ToggleTeal())) ApplyAll(app, chan_);
    if (ImGui::IsItemHovered()) Hint("Copy this instrument's LEVEL, PAN and EQ into every other scene and brick (see the scope switch under A / B). Each scene gets its own Undo step.");

    // EQ curve over the instrument's spectrum
    const float ex0 = wp.x + 10.0f, ex1 = wp.x + w - 10.0f, ey0 = wp.y + 34.0f, ey1 = wp.y + h - 70.0f;
    const float cw = ex1 - ex0, ch = ey1 - ey0;
    const float lo20 = std::log10(20.0f), span = std::log10(20000.0f) - lo20;
    auto fx = [&](float f) { return ex0 + (std::log10(std::clamp(f, 20.0f, 20000.0f)) - lo20) / span * cw; };
    auto gy = [&](float db) { return ey0 + ch * 0.5f - std::clamp(db, -kEqMaxDb, kEqMaxDb) / kEqMaxDb * (ch * 0.5f - 4.0f); };
    dl->AddRectFilled(ImVec2(ex0, ey0), ImVec2(ex1, ey1), vis::kInset, 4.0f);
    for (const MixSpot& s : rep.spots) {
        if (std::find(s.tracks.begin(), s.tracks.end(), chan_) == s.tracks.end()) continue;
        dl->AddRectFilled(ImVec2(fx(s.loHz), ey0), ImVec2(std::max(fx(s.loHz) + 3.0f, fx(s.hiHz)), ey1), IM_COL32(232, 76, 76, 46));
    }
    if (sp.present[static_cast<size_t>(chan_)]) {
        std::array<float, kMixBands> sm{}; Smooth(sp.db[static_cast<size_t>(chan_)], sm);
        float pk = -120.0f; for (float v : sm) pk = std::max(pk, v);
        for (int b = 0; b < kMixBands; ++b) {
            const float a = std::clamp((sm[static_cast<size_t>(b)] - (pk - 48.0f)) / 48.0f, 0.0f, 1.0f);
            const float xa = fx(MixBandEdgeHz(static_cast<float>(b))), xb = fx(MixBandEdgeHz(static_cast<float>(b + 1)));
            dl->AddRectFilled(ImVec2(xa, ey1 - (ch - 6.0f) * a), ImVec2(xb + 0.5f, ey1), ColorAlpha(col, 46));
        }
    }
    for (float f : {50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f}) {
        dl->AddLine(ImVec2(fx(f), ey0 + 1), ImVec2(fx(f), ey1 - 1), IM_COL32(255, 255, 255, 14), 1.0f);
        const std::string l = HzLabel(f); dl->AddText(ImVec2(fx(f) + 2.0f, ey1 - 14.0f), IM_COL32(120, 124, 140, 255), l.c_str());
    }
    for (float d : {-10.0f, 10.0f}) dl->AddLine(ImVec2(ex0 + 1, gy(d)), ImVec2(ex1 - 1, gy(d)), IM_COL32(255, 255, 255, 12), 1.0f);
    dl->AddLine(ImVec2(ex0 + 1, gy(0)), ImVec2(ex1 - 1, gy(0)), vis::kLineHi, 1.0f);
    dl->AddText(ImVec2(ex0 + 3, ey0 + 1), vis::kTextDim, "+15 dB");
    const TrackEqParams ep = TrackEqOf(tk);
    const double sr = std::max(8000, app.engine.SampleRate());
    {
        std::vector<ImVec2> pts(121);
        for (int i = 0; i <= 120; ++i) { const float f = std::pow(10.0f, lo20 + span * i / 120.0f); pts[static_cast<size_t>(i)] = ImVec2(ex0 + cw * i / 120.0f, gy(static_cast<float>(TrackEqResponseDb(ep, sr, f)))); }
        dl->PushClipRect(ImVec2(ex0, ey0), ImVec2(ex1, ey1), true);
        dl->AddPolyline(pts.data(), 121, ep.Flat() ? vis::kTextDim : vis::kAccent, ImDrawFlags_None, 2.0f);
        dl->PopClipRect();
    }
    struct Node { float f; bool on; };
    const Node nodes[4] = {{ep.hpfHz, ep.HpfOn()}, {ep.lowHz, ep.lowDb != 0.0f}, {ep.midHz, ep.midDb != 0.0f}, {ep.highHz, ep.highDb != 0.0f}};
    static const ImU32 kNodeCol[4] = {IM_COL32(240, 110, 110, 255), IM_COL32(95, 220, 200, 255), IM_COL32(242, 163, 61, 255), IM_COL32(150, 170, 255, 255)};
    static const char* kNodeName[4] = {"HPF", "LOW", "MID", "HIGH"};
    ImVec2 npos[4];
    for (int n = 0; n < 4; ++n) {
        npos[n] = ImVec2(fx(nodes[n].f), gy(static_cast<float>(TrackEqResponseDb(ep, sr, nodes[n].f))));
        if (n == 0 && !nodes[0].on) npos[0] = ImVec2(fx(20.0f) + 6.0f, gy(0));
        if (n != 0 && !nodes[n].on) npos[n].y = gy(0);
    }
    ImGui::SetCursorScreenPos(ImVec2(ex0, ey0));
    ImGui::InvisibleButton("##eqhit", ImVec2(cw, ch));
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 mp = ImGui::GetIO().MousePos;
    auto nearest = [&]() { int best = 0; float bd = 1e9f; for (int n = 0; n < 4; ++n) { const float d = std::fabs(mp.x - npos[n].x) + 0.35f * std::fabs(mp.y - npos[n].y); if (d < bd) { bd = d; best = n; } } return best; };
    const int hoverNode = hov ? nearest() : -1;
    for (int n = 0; n < 4; ++n) {
        const float r = (n == hoverNode || n == eqDrag_) ? 6.0f : 4.5f;
        if (nodes[n].on) dl->AddCircleFilled(npos[n], r, kNodeCol[n]); else dl->AddCircle(npos[n], r, kNodeCol[n], 0, 1.5f);
    }
    if (ImGui::IsItemActivated()) eqDrag_ = nearest();
    if (ImGui::IsMouseDoubleClicked(0) && hov && eqDrag_ >= 0) {
        const int n = eqDrag_;
        EditStrip(app, chan_, [n](Track& t) { if (n == 0) t.eqHpfHz = 20.0f; else if (n == 1) t.eqLowDb = 0.0f; else if (n == 2) t.eqMidDb = 0.0f; else t.eqHighDb = 0.0f; });
        eqDrag_ = -1; reanalyse_ = true;
    } else if (ImGui::IsItemActive() && eqDrag_ >= 0) {
        const float f = std::pow(10.0f, lo20 + span * std::clamp((mp.x - ex0) / cw, 0.0f, 1.0f));
        float db = std::clamp((ey0 + ch * 0.5f - mp.y) / (ch * 0.5f - 4.0f) * kEqMaxDb, -kEqMaxDb, kEqMaxDb);
        db = std::round(db * 2.0f) / 2.0f;
        const int n = eqDrag_;
        EditStrip(app, chan_, [n, f, db](Track& t) {
            switch (n) {
                case 0: t.eqHpfHz = f < 22.0f ? 20.0f : std::clamp(f, 20.0f, 500.0f); break;
                case 1: t.eqLowHz = std::clamp(f, 30.0f, 500.0f); t.eqLowDb = db; break;
                case 2: t.eqMidHz = std::clamp(f, 80.0f, 10000.0f); t.eqMidDb = db; break;
                default: t.eqHighHz = std::clamp(f, 1500.0f, 16000.0f); t.eqHighDb = db; break;
            }
        });
    }
    if (ImGui::IsItemDeactivated() && eqDrag_ >= 0) { eqDrag_ = -1; reanalyse_ = true; }
    if (hov) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f && hoverNode == 2) EditStrip(app, chan_, [wheel](Track& t) { t.eqMidQ = std::clamp(t.eqMidQ * (wheel > 0 ? 1.12f : 1.0f / 1.12f), 0.3f, 6.0f); });
        if (hoverNode >= 0) {
            const float hz = hoverNode == 0 ? ep.hpfHz : hoverNode == 1 ? ep.lowHz : hoverNode == 2 ? ep.midHz : ep.highHz;
            const float gdb = hoverNode == 1 ? ep.lowDb : hoverNode == 2 ? ep.midDb : hoverNode == 3 ? ep.highDb : 0.0f;
            if (hoverNode == 0) Hint("%s: HPF %s Hz%s. Drag = move. Double-click = off.", MixTrackName(chan_), HzLabel(hz).c_str(), ep.HpfOn() ? "" : " (off)");
            else Hint("%s %s: %s Hz, %+.1f dB%s. Drag = frequency and gain. %sDouble-click = flat.", MixTrackName(chan_), kNodeName[hoverNode], HzLabel(hz).c_str(), static_cast<double>(gdb), hoverNode == 2 ? (", Q " + std::to_string(ep.midQ).substr(0, 4)).c_str() : "", hoverNode == 2 ? "Wheel = width. " : "");
        }
        if (ImGui::IsMouseReleased(0) && eqDrag_ < 0) {}
    }

    // level and pan
    auto slider = [&](const char* id, float x, float y, float sw, float v, float lo, float hi, const char* label, const std::string& text, float def, const std::function<void(float)>& set) {
        dl->AddText(ImVec2(x, y + 1), vis::kTextDim, label);
        const float tx = x + 40.0f, tw = sw - 40.0f - 52.0f;
        const float u = std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f);
        dl->AddRectFilled(ImVec2(tx, y + 7), ImVec2(tx + tw, y + 11), vis::kInset, 2.0f);
        const float x0z = tx + tw * std::clamp((0.0f - lo) / (hi - lo), 0.0f, 1.0f);
        if (lo < 0.0f) dl->AddRectFilled(ImVec2(std::min(x0z, tx + tw * u), y + 7), ImVec2(std::max(x0z, tx + tw * u), y + 11), col, 2.0f);
        else dl->AddRectFilled(ImVec2(tx, y + 7), ImVec2(tx + tw * u, y + 11), col, 2.0f);
        dl->AddCircleFilled(ImVec2(tx + tw * u, y + 9), 6.0f, vis::kText);
        dl->AddText(ImVec2(tx + tw + 8.0f, y + 1), vis::kText, text.c_str());
        ImGui::SetCursorScreenPos(ImVec2(tx - 6.0f, y));
        ImGui::InvisibleButton(id, ImVec2(tw + 12.0f, 18.0f));
        if (ImGui::IsItemActive()) {
            const float k = ImGui::GetIO().KeyShift ? 0.25f : 1.0f; (void)k;
            set(std::clamp(lo + (hi - lo) * std::clamp((ImGui::GetIO().MousePos.x - tx) / tw, 0.0f, 1.0f), lo, hi));
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { set(def); reanalyse_ = true; }
        if (ImGui::IsItemDeactivated()) reanalyse_ = true;
        return ImGui::IsItemHovered();
    };
    const float sy = wp.y + h - 58.0f, sw = (w - 28.0f) * 0.5f;
    char vb[24]; std::snprintf(vb, sizeof vb, "%.0f%%", static_cast<double>(tk.volume * 100.0f));
    if (slider("##lvl", wp.x + 10.0f, sy, sw, tk.volume, 0.0f, 1.0f, "LEVEL", vb, 0.8f, [&](float v) { EditStrip(app, chan_, [v](Track& t) { t.volume = std::round(v * 100.0f) / 100.0f; }); }))
        Hint("%s level. Drag to change, double-click = 80%%. The picture follows when you let go.", MixTrackName(chan_));
    char pb[24]; if (std::fabs(tk.pan) < 0.005f) std::snprintf(pb, sizeof pb, "C"); else std::snprintf(pb, sizeof pb, "%s %.0f", tk.pan < 0 ? "L" : "R", static_cast<double>(std::fabs(tk.pan) * 100.0f));
    if (slider("##pan", wp.x + 18.0f + sw, sy, sw, tk.pan, -1.0f, 1.0f, "PAN", pb, 0.0f, [&](float v) { EditStrip(app, chan_, [v](Track& t) { t.pan = std::fabs(v) < 0.04f ? 0.0f : std::round(v * 100.0f) / 100.0f; }); }))
        Hint("%s pan. Drag to change, double-click = centre.", MixTrackName(chan_));

    // where this instrument is part of a fight
    std::string line = "Not part of any overlap.";
    for (size_t i = 0; i < rep.spots.size(); ++i) {
        const MixSpot& s = rep.spots[i];
        if (std::find(s.tracks.begin(), s.tracks.end(), chan_) == s.tracks.end()) continue;
        char b[160]; std::snprintf(b, sizeof b, "In overlap %d (%s-%s Hz): %s.", static_cast<int>(i) + 1, HzLabel(s.loHz).c_str(), HzLabel(s.hiHz).c_str(), s.yielder == chan_ ? "this one is asked to make room - cut in the red zone" : "this one keeps the space");
        line = b; break;
    }
    WrapText(dl, ImVec2(wp.x + 12, wp.y + h - 32.0f), w - 24.0f, vis::kTextDim, line, 15.0f, 2);
}

} // namespace t8::desktop
