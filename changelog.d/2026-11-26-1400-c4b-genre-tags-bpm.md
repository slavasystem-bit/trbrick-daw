## Stage C4b (part 6): GENRE, TAGS and BPM in the SAMPLES browser

ROADMAP "Samples in one place" (C4b), item 4 (guessed part); approved mockup `docs/mockups/v45_samples_unified.png`, section 2.

- **Guessed genre and tags** (`src/core/SampleTags.h`, pure, tested): from the words of the folders and file name, camelCase and `_` split ("BL_HipHop_95bpm" = HIP HOP, "DnB" = DNB). Genres use the STYLES words: HOUSE TECHNO ELECTRO BREAKS HIP HOP DISCO DNB MINIMAL GARAGE; the first one named in the path wins. Tags: DUSTY (also vinyl, dust, crackle), PUNCHY, DARK, LO-FI, LIVE. Guessed once per file (cached).
- **Filters** under TYPE: GENRE (one), TAGS (all chosen must be on the file), BPM from - to (the program's number field; "any" = open; files without a tempo hide while a tempo is set). They add up with TYPE, the folder and the search; the search also finds genre and tag words.
- **Column GENRE · TAGS** (sortable) in the library and the collections; the selected file shows them too.
- Own corrections of genre / tags (a file beside the library) and performance sets come in later parts; the samples and folders are never changed.
=======
