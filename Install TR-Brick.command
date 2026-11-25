#!/bin/bash
# TR-Brick -- one-time install (v44.7).
#
# Do this ONCE, the way you already open the program (double-click this file; if macOS
# complains, allow it in System Settings > Privacy & Security, "Open Anyway").
# After that a button "TR-Brick" sits on the Desktop. From then on an update is just:
#   double-click TR-Brick on the Desktop (it fetches the newest version from GitHub).
# Without a zip at all: see docs/MAC_SETUP.md (brew install gh, gh auth login, one command).
#
# It makes: ~/TR-Brick/launch.sh   the permanent launcher
#           ~/TR-Brick/app         the program, the build, and YOUR tracks (projects/, backups/)
#           ~/Desktop/TR-Brick.command   the button
# Your old tracks from older TR-Brick folders in Downloads are copied over (nothing is deleted).
#
# Test switches: TRB_HOME, TRB_DESKTOP, TRB_DOWNLOADS, TRB_NO_LAUNCH, TRB_NO_PAUSE (see tools/mac/launch.sh).

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${TRB_HOME:-$HOME/TR-Brick}"
DESK="${TRB_DESKTOP:-$HOME/Desktop}"
DL="${TRB_DOWNLOADS:-$HOME/Downloads}"
APP="$ROOT/app"

pause() { [ -n "${TRB_NO_PAUSE:-}" ] || read -r -p "Press Enter to close..." _; }

echo "== TR-Brick: one-time install =="
[ -f "$HERE/tools/mac/launch.sh" ] || { echo "!! tools/mac/launch.sh is missing next to this file."; pause; exit 1; }

mkdir -p "$ROOT" "$APP/projects" "$APP/backups" || { echo "!! Cannot create $ROOT"; pause; exit 1; }
cp "$HERE/tools/mac/launch.sh" "$ROOT/launch.sh" && chmod +x "$ROOT/launch.sh"

# ---- bring your old tracks along (newest folder first, never overwrite) -----------------------------------------------
moved=0
for d in $(ls -dt "$DL"/trbrick-daw* 2>/dev/null); do
    [ -d "$d" ] || continue
    [ "$d" = "$HERE" ] && continue
    if [ -f "$d/project.t8proj" ] && [ ! -f "$APP/project.t8proj" ]; then
        cp -p "$d/project.t8proj" "$APP/project.t8proj" && moved=$((moved+1))
    fi
    if [ -d "$d/projects" ]; then
        n="$(ls "$d/projects"/*.t8proj 2>/dev/null | wc -l | tr -d ' ')"
        rsync -a --ignore-existing "$d/projects/" "$APP/projects/" && moved=$((moved+n))
    fi
done
[ "$moved" -gt 0 ] && echo "-- Copied $moved earlier project file(s) into $APP --"

# ---- the Desktop button (made here, so macOS does not flag it as downloaded) ---------------------------------------
mkdir -p "$DESK" 2>/dev/null
BUTTON="$DESK/TR-Brick.command"
printf '#!/bin/bash\nexec "%s/launch.sh"\n' "$ROOT" > "$BUTTON" && chmod +x "$BUTTON"
if [ -f "$BUTTON" ]; then echo "-- Button created: $BUTTON --"; else echo "!! Could not create the Desktop button; start $ROOT/launch.sh instead."; fi

echo ""
echo "-- Installing the program (first build takes a few minutes, one time only) --"
TRB_SOURCE_DIR="$HERE" exec "$ROOT/launch.sh"
