#pragma once
// User-saved synth patch presets for the SYNTH tab's PRESET row -- the
// save-your-own counterpart to SynthPresets.h's 10 built-in presets,
// except these are written to/read from disk under "presets/" instead of
// being compiled in, so a live performer can dial in a sound by hand and
// save it for later (B on the PRESET row, see SynthView.cpp) instead of
// being limited to the built-in set. Same on-disk-vs-compiled-in split as
// CustomPatterns.h (user drum patterns) vs. DrumPatterns.h (built-in
// genre templates) -- deliberately header-only for the same reason
// CustomPatterns.h is: this needs its own small file-format helpers but
// doesn't need a translation unit of its own.
//
// File format ("presets/<name>.t8syn"): a flat key=value dump of every
// SynthPatch field, no [section] header needed since a preset file only
// ever holds one patch -- same field names Persistence.cpp's WriteSynth
// uses for the [synthN] section of a project save, so the two formats
// stay easy to eyeball against each other, but this is its own
// independent reader/writer (core/ deliberately doesn't reach into
// Persistence.cpp's file-local helpers -- see WaveformFromName/
// LfoTargetFromName below, small intentional duplicates of the
// same-named functions there).
#include "Types.h"
#include "FMSynth.h"
#include "Wavetables.h"
#include "SamplerText.h"
#include "UserDirs.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace t8 {

// Same idea as CustomPatterns.h's SanitizePatternFileName -- kept as its
// own copy for the same reason that one is (core/ doesn't depend on ui/).
inline std::string SanitizePresetFileName(const std::string& raw) {
    std::string out;
    for (char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == ' ') out += c;
    }
    size_t a = out.find_first_not_of(' ');
    size_t b = out.find_last_not_of(' ');
    if (a == std::string::npos) return "PRESET";
    return out.substr(a, b - a + 1);
}

