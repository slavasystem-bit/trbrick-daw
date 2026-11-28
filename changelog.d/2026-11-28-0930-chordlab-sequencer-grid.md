## CHORD LAB: the chord loop as a piece of the sequencer

- THE LOOP is drawn like the LIVE grid: pages of 16 steps (12 on triplet grids) shared with the sequencer
  (`AppState::deskStepPage`), step numbers, Kick above the chords and Bass / Synth2 / Synth3 below (read-only; the rows
  WHAT FITS reads are bright). The width no longer changes when a chord is selected, dropped or recorded (the "next chord ?"
  room used to widen the loop and vanish on a click).
- The chord loop length is the chord track's length as the sequencer plays it. `FitLoop` is gone: dropping, START FROM and
  REC WHAT I PLAY write inside the loop and never grow it; START FROM shortens the chords to fit; EMPTY keeps the length.
  New LEN choice: = SCENE (follows the scene's LEN) or 1 / 2 / 4 bars (the track's own length). Chords past the end stay,
  shaded, and are not played.
- Chords can be dragged to another step (Option-drag copies; the chords it lands on give way), the length edge snaps to steps,
  Left / Right move the selected chord by a step (Shift: a beat). Each is one undo step.
