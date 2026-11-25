#!/bin/bash
# Runs the offline tests in a throw-away folder, so the files they write
# (selftest_*.t8proj, selftest_data/, ui_settings.cfg, ...) never land in the repo.
#
#   tools/run_tests.sh <build-dir> selftest            # t8_selftest
#   tools/run_tests.sh <build-dir> fuzz [fuzz args]    # t8_fuzz, e.g. --seeds 30
#
# The tests read the sources by relative path (src/..., kits/808, CMakeLists.txt),
# so the scratch folder gets a symlink to every top-level entry of the repo and the
# test runs from there. Writes into new top-level names stay in the scratch folder.
# Exit code = the test's exit code. Set T8_KEEP_TMP=1 to keep the folder for a look.

set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:?usage: run_tests.sh <build-dir> selftest|fuzz [args]}"
WHAT="${2:?usage: run_tests.sh <build-dir> selftest|fuzz [args]}"
shift 2
BUILD="$(cd "$BUILD" && pwd)" || { echo "run_tests: no build folder $BUILD"; exit 2; }

case "$WHAT" in
    selftest) BIN="$BUILD/t8_selftest" ;;
    fuzz)     BIN="$BUILD/t8_fuzz" ;;
    *) echo "run_tests: unknown test '$WHAT' (selftest or fuzz)"; exit 2 ;;
esac
[ -x "$BIN" ] || { echo "run_tests: $BIN is not built"; exit 2; }

TMP="$(mktemp -d "${TMPDIR:-/tmp}/t8_${WHAT}.XXXXXX")" || exit 2
cleanup() { [ -n "${T8_KEEP_TMP:-}" ] && echo "run_tests: kept $TMP" || rm -rf "$TMP"; }
trap cleanup EXIT

for e in "$REPO"/* "$REPO"/.[!.]*; do
    [ -e "$e" ] || continue
    name="$(basename "$e")"
    case "$name" in .git|build|build-*|selftest_*|ui_settings.cfg|imgui.ini) continue ;; esac
    ln -s "$e" "$TMP/$name"
done

# t8_selftest is one very large main(); sanitizer builds need more than the default 8 MB stack
ulimit -s "$(ulimit -H -s)" 2>/dev/null

cd "$TMP" || exit 2
"$BIN" "$@"