// Every "*.t8syn" file directly under "presets/", name with the extension
// stripped, alphabetically sorted -- same rescan-on-every-call convention
// as ListCustomPatternNames (cheap, small folder, stays in sync with
// anything added/removed by hand on the SD card).
inline std::vector<std::string> ListCustomSynthPresetNames() {
    std::vector<std::string> out;
    DIR* d = opendir(t8::userdirs::SynthPresetsDir().c_str());
    if (d) {
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            std::string name = ent->d_name;
            const std::string ext = ".t8syn";
            if (name.size() > ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0) {
                out.push_back(name.substr(0, name.size() - ext.size()));
            }
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Writes every SynthPatch field as "presets/<name>.t8syn". Returns false
// only on a disk write failure. Overwrites if the same name is saved
// again, same as SaveCustomDrumPattern.
inline bool SaveCustomSynthPreset(const std::string& rawName, const SynthPatch& p) {
    mkdir(t8::userdirs::SynthPresetsDir().c_str(), 0755); // fine if it already exists
    std::string path = (t8::userdirs::SynthPresetsDir() + "/") + SanitizePresetFileName(rawName) + ".t8syn";
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "waveform=" << WaveformName(p.waveform) << "\n";
    f << "cutoffHz=" << p.cutoffHz << "\n";
    f << "resonance=" << p.resonance << "\n";
    f << "envAmount=" << p.envAmount << "\n";
    f << "attack=" << p.attack << "\n";
    f << "decay=" << p.decay << "\n";
    f << "sustain=" << p.sustain << "\n";
    f << "release=" << p.release << "\n";
    f << "filterDecay=" << p.filterDecay << "\n";
    f << "glideTime=" << p.glideTime << "\n";
    f << "volume=" << p.volume << "\n";
    f << "lfoTarget=" << LfoTargetName(p.lfoTarget) << "\n";
    f << "lfoRateHz=" << p.lfoRateHz << "\n";
    f << "lfoDepth=" << p.lfoDepth << "\n";
    f << "lfoShape=" << WaveformName(p.lfoShape) << "\n";
    // engine + the v26 thickening (all optional on read)
    f << "engineType=" << (p.engineType == SynthEngineType::FM ? "FM" : p.engineType == SynthEngineType::Wavetable ? "WT" : p.engineType == SynthEngineType::Sample ? "SMP" : "SUB") << "\n";
    f << "fmAlgorithm=" << p.fmAlgorithm << "\n";
    for (int i = 0; i < 4; ++i) { f << "fmOp" << i << "Ratio=" << p.fmOps[i].ratio << "\n"; f << "fmOp" << i << "Level=" << p.fmOps[i].level << "\n"; }
    f << "wavetableBank=" << p.wavetableBank << "\n";
    if (p.fmIndexScale != 8.0f) f << "fmIndexScale=" << p.fmIndexScale << "\n";
    if (p.fmModEnv > 0.0f) f << "fmModEnv=" << p.fmModEnv << "\n";
    f << "wtPosition=" << p.wtPosition << "\n";
    f << "unisonVoices=" << p.unisonVoices << "\n";
    f << "unisonDetune=" << p.unisonDetune << "\n";
    f << "unisonSpread=" << p.unisonSpread << "\n";
    f << "subLevel=" << p.subLevel << "\n";
    f << "subOctave=" << p.subOctave << "\n";
    f << "noiseLevel=" << p.noiseLevel << "\n";
    f << "drive=" << p.drive << "\n";
    if (p.phaseSync) f << "phaseSync=1\n";
    f << "osc2Wave=" << WaveformName(p.osc2Wave) << "\n";
    f << "osc2Level=" << p.osc2Level << "\n";
    f << "osc2Semi=" << p.osc2Semi << "\n";
    f << "osc2Detune=" << p.osc2Detune << "\n";
    f << "filter24=" << (p.filter24 ? 1 : 0) << "\n";
    f << "acid=" << (p.acid ? 1 : 0) << "\n";
    f << "drift=" << p.drift << "\n";
    f << "keytrack=" << p.keytrack << "\n";
    if (p.engineType == SynthEngineType::Sample || p.sample.HasFile()) f << "sample=" << SamplerToString(ToSamplerPatch(p.sample)) << "\n";   // v44.53
    return true;
}

// Reads back "presets/<name>.t8syn". Returns false if the file doesn't
// exist / can't be opened (e.g. deleted from the SD card between the
// PRESET row being browsed and A being pressed). Every key is optional on
// read -- a missing key just leaves that field at SynthPatch's own struct
// default, same backward-compat convention as Persistence.cpp.
inline bool LoadCustomSynthPreset(const std::string& name, SynthPatch& out) {
    std::string path = (t8::userdirs::SynthPresetsDir() + "/") + name + ".t8syn";
    std::ifstream f(path);
    if (!f) return false;
    out = SynthPatch{};

    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return std::string();
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    };

    std::string line;
    while (std::getline(f, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty()) continue;
        size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(trimmed.substr(0, eq));
        std::string val = trim(trimmed.substr(eq + 1));
        if (val.empty()) continue;

        if (key == "waveform") {
            if (val == "Sine") out.waveform = Waveform::Sine;
            else if (val == "Tri") out.waveform = Waveform::Triangle;
            else if (val == "Saw") out.waveform = Waveform::Saw;
            else if (val == "Sqr") out.waveform = Waveform::Square;
        } else if (key == "cutoffHz") out.cutoffHz = static_cast<float>(std::atof(val.c_str()));
        else if (key == "resonance") out.resonance = static_cast<float>(std::atof(val.c_str()));
        else if (key == "envAmount") out.envAmount = static_cast<float>(std::atof(val.c_str()));
        else if (key == "attack") out.attack = static_cast<float>(std::atof(val.c_str()));
        else if (key == "decay") out.decay = static_cast<float>(std::atof(val.c_str()));
        else if (key == "sustain") out.sustain = static_cast<float>(std::atof(val.c_str()));
        else if (key == "release") out.release = static_cast<float>(std::atof(val.c_str()));
        else if (key == "filterDecay") out.filterDecay = static_cast<float>(std::atof(val.c_str()));
        else if (key == "glideTime") out.glideTime = static_cast<float>(std::atof(val.c_str()));
        else if (key == "volume") out.volume = static_cast<float>(std::atof(val.c_str()));
        else if (key == "lfoTarget") out.lfoTarget = LfoTargetFromText(val);
        else if (key == "lfoRateHz") out.lfoRateHz = static_cast<float>(std::atof(val.c_str()));
        else if (key == "lfoDepth") out.lfoDepth = static_cast<float>(std::atof(val.c_str()));
        else if (key == "lfoShape") {
            if (val == "Sine") out.lfoShape = Waveform::Sine;
            else if (val == "Tri") out.lfoShape = Waveform::Triangle;
            else if (val == "Saw") out.lfoShape = Waveform::Saw;
            else if (val == "Sqr") out.lfoShape = Waveform::Square;
        }
        else if (key == "engineType") out.engineType = val == "FM" ? SynthEngineType::FM : val == "WT" ? SynthEngineType::Wavetable : val == "SMP" ? SynthEngineType::Sample : SynthEngineType::Subtractive;
        else if (key == "fmAlgorithm") out.fmAlgorithm = std::clamp(std::atoi(val.c_str()), 0, kNumFmAlgorithms - 1);
        else if (key.rfind("fmOp", 0) == 0 && key.size() > 5) {
            int op = key[4] - '0';
            if (op >= 0 && op < 4) {
                if (key.substr(5) == "Ratio") out.fmOps[op].ratio = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.25f, 16.0f);
                else if (key.substr(5) == "Level") out.fmOps[op].level = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.0f, 1.0f);
            }
        }
        else if (key == "wavetableBank") out.wavetableBank = std::clamp(std::atoi(val.c_str()), 0, kNumWavetableBanks - 1);
        else if (key == "fmIndexScale") out.fmIndexScale = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.25f, 8.0f);
        else if (key == "fmModEnv") out.fmModEnv = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.0f, 1.0f);
        else if (key == "wtPosition") out.wtPosition = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.0f, 1.0f);
        else if (key == "unisonVoices") out.unisonVoices = std::clamp(std::atoi(val.c_str()), 1, kMaxUnisonVoices);
        else if (key == "unisonDetune") out.unisonDetune = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.0f, 1.0f);
        else if (key == "unisonSpread") out.unisonSpread = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.0f, 1.0f);
        else if (key == "subLevel") out.subLevel = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.0f, 1.0f);
        else if (key == "subOctave") out.subOctave = std::clamp(std::atoi(val.c_str()), 1, 2);
        else if (key == "noiseLevel") out.noiseLevel = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.0f, 1.0f);
        else if (key == "phaseSync") out.phaseSync = std::atoi(val.c_str()) != 0;
        else if (key == "drive") out.drive = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.0f, 1.0f);
        else if (key == "osc2Wave") {
            if (val == "Sine") out.osc2Wave = Waveform::Sine;
            else if (val == "Tri") out.osc2Wave = Waveform::Triangle;
            else if (val == "Saw") out.osc2Wave = Waveform::Saw;
            else if (val == "Sqr") out.osc2Wave = Waveform::Square;
        }
        else if (key == "osc2Level") out.osc2Level = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.0f, 1.0f);
        else if (key == "osc2Semi") out.osc2Semi = std::clamp(std::atoi(val.c_str()), -24, 24);
        else if (key == "osc2Detune") out.osc2Detune = std::clamp(std::atoi(val.c_str()), -50, 50);
        else if (key == "filter24") out.filter24 = std::atoi(val.c_str()) != 0;
        else if (key == "acid") out.acid = std::atoi(val.c_str()) != 0;
        else if (key == "drift") out.drift = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.0f, 1.0f);
        else if (key == "keytrack") out.keytrack = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.0f, 1.0f);
        else if (key == "sample") FromSamplerPatch(out.sample, SamplerFromString(val));   // v44.53
    }
    return true;
}

} // namespace t8
