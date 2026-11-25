#!/bin/bash
# TR-Brick -- the permanent launcher (v44.7).
#
# What it does, every time you double-click the TR-Brick button on the Desktop:
#   1. looks in ~/Downloads for the newest trbrick-daw-source-*.zip;
#   2. if it is a new one, unpacks it and copies the program into ~/TR-Brick/app
#      (your own tracks, backups and the project file are never overwritten);
#   3. builds only what changed (the build folder stays between versions, so an
#      update takes seconds, not minutes);
#   4. starts the program.
#
# Test switches (used by the self-checks, harmless otherwise):
#   TRB_HOME        where the program lives          (default ~/TR-Brick)
#   TRB_DOWNLOADS   where the zip is looked for      (default ~/Downloads)
#   TRB_SOURCE_DIR  take the program from this folder instead of a zip
#   TRB_NO_LAUNCH=1 build but do not start the program
#   TRB_NO_PAUSE=1  never wait for Enter
#   TRB_CMAKE_ARGS  extra cmake arguments

set -u
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

ROOT="${TRB_HOME:-$HOME/TR-Brick}"
DL="${TRB_DOWNLOADS:-$HOME/Downloads}"
APP="$ROOT/app"
STATE="$ROOT/installed.txt"
INCOMING="$ROOT/incoming"

pause() { [ -n "${TRB_NO_PAUSE:-}" ] || read -r -p "Press Enter to close..." _; }
fail() { echo ""; echo "!! $1"; echo ""; pause; exit 1; }

echo "== TR-Brick =="
echo "   Your projects and recordings: ~/Documents/TR-Brick"

# ---- what is available to install --------------------------------------------------------------------------------------
SRC=""; ID=""
if [ -n "${TRB_SOURCE_DIR:-}" ]; then
    SRC="$TRB_SOURCE_DIR"; ID="dir:$TRB_SOURCE_DIR"
else
    ZIP="$(ls -t "$DL"/trbrick-daw-source-*.zip 2>/dev/null | head -n 1)"
    if [ -n "$ZIP" ]; then
        ID="$(basename "$ZIP") $(cksum "$ZIP" | awk '{print $1"-"$2}')"
    fi
fi

OLD_ID=""; [ -f "$STATE" ] && OLD_ID="$(cat "$STATE")"

if [ -z "$ID" ] && [ ! -d "$APP/src" ]; then
    fail "No TR-Brick archive found in $DL. Download trbrick-daw-source-....zip and double-click TR-Brick again."
fi

# ---- install / update the program files -------------------------------------------------------------------------------
if [ -n "$ID" ] && [ "$ID" != "$OLD_ID" ]; then
    if [ -z "$SRC" ]; then
        echo "-- New version found: $(basename "$ZIP") -- unpacking --"
        rm -rf "$INCOMING"; mkdir -p "$INCOMING"
        if ! unzip -q "$ZIP" -d "$INCOMING"; then rm -rf "$INCOMING"; fail "Could not unpack $ZIP (is the download complete?)."; fi
        SRC="$INCOMING/trbrick-daw"
    else
        echo "-- Installing from $SRC --"
    fi
    [ -f "$SRC/CMakeLists.txt" ] || { rm -rf "$INCOMING"; fail "The archive does not look like TR-Brick (no CMakeLists.txt)."; }
    command -v xattr >/dev/null 2>&1 && xattr -dr com.apple.quarantine "$SRC" 2>/dev/null

    mkdir -p "$APP/projects" "$APP/backups"
    # everything that is program (code) is replaced exactly, so a removed file really disappears
    for d in src tools third_party docs assets examples; do
        [ -d "$SRC/$d" ] && { mkdir -p "$APP/$d"; rsync -a --delete "$SRC/$d/" "$APP/$d/"; }
    done
    # factory content (kits, presets, patterns, grooves) is added/updated; what you made yourself stays
    for d in kits presets patterns grooves; do
        [ -d "$SRC/$d" ] && { mkdir -p "$APP/$d"; rsync -a "$SRC/$d/" "$APP/$d/"; }
    done
    # top-level files (CMakeLists, README, CHANGELOG, scripts)
    for f in "$SRC"/*; do
        [ -f "$f" ] || continue
        case "$(basename "$f")" in project.t8proj|imgui.ini|ui_settings.cfg|selftest_*) continue ;; esac
        cp -p "$f" "$APP/"
    done
    # your project file and your projects: only created if missing, never overwritten
    [ -f "$APP/project.t8proj" ] || { [ -f "$SRC/project.t8proj" ] && cp -p "$SRC/project.t8proj" "$APP/project.t8proj"; }
    [ -d "$SRC/projects" ] && rsync -a --ignore-existing "$SRC/projects/" "$APP/projects/"

    # the launcher updates itself (takes effect on the next start; the swap is safe while this one runs)
    if [ -f "$APP/tools/mac/launch.sh" ] && ! cmp -s "$APP/tools/mac/launch.sh" "$ROOT/launch.sh" 2>/dev/null; then
        cp "$APP/tools/mac/launch.sh" "$ROOT/launch.sh.new" && chmod +x "$ROOT/launch.sh.new" && mv "$ROOT/launch.sh.new" "$ROOT/launch.sh"
    fi
    rm -rf "$INCOMING"
    printf '%s' "$ID" > "$STATE"
else
    echo "-- Already the newest version --"
fi

[ -d "$APP/src" ] || fail "The program folder is missing: $APP"
cd "$APP" || fail "Cannot open $APP"

# ---- build (only what changed) --------------------------------------------------------------------------------------------
command -v cmake >/dev/null 2>&1 || fail "cmake is not installed. One-time setup, run in Terminal:  brew install cmake sdl2"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"

mkdir -p build-mac
if [ -f build-mac/CMakeCache.txt ] && ! grep -q "^CMAKE_HOME_DIRECTORY:INTERNAL=$APP\$" build-mac/CMakeCache.txt; then
    echo "-- The program folder moved: starting a clean build --"
    rm -rf build-mac; mkdir -p build-mac
fi
cd build-mac || fail "Cannot open the build folder."
if [ ! -f CMakeCache.txt ]; then
    echo "-- First build (slower, one time only) --"
    # shellcheck disable=SC2086
    cmake .. -DCMAKE_BUILD_TYPE=Release ${TRB_CMAKE_ARGS:-} || fail "cmake configure failed -- see the error above."
fi
echo "-- Building (only what changed) --"
make -j"$JOBS" trbrick_daw || fail "Build failed -- see the errors above. Your tracks are safe in ~/Documents/TR-Brick/Projects."
cd "$APP" || exit 1

# ---- start --------------------------------------------------------------------------------------------------------------------
if [ -n "${TRB_NO_LAUNCH:-}" ]; then echo "-- Build done (start skipped) --"; exit 0; fi
echo ""
echo "-- Starting TR-Brick (close its window to stop) --"
./build-mac/trbrick_daw
echo ""
echo "TR-Brick closed."
