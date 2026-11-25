# Vendored Dear ImGui (docking branch)

Core sources and the SDL2 + SDLRenderer2 backends copied verbatim from
[ocornut/imgui](https://github.com/ocornut/imgui), `docking` branch,
commit `d88f238d1d363ef387382b2e29ad12cf96f826ac` (2026-10-15).
`LICENSE.txt` copied alongside (MIT).

## Why `docking`, not `master`

`master` is single-window-per-OS-window only. `docking` adds dockable/
tabbed panels within one OS window (`ImGui::DockSpace`), which is what
lets the new desktop UI show Sequencer + Mixer + FX etc. side by side (or
as switchable tabs the user can rearrange) instead of one full-screen tab
at a time like the handheld UI. It's not an experimental fork -- it's
ocornut's own long-running integration branch, merged from `master`
regularly (this checkout already includes everything through the above
commit), and it's what most real-world ImGui-based apps ship with.

## Why a straight file copy instead of a `third_party/SDL`-style subbuild

Same reasoning as `third_party/RtMidi` (see its own `VENDORED.md`): ImGui
has no configure step and no generated headers, so there's nothing a
subproject build would buy here. The core `.cpp`/`.h` files plus the two
backend files actually used (`imgui_impl_sdl2`, `imgui_impl_sdlrenderer2`)
are added directly to `trbrick_daw_desktop`'s own source list in the
top-level `CMakeLists.txt`.

`imgui_demo.cpp` is vendored too (it's part of the standard distribution
and `ShowDemoWindow()` is handy for development) but NOT compiled into
`trbrick_daw_desktop` -- see the CMake target.

## Why SDLRenderer2, not an OpenGL backend

The existing `trbrick_daw` (handheld) target already draws through
`SDL_Renderer` (`src/ui/Renderer.cpp`), not raw OpenGL/Metal/Vulkan --
there's no GL context anywhere in this codebase to hook into, and adding
one just for ImGui would mean pulling in GLEW/GLAD or similar for no
benefit. `imgui_impl_sdlrenderer2` renders ImGui through the *same* kind
of `SDL_Renderer` the app already uses, so `trbrick_daw_desktop` creates
its own `SDL_Renderer` the normal way and ImGui draws into it directly --
no new rendering API for this project to depend on.

## What's intentionally NOT vendored

`imgui_impl_opengl3.*`, `imgui_impl_metal.*`, and every other backend
under `backends/` upstream -- this project only ever needs the one SDL2 +
SDLRenderer2 pair. `misc/` (the Lua/Odin/etc. language bindings, the
single-file font-loading helpers for compressing a TTF into a header) is
also not vendored; the desktop UI loads its bundled TTF straight off disk
at runtime instead (see `src/ui_desktop/`).
