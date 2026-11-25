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
#   5. runs the self-check (t8_selftest) on this Mac whenever the version changed;
#   6. starts the program.
# Safety rules (stage N0):
#   - a pull request starts only when ALL its GitHub checks are green. Red, still running
#     or not started yet = it refuses and explains why (the main version can be started instead);
#   - if the self-check fails on this Mac, a pull request is not started; for the main version
#     the last version that passed the self-check is started instead (if there is one).
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
#   TRB_SKIP_SELFTEST=1    do not run the self-check after the build (emergency only)
#   TRB_ALLOW_UNCHECKED=1  start a pull request even if its GitHub checks are not green (for developers)

set -u
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

ROOT="${TRB_HOME:-$HOME/TR-Brick}"
DESK="${TRB_DESKTOP:-$HOME/Desktop}"
REPO="${TRB_REPO:-slavasystem-bit/trbrick-daw}"
APP="$ROOT/app"
CLONE="$ROOT/repo"
STATE="$ROOT/installed.txt"
TESTED="$ROOT/selftest-passed.txt"   # which installed version last passed the self-check on this Mac
GOOD="$ROOT/last-good"               # copy of the last program that passed it

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
            echo "$list" | while IFS=$'\t' read -r n t; do
                printf '   %-6s = test pull request #%s: %s  [%s]\n' "$n" "$n" "$t" "$(pr_checks_word "$(pr_checks_state "$n")")"
            done
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
        REF="refs/pull/$choice/head"; LABEL="pull request #$choice (TEST version)"; PR="$choice"
        [ -z "${TRB_GIT_URL:-}" ] && gate_pr "$choice"
    fi
}

# ---- the gate: a pull request starts only when all its GitHub checks are green -------------------------------------
pr_checks_state() {  # prints green | running | red | none | unknown
    local b
    b="$(gh pr checks "$1" --repo "$REPO" --json bucket --jq '.[].bucket' 2>/dev/null)"
    local rc=$?
    if [ -z "$b" ]; then
        # no rows: either the checks have not started yet, or GitHub could not be asked
        if gh pr view "$1" --repo "$REPO" --json number >/dev/null 2>&1; then echo none; else echo unknown; fi
        return
    fi
    [ $rc -ne 0 ] && [ $rc -ne 8 ] && [ $rc -ne 1 ] && { echo unknown; return; }
    if echo "$b" | grep -qE '^(fail|cancel)$'; then echo red
    elif echo "$b" | grep -qx pending; then echo running
    else echo green; fi
}
pr_checks_word() {
    case "$1" in green) echo "checks green" ;; running) echo "checks still running" ;; red) echo "CHECKS FAILED" ;;
                 none) echo "checks not started" ;; *) echo "checks unknown" ;; esac
}
gate_pr() {          # on refusal: explains, then either switches to the main version or exits
    local n="$1" st; st="$(pr_checks_state "$n")"
    PR_SHA="$(gh pr view "$n" --repo "$REPO" --json headRefOid --jq .headRefOid 2>/dev/null)"
    [ "$st" = green ] && { echo "-- Pull request #$n: all GitHub checks are green --"; return 0; }
    if [ -n "${TRB_ALLOW_UNCHECKED:-}" ]; then
        warn "Pull request #$n: $(pr_checks_word "$st"). Starting anyway because TRB_ALLOW_UNCHECKED is set."; return 0
    fi
    local why
    case "$st" in
        red)     why="Its automatic checks FAILED on GitHub: something in this change is broken.
   Do not test or merge it. Tell Claude in the project, it will look at the failure." ;;
        running) why="Its automatic checks are still running on GitHub (they take about 15-30 minutes).
   Try again a bit later." ;;
        none)    why="Its automatic checks have not started yet on GitHub. Try again in a few minutes." ;;
        *)       why="Could not ask GitHub about its checks (internet?). Try again later." ;;
    esac
    warn "Pull request #$n will NOT be started.
   $why"
    refuse_pr
}
refuse_pr() {        # after a refusal: Enter = main version, otherwise close the window
    if [ -n "${TRB_NO_PAUSE:-}" ]; then exit 3; fi
    local a=""
    read -r -p "Press Enter to start the main version instead, or close this window to stop: " a
    REF="main"; LABEL="main version"; PR=""; PR_SHA=""
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
    if [ -n "${PR_SHA:-}" ] && [ "$(G -C "$CLONE" rev-parse HEAD)" != "$PR_SHA" ]; then
        warn "Pull request #$PR changed on GitHub a moment ago; its new checks have not finished.
   It will NOT be started. Try again later."
        refuse_pr
        G -C "$CLONE" fetch -q --depth 1 "$url" "$REF" || return 1
        G -C "$CLONE" checkout -q --force --detach FETCH_HEAD || return 1
    fi
    G -C "$CLONE" clean -q -fdx
    local sha; sha="$(G -C "$CLONE" rev-parse --short HEAD)"
    echo "   $LABEL: $(G -C "$CLONE" log -1 --format='%s' | cut -c1-70) [$sha]"
    SRC="$CLONE"; ID="git:$REF:$(G -C "$CLONE" rev-parse HEAD)"
}

