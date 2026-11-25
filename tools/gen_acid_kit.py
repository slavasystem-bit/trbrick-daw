#!/usr/bin/env python3
"""
Generates a procedurally-synthesized "ACID"-style kit: an aggressive,
driving techno/acid-house voice -- hard-clipped kick, snappy tight clap,
hard hats -- built to sit underneath a resonant acid bassline without
getting buried. Nothing here is a sampled/ripped recording of any real
product (see gen_808_kit.py for the full rationale, shared by every kit
in this repo). Uses tools/kit_synth_common.py.

Output: kits/acid/{kick,snare,clap,chh,ohh,perc}.wav
"""
import os
import sys
import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from kit_synth_common import write_wav, kick, snare, clap, hat, perc


def main():
    out_dir = os.path.join(os.path.dirname(__file__), "..", "kits", "acid")
    os.makedirs(out_dir, exist_ok=True)
    np.random.seed(3030)

    write_wav(os.path.join(out_dir, "kick.wav"),
              kick(dur=0.4, start_freq=220, end_freq=50, pitch_decay=0.04, amp_decay=0.22, drive=2.2, click=0.8))
    write_wav(os.path.join(out_dir, "kick_2.wav"),
              kick(dur=0.32, start_freq=250, end_freq=52, pitch_decay=0.03, amp_decay=0.16, drive=2.4, click=0.9))
    write_wav(os.path.join(out_dir, "snare.wav"),
              snare(dur=0.28, tone_freqs=[210, 390], tone_decay=0.03, noise_lo=900, noise_hi=9000,
                    noise_decay=0.09, drive=1.6))
    write_wav(os.path.join(out_dir, "clap.wav"),
              clap(dur=0.35, low_hz=1000, high_hz=7000, hits=[0.0, 0.008, 0.016, 0.026], burst_decay=0.015,
                   tail_start=0.03, tail_decay=0.07, drive=1.6))
    write_wav(os.path.join(out_dir, "chh.wav"),
              hat(dur=0.07, freqs=[240, 360, 430, 580, 650, 950], hp_hz=4500, decay=0.012, drive=1.4))
    write_wav(os.path.join(out_dir, "ohh.wav"),
              hat(dur=0.35, freqs=[240, 360, 430, 580, 650, 950], hp_hz=4500, decay=0.20, drive=1.4))
    write_wav(os.path.join(out_dir, "perc.wav"),
              perc(dur=0.15, start_freq=500, end_freq=350, pitch_decay=0.03, amp_decay=0.12, drive=1.5))

    print("Wrote ACID-style kit to", os.path.abspath(out_dir))
    for f in sorted(os.listdir(out_dir)):
        print(" -", f)


if __name__ == "__main__":
    main()
