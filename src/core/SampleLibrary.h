#pragma once
// v44.51: the user's own sample library. One folder (default ~/Documents/TR-Brick/Library/Samples, or any folder the user
// chooses), sub-folders by instrument: Kicks/, Snares/, Hats/, Fills/ ... anywhere below it. The program never changes the
// library: it only lists it, plays a file to audition it and COPIES a chosen file into the project (samples/ beside the
// project, or the drum kit of the scene), so a project stays complete when the library moves.
//
// The category of a file is guessed from the names of its folders (nearest folder first) and then from the file name:
//   KICK SNARE CLAP HAT PERC BASS SYNTH FX LOOP, otherwise OTHER.  A tempo in the name ("170 bpm") makes a file a LOOP; a loop is
//   heard at the project tempo and goes to the SAMPLER as a LOOP with AUTO tempo fit.
// A folder  <library>/Kits/<name>/  holding kick.wav, snare.wav ... is a drum kit: SyncLibraryKits() copies it into the
// program's kits/ folder, where it shows up under MY KITS next to the factory kits.
#include <string>
#include <vector>

namespace t8 {

struct LibSample {
    std::string rel;        // path below the library root, '/' separated ("Kicks/Deep/kick_03.wav")
    std::string name;       // file name without extension
    std::string folder;     // name of the nearest folder ("Deep"), empty for files directly in the root
    std::string category;   // one of kLibCategories
    unsigned long long bytes = 0;
    double bpm = 0.0;       // v44.52.1: the tempo written in the name ("... 170 bpm"), 0 = none
    bool locked = false;    // v44.56: a protected Ableton pack sample (AIFF-C "able"): listed, but only Live can read it
};

constexpr int kNumLibCategories = 10;
inline const char* const kLibCategories[kNumLibCategories] = {"KICK", "SNARE", "CLAP", "HAT", "PERC", "BASS", "SYNTH", "FX", "LOOP", "OTHER"};

// The category of a path below the library root ("Hats/open/ohh_01.wav" -> "HAT"). Pure function, used by the scan and the tests.
std::string ClassifyLibraryPath(const std::string& rel);

// Every .wav / .wave below `root` (recursive, hidden folders skipped, "Kits" excluded), sorted by category then path.
// At most `maxFiles` entries (the rest is ignored; `truncated` tells).
std::vector<LibSample> ScanSampleLibrary(const std::string& root, bool* truncated = nullptr, size_t maxFiles = 20000);

// The library root: TRB_SAMPLES_DIR, else the folder the user chose (remembered), else <Library>/Samples (created).
std::string SamplesRoot();
void SetSamplesRoot(const std::string& dir);   // "" = back to the default

// Copies <root>/Kits/<name>/*.wav into <kitsDir>/<name>/ for the kits that are not there yet (or lack files). Returns how many
// kit folders were created or completed.
int SyncLibraryKits(const std::string& root, const std::string& kitsDir);

} // namespace t8
