#!/usr/bin/env python3
"""
Shared synthesis building blocks used by the gen_*_kit.py scripts that
ship with this repo. Nothing here is a sampled/ripped recording of any
real drum machine -- every sound is built from oscillators, noise and
envelopes, same philosophy as gen_808_kit.py/gen_909_kit.py/gen_707_kit.py/
gen_linn_kit.py (which predate this file and still contain their own
full copies of similar functions -- left untouched rather than risking
a refactor of working, already-shipped generators).

This module exists so the additional ~10 kits added in a later round
(see gen_606_kit.py etc.) don't each need their own ~150-line copy of the
same kick/snare/clap/hat/perc synthesis code -- every one of those scripts
just calls these functions with kit-specific parameters (frequencies,
decays, drive amount) to land in a different sonic territory, plus one
or two kit-specific finishing touches (e.g. bitcrushing for the lo-fi kit).
"""
import numpy as np
import wave

SR = 44100


def write_wav(path, samples, sr=SR):
    samples = np.clip(samples, -1.0, 1.0)
    pcm = (samples * 32767.0).astype(np.int16)
    with wave.open(path, "w") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(pcm.tobytes())


def t_axis(dur, sr=SR):
    n = int(sr * dur)
    return np.arange(n) / sr, n


def exp_env(t, decay):
    return np.exp(-t / decay)


def band_noise(n, low_hz, high_hz, sr=SR):
    """Crude FFT band-pass of white noise (fine for one-shot percussive use)."""
    noise = np.random.uniform(-1, 1, n)
    spec = np.fft.rfft(noise)
    freqs = np.fft.rfftfreq(n, 1.0 / sr)
    mask = (freqs >= low_hz) & (freqs <= high_hz)
    spec = spec * mask
    out = np.fft.irfft(spec, n)
    peak = np.max(np.abs(out)) or 1.0
    return out / peak


def metallic_noise(n, base_freqs, hp_hz, sr=SR):
    """Several detuned square waves ring-mixed then high-passed, leaving
    only metallic upper harmonics -- the classic analog hat-circuit trick
    also used by gen_808_kit.py/gen_909_kit.py."""
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


def bitcrush(sig, bits=8, rate_divide=1):
    """Amplitude quantization + optional sample-and-hold downsampling --
    used only by the lo-fi kit to get a dusty, degraded character."""
    levels = 2 ** bits
    q = np.round(sig * levels) / max(1, levels)
    if rate_divide > 1:
        held = np.repeat(q[::rate_divide], rate_divide)[: len(q)]
        return held
    return q


def kick(dur, start_freq, end_freq, pitch_decay, amp_decay, drive=1.6, click=0.5, click_dur=0.004, sr=SR):
    t, n = t_axis(dur, sr)
    freq = end_freq + (start_freq - end_freq) * np.exp(-t / pitch_decay)
    phase = 2 * np.pi * np.cumsum(freq) / sr
    tone = np.sin(phase)
    amp = exp_env(t, amp_decay)
    sig = tone * amp
    if click > 0:
        cn = int(sr * click_dur)
        csig = np.random.uniform(-1, 1, cn) * np.exp(-np.arange(cn) / (sr * 0.001))
        sig[:cn] += csig * click
    return np.tanh(sig * drive) * 0.98


def snare(dur, tone_freqs, tone_decay, noise_lo, noise_hi, noise_decay, tone_mix=0.5, noise_mix=0.9, drive=1.4, sr=SR):
    t, n = t_axis(dur, sr)
    tone = sum(np.sin(2 * np.pi * f * t) for f in tone_freqs) / len(tone_freqs)
    tone = tone * exp_env(t, tone_decay)
    noise = band_noise(n, noise_lo, noise_hi, sr) * exp_env(t, noise_decay)
    sig = tone * tone_mix + noise * noise_mix
    return np.tanh(sig * drive) * 0.95


def clap(dur, low_hz, high_hz, hits, burst_decay, tail_start, tail_decay, drive=1.4, sr=SR):
    t, n = t_axis(dur, sr)
    body = band_noise(n, low_hz, high_hz, sr)
    env = np.zeros(n)
    for h in hits:
        idx = int(h * sr)
        seg = exp_env(t, burst_decay)
        shifted = np.zeros(n)
        if idx < n:
            shifted[idx:] = seg[: n - idx]
        env += shifted
    ts = int(tail_start * sr)
    tail = np.zeros(n)
    tail[ts:] = exp_env(t[: n - ts], tail_decay)
    env = np.clip(env * 0.9 + tail * 0.6, 0, None)
    sig = body * env
    return np.tanh(sig * drive) * 0.95


def hat(dur, freqs, hp_hz, decay, drive=1.2, sr=SR):
    """Used for both closed and open hats -- caller picks dur/decay."""
    t, n = t_axis(dur, sr)
    sig = metallic_noise(n, freqs, hp_hz, sr) * exp_env(t, decay)
    return np.tanh(sig * drive) * 0.9


def perc(dur, start_freq, end_freq, pitch_decay, amp_decay, drive=1.3, sr=SR):
    t, n = t_axis(dur, sr)
    freq = end_freq + (start_freq - end_freq) * np.exp(-t / pitch_decay)
    phase = 2 * np.pi * np.cumsum(freq) / sr
    tone = np.sin(phase)
    amp = exp_env(t, amp_decay)
    sig = tone * amp
    return np.tanh(sig * drive) * 0.95
