#include "WavFile.h"
#include "Resample.h"
#include "FileSafety.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <iostream>
#include <filesystem>

namespace t8 {

namespace {
struct RiffChunk { char id[4]; uint32_t size; };

uint32_t rd32(const uint8_t* p) { return p[0] | (p[1]<<8) | (p[2]<<16) | (uint32_t(p[3])<<24); }
uint16_t rd16(const uint8_t* p) { return p[0] | (p[1]<<8); }

// LIVE-8: a damaged or hostile file must never make the reader go past its buffers, allocate gigabytes or resample forever.
// A chunk can claim any size (up to 4 GB); what is read is limited to what the file really holds, and audio data larger than
// this is refused (3 minutes of 8-channel 192 kHz 32-bit audio is ~440 MB).
constexpr uint64_t kMaxAudioBytes = 512ull * 1024 * 1024;
uint64_t FileSize(FILE* f) {
    const long here = std::ftell(f);
    if (here < 0 || std::fseek(f, 0, SEEK_END) != 0) return 0;
    const long end = std::ftell(f);
    std::fseek(f, here, SEEK_SET);
    return end > 0 ? static_cast<uint64_t>(end) : 0;
}
uint64_t BytesLeft(FILE* f, uint64_t fileSize) {
    const long here = std::ftell(f);
    return (here < 0 || static_cast<uint64_t>(here) >= fileSize) ? 0 : fileSize - static_cast<uint64_t>(here);
}
// Rates outside this make no sense for audio and would make the resampler produce absurd amounts of data.
bool PlausibleRate(double r) { return r >= 1000.0 && r <= 768000.0; }
} // namespace

namespace {
bool DecodeWavFile(const std::string& path, std::vector<float>& srcOut, int& srcChannelsOut, uint32_t& sampleRateOut) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::cerr << "[wav] cannot open " << path << "\n";
        return false;
    }
    uint8_t riffHdr[12];
    if (std::fread(riffHdr, 1, 12, f) != 12 || std::memcmp(riffHdr, "RIFF", 4) != 0 ||
        std::memcmp(riffHdr + 8, "WAVE", 4) != 0) {
        std::cerr << "[wav] not a RIFF/WAVE file: " << path << "\n";
        std::fclose(f);
        return false;
    }

    uint16_t formatTag = 1, numChannels = 1, bitsPerSample = 16;
    uint32_t sampleRate = 44100;
    std::vector<uint8_t> dataBytes;
    bool haveFmt = false, haveData = false;

    const uint64_t fileSize = FileSize(f);
    bool tooBig = false;
    uint8_t chunkHdr[8];
    while (std::fread(chunkHdr, 1, 8, f) == 8) {
        char id[5] = {0};
        std::memcpy(id, chunkHdr, 4);
        uint32_t size = rd32(chunkHdr + 4);
        if (std::memcmp(id, "fmt ", 4) == 0) {
            if (size < 16 || size > 4096) break;   // LIVE-8: the fields below are 16 bytes; a shorter chunk was read past its end
            std::vector<uint8_t> fmt(size);
            if (std::fread(fmt.data(), 1, size, f) != size) break;
            formatTag     = rd16(fmt.data() + 0);
            numChannels   = rd16(fmt.data() + 2);
            sampleRate    = rd32(fmt.data() + 4);
            bitsPerSample = rd16(fmt.data() + 14);
            if (formatTag == 0xFFFE && size >= 26) formatTag = rd16(fmt.data() + 24);   // v44.50: WAVE_FORMAT_EXTENSIBLE (what most DAWs write for 24-bit, 32-bit and surround): the real tag is the first two bytes of the sub-format
            haveFmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            const uint64_t left = BytesLeft(f, fileSize);   // LIVE-8: a header that claims 4 GB only gets what the file holds
            if (std::min<uint64_t>(size, left) > kMaxAudioBytes) { tooBig = true; break; }
            dataBytes.resize(static_cast<size_t>(std::min<uint64_t>(size, left)));
            const size_t got = std::fread(dataBytes.data(), 1, dataBytes.size(), f);   // v44.50: a file cut short (or a streamed one with a wrong size) still plays what is there
            dataBytes.resize(got);
            haveData = got > 0;
            if (got != size) break;
        } else {
            // skip unknown chunk (LIST, fact, etc.)
            if (std::fseek(f, static_cast<long>(size), SEEK_CUR) != 0) break;
        }
        if (size % 2 == 1) std::fseek(f, 1, SEEK_CUR); // chunks are word-aligned
        if (haveFmt && haveData) break;
    }
    std::fclose(f);

