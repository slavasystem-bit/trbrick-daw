#!/usr/bin/env python3
"""
K1 (v44.32): new kicks and the rest of the TR-808-style percussion, added NEXT TO the files t8_kitgen writes (nothing is overwritten,
the old default sounds stay as they are). Run it after t8_kitgen:   python3 tools/gen_k1_kits.py
Everything is synthesized here (sine bodies with a pitch fall, noise bursts, resonators); no recording of any machine is used.

Why: the measured references (docs/ROADMAP.md, row 14g) have a kick that is a tuned sub (43-49 Hz, F1 / G1) with a long tail and an audible
click (-20 ... -33 dB, measured as the 1.5-6 kHz energy of the first 12 ms against the 25-500 Hz energy of the first 80 ms). The old 808 kick
measured -54 dB. The three new kicks are calibrated to the same measure.

kits/808:      kick_3 = 808 SNAP, kick_4 = 808 HARD, kick_5 = 808 SUB F1;
               perc_4 cowbell, perc_5 clave, perc_6 rimshot, perc_7 maracas, perc_8 conga (mid); ohh_2 = cymbal
kits/trap808:  kick_3 = 808 SNAP
kits/techno:   kick_4 = 808 HARD
kits/808perc:  a new kit around the percussion: kick = SNAP, snare / clap / hats from the 808 kit,
               perc = conga low, perc_2 = conga mid, perc_3 = conga high, perc_4 cowbell, perc_5 clave, perc_6 rimshot, perc_7 maracas,
               perc_8 = high tom; ohh_2 = cymbal
"""
import os, shutil
import numpy as np
import scipy.signal as ss
import soundfile as sf

SR = 48000
KITS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "kits")
rng = np.random.default_rng(808)


def T(d): return np.arange(int(SR * d)) / SR
def bp(x, lo, hi, o=2): return ss.sosfilt(ss.butter(o, [lo, hi], 'band', fs=SR, output='sos'), x)
def hp(x, f, o=2): return ss.sosfilt(ss.butter(o, f, 'high', fs=SR, output='sos'), x)
def sweep(t, f0, f1, tau):
    f = f1 + (f0 - f1) * np.exp(-t / tau)
    return np.sin(2 * np.pi * np.cumsum(f) / SR)
def norm(x, db): return x / np.abs(x).max() * 10 ** (db / 20)


def click_metric(y):
    """1.5-6 kHz energy of the first 12 ms against the 25-500 Hz energy of the first 80 ms, in dB (the same measure as the reference analysis)."""
    lo = ss.sosfilt(ss.butter(4, [25, 500], 'band', fs=SR, output='sos'), y)
    hi = ss.sosfilt(ss.butter(4, [1500, 6000], 'band', fs=SR, output='sos'), y[:int(.012 * SR)])
    return 10 * np.log10((hi ** 2).sum() + 1e-12) - 10 * np.log10((lo[:int(.08 * SR)] ** 2).sum() + 1e-12)


def kick_raw(root, decay, f0, tau, drive, click_db, punch, dur):
    t = T(dur); att = np.minimum(1, t / 0.0007)
    body = sweep(t, f0, root, tau) * np.exp(-t / decay) * att
    pun = sweep(t, 170, 95, 0.018) * np.exp(-t / 0.05) * punch
    c = bp(rng.uniform(-1, 1, len(t)) * np.exp(-t / 0.0025), 1800, 6500, 3) * (t < 0.014)
    sig = np.tanh((body + pun) * drive); sig /= np.abs(sig).max()
    return norm(sig + c / np.abs(c).max() * 10 ** (click_db / 20) * 0.9, -1.0)


def kick(target_click, **kw):
    lo, hi = -40.0, 10.0
    state = rng.bit_generator.state
    for _ in range(18):
        mid = (lo + hi) / 2
        rng.bit_generator.state = state
        if click_metric(kick_raw(click_db=mid, **kw)) < target_click: lo = mid
        else: hi = mid
    rng.bit_generator.state = state
    return kick_raw(click_db=(lo + hi) / 2, **kw)


def cowbell():
    t = T(0.5)
    def sq(f):
        s = np.zeros_like(t)
        for k in range(1, 40, 2):
            if f * k < 14000: s += np.sin(2 * np.pi * f * k * t) / k
        return s
    a = sq(540) * (0.6 * np.exp(-t / 0.14) + 0.4 * np.exp(-t / 0.012)) + sq(800) * (0.8 * np.exp(-t / 0.09) + 0.5 * np.exp(-t / 0.01))
    return norm(np.tanh(bp(a, 500, 3500, 2) * 1.5), -7.0)

