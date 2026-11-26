## Stage C4b (part 8): own genre and tags of library samples

ROADMAP "Samples in one place" (C4b), item 4 (the author's corrections); approved mockup `docs/mockups/v45_samples_unified.png`, section 2.

- **GENRE / TAGS window** in the SAMPLES browser: right-click a library row (or the GENRE / TAGS... button under SELECTED). GENRE is one of NONE and the nine genres, TAGS add up; BACK TO THE GUESS forgets the own choice. The list, the filters and the search use the new words at once; SELECTED says "your own" or "guessed".
- **Kept in `Documents/TR-Brick/Library/sample_tags.txt`** (`src/core/SampleTagStore.h`, pure, tested): one line per corrected file, `path<TAB>GENRE<TAB>TAG,TAG`. Read once off the UI thread, every change written whole (atomic) on the loader thread, in order. A failed save says CANNOT SAVE THE SAMPLE TAGS and the choice stays for this session. The samples and folders are never changed.
- At quit, a change whose write was still waiting behind other loader work is written after the loader stops (`AppState::FlushSampleTags`), so the last click is never lost. After an edit the same file stays selected even when the sort or a filter moves its row.
- Damaged lines, unknown genres or tags (a hand-edited or newer file) and paths outside the library are skipped, never stop the browser; the next save writes only what was understood.
- While the window is open it owns the keys (no search typing, browse, take or close underneath); Return / Esc / DONE close it.
- Not yet: editing several files at once, own genre names, and following a file that was renamed or moved in Finder (its line is then kept but unused).