    if (tooBig) { std::cerr << "[wav] audio data larger than " << (kMaxAudioBytes >> 20) << " MB, refused: " << path << "\n"; return false; }
    if (!haveFmt || !haveData) {
        std::cerr << "[wav] missing fmt/data chunk: " << path << "\n";
        return false;
    }
    if (numChannels > 64 || !PlausibleRate(sampleRate)) { std::cerr << "[wav] bad format (channels " << numChannels << ", rate " << sampleRate << ") in " << path << "\n"; return false; }

    int srcChannels = numChannels > 0 ? numChannels : 1;
    std::vector<float> src;
    if (formatTag == 1 && bitsPerSample == 16) {
        size_t n = dataBytes.size() / 2;
        src.resize(n);
        const int16_t* s = reinterpret_cast<const int16_t*>(dataBytes.data());
        for (size_t i = 0; i < n; ++i) src[i] = s[i] / 32768.0f;
    } else if (formatTag == 1 && bitsPerSample == 8) {
        size_t n = dataBytes.size();
        src.resize(n);
        for (size_t i = 0; i < n; ++i) src[i] = (dataBytes[i] - 128) / 128.0f;
    } else if (formatTag == 3 && bitsPerSample == 32) { // IEEE float
        size_t n = dataBytes.size() / 4;
        src.resize(n);
        std::memcpy(src.data(), dataBytes.data(), n * 4);
    } else if (formatTag == 1 && bitsPerSample == 24) {
        size_t n = dataBytes.size() / 3;
        src.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const uint8_t* b = &dataBytes[i * 3];
            int32_t v = (int32_t)(b[0] | (b[1] << 8) | (b[2] << 16));
            if (v & 0x800000) v |= ~0xFFFFFF; // sign extend
            src[i] = v / 8388608.0f;
        }
    } else if (formatTag == 1 && bitsPerSample == 32) {   // v44.50: 32-bit integer PCM
        size_t n = dataBytes.size() / 4;
        src.resize(n);
        for (size_t i = 0; i < n; ++i) { int32_t v; std::memcpy(&v, &dataBytes[i * 4], 4); src[i] = static_cast<float>(v / 2147483648.0); }
    } else if (formatTag == 3 && bitsPerSample == 64) {   // v44.50: 64-bit float
        size_t n = dataBytes.size() / 8;
        src.resize(n);
        for (size_t i = 0; i < n; ++i) { double v; std::memcpy(&v, &dataBytes[i * 8], 8); src[i] = static_cast<float>(v); }
    } else {
        std::cerr << "[wav] unsupported format tag=" << formatTag << " bits=" << bitsPerSample << " in " << path << "\n";
        return false;
    }

    src.resize(src.size() - src.size() % static_cast<size_t>(srcChannels));   // whole frames only
    if (src.empty()) { std::cerr << "[wav] no audio in " << path << "\n"; return false; }
    srcOut = std::move(src); srcChannelsOut = srcChannels; sampleRateOut = sampleRate;
    return true;
}

// ---- AIFF / AIFF-C (v44.56) ------------------------------------------------------------------------------------------------------
uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]); }
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
// 80-bit IEEE extended (sign + 15 bit exponent + 64 bit mantissa, big endian) -> double
double Extended80(const uint8_t* p) {
    const int sign = (p[0] & 0x80) ? -1 : 1;
    const int exp = ((p[0] & 0x7F) << 8) | p[1];
    uint64_t mant = 0;
    for (int i = 0; i < 8; ++i) mant = (mant << 8) | p[2 + i];
    if (exp == 0 && mant == 0) return 0.0;
    if (exp == 0x7FFF) return 0.0;   // inf / nan: not a rate
    return sign * std::ldexp(static_cast<double>(mant), exp - 16383 - 63);
}

