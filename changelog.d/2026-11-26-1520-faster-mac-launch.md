## Mac launcher: faster start after an update

- The self-check on the Mac is skipped when GitHub's own Mac ("macOS build and selftest") already built
  this exact version and passed the same self-check. Building the self-check (one very large file) and
  running it took 2-3 minutes after every update. It still runs on the Mac when GitHub has not checked
  the version yet (checks still running, offline, documents-only change), as before.
- When the self-check is needed, it is built together with the program instead of after it.
- The version menu asks GitHub about all open pull requests at once instead of one by one.
