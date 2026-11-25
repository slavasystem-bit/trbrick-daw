#include "Risers.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <filesystem>
#include <cctype>

namespace t8 {

namespace {
// Matches tools/gen_risers.py's generation order exactly -- riser0.wav is
// index 0/"WHITE NOISE SWEEP", etc. Kept here rather than read from the
// WAV files themselves (which carry no metadata) so the on-screen browse
// list always has a real name instead of "RISER 3".
constexpr const char* kRiserNames[RiserPlayer::kMaxRisers] = {
    "WHITE NOISE SWEEP", "PITCH RISE", "REVERSE SWELL", "TENSION DRONE",
    "FILTERED BUILD", "SUB RISER", "METALLIC SWEEP", "SIREN SWEEP",
    "IMPACT RISER", "GLITCH BUILD",
};

// ---- v44.5: synthesized transitions (no samples needed) -------------------------------------------------------------------
// Deterministic noise (a fixed seed), so the same sound comes out on every start.
struct NoiseGen {
    uint32_t s;
    explicit NoiseGen(uint32_t seed) : s(seed) {}
    float Next() { s = s * 1664525u + 1013904223u; return static_cast<float>(static_cast<int32_t>(s)) / 2147483648.0f; }
};
SampleBuffer MakeStereo(int frames, int sampleRate) {
    SampleBuffer b; b.channels = 2; b.sampleRate = sampleRate; b.data.assign(static_cast<size_t>(frames) * 2, 0.0f); return b;
}
void NormalizeFade(SampleBuffer& b, float peakTarget, int fadeFrames) {
    float pk = 0.0f; for (float v : b.data) pk = std::max(pk, std::fabs(v));
    const float g = pk > 1e-6f ? peakTarget / pk : 1.0f;
    for (float& v : b.data) v *= g;
    const int n = b.frames();
    for (int i = 0; i < std::min(fadeFrames, n); ++i) {            // click-free ends
        const float f = static_cast<float>(i) / fadeFrames;
        for (int c = 0; c < 2; ++c) { b.data[static_cast<size_t>(n - 1 - i) * 2 + c] *= f; if (i < fadeFrames / 4) b.data[static_cast<size_t>(i) * 2 + c] *= static_cast<float>(i) / (fadeFrames / 4); }
    }
}
// DOWNLIFTER: noise whose low-pass falls from the top to the bottom while it fades - the exhale after a drop.
SampleBuffer MakeDownlifter(int sr) {
    const double len = 2.6; SampleBuffer b = MakeStereo(static_cast<int>(len * sr), sr);
    NoiseGen n0(11), n1(23); float l0 = 0, l1 = 0, l0b = 0, l1b = 0;
    for (int i = 0; i < b.frames(); ++i) {
        const double t = i / (len * sr), fc = 9000.0 * std::pow(180.0 / 9000.0, t);
        const float a = 1.0f - std::exp(-6.2831853f * static_cast<float>(fc) / sr);
        const float env = static_cast<float>(std::pow(1.0 - t, 1.6));
        l0 += a * (n0.Next() - l0); l0b += a * (l0 - l0b);
        l1 += a * (n1.Next() - l1); l1b += a * (l1 - l1b);
        b.data[static_cast<size_t>(i) * 2] = l0b * env; b.data[static_cast<size_t>(i) * 2 + 1] = l1b * env;
    }
    NormalizeFade(b, 0.8f, sr / 50); return b;
}
// IMPACT: a pitch-dropping boom, a short noise crack and a long dark tail.
SampleBuffer MakeImpact(int sr) {
    const double len = 3.2; SampleBuffer b = MakeStereo(static_cast<int>(len * sr), sr);
    NoiseGen n0(31), n1(47); double ph = 0.0; float c0 = 0, c1 = 0, t0 = 0, t1 = 0;
    for (int i = 0; i < b.frames(); ++i) {
        const double t = i / static_cast<double>(sr);
        const double f = 34.0 + 70.0 * std::exp(-t / 0.09);
        ph += 6.2831853 * f / sr;
        const float boom = static_cast<float>(std::sin(ph) * std::exp(-t / 0.55) * (1.0 - std::exp(-t / 0.002)));
        const float ac = 1.0f - std::exp(-6.2831853f * 3500.0f / sr), at = 1.0f - std::exp(-6.2831853f * 380.0f / sr);
        const float x0 = n0.Next(), x1 = n1.Next();
        c0 += ac * (x0 - c0); c1 += ac * (x1 - c1); t0 += at * (x0 - t0); t1 += at * (x1 - t1);
        const float crack = static_cast<float>(std::exp(-t / 0.045));
        const float tail = static_cast<float>(std::exp(-t / 0.9));
        b.data[static_cast<size_t>(i) * 2] = boom * 0.9f + c0 * crack * 0.8f + t0 * tail * 1.8f;
        b.data[static_cast<size_t>(i) * 2 + 1] = boom * 0.9f + c1 * crack * 0.8f + t1 * tail * 1.8f;
    }
    NormalizeFade(b, 0.9f, sr / 40); return b;
}
// SUB DROP: a sine that sinks from the low mids to the bottom octave, a touch of saturation, and fades.
SampleBuffer MakeSubDrop(int sr) {
    const double len = 2.8; SampleBuffer b = MakeStereo(static_cast<int>(len * sr), sr);
    double ph = 0.0;
    for (int i = 0; i < b.frames(); ++i) {
        const double t = i / static_cast<double>(sr);
        const double f = 27.0 + 95.0 * std::exp(-t / 0.5);
        ph += 6.2831853 * f / sr;
        const double env = (1.0 - std::exp(-t / 0.004)) * std::exp(-t / 0.95);
        const float v = static_cast<float>(std::tanh(1.6 * std::sin(ph)) * env);
        b.data[static_cast<size_t>(i) * 2] = v; b.data[static_cast<size_t>(i) * 2 + 1] = v;
    }
    NormalizeFade(b, 0.9f, sr / 50); return b;
}
// REVERSE CRASH: a bright noise burst with a long decay, played backwards - it swells and ends exactly on the beat.
SampleBuffer MakeReverseCrash(int sr) {
    const double len = 3.0; SampleBuffer b = MakeStereo(static_cast<int>(len * sr), sr);
    NoiseGen n0(59), n1(71); float h0 = 0, h1 = 0, p0 = 0, p1 = 0;
    for (int i = 0; i < b.frames(); ++i) {
        const double t = i / static_cast<double>(sr);
        const float env = static_cast<float>(std::exp(-t / 0.75) * (1.0 - std::exp(-t / 0.003)));
        const float x0 = n0.Next(), x1 = n1.Next();
        h0 = x0 - p0; p0 = x0; h1 = x1 - p1; p1 = x1;          // first difference = a rough high-pass
        b.data[static_cast<size_t>(i) * 2] = h0 * env; b.data[static_cast<size_t>(i) * 2 + 1] = h1 * env;
    }
    const int n = b.frames();                                  // reverse in place
    for (int i = 0; i < n / 2; ++i) for (int c = 0; c < 2; ++c) std::swap(b.data[static_cast<size_t>(i) * 2 + c], b.data[static_cast<size_t>(n - 1 - i) * 2 + c]);
    NormalizeFade(b, 0.8f, sr / 60); return b;
}
}

int TransitionSlotOfStem(const std::string& stem) {
    if (stem.size() < 3 || stem[0] != 't' || !std::isdigit(static_cast<unsigned char>(stem[1])) || !std::isdigit(static_cast<unsigned char>(stem[2]))) return -1;
    const int nn = (stem[1] - '0') * 10 + (stem[2] - '0');
    return nn >= 1 && nn <= RiserPlayer::kNumUser ? nn - 1 : -1;
}
std::string TransitionDisplayName(const std::string& stem) {
    std::string n = stem.size() > 4 && TransitionSlotOfStem(stem) >= 0 ? stem.substr(4) : stem;
    for (char& c : n) c = c == '_' ? ' ' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return n.empty() ? stem : n;
}
std::vector<UserTransition> LoadUserTransitionFolder(const std::string& dir, int sampleRate) {
    namespace fs = std::filesystem;
    std::vector<UserTransition> list;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return list;
    for (const auto& f : fs::directory_iterator(dir, ec)) {
        if (!f.is_regular_file(ec) || f.path().extension() != ".wav") continue;
        const std::string stem = f.path().stem().string();
        const int slot = TransitionSlotOfStem(stem);
        if (slot < 0) continue;
        UserTransition u; u.slot = slot; u.name = TransitionDisplayName(stem);
        if (LoadWav(f.path().string(), u.audio, sampleRate) && !u.audio.empty()) list.push_back(std::move(u));
    }
    return list;
}

int RiserPlayer::LoadFromFolder(const std::string& folder, int sampleRate) {
    loadedCount_ = 0;
    countSlots_ = 0;
    loadedSampleRate_ = sampleRate;
    for (int i = 0; i < kMaxRisers; ++i) {
        std::string path = folder + "/riser" + std::to_string(i) + ".wav";
        auto buf = std::make_shared<SampleBuffer>();
        if (LoadWav(path, *buf, sampleRate)) {
            samples_[i] = buf;
            names_[i] = kRiserNames[i];
            ComputeHit(i);
            ++loadedCount_;
        } else {
            samples_[i].reset();
        }
    }
    // the synthesized transitions come after the stock files, at fixed indices (a project stores the index)
    AddGenerated(kMaxRisers + 0, "DOWNLIFTER", MakeDownlifter(sampleRate));
    AddGenerated(kMaxRisers + 1, "IMPACT", MakeImpact(sampleRate));
    AddGenerated(kMaxRisers + 2, "SUB DROP", MakeSubDrop(sampleRate));
    AddGenerated(kMaxRisers + 3, "REVERSE CRASH", MakeReverseCrash(sampleRate));
    for (int i = kFirstUser; i < kMaxSounds; ++i) { samples_[i].reset(); names_[i].clear(); }
    countSlots_ = kFirstUser;   // user sounds extend this (SetUserSound)
    return loadedCount_;
}

void RiserPlayer::ClearUserSounds() {
    StopAllVoices();
    for (int i = kFirstUser; i < kMaxSounds; ++i) { samples_[i].reset(); names_[i].clear(); }
    countSlots_ = kFirstUser;
}

void RiserPlayer::SetUserSound(int slot, const std::string& name, SampleBuffer&& buf) {
    if (slot < 0 || slot >= kNumUser || buf.empty()) return;
    StopAllVoices();
    const int idx = kFirstUser + slot;
    samples_[idx] = std::make_shared<SampleBuffer>(std::move(buf));
    names_[idx] = name;
    ComputeHit(idx);
    countSlots_ = std::max(countSlots_, idx + 1);
}

void RiserPlayer::AddGenerated(int index, const char* name, SampleBuffer&& buf) {
    samples_[index] = std::make_shared<SampleBuffer>(std::move(buf));
    names_[index] = name;
    ComputeHit(index);
}

// Hit point: loudest 20 ms block of a lightly smoothed RMS envelope (RMS, not peak: a noise sweep's single highest sample says
// nothing about where it is loudest).
void RiserPlayer::ComputeHit(int index) {
    const SampleBuffer* buf = samples_[index].get();
    if (!buf) return;
    const int sampleRate = loadedSampleRate_;
    const int ch = buf->channels, n = buf->frames(), blk = std::max(1, sampleRate / 50);
    std::vector<float> env;
    for (int f = 0; f < n; f += blk) {
        double e = 0.0; int cnt = 0;
        for (int j = f; j < std::min(n, f + blk); ++j)
            for (int c = 0; c < ch; ++c) { const double v = buf->data[static_cast<size_t>(j) * ch + c]; e += v * v; ++cnt; }
        env.push_back(static_cast<float>(std::sqrt(e / std::max(1, cnt))));
    }
    size_t best = 0; float bv = -1.0f;
    for (size_t k = 0; k < env.size(); ++k) {
        float sm = env[k] * 0.5f + (k > 0 ? env[k - 1] : env[k]) * 0.25f + (k + 1 < env.size() ? env[k + 1] : env[k]) * 0.25f;
        if (sm > bv + 1e-6f) { bv = sm; best = k; }
    }
    hitFrame_[index] = std::min<double>(n - 1, static_cast<double>(best) * blk + blk * 0.5);
}

const std::string& RiserPlayer::NameAt(int index) const {
    static const std::string kEmpty;
    if (index < 0 || index >= kMaxSounds) return kEmpty;
    return names_[index];
}

double RiserPlayer::DurationSeconds(int index) const {
    if (index < 0 || index >= kMaxSounds || !samples_[index]) return 0.0;
    if (loadedSampleRate_ <= 0) return 0.0;
    return static_cast<double>(samples_[index]->frames()) / loadedSampleRate_;
}

std::shared_ptr<const SincTable> RiserPlayer::SincFor(double rate) {
    if (std::fabs(rate - 1.0) < 1e-6) return nullptr;
    return SharedSincForRate(rate);   // N1 (LIVE-9): built at startup, nothing is built or freed on the audio thread
}

void RiserPlayer::Trigger(int index, double rate) {
    if (index < 0 || index >= kMaxSounds || !samples_[index]) return;
    // Steal the next voice round-robin -- with kMaxVoices==2 this only
    // matters if a riser is re-triggered before the previous one finished,
    // which just crossfades into silence on the stolen voice rather than
    // clicking (the old voice's samples simply stop being read).
    RiserVoice& v = voices_[static_cast<size_t>(nextVoice_)];
    nextVoice_ = (nextVoice_ + 1) % kMaxVoices;
    v.buffer = samples_[index].get();
    v.playhead = 0.0;
    v.active = true;
    v.rate = (rate > 0.0) ? rate : 1.0;
    v.delay = 0.0; v.fadeInTotal = 0; v.fadeInDone = 0; v.gain = 1.0f;
    v.sinc = SincFor(v.rate);
}

void RiserPlayer::TriggerAligned(int index, double delayFrames, double startFrame, double rate, float gain) {
    if (index < 0 || index >= kMaxSounds || !samples_[index]) return;
    RiserVoice& v = voices_[static_cast<size_t>(nextVoice_)];
    nextVoice_ = (nextVoice_ + 1) % kMaxVoices;
    v.buffer = samples_[index].get();
    v.playhead = std::max(0.0, startFrame);
    v.rate = (rate > 0.0) ? rate : 1.0;
    v.delay = std::max(0.0, delayFrames);
    v.fadeInTotal = startFrame > 1.0 ? std::max(1, static_cast<int>(0.06 * loadedSampleRate_)) : 0;
    v.fadeInDone = 0;
    v.gain = std::max(0.0f, gain);
    v.sinc = SincFor(v.rate);
    v.active = true;
}

void RiserPlayer::Render(float* out, int numFrames, int sampleRate) {
    for (auto& v : voices_) {
        if (!v.active || !v.buffer) continue;
        const SampleBuffer& buf = *v.buffer;
        int srcFrames = buf.frames();
        int ch = buf.channels;
        if (srcFrames < 2) { v.active = false; continue; }

        // See kReleaseSeconds' comment in Risers.h -- the last
        // `kReleaseSeconds` of REAL (wall-clock) playback time gets
        // linearly ramped to 0 gain, independent of the sample's own
        // content, so playback always tapers out instead of hard-stopping
        // the instant the buffer runs out. Computed per-voice (not once
        // for the whole Render call) because each voice can be playing
        // back at a different `rate` (see ComputeRiserBarSyncRate) -- the
        // release window in SOURCE-sample terms has to scale by rate too,
        // or a sped-up riser would get a shorter real-time tail than a
        // slowed-down one triggered alongside it.
        double releaseSamples = std::max(1.0, kReleaseSeconds * sampleRate * v.rate);
        // The tail starts after the hit, never before: the climax must not be ducked ahead of the beat.
        const double minRelease = std::max(1.0, kMinReleaseSeconds * sampleRate * v.rate);
        const int hitIdx = static_cast<int>(std::distance(samples_.begin(), std::find_if(samples_.begin(), samples_.end(),
                               [&](const std::shared_ptr<SampleBuffer>& p) { return p.get() == v.buffer; })));
        const double hit = (hitIdx >= 0 && hitIdx < kMaxSounds) ? hitFrame_[hitIdx] : 0.0;
        double releaseStart = std::max(hit, srcFrames - releaseSamples);
        releaseStart = std::min(releaseStart, srcFrames - minRelease);
        releaseStart = std::max(0.0, releaseStart);
        releaseSamples = std::max(1.0, srcFrames - releaseStart);

        for (int i = 0; i < numFrames; ++i) {
            if (v.delay > 0.0) { v.delay -= 1.0; continue; }
            int idx = static_cast<int>(v.playhead);
            if (idx >= srcFrames - 1) { v.active = false; break; }
            double frac = v.playhead - idx;

            float l, r;
            if (!v.sinc) {                                           // rate 1: the stored samples, untouched
                l = buf.data[static_cast<size_t>(idx) * ch];
                r = ch == 1 ? l : buf.data[static_cast<size_t>(idx) * ch + 1];
            } else {                                                 // varispeed (bar sync): band-limited interpolation
                l = v.sinc->At(buf.data.data(), srcFrames, ch, 0, idx, frac);
                r = ch == 1 ? l : v.sinc->At(buf.data.data(), srcFrames, ch, 1, idx, frac);
            }

            if (v.playhead > releaseStart) {
                float releaseGain = static_cast<float>(
                    std::max(0.0, 1.0 - (v.playhead - releaseStart) / releaseSamples));
                l *= releaseGain;
                r *= releaseGain;
            }

            if (v.fadeInDone < v.fadeInTotal) {
                float g = static_cast<float>(v.fadeInDone) / static_cast<float>(v.fadeInTotal);
                g = 0.5f - 0.5f * std::cos(3.14159265f * g);
                l *= g; r *= g; ++v.fadeInDone;
            }
            l *= v.gain; r *= v.gain;
            out[static_cast<size_t>(i) * 2 + 0] += l;
            out[static_cast<size_t>(i) * 2 + 1] += r;
            v.playhead += v.rate;
        }
    }
}

} // namespace t8
