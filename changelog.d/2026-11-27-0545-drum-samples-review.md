## Drum samples in the project: review follow-ups

- A copy of a kit in the project's folder that the open project does not name (another project's in a shared `Projects/`
  folder, or one left by an earlier Untitled) is no longer reused by a drop: the new copy gets a free name (`<kit>_2` ...) and
  starts from the factory kit, so a project's takes and its 8-take limit stay its own. The project's own copy is reused.
- Two quick drops on two drums of one factory kit both land, in one copy of the kit (the second was refused and left an orphan take).
- A scene or a drum track copied in one project and pasted into a project in another folder brings its project kit along
  (under a free name if that folder has another kit of the name). The files are copied before the paste takes the scene
  lock, so the sound never waits for the disk; the track menu's paste runs on the next frame for that reason.
- Tests: selftest for all three.
