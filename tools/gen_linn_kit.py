#!/usr/bin/env python3
"""
Generates a procedurally-synthesized "80s digital drum machine" style kit
(LinnDrum/LM-1 territory) as 16-bit mono WAV files. Same disclaimer as the
other gen_*_kit.py scripts: everything here is synthesized from oscillators/
noise/envelopes, not a sampled/ripped recording of any real product --
shaped to emulate the character of that era's voice: a deep, quick "thwip"
kick with almost no ringing, a very bright and snappy gated-sounding snare,
a dense stacked clap, glassy hi-hats, and a woody sidestick-ish perc hit.
The fourth kit alongside 808 (boomy/analog), 909 (punchy/aggressive) and 707
(clean/tight) -- this one is the brightest and snappiest of the four.

Output: kits/linn/{kick,snare,clap,chh,ohh,perc}.wav
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

def _glassy_noise(n, base_freqs, hp_hz, sr=SR):
    t = np.arange(n) / sr
    mix = np.zeros(n)
    for f in base_freqs:
        mix += np.sin(2 * np.pi * f * t) + 0.3 * np.sin(2 * np.pi * f * 2.01 * t)
    mix /= len(base_freqs)
    spec = np.fft.rfft(mix)
    freqs = np.fft.rfftfreq(n, 1.0 / sr)
    spec[freqs < hp_hz] = 0
    out = np.fft.irfft(spec, n)
    peak = np.max(np.abs(out)) or 1.0
    return out / peak

def kick(dur=0.32, start_freq=175.0, end_freq=58.0, pitch_decay=0.018, amp_decay=0.13):
    """Deep, very fast "thwip" -- almost all of the pitch drop happens in
    the first ~20ms, leaving a short, punchy low-end thud with little
    ringing tail (the defining 80s digital-kick character)."""
    t, n = t_axis(dur)
    freq = end_freq + (start_freq - end_freq) * np.exp(-t / pitch_decay)
    phase = 2 * np.pi * np.cumsum(freq) / SR
    tone = np.sin(phase)
    amp = exp_env(t, amp_decay)
    sig = tone * amp
    click_n = int(SR * 0.0025)
    click_sig = np.random.uniform(-1, 1, click_n) * np.exp(-np.arange(click_n) / (SR * 0.0005))
    sig[:click_n] += click_sig * 0.55
    sig = np.tanh(sig * 1.8) * 0.97
    return sig

def snare(dur=0.22):
    """Bright, snappy, almost "gated" -- fast attack, quick tight decay,
    lots of high-frequency noise energy relative to the low tonal body."""
    t, n = t_axis(dur)
    tone = (np.sin(2 * np.pi * 200 * t) * 0.35 + np.sin(2 * np.pi * 400 * t) * 0.35)
    tone *= exp_env(t, 0.025)
    noise = _band_noise(n, 1200, 11000)
    noise *= exp_env(t, 0.05)
    sig = tone * 0.3 + noise * 1.0
    sig = np.tanh(sig * 1.6) * 0.95
    return sig

def clap(dur=0.32):
    """Dense stacked clap -- five very close, very short bursts (denser
    than 808/909/707) for that thick 80s clap-snare-layer character."""
    t, n = t_axis(dur)
    body = _band_noise(n, 900, 8500)
    env = np.zeros(n)
    for h in (0.0, 0.006, 0.012, 0.018, 0.026):
        idx = int(h * SR)
        seg = exp_env(t, 0.014)
        shifted = np.zeros(n)
        if idx < n:
            shifted[idx:] = seg[: n - idx]
        env += shifted
    tail_start = int(0.03 * SR)
    tail = np.zeros(n)
    tail[tail_start:] = exp_env(t[: n - tail_start], 0.075)
    env = np.clip(env * 0.8 + tail * 0.55, 0, None)
    sig = body * env
    return np.tanh(sig * 1.5) * 0.95

def closed_hat(dur=0.055):
    t, n = t_axis(dur)
    sig = _glassy_noise(n, [420.0, 630.0, 890.0, 1330.0, 1810.0, 2450.0, 3100.0], 5000) * exp_env(t, 0.009)
    return np.tanh(sig * 1.2) * 0.88

def open_hat(dur=0.38):
    t, n = t_axis(dur)
    sig = _glassy_noise(n, [420.0, 630.0, 890.0, 1330.0, 1810.0, 2450.0, 3100.0], 5000) * exp_env(t, 0.2)
    return np.tanh(sig * 1.2) * 0.88

def perc(dur=0.1):
    """Woody sidestick -- a short noise crack with a low resonant tone
    spike, thinner and drier than the other kits' rimshot/cowbell slots."""
    t, n = t_axis(dur)
    tone = np.sin(2 * np.pi * 700 * t) * exp_env(t, 0.012)
    noise = _band_noise(n, 1000, 6000) * exp_env(t, 0.02)
    sig = tone * 0.45 + noise * 0.7
    return np.tanh(sig * 1.5) * 0.9

def main():
    out_dir = os.path.join(os.path.dirname(__file__), "..", "kits", "linn")
    os.makedirs(out_dir, exist_ok=True)
    np.random.seed(1980)

    write_wav(os.path.join(out_dir, "kick.wav"), kick())
    write_wav(os.path.join(out_dir, "snare.wav"), snare())
    write_wav(os.path.join(out_dir, "clap.wav"), clap())
    write_wav(os.path.join(out_dir, "chh.wav"), closed_hat())
    write_wav(os.path.join(out_dir, "ohh.wav"), open_hat())
    write_wav(os.path.join(out_dir, "perc.wav"), perc())

    write_wav(os.path.join(out_dir, "kick_2.wav"), kick(start_freq=200, end_freq=64, pitch_decay=0.014, amp_decay=0.1))
    write_wav(os.path.join(out_dir, "snare_2.wav"), snare(dur=0.16))

    print("Wrote linn-style kit to", os.path.abspath(out_dir))
    for f in sorted(os.listdir(out_dir)):
        print(" -", f)

if __name__ == "__main__":
    main()
