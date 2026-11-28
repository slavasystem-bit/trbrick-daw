## CHORD LAB: undo step taken under the scene lock

- Dragging the length edge of a chord or picking a length from its right-click menu pushed the undo step (or started the
  chord draft) without holding the scene lock, so the audio thread could read the project while it changed.
  `ChordLabPanel::BeginEdit` now takes the (recursive) scene lock itself. Found by `t8_uitest --seeds 60` (seed 50).
