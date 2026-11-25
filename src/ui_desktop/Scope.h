#pragma once
// Master-bus oscilloscope + the envelope / LFO previews, all drawn with the
// shared well style from Visual.h so they read as one family.
#include "ValueField.h"
#include "Visual.h"
#include "imgui.h"
#include "InfoLine.h"
#include "core/AudioEngine.h"
#include "core/Types.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace t8::desktop {

namespace detail {
// In-place radix-2 FFT (n a power of two) on interleaved re/im -- only used for the spectrum picture.
inline void Fft(float* re, float* im, int n) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -6.2831853f / static_cast<float>(len);
        float wr = std::cos(ang), wi = std::sin(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; ++k) {
                int a = i + k, b = i + k + len / 2;
                float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr; im[b] = im[a] - ti; re[a] += tr; im[a] += ti;
                float ncr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = ncr;
            }
        }
    }
}
}

// Log-frequency spectrum of the post-master mix: 32 bands 40 Hz - 16 kHz, dB scale with a
// slow fall-off and a peak cap per band. Drawn into the same well as the scope.
inline void DrawSpectrum(const AudioEngine& engine, ImVec2 a, ImVec2 b) {
    constexpr int N = 2048, kBands = 32;
    static float re[N], im[N], mono[N];
    static float level[kBands] = {}, cap[kBands] = {};
    engine.CopyRecent(mono, N);
    for (int i = 0; i < N; ++i) {
        float w = 0.5f - 0.5f * std::cos(6.2831853f * i / (N - 1));
        re[i] = mono[i] * w; im[i] = 0.0f;
    }
    detail::Fft(re, im, N);
    const float sr = static_cast<float>(std::max(8000, engine.SampleRate()));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float W = b.x - a.x - 6.0f, H = b.y - a.y - 6.0f;
    for (int k = 0; k < kBands; ++k) {
        float f0 = 40.0f * std::pow(16000.0f / 40.0f, static_cast<float>(k) / kBands);
        float f1 = 40.0f * std::pow(16000.0f / 40.0f, static_cast<float>(k + 1) / kBands);
        int i0 = std::clamp(static_cast<int>(f0 / sr * N), 1, N / 2 - 1), i1 = std::clamp(static_cast<int>(f1 / sr * N), i0 + 1, N / 2);
        float m = 0.0f;
        for (int i = i0; i < i1; ++i) m = std::max(m, std::sqrt(re[i] * re[i] + im[i] * im[i]));
        float db = 20.0f * std::log10(std::max(m / (N * 0.25f), 1e-6f));
        float v = std::clamp((db + 78.0f) / 78.0f, 0.0f, 1.0f);
        level[k] = v > level[k] ? v : level[k] * 0.86f + v * 0.14f;
        cap[k] = level[k] > cap[k] ? level[k] : std::max(0.0f, cap[k] - 0.006f);
        float x0 = a.x + 3.0f + W * k / kBands, x1 = a.x + 3.0f + W * (k + 1) / kBands - 1.0f;
        float y = b.y - 3.0f - H * level[k];
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, b.y - 3.0f), IM_COL32(80, 214, 190, 200));
        dl->AddRectFilled(ImVec2(x0, b.y - 3.0f - H * cap[k] - 1.5f), ImVec2(x1, b.y - 3.0f - H * cap[k]), vis::kAccentHi);
    }
}

// ---- Detailed analyzer window (double-click the header scope) ------------------------------
// 4096-point FFT of the post-master mono mix, 160 log bands 20 Hz - 20 kHz, dB grid, peak hold,
// hover read-out (frequency / level / nearest note), freeze, optional +4.5 dB/oct tilt.
struct SpectrumAnalyzer {
    static constexpr int N = 4096, B = 160;
    float level[B] = {}, hold[B] = {}, holdAge[B] = {};
    float shown[B] = {};          // what is drawn (level or the frozen copy)
    bool freeze = false, tilt = false;
    bool open = false;
    float dbFloor = -90.0f;

    static float BandFreq(float t, float fmax) { return 20.0f * std::pow(fmax / 20.0f, t); }

