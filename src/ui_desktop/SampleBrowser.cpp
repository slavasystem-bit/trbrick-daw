#include "SampleBrowser.h"
#include "BrowserCore.h"
#include "InfoLine.h"
#include "Interaction.h"
#include "LiveWidgets.h"
#include "TrackStyle.h"
#include "Visual.h"
#include "core/SampleLibrary.h"
#include "core/UserDirs.h"
#include "core/WavFile.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sys/stat.h>

namespace t8::desktop {

namespace {
std::string Lower(std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
std::string Upper(std::string s) { for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); return s; }
std::string DirOf(const std::string& rel) { const size_t s = rel.find_last_of('/'); return s == std::string::npos ? std::string() : rel.substr(0, s); }
std::string BaseOf(const std::string& p) { const size_t s = p.find_last_of('/'); return s == std::string::npos ? p : p.substr(s + 1); }
std::string FavFile() { return userdirs::FavoritesDir() + "/favorites_samples.txt"; }
std::string RecentFile() { return userdirs::FavoritesDir() + "/recent_samples.txt"; }
constexpr size_t kRecentMax = 20;
const char* SourceName(SampleBrowser::Source s, AppState::LibTarget t) {
    switch (s) {
        case SampleBrowser::Source::Project: return t == AppState::LibTarget::Drum ? "THIS KIT" : "THIS PROJECT";
        case SampleBrowser::Source::Library: return "MY LIBRARY";
        case SampleBrowser::Source::Kits: return "KITS";
        case SampleBrowser::Source::BuiltIn: return "BUILT-IN";
        case SampleBrowser::Source::Favorites: return "FAVORITES";
        case SampleBrowser::Source::Recent: return "RECENT";
    }
    return "";
}
// the library's own key ("lib:<rel>") is the plain rel, so the favourites saved by the old MY SAMPLES browser keep working
std::string LibFavKey(const std::string& rel) { return rel; }
}

std::string SampleBrowser::Who(AppState& app) const {
    std::string who;
    if (target_ == AppState::LibTarget::Drum) who = TrackDisplayName(app.project.patterns[static_cast<size_t>(scene_)].tracks[static_cast<size_t>(drumTrack_)]);
    else if (target_ == AppState::LibTarget::Sampler)
        who = sampleTarget_ < 0 ? "SAMPLER" : sampleTarget_ >= kNumSynthTracks ? "CHORD"
            : TrackDisplayName(app.project.patterns[static_cast<size_t>(scene_)].tracks[static_cast<size_t>(std::clamp(kNumDrumTracks + sampleTarget_, 0, kNumTracks - 1))]);
    else who = "TRANSITION BEFORE BRICK " + std::to_string(std::max(0, brick_) + 1);
    return Upper(who);
}

void SampleBrowser::LoadLists() {
    favs_.clear(); recent_.clear();
    std::string line;
    { std::ifstream f(FavFile()); while (std::getline(f, line)) { while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back(); if (!line.empty()) favs_.insert(line); } }
    { std::ifstream f(RecentFile()); while (std::getline(f, line) && recent_.size() < kRecentMax) { while (!line.empty() && line.back() == '\r') line.pop_back(); if (!line.empty()) recent_.push_back(line); } }
}
void SampleBrowser::SaveFavorites() const {
    mkdir(userdirs::FavoritesDir().c_str(), 0755);
    std::ofstream f(FavFile(), std::ios::trunc);
    for (const auto& k : favs_) f << k << "\n";
}
void SampleBrowser::SaveRecent() const {
    mkdir(userdirs::FavoritesDir().c_str(), 0755);
    std::ofstream f(RecentFile(), std::ios::trunc);
    for (const auto& k : recent_) f << k << "\n";
}
void SampleBrowser::PushRecent(const std::string& favKey) {
    recent_.erase(std::remove(recent_.begin(), recent_.end(), favKey), recent_.end());
    recent_.insert(recent_.begin(), favKey);
    if (recent_.size() > kRecentMax) recent_.resize(kRecentMax);
    SaveRecent();
}

void SampleBrowser::Open(AppState& app, AppState::LibTarget target, int brick) {
    target_ = target; brick_ = brick;
    sampleTarget_ = app.libSampleTarget;
    drumTrack_ = std::clamp(app.seqCursorTrack, 0, kNumDrumTracks - 1);
    scene_ = std::clamp(app.project.currentPattern, 0, kNumSlotsTotal - 1);
    LoadLists();
    auto it = lastSrc_.find(static_cast<int>(target));
    src_ = it != lastSrc_.end() ? it->second : Source::Project;
    if (src_ == Source::Kits && target != AppState::LibTarget::Drum) src_ = Source::Project;
    if (src_ == Source::BuiltIn && target != AppState::LibTarget::Transition) src_ = Source::Project;
    if (src_ == Source::Project && target == AppState::LibTarget::Sampler && app.SampleNames().empty()) src_ = Source::Library;
    type_ = -1;
    if (target == AppState::LibTarget::Drum) {
        static const int kSlotCat[6] = {0, 1, 2, 3, 3, 4};   // KICK SNARE CLAP HAT HAT PERC
        const int slot = app.project.patterns[static_cast<size_t>(scene_)].tracks[static_cast<size_t>(drumTrack_)].slotIndex;
        if (slot >= 0 && slot < 6) type_ = kSlotCat[slot];
    } else if (target == AppState::LibTarget::Transition) type_ = 7;   // FX
    search_[0] = '\0'; sel_ = -1; scrollToSel_ = true; focusSearch_ = false; dirty_ = true;
    wavePtr_ = nullptr; kitAudition_.clear(); kitWanted_.clear();
    kitItems_.clear(); kitScanDone_ = false;
    if (target == AppState::LibTarget::Drum) ScanKits(app);
    folder_.clear();
    folderCountsOf_ = static_cast<size_t>(-1);   // the library may have been rescanned
    wantOpen_ = true;
}

// KITS: which files of this drum every kit folder has (kick.wav, kick_2.wav ...). Listed on the loader thread; the result is
// used only by the browser opening that asked for it (token).
void SampleBrowser::ScanKits(AppState& app) {
    const unsigned tok = ++kitScanToken_;
    const std::vector<std::string> kits = app.KitFolders();
    const int slot = app.project.patterns[static_cast<size_t>(scene_)].tracks[static_cast<size_t>(drumTrack_)].slotIndex;
    const std::string stem = DrumSlotFileStem(static_cast<DrumSlot>(std::clamp(slot, 0, kNumDrumTracks - 1)));
    const std::string type = kLibCategories[std::min(slot < 3 ? slot : slot < 5 ? 3 : 4, kNumLibCategories - 1)];
    auto out = std::make_shared<std::vector<Item>>();
    app.loader.Post([out, kits, stem, type] {
        auto exists = [](const std::string& p) { struct stat st{}; return ::stat(p.c_str(), &st) == 0; };
        for (const std::string& k : kits) {
            const std::string kitName = k.rfind("kits/", 0) == 0 ? k.substr(5) : BaseOf(k);
            for (int v = 1; v <= kMaxKitVariants; ++v) {
                const std::string file = v == 1 ? stem + ".wav" : stem + "_" + std::to_string(v) + ".wav";
                const std::string path = k + "/" + file;
                if (!exists(path)) continue;
                Item it; it.src = Source::Kits; it.key = path; it.name = file; it.folder = kitName; it.type = type; it.favKey = "kit:" + path;
                out->push_back(std::move(it));
            }
        }
    }, [this, out, tok] {
        if (tok != kitScanToken_) return;   // the browser was opened again for another drum meanwhile
        kitItems_ = std::move(*out); kitScanDone_ = true; dirty_ = true;
    });
}

void SampleBrowser::Rebuild(AppState& app) {
    bool collecting = false;   // FAVORITES / RECENT look through every copy, not only IN USE
    auto project = [&](std::vector<Item>& out) {
        if (target_ == AppState::LibTarget::Sampler) {
            const std::map<std::string, std::string> usage = SampleUsage(app);   // C4b (g): IN USE = plays somewhere; ALL COPIED = every copy
            inUseCount_ = copiedCount_ = 0;
            for (const std::string& nm : app.SampleNames()) {
                const auto u = usage.find(nm);
                ++copiedCount_; if (u != usage.end()) ++inUseCount_;
                if (inUseOnly_ && u == usage.end() && !collecting) continue;
                Item it; it.src = Source::Project; it.key = nm; it.name = nm; it.folder = u != usage.end() ? u->second : "not used"; it.favKey = "prj:" + nm; out.push_back(std::move(it));
            }
        } else if (target_ == AppState::LibTarget::Drum) {
            const std::string kit = app.TrackKitFolder(scene_, drumTrack_);
            const DrumKit::BankPtr b = app.KitBank(kit);
            if (!b) waitKit_ = app.KitPending(kit);   // still being read: rebuilt every frame until it is there
            if (b) {
                const auto& names = b->names[static_cast<size_t>(drumTrack_)];
                for (size_t v = 0; v < names.size(); ++v) {
                    Item it; it.src = Source::Project; it.key = kit + "/" + names[v]; it.name = names[v]; it.variant = static_cast<int>(v);
                    it.folder = kit.rfind("kits/", 0) == 0 ? kit.substr(5) : BaseOf(kit); it.favKey = "kit:" + it.key;
                    out.push_back(std::move(it));
                }
            }
        } else {
            const RiserPlayer& rp = app.engine.Risers();
            for (int r = RiserPlayer::kFirstUser; r < rp.Count(); ++r) if (!rp.NameAt(r).empty()) { Item it; it.src = Source::Project; it.riser = r; it.key = std::to_string(r); it.name = rp.NameAt(r); it.folder = "samples/transitions"; it.type = "FX"; it.favKey = "riser:" + rp.NameAt(r); out.push_back(std::move(it)); }
        }
    };
    auto library = [&](std::vector<Item>& out) {
        const std::vector<LibSample>* lib = app.LibraryListIfReady();   // listed on the loader thread, never here
        if (!lib) { waitLib_ = true; return; }
        for (const LibSample& ls : *lib) {
            Item it; it.src = Source::Library; it.key = ls.rel; it.name = ls.locked ? ls.name + "  [LOCKED]" : ls.name; it.folder = DirOf(ls.rel); it.type = ls.category; it.bpm = ls.bpm; it.favKey = LibFavKey(ls.rel);
            out.push_back(std::move(it));
        }
    };
    auto builtIn = [&](std::vector<Item>& out) {
        const RiserPlayer& rp = app.engine.Risers();
        for (int r = 0; r < std::min(rp.Count(), static_cast<int>(RiserPlayer::kFirstUser)); ++r) if (!rp.NameAt(r).empty()) { Item it; it.src = Source::BuiltIn; it.riser = r; it.key = std::to_string(r); it.name = rp.NameAt(r); it.folder = "built-in"; it.type = "FX"; it.favKey = "riser:" + rp.NameAt(r); out.push_back(std::move(it)); }
    };
    auto everything = [&](std::vector<Item>& out) {
        collecting = true;
        project(out); library(out);
        if (target_ == AppState::LibTarget::Drum) out.insert(out.end(), kitItems_.begin(), kitItems_.end());
        if (target_ == AppState::LibTarget::Transition) builtIn(out);
    };
    items_.clear();
    waitKit_ = waitLib_ = false;
    switch (src_) {
        case Source::Project: project(items_); break;
        case Source::Library: library(items_); break;
        case Source::Kits: items_ = kitItems_; break;
        case Source::BuiltIn: builtIn(items_); break;
        case Source::Favorites: { std::vector<Item> all; everything(all); for (auto& it : all) if (favs_.count(it.favKey)) items_.push_back(std::move(it)); break; }
        case Source::Recent: {
            std::vector<Item> all; everything(all);
            for (const std::string& k : recent_) for (auto& it : all) if (it.favKey == k) { items_.push_back(it); break; }
            break;
        }
    }
    vis_.clear();
    for (size_t i = 0; i < items_.size(); ++i) if (Visible(items_[i])) vis_.push_back(static_cast<int>(i));
    if (src_ != Source::Recent) {   // RECENT keeps its order (newest first)
        std::vector<Item> shown; shown.reserve(vis_.size());
        for (int i : vis_) shown.push_back(items_[static_cast<size_t>(i)]);
        SortItems(shown, sortCol_, sortDesc_);
        items_ = std::move(shown);
        vis_.resize(items_.size());
        for (size_t i = 0; i < vis_.size(); ++i) vis_[i] = static_cast<int>(i);
    }
    const int n = static_cast<int>(vis_.size());
    sel_ = n == 0 ? -1 : std::clamp(sel_, 0, n - 1);
    dirty_ = false;
}

bool SampleBrowser::Visible(const Item& it) const {
    if (src_ == Source::Library && !folder_.empty() && it.key.compare(0, folder_.size() + 1, folder_ + "/") != 0) return false;
    if (type_ >= 0 && (src_ == Source::Library || src_ == Source::Favorites || src_ == Source::Recent) && it.type != kLibCategories[type_]) return false;
    if (search_[0]) {
        const std::string hay = Lower(it.name + " " + it.folder + " " + it.type);
        std::string q = Lower(search_), w;
        for (size_t p = 0; p <= q.size(); ++p) {   // every word of the search must be there
            if (p == q.size() || q[p] == ' ') { if (!w.empty() && hay.find(w) == std::string::npos) return false; w.clear(); }
            else w += q[p];
        }
    }
    return true;
}

void SampleBrowser::Hear(AppState& app, const Item& it) {
    auto play = [&](const SamplePtr& s, double speed = 1.0) {
        if (!s) return;
        keep_.push_back(s);
        while (keep_.size() > 16) keep_.erase(keep_.begin());
        app.engine.AuditionSample(s, AppState::kAuditionGain, speed);
    };
    auto fromBuffer = [](const std::shared_ptr<const SampleBuffer>& b, const std::string& name) -> SamplePtr {
        if (!b || b->frames() < 2) return nullptr;
        auto sd = std::make_shared<SampleData>();
        sd->data = b->data; sd->channels = std::clamp(b->channels, 1, 2); sd->sampleRate = b->sampleRate; sd->name = name;
        return sd;
    };
    app.CancelLibraryAudition(); kitWanted_.clear();   // a file still being read for an earlier HEAR must not play over this one
    if (it.riser >= 0) {   // transitions play through the riser player (we hold the scene lock, like the sequencer)
        const int b = std::clamp(brick_, 0, std::max(0, app.project.songChainLength - 1));
        app.engine.Risers().TriggerAligned(it.riser, 0.0, 0.0, 1.0, LevelDbToGain(app.project.songChain[static_cast<size_t>(b)].levels.transitionDb));
        return;
    }
    switch (it.src) {
        case Source::Library: app.AuditionLibrary(it.key); return;
        case Source::Project:
            if (target_ == AppState::LibTarget::Sampler) {
                const SamplePtr s = app.SampleFor(it.key);
                if (s) play(s); else app.SetStatus("READING " + it.name + " ...");
            } else if (target_ == AppState::LibTarget::Drum) {
                const DrumKit::BankPtr b = app.KitBank(app.TrackKitFolder(scene_, drumTrack_));
                const auto& v = b ? b->variants[static_cast<size_t>(drumTrack_)] : std::vector<std::shared_ptr<const SampleBuffer>>{};
                if (it.variant >= 0 && it.variant < static_cast<int>(v.size())) play(fromBuffer(v[static_cast<size_t>(it.variant)], it.name));
            }
            return;
        default: break;
    }
    // a kit file (or a favourite / recent one): read on the loader thread, played when it arrives if it is still the wanted one
    const std::string path = it.src == Source::Library ? app.LibraryPath(it.key) : it.key;
    auto c = kitAudition_.find(path);
    if (c != kitAudition_.end()) { kitWanted_.clear(); play(c->second); return; }
    kitWanted_ = path;
    auto out = std::make_shared<SamplePtr>();
    const int rate = app.engine.SampleRate();
    AppState* ap = &app;
    app.loader.Post([out, path, rate] {
        SampleBuffer buf;
        if (LoadWav(path, buf, rate) && buf.frames() >= 2) {
            auto sd = std::make_shared<SampleData>();
            sd->data = std::move(buf.data); sd->channels = std::clamp(buf.channels, 1, 2); sd->sampleRate = rate; sd->name = BaseOf(path);
            *out = sd;
        }
    }, [this, out, path, ap] {
        if (kitAudition_.size() > 40) kitAudition_.clear();
        kitAudition_[path] = *out;
        if (path != kitWanted_ || !open_) return;   // another file was chosen meanwhile, or the browser closed
        kitWanted_.clear();
        if (!*out) { ap->SetStatus("CANNOT READ " + BaseOf(path)); return; }
        keep_.push_back(*out);
        while (keep_.size() > 16) keep_.erase(keep_.begin());
        ap->engine.AuditionSample(*out, AppState::kAuditionGain);
    });
}

void SampleBrowser::Take(AppState& app, const Item& it) {
    app.CancelLibraryAudition(); kitWanted_.clear();
    app.engine.AuditionStop();   // what was being heard stops when a file is chosen
    PushRecent(it.favKey);
    if (target_ == AppState::LibTarget::Transition) {
        if (it.riser >= 0) {
            if (brick_ < 1 || brick_ >= app.project.songChainLength) { app.SetStatus("A TRANSITION GOES BEFORE BRICK 2 OR LATER"); return; }
            app.PushSoundUndo();
            app.project.songChain[static_cast<size_t>(brick_)].riser = static_cast<int8_t>(it.riser);
            app.sequencer.RefreshArrangement();
            app.SetStatus("TRANSITION BEFORE BRICK " + std::to_string(brick_ + 1) + ": " + it.name);
        } else if (it.src == Source::Library) app.AssignLibraryTransition(brick_, it.key);
        return;
    }
    if (target_ == AppState::LibTarget::Drum) {
        if (it.src == Source::Project && it.variant >= 0) {   // a variant of this drum in its kit: every step follows it (one Undo step)
            Track& t = app.project.patterns[static_cast<size_t>(scene_)].tracks[static_cast<size_t>(drumTrack_)];
            app.PushUndo();
            for (Step& st : t.steps) st.variant = kAutoVariant;
            t.defaultVariant = static_cast<uint8_t>(it.variant);
            app.SetStatus(TrackDisplayName(t) + " SAMPLE: " + it.name);
            return;
        }
        app.AssignSampleToSlot(it.src == Source::Library ? app.LibraryPath(it.key) : it.key);
        return;
    }
    // the sampler, or a synth / the chord on ENGINE > SAMPLE
    if (it.src == Source::Project) { app.SetSamplerFile(it.key, sampleTarget_); return; }
    const std::string path = it.src == Source::Library ? app.LibraryPath(it.key) : it.key;
    LibSample ls; bool isLoop = false;
    if (it.src == Source::Library) if (const auto* lib = app.LibraryListIfReady()) for (const LibSample& l : *lib) if (l.rel == it.key) { ls = l; isLoop = l.category == "LOOP"; break; }
    const int tgt = sampleTarget_;
    AppState* ap = &app;
    app.AssignSampleToSampler(path, tgt, isLoop ? std::function<void(int)>([ap, ls, tgt](int scene) { ap->SetSamplerLoopFromLibraryInScene(ls, tgt, scene); }) : std::function<void(int)>());
}

void SampleBrowser::DrawWave(AppState& app, const Item& it, float w, float h) {
    const float* data = nullptr; int frames = 0, ch = 1;
    SamplePtr s; std::shared_ptr<const SampleBuffer> b;
    if (it.src == Source::Project && target_ == AppState::LibTarget::Sampler) s = app.SampleFor(it.key);
    else if (it.src == Source::Project && target_ == AppState::LibTarget::Drum) {
        const DrumKit::BankPtr bank = app.KitBank(app.TrackKitFolder(scene_, drumTrack_));
        if (bank && it.variant >= 0 && it.variant < static_cast<int>(bank->variants[static_cast<size_t>(drumTrack_)].size())) b = bank->variants[static_cast<size_t>(drumTrack_)][static_cast<size_t>(it.variant)];
    } else if (it.src == Source::Library) { auto f = app.libAudition_.find(app.LibraryPath(it.key)); if (f != app.libAudition_.end()) s = f->second; }
    else if (it.riser < 0) { auto f = kitAudition_.find(it.key); if (f != kitAudition_.end()) s = f->second; }
    if (s) { data = s->data.data(); frames = s->frames(); ch = s->channels; }
    else if (b) { data = b->data.data(); frames = b->frames(); ch = b->channels; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + w, p0.y + h);
    ImGui::Dummy(ImVec2(w, h));
    dl->AddRectFilled(p0, p1, vis::kCell, 5.0f);
    dl->AddRect(p0, p1, vis::kLine, 5.0f);
    if (!data || frames < 2) {
        const char* msg = it.riser >= 0 ? "HEAR plays it" : "HEAR shows the waveform";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f), vis::kTextDim, msg);
        return;
    }
    const int cols = std::max(8, static_cast<int>(w) - 8);
    if (wavePtr_ != data || static_cast<int>(peaks_.size()) != cols * 2) {   // once per sample, not per frame
        wavePtr_ = data;
        peaks_.assign(static_cast<size_t>(cols) * 2, 0.0f);
        const int c2 = std::max(1, ch);
        for (int c = 0; c < cols; ++c) {
            const int f0 = static_cast<int>(static_cast<int64_t>(frames) * c / cols), f1 = std::max(f0 + 1, static_cast<int>(static_cast<int64_t>(frames) * (c + 1) / cols));
            float lo = 0.0f, hi = 0.0f;
            for (int f = f0; f < f1 && f < frames; ++f) { const float v = data[static_cast<size_t>(f) * static_cast<size_t>(c2)]; lo = std::min(lo, v); hi = std::max(hi, v); }
            peaks_[static_cast<size_t>(c) * 2] = lo; peaks_[static_cast<size_t>(c) * 2 + 1] = hi;
        }
    }
    const float mid = (p0.y + p1.y) * 0.5f, amp = h * 0.5f - 5.0f;
    for (int c = 0; c < cols; ++c) {
        const float x = p0.x + 4.0f + static_cast<float>(c);
        dl->AddLine(ImVec2(x, mid - peaks_[static_cast<size_t>(c) * 2 + 1] * amp), ImVec2(x, mid - peaks_[static_cast<size_t>(c) * 2] * amp - 1.0f), vis::kViolet);
    }
}

