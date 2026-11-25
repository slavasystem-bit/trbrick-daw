#include "ZipWriter.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>

namespace t8 {

namespace {

// Standard reflected CRC-32 (IEEE 802.3, polynomial 0xEDB88320) --
// table-based, computed once on first use. ZIP's local/central headers
// both require a correct CRC-32 of the (uncompressed, since we only ever
// write STORE entries) data.
uint32_t Crc32(const std::vector<uint8_t>& data) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();

    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t b : data) {
        crc = table[(crc ^ b) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

void Push32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}
void Push16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
void PushStr(std::vector<uint8_t>& out, const std::string& s) {
    out.insert(out.end(), s.begin(), s.end());
}

} // namespace

bool WriteStoreZip(const std::string& path, const std::vector<ZipEntry>& entries) {
    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        std::string dir = path.substr(0, slash);
        struct stat st{};
        if (::stat(dir.c_str(), &st) != 0) ::mkdir(dir.c_str(), 0755);
    }

    std::vector<uint8_t> out;
    std::vector<uint32_t> localOffsets(entries.size());
    std::vector<uint32_t> crcs(entries.size());

    // A fixed, valid MS-DOS date/time (1980-01-01 00:00:00, the MS-DOS
    // epoch) -- the actual date doesn't matter for anything here (these
    // are freshly-bounced stems, not archival files), and a real clock
    // read would need <ctime> plumbing a reader doesn't care about either.
    constexpr uint16_t kDosTime = 0x0000;
    constexpr uint16_t kDosDate = 0x0021;

    // ---- Local file header + data, one entry at a time --------------------
    for (size_t i = 0; i < entries.size(); ++i) {
        const ZipEntry& e = entries[i];
        uint32_t crc = Crc32(e.data);
        crcs[i] = crc;
        localOffsets[i] = static_cast<uint32_t>(out.size());

        Push32(out, 0x04034b50u); // local file header signature
        Push16(out, 20);          // version needed to extract
        Push16(out, 0);           // general purpose bit flag
        Push16(out, 0);           // compression method: 0 = store
        Push16(out, kDosTime);
        Push16(out, kDosDate);
        Push32(out, crc);
        Push32(out, static_cast<uint32_t>(e.data.size())); // compressed size == uncompressed (store)
        Push32(out, static_cast<uint32_t>(e.data.size()));
        Push16(out, static_cast<uint16_t>(e.name.size()));
        Push16(out, 0); // extra field length
        PushStr(out, e.name);
        out.insert(out.end(), e.data.begin(), e.data.end());
    }

    // ---- Central directory -------------------------------------------------
    uint32_t centralDirStart = static_cast<uint32_t>(out.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        const ZipEntry& e = entries[i];
        Push32(out, 0x02014b50u); // central file header signature
        Push16(out, 20);          // version made by
        Push16(out, 20);          // version needed to extract
        Push16(out, 0);           // general purpose bit flag
        Push16(out, 0);           // compression method: 0 = store
        Push16(out, kDosTime);
        Push16(out, kDosDate);
        Push32(out, crcs[i]);
        Push32(out, static_cast<uint32_t>(e.data.size()));
        Push32(out, static_cast<uint32_t>(e.data.size()));
        Push16(out, static_cast<uint16_t>(e.name.size()));
        Push16(out, 0); // extra field length
        Push16(out, 0); // file comment length
        Push16(out, 0); // disk number start
        Push16(out, 0); // internal file attributes
        Push32(out, 0); // external file attributes
        Push32(out, localOffsets[i]); // relative offset of local header
        PushStr(out, e.name);
    }
    uint32_t centralDirSize = static_cast<uint32_t>(out.size()) - centralDirStart;

    // ---- End of central directory record -----------------------------------
    Push32(out, 0x06054b50u);
    Push16(out, 0); // this disk
    Push16(out, 0); // disk with start of central directory
    Push16(out, static_cast<uint16_t>(entries.size())); // entries on this disk
    Push16(out, static_cast<uint16_t>(entries.size())); // total entries
    Push32(out, centralDirSize);
    Push32(out, centralDirStart);
    Push16(out, 0); // comment length

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t written = std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    return written == out.size();
}

} // namespace t8
