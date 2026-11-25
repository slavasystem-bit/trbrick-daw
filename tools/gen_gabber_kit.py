#!/usr/bin/env python3
"""
Generates a procedurally-synthesized "GABBER"-style kit: hardcore/gabber
territory -- a heavily overdriven, distorted kick pushed well past clean
saturation into a crunchy square-ish clip, plus hard, fast everything
else. Nothing here is a sampled/ripped recording of any real product
(see gen_808_kit.py for the full rationale, shared by every kit in this
repo). Uses tools/kit_synth_common.py.

Output: kits/gabber/{kick,snare,clap,chh,ohh,perc}.wav
"""
import os
import sys
import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from kit_synth_common import write_wav, kick, snare, clap, hat, perc


def main():
    out_dir = os.path.join(os.path.dirname(__file__), "..", "kits", "gabber")
    os.makedirs(out_dir, exist_ok=True)
    np.random.seed(4747)

    # drive=4.5 pushes tanh() hard enough into saturation to read as a
    # crunchy, overdriven "hoover kick" rather than a clean 909-style thump.
    write_wav(os.path.join(out_dir, "kick.wav"),
              kick(dur=0.45, start_freq=250, end_freq=45, pitch_decay=0.025, amp_decay=0.30, drive=4.5,
                   click=1.2, click_dur=0.005))
    write_wav(os.path.join(out_dir, "kick_2.wav"),
              kick(dur=0.38, start_freq=280, end_freq=48, pitch_decay=0.02, amp_decay=0.24, drive=4.8,
                   click=1.3, click_dur=0.004))
    write_wav(os.path.join(out_dir, "snare.wav"),
              snare(dur=0.26, tone_freqs=[220, 400], tone_decay=0.025, noise_lo=1000, noise_hi=9000,
                    noise_decay=0.08, drive=2.0))
    write_wav(os.path.join(out_dir, "clap.wav"),
              clap(dur=0.3, low_hz=1200, high_hz=8000, hits=[0.0, 0.007, 0.015, 0.024], burst_decay=0.014,
                   tail_start=0.025, tail_decay=0.06, drive=1.8))
    write_wav(os.path.join(out_dir, "chh.wav"),
              hat(dur=0.065, freqs=[260, 380, 470, 620, 780, 1000], hp_hz=5000, decay=0.011, drive=1.6))
    write_wav(os.path.join(out_dir, "ohh.wav"),
              hat(dur=0.32, freqs=[260, 380, 470, 620, 780, 1000], hp_hz=5000, decay=0.18, drive=1.6))
    write_wav(os.path.join(out_dir, "perc.wav"),
              perc(dur=0.2, start_freq=400, end_freq=280, pitch_decay=0.03, amp_decay=0.10, drive=2.0))

    print("Wrote GABBER-style kit to", os.path.abspath(out_dir))
    for f in sorted(os.listdir(out_dir)):
        print(" -", f)


if __name__ == "__main__":
    main()
