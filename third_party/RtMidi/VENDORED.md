# Vendored RtMidi

`RtMidi.h`/`RtMidi.cpp` copied verbatim from
[thestk/rtmidi](https://github.com/thestk/rtmidi), commit
`a3233c22949342f6697681e2cf2403e27fcf0c9e` (2025-11-27 on `master`).
`LICENSE` copied alongside (MIT-style; see its text).

## Why a straight file copy instead of a `third_party/SDL`-style subbuild

RtMidi's entire C++ API is these two files -- no configure step, no
platform-detection headers to generate, no other sources needed (the only
other files in the upstream repo are `rtmidi_c.*`, an optional C wrapper
this project doesn't use, and packaging/build-system scaffolding). Given
that, treating it as a subproject with its own CMake build (like SDL2 gets)
would add machinery for no benefit -- these two files are added directly
to `trbrick_daw`'s own source list in the top-level `CMakeLists.txt`,
same as e.g. `src/ui/Font5x7.cpp`.

## Backend selection (why no MIDI code changes are needed to add a platform)

RtMidi picks its backend at COMPILE TIME via preprocessor defines --
`__MACOSX_CORE__` (CoreMIDI), `__LINUX_ALSA__` (ALSA), `__UNIX_JACK__`,
`__WINDOWS_MM__`, etc. If NONE of those are defined, `RtMidi.cpp` falls
back automatically to a `__RTMIDI_DUMMY__` backend -- a real, valid
implementation, just one with zero ports (`getPortCount()` returns 0),
so the app builds and runs identically everywhere, just without live MIDI
input, until a real backend is enabled for that build. See the top-level
`CMakeLists.txt` for how each target enables the appropriate define:

- **macOS**: `__MACOSX_CORE__` is always enabled when `APPLE` is true --
  CoreMIDI/CoreFoundation/CoreAudio are system frameworks present on
  every Mac, no separate install needed.
- **Linux (including this sandbox's x86_64 desktop build)**:
  `__LINUX_ALSA__` is enabled only if `find_library(asound)` and
  `alsa/asoundlib.h` are both found -- silently falls back to the dummy
  backend otherwise (which is exactly what happens in the sandbox this
  was built in, since it has no apt/package-mirror access -- see the
  main README's "Why not just apt install" section for the same story
  with SDL2). On a normal dev machine, `apt install libasound2-dev` (or
  equivalent) before configuring picks this up automatically.
- **aarch64 (TrimUI Brick)**: same ALSA detection, but the cross
  toolchain doesn't currently have ALSA headers/lib in its sysroot --
  this needs either cross-building `alsa-lib` from source or borrowing
  `libasound.so`/headers from the device's own muOS rootfs (same
  "link against the device's own lib" precedent this project already
  uses for `libSDL2.so` on real hardware -- see the main README). Until
  that's done, the aarch64 build uses the dummy backend: it builds and
  runs fine, just without MIDI.
