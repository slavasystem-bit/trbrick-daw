#!/usr/bin/env python3
"""
Generates a procedurally-synthesized "LOFI"-style kit: dusty, degraded,
filtered character for lo-fi hip-hop-style grooves. Starts from ordinary
808-ish drum voices, then runs every sample through bitcrush() (amplitude
quantization + sample-and-hold downsampling) to get the gritty, slightly
"underwater" texture associated with the genre -- no vinyl-rip samples
involved, just deliberate digital degradation of a synthesized source
(see gen_808_kit.py for the full rationale shared by every kit in this
repo). Uses tools/kit_synth_common.py.

Output: kits/lofi/{kick,snare,clap,chh,ohh,perc}.wav
"""
import os
import sys
import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from kit_synth_common import write_wav, kick, snare, clap, hat, perc, bitcrush


def main():
    out_dir = os.path.join(os.path.dirname(__file__), "..", "kits", "lofi")
    os.makedirs(out_dir, exist_ok=True)
    np.random.seed(1010)

    write_wav(os.path.join(out_dir, "kick.wav"),
              bitcrush(kick(dur=0.45, start_freq=150, end_freq=50, pitch_decay=0.08, amp_decay=0.3,
                            drive=1.6, click=0.85), bits=8, rate_divide=2))
    write_wav(os.path.join(out_dir, "kick_2.wav"),
              bitcrush(kick(dur=0.38, start_freq=165, end_freq=52, pitch_decay=0.065, amp_decay=0.24,
                            drive=1.6, click=0.8), bits=8, rate_divide=2))
    write_wav(os.path.join(out_dir, "snare.wav"),
              bitcrush(snare(dur=0.3, tone_freqs=[180, 300], tone_decay=0.05, noise_lo=800, noise_hi=4500,
                             noise_decay=0.10, drive=1.2), bits=7, rate_divide=3))
    write_wav(os.path.join(out_dir, "clap.wav"),
              bitcrush(clap(dur=0.35, low_hz=700, high_hz=4000, hits=[0.0, 0.012, 0.026], burst_decay=0.02,
                            tail_start=0.03, tail_decay=0.09, drive=1.2), bits=7, rate_divide=3))
    write_wav(os.path.join(out_dir, "chh.wav"),
              bitcrush(hat(dur=0.09, freqs=[210, 310, 390, 500, 600, 760], hp_hz=3200, decay=0.016, drive=1.0),
                       bits=7, rate_divide=2))
    write_wav(os.path.join(out_dir, "ohh.wav"),
              bitcrush(hat(dur=0.4, freqs=[210, 310, 390, 500, 600, 760], hp_hz=3200, decay=0.26, drive=1.0),
                       bits=7, rate_divide=2))
    write_wav(os.path.join(out_dir, "perc.wav"),
              bitcrush(perc(dur=0.3, start_freq=300, end_freq=210, pitch_decay=0.05, amp_decay=0.16, drive=1.1),
                       bits=7, rate_divide=3))

    print("Wrote LOFI-style kit to", os.path.abspath(out_dir))
    for f in sorted(os.listdir(out_dir)):
        print(" -", f)


if __name__ == "__main__":
    main()
