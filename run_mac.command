#!/bin/bash
# TR8DAW -- one-click build + launch for the new mouse+keyboard
# Mac app (trbrick_daw).
#
# Double-click this file in Finder, or run it from a terminal with
# "./run_mac.command" -- either way it finds its own folder
# first, so it always builds/runs the right thing no matter where it's
# launched from.
#
# First run does a full cmake configure + build (slower); every run after
# that only recompiles what changed, so it's quick.

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR" || { echo "Could not find the project folder."; read -p "Press Enter to close..." _; exit 1; }

echo "== TR8DAW: build + run =="
echo "Project folder: $SCRIPT_DIR"
echo ""

if ! command -v cmake >/dev/null 2>&1; then
    echo "cmake is not installed. One-time setup, run this in Terminal:"
    echo "  brew install cmake sdl2"
    echo ""
    read -p "Press Enter to close..." _
    exit 1
fi

mkdir -p build-mac
cd build-mac || { echo "Could not create/enter build-mac/"; read -p "Press Enter to close..." _; exit 1; }

if [ ! -f CMakeCache.txt ]; then
    echo "-- First run: configuring the build (this is slower, one-time) --"
    if ! cmake .. -DCMAKE_BUILD_TYPE=Release; then
        echo ""
        echo "cmake configure failed -- see the error above."
        read -p "Press Enter to close..." _
        exit 1
    fi
fi

echo "-- Building (only recompiles what changed -- first build of this"
echo "   target compiles Dear ImGui too, so it's a bit slower than usual) --"
if ! make -j4 trbrick_daw; then
    echo ""
    echo "Build failed -- see the errors above. Common fix: delete the"
    echo "build-mac folder and run this script again for a clean rebuild."
    read -p "Press Enter to close..." _
    exit 1
fi

cd "$SCRIPT_DIR" || exit 1
echo ""
echo "-- Launching TR8DAW (close its window, or Ctrl+C here, to stop) --"
./build-mac/trbrick_daw

echo ""
echo "App closed."
read -p "Press Enter to close this window..." _
