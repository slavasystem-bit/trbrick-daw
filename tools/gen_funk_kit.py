#!/usr/bin/env python3
"""
Generates a procedurally-synthesized "FUNK"-style kit: warm, punchy and
a little more full-bodied than the 808/909 kits -- aimed at funk/disco/
boom-bap-style grooves where the drums need to sit forward and "live"
rather than mechanical. A woody, conga-like perc voice rounds it out.
Nothing here is a sampled/ripped recording of any real product (see
gen_808_kit.py for the full rationale, shared by every kit in this
repo). Uses tools/kit_synth_common.py.

Output: kits/funk/{kick,snare,clap,chh,ohh,perc}.wav
"""
import os
import sys
import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from kit_synth_common import write_wav, kick, snare, clap, hat, perc


def main():
    out_dir = os.path.join(os.path.dirname(__file__), "..", "kits", "funk")
    os.makedirs(out_dir, exist_ok=True)
    np.random.seed(6161)

    write_wav(os.path.join(out_dir, "kick.wav"),
              kick(dur=0.5, start_freq=140, end_freq=50, pitch_decay=0.09, amp_decay=0.32, drive=1.3, click=0.5))
    write_wav(os.path.join(out_dir, "kick_2.wav"),
              kick(dur=0.42, start_freq=155, end_freq=53, pitch_decay=0.075, amp_decay=0.26, drive=1.3, click=0.45))
    write_wav(os.path.join(out_dir, "snare.wav"),
              snare(dur=0.34, tone_freqs=[190, 320], tone_decay=0.06, noise_lo=700, noise_hi=5500,
                    noise_decay=0.13, drive=1.3))
    write_wav(os.path.join(out_dir, "clap.wav"),
              clap(dur=0.4, low_hz=800, high_hz=5000, hits=[0.0, 0.013, 0.027], burst_decay=0.021,
                   tail_start=0.035, tail_decay=0.11, drive=1.25))
    write_wav(os.path.join(out_dir, "chh.wav"),
              hat(dur=0.09, freqs=[230, 340, 420, 540, 650, 820], hp_hz=3500, decay=0.017, drive=1.0))
    write_wav(os.path.join(out_dir, "ohh.wav"),
              hat(dur=0.42, freqs=[230, 340, 420, 540, 650, 820], hp_hz=3500, decay=0.3, drive=1.0))
    write_wav(os.path.join(out_dir, "perc.wav"),
              perc(dur=0.35, start_freq=320, end_freq=220, pitch_decay=0.06, amp_decay=0.2, drive=1.2))

    print("Wrote FUNK-style kit to", os.path.abspath(out_dir))
    for f in sorted(os.listdir(out_dir)):
        print(" -", f)


if __name__ == "__main__":
    main()
