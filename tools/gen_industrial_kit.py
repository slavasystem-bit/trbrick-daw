#!/usr/bin/env python3
"""
Generates a procedurally-synthesized "INDUSTRIAL"-style kit: harsh,
heavily-driven and metallic -- for industrial techno / EBM-adjacent
grooves. Hard-clipped kick and snare, a metallic "clang" in place of a
tuned perc voice, and dense high-passed hats. Nothing here is a sampled/
ripped recording of any real product (see gen_808_kit.py for the full
rationale, shared by every kit in this repo). Uses tools/kit_synth_common.py.

Output: kits/industrial/{kick,snare,clap,chh,ohh,perc}.wav
"""
import os
import sys
import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from kit_synth_common import write_wav, kick, snare, clap, hat, perc


def main():
    out_dir = os.path.join(os.path.dirname(__file__), "..", "kits", "industrial")
    os.makedirs(out_dir, exist_ok=True)
    np.random.seed(1919)

    write_wav(os.path.join(out_dir, "kick.wav"),
              kick(dur=0.4, start_freq=180, end_freq=40, pitch_decay=0.03, amp_decay=0.18, drive=3.0,
                   click=1.0, click_dur=0.006))
    write_wav(os.path.join(out_dir, "kick_2.wav"),
              kick(dur=0.32, start_freq=210, end_freq=42, pitch_decay=0.025, amp_decay=0.14, drive=3.2,
                   click=1.1, click_dur=0.005))
    write_wav(os.path.join(out_dir, "snare.wav"),
              snare(dur=0.26, tone_freqs=[150, 500], tone_decay=0.02, noise_lo=1500, noise_hi=11000,
                    noise_decay=0.07, drive=2.2))
    write_wav(os.path.join(out_dir, "clap.wav"),
              clap(dur=0.3, low_hz=1500, high_hz=9000, hits=[0.0, 0.006, 0.014], burst_decay=0.012,
                   tail_start=0.02, tail_decay=0.05, drive=2.0))
    write_wav(os.path.join(out_dir, "chh.wav"),
              hat(dur=0.06, freqs=[300, 410, 530, 700, 880, 1100, 1400], hp_hz=5500, decay=0.010, drive=1.8))
    write_wav(os.path.join(out_dir, "ohh.wav"),
              hat(dur=0.3, freqs=[300, 410, 530, 700, 880, 1100, 1400], hp_hz=5500, decay=0.15, drive=1.8))
    # Metallic clang in place of a tuned perc voice -- lower high-pass and a
    # longer decay than the hats so it reads as a resonant clang, not a hat.
    write_wav(os.path.join(out_dir, "perc.wav"),
              hat(dur=0.35, freqs=[150, 260, 340, 470], hp_hz=2500, decay=0.12, drive=1.6))

    print("Wrote INDUSTRIAL-style kit to", os.path.abspath(out_dir))
    for f in sorted(os.listdir(out_dir)):
        print(" -", f)


if __name__ == "__main__":
    main()