// G.711 companded bytes (AIFF-C "ulaw" / "alaw") -> -1..1
float DecodeUlaw(uint8_t u) { u = static_cast<uint8_t>(~u); const int sign = u & 0x80, exp = (u >> 4) & 7, mant = u & 15; int v = ((mant << 3) + 0x84) << exp; v -= 0x84; return static_cast<float>((sign ? -v : v) / 32768.0); }
float DecodeAlaw(uint8_t a) { a ^= 0x55; const int sign = a & 0x80, exp = (a >> 4) & 7, mant = a & 15; int v = exp == 0 ? (mant << 4) + 8 : ((mant << 4) + 0x108) << (exp - 1); return static_cast<float>((sign ? v : -v) / 32768.0); }

// Reads an AIFF / AIFF-C file into interleaved floats at the rate of the file. Supported: big-endian PCM of 1..4 bytes per sample (a sample
// size that is not a whole number of bytes is stored left-justified, as the standard says), AIFF-C "NONE" / "twos", the little-endian
// "sowt" that Apple writes, "in24" / "in32", 32 / 64 bit float ("fl32" / "FL32" / "fl64" / "FL64"), and the G.711 "ulaw" / "alaw".
// Other compressed variants (ima4, MACE ...) are refused.
// Works on a file cut short (what is there plays), like the WAV reader.
bool DecodeAiffFile(const std::string& path, std::vector<float>& srcOut, int& srcChannelsOut, uint32_t& sampleRateOut) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::cerr << "[aiff] cannot open " << path << "\n"; return false; }
    uint8_t hdr[12];
    if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "FORM", 4) != 0 || (std::memcmp(hdr + 8, "AIFF", 4) != 0 && std::memcmp(hdr + 8, "AIFC", 4) != 0)) {
        std::cerr << "[aiff] not a FORM/AIFF file: " << path << "\n";
        std::fclose(f);
        return false;
    }
    const bool aifc = std::memcmp(hdr + 8, "AIFC", 4) == 0;
    int channels = 0, bits = 0;
    uint32_t frames = 0;
    double rate = 0.0;
    char comp[5] = "NONE";
    std::vector<uint8_t> bytes;
    bool haveComm = false, haveSsnd = false, tooBig = false;
    const uint64_t fileSize = FileSize(f);
    uint8_t ch[8];
    while (std::fread(ch, 1, 8, f) == 8) {
        const uint32_t size = be32(ch + 4);
        if (std::memcmp(ch, "COMM", 4) == 0) {
            if (size < 18 || size > 4096) break;
            std::vector<uint8_t> c(size);
            if (std::fread(c.data(), 1, size, f) != size) break;
            channels = be16(c.data());
            frames = be32(c.data() + 2);
            bits = be16(c.data() + 6);
            rate = Extended80(c.data() + 8);
            if (aifc && size >= 22) { std::memcpy(comp, c.data() + 18, 4); comp[4] = 0; }
            haveComm = true;
        } else if (std::memcmp(ch, "SSND", 4) == 0) {
            if (size < 8) break;
            uint8_t so[8];
            if (std::fread(so, 1, 8, f) != 8) break;
            const uint32_t offset = be32(so);
            const size_t want = size - 8;
            if (offset > 0 && std::fseek(f, static_cast<long>(offset), SEEK_CUR) != 0) break;
            const size_t dataLen = static_cast<size_t>(std::min<uint64_t>(want > offset ? want - offset : 0, BytesLeft(f, fileSize)));   // LIVE-8
            if (dataLen > kMaxAudioBytes) { tooBig = true; break; }
            bytes.resize(dataLen);
            const size_t got = std::fread(bytes.data(), 1, dataLen, f);
            bytes.resize(got);
            haveSsnd = got > 0;
            if (got != dataLen || dataLen != (want > offset ? want - offset : 0)) break;
            if (offset > 0) { /* the pad after the data is skipped with the chunk padding below */ }
        } else {
            if (std::fseek(f, static_cast<long>(size), SEEK_CUR) != 0) break;
        }
        if (size % 2 == 1) std::fseek(f, 1, SEEK_CUR);   // chunks are word-aligned
        if (haveComm && haveSsnd) break;
    }
    std::fclose(f);
    if (tooBig) { std::cerr << "[aiff] audio data larger than " << (kMaxAudioBytes >> 20) << " MB, refused: " << path << "\n"; return false; }
    if (!haveComm || !haveSsnd) { std::cerr << "[aiff] missing COMM / SSND chunk: " << path << "\n"; return false; }
    if (channels < 1 || channels > 64 || bits < 1 || bits > 64 || !PlausibleRate(rate)) { std::cerr << "[aiff] bad format in " << path << "\n"; return false; }

    const std::string c(comp);
    const bool little = c == "sowt";
    const bool pcm = !aifc || c == "NONE" || c == "none" || c == "twos" || c == "sowt" || c == "in24" || c == "in32" || c == "raw ";
    const bool flt = aifc && (c == "fl32" || c == "FL32" || c == "fl64" || c == "FL64");
    const bool ulaw = aifc && (c == "ulaw" || c == "ULAW"), alaw = aifc && (c == "alaw" || c == "ALAW");
    if (ulaw || alaw) {   // one byte per sample
        const size_t frameBytes1 = static_cast<size_t>(channels);
        size_t n1 = bytes.size() / frameBytes1;
        if (frames > 0 && frames < n1) n1 = frames;
        if (n1 == 0) { std::cerr << "[aiff] no audio in " << path << "\n"; return false; }
        std::vector<float> src1(n1 * frameBytes1);
        for (size_t i = 0; i < src1.size(); ++i) src1[i] = ulaw ? DecodeUlaw(bytes[i]) : DecodeAlaw(bytes[i]);
        srcOut = std::move(src1); srcChannelsOut = channels; sampleRateOut = static_cast<uint32_t>(rate + 0.5);
        return true;
    }
    if (c == "able") { std::cerr << "[aiff] protected Ableton Live pack sample ('able'): only Live can read it: " << path << "\n"; return false; }
    if (!pcm && !flt) { std::cerr << "[aiff] unsupported compression '" << c << "' in " << path << "\n"; return false; }
    int bps = (bits + 7) / 8;   // bytes per sample
    if (flt) bps = (c == "fl64" || c == "FL64") ? 8 : 4;
    if (bps < 1 || bps > 8 || (pcm && bps > 4)) { std::cerr << "[aiff] unsupported sample size " << bits << " in " << path << "\n"; return false; }
    const size_t frameBytes = static_cast<size_t>(bps) * static_cast<size_t>(channels);
    size_t nFrames = bytes.size() / frameBytes;
    if (frames > 0 && frames < nFrames) nFrames = frames;   // the file says how many frames there are (anything after is padding)
    if (nFrames == 0) { std::cerr << "[aiff] no audio in " << path << "\n"; return false; }
    std::vector<float> src(nFrames * static_cast<size_t>(channels));
    for (size_t i = 0; i < src.size(); ++i) {
        const uint8_t* b = &bytes[i * static_cast<size_t>(bps)];
        if (flt) {
            if (bps == 4) { uint32_t u = little ? (uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24)) : be32(b); float v; std::memcpy(&v, &u, 4); src[i] = v; }
            else { uint64_t u = 0; for (int k = 0; k < 8; ++k) u = (u << 8) | b[k]; double v; std::memcpy(&v, &u, 8); src[i] = static_cast<float>(v); }
            continue;
        }
        int32_t v = 0;   // two's complement, big endian (sowt: little endian), left-justified in `bps` bytes
        if (little) { uint32_t u = 0; for (int k = bps - 1; k >= 0; --k) u = (u << 8) | b[k]; v = static_cast<int32_t>(u << (32 - 8 * bps)) >> (32 - 8 * bps); }
        else { uint32_t u = 0; for (int k = 0; k < bps; ++k) u = (u << 8) | b[k]; v = static_cast<int32_t>(u << (32 - 8 * bps)) >> (32 - 8 * bps); }
        src[i] = static_cast<float>(static_cast<double>(v) / std::ldexp(1.0, 8 * bps - 1));
    }
    srcOut = std::move(src); srcChannelsOut = channels; sampleRateOut = static_cast<uint32_t>(rate + 0.5);
    return true;
}

