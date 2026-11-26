# changelog.d: one small file per pull request

**Do not edit `CHANGELOG.md` in a normal pull request.** Almost every PR used to add its entry at the
top of `CHANGELOG.md`, so every merge into main gave all other open PRs a conflict there. GitHub does
not run CI on a PR with a conflict, so each one had to be re-merged and wait for the full CI again.
A new file never conflicts.

## How to add your entry

```sh
tools/changelog.sh new short-slug "Fix: what changed, in plain words"
```

This creates `changelog.d/YYYY-MM-DD-HHMM-short-slug.md` (UTC time, so files sort newest first).
Write the entry exactly as it should appear in `CHANGELOG.md`: a `## Title` line, an empty line,
then the bullets. English, like the rest of the changelog. One fragment per PR; review fixes edit the
same fragment.

```markdown
## Fix: the arrow keys move the selected notes in the piano roll again

- After a click in the piano roll the arrow keys stay with the selected notes ...
```

- `tools/changelog.sh preview` shows all pending entries, newest first.
- `tools/changelog.sh check` checks the names and the `## ` first line (CI runs it too).

## Release (folding the fragments into CHANGELOG.md)

`tools/changelog.sh release` puts all fragments at the top of `CHANGELOG.md` (newest first) and deletes
them. Do it in its own small PR (🟢), e.g. at the end of a stage or before a version zip. Only such a
PR may change `CHANGELOG.md`: CI fails a PR that edits `CHANGELOG.md` without removing fragments.
Until the release, the newest changes are here, not in `CHANGELOG.md`.
