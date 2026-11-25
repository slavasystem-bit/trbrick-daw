#pragma once
// Minimal WAV / AIFF reader and WAV writer: WAV PCM 8/16/24/32 or float, AIFF / AIFF-C PCM 8..32 bit and float, mono or stereo, any sample
// rate (loaded samples get converted to the engine's kSampleRate at load
// time so playback pitch is always correct regardless of source file rate).
#include <cctype>
#include <string>
#include <vector>
#include <cstdint>

namespace t8 {

struct SampleBuffer {
    std::vector<float> data; // interleaved if channels==2, else mono
    int channels = 1;
    int sampleRate = 44100;
    int frames() const { return channels > 0 ? static_cast<int>(data.size()) / channels : 0; }
    bool empty() const { return data.empty(); }
};

// v44.56: the kinds of sample file the program reads. LoadWav reads WAV and AIFF / AIFF-C by what is inside the file (its header), so a
// misnamed file still opens; these helpers decide by the name (lists, drops, pickers).
inline std::string SampleFileExt(const std::string& path) {
    const size_t dot = path.find_last_of('.'), slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return std::string();
    std::string e = path.substr(dot);
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}
inline bool IsAiffFileName(const std::string& path) { const std::string e = SampleFileExt(path); return e == ".aif" || e == ".aiff" || e == ".aifc"; }
inline bool IsWavFileName(const std::string& path) { const std::string e = SampleFileExt(path); return e == ".wav" || e == ".wave"; }
inline bool IsSampleFileName(const std::string& path) { return IsWavFileName(path) || IsAiffFileName(path); }

// v44.56: why a file cannot be read, for the info line: "" when nothing special is known (a damaged file, not a sample ...), else a short sentence.
// Ableton Live packs store their samples as AIFF-C "able" ("Ableton Content"): the audio is protected and only Live itself can open it.
std::string UnreadableReason(const std::string& path);
// True for such a protected Ableton pack file (a cheap header check; the scan of the library uses it).
bool IsProtectedAbletonAiff(const std::string& path);

// Copies a sample file to `dstWav` as a WAV: a WAV is copied byte for byte, an AIFF is decoded (at its own sample rate) and written as 24-bit
// PCM. Used where the project needs real .wav files (drum kits, transitions). Returns false when it cannot be read or written.
bool ImportAudioAsWav(const std::string& src, const std::string& dstWav);

// Loads a WAV or AIFF / AIFF-C file and resamples (windowed sinc) to targetSampleRate.
// Returns false on failure (missing file / unsupported format); logs to stderr.
bool LoadWav(const std::string& path, SampleBuffer& out, int targetSampleRate);

// Writes a mono or stereo float buffer as 16-bit PCM WAV. Used by the
// offline self-test renderer, and available for users who want to bounce
// a pattern to a file.
bool WriteWavPCM16(const std::string& path, const std::vector<float>& interleaved,
                    int channels, int sampleRate);

// 24-bit PCM writer (used by the kit generator tool).
bool WriteWavPCM24(const std::string& path, const std::vector<float>& interleaved, int channels, int sampleRate);

// Same encoding as WriteWavPCM16, but returns the complete file bytes
// (RIFF/WAVE header + 16-bit PCM data) in memory instead of writing to
// disk (WriteWavPCM16 writes these bytes; the live recorders stream with
// WavStreamWriter, see WavStream.h, using the same encoding).
std::vector<uint8_t> BuildWavPCM16Bytes(const std::vector<float>& interleaved,
                                         int channels, int sampleRate);

} // namespace t8
