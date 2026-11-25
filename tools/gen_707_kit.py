#!/usr/bin/env python3
"""
Generates a procedurally-synthesized "classic 707" style drum kit as 16-bit
mono WAV files. Same disclaimer as gen_808_kit.py/gen_909_kit.py: everything
here is synthesized from oscillators/noise/envelopes, not a sampled/ripped
recording of any real product -- shaped to emulate the *character* of a
cleaner, more "digital-sounding" late-80s drum machine voice: a short, tight
thump of a kick with very little pitch sweep, a crisp higher-pitched snare
with a fast, clean decay, cleaner (less metallic-buzzy) hi-hats, and a
cowbell-ish tone for the perc slot. Meant to sit clearly apart from both the
808 (boomy/analog) and 909 (punchy/aggressive) kits -- tighter and cleaner
across the board.

Output: kits/707/{kick,snare,clap,chh,ohh,perc}.wav
Same alt-take convention as the other kits.
"""
import numpy as np
import wave
import os

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

def _band_noise(n, low_hz, high_hz, sr=SR):
    noise = np.random.uniform(-1, 1, n)
    spec = np.fft.rfft(noise)
    freqs = np.fft.rfftfreq(n, 1.0 / sr)
    mask = (freqs >= low_hz) & (freqs <= high_hz)
    spec = spec * mask
    out = np.fft.irfft(spec, n)
    peak = np.max(np.abs(out)) or 1.0
    return out / peak

def _metallic_noise(n, base_freqs, hp_hz, sr=SR):
    t = np.arange(n) / sr
    mix = np.zeros(n)
    for f in base_freqs:
        mix += np.sin(2 * np.pi * f * t)  # sines, not squares -- cleaner/less buzzy than 808/909
    mix /= len(base_freqs)
    spec = np.fft.rfft(mix)
    freqs = np.fft.rfftfreq(n, 1.0 / sr)
    spec[freqs < hp_hz] = 0
    out = np.fft.irfft(spec, n)
    peak = np.max(np.abs(out)) or 1.0
    return out / peak

def kick(dur=0.3, start_freq=145.0, end_freq=62.0, pitch_decay=0.022, amp_decay=0.15):
    """707-style kick: short, tight, very little pitch sweep -- a clean
    "thump" rather than a boomy sub-drop or a distorted click-kick."""
    t, n = t_axis(dur)
    freq = end_freq + (start_freq - end_freq) * np.exp(-t / pitch_decay)
    phase = 2 * np.pi * np.cumsum(freq) / SR
    tone = np.sin(phase)
    amp = exp_env(t, amp_decay)
    sig = tone * amp
    click_n = int(SR * 0.002)
    click_sig = np.random.uniform(-1, 1, click_n) * np.exp(-np.arange(click_n) / (SR * 0.0008))
    sig[:click_n] += click_sig * 0.5
    # Driven harder into saturation than the other kits' pre-tanh gain
    # would suggest -- not for extra "dirt" (the short envelope above is
    # what keeps this one sounding clean/tight), just to land the peak
    # level in the same ballpark as the other three kits so the mixer's
    # normal volume/level staging doesn't leave this one sitting quieter.
    sig = np.tanh(sig * 2.2) * 0.97
    return sig

def snare(dur=0.2):
    """707-style snare: higher-pitched, cleaner, faster decay -- more
    "digital crack" than the 808/909's fuller-bodied hits."""
    t, n = t_axis(dur)
    tone = (np.sin(2 * np.pi * 270 * t) * 0.4 + np.sin(2 * np.pi * 460 * t) * 0.4)
    tone *= exp_env(t, 0.02)
    noise = _band_noise(n, 1500, 10000)
    noise *= exp_env(t, 0.06)
    sig = tone * 0.35 + noise * 0.9
    return np.tanh(sig * 1.3) * 0.92

def clap(dur=0.3):
    t, n = t_axis(dur)
    body = _band_noise(n, 1200, 8000)
    env = np.zeros(n)
    for h in (0.0, 0.01, 0.02):
        idx = int(h * SR)
        seg = exp_env(t, 0.018)
        shifted = np.zeros(n)
        if idx < n:
            shifted[idx:] = seg[: n - idx]
        env += shifted
    tail_start = int(0.028 * SR)
    tail = np.zeros(n)
    tail[tail_start:] = exp_env(t[: n - tail_start], 0.06)
    env = np.clip(env * 0.8 + tail * 0.45, 0, None)
    sig = body * env
    return np.tanh(sig * 1.3) * 0.92

def closed_hat(dur=0.06):
    t, n = t_axis(dur)
    sig = _metallic_noise(n, [400.0, 620.0, 810.0, 1200.0, 1600.0, 2100.0], 5500) * exp_env(t, 0.011)
    return np.tanh(sig * 1.1) * 0.85

def open_hat(dur=0.35):
    t, n = t_axis(dur)
    sig = _metallic_noise(n, [400.0, 620.0, 810.0, 1200.0, 1600.0, 2100.0], 5500) * exp_env(t, 0.18)
    return np.tanh(sig * 1.1) * 0.85

def perc(dur=0.35):
    """707-style cowbell: two closely-tuned square-ish tones beating
    against each other, the classic cowbell circuit trick."""
    t, n = t_axis(dur)
    f1, f2 = 587.0, 845.0
    tone = np.sign(np.sin(2 * np.pi * f1 * t)) * 0.5 + np.sign(np.sin(2 * np.pi * f2 * t)) * 0.5
    amp = exp_env(t, 0.09)
    sig = tone * amp
    return np.tanh(sig * 1.4) * 0.9

def main():
    out_dir = os.path.join(os.path.dirname(__file__), "..", "kits", "707")
    os.makedirs(out_dir, exist_ok=True)
    np.random.seed(7077)

    write_wav(os.path.join(out_dir, "kick.wav"), kick())
    write_wav(os.path.join(out_dir, "snare.wav"), snare())
    write_wav(os.path.join(out_dir, "clap.wav"), clap())
    write_wav(os.path.join(out_dir, "chh.wav"), closed_hat())
    write_wav(os.path.join(out_dir, "ohh.wav"), open_hat())
    write_wav(os.path.join(out_dir, "perc.wav"), perc())

    write_wav(os.path.join(out_dir, "kick_2.wav"), kick(start_freq=170, end_freq=68, pitch_decay=0.018, amp_decay=0.11))
    write_wav(os.path.join(out_dir, "perc_2.wav"), perc(dur=0.2))

    print("Wrote 707-style kit to", os.path.abspath(out_dir))
    for f in sorted(os.listdir(out_dir)):
        print(" -", f)

if __name__ == "__main__":
    main()