void SampleBrowser::Draw(AppState& app) {
    if (wantOpen_) { ImGui::OpenPopup("##sampleBrowser"); wantOpen_ = false; open_ = true; }
    if (!open_) return;
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 sz(std::min(1120.0f, io.DisplaySize.x - 60.0f), std::min(640.0f, io.DisplaySize.y - 60.0f));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
    bool keepOpen = true;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
    if (!ImGui::BeginPopupModal("##sampleBrowser", &keepOpen, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar)) {
        ImGui::PopStyleVar();
        open_ = false;
        return;
    }
    ImGui::PopStyleVar();
    // a source that needs data still on its way (a kit bank, the kit scan) is rebuilt every frame until it is there
    if (waitKit_ || waitLib_) dirty_ = true;   // the kit or the library is still being read
    if (dirty_) Rebuild(app);
    const int n = static_cast<int>(vis_.size());
    const std::string who = Who(app);

    // ---- top bar: title, search, LIST / GRID, close ----
    ImGui::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f), "SAMPLE FOR: %s", who.c_str());
    if (target_ != AppState::LibTarget::Transition) { ImGui::SameLine(0, 6); ImGui::TextDisabled("(scene %d)", scene_ + 1); }
    ImGui::SameLine(0, 18);
    if (focusSearch_) { ImGui::SetKeyboardFocusHere(); focusSearch_ = false; }
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::InputTextWithHint("##sbSearch", "search name, folder, type ...", search_, sizeof(search_))) { dirty_ = true; sel_ = 0; scrollToSel_ = true; }
    const bool searchActive = ImGui::IsItemActive();
    if ((io.KeySuper || io.KeyCtrl) && ImGui::IsKeyPressed(ImGuiKey_F, false)) focusSearch_ = true;
    bool wantHear = browser::HearKey(searchActive, search_);
    if (browser::CaptureTyping(search_, sizeof(search_), searchActive)) { dirty_ = true; sel_ = 0; scrollToSel_ = true; }
    ImGui::SameLine(ImGui::GetWindowWidth() - 14.0f - 22.0f - 8.0f - 2 * 52.0f - 4.0f);
    if (ToggleButton("LIST##sbList", list_, ImVec2(52, 0), ToggleTab())) { list_ = true; scrollToSel_ = true; }
    if (ImGui::IsItemHovered()) Hint("One file per row: name, folder, type, tempo. A click on a column title sorts.");
    ImGui::SameLine(0, 4);
    if (ToggleButton("GRID##sbGrid", !list_, ImVec2(52, 0), ToggleTab())) { list_ = false; scrollToSel_ = true; }
    if (ImGui::IsItemHovered()) Hint("Cards, like the kit browser.");
    ImGui::SameLine(0, 8);
    if (ImGui::Button("X##sbClose", ImVec2(22, 0))) keepOpen = false;
    if (ImGui::IsItemHovered()) Hint("Close without a change (Esc)");

    const bool copies = src_ == Source::Project && target_ == AppState::LibTarget::Sampler;
    if (copies) {   // ---- THIS PROJECT (sampler / synth): IN USE / ALL COPIED (mockup section 2a) ----
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("SHOW");
        ImGui::SameLine(0, 8);
        char lab[48];
        std::snprintf(lab, sizeof lab, "IN USE  %d##sbInUse", inUseCount_);
        if (ToggleButton(lab, inUseOnly_, ImVec2(0, 0), ToggleTab())) { inUseOnly_ = true; dirty_ = true; sel_ = 0; scrollToSel_ = true; }
        if (ImGui::IsItemHovered()) Hint("Only the samples that play somewhere in this project, and where.");
        ImGui::SameLine(0, 4);
        std::snprintf(lab, sizeof lab, "ALL COPIED  %d##sbAllCopied", copiedCount_);
        if (ToggleButton(lab, !inUseOnly_, ImVec2(0, 0), ToggleTab())) { inUseOnly_ = false; dirty_ = true; sel_ = 0; scrollToSel_ = true; }
        if (ImGui::IsItemHovered()) Hint("Every file ever copied into samples/ beside the project, also the ones nothing plays any more.");
    }
    // ---- TYPE chips (library and the collections; the drums and transitions open on their own type) ----
    const bool typed = src_ == Source::Library || src_ == Source::Favorites || src_ == Source::Recent;
    if (!copies) {
    ImGui::BeginDisabled(!typed);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("TYPE");
    ImGui::SameLine(0, 8);
    if (ToggleButton("ALL##sbtAll", type_ < 0, ImVec2(0, 0), ToggleTab())) { type_ = -1; dirty_ = true; sel_ = 0; }
    for (int c = 0; c < kNumLibCategories; ++c) {
        ImGui::SameLine(0, 4);
        char lab[32]; std::snprintf(lab, sizeof lab, "%s##sbt%d", kLibCategories[c], c);
        if (ToggleButton(lab, type_ == c, ImVec2(0, 0), ToggleTab())) { type_ = type_ == c ? -1 : c; dirty_ = true; sel_ = 0; }
    }
    ImGui::EndDisabled();
    if (!typed && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) Hint("TYPE filters MY LIBRARY, FAVORITES and RECENT.");
    }
    ImGui::Dummy(ImVec2(1, 2));

    const float footerH = 26.0f;
    const float bodyH = ImGui::GetContentRegionAvail().y - footerH;
    ImDrawList* dl = nullptr;

    // ---- left: sources, library folders, collections ----
    ImGui::BeginChild("##sbLeft", ImVec2(196.0f, bodyH), false);
    dl = ImGui::GetWindowDrawList();
    auto row = [&](const char* id, const std::string& label, bool on, int count, float indent, bool hasKids, bool expanded) -> int {
        ImGui::PushID(id);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x, h = 22.0f;
        const bool clicked = ImGui::InvisibleButton("##r", ImVec2(w, h));
        const bool hov = ImGui::IsItemHovered();
        const ImVec2 p1(p0.x + w, p0.y + h);
        if (on) { dl->AddRectFilled(p0, p1, IM_COL32(242, 163, 61, 38), 4.0f); dl->AddRect(p0, p1, vis::kAccentDim, 4.0f); }
        else if (hov) dl->AddRectFilled(p0, p1, vis::kCellHover, 4.0f);
        float x = p0.x + 8.0f + indent;
        int res = clicked ? 1 : 0;
        if (hasKids) {
            const float cy = p0.y + h * 0.5f;
            if (expanded) dl->AddTriangleFilled(ImVec2(x, cy - 2), ImVec2(x + 7, cy - 2), ImVec2(x + 3.5f, cy + 3), vis::kTextDim);
            else dl->AddTriangleFilled(ImVec2(x + 1, cy - 4), ImVec2(x + 1, cy + 4), ImVec2(x + 6, cy), vis::kTextDim);
            if (clicked && io.MousePos.x < x + 12.0f) res = 2;   // the triangle: open / close only
        }
        x += 12.0f;
        ImGui::PushClipRect(ImVec2(x, p0.y), ImVec2(p1.x - 36, p1.y), true);
        dl->AddText(ImVec2(x, p0.y + 3), on ? vis::kText : vis::kTextDim, label.c_str());
        ImGui::PopClipRect();
        if (count >= 0) { char cnt[16]; std::snprintf(cnt, sizeof cnt, "%d", count); dl->AddText(ImVec2(p1.x - 6 - ImGui::CalcTextSize(cnt).x, p0.y + 3), vis::kTextDim, cnt); }
        ImGui::PopID();
        return res;
    };
    auto setSrc = [&](Source s) { if (src_ != s) { src_ = s; lastSrc_[static_cast<int>(target_)] = s; folder_.clear(); sel_ = 0; dirty_ = true; scrollToSel_ = true; } };
    ImGui::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f), "SOURCE");
    if (row("src0", SourceName(Source::Project, target_), src_ == Source::Project, -1, 0, false, false)) setSrc(Source::Project);
    if (ImGui::IsItemHovered()) Hint(target_ == AppState::LibTarget::Drum ? "The samples of this drum in its kit (kick.wav, kick_2.wav ...)."
                                     : target_ == AppState::LibTarget::Transition ? "The transitions of this project (samples/transitions beside it)."
                                     : "The samples of this project (samples/ beside it).");
    const std::vector<LibSample>* libList = app.LibraryListIfReady();
    if (row("src1", libList ? "MY LIBRARY" : "MY LIBRARY (reading...)", src_ == Source::Library && folder_.empty(), libList ? static_cast<int>(libList->size()) : -1, 0, false, false)) { setSrc(Source::Library); if (!folder_.empty()) { folder_.clear(); dirty_ = true; } }
    if (ImGui::IsItemHovered()) Hint("Your own samples (Library > Show Samples Folder). A file you take is copied, the library is never changed.");
    if (target_ == AppState::LibTarget::Drum) {
        if (row("src2", kitScanDone_ ? "KITS" : "KITS (reading...)", src_ == Source::Kits, kitScanDone_ ? static_cast<int>(kitItems_.size()) : -1, 0, false, false)) setSrc(Source::Kits);
        if (ImGui::IsItemHovered()) Hint("The same drum in every kit that comes with the program.");
    }
    if (target_ == AppState::LibTarget::Transition) {
        if (row("src3", "BUILT-IN", src_ == Source::BuiltIn, -1, 0, false, false)) setSrc(Source::BuiltIn);
        if (ImGui::IsItemHovered()) Hint("The risers and impacts that come with the program.");
    }
    if (target_ == AppState::LibTarget::Sampler) {
        if (row("src4", "FROM FINDER...", false, -1, 0, false, false)) { app.RequestSamplePick(sampleTarget_); keepOpen = false; }
        if (ImGui::IsItemHovered()) Hint("Choose a .wav or .aif anywhere on the Mac. It is copied into this project.");
    }
    ImGui::Dummy(ImVec2(1, 6));
    ImGui::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f), "LIBRARY FOLDERS");
    {   // the folder tree of the library, with the number of files below each folder
        static const std::vector<LibSample> kNone;
        const auto& lib = libList ? *libList : kNone;
        if (libList && folderCountsOf_ != lib.size()) {
            folderCounts_.clear();
            for (const LibSample& ls : lib) for (const std::string& f : FolderChain(ls.rel)) ++folderCounts_[f];
            folderCountsOf_ = lib.size();
        }
        const std::map<std::string, int>& counts = folderCounts_;
        if (!libList) ImGui::TextDisabled("  (reading...)");
        else if (counts.empty()) ImGui::TextDisabled("  (no folders)");
        for (const auto& [f, cnt] : counts) {
            const std::vector<std::string> chain = FolderChain(f + "/x");
            bool shown = true;
            for (size_t k = 0; k + 1 < chain.size(); ++k) if (!openFolders_.count(chain[k])) { shown = false; break; }
            if (!shown) continue;
            const auto next = counts.upper_bound(f);
            const bool kids = next != counts.end() && next->first.compare(0, f.size() + 1, f + "/") == 0;
            const bool expanded = openFolders_.count(f) > 0;
            const int r = row(f.c_str(), BaseOf(f), src_ == Source::Library && folder_ == f, cnt, 12.0f * static_cast<float>(chain.size() - 1), kids, expanded);
            if (r == 2 || (r == 1 && kids && folder_ == f)) { if (expanded) openFolders_.erase(f); else openFolders_.insert(f); }
            else if (r == 1) { setSrc(Source::Library); folder_ = f; if (kids) openFolders_.insert(f); dirty_ = true; sel_ = 0; scrollToSel_ = true; }
        }
    }
    ImGui::Dummy(ImVec2(1, 6));
    ImGui::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f), "MY COLLECTIONS");
    if (row("col0", "FAVORITES", src_ == Source::Favorites, static_cast<int>(favs_.size()), 0, false, false)) setSrc(Source::Favorites);
    if (ImGui::IsItemHovered()) Hint("Files you starred (click the star in a row).");
    if (row("col1", "RECENT", src_ == Source::Recent, static_cast<int>(recent_.size()), 0, false, false)) setSrc(Source::Recent);
    if (ImGui::IsItemHovered()) Hint("The last 20 files you took, newest first.");
    ImGui::EndChild();
    ImGui::SameLine(0, 10);

    // ---- middle: the files ----
    const float detailW = 232.0f;
    ImGui::BeginChild("##sbFiles", ImVec2(ImGui::GetContentRegionAvail().x - detailW - 10.0f, bodyH), false);
    dl = ImGui::GetWindowDrawList();
    const float fullW = ImGui::GetContentRegionAvail().x;
    const float cardW = 176.0f, cardH = 52.0f, gap = 8.0f;
    const int cols = list_ ? 1 : std::max(1, static_cast<int>((fullW + gap) / (cardW + gap)));
    bool pick = false, moved = false, favToggled = false;
    if (n > 0 && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        int d = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) d = cols;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) d = -cols;
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) d = cols * 10;
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) d = -cols * 10;
        const bool lr = !searchActive || search_[0] == '\0';
        if (lr && !list_ && ImGui::IsKeyPressed(ImGuiKey_RightArrow)) d = 1;
        if (lr && !list_ && ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) d = -1;
        if (d != 0) { const int ns = std::clamp((sel_ < 0 ? 0 : sel_) + d, 0, n - 1); if (ns != sel_) { sel_ = ns; moved = true; scrollToSel_ = true; } }
        if (interaction::KeyConfirm()) pick = true;
    }
    {   // Left / Right in LIST: close / open the folder of the library (Left also goes up to the parent folder)
        const bool lr = list_ && src_ == Source::Library && (!searchActive || search_[0] == '\0') && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (lr && ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            if (!folder_.empty()) openFolders_.insert(folder_);
            else if (sel_ >= 0 && sel_ < n) { const std::string d0 = DirOf(items_[static_cast<size_t>(vis_[static_cast<size_t>(sel_)])].key); if (!d0.empty()) { folder_ = d0; for (const auto& f : FolderChain(d0 + "/x")) openFolders_.insert(f); dirty_ = true; sel_ = 0; } }
        }
        if (lr && ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && !folder_.empty()) {
            if (openFolders_.count(folder_)) openFolders_.erase(folder_);
            else { folder_ = DirOf(folder_); dirty_ = true; sel_ = 0; }
        }
    }
    if (moved && autoHear_) wantHear = true;

    auto drawStar = [&](const Item& it, ImVec2 c, bool rowHov) -> bool {   // returns true when the star was clicked
        const bool fav = favs_.count(it.favKey) > 0;
        const bool onStar = rowHov && std::fabs(io.MousePos.x - c.x) < 10 && std::fabs(io.MousePos.y - c.y) < 10;
        browser::DrawStar(dl, c, fav, onStar, vis::kText);
        return onStar;
    };
    auto clickItem = [&](int i, const Item& it, bool onStar) {
        if (onStar) { if (!favs_.erase(it.favKey)) favs_.insert(it.favKey); favToggled = true; return; }
        sel_ = i; if (autoHear_) wantHear = true;
    };
    const bool showBpm = src_ == Source::Library || src_ == Source::Favorites || src_ == Source::Recent;
    const float xName = 26.0f, xFolder = fullW * 0.46f, xType = fullW * 0.76f, xBpm = fullW * 0.88f;
    if (list_) {   // column titles: a click sorts by that column, a second click turns the order around
        const ImVec2 h0 = ImGui::GetCursorScreenPos();
        const char* const kCols[4] = {"NAME", copies ? "WHERE IT PLAYS" : "FOLDER", "TYPE", "BPM"};
        const float xs[4] = {xName, xFolder, xType, xBpm};
        for (int c = 0; c < 4; ++c) {
            if (c == 3 && !showBpm) continue;
            const float x1 = c < 3 ? xs[c + 1] : fullW;
            ImGui::SetCursorScreenPos(ImVec2(h0.x + xs[c], h0.y));
            ImGui::PushID(c);
            if (ImGui::InvisibleButton("##col", ImVec2(std::max(10.0f, x1 - xs[c] - 4.0f), 20.0f)) && src_ != Source::Recent) {
                if (sortCol_ == c) sortDesc_ = !sortDesc_; else { sortCol_ = c; sortDesc_ = false; }
                dirty_ = true;
            }
            if (ImGui::IsItemHovered()) Hint(src_ == Source::Recent ? "RECENT is always newest first." : "Sort by this column (click again = the other way round)");
            ImGui::PopID();
            std::string lab = kCols[c];
            if (sortCol_ == c && src_ != Source::Recent) lab += sortDesc_ ? "  v" : "  ^";
            dl->AddText(ImVec2(h0.x + xs[c], h0.y + 3), vis::kTextDim, lab.c_str());
        }
        ImGui::SetCursorScreenPos(ImVec2(h0.x, h0.y + 22.0f));
        dl->AddLine(ImVec2(h0.x, h0.y + 21.0f), ImVec2(h0.x + fullW, h0.y + 21.0f), vis::kLine);
    }
    ImGui::BeginChild("##sbRows", ImVec2(0, 0), false);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 1.0f));   // one row = rowH exactly, as the clipper expects
    dl = ImGui::GetWindowDrawList();
    const float rowH = list_ ? 22.0f : cardH + gap;
    const int rows = list_ ? n : (n + cols - 1) / cols;
    if (scrollToSel_ && sel_ >= 0) {   // keep the selection in view (the clipper skips rows that are not drawn)
        const float y = (sel_ / cols) * rowH, top = ImGui::GetScrollY(), hgt = ImGui::GetWindowHeight();
        if (y < top || y + rowH > top + hgt) ImGui::SetScrollY(std::max(0.0f, y - hgt * 0.5f));
    }
    ImGuiListClipper clip;
    clip.Begin(rows, rowH);
    while (clip.Step()) {
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
            const ImVec2 rowO = ImGui::GetCursorScreenPos();
            for (int cc = 0; cc < cols; ++cc) {
                const int i = r * cols + cc;
                if (i >= n) break;
                const Item& it = items_[static_cast<size_t>(vis_[static_cast<size_t>(i)])];
                ImGui::PushID(i);
                if (list_) {
                    const ImVec2 p0 = ImGui::GetCursorScreenPos();
                    const float w = ImGui::GetContentRegionAvail().x;
                    ImGui::InvisibleButton("##row", ImVec2(w, rowH - 1.0f));
                    const bool hov = ImGui::IsItemHovered();
                    const ImVec2 p1(p0.x + w, p0.y + rowH - 1.0f);
                    if (i == sel_) dl->AddRectFilled(p0, p1, IM_COL32(242, 163, 61, 34), 3.0f);
                    else if (hov) dl->AddRectFilled(p0, p1, vis::kCellHover, 3.0f);
                    const bool onStar = drawStar(it, ImVec2(p0.x + 10.0f, p0.y + rowH * 0.5f), hov);
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) clickItem(i, it, onStar);
                    if (hov && !onStar && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { sel_ = i; pick = true; }
                    auto cell = [&](float x0, float x1, const std::string& s, ImU32 col) {
                        ImGui::PushClipRect(ImVec2(p0.x + x0, p0.y), ImVec2(p0.x + x1 - 6.0f, p1.y), true);
                        dl->AddText(ImVec2(p0.x + x0, p0.y + 3), col, s.c_str());
                        ImGui::PopClipRect();
                    };
                    cell(xName, xFolder, it.name, vis::kText);
                    cell(xFolder, xType, it.folder, vis::kTextDim);
                    cell(xType, xBpm, it.type, vis::kTextDim);
                    if (showBpm && it.bpm > 0.0) { char b[16]; std::snprintf(b, sizeof b, "%.0f", it.bpm); cell(xBpm, fullW, b, vis::kTextDim); }
                    if (hov) Hint("%s\nclick = select   Space / HEAR = listen   Return / double-click = use for %s", it.name.c_str(), who.c_str());
                } else {
                    const ImVec2 p0(rowO.x + cc * (cardW + gap), rowO.y), p1(p0.x + cardW, p0.y + cardH);
                    ImGui::SetCursorScreenPos(p0);
                    ImGui::InvisibleButton("##card", ImVec2(cardW, cardH));
                    const bool hov = ImGui::IsItemHovered();
                    dl->AddRectFilled(p0, p1, hov ? vis::kCellHover : vis::kCell, 6.0f);
                    dl->AddRect(p0, p1, i == sel_ ? vis::kAccent : vis::kLine, 6.0f, 0, i == sel_ ? 2.0f : 1.0f);
                    const bool onStar = drawStar(it, ImVec2(p1.x - 14.0f, p0.y + 14.0f), hov);
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) clickItem(i, it, onStar);
                    if (hov && !onStar && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { sel_ = i; pick = true; }
                    ImGui::PushClipRect(ImVec2(p0.x + 8, p0.y), ImVec2(p1.x - 26, p1.y), true);
                    dl->AddText(ImVec2(p0.x + 10, p0.y + 7), vis::kText, it.name.c_str());
                    dl->AddText(ImVec2(p0.x + 10, p0.y + 27), vis::kTextDim, (it.type.empty() ? it.folder : it.type + "  " + it.folder).c_str());
                    ImGui::PopClipRect();
                    if (hov) Hint("%s\nclick = select   Space / HEAR = listen   Return / double-click = use for %s", it.name.c_str(), who.c_str());
                }
                ImGui::PopID();
            }
            if (!list_) { ImGui::SetCursorScreenPos(rowO); ImGui::Dummy(ImVec2(1.0f, rowH - 1.0f)); }
        }
    }
    clip.End();
    ImGui::PopStyleVar();
    if (n == 0) {
        ImGui::Dummy(ImVec2(1, 20));
        const bool narrowed = search_[0] || (type_ >= 0 && (src_ == Source::Library || src_ == Source::Favorites || src_ == Source::Recent)) || (src_ == Source::Library && !folder_.empty());
        const char* msg = src_ == Source::Kits && !kitScanDone_ ? "Reading the kit folders ..."
                        : waitKit_ ? "Reading the kit ..."
                        : waitLib_ ? "Reading the library ..."
                        : narrowed ? "Nothing here matches - clear the search or the TYPE."
                        : src_ == Source::Project && target_ == AppState::LibTarget::Drum ? "This kit has no sample for this drum. Try KITS or MY LIBRARY."
                        : copies && inUseOnly_ && copiedCount_ > 0 ? "No sample plays in this project yet. ALL COPIED shows every file copied into it."
                        : copies ? "This project has no samples yet. Take one from MY LIBRARY or FROM FINDER..."
                        : src_ == Source::Favorites && favs_.empty() ? "No favorites yet - click the star in a row."
                        : src_ == Source::Recent && recent_.empty() ? "Nothing taken yet."
                        : src_ == Source::Library && libList && libList->empty() ? "The library is empty. Put .wav / .aif files in the samples folder (Library > Show Samples Folder) and use Library > Rescan Samples."
                        : "Nothing here matches - clear the search or the TYPE.";
        ImGui::TextWrapped("%s", msg);
    }
    scrollToSel_ = false;
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::SameLine(0, 10);

    // ---- right: the selected file ----
    ImGui::BeginChild("##sbDetail", ImVec2(0, bodyH), true, ImGuiWindowFlags_NoScrollbar);
    const Item* cur = sel_ >= 0 && sel_ < n ? &items_[static_cast<size_t>(vis_[static_cast<size_t>(sel_)])] : nullptr;
    const float dw = ImGui::GetContentRegionAvail().x;
    ImGui::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f), "SELECTED");
    if (cur) {
        DrawWave(app, *cur, dw, 64.0f);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + dw);
        ImGui::TextUnformatted(cur->name.c_str());
        std::string meta = cur->type;
        if (cur->bpm > 0.0) { char b[24]; std::snprintf(b, sizeof b, "%s%.0f BPM", meta.empty() ? "" : "  ", cur->bpm); meta += b; }
        if (!meta.empty()) ImGui::TextDisabled("%s", meta.c_str());
        ImGui::TextDisabled("%s", (cur->src == Source::Library ? "My library / " + cur->folder : cur->folder).c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(1, 6));
        if (ImGui::Button("HEAR##sbHear", ImVec2(60, 0))) wantHear = true;
        if (ImGui::IsItemHovered()) Hint("Listen without taking it (Space)");
        ImGui::SameLine(0, 4);
        if (ToggleButton("AUTO##sbAuto", autoHear_, ImVec2(60, 0), ToggleTab())) autoHear_ = !autoHear_;
        if (ImGui::IsItemHovered()) Hint("AUTO-HEAR: play every file you click or browse to.\nOff by default, so a live set stays silent while you pick.");
        ImGui::Dummy(ImVec2(1, 4));
        const std::string use = "USE FOR " + who;
        if (ImGui::Button(use.c_str(), ImVec2(dw, 0))) pick = true;
        if (ImGui::IsItemHovered()) Hint(cur->src == Source::Project || cur->riser >= 0 ? "Take it (Return / double-click). Cmd+Z brings the previous one back."
                                                                                         : "Take it (Return / double-click): the file is copied into this project, the original is not changed.");
    } else ImGui::TextDisabled("Nothing selected.");
    ImGui::EndChild();
    if (favToggled) { SaveFavorites(); if (src_ == Source::Favorites) dirty_ = true; }

    // ---- footer ----
    ImGui::TextDisabled("%d shown   |   click = select   Space / HEAR = listen   Up / Down = browse   Left / Right = folders   Return / double-click = take   Esc = %s",
                        n, search_[0] ? "clear search" : "close without a change");

    if (wantHear && cur) Hear(app, *cur);
    {
        const bool hadSearch = search_[0] != '\0';
        if (browser::EscapeClosesOrClears(search_, &sel_, &scrollToSel_)) keepOpen = false;
        if (hadSearch && search_[0] == '\0') dirty_ = true;
    }
    if (pick && cur) { Take(app, *cur); keepOpen = false; }
    if (!keepOpen) { kitWanted_.clear(); app.CancelLibraryAudition(); ImGui::CloseCurrentPopup(); open_ = false; }
    ImGui::EndPopup();
}

} // namespace t8::desktop
