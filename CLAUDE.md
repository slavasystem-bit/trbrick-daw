# Notes for agents working in this repository

- **Changelog: never edit `CHANGELOG.md` in a normal PR.** Add one file
  `changelog.d/YYYY-MM-DD-HHMM-slug.md` with `tools/changelog.sh new slug "Title"` (English,
  starts with `## Title`). Details: `changelog.d/README.md`. Editing the top of `CHANGELOG.md`
  makes every other open PR conflict, and GitHub then stops running their CI.
- Working rules, languages, build and tests: `docs/HANDOFF.md` (Ukrainian).
