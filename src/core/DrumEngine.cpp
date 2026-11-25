#include "DrumEngine.h"
#include "Stereo.h"
#include <cmath>
#include <iostream>
#include <sys/stat.h>

namespace t8 {

namespace {
bool FileExists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}
}

int DrumKit::LoadFromFolder(const std::string& folder, int sampleRate) {
    int loadedRequired = 0;
    BankPtr bank = Build(folder, sampleRate, &loadedRequired);
    folder_ = folder;
    std::shared_ptr<const Bank> old = Current();
    std::atomic_store(&bank_, bank);
    retired_.push_back(std::move(old));
    // Voices only play a sample for a second or so; keeping the last few banks alive guarantees the audio
    // thread never drops the last reference to one.
    while (retired_.size() > 4) retired_.erase(retired_.begin());
    return loadedRequired;
}

DrumKit::BankPtr DrumKit::Build(const std::string& folder, int sampleRate, int* loadedOut) const {
    auto bank = std::make_shared<Bank>();
    int loadedRequired = 0;
    for (int i = 0; i < kNumDrumTracks; ++i) {
        auto slot = static_cast<DrumSlot>(i);
        std::string stem = DrumSlotFileStem(slot);

        // Primary file: <stem>.wav
        std::string primary = folder + "/" + stem + ".wav";
        auto buf = std::make_shared<SampleBuffer>();
        if (LoadWav(primary, *buf, sampleRate)) {
            bank->variants[i].push_back(buf);
            bank->names[i].push_back(stem + ".wav");
            ++loadedRequired;
        } else {
            std::cerr << "[kit] missing required sample: " << primary << "\n";
        }

        // Extra variants: <stem>_2.wav, <stem>_3.wav, ... up to kMaxKitVariants
        for (int v = 2; v <= kMaxKitVariants; ++v) {
            std::string alt = folder + "/" + stem + "_" + std::to_string(v) + ".wav";
            if (!FileExists(alt)) continue;
            auto vb = std::make_shared<SampleBuffer>();
            if (LoadWav(alt, *vb, sampleRate)) { bank->variants[i].push_back(vb); bank->names[i].push_back(stem + "_" + std::to_string(v) + ".wav"); }
        }
    }
    if (loadedOut) *loadedOut = loadedRequired;
    return bank;
}

std::shared_ptr<const SampleBuffer> DrumKit::Variant(DrumSlot slot, int variantIndex) const {
    auto bank = Current();
    const auto& v = bank->variants[static_cast<int>(slot)];
    if (v.empty()) return nullptr;
    if (variantIndex < 0 || variantIndex >= static_cast<int>(v.size())) variantIndex = 0;
    return v[variantIndex];
}

int DrumKit::VariantCount(DrumSlot slot) const {
    return static_cast<int>(Current()->variants[static_cast<int>(slot)].size());
}

std::shared_ptr<const SincTable> DrumEngine::SincFor(double rate) {
    for (auto& c : sincCache_) if (c.table && std::fabs(c.rate - rate) < 1e-9) return c.table;
    // pitching up: the passband must stay below the new Nyquist, otherwise the top end folds back (aliasing)
    auto table = std::make_shared<const SincTable>(16, 128, std::min(1.0, 0.95 / rate));
    sincCache_[sincNext_] = CachedSinc{rate, table};
    sincNext_ = (sincNext_ + 1) % static_cast<int>(sincCache_.size());
    return table;
}

DrumVoice* DrumEngine::AllocVoice() {
    // Prefer a free voice; otherwise steal the one that has been playing the longest (it is the quietest).
    DrumVoice* target = nullptr;
    for (int k = 0; k < kMaxVoices && !target; ++k) {
        int vi = (nextVoice_ + k) % kMaxVoices;
        if (!voices_[vi].active) target = &voices_[vi];
    }
    if (!target) {
        target = &voices_[0];
        for (auto& v : voices_) if (v.ageSeconds > target->ageSeconds) target = &v;
    }
    nextVoice_ = static_cast<int>(target - voices_.data() + 1) % kMaxVoices;
    return target;
}

