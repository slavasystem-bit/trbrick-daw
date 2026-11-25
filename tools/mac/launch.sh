#!/bin/bash
# TR-Brick -- the permanent launcher (GitHub edition). TRB_LAUNCHER_GITHUB=1
#
# What it does, every time you double-click the TR-Brick button on the Desktop:
#   1. asks GitHub which versions exist: the main version, plus any open pull requests
#      (proposed changes waiting to be checked). If there are pull requests, it shows a
#      short menu; pressing Enter (or waiting 15 seconds) picks the main version;
#   2. downloads that version into ~/TR-Brick/repo (a clean copy, never edited by hand);
#   3. if it is a different version from the one installed, copies the program into
#      ~/TR-Brick/app (your own tracks, backups and the project file are never overwritten);
#   4. builds only what changed (the build folder stays between versions);
#   5. starts the program.
# No internet, or GitHub not set up? It starts the version that is already installed.
# Your projects and recordings in ~/Documents/TR-Brick are never touched by this script.
#
# One-time setup (see docs/MAC_SETUP.md):  brew install cmake sdl2 gh   then   gh auth login
#
# Test switches (used by the self-checks, harmless otherwise):
#   TRB_HOME        where the program lives          (default ~/TR-Brick)
#   TRB_DESKTOP     where the Desktop button goes    (default ~/Desktop)
#   TRB_REPO        GitHub repository                (default slavasystem-bit/trbrick-daw)
#   TRB_GIT_URL     fetch from this git URL/path instead of GitHub (skips gh entirely)
#   TRB_CHOICE      "main" or a pull request number: skip the menu
#   TRB_SOURCE_DIR  take the program from this folder instead of GitHub
#   TRB_OFFLINE=1   do not contact GitHub, start what is installed
#   TRB_NO_LAUNCH=1 build but do not start the program
#   TRB_NO_PAUSE=1  never wait for Enter
#   TRB_CMAKE_ARGS  extra cmake arguments

set -u
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

ROOT="${TRB_HOME:-$HOME/TR-Brick}"
DESK="${TRB_DESKTOP:-$HOME/Desktop}"
REPO="${TRB_REPO:-slavasystem-bit/trbrick-daw}"
APP="$ROOT/app"
CLONE="$ROOT/repo"
STATE="$ROOT/installed.txt"

pause() { [ -n "${TRB_NO_PAUSE:-}" ] || read -r -p "Press Enter to close..." _; }
fail() { echo ""; echo "!! $1"; echo ""; pause; exit 1; }
warn() { echo ""; echo "!! $1"; echo ""; }

echo "== TR-Brick =="
echo "   Your projects and recordings: ~/Documents/TR-Brick"

mkdir -p "$ROOT" || fail "Cannot create $ROOT"

# ---- the Desktop button (made here, so macOS does not flag it as downloaded) ---------------------------------------
BUTTON="$DESK/TR-Brick.command"
if [ ! -f "$BUTTON" ] && [ -d "$DESK" ]; then
    printf '#!/bin/bash\nexec "%s/launch.sh"\n' "$ROOT" > "$BUTTON" && chmod +x "$BUTTON" && echo "-- Button created: $BUTTON --"
fi

# ---- git with GitHub sign-in through gh -------------------------------------------------------------------------------
G() { git -c credential.helper= -c 'credential.helper=!gh auth git-credential' -c advice.detachedHead=false "$@"; }

github_ready() {
    [ -n "${TRB_GIT_URL:-}" ] && return 0
    if ! command -v git >/dev/null 2>&1; then
        warn "git is missing. One-time setup, run in Terminal:  xcode-select --install"; return 1
    fi
    if ! command -v gh >/dev/null 2>&1; then
        warn "The GitHub helper (gh) is not installed. One-time setup, run in Terminal:
     brew install gh
     gh auth login
   (step-by-step: docs/MAC_SETUP.md)"; return 1
    fi
    if ! gh auth status >/dev/null 2>&1; then
        warn "You are not signed in to GitHub (or there is no internet). One-time setup, run in Terminal:
     gh auth login"; return 1
    fi
    return 0
}

