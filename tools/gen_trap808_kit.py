#!/usr/bin/env python3
"""
Generates a procedurally-synthesized "TRAP808"-style kit: a deep,
long-tailed sub kick (the "808" in the trap sense: pitched, sustained,
almost bass-like) paired with very short, crisp, high-passed hats built
to read clearly even when programmed as dense 16th/32nd-note rolls.
Nothing here is a sampled/ripped recording of any real product (see
gen_808_kit.py for the full rationale, shared by every kit in this
repo). Uses tools/kit_synth_common.py.

Output: kits/trap808/{kick,snare,clap,chh,ohh,perc}.wav
"""
import os
import sys
import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from kit_synth_common import write_wav, kick, snare, clap, hat, perc


def main():
    out_dir = os.path.join(os.path.dirname(__file__), "..", "kits", "trap808")
    os.makedirs(out_dir, exist_ok=True)
    np.random.seed(8080)

    # Long amp_decay/pitch_decay = a sustained, almost-tonal sub kick rather
    # than a short punchy thump -- the trap "808" convention.
    write_wav(os.path.join(out_dir, "kick.wav"),
              kick(dur=0.9, start_freq=110, end_freq=35, pitch_decay=0.15, amp_decay=0.7, drive=1.1, click=0.3))
    write_wav(os.path.join(out_dir, "kick_2.wav"),
              kick(dur=0.7, start_freq=125, end_freq=38, pitch_decay=0.12, amp_decay=0.55, drive=1.15, click=0.35))
    write_wav(os.path.join(out_dir, "snare.wav"),
              snare(dur=0.3, tone_freqs=[190, 340], tone_decay=0.035, noise_lo=1200, noise_hi=9000,
                    noise_decay=0.08, drive=1.5))
    write_wav(os.path.join(out_dir, "clap.wav"),
              clap(dur=0.32, low_hz=1000, high_hz=7000, hits=[0.0, 0.008, 0.017, 0.027], burst_decay=0.014,
                   tail_start=0.028, tail_decay=0.06, drive=1.5))
    # Very short "tick" hats -- crisp enough that a fast trap-style roll of
    # closely-packed hits still reads as distinct notes rather than a blur.
    write_wav(os.path.join(out_dir, "chh.wav"),
              hat(dur=0.045, freqs=[350, 500, 650, 850, 1050, 1300, 1600], hp_hz=6000, decay=0.008, drive=1.3))
    write_wav(os.path.join(out_dir, "ohh.wav"),
              hat(dur=0.3, freqs=[350, 500, 650, 850, 1050, 1300, 1600], hp_hz=6000, decay=0.25, drive=1.3))
    write_wav(os.path.join(out_dir, "perc.wav"),
              perc(dur=0.2, start_freq=380, end_freq=260, pitch_decay=0.04, amp_decay=0.15, drive=1.4))

    print("Wrote TRAP808-style kit to", os.path.abspath(out_dir))
    for f in sorted(os.listdir(out_dir)):
        print(" -", f)


if __name__ == "__main__":
    main()
