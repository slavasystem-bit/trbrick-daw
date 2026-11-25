#include "UserDirs.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace t8::userdirs {
namespace fs = std::filesystem;

namespace {
struct State {
    std::string dataRoot, serviceRoot;
    bool ensured = false;
    bool legacy = false;
    std::string projectName;
    std::string lastOutput;
};
State& S() { static State s; return s; }

std::string HomeDir() {
    const char* h = std::getenv("HOME");
    return (h && *h) ? std::string(h) : std::string("/tmp");
}

bool MkdirP(const std::string& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    struct stat st{};
    return ::stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && ::access(dir.c_str(), W_OK) == 0;
}

void ResolveRoots() {
    State& s = S();
    if (!s.dataRoot.empty()) return;
    const char* d = std::getenv("TRB_DATA_DIR");
    s.dataRoot = (d && *d) ? std::string(d) : HomeDir() + "/Documents/TR-Brick";
    const char* v = std::getenv("TRB_SERVICE_DIR");
    if (v && *v) s.serviceRoot = v;
    else {
#ifdef __APPLE__
        s.serviceRoot = HomeDir() + "/Library/Application Support/TR-Brick";
#else
        s.serviceRoot = HomeDir() + "/.local/share/TR-Brick";
#endif
    }
}

std::string TimeStamp(long long t) {
    std::time_t tt = t > 0 ? static_cast<std::time_t>(t) : std::time(nullptr);
    std::tm tmv{};
    localtime_r(&tt, &tmv);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H.%M", &tmv);
    return buf;
}

bool EndsWith(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}
} // namespace

void SetRootsForTest(const std::string& dataRoot, const std::string& serviceRoot) {
    State& s = S();
    s.dataRoot = dataRoot;
    s.serviceRoot = serviceRoot;
    s.ensured = false;
    s.legacy = false;
    s.projectName.clear();
    s.lastOutput.clear();
    EnsureDirs();
}

bool EnsureDirs() {
    State& s = S();
    if (s.ensured) return !s.legacy;
    ResolveRoots();
    s.ensured = true;
    s.legacy = !(MkdirP(s.dataRoot + "/Projects") && MkdirP(s.dataRoot + "/Recordings") && MkdirP(s.serviceRoot + "/Backups"));
    if (!s.legacy) {   // the library folders are a convenience: failing to create them does not switch to the old layout
        MkdirP(s.dataRoot + "/Library/Beats"); MkdirP(s.dataRoot + "/Library/Presets"); MkdirP(s.dataRoot + "/Library/Favorites");
    }
    if (s.legacy) std::fprintf(stderr, "NOTE: cannot create %s; using the folders next to the program.\n", s.dataRoot.c_str());
    return !s.legacy;
}

bool InLegacyMode() { EnsureDirs(); return S().legacy; }
std::string Root() { EnsureDirs(); return S().legacy ? std::string(".") : S().dataRoot; }
std::string ProjectsDir() { EnsureDirs(); return S().legacy ? std::string("projects") : S().dataRoot + "/Projects"; }
std::string RecordingsDir() { EnsureDirs(); return S().legacy ? std::string("recordings") : S().dataRoot + "/Recordings"; }
std::string ServiceDir() { EnsureDirs(); return S().legacy ? std::string(".") : S().serviceRoot; }
std::string LibraryDir() { EnsureDirs(); return S().legacy ? std::string(".") : S().dataRoot + "/Library"; }
std::string BeatsDir() { EnsureDirs(); return S().legacy ? std::string("patterns") : S().dataRoot + "/Library/Beats"; }
std::string SynthPresetsDir() { EnsureDirs(); return S().legacy ? std::string("presets") : S().dataRoot + "/Library/Presets"; }
std::string FavoritesDir() { EnsureDirs(); return S().legacy ? std::string("presets") : S().dataRoot + "/Library/Favorites"; }
std::string BackupsDir() { EnsureDirs(); return S().legacy ? std::string() : S().serviceRoot + "/Backups"; }

std::string UntitledPath() {
    EnsureDirs();
    const std::string dir = ServiceDir() + "/Untitled";
    MkdirP(dir);
    return dir + "/Untitled.t8proj";
}
bool IsUntitledPath(const std::string& path) { return path == ServiceDir() + "/Untitled/Untitled.t8proj"; }   // no mkdir: called every frame
std::string RecoveryPathFor(const std::string& projectPath) {
    EnsureDirs();
    const std::string dir = ServiceDir() + "/Recovery";
    MkdirP(dir);
    return dir + "/" + ProjectDisplayName(projectPath) + ".t8proj";
}

