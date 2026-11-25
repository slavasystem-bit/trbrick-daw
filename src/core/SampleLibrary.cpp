#include "SampleLibrary.h"
#include "TempoName.h"
#include "WavFile.h"
#include "UserDirs.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace t8 {
namespace {

namespace fs = std::filesystem;

std::string Lower(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }

// Words of a name: letters / digits runs, lower case ("Closed_HH-02" -> closed, hh, 02).
std::vector<std::string> Tokens(const std::string& s) {
    std::vector<std::string> t; std::string cur;
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) cur += static_cast<char>(std::tolower(c));
        else if (!cur.empty()) { t.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) t.push_back(cur);
    return t;
}
bool Starts(const std::string& t, const char* p) { return t.rfind(p, 0) == 0; }
bool Ends(const std::string& t, const char* p) { const std::string s(p); return t.size() >= s.size() && t.compare(t.size() - s.size(), s.size(), s) == 0; }

// The category one word stands for ("" = none). LOOP is decided first by the caller.
const char* WordCategory(const std::string& t) {
    if (Starts(t, "loop") || t == "lp") return "LOOP";
    if (Starts(t, "kick") || t == "bd" || t == "bassdrum") return "KICK";
    if (Starts(t, "snare") || t == "sd" || t == "snr") return "SNARE";
    if (Starts(t, "clap") || t == "cp") return "CLAP";
    if (t == "hh" || t == "chh" || t == "ohh" || Starts(t, "hihat") || Ends(t, "hh") || t == "hat" || t == "hats" || Ends(t, "hat") || Ends(t, "hats") || t == "cymbals" ) return "HAT";
    if (Starts(t, "perc") || Starts(t, "tom") || Starts(t, "rim") || Starts(t, "shaker") || Starts(t, "conga") || Starts(t, "bongo") || Starts(t, "cowbell") ||
        Starts(t, "clave") || Starts(t, "ride") || Starts(t, "cymbal") || Starts(t, "tamb") || Starts(t, "snap") || Starts(t, "woodblock") || t == "bell" || t == "bells") return "PERC";
    if (Starts(t, "bass") || Starts(t, "subbass") || Starts(t, "reese") || t == "808") return "BASS";
    if (Starts(t, "synth") || Starts(t, "lead") || Starts(t, "pad") || Starts(t, "pluck") || Starts(t, "stab") || Starts(t, "chord") || Starts(t, "keys") ||
        Starts(t, "piano") || Starts(t, "arp") || Starts(t, "organ") || Starts(t, "string")) return "SYNTH";
    if (t == "fx" || t == "sfx" || Starts(t, "fill") || Starts(t, "riser") || t == "rise" || Starts(t, "sweep") || Starts(t, "impact") || Starts(t, "transition") ||
        t == "trans" || Starts(t, "uplift") || Starts(t, "downlift") || Starts(t, "crash") || Starts(t, "noise") || Starts(t, "whoosh") || Starts(t, "reverse") ||
        Starts(t, "build") || t == "drop" || Starts(t, "boom") || Starts(t, "swell") || Starts(t, "effect") || Starts(t, "texture")) return "FX";
    return nullptr;
}

int CatRank(const std::string& c) { for (int i = 0; i < kNumLibCategories; ++i) if (c == kLibCategories[i]) return i; return kNumLibCategories - 1; }

bool IsWav(const fs::path& p) { const std::string e = Lower(p.extension().string()); return e == ".wav" || e == ".wave"; }

std::string RootFile() { return userdirs::ServiceDir() + "/samples_root.txt"; }

} // namespace

