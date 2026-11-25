#pragma once
// v44.49: STYLE = one named package that gathers what a style needs in one place: the drum pattern (with swing and the usual scene
// length), the drum kit, a groove, the sound of bass / synth 2 / synth 3 / chord, a bass line, a chord progression and a tempo.
// Everything here is picked from the library the program already has (AllGenrePatterns, AllSynthPresets, AllChordProgressions,
// BuiltinGrooves, the kits folder) and written from convention, like those: a style is a starting point in the manner of its
// genre, not a copy of any record. Applying a style (AppState::ApplyStyle) writes it into the edited scene as ONE undo step; the
// key and the scale of the project are never changed (the notes follow them).
#include <string>
#include <vector>

namespace t8 {

// One note of the bass line, written in 16ths of ONE bar (like the genre patterns). `semis` is the distance from the root of the
// key in semitones; the note is snapped into the current scale, so the same line is minor in a minor key and so on.
struct StyleBassNote { int step; int semis; int len; bool slide = false; };

struct StyleDef {
    const char* name;
    const char* category;      // HOUSE TECHNO ELECTRO BREAKS HIP HOP (the browser groups by it)
    const char* tags;          // words the browser filters by, separated by |: DRIVING DARK LAID BACK SWUNG SLOW FAST
    const char* pattern;       // name in AllGenrePatterns()
    int bpm;
    const char* kit;           // kit folder, e.g. "kits/909"
    const char* groove;        // name in BuiltinGrooves() or "" for none
    float grooveAmount;
    const char* bass;          // names in AllSynthPresets()
    const char* syn2;
    const char* syn3;
    const char* chord;
    const char* progression;   // name in AllChordProgressions()
    int chordBeats;            // length of one chord in beats
    int bassOctave;            // octave of the root (the project's octave convention: octave 2 = C2 = MIDI 36)
    std::vector<StyleBassNote> bassLine;
};

const std::vector<StyleDef>& AllStyles();

// Lookups by name (-1 when the library has no such entry). Used by ApplyStyle and by the self-test that checks every style resolves.
int FindGenrePatternIndex(const std::string& name);
int FindSynthPresetIndex(const std::string& name);
int FindProgressionIndex(const std::string& name);
int FindBuiltinGrooveIndex(const std::string& name);

} // namespace t8
