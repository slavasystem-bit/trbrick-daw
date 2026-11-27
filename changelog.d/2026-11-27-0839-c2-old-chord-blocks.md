## Stage C2 part 3b: the old chord blocks are gone

- Removed, because CHORD LAB does it now: the VOICING block of the chord instrument (triad / 7th, STACK / ARP, arp style and rate, chord length), the CHORD EDITOR window (`ChordEditor.h`) and the CHORD column of RANDOM / KIT.
- A double-click on a chord slot (LIVE, the CHORDS row) opens CHORD LAB on that slot; its right-click menu says "Open in CHORD LAB".
- RANDOM (chord instrument block), START FROM (CHORD LAB) and the style chords write held triads; ARP / STRUM / RHYTHM are the PLAY layer, never baked into steps. `ChordOutputMode`, `ArpStyle` and the `chd*` voicing settings are gone; fuzz op 52 now applies a named progression.
