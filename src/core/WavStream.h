#pragma once
// N1 (audit v44.58 LIVE-1 / LIVE-7): a WAV file written while it is being recorded. The recorders used to keep the whole take
// in memory and write it in one go on STOP, on the UI thread, while the audio thread waited for the scene lock (about 0.25 s
// of silence per 10 minutes recorded), and a crash lost the whole set. Now the drain thread appends every few milliseconds
// and refreshes the header sizes about once a second (Flush), so a crash leaves a playable file up to the last second.
// 16-bit PCM, the same encoding as WriteWavPCM16. Not thread-safe: one thread (the recorder's drain thread) owns it.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace t8 {

class WavStreamWriter {
public:
    ~WavStreamWriter() { Close(); }
    // Creates (or truncates) the file and writes a header for an empty file.
    bool Open(const std::string& path, int channels, int sampleRate);
    // Appends `frames` frames of interleaved float. Returns false on a write error or when the file reached the WAV limit
    // (4 GB; see Full()); nothing more is written then, and what is already there stays valid after Close().
    bool Write(const float* interleaved, size_t frames);
    // Rewrites the header sizes for what is written so far and pushes the data to the disk cache.
    bool Flush();
    // Final header, closes the file. Returns false if anything failed on the way. Safe to call twice.
    bool Close();

    bool IsOpen() const { return f_ != nullptr; }
    bool Full() const { return full_; }
    bool Failed() const { return failed_; }
    uint64_t Frames() const { return channels_ > 0 ? dataBytes_ / (2u * static_cast<unsigned>(channels_)) : 0; }
    const std::string& Path() const { return path_; }

    // Largest data chunk a 32-bit WAV header can describe (whole frames).
    static uint64_t MaxDataBytes(int channels);
    // Tests: pretend the limit is this small (0 = the real one).
    static void SetMaxDataBytesForTest(uint64_t bytes);

private:
    bool WriteHeader();
    std::FILE* f_ = nullptr;
    std::string path_;
    int channels_ = 2, sampleRate_ = 44100;
    uint64_t dataBytes_ = 0;
    bool full_ = false, failed_ = false;
    std::vector<int16_t> pcm_;   // conversion buffer, grows once
};

} // namespace t8