    void Update(const AudioEngine& engine, float dt) {
        if (freeze) return;
        static float re[N], im[N], mono[N], mag[N / 2 + 1];
        engine.CopyRecent(mono, N);
        for (int i = 0; i < N; ++i) {
            float w = 0.5f - 0.5f * std::cos(6.2831853f * i / (N - 1));
            re[i] = mono[i] * w; im[i] = 0.0f;
        }
        detail::Fft(re, im, N);
        for (int i = 0; i <= N / 2; ++i) mag[i] = std::sqrt(re[i] * re[i] + im[i] * im[i]) / (N * 0.25f);
        const float sr = static_cast<float>(std::max(8000, engine.SampleRate()));
        const float fmax = std::min(20000.0f, sr * 0.45f);
        for (int k = 0; k < B; ++k) {
            float f0 = BandFreq(static_cast<float>(k) / B, fmax), f1 = BandFreq(static_cast<float>(k + 1) / B, fmax);
            float b0 = f0 / sr * N, b1 = f1 / sr * N, m;
            if (b1 - b0 < 1.0f) {                     // narrower than a bin: interpolate between neighbours
                float c = 0.5f * (b0 + b1);
                int i = std::clamp(static_cast<int>(c), 1, N / 2 - 1); float fr = c - static_cast<float>(i);
                m = mag[i] * (1.0f - fr) + mag[i + 1] * fr;
            } else {
                int i0 = std::clamp(static_cast<int>(b0), 1, N / 2 - 1), i1 = std::clamp(static_cast<int>(b1) + 1, i0 + 1, N / 2);
                m = 0.0f; for (int i = i0; i < i1; ++i) m = std::max(m, mag[i]);
            }
            float db = 20.0f * std::log10(std::max(m, 1e-7f));
            if (tilt) db += 4.5f * std::log2(std::sqrt(f0 * f1) / 1000.0f);
            float v = std::clamp((db - dbFloor) / (0.0f - dbFloor), 0.0f, 1.0f);
            level[k] = v > level[k] ? v : level[k] * 0.82f + v * 0.18f;
            if (level[k] >= hold[k]) { hold[k] = level[k]; holdAge[k] = 0.0f; }
            else { holdAge[k] += dt; if (holdAge[k] > 1.2f) hold[k] = std::max(level[k], hold[k] - dt * 0.25f); }
        }
        std::copy(level, level + B, shown);
    }
};

inline const char* NoteNameOf(float hz, int* cents = nullptr) {
    static char buf[16];
    static const char* const nm[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    float m = 69.0f + 12.0f * std::log2(std::max(hz, 1.0f) / 440.0f);
    int mi = static_cast<int>(std::lround(m));
    if (cents) *cents = static_cast<int>(std::lround((m - mi) * 100.0f));
    std::snprintf(buf, sizeof buf, "%s%d", nm[((mi % 12) + 12) % 12], mi / 12 - 1);
    return buf;
}

inline SpectrumAnalyzer& Analyzer() { static SpectrumAnalyzer a; return a; }

inline void DrawSpectrumWindow(const AudioEngine& engine) {
    SpectrumAnalyzer& an = Analyzer();
    if (!an.open) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(std::min(820.0f, io.DisplaySize.x - 40.0f), std::min(400.0f, io.DisplaySize.y - 40.0f)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(360, 220), ImVec2(4000, 3000));
    bool open = an.open;
    if (ImGui::Begin("SPECTRUM", &open, ImGuiWindowFlags_NoCollapse)) {
        an.Update(engine, io.DeltaTime);
        if (ImGui::Button(an.freeze ? "FROZEN" : "FREEZE")) an.freeze = !an.freeze;
        ImGui::SameLine();
        if (ImGui::Button("RESET PEAKS")) std::fill(an.hold, an.hold + an.B, 0.0f);
        ImGui::SameLine();
        if (ImGui::Button(an.tilt ? "TILT +4.5 dB/oct" : "TILT OFF")) { an.tilt = !an.tilt; std::fill(an.hold, an.hold + an.B, 0.0f); }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        static const float kFloors[3] = {-60.0f, -90.0f, -120.0f};
        static int fi = 1;
        if (ChoiceBegin("##floor", fi == 0 ? "60 dB" : fi == 1 ? "90 dB" : "120 dB")) {
            for (int i = 0; i < 3; ++i) if (ImGui::Selectable(i == 0 ? "60 dB" : i == 1 ? "90 dB" : "120 dB", i == fi)) { fi = i; an.dbFloor = kFloors[i]; }
            ChoiceEnd();
        }
        ImVec2 a = ImGui::GetCursorScreenPos();
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 size(std::max(200.0f, avail.x), std::max(120.0f, avail.y));
        ImGui::InvisibleButton("##specbig", size);
        const bool hov = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 b(a.x + size.x, a.y + size.y);
        vis::DrawWell(dl, a, b);
        const float L = 34.0f, R = 8.0f, T = 8.0f, Bm = 18.0f;
        const float x0 = a.x + L, x1 = b.x - R, y0 = a.y + T, y1 = b.y - Bm, W = x1 - x0, H = y1 - y0;
        const float sr = static_cast<float>(std::max(8000, engine.SampleRate()));
        const float fmax = std::min(20000.0f, sr * 0.45f);
        auto xOf = [&](float f) { return x0 + W * std::log(f / 20.0f) / std::log(fmax / 20.0f); };
        // horizontal dB grid
        for (float db = 0.0f; db > an.dbFloor + 1.0f; db -= (an.dbFloor > -70.0f ? 10.0f : 20.0f)) {
            float y = y1 - H * (db - an.dbFloor) / (0.0f - an.dbFloor);
            dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), vis::kLine, 1.0f);
            char t[8]; std::snprintf(t, sizeof t, "%d", static_cast<int>(db));
            dl->AddText(ImVec2(a.x + 4, y - 7), vis::kLineHi, t);
        }
        // vertical frequency grid
        static const float kF[] = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
        static const char* const kL[] = {"20", "50", "100", "200", "500", "1k", "2k", "5k", "10k", "20k"};
        for (int i = 0; i < 10; ++i) {
            if (kF[i] > fmax) break;
            float x = xOf(kF[i]);
            dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), vis::kLine, 1.0f);
            dl->AddText(ImVec2(std::min(x - 6, x1 - 18), y1 + 2), vis::kLineHi, kL[i]);
        }
        // spectrum: filled columns + outline, peak-hold line
        static ImVec2 top[SpectrumAnalyzer::B], pk[SpectrumAnalyzer::B];
        for (int k = 0; k < an.B; ++k) {
            float xa = x0 + W * k / an.B, xb = x0 + W * (k + 1) / an.B;
            float ya = y1 - H * an.shown[k];
            dl->AddRectFilled(ImVec2(xa, ya), ImVec2(xb, y1), IM_COL32(80, 214, 190, 70));
            top[k] = ImVec2(0.5f * (xa + xb), ya);
            pk[k] = ImVec2(0.5f * (xa + xb), y1 - H * an.hold[k]);
        }
        dl->AddPolyline(pk, an.B, vis::kAccentHi, 0, 1.0f);
        dl->AddPolyline(top, an.B, vis::kTeal, 0, 1.8f);
        // hover read-out
        if (hov) {
            ImVec2 mp = io.MousePos;
            if (mp.x >= x0 && mp.x <= x1) {
                float t = (mp.x - x0) / W;
                float f = 20.0f * std::pow(fmax / 20.0f, t);
                int k = std::clamp(static_cast<int>(t * an.B), 0, an.B - 1);
                float db = an.dbFloor + an.shown[k] * (0.0f - an.dbFloor);
                dl->AddLine(ImVec2(mp.x, y0), ImVec2(mp.x, y1), vis::kAccentHi, 1.0f);
                int cents = 0; const char* nn = NoteNameOf(f, &cents);
                ValueTip("%.0f Hz   %.1f dB   %s %+dc", f, db, nn, cents);
            }
        }
    }
    ImGui::End();
    an.open = open;
}

