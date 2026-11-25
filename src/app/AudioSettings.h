#pragma once
// N1 (LIVE-5 / LIVE-6): the Settings > Audio choices - which output the sound goes to and how big the device buffer is. Kept
// free of SDL so the rules can be tested; main.cpp opens the device with them.
#include <string>
#include <vector>

namespace t8::audiosettings {

inline constexpr int kBufferChoices[3] = {128, 256, 512};
inline constexpr int kDefaultBuffer = 128;

// A saved / typed buffer size: only the three offered sizes are used, anything else is the default.
inline int ValidBuffer(int frames) {
    for (int b : kBufferChoices) if (frames == b) return b;
    return kDefaultBuffer;
}

// Which device to open: the chosen name when it is plugged in, otherwise "" (= the system default). An empty choice is the
// system default too.
inline std::string OutputToOpen(const std::string& chosen, const std::vector<std::string>& available) {
    if (chosen.empty()) return std::string();
    for (const std::string& n : available) if (n == chosen) return chosen;
    return std::string();
}

// Buffer length in milliseconds for the labels ("2.9 ms").
inline double BufferMs(int frames, int rate) { return rate > 0 ? 1000.0 * frames / rate : 0.0; }

} // namespace t8::audiosettings