// The AIFF-C compression type of a file ("NONE", "sowt", "able" ...), "" for plain AIFF or when the header cannot be read. Walks the chunks to COMM.
std::string AiffCompressionType(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return std::string();
    std::string out;
    uint8_t h[12];
    if (std::fread(h, 1, 12, f) == 12 && std::memcmp(h, "FORM", 4) == 0 && std::memcmp(h + 8, "AIFC", 4) == 0) {
        uint8_t ch[8];
        for (int n = 0; n < 40 && std::fread(ch, 1, 8, f) == 8; ++n) {
            const uint32_t size = be32(ch + 4);
            if (std::memcmp(ch, "COMM", 4) == 0) {
                uint8_t c[22] = {0};
                if (size >= 22 && std::fread(c, 1, 22, f) == 22) out.assign(reinterpret_cast<const char*>(c + 18), 4);
                break;
            }
            if (std::fseek(f, static_cast<long>(size + (size & 1u)), SEEK_CUR) != 0) break;
        }
    }
    std::fclose(f);
    return out;
}

bool IsAiffHeader(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t h[12];
    const bool ok = std::fread(h, 1, 12, f) == 12 && std::memcmp(h, "FORM", 4) == 0 && (std::memcmp(h + 8, "AIFF", 4) == 0 || std::memcmp(h + 8, "AIFC", 4) == 0);
    std::fclose(f);
    return ok;
}

