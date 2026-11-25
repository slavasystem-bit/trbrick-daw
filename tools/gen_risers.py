#!/usr/bin/env python3
"""
Generates 10 procedurally-synthesized "riser" one-shots -- the build-up/
tension sweep a live performer drops in before a drop or section change
(live-performance round: see AppState::TriggerRiser, ApcActions.h's
RiserRandom/RiserNext, and src/core/Risers.h/.cpp for how these get
played back). Nothing here is a sampled/ripped recording of anything --
every sound is synthesized from oscillators, noise and envelopes, same
philosophy as every gen_*_kit.py script in this folder. Uses
tools/kit_synth_common.py for the shared building blocks (band_noise,
metallic_noise, exp_env, bitcrush) plus a handful of small riser-specific
helpers below (rising pitch/filter sweeps, none of which existed in
kit_synth_common since no other kit needed a multi-second build-up shape).

Output: kits/risers/riser0.wav .. riser9.wav (mono, 44.1kHz, 16-bit PCM --
same convention as every drum kit) -- see src/core/Risers.cpp's
kRiserNames for the name that maps to each index; the order here MUST
match that array exactly.
"""
import os
import sys
import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from kit_synth_common import write_wav, t_axis, exp_env, band_noise, metallic_noise, bitcrush, SR


def rising_env(t, dur, shape=2.0):
    """0..1 amplitude ramp, slow start then accelerating toward the end --
    `shape` > 1 concentrates more of the rise in the back half, which
    reads as "building tension" rather than a linear fade-in."""
    x = np.clip(t / dur, 0.0, 1.0)
    return x ** shape


def swell_env(t, dur, peak_frac=0.85):
    """Rises smoothly to full volume around peak_frac through the sound,
    then decays a little -- the "reversed cymbal" shape."""
    x = np.clip(t / dur, 0.0, 1.0)
    up = np.clip(x / peak_frac, 0.0, 1.0) ** 1.5
    down = np.where(x > peak_frac, 1.0 - (x - peak_frac) / max(1e-6, 1 - peak_frac), 1.0)
    return up * np.clip(down, 0.0, 1.0)


def pitch_sweep_tone(dur, start_freq, end_freq, rise_tau, shape="sine", sr=SR):
    """Phase-accumulated oscillator sweeping from start_freq to end_freq --
    same cumsum-of-instantaneous-frequency trick kit_synth_common.kick()
    uses for its pitch envelope, just rising instead of falling."""
    t, n = t_axis(dur, sr)
    freq = start_freq + (end_freq - start_freq) * (1.0 - np.exp(-t / rise_tau))
    phase = 2 * np.pi * np.cumsum(freq) / sr
    if shape == "saw":
        ph = (phase / (2 * np.pi)) % 1.0
        return 2.0 * ph - 1.0
    return np.sin(phase)


def one_pole_hpf_sweep(sig, start_hz, end_hz, sr=SR):
    """Applies a high-pass filter whose cutoff rises from start_hz to
    end_hz across the signal -- brightens noise over time, the core of a
    classic "noise riser". Simple per-sample one-pole (a = 1 -
    exp(-2*pi*fc/fs)), same design as the C++ engine's MasterFilter."""
    n = len(sig)
    t = np.arange(n) / sr
    dur = n / sr
    fc = start_hz + (end_hz - start_hz) * (t / max(dur, 1e-6))
    a = 1.0 - np.exp(-2 * np.pi * fc / sr)
    lp = np.zeros(n)
    prev = 0.0
    for i in range(n):
        prev = prev + a[i] * (sig[i] - prev)
        lp[i] = prev
    return sig - lp