std::string ClassifyLibraryPath(const std::string& rel) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : rel) { if (c == '/' || c == '\\') { if (!cur.empty()) parts.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.empty()) return "OTHER";
    std::string file = parts.back(); parts.pop_back();
    const size_t dot = file.find_last_of('.'); if (dot != std::string::npos) file = file.substr(0, dot);
    std::vector<std::vector<std::string>> words;   // nearest folder first, the file name last
    for (size_t i = parts.size(); i-- > 0;) words.push_back(Tokens(parts[i]));
    words.push_back(Tokens(file));
    if (TempoFromName(file) > 0.0) return "LOOP";   // v44.52.1: a tempo in the name
    for (const auto& w : words) for (const auto& t : w) if (const char* c = WordCategory(t)) if (std::string(c) == "LOOP") return "LOOP";
    for (const auto& w : words) for (const auto& t : w) if (const char* c = WordCategory(t)) return c;
    return "OTHER";
}

std::vector<LibSample> ScanSampleLibrary(const std::string& root, bool* truncated, size_t maxFiles) {
    std::vector<LibSample> out;
    if (truncated) *truncated = false;
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec)) return out;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        const std::string fn = p.filename().string();
        if (!fn.empty() && fn[0] == '.') { if (it->is_directory(ec)) it.disable_recursion_pending(); continue; }
        if (it->is_directory(ec)) {
            if (it.depth() == 0 && Lower(fn) == "kits") it.disable_recursion_pending();   // kits are copied by SyncLibraryKits, not listed as samples
            continue;
        }
        if (!it->is_regular_file(ec) || !IsSampleFileName(p.string())) continue;   // v44.56: .wav, .wave, .aif, .aiff, .aifc
        if (out.size() >= maxFiles) { if (truncated) *truncated = true; break; }
        LibSample s;
        s.rel = fs::relative(p, root, ec).generic_string();
        s.name = p.stem().string();
        s.folder = p.parent_path() == fs::path(root) ? std::string() : p.parent_path().filename().string();
        s.category = ClassifyLibraryPath(s.rel);
        s.bytes = static_cast<unsigned long long>(fs::file_size(p, ec));
        s.bpm = TempoFromName(s.name);
        s.locked = IsAiffFileName(p.string()) && IsProtectedAbletonAiff(p.string());
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(), [](const LibSample& a, const LibSample& b) {
        const int ra = CatRank(a.category), rb = CatRank(b.category);
        if (ra != rb) return ra < rb;
        return Lower(a.rel) < Lower(b.rel);
    });
    return out;
}

std::string SamplesRoot() {
    if (const char* e = std::getenv("TRB_SAMPLES_DIR")) if (*e) return e;
    std::ifstream in(RootFile());
    std::string line;
    if (in && std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.pop_back();
        std::error_code ec;
        if (!line.empty() && fs::is_directory(line, ec)) return line;
    }
    const std::string def = userdirs::LibraryDir() + "/Samples";
    std::error_code ec;
    fs::create_directories(def, ec);
    return def;
}

void SetSamplesRoot(const std::string& dir) {
    std::error_code ec;
    if (dir.empty()) { fs::remove(RootFile(), ec); return; }
    std::ofstream out(RootFile(), std::ios::trunc);
    out << dir << "\n";
}

int SyncLibraryKits(const std::string& root, const std::string& kitsDir) {
    std::error_code ec;
    const fs::path src = fs::path(root) / "Kits";
    if (root.empty() || !fs::is_directory(src, ec)) return 0;
    int changed = 0;
    for (const auto& d : fs::directory_iterator(src, ec)) {
        if (!d.is_directory(ec)) continue;
        const std::string name = d.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        const fs::path dst = fs::path(kitsDir) / name;
        bool any = false;
        for (const auto& f : fs::directory_iterator(d.path(), ec)) {
            if (!f.is_regular_file(ec) || !IsWav(f.path())) continue;
            const fs::path to = dst / f.path().filename();
            if (fs::exists(to, ec)) continue;
            fs::create_directories(dst, ec);
            fs::copy_file(f.path(), to, ec);
            if (!ec) any = true;
        }
        if (any) ++changed;
    }
    return changed;
}

} // namespace t8