// Decodes any supported sample file at the rate it has. The kind is found from the header, not from the extension.
bool DecodeAnyFile(const std::string& path, std::vector<float>& src, int& channels, uint32_t& rate) {
    if (IsAiffHeader(path)) return DecodeAiffFile(path, src, channels, rate);
    return DecodeWavFile(path, src, channels, rate);
}

} // namespace

bool LoadWav(const std::string& path, SampleBuffer& out, int targetSampleRate) {
    std::vector<float> src; int srcChannels = 1; uint32_t sampleRate = 44100;
    if (!DecodeAnyFile(path, src, srcChannels, sampleRate)) return false;
    if (srcChannels > 2) {   // v44.50: surround / multichannel files: keep the front pair (the engine plays mono or stereo samples)
        const size_t frames = src.size() / static_cast<size_t>(srcChannels);
        std::vector<float> two(frames * 2);
        for (size_t i = 0; i < frames; ++i) { two[i * 2] = src[i * static_cast<size_t>(srcChannels)]; two[i * 2 + 1] = src[i * static_cast<size_t>(srcChannels) + 1]; }
        src = std::move(two);
        srcChannels = 2;
    }
    // Sample-rate conversion with a band-limited (Kaiser sinc) resampler, so 48 kHz / 96 kHz kits keep their top end.
    out.data = (static_cast<int>(sampleRate) == targetSampleRate || sampleRate == 0)
                   ? std::move(src)
                   : ResampleInterleaved(src, srcChannels, static_cast<int>(sampleRate), targetSampleRate);
    out.channels = srcChannels;
    out.sampleRate = targetSampleRate;
    return true;
}

// DATA-5: a recording / stem / imported sample is written to <path>.tmp, flushed to the disk and renamed: a crash or a full disk
// never leaves half a WAV under the real name, and a failed write is reported (before, fwrite errors were ignored).
static bool WriteBytesAtomic(const std::string& path, const std::vector<uint8_t>& bytes) {
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool wrote = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    const bool closed = std::fclose(f) == 0;
    if (!wrote || !closed) { std::remove(tmp.c_str()); return false; }
    FlushFileToDisk(tmp);
    if (std::rename(tmp.c_str(), path.c_str()) != 0) { std::remove(tmp.c_str()); return false; }
    FlushDirOf(path);
    return true;
}

bool WriteWavPCM16(const std::string& path, const std::vector<float>& interleaved,
                    int channels, int sampleRate) {
    return WriteBytesAtomic(path, BuildWavPCM16Bytes(interleaved, channels, sampleRate));
}

