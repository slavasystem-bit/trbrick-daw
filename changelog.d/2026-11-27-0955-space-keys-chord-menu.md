## Fix: Space and the other keys work again after a right-click in CHORD LAB

- A right-click on a chord of the CHORD LAB loop opened its menu under the wrong id: the menu never showed, but the program
  counted it as open, so Space (play / stop), M, C, F, Tab and every other key shortcut stopped working until restart. The menu
  now opens (length, split, delete) and Esc closes it.
- Safety net: a popup that is opened but never drawn is closed one frame later (`CloseOrphanPopups` in `Shell.cpp`), so such a
  mistake anywhere else can no longer take the keyboard away. The UI test fails when the net has to close one.
- Space also counts when the window reports a new press while an earlier Space still looks held (a lost key-up), and a Space
  that is held back says why in the info line (a menu or window is open; Cmd or Ctrl looks held).
- UI test: Space starts and stops on LIVE and TRACK with each tool open, after chords are played in CHORD LAB, after the
  right-click menu of a loop chord, and after a lost key-up.
