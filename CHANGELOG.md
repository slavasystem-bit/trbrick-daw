# CHANGELOG

Newest first. Older entries (v22-v24) were moved here from the README.

## Stage N1 (part 2): recordings go straight to disk

Audit v44.58 LIVE-1, LIVE-3, LIVE-7, part of TEST-4.

- **`WavStreamWriter` (new, `src/core/WavStream.*`):** 16-bit PCM WAV appended while it is recorded; the header sizes are refreshed about once a second, so a crash leaves a playable file up to the last second. Stops at the 32-bit WAV limit (4 GB) and says so.
- **Recorder (REC):** the drain thread writes into the file; the take no longer grows in RAM. STOP only flips a flag and returns ("SAVING RECORDING..."); the drain thread closes the file and the "SAVED ..." message arrives through `Recorder::TakeResult()` (polled by the main loop, which also sets the last-output path on the UI thread). Before, STOP wrote the whole WAV on the UI thread inside the header panel's scene lock: about 0.25 s of silence per 10 minutes recorded (LIVE-1).
- **LIVE-3 race fixed:** only the drain thread reads the ring and touches the file; the old final drain on the UI thread ran concurrently with the drain thread on the same vector. The ring is 4 seconds deep (was 1 s at 44.1 kHz, less at 48 kHz) and frames dropped on overflow are counted and reported.
- **MultitrackRecorder (Record Stems):** same design, eight WAV files in `Recordings/<project> <date> stems/` (new `userdirs::RecordingFolder`) instead of a ZIP built on stop (a ZIP could not exceed 4 GB; a one-hour set in stems is about 5 GB). All stems take the same number of frames per pass, so the files stay exactly the same length. A take with no sound leaves no folder.
- **Quit during a take:** `Recorder::Shutdown()` / `MultitrackRecorder::Shutdown()` (called after the audio device is closed) finish the file instead of dropping the take.
- Selftest (TEST-4): every pushed frame lands in the file, exact sample values, the file is on disk while recording, STOP costs the UI thread < 5 ms, twelve quick REC on/off with the audio thread pushing, the WAV-limit stop, quit during a take, stems same length from the first sound, an empty stems take leaves nothing.

## v44.59 (stage Н2): nothing gets lost when saving and opening

Audit v44.58 items DATA-1, DATA-2, DATA-3, DATA-4, DATA-5 (project files), DATA-7 and LIVE-4.