// Live waveform of the post-master mix. Triggers on a rising zero-crossing so
// periodic material holds still instead of scrolling. Idle = a flat line.
inline void DrawScope(const AudioEngine& engine, ImVec2 size) {
    ImVec2 a = ImGui::GetCursorScreenPos();
    static bool showSpectrum = false;
    // A double-click opens the big analyzer and restores the scope / spectrum choice made before the first click.
    static bool before = false; static double lastClick = -10.0;
    if (ImGui::InvisibleButton("##scope", size)) {
        double now = ImGui::GetTime();
        if (now - lastClick > 0.5) before = showSpectrum;   // state before the first click of a possible double-click
        lastClick = now;
        showSpectrum = !showSpectrum;
    }
    if (ImGui::IsItemHovered()) {
        if (ImGui::IsMouseDoubleClicked(0)) { Analyzer().open = true; showSpectrum = before; }
        Hint(showSpectrum ? "Spectrum (40 Hz - 16 kHz). Click: oscilloscope.\nDouble-click: detailed analyzer" : "Oscilloscope. Click: spectrum.\nDouble-click: detailed analyzer");
    }
    DrawSpectrumWindow(engine);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 b(a.x + size.x, a.y + size.y);
    vis::DrawWell(dl, a, b);
    if (showSpectrum) { DrawSpectrum(engine, a, b); return; }

    constexpr int kWin = 512, kLook = 1536;
    static float buf[kLook];
    engine.CopyScope(buf, kLook);
    // find a rising crossing in the older half so a full window follows it
    int start = kLook - kWin;
    for (int i = 1; i < kLook - kWin; ++i) {
        if (buf[i - 1] <= 0.0f && buf[i] > 0.0f) { start = i; break; }
    }
    float mid = (a.y + b.y) * 0.5f, amp = (size.y * 0.5f - 3.0f);
    dl->AddLine(ImVec2(a.x + 3, mid), ImVec2(b.x - 3, mid), vis::kLine, 1.0f);
    static ImVec2 pts[kWin];
    float peak = 0.0f;
    for (int i = 0; i < kWin; ++i) {
        float v = std::clamp(buf[start + i] * 1.6f, -1.0f, 1.0f);   // a little headroom gain for readability
        peak = std::max(peak, std::fabs(buf[start + i]));
        pts[i] = ImVec2(a.x + 3 + (size.x - 6) * i / (kWin - 1), mid - v * amp);
    }
    dl->AddPolyline(pts, kWin, peak > 0.97f ? vis::kRed : vis::kTeal, 0, 1.4f);
}

