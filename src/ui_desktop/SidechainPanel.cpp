#include "SidechainPanel.h"
#include "Visual.h"
#include "TrackStyle.h"
#include "LiveWidgets.h"
#include "InfoLine.h"
#include "ValueField.h"
#include "KnobWidgets.h"
#include "core/MixAnalysis.h"
#include "core/Pump.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace t8::desktop {

bool PumpsDiffer(const std::array<PumpSettings, kNumPumps>& a, const std::array<PumpSettings, kNumPumps>& b) {
    for (int p = 0; p < kNumPumps; ++p) {
        const PumpSettings &x = a[static_cast<size_t>(p)], &y = b[static_cast<size_t>(p)];
        if (x.on != y.on || x.source != y.source || x.attackMs != y.attackMs || x.holdMs != y.holdMs || x.releaseMs != y.releaseMs ||
            x.releaseDiv != y.releaseDiv || x.curve != y.curve || x.depth != y.depth) return true;
    }
    return false;
}

bool PumpInUse(const PumpSettings& p) {
    if (p.on) return true;
    for (float d : p.depth) if (d > 0.0f) return true;
    return false;
}

const char* PumpSourceName(int source) {
    if (source == kPumpSourceEvery4) return "Every 1/4";
    if (source == kPumpSourceEvery8) return "Every 1/8";
    if (source == kPumpSourceEvery16) return "Every 1/16";
    return MixTrackName(source);
}

namespace {

constexpr ImU32 kPumpAmber = IM_COL32(226, 160, 74, 255);
constexpr float kHeadH = 226.0f;

// Hits of a pump's source over the first `steps` grid steps of the edited scene (the instrument's active steps, or the beat).
std::vector<int> SourceHits(const Pattern& pat, const PumpSettings& p, int steps, int spb) {
    std::vector<int> hits;
    for (int s = 0; s < steps; ++s) {
        bool hit = false;
        if (p.source >= kPumpSourceEvery4) {
            const int perBeat = p.source == kPumpSourceEvery4 ? 1 : (p.source == kPumpSourceEvery8 ? 2 : 4);
            hit = (s * perBeat) % std::max(1, spb) == 0;
        } else if (p.source >= 0 && p.source < kNumTracks) {
            const Track& t = pat.tracks[static_cast<size_t>(p.source)];
            const int len = std::max(1, t.length > 0 ? t.length : pat.masterLength);
            hit = t.steps[static_cast<size_t>(s % std::min(len, kMaxSteps))].active;
        }
        if (hit) hits.push_back(s);
    }
    return hits;
}

// The envelope of one pump over `n` points spanning `steps` grid steps, with hits at `hits` (simulated at a low rate).
std::vector<float> SimEnvelope(const PumpSettings& p, const std::vector<int>& hits, int steps, double samplesPerStep, double samplesPerSixteenth,
                               float rate, int n) {
    std::vector<float> out(static_cast<size_t>(n), 0.0f);
    PumpEnvelope env;
    const PumpTiming tm = MakePumpTiming(p, rate, samplesPerSixteenth);
    const int total = std::max(1, static_cast<int>(steps * samplesPerStep));
    size_t hi = 0;
    // run one pass first so a long RELEASE from the end of the window carries into its start, as it does in a loop
    for (int pass = 0; pass < 2; ++pass) {
        hi = 0;
        for (int i = 0; i < total; ++i) {
            while (hi < hits.size() && static_cast<int>(hits[hi] * samplesPerStep) <= i) { env.Trigger(); ++hi; }
            const float v = env.Next(tm);
            if (pass == 1) out[static_cast<size_t>(std::min(n - 1, static_cast<int>(static_cast<long long>(i) * n / total)))] = std::max(out[static_cast<size_t>(std::min(n - 1, static_cast<int>(static_cast<long long>(i) * n / total)))], v);
        }
    }
    return out;
}

}   // namespace

