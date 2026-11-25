#!/usr/bin/env python3
"""
Generates a procedurally-synthesized "classic 909" style drum kit as 16-bit
mono WAV files. As with gen_808_kit.py, nothing here is a sampled/ripped
recording of any real Roland (or other) product -- every sound is built from
oscillators, noise and envelopes shaped to emulate the *circuit behaviour* of
a classic analog/digital-hybrid 909-style drum voice: a punchier, more
distorted kick with a sharper attack click, a snappier noise-forward snare,
brighter/harder hi-hats, a tighter multi-burst clap, and a rimshot-ish perc
hit. Distinctly punchier and more aggressive than the 808 kit -- this is the
"four on the floor techno/house" voice, not the "boomy hip-hop" one.

Output: kits/909/{kick,snare,clap,chh,ohh,perc}.wav
Same alt-take convention as the 808 kit: kick_2.wav etc. are picked up
automatically as extra variants for that pad.
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
        mix += np.sign(np.sin(2 * np.pi * f * t))
    mix /= len(base_freqs)
    spec = np.fft.rfft(mix)
    freqs = np.fft.rfftfreq(n, 1.0 / sr)
    spec[freqs < hp_hz] = 0
    out = np.fft.irfft(spec, n)
    peak = np.max(np.abs(out)) or 1.0
    return out / peak

def kick(dur=0.42, start_freq=210.0, end_freq=52.0, pitch_decay=0.045, amp_decay=0.24):
    """909-style kick: faster pitch sweep than the 808, a sharper/louder
    attack click, and harder saturation for that "punchy techno kick" edge."""
    t, n = t_axis(dur)
    freq = end_freq + (start_freq - end_freq) * np.exp(-t / pitch_decay)
    phase = 2 * np.pi * np.cumsum(freq) / SR
    tone = np.sin(phase)
    amp = exp_env(t, amp_decay)
    sig = tone * amp
    click_n = int(SR * 0.003)
    click_sig = np.random.uniform(-1, 1, click_n) * np.exp(-np.arange(click_n) / (SR * 0.0006))
    sig[:click_n] += click_sig * 0.9
    sig = np.tanh(sig * 2.3) * 0.98
    return sig

def snare(dur=0.28):
    """909-style snare: noisier and brighter than the 808's, shorter tonal
    body, faster noise decay for a crisp, forward crack."""
    t, n = t_axis(dur)
    tone = (np.sin(2 * np.pi * 210 * t) * 0.5 + np.sin(2 * np.pi * 390 * t) * 0.5)
    tone *= exp_env(t, 0.03)
    noise = _band_noise(n, 900, 9000)
    noise *= exp_env(t, 0.09)
    sig = tone * 0.4 + noise * 0.95
    sig = np.tanh(sig * 1.5) * 0.95
    return sig

def clap(dur=0.35):
    """Tighter, denser multi-burst than the 808's clap -- four quick hits
    instead of three, shorter gaps, brighter band."""
    t, n = t_axis(dur)
    body = _band_noise(n, 1000, 7000)
    env = np.zeros(n)
    for h in (0.0, 0.008, 0.016, 0.026):
        idx = int(h * SR)
        seg = exp_env(t, 0.016)
        shifted = np.zeros(n)
        if idx < n:
            shifted[idx:] = seg[: n - idx]
        env += shifted
    tail_start = int(0.032 * SR)
    tail = np.zeros(n)
    tail[tail_start:] = exp_env(t[: n - tail_start], 0.07)
    env = np.clip(env * 0.85 + tail * 0.5, 0, None)
    sig = body * env
    return np.tanh(sig * 1.5) * 0.95

def closed_hat(dur=0.07):
    t, n = t_axis(dur)
    sig = _metallic_noise(n, [233.0, 349.0, 411.0, 561.0, 617.0, 890.0, 1050.0], 4500) * exp_env(t, 0.013)
    return np.tanh(sig * 1.3) * 0.92

def open_hat(dur=0.42):
    t, n = t_axis(dur)
    sig = _metallic_noise(n, [233.0, 349.0, 411.0, 561.0, 617.0, 890.0, 1050.0], 4500) * exp_env(t, 0.22)
    return np.tanh(sig * 1.3) * 0.92

def perc(dur=0.15):
    """909-style rimshot: a short noise crack plus a woody tone spike --
    fills the flexible 'perc' slot with something snappier than a tom."""
    t, n = t_axis(dur)
    tone = np.sin(2 * np.pi * 950 * t) * exp_env(t, 0.02)
    noise = _band_noise(n, 1500, 8000) * exp_env(t, 0.035)
    sig = tone * 0.5 + noise * 0.8
    return np.tanh(sig * 1.6) * 0.95

def main():
    out_dir = os.path.join(os.path.dirname(__file__), "..", "kits", "909")
    os.makedirs(out_dir, exist_ok=True)
    np.random.seed(9099)

    write_wav(os.path.join(out_dir, "kick.wav"), kick())
    write_wav(os.path.join(out_dir, "snare.wav"), snare())
    write_wav(os.path.join(out_dir, "clap.wav"), clap())
    write_wav(os.path.join(out_dir, "chh.wav"), closed_hat())
    write_wav(os.path.join(out_dir, "ohh.wav"), open_hat())
    write_wav(os.path.join(out_dir, "perc.wav"), perc())

    write_wav(os.path.join(out_dir, "kick_2.wav"), kick(start_freq=260, end_freq=58, pitch_decay=0.03, amp_decay=0.16))
    write_wav(os.path.join(out_dir, "snare_2.wav"), snare(dur=0.2))

    print("Wrote 909-style kit to", os.path.abspath(out_dir))
    for f in sorted(os.listdir(out_dir)):
        print(" -", f)

if __name__ == "__main__":
    main()