void SetProjectName(const std::string& name) { S().projectName = name; }
const std::string& ProjectName() { return S().projectName; }

std::string ProjectDisplayName(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::string ext = ".t8proj";
    if (EndsWith(base, ext)) base.resize(base.size() - ext.size());
    return base;
}

std::string SanitizeName(const std::string& raw, const char* fallback) {
    std::string out;
    for (unsigned char c : raw) {
        if (c < 0x20 || c == 0x7f) continue;
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') continue;
        out += static_cast<char>(c);
    }
    size_t a = out.find_first_not_of(" .");
    if (a == std::string::npos) return fallback;
    size_t b = out.find_last_not_of(' ');
    out = out.substr(a, b - a + 1);
    constexpr size_t kMax = 80;
    if (out.size() > kMax) {   // cut on a character boundary (never inside a UTF-8 sequence)
        size_t cut = kMax;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out.resize(cut);
        size_t e = out.find_last_not_of(' ');
        out = e == std::string::npos ? std::string(fallback) : out.substr(0, e + 1);
    }
    return out;
}

std::string StampedPath(const std::string& dir, const std::string& project, const std::string& suffix, const std::string& ext,
                        long long unixTime) {
    MkdirP(dir);
    const std::string name = SanitizeName(project.empty() ? std::string("TR-Brick") : project, "TR-Brick");
    const std::string stem = dir + "/" + name + " " + TimeStamp(unixTime) + suffix;
    std::string path = stem + "." + ext;
    struct stat st{};
    for (int n = 2; ::stat(path.c_str(), &st) == 0 && n < 1000; ++n) path = stem + " " + std::to_string(n) + "." + ext;
    return path;
}

std::string RecordingPath(const std::string& suffix, const std::string& ext) {
    return StampedPath(RecordingsDir(), S().projectName, suffix, ext);
}

std::string RecordingFolder(const std::string& suffix) {
    const std::string dir = RecordingsDir();
    MkdirP(dir);
    const std::string name = SanitizeName(S().projectName.empty() ? std::string("TR-Brick") : S().projectName, "TR-Brick");
    const std::string stem = dir + "/" + name + " " + TimeStamp(0) + suffix;
    std::string path = stem;
    for (int n = 2; n < 1000; ++n) {
        if (::mkdir(path.c_str(), 0755) == 0) return path;
        if (errno != EEXIST) return std::string();
        path = stem + " " + std::to_string(n);
    }
    return std::string();
}

std::string DisplayPath(const std::string& path) {
    const std::string root = Root() + "/";
    if (!S().legacy && path.compare(0, root.size(), root) == 0) return path.substr(root.size());
    return path;
}

std::string Tilde(const std::string& path) {
    const std::string home = HomeDir();
    if (home.size() > 1 && path.compare(0, home.size(), home) == 0 && (path.size() == home.size() || path[home.size()] == '/'))
        return "~" + path.substr(home.size());
    return path;
}

void SetLastOutput(const std::string& path) { S().lastOutput = path; }
const std::string& LastOutput() { return S().lastOutput; }

