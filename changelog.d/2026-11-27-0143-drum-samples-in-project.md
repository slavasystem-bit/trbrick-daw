## Drum samples are added into the project

- A `.wav` / `.aif` dropped on a drum track (or a library file taken for it in the SAMPLES browser) is now ADDED as the drum's
  next take (`snare_3.wav` ...): nothing in the kit is replaced or deleted, and every step plays the new take.
- The kit is copied into the project's own folder, `samples/kits/<kit>` beside the project file (once; a copy already there is
  used as it is), instead of `kits/mykit` beside the program. SAVE AS copies the project's kits beside the new file. The KIT
  row shows such a kit as "<kit> (PROJECT)". One Undo brings back the previous kit and sample.
- Project format 11: `kitFolder` / `trackKit` may be `samples/kits/<kit>`, relative to the project's folder; a name with `..`
  or a deeper path is not read. An older TR-Brick keeps the file and saves a copy. Older `kits/mykit...` kits keep working.
- SAVE AS (which now copies the samples before it writes the file) into a folder whose `samples/kits/<kit>` holds other takes under the same file names copies the kit under a free
  name (`<kit>_2` ...) and points the project, its Undo steps and the clipboards at it, so it keeps playing its own samples.
- At start the project kit is read from beside the project file (no false "drums will be silent" warning).
- Kit banks are cached by their place on disk, so two projects with a kit of the same name never share a bank.
- Tests: selftest (drop, factory kit untouched, Undo, second drop, SAVE AS, file round trip, unsafe names), fuzz op DrumSample.
