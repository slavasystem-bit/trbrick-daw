# TR-Brick DAW

A groovebox / live-jam DAW for the Mac. Six sample-based drum voices, three synth tracks (subtractive,
FM or wavetable engine each) and a polyphonic chord track, a 64-step sequencer with a piano roll, 16
scenes with a song chain, a per-track channel EQ and insert FX, shared delay / reverb, a master chain, and
live recording (master WAV, per-track stems, MIDI capture). Everything is on one screen, built for playing a
set and ending up with a finished track: jam first, then master.

Mac only since v31 (the handheld / muOS build and the APC mini code were removed; they live in the v30 zip).

## Run it

```sh
brew install cmake sdl2          # once
./run_mac.command                # builds into build-mac/ and starts the app
```

or by hand:

```sh
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release
cmake --build build-mac -j8
./build-mac/trbrick_daw
```

Run it from the project folder (or anywhere inside it): the kits (`kits/`) resolve against the folder that contains them. Your own files live in `~/Documents/TR-Brick` (`Projects`, `Recordings`, `Library/{Beats,Presets,Favorites}`). `--project file.t8proj` opens a project (otherwise a session starts
blank), `--kit kits/909` picks the drum kit, `--scale N` sets the UI render scale. Saves are atomic; the project file changes only when you save (Cmd+S), unsaved work is kept in a recovery copy, and File > Browse Versions restores
earlier versions. File / Edit / View / Library / Settings are in the menu bar at the top.

## The LIVE screen

Header (transport, BPM, scene, launch quantize, key / scale, FILE, MASTER FX, CHAIN, UNDO, scope / spectrum) ->
SCENES (collapsed at every launch; open the triangle for the chain and chord rows) -> sequencer and mixer or piano roll
side by side -> context block for the selected track (voice / EQ / FX, or OSC / FILTER / ENV + LFO / EQ / FX) -> RANDOM / KIT and
the LIVE block (master filter XY pad, MUTE ALL, STUTTER, FILL, RISER). See `docs/PERFORMER_GUIDE.md` for how to
play it without breaking the groove.