namespace detail {
// time knob (0..2 s) -> 0..1 width share, same sqrt law the param cells use
inline float TimeShare(float s) { return std::sqrt(std::clamp(s / 2.0f, 0.0f, 1.0f)); }
}

// ADSR drawn to the patch: segment widths follow the (sqrt-scaled) times,
// sustain is a fixed-width plateau.
inline void DrawEnvelope(const SynthPatch& p, ImVec2 size, ImU32 col = vis::kAccent) {
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##adsr", size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 b(a.x + size.x, a.y + size.y);
    vis::DrawWell(dl, a, b);
    float wA = 0.12f + detail::TimeShare(p.attack), wD = 0.12f + detail::TimeShare(p.decay);
    float wS = 0.45f, wR = 0.12f + detail::TimeShare(p.release);
    float tot = wA + wD + wS + wR;
    float x0 = a.x + 4, W = size.x - 8, yBot = b.y - 4, yTop = a.y + 4, H = yBot - yTop;
    float sus = std::clamp(p.sustain, 0.0f, 1.0f);
    float xA = x0 + W * wA / tot, xD = xA + W * wD / tot, xS = xD + W * wS / tot, xR = x0 + W;
    ImVec2 pts[5] = { {x0, yBot}, {xA, yTop}, {xD, yBot - H * sus}, {xS, yBot - H * sus}, {xR, yBot} };
    for (int i = 0; i < 4; ++i) {
        // soft fill under the curve: one convex quad per segment
        ImVec2 q[4] = { pts[i], pts[i + 1], {pts[i + 1].x, yBot}, {pts[i].x, yBot} };
        dl->AddConvexPolyFilled(q, 4, (col & 0x00FFFFFF) | (40u << 24));
    }
    dl->AddPolyline(pts, 5, col, 0, 1.8f);
    // segment ticks
    for (float x : {xA, xD, xS}) dl->AddLine(ImVec2(x, yBot - 3), ImVec2(x, yBot), vis::kLineHi, 1.0f);
}

// One LFO cycle in the chosen shape, scaled by depth, with the live phase dot
// when the engine's LFO would be running (rate-driven from wall time).
inline void DrawLfoPreview(const SynthPatch& p, ImVec2 size) {
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##lfo", size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 b(a.x + size.x, a.y + size.y);
    vis::DrawWell(dl, a, b);
    bool on = p.lfoTarget != LfoTarget::Off;
    float mid = (a.y + b.y) * 0.5f, amp = (size.y * 0.5f - 4.0f) * std::clamp(p.lfoDepth, 0.0f, 1.0f);
    dl->AddLine(ImVec2(a.x + 3, mid), ImVec2(b.x - 3, mid), vis::kLine, 1.0f);
    constexpr int N = 48;
    ImVec2 pts[N + 1];
    auto shapeAt = [&](float ph) {
        switch (p.lfoShape) {
            case Waveform::Sine: return std::sin(ph * 6.2831853f);
            case Waveform::Triangle: return ph < 0.5f ? (ph * 4.0f - 1.0f) : (3.0f - ph * 4.0f);
            case Waveform::Saw: return 1.0f - 2.0f * ph;
            case Waveform::Square: return ph < 0.5f ? 1.0f : -1.0f;
        }
        return 0.0f;
    };
    for (int i = 0; i <= N; ++i) {
        float ph = static_cast<float>(i) / N;
        pts[i] = ImVec2(a.x + 3 + (size.x - 6) * ph, mid - shapeAt(ph) * (on ? amp : 0.0f));
    }
    ImU32 col = on ? vis::kViolet : vis::kLineHi;
    dl->AddPolyline(pts, N + 1, col, 0, 1.6f);
    if (on) {
        float rate = std::max(0.05f, p.lfoRateHz);
        float ph = std::fmod(static_cast<float>(ImGui::GetTime()) * rate, 1.0f);
        dl->AddCircleFilled(ImVec2(a.x + 3 + (size.x - 6) * ph, mid - shapeAt(ph) * amp), 3.0f, vis::kAccentHi);
    }
}

} // namespace t8::desktop
