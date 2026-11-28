## MIX LAB > SIDECHAIN: the pumps editor (part 2)

- MIX LAB has two views, switched at the left of its top row: FREQUENCIES (the analysis, as before) and SIDECHAIN
  (`ui_desktop/SidechainPanel.cpp`, per the approved mockup `mockups/sidechain/sidechain_mockup.png`).
- One column per pump (+ ADD PUMP up to 4): ON, SOURCE (any instrument or Every 1/4 / 1/8 / 1/16), the shape drawn over two
  beats with a live dip marker, ATTACK / HOLD / RELEASE knobs (ValueField box, same gestures as MASTER FX), RELEASE AS ms or a
  note length, LINE / SMOOTH / HARD.
- Below: one row per instrument. A + cell adds it at 50 %; a filled cell is a ValueField bar (drag, Shift finer, wheel,
  double-click to type, Option+click or right-click removes) with the live dip as a teal meter; the source's own cell reads "source".
- Right: what the pumps do to the selected instrument over the first two beats of the edited scene (its real hits), QUICK START
  presets (CLASSIC PUMP, SOFT GLUE, HARD EDM, SNARE MAKES ROOM, GHOST 1/4) filling the first free pump, and how to use it.
- Edits go into `project.fx.pumps` and the engine's copy under the scene lock with one coalesced sound undo.
- MASTER FX's SIDECHAIN row is now a status line with OPEN SIDECHAIN (the stopgap knobs from part 1 are gone).
- Review #62 advice: steady-beat pumps now follow the scene's swing (the sequencer schedules a swung off-beat like a hat:
  `AudioEngine::PumpBeatMask` + `PumpFire`, a `RatchetEvent` with `pumpIndex`); MIX LAB writes the pumps through
  `AudioEngine::SetPumps`, under the engine lock.
- Not in this part (differs from the mockup): Option+click on a cell removes it (the app-wide reset gesture) instead of
  soloing the source and the instrument.