def clave():
    t = T(0.12)
    return norm(np.sin(2 * np.pi * 2500 * t) * np.exp(-t / 0.011) + 0.4 * np.sin(2 * np.pi * 3320 * t) * np.exp(-t / 0.006), -6.0)

def rimshot():
    t = T(0.15)
    s = np.sin(2 * np.pi * 500 * t) * np.exp(-t / 0.012) + 0.8 * np.sin(2 * np.pi * 1700 * t) * np.exp(-t / 0.007)
    s += hp(rng.uniform(-1, 1, len(t)), 3000) * np.exp(-t / 0.003) * 0.6
    return norm(hp(s, 350), -6.5)

def maracas():
    t = T(0.12)
    return norm(hp(rng.uniform(-1, 1, len(t)), 5500, 3) * np.minimum(1, t / 0.006) * np.exp(-t / 0.028), -9.0)

def tom_or_conga(f, dec, bend=1.18, drive=1.3):
    t = T(0.45)
    s = sweep(t, f * bend, f, 0.02) * np.exp(-t / dec)
    s += bp(rng.uniform(-1, 1, len(t)), 800, 3000) * np.exp(-t / 0.004) * 0.25
    return norm(np.tanh(s * drive), -6.0)

def cymbal():
    t = T(1.6); s = np.zeros_like(t)
    for f in [205.3, 304.4, 369.6, 522.7, 540.0, 800.0]:
        s += np.sign(np.sin(2 * np.pi * f * t)) + 0.6 * np.sign(np.sin(2 * np.pi * f * 2.7 * t))
    return norm(bp(s, 3500, 12000, 3) * (0.35 * np.exp(-t / 0.9) + 0.65 * np.exp(-t / 0.12)), -8.0)


def save(kit, name, y):
    d = os.path.join(KITS, kit); os.makedirs(d, exist_ok=True)
    sf.write(os.path.join(d, name + ".wav"), y.astype(np.float64), SR, subtype="PCM_24")


def main():
    snap = kick(-25, root=48.0, decay=0.22, f0=150.0, tau=0.028, drive=1.8, punch=0.45, dur=0.9)
    hard = kick(-20, root=45.0, decay=0.30, f0=170.0, tau=0.028, drive=3.2, punch=0.45, dur=0.9)
    sub = kick(-30, root=43.65, decay=0.60, f0=120.0, tau=0.035, drive=1.4, punch=0.25, dur=1.3)
    cb, cl, rs, mr, cym = cowbell(), clave(), rimshot(), maracas(), cymbal()
    cg_lo, cg_mid, cg_hi = tom_or_conga(205, 0.30), tom_or_conga(275, 0.22), tom_or_conga(380, 0.16)
    tom_hi = tom_or_conga(180, 0.28, bend=1.5, drive=1.6)

    save("808", "kick_3", snap); save("808", "kick_4", hard); save("808", "kick_5", sub)
    for n, y in [("perc_4", cb), ("perc_5", cl), ("perc_6", rs), ("perc_7", mr), ("perc_8", cg_mid), ("ohh_2", cym)]: save("808", n, y)
    save("trap808", "kick_3", snap)
    save("techno", "kick_4", hard)

    # the 808perc kit: the 808 kit's own snare / clap / hats around the percussion
    os.makedirs(os.path.join(KITS, "808perc"), exist_ok=True)
    for n in ["snare", "clap", "chh", "ohh"]:
        shutil.copyfile(os.path.join(KITS, "808", n + ".wav"), os.path.join(KITS, "808perc", n + ".wav"))
    save("808perc", "kick", snap)
    for n, y in [("perc", cg_lo), ("perc_2", cg_mid), ("perc_3", cg_hi), ("perc_4", cb), ("perc_5", cl), ("perc_6", rs), ("perc_7", mr), ("perc_8", tom_hi), ("ohh_2", cym)]:
        save("808perc", n, y)
    for name, y in [("808 snap", snap), ("808 hard", hard), ("808 sub F1", sub)]:
        print(f"{name:12s} click {click_metric(y):6.1f} dB")


if __name__ == "__main__":
    main()
