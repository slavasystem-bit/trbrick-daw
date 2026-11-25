#!/usr/bin/env python3
"""
Finishing pass for the riser one-shots (v36). Run after tools/gen_risers.py (or on its own: it is safe to run twice).

For every kits/risers/riserN.wav it
  1. folds the file to mono and removes everything below 25 Hz,
  2. makes it stereo with a mono-compatible decorrelator (the added signal sits on the side channel only, built from the
     part above ~200 Hz, so the mono sum equals the original and subs stay centred),
  3. sets the level of the loudest 400 ms (ITU-R BS.1770 K-weighted, the "momentary" loudness) to TARGET_LUFS so every riser
     peaks at the same loudness, and keeps the sample peak under PEAK_CEIL_DB,
  4. writes 24-bit / 48 kHz stereo.
Prints a table so the result can be checked.
"""
import os
import sys
import wave
import numpy as np
from scipy import signal

TARGET_LUFS = -9.5      # loudest 400 ms of every riser, measured at full scale (the groove sits around -8 .. -9 LUFS-S at the default master)
PEAK_CEIL_DB = -1.5
OUT_SR = 48000
SPREAD = 0.6


def read_wav(path):
    with wave.open(path) as w:
        n, ch, sr, sw = w.getnframes(), w.getnchannels(), w.getframerate(), w.getsampwidth()
        raw = w.readframes(n)
    if sw == 2:
        x = np.frombuffer(raw, dtype='<i2').astype(np.float64) / 32768.0
    elif sw == 3:
        a = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = a[:, 0] | (a[:, 1] << 8) | (a[:, 2] << 16)
        x = np.where(v & 0x800000, v - (1 << 24), v) / 8388608.0
    else:
        raise SystemExit(f"unsupported sample width {sw} in {path}")
    return x.reshape(-1, ch), sr


def write_wav24(path, x, sr):
    pcm = np.clip(np.round(x * 8388607.0), -8388608, 8388607).astype(np.int32)
    b = np.zeros((pcm.size, 3), dtype=np.uint8)
    flat = pcm.reshape(-1)
    b[:, 0] = flat & 0xFF; b[:, 1] = (flat >> 8) & 0xFF; b[:, 2] = (flat >> 16) & 0xFF
    with wave.open(path, 'w') as w:
        w.setnchannels(pcm.shape[1]); w.setsampwidth(3); w.setframerate(sr); w.writeframes(b.tobytes())


def k_weight(x, sr):
    G, f0, Q = 3.999843853973347, 1681.974450955533, 0.7071752369554196
    K = np.tan(np.pi * f0 / sr); Vh = 10 ** (G / 20); Vb = Vh ** 0.4996667741545416; a0 = 1 + K / Q + K * K
    y = signal.lfilter([(Vh + Vb * K / Q + K * K) / a0, 2 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0],
                       [1, 2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0], x, axis=0)
    g0, q0 = 38.13547087602444, 0.5003270373238773
    K = np.tan(np.pi * g0 / sr); a0 = 1 + K / q0 + K * K
    return signal.lfilter([1 / a0, -2 / a0, 1 / a0], [1, 2 * (K * K - 1) / a0, (1 - K / q0 + K * K) / a0], y, axis=0)


def max_momentary(x, sr):
    ms = (k_weight(x, sr) ** 2).sum(axis=1)
    blk, hop = int(0.4 * sr), int(0.1 * sr)
    vals = [ms[i:i + blk].mean() for i in range(0, max(1, len(ms) - blk), hop)]
    return -0.691 + 10 * np.log10(max(max(vals), 1e-12))


def allpass(x, delay, g):
    y = np.zeros_like(x); buf = np.zeros(delay)
    for i in range(len(x)):          # y = -g*x + z ; z <- x + g*y   (delay-line form, matches core/Stereo.h)
        z = buf[i % delay]
        y[i] = -g * x[i] + z
        buf[i % delay] = x[i] + g * y[i]
    return y


def widen(mono, sr):
    hp = signal.butter(2, 200, 'hp', fs=sr, output='sos')
    d = signal.sosfilt(hp, mono)
    d = allpass(d, int(0.0053 * sr), 0.55)
    d = allpass(d, int(0.0081 * sr), 0.55)
    side = SPREAD * d
    return np.stack([mono + side, mono - side], axis=1)


def main():
    folder = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'kits', 'risers')
    files = sorted(f for f in os.listdir(folder) if f.startswith('riser') and f.endswith('.wav'))
    print(f"{'file':12s} {'before LUFS-M':>14s} {'after':>8s} {'peak dB':>8s} {'L/R corr':>9s} {'mono err':>9s}")
    for f in files:
        path = os.path.join(folder, f)
        x, sr = read_wav(path)
        before = max_momentary(np.repeat(x.mean(axis=1, keepdims=True), 2, axis=1), sr)
        mono = x.mean(axis=1)
        mono = signal.sosfilt(signal.butter(2, 25, 'hp', fs=sr, output='sos'), mono)
        st = widen(mono, sr)
        mono_err = np.abs((st[:, 0] + st[:, 1]) / 2 - mono).max() / max(np.abs(mono).max(), 1e-9)
        st = signal.resample_poly(st, OUT_SR // 300, sr // 300, axis=0) if sr != OUT_SR else st
        g = 10 ** ((TARGET_LUFS - max_momentary(st, OUT_SR)) / 20)
        pk = np.abs(st).max() * g
        ceil = 10 ** (PEAK_CEIL_DB / 20)
        if pk > ceil:
            g *= ceil / pk
        st = st * g
        write_wav24(path, st, OUT_SR)
        after = max_momentary(st, OUT_SR)
        corr = np.corrcoef(st[:, 0], st[:, 1])[0, 1]
        print(f"{f:12s} {before:14.1f} {after:8.1f} {20 * np.log10(np.abs(st).max()):8.1f} {corr:9.2f} {mono_err:9.1f}")


if __name__ == '__main__':
    main()