- **DATA-1, a file from a newer version.** `kProjectFormatVersion` is 8 (keys added in v44.51-v44.58 never raised it). Loading a file with a higher format sets `LastLoadNewerFormat()`; the app keeps it in `AppState::docNewerFormat`, says it in a dialog, and Cmd+S opens "Save a copy" instead of writing over the file (Rename only moves the file, never rewrites it). New guard: `tools/project_format_keys.txt` lists every key the writer can write for the current format; selftest compares it with `Persistence.cpp` and fails when a key is added without raising the format.
- **DATA-2, a failed open is visible.** Startup and Open show a dialog (`docNoticeTitle / docNoticeText`) for: could not be opened, opened from `.bak`, newer version, repaired values. A startup load that fails continues with a new Untitled project (before: an empty project kept the old path, so Cmd+S wrote nothing over it). `RESTORE NEWEST COPY` (`AppState::RestoreFailedFromBackup`) brings back the newest dated copy as the project file.
- **DATA-3, damaged numbers.** A sanity pass after parsing (`Sanitizer` in `Persistence.cpp`) forces every index, length, track type / slot and DSP value into range and replaces NaN / inf; the count is reported (`LastLoadFixedValues()`). A healthy file is never changed (selftest: the example project needs 0 repairs).
- **DATA-4, backups.** Names are `<stem>~<folder code>-YYYYMMDD-HHMMSS.t8proj` (8 hex digits of the folder's path), so equal names in different folders do not prune each other. No copy when the file equals the newest copy; two copies in one second take the next second. Kept: newest 10 plus the newest copy of each of 14 older days. Older backup names are still listed, never pruned. The Versions list shows `BackupLabel()` (date and time).
- **DATA-5, write safety.** `FileSafety.h`: `F_FULLFSYNC` on macOS (fsync elsewhere) and a folder fsync after rename. `SaveProject` keeps `.bak` as a hard link (copy fallback), so the project path never disappears during a save. `.bak` recovery and backups use `WriteFileAtomic` (tmp + flush + rename). `RestoreBackupFile` checks every write and stays Edited when it fails.
- **DATA-7, the starter brick.** The first-start brick is `CopySceneToBrickSlot(0)` instead of pointing at LIVE scene 1. On load, any chain step pointing at a LIVE scene (old starter projects, format < 5 files) gets a copy in a free brick slot (generalises the format < 5 migration; the copy follows the track sound like a thrown brick).
- **LIVE-4, the "Edited" check and recovery.** `DocHash`, `WriteRecovery`, `SaveDocument`, `SaveProjectAs` and restores copy the project under the scene lock and serialise / write outside it (`SaveSnapshot`). Measured in the cloud on the example project: lock held 0.33 ms for the copy instead of 0.94 ms for the text plus the file write and fsync. While Edited, the check runs every 2 s instead of every 0.5 s. `AdoptProject` now takes the lock itself.
- **LIVE-8, damaged WAV / AIFF files.** A chunk that claims more than the file holds only gets what is there (a "4 GB" header no longer allocates 4 GB and stalls for seconds); a `fmt ` chunk shorter than 16 bytes is refused (it was read past its end, ASan); channel counts above 64 and sample rates outside 1-768 kHz are refused (a 1 Hz rate made the resampler produce 44100x the data); audio data above 512 MB is refused; only whole frames are kept.
- **DATA-5, WAV writes.** `WriteWavPCM16` / `WriteWavPCM24` (the set recording, and the `t8_render` / `t8_kitgen` tools) write `<path>.tmp`, check every write, flush to the disk and rename; a failed write returns false. (The stems ZIP is written by `MultitrackRecorder`, left to the audio stage.)
- **DATA-6, LINK with Undo and the SCENES row gestures (mockup approved by the author, 25 Nov 2026).** The EDIT and LINK modes and the PADS label are gone. The SCENES row has `CLICK: LAUNCH | OPEN` (`AppState::clickOpens`, saved as `clickopen=` in ui_settings.cfg), `GO nn` (lit while a scene is open apart) and a LINK action button. Shift+click on a pad selects a range, Cmd+click adds one (`SelectScenePad`, `sceneSel`); `LINK 01-03` links the selection (`LinkSelectedScenes`; mute / solo from the open scene when it is selected, else the first); with nothing selected and the open scene linked the button is `UNLINK nn`. A double click always opens without launching (`OpenScenePad`: a launch the first click queued on BEAT / BAR is cancelled; with LAUNCH = NOW the first click has already launched). The release that ends a double click is ignored (`ScenePadGesture`: ImGui's Button fires on both releases; found in review). A launch click while a scene is open apart sends the editor back to the playing scene first (`LaunchScenePad`). Return is unchanged (play from brick). New undo kind `UndoEntry::Kind::Link`: the groups and the mute / solo of all 16 LIVE scenes, applicable whatever scene is open; used by the LINK button, UNLINK, and the right-click menu. Mute toggles are not undo steps, so undoing a LINK also puts back mutes changed after it.
- **TEST-3, fuzz.** Seven new ops (65-71): LINK on selected pads / UNLINK / menu toggle with undo and redo (after LINK every selected scene is in the reference's group with its mute / solo; undoing a LINK step restores the groups exactly); OPEN / GO / launch click while held apart / the CLICK switch (opening never moves the player or leaves the opened scene queued); the armed scene (a switch on the coming scene never changes the playing one); PITCH fader / steps / push / range, and every render now goes through `VariSpeed::Process` like the audio callback; riser, live break, repeater; master FX values; reopening a damaged copy of the project (`LoadProjectFile`, repaired or refused). The fuzz now keeps its user folders in `selftest_tmp_fuzz/`. Not covered here: MIX LAB / CHORD LAB and recording (recording belongs to the audio stage). With `--threads` the invariants are now read under the scene lock (TSan found the harness reading them after unlock). The `octaves v44.3` selftest no longer fails when one random part comes out empty (flaky).
- Tests: 42 new selftest checks (`n2 ...`) and 7 for damaged audio files (`live-8:`, including 400 randomly damaged WAVs), 12 for LINK undo and the pad gestures (`link v44.59:`, `pads v44.59:`); updated: format number literals, the v44.5 chain persistence check (its LIVE-slot step is now a brick copy), the u8a backup name.

## Stage N1 (part 1): measure audio dropouts

First step of "the sound never stops" (audit v44.58, LIVE-5 / TEST-6): measure before fixing.

- **DSP / XRUN readout moved to the tab row**, right-aligned left of PITCH (`HeaderPanel::DrawDropouts`). At the end of the header row it was hidden whenever the window was 1440 px or narrower. It is always shown now: grey `XRUN 0`, red `XRUN 3 (250 ms)` with the longest dropout.
- **Who held the sound:** the audio thread measures how long it waited for the scene lock in every callback (`Mixer::audioLockWaitUs`), and `SceneMutex` measures how long any other thread held it (`MaxHoldUs`, the audio thread's own holds are not counted). The info-line text of the readout shows the longest dropout, how much of it was spent waiting for the screen, the longest screen hold overall, and the last 5 dropouts (time ago, length, wait). A click resets all of it.
- Dropout log: the callback keeps the last 8 dropouts (`Mixer::dropouts`, length and lock wait), no allocation on the audio thread.
- Selftest: the lock wait and the screen hold are measured, the audio thread's own render is not counted, the reset clears the log.

## Stage N0: safety net for merges

Nothing gets played or merged without green tests (audit v44.58: TEST-1, TEST-2, part of CODE-5 and TEST-7).

- **CI (`.github/workflows/build.yml`):** four jobs on every pull request and on main: Linux build + `t8_selftest` + `t8_fuzz --seeds 30` (and a check that the tests left no files in the repo); ASan + UBSan build running selftest and `t8_fuzz --seeds 10`; TSan build running `t8_fuzz --threads`; macOS build of everything including the app + selftest.
- **Launcher gate (`tools/mac/launch.sh`):** branch protection is not available on a free private repo, so the TR-Brick button is the gate. The PR menu shows each PR's check state; a PR starts only when all its checks are green (red, running or not started = refused, with Enter to start main instead). The commit that is fetched must be the one whose checks were read. After a build that changed the installed version the launcher builds and runs `t8_selftest` on the Mac; a failing PR is not started, a failing main (self-check, configure or build) falls back to the last main build that passed (`~/TR-Brick/last-good`; a PR never becomes the fallback). The self-check runner is built into the launcher, so PRs made before this one are checked too. Developer switches: `TRB_ALLOW_UNCHECKED=1`, `TRB_SKIP_SELFTEST=1`.
- **TEST-2:** the selftest "a long note across a chain step is released" had its render loop inside a comment and always passed; the loop is back (and the check was seen failing when the render stops too early).
- **Warnings:** `-Wall -Wextra` on all first-party targets (imgui moved into its own `t8imgui` library, so third_party stays quiet). `T8_WERROR=ON` (used by CI) makes unused variables / values and misleading indentation errors in `tools/`. The remaining warnings in `src/` are listed for a later step. `T8_SANITIZE=address,undefined|thread` builds the checking variants.
- **Tests run in a scratch folder:** `tools/run_tests.sh <build> selftest|fuzz [args]` runs them in a temp folder with symlinks to the repo, so `selftest_*` files no longer land in the repo root.

## Launcher: straight from GitHub

No more zips: the TR-Brick button on the Desktop takes the program from the GitHub repository.

- **What it does:** each start asks GitHub for the main version and downloads it into `~/TR-Brick/repo` (a clean copy); if it differs from what is installed, it is copied into `~/TR-Brick/app` exactly as before (your project file, `projects/`, `backups/` and own kits/presets are never overwritten), then only what changed is built and the program starts.
- **Testing a pull request:** when pull requests are open, a short menu lists them; typing a number starts that version, Enter (or 15 seconds) starts main. The next start with Enter goes back to main.
- **No internet / not signed in:** a clear message, then the installed version starts (safe on stage).
- **Self-update:** the launcher replaces itself only from the main version and only with another GitHub launcher (marker `TRB_LAUNCHER_GITHUB=1`), so testing an older pull request cannot bring back the zip launcher.
- **Sign-in:** through the GitHub CLI (`gh auth login`, once); git uses `gh auth git-credential`, so no passwords or tokens are stored by the script. One-time setup in Ukrainian: `docs/MAC_SETUP.md`.
- Test switches: `TRB_GIT_URL`, `TRB_CHOICE`, `TRB_OFFLINE`, `TRB_DESKTOP` (plus the old ones). The zip path (`~/Downloads/trbrick-daw-source-*.zip`) is gone; `TRB_SOURCE_DIR` and `Install TR-Brick.command` still work.

## v44.58

Playing live between scenes: LINK (scenes that share mute and solo), EDIT (open a scene without launching it) and the switches that follow a waiting launch.

- **LINK.** Scenes can be tied into a group; the group shares mute and solo. A change in one scene is made in all of them, and moving from one to another leaves the mixer exactly as it was (the sequencer carries the flags over on arrival, so flags that drifted apart cannot be heard as a jump). Scenes that are not linked keep their own mute and solo, as before: pick unlinked scenes for a hard change. Shift+click on a pad joins / leaves the group of the edited scene; the LINK button (PADS row under the scenes) makes a plain click do the same; the right-click menu of a pad has "Link with the edited scene" / "Leave its link group". Linked pads share a coloured bar along the top that runs through the gap; the row shows `LINKED 01+02+03: SAME MUTE / SOLO`. Joining a group takes the mute / solo of the scene it joins (the ones you hear). Groups are saved with the project (`sceneLink=` in the file; older files read as no links) and are kept by position (they do not follow a swapped scene's content). Not undone by Undo.
- **EDIT.** The EDIT button: while the music plays, a click on a pad only opens that scene in the editor (amber outline); the scene that plays keeps playing (teal triangle). Correct it - notes, sound, mutes - nothing is heard. GO (or a double click on the pad) launches it with the usual LAUNCH quantize; at the boundary the editor and the player are one again. The row says `EDITING 10 - PLAYING 07 - GO launches 10`. EDIT off: a click launches, as before. Stopped, or in song mode, a click opens the scene as it always did.
- **The coming scene.** While a scene launch waits (you clicked a pad with LAUNCH on BEAT / BAR), the mute / solo / group buttons of the LIVE panel and NEXT BAR's queue mean the scene that is COMING, not the one that plays: click the pad, then the kick, and the kick is muted from the first beat of the new scene, while the playing scene is not touched. The panel says `NEXT: SCENE 12`. The change is a real edit of that scene (it stays if the launch is cancelled). Not tied to NEXT BAR: it holds whenever a launch is waiting; with the scenes linked it simply is the same change.
- **How:** `Project::sceneLink`, `AppState::LinkScenes / UnlinkScene / ToggleSceneLink`, `SetMuteScene` / `SetSoloScene` write to the whole group, `Sequencer::CarryLinkedFlags` (in `ApplyScene`); EDIT is the existing play pin used for the TRACK tab (`PinPlayToPlaying`, released when the edited scene is launched); the coming scene is `AppState::ArmScope`, which for the length of one draw / click makes the edited scene the waiting one and keeps the player where it is.
- Tests: 19 checks (`link:` / `edit:` / `arm:`): the group writes and the mixer, carry-over on arrival, an unlinked scene keeps its own, toggling and the group of one, save and load, EDIT keeps the player and the mixer apart and GO starts it on the bar line, the coming scene is muted without touching the playing one and is silent from its first beat.

## v44.57

PITCH: a turntable pitch fader for the whole output, to play the project along with vinyl.

- **What it does:** everything plays faster or slower, tempo and pitch together, like a record on a Technics deck: drums, synths, loops, samples, delay and reverb tails. +2.3% turns 120 BPM into 122.8 BPM and raises everything by 0.4 semitone. Range +-8% or +-16% (the 8% / 16% switch), step 0.1%. Like a deck the fader starts at 0 each time the program starts (only the range is remembered, `pitchrange=` in ui_settings.cfg); it is not in the project file.
- **Where:** a PITCH button at the right end of the tab row (shows the value, e.g. `+2.3%`). Click it or press **P**: the strip opens with the fader, the range switch and the real tempo (`= 122.8 BPM`).
- **The D-pad:** while PITCH is on the arrow keys are the fader (they are taken for the moment, so the grid, the piano roll and lists do not move): up / down faster / slower by 0.1% (Shift 1%, held keys repeat), left / right = a push of 4% while held (pitch bend, back when released), Return = 0%, R = range, P or Esc = leave. Cmd + arrows (scene / instrument) are not taken. The fader can also be dragged (double click or Option + click = 0%; a small detent at the centre).
- **How:** `core/VariSpeed.h` sits between the engine and the sound card (the engine keeps its own clock; band-limited sinc interpolation, a fader with about 20 ms of weight, no allocation or lock on the audio thread). At speed 1.0 it is a bit for bit copy and adds no delay; coming back to 0% lands on whole frames again, so the unfiltered copy resumes. Recording (REC) takes what is heard, so it carries the pitch; offline exports and MIDI capture are not affected.
- Tests: 21 checks (`pitch: ...`): bit for bit at 1.0 in blocks of any size, the engine asked for exactly what is played, the pitch of a test tone at +5 / -5 / +16 / -16% to within 1.5 Hz with no click, the same result for any block size, a bit for bit return to 0%, the range / step / push arithmetic, and the real sequencer through the stage. Not tried: with real vinyl and a real mixer - the beat matching by ear is up to the player.

## v44.56.2

A helper for protected Ableton pack samples in bulk: `tools/ableton_slice.py` (also placed in the user's Samples folder). No change to the program itself.

- **The way:** drop a whole folder of samples on ONE audio track in Live (Warp off), export that track (File > Export Audio/Video, Normalize off, same sample rate, no dither) and run the script on the folder. It cuts the long WAV back into one WAV per sample, named like the original, and the library lists them after Rescan Samples.
- **How it knows where to cut, without opening the protected data:** the frame count and sample rate are in the AIFF header, which is not protected. Live puts each sample on the next bar line after the previous one (found on a real export of 63 `able` files: starts on a 2 s grid at 120 BPM, one whole export matching the prediction to the frame, 15,480,621 frames). The script tries the tempo and the file order and keeps the reading in which every gap between samples is digital silence. Live orders the dropped files in natural order (`bpm2` before `bpm10`): the length alone cannot tell the order (each sample takes a whole number of bars wherever it stands) and can fit a wrong tempo by chance (192 BPM fitted that export too), so the audio decides. If nothing fits, or two readings fit equally, nothing is written.
- Options: `--export`, `--bpm`, `--out`, `--dry-run`, `--force`, `--locked-only`, `--stash-export`. Python 3, standard library only. Cuts are byte for byte (tested on generated files in both orders: identical to the sources); on the real folder only the dry run has been made.

## v44.56.1

Protected Ableton pack samples are recognised and explained instead of failing silently. Found with a real library: of 1412 `.aif` files from Ableton Live packs (Sample Magic, Solid Sounds ...), 877 are AIFF-C with the compression type `able` ("Ableton Content"). Their audio is protected (the data is scrambled; ffmpeg, QuickTime and VLC cannot read it either), so only Live can open them; the other 520 (the Ableton multisamples: AIFF, `NONE`, `sowt`) read bit for bit like the same files converted by ffmpeg.

- The library marks them `[LOCKED]` (and tags them LOCKED), the rescan says how many ("LIBRARY: 1442 SAMPLES (877 PROTECTED ABLETON FILES: ONLY LIVE CAN READ THEM)"), and choosing / hearing one says why in the info line: export it from Live (as WAV or AIFF) first. The same for a dropped file and for a transition. Compressed AIFF-C (ima4, MACE ...) gets its own short message.
- `UnreadableReason()` and `IsProtectedAbletonAiff()` in `core/WavFile`; `LibSample::locked`. The program does not try to open the protected data.
- Tests: 4 more checks (a fake `able` file is refused, told apart from plain AIFF-C, marked in the library, explained in the info line).

## v44.56

AIFF: the program reads `.aif`, `.aiff` and `.aifc` samples and loops everywhere it read `.wav` before.

- **One reader for both** (`core/WavFile.cpp`, `LoadWav`): the kind of file is found from its header (`FORM` + `AIFF` / `AIFC` or `RIFF` + `WAVE`), not from its name, so a misnamed file still opens. AIFF: big-endian PCM of 8 / 12 / 16 / 24 / 32 bit (a size that is not a whole number of bytes is left-justified, as the standard says; 8-bit is signed here, unlike WAV), the 80-bit sample rate, extra chunks (APPL, MARK, INST ...) skipped with their padding, a non-zero SSND offset, a file cut short plays what is there. AIFF-C: `NONE`, `twos`, Apple's little-endian `sowt`, `in24` / `in32`, `fl32` / `fl64` float, and `ulaw` / `alaw`. Compressed variants (ima4, MACE) are refused with a clear message. More than two channels: the front pair, as for WAV. Sample rates are converted like WAV (windowed sinc).
- **Everywhere a sample is chosen:** the sampler and the SAMPLE engine of the synths and the chord (list of `samples/`, file window, drop on the window), the sample library (`.aif` / `.aiff` / `.aifc` listed, tempo read from the name: `Funk Break 95 bpm.aif` is a LOOP of 95 BPM), library audition, HEAR. A file chosen for the sampler is copied into `samples/` as it is, with its own extension (SAVE AS copies it with the project).
- **Where the project needs real WAV files** (a drum track's sample, which lives in a kit; a transition before a brick): an AIFF is converted when it is copied in (`ImportAudioAsWav`: decoded at its own sample rate and written as 24-bit PCM). The original is not touched. A `Kits/<name>/` folder of the library still needs `.wav` names (kick.wav, snare.wav ...).
- The native file windows (macOS, zenity, kdialog) offer WAV and AIFF.
- Not read: the loop points and tempo that Apple Loops / samplers store inside an AIFF (the tempo still comes from the file name, as for WAV), and the marker of a file.
- Tests: 25 selftest checks (`aiff: ...`) with generated files of every kind, damaged files (300 random breakages) and the library / sampler plumbing; files written by ffmpeg (16 / 24 / 32 bit, 8 bit, sowt, fl32, a-law, mu-law) were compared by hand with the same audio as WAV: identical (8-bit within its own rounding).

## v44.55

EUCLID can now spread a melody. Before, EUCLID wrote one note (a copy of the first) on every hit and erased the track; now the notes of the track stay and are placed on the hits in turn, and turning it off brings the original back.

- **SPREAD MELODY** (a button in the EUCLID window, for drum and synth tracks; lit while on, the EUCLID button shows `EUCLID 5/16`). The active steps of the track are kept (`Track::euclidOrig`, with their step index) and the steps of the track are derived from them: note 1 on hit 1, note 2 on hit 2 ... and round again when there are more hits than notes (4 notes on 5 hits drift against the figure every pass). Slide and nudge of a note do not travel to its new place; everything else (note, length, accent, ghost, velocity, probability, ratchet, drum variant) does.
- **Live:** HITS / STEPS / ROTATE and the presets re-derive from the kept melody at once; a run of changes is one undo step. A drum track spreads its variants the same way.
- **OFF puts the original back exactly.** If you edited steps by hand while it was on (or something else rewrote the track), OFF keeps what is there and says so: nothing is lost. An empty track gets the default hit and is empty again after OFF. Not for the chord track.
- **APPLY ON THIS TRACK** is unchanged (the plain overwrite with one repeated note) and ends the kept state.
- The state is saved in the project (`euclid=` and `eorigN=` lines, only when on: older files are byte for byte the same), travels with COPY INSTRUMENT and undo, and is not part of a track's sound.
- The cycle restarts at the loop point (no drift from one pass of the track to the next).
- Tests: 14 selftest checks (`euclid keep: ...`) and an EuclidKeep fuzz op.

## v44.54

NEXT BAR: the instrument switches of the LIVE panel can queue to the next bar line, like a fill. A button next to the LIVE title chooses between real time (the default, as before) and "from the next bar".

- **NEXT BAR button** (LIVE panel, next to the title; lit = on; remembered in `ui_settings.cfg` as `nextbar=`). On: a click on an instrument bar (mute), an S (solo) or a group button (ALL DRUMS, DRUMS, HATS, DROP, MUTE ALL, DROP+CHORD) waits and takes effect exactly on the next bar line, the same moment a FILL starts. Off: it acts at once.
- **A waiting button pulses** with the white outline of a pending STUTTER; a second click on it cancels it. Several clicks can wait together (up to 64) and are applied in the order of the clicks.
- **Applied inside the sequencer** (`Sequencer::SetBarHook` / `ArmBarHook`, called from `FireTick` after a scene launch on the same line): sample-accurate, not up to a frame late. The queue (`AppState::queuedSwitches_`) is reserved once, so the audio thread never allocates; while it is applied `PushUndo`, `PushSoundUndo` and `SetStatus` do nothing (they are not audio-thread safe) and the undo step is pushed at the click.
- **Rules:** stopped, or editing a scene other than the one that plays, or song mode: a click acts at once (no bar to wait for). Stopping the transport applies whatever waits. A click queued on a scene that is left before the bar line is dropped. Not queued on purpose: STUTTER, FILL / RISER, scene launches (they keep LAUNCH NOW / BEAT / BAR) and WHO PLAYS on TRACK.
- Tests: 12 selftest checks (`next bar: ...`) and a NextBar fuzz op (also with the audio thread).

## v44.53

A fourth synth engine: SAMPLE. Any synth (Bass, Synth2, Synth3) and the chord track can play a file of the project instead of an oscillator, at the pitch of the note. Audio of every existing project unchanged (audit identical; fuzz 30 seeds + 8 with the audio thread: 0 failures; new fields are written only when used, older files stay byte for byte the same).

- **ENGINE > SAMPLE** (`core/SampleSynth.h`, the fourth entry of `SynthEngineType`; click the engine bar to cycle). It is built from the same parts as the SAMPLER track (`SamplerVoice`): ONE-SHOT plays the sample at the pitch of the note (ROOT plays it unchanged), SLICES (the note picks the slice: ROOT = the first; HITS marks, FIND, SENS and LAY OUT work as on the sampler), LOOP (a note starts the loop from its beginning, fitted to whole bars at the project tempo, it runs while the note is held, key-synced). A new note always restarts the sample from its beginning.
- **The sound is the synth's:** ADSR amplitude envelope (a held note holds the sample, NOTE OFF starts RELEASE), low-pass filter with its envelope, keytrack, LFO (pitch / cutoff / volume), GLIDE on slid notes (the sample keeps playing, only its pitch slides; not on SLICES), VOLUME, UNISON (detuned copies of the sample), SUB, NOISE, DRIVE, channel EQ / FX / AUTO. The sample's own ATTACK is gone (the synth's ATTACK does that). Switching an engine to SAMPLE opens the filter (cutoff 18 kHz, ENV AMT 0, with the filter wide open the sample passes untouched), sustain 100 %, release at least 250 ms.
- **The chord track** plays one sample voice per chord tone: a chord is the sample at several pitches at once.
- **One sample per scene and per synth**, like the sampler: the file name is part of the synth patch (`SynthSample`, plain data, 95 characters of name; the audio thread never allocates). Scenes, bricks, THROW, TAKE SOUND, copying sounds and SAVE AS (copies the samples) carry it with the patch. The engine hands the samples over at scene changes the same way the sampler's (`Sequencer::SetSceneSynthSamples`, `AudioEngine::SynthSampleSet`).
- **Editor:** the OSC tab becomes the SAMPLE tab: PLAY (MODE, ROOT, SLICES, FIND, SENS, LAY OUT, FIT TO), SAMPLE (START, LENGTH), TUNE / LEVEL (FINE, GAIN, DIRECT.) are the same cells as on the sampler (one code path: `DrawSamplerVoice` works on a "sample target": the sampler or a synth). Under the engine bar: the sample picker (< name >, HEAR = the note ROOT through the synth, MINE = the library) and the waveform card with slices and marks. A WAV dropped on the window goes to the selected synth when it is on SAMPLE. LAY OUT on a synth holds each slice until the next one (a synth releases at the end of its note); it is not offered on the chord track.
- **Files:** `sample=file|root|start|0|length|0|reverse[|fine|gain|mode|slices|fit[|marks]]` under the synth / chord patch (same text form as `sampler=`, now in `core/SamplerText.h`), `engineType=SMP`; custom synth presets carry it too (a preset used in another project shows the sample as missing, in red, until the file is there).
- Tests: 43 new checks (pitch, envelope, retrigger, SLICES, key-synced LOOP, filter, VOLUME, chord, unison, save / load, presets, scene hand-over, drop, undo, marks, LAY OUT); the fuzz has a SynthSample op (engine switches, absurd and missing names, modes, marks, layout, steps). Also fixed: the hint of a missing sample on the sampler card printed a `%s` without an argument.
- Known limits: a LOOP pitched by a note changes its tempo too (varispeed, no time-stretch); ENGINE is cycled by a click (no list); the HEAR button plays one note, a missing file is silence (no error sound); no multi-sampler (one file per synth and scene); Bass and Synth2 stay monophonic. Planned, only on the author's word: Synth3 polyphonic like the chord track, TUNE / DECAY automation for samples.

## v44.52.1

Follow-up to v44.52 after the first slices session: loops that play, slices that follow the hits, loops in the library. Audio of every existing project unchanged (audit identical, fuzz unchanged; new fields are written only when used).

- **LAY OUT > STEPS** (SLICES mode): writes the slice notes (ROOT, ROOT+1 ...) on the sampler's steps, each where its slice starts in the loop at the project tempo, and gives the track the loop's length (up to 64 steps). Undo brings the old steps back. (`AppState::LayoutSamplerSlices`.) With equal slices the notes sit evenly; with HITS they sit where the hits really are, so a break keeps its groove.
- **HITS slices:** the SLICES cell has a fifth value, HITS. FIND > HITS finds the hits of the sample (`core/Onsets.h`: energy-rise detection, no tempo assumed; SENS 0..100 %, at most 32 marks) and puts a numbered mark on each; on the waveform a click adds a mark and a right-click removes the nearest one. Slice i plays from mark i to mark i + 1; the played part (START / LENGTH) limits the marks that count. Saved as `|marks` at the end of the `sampler=` line (only when there are marks).
- **LOOP seam:** a 2 ms crossfade at the wrap (no click on a loop that does not close perfectly). The speed is corrected by the overlap, so the downbeat still comes back every bar to the sample (tested: exactly 1.000 s at 240 BPM). Side effect: a hit right at the start of the loop keeps about 70 % of its peak (the attack is softened over 2 ms).
- **Loops in the library:** a tempo in the file name ("170 bpm", `core/TempoName.h`) makes the file a LOOP and shows "170 BPM" in the list; HEAR plays it at the project tempo (varispeed, like the sampler); choosing it for the SAMPLER sets MODE LOOP with AUTO tempo fit.
- **AUTO tempo fit** reads the tempo from the file name (see v44.52 follow-up above) and the FIT TO cell shows the result (AUTO 4B).
- The audition in the library is quieter (gain 0.4, was 0.85). Choosing a sample (library or the sampler's list) stops the audition and what the sampler still played (HEAR, a long hit).
- Tests: seam crossfade (no click, tempo exact), layout (equal slices, 4-bar 16 slices, outside SLICES), onset detection (six bursts within 10 ms, a steady tone none, the strongest N), marks (slice boundaries, save / load), tempo in names, loop category. selftest, fuzz: all pass; audit identical.
- Not checked here: sound through a real audio device; FIND on real music (only synthetic bursts and a click track were measured); the native file windows.

## v44.52

One sample editor on every instrument that plays a sample (drum tracks and the SAMPLER), plus SLICES and LOOP on the sampler. Audio of every existing project unchanged (audit identical to v44.51, fuzz unchanged, old files byte-compatible: new fields are written only when used).

- **One SAMPLE card, same knobs, same words** (`ContextPanel::DrawWaveCard`, `DrawSampleEditBlocks`): START, LENGTH, ATTACK, FINE (cents), GAIN (dB), DIRECT. (FORWARD / REVERSE). On a drum track they edit `Track::edit` (`SampleEdit`, per scene); on the sampler they edit the same values in `SamplerPatch`. Drum tracks also get the waveform card (under the KIT block, from the scene's kit) with START / LENGTH lit. TUNE and DECAY stay in the drum VOICE block; the sampler's pitch is ROOT + the step's note (+ FINE).
- **Drum engine** applies the edit when a hit starts (`DrumEngine::Trigger`: start / end frame, reverse, gain, fine cents, attack in place of the fixed 0.6 ms fade-in; `RenderSlot` stops at the end with the usual click-free fade). The default edit changes nothing (tested bit-exact against the old path).
- **SAMPLER MODE:** ONE-SHOT (as before), SLICES, LOOP. `SamplerPatch::mode / slices / fitBars`.
  - **SLICES:** the part that plays (START / LENGTH) is cut into 4 / 8 / 16 / 32 equal parts; the step's note is the slice number (ROOT = the first). No transpose in this mode. Numbered slice marks on the waveform. No transient detection (equal parts only).
  - **LOOP:** plays by itself with the scene, wraps at the end of the played part, restarts with the transport and when the scene's sample or the mode changes; START / LENGTH / FINE / DIRECTION follow live. Steps are not used. STOP fades it out.
  - **Tempo fit (FIT TO / BARS):** OFF (native speed), AUTO (nearest of 1 / 2 / 4 / 8 / 16 bars) or 1 / 2 / 4 / 8 bars of the project tempo. Done by varispeed: the sample is sped up or slowed down and its pitch moves with it (no time-stretch). The sequencer hands the tempo to the voice (`AudioEngine::SamplerSetTempo`); `SamplerLoopRestart`, `SamplerLoopPreview` (HEAR on a LOOP plays one pass).
- **Effects, EQ, sends and automation:** the sampler already had the same EQ, FX (sends, drive, chorus, auto-pan) and AUTO lanes (pan, reverb, delay) as a drum track; they act on the loop and slices too. Not yet: TUNE / DECAY lanes for the sampler, and an LFO (drums have none either; LFO exists only on synth patches).
- Persistence: `sedit=start|length|attack|fine|gain|rev` per drum track and an extended `sampler=` line (`|tuneCents|gainDb|mode|slices|fitBars`), both written only when not default.
- **AUTO fit fix (found by a user's 4-bar 170 BPM loop at 120 BPM):** AUTO used to pick the nearest of 1 / 2 / 4 / 8 / 16 bars, and 170 / 120 is almost exactly the square root of 2, so it chose 2 bars and squeezed 4 bars into 2 (1.41x faster, a tritone higher). Now a tempo in the file name ("170 bpm", "90BPM", "BPM 128") gives the exact speed (project / file tempo) and the bars; the FIT TO cell shows the result ("AUTO 4B") and its hint says what was decided and how far apart slice notes must be.
- Tests: SampleEdit save / load, drum default bit-exact, START / LENGTH / GAIN / REVERSE on drums, slice mapping and length, fit to bars at two tempi, LOOP (wrap, no clicks, tempo change, fade out, notes ignored). selftest, fuzz: all pass; audit identical.
- Not checked here: sound through a real audio device; LOOP seam is a plain wrap (a loop sample should be cut on a bar); the REVERSE test on drums only checks that the sound comes from the tail.

## v44.51

"MY SAMPLES": the user's own sample library (one-shots; loops, slices and tempo fit come next, as v44.52). Audio of every existing project unchanged (audit and fuzz unchanged).

- **One library folder, sub-folders by instrument** (`core/SampleLibrary.h`): default `~/Documents/TR-Brick/Library/Samples/`, or any folder (Library > Choose Samples Folder..., or `TRB_SAMPLES_DIR`). Every `.wav` below it is listed (recursive, hidden folders skipped, 20000 files at most). The category (KICK SNARE CLAP HAT PERC BASS SYNTH FX LOOP OTHER) is guessed from the folder names (nearest first), then from the file name; a loop word anywhere makes it LOOP. The library is only read: a chosen file is COPIED into the project, so the project stays complete when the library moves.
- **MY SAMPLES browser** (the same card window as kits and beats: categories with counts, search, favourites, HEAR / Space, Enter / double-click to use). Opened from: the MINE button of a drum track's KIT block (starts on the track's own category), the sample list of the SAMPLER, and "My samples..." in the transition menus on TRACK. HEAR plays through the master bus on a voice of its own (`AudioEngine::AuditionSample`), no track is touched.
- **Drum tracks:** the pick replaces the track's sample in a kit of the scene (`kits/mykitN`, the existing drop mechanism; factory kits are never changed).
- **Own kits:** a folder `<library>/Kits/<name>/` holding `kick.wav, snare.wav, clap.wav, chh.wav, ohh.wav, perc.wav` (and `_2` variants) is copied into `kits/` at start and by Library > Rescan Samples (only what is missing), and then lives under MY KITS.
- **Own transitions (the sounds before a brick; fills use them too):** a library file chosen as a transition is copied to `<project>/samples/transitions/tNN_name.wav`; NN (01..40) is its slot and the brick stores the riser index `kFirstUser + NN - 1`, so nothing changes in the project file format and the index stays valid. The folder is read when the project opens, travels with SAVE AS, and `t8_render` reads it too. The same file chosen again keeps its slot; a random riser never picks an empty slot.
- **SAMPLER:** the sample list has "MY SAMPLES (LIBRARY)..."; new FINE (-100..100 cents) and GAIN (-24..+12 dB) cells (`SamplerPatch::tuneCents`, `gainDb`; written to the project only when used, so older files stay byte for byte the same).
- Library menu: Rescan Samples, Choose Samples Folder..., Show Samples Folder in Finder. The native folder window uses the same `FilePicker` (osascript / zenity / kdialog).
- Tests: classification and scan, kit sync, audition (heard, stopped), own transitions (slot, name, reopening, SAVE AS, sound in the chain), sampler from the library, FINE / GAIN (save / load, dB scaling). selftest, fuzz, audit: all pass.
- Not checked here: the native folder window (no zenity / osascript on this machine), and sound through a real audio device. Loops in the library are listed (LOOP) and play as plain one-shots; no tempo fit yet.

## v44.50.1

Follow-up to v44.50 after the first hands-on session (tabs, automation, CHORD LAB). Audio of every existing project unchanged. Code comments call these changes "v44.51" (their working label); SLICES + LOOP remain the real v44.51.

- **LIVE and TRACK no longer start each other.** Switching the tab never changes what plays. `Sequencer::PinPlayToCurrent()` / `ReleasePin()` / `IsPinned()`: when TRACK is opened while the jam plays, the jam keeps playing and the editor shows the brick (its scene is edited without the loop jumping); the pin ends on Stop, in song mode and on PLAY HERE. Going to LIVE while the track plays leaves the track playing.
- **What plays is shown in words.** A dot on the tab that plays (LIVE / TRACK) and a trailing tab-bar button "PLAYING: LIVE 01" / "PLAYING: TRACK 2/3". When the tab on screen is not the one that plays, the button reads "... - CLICK: PLAY LIVE" (or TRACK) and `AppState::PlayHere()` moves the music here. Merging the two tabs is a later decision.
- **DROP+CHORD** replaces ALL ON in WHO PLAYS and LIVE: keeps kick, bass and chord, mutes the rest; a second press restores the previous mutes (`SwKeepOn`, `SwToggleKeep`).
- **CHORD LAB opens on TRACK too** (menu View / Esc / button), with the strip of bricks above it; buttons "BRICK n PLAYS THIS JAM" and "CLOSE" instead of THROW ONTO THE TRACK.
- **Clicking a brick only selects it** (it used to jump the loop to that brick); a running loop is not interrupted, so a brick that is not playing can be edited.
- **Automation:** a MIX / instrument lane point dragged to the bottom now really means "off" (the old mapping left it near -30 dB: 0..-36 dB over the upper 80 % of the row, -36..-48 over the next 10 %, the last tenth is off). The instrument LEVEL lane is the second row (before FILTER) so a short window still shows it. "+ transition" is always drawn as a quiet ghost on bricks 2.., and a missing riser sound now says so in the info line. Checked by rendering: a MIX lane at off gives -120 dB for the whole brick (new selftest). Automation applies only while the TRACK plays (song mode), not while a jam plays on LIVE.
- Tests: pin mechanism, MIX lane at off silent; three older checks updated to the new tab behaviour. selftest, fuzz: all pass.
- Not checked here: the native file window (no zenity / osascript on this machine), a really short window on a real screen, and sound through a real audio device.

## v44.50

"SAMPLER": step 4 of stage S, first release (the 11th instrument, one-shot). Audio of every existing project unchanged (audit unchanged, 12 rounding lines).

- **The sampler is the 11th track, after the chord** (index 10; indices 0-9 are exactly what they were). It is a full instrument everywhere the other ten are: a row in the sequencer (M / S, steps, ratchets, probability, piano roll; a new step is C4 and so is the default ROOT), the mixer (fader, pan, sends, EQ, insert FX), MIX LAB, WHO PLAYS and the LIVE bars (11 bars in the width the ten had), the arrangement energy, the stems (`08_sampler.wav`), SOUND parts (a brick can carry its own sampler sample; THROW / TAKE SOUND copy it), SCENE mute masks, ARRANGE mutes. `TrackType::Sampler`, `IsNoteTrackType()` (synth or sampler: note rows, note edit).
- **One sample per scene.** `SamplerPatch`: file, ROOT (the note that plays it unchanged), START, LENGTH (how much of the rest plays, then a click-free fade), ATTACK, MONO / POLY (mono = a new hit cuts the previous one in 3 ms, poly = 4 voices), FORWARD / REVERSE. A step on another note plays it higher or lower (pitch + speed together, Catmull-Rom interpolation). The audio thread only ever sees immutable shared buffers; the cache is never freed during a session, so no sample is deleted under a playing voice.
- **Where samples live:** WAV files dropped on the window (with the sampler selected) or picked from the list are **copied into `samples/` beside the project file**; the `.t8proj` stores only the file name. The same file twice is not copied twice, a different file with the same name gets `_2`. SAVE AS copies the samples the project uses next to the new file (nothing is moved or deleted). A missing file shows a red dot, says so in the info line and plays silence; it is found again when the file comes back.
- **The SAMPLER panel** (select the row): SAMPLE card (`<` name `>`, HEAR, the list of `samples/`, the waveform with the START line and the end set by LENGTH, duration, mono / stereo), OUTPUT, tabs VOICE / EQ / FX / AUTO like a drum. AUTO has PAN, REVERB and DELAY (the sampler has no filter yet).
- **Project format 7.** Sound parts grow from 11 to 12 (the sampler is part 10, the kit moved to 11): a format 6 file opens with its brick "own sound" bit for the kit moved over; a project without track 10 gets an empty sampler. `sampler=file|root|start|attackMs|decay|poly|reverse` in each `[patternN]` block.
- **Choose a file with a click:** the SAMPLE card (and "+ ADD A .WAV FILE..." at the top of the list) opens the system's own file window (macOS: `osascript`; Linux: `zenity` or `kdialog`), run on a worker thread so the sound keeps going; the chosen WAV is copied into `samples/` exactly like a dropped one. `app/FilePicker.h`. A system without such a program says so in the info line (drop still works).
- **WAV files from DAWs now load.** The loader used to refuse WAVE_FORMAT_EXTENSIBLE (what most DAWs write for 24-bit, 32-bit and surround files) and 32-bit integer PCM; both now load, 64-bit float too, a file cut short plays what is there, and files with more than two channels keep their front pair. A file that cannot be read now says so for 6 seconds in the info line (before: 2 seconds, easy to miss).
- **TRACK page, 11 rows:** the sampler row took 20 px from the strip above (the editor under it reserves 20 px per row), so the instrument rows of the strip fell to 5-6 px. The editor now gets 18 px per row, the instrument rows of the strip are at least 12 px, and the lane rows (MIX, FILTER, ...) under them give way first (20-30 px each, fewer of them on a short window).
- Also: the MIX LAB analysis and `t8_render` read `samples/` too; fuzz got a Sampler op (assign, list, missing file, every knob, steps, mute, hand-over to the engine).
- Fixed on the way: the sampler output is always mixed (not only while a voice sounds), so a hit right after an unmute does not leak through a mute ramp that never ran.
- Selftest (25 new checks, 4 of them for WAV formats): constants and 11 tracks, assign / copy / undo, cache, audible playback, silence without a sample, an octave up plays twice as fast, mute, save / load, a format 6 file opens, old kit bit remapped, CopyPart, scene mute masks, group masks, SAVE AS copies samples. Fuzz (normal, with audio thread, ASan): 0 failures. ASan selftest: no memory errors; its one failure is the real-time budget of the thickest synth (an ASan build is several times slower; the normal build passes). Checked in the window: the row, WHO PLAYS and LIVE bars, the TRACK page with 11 rows, MIX LAB, the mixer, the sample list, the waveform, painting steps, play, save; `t8_render` of that project is audible.
- Not checked: the native file window itself (this machine has neither osascript nor zenity; the status message for a missing dialog program was seen, the dialog on the Mac was not); the TRACK page on a really short window (the test window cannot be made shorter than the screen it is on; the formula was checked at 900 points: instrument rows 11-12 px); a file dropped on the window was checked with an injected SDL drop event (a 24-bit EXTENSIBLE 48 kHz stereo file), a drop by hand was not; AIFF and MP3 (WAV only, mono or stereo, up to 3 minutes); the sampler on the Mac; sample rates other than the engine's are converted when loaded, quality of that conversion not judged by ear.
- Next release: **v44.51** SLICES (transients, even cuts, TO PATTERN) and LOOP with a simple tempo fit (speed and pitch together, no time-stretch).

## v44.49

"STYLE": step 2 of stage S of the roadmap. Audio unchanged (audit unchanged, 12 rounding lines).

- **A style is one package in one place:** the drum pattern (with its swing and typical length), the kit, a groove, the presets of bass / synth 2 / synth 3 / chord, a bass line, a chord progression and a tempo. `core/Styles.h/.cpp`; 12 styles built only from what the program already had: HOUSE, DEEP HOUSE, TECH HOUSE, UK GARAGE, TECHNO, ACID TECHNO, ELECTRO, BREAKS, JUNGLE, DRUM AND BASS, BOOM BAP, LO-FI HIP HOP. Everything is written from convention, like the patterns and presets it points at, and is a starting point, not a copy of any record.
- **`AppState::ApplyStyle(index)` writes it into the edited scene as ONE undo step**, in four parts that can be switched off: BEAT (drums, swing, the groove baked into the steps, the kit, the scene grows to the typical length and is never shortened), SOUND (the four presets), NOTES (the one-bar bass line repeated over the scene and snapped into the current KEY and SCALE; the chord progression repeated until the scene is full, held chords, written even when the CHORDS tab is on arpeggio; with the chord bank on the chords go to the edited chord scene), TEMPO (the style's BPM, **off by default**). The key and the scale are never changed. Works on every grid (1/16, 1/8T, 1/16T, 1/32).
- **RANDOM / KIT panel:** a STYLE row above KIT and BEAT (`<` name `>` APPLY) and the chips **B S N T** under it for the four parts; CLEAR ALL moved next to the chips (the panel has no room for another row). The name opens the **STYLES browser** (the TYPICAL BEATS window with the same categories, search, tags and favourites) and a strip under the cards that says, in words, what APPLY will do for the selected style and has the four part switches; parts that are off are dim. Enter or double-click applies.
- `ListBrowser::Config::detailH` / `detail`: an optional strip the caller draws for the selected card. `ApplyDegreeSequence` got `undo`, `lenOverride` and `forceStack` parameters (defaults keep every old caller as it was).
- Selftest (10 checks): all 12 styles resolve in the library (pattern, presets, progression, groove, kit); each style on 1/16, 1/8T and 1/32 in D DORIAN-like minor mode: kick, bass in the scale, chords, kit, sound written, key unchanged, tempo untouched, ONE undo restores the scene; each part alone; all parts off applies nothing; chord bank; a two-bar style grows 16 -> 32 and a 64-step scene is not shortened. Fuzz gets a Style op (normal and ASan): 0 failures. ASan selftest needs `ulimit -s unlimited` (the frame of `main` has outgrown 8 MB). Checked in the window: STYLE row and chips fit, APPLY, the browser, a double-click apply of UK GARAGE (LEN 16 -> 32), UNDO back to 16.
- Not checked: the sound of the styles by ear on the Mac (they are written from convention and nobody has listened to all 12); the key / scale of other modes than DORIAN in the window; keyboard keys in the browser; MY STYLES (saving a style from the current scene) does not exist yet and the category is empty.
- Next release of stage S: the new styles in waves of 3-4 (ambient, synthwave, italo, trance, hardstyle / gabber, pop, rock, reggae, dub, drill, detroit and other techno / electro variations).

## v44.48

"One set of switches": WHO PLAYS and the LIVE panel. Audio unchanged (audit unchanged, 12 rounding lines).

- **Fixed: the chord bar of WHO PLAYS was an empty outline you could not press** when the chord bank was on. `TrackHasNotes` looked only at the chord steps of the scene, but with the bank on the chords live in the chord scenes; the chord track now "has notes" when any chord scene has chords.
- **WHO PLAYS: S under every bar** (solo = every other instrument off; press again brings back what played before) and **group buttons** to the right of the bars: ALL DRUMS, DRUMS (snare + clap + perc), HATS, DROP, MUTE ALL and the new ALL ON (every mute and solo off). A bar that is off only because another instrument is soloed has a dashed outline, a muted one a plain dim one. The tabs LEVEL / FILL / TRANS / PARAM moved to the right (the PARAM row still fits at 1440 points).
- **LIVE panel: the same ten bars.** They replace the pads KICK / BASS / SYN2 / SYN3 and add the clap, open hat, perc and chord; click = mute, S under = solo, same icons and colours as WHO PLAYS and the sequencer. The group buttons stand to the right of them (MUTE ALL moved there from the right column). To make room the XY pad is 84 (was 92), the FILL and RISER rows lost their labels (the buttons name them) and the drop length reads "1 BAR" (the word DROP is in the hint).
- **One code path.** `AppState::Sw*(brick, ...)` (brick >= 0 = the mask of that brick, -1 = the flags of the scene) is used by both screens, and the M and S of the sequencer write the same stores, so the three places always agree. The LIVE scene and a brick keep separate stores on purpose (a scene thrown onto the track folds its mutes and solos into the brick's mask). DROP stays lit only while the kick and bass play and the rest is off (MUTE ALL after a DROP ends it). The old group readers (`DrumMuteGroupIsMuted`, `SynthIsMuted`, `AllDrumsMuted`, `ChordIsMuted`) read the brick mask on a brick.
- Shared drawing in `ui_desktop/InstrumentSwitches.h`. Mockup: `docs/mockups/v44_48_who_plays_live.png`.
- Selftest: chord bank and `TrackHasNotes`, mute / solo / groups / DROP / ALL ON on a scene and on a brick, source ratchets. Fuzz (normal and ASan): 0 failures. Checked in the window: mute, solo, every group button and ALL ON on LIVE (the sequencer M and S followed); THROW onto the track; WHO PLAYS S and groups on TRACK (the sequencer followed); a run of 20 clicks across both screens with no crash.
- Not checked: the chord bar with the chord bank on in the window (covered by the selftest only); Cmd+Z after these presses.

## v44.47

"Long beats": step 1 of stage S of the roadmap. Audio unchanged (audit unchanged, 12 rounding lines).

- **Typical beats can be several bars long.** `GenrePattern::bars` (default 1) and step numbers in 16ths from the start of the pattern (a 4-bar pattern has steps 0..63). 13 patterns are long now: AMEN 4 bars; JUNGLE, BREAKBEAT, BREAKS, DRUM AND BASS, UK GARAGE, DUBSTEP, TRAP, HIP HOP, BOOM BAP, LO-FI HIP HOP, FUNK and AFROBEAT 2 bars. Bar 1 is always the old one-bar template; the later bars are the usual variations of the style (a moved kick, a ghost note, a pickup into the loop), kept as a table of edits in `DrumPatterns.cpp` (`kLongForms`, `kLongEdits`) and written from convention, not transcribed from any recording (AMEN included).
- **The scene follows the pattern.** Applying a long beat brings a shorter scene up to the typical length (AMEN: 16 -> 64 steps); the tracks that follow the master length repeat what they had, exactly as with LEN, and the whole change is one undo step. A scene that is already as long or longer is never shortened; the pattern repeats over it. A single-slot swap (kick swap, group swap) never changes the scene length.
- **Other grids.** A scene holds 64 steps, so on 1/32 or 1/16T only 2 of the 4 AMEN bars fit and on 1/8T 5 do; the status line says "ONLY THE FIRST 2 OF 4 BARS FIT THE 1/32 GRID".
- **TYPICAL BEATS browser:** a LONG tag and filter; BUSY and SPARSE are counted per bar so a 4-bar beat is not busy just for being long; the footer says a LONG beat lengthens the scene.
- Shared code: `AppState::ResizeSceneNoUndo`, `GenreBarsFit`, `WriteGenreSlot` (ApplyGenrePattern and the swap used two copies of the same loop).
- Selftest: data (13 long patterns, bar 1 unchanged, steps inside their bars), growth 16 -> 64, tiling of the other tracks, one undo step, no shortening, 1/32, 1/16T, swap leaves the length. Fuzz (normal and ASan): 0 failures. Checked in the window: AMEN from the browser (LEN 64, page 2 has the second bar), UNDO, APPLY twice, UNDO twice.

## v44.46

"CHORD LAB is honest": findings B6, B7 and part of C3 of the v44.43 audit, and a fix for a crash that v44.44 and v44.45 shipped. Audio unchanged (audit unchanged, 12 rounding lines).

- **Fixed: the second change in CHORD LAB crashed the program** (v44.44 and v44.45). `ChordLabPanel::BeginEdit` called itself when the chord bank was on, because a mass replacement of PushUndo by BeginEdit had also hit its own body. The first change after opening (bank off) worked, so a one-edit check did not see it. Found by making three edits in a row in the window; the selftest cannot reach the panel, so the check is the GUI run (see the audit notes).
- **B6: the sound of the chords.** A JAM | SOUND switch in the CHORD LAB header: SOUND shows the SOUND tabs of the chord instrument (preset, oscillators, filter, envelope + LFO, EQ, FX, AUTO) in the place of the palette, and the loop keeps playing. The sequencer above jumps to the chord row.
- **B7: NEXT is gone.** It was a formula (common tones, root motion, a bonus for home), not a fact. WHY now says only what can be counted: every note in the key or which one is outside it (named with the real scale, "C dorian", not "C minor"); the degree of the chord in the scale or that it is borrowed; how many notes it shares with the chord before (and how many are new); how many of the bass / synth notes under the block it holds and which are missing. The sentences about what a chord "wants" are removed.
- **B7: MATCH under every block.** Each block of the loop shows how many of the notes under its own steps its chord holds ("12/12 notes", a fit bar); with a block selected, the palette is scored against the notes under that block (what could stand there instead), with none against the bars chosen next to MATCH. MATCH is now a switch and follows every edit of the notes (`LabMelodyFromRange`, `LabFitOne`).
- **C3: one set of chord names.** The chord editor spells chords like the rest (flats follow the key: Bb, not A#; `LabPcName`), 6 and m6 chords are named in CHORD LAB too (C E G A is C6, A C E G is Am7; the lowest note decides). "CMIN" next to "Cm" is gone.
- **C3: PROGRESSION is gone from the CHORDS tab** (the progression picker, CUSTOM, APPLY and the four hand-picked degrees): START FROM in CHORD LAB does the same, with the chords in front of you. The tab keeps OPEN CHORD LAB, the key tools (ADJUST CHORDS TO SCALE, FOLLOW KEY, ADJUST MELODY TOO), RANDOM, CLEAR, VOICING and CHORD EDITOR.
- Not in this round (waits for use): CHORD LAB v2 (a PLAY layer in the sequencer, a hint for the place in the loop), the palette chip names in other places (the BRICK menu, chain labels), voice-leading.
- Selftest: WHY facts and scale names, notes under a block, source ratchets for JAM | SOUND, no NEXT, no PROGRESSION, names.

## v44.45

"Everything fits and is called the same": findings B1-B4, B8, B9 and part of C2 of the v44.43 audit. Audio unchanged (audit unchanged, 12 rounding lines).

- **B1 fixed: the bottom of TRACK fits.** The room the scene editor needs (10 rows of 20, header, footer, padding) is now worked out from its own layout, and the brick strip gives way for it (170-420 points); before, a fixed 236 was too little, so the sequencer hint ran into the info line and the fader was cut. Checked at 1440x900 and 1280x800.
- **B2 fixed: a small window keeps the header.** The automatic interface size starts to shrink below 1440 points of width (was 1400): at 1280x800 the UNDO field was cut in half. CHORD LAB text no longer goes below 10 points (was 9).
- **B3 fixed: an empty TRACK does not edit the LIVE scene.** With no bricks it shows "THE TRACK IS EMPTY" with GO TO LIVE and THROW THE LIVE SCENE ONTO THE TRACK. Scenes are edited on LIVE only.
- **B4 fixed:** MIX LAB names a brick "BRICK 2", not by its hidden slot ("SCENE 18").
- **B8 fixed: Option (Alt) copies.** Dragging a step with Option copies it (was Ctrl, which is Cmd on a Mac and nowhere else a copy key); the hints say Option on a Mac and Alt elsewhere (`interaction::OptionName`, `IsCopy`), also in the lanes.
- **B9: MIX LAB balance.** The bar scale grows with the data (12 dB at least, steps of 6 up to 36), so a bar is never cut off while its label says +25 dB; the scale is written under the bars. The sentence under the bars has room for three lines. Two cards for the same instruments with the same advised change (kick and bass, once in MUD and once in SUB) are one card; the second band is added to its text.
- **C2 (part): one set of words.** PATTERN is SCENE everywhere you read it (header, menus, messages, lanes), THROW INTO THE CHAIN is THROW ONTO THE TRACK, CHAIN FULL is TRACK FULL, "scenes of the chain" is "scenes of the track". View now lists CHORD LAB and MASTER FX next to LIVE, TRACK and MIX LAB. Esc closes CHORD LAB and takes MIX LAB back to LIVE (an open popup or a text box takes Esc first).
- Not in this round: one look for the tab row (MASTER FX is still a dropdown next to the tabs), keyboard check on a real keyboard (keys do not reach the app in the test environment: Esc, Space, Tab are still unverified), text smaller than 11 points elsewhere.
- Selftest: the source ratchets for the new layout, words, Option copy and View entries.

## v44.44

"Nothing is lost and nothing breaks": the critical findings of the v44.43 audit (`docs/AUDIT_v44.43.md`, items A1-A6 and C1). No new features. Audio unchanged (audit unchanged, 12 rounding lines).

- **A1 fixed: LFO to WT POS was lost on save / load** (project files and custom synth presets). Both readers now use one function, `LfoTargetFromText` (case-insensitive over `LfoTargetName`), so a name that is written can always be read. The fuzz round trip, which showed 6 differences before, shows none.
- **A2 fixed**: hovering the MIX or FILTER row icon on TRACK read past the end of the instrument name table (`BricksPanel.cpp`; found by random GUI actions under ASan).
- **A3 fixed: opening CHORD LAB changes nothing.** Before, it switched the chord bank on for the whole project and copied a slot on every open (three openings filled slots 1-4), and UNDO did not bring it back. Now the panel shows the scene's own chords, and the first change you make starts the draft: the chords are copied once into the first free slot, the bank goes on, and that and the change are one UNDO step (`AppState::StartChordDraft`, `PushUndoBank`). The undo step remembers bank on / off and the edited slot, so UNDO of BANK: ON works too (it did not before). ADD TO SCENE and THROW INTO THE CHAIN wait for a draft.
- **A4 fixed: auditioning a chord no longer cuts the chord of the loop.** The panel plays the chord on its own voices (`AudioEngine::ChordAuditionOn / ChordAuditionOff`), through the chord track strip, not logged to MIDI capture; Panic stops it.
- **A5 / B5: the old live chord (PLAY / LIVE under CUSTOM CHORDS) is removed.** It replaced the notes of every active step with a full stack, so it broke ARP and showed STACK changes only at the next step. CUSTOM CHORDS keeps the four degrees used by APPLY and the CHORD EDITOR button. CHORD LAB is the place for chords; a better live layer is planned after the three stabilisation releases.
- **A6 fixed: six voices everywhere.** MIDI capture and the notes export kept only four notes of a chord, and HEAR in the chord editor played four of six.
- **C1: the scene lock is checked, not only agreed.** The lock is now a `SceneMutex` that knows its owner; `PushUndo`, `PushUndoBank` and `PushSoundUndo` count a call made without it (`Sequencer::SceneLockViolations`, one line on stderr). The first catch: `LoadStarterGroove` took its undo copy before the lock; fixed.
- New tool `t8_fuzz` (`tools/fuzz_main.cpp`): random sequences of 60 UI operations with audio in between, invariants, a save / load / save round trip and the lock check; `--threads` renders on a second thread. Meant for the sanitizer builds.
- Selftest: LFO targets round trip (project and preset), draft start / undo / redo, six notes in capture, audition voice, lock counter.

## v44.43

CHORD LAB: a panel over the LIVE screen for jamming with chords, and chords of up to six voices. Audio of existing projects unchanged (audit unchanged).

- **CHORD LAB** (button next to BANK on the CHORDS row): a palette of the chords of the key in four rows (triads, 7ths or 9ths with the **9THS** switch, sus / add, borrowed), with the roman number of each; a hollow dot marks a chord outside the key. Click = hear it and see WHY, drag it into the loop, double-click = add it after the last chord. Spelling follows the key (flats for C dorian, F minor, Bb major ...).
- **MATCH**: reads the bass and synth notes of the scene (bars 1, 1-2 or all; bass on the beat counts double) and marks every chord with a fit bar and a teal outline for the best ones. Nothing is chosen for you.
- **The jam loop**: blocks of chords on a beat grid. Drag the orange edge to change a length (the neighbour gives way, one beat is the smallest), SIZE buttons (1/2 BAR, 1 BAR, 2 BARS, SPLIT), right-click = delete, EMPTY, START FROM a typical progression (the 7TH / TRIAD choice of the CHORDS tab carries over). The chord loop runs on its own length (up to 64 steps) over the scene.
- **WHY and NEXT**: the reasons are built from checkable facts (is every note in the key, which notes it shares with the chord before, what the degree does in a major or minor key, how much of the melody it holds and which melody note it misses); NEXT lists four chords that tend to follow, each with its reason. No model, no randomness.
- **Jam in a draft**: the jam lives in a chord bank slot (the bank is switched on; a slot that already has chords is copied into the next empty slot, so nothing is overwritten). The slot pad shows DRAFT while the panel is open.
- **FROM JAM TO TRACK**: ADD TO SCENE writes the loop into the scene's own chord track with the key and scale it was built in (one Undo step, the slot keeps its copy; `AppState::CommitChordSlotToScene`). THROW INTO THE CHAIN now stamps the chord scene you were hearing on the new brick when the bank is on (before, a brick thrown with the bank on played whatever chord scene happened to be current); with the bank off nothing changes. TRACK opens the arrangement.
- **Six chord voices** (was four): `kMaxChordVoices` = 6 (now in Types.h), `Step::chordNotes` holds six tones. The 5th and 6th tone are written at the very end of the step line, so a project without them is byte-identical to before and older versions still read newer files (they just keep four tones). 9th chords use five voices.
- The chord slot labels name chords properly (Am7, Adim, Cadd9; before, a diminished chord read as minor).
- Selftest: worst-case CPU check now uses a six-note chord and the best of three runs, limit 70 % of real time (was 50 % with four voices; this machine measures 37-57 %).
- Not in this round: arpeggiated output of the jam (it writes held stacks), moving blocks by dragging them, MIDI input, a voice-leading option for the stacks.

## v44.42

MIX LAB gets hands: level, pan and a full channel EQ for the selected instrument, and one button that copies the result into every scene and brick. Audio unchanged (audit unchanged).

- **CHANNEL panel** (bottom right of MIX LAB): pick an instrument (click its name in the "who lives where" strips, or < >), then drag its level and pan, and shape the four-band channel EQ (HPF, low shelf, MID bell, high shelf: drag a node, wheel on MID = width, double-click = flat). The EQ curve is drawn over what the instrument contains and over the overlap zones it takes part in, so a cut lands where the fight is. The picture listens again when you let go.
- Every edit is an A / B change like a fix (one Undo step for the scene; A / B, KEEP FIXES and UNDO ALL cover faders and EQ too). FLAT EQ puts the EQ back to flat.
- **APPLY TO ALL** (under A / B): keeps the changes and copies the LEVEL, PAN and EQ of every instrument you touched into the other scenes and bricks. The switch next to it chooses ALL SCENES + BRICKS or ONLY THE CHAIN. **TO ALL SCENES** in CHANNEL does the same for the one selected instrument (also when nothing was changed yet).
- `AppState::MatchLevelPanEq(srcScene, track, onlyChain)`: only level, pan and the EQ fields are copied; sends, insert FX, mute / solo and notes of the other scenes stay; scenes that nothing uses are skipped; each changed scene gets its own undo entry.
- Tests: 8 checks (reaches live scenes and chain bricks, unused brick slot untouched, sends / mute / other EQ bands untouched, one undo entry per scene, repeat = no change, ONLY THE CHAIN, source check for CHANNEL and both buttons).

## v44.41

MIX LAB: a new tab that shows where the instruments of the played scene fight for the same frequencies, and what to turn to make room. Audio unchanged (audit unchanged); the only engine change is an optional per-track tap that is off in normal playback.

- **Analysis** (`src/core/MixAnalysis.h/.cpp`): the scene is rendered once, offline, on a private engine in a worker thread (`src/app/MixLab.h`, `RenderMixStems`; 2 loops of the scene, 3-9 s), every track is tapped after its fader and pan (`Mixer::tapMono`, before the delay / reverb sends and the master chain), and each stem is analysed with an own FFT (8192-point Hann, hop 4096, only the frames where the instrument really plays) into 120 log bands (20 Hz - 20 kHz, 12 per octave).
- **Overlap spots**: a band is contested when two or more instruments are within 8 dB of the loudest one and the spot is loud enough to matter; contiguous contested bands become spots (at most 5, worst first). Each gets a title, words, the instruments involved, and ONE concrete fix for the instrument lower in the priority list: a channel-EQ cut (HPF, low shelf, MID bell, high shelf), a level change when the EQ band is already in use, or PAN for the two hats.
- **Tab MIX LAB** (View menu, tab bar): frequency map with one curve per instrument and the dashed whole mix, "who lives where" strips, red overlap zones with numbers, cards with SHOW IT (temporary solo of the colliding instruments, put back when pressed again or when leaving the tab) and TRY IT (the change is applied for real, one undo step), tonal balance of the whole mix against a typical track (six zones, +/-3 dB band, click a bar to see whose energy it is), the priority list **WHO KEEPS THE SPACE** (drag; the report is rebuilt at once), and A / B with KEEP FIXES / UNDO ALL ("Overlaps 4 -> 1, loudness of the mix -0.4 dB").
- Simplifications, said plainly: it analyses the scene as a loop (not the whole track); the delay / reverb returns and the master chain are not in the picture; advice comes from templates, not from listening; the "typical track" curve is a rough guide.
- Tests: 17 checks (calibration of the FFT / bands, spot found for kick + bass in the sub, who keeps the space, the priority list changes the advice, no spot for instruments far apart, fixes land on the right track fields and fall back to LEVEL when the band is in use, tonal balance, offline render taps the kick low / muted instruments are not counted / worker thread, source checks for the tab).

## v44.40

LIVE and TRACK: a red kick, the level of every instrument in the sequencer rows, and launch feedback on the scene slots. Audio unchanged (audit unchanged).

- **Kick is red** (238,74,60) instead of orange, so the amber accent now means only "selected / primary". REC gets an outline and a dot (it is not a flat red fill any more). The TRANS page keeps its lighter salmon, CLEAR ALL its dark red.
- **Level stripe.** The coloured stripe at the start of every sequencer row (LIVE and TRACK) is now the level of that instrument (post fader, from `Mixer::tracks[i].peak`): it rises and falls with the sound, shows a sliver when silent and dims when the instrument is muted. The old hairline under the name is gone.
- **Launch feedback.** The slot that plays shows how far through its loop it is (a green bar along the bottom, `Sequencer::SceneProgress()`). A queued slot gets a warm wash and a countdown in beats (`Sequencer::BeatsUntilLaunch()`, the part of the step already gone is taken off), and a line "SCENE 03 launches in 1.2 beats (BAR)" with a progress ring appears under the chord slots. Every slot with content shows sixteen dots: what the scene plays across its loop (brighter = more instruments).
- `Sequencer::LaunchQuantumBeats()` (1 for BEAT, the loop for BAR).
- Tests: 7 checks (colours, REC outline, stripe, slot feedback source checks; no countdown with nothing queued; countdown in beats and going down).

## v44.39

LIVE: parameter curves on a scene (the AUTO tab). Without curves the audio is unchanged (audit unchanged).

- **AUTO** (a new tab of the LIVE instrument block, after FX; drums and synths, the chord track through its SOUND view): the same knobs as the PARAM page of a brick (PAN, REVERB, DELAY for every instrument, TUNE and DECAY for drums, CUTOFF and RESONANCE for the synths) drawn as a curve over the scene's loop. A star on a knob chip = the scene already moves it. Click on empty space = add a point, drag points, Shift = finer, Option + click = 0, double-click an inner point = remove it, Esc = cancel; FLAT / UP / DOWN / remove tiles, START and END in the unit of the knob. While the scene plays a line and a dot show where the curve is now. The row is created by the first edit (looking at an instrument never adds one). Up to 6 curves per scene.
- Data: `Pattern::params` (a `StepParams`, the same type a brick has), saved as `params=` in `[patternN]`. Engine: while a scene plays on LIVE (no song chain), `Sequencer::UpdateAutomation` fills `Mixer::arrParam` from the scene's rows with the progress through the loop (`Sequencer::SceneProgress()`); the sound path is the one v44.38 added, so a curve sounds exactly like the same row on a brick.
- THROW INTO THE CHAIN moves the scene's curves into the brick (its PARAM rows) and the brick's own scene carries none; LIVE keeps its curves. UPDATE FROM LIVE takes them along.
- `LaneWidget.h` (`LaneEditor`) is the shared curve editor, `ParamText.h` holds the words (`ParamValueText`, `ParamHundred`).
- Not in this step: MIX / FILTER / level curves on a LIVE scene, recording knob movement live (14в).
- Tests: 12 checks (file round trip, SceneProgress, the curve runs once round the loop and restarts, flat row = no row bit for bit, a CUTOFF curve changes the sound, THROW and UPDATE FROM LIVE, source checks).

## v44.38

TRACK: parameter rows (PARAM), one mute for a brick, the Tab key, a wider name column. Without parameter rows the audio is unchanged (audit unchanged).

- **PARAM** (a fourth coloured page of the inspector, violet): one knob of one instrument moves over the brick. Parameters: PAN, REVERB send and DELAY send of every instrument, TUNE and DECAY of the six drums, CUTOFF and RESONANCE of bass / synth 2 / synth 3 / chord. The lane is an OFFSET from the value the sound has (-100..+100, 0 = untouched), so no patch is overwritten and the same brick works on any sound; 100 = full side / +100% send / 12 semitones / x2 decay / 3 octaves / +50% resonance (the caption under each field says it in the unit of the knob). One searchable picker (type "bass cut", "hat pan"; the rows the brick already has come first), START and END fields, FLAT / UP / DOWN, remove row, up to six rows per brick. A fourth lane row under the bricks (the chosen parameter, drawn in the instrument's colour) appears once a brick has a row; drag the dots like on MIX and FILTER, click an empty cell to start a row on that brick, right-click opens the PARAM page.
- Engine: `Mixer::arrParam` (offset per instrument and parameter, written by the sequencer from the playing brick). Pan and the two sends are applied per sample inside `MixTrack` (ramped across the block; with all offsets zero the old fast path runs), tune and decay when a drum is struck (`Sequencer::ApplyDrumMods`), cutoff and resonance by the synths (`SetMod`, never part of the patch; smoothed by the existing 4 ms cutoff smoother). File: `prmN=track,param,lane;...` per brick (a flat row stays).
- **One mute for a brick.** WHO PLAYS, the M and S buttons of the sequencer on TRACK and the keys are now one switch, the `muteMask` of the brick. M = that instrument is off in the brick; S = every other instrument off (press again to bring back what played before). A scene's own mute / solo flags are the LIVE jam: when a scene becomes a brick (THROW, +) they are folded into WHO PLAYS and cleared; files from before are converted on load. On LIVE nothing changed.
- **Tab** goes between LIVE and TRACK.
- The name column of the sequencer is wider (152 px): ClosedHH / OpenHH no longer run into the pictogram or the M button.
- Fix: with the parameter rows ChainStep grew enough for GCC 13 (-O2) to build `ChainStep{1, 1, 2}` wrongly (the fields after the second came out as zero); ChainStep has constructors now (same call sites).
- Tests: 23 checks (rows: data rules, file round trip, offset follows the lane, PAN row = the same as pan on the track, CUTOFF row darkens the bass, no rows = unchanged; mute: fold on throw, M / S write the mask, solo and restore, old files converted; source checks for the screens).

## v44.37

Pictograms on the remaining tabs. No audio changes (audit unchanged).

- The ten instrument pictograms (`DrawInstrumentGlyph`) now also label the instrument everywhere else: sequencer rows (icon + short name), the volume strip, the LANES and Piano Roll headers, the context header (replaces the colour dot) and the SOUND chips on TRACK. The SOUND chips are icon-only 36 px tiles (`GlyphTile` with `instrument` and `idleCol`) with a colour underline and the dot for an own sound; the name is in the info line.
- Left as words on purpose: the LIVE performance buttons (KICK / DRUMS / HATS / ... / DROP). Row icons get small when the rows are low (short window).
- Tests: 2 source checks (where the instrument pictograms are used; `GlyphTile` instrument argument).

## v44.36

TRACK: pictograms for the ten instruments, the brick actions in the left column of the inspector, TRIM with beats. No audio changes (audit unchanged).

- **Instrument pictograms** (`BrickGlyphs.h`: `DrawInstrumentGlyph`, `InstrumentName`): kick (bass drum), snare, clap (two hands), closed / open hat (plates), perc (conga), bass (a long sub wave), synth 2 (saw), synth 3 (square), chord (three notes on a stem), in the track colours. Used on the WHO PLAYS bars of the inspector (dark on a lit bar, own colour on a dim one) and as the row labels under the bricks, where the label column shrinks from 52-66 px to 34 px, so the bricks get the room. BRICKS, FILLS, TRANSITIONS, MIX and FILTER got pictograms too (new glyphs Bricks, Mix, Filter). The name and what the row does are in the info line when the mouse is over it.
- **Brick actions** (copy, update from LIVE, repeat same, reset, remove) moved from the far right of the inspector to the left column under the brick title, where the eye starts; the role list sits under them, the length is on the title line. The pages (LEVEL / FILL / TRANS) get the room that was freed on the right.
- **TRIM:** the list offers 1-3 beats as well as whole bars (before, a one-bar scene had nothing but FULL); when a scene is too short for any cut the list is greyed out and the info line says why. Labels read "1 BEAT", "2 BEATS", "1 BAR".
- "1 bars" -> "1 bar" in the TRACK header and the brick length.
- Tests: 3 source checks (instrument glyphs and where they are used, actions in the left column and the narrow label column, TRIM beats and the reason).

## v44.35

TRACK: a way to loop the WHOLE track while you edit one brick. No audio changes (audit unchanged).

- **Why:** LOOP went round "the selected brick or range". Select all bricks with Shift, click one brick to change it, and the selection (so the loop) shrank to that brick: the transitions between the bricks were gone from the loop.
- **ALL** (new button next to LOOP, needs LOOP on and two or more bricks): the whole track goes round, with the transitions, whatever brick or range is selected or edited. Clicking a brick only changes what is edited, the music does not jump; double-click a brick starts playing from it. Space starts at the selected brick, Home at the first one. Switching ALL while playing does not jump either. ALL off: LOOP is as before (the selected brick or range). LOOP off: the track plays once and stops, as before.
- Implementation: `AppState::loopAll` / `SetLoopAll`; with it `ApplyTransportFlags` hands the sequencer the range 0..last (the sequencer clamps the high end to the last brick, so added and removed bricks are followed).
- Tests: 7 checks (flags, 1-2-3-1 order, selecting a brick or a range does not narrow the loop, the track goes on while brick 2 is edited, ALL off, LOOP off).

## v44.34

U8a: the brick inspector on the TRACK tab is redrawn with pictograms (one shared set) and three coloured pages. No audio changes (audit unchanged).

- **Why:** the block under the automation was a wall of words and buttons (ROLE, REPEATS, TRIM, WHO PLAYS, DYNAMICS, FILL, TRANSITION, COPY BRICK, UPDATE FROM LIVE, REPEAT SAME, REMOVE, RESET, LEVEL OF, START, END, FLAT, FADE IN, FADE OUT, FILL LEVEL, TRANSITION LEVEL). Reference: Teenage Engineering (OP-1 / OP-Z: colour = page = value, few values at a time, words kept short) and pocket synths (FM-1: draw the structure).
- **Layout, left to right:** the brick (length, role chip) | PLAY (repeats, trim) | WHO PLAYS (ten bars in the instrument colours: click or drag = off / on, right-click = the instrument LEVEL works on, a ring marks it; the bar rises / falls with its level shape; a white dot = own sound; a list icon opens the quick choices EVERYONE / NO DRUMS ...) | three pages LEVEL (blue), FILL (yellow), TRANS (red), one open at a time, a dot on the tab when the page has something | the five actions as pictograms (copy, update from LIVE, repeat same, reset, remove).
- **LEVEL page:** lane (MIX / FILTER / instrument), a small picture of the ramp, START and END, three shapes (flat / fade in / fade out, the lit one shows the current shape), DYNAMICS presets. **FILL page:** kind, when, length, level and a strip that shows where the fill lands. **TRANS page:** riser, bars before, level and a strip with the lead-in.
- **New file `ui_desktop/BrickGlyphs.h`:** 14 pictograms drawn with ImDrawList lines (no font, no image files), the tile `GlyphTile`, and the colour rule (amber = play, green = who plays, blue = level, yellow = fill, red = transition). The same set will be used on the other tabs.
- Words moved to the info line: every picture says what it does there when the mouse is over it. Nothing was removed; the inspector is not taller (104 px).
- **Transition on the border of two bricks:** the TRANS page has an IN / OUT switch. IN = the transition that leads into this brick (as before), OUT = the one that leads out of it into the next brick (it is the IN of the next brick, edited from here). The first brick can only have OUT, the last one only IN; a one-brick track says a transition needs two bricks. The tab shows its dot when either border has a transition.
- Tests: 8 source checks (the set has all 14 pictograms and uses no font or image, the three pages exist, the actions are pictograms, every old action and field is still reachable, the height is unchanged).

## v44.33

Fix: a brick thrown from a scene with its own drum kit played the wrong kit. The audio of existing scenes does not change (audit unchanged).

- **Cause:** when THROW (or "+", or COPY BRICK) copies a scene into a brick slot, the program handed the kits to the sequencer before the new brick was in the chain, so the slot was skipped as "not in use" - and the kit of that slot was marked as done. Later checks saw nothing changed, so the brick never got its kit and played the project's default kit instead, with the sample numbers of the scene. The same scene sounded different on TRACK than on LIVE, and picking percussion samples on a brick gave the wrong sounds (for example mostly cowbells, which come from another kit).
- **Fix:** the kit sync also remembers which slots were in use; when a brick appears (or goes away) the banks are handed over again.
- Tests: a scene with kit 808perc and a chosen percussion sample (as the default sample of the instrument and as the sample of one step) renders identically as a LIVE loop and as a brick, for samples 1, 4 and 5 (before the fix: six failures).

## v44.32

K1: new kicks and the rest of the 808 percussion, from the analysis of the eight reference tracks. Existing sounds are not changed (audit unchanged); everything new sits in new slots.

- **Measured:** in the references the kick is a tuned sub (body 43-49 Hz = F1 / G1, starting from 80-115 Hz) with a tail over 350 ms and an audible click (-20 ... -33 dB; the measure is the 1.5-6 kHz energy of the first 12 ms against the 25-500 Hz energy of the first 80 ms). The old 808 kick measured -54 dB (909 -43, trap808 -54): that is why it was not "snappy".
- **New kicks** (calibrated to that measure by the generator): **808 SNAP** (48 Hz, short tail, click -25 dB) = `kick_3` in kits 808 and trap808, **808 HARD** (45 Hz, driven, click -20 dB) = `kick_4` in 808 and techno, **808 SUB F1** (43.65 Hz tuned to F1, 0.6 s tail, click -30 dB) = `kick_5` in 808.
- **808 percussion:** kit 808 gets cowbell (`perc_4`, two square waves 540 / 800 Hz), clave (`perc_5`, about 2.5 kHz), rimshot (`perc_6`, resonances near 500 Hz and 1.7 kHz), maracas (`perc_7`), a conga (`perc_8`) and a cymbal (`ohh_2`, the six 808 oscillator frequencies).
- **New kit 808perc:** conga low / mid / high (`perc`, `perc_2`, `perc_3`), cowbell, clave, rimshot, maracas, high tom (`perc_8`), cymbal (`ohh_2`), the SNAP kick, and the snare, clap and hats of the 808 kit.
- `tools/gen_k1_kits.py` makes the files (24-bit, 48 kHz, mono, nothing is overwritten); run it after `t8_kitgen`.
- Tests: the 808 kit has 5 kicks and 8 percussion sounds, 808perc loads, the new kick has a far stronger click than the old one, levels natural and no clipping.

## v44.31

U7: browsers and windows that ask something answer the same keys. The audio does not change (audit unchanged).

- **One shared core for the list browsers** (`BrowserCore.h`): typing anywhere goes to the search, Space (empty search) = hear, arrows / PageUp / PageDown move, Return picks, Esc clears the search and then closes; the star and the tag chips of a card are drawn by one function. The synth preset browser and the kit / beat browser both use it, so they cannot drift apart again (about 90 duplicated lines are gone).
- **One contract for dialogs** (`Interaction.h`: `KeyConfirm`, `KeyCancel`): Return = the main button, Esc = cancel / close, and Esc only counts for the window that has the focus (Esc in a rename box closes the box, not the dialog under it). Used by Save / Don't save, Name, Open, Versions, My Beats and the rename box. The crash-recovery window: Return = RESTORE, Esc does nothing on purpose (cancelling would mean throwing the work away).
- **Open a project** works from the keyboard: Up / Down choose, Return opens, Esc cancels.
- Tests: source ratchets (no own Return / Esc checks in the dialogs and browsers; both browsers use the shared core; the Open list takes arrow keys).

## v44.30

U6: select several things and act on them together. The audio does not change (audit unchanged).

- **Bricks:** Shift+click (or Cmd+click) selects the range from the last selected brick to the one you click; Cmd+A selects every brick; **dragging a rubber band over empty space in the brick row** selects the bricks it touches; Esc ends the range. With a range selected, **LOOP goes round the whole range** (so Cmd+A + LOOP repeats the whole track from the start). Delete removes all bricks of the range, Cmd+D puts the range (or the brick) again right after itself; both are one undo step.
- **Piano roll:** Shift+drag draws a rubber band over notes (it adds to the selection), Shift / Cmd+click adds or removes one note, Cmd+A selects every note of the track, a plain click elsewhere ends the selection. Selected notes (white outline): **drag = move in time and pitch together, Option+drag = a copy**, Up / Down = a semitone (Shift = an octave), Left / Right = a step (Shift = a beat), Delete / Backspace removes them, Cmd+D puts them again right after themselves, dragging the right edge of one of them changes the length of all. A moved note keeps its length, probability, ratchet, nudge and slide; on the chord track one note of a chord can be moved alone. Nothing can be pushed off the grid, and a drag can go there and back without losing a note (it works from a snapshot).
- While notes are selected and the mouse is over the roll, the arrow keys belong to the notes (not to the parameter of the instrument block below).
- New `core/NoteEdit.h` (group editing as pure functions on a track), `AppState`: `SelectBrickRange`, `SelectAllBricks`, `ClearBrickRange`, `RemoveBrickRange`, `DuplicateBrickRange`.
- Tests: bricks (range, backwards, all, loop range, remove / duplicate and their undo), notes (octave, copy, clamp, there and back, length, delete, duplicate, chord note), source ratchets.
- Not in this step: selecting steps of the drum grid with a band and selecting several tracks (the next steps will use the same helpers).

## v44.29

U5: no duplicate places for the same setting. The audio does not change (audit unchanged).

- **The "Step settings" window and "Fine-tune numbers..." are gone.** Everything they had is in the brick inspector (right of the strip) and in the right-click menu of the brick: the inspector row "LEVEL OF" now offers MIX and FILTER next to the instruments, with their start / end fields and FLAT / FADE IN / FADE OUT.
- **RESET** (inspector button and menu item "Reset brick"): puts everything the brick adds on top of its scene back to nothing (mutes, fill, transition, trim, automation, levels). The notes are not touched. One undo step; it is never merged into the previous edit's undo step.
- Tests: source ratchets (the window code and its menu items must not come back), RESET puts everything back, one undo step, the notes stay.

## v44.28

U4: one player. The audio does not change (audit unchanged).

- **PLAY, LOOP, |<** instead of BRICK / TRACK / LOOP / FROM START. PLAY (Space) starts at the selected brick. **LOOP on** (the default) keeps the selected brick, or a selected range of bricks, going round; **LOOP off** plays the bricks in order and stops at the end of the track. **Home** (and the `|<` button) goes to the first brick; when it plays, it restarts there, when it is stopped it only goes there. **Return** plays from the selected brick. With LOOP on, a click on another brick while it plays moves the loop there. On LIVE the player is unchanged: it plays the scene you are on (v44.27).
- **Sequencer:** `SetSongLoopRange(lo, hi)`: after the last brick of the range the chain goes back to the first one (U6 will select the range; for now a range comes from `AppState::selBrickHi`). `AppState`: `loopOn`, `SetLoop`, `TrackToStart`, `LoopRange`; `playMode`, `trackLoop`, `SetPlayMode`, `SetTrackLoop` and `PlayTrackFromStart` are gone.
- Tests: the player block was rewritten (LOOP on / off, Home running and stopped, a range going round, Space on TRACK and on LIVE).
- Plan: parameter automation rows (with a parameter picker), LFO on parameters, duplicate sound as variant (A/B) and recording a live performance into bricks were added to the roadmap.

## v44.27

Two player problems found by use. The audio engine does not change (audit unchanged).

- **A thrown brick sounds like the scene you threw.** THROW (and "+" with the scene you were on) used to replace the sound of the copy with the sound of the track, so the same piece sounded different on TRACK than on LIVE as soon as the LIVE scene had its own patch, level, pan, FX or kit. Now the brick is an exact copy: in the parts where the scene sounds the same as the track the brick follows the track, in the parts where it differs the brick keeps the scene's sound as its OWN (the dot on the instrument; WHOLE TRACK in SOUND gives it the track's sound again). The track itself is not changed by a throw (the first brick still sets it).
- **LIVE plays LIVE.** Going to the LIVE tab while the track plays now leaves the chain and continues with the scene shown on LIVE (the beat keeps running). Play / Space on LIVE always plays that scene, also when the player was left in TRACK mode (before, it ran the whole track under a screen that edits something else). Home still plays the whole track on purpose.
- Tests: 7 new checks (the same scene renders identically as a LIVE loop and as a brick with a different sound, own marks only where it differs, the player on both tabs). Four older tests changed their expectation (a thrown brick no longer takes the track's sound).
- Next: U4, the player itself (PLAY + LOOP on the selected brick, Home, no BRICK / TRACK modes).

## v44.26

A kit for one drum, and the kit browser on TRACK. The sound of existing projects does not change (audio audit unchanged).

- **The kit browser did not open on TRACK.** The KIT picker of a drum (name button, ALL) asked for the browser, but the browser window was drawn on LIVE only, so on TRACK nothing happened (and it popped up later when you went to LIVE). `BottomPanel::DrawBrowsers` is now drawn on both tabs.
- **THIS DRUM: THE KIT | OWN KIT** (SOUND view, next to the kit scope): one drum can play from another kit than the rest of the brick (a snare from 808 in a 909 brick). OWN KIT starts as a copy of the current kit; the KIT picker, the arrows and the browser then change the kit of that drum only. A dropped WAV goes into that drum's kit. It is a part of the instrument's sound, so it follows the WHOLE TRACK / THIS BRICK ONLY choice. New `Track::kitFolder` (file key `trackKit=`, written only when set); the played bank is put together from the kit of the scene and those drums (`kitMixed_`), nothing in the audio engine changed.
- Tests: 11 new checks (effective kit and variant counts, the composed bank, back to THE KIT, the picker routing, the sound part, save / load, UI ratchets).

## v44.25

Fixes from use of the TRACK screen. The audio does not change (audit unchanged).

- **Lane ends can be grabbed.** The start and the end point of the MIX, FILTER and instrument lanes did nothing when taken on a lane that was still flat: the lane was turned flat again on the first frame of the drag and the drag stopped. Now the lane stays editable while the mouse is down (`Lane::MovePoint(..., keepFlatPoints)`) and goes flat again on release if every point is back at 0.
- **Fills and transitions on the lane can be moved and removed.** A fill block: drag it sideways to move it inside the brick (whole beats earlier than the end of the pass / of the N-bar block; new `FillRule::shiftBeats`, saved as `fsh<N>=`), or drop it on another brick (that brick gets it as its own fill, the first one gets "no fill"). A click without moving removes it (undo brings it back). A transition block: drag it to the start of another brick (it always lands on a first beat), a click removes it. The left edge still sets the length.
- **A break can be chosen.** The fill kind BREAK was always "a random break"; now the right-click menu of the fill (Break) and the FILL list of the inspector offer RANDOM or one named break from the library.
- **More room for the lanes:** the strip is taller (the editor below keeps about 236 px less reserved), and a lane row is at least 30 px (was 18).
- Tests: 12 new checks (an end of a flat lane survives a drag, a shifted fill plays in the right window for LAST LOOP / EVERY N BARS, shift limit, save / load, the UI ratchets).
- Not done: a transition cannot start anywhere but on a first beat (it moves between bricks, not inside one).

## v44.24

The sound of an instrument on TRACK (S2 of the interaction plan): one sound for each instrument for the whole track, and a brick can take its own. The sound of what existed does not change (audio audit unchanged).

- **NOTES | SOUND | MIX** under the brick inspector on TRACK. NOTES is the editor as before. SOUND is the same instrument block as on LIVE (kit, voice, EQ, FX, synth patch, kick) for the selected instrument, with ten instrument chips above it (a white dot on a chip = this brick has its own sound for it). MIX is the mixer of the brick. The label says where you are (`BRICK 2`).
- **WHOLE TRACK / THIS BRICK ONLY** (SOUND, per instrument): by default a brick follows the sound of the track, so a change made on any brick that follows is a change of the track and reaches every other brick that follows (`AppState::SyncBrickSound`, every third frame; Undo just puts the edited brick back and the next sync puts the others back with it). THIS BRICK ONLY gives the brick its own copy of that instrument (mark in `Pattern::ownSound`, one bit per part); WHOLE TRACK gives it the track's sound again. Both are one undo step. **APPLY TO ALL BRICKS** makes this brick's sound the sound of the track and of every brick, own sounds included (one undo step). The drum kit has its own switch (TRACK'S KIT / THIS BRICK'S KIT) because it is one folder for all the drums of a brick.
- **A sound is eleven parts** (`core/SoundParts.h`): the ten instruments (channel strip = level, pan, sends, insert FX, EQ, drum voice, plus the synth patch / chord patch / built-in kick) and the kit. Notes, the track's own length and swing, and mute / solo are not part of it. So the level, pan, EQ and FX of an instrument follow the track too (set in MIX or SOUND) and the per-brick balance is the level lanes. The track's sound now holds whole channels, not only voices (v44.23's TAKE SOUND TO TRACK, the first throw and UPDATE FROM LIVE use the same parts).
- **Own sounds in the strip:** a white dot with a violet ring in the cell of the instrument on the brick.
- **Files:** format 6. `ownSound=<bits>` per brick scene. A file older than v44.24 takes the first brick's sound as the sound of the track and every other brick keeps the parts that sounded different as its own, so nothing changes in what it plays.
- Tests: 32 new checks (parts and their text, whole-track edits reaching the others, undo of them, own sounds left alone, follow / apply and their undo, the kit, a new brick, COPY BRICK, save / load, an older file, the UI).
- Not done: the MIX of the whole track (master level, tempo of the track) is still the existing MASTER / header; the sound of a brick is still shown by instrument chips, not by rows next to the timeline (the strip only marks own sounds). A per-brick balance other than the level lanes needs THIS BRICK ONLY on the instrument.

## v44.23

Independent bricks, part two (S1b of the interaction plan): the sound of the track, taking a sound on purpose, updating a brick from LIVE, and "where am I". No sound change for existing projects (audio audit unchanged).

- **The sound of the track** lives in one more hidden slot (`kTrackSoundSlot`, `Project::trackSoundMask` says which parts exist: three synths, chord, kick, drum voices, kit). A brick thrown from LIVE (THROW, `+`, scene pad menu) gets the sound of the track for every part that is defined, so jamming with other sounds on LIVE no longer reaches the track. A part the track does not have yet is taken from the thrown scene, so the first brick sets the sound of the track. One undo step takes back the brick and the track's sound together.
- **TAKE SOUND TO TRACK** (LIVE, under CHORD BANK; also in the scene pad menu): the sound of the scene you are on becomes the sound of the whole track and every brick switches to it. Notes and brick settings stay. One undo step restores the track's and the bricks' sounds (the undo entry carries them).
- **UPDATE FROM LIVE** (TRACK inspector): the brick's notes, timing and track levels are replaced by the current version of the LIVE scene you were on; the brick keeps its sound, role, fill and mutes. Undoable. Disabled while that LIVE scene is empty. A brick that shares its slot (REPEAT SAME) changes together.
- **Where am I:** the LIVE tab and its line are amber, the TRACK tab and its line teal; the editor's label says `LIVE  SCENE 03` or `BRICK 2  NOTES` in the same colour, with a hint.
- Files: key `trackSound=<mask>` and the sound as `[pattern80]`, written only when the track has a sound. A file without it (older than v44.23) takes the sound of its first brick as the sound of the track. The kit and drum-voice copies of CopySceneSound share one function (`CopySoundParts`).
- The LIVE scene row is 12 px taller (104) to fit the new button.
- Tests: 27 new checks (first throw sets the track sound, later throws keep it, the live scene is not touched, undo / redo of both, take sound and its undo, update from LIVE keeps sound / role / mutes, save / load, an older file, the UI ratchets).
- Not done (S2): editing the sound on TRACK (instrument rows, SOUND panel, WHOLE TRACK / THIS BRICK ONLY, own-sound marker). Until then the sound of a brick can only be changed through LIVE (TAKE SOUND TO TRACK).

## v44.22

Independent bricks, part one (S1a of the interaction plan). A brick now owns its notes: THROW copies the scene instead of pointing at it. The sound of what existed does not change (audio audit unchanged).

- **Hidden brick slots:** `kNumPatterns` is now 80 = `kNumLiveScenes` (16, the jam pad) + `kNumBrickSlots` (64, one for each possible brick). THROW INTO THE CHAIN, the `+` on the strip and "Throw a copy into the chain" in the scene pad menu copy the whole scene (notes, length, level / FX, mute flags, sound) into the first free brick slot. COPY BRICK makes another independent copy; REPEAT SAME still shares one slot between bricks. A slot is "in use" while a brick points at it and is free again when the last such brick is removed or the throw is undone.
- **The editor follows the tab:** on LIVE it edits a live scene (the last one is remembered in `Project::liveScene`), on TRACK it edits the notes of the selected brick. Jamming on LIVE afterwards does not change a brick and editing a brick does not change the live scene. The header arrows are PATTERN on LIVE and BRICK n/m on TRACK (`[` `]` do the same); the brick tile and the inspector show the brick number, no scene number.
- **`Project::patterns` is a heap-backed `PatternBank`** (a Project stays small enough for the stack; copy only).
- **Format 5:** the file writes the 16 live scenes and only the brick slots a brick points at; `currentPattern` is the live scene to open on. **Old files convert on load:** every distinct live scene used by a brick gets one hidden copy (bricks that shared a scene still share), the live scenes stay as they were. Nothing is changed on disk until you save. Format 5 is not readable by older programs.
- Kit / sound copy commands ("all scenes", "scenes of the chain", MATCH LEVEL) now go through the scenes in use (live scenes + bricks); the lists of scenes in the instrument menu offer the live scenes only.
- The scene pads on LIVE no longer show "in the chain" (a brick is a copy now); "Add to chain / Remove from chain" in the pad menu became "Throw a copy into the chain".
- Tests: 20 new checks (slot bookkeeping, 64 bricks fill 64 slots, a brick sounds after its live scene was emptied, save / load of slots and sharing, a format 4 file converted, source ratchets for the tab / stepper); the throw / bricks / player / copy tests were rewritten for slots.
- Not done (S1b, v44.23): keeping the track's sound at THROW, "take this scene's sound", "Update from LIVE", the "where am I" colours, LIVE not changing the track. Sound still travels with the copied scene.

## v44.21

Free points on the dynamics and one DJ filter (U3b of the interaction plan). The sound of what already existed does not change: two points are exactly the line the step always had, and the audio audit is unchanged.

- **Lanes with free points** (`Lane` in `core/ArrangeTypes.h`): the MIX level, the filter and the level of every instrument are now lines of up to 8 points over a brick (the two ends and up to 6 in between, anywhere). Click on empty space in a lane to add a point, drag it (inner points move sideways too, never past their neighbours), double-click an inner point to remove it, Option + click puts a point back to 0, Shift drags five times finer, Esc cancels the drag (one gesture = one undo step). A lane that is 0 everywhere is stored as nothing.
- **One look for the rows:** under the bricks, MIX, FILTER and the instrument you have selected (the cursor track) are drawn and edited by one piece of code. Right-click gives the ready-made shapes (BUILD, BREAK, FADE ... for the brick; FADE IN, SWELL ... for an instrument). The fill and transition levels stay single numbers in the inspector (a curve means nothing for them).
- **One DJ filter instead of HPF and LPF:** a single lane from -100 (low-pass closes, darkens) through 0 (open) to +100 (high-pass, thins from below). The line is violet below the middle and amber above it. The roles (INTRO, BUILD, ...) and the six shapes keep their sound. The performer's live HPF / LPF pads are not changed.
- **Old projects convert on load** (nothing is changed on disk until you save): the two ends of the old level lane and instrument lanes become two-point lanes; at each end the stronger of the old HPF / LPF becomes the filter value (HPF 0 -> 60 with LPF 40 -> 0 becomes -40 -> +60). Both filters at once cannot be said with one knob, so a project that used them together will sound a little different. The file format is 4 now (keys `dyn<N>`, `lv2<N>`; older programs refuse to open it).
- The CHAIN STEP window shows the two ends of MIX and FILTER as numbers; the DYNAMICS preset list is the same.
- Tests: 31 new checks (lane arithmetic, the six edges of a point, the sound of a dip and of the filter lane on a real sequencer, free points through save / load, an older project, one editor for three rows).
- Not done: the points are tied to the brick (they move and go with it, as decided); a point cannot sit between two bricks. The rows are about 17 px high on a 900 px window (more on a taller one).

## v44.20

One look for lists, small actions and short options (U3a of the interaction plan). Interface only: no sound change (audio audit unchanged).

- **Lists:** every drop-down of the program now opens through one function (`ChoiceBegin` / `Choice` in `ValueField.h`); nothing in `ui_desktop` calls `ImGui::BeginCombo` any more (a test counts it). The 21 lists keep their items and widths, so the next change of their look is made in one place.
- **Short exclusive options are `Segmented`:** LAUNCH (NOW / BEAT / BAR) in the header and "bars before" of a transition (1 / 2 / 4 / 8) in the brick inspector are buttons side by side, one click instead of open-and-pick. `Segmented` is now one item for the hint line, so the info line explains the whole row, not only the last button. The header got a little tighter (14 -> 10 px gaps) so UNDO still fits at 1440 px.
- **Click targets:** `MiniButton` replaces `ImGui::SmallButton` (10 places); every small action is at least `kMinTarget` = 21 px high. A test forbids `SmallButton`.
- **Decided to move to U3b:** one look for the rows (MIX, FILTER, an instrument, fill, transition). The row editor is rewritten there anyway for free points and the DJ filter, so doing it now would mean doing it twice.
- Tests: 4 new checks. Not done on purpose: converting the longer lists to a different widget (they stay lists), moving PATTERN `<` `>` (S1 / S2).

## v44.19

A real menu bar and a Library folder (U8c of the interaction plan). The FILE drawer is gone; everything that is not a performance control is in the menus at the top of the window, as in every Mac program. No sound change (audio audit unchanged).

- **Menu bar** (`ui_desktop/MenuBar.{h,cpp}`): **File** (New, Open..., Open Recent, Save, Save As..., Rename..., Duplicate..., Revert to Saved, Browse Versions..., Export, Show in Finder, Quit), **Edit** (Undo, Redo, Copy / Paste Track, Copy / Paste Pattern), **View** (LIVE / TRACK, Interface Size, Gestures and Shortcuts), **Library** (My Beats..., Show Library in Finder), **Settings** (Restart Audio, Show Data Folder in Finder). Every item shows its shortcut (Cmd+ on macOS, Ctrl+ elsewhere) and says in the info line what it does. Undo / Redo / Paste / Revert are greyed out when there is nothing to do.
- **Open Recent** is a submenu (ten newest projects, the open one is ticked). **Browse Versions...** is a window with the dated copies and a confirmation before one replaces the project.
- **One Export menu:** Record Stems, Record MIDI (both tick while running, and the File menu and the menu bar turn red with "RECORDING STEMS" / "CAPTURING MIDI" so a running capture is never forgotten), Notes as Text, Show Recordings, Show Last File.
- **Removed as duplicates:** MASTER WAV (REC in the header is the one master recording), the INTERFACE SIZE and RESTART AUDIO blocks of the old drawer (now View and Settings), the drum-beat list in the drawer (now Library > My Beats), the RECENT list and the BACKUPS fold-out (now submenu and window). `FilePanel.{h,cpp}` are deleted. MASTER FX stays in the tab bar.
- **Library folder:** saved beats, synth presets and the favourites of the browsers now live in `Documents/TR-Brick/Library/` (`Beats`, `Presets`, `Favorites`), next to Projects and Recordings, so they survive updates and can be copied to another computer. The first start copies what older versions kept in `patterns/` and `presets/` next to the program (nothing is deleted; marker `Library/.migrated`).
- Tests: 14 new checks (library folders, migration, each menu once, every command in one place only, no second master recording button, Undo / Redo enablement).
- Not done yet: user-made kits still sit next to the program (`kits/mykit*`), the factory kits stay there on purpose; a native file picker for Open; Rename does not move old dated backups; combo boxes are not yet `Choice` (U3).

## v44.18

The document model of Logic / GarageBand (U8b of the interaction plan): explicit Save, "Edited", an Untitled project, a recovery copy and the question "Save / Don't Save / Cancel". No sound change (audio audit unchanged).

- **Cmd+S saves, and only Cmd+S.** Before, the program wrote over the project file every 30 seconds and again at quit, so a mistake could not be taken back by closing without saving. Now the file changes only when you save. Shift+Cmd+S = Save As (new name, you go on in the new file, the old one stays as it was saved; a name that exists gets "(2)", nothing is ever replaced).
- **"Edited"** in the window title and in FILE. It is a comparison of the open project with the saved one (not a flag set by every edit), so undoing back to the saved state clears it, and looking at another scene is not an edit.
- **Untitled.** A new project (Cmd+N, or the very first start) is "Untitled" and lives in the service folder (`.../TR-Brick/Untitled`) until Save asks for a name; only saved projects appear in the Projects folder. A closed Untitled with work in it comes back at the next start (and counts as edited).
- **Recovery copy:** while there are unsaved changes the program writes a copy every 10 seconds into `.../TR-Brick/Recovery/<name>.t8proj` (never over your file). After a crash the next start asks "TR-Brick did not close properly: RESTORE / DISCARD"; Save, Don't Save and a clean quit remove the copy.
- **Save / Don't Save / Cancel** when something is unsaved and you quit (window close, Cmd+Q, FILE > QUIT), make a new project (Cmd+N), open another (Cmd+O, RECENT, a `.t8proj` dropped on the window) or revert. For an Untitled project "Save" first asks for a name, then goes on with what you wanted.
- **Cmd+O** opens a list of the projects (newest first, with date); **Cmd+N** a new project. **FILE** now has NEW, OPEN..., SAVE, SAVE AS..., RENAME..., DUPLICATE..., REVERT (back to the last saved version) and QUIT; BACKUPS (earlier versions) stay.
- **Removed as duplicates:** the three inline forms NEW PROJECT, SAVE AS and OPEN (each had its own name field and button), "SAVE & QUIT" with its own confirmation (Cmd+Q asks only when needed).
- `SaveProject(..., keepHistory)`: a plain write for recovery copies (no dated backup, no `.bak`); `ProjectToString`. `AppState::RequestDoc` is the one door for everything that leaves the open document.
- Tests: 28 new checks (Edited, Untitled, Save As, Rename, Duplicate, Revert, recovery, every answer of the dialog, "autosave never touches the project file").
- Not done yet (U8c): the real File / Edit / View / Settings menu row, Open Recent as a menu, Browse Versions as a window; Rename does not move the old dated backups (they stay under the old name).

## v44.17

Hints and cursors (U2 of the interaction plan). One place explains the program, the same on every tab. No sound change (audio audit unchanged).

- **Info line** at the bottom of the window (`ui_desktop/InfoLine.h`, like Quick Help in Logic or Info View in Live): whatever is under the mouse says there what it does, and for a number field or a fader the gestures that work on it follow in amber (drag, Shift, wheel, double-click, Option+click). All 167 pop-up tooltips moved into it (`Hint(...)` in place of `ImGui::SetTooltip`); there are no more tooltips that hide the next control or appear late. Texts that listed the gestures of a number field were shortened, the line adds them itself. The line keeps the last text for a quarter of a second so it does not flicker between two controls; with nothing under the mouse it shows a short reminder. The status messages (SAVED ..., UNDO ...) stay at the right of the header.
- **Value bubbles stay** as small tooltips only where a number follows the drag (`ValueTip`: brick beats and level while dragging, spectrum readout, step velocity / nudge, "copy step", knobs).
- **One cursor rule:** the hand on everything that can be pressed; resize arrows on number fields and faders; the text cursor in fields. Lists and menus keep the arrow (as in macOS menus).
- **"?" screen** (`?` or F1, Esc closes): the gestures of numbers, editing and keys on one screen (only what works today).
- The layout gives 44 px to the info line; the rows of the sequencer grid and the context block are a little lower on a 900 px window.
- Tests: no `ImGui::SetTooltip` may appear in `ui_desktop`.
- Not done on purpose: target sizes (all buttons are already 21 px or more; a fixed rule comes with U3 when the rows are redrawn), and removal of tooltips that duplicate a label (the usage counter of U9 decides).

## v44.16

One number control on every tab (U1b of the interaction plan). Every slider, drag box, checkbox and +/- pair of the program is now `ValueField` / `Check`; the same gestures work everywhere (drag, Shift = fine, wheel, double-click to type, Option+click = reset, Esc during a drag). No sound change (audio audit unchanged).

- **Replaced (34 places):** mixer faders (volume with the level meter, pan with the centre notch), master fader, the volume / pan pair of the piano roll, the track fader of the lanes, HUMANIZE amount (piano roll and lanes), pattern and track SWING, GROOVE amount, EUCLID hits / steps / rotate, rhythm min / max notes, the step popup (velocity, nudge, ACCENT / GHOST / TIE as lit buttons), chord 7ths, FX PING-PONG and sidechain ENABLED, the brick level fields in dB (fill, transition, per-instrument start / end, in the inspector, the right-click menus and the step panel), TRIM steps, filter percent.
- **Faders** (`BarSpec`): a drag moves the value 1:1 along the bar; a click without movement changes nothing (before, a click jumped to the mouse). Right-click no longer resets: Option+click does, like everywhere else. Typing a percent works (double-click).
- **dB fields** (`DbSpec`): one look, the lowest value reads "off", coalesced undo.
- **Removed as duplicates:** the chord LENGTH buttons `-` `+` `x2` `/2` (one field now), the old `HSlider`, the REPEATS `-` / `+` (done in v44.15). The header PATTERN `<` `>` stays for now: on TRACK it is the only scene switch (decided together with the S1 / S2 steps).
- **Ratchet is 0:** the self-test fails if a raw `Slider* / Drag* / Checkbox / RadioButton / Input(Int|Float) / HSlider(` appears in `ui_desktop` again.
- Still the same as in v44.15: the timing check "thick synths < 50 % of real time" depends on the machine (55-62 % on this build machine).

## v44.15

The common base of the interaction model (U1a). One number control and one undo/redo for the whole program; the first four fields use it, the rest follow in U1b. No sound change (audio audit unchanged).

- **`ValueField`** (`ui_desktop/ValueField.h`, rules in `ui_desktop/Interaction.h`): drag horizontally or vertically to change a value (3 px before it counts as a drag, so a click does nothing), Shift = fine (x0.1), mouse wheel = one step (Shift = fine), double-click = type the number (Enter or a click elsewhere applies, Esc cancels), Option(Alt)+click = back to the default, Esc during a drag puts the value back. Hover lights the field and shows the resize cursor; a thin bar along the bottom shows the position. Result flags `started / changed / finished / cancelled` mean one undo step per gesture (before, BPM and SWING pushed an undo step on every frame of a drag).
- **`Segmented`** and **`Choice`** (same header): the one look for "a few exclusive options" and for "pick from a list".
- **First adopters:** header BPM (20-300, 0.5 per pixel, wheel 1, Option+click = 120), LEN (still applied when the gesture ends, so growing a pattern tiles it once), SWING (0-75 %), and REPEATS of a brick (1-99; replaces the `-` / `+` buttons).
- **Redo:** Shift+Cmd+Z (also Cmd+Y). Undo now moves the replaced state to a redo stack; any new edit clears it; a scene's redo steps wait for that scene. `AppState::ClearUndo()`, `DropLastUndo()`.
- **Ratchet test:** the self-test counts raw `SliderInt / SliderFloat / DragInt / DragFloat / Checkbox / HSlider(` calls in `ui_desktop` (34 now, limit 34); U1b brings the limit to 0, and nobody can add a new raw widget in the meantime. New tests for the drag / wheel / typing rules and for redo (pattern, sound, other scene).
- Known: the self-test "thick synths ... < 50 % of real time" is a timing check and fails on slower machines (55-62 % on the build machine of this release); the sound itself is unchanged.

## v44.14

Where the files are (U8a of the interaction plan; author's question: "when the program starts from the icon I do not understand where the projects and recordings are saved"). The decisions about scenes / bricks / sound (LIVE is a sketch pad, bricks are independent copies, instrument sound for the whole track with an own sound per brick on request) are recorded in `docs/INTERACTION_MODEL.md` section 13 and `docs/ROADMAP.md` (steps S1 and S2); this release only moves the files.

- **User folders** (`core/UserDirs.h/.cpp`): `~/Documents/TR-Brick/Projects` (your `.t8proj` files), `~/Documents/TR-Brick/Recordings` (every recording and export) and, for the program itself, `~/Library/Application Support/TR-Brick/Backups` (dated copies of projects; `~/.local/share/TR-Brick` on Linux). `TRB_DATA_DIR` / `TRB_SERVICE_DIR` override the places. If the folders cannot be created the program falls back to the old relative folders, so saving never stops working.
- **One-time migration** at the first start: `project.t8proj`, `projects/`, `recordings/`, `exports/` and `backups/` of the program folder are copied (never moved, never overwritten); `take_<number>.wav` and the like get readable names. A `.migrated` marker keeps it to once.
- **Readable names:** recordings and exports are called `<project> 2026-10-26 14.32.wav`, `... stems.zip`, `... capture.mid` / `.txt`, `... notes.txt` (a second one in the same minute gets ` 2`); the status line shows the short path (`Recordings/...`). Project names may be in any alphabet (before, Ukrainian letters were dropped and the name became "PROJECT"; a new project without a name is "Untitled").
- **Start with the last project.** Before, a start without `--project` was always a blank project that autosaved over `project.t8proj`; now the program continues with the project that was open last (`lastproject=` in `ui_settings.cfg`), a first start opens the migrated `project.t8proj` or a new "Untitled". The window is named after the open project.
- **Backups are per project** and sit in the service folder (before: next to the project, all projects mixed in one list). "a" no longer claims the backups of "a-b".
- **FILE popover:** SHOW IN FINDER (the open project), "Saved in ~/Documents/TR-Brick/Recordings" with SHOW RECORDINGS and SHOW LAST FILE (`open -R` on macOS, the folder in the file manager on Linux).
- **Update script:** `ui_settings.cfg` is no longer overwritten by an update (it now holds the last project).
- Tests: the folders, name rules (Ukrainian kept, separators dropped, UTF-8 safe cut), recording names and collisions, the project list, migration (copies, readable names, originals kept, once only), per-project backups. The self-test now works in a scratch folder and never touches the real Documents. Audio audit unchanged.
- Known: kits you drop in (`kits/mykit`), custom beats and synth presets still live next to the program (they move with the library in U8c); the Mac first start may ask once for access to Documents; Mac not tested.

## v44.13

Dynamics for every instrument, for the transition and for the fill (author's request after v44.10.2, repeated after v44.11: "the dynamics are needed on each instrument, and on the transitions and the fills").

- **Data** (`StepLevels` in `ArrangeTypes.h`, `ChainStep::levels`): for each of the 10 instruments a level START and END (dB, -48 = silent, 0 = untouched, up to +6) over everything the brick plays (all repeats, the same linear-in-dB line as the mix level), plus one level for the transition that leads INTO the brick and one for the hits of its fill. Saved in the project as `lvlN=` lines next to `arrN=` (written only when something is set; a file from before v44.13 loads with no levels). Copy as new scene, repeat same and reset step carry / clear them with the brick.
- **Engine.** Every audio block the sequencer publishes the gain of each instrument (`Mixer::arrTrackGain`); `Mixer::MixTrack` multiplies it into the track's own mute ramp, so a fade never steps and the performer's faders and M / S buttons stay independent. The transition level is a gain on the riser voice (`RiserPlayer::TriggerAligned(..., gain)`, armed with `SetPendingRiser(..., gain)`); the fill level scales the velocity of the roll hits and the level of the break hits of a fill (a DROP OUT fill has no hits to scale). Stop / song off release everything back to 1.
- **WHO PLAYS table:** a cell with a level shape is drawn as a bar that rises or falls with the level over the brick. Shift + drag up / down on a cell sets the level (left half of the cell = start, right half = end, shown in a tooltip while dragging); right-click opens START / END sliders and shapes (FLAT, FADE IN, FADE OUT, SWELL, DIP, QUIETER, MUCH QUIETER, and "same shape on all bricks of this scene"). Plain click / drag still switches an instrument off / on.
- **Inspector row 3:** LEVEL OF (an instrument; a star marks the ones with a shape), START, END, FLAT / FADE IN / FADE OUT, FILL LEVEL, TRANSITION LEVEL. The inspector grew from 74 to 104 px (the TRACK strip and the editor share the rest).
- **Fills / transition lane:** a block is drawn paler the quieter its level; the right-click menu of a fill and of a transition has a level slider.
- Tests: the data (flat lane, fade, clamping), save / load and older file, setters, an instrument fading while another keeps its level, a silenced instrument, the transition at 0 / -6 / off, the fill at 0 / -12 / off. Audio audit unchanged.
- Known: the levels are a straight line over a brick (free points over the whole track come in v44.14 with the DJ filter); the amber / violet filter lines are still not draggable; Mac not tested.

## v44.12

Start flow and a static layout (author's feedback after v44.11: the program should open on LIVE, jam first, THROW INTO THE CHAIN leads to TRACK; windows "float" and get squeezed / stretched; TRACK does not need the RANDOM / KIT and LIVE blocks).

- **Start on LIVE** (`uiTabRequest = 2`); the tab order is now LIVE, TRACK. **THROW INTO THE CHAIN** sits at the right of the scene pads on LIVE (amber); a throw adds the brick, selects it and opens TRACK. The button is gone from the TRACK top bar (TRACK empty state says to go to LIVE or use + on the strip).
- **Static layout.** Every block has a fixed height and nothing is toggled away any more: LIVE = header 42, scenes + chords 92, scene editor (grid | mixer; the only flexible part, its rows are 18..34 px), instrument block (fixed 214, gets the leftover on a tall window), RANDOM / KIT + LIVE 132. TRACK = header 42, strip (fixed 240..340 px, 320 on a normal window), inspector 74, scene editor (rows up to 30 px). Removed: the START HERE card and its slim bar, SHOW INSTRUMENT SETTINGS / HIDE, the scene row collapse triangle, HIDE MIX. STARTER GROOVE shows in the footer line of the grid when the scene is empty (replacing the hint, the line keeps its height).
- **TRACK has no RANDOM / KIT, no LIVE block and no instrument block** (sound design stays on LIVE); the freed height went to the strip, so the WHO PLAYS table has readable rows on a 795 px window.
- No engine change; selftest passes; audio audit unchanged.

## v44.11

The inspector (author's go-ahead after v44.10.2: "an inspector under the selected brick", with START HERE removed but THROW INTO THE CHAIN kept in plain sight).

- **Inspector**: two rows between the strip and the scene editor, always about the selected brick (`BricksPanel::DrawInspector`). Row 1: ROLE, REPEATS (- / +), TRIM (FULL / 1 / 2 / 4 / 8 bars), WHO PLAYS (EVERYONE, NO KICK + BASS, NO DRUMS, NO BASS, KICK + BASS ONLY, CHORD ONLY, CUSTOM), DYNAMICS (the presets of v44.10.2). Row 2: FILL (scene's / none / own kind, and for an own fill WHEN and LENGTH), TRANSITION IN (sound and how many bars before; disabled on the first brick), COPY AS NEW SCENE, REPEAT SAME (the old "Duplicate"), REMOVE. Every control has a tooltip and the same undo behaviour as before (`PushSoundUndo`, `PushUndoForScene` for the role). No popup is needed any more; "Step settings" stays for "Fine-tune numbers...".
- **Top bar of TRACK**: REPEATS and COPY AS NEW SCENE moved into the inspector. New: an amber **THROW INTO THE CHAIN** button next to the track name (the way from jamming to structure); after it the new brick is selected. On LIVE the same button sits in the slim bar under the scene editor.
- **START HERE card removed.** What is left under the scene editor is one slim bar: STARTER GROOVE (empty scenes only), THROW INTO THE CHAIN (LIVE only), SHOW INSTRUMENT SETTINGS and a one-line hint. The freed height goes to the strip (the WHO PLAYS table now has readable rows and names on a 900 px window) and, on LIVE, to taller grid rows.
- No engine change. Audio audit unchanged; selftest passes.
- Known: per-instrument dynamics (asked after v44.10.2) are planned in v44.12 with free points and the DJ filter; the amber / violet filter lines are still not draggable; Mac not tested.

## v44.10.2

The FILLS / TRANSITION lane and readable dynamics presets (author's feedback: "I can't hit the beat when I click a transition", "I want fills and transitions visible under the bricks and editable", "I don't understand the filter presets in the dynamics block").

- **FILLS / TRANSITION lane** under the bricks (two thin rows, aligned with the bars). FILLS: an orange block on the last beats of a brick where its drum fill plays (the scene's fill, or the brick's own). TRANSITIONS: a violet block with the sound name, ending at a white line on the downbeat of the brick it leads into.
  - Drag the left edge of a transition: it snaps to 1 / 2 / 4 / 8 bars before the brick (no clicking in time needed; the transition is back-timed so its peak lands exactly on the downbeat). Drag the left edge of a fill: 1 / 2 / 4 / 8 beats.
  - Click an empty slot to add (a transition from the role habit, a fill from the scene's fill); right-click: sound / start / kind / when / length / none. Brick 1 has no transition slot (nothing leads into it, only on wrap).
  - A fill longer than one pass of the brick is drawn clamped to the pass.
- **Dynamics presets.** The presets (FLAT, BUILD, BREAK, DROP IN, FADE OUT, FADE IN) used to exist only in the Step settings window, while the DYNAMICS block on the strip was only a drawing. Now: right-click the DYNAMICS block of a brick = the same presets with a one-line explanation each (and "Fine-tune numbers..." for the sliders); every brick's column is labelled with the shape it has (the exact preset name, or what it does: DARK, OPENS, DARKENS, THINS OUT, FILLS OUT, SWELL, DIP, FADE IN / OUT, CUSTOM); a hover tooltip says what teal / amber / violet are. One table (`DynPresets.h`) serves both places.
- Tests: preset table, names of preset and hand-made shapes. Audio audit unchanged.
- Known: the amber / violet filter lines are still not draggable (v44.12 replaces HPF + LPF with one DJ filter on free points); Step settings is still a popup (v44.11); the jump from the ruler goes to the start of a brick.

## v44.10.1

Repeats field and zoom for a long track (author's request after v44.10).

- **REPEATS** (top bar of TRACK): `-` / `+` and the number for the selected brick, 1..99 (`AppState::SetBrickRepeats`; a trimmed brick shows "trim" and `+` makes it full length). The same number is written on every brick (`x4`, `x8`), where the old LIVE chain row had its stepper. Dragging the right edge still does the same thing.
- **Zoom.** `-` / `+` buttons, FIT (the whole track in the window), BRICK (the selected brick fills ~60% of the window), Ctrl / Cmd + mouse wheel over the strip (zooms around the mouse), plain wheel / trackpad scrolls sideways. 3..240 px per bar (it was 18..72, so a long track is fit as a whole now).
- **Overview.** When the track is wider than the window, a thin map of the whole track appears in the top bar: role colours, the window as a frame, the playhead; click or drag to move the window.
- Labels on bricks no longer collide at narrow widths (the M / F / T / L / D letters are hidden when they would overlap the number and xN).
- Tests: the repeats field (set, same value, limits 1..99, trimmed -> full, bad index). Audio audit unchanged.

## v44.10

One screen and one player. The author's feedback on v44.9: the same chain lived in two places (LIVE and TRACK) with different features, there was no real player (everything looped, no start from the beginning, no start from a chosen place), a brick could not be copied as a new scene from TRACK, and the work needed too many trips between tabs and menus.

- **One screen.** The TRACK tab is now the whole working screen: the bricks strip on top, the scene of the selected brick below (grid, mixer / piano roll, instrument block, RANDOM / KIT + LIVE). Selecting a brick selects its scene; selecting a scene elsewhere (pads, PATTERN arrows) selects its brick. No tab trips to edit a brick. The scene editor is one shared layout (`drawSceneEditor` in `main.cpp`) used by both tabs.
- **The chain row is gone from LIVE.** The chain is edited only on TRACK. LIVE keeps the scene pads (launch, swap, role / fill menus) and the chord bank. The header button that was called CHAIN (master compressor chain) is renamed MASTER, so "chain" means one thing only.
- **Player** (`AppState::TransportToggle`, `PlayFromBrick`, `PlayTrackFromStart`, `Sequencer::PlayChainFrom`). PLAY / Space: mode BRICK loops the brick you are on (scene + what the brick adds: mutes, fill, dynamics); mode TRACK runs the bricks in order from the selected one and stops after the last (LOOP wraps instead). FROM START / Home: the whole track from the first brick, also in the middle of playing (a clean restart: held notes are released). Click the ruler above a brick, or double-click a brick: play from there. In BRICK mode a click on another brick while playing moves the loop. Playhead across the whole strip. When playback stops (also by itself at the end) song mode is released (`TickTransport`), so the editor and the audio mixer are on the same scene again.
- **Copy as a new scene** (button and right-click menu): the brick's scene is copied into the next free scene and a new brick with the copy is put right after the original, so a variation can be changed without touching the first. "Duplicate" in the menu still repeats the SAME scene. Two undo steps (chain, then the scene slot).
- **Strip** is compact (the table "who plays" and the dynamics line fit under the bricks; HIDE MIX folds them, they fold by themselves on short windows). When the rows are too thin to carry names, the name of the row under the mouse is shown.
- **Sequencer:** `SetSongModeActive(active, startPos)`, `PlayChainFrom`, `SetSongLoopStep`, `SetSongRunOnce`. Without these flags the old behaviour is unchanged.
- **Tests:** TRACK from a brick, moves on, stops after the last brick, song mode released, LOOP wraps, BRICK loops one brick, FROM START in the middle of playing, Space stops, Space in BRICK mode, a scene outside the track plays as the classic loop, copy as scene (notes, independence, bad indexes). Audio audit unchanged.
- Known: dynamics are still two points per brick and the filters are still edited in Step settings (v44.12); Step settings is still a popup (v44.11); the jump from the ruler goes to the start of a brick, not to a bar.

## v44.9

The bricks screen: the whole track as ONE strip. It is the main screen now (new first tab TRACK).

- **TRACK tab** (`ui_desktop/BricksPanel.h/.cpp`), a front-end onto `Project::songChain`; the engine and the file format did not change.
  - **Bricks.** One brick per chain step; width = length in bars, colour = role of its scene, name = scene. Drag the right edge to change the length (whole scene passes when at least one scene long, trim in steps below; Shift = fine, steps). Drag a brick to move it. Double-click opens its scene in the LIVE tab. Right-click: role, duplicate, remove, step settings. `+` adds a brick, Delete removes the selected one.
  - **Who plays.** Table under the bricks: instrument x brick. Click or drag over cells to switch instruments off and on for that brick (`ChainStep::muteMask`).
  - **Dynamics.** One line under the table: level at the start and the end of each brick (drag the dots; dB, -48 = silence), HPF / LPF sweeps as thin lines (edited in Step settings).
  - Buttons: PLAY TRACK (song mode), FILL FROM ROLES (`AutoArrangeChain`), EDIT SCENE.
- **AppState actions** (one undo step each): `MoveChainStep`, `RemoveChainStep`, `DuplicateChainStep`, `AddChainStep`, `SetBrickBars`, `SetBrickLevel`, `SetBrickTrackMuted`; `uiTabRequest` lets any panel switch tabs.
- **First launch** puts the starter groove into scene 1 and one brick in the chain, so TRACK is not empty.
- `ChainPanel` exposes `OpenStepEditor()` / `DrawEditors()` so TRACK reuses the step settings window.
- **Tests:** move / remove / duplicate / add, length snapping, level clamp, mute cell, undo of each. Audio audit unchanged.
- Known: lengths snap to whole scene passes; no off-grid insert yet (v45); very long chains scroll sideways (18 px per bar minimum). Not tested on a real Mac.

## v44.8

The first sixty seconds: what a newcomer sees and does when the program opens.

- **Starter groove** (`core/StarterGroove.h`): a slow, warm one-bar groove (72 BPM, a touch of swing: kick x3, snare + clap, closed hat on the off-beats, a perc hit, three bass notes C2 / Eb2 / Bb1, a held Cm7 chord). `AppState::LoadStarterGroove()` (one undo step) writes it into the edited scene; `CurrentPatternIsEmpty()` tells when to offer it. On the very first launch of this version, a blank project starts with it already in scene 1, so Play makes a sound at once.
- **"Start here" card** (`ui_desktop/StartCard.h`). The block of instrument knobs under the grid starts folded; a card with three steps takes its place: 1 click squares or press STARTER GROOVE (only when the scene is empty), 2 press Space to play, 3 THROW INTO THE CHAIN. Clicking an instrument name or SHOW INSTRUMENT SETTINGS opens the sound controls; HIDE (next to COPY PARAMS) folds them again. The choice is remembered in `ui_settings.cfg` (`detail=`); the very first launch writes it, so the folded start happens once. Rows of the grid are a bit taller while the block is folded.
- **Throw into the chain** (`AppState::ThrowIntoChain()`): the edited scene is appended as the next brick of the chain (x1), the chain row is shown, one undo step; a full chain refuses with a status line.
- **Tests:** starter groove contents and tempo, chord, undo; throw: first brick, append, full chain. Audio audit unchanged.

## v44.7

Launch in two clicks. The slowest part of every update was not the program but the start: a new folder each time (so a full build from scratch), and macOS warnings about a "downloaded" script with a fingerprint check each time.

- **`Install TR-Brick.command`** (once). Creates `~/TR-Brick/launch.sh`, the program folder `~/TR-Brick/app` and a button `~/Desktop/TR-Brick.command`. Files made by the installer are not flagged by macOS as downloaded, so the button opens without warnings. Earlier tracks (`project.t8proj`, `projects/*.t8proj`) from older `trbrick-daw*` folders in Downloads are copied over, never overwritten.
- **`tools/mac/launch.sh`** (the button). Takes the newest `trbrick-daw-source-*.zip` from Downloads (recognised by name and checksum, so the same file is not installed twice), unpacks it, removes the quarantine flag, syncs the program into `~/TR-Brick/app` (`src`, `tools`, `third_party`, `docs`, `assets`, `examples` exactly, with deletions; kits / presets / patterns / grooves are added and updated, yours stay), keeps the build folder between versions (only changed files are recompiled), then starts the program. `project.t8proj`, `projects/`, `backups/` and `imgui.ini` are never overwritten. The launcher updates itself from the archive.
- **Clear messages.** No archive in Downloads, a broken download, missing cmake (`brew install cmake sdl2`), a failed build (the tracks stay safe).
- **Tested** on Linux with a fake home folder: first build 59 s, update with one changed source file 6 s (one file recompiled), same archive again 0.2 s, user project / extra kit / extra project survive, a file removed from the source disappears, old tracks migrate. Not tested on a Mac (no access): the first start there may need a small fix.
- `run_mac.command` stays as a fallback.

## v44.6

Arrangement dynamics and transitions: a chain step can move the whole mix over its length, and the chain can be filled from the scene roles.

- **Data.** `ChainStep::automation` (`StepAuto`, `core/ArrangeTypes.h`): level start/end in dB (-48..+6, -48 = silence) and HPF / LPF master-filter sweeps start/end (0..100 %). `StepAutoAt()` interpolates over everything the step plays (all repeats); the level is interpolated in dB so a fade sounds even. Saved on the `arrN=` line (16 values; the reader still accepts the old 10-value line).
- **Engine.** `Sequencer::ChainStepProgress()` gives the position inside the playing chain step, `UpdateAutomation()` publishes it to `Mixer::arrGain / arrHpf / arrLpf` (atomics). `AudioEngine` applies the gain after the master chain, together with the master volume, with a per-block ramp (no zipper noise); the master filter uses max(live pad, arrangement) so the performer's XY pad still works on top.
- **Roles -> habits.** `RoleAuto`, `RoleMutes`, `RoleTransition` (INTRO opens up from dark and quiet, BUILD thins out from below, BREAK is darker and loses kick + bass, OUTRO fades, the last step of the chain ends in silence; reverse crash into a DROP, downlifter into a BREAK, impact into a BUILD after a BREAK). `core/ArrangeAuto.h`: `AutoArrangeChain(project, replace)`.
- **UI.** CHAIN STEP window: new DYNAMICS section (level, HPF, LPF start -> end, presets FLAT / BUILD / BREAK / DROP IN / FADE OUT / FADE IN), RESET THIS STEP clears it. Slot shows the letter `D` and its energy bar follows the level; tooltip lists the dynamics. Right-click a slot: **Fill empty steps from the roles** (steps that have nothing yet) and **Replace all from the roles**; one undo step.
- **Tests.** `dynamics` (interpolation, normalisation, gain ramp, persistence incl. old files) and `roles` (fill / replace, transitions, last-step silence) in `tools/selftest_main.cpp`. Audio audit unchanged (drift <= 0.0xx).

## v44.5.4

The sound belongs to the scene: synth and chord patches, the built-in kick, the drum kit. (Drum tune / decay / default sample were already per scene.)

- **Data.** `Pattern` now carries `synthPatches[3]`, `chordPatch`, `kick` (`KickSynthSettings`) and `kitFolder` (empty = the project kit). The synth types moved above `Pattern` in `Types.h`. `Project::baseSynthPatches` / `baseChordPatch` are only the base an old file is upgraded from; `Project::kitFolder` is the default kit; the editor and the engine never read the base. `Mixer::kickSynth` replaces `fx.kick` as what the kick voice reads (`SyncFromPattern` copies the scene's kick).
- **Engine follows the PLAYED scene.** `Sequencer::ApplySceneSound(p)` runs at every scene change (launch, chain step, song mode on / off, scene swap, undo, load): a synth / chord patch is only set when it differs (`SamePatch`, so a chain of scenes with the same sound never disturbs a sounding note), the kick goes to the mixer, the kit is activated with one atomic store.
  The editor shows and edits the EDITED scene (`AppState::EditedPatch`, `PushEditedPatchToEngine`, `EditedKick`, `PushEditedKick`); it reaches the engine only when that scene is also the played one.
- **Kits.** `DrumKit::Build()` reads a folder into a bank without making it sound (UI thread); `Activate()` swaps a bank (audio thread safe). `AppState` keeps a per-folder bank cache (`KitBank`, `SyncSceneKits` - checked every frame, builds missing banks, hands the per-scene set to `Sequencer::SetSceneBanks`, retires unused banks so the audio thread never frees one). Picking a kit (`LoadKitFolder`), the arrows and the browser now set the EDITED scene's kit; `=ALL` next to the picker (`SetKitAllScenes`) sets one kit for every scene. A dropped sample goes into a private kit of the edited scene (`AssignSampleToSlot`). Variant counts of the sample picker come from the edited scene's kit (`SceneKitVariantCount`).
- **Undo.** Sound undo entries carry the scene (`UndoEntry::patternIndex`, patches + kick of that scene); a scene snapshot restores kit and kick too.
- **Copy sound** (`CopySceneSound`, `CopySoundToScenes(src, scene | kToAll | kToChain, what)`; `what` = synth 1-3, chord, kick, drum voices, kit). Menus (`InstrumentMenu.h`): scene pad > Sound of this scene > copy everything / synth + chord patches / drum sound / only the kit to ...; a track's menu > Copy this patch (or the drum voices) to scenes. One undo entry per receiving scene.
- **Saved** as `[patternN.synthM]` / `[patternN.chordpatch]` sections, `kitFolder=` (only when the scene has its own) and `kickSynth=` in `[patternN]`; the global `[synthM]` sections hold scene 1's sound for the base. Project format 3. An older file (one sound for the project, kick in `[fx]`) gives every scene that sound and the project kit.
- Tests: engine follows launch / chain / song mode, editing a scene that is not played, per-scene undo, copy sound (chain / all / undo), save / load per scene and the upgrade of an old file. Selftest changes: kit load / drop now set the scene's kit (the drop test no longer deletes the project kit - careful, an earlier build of this stage removed `kits/electro` there).

## v44.5.3

- **Chain slots swap.** Dropping a chain slot on another one now swaps the two (like scene pads do); Shift+drop inserts it there as before (`SwapSlots` / `MoveSlot` in `ChainPanel.cpp`). Checked with a headless ImGui run that simulates the mouse drag.

## v44.5.2

Match the level of one instrument across scenes.

- `AppState::MatchInstrumentLevel(srcScene, track, onlyChain, withMix)`: takes the track's fader from the source scene and sets it in the other scenes (all 16, or only the scenes used in the chain). `withMix` also copies pan, delay / reverb sends, insert drive / EQ tilt / chorus, autopan, width / spread and the 3-band EQ + HPF (the whole mixer strip). Notes, mute and solo are not touched; scenes that already agree are skipped; the played scene's mixer follows at once. Each changed scene gets its own undo entry (`PushUndoForScene`).
- UI: `Match level in other scenes` in the same instrument menu (track name in the grid, scene pad > Instruments of this scene): fader or whole strip, to all scenes or to the chain's scenes; the menu shows the current fader value. The status line says how many scenes changed.
- Tests: chain-only vs all scenes, fader-only leaves pan / sends, mute and notes kept, whole strip, nothing-to-do case, mixer follows, undo.

## v44.5.1

Mute and solo belong to the scene again; an instrument can be copied between scenes and erased in a scene.

- **Mute / solo per scene.** Each pattern keeps its own `mute` / `solo` flags (they were always saved per pattern). `AppState::SetMuteScene` / `SetSoloScene` write the EDITED scene and, only when that scene is also the played one, the audio-thread mixer. A scene change (launch, chain advance, song mode on / off) now loads the new scene's flags: `Mixer::SyncFromPattern` is called with `keepMuteSolo = false` at every scene change (`Sequencer::ApplyScene`, `AdvanceSongChain`, `SetSongModeActive`, scene swap, `SwitchPattern`). This replaces the project-wide behaviour of v44.1-v44.5 (`SetMuteAllScenes`, removed).
  - M / S buttons, the mute pads (BASS / SYN2 / SYN3, drum groups, ALL DRUMS), MUTE ALL and DROP act on the edited scene. DROP remembers its "before the drop" state per scene (`dropActiveScene`, `dropPrevMuteScene`).
  - Undo still leaves mute / solo alone. Pasting a track (Cmd+V) keeps the destination's mute / solo. The chain-step mute mask (`arrMuteMask`, v44.5) is separate and unchanged.
- **Instruments between scenes** (`AppState::CopyInstrument(srcScene, srcTrack, dstScene, dstTrack = same)`): copies the whole track - notes (all 64 steps), own length, level / pan / sends / insert FX / EQ - and keeps the destination's name, type, mute and solo. Another destination track must be of the same kind (drum onto drum, synth onto synth, chord onto chord). A destination that is being played picks it up at once. The status line shows the scene lengths when they differ.
- **Erase an instrument** (`AppState::ClearInstrument(scene, track)`): empties every step of the track in that scene (also beyond the visible length); level, FX and length settings stay. Works for drums, synths and the chord track (with the chord bank on, the edited scene's chords live in the bank and that is what is cleared).
- Undo for these two (`PushUndoForScene`): an entry for the destination scene; Undo takes it back while that scene is the edited one.
- UI (`ui_desktop/InstrumentMenu.h`): right-click a track name in the sequencer grid > Copy to scene / Copy to another track of this scene / Erase this instrument; right-click a scene pad > Instruments of this scene > (track) > the same items, plus Copy to the edited scene.
- Tests: per-scene mute / solo with a real sequencer (song mode start, chain advance, a flag written to a scene that is not played), DROP per scene, copy (notes beyond step 16, level / sends / own length, kept mute and identity, type mismatch, onto itself, playing destination, undo per scene), erase (all steps, undo, drum, chord of another scene), save / load of per-scene flags. Two older mute-pad tests changed from "every scene" to "edited scene".

## v44.5

The chain as a score: mutes, fills, transitions and trim on a chain step, a role and a fill on a scene. (Plan: `docs/ROADMAP.md`, stage R, items R4-R7.) None of it changes the steps of a scene.

- **Chain step overlays** (`ChainStep`: `muteMask`, `fillMode` / `fill`, `riser` / `riserBars`, `trimSteps`; `core/ArrangeTypes.h`). Edited in the **CHAIN STEP** window (right-click a slot > Step settings...).
  - **Mutes:** `Mixer::arrMuteMask` (atomic) is applied in `Mixer::ShouldPlay`, so the usual ramped mute gain removes the clicks; it is set by `Sequencer::ApplyStepOverlay` when a step starts (play, song mode on, chain advance) and cleared on STOP / song mode off. The performer's M / SOLO / DROP are separate and untouched.
  - **Trim:** `Sequencer::LoopSteps()` (the pass of a trimmed step), used for the chain advance and for the early-hit lookahead. Lengths, tooltips and the chain total use `ChainStepLoopSteps / Bars / Seconds`.
  - **Transition:** the next step's riser is armed once, when no more than `riserBars` bars of the playing step are left, through the existing back-timed `SetPendingRiser`.
- **Fills** (`FillRule`: WHEN off / last loop / every loop / every N bars, LENGTH 1-8 beats, TYPE break / hat roll / snare build / drop out / random). `EvalFill` is a pure function; `Sequencer::FillAt` takes the scene's rule or the step's override. In `FireTick` the window either plays the LAST beats of a library break through the live-break path, rolls the closed hat / the snare (1..4 hits per step, velocity ramp), or silences the drums.
  A live break wins over a fill; the early-hit lookahead stays out of fill windows. Scenes carry a default fill (`Pattern::fill`), so it plays in free play too.
- **Scene role** (`Pattern::role`: none / INTRO / BUILD / DROP / BREAK / OUTRO): a colour bar on the scene pad and the colour of the energy line. `core/ArrangeEnergy.h`: `SceneEnergy(pattern, muteMask)` (weighted share of the tracks that play, 0..1) for the slot's energy line.
- **Four synthesized transition sounds** after the ten stock risers (`RiserPlayer::kMaxSounds` = 14; fixed indices 10-13): DOWNLIFTER, IMPACT, SUB DROP, REVERSE CRASH, generated at load (deterministic noise, no samples), hit point computed like the stock files. They are also in the live RISER row (`Count()` is 14).
- Saved in the project: `arrN=mutes,fillMode,when,kind,beats,everyBars,breakIndex,riser,riserBars,trim` after `stepN` (only when something is set), and `role=` / `fill=` in a pattern. Older projects load unchanged.
- Not done yet (still in the plan): roles do not set default fills / mutes; no HEAR for a transition in the step window; the energy is a bar per slot, not a curve; per-step fill length is limited to 1-8 beats.
- Tests: pure fill windows (every loop, last loop, every N bars, free play, triplet grid, clipping), save / load of the overlays, energy, the mute mask, and on a real sequencer: mutes follow the chain, trim loops after N steps, STOP lets go, the transition is armed, the four sounds render finite audio, DROP OUT silences exactly its window, HAT ROLL gets denser.

## v44.4

RHYTHM randomizer, EUCLID / ROTATE, the length of a scene in bars and of the whole chain. (Plan: `docs/ROADMAP.md`, stage R, items R1-R3.)

- **RHYTHM randomizer.** The SIMPLE / COMPLEX button of BASS / SYNTH 2 / SYNTH 3 cycles SIMPLE -> COMPLEX -> RHYTHM. RHYTHM writes a rhythmic figure with a chosen number of notes per bar (min..max, 1..16 - one note per bar is possible) instead of a melodic walk.
  Settings in the OCT window (second half): NOTES PER BAR, PITCH (ROOT / ROOT+5TH / CHORD / SCALE), LENGTH (SHORT / HELD / MIX), FIRST ON THE ONE, SAME EVERY BAR (one bar repeated or every bar different), ROLL. Notes sit on the beats more often than between them
  (`PickRhythmSteps`, step weights 6 / 3 / 1); a bar is 4 beats of the grid (12 steps on the triplet grids); a track with its own length is respected. Stored per part in the project (`rndRhythm0..2=min,max,pitch,length,onTheOne,sameEveryBar`).
  `AppState::RandomizeRhythm`, `RandomizeMelodicMode`; the chord lookup of the melodic randomizer became `ChordSoundingAt` (shared). New file `core/RhythmRandom.h`.
- **EUCLID and ROTATE** (`core/Euclid.h`, `AppState::ApplyEuclid` / `RotateTrack`, `ui_desktop/EuclidWidget.h`). EUCLID button in the lane panel (drums) and the piano roll (synths, chords): HITS / STEPS / ROTATE with a preview, presets 3-3-2, 5/8, 5/16, 7/16, 9/16, 11/16, 4/16;
  the hits copy the first existing hit of the track (key root on an empty synth track); not for the chord track. ROTATE TRACK (`< 1`, `1 >`, `< BEAT`, `BEAT >`) slides all steps round the track length, chords too. One undo step each.
- **Length in bars.** `PatternBeats / PatternBars / PatternSeconds` (Types.h). LEN shows the bars next to the field and in its tooltip; a chain slot's tooltip shows scene x repeats in bars and seconds; the chain panel shows the length of the whole chain (bars and m:ss, each slot at its scene's length, repeats and tempo).
  The tempo of every scene is used in song mode (checked in `Sequencer::SamplesPerStep`: the playing pattern's bpm).
- Limit of 64 steps looked at, not changed: `Step` is 16 bytes, a pattern 12 KB, the project 200 KB; 256 steps would make a project about 700 KB (and the undo history about four times larger); 52 places use `kMaxSteps` (page UI, persistence, sequencer loops) and need an audit.
- Tests: rhythm (exact note counts, beat bias, one note per bar held to the end, SAME EVERY BAR identical bars, ROOT+5TH, CHORD root, own track length, triplet grid, save / load, undo), euclid (3-3-2, counts, repeat, velocity copy, undo, chord refused, key root), rotate (wrap, nudge, undo), lengths in bars.

## v44.3.1

- **Fix: LEN typed with the keyboard did nothing.** Ctrl+click the LEN field, type a number, ENTER: the pattern length stayed as it was (the typed value arrived in the same frame in which the field stopped being active and the old value was applied). The typed value is applied now; dragging works as before.

## v44.3

ALL DRUMS pad, octave sets for the random synths, FILE / MASTER FX next to LIVE, the groove window (choose, then BAKE), audio recovery after sleep.

- **ALL DRUMS pad** replaces the CHORD pad in the LIVE mute grid: kick, snare, clap, hats and perc muted together (in every scene); press again and they all return. The chord track keeps its own M button.
- **Random octaves per part.** Before, BASS used octaves 1-2 and SYNTH 2 / 3 octaves 3-5, fixed in the code. Now each part has an **OCT** button (RANDOM / KIT block, under SIMPLE / COMPLEX) that opens a small window above it: eight octave
  cells C0..C7 that you switch on and off **independently** (several at once, gaps allowed, e.g. 1 and 3), quick presets LOW 1-2 / MID 3-4 / HIGH 4-6 / WIDE 2-5, and a line "N notes of C DORIAN, from C1 to A#2". At least one octave always stays on.
  Saved with the project (`rndOctaves=`); the defaults are the old ranges, so nothing changes until you touch it. The CHORD column has no octave set (chords are built around octave 3).
- **FILE and MASTER FX** moved up into the tab row, right next to LIVE (the popovers are unchanged). The header is one row again (42 px) because room was freed; CHAIN and UNDO stay in it.
- **Groove window.** The GROOVE button in the header opens the groove window **in place of the mixer / piano roll / lanes, whatever track is selected**, so its drawing is always visible (X CLOSE or the button again brings the mixer back).
  The mechanic is the classic one again: **1 CHOOSE** a groove (or SAVE FROM TRACK / USE TRACK'S FEEL), **2 AMOUNT**, **3 BAKE** - only the bake writes it into the steps of the tracks marked in BAKE INTO. Choosing alone changes nothing you hear; after the bake
  nothing stays chosen (no double bake by accident); one undo step. The velocity / timing shape is drawn with the mouse right there. RESET (next to GROOVE) still resets swing, track swings and the chosen groove at any time.
  The GROOVE tab and its buttons are gone from the per-track LANES panel (it keeps VELOCITY / NUDGE / SWING). A live groove layer written by an older version keeps playing until RESET or BAKE.
- **Audio recovery (bug: after the laptop slept, PLAY moved the playhead but nothing was heard).** A watchdog in the main loop now reopens the sound card and clears the effect state when it sees: a long pause (wake from sleep), an output device added / removed
  (headphones, Bluetooth, display speakers), the audio callback standing still, or a device that stopped running; **FILE > AUDIO > RESTART AUDIO** does it by hand. The engine also never hands a NaN / infinity to the limiter or the sound card any more (such a value
  stuck in the delay / reverb / master chain makes the output silent for good); it clears the effects and says so in the status line.
- Tests: ALL DRUMS, groove choose / bake / undo / reset, octave sets (gap set, range = old band, every random note inside the chosen octaves, save / load, defaults), NaN guard and recovery.

## v44.2

Scales, silent selecting, LEN that repeats, triplets that keep the tempo, groove + RESET in the header.

- **Scales.** The SCALE list now has explicit **MAJOR (IONIAN)** and **MINOR (AEOLIAN)** at the top, then the modes, then **HARMONIC MINOR, MELODIC MINOR, PENTATONIC MAJOR, PENTATONIC MINOR, BLUES**, then CHROMATIC
  (13 scales; the stored index of every old scale is unchanged, so old projects load as before; the menu order is separate: `kScaleMenuOrder`). The list is shown whole, without scrolling. **Chords** need seven degrees, so the
  pentatonic / blues / chromatic scales are harmonised from their parent major / minor scale (`ChordScaleFor`); HARMONIC MINOR gives V = MAJOR and III = AUGMENTED, as it should. The randomizers use the chosen scale itself,
  and with CHORDS ON the strong beats still land on chord tones. Tests: every scale x key (notes inside the scale, snapping, chords sorted and inside the parent scale, quality labels, randomizer output).
- **Selecting is silent.** Picking a synth preset or a kit (click, arrows, the `<` `>` buttons) no longer plays anything, so you can choose during a live set. In the browser windows there is a **HEAR** button
  (and the **Space** key) to listen to the selected card without picking it, and an **AUTO** switch (off by default) that plays every card you click or browse to.
- **LEN repeats the pattern.** Making the pattern longer copies what was there over the new part (16 -> 64 = four copies; drums, accents, synths and the chord track with their chords). Only tracks that follow the master
  length and have nothing written in the new part are filled; tracks with their own length are left alone; shrinking just moves the end, the steps behind it stay. The value is applied when the drag / typing ends. One undo step.
- **Triplet grids keep the tempo.** Switching the GRID (any pair of 1/16, 1/32, 1/16T, 1/8T) now moves every note to the nearest step **in time** (before, the triplet grids only re-read the steps, which made the pattern sound
  faster or slower). Quarter and eighth notes land exactly, 16ths between the triplet steps snap to the nearest one; when two notes land on one cell the second goes to the free neighbour, and only if there is none are they
  merged (the status line shows the count). Note lengths and nudges scale with the step size; the length keeps its bars.
- **GROOVE moved to the header**, next to SWING: groove picker, amount and a **RESET** button that is always there (dimmed when the pattern is already straight): swing 0, all tracks back on the pattern swing, groove off.
  Undoable. The groove shape, the track chips, BAKE and SAVE stay in LANES > GROOVE. The header has two rows now (transport and musical settings on top; FILE, MASTER FX, CHAIN, UNDO and the meters below).
- Tests: grid conversion in time (timing of the converted pattern, quarter notes on both triplet grids, collisions), LEN (tiling, chords, own-length tracks, shrink / regrow, undo), RESET, silent kit load.

## v44.1

Big browsers for kits and beats, mute pads with DROP, one layout for drum and synth panels.

- **Kit browser and beat browser** - the same big window as the synth preset browser (categories with counts, search, tag chips, card grid, favourites with the star). KIT and BEAT in the
  bottom strip are now a wide name button (opens the window) between two arrows. **Kits**: categories CLASSIC / ELECTRONIC / HARD / GROOVE / MY KITS (a folder you add to `kits/` lands there),
  tags such as SUB, PUNCHY, DRIVEN, LONG; a click loads the kit and plays a short kick / hat / snare phrase so you can hear it, the arrows browse the same way; the arrows next to the button load the
  neighbour kit at once (the old LOAD button is gone - what you see is what is loaded). `risers/` is no longer offered as a kit. **Beats**: the 27 typical beats in HOUSE / TECHNO / ELECTRO / BREAKS /
  HIP HOP / GROOVE plus MY BEATS (saved ones), tags 4/4, BROKEN, SWING, BUSY, SPARSE derived from the pattern; a click only selects, Enter / double-click / APPLY replaces the drum tracks (Undo brings them back).
  Favourites: `presets/favorites_kits.txt`, `presets/favorites_beats.txt`.
- **MUTE PADS in the LIVE block**: a 4 x 2 grid of big pads next to the filter pad - KICK, DRUMS, HATS, **CHORD** (new) / BASS, SYN2, SYN3, **DROP** (new). Red = muted. **DROP** mutes everything except the kick and
  the bass; press it again and every track returns to what it was before (a hat you had muted stays muted, a kick you muted stays muted). If you unmute something by hand the drop ends by itself.
  MUTE ALL stays next to STUTTER. The mute chips left the RANDOM / KIT block; CLEAR ALL moved under the BEAT row there; the random columns are a little narrower so everything fits one strip.
- **Drum panel = synth panel layout.** A drum track now has the same structure as a synth: left KIT block (kit name button + arrows, and a SAMPLE card where click / right-click change the track's default sample -
  the VARIANT cell became this card), then OUTPUT (VOLUME and PAN of the track), then the tabs VOICE / EQ / FX in exactly the place where a synth has OSC / FILTER / ... Switching between a synth and a drum track no
  longer moves the tabs to the left. VOICE holds TUNE / DECAY and, for the kick, the KICK SYNTH.
- Tests: CHORD mute (pattern, live mixer, all scenes), DROP (mutes, keeps kick + bass, restores, previous mutes survive, ends on a manual unmute), kit list without `risers`, kit descriptions for every shipped kit, kit load request.

## v44

Synth preset library rebuilt, large preset browser, master bus without distortion.

- **Audit of the 16 old synth presets.** Rendered each one the way it is played (`t8_presetaudit`): the levels were 23 dB apart (ACID BASS -26 dB RMS, STAB +-0), only 6 of 21 possible
  thickening layers (UNISON, SUB, OSC2, 24 dB, DRIVE, KEYTRACK, ACID, DRIFT) were used anywhere, the FM presets buzzed (broadband noise instead of bells / pianos) and the bass presets
  had no weight. Cause of the FM noise: the modulator is added to the phase in cycles, so level 1.0 meant a modulation index of 50 rad.
- **New library: 56 presets in 6 categories** (BASS 17, LEAD 10, PAD 9, KEYS 7, PLUCK 9, FX 4) built on the whole engine (unison, sub, second oscillator, 24 dB / ACID filter, drive, drift,
  key tracking, phase sync). Every preset has a category and its volume is set by measurement (`SynthPresetLevels.inc`, generated by `t8_presetaudit --write-levels`) so most presets inside a category
  sit within about +-1.5 dB RMS of the category target (BASS -11, LEAD -13, PAD / KEYS / PLUCK / FX -14 dB) and under -1 dBFS; the few that cannot reach it without clipping (ACID BASS -17, PLUCK -17.5, ARP PLUCK -20: very short, peaky sounds) are held at the peak ceiling. The spread over the whole library fell from 23 dB to 12 dB, and 7 dB inside any category. Bass / stab / arp presets were checked for mono compatibility. Old projects are untouched: they store the patch, not the preset name.
- **FM engine: two new rows** (`MOD RANGE` and `MOD ENV` in the FM parameters). `MOD RANGE` (`fmIndexScale`) scales the modulation depth, `MOD ENV` (`fmModEnv`) makes it follow the
  amp envelope, so bells and electric pianos get dark as they fade instead of buzzing. Defaults are the old values (8.0 and 0), so existing FM patches sound exactly as before; the new
  presets use 1.0. Saved only when they differ from the default (projects and `.t8syn` presets).
- **VOLUME range 0..2.0** (was 1.2): the acid presets needed the headroom. The UI shows the same fractions of the bar as before for old values.
- **Preset browser** (SYNTH / CHORD SOUND -> PRESET): the small drop-down is replaced by a button with the current name and category colour; it opens a large window - category list with counts
  (ALL, FAVORITES, BASS, LEAD, PAD, KEYS, PLUCK, FX, MY PRESETS), search (just start typing; it matches name, category and tags), tag chips (UNI SUB OSC2 24dB ACID LFO FM WT, derived from the
  patch) and a scrolling grid of cards. **Click = hear it** (the preset is applied and an audition note plays: a bass low, a pad as a chord), arrows move through the grid and
  apply / audition, Enter or double-click picks, Esc clears the search and then closes. The star on a card marks a favourite (`presets/favorites.txt`). Presets you save (SAVE next to the button)
  appear under MY PRESETS. `<` / `>` step through the category the browser was last left on.
- **Master bus audit** (all four chain presets, whole path chain + limiter, new `master.preset_*` rows in `t8_audit`). Found: the master saturation was `tanh(5x)` on the whole signal - an
  intermodulation distortion of -12 dB on a kick + hat mix with LOUD (the "distorted loud preset"), and a THD of -23 dB at -6 dBFS. Fixes: (1) the saturation is now a knee clipper that is exactly
  linear below the knee and rounds only the peaks above it (a slightly lower knee on the negative half = a little 2nd harmonic); (2) LOUD saturation 0.35 -> 0.25; (3) the compressor
  detector holds its peak for 14 ms before the release starts, so it no longer ripples at twice the bass frequency (that was gain modulation: -48 dB IMD with a 55 Hz tone). Measured after:
  IMD of 55 Hz + 1 kHz at -6 dBFS through LOUD -100 dB (was -12), 1 kHz THD+N under -65 dB on CLEAN / WARM / LOUD at -12 and -6 dBFS; LO-FI keeps its crunch at -6 dBFS (-49 dB). The limiter
  was clean all along (-113 dB) and nothing drops out: peaks stay inside the ceiling, a steady tone never dips.
- Tests (about 30 new): preset library (count, unique names, categories, finite, not buried, headroom, category and library spread, tags), FM scale round trip (preset file, project file, legacy default),
  master presets (THD+N, ceiling, no dropouts, LOUD IMD, knee clipper). Baselines renamed to v44 (`docs/AUDIO_BASELINE_v44.txt`).

## v43.1

Default kit, SAVE AS, rebuilt kits.

- **New projects start on the `acid_v2` kit** with a default sample per drum track: **kick 1, snare 2, clap 1, closed hat 1, open hat 1, perc 3**. The choice
  is stored per track (`Track::defaultVariant`, saved as `defaultVariant=` only when not 1). A step follows it until you set a variant on that step
  (right-click) - `Step::variant = 255` means AUTO. The VARIANT cell in the track panel now changes the track default (and sets every step back to AUTO).
  Old project files have no such key and keep playing sample 1, exactly as before. Saved beats write the variant a hit really plays.
- **SAVE AS NEW** (FILE tab): type a name, press SAVE AS (or Enter) and what is open now is written to a new project file (`projects/<name>.t8proj`, a number
  is added if the name exists) and becomes the open project. The file you were in is not touched, so work started without creating a project no longer has
  to end up in `project.t8proj`. NEW PROJECT is unchanged (empty project, current one saved first).
- **Kit audit and rebuild.** Measured every kit: the 14 classic ones were 16-bit / 44.1 kHz mono with tails cut at 20-70 % of the peak (a click at the end of every
  kick and perc), the first sample not at zero, levels 7 dB apart between kits, hats 8 dB hotter than in the new kits, one variant per slot. All 14 (606, 707, 808,
  909, acid, cr78, dub, funk, gabber, industrial, linn, lofi, minimal, trap808) are now generated by `t8_kitgen` with the same chain as the newer kits: synthesis at
  192 kHz, band-limited resample to 48 kHz / 24 bit, tail fade, drum-machine level proportions, stereo hats and claps, and 2-3 variants per slot
  (kick x2, snare x2, hat x2, perc x3), so the default sample numbers exist in every kit. Character per kit: 808 / trap808 long sine subs, 909 / acid driven kicks, 606 / 707 short and bright,
  CR-78 soft and woody, dub long kicks and spring-like snare tails, funk dry with a beater click, gabber / industrial heavily distorted / metallic,
  linn bit-reduced, lofi crushed and dusty, minimal tiny and clicky. Risers were checked and left as they are.
- Measured after the rebuild: no DC, no clipping, every file starts and ends at zero (end level < 0.1 % of the peak except the deliberately saturated gabber / industrial / trap kicks, < 2 %).
- Tests: defaults per slot, AUTO vs explicit variant, round trip, old file, SAVE AS (never overwrites, keeps the old file) - 14 new checks. One test threshold
  (soft hat darker than a loud one) relaxed because the new hats sit above 7 kHz. Baselines renamed to v43.1.
- Compiler warnings fixed: two tooltips in the LANES panel used a bare `%` in a printf-style format (now `%%`; clang warned on macOS), and a few small text buffers
  that gcc could flag as possibly truncated were enlarged.
- Not changed: the old `tools/gen_*_kit.py` scripts (history only).

## v43

Grid: 1/32 and triplets.

- **GRID selector in the transport header** (per pattern): **1/16** (default), **1/32**, **1/16T** (triplet sixteenths, 6 steps per beat) and **1/8T**
  (3 steps per beat). BPM always counts quarter notes; a bar is 4 x steps-per-beat steps. The sequencer, the piano roll and the LANES editor show
  16 columns (groups of 4) on 1/16 and 1/32 and 12 columns (groups of 3) on the triplet grids, paged as before.
- **1/16 <-> 1/32 converts the pattern**: every step index and note length doubles (or halves, odd steps dropped) and the nudge is rescaled, so it plays at
  exactly the same moments (tested). (v44.2: the triplet grids also convert in time now.) The length keeps its number of bars. Undoable.
- **Swing on 1/32** delays the odd sixteenths (two steps each); the triplet grids take no swing. **Groove** positions are sixteenths of the bar, so on 1/32 a position
  covers two steps.
- Launch quantize (beat / bar), riser bar sync, live fills and the stutter / beat repeat are counted in beats and bars of the grid (a fill is written in
  sixteenths and lands on the nearest step of a triplet grid). Typical beats and saved beats are mapped onto the pattern's grid (saved beats remember theirs: `grid=`).
- Saved per pattern as `stepsPerBeat` (only when not 4); old files are 1/16. 17 new tests.
- Baselines renamed to v43; the rolling render moved by 0.04 dB because tied bass notes (v40.2) now keep their level.
- Not converted: the chord bank scenes keep their steps when the grid changes; the random generators still think in sixteenths.

## v42

Grooves (phase 4.1, groove templates).

- **Groove = a bar of 16 steps with a timing push (+-50 % of a step) and a velocity offset for each.** It is a live layer on the pattern
  (`Pattern::groove` + `grooveAmount` 0-100 %): the sequencer adds `amount x groove` to every step of the tracks that follow it, so the amount can be
  moved while playing and the steps themselves stay untouched. Repeats every 16 steps whatever the pattern length. Negative timing pulls DRUM hits ahead
  of the grid (melodic tracks stay on the grid). Swing stays a separate parameter and adds to the groove.
- **GROOVE tab in LANES**: groove list (11 built in: ELECTRO PUSH, TECHNO DRIVE, BREAKS SHUFFLE, ACID BOUNCE, MPC 16TH 58, MPC 16TH 66 TRIPLET, LAID BACK, PUSHING,
  DRUNK, HUMAN TIGHT, FOUR FLOOR ACCENT, plus your own), AMOUNT, chips to switch the groove on / off for each of the 10 tracks, and the groove drawn as
  two boxes (VELOCITY on top, TIMING below) that you can edit with the mouse (right-click a step = flat).
- **BAKE** writes the groove at the current amount into the velocity and nudge of the steps and switches the layer off (the result plays at the same times as the live groove).
- **SAVE FROM TRACK** captures the feel of the selected track (average nudge and velocity relative to 0.8 at every position of the bar) as a new groove file in
  `grooves/*.t8groove`; **USE TRACK'S FEEL** puts it on the whole pattern without saving.
- Saved with the pattern (`groove`, `grooveAmount`, `grooveName`; tracks write `grooveOn=0` only when off). Old files load with no groove.
- `src/core/Grooves.h`; 15 new tests.

## v41

SWING tab in the LANES panel (drum tracks).

- **Third tab SWING** next to VELOCITY and NUDGE: the swing of the whole pattern (slider 0-75 %, with its MPC equivalent, quick buttons
  STRAIGHT / LIGHT 20 / MPC 58 / MPC 62 / TRIPLET) and the swing of the selected track on its own (`PATTERN` = follow the pattern, `OWN` = own value).
- **Timing preview**: the 16-step page as a timeline; a filled dot is where the hit of this track really sounds (swing + nudge), the ring is the
  straight grid position, the size is the velocity.
- The SWING field stays in the transport header too (it is global and the fastest thing to reach while jamming; the same value).
- One shared function `StepHitOffset` (Types.h) now computes the hit position for the sequencer and for the preview.

## v40.2

- **Fixed: a tied / slid note was almost silent on plucky patches** (ACID BASS and any patch with sustain near 0). A tie keeps the
  envelopes running, but the previous note had already faded, so the tied note continued at ~10 % of the level (measured: step rms
  0.004 against 0.036). Now, if the amplitude has fallen below 0.35 when a tie arrives, it is lifted back with a soft attack from the
  current level (no click); sustained pads and the endless-loop tie are untouched. All three engines (subtractive, FM, wavetable).
- **COPY PARAMS** button in the header of every instrument panel (drums, synths, chord): copies as text the synth patch (or the drum
  voice and the kick synth), the track (volume, pan, EQ, sends, steps with nudge / velocity) and the effects, ready to paste into a question.
  Text comes from the same writer as the project file (`DescribeInstrument`).
- 6 new tests.

## v40.1

Piano roll refinements.

- **Mouse wheel scrolls the pitches in ALL NOTES / SCALE: MARK / ONLY** (until now only SCALE+USED scrolled). Two semitones per notch.
- **Right-click a note: OCTAVE UP +12 / DOWN -12** (in the step popup). A synth note moves; in a chord only the clicked note moves
  (right-click from outside the roll, e.g. in the sequencer: the whole chord). Nothing moves when it would leave 0..127. The OCT + / OCT - buttons are gone.
- **HUMANIZE in the roll header** (with its amount in %): random jitter of the velocity or the timing (the lane under the notes, VEL / NUDGE, picks which).

## v40

Velocity per step, the LANES editor and drawing in the piano roll.

- **Free velocity per step** (`Step::vel`, 1-127). Not set (0) = the classic values (normal 0.8, accent 1.0, ghost 0.45), so nothing
  sounds different in existing projects. When set it wins over ACCENT / GHOST; the grid shows a value at the top as an accent and a
  very soft one as a ghost. The right-click popup has an exact VELOCITY slider (AUTO = back to the flags). Saved as an optional trailing
  step field, old files load unchanged.
- **LANES panel** replaces the mixer while a DRUM track is selected: a tall bar lane for the selected track on the 16-step page.
  **VELOCITY** and **NUDGE** tabs; press and drag across the steps to draw (fast drags interpolate, Alt-drag = straight line), right-click a
  step = back to default. Bars of never-edited steps are hatched. HUMANIZE (random jitter, amount in %) and RESET for the whole lane.
  `X MIXER` brings the mixer back, the mixer has a `LANES` button for the way back. The track volume strip stays under the lane.
- **Lane under the piano roll** for synth and chord tracks (button `LANE`): the same VEL / NUDGE bars under the notes.
- **Piano roll drawing fixed**: click-and-drag painting never worked in the roll (the pressed cell blocked the hover of the next ones);
  now you can drag across steps and pitches to draw notes, or erase a run, like in the sequencer.
- 9 new tests (defaults, priority, grid reading, the sequencer plays vel 30 softer than vel 127, persistence); `t8_selftest` passes.

## v39

Microtiming and a swing fix (phase 4.1 of `docs/AUDIO_AUDIT_AND_PLAN.md`).

- **Swing bug fixed.** The old swing made the step AFTER an odd step longer, so with swing 50 % every 16-step bar lasted 18 steps
  (the tempo slowed down) and the even steps were the late ones. Now every step has exactly the same length and swing moves the
  HITS: on every odd step a hit is delayed by swing / 2 of a step. Measured at 120 BPM, swing 50 %: bar starts at 0.000 ms error,
  odd steps 0.009 ms. Riser and fill timing, which assumed equal ticks, are exact now too.
- **NUDGE per step** (right-click a step): -50 ... +50 % of a step. Positive = laid back, negative = pushed ahead of the grid. Negative
  values work on drum tracks (the sequencer looks one step ahead and schedules the hit inside the previous step, sample-accurate, with
  the probability roll made once); synth and chord notes can be laid back only. Ratchets keep their spacing inside the shifted step.
- **GHOST** (right-click a step): a quiet in-between hit (velocity 0.45, so the velocity-tone makes it darker as well). Exclusive with ACCENT.
- **SWING per track** (bottom row of the track panel, `PAT` = follow the pattern swing): a straight kick under swung hats, or the other way round.
- The grid shows a dimmed cell with a ring for a ghost hit and a small marker at the bottom edge that moves left / right with the nudge.
- Saving: step lines get two optional trailing fields (`nudge`, `ghost`), tracks an optional `trackSwing`; old files load unchanged and
  projects without these features are saved byte-for-byte as before.
- `t8_audit`: new `timing.*` rows. 18 new tests; `t8_selftest` passes.

## v38

Reverb v2 and delay v2 (phase 3 of `docs/AUDIO_AUDIT_AND_PLAN.md`; closes findings S2, E1 and E2).

- **Reverb: stereo 8-line feedback delay network** instead of the mono Freeverb. A mono click now gives a stereo tail whose
  left / right correlation is about -0.06 (was exactly 1.0, a mono reverb). 4 allpass diffusers per side, a Householder feedback
  matrix, lines of 30-65 ms with slow modulation (no metallic ringing: the semitone-band ripple of the tail is 3.9 dB), a 100 Hz
  high-pass on the input so bass does not muddy the tail. Left input excites the left output mix and right input the right one, so
  stereo sources keep their image.
- **SIZE keeps its meaning** (same RT60 curve: 0.5 -> 0.28 s, 0.8 -> 0.9 s, 0.9 -> 2 s, 0.97 -> 6+ s) and the wet level was calibrated
  to the old reverb (within 1.5 dB for white noise at sizes 0.3-0.9), so existing projects keep their balance.
- **New REVERB knobs: DAMP** (how fast the highs die away: lows decay with RT60, highs with RT60 x 1 ... 0.12) and **PRE**
  (pre-delay 0-120 ms, default 12). Saved in the project (`reverbDamp`, `reverbPreMs`).
- **Delay: interpolated and gliding time.** Changing the DIVISION, tempo or pattern used to jump the read position (a step 1.9 times
  larger than normal = a click). Now a 4-point Hermite read follows a time that glides to the new value in about 80 ms (bends the
  repeats like a tape echo; measured step 0.9x steady). Echoes land on exact fractional times.
- **Delay feedback path**: 150 Hz high-pass, **TONE** low-pass (1.2-12 kHz, new knob; repeats get darker), soft limiter so FEEDBACK
  95 % cannot run away. **PING-PONG** checkbox: first echo left, second right, and so on. Saved as `delayTone`, `delayPingPong`.
- `t8_audit`: new `delay.*` rows and `reverb.damp*`, `reverb.*band_ripple` rows. 15 new tests; `t8_selftest` passes.

## v37.1

- **UNISON no longer changes the character of the sound.** The copies of a note used to start with fixed golden-ratio phases, so
  their sum was a comb filter: some harmonics dipped by up to 13 dB (and with zero spread and no detune some cancelled almost
  completely), which sounded hollow / "square-ish" and different from the single voice, and the sum was about 2 dB louder.
  Now each unison count (2..16) has start phases found by a numerical search: mirrored left/right (both ears get the same timbre),
  the spectrum stays within about 9 dB of the single voice at worst (typically 3-6) and the loudness within 2 dB of it (power-normalised gains,
  `1/sqrt(n)` instead of `1.25/sqrt(n)`: patches that use unison come out about 2 dB quieter). New tests measure harmonics 1..16 of
  both channels for n = 2..7 at spread 0 / 0.7 / 1.

## v37

Synth v2 (phase 2B of `docs/AUDIO_AUDIT_AND_PLAN.md`) and a fill fix.

- **New filter** (`core/SynthFilter.h`): zero-delay-feedback, so the cutoff is accurate across the whole range. The old filter
  stopped working above ~7 kHz (8, 12 and 18 kHz sounded the same); now the cutoff opens the top end up to ~18 kHz and the knob range
  was extended to 18 kHz. Three modes on the FILTER row (was SLOPE): **12 DB**, **24 DB** (four-pole ladder) and **ACID**.
  Resonance is saturated inside the loop: at the top of the knob every mode **self-oscillates** with a clean sine on the cutoff
  (within 1 % from 200 Hz to 12 kHz) at a fixed, safe level, and nothing blows up on hot input or per-sample cutoff jumps.
  The wavetable engine uses the same filter.
- **2x oversampling** of the oscillators and the filter with a half-band decimator (-58 dB at 30 kHz, flat to 15 kHz, 11 samples
  latency). Saw aliasing: C6 -31.8 -> -45.0 dB, C7 -27.8 -> -40.1 dB (relative to the harmonics). Fast tanh and a folded FIR keep
  the cost sensible: the 33-voice worst case needs about 32 % of real time on the x86 sandbox.
- **ACID mode** (FILTER row, preset **ACID BASS**, new **ACID SQUARE**): accented steps open the filter much wider and sweep faster;
  accents in a row build on each other and drain in about half a second (the 303 accent capacitor), resonance rises a little with
  them; a **slide** is now a fixed 60 ms glide that is linear in pitch (or your GLIDE time, if longer) instead of an exponential
  approach. A step is an accent when its velocity is 1.0, so a MIDI keyboard played hard accents too.
- **DRIFT** (thicken group, all synth rows): slow, independent random wander of pitch (about 5 cents rms at 100 %) and cutoff
  per voice and per unison copy. Off by default; light amounts were added to WARM PAD, REESE BASS, AMBIENT DRONE, SUPER SAW,
  WIDE PAD and the two acid presets.
- Projects and presets store `acid` and `drift`; older files load as before (12/24 dB, no drift), only brighter at high cutoffs.
- **Fix: fills play the track's chosen sample.** A live break always used the first sample of every drum slot, whatever variant
  you had picked in the sequencer. It now uses the variant most of the track's active steps use (`TrackSampleVariant`).
  Ratchets already used the step's own variant.
- `t8_audit` has new `filter.*`, `acid.*` and `synth.drift1_*` rows (slopes, resonant peak on the cutoff up to 16 kHz,
  self-oscillation pitch and level, half-band response, accent brightness, drift). The pad loop test measures over 60 ms instead of
  20 ms because the short window depended on the phase of the chord. 14 new tests; `t8_selftest` passes.

## v36

Level matching of fills and risers (phase 2A, last part).

- **Fills (live breaks) are level-matched.** Each of the 16 breaks has a trim (`BreakTrimDb` in `DrumBreaks.cpp`) measured with the
  new `fill.*` rows of `t8_audit`: a groove bar at 124 BPM, then each break over the same bar, loudest 1 s window. KICK ROLL,
  STUTTER KICK and DOUBLE TIME were 3-6 dB too loud, SNARE/HAT/TOM ROLL slightly quiet. Now every fill sits within about
  -3 ... +1 dB of the groove on the factory kits (spread 3-3.7 dB; `breaks` 5.4 dB, `linn` 4.6 dB because of their sample character).
- **Risers are level-matched, stereo and 24-bit.** `tools/finish_risers.py` (run once on `kits/risers`): mono fold, DC/rumble
  high-pass, soft stereo widening (mono sum untouched), 48 kHz / 24-bit, gain so the loudest moment is -9.5 LUFS-M with a
  -1.5 dBFS peak ceiling. All 10 risers now hit at the same loudness.
- **Riser varispeed is band-limited** (the sinc table from v34 instead of linear interpolation) and stereo-aware.
- **Riser hit point is found by smoothed RMS** instead of the sample peak, so a widened / levelled riser still lands on the bar line.
- Tests: hit-point detection, bar-line landing at several tempos, the short-lead (1 bar at 170 BPM) case compensates for the
  peak-vs-hit offset like the other tests.

## v35

Drum kits and sample drop (second half of phase 2A).

- **Four new kits**, generated by the new `t8_kitgen` tool: `electro`, `techno`, `breaks`, `acid_v2` (the old `acid` stays). 24-bit /
  48 kHz WAVs synthesised at 192 kHz and converted with the engine's sinc resampler; hats, claps and shakers are stereo; every
  slot has several takes; levels keep real proportions (kick -0.8 dBFS, closed hat -9) instead of everything normalised.
  Described in `docs/REFERENCE.md`. Rerun `t8_kitgen` to regenerate them.
- **Drop a WAV on the window** with a drum track selected: the sample replaces that slot in a private copy of the current kit
  (`kits/mykit`, `mykit_2`, ...); factory kits are never modified. Any rate / bit depth works.
- `WriteWavPCM24` added to the WAV module. Tests: all new kits load, level ratios, stereo hats, the drop flow.

## v34

Drum engine v2 (first half of phase 2A of `docs/AUDIO_AUDIT_AND_PLAN.md`; the new kits follow in the next round).

- **DECAY 1.0 = the sample exactly as recorded.** The old engine laid an exponential envelope over every sample, even at 1.0
  (-5 dB at 200 ms, -10 dB at 400 ms). Now the engine output equals the file bit for bit (audit: residual below -300 dB).
  Below 1.0 the tail is shortened (0.5 gives about what the old 1.0 did), above 1.0 it is lifted by up to +6 dB towards the end.
  Projects keep their saved DECAY values, so old projects ring longer than before until the knob is lowered.
- **Band-limited TUNE** (`core/Resample.h`): a 16-tap Kaiser sinc with 128 sub-phases replaces linear interpolation. Pitching
  up lowers the filter edge to the new Nyquist so nothing folds back. A 9 kHz tone pitched +3 st keeps its level within 0.3 dB;
  a 19 kHz tone pitched up 7 st is removed (>34 dB). Rate 1.0 skips interpolation entirely.
- **Sinc sample-rate conversion on load**: 44.1 / 48 / 96 kHz, 8 / 16 / 24-bit and float WAVs keep their top end (was linear).
- **Velocity tone**: soft hits (velocity below ~0.55) get a low-pass that closes with the velocity; normal and accented hits are untouched.
- **KICK SYNTH** (Kick track > VOICE > KICK SYNTH, project-wide): SOURCE = SAMPLE / SYNTH / LAYER. A sine body with an
  exponential pitch sweep (SWEEP x, SWEEP T), LENGTH, a noise CLICK and DRIVE. **KEY LOCK** tunes the fundamental to the project
  key's root in the 40..80 Hz range (C = 65 Hz, F = 44 Hz, A = 55 Hz) so the kick and the bass sit on the same note; the track
  TUNE still shifts it. Saved in the project (`kick*` keys), default SAMPLE so nothing changes until you switch it.
- Tests: as-recorded playback, decay shortening / lift, tune level and anti-alias, velocity tone, kick synth pitch / key lock /
  layer / save-load, 48 kHz loader. Audit rows `drum.kick.vs_source_residual` and `drum.kick.decay0.5_*`. Baselines are now v34.
- Next: kits ELECTRO / TECHNO / BREAKS / ACID (24-bit, 48 kHz, stereo) and a drop-a-WAV-on-a-slot flow, then synth v2.

## v33

Stereo release (phase 1.3 of `docs/AUDIO_AUDIT_AND_PLAN.md`): the mix is no longer fully mono.

- **Constant-power pan law** (`core/Stereo.h`, `PanGains`): the centre stays at unity (centred tracks sound as before),
  a hard-panned track is no longer 3 dB quieter than a centred one. Used by the mixer, autopan and per-hit drum pan.
- **Per-track WIDTH and SPREAD** (selected track > FX tab > STEREO). WIDTH scales the side signal (0 = mono, 100 % = as is,
  200 %). SPREAD adds a decorrelated side signal made from the high-passed mid (two all-passes), so a mono source
  (hats, percussion, a plain synth) becomes stereo while the **mono sum stays bit-for-bit the original** and everything
  below ~220 Hz stays centred. Defaults for new projects: kick 0, snare 15 %, clap 35 %, hats 45 %, perc 50 %, bass 0, other synths 35 %.
  Width 100 % + spread 0 is a bit-exact bypass. Saved per scene (`width`, `spread`).
- Measured: a closed hat through the default settings gets L/R correlation 0.69 (was 1.00) with mono-sum error 0.
- Tests: pan law, bypass, mono compatibility, decorrelation, width 0, bass stays centred, save/load. `t8_audit` has
  new `mixer.*` rows. Baselines renamed to v33 (`docs/AUDIO_BASELINE_v33.txt`, `docs/RENDER_BASELINE_rolling_v33.txt`).
- Not yet: stereo reverb (S2) - comes with the reverb rewrite (phase 3).

## v32

Master bus release (phase 1 of `docs/AUDIO_AUDIT_AND_PLAN.md`): the output no longer clips softly all the time; a real limiter does the work.

- **No forced `tanh` any more.** The master volume goes straight to the limiter, so a mix below the ceiling is left untouched
  (before: 1.7 % THD at -6 dBFS, 5.8 % at 0 dBFS and up to -1.5 dB of lost level).
- **Look-ahead true-peak limiter, always in the path** (`core/Limiter.h`): 1.5 ms look-ahead, 8-tap sinc 4x true-peak estimate
  (`core/TruePeak.h`), instant attack, 30 ms hold and 120 ms release so a steady bass tone keeps one constant gain.
  Measured: 50 Hz at +4 dB over the ceiling comes out at THD+N -134 dB (v31: -34 dB) and never above the ceiling.
  The latency (~1.6 ms) is constant, also when the limiter is off. Defaults: ON, ceiling -1.0 dBTP; independent of the CHAIN switch.
- **DRIVE is oversampled 2x** (`Oversampler2x` + `Saturator` in `core/Dsp.h`, 63-tap Kaiser half-band, delay-compensated dry path):
  aliasing of a 6 kHz sine at full drive -45.8 dB vs -20.6 dB with plain tanh. DRIVE at 0 is a clean bypass.
- **MONO BASS** (MASTER FX > OUTPUT, default 120 Hz, 0 = off): Linkwitz-Riley 4th-order crossover, everything below goes to the
  centre, the sum stays flat. **COMP SC HPF** (default 80 Hz): the master compressor detector ignores the lows, so the kick no
  longer pumps the whole mix (50 Hz THD+N -52.6 -> -60.2 dB).
- **Loudness meter** (`core/LoudnessMeter.h`): BS.1770-4 / R128 momentary, short-term, gated integrated and true peak on the
  final output. Shown next to the master fader (`-13.1 LUFS  TP -2.2`; click resets) and in the header when there is room.
  The old RMS readout is gone.
- Tests: new selftest blocks for the limiter (ceiling, true peak, off = clamp), oversampler, mono bass, detector HPF, LUFS
  (reference tones) and persistence of the new keys; timing tests now account for the fixed bus latency. Baselines:
  the baselines in `docs/` (now v35).
- Not in this round (next): per-track width / decorrelation, pan law, stereo hats and synths (phase 1.3), then phase 2.

## v31

Foundation release: the app is now Mac-only and the engine got its measuring tools.

- **Handheld / muOS build removed.** The pixel-art views, `InputMap`, the LEGACY tab, `main.cpp` for the device,
  the aarch64 toolchain and packaging, and the APC mini support are gone (recover them from the v30 zip). The single
  executable is `trbrick_daw`. Old project files still load (the format did not change).
- **Keyboard shortcuts** (`ui_desktop/Shortcuts.cpp`): Space, Cmd+Z, Cmd+S, Cmd+C/V (+Shift = scene), `[` `]`, digits,
  Cmd+Left/Right. Right-click a track name: copy / paste track or scene, drum rhythm from the next genre / random.
- **Scenes collapsed at every launch** (the chain and chord rows open with the triangle; the state is no longer remembered).
- **Limits lifted for the Mac**: unison 1..16 (chord 8 per tone), 64 drum voices, chain of 64 steps, 256 undo levels,
  audio buffer 128 frames (was 256), no global unison cap.
- **Safe kit loading**: the kit is an immutable snapshot swapped atomically, so changing the kit while drums play can no
  longer touch freed memory (covered by a new selftest block).
- **Tools**: `t8_audit` (DSP measurements with `--out` / `--compare`) and `t8_render` (offline render + LUFS / peak / stereo
  report); baselines in `docs/`.
- **Cleanup**: `AppState` lost the handheld cursors, text entry, Y-hold and APC helpers; the old README (3500 lines) and
  plans were dropped (kept in the v30 zip), kit / pattern / preset tables to `docs/REFERENCE.md`; stray test projects and probes
  removed; folder listings refresh every 3 s instead of on tab switches.

## v30

- **PHASE SYNC (v30)**: new PH.SYNC switch (OSC tab, UNISON / LAYERS), per synth, OFF by default.
  With it on, every new note restarts all oscillators (all unison copies, OSC 2, FM operators,
  wavetable, sub) from the same start phase, so each note begins identically and keeps a fixed
  phase relation to the kick - the slow "drift" between a rolling bass and the kick goes away.
  A note that starts over a still-sounding one first ducks the old one for ~1.5 ms (no click);
  slides and notes from silence are unchanged. It is saved only when ON, so projects and
  custom presets written without it are identical to before and still open in v29.

## v29

- **Channel EQ on every track (v29)**: HPF (12 or 24 dB/oct, 20-500 Hz), low shelf, mid bell
  (frequency, gain, Q) and high shelf, +-15 dB, before the insert FX and the fader. A response
  curve shows the real filter (not a sketch); drag the dots (frequency left / right, gain up /
  down), mouse wheel = mid Q, double-click a dot = flatten it, FLAT resets the bands. Drum
  tracks now have VOICE / EQ / FX tabs, synths and the chord sound have OSC / FILTER / ENV + LFO / EQ / FX.
  Flat = bit-identical to before; coefficient ramps make every move click-free; saved per
  scene with the track (written only when not flat, old files load unchanged).

## v28

- **Track volume under the piano roll (v28)**: the strip under the roll is now the fader of
  the track being edited (name in the track colour, live peak, value) instead of MASTER, so
  the volume can be balanced while writing notes. MASTER stays on the MIXER view.

## v27

- **Collapsible chain (v27)**: the triangle next to SCENES hides the CHAIN and CHORDS rows
  (the scene pads and CHAIN ON/OFF stay), and the instrument editor below gets the space.
  The state is remembered in `ui_settings.cfg`.
- **Synth editor in tabs (v27)**: PRESET + engine button and OUTPUT (GLIDE, VOLUME) stay on
  the left; the right side has OSC (source, OSC 2, unison / layers), FILTER, ENV + LFO and
  FX (sends, insert FX, length) tabs with larger cells. FM has no filter, its FILTER tab says so.

## v26

- **Spectrum on double-click (v26)**: a single click on the header scope still toggles
  scope / 32-band bars; a double-click opens a detailed analyzer window (4096-point FFT,
  160 log bands 20 Hz - 20 kHz, peak hold, FREEZE, tilt, dB floor, note name under the
  cursor).
- **Riser hits the beat (v26)**: the riser is now back-timed. The loudest point of each
  riser file is found when it is loaded and the start is delayed so that point lands on the
  bar line chosen with the new DROP 1 / 2 / 4 / 8 BAR selector. If there is not enough lead
  time it plays slightly faster (up to 1.2x) or fades in part-way instead of missing the beat.
- **Randomizer (v26)**: melodic walk (mostly 0-2 scale degrees, strong-beat bias, no random
  leaps), slides only between notes up to 2 semitones apart. The old output was technically
  in key (9441 notes tested); the "out of key" impression came from slides and leaps and from
  chords in another key. New ON CHORDS / KEY ONLY toggle: with ON CHORDS strong beats land on
  chord tones of the chord track and the bass prefers the chord root. SNAP button in the
  piano roll moves existing notes into the scale (undoable). Status line says which key /
  scale was used.
- **Piano roll (v26)**: SCALE+USED shows all scale notes together with every pitch already
  used in the pattern, out-of-key notes are red with "!", degree numbers, scrolling by wheel /
  OCT. GHOST shows other melodic tracks' notes as outlines.
- **Chord editor in the chain (v26)**: double-click a chord slot (or right-click > Chord
  editor, or CHORD EDITOR... in the CUSTOM CHORDS block): timeline, degree buttons I-VII
  (optional 7ths), root, 16 chord types, inversion, length, add / delete / hear. Each chain
  step can also pick a chord scene (right-click > Chord scene, violet "cN" tag); it is
  applied when the song chain moves to that step with CHORD BANK on. Saved in the project
  (old files load unchanged).
- **Thicker synths (v26)**: up to 7 unison copies per voice (detune, stereo spread), sub
  oscillator (1 or 2 octaves below), noise, drive, a second oscillator with its own wave /
  semitone / detune, 24 dB filter slope and key tracking. All defaults are off and the
  default sound is sample-identical to v25. Handheld caps unison at 3, chords at 3 on every
  device. New presets: SUPER SAW, FAT BASS, SUB+SAW, WIDE PAD. Custom synth presets now also
  store engine, FM and wavetable settings. New projects start with louder synths
  (bass 75 %, synths 65 %, chords 60 %; existing projects keep their levels).

## v25

- **Sub-bass clicks (v25)**: the click at the start of a track came from the sidechain
  duck: a *muted* kick still triggers it, and the duck used to jump instantly. With a
  master HPF the sub-bass body is removed, so the jump was left as a bare step. The duck is
  now slew-limited (down 6 ms, up 10 ms) and smoothed. Verified against v24 with an A/B
  probe (click metric 0.0437 -> 0.0006). Drum voices also fade in (0.6 ms, kick 1.5 ms),
  hat choke is a 4 ms fade instead of a cut, and there are 32 voices with oldest-stealing.
- **Stutter is quantised (v25)**: STUTTER on/off and its density wait for the LAUNCH grid
  (NOW / BEAT / BAR); the button shows "armed" while it waits.
- **Chord bank (v25)**: CHAIN panel > CHORDS row, switch with CHORD BANK: ON. 8 independent
  chord scenes, the chord track's steps come from the sounding one while volume / pan / sends
  / mute stay on the pattern. Click a slot = edit it and launch it on the LAUNCH grid (the
  old chord is released on the boundary, or tied if the new one starts with a slide /
  full-length step). Right-click: edit only, copy this scene's chords here, duplicate to
  next, clear, rename. Undo, FOLLOW KEY, key / scale labels and the save file cover it.
- **Interface (v25)**: FILE > INTERFACE SIZE (AUTO / 75 ... 150 %), minimum window 800x520,
  scrolling context block; the LEGACY tab is hidden (FILE > LEGACY TAB to show it).
  Header scope toggles to a 32-band spectrum on click; each sequencer track has a peak
  hairline. Settings live in `ui_settings.cfg`.
- **Audio safety (v25)**: DSP load / xrun counter in the header; notes crossing a loop or
  scene edge are released (Panic); oscillators band-limited; device-rate engine.

## v22 - v24

- **Sharpness / HiDPI (v22)**: the UI is rendered at `uiScale`x into an
  offscreen texture which is stretched over the window, so the layout
  always fills the window and text is crisp on Retina. Default 2x on macOS,
  otherwise the reported pixel/point ratio; override with `--scale N` /
  `T8_SCALE=N` (1 = old soft 1:1, fastest). Root cause of the earlier "UI at
  half size in the corner": the SDLRenderer2 backend scales only clip rects by
  `FramebufferScale`; vertex positions need `SDL_RenderSetScale`, which was
  never called. The startup line `window ... points, renderer output ...` and
  `UI render scale Nx` show what is in use.
- **Visual system**: `ui_desktop/Visual.h` holds the colour/metric tokens
  (accent amber, teal = synth/playing, violet = chord, red = rec/clip, three
  surface levels, line colours) used by every custom-drawn widget.
- **Sound visualisation**: master-bus oscilloscope in the header
  (`Scope.h`, fed by `AudioEngine::CopyScope`, zero-crossing triggered, turns
  red near clipping), live ADSR shape and LFO cycle (with moving phase dot)
  in the synth block, per-track peak fill in the mixer sliders.

- **Project data safety (v23)**: saving writes `<file>.tmp`, fsyncs and renames
  it in (the previous file stays as `<file>.bak`), so a crash or power cut can
  never leave a half-written project. Files carry `format=2` and a final
  `[end]` line; a file that is incomplete or has an unparsable value is
  reported (not a crash), the damaged file is kept as `<file>.corrupt` and the
  `.bak` is loaded instead. Dated copies go to `backups/` (newest 10 per
  project) on every successful open and before the first save of a session -
  the app starts blank and autosaves over `project.t8proj`, so yesterday's
  work is kept there. FILE > BACKUPS restores one. A failed "open project"
  no longer blanks the project that is open.
- **Undo (v23)**: pattern snapshots are tagged with their scene (undo only
  reverts the scene you are editing); a second kind of step covers the
  project-wide sound state - synth/chord patches, presets, master FX, master
  volume and the chain. A knob drag is one step (changes <0.8 s apart merge).
  Mute/solo are never touched by Undo. Swapping scenes is not undoable (only
  the chain it rewrites is).
- **Drum beats (v23)**: FILE > DRUM BEATS saves the current scene's drum tracks
  under a name (same name = update), lists saved beats with APPLY / RENAME /
  DELETE, and keeps each hit's sample variant, ratchet and probability (older
  `.t8pat` files still load). The beats also show up in the BEAT picker of
  the RANDOM / KIT block.
- **Live (v24)**: the LAUNCH selector in the header (NOW / BEAT / BAR) makes a
  scene click wait for the next beat or bar; the pending scene pulses with a
  countdown. The chain shows a progress bar on the playing slot and marks the
  next one. A tie / slide step shows a link glyph (right-click: TIE / SLIDE).
- **Notes (v24)**: drag the right edge of a note's bar (Sequencer and Piano
  Roll, pitched tracks) to set its length; the right-click list is still there
  for long values. Every instrument has its own colour. Ratchet = N dark ticks
  + "xN", probability = the cell is filled to that percentage + the number.
  Piano Roll > SCALE: OFF / MARK (rows outside the key/scale dimmed, tonic
  tinted) / ONLY (just the scale notes) - key/scale come from the header.
- **Key / scale vs chords (v24)**: changing KEY / SCALE no longer touches
  written chords or the live chord. CHORDS > ADJUST CHORDS TO SCALE (and
  ADJUST MELODY TOO) transposes by the key change and snaps to the new scale,
  undoable; FOLLOW KEY does it automatically for every scene. A "!" next to
  the scale shows that this scene's chords were written in another key.
- **Master chain (v24)**: MASTER FX > MASTER CHAIN. Compressor (threshold,
  ratio, attack, release, makeup, mix = parallel; GLUE / PUNCH / SOFT) with a
  gain-reduction meter, plus HPF, low / mid / high tone, tape-style
  saturation, stereo width and a peak limiter with ceiling. Presets CLEAN /
  WARM / LOUD / LO-FI; the header CHAIN button switches it on or off. Order:
  HPF > tone > saturation > compressor > width > master volume > limiter.
  Everything is off by default, so old projects sound the same. The header
  shows GR and a short-term RMS (a rough loudness guide, not true LUFS).
