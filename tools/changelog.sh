#!/usr/bin/env bash
# Changelog fragments: every pull request adds ONE new file changelog.d/<YYYY-MM-DD-HHMM>-<slug>.md
# instead of editing the top of CHANGELOG.md. New files never conflict, so a PR stays mergeable
# (and its CI keeps running) when main moves. See changelog.d/README.md.
#
#   tools/changelog.sh new <slug> "<Title>"   create a fragment with the right name (UTC time)
#   tools/changelog.sh preview                print the fragments, newest first, as they will appear
#   tools/changelog.sh check                  check names and that each fragment starts with "## "
#   tools/changelog.sh release                fold all fragments into CHANGELOG.md (newest first, under
#                                             the intro) and delete them; done in its own small PR
set -euo pipefail
cd "$(dirname "$0")/.."
DIR=changelog.d
NAME_RE='^[0-9]{4}-[0-9]{2}-[0-9]{2}-[0-9]{4}-[a-z0-9][a-z0-9-]*\.md$'

fragments() {  # newest first; README.md is not a fragment
    ls "$DIR" 2>/dev/null | grep -v '^README\.md$' | grep '\.md$' | sort -r || true
}

check() {
    local bad=0 f
    for f in $(ls "$DIR" | grep -v '^README\.md$' || true); do
        if ! echo "$f" | grep -qE "$NAME_RE"; then
            echo "changelog.d/$f: name must be YYYY-MM-DD-HHMM-short-slug.md (lower case, UTC time)"; bad=1; continue
        fi
        if [ "$(head -c 3 "$DIR/$f")" != "## " ]; then
            echo "changelog.d/$f: must start with a '## Title' line"; bad=1
        fi
        if grep -nE '^(<<<<<<<|=======|>>>>>>>)' "$DIR/$f"; then
            echo "changelog.d/$f: merge conflict markers"; bad=1
        fi
    done
    return $bad
}

preview() {
    local f
    for f in $(fragments); do cat "$DIR/$f"; printf '\n'; done
}

case "${1:-}" in
    new)
        slug="${2:?usage: tools/changelog.sh new <slug> \"<Title>\"}"; title="${3:-Title}"
        f="$DIR/$(date -u +%Y-%m-%d-%H%M)-$slug.md"
        [ -e "$f" ] && { echo "$f already exists"; exit 1; }
        printf '## %s\n\n- \n' "$title" > "$f"; echo "$f" ;;
    preview) preview ;;
    check) check && echo "changelog fragments OK" ;;
    release)
        check
        [ -n "$(fragments)" ] || { echo "no fragments in $DIR"; exit 0; }
        body="$(mktemp)"; out="$(mktemp)"
        preview > "$body"
        # the intro is everything before the first "## " heading
        awk -v body="$body" '
            !done && /^## / { while ((getline l < body) > 0) print l; done = 1 }
            { print }
            END { if (!done) while ((getline l < body) > 0) print l }
        ' CHANGELOG.md > "$out"
        mv "$out" CHANGELOG.md; rm -f "$body"
        for f in $(fragments); do
            if git ls-files --error-unmatch "$DIR/$f" >/dev/null 2>&1; then git rm -qf "$DIR/$f"; else rm -f "$DIR/$f"; fi
        done
        echo "CHANGELOG.md updated; fragments removed" ;;
    *) sed -n '2,12p' "$0"; exit 1 ;;
esac