def main():
    out_dir = os.path.join(os.path.dirname(__file__), "..", "kits", "risers")
    os.makedirs(out_dir, exist_ok=True)
    np.random.seed(7171)
    dur = 3.0
    t, n = t_axis(dur)

    # 0. WHITE NOISE SWEEP -- raw noise, high-pass sweeping up (brightens
    # over time) under a rising amplitude ramp.
    sig = one_pole_hpf_sweep(np.random.uniform(-1, 1, n), 20, 9000) * rising_env(t, dur, 2.2)
    write_wav(os.path.join(out_dir, "riser0.wav"), np.tanh(sig * 1.3) * 0.95)

    # 1. PITCH RISE -- a saw tone sweeping low to high.
    sig = pitch_sweep_tone(dur, 70, 1800, rise_tau=1.1, shape="saw") * rising_env(t, dur, 1.6)
    write_wav(os.path.join(out_dir, "riser1.wav"), np.tanh(sig * 1.2) * 0.9)

    # 2. REVERSE SWELL -- band-passed noise with a "reversed cymbal" swell
    # envelope (peaks late, short decay tail).
    sig = band_noise(n, 400, 12000) * swell_env(t, dur, peak_frac=0.88)
    write_wav(os.path.join(out_dir, "riser2.wav"), np.tanh(sig * 1.1) * 0.9)

    # 3. TENSION DRONE -- two slightly-detuned rising tones plus a slow
    # tremolo that speeds up, for a classic "tension building" feel.
    tone_a = pitch_sweep_tone(dur, 110, 440, rise_tau=1.4)
    tone_b = pitch_sweep_tone(dur, 112, 446, rise_tau=1.4)  # slightly detuned for beating
    tremolo_rate = 1.0 + 7.0 * (t / dur)  # speeds up from 1Hz to 8Hz
    tremolo = 0.6 + 0.4 * np.sin(2 * np.pi * np.cumsum(tremolo_rate) / SR)
    sig = (tone_a + tone_b) * 0.5 * tremolo * rising_env(t, dur, 1.4)
    write_wav(os.path.join(out_dir, "riser3.wav"), np.tanh(sig * 1.3) * 0.9)

    # 4. FILTERED BUILD -- band-passed noise whose band narrows and climbs
    # over time (approximated as several concatenated band_noise segments
    # of rising center frequency), amplitude accelerating hard at the end.
    segs = 8
    seg_n = n // segs
    chunks = []
    for i in range(segs):
        lo = 150 + i * 500
        hi = lo + 900
        chunks.append(band_noise(seg_n, lo, hi))
    sig = np.concatenate(chunks)
    sig = sig * rising_env(np.arange(len(sig)) / SR, dur, 2.8)
    write_wav(os.path.join(out_dir, "riser4.wav"), np.tanh(sig * 1.4) * 0.95)

    # 5. SUB RISER -- a heavy low sine rising gently, felt more than heard
    # until it swells at the very end.
    sig = pitch_sweep_tone(dur, 38, 180, rise_tau=1.8) * rising_env(t, dur, 3.0)
    write_wav(os.path.join(out_dir, "riser5.wav"), np.tanh(sig * 1.6) * 0.95)

    # 6. METALLIC SWEEP -- inharmonic ring-modulated tones (same trick the
    # hat kits use) with a rising high-pass, several detuning segments.
    segs = 6
    seg_n = n // segs
    chunks = []
    for i in range(segs):
        base = 900 + i * 350
        chunks.append(metallic_noise(seg_n, [base, base * 1.6, base * 2.3], hp_hz=1500))
    sig = np.concatenate(chunks)
    sig = sig * rising_env(np.arange(len(sig)) / SR, dur, 2.0)
    write_wav(os.path.join(out_dir, "riser6.wav"), np.tanh(sig * 1.2) * 0.9)

    # 7. SIREN SWEEP -- a wobbling rising-pitch tone, classic air-raid-siren
    # riser character.
    base_sweep = 220 + (900 - 220) * (1 - np.exp(-t / 1.2))
    wobble = 1.0 + 0.08 * np.sin(2 * np.pi * 5.0 * t)
    freq = base_sweep * wobble
    phase = 2 * np.pi * np.cumsum(freq) / SR
    sig = np.sin(phase) * rising_env(t, dur, 1.5)
    write_wav(os.path.join(out_dir, "riser7.wav"), np.tanh(sig * 1.2) * 0.9)

    # 8. IMPACT RISER -- builds like riser0's noise sweep for ~90% of the
    # length, then punctuates with a short sharp downward-pitched thump.
    build_dur = dur * 0.85
    tb, nb = t_axis(build_dur)
    build = one_pole_hpf_sweep(np.random.uniform(-1, 1, nb), 30, 7000) * rising_env(tb, build_dur, 2.0)
    hit_dur = dur - build_dur
    th, nh = t_axis(hit_dur)
    hit_freq = 180 * np.exp(-th / 0.05) + 40
    hit_phase = 2 * np.pi * np.cumsum(hit_freq) / SR
    hit = np.sin(hit_phase) * exp_env(th, 0.15)
    sig = np.concatenate([build, hit])[:n]
    write_wav(os.path.join(out_dir, "riser8.wav"), np.tanh(sig * 1.5) * 0.97)

    # 9. GLITCH BUILD -- noise riser with increasingly dense on/off
    # stutter gating and bitcrushing toward the end, for a digital/glitchy
    # build rather than a smooth analog one.
    raw = one_pole_hpf_sweep(np.random.uniform(-1, 1, n), 40, 6000)
    gate_rate = 2 + 30 * (t / dur) ** 2  # gate speeds up from 2Hz to 32Hz
    gate = (np.sin(2 * np.pi * np.cumsum(gate_rate) / SR) > -0.2).astype(np.float64)
    crushed = bitcrush(raw, bits=6, rate_divide=1)
    sig = crushed * gate * rising_env(t, dur, 2.0)
    write_wav(os.path.join(out_dir, "riser9.wav"), np.tanh(sig * 1.3) * 0.95)

    written = sorted(f for f in os.listdir(out_dir) if f.endswith(".wav"))
    print(f"Wrote {len(written)} riser samples to {out_dir}:")
    for f in written:
        print(f"  {f}")


if __name__ == "__main__":
    main()