std::vector<uint8_t> BuildWavPCM16Bytes(const std::vector<float>& interleaved,
                                         int channels, int sampleRate) {
    uint32_t dataSize = static_cast<uint32_t>(interleaved.size() * 2);
    uint32_t byteRate = static_cast<uint32_t>(sampleRate * channels * 2);
    uint16_t blockAlign = static_cast<uint16_t>(channels * 2);

    std::vector<uint8_t> out;
    out.reserve(44 + dataSize);
    auto push8 = [&](const char* s, size_t n) { out.insert(out.end(), s, s + n); };
    auto push32 = [&](uint32_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    };
    auto push16 = [&](uint16_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    };

    push8("RIFF", 4);
    push32(36 + dataSize);
    push8("WAVE", 4);
    push8("fmt ", 4);
    push32(16);
    push16(1); // PCM
    push16(static_cast<uint16_t>(channels));
    push32(static_cast<uint32_t>(sampleRate));
    push32(byteRate);
    push16(blockAlign);
    push16(16);
    push8("data", 4);
    push32(dataSize);

    size_t dataStart = out.size();
    out.resize(dataStart + interleaved.size() * 2);
    int16_t* pcm = reinterpret_cast<int16_t*>(out.data() + dataStart);
    for (size_t i = 0; i < interleaved.size(); ++i) {
        float v = std::max(-1.0f, std::min(1.0f, interleaved[i]));
        pcm[i] = static_cast<int16_t>(v * 32767.0f);
    }
    return out;
}

bool WriteWavPCM24(const std::string& path, const std::vector<float>& interleaved, int channels, int sampleRate) {
    const uint32_t dataSize = static_cast<uint32_t>(interleaved.size() * 3);
    std::vector<uint8_t> out;
    out.reserve(44 + dataSize);
    auto push32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    auto push16 = [&](uint16_t v) { out.push_back(static_cast<uint8_t>(v)); out.push_back(static_cast<uint8_t>(v >> 8)); };
    auto tag = [&](const char* t) { out.insert(out.end(), t, t + 4); };
    tag("RIFF"); push32(36 + dataSize); tag("WAVE"); tag("fmt "); push32(16); push16(1); push16(static_cast<uint16_t>(channels));
    push32(static_cast<uint32_t>(sampleRate)); push32(static_cast<uint32_t>(sampleRate * channels * 3)); push16(static_cast<uint16_t>(channels * 3)); push16(24);
    tag("data"); push32(dataSize);
    for (float f : interleaved) {
        const int32_t v = static_cast<int32_t>(std::lround(std::max(-1.0f, std::min(1.0f, f)) * 8388607.0));
        out.push_back(static_cast<uint8_t>(v)); out.push_back(static_cast<uint8_t>(v >> 8)); out.push_back(static_cast<uint8_t>(v >> 16));
    }
    return WriteBytesAtomic(path, out);
}


bool ImportAudioAsWav(const std::string& src, const std::string& dstWav) {
    if (!IsAiffFileName(src)) {   // a WAV: the bytes are copied as they are
        std::error_code ec;
        std::filesystem::copy_file(src, dstWav, std::filesystem::copy_options::overwrite_existing, ec);
        return !ec;
    }
    std::vector<float> data; int ch = 1; uint32_t rate = 44100;
    if (!DecodeAnyFile(src, data, ch, rate) || data.empty()) return false;
    if (ch > 2) {
        const size_t frames = data.size() / static_cast<size_t>(ch);
        std::vector<float> two(frames * 2);
        for (size_t i = 0; i < frames; ++i) { two[i * 2] = data[i * static_cast<size_t>(ch)]; two[i * 2 + 1] = data[i * static_cast<size_t>(ch) + 1]; }
        data = std::move(two); ch = 2;
    }
    return WriteWavPCM24(dstWav, data, ch, static_cast<int>(rate));
}


bool IsProtectedAbletonAiff(const std::string& path) { return AiffCompressionType(path) == "able"; }

std::string UnreadableReason(const std::string& path) {
    const std::string c = AiffCompressionType(path);
    if (c == "able") return "PROTECTED ABLETON PACK SAMPLE - ONLY LIVE CAN READ IT. EXPORT IT FROM LIVE AS WAV OR AIFF";
    if (c == "ima4" || c == "MAC3" || c == "MAC6" || c == "GSM " || c == "QDM2") return "COMPRESSED AIFF (" + c + ") - CONVERT IT TO WAV OR PLAIN AIFF";
    return std::string();
}

} // namespace t8
