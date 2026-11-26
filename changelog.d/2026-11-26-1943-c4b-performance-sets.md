## Stage C4b (part 9): performance sets and the right-click menu in the SAMPLES browser

ROADMAP "Samples in one place" (C4b), item 4 (collections: sets for a gig); approved mockup `docs/mockups/v45_samples_unified.png`, section 2 ("right-click a row = Tags... / Show in Finder / Add to set", "Set 26 Nov" under MY COLLECTIONS).

- **Right-click a file = a menu**: GENRE / TAGS... (library files), ADD TO SET (each set, "in it" when it already is; NEW SET makes "Set 26 Nov", "Set 26 Nov 2" ...), REMOVE FROM <set> (inside a set), SHOW IN FINDER (library, kit and project files).
- **Sets under MY COLLECTIONS** with their counts. A set holds any file the favourites can (library, kits, this project, transitions); TYPE, GENRE, TAGS, BPM, search and sorting work in it. Right-click a set: RENAME (Return) or DELETE THIS SET (only the list goes; the files stay).
- **Kept in `Documents/TR-Brick/Library/Favorites/sample_sets.txt`** (`src/core/SampleSetStore.h`, pure, tested): `[name]` then one `- key` line per file. Read off the UI thread, every change written whole (atomic) on the loader thread in order, and the last one again at quit (`AppState::FlushSampleSets`). A failed save says CANNOT SAVE THE SAMPLE SETS. Damaged lines, repeats and a second set of the same name are skipped. At most 64 sets of 2000 files, names up to 40 characters.
- While a menu is open it owns the keys (no search typing, browse, take or close underneath); Esc closes it.
