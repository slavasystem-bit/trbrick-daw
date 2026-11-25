#include "WavStream.h"
#include <algorithm>
#include <atomic>

namespace t8 {

namespace {
std::atomic<uint64_t> gTestLimit{0};
void Put16(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
void Put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
} // namespace

uint64_t WavStreamWriter::MaxDataBytes(int channels) {
    const uint64_t frame = 2u * static_cast<unsigned>(std::max(1, channels));
    const uint64_t t = gTestLimit.load();
    const uint64_t lim = t ? t : 0xFFFFFFFFull - 36u;   // RIFF size = 36 + data must fit in 32 bits
    return lim / frame * frame;
}
void WavStreamWriter::SetMaxDataBytesForTest(uint64_t bytes) { gTestLimit.store(bytes); }

bool WavStreamWriter::Open(const std::string& path, int channels, int sampleRate) {
    Close();
    path_ = path; channels_ = std::max(1, channels); sampleRate_ = sampleRate;
    dataBytes_ = 0; full_ = false; failed_ = false;
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) { failed_ = true; return false; }
    if (!WriteHeader()) { std::fclose(f_); f_ = nullptr; failed_ = true; return false; }
    return true;
}

bool WavStreamWriter::WriteHeader() {
    uint8_t h[44];
    const uint32_t data = static_cast<uint32_t>(dataBytes_);
    std::copy_n("RIFF", 4, h); Put32(h + 4, 36u + data); std::copy_n("WAVE", 4, h + 8);
    std::copy_n("fmt ", 4, h + 12); Put32(h + 16, 16); Put16(h + 20, 1); Put16(h + 22, static_cast<uint16_t>(channels_));
    Put32(h + 24, static_cast<uint32_t>(sampleRate_)); Put32(h + 28, static_cast<uint32_t>(sampleRate_ * channels_ * 2));
    Put16(h + 32, static_cast<uint16_t>(channels_ * 2)); Put16(h + 34, 16);
    std::copy_n("data", 4, h + 36); Put32(h + 40, data);
    if (std::fseek(f_, 0, SEEK_SET) != 0) return false;
    if (std::fwrite(h, 1, sizeof h, f_) != sizeof h) return false;
    return std::fseek(f_, 0, SEEK_END) == 0;
}

bool WavStreamWriter::Write(const float* interleaved, size_t frames) {
    if (!f_ || failed_ || full_) return false;
    const uint64_t frameBytes = 2u * static_cast<unsigned>(channels_);
    const uint64_t room = (MaxDataBytes(channels_) - std::min(dataBytes_, MaxDataBytes(channels_))) / frameBytes;
    if (frames > room) { frames = static_cast<size_t>(room); full_ = true; }
    const size_t n = frames * static_cast<size_t>(channels_);
    if (pcm_.size() < n) pcm_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const float v = std::max(-1.0f, std::min(1.0f, interleaved[i]));
        pcm_[i] = static_cast<int16_t>(v * 32767.0f);   // little-endian on every machine this runs on (as WriteWavPCM16)
    }
    if (n && std::fwrite(pcm_.data(), sizeof(int16_t), n, f_) != n) { failed_ = true; return false; }
    dataBytes_ += frames * frameBytes;
    return !full_;
}

bool WavStreamWriter::Flush() {
    if (!f_) return false;
    if (!WriteHeader() || std::fflush(f_) != 0) failed_ = true;
    return !failed_;
}

bool WavStreamWriter::Close() {
    if (!f_) return !failed_;
    if (!WriteHeader()) failed_ = true;
    if (std::fclose(f_) != 0) failed_ = true;
    f_ = nullptr;
    return !failed_;
}

} // namespace t8
