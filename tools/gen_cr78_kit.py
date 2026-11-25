#!/usr/bin/env python3
"""
Generates a procedurally-synthesized "CR78-style" kit: an early-vintage,
combo-organ-era rhythm-box character -- warm, soft-edged, understated.
Mellower than the 606 kit (longer/rounder kick, gentler hats, softer
clap) for ballad/lo-fi/downtempo grooves. Nothing here is a sampled/
ripped recording of any real product (see gen_808_kit.py for the full
rationale, shared by every kit in this repo). Uses tools/kit_synth_common.py.

Output: kits/cr78/{kick,snare,clap,chh,ohh,perc}.wav
"""
import os
import sys
import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from kit_synth_common import write_wav, kick, snare, clap, hat, perc


def main():
    out_dir = os.path.join(os.path.dirname(__file__), "..", "kits", "cr78")
    os.makedirs(out_dir, exist_ok=True)
    np.random.seed(7878)

    write_wav(os.path.join(out_dir, "kick.wav"),
              kick(dur=0.5, start_freq=120, end_freq=45, pitch_decay=0.10, amp_decay=0.35, drive=1.5, click=0.65))
    write_wav(os.path.join(out_dir, "kick_2.wav"),
              kick(dur=0.42, start_freq=135, end_freq=48, pitch_decay=0.08, amp_decay=0.28, drive=1.5, click=0.6))
    write_wav(os.path.join(out_dir, "snare.wav"),
              snare(dur=0.32, tone_freqs=[170, 300], tone_decay=0.06, noise_lo=700, noise_hi=4000,
                    noise_decay=0.12, drive=1.1))
    write_wav(os.path.join(out_dir, "clap.wav"),
              clap(dur=0.4, low_hz=700, high_hz=3500, hits=[0.0, 0.014, 0.030], burst_decay=0.022,
                   tail_start=0.04, tail_decay=0.10, drive=1.0))
    write_wav(os.path.join(out_dir, "chh.wav"),
              hat(dur=0.09, freqs=[220, 330, 400, 510, 600, 750], hp_hz=3000, decay=0.020, drive=1.0))
    write_wav(os.path.join(out_dir, "ohh.wav"),
              hat(dur=0.45, freqs=[220, 330, 400, 510, 600, 750], hp_hz=3000, decay=0.30, drive=1.0))
    write_wav(os.path.join(out_dir, "perc.wav"),
              perc(dur=0.32, start_freq=260, end_freq=180, pitch_decay=0.05, amp_decay=0.20, drive=1.1))

    print("Wrote CR78-style kit to", os.path.abspath(out_dir))
    for f in sorted(os.listdir(out_dir)):
        print(" -", f)


if __name__ == "__main__":
    main()