# ---- which version: main, or an open pull request -----------------------------------------------------------------
choose_version() {   # sets REF (git ref to fetch) and LABEL (what to show)
    REF="main"; LABEL="main version"
    local choice="${TRB_CHOICE:-}"
    if [ -z "$choice" ] && [ -z "${TRB_GIT_URL:-}" ]; then
        local list
        list="$(gh pr list --repo "$REPO" --state open --limit 20 \
                 --json number,title --jq '.[] | "\(.number)\t\(.title)"' 2>/dev/null)"
        if [ -n "$list" ]; then
            echo ""
            echo "Which version to start?"
            echo "   Enter  = main version (the one you play)"
            echo "$list" | while IFS=$'\t' read -r n t; do printf '   %-6s = test pull request #%s: %s\n' "$n" "$n" "$t"; done
            echo ""
            read -r -t 15 -p "Your choice (Enter = main, starts by itself in 15 s): " choice || { choice=""; echo ""; }
            choice="$(echo "$choice" | tr -d ' #')"
            if [ -n "$choice" ] && ! echo "$list" | cut -f1 | grep -qx "$choice"; then
                echo "-- \"$choice\" is not in the list: starting the main version --"
                choice=""
            fi
        fi
    fi
    if [ -n "$choice" ] && [ "$choice" != "main" ]; then
        REF="refs/pull/$choice/head"; LABEL="pull request #$choice (TEST version)"
    fi
}

# ---- download the chosen version into the clean copy ------------------------------------------------------------------
fetch_version() {    # sets SRC and ID on success
    local url="${TRB_GIT_URL:-https://github.com/$REPO.git}"
    if [ ! -d "$CLONE/.git" ]; then
        echo "-- First download from GitHub (one time, may take a minute) --"
        rm -rf "$CLONE"
        G init -q "$CLONE" || return 1
    fi
    echo "-- Checking GitHub for: $LABEL --"
    G -C "$CLONE" fetch -q --depth 1 "$url" "$REF" || return 1
    G -C "$CLONE" checkout -q --force --detach FETCH_HEAD || return 1
    G -C "$CLONE" clean -q -fdx
    local sha; sha="$(G -C "$CLONE" rev-parse --short HEAD)"
    echo "   $LABEL: $(G -C "$CLONE" log -1 --format='%s' | cut -c1-70) [$sha]"
    SRC="$CLONE"; ID="git:$REF:$(G -C "$CLONE" rev-parse HEAD)"
}

SRC=""; ID=""; REF="main"; LABEL="installed version"
if [ -n "${TRB_SOURCE_DIR:-}" ]; then
    SRC="$TRB_SOURCE_DIR"; ID="dir:$TRB_SOURCE_DIR"
elif [ -z "${TRB_OFFLINE:-}" ] && github_ready; then
    choose_version
    if ! fetch_version; then
        SRC=""; ID=""
        warn "Could not download from GitHub (no internet?)."
    fi
fi

OLD_ID=""; [ -f "$STATE" ] && OLD_ID="$(cat "$STATE")"

if [ -z "$ID" ]; then
    [ -d "$APP/src" ] || fail "TR-Brick is not installed yet and GitHub is not reachable. Finish the setup in docs/MAC_SETUP.md and try again."
    echo "-- Starting the version that is already installed --"
fi

# ---- install / update the program files -------------------------------------------------------------------------------
if [ -n "$ID" ] && [ "$ID" != "$OLD_ID" ]; then
    echo "-- Installing: $LABEL --"
    [ -f "$SRC/CMakeLists.txt" ] || fail "The download does not look like TR-Brick (no CMakeLists.txt)."
    command -v xattr >/dev/null 2>&1 && xattr -dr com.apple.quarantine "$SRC" 2>/dev/null

    mkdir -p "$APP/projects" "$APP/backups"
    # everything that is program (code) is replaced exactly, so a removed file really disappears
    for d in src tools third_party docs assets examples; do
        [ -d "$SRC/$d" ] && { mkdir -p "$APP/$d"; rsync -a --delete --exclude .git "$SRC/$d/" "$APP/$d/"; }
    done
    # factory content (kits, presets, patterns, grooves) is added/updated; what you made yourself stays
    for d in kits presets patterns grooves; do
        [ -d "$SRC/$d" ] && { mkdir -p "$APP/$d"; rsync -a --exclude .git "$SRC/$d/" "$APP/$d/"; }
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

    # the launcher updates itself, only from the main version and only to another GitHub launcher
    # (so testing an older pull request can never bring back the old zip launcher)
    NEWL="$APP/tools/mac/launch.sh"
    if [ "$REF" = "main" ] && [ -f "$NEWL" ] && grep -q "TRB_LAUNCHER_GITHUB=1" "$NEWL" \
       && ! cmp -s "$NEWL" "$ROOT/launch.sh" 2>/dev/null; then
        cp "$NEWL" "$ROOT/launch.sh.new" && chmod +x "$ROOT/launch.sh.new" && mv "$ROOT/launch.sh.new" "$ROOT/launch.sh"
    fi
    printf '%s' "$ID" > "$STATE"
elif [ -n "$ID" ]; then
    echo "-- Already installed: $LABEL --"
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
case "$REF" in refs/pull/*) echo "-- Starting TR-Brick: $LABEL. Next time press Enter to go back to the main version. --" ;;
                          *) echo "-- Starting TR-Brick (close its window to stop) --" ;; esac
./build-mac/trbrick_daw
echo ""
echo "TR-Brick closed."
