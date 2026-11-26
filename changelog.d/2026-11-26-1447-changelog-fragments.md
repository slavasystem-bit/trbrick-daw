## Process: changelog entries go into changelog.d/ (no more CHANGELOG conflicts)

- Every pull request now adds its own small file `changelog.d/YYYY-MM-DD-HHMM-slug.md` (`tools/changelog.sh new slug "Title"`) instead of editing the top of `CHANGELOG.md`. A new file never conflicts, so an open PR stays mergeable and its CI keeps running when main moves (before, every merge gave the other open PRs a CHANGELOG conflict and a fresh ~17-minute CI run).
- `tools/changelog.sh release` folds the fragments into `CHANGELOG.md` (newest first) in its own small PR; `preview` shows the pending ones.
- CI: the fragments are checked (name, `## ` title), and a PR that edits `CHANGELOG.md` without folding fragments fails with a hint.
- Rules for all contributors: `changelog.d/README.md`, `CLAUDE.md`, `docs/HANDOFF.md`.
