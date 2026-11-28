#include "FxPanel.h"
#include "KnobWidgets.h"
#include "LiveWidgets.h"
#include "ValueField.h"
#include "MixLabPanel.h"
#include "Visual.h"
#include "imgui.h"
#include "InfoLine.h"
#include <algorithm>
#include <cstdio>
#include <mutex>

namespace t8::desktop {

void FxPanel::Draw(AppState& app) {
    SceneLock lock(app.sequencer.PatternMutex(), "FX");
    FxSettings& fx = app.project.fx;
    const ImVec2 kKnobSize(58.0f, 58.0f);
    static const FxSettings kFxDef{};                 // C3: Option+click on a knob puts these back
    static const MasterChainSettings kChainDef{};

    ImGui::PushID("FxPanel");
    // C1: the title (MASTER FX, what it does, key F, x) is the tool strip that main draws above

    // ---- DELAY --------------------------------------------------------
    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f), "DELAY");
    // Tempo-synced division, not a free time knob -- same prev/next
    // browse as every other discrete-index control in this UI (Scenes/
    // KIT browser), since kDelayDivisions is a fixed named list, not a
    // continuous range.
    ImGui::TextDisabled("TIME");
    ImGui::SameLine();
    if (MiniButton("<##dt")) { app.PushSoundUndo(); fx.delayDivisionIndex = (fx.delayDivisionIndex - 1 + kNumDelayDivisions) % kNumDelayDivisions; }
    ImGui::SameLine();
    ImGui::Text("%s", kDelayDivisions[std::clamp(fx.delayDivisionIndex, 0, kNumDelayDivisions - 1)].label);
    ImGui::SameLine();
    if (MiniButton(">##dt")) { app.PushSoundUndo(); fx.delayDivisionIndex = (fx.delayDivisionIndex + 1) % kNumDelayDivisions; }

    float newFeedback = fx.delayFeedback;
    char fbBuf[8]; std::snprintf(fbBuf, sizeof(fbBuf), "%d%%", static_cast<int>(fx.delayFeedback * 100));
    KnobColumn("FEEDBACK", fbBuf, [&] {
        newFeedback = KnobAbsolute("fb", fx.delayFeedback, 0.0f, 0.9f, "DELAY FEEDBACK %.0f%%", kKnobSize, kFxDef.delayFeedback, 100.0f);
    });
    ImGui::SameLine(0, 18);
    float newTone = fx.delayTone;
    char tnBuf[8]; std::snprintf(tnBuf, sizeof(tnBuf), "%d%%", static_cast<int>(fx.delayTone * 100 + 0.5f));
    KnobColumn("TONE", tnBuf, [&] {
        newTone = KnobAbsolute("dtone", fx.delayTone, 0.0f, 1.0f, "DELAY TONE %.0f%%", kKnobSize, kFxDef.delayTone, 100.0f);
    });
    ImGui::SameLine(0, 18);
    bool newPing = fx.delayPingPong;
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(1, 14));
    Check("PING-PONG", &newPing);
    ImGui::EndGroup();
    ImGui::SameLine(0, 18);
    float newDelayLevel = fx.delayLevel;
    char dlBuf[8]; std::snprintf(dlBuf, sizeof(dlBuf), "%d%%", static_cast<int>(fx.delayLevel * 100));
    KnobColumn("LEVEL", dlBuf, [&] {
        newDelayLevel = KnobAbsolute("dl", fx.delayLevel, 0.0f, 1.0f, "DELAY LEVEL %.0f%%", kKnobSize, kFxDef.delayLevel, 100.0f);
    });
    ImGui::EndGroup();

    ImGui::SameLine(0, 48);

    // ---- REVERB ---------------------------------------------------------
    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f), "REVERB");
    float newSize = fx.reverbSize;
    char szBuf[8]; std::snprintf(szBuf, sizeof(szBuf), "%d%%", static_cast<int>(fx.reverbSize * 100));
    KnobColumn("SIZE", szBuf, [&] {
        newSize = KnobAbsolute("rs", fx.reverbSize, 0.0f, 0.97f, "REVERB SIZE %.0f%%", kKnobSize, kFxDef.reverbSize, 100.0f);
    });
    ImGui::SameLine(0, 18);
    float newDamp = fx.reverbDamp;
    char dmBuf[8]; std::snprintf(dmBuf, sizeof(dmBuf), "%d%%", static_cast<int>(fx.reverbDamp * 100 + 0.5f));
    KnobColumn("DAMP", dmBuf, [&] {
        newDamp = KnobAbsolute("rd", fx.reverbDamp, 0.0f, 1.0f, "REVERB DAMP %.0f%%", kKnobSize, kFxDef.reverbDamp, 100.0f);
    });
    ImGui::SameLine(0, 18);
    float newPre = fx.reverbPreMs;
    char prBuf[12]; std::snprintf(prBuf, sizeof(prBuf), "%dMS", static_cast<int>(fx.reverbPreMs + 0.5f));
    KnobColumn("PRE", prBuf, [&] {
        newPre = KnobAbsolute("rpre", fx.reverbPreMs, 0.0f, 120.0f, "REVERB PRE-DELAY %.0fMS", kKnobSize, kFxDef.reverbPreMs);
    });
    ImGui::SameLine(0, 18);
    float newRevLevel = fx.reverbLevel;
    char rlBuf[8]; std::snprintf(rlBuf, sizeof(rlBuf), "%d%%", static_cast<int>(fx.reverbLevel * 100));
    KnobColumn("LEVEL", rlBuf, [&] {
        newRevLevel = KnobAbsolute("rl", fx.reverbLevel, 0.0f, 1.0f, "REVERB LEVEL %.0f%%", kKnobSize, kFxDef.reverbLevel, 100.0f);
    });
    ImGui::EndGroup();

    ImGui::SameLine(0, 48);

    // ---- DISTORTION -------------------------------------------------------
    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f), "DISTORTION");
    float newDrive = fx.distortionDrive;
    char drBuf[8]; std::snprintf(drBuf, sizeof(drBuf), "%d%%", static_cast<int>(fx.distortionDrive * 100));
    KnobColumn("DRIVE", drBuf, [&] {
        newDrive = KnobAbsolute("dr", fx.distortionDrive, 0.0f, 1.0f, "MASTER DRIVE %.0f%%", kKnobSize, kFxDef.distortionDrive, 100.0f);
    });
    ImGui::EndGroup();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- SIDECHAIN: lives in MIX LAB > SIDECHAIN (4 pumps); here only where it went and how it stands ----------------
    {
        ImGui::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f), "SIDECHAIN");
        int on = 0;
        for (const auto& pp : fx.pumps) if (pp.on) ++on;
        ImGui::SameLine(0, 12);
        ImGui::TextDisabled(on ? "%d pump(s) on - edited in MIX LAB > SIDECHAIN" : "all pumps off - edited in MIX LAB > SIDECHAIN", on);
        ImGui::SameLine(0, 12);
        if (ImGui::Button("OPEN SIDECHAIN")) { MixLabPanel::requestSidechain = true; app.OpenTool(AppState::Tool::MixLab); }
        if (ImGui::IsItemHovered()) Hint("SIDECHAIN moved to MIX LAB: up to 4 pumps, any instrument or a steady beat turning any instruments down.");
    }

    // Single sync point at the end -- same "mutate the project copy, push
    // once to the live Mixer::fx copy" shape FxView.cpp's AdjustFxParam
    // call sites use, just done once per frame here instead of once per
    // field (every field above writes straight into `fx`, which IS
    // app.project.fx by reference).
    bool changed = (newFeedback != fx.delayFeedback) || (newTone != fx.delayTone) || (newPing != fx.delayPingPong) || (newDelayLevel != fx.delayLevel) ||
                   (newSize != fx.reverbSize) || (newRevLevel != fx.reverbLevel) || (newDamp != fx.reverbDamp) || (newPre != fx.reverbPreMs) ||
                   (newDrive != fx.distortionDrive);
    if (changed) app.PushSoundUndo();
    fx.delayFeedback = newFeedback;
    fx.delayTone = newTone;
    fx.delayPingPong = newPing;
    fx.delayLevel = newDelayLevel;
    fx.reverbSize = newSize;
    fx.reverbLevel = newRevLevel;
    fx.reverbDamp = newDamp;
    fx.reverbPreMs = newPre;
    fx.distortionDrive = newDrive;
    if (changed) {
        app.engine.GetMixer().fx = fx;
    }


    // =====================================================================
    // MASTER CHAIN: compressor + "finished track" polish
    // =====================================================================
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    {
        MasterChainSettings& m = fx.master;
        const ImVec4 amber(0.95f, 0.64f, 0.24f, 1.0f);
        bool mcChanged = false;
        auto touch = [&] { if (!mcChanged) { app.PushSoundUndo(); mcChanged = true; } };
        // a knob that edits one float of the chain; `preset` (if given) goes to -1 = custom
        auto knob = [&](const char* id, const char* label, float MasterChainSettings::*f, float lo, float hi, const char* tip, const char* valFmt, int* preset, float vscale = 1.0f) {
            float& v = m.*f;
            char vb[24]; std::snprintf(vb, sizeof(vb), valFmt, v * vscale);
            float nv = v;
            KnobColumn(label, vb, [&] { nv = KnobAbsolute(id, v, lo, hi, tip, kKnobSize, kChainDef.*f, vscale); });
            if (nv != v) { touch(); v = nv; if (preset) *preset = -1; }
        };
        auto presetRow = [&](const char* idp, const char* const* names, int n, int cur, auto&& apply) {
            for (int i = 0; i < n; ++i) {
                if (i > 0) ImGui::SameLine(0, 4);
                char id[24]; std::snprintf(id, sizeof(id), "%s##%s%d", names[i], idp, i);
                if (ToggleButton(id, cur == i, ImVec2(56, 0), ToggleTab())) { touch(); apply(i); }
            }
        };

        // ---- header row: chain on/off + presets ----
        ImGui::TextColored(amber, "MASTER CHAIN");
        ImGui::SameLine(0, 14);
        if (ToggleButton(m.chainOn ? "CHAIN ON##mcon" : "CHAIN OFF##mcon", m.chainOn, ImVec2(92, 0), ToggleTeal())) {
            touch(); m.chainOn = !m.chainOn;
            if (m.chainOn && m.chainPreset < 0 && !m.compOn && m.lowDb == 0 && m.highDb == 0 && m.saturation == 0) ApplyMasterPreset(m, 0);
        }
        if (ImGui::IsItemHovered()) Hint("High-pass, tone, saturation, stereo width and the output limiter.\nOff = no tone/width/saturation. The limiter and mono bass stay in OUTPUT.");
        ImGui::SameLine(0, 14);
        ImGui::TextDisabled("PRESET");
        ImGui::SameLine(0, 6);
        presetRow("mp", kMasterPresetNames, kNumMasterPresets, m.chainOn ? m.chainPreset : -1, [&](int i) { ApplyMasterPreset(m, i); });
        ImGui::SameLine(0, 10);
        ImGui::TextDisabled("CLEAN polish / WARM tape / LOUD dense / LO-FI crunch");
        ImGui::Spacing();

        // ---- compressor ----
        ImGui::BeginGroup();
        ImGui::TextColored(amber, "COMPRESSOR");
        ImGui::SameLine(0, 10);
        if (ToggleButton("ON##compon", m.compOn, ImVec2(44, 0), ToggleTeal())) { touch(); m.compOn = !m.compOn; }
        ImGui::SameLine(0, 10);
        presetRow("cp", kCompPresetNames, kNumCompPresets, m.compPreset, [&](int i) { ApplyCompPreset(m, i); m.chainPreset = -1; });
        ImGui::SameLine(0, 12);
        {   // gain-reduction meter: fills from the left, 0..12 dB
            float gr = app.engine.GetMasterChain().GainReductionDb();
            ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = 150.0f, h = ImGui::GetFrameHeight() - 6.0f;
            p.y += 3.0f;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), vis::kInset, 3.0f);
            float fill = std::clamp(gr / 12.0f, 0.0f, 1.0f) * w;
            if (m.compOn && fill > 0.5f) dl->AddRectFilled(p, ImVec2(p.x + fill, p.y + h), vis::kAccent, 3.0f);
            char gb[24]; std::snprintf(gb, sizeof(gb), m.compOn ? "GR -%.1f dB" : "GR off", gr);
            dl->AddText(ImVec2(p.x + 6.0f, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f), vis::kText, gb);
            ImGui::Dummy(ImVec2(w, ImGui::GetFrameHeight()));
        }
        knob("cth", "THRESHOLD", &MasterChainSettings::compThreshDb, -40.0f, 0.0f, "THRESHOLD %.0f dB", "%.0f dB", &m.compPreset);
        ImGui::SameLine(0, 14);
        knob("crt", "RATIO", &MasterChainSettings::compRatio, 1.0f, 10.0f, "RATIO %.1f : 1", "%.1f:1", &m.compPreset);
        ImGui::SameLine(0, 14);
        knob("cat", "ATTACK", &MasterChainSettings::compAttackMs, 1.0f, 100.0f, "ATTACK %.0f ms", "%.0f ms", &m.compPreset);
        ImGui::SameLine(0, 14);
        knob("crl", "RELEASE", &MasterChainSettings::compReleaseMs, 20.0f, 800.0f, "RELEASE %.0f ms", "%.0f ms", &m.compPreset);
        ImGui::SameLine(0, 14);
        knob("cmk", "MAKEUP", &MasterChainSettings::compMakeupDb, 0.0f, 12.0f, "MAKEUP +%.1f dB", "+%.1f dB", &m.compPreset);
        ImGui::SameLine(0, 14);
        knob("cmx", "MIX", &MasterChainSettings::compMix, 0.0f, 1.0f, "MIX %.0f%% (below 100%% = parallel compression)", "%.0f%%", &m.compPreset, 100.0f);
        ImGui::EndGroup();

        ImGui::Spacing();

        // ---- tone / polish ----
        ImGui::BeginGroup();
        ImGui::TextColored(amber, "TONE / POLISH");
        if (!m.chainOn) { ImGui::SameLine(0, 10); ImGui::TextDisabled("(chain is off)"); }
        if (!m.chainOn) ImGui::BeginDisabled();
        knob("hpf", "HI-PASS", &MasterChainSettings::hpfHz, 20.0f, 400.0f, "HIGH-PASS %.0f Hz", "%.0f Hz", &m.chainPreset);
        ImGui::SameLine(0, 14);
        knob("lo", "LOW", &MasterChainSettings::lowDb, -9.0f, 9.0f, "LOW SHELF %+.1f dB", "%+.1f", &m.chainPreset);
        ImGui::SameLine(0, 14);
        knob("mi", "MID", &MasterChainSettings::midDb, -9.0f, 9.0f, "MID %+.1f dB", "%+.1f", &m.chainPreset);
        ImGui::SameLine(0, 14);
        knob("hi", "HIGH", &MasterChainSettings::highDb, -9.0f, 9.0f, "HIGH SHELF %+.1f dB", "%+.1f", &m.chainPreset);
        ImGui::SameLine(0, 14);
        knob("wd", "WIDTH", &MasterChainSettings::width, 0.0f, 2.0f, "STEREO WIDTH %.0f%% (100%% = as is, 0 = mono)", "%.0f%%", &m.chainPreset, 100.0f);
        ImGui::SameLine(0, 14);
        knob("sat", "SATURATE", &MasterChainSettings::saturation, 0.0f, 1.0f, "SATURATION %.0f%%", "%.0f%%", &m.chainPreset, 100.0f);
        if (!m.chainOn) ImGui::EndDisabled();
        ImGui::EndGroup();

        ImGui::Spacing();

        // ---- output: limiter + mono bass (always active, independent of the chain) ----
        ImGui::BeginGroup();
        ImGui::TextColored(amber, "OUTPUT");
        ImGui::SameLine(0, 10);
        if (ToggleButton("LIMITER##limon", m.limiterOn, ImVec2(76, 0), ToggleTeal())) { touch(); m.limiterOn = !m.limiterOn; }
        if (ImGui::IsItemHovered()) Hint("Look-ahead true-peak limiter (1.5 ms). Always the last stage,\nso no peak can pass the ceiling, also between the samples.\nOff = only a hard clip at 0 dBFS.");
        ImGui::SameLine(0, 12);
        {   // limiter gain reduction, same style as the compressor meter
            const float lg = app.engine.GetLimiter().GainReductionDb();
            ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = 120.0f, h = ImGui::GetFrameHeight() - 6.0f;
            p.y += 3.0f;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), vis::kInset, 3.0f);
            const float fill = std::clamp(lg / 6.0f, 0.0f, 1.0f) * w;
            if (m.limiterOn && fill > 0.5f) dl->AddRectFilled(p, ImVec2(p.x + fill, p.y + h), vis::kAccent, 3.0f);
            char gb[24]; std::snprintf(gb, sizeof(gb), m.limiterOn ? "LIM -%.1f dB" : "LIM off", lg);
            dl->AddText(ImVec2(p.x + 6.0f, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f), vis::kText, gb);
            ImGui::Dummy(ImVec2(w, ImGui::GetFrameHeight()));
        }
        knob("ceil", "CEILING", &MasterChainSettings::ceilingDb, -6.0f, 0.0f, "TRUE-PEAK CEILING %.1f dB", "%.1f dB", &m.chainPreset);
        ImGui::SameLine(0, 14);
        knob("mbass", "MONO BASS", &MasterChainSettings::monoBassHz, 0.0f, 250.0f, "MONO BELOW %.0f Hz (0 = off)", "%.0f Hz", nullptr);
        if (ImGui::IsItemHovered()) Hint("Everything below this frequency is summed to the centre (flat crossover).\nKeeps kick and bass solid on club systems and vinyl. 0 = off.");
        ImGui::SameLine(0, 14);
        knob("scs", "COMP SC HPF", &MasterChainSettings::compScHpfHz, 20.0f, 300.0f, "COMPRESSOR DETECTOR HIGH-PASS %.0f Hz", "%.0f Hz", &m.compPreset);
        if (ImGui::IsItemHovered()) Hint("The compressor ignores content below this frequency, so the kick and\nsub do not pump the whole mix. Only the detector is filtered.");
        ImGui::EndGroup();

        if (mcChanged) {
            m.compMix = std::clamp(m.compMix, 0.0f, 1.0f);
            app.engine.GetMixer().fx = fx;
        }
    }

    ImGui::PopID();
}

} // namespace t8::desktop
