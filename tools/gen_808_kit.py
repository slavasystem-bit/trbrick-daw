#!/usr/bin/env python3
"""
Generates a procedurally-synthesized "classic 808" style drum kit as 16-bit
mono WAV files. Nothing here is a sampled/ripped recording of any real
Roland (or other) product -- every sound is built from oscillators, noise
and envelopes that emulate the *circuit behaviour* of a classic analog
808-style drum voice (sine kick with pitch envelope, tone+noise snare,
band-passed noise claps, six-square-wave metallic hi-hats, tuned tom-ish
percussion). This keeps the shipped kit fully original while still landing
in the right sonic territory for a T8-style live kit.

Output: kits/808/{kick,snare,clap,chh,ohh,perc}.wav
Users can drop in their own kit folders (see README) or add extra
variations named e.g. kick_2.wav, kick_3.wav alongside kick.wav -- the
engine picks these up automatically as alternate takes for that pad.
"""
import numpy as np
import wave
import struct
import os
import math

SR = 44100

def write_wav(path, samples, sr=SR):
    samples = np.clip(samples, -1.0, 1.0)
    pcm = (samples * 32767.0).astype(np.int16)
    with wave.open(path, "w") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(pcm.tobytes())

def t_axis(dur):
    n = int(SR * dur)
    return np.arange(n) / SR, n

def exp_env(t, decay):
    return np.exp(-t / decay)

def kick(dur=0.55, start_freq=155.0, end_freq=48.0, pitch_decay=0.09, amp_decay=0.45, click=True):
    t, n = t_axis(dur)
    # Pitch envelope: exponential fall from start_freq to end_freq
    freq = end_freq + (start_freq - end_freq) * np.exp(-t / pitch_decay)
    phase = 2 * np.pi * np.cumsum(freq) / SR
    tone = np.sin(phase)
    amp = exp_env(t, amp_decay)
    sig = tone * amp
    if click:
        click_n = int(SR * 0.004)
        click_sig = (np.random.uniform(-1, 1, click_n) * np.exp(-np.arange(click_n) / (SR * 0.001)))
        sig[:click_n] += click_sig * 0.5
    sig = np.tanh(sig * 1.6) * 0.98
    return sig

def snare(dur=0.35):
    t, n = t_axis(dur)
    # Tonal component: two sines (like 808's dual oscillator body) with fast decay
    tone = (np.sin(2 * np.pi * 180 * t) * 0.6 + np.sin(2 * np.pi * 330 * t) * 0.4)
    tone *= exp_env(t, 0.045)
    # Noise component: high-passed white noise with its own envelope
    noise = np.random.uniform(-1, 1, n)
    # simple 1-pole high-pass
    hp = np.zeros(n)
    a = 0.92
    prev_x, prev_y = 0.0, 0.0
    for i in range(n):
        y = a * (prev_y + noise[i] - prev_x)
        hp[i] = y
        prev_x, prev_y = noise[i], y
    hp *= exp_env(t, 0.11)
    sig = tone * 0.55 + hp * 0.75
    sig = np.tanh(sig * 1.3) * 0.95
    return sig

def _band_noise(n, low_hz, high_hz, sr=SR):
    """Crude FFT band-pass of white noise (fine for one-shot percussive use)."""
    noise = np.random.uniform(-1, 1, n)
    spec = np.fft.rfft(noise)
    freqs = np.fft.rfftfreq(n, 1.0 / sr)
    mask = (freqs >= low_hz) & (freqs <= high_hz)
    spec = spec * mask
    out = np.fft.irfft(spec, n)
    peak = np.max(np.abs(out)) or 1.0
    return out / peak

def clap(dur=0.4):
    t, n = t_axis(dur)
    body = _band_noise(n, 800, 5500)
    # three quick repeats then a longer tail, like the 808's multi-trigger clap
    env = np.zeros(n)
    hits = [0.0, 0.012, 0.024]
    for h in hits:
        idx = int(h * SR)
        seg = exp_env(t, 0.02)
        shifted = np.zeros(n)
        if idx < n:
            shifted[idx:] = seg[: n - idx]
        env += shifted
    tail_start = int(0.03 * SR)
    tail = np.zeros(n)
    tail[tail_start:] = exp_env(t[: n - tail_start], 0.09)
    env = np.clip(env * 0.9 + tail * 0.6, 0, None)
    sig = body * env
    sig = np.tanh(sig * 1.4) * 0.95
    return sig

def _metallic_noise(n, sr=SR):
    """Six detuned square waves ring-mixed, like the real 808 hat circuit,
    then high-passed to leave only the metallic upper harmonics."""
    base_freqs = [205.3, 304.4, 369.6, 522.7, 540.0, 800.0]  # 808-inspired ratios
    t = np.arange(n) / sr
    mix = np.zeros(n)
    for f in base_freqs:
        mix += np.sign(np.sin(2 * np.pi * f * t))
    mix /= len(base_freqs)
    # high-pass to strip the fundamental buzz, leaving hiss-like metal
    spec = np.fft.rfft(mix)
    freqs = np.fft.rfftfreq(n, 1.0 / sr)
    spec[freqs < 3500] = 0
    out = np.fft.irfft(spec, n)
    peak = np.max(np.abs(out)) or 1.0
    return out / peak

def closed_hat(dur=0.09):
    t, n = t_axis(dur)
    sig = _metallic_noise(n) * exp_env(t, 0.018)
    return np.tanh(sig * 1.2) * 0.9

def open_hat(dur=0.5):
    t, n = t_axis(dur)
    sig = _metallic_noise(n) * exp_env(t, 0.28)
    return np.tanh(sig * 1.2) * 0.9

def perc(dur=0.3, start_freq=330.0, end_freq=220.0):
    """A tuned tom/conga-ish percussion hit -- the kit's flexible 'perc' slot."""
    t, n = t_axis(dur)
    freq = end_freq + (start_freq - end_freq) * np.exp(-t / 0.05)
    phase = 2 * np.pi * np.cumsum(freq) / SR
    tone = np.sin(phase)
    amp = exp_env(t, 0.16)
    sig = tone * amp
    return np.tanh(sig * 1.3) * 0.95

def main():
    out_dir = os.path.join(os.path.dirname(__file__), "..", "kits", "808")
    os.makedirs(out_dir, exist_ok=True)
    np.random.seed(8088)

    write_wav(os.path.join(out_dir, "kick.wav"), kick())
    write_wav(os.path.join(out_dir, "snare.wav"), snare())
    write_wav(os.path.join(out_dir, "clap.wav"), clap())
    write_wav(os.path.join(out_dir, "chh.wav"), closed_hat())
    write_wav(os.path.join(out_dir, "ohh.wav"), open_hat())
    write_wav(os.path.join(out_dir, "perc.wav"), perc())

    # A couple of alternate takes to demonstrate the "add more samples" feature:
    # any file named "<slot>_<n>.wav" in a kit folder is picked up as an
    # extra variant selectable per-step in the sequencer.
    write_wav(os.path.join(out_dir, "kick_2.wav"), kick(start_freq=190, end_freq=55, pitch_decay=0.06, amp_decay=0.3))
    write_wav(os.path.join(out_dir, "perc_2.wav"), perc(start_freq=440, end_freq=300))
    write_wav(os.path.join(out_dir, "snare_2.wav"), snare(dur=0.25))

    print("Wrote 808-style kit to", os.path.abspath(out_dir))
    for f in sorted(os.listdir(out_dir)):
        print(" -", f)

if __name__ == "__main__":
    main()
