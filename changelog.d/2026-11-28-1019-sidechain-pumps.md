## Sidechain pumps: any instrument or a steady beat ducks any instruments (part 1, engine)

- The one Kick -> Bass sidechain is replaced by up to 4 independent **pumps** (`PumpSettings` in `core/Types.h`, envelope in
  `core/Pump.h`). Each pump has a source (the hits of any of the 11 instruments, muted or not, or a steady 1/4, 1/8 or 1/16 beat
  fired by the sequencer's grid), ATTACK (2..50 ms), HOLD (0..500 ms), RELEASE (10..1000 ms or 1/32..1/2 following the tempo),
  a LINE / SMOOTH / HARD curve, and its own depth for every instrument. Where several pumps duck one instrument, the deepest
  dip wins. Slide (legato) notes do not fire a pump.
- Applied in `AudioEngine::Render` on each instrument's raw dry buffer, ahead of its EQ and inserts (drums too). The envelopes
  are computed once per block; an instrument no pump reaches costs one check. No allocation on the audio thread (buffers sized
  in `Init`), a 0.5 ms smoother after the envelope, and a 10 ms glide back when a pump is switched off or re-pointed while down.
- Project format 13: `pumpOnN`, `pumpSrcN`, `pumpAttackMsN`, `pumpHoldMsN`, `pumpReleaseMsN`, `pumpReleaseDivN`, `pumpCurveN`,
  `pumpDepthN` in `[fx]`. A file from before keeps its sound: its `sidechainEnabled / Amount / Release` become pump 1
  (Kick -> Bass, LINE, attack 6 ms x amount, release amount x release - attack); selftest checks the converted envelope stays
  within 0.06 of the old one (it measures 0.024).
- Known, accepted change (person's choice 2026-11-28): the old sidechain advanced its duck only while the bass synth was
  sounding, so a kick in a bass rest left the duck frozen until the next bass note, which then started ducked. The pumps run all
  the time, as they should. Old projects where the bass plays with rests therefore pump less than before (in the review's
  example file, about a third of the old effect); a held bass sounds the same.
- MASTER FX's SIDECHAIN row now edits pump 1's Bass depth and release until the full editor lands in MIX LAB > SIDECHAIN (part 2).
- Tests: envelope stages and curves, steady-beat sources on straight and triplet grids, the deeper dip wins, untouched
  instruments stay untouched, click-free fastest attack and switch-off on a 55 Hz sine, save / load round trip, old-file conversion.
