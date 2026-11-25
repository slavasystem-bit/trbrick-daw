#!/usr/bin/env python3
"""
Generates a procedurally-synthesized "DUB"-style kit: deep and spacious --
this synth engine has no convolution reverb, so the "room" character
here comes from deliberately long natural decays (roomy noise tails on
the snare/clap, a long-ringing open hat and perc/tom) rather than an
actual reverb send; pair with the FX tab's reverb send for the full dub
effect. Nothing here is a sampled/ripped recording of any real product
(see gen_808_kit.py for the full rationale, shared by every kit in this
repo). Uses tools/kit_synth_common.py.

Output: kits/dub/{kick,snare,clap,chh,ohh,perc}.wav
"""
import os
import sys
import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from kit_synth_common import write_wav, kick, snare, clap, hat, perc


def main():
    out_dir = os.path.join(os.path.dirname(__file__), "..", "kits", "dub")
    os.makedirs(out_dir, exist_ok=True)
    np.random.seed(5151)

    write_wav(os.path.join(out_dir, "kick.wav"),
              kick(dur=0.9, start_freq=130, end_freq=40, pitch_decay=0.12, amp_decay=0.6, drive=1.25, click=0.45))
    write_wav(os.path.join(out_dir, "kick_2.wav"),
              kick(dur=0.75, start_freq=145, end_freq=42, pitch_decay=0.1, amp_decay=0.5, drive=1.25, click=0.4))
    write_wav(os.path.join(out_dir, "snare.wav"),
              snare(dur=0.45, tone_freqs=[160, 280], tone_decay=0.09, noise_lo=700, noise_hi=4000,
                    noise_decay=0.25, drive=1.1))
    write_wav(os.path.join(out_dir, "clap.wav"),
              clap(dur=0.55, low_hz=700, high_hz=3500, hits=[0.0, 0.016, 0.034], burst_decay=0.03,
                   tail_start=0.05, tail_decay=0.22, drive=1.1))
    write_wav(os.path.join(out_dir, "chh.wav"),
              hat(dur=0.1, freqs=[200, 300, 380, 480, 580, 720], hp_hz=3000, decay=0.02, drive=0.9))
    write_wav(os.path.join(out_dir, "ohh.wav"),
              hat(dur=0.7, freqs=[200, 300, 380, 480, 580, 720], hp_hz=3000, decay=0.5, drive=0.9))
    write_wav(os.path.join(out_dir, "perc.wav"),
              perc(dur=0.55, start_freq=260, end_freq=170, pitch_decay=0.08, amp_decay=0.35, drive=1.1))

    print("Wrote DUB-style kit to", os.path.abspath(out_dir))
    for f in sorted(os.listdir(out_dir)):
        print(" -", f)


if __name__ == "__main__":
    main()
