#include "Effects.h"
#include <algorithm>
#include <cmath>

namespace t8 {

// ---- DelayEffect ----------------------------------------------------------

void DelayEffect::Init(int sampleRate) {
    sampleRate_ = sampleRate;
    // 2.0s max delay time is comfortably past FxSettings::delayTimeSec's
    // documented 1.5s ceiling, with a little headroom.
    size_t maxSamples = static_cast<size_t>(sampleRate_ * 2.0) + 1;
    bufL_.assign(maxSamples, 0.0f);
    bufR_.assign(maxSamples, 0.0f);
    writePos_ = 0;
}

void DelayEffect::Process(const float* input, float* outWet, int numFrames, float timeSec, float feedback, float tone, bool pingPong) {
    size_t bufSize = bufL_.size();
    if (bufSize == 0) { // Init() never called -- fail safe to silence, not a crash
        std::fill(outWet, outWet + static_cast<size_t>(numFrames) * 2, 0.0f);
        return;
    }
    const double maxDelay = static_cast<double>(bufSize) - 4.0;
    const double target = std::clamp(static_cast<double>(timeSec) * sampleRate_, 1.0, maxDelay);
    if (curDelay_ < 0.0) curDelay_ = target;
    const float fb = std::clamp(feedback, 0.0f, 0.95f); // hard safety ceiling regardless of what the UI allows
    // glide: close the gap in about 80 ms (at most 2 samples per sample, so a big jump bends the repeats instead of clicking)
    const double glide = 1.0 - std::exp(-1.0 / (0.08 * sampleRate_));
    const float fc = 1200.0f * std::pow(10.0f, std::clamp(tone, 0.0f, 1.0f));   // 1.2 kHz .. 12 kHz
    const float lpA = 1.0f - std::exp(-2.0f * 3.14159265f * fc / static_cast<float>(sampleRate_));
    const float hpA = std::exp(-2.0f * 3.14159265f * 150.0f / static_cast<float>(sampleRate_));

    for (int i = 0; i < numFrames; ++i) {
        double step = (target - curDelay_) * glide;
        step = std::clamp(step, -2.0, 2.0);
        curDelay_ += step;
        // 4-point Hermite read at a fractional delay
        const double rp = static_cast<double>(writePos_) + static_cast<double>(bufSize) - curDelay_;
        const size_t i1 = static_cast<size_t>(rp) % bufSize;
        const float fr = static_cast<float>(rp - std::floor(rp));
        const size_t i0 = (i1 + bufSize - 1) % bufSize, i2 = (i1 + 1) % bufSize, i3 = (i1 + 2) % bufSize;
        auto herm = [&](const std::vector<float>& b) {
            const float y0 = b[i0], y1 = b[i1], y2 = b[i2], y3 = b[i3];
            const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
            return ((c3 * fr + c2) * fr + c1) * fr + y1;
        };
        const float wetL = herm(bufL_), wetR = herm(bufR_);
        outWet[static_cast<size_t>(i) * 2 + 0] = wetL;
        outWet[static_cast<size_t>(i) * 2 + 1] = wetR;

        const float inL = input[static_cast<size_t>(i) * 2 + 0];
        const float inR = input[static_cast<size_t>(i) * 2 + 1];
        // feedback path: high-pass, tone low-pass, soft limit
        float fL = wetL, fR = wetR;
        if (pingPong) std::swap(fL, fR);                       // the repeats cross over to the other side
        hpL_ = hpA * (hpL_ + fL - hpPrevL_); hpPrevL_ = fL; fL = hpL_;
        hpR_ = hpA * (hpR_ + fR - hpPrevR_); hpPrevR_ = fR; fR = hpR_;
        lpL_ += lpA * (fL - lpL_); fL = lpL_;
        lpR_ += lpA * (fR - lpR_); fR = lpR_;
        float wL = inL, wR = inR;
        if (pingPong) { const float m = 0.5f * (inL + inR); wL = m; wR = 0.0f; }   // the first echo comes out of the left
        auto lim = [](float v) { return std::fabs(v) < 0.6f ? v : std::copysign(0.6f + 0.4f * std::tanh((std::fabs(v) - 0.6f) / 0.4f), v); };
        bufL_[writePos_] = wL + lim(fL * fb);
        bufR_[writePos_] = wR + lim(fR * fb);
        writePos_ = (writePos_ + 1) % bufSize;
    }
}

// ---- ReverbEffect (v2: 8-line stereo FDN) ------------------------------------------------------------------------
namespace {
// line lengths in samples at 48 kHz (mutually coprime, 30-65 ms), allpass diffuser lengths for the left / right input
constexpr int kFdnLengths[8] = {1423, 1637, 1873, 2087, 2339, 2593, 2837, 3089};
constexpr int kDiffLeft[4]  = {113, 163, 223, 313};
constexpr int kDiffRight[4] = {127, 181, 241, 337};
constexpr float kDiffG = 0.6f;
// output mixes: rows of an 8x8 Hadamard matrix (orthogonal => decorrelated left / right)
constexpr float kOutL[8] = { 1, -1,  1, -1,  1, -1,  1, -1};
constexpr float kOutR[8] = { 1,  1, -1, -1,  1,  1, -1, -1};
constexpr double kRevPi = 3.14159265358979323846;
}

float ReverbEffect::Rt60ForSize(float size) {
    const float s = std::clamp(size, 0.05f, 0.97f);
    return std::clamp(0.085f / -std::log10(s), 0.12f, 8.0f);   // the old Freeverb curve
}

void ReverbEffect::Init(int sampleRate) {
    sampleRate_ = sampleRate;
    const double rateScale = sampleRate_ / 48000.0;
    const int depth = static_cast<int>(6 * rateScale) + 2;           // room for the modulation
    for (int i = 0; i < kLines; ++i) {
        Line& l = lines_[i];
        l.len = static_cast<float>(kFdnLengths[i] * rateScale);
        l.buf.assign(static_cast<size_t>(l.len) + depth + 8, 0.0f);
        l.pos = 0; l.lp = 0.0f; l.gain = 0.0f; l.k = 0.0f;
        const double inc = 2.0 * kRevPi * (0.11 + 0.043 * i) / sampleRate_;   // 0.11 .. 0.41 Hz, different per line
        l.rc = std::cos(inc); l.rs = std::sin(inc);
        const double ph = 2.0 * kRevPi * (0.137 * i);
        l.cs = std::cos(ph); l.sn = std::sin(ph);
    }
    for (int side = 0; side < 2; ++side) {
        for (int a = 0; a < 4; ++a) {
            const int len = side == 0 ? kDiffLeft[a] : kDiffRight[a];
            diffusers_[side][a].buf.assign(std::max<size_t>(1, static_cast<size_t>(len * rateScale)), 0.0f);
            diffusers_[side][a].pos = 0;
        }
        pre_[side].assign(static_cast<size_t>(sampleRate_ * 0.125) + 2, 0.0f);
        hpState_[side] = hpPrev_[side] = 0.0f;
    }
    prePos_ = 0;
    hpCoef_ = std::exp(-2.0f * static_cast<float>(kRevPi) * 100.0f / static_cast<float>(sampleRate_));
    outGain_ = 0.42f;
}

void ReverbEffect::Process(const float* input, float* outWet, int numFrames, float size, float damp, float preDelayMs) {
    if (lines_[0].buf.empty()) { std::fill(outWet, outWet + static_cast<size_t>(numFrames) * 2, 0.0f); return; }   // Init() never called
    const float sr = static_cast<float>(sampleRate_);
    const float rt60 = Rt60ForSize(size) * 1.35f;   // the damping shortens the broadband decay: this keeps the measured RT60 on the SIZE curve   // the damping low-pass shortens the broadband decay: this keeps the measured RT60 on the SIZE curve
    // damping (Jot style): the lows decay with RT60, the highs (above ~5 kHz) with RT60 x ratio, ratio = 1 (DAMP 0) ... 0.12 (DAMP 1)
    const float a = std::exp(-2.0f * static_cast<float>(kRevPi) * 5000.0f / sr);
    const float ratio = std::pow(0.12f, std::clamp(damp, 0.0f, 1.0f));
    float targetGain[kLines], targetK[kLines];
    for (int i = 0; i < kLines; ++i) {
        const float perPass = -3.0f * lines_[i].len / sr;                          // log10 gain per trip = perPass / RT60
        targetGain[i] = std::pow(10.0f, perPass / rt60);
        targetK[i] = 1.0f - std::pow(10.0f, perPass * (1.0f / (rt60 * ratio) - 1.0f / rt60));   // 1 - (high gain / low gain)
    }
    const size_t preLen = pre_[0].size();
    const size_t preSamples = std::min(preLen - 1, static_cast<size_t>(std::clamp(preDelayMs, 0.0f, 120.0f) * 0.001f * sr));
    const float modDepth = 2.5f * sr / 48000.0f;
    const float szc = std::clamp(size, 0.0f, 0.97f);
    const float outGain = outGain_ * (0.4f + 0.4f * szc * szc);   // calibrated to the old reverb's loudness (white noise in, sizes 0.3 - 0.9 within 1.5 dB)

    for (int n = 0; n < numFrames; ++n) {
        // input: high-pass, pre-delay, diffusion
        float x[2];
        for (int c = 0; c < 2; ++c) {
            const float in = input[static_cast<size_t>(n) * 2 + c];
            hpState_[c] = hpCoef_ * (hpState_[c] + in - hpPrev_[c]);      // one-pole high-pass
            hpPrev_[c] = in;
            pre_[c][prePos_] = hpState_[c];
            float v = pre_[c][(prePos_ + preLen - preSamples) % preLen];
            for (auto& ap : diffusers_[c]) {
                const float bo = ap.buf[ap.pos];
                const float y = -v * kDiffG + bo;
                ap.buf[ap.pos] = v + bo * kDiffG;
                ap.pos = (ap.pos + 1) % ap.buf.size();
                v = y;
            }
            x[c] = v;
        }
        prePos_ = (prePos_ + 1) % preLen;

        // read the lines (linear interpolation, slowly modulated)
        float y[kLines], sum = 0.0f, outL = 0.0f, outR = 0.0f;
        for (int i = 0; i < kLines; ++i) {
            Line& l = lines_[i];
            const double ncs = l.cs * l.rc - l.sn * l.rs, nsn = l.sn * l.rc + l.cs * l.rs;
            l.cs = ncs; l.sn = nsn;
            const float d = l.len + modDepth * static_cast<float>(l.sn);
            const size_t sz = l.buf.size();
            const float rp = static_cast<float>(l.pos) + static_cast<float>(sz) - d;
            const size_t i0 = static_cast<size_t>(rp), i1 = i0 + 1;
            const float fr = rp - static_cast<float>(i0);
            const float v = l.buf[i0 % sz] * (1.0f - fr) + l.buf[i1 % sz] * fr;
            l.gain += (targetGain[i] - l.gain) * 0.002f;
            l.k += (targetK[i] - l.k) * 0.002f;
            l.lp = (1.0f - a) * v + a * l.lp;
            y[i] = ((1.0f - l.k) * v + l.k * l.lp) * l.gain;
            sum += y[i];
            outL += kOutL[i] * v; outR += kOutR[i] * v;
        }
        // Householder feedback matrix + input injection
        const float mix = sum * (2.0f / kLines);
        for (int i = 0; i < kLines; ++i) {
            Line& l = lines_[i];
            l.buf[l.pos] = (kOutL[i] * x[0] + kOutR[i] * x[1]) * 0.35f + (y[i] - mix);   // left excites the left output mix, right the right one
            l.pos = (l.pos + 1) % l.buf.size();
        }
        outWet[static_cast<size_t>(n) * 2 + 0] = outL * outGain;
        outWet[static_cast<size_t>(n) * 2 + 1] = outR * outGain;
    }
}

// ---- TrackInsertFx ------------------------------------------------------
namespace { constexpr double kInsertFxPi = 3.14159265358979323846; }

void TrackInsertFx::Init(int sampleRate) {
    sampleRate_ = sampleRate;
    // One-pole low-pass coefficient for a fixed ~800Hz pivot (standard RC
    // one-pole design: a = 1 - exp(-2*pi*fc/fs)). The tilt EQ crossfades
    // between this low-pass and its complementary high-pass (input minus
    // low-pass) -- see Process() below.
    float fc = 800.0f;
    eqCoeff_ = 1.0f - std::exp(-2.0f * static_cast<float>(kInsertFxPi) * fc / static_cast<float>(sampleRate_));
    eqLpL_ = eqLpR_ = 0.0f;
    // ~35ms ceiling comfortably covers the chorus's modulated 6..18ms
    // delay range below, with a little headroom.
    size_t maxDelaySamples = static_cast<size_t>(sampleRate_ * 0.035) + 4;
    chorusBufL_.assign(maxDelaySamples, 0.0f);
    chorusBufR_.assign(maxDelaySamples, 0.0f);
    chorusWritePos_ = 0;
    chorusPhase_ = 0.0f;
}

void TrackInsertFx::Process(float* buf, int numFrames, float drive, float eqTilt, float chorusMix) {
    drive = std::clamp(drive, 0.0f, 1.0f);
    float tilt = std::clamp(eqTilt, -1.0f, 1.0f);
    chorusMix = std::clamp(chorusMix, 0.0f, 1.0f);
    // Fast path: all three off (the default) -- existing projects/presets
    // must sound bit-identical to before this feature existed.
    if (drive <= 0.0f && tilt == 0.0f && chorusMix <= 0.0f) return;
    size_t bufSize = chorusBufL_.size();
    if (bufSize == 0) return; // Init() never called -- fail safe, skip rather than crash

    float driveGain = 1.0f + drive * 4.0f; // same 1..5 curve as the master-bus DRIVE control
    // Complementary low/high shelf gains for the tilt: at tilt=0 both are
    // 1.0 (flat). +-0.6 range keeps the extremes musical rather than
    // completely killing one band.
    float lowGain = 1.0f - tilt * 0.6f;
    float highGain = 1.0f + tilt * 0.6f;

    constexpr float kChorusLfoHz = 0.6f;
    constexpr float kChorusBaseMs = 12.0f;
    constexpr float kChorusDepthMs = 6.0f;
    float lfoInc = kChorusLfoHz / static_cast<float>(sampleRate_);
    float depthMs = kChorusDepthMs * chorusMix; // low mix = gentler modulation, not just less blend

    for (int i = 0; i < numFrames; ++i) {
        float l = buf[static_cast<size_t>(i) * 2 + 0];
        float r = buf[static_cast<size_t>(i) * 2 + 1];

        // ---- 1. EQ: one-knob tilt (complementary low/high shelf) --------
        if (tilt != 0.0f) {
            eqLpL_ += eqCoeff_ * (l - eqLpL_);
            eqLpR_ += eqCoeff_ * (r - eqLpR_);
            float hpL = l - eqLpL_;
            float hpR = r - eqLpR_;
            l = eqLpL_ * lowGain + hpL * highGain;
            r = eqLpR_ * lowGain + hpR * highGain;
        }

        // ---- 2. Drive: same tanh saturation knee as the master bus. -----
        if (drive > 0.0f) {
            l = std::tanh(l * driveGain);
            r = std::tanh(r * driveGain);
        }

        // ---- 3. Chorus: short sine-LFO-modulated delay, mixed with dry. -
        if (chorusMix > 0.0f) {
            chorusBufL_[chorusWritePos_] = l;
            chorusBufR_[chorusWritePos_] = r;
            float lfo = 0.5f * (1.0f + std::sin(2.0f * static_cast<float>(kInsertFxPi) * chorusPhase_));
            float delayMs = kChorusBaseMs + depthMs * lfo;
            float delaySamples = delayMs * 0.001f * static_cast<float>(sampleRate_);
            float readPos = static_cast<float>(chorusWritePos_) - delaySamples;
            while (readPos < 0.0f) readPos += static_cast<float>(bufSize);
            size_t idx0 = static_cast<size_t>(readPos) % bufSize;
            size_t idx1 = (idx0 + 1) % bufSize;
            float frac = readPos - std::floor(readPos);
            float wetL = chorusBufL_[idx0] * (1.0f - frac) + chorusBufL_[idx1] * frac;
            float wetR = chorusBufR_[idx0] * (1.0f - frac) + chorusBufR_[idx1] * frac;
            l = l * (1.0f - 0.5f * chorusMix) + wetL * 0.5f * chorusMix;
            r = r * (1.0f - 0.5f * chorusMix) + wetR * 0.5f * chorusMix;
            chorusWritePos_ = (chorusWritePos_ + 1) % bufSize;
            chorusPhase_ += lfoInc;
            if (chorusPhase_ >= 1.0f) chorusPhase_ -= 1.0f;
        }

        buf[static_cast<size_t>(i) * 2 + 0] = l;
        buf[static_cast<size_t>(i) * 2 + 1] = r;
    }
}

// ---- MasterFilter -----------------------------------------------------

namespace {
// Same one-pole design TrackInsertFx's tilt EQ pivot uses (a = 1 -
// exp(-2*pi*fc/fs)), just parameterized on a live cutoff instead of a
// fixed 800Hz pivot. Clamped well inside Nyquist so a runaway fader value
// can never produce an unstable coefficient.
float OnePoleCoeff(float fc, int sampleRate) {
    fc = std::clamp(fc, 10.0f, static_cast<float>(sampleRate) * 0.45f);
    return 1.0f - std::exp(-2.0f * static_cast<float>(kInsertFxPi) * fc / static_cast<float>(sampleRate));
}
}

void MasterFilter::Init(int sampleRate) {
    sampleRate_ = sampleRate;
    hpLp1L_ = hpLp1R_ = hpLp2L_ = hpLp2R_ = 0.0f;
    lpL1_ = lpR1_ = lpL2_ = lpR2_ = 0.0f;
    hpSmooth_ = lpSmooth_ = 0.0f;
}

void MasterFilter::Process(float* buf, int numFrames, float hpAmount, float lpAmount) {
    hpAmount = std::clamp(hpAmount, 0.0f, 1.0f);
    lpAmount = std::clamp(lpAmount, 0.0f, 1.0f);
    const bool idle = hpAmount <= 0.0f && lpAmount <= 0.0f && hpSmooth_ < 0.0005f && lpSmooth_ < 0.0005f;
    if (idle) { // both faders untouched and fully released -- fast path, byte-identical to before this feature existed
        hpSmooth_ = lpSmooth_ = 0.0f;
        hpLp1L_ = hpLp1R_ = hpLp2L_ = hpLp2R_ = 0.0f;
        lpL1_ = lpR1_ = lpL2_ = lpR2_ = 0.0f;
        return;
    }

    // The fader value is smoothed (~25 ms) and the coefficients refreshed every
    // 32 frames: a fader jump or a release back to 0 no longer steps the filter
    // (which clicked on a bass with a low LPF), and the filter keeps running
    // until it is fully open, so switching it off is seamless.
    constexpr int kChunk = 32;
    const float smoothK = 1.0f - std::exp(-static_cast<float>(kChunk) / (0.025f * static_cast<float>(sampleRate_)));
    for (int base = 0; base < numFrames; base += kChunk) {
        const int n = std::min(kChunk, numFrames - base);
        hpSmooth_ += (hpAmount - hpSmooth_) * smoothK;
        lpSmooth_ += (lpAmount - lpSmooth_) * smoothK;
        const float hpS = hpSmooth_, lpS = lpSmooth_;
        // Squared curve on both: most of the audible sweep lands in the top
        // half of the fader's travel (a real filter knob's low end barely does
        // anything either).
        float hpFc = 20.0f + hpS * hpS * 3980.0f;      // 20Hz (open) .. 4000Hz (aggressive)
        float lpFc = 20000.0f - lpS * lpS * 19850.0f;  // 20000Hz (open) .. 150Hz (aggressive)
        float hpA = OnePoleCoeff(hpFc, sampleRate_);
        float lpA = OnePoleCoeff(lpFc, sampleRate_);
        const bool doHp = hpS > 0.0005f, doLp = lpS > 0.0005f;

        for (int i = base; i < base + n; ++i) {
            float l = buf[static_cast<size_t>(i) * 2 + 0];
            float r = buf[static_cast<size_t>(i) * 2 + 1];
            if (doHp) {
                // HPF = input minus a low-pass, cascaded twice for a steeper slope.
                hpLp1L_ += hpA * (l - hpLp1L_); l = l - hpLp1L_;
                hpLp2L_ += hpA * (l - hpLp2L_); l = l - hpLp2L_;
                hpLp1R_ += hpA * (r - hpLp1R_); r = r - hpLp1R_;
                hpLp2R_ += hpA * (r - hpLp2R_); r = r - hpLp2R_;
            }
            if (doLp) {
                lpL1_ += lpA * (l - lpL1_); l = lpL1_;
                lpL2_ += lpA * (l - lpL2_); l = lpL2_;
                lpR1_ += lpA * (r - lpR1_); r = lpR1_;
                lpR2_ += lpA * (r - lpR2_); r = lpR2_;
            }
            buf[static_cast<size_t>(i) * 2 + 0] = l;
            buf[static_cast<size_t>(i) * 2 + 1] = r;
        }
    }
}

// ---- RepeaterEffect -----------------------------------------------------

void RepeaterEffect::Init(int sampleRate) {
    // Generous ceiling: a full step at the slowest allowed bpm (20bpm ->
    // 0.75s/16th note, see Sequencer::SamplesPerStep's 20bpm floor); the longest
    // density asks for 8 steps = 6s at that floor, so 8s covers every case.
    capacity_ = static_cast<int>(sampleRate * 8.0) + 1;
    bufferL_.assign(static_cast<size_t>(capacity_), 0.0f);
    bufferR_.assign(static_cast<size_t>(capacity_), 0.0f);
    captureLen_ = 0;
    writePos_ = 0;
    playPos_ = 0;
    capturing_ = false;
    wasOn_ = false;
}

void RepeaterEffect::Process(float* buf, int numFrames, bool on, int captureLenFrames) {
    if (bufferL_.empty()) return; // Init() never called -- fail safe, skip rather than crash
    constexpr int kFade = 64;      // ~1.5 ms: rounds the loop seam and the hand-back to live audio
    if (!on) {
        if (wasOn_) {            // just switched off
            fadeLeft_ = capturing_ ? 0 : kFade;   // crossfade out of the loop (nothing to fade if it never started)
            capturing_ = false;
            writePos_ = 0;
        }
        wasOn_ = false;
        if (fadeLeft_ > 0 && captureLen_ > 0) {
            for (int i = 0; i < numFrames && fadeLeft_ > 0; ++i, --fadeLeft_) {
                float x = 1.0f - static_cast<float>(fadeLeft_) / static_cast<float>(kFade);   // 0 = loop, 1 = live
                size_t k = static_cast<size_t>(i) * 2;
                buf[k + 0] = bufferL_[static_cast<size_t>(playPos_)] * (1.0f - x) + buf[k + 0] * x;
                buf[k + 1] = bufferR_[static_cast<size_t>(playPos_)] * (1.0f - x) + buf[k + 1] * x;
                playPos_ = (playPos_ + 1) % std::max(1, captureLen_);
            }
        }
        if (fadeLeft_ <= 0) playPos_ = 0;
        return; // transparent passthrough
    }

    int want = std::clamp(captureLenFrames, 1, capacity_);
    if (!wasOn_ || want != captureLen_) {
        // Just engaged, or the density changed mid-loop -- (re)start the
        // capture at the new length. The caller (Sequencer) only flips these on
        // step boundaries, so the window starts on the grid.
        captureLen_ = want;
        capturing_ = true;
        writePos_ = 0;
        playPos_ = 0;
        fadeLeft_ = 0;
    }
    wasOn_ = true;

    for (int i = 0; i < numFrames; ++i) {
        if (capturing_) {
            // Pass the real audio through unmodified while capturing, so
            // engaging the effect is seamless rather than a dropout.
            bufferL_[static_cast<size_t>(writePos_)] = buf[static_cast<size_t>(i) * 2 + 0];
            bufferR_[static_cast<size_t>(writePos_)] = buf[static_cast<size_t>(i) * 2 + 1];
            ++writePos_;
            if (writePos_ >= captureLen_) {
                capturing_ = false;
                playPos_ = 0;
            }
        } else {
            // short fade in / out at each end of the window, so the seam of the
            // loop does not step (a click on bass-heavy material)
            float g = 1.0f;
            const int tail = captureLen_ - 1 - playPos_;
            const int edge = std::min(playPos_, tail);
            if (captureLen_ > 4 * kFade / 2 && edge < kFade / 2) g = static_cast<float>(edge + 1) / static_cast<float>(kFade / 2 + 1);
            buf[static_cast<size_t>(i) * 2 + 0] = bufferL_[static_cast<size_t>(playPos_)] * g;
            buf[static_cast<size_t>(i) * 2 + 1] = bufferR_[static_cast<size_t>(playPos_)] * g;
            playPos_ = (playPos_ + 1) % std::max(1, captureLen_);
        }
    }
}

} // namespace t8
