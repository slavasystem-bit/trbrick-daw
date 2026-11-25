#!/usr/bin/env python3
"""
Generates a procedurally-synthesized "606-style" kit: light, papery,
softer-edged analog character -- thinner/higher hats than the 808/909,
a gentle low-punch kick with almost no click, built for quiet practice-
amp-style grooves rather than a big room. Nothing here is a sampled/
ripped recording of any real product (see gen_808_kit.py for the full
rationale, shared by every kit in this repo). Uses tools/kit_synth_common.py.

Output: kits/606/{kick,snare,clap,chh,ohh,perc}.wav
"""
import os
import sys
import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from kit_synth_common import write_wav, kick, snare, clap, hat, perc


def main():
    out_dir = os.path.join(os.path.dirname(__file__), "..", "kits", "606")
    os.makedirs(out_dir, exist_ok=True)
    np.random.seed(6066)

    write_wav(os.path.join(out_dir, "kick.wav"),
              kick(dur=0.35, start_freq=150, end_freq=50, pitch_decay=0.07, amp_decay=0.22, drive=1.5, click=0.75))
    write_wav(os.path.join(out_dir, "kick_2.wav"),
              kick(dur=0.3, start_freq=170, end_freq=55, pitch_decay=0.055, amp_decay=0.18, drive=1.55, click=0.7))
    write_wav(os.path.join(out_dir, "snare.wav"),
              snare(dur=0.3, tone_freqs=[190, 260], tone_decay=0.05, noise_lo=1200, noise_hi=7000,
                    noise_decay=0.09, drive=1.1))
    write_wav(os.path.join(out_dir, "clap.wav"),
              clap(dur=0.35, low_hz=900, high_hz=5000, hits=[0.0, 0.012, 0.024], burst_decay=0.018,
                   tail_start=0.03, tail_decay=0.08, drive=1.1))
    write_wav(os.path.join(out_dir, "chh.wav"),
              hat(dur=0.08, freqs=[280, 410, 500, 650, 900, 1200], hp_hz=5000, decay=0.012, drive=1.0))
    write_wav(os.path.join(out_dir, "ohh.wav"),
              hat(dur=0.4, freqs=[280, 410, 500, 650, 900, 1200], hp_hz=5000, decay=0.22, drive=1.0))
    write_wav(os.path.join(out_dir, "perc.wav"),
              perc(dur=0.3, start_freq=300, end_freq=200, pitch_decay=0.05, amp_decay=0.18, drive=1.1))

    print("Wrote 606-style kit to", os.path.abspath(out_dir))
    for f in sorted(os.listdir(out_dir)):
        print(" -", f)


if __name__ == "__main__":
    main()
