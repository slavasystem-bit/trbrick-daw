#!/usr/bin/env python3
"""
Generates a procedurally-synthesized "MINIMAL"-style kit: ultra-short,
clicky and percussive -- built for minimal techno, where the groove
comes from tight, small sounds and negative space rather than big,
sustained drums. Every voice here has a much shorter decay than the
other kits in this repo. Nothing here is a sampled/ripped recording of
any real product (see gen_808_kit.py for the full rationale, shared by
every kit in this repo). Uses tools/kit_synth_common.py.

Output: kits/minimal/{kick,snare,clap,chh,ohh,perc}.wav
"""
import os
import sys
import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from kit_synth_common import write_wav, kick, snare, clap, hat, perc


def main():
    out_dir = os.path.join(os.path.dirname(__file__), "..", "kits", "minimal")
    os.makedirs(out_dir, exist_ok=True)
    np.random.seed(2020)

    write_wav(os.path.join(out_dir, "kick.wav"),
              kick(dur=0.22, start_freq=170, end_freq=55, pitch_decay=0.025, amp_decay=0.09, drive=1.5,
                   click=0.6, click_dur=0.003))
    write_wav(os.path.join(out_dir, "kick_2.wav"),
              kick(dur=0.18, start_freq=190, end_freq=58, pitch_decay=0.02, amp_decay=0.07, drive=1.6,
                   click=0.65, click_dur=0.0025))
    write_wav(os.path.join(out_dir, "snare.wav"),
              snare(dur=0.15, tone_freqs=[200, 360], tone_decay=0.015, noise_lo=1200, noise_hi=8000,
                    noise_decay=0.035, drive=1.3))
    write_wav(os.path.join(out_dir, "clap.wav"),
              clap(dur=0.14, low_hz=1200, high_hz=7000, hits=[0.0, 0.006, 0.012], burst_decay=0.008,
                   tail_start=0.018, tail_decay=0.03, drive=1.3))
    write_wav(os.path.join(out_dir, "chh.wav"),
              hat(dur=0.04, freqs=[260, 380, 470, 610, 760, 950], hp_hz=5000, decay=0.007, drive=1.1))
    write_wav(os.path.join(out_dir, "ohh.wav"),
              hat(dur=0.15, freqs=[260, 380, 470, 610, 760, 950], hp_hz=5000, decay=0.09, drive=1.1))
    write_wav(os.path.join(out_dir, "perc.wav"),
              perc(dur=0.1, start_freq=450, end_freq=340, pitch_decay=0.02, amp_decay=0.06, drive=1.3))

    print("Wrote MINIMAL-style kit to", os.path.abspath(out_dir))
    for f in sorted(os.listdir(out_dir)):
        print(" -", f)


if __name__ == "__main__":
    main()