void DrumEngine::Trigger(DrumSlot slot, float velocity, int variant, const DrumVoiceParams& params, const KickSynthSettings* kick) {
    if (!kit_) return;
    const int kickMode = (slot == DrumSlot::Kick && kick) ? std::clamp(kick->mode, 0, 2) : 0;
    std::shared_ptr<const SampleBuffer> buf = kickMode == 1 ? nullptr : kit_->Variant(slot, variant);
    const bool haveSample = buf && !buf->empty();
    if (!haveSample && kickMode == 0) return;

    // Closed-hat choke: triggering the closed hat cuts any playing open hat,
    // exactly like a real 808/909/TR-8 sourced from one analog VCA per hat pair.
    if (slot == DrumSlot::ClosedHat) {
        for (auto& v : voices_) {
            if (v.active && !v.choking && v.slot == static_cast<int>(DrumSlot::OpenHat)) { v.choking = true; v.chokeLeft = kChokeSec; }
        }
    }

    const float vel = std::clamp(velocity, 0.0f, 1.5f);
    const float gain = std::max(0.0f, std::min(1.5f, vel * params.level));

    auto reset = [&](DrumVoice* v) {
        v->playhead = 0.0; v->rate = 1.0; v->startFrame = 0.0; v->endFrame = 0.0; v->reverse = false; v->pan = 0.0f; v->active = true; v->slot = static_cast<int>(slot);
        v->ageSeconds = 0.0; v->choking = false; v->chokeLeft = 0.0;
        v->env = 1.0f; v->envMul = 1.0f; v->envRate = 0.0; v->envFloor = 0.0f;
        v->sinc = nullptr; v->toneFc = 0.0f; v->toneCoef = 1.0f; v->toneL = v->toneR = 0.0f; v->synth = false; v->kick.active = false;
    };

    if (haveSample) {
        DrumVoice* target = AllocVoice();
        reset(target);
        target->buffer = std::move(buf);
        const SampleEdit& ed = params.edit;   // v44.52: START / LENGTH / ATTACK / FINE / GAIN / REVERSE; the defaults change nothing
        target->rate = std::pow(2.0, params.tune / 12.0 + ed.fineCents / 1200.0);
        if (std::fabs(target->rate - 1.0) > 1e-4) target->sinc = SincFor(target->rate);
        else target->rate = 1.0;
        const int srcN = target->buffer->frames();
        target->startFrame = std::clamp(static_cast<double>(ed.start), 0.0, 0.95) * srcN;
        target->endFrame = srcN - 1.0;
        if (ed.length < 0.9999f) target->endFrame = std::min(srcN - 1.0, target->startFrame + std::max(8.0, (srcN - target->startFrame) * std::clamp(static_cast<double>(ed.length), 0.02, 1.0)));
        target->reverse = ed.reverse;
        target->playhead = ed.reverse ? std::max(target->startFrame, target->endFrame - 1.0) : target->startFrame;
        if (ed.reverse) target->rate = -target->rate;
        target->gain = gain * (ed.gainDb == 0 ? 1.0f : std::pow(10.0f, static_cast<float>(ed.gainDb) / 20.0f));
        target->fadeInSec = ed.attackMs > 0.05f ? ed.attackMs * 0.001 : (slot == DrumSlot::Kick ? 0.0015 : 0.0006);   // a kick has sub content: a slightly longer ramp
        // DECAY knob: 1.0 plays the sample exactly as recorded. Below 1 an exponential envelope shortens the tail
        // (tau = 0.35 s * d / (1 - d): 0.5 -> 0.35 s, 0.25 -> 0.12 s, 0.9 -> 3 s). Above 1 the tail is lifted by up to
        // +6 dB towards the end of the sample.
        const double d = std::clamp(static_cast<double>(params.decay), 0.05, 2.0);
        if (d < 0.999) { target->envRate = (1.0 - d) / (0.35 * d); target->envFloor = 0.001f; }
        else if (d > 1.001) {
            const double dur = std::max(0.02, target->buffer->frames() / (std::fabs(target->rate) * std::max(1, target->buffer->sampleRate)));
            target->envRate = -(d - 1.0) * 0.69314718 / dur;
        }
        // velocity tone: a hit softer than ~0.55 gets a low-pass that closes with the velocity (1.2 kHz * 10^(2.2 v))
        const float fc = 1200.0f * std::pow(10.0f, 2.2f * std::clamp(vel, 0.0f, 1.0f));
        target->toneFc = fc < 18000.0f ? fc : 0.0f;
    }
    if (kickMode >= 1) {
        DrumVoice* target = AllocVoice();
        reset(target);
        target->synth = true;
        target->gain = std::max(0.0f, std::min(1.5f, vel * kick->level * (kickMode == 2 ? 0.8f : 1.0f)));
        target->fadeInSec = 0.0;
        noiseSeed_ = noiseSeed_ * 1664525u + 1013904223u;
        target->kick.Start(*kick, params.keyRoot, params.tune, noiseSeed_);
    }
}

