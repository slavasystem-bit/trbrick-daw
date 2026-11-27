## Stage N1 (LIVE-5, part 3): the SAMPLES list is built before the scene lock

Found with the XRUN hint on the Mac after part 2: the screen held the sound up to 39 ms, named BROWSERS (128-frame buffer = 2.9 ms).

- **The SAMPLES browser builds its list before the scene lock** (`SampleBrowser::Prepare`, called by `BottomPanel::DrawBrowsers` before it locks). Building walks the whole library (thousands of names, genre / tag guesses, sorting), lists `samples/` on disk and counts where each sample plays; it reads only the screen thread's own caches and names in the project, which only the screen thread changes. Before, it ran inside the lock on every change: opening the window, every typed letter in the search, every filter click, and every frame while the library was still being read.
- `Draw` only shows the list it has; a change made while drawing is built on the next frame. Opening the window forgets the previous list, so another target's files are never shown for that one frame.
- The RANDOM / KIT and LIVE rows of the bottom panel hold the lock under their own name, **RANDOM / LIVE ROWS**, so BROWSERS now means the browser windows only.
- 2 new `t8_selftest` source checks (Prepare before the lock; no rebuild inside Draw). `t8_uitest --seeds 12`: scene lock misses 0.
