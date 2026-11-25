#pragma once
// Small helpers that make a file write survive a crash or a power cut (DATA-5): push the bytes of a file and the folder entry
// that names it all the way to the disk. Used by project saves, backups and WAV writes.
#include <string>
#include <fcntl.h>
#include <unistd.h>

namespace t8 {

// Forces the contents of `path` to the disk. On macOS plain fsync only hands the data to the drive, whose own cache can still
// lose it on a power cut; F_FULLFSYNC asks the drive to write it out (falls back to fsync where it is not supported).
inline bool FlushFileToDisk(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    bool ok = false;
#if defined(__APPLE__) && defined(F_FULLFSYNC)
    ok = ::fcntl(fd, F_FULLFSYNC) == 0;
#endif
    if (!ok) ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

// After a rename: forces the folder entry itself to the disk, so the new name is not lost with the folder's cache.
inline void FlushDirOf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    const std::string dir = slash == std::string::npos ? std::string(".") : (slash == 0 ? std::string("/") : path.substr(0, slash));
    const int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd < 0) return;
    ::fsync(fd);
    ::close(fd);
}

} // namespace t8
