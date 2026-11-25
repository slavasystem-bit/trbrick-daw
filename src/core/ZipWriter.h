#pragma once
// Minimal ZIP archive writer -- STORE method only (no compression). WAV
// PCM data doesn't compress well anyway (it's already near-random at the
// bit level), so skipping DEFLATE avoids vendoring a compression library
// just for this: a stored (uncompressed) ZIP is a completely standard,
// valid archive that Finder/Explorer/Ableton's importer/any unzip tool
// opens with no special handling. Used by MultitrackRecorder to bundle
// its 7 stem WAVs into one archive.
#include <cstdint>
#include <string>
#include <vector>

namespace t8 {

struct ZipEntry {
    std::string name;        // path/filename as it should appear inside the archive
    std::vector<uint8_t> data;
};

// Writes `entries` into a single STORE-method ZIP file at `path`, creating
// the containing directory if needed. Returns false only on a file-write
// failure.
bool WriteStoreZip(const std::string& path, const std::vector<ZipEntry>& entries);

} // namespace t8
