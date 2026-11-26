## Stage C4b (part 10): edit the sample on its waveform

ROADMAP "Samples in one place" (C4b), item 5 (gestures of the waveform and the marks); approved mockup `docs/mockups/v45_samples_unified.png`, section 3 ("Start and length", "Slice marks").

- **Drag the white line = START, the coloured line = the end (LENGTH)** on the waveform of every drum track and of the sampler / a synth on SAMPLE. The same values as the START and LENGTH knobs (they follow at once); moving START keeps the end where it is. The line under the mouse gets thicker and the cursor shows it can be dragged. One Cmd+Z undoes a whole drag; a click without a move changes nothing.
- **HITS marks** like the points in the rows: click on nothing = add a mark, drag a mark = move it (it may pass a neighbour; marks stay in order and never land on each other), double-click a mark = delete it, right-click = menu (ADD A MARK HERE, DELETE THIS MARK, CLEAR ALL MARKS). Before: right-click deleted the nearest mark.
- A waveform with a sample no longer opens the SAMPLES browser on a click (the name does, and an empty card still does), so a drag can never open it by mistake.
- `AppState::MoveSliceMark` (no Undo of its own: one per drag) and `AppState::ClearSliceMarks` (undoable), tested.
