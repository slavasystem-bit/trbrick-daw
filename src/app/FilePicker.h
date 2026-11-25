#pragma once
// v44.50: a native "choose a file" window without a GUI toolkit: the system's own dialog is started as a child process on a worker
// thread (so the program, and the sound, keep running while it is open) and the chosen path is collected on a later frame.
//   macOS : osascript "choose file"      Linux : zenity, else kdialog      (no dialog program found -> Start() returns false)
// Only one dialog at a time. Nothing here touches the project; the caller decides what to do with the path.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

namespace t8 {

class FilePicker {
public:
    ~FilePicker() { Join(); }

    bool Busy() const { return busy_.load(); }

    // Opens the dialog for sample files (WAV and AIFF). Returns false if one is already open or the system has no dialog program.
    bool StartWav() {
        if (busy_.load()) return false;
        Join();
#if defined(__APPLE__)
        const std::string cmd = "osascript -e 'POSIX path of (choose file with prompt \"Choose a sample (WAV or AIFF)\" of type {\"wav\", \"wave\", \"aif\", \"aiff\", \"aifc\", \"com.microsoft.waveform-audio\", \"public.aiff-audio\", \"public.aifc-audio\"})' 2>/dev/null";
#elif defined(__linux__)
        std::string cmd;
        if (Has("zenity")) cmd = "zenity --file-selection --title='Choose a sample (WAV or AIFF)' --file-filter='Audio | *.wav *.WAV *.wave *.aif *.AIF *.aiff *.AIFF *.aifc' 2>/dev/null";
        else if (Has("kdialog")) cmd = "kdialog --getopenfilename . '*.wav *.WAV *.wave *.aif *.AIF *.aiff *.AIFF *.aifc|WAV and AIFF audio' 2>/dev/null";
        else return false;
#else
        return false;
#endif
        return Launch(cmd);
    }

    // v44.51: the same for a folder (the sample library).
    bool StartFolder() {
        if (busy_.load()) return false;
        Join();
#if defined(__APPLE__)
        const std::string cmd = "osascript -e 'POSIX path of (choose folder with prompt \"Choose your samples folder\")' 2>/dev/null";
#elif defined(__linux__)
        std::string cmd;
        if (Has("zenity")) cmd = "zenity --file-selection --directory --title='Choose your samples folder' 2>/dev/null";
        else if (Has("kdialog")) cmd = "kdialog --getexistingdirectory . 2>/dev/null";
        else return false;
#else
        return false;
#endif
        return Launch(cmd);
    }

    // True once, with the chosen path, after a dialog was closed with a file (a cancelled dialog gives nothing).
    bool Poll(std::string& path) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!ready_) return false;
        ready_ = false;
        if (result_.empty()) return false;
        path = result_;
        return true;
    }

private:
    bool Launch(const std::string& cmd) {
        busy_.store(true);
        worker_ = std::thread([this, cmd]() {
            std::string out;
            if (FILE* p = popen(cmd.c_str(), "r")) {
                char buf[512];
                while (std::fgets(buf, sizeof(buf), p)) out += buf;
                pclose(p);
            }
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
            { std::lock_guard<std::mutex> lk(mtx_); result_ = out; ready_ = true; }
            busy_.store(false);
        });
        return true;
    }
    static bool Has(const char* prog) {
        const std::string cmd = std::string("command -v ") + prog + " >/dev/null 2>&1";
        return std::system(cmd.c_str()) == 0;
    }
    void Join() { if (worker_.joinable()) worker_.join(); }
    std::atomic<bool> busy_{false};
    std::mutex mtx_;
    std::string result_;
    bool ready_ = false;
    std::thread worker_;
};

} // namespace t8