SRC=""; ID=""; REF="main"; LABEL="installed version"; PR=""; PR_SHA=""
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
CUR_ID="$(cat "$STATE" 2>/dev/null)"
IS_TEST=""; case "$CUR_ID" in *refs/pull/*) IS_TEST=1 ;; esac   # the installed version is a pull request (also when offline)
# A pull request chosen in THIS run ($PR) is refused when it fails. One that is only left installed from an
# earlier test (offline, GitHub unreachable) falls back to the last good main version, so a show never ends with no app.
LEFTOVER=""; [ -n "$IS_TEST" ] && [ -z "$PR" ] && LEFTOVER=" (the installed version is a TEST version left from an earlier check)"

# The last main version that passed the self-check on this Mac (a pull request never becomes the fallback).
# Only the program file is kept: it starts beside the newer version's kits / presets / assets in $APP.
build_failed() {
    if [ -z "$PR" ] && [ -x "$GOOD/trbrick_daw" ]; then
        warn "$1$LEFTOVER
   Starting the last main version that passed the self-check instead. Tell Claude in the project."
        cd "$APP" || exit 1
        [ -n "${TRB_NO_LAUNCH:-}" ] && { echo "-- Fallback ready (start skipped) --"; exit 0; }
        "$GOOD/trbrick_daw"; echo ""; echo "TR-Brick closed."; pause; exit 0
    fi
    fail "$1 Your tracks are safe in ~/Documents/TR-Brick/Projects."
}

# Runs t8_selftest in a throw-away folder (the same idea as tools/run_tests.sh, built in here so that it
# also works for older versions that do not have that script): the folder links to the program files,
# and whatever the test writes stays in the folder.
run_selftest() {
    local tmp e name rc
    tmp="$(mktemp -d "${TMPDIR:-/tmp}/trb_selftest.XXXXXX")" || return 1
    for e in "$APP"/*; do
        name="$(basename "$e")"
        case "$name" in build|build-*|selftest_*|ui_settings.cfg|imgui.ini) continue ;; esac
        ln -s "$e" "$tmp/$name"
    done
    ( cd "$tmp" && ulimit -s "$(ulimit -H -s)" 2>/dev/null; "$APP/build-mac/t8_selftest" ); rc=$?
    rm -rf "$tmp"
    return $rc
}
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
    cmake .. -DCMAKE_BUILD_TYPE=Release ${TRB_CMAKE_ARGS:-} || { rm -f CMakeCache.txt; build_failed "cmake configure failed -- see the error above."; }
fi
echo "-- Building (only what changed) --"
make -j"$JOBS" trbrick_daw || build_failed "Build failed -- see the errors above."
cd "$APP" || exit 1

# ---- self-check on this Mac (only when the installed version changed) ---------------------------------------------------
APP_BIN="./build-mac/trbrick_daw"
if [ -n "${TRB_SKIP_SELFTEST:-}" ]; then
    echo "-- Self-check skipped (TRB_SKIP_SELFTEST) --"
elif [ -n "$CUR_ID" ] && [ "$(cat "$TESTED" 2>/dev/null)" = "$CUR_ID" ]; then
    echo "-- Self-check: already passed for this version --"
else
    echo "-- Self-check on this Mac (about a minute, only after an update) --"
    LOG="$ROOT/selftest-last.log"
    if ( cd build-mac && make -j"$JOBS" t8_selftest ) > "$LOG" 2>&1 && run_selftest >> "$LOG" 2>&1; then
        echo "   Self-check passed ($(grep -c '^\[ OK \]' "$LOG") checks)."
        printf '%s' "$CUR_ID" > "$TESTED"
        if [ -z "$IS_TEST" ]; then
            mkdir -p "$GOOD" && cp -p "$APP_BIN" "$GOOD/trbrick_daw.new" && mv "$GOOD/trbrick_daw.new" "$GOOD/trbrick_daw" \
                && printf '%s' "$CUR_ID" > "$GOOD/version.txt"
        fi
    else
        rm -f "$TESTED"
        grep '^\[FAIL\]' "$LOG" 2>/dev/null | head -5
        if [ -n "$PR" ]; then
            fail "The self-check FAILED on this Mac for $LABEL, so it will NOT be started.
   Do not merge it. Tell Claude in the project (details: $LOG)."
        elif [ -x "$GOOD/trbrick_daw" ]; then
            warn "The self-check FAILED on this Mac$LEFTOVER.
   Starting the last main version that passed it instead (details: $LOG). Tell Claude in the project."
            APP_BIN="$GOOD/trbrick_daw"
        else
            warn "The self-check FAILED on this Mac and there is no earlier good version to fall back to.
   Starting it anyway; be careful and tell Claude in the project (details: $LOG)."
            [ -n "${TRB_NO_PAUSE:-}" ] || read -r -p "Press Enter to start..." _
        fi
    fi
fi

# ---- start --------------------------------------------------------------------------------------------------------------------
if [ -n "${TRB_NO_LAUNCH:-}" ]; then echo "-- Build done (start skipped) --"; exit 0; fi
echo ""
case "$REF" in refs/pull/*) echo "-- Starting TR-Brick: $LABEL. Next time press Enter to go back to the main version. --" ;;
                          *) echo "-- Starting TR-Brick (close its window to stop) --" ;; esac
"$APP_BIN"
echo ""
echo "TR-Brick closed."