// ---------------------------------------------------------------------------------------------------------------------------
void SidechainPanel::Draw(AppState& app, float width, float height) {
    std::array<PumpSettings, kNumPumps> np = app.project.fx.pumps;   // edited here, committed once at the end
    int used = 0;
    for (int p = 0; p < kNumPumps; ++p) if (PumpInUse(np[static_cast<size_t>(p)])) used = p + 1;
    const int shown = std::clamp(std::max(1, used) + shownExtra_, 1, kNumPumps);
    if (shown <= used) shownExtra_ = 0;

    const float gap = 8.0f;
    const float rightW = std::clamp(width * 0.30f, 340.0f, 460.0f);
    const float leftW = width - rightW - gap - 12.0f;
    const float nameW = 104.0f;
    const float colW = std::min(240.0f, (leftW - nameW - 30.0f) / static_cast<float>(kNumPumps));

    ImGui::BeginChild("ScLeft", ImVec2(leftW, height), ImGuiChildFlags_Borders);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kPumpAmber), "SIDECHAIN - one hit turns other instruments down for a moment");
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::Dummy(ImVec2(nameW - 8.0f, kHeadH));
    for (int p = 0; p < shown; ++p) {
        ImGui::SameLine(0, p == 0 ? 0.0f : 6.0f);
        ImGui::PushID(p);
        DrawPump(app, np, p, colW, kHeadH);
        ImGui::PopID();
    }
    if (shown < kNumPumps) {
        ImGui::SameLine(0, 6.0f);
        if (ImGui::Button("+ ADD PUMP\n(up to 4)", ImVec2(colW * 0.6f, kHeadH))) ++shownExtra_;
        if (ImGui::IsItemHovered()) Hint("Another pump: its own source, shape and instruments. Up to 4.");
    }
    ImGui::Dummy(ImVec2(0, 6));
    DrawCells(app, np, shown, nameW, colW);
    ImGui::EndChild();

    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    const float curveH = 230.0f, presetH = 150.0f;
    ImGui::BeginChild("ScCurve", ImVec2(rightW, curveH), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawCurve(app, np, rightW, curveH);
    ImGui::EndChild();
    ImGui::BeginChild("ScPresets", ImVec2(rightW, presetH), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawPresets(app, np, rightW);
    ImGui::EndChild();
    ImGui::BeginChild("ScHow", ImVec2(rightW, std::max(60.0f, height - curveH - presetH - 2 * gap)), ImGuiChildFlags_Borders);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kPumpAmber), "HOW TO USE");
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rightW - 24.0f);
    ImGui::TextDisabled("1. Pick a SOURCE: any instrument (each hit fires the pump, even when it is muted) or a steady beat "
                        "(1/4, 1/8, 1/16) that pumps without a kick.");
    ImGui::TextDisabled("2. Click a + cell to turn that instrument down when the source hits. Drag the cell left / right for how much.");
    ImGui::TextDisabled("3. Shape it: ATTACK how fast it goes down, HOLD how long it stays, RELEASE how long it takes to come back "
                        "(ms, or a note length that follows the tempo).");
    ImGui::TextDisabled("4. The teal line in a cell shows the dip live while playing. Where two pumps reach one instrument, the deeper dip wins.");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::EndGroup();

    if (PumpsDiffer(np, app.project.fx.pumps)) {
        SceneLock lock(app.sequencer.PatternMutex(), "MIX LAB SIDECHAIN");
        app.PushSoundUndo();   // coalesces a drag into one undo step
        app.project.fx.pumps = np;
        app.engine.SetPumps(np);   // only the pumps, under the engine lock (delayTimeSec there follows the tempo on its own)
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
void SidechainPanel::DrawPump(AppState& app, std::array<PumpSettings, kNumPumps>& np, int p, float w, float h) {
    PumpSettings& ps = np[static_cast<size_t>(p)];
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(30, 32, 39, 255), 4.0f);
    dl->AddRect(p0, ImVec2(p0.x + w, p0.y + h), vis::kLine, 4.0f);
    ImGui::BeginGroup();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 6, p0.y + 6));
    ImGui::BeginGroup();
    const float inner = w - 12.0f;
    if (!ps.on) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.55f);

    char title[24]; std::snprintf(title, sizeof title, "PUMP %d", p + 1);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(title);
    ImGui::SameLine(inner - 46.0f);
    if (!ps.on) ImGui::PopStyleVar();
    if (ToggleButton(ps.on ? "ON" : "OFF", ps.on, ImVec2(46, 0), ToggleTeal())) ps.on = !ps.on;
    if (ImGui::IsItemHovered()) Hint("%s: switch this pump on or off (the instruments come back up in 10 ms).", title);
    if (!ps.on) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.55f);

    // SOURCE
    {
        const char* items[kNumPumpSources];
        for (int s = 0; s < kNumPumpSources; ++s) items[s] = PumpSourceName(s);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("SOURCE");
        ImGui::SameLine(0, 6);
        int src = ps.source;
        if (Choice("##src", &src, items, kNumPumpSources, inner - 56.0f)) ps.source = src;
        if (ImGui::IsItemHovered()) Hint("SOURCE: whose hits fire this pump. An instrument fires on every hit, even muted; Every 1/4, 1/8, 1/16 fires on the beat while playing.");
    }

    // the shape, drawn: two hits a beat apart at the tempo of the edited scene
    {
        const ImVec2 g0 = ImGui::GetCursorScreenPos();
        const float gh = 44.0f;
        dl->AddRectFilled(g0, ImVec2(g0.x + inner, g0.y + gh), vis::kInset, 3.0f);
        const Pattern& pat = app.CurrentPattern();
        const int spb = std::max(1, pat.stepsPerBeat);
        const float rate = 2000.0f;
        const double sixteenth = 60.0 / std::max(20.0f, pat.bpm) / 4.0 * rate;
        const double stepLen = 60.0 / std::max(20.0f, pat.bpm) / spb * rate;
        const std::vector<int> hits = {0, spb};
        const int n = static_cast<int>(inner);
        PumpSettings shape = ps; shape.on = true;
        const std::vector<float> env = SimEnvelope(shape, hits, 2 * spb, stepLen, sixteenth, rate, n);
        for (int i = 1; i < n; ++i)
            dl->AddLine(ImVec2(g0.x + i - 1, g0.y + 4 + env[static_cast<size_t>(i - 1)] * (gh - 8)), ImVec2(g0.x + i, g0.y + 4 + env[static_cast<size_t>(i)] * (gh - 8)), kPumpAmber, 1.6f);
        const float live = app.engine.PumpLevel(p);
        if (live > 0.0f) dl->AddRectFilled(ImVec2(g0.x + inner - 5, g0.y + 4 + (1.0f - live) * 0.0f), ImVec2(g0.x + inner - 2, g0.y + 4 + live * (gh - 8)), vis::kTeal);
        ImGui::Dummy(ImVec2(inner, gh));
        if (ImGui::IsItemHovered()) Hint("The shape of this pump over two beats (the dip = how far an instrument goes down at full depth).");
    }

    // ATTACK / HOLD / RELEASE
    {
        const ImVec2 ks(30.0f, 30.0f);
        const float cw = inner / 3.0f;
        char buf[16];
        ImGui::BeginGroup();
        std::snprintf(buf, sizeof buf, "%.0f ms", ps.attackMs);
        KnobColumn("ATTACK", buf, [&] { ps.attackMs = KnobAbsolute("atk", ps.attackMs, kPumpMinAttackMs, 50.0f, "ATTACK %.0f ms", ks, 3.0f); });
        if (ImGui::IsItemHovered()) Hint("ATTACK: how fast the instruments go down after a hit (2 ms = at once, without a click).");
        ImGui::EndGroup();
        ImGui::SameLine(cw + 6.0f);
        ImGui::BeginGroup();
        std::snprintf(buf, sizeof buf, "%.0f ms", ps.holdMs);
        KnobColumn("HOLD", buf, [&] { ps.holdMs = KnobAbsolute("hold", ps.holdMs, 0.0f, 500.0f, "HOLD %.0f ms", ks, 0.0f); });
        if (ImGui::IsItemHovered()) Hint("HOLD: how long they stay down before coming back.");
        ImGui::EndGroup();
        ImGui::SameLine(2.0f * cw + 6.0f);
        ImGui::BeginGroup();
        if (ps.releaseDiv >= 0) std::snprintf(buf, sizeof buf, "%s", PumpReleaseDivName(ps.releaseDiv));
        else std::snprintf(buf, sizeof buf, "%.0f ms", ps.releaseMs);
        KnobColumn("RELEASE", buf, [&] {
            const float r = KnobAbsolute("rel", ps.releaseMs, 10.0f, 1000.0f, "RELEASE %.0f ms", ks, 100.0f);
            if (r != ps.releaseMs) { ps.releaseMs = r; ps.releaseDiv = -1; }   // turning the knob means milliseconds again
        });
        if (ImGui::IsItemHovered()) Hint("RELEASE: how long they take to come back. The menu below sets it as a note length that follows the tempo.");
        ImGui::EndGroup();
        static const char* kRel[kNumPumpReleaseDivs + 1] = {"ms (knob)", "1/32 note", "1/16 note", "1/8 note", "1/4 note", "1/2 note"};
        int ri = ps.releaseDiv + 1;
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("RELEASE AS");
        ImGui::SameLine(0, 6);
        if (Choice("##reldiv", &ri, kRel, kNumPumpReleaseDivs + 1, inner - ImGui::CalcTextSize("RELEASE AS").x - 6.0f)) ps.releaseDiv = ri - 1;
        if (ImGui::IsItemHovered()) Hint("RELEASE in ms (the knob), or as a note length that follows the tempo (1/8 note = an eighth at the scene's BPM).");
    }

    // curve
    {
        static const char* kCurve[kNumPumpCurves] = {"LINE", "SMOOTH", "HARD"};
        int c = std::clamp(ps.curve, 0, kNumPumpCurves - 1);
        if (Segmented("##curve", &c, kCurve, kNumPumpCurves, inner / 3.0f - 2.0f)) ps.curve = c;
        if (ImGui::IsItemHovered()) Hint("The way back: LINE straight, SMOOTH soft at both ends, HARD stays down and snaps back.");
    }
    if (!ps.on) ImGui::PopStyleVar();
    ImGui::EndGroup();
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(w, h));
}