bool Reveal(const std::string& path) {
    if (path.empty()) return false;
    struct stat st{};
    const bool isDir = ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    std::vector<std::string> argv;
#ifdef __APPLE__
    argv = isDir ? std::vector<std::string>{"open", path} : std::vector<std::string>{"open", "-R", path};
#else
    argv = {"xdg-open", isDir ? path : fs::path(path).parent_path().string()};
#endif
    pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {                // child: detach from the terminal, then start the helper in a grandchild (no zombies)
        pid_t g = ::fork();
        if (g != 0) ::_exit(0);
        int nul = ::open("/dev/null", O_RDWR);
        if (nul >= 0) { ::dup2(nul, 0); ::dup2(nul, 1); ::dup2(nul, 2); }
        std::vector<char*> cargv;
        for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        ::execvp(cargv[0], cargv.data());
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return true;
}

namespace {
// "take_1793025120.wav" -> a readable name; anything else keeps its own name.
std::string ReadableLegacyName(const std::string& file, long long* tOut) {
    struct Rule { const char* prefix; const char* suffix; const char* ext; };
    static const Rule rules[] = {{"take_", "", ".wav"}, {"stems_", " stems", ".zip"}, {"notes_", " notes", ".txt"},
                                 {"capture_", " capture", ".txt"}, {"capture_", " capture", ".mid"}};
    for (const Rule& r : rules) {
        const std::string pre = r.prefix, ext = r.ext;
        if (file.rfind(pre, 0) != 0 || !EndsWith(file, ext)) continue;
        const std::string digits = file.substr(pre.size(), file.size() - pre.size() - ext.size());
        if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) continue;
        *tOut = std::atoll(digits.c_str());
        return std::string(r.suffix) + ext;   // suffix + extension; the caller adds "<Name> <stamp>"
    }
    return std::string();
}

int CopyAll(const fs::path& from, const fs::path& to, const std::string& ext, bool rename) {
    std::error_code ec;
    if (!fs::is_directory(from, ec)) return 0;
    int n = 0;
    for (const auto& e : fs::directory_iterator(from, ec)) {
        if (!e.is_regular_file(ec)) continue;
        const std::string file = e.path().filename().string();
        if (!ext.empty() && !EndsWith(file, ext)) continue;
        fs::path dest = to / file;
        if (rename) {
            long long t = 0;
            const std::string tail = ReadableLegacyName(file, &t);   // " stems.zip", ".wav" ...
            if (!tail.empty() && t > 0) {
                const size_t dot = tail.find_last_of('.');
                dest = StampedPath(to.string(), "Recording", tail.substr(0, dot), tail.substr(dot + 1), t);
            }
        }
        if (fs::exists(dest, ec)) continue;
        fs::copy_file(e.path(), dest, fs::copy_options::skip_existing, ec);
        if (!ec) ++n;
    }
    return n;
}
} // namespace

MigrationResult MigrateLegacy(const std::string& legacyDir) {
    MigrationResult r;
    if (InLegacyMode()) return r;
    std::error_code ec;
    const fs::path legacy(legacyDir);
    const fs::path marker = fs::path(S().dataRoot) / ".migrated";
    if (fs::exists(marker, ec)) return r;
    // The program's own folder as the data root would copy a folder into itself.
    if (fs::exists(legacy, ec) && fs::equivalent(legacy, S().dataRoot, ec)) return r;

    r.ran = true;
    const fs::path projects = fs::path(S().dataRoot) / "Projects";
    const fs::path legacyProject = legacy / "project.t8proj";
    if (fs::is_regular_file(legacyProject, ec) && !fs::exists(projects / "project.t8proj", ec)) {
        fs::copy_file(legacyProject, projects / "project.t8proj", fs::copy_options::skip_existing, ec);
        if (!ec) ++r.projects;
    }
    r.projects += CopyAll(legacy / "projects", projects, ".t8proj", false);
    r.recordings += CopyAll(legacy / "recordings", fs::path(S().dataRoot) / "Recordings", "", true);
    r.recordings += CopyAll(legacy / "exports", fs::path(S().dataRoot) / "Recordings", "", true);
    r.backups += CopyAll(legacy / "backups", fs::path(S().serviceRoot) / "Backups", ".t8proj", false);
    r.backups += CopyAll(legacy / "projects" / "backups", fs::path(S().serviceRoot) / "Backups", ".t8proj", false);

    if (FILE* f = std::fopen(marker.string().c_str(), "w")) {
        std::fprintf(f, "Copied from %s: %d projects, %d recordings/exports, %d backups. Nothing was deleted there.\n",
                     legacyDir.c_str(), r.projects, r.recordings, r.backups);
        std::fclose(f);
    }
    return r;
}

LibraryMigration MigrateLibrary(const std::string& legacyDir) {
    LibraryMigration r;
    if (InLegacyMode()) return r;
    std::error_code ec;
    const fs::path marker = fs::path(LibraryDir()) / ".migrated";
    if (fs::exists(marker, ec)) return r;
    const fs::path legacy(legacyDir);
    if (fs::exists(legacy, ec) && fs::equivalent(legacy, fs::path(LibraryDir()).parent_path(), ec)) return r;
    r.ran = true;
    r.beats = CopyAll(legacy / "patterns", BeatsDir(), ".t8pat", false);
    r.presets = CopyAll(legacy / "presets", SynthPresetsDir(), ".t8syn", false);
    r.favorites = CopyAll(legacy / "presets", FavoritesDir(), ".txt", false);
    if (FILE* f = std::fopen(marker.string().c_str(), "w")) {
        std::fprintf(f, "Copied from %s: %d beats, %d synth presets, %d favourite lists. Nothing was deleted there.\n",
                     legacyDir.c_str(), r.beats, r.presets, r.favorites);
        std::fclose(f);
    }
    return r;
}

} // namespace t8::userdirs