void DrumEngine::RenderSlot(DrumSlot slot, float* out, int numFrames, int sampleRate) {
    double dt = 1.0 / sampleRate;
    int slotInt = static_cast<int>(slot);
    for (auto& v : voices_) {
        if (!v.active || v.slot != slotInt) continue;
        float pl, pr;
        PanGains(v.pan, pl, pr);

        if (v.synth) {
            for (int i = 0; i < numFrames; ++i) {
                float g = 1.0f;
                if (v.choking) { g *= static_cast<float>(std::max(0.0, v.chokeLeft / kChokeSec)); v.chokeLeft -= dt; }
                const float s = v.kick.Next(dt) * v.gain * g;
                out[static_cast<size_t>(i) * 2 + 0] += s * pl;
                out[static_cast<size_t>(i) * 2 + 1] += s * pr;
                v.ageSeconds += dt;
                if (!v.kick.active || (v.choking && v.chokeLeft <= 0.0)) { v.active = false; break; }
            }
            continue;
        }

        if (!v.buffer) continue;
        const SampleBuffer& buf = *v.buffer;
        const int srcFrames = buf.frames();
        const int ch = buf.channels;
        const float* data = buf.data.data();
        const float envMulBlock = v.envRate != 0.0 ? static_cast<float>(std::exp(-v.envRate * dt)) : 1.0f;
        const float toneA = v.toneFc > 0.0f ? 1.0f - std::exp(-6.28318530718f * v.toneFc * static_cast<float>(dt)) : 1.0f;

        for (int i = 0; i < numFrames; ++i) {
            const int idx = static_cast<int>(v.playhead);
            if (v.reverse ? (v.playhead <= v.startFrame || idx < 0) : (idx >= srcFrames - 1 || v.playhead >= v.endFrame)) { v.active = false; break; }
            const double frac = v.playhead - idx;

            float l, r;
            if (!v.sinc) {                                           // rate 1: the stored samples, untouched
                l = data[static_cast<size_t>(idx) * ch];
                r = ch == 1 ? l : data[static_cast<size_t>(idx) * ch + 1];
            } else {
                l = v.sinc->At(data, srcFrames, ch, 0, idx, frac);
                r = ch == 1 ? l : v.sinc->At(data, srcFrames, ch, 1, idx, frac);
            }
            if (toneA < 1.0f) { v.toneL += toneA * (l - v.toneL); v.toneR += toneA * (r - v.toneR); l = v.toneL; r = v.toneR; }

            float g = v.env;
            if (v.ageSeconds < v.fadeInSec) {                                    // raised-cosine fade-in
                float x = static_cast<float>(v.ageSeconds / v.fadeInSec);
                g *= 0.5f - 0.5f * std::cos(3.14159265f * x);
            }
            double framesLeft = v.reverse ? v.playhead - v.startFrame : v.endFrame - v.playhead;   // fade out the last ~1.5 ms of the played part (4 ms when LENGTH cut it)
            const double fadeFrames = v.endFrame < srcFrames - 1.5 ? std::max(64.0, 0.004 * sampleRate) : 64.0;
            if (framesLeft < fadeFrames) g *= static_cast<float>(std::max(0.0, framesLeft) / fadeFrames);
            if (v.choking) {
                g *= static_cast<float>(std::max(0.0, v.chokeLeft / kChokeSec));
                v.chokeLeft -= dt;
            }
            out[static_cast<size_t>(i) * 2 + 0] += l * v.gain * g * pl;
            out[static_cast<size_t>(i) * 2 + 1] += r * v.gain * g * pr;

            v.playhead += v.rate;
            v.ageSeconds += dt;
            v.env *= envMulBlock;
            if (v.env > 2.0f) v.env = 2.0f;
            if ((v.envFloor > 0.0f && v.env < v.envFloor) || (v.choking && v.chokeLeft <= 0.0)) { v.active = false; break; }
        }
    }
}

} // namespace t8
