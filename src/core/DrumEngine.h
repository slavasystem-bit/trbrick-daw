#pragma once
// Sample-based drum engine: 6 fixed slots (Kick/Snare/Clap/ClosedHH/OpenHH/Perc),
// each backed by a "kit" folder of WAV files. Each slot supports multiple
// alternate-take variants (kick.wav, kick_2.wav, kick_3.wav, ...) selectable
// per step, and users can drop their own kit folders under kits/<name>/ with
// the same 6 filenames to completely replace the sound set.
#include "Types.h"
#include "WavFile.h"
#include "Resample.h"
#include "KickSynth.h"
#include <array>
#include <string>
#include <vector>
#include <memory>

namespace t8 {

// One playing instance of a drum sample.
struct DrumVoice {
    std::shared_ptr<const SampleBuffer> buffer;   // keeps the sample alive even if the kit is swapped while this voice plays
    double playhead = 0.0;     // in source frames
    double rate = 1.0;         // playback rate (pitch/tune)
    float gain = 0.0f;
    float pan = 0.0f;
    bool active = false;
    double endFrame = 0.0;     // v44.52: where the played part ends (forward) ...
    double startFrame = 0.0;   // ... and where it starts (reverse plays down to it); 0 / 0 = the whole sample
    bool reverse = false;
    int slot = -1;             // which DrumSlot this voice belongs to (for choke logic)
    double ageSeconds = 0.0;   // time since trigger, drives the decay-shaping envelope
    // Decay shaping (v34): `env` is a running gain multiplied by `envMul` every sample. 1.0 / 1.0 = the sample exactly as
    // recorded (decay knob at 1.0); below 1.0 the tail is shortened, above 1.0 it is lifted slowly towards the end.
    float env = 1.0f;
    float envMul = 1.0f;
    double envRate = 0.0;      // 1/s: > 0 decays, < 0 lifts, 0 = untouched
    float envFloor = 0.001f;   // a voice whose shaped gain falls under this is finished
    std::shared_ptr<const SincTable> sinc;   // interpolation kernel for pitched playback (null = rate 1, read samples directly)
    // Velocity tone: soft hits are darker (one-pole low-pass), a full-velocity hit is left untouched.
    float toneFc = 0.0f;       // Hz, 0 = bypass
    float toneCoef = 1.0f;
    float toneL = 0.0f, toneR = 0.0f;
    bool synth = false;        // built-in kick instead of a sample
    KickSynthState kick;
    // Anti-click: every sample starts abruptly (most one-shots begin at a non-zero
    // value), so a voice fades in over fadeInSec; a voice that is cut (hat choke,
    // stolen voice) fades out over kChokeSec instead of being switched off, and
    // the last few source frames fade to zero so a sample that ends on a
    // non-zero value does not click either.
    double fadeInSec = 0.0006;
    bool choking = false;
    double chokeLeft = 0.0;    // seconds of the choke fade still to run
};

// The kit is an immutable snapshot ("bank") that is swapped atomically. LoadFromFolder builds a complete new
// bank off to the side and publishes it with one atomic store, so the audio thread never sees a half-loaded
// kit, and voices that are already playing keep their own reference to the old samples. Replaced banks are kept
// alive for a while ("retired") so the last reference is never dropped inside the audio callback.
class DrumKit {
public:
    struct Bank {
        std::array<std::vector<std::shared_ptr<const SampleBuffer>>, kNumDrumTracks> variants;
        std::array<std::vector<std::string>, kNumDrumTracks> names;   // C4b: the file of each variant (kick.wav, kick_2.wav ...), same order
    };
    using BankPtr = std::shared_ptr<const Bank>;
    // v44.5.4: a kit can belong to a scene. Build() reads a folder into a bank WITHOUT making it the sounding one (UI thread; reads
    // the disk). Activate() makes a bank the sounding one with one atomic store - cheap enough for the audio thread, but the caller
    // must keep the bank (and the one it replaces) referenced elsewhere so the last reference is never dropped there.
    BankPtr Build(const std::string& folder, int sampleRate, int* loadedRequired = nullptr) const;
    BankPtr CurrentBank() const { return std::atomic_load(&bank_); }
    void Activate(const BankPtr& b) { if (b) std::atomic_store(&bank_, b); }

    // Loads kit from a folder containing kick.wav, snare.wav, clap.wav,
    // chh.wav, ohh.wav, perc.wav, plus optional *_2.wav, *_3.wav variants.
    // Returns the number of the 6 required slots successfully loaded. UI thread only.
    int LoadFromFolder(const std::string& folder, int sampleRate);

    // Audio-thread safe (atomic snapshot read). The returned pointer keeps the sample alive.
    std::shared_ptr<const SampleBuffer> Variant(DrumSlot slot, int variantIndex) const;
    int VariantCount(DrumSlot slot) const;
    const std::string& FolderPath() const { return folder_; }

private:
    std::shared_ptr<const Bank> Current() const { return std::atomic_load(&bank_); }

    std::shared_ptr<const Bank> bank_ = std::make_shared<Bank>();
    std::vector<std::shared_ptr<const Bank>> retired_;   // UI thread only
    std::string folder_;
};

class DrumEngine {
public:
    static constexpr int kMaxVoices = 64;
    static constexpr double kChokeSec = 0.004;

    void SetKit(DrumKit* kit) { kit_ = kit; }

    // velocity: 0..1 (accented steps should pass ~1.0, normal ~0.75)
    // `kick` (optional): the project's built-in kick settings; used when the slot is Kick and its mode is SYNTH / LAYER.
    void Trigger(DrumSlot slot, float velocity, int variant, const DrumVoiceParams& params, const KickSynthSettings* kick = nullptr);

    // Renders only the voices belonging to `slot`, ADDING into `out`
    // (interleaved stereo). Called once per drum slot per audio block so the
    // mixer can apply independent volume/pan/mute/solo per track.
    void RenderSlot(DrumSlot slot, float* out, int numFrames, int sampleRate);

private:
    DrumKit* kit_ = nullptr;
    std::array<DrumVoice, kMaxVoices> voices_{};
    int nextVoice_ = 0;
    uint32_t noiseSeed_ = 1234567u;

    // interpolation kernels by playback rate (small cache; built on first use of a tune value)
    struct CachedSinc { double rate = 0.0; std::shared_ptr<const SincTable> table; };
    std::array<CachedSinc, 16> sincCache_{};
    int sincNext_ = 0;
    std::shared_ptr<const SincTable> SincFor(double rate);
    DrumVoice* AllocVoice();
};

} // namespace t8