// ---------------------------------------------------------------------------------------------------------------------------
void SidechainPanel::DrawCells(AppState& app, std::array<PumpSettings, kNumPumps>& np, int shown, float nameW, float colW) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::TextDisabled("WHO IS TURNED DOWN, AND HOW MUCH");
    const float rowH = 26.0f;
    for (int t = 0; t < kNumTracks; ++t) {
        ImGui::PushID(100 + t);
        const ImVec2 r0 = ImGui::GetCursorScreenPos();
        dl->AddCircleFilled(ImVec2(r0.x + 8, r0.y + rowH * 0.5f), 4.0f, TrackColor(t));
        dl->AddText(ImVec2(r0.x + 20, r0.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f), sel_ == t ? vis::kAccentHi : vis::kText, MixTrackName(t));
        ImGui::InvisibleButton("##name", ImVec2(nameW - 8.0f, rowH));
        if (ImGui::IsItemClicked()) sel_ = t;
        if (ImGui::IsItemHovered()) Hint("%s: click to see on the right what the pumps do to it.", MixTrackName(t));
        for (int p = 0; p < shown && p < kNumPumps; ++p) {
            PumpSettings& ps = np[static_cast<size_t>(p)];
            ImGui::SameLine(nameW + static_cast<float>(p) * (colW + 6.0f));
            ImGui::PushID(p);
            const ImVec2 c0 = ImGui::GetCursorScreenPos();
            float& d = ps.depth[static_cast<size_t>(t)];
            if (ps.source == t) {
                dl->AddRectFilled(c0, ImVec2(c0.x + colW, c0.y + rowH - 2), IM_COL32(48, 36, 18, 255), 3.0f);
                dl->AddRect(c0, ImVec2(c0.x + colW, c0.y + rowH - 2), IM_COL32(201, 138, 30, 140), 3.0f);
                const ImVec2 ts = ImGui::CalcTextSize("source");
                dl->AddText(ImVec2(c0.x + (colW - ts.x) * 0.5f, c0.y + (rowH - 2 - ts.y) * 0.5f), kPumpAmber, "source");
                ImGui::Dummy(ImVec2(colW, rowH - 2));
                if (ImGui::IsItemHovered()) Hint("PUMP %d: %s is the source; its own hits fire this pump.", p + 1, MixTrackName(t));
            } else if (d <= 0.0f) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(IM_COL32(29, 31, 38, 255)));
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(vis::kTextDim));
                if (ImGui::Button("+", ImVec2(colW, rowH - 2))) { d = 0.5f; sel_ = t; }
                ImGui::PopStyleColor(2);
                if (ImGui::IsItemHovered()) Hint("PUMP %d: click to turn %s down by 50 %% when %s hits.", p + 1, MixTrackName(t), PumpSourceName(ps.source));
            } else {
                const float barW = colW - 96.0f;   // the bar, then the number beside it (the thumb would sit on top of it)
                ValueSpec sp = BarSpec(0.0f, 1.0f, 0.0f, barW);
                sp.color = IM_COL32(226, 160, 74, 160);
                sp.scale = 100.0f; sp.unit = "%"; sp.fmt = "%.0f";
                sp.height = rowH - 2;
                const float live = ps.on ? app.engine.PumpLevel(p) * d : 0.0f;
                if (live > 0.0f) sp.meter = live;
                float v = d;
                const ValueResult r = ValueField("##d", &v, sp, barW);
                if (r.started || r.changed) sel_ = t;
                bool remove = ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
                d = remove ? 0.0f : std::clamp(v, 0.0f, 1.0f);
                char txt[48];
                const float g = 1.0f - d;
                if (g > 0.0005f) std::snprintf(txt, sizeof txt, "%.0f%% %.0f dB", d * 100.0f, 20.0f * std::log10(g));
                else std::snprintf(txt, sizeof txt, "%.0f%% silent", d * 100.0f);
                dl->AddText(ImVec2(c0.x + barW + 8, c0.y + (rowH - 2 - ImGui::GetTextLineHeight()) * 0.5f), vis::kText, txt);
                if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                    Hint("%s <- %s: down by %s. Drag left / right to change, double-click to type, Option+click or right-click to remove.",
                         MixTrackName(t), PumpSourceName(ps.source), txt);
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
void SidechainPanel::DrawCurve(AppState& app, const std::array<PumpSettings, kNumPumps>& np, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (sel_ < 0 || sel_ >= kNumTracks) {
        sel_ = kNumDrumTracks;   // Bass, unless a pump reaches something else
        for (int t = 0; t < kNumTracks && sel_ == kNumDrumTracks; ++t)
            for (const auto& p : np) if (p.on && p.depth[static_cast<size_t>(t)] > 0.0f) { sel_ = t; break; }
    }
    char title[64]; std::snprintf(title, sizeof title, "WHAT IT DOES TO %s (2 beats)", MixTrackName(sel_));
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kPumpAmber), "%s", title);
    const ImVec2 g0 = ImGui::GetCursorScreenPos();
    const float gw = w - 22.0f, gh = h - 74.0f;
    dl->AddRectFilled(g0, ImVec2(g0.x + gw, g0.y + gh), vis::kInset, 3.0f);
    const Pattern& pat = app.CurrentPattern();
    const int spb = std::max(1, pat.stepsPerBeat), steps = 2 * spb;
    const float rate = 2000.0f;
    const double sixteenth = 60.0 / std::max(20.0f, pat.bpm) / 4.0 * rate;
    const double stepLen = 60.0 / std::max(20.0f, pat.bpm) / spb * rate;
    const int n = static_cast<int>(gw);
    std::vector<float> gain(static_cast<size_t>(n), 1.0f);
    int reaching = 0, firstPump = -1;
    std::vector<int> firstHits;
    for (int p = 0; p < kNumPumps; ++p) {
        const PumpSettings& ps = np[static_cast<size_t>(p)];
        const float d = ps.depth[static_cast<size_t>(sel_)];
        if (!ps.on || d <= 0.0f || ps.source == sel_) continue;
        const std::vector<int> hits = SourceHits(pat, ps, steps, spb);
        const std::vector<float> env = SimEnvelope(ps, hits, steps, stepLen, sixteenth, rate, n);
        for (int i = 0; i < n; ++i) gain[static_cast<size_t>(i)] = std::min(gain[static_cast<size_t>(i)], 1.0f - d * env[static_cast<size_t>(i)]);
        if (firstPump < 0) { firstPump = p; firstHits = hits; }
        ++reaching;
    }
    const float top = g0.y + 18.0f, bot = g0.y + gh - 22.0f;
    float lowest = 1.0f;
    for (int i = 1; i < n; ++i) {
        lowest = std::min(lowest, gain[static_cast<size_t>(i)]);
        dl->AddLine(ImVec2(g0.x + i - 1, bot - gain[static_cast<size_t>(i - 1)] * (bot - top)), ImVec2(g0.x + i, bot - gain[static_cast<size_t>(i)] * (bot - top)), TrackColor(sel_), 2.0f);
    }
    dl->AddText(ImVec2(g0.x + 6, g0.y + 2), vis::kTextDim, "full volume");
    if (reaching > 0) {
        for (int s : firstHits) {
            const float x = g0.x + static_cast<float>(s) / static_cast<float>(steps) * gw;
            dl->AddRectFilled(ImVec2(x, g0.y + gh - 16), ImVec2(x + 3, g0.y + gh - 3), vis::kRed);
        }
        char lb[64];
        if (lowest > 0.0005f) std::snprintf(lb, sizeof lb, "lowest %.0f dB (%.0f%%)", 20.0f * std::log10(lowest), lowest * 100.0f);
        else std::snprintf(lb, sizeof lb, "lowest: silent");
        dl->AddText(ImVec2(g0.x + gw - ImGui::CalcTextSize(lb).x - 6, g0.y + 2), vis::kTextDim, lb);
        char hl[48]; std::snprintf(hl, sizeof hl, "%s hits", PumpSourceName(np[static_cast<size_t>(firstPump)].source));
        dl->AddText(ImVec2(g0.x + 10, g0.y + gh - 18), vis::kRed, hl);   // beside the first marker (the window starts on a hit or near one)
    }
    ImGui::Dummy(ImVec2(gw, gh));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + gw);
    if (reaching == 0) ImGui::TextDisabled("No pump that is on turns %s down. Click a + cell in its row.", MixTrackName(sel_));
    else if (reaching == 1) ImGui::TextDisabled("Drawn from the hits in the edited scene (its first two beats).");
    else ImGui::TextDisabled("%d pumps turn %s down; at every moment the deeper dip wins.", reaching, MixTrackName(sel_));
    ImGui::PopTextWrapPos();
}