v44.18: Save works like in Logic: Cmd+S saves, the title says "Edited", a new project is Untitled until you name it, quitting asks Save / Don't Save / Cancel, a crash leaves a recovery copy; Cmd+N / O / Shift+S.
v44.17: one info line at the bottom of the window says what is under the mouse (no more pop-up tooltips), hand cursor on everything clickable, `?` / F1 shows all gestures.
v44.16: every slider, fader, checkbox and +/- pair of the program is now the same field (drag, Shift = fine, wheel, double-click to type, Option+click = reset).
v44.15: one number field for BPM / LEN / SWING / REPEATS (drag, Shift = fine, wheel, double-click to type, Option+click = reset) and Redo (Shift+Cmd+Z).
v44.14: your files live in `~/Documents/TR-Brick` (Projects, Recordings; backups in the program's service folder), recordings are named by project and date, the program continues with the last open project, FILE has SHOW IN FINDER.
v44.13: levels for every instrument, the transition and the fill on each brick (Shift + drag a cell of WHO PLAYS, right-click it, or the third inspector row).

v44.12: the program opens on LIVE; THROW INTO THE CHAIN (amber, right of the scene pads) adds the brick and opens TRACK; static layout (no fold-away blocks); TRACK = strip + inspector + scene editor, without RANDOM / KIT and LIVE blocks.

v44.11: the inspector - two rows under the strip with everything about the selected brick (role, repeats, trim, who plays, dynamics, fill, transition, copy / repeat / remove); START HERE is gone, THROW INTO THE CHAIN is an amber button on the TRACK top bar.

v44.10.2: FILLS / TRANSITION lane under the bricks (drag to snap to bars, right-click for details) and readable dynamics presets (right-click the DYNAMICS block, each brick shows its shape name).

v44.10.1: REPEATS field (- / + and xN on each brick), zoom for a long track (+/-, FIT, BRICK, Ctrl/Cmd + wheel) and an overview map.

v44.10: one screen and one player: the bricks on top, the scene of the selected brick below; PLAY loops a brick or runs the track (BRICK / TRACK / LOOP), FROM START / Home, click the ruler to play from a brick, COPY AS NEW SCENE. The chain row left LIVE.

v44.9: the TRACK tab, the main screen: the whole track as one strip of bricks (length = drag the edge, who plays = click the table, dynamics = drag the dots, double-click = open the scene).

v44.8: first sixty seconds: a starter groove in the first scene, a "start here" card instead of the knob wall (open it by clicking an instrument name), and THROW INTO THE CHAIN.

v44.7: launch in two clicks: run "Install TR-Brick.command" once; after that a TR-Brick button on the Desktop takes the newest zip from Downloads, builds only what changed and starts the program (your tracks stay in ~/TR-Brick/app/projects).

v44.58: LINK (scenes that share mute and solo), EDIT / GO (open a scene without launching it), and the LIVE switches follow a waiting launch (NEXT: SCENE n).
v44.57: PITCH - a turntable pitch fader for the whole output (tempo and pitch together, +-8% / +-16%); key P, then the arrow keys.
v44.56.2: tools/ableton_slice.py cuts a Live export of protected Ableton samples back into single WAV files.
v44.56: AIFF (.aif / .aiff / .aifc) samples and loops everywhere .wav worked: sampler, SAMPLE engine, library, drop, file window.
v44.55: EUCLID > SPREAD MELODY - the notes of a track stay and are placed on the euclidean hits in turn; off brings the original back.
v44.54: NEXT BAR - a button in the LIVE panel makes mute, solo and the group buttons wait for the next bar line (like a fill); off = real time.
v44.53: a fourth synth engine, SAMPLE: Bass, Synth2, Synth3 and the chord play a file of the project at the pitch of the note (ONE-SHOT, SLICES, LOOP), shaped by the synth's own envelope, filter, LFO, unison and FX.
v44.52.1: slices by hits (FIND, marks on the waveform), LAY OUT > STEPS, loop seam crossfade, loops in the library (tempo from the name).
v44.52: one SAMPLE editor on drums and sampler (START, LENGTH, ATTACK, FINE, GAIN, REVERSE); sampler SLICES and LOOP fitted to bars of the project tempo.
v44.51: MY SAMPLES - your own library (one folder, sub-folders by instrument): browser with HEAR, drum tracks, own kits (Kits/<name>), own transitions, SAMPLER FINE / GAIN.
v44.50.1: LIVE / TRACK never start each other, "PLAYING:" indicator, DROP+CHORD, CHORD LAB on TRACK, brick click only selects, MIX lane "off" really silent.
v44.50: SAMPLER - the 11th instrument: drop a WAV (copied into samples/ beside the project), play it from the step row and piano roll, ROOT / START / LENGTH / ATTACK / MONO-POLY / REVERSE, a waveform card; project format 7 (old projects open).
v44.49: STYLE - one place for beat, kit, groove, sound, bass line, chords and tempo (12 styles, four parts B S N T, one undo step); new STYLES browser.
v44.48: WHO PLAYS gets S under every bar and group buttons (ALL DRUMS, DRUMS, HATS, DROP, MUTE ALL, ALL ON); the LIVE panel gets the same ten bars; the chord bar works with the chord bank on.
v44.47: long typical beats (AMEN 4 bars, 12 more 2 bars); applying one brings the scene up to the typical length (one undo step), a LONG tag in TYPICAL BEATS.
v44.46: CHORD LAB is honest: JAM | SOUND, MATCH under every block, WHY only from counts, no NEXT, one set of chord names; fixes a crash on the second edit in CHORD LAB (v44.44-45).
v44.45: everything fits (TRACK bottom, 1280x800 header), an empty TRACK has an empty state, Option copies, MIX LAB balance is honest, one set of words (SCENE, TRACK), Esc closes CHORD LAB.
v44.44: stabilisation after the audit: LFO to WT POS no longer lost on save, CHORD LAB changes nothing until you edit (one undo step), chord audition does not cut the loop, six notes in capture, the old live chord removed, scene lock checked. New tool t8_fuzz.
v44.43: CHORD LAB, a panel over LIVE: palette of the key (with borrowed chords), MATCH to the bass / synth notes, a jam loop with resizable blocks, WHY and NEXT, then ADD TO SCENE and THROW INTO THE CHAIN. Chords now have up to six voices.
v44.42: MIX LAB gets CHANNEL (level, pan, four-band EQ of one instrument over its spectrum) and APPLY TO ALL (copies level / pan / EQ into every scene and brick, or only the chain).
v44.41: MIX LAB tab - where the instruments fight for the same frequencies (frequency map, overlap cards with SHOW IT / TRY IT and A / B, tonal balance, priority list).
v44.40: red kick, level stripe in the sequencer rows, launch feedback on the scene slots (progress, countdown in beats, content dots).
v44.39: parameter curves on a LIVE scene (the AUTO tab of the instrument block); THROW moves them into the brick.
v44.38: PARAM page (pan, sends, tune, decay, cutoff, resonance over a brick), one mute for a brick (WHO PLAYS = M / S), Tab between LIVE and TRACK.
v44.37: the instrument pictograms on every tab (sequencer rows, mixer strip, lanes, piano roll, context header, SOUND chips).
v44.36: pictograms for the ten instruments (WHO PLAYS bars and the rows under the bricks), the brick actions in the left column of the inspector, TRIM with beats.
v44.35: ALL next to LOOP on TRACK: the whole track goes round (with transitions) while you edit any brick.
v44.34: the brick inspector on TRACK is redrawn: pictograms, ten WHO PLAYS bars and three coloured pages (LEVEL / FILL / TRANS) instead of a wall of words.
v44.33: fix - a brick thrown from a scene with its own kit now plays that kit (before: the default kit, wrong percussion samples).
v44.32: new 808 kicks (SNAP / HARD / SUB F1), full 808 percussion (cowbell, clave, rimshot, maracas, congas, cymbal), new kit 808perc.
v44.31: one keyboard contract for browsers and dialogs (Return = main button, Esc = cancel), arrows in the Open list.
v44.30: select several bricks (Shift+click, rubber band, Cmd+A; LOOP goes round the range) and several notes in the roll (rubber band, move, octave, copy, delete).
v44.29: the Step settings window is gone - the inspector has MIX / FILTER in LEVEL OF and a RESET button.
v44.28: one player - PLAY, LOOP (round the selected brick / bricks, off = play on and stop), Home / |< to the first brick, Return plays from the selected brick.

v44.27: a thrown brick sounds exactly like its scene (own sound where it differs from the track), and on LIVE the player plays the LIVE scene, not the track.

v44.26: one drum can play from a kit of its own (THIS DRUM: THE KIT / OWN KIT in the SOUND view), and the kit browser opens on TRACK too.

v44.25: TRACK fixes - fills and transitions can be moved (inside a brick, onto another brick) and removed by a click, a break can be picked instead of RANDOM, taller MIX / FILTER / instrument lanes, the end points of a lane can be grabbed.

v44.24: sound of an instrument on TRACK - NOTES | SOUND | MIX under the inspector, WHOLE TRACK or THIS BRICK ONLY per instrument (a dot marks an own sound), APPLY TO ALL BRICKS; project format 6.

v44.23: the track has one sound of its own - a thrown brick takes it (jamming on LIVE does not reach the track), TAKE SOUND TO TRACK replaces it on purpose, UPDATE FROM LIVE refreshes a brick's notes; LIVE is amber, TRACK teal.

v44.22: a brick owns its notes - THROW copies the scene into a hidden brick slot (64 of them), the TRACK editor edits the brick and LIVE stays a sketch pad; project format 5, older files convert on load.

v44.6: chain steps get DYNAMICS (level and HPF / LPF sweeps start -> end) and the chain can be filled from the scene roles (dynamics, break mutes, risers / impacts between roles). Since v44.21 the dynamics are lanes with free points (MIX, one DJ filter, instrument levels) edited under the bricks.

v44.5.4: the sound (synth / chord patches, kick synth, kit) belongs to each scene; copy it between scenes from the scene pad menu.

v44.5.2: right-click a track name > Match level in other scenes (fader or whole strip, all scenes or the chain's).

v44.5.1: mute / solo are per scene again; right-click a track name (or a scene pad) to copy an instrument to another scene / track or erase it.

v44.5: the CHAIN is a score - right-click a slot > Step settings (mutes, fill, transition, trim); a scene has a role and a default fill (right-click the pad).

v44.4: the melodic RANDOM parts have a third mode RHYTHM (a figure with a chosen number of notes per bar, even one); EUCLID / ROTATE sit in the lane panel and the piano roll; LEN shows the length in bars.

### Keyboard

| Key | Action |
| --- | --- |
| Space | play / stop |
| Cmd+Z | undo |
| Cmd+S | save |
| Cmd+C / Cmd+V | copy / paste the selected track |
| Cmd+Shift+C / Cmd+Shift+V | copy / paste the whole scene |
| `[` / `]` | previous / next scene |
| Cmd+Left / Cmd+Right | previous / next scene (alternative) |
| 1 ... 9, 0 | select track 1 ... 10 |
| arrows (+Shift) | move between parameter cells / change the value (bigger steps with Shift) |

Right-click a track name for copy / paste (track or scene) and, on drums, "rhythm from the next genre" / "random rhythm".
Right-click a step for ratchet, probability, accent, variant, length and slide.

## Tools

| Tool | What it does |
| --- | --- |
| `build-x86_64/t8_selftest` | offline engine checks (NaN / silence / clipping, sample-accurate timing, persistence, thread safety); must print `ALL CHECKS PASSED` |
| `t8_audit [--out f] [--compare f]` | measures the DSP (aliasing, filter ceiling, reverb RT60 and stereo, compressor and limiter distortion, drum tails). Baseline: `docs/AUDIO_BASELINE_v44.txt` |
| `t8_presetaudit [--wav dir] [--csv f] [--write-levels f]` | renders every built-in synth preset the way it is played and prints level (RMS / peak / crest), spectrum, stereo, mono compatibility, length and which thickening layers it uses. `--write-levels` regenerates `src/core/SynthPresetLevels.inc` (run it a few times, the drive is non-linear in volume) |
| `t8_kitgen [outdir]` | regenerates all 18 drum kits (24-bit, 48 kHz, synthesised at 192 kHz; since v43.1 this includes the classic 606 / 707 / 808 / 909 / acid / cr78 / dub / funk / gabber / industrial / linn / lofi / minimal / trap808) into `kits/` and prints a level / length / stereo report |
| `t8_render project.t8proj [--scene N] [--bars N] [--out f.wav]` | renders a project offline and prints peak, RMS, LUFS, stereo correlation. Example: `examples/rolling.t8proj` (baseline in `docs/RENDER_BASELINE_rolling_v44.txt`) |

Build them with `cmake --build build-mac --target t8_selftest t8_audit t8_render t8_kitgen` (or the whole tree). Run them from the
repo root so `kits/` resolves.

## Layout

- `src/core/` engine and data model, no UI dependency (`t8core`): `AudioEngine`, `Sequencer`, `DrumEngine`, synth engines,
  `Effects`, `MasterChain`, `Persistence` (`.t8proj`), recorders.
- `src/app/` `AppState` (the state and actions shared by the UI: cursors, undo, clipboards, generators) and parameter lists.
- `src/ui_desktop/` the ImGui panels and `Shortcuts.cpp`; `src/main.cpp` is the window / audio / main loop.
- `kits/` 18 synthesized drum kits and risers (generated by `t8_kitgen`; the old `tools/gen_*_kit.py` scripts are kept only as history and no longer produce what ships), and `presets/` / `patterns/` (legacy folders: since v44.19 your presets, beats and favourites live in `~/Documents/TR-Brick/Library`, the first start copies them there), `examples/` sample projects, `third_party/imgui` and `third_party/sdl2-x86_64` (Linux dev build only),
  `third_party/RtMidi` (vendored for the planned MIDI input, not built yet).
- Data the app writes: `project.t8proj`, `backups/`, `recordings/`, `exports/`, `ui_settings.cfg`.

## Documents

- `docs/ROADMAP.md` what is planned and in what order (P = performance, Q = sound, H..O = the rest).
- `docs/INTERACTION_MODEL.md` one interaction logic for the whole program (widgets, gestures, per-view table, checklist before adding any control). Read it before adding anything new.
- `docs/AUDIO_AUDIT_AND_PLAN.md` the measured audit of the engine and the phased plan to reach the target sound.
- `docs/PERFORMER_GUIDE.md` live use. `docs/REFERENCE.md` kits, typical patterns, synth presets.
- `CHANGELOG.md` per-version changes (the old handheld-era README and plans are in the v30 zip).

## Limits (v31)

16 scenes x 64 steps, chain of up to 64 steps, 10 tracks, up to 16 unison voices per synth (8 per chord tone), 64 drum voices,
256 undo steps, audio buffer 128 frames at the device sample rate.