// ---------------------------------------------------------------------------------------------------------------------------
void SidechainPanel::DrawPresets(AppState& app, std::array<PumpSettings, kNumPumps>& np, float w) {
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kPumpAmber), "QUICK START");
    struct Preset { const char* name; const char* hint; int source; float attack, hold, release; int div, curve; float d[kNumTracks]; };
    //                                                                                Kick Sn  Cl  CH  OH  Pc  Bass S2  S3  Chd Smp
    static const Preset kPresets[] = {
        {"CLASSIC PUMP", "Kick turns Bass, Chord and Synth2 down, back in an eighth note.", 0, 3, 10, 0, 2, 1, {0, 0, 0, 0, 0, 0, .65f, .35f, 0, .40f, 0}},
        {"SOFT GLUE", "A gentle dip on every kick: the mix breathes without obvious pumping.", 0, 8, 0, 150, -1, 1, {0, 0, 0, 0, 0, 0, .30f, .20f, .20f, .20f, 0}},
        {"HARD EDM", "Deep, long, and snapping back: the big-room pump.", 0, 2, 30, 0, 3, 2, {0, 0, 0, 0, 0, 0, .90f, .80f, .80f, .80f, .70f}},
        {"SNARE MAKES ROOM", "The snare turns the chord and pads down a little so it cuts through.", 1, 2, 0, 120, -1, 1, {0, 0, 0, 0, 0, 0, 0, .25f, 0, .30f, .25f}},
        {"GHOST 1/4 (NO KICK)", "Pumps on every beat even without a kick: for breakdowns and intros.", kPumpSourceEvery4, 2, 20, 0, 3, 2, {0, 0, 0, 0, 0, 0, .60f, .50f, 0, .50f, 0}},
    };
    float x = 0.0f;
    for (const Preset& pr : kPresets) {
        const float bw = ImGui::CalcTextSize(pr.name).x + 18.0f;
        if (x > 0.0f && x + bw < w - 20.0f) ImGui::SameLine(0, 6); else x = 0.0f;
        if (ImGui::Button(pr.name, ImVec2(bw, 0))) {
            int free = -1;
            for (int p = 0; p < kNumPumps && free < 0; ++p) if (!PumpInUse(np[static_cast<size_t>(p)])) free = p;
            if (free < 0) app.SetStatus("SIDECHAIN: ALL 4 PUMPS ARE IN USE - CLEAR ONE FIRST (OPTION+CLICK ITS CELLS, OFF)");
            else {
                PumpSettings& ps = np[static_cast<size_t>(free)];
                ps = PumpSettings{};
                ps.on = true; ps.source = pr.source; ps.attackMs = pr.attack; ps.holdMs = pr.hold;
                ps.releaseMs = pr.release > 0 ? pr.release : 100.0f; ps.releaseDiv = pr.div; ps.curve = pr.curve;
                for (int t = 0; t < kNumTracks; ++t) ps.depth[static_cast<size_t>(t)] = pr.d[t];
                app.SetStatus(std::string("SIDECHAIN: ") + pr.name + " IN PUMP " + std::to_string(free + 1) + " (CMD+Z UNDOES IT)");
            }
        }
        if (ImGui::IsItemHovered()) Hint("%s: %s Fills the first free pump; everything stays editable.", pr.name, pr.hint);
        x += bw + 6.0f;
    }
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextDisabled("A preset fills one free pump. Cmd+Z undoes it.");
}

} // namespace t8::desktop
