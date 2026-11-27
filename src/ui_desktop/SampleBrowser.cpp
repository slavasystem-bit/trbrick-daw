#include "SampleBrowser.h"
#include "ValueField.h"
#include "BrowserCore.h"
#include "InfoLine.h"
#include "Interaction.h"
#include "LiveWidgets.h"
#include "TrackStyle.h"
#include "Visual.h"
#include "core/SampleLibrary.h"
#include "core/UserDirs.h"
#include <ctime>
#include <unordered_map>
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
        case SampleBrowser::Source::Set: return "SET";
    }
    return "";
}
// "Set 26 Nov": the default name of a new performance set (C4b v3)
std::string TodaySetName() {
    static const char* const kMon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    return "Set " + std::to_string(tm.tm_mday) + " " + kMon[std::clamp(tm.tm_mon, 0, 11)];
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
    else if (target_ == AppState::LibTarget::Fill) who = brick_ >= 0 ? "FILL OF BRICK " + std::to_string(brick_ + 1) : "FILL OF SCENE " + std::to_string(-brick_);
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
    items_.clear(); vis_.clear();   // LIVE-5: the list is built on the next frame (Prepare); never show another target's files meanwhile
    sampleTarget_ = app.libSampleTarget;
    drumTrack_ = std::clamp(app.seqCursorTrack, 0, kNumDrumTracks - 1);
    scene_ = std::clamp(app.project.currentPattern, 0, kNumSlotsTotal - 1);
    LoadLists();
    auto it = lastSrc_.find(static_cast<int>(target));
    src_ = it != lastSrc_.end() ? it->second : Source::Project;
    if (src_ == Source::Kits && target != AppState::LibTarget::Drum) src_ = Source::Project;
    if (src_ == Source::BuiltIn && target != AppState::LibTarget::Transition) src_ = Source::Project;
    if (src_ == Source::Set) {   // a set that is gone (deleted meanwhile): back to FAVORITES
        const SampleSetList* sets = app.SampleSetsIfReady();
        if (set_ < 0 || (sets && set_ >= static_cast<int>(sets->size()))) { src_ = Source::Favorites; set_ = -1; }
    }
    if (src_ == Source::Project && (target == AppState::LibTarget::Sampler || target == AppState::LibTarget::Fill) && app.SampleNames().empty()) src_ = Source::Library;
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
    inUseOnly_ = true;                           // THIS PROJECT always opens on IN USE
    ++trashToken_; trashStage_ = 0; trashList_.clear();
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
        if (SampleFiles()) {
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
        const SampleTagMap* own = app.SampleTagsIfReady();               // the user's own genre / tags, read on the loader thread too
        if (!lib || !own) { waitLib_ = true; return; }
        for (const LibSample& ls : *lib) {
            Item it; it.src = Source::Library; it.key = ls.rel; it.name = ls.locked ? ls.name + "  [LOCKED]" : ls.name; it.folder = DirOf(ls.rel); it.type = ls.category; it.bpm = ls.bpm; it.favKey = LibFavKey(ls.rel);
            if (auto o = own->find(ls.rel); o != own->end()) { it.genre = o->second.genre; it.tags = o->second.tags; it.ownTags = true; }
            else { const auto& g = Guess(ls.rel); it.genre = g.first; it.tags = g.second; }
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
        case Source::Set: {   // C4b v3: a performance set keeps the order the files were added in (sortable like FAVORITES)
            const SampleSetList* sets = app.SampleSetsIfReady();
            if (!sets) { waitLib_ = true; break; }
            if (set_ < 0 || set_ >= static_cast<int>(sets->size())) break;
            std::vector<Item> all; everything(all);
            std::unordered_map<std::string, size_t> at; at.reserve(all.size());
            for (size_t i = 0; i < all.size(); ++i) at.emplace(all[i].favKey, i);
            for (const std::string& k : (*sets)[static_cast<size_t>(set_)].keys) if (auto f = at.find(k); f != at.end()) items_.push_back(all[f->second]);
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
    if (!selKeep_.empty()) {   // review of PR 35: after a GENRE / TAGS edit the sorted / filtered row can move: keep the same file
        for (int i = 0; i < n; ++i) if (items_[static_cast<size_t>(vis_[static_cast<size_t>(i)])].favKey == selKeep_) { sel_ = i; scrollToSel_ = true; break; }
        selKeep_.clear();
    }
    sel_ = n == 0 ? -1 : std::clamp(sel_, 0, n - 1);
    dirty_ = false;
}

bool SampleBrowser::Visible(const Item& it) const {
    if (src_ == Source::Library && !folder_.empty() && it.key.compare(0, folder_.size() + 1, folder_ + "/") != 0) return false;
    const bool typed = src_ == Source::Library || src_ == Source::Favorites || src_ == Source::Recent || src_ == Source::Set;
    if (type_ >= 0 && typed && it.type != kLibCategories[type_]) return false;
    if (genre_ >= 0 && typed && it.genre != kSampleGenres[genre_]) return false;
    if (tagMask_ && typed && (it.tags & tagMask_) != tagMask_) return false;
    if ((bpmLo_ > 0 || bpmHi_ > 0) && typed) {
        if (it.bpm <= 0.0) return false;
        if (bpmLo_ > 0 && it.bpm < bpmLo_ - 0.5) return false;
        if (bpmHi_ > 0 && it.bpm > bpmHi_ + 0.5) return false;
    }
    if (search_[0]) {
        const std::string hay = Lower(it.name + " " + it.folder + " " + it.type + " " + GenreTagsText(it));
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
            if (SampleFiles()) {
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
    if (target_ == AppState::LibTarget::Fill) {   // C4b: the SAMPLE fill of a brick / a scene (a library file is copied into samples/ first)
        if (it.src == Source::Project) app.SetFillSample(brick_, it.key);
        else app.AssignSampleToFill(it.src == Source::Library ? app.LibraryPath(it.key) : it.key, brick_);
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
    if (it.src == Source::Project && SampleFiles()) s = app.SampleFor(it.key);
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

const std::pair<std::string, unsigned>& SampleBrowser::Guess(const std::string& rel) {
    auto g = guess_.find(rel);
    if (g == guess_.end()) {
        unsigned m = 0;
        for (const std::string& t : GuessSampleTags(rel)) for (int i = 0; i < kNumSampleTags; ++i) if (t == kSampleTags[i]) m |= 1u << i;
        g = guess_.emplace(rel, std::make_pair(GuessSampleGenre(rel), m)).first;
    }
    return g->second;
}

void SampleBrowser::Prepare(AppState& app) {
    if (!open_ && !wantOpen_) return;
    // a source that needs data still on its way (a kit bank, the kit scan) is rebuilt every frame until it is there
    if (waitKit_ || waitLib_) dirty_ = true;   // the kit or the library is still being read
    if (dirty_) Rebuild(app);
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
    const int n = static_cast<int>(vis_.size());
    const std::string who = Who(app);

    // ---- top bar: title, search, LIST / GRID, close ----
    ImGui::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f), "SAMPLE FOR: %s", who.c_str());
    if (target_ != AppState::LibTarget::Transition && target_ != AppState::LibTarget::Fill) { ImGui::SameLine(0, 6); ImGui::TextDisabled("(scene %d)", scene_ + 1); }
    ImGui::SameLine(0, 18);
    if (focusSearch_) { ImGui::SetKeyboardFocusHere(); focusSearch_ = false; }
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::InputTextWithHint("##sbSearch", "search name, folder, type, genre, tag ...", search_, sizeof(search_))) { dirty_ = true; sel_ = 0; scrollToSel_ = true; }
    const bool searchActive = ImGui::IsItemActive();
    // C4b part 6 (review): a number typed into a BPM field belongs to that field: no search capture, no browse / take / close keys.
    // The field is drawn further down, so last frame's state is used (ImGui sets WantTextInput while a text field is being edited).
    const bool tagsBusy = !tagsKey_.empty() || !tagsWant_.empty() || menuWant_ || menuOpen_ || setMenuOpen_;   // a window or menu owns the keys
    const bool fieldTyping = (io.WantTextInput && !searchActive) || bpmTyping_ || tagsBusy;
    bpmTyping_ = false;   // set again below while a BPM field is being typed into
    if ((io.KeySuper || io.KeyCtrl) && ImGui::IsKeyPressed(ImGuiKey_F, false)) focusSearch_ = true;
    bool wantHear = trashStage_ == 0 && !fieldTyping && browser::HearKey(searchActive, search_);
    if (trashStage_ == 0 && !fieldTyping && browser::CaptureTyping(search_, sizeof(search_), searchActive)) { dirty_ = true; sel_ = 0; scrollToSel_ = true; }
    ImGui::SameLine(ImGui::GetWindowWidth() - 14.0f - 22.0f - 8.0f - 2 * 52.0f - 4.0f);
    if (ToggleButton("LIST##sbList", list_, ImVec2(52, 0), ToggleTab())) { list_ = true; scrollToSel_ = true; }
    if (ImGui::IsItemHovered()) Hint("One file per row: name, folder, type, tempo. A click on a column title sorts.");
    ImGui::SameLine(0, 4);
    if (ToggleButton("GRID##sbGrid", !list_, ImVec2(52, 0), ToggleTab())) { list_ = false; scrollToSel_ = true; }
    if (ImGui::IsItemHovered()) Hint("Cards, like the kit browser.");
    ImGui::SameLine(0, 8);
    if (ImGui::Button("X##sbClose", ImVec2(22, 0))) keepOpen = false;
    if (ImGui::IsItemHovered()) Hint("Close without a change (Esc)");

    const bool copies = src_ == Source::Project && SampleFiles();
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
        const char* trashLab = trashStage_ == 1 ? "LOOKING ...##sbTrash" : trashStage_ == 3 ? "MOVING ...##sbTrash" : "MOVE UNUSED TO TRASH...##sbTrash";
        {   // at the right end of the row, as in the mockup
            const float bw = ImGui::CalcTextSize("MOVE UNUSED TO TRASH...").x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 18.0f, ImGui::GetWindowContentRegionMax().x - bw));
        }
        ImGui::BeginDisabled(trashStage_ != 0 || copiedCount_ <= inUseCount_);
        if (ImGui::Button(trashLab)) {
            trashStage_ = 1;
            const unsigned tok = trashToken_;
            AppState* ap = &app;
            app.PlanTrashUnusedSamples([this, tok, ap](std::vector<std::string> list, int kept, std::string err) {
                if (tok != trashToken_) return;   // the browser was closed or opened again meanwhile
                trashStage_ = 0;
                if (!err.empty()) { ap->SetStatus("NOTHING MOVED: " + err); return; }
                if (list.empty()) { ap->SetStatus(kept > 0 ? "NOTHING TO MOVE: a saved version or another project still uses every copy" : "NOTHING TO MOVE: every copy is used"); return; }
                trashList_ = std::move(list); trashKept_ = kept; trashStage_ = 2;
            });
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            Hint("%s", copiedCount_ <= inUseCount_ ? "Every copy plays somewhere: nothing to move."
                                                   : "Move the copies nothing uses to the Trash (asks first). Kept: what this project, its Undo,\nits saved versions or another project in the same folder still uses.");
    }
    // ---- TYPE chips (library and the collections; the drums and transitions open on their own type) ----
    const bool typed = src_ == Source::Library || src_ == Source::Favorites || src_ == Source::Recent || src_ == Source::Set;
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
    // GENRE and TAGS (C4b v1): guessed from the folder and file names; the filters add up with TYPE, the folder and the search
    ImGui::BeginDisabled(!typed);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("GENRE");
    ImGui::SameLine(0, 8);
    if (ToggleButton("ALL##sbgAll", genre_ < 0, ImVec2(0, 0), ToggleTab())) { genre_ = -1; dirty_ = true; sel_ = 0; }
    for (int g = 0; g < kNumSampleGenres; ++g) {
        ImGui::SameLine(0, 4);
        char lab[32]; std::snprintf(lab, sizeof lab, "%s##sbg%d", kSampleGenres[g], g);
        if (ToggleButton(lab, genre_ == g, ImVec2(0, 0), ToggleTab())) { genre_ = genre_ == g ? -1 : g; dirty_ = true; sel_ = 0; }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) Hint("%s", typed ? "The genre the program reads from the folder and file names (\"BL_HipHop_95bpm\" = HIP HOP)." : "GENRE filters MY LIBRARY, FAVORITES and RECENT.");
    ImGui::BeginDisabled(!typed);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("TAGS ");
    ImGui::SameLine(0, 8);
    for (int t = 0; t < kNumSampleTags; ++t) {
        if (t) ImGui::SameLine(0, 4);
        char lab[32]; std::snprintf(lab, sizeof lab, "%s##sbtg%d", kSampleTags[t], t);
        if (ToggleButton(lab, (tagMask_ >> t) & 1u, ImVec2(0, 0), ToggleTab())) { tagMask_ ^= 1u << t; dirty_ = true; sel_ = 0; }
    }
    ImGui::SameLine(0, 18);
    ImGui::TextDisabled("BPM");
    ImGui::SameLine(0, 6);
    auto bpmField = [&](const char* id, int* v, const char* hint) {   // the program's number field: drag, wheel, type; Option+click = any
        float f = static_cast<float>(*v);
        ValueSpec sp; sp.lo = 0.0f; sp.hi = 300.0f; sp.step = 0.5f; sp.wheel = 1.0f; sp.def = 0.0f; sp.integer = true; sp.offText = "any";
        if (ValueField(id, &f, sp, 48.0f).changed) { *v = static_cast<int>(f); dirty_ = true; sel_ = 0; }
        if (ImGui::IsItemHovered()) Hint("%s", hint);
        ImGui::PushID(id); const ImGuiID wid = ImGui::GetID("##vf"); ImGui::PopID();   // the same id ValueField uses
        if (valuefield_detail::E().id == wid) bpmTyping_ = true;   // double-clicked: the number is being typed
    };
    bpmField("##sbBpmLo", &bpmLo_, "Lowest tempo (any = no limit). Files without a tempo in the name are hidden while a tempo is set.");
    ImGui::SameLine(0, 4);
    ImGui::TextDisabled("-");
    ImGui::SameLine(0, 4);
    bpmField("##sbBpmHi", &bpmHi_, "Highest tempo (any = no limit).");
    ImGui::EndDisabled();
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
    if (const SampleSetList* sets = app.SampleSetsIfReady()) {   // C4b v3: performance sets
        for (int si = 0; si < static_cast<int>(sets->size()); ++si) {
            const SampleSet& st = (*sets)[static_cast<size_t>(si)];
            char id[16]; std::snprintf(id, sizeof id, "set%d", si);
            if (row(id, st.name, src_ == Source::Set && set_ == si, static_cast<int>(st.keys.size()), 0, false, false)) { if (src_ != Source::Set || set_ != si) { set_ = si; setSrc(Source::Set); dirty_ = true; sel_ = 0; } }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { setMenu_ = si; std::snprintf(renameBuf_, sizeof renameBuf_, "%s", st.name.c_str()); ImGui::OpenPopup("##sbSetMenu"); }
            if (ImGui::IsItemHovered()) Hint("A set you gathered (right-click a file > ADD TO SET).\nRight-click here: rename or delete the set (the files stay).");
        }
        if (sets->empty()) { ImGui::TextDisabled("  sets: right-click a file"); if (ImGui::IsItemHovered()) Hint("Right-click a file > ADD TO SET > NEW SET gathers files for a gig."); }
        setMenuOpen_ = false;
        if (ImGui::BeginPopup("##sbSetMenu")) {
            setMenuOpen_ = true;
            const bool valid = setMenu_ >= 0 && setMenu_ < static_cast<int>(sets->size());
            if (!valid) ImGui::CloseCurrentPopup();
            else {
                ImGui::TextDisabled("RENAME (Return)");
                ImGui::SetNextItemWidth(200.0f);
                if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                if (ImGui::InputText("##sbSetName", renameBuf_, sizeof renameBuf_, ImGuiInputTextFlags_EnterReturnsTrue)) {
                    if (!app.RenameSampleSet(setMenu_, renameBuf_)) app.SetStatus("THAT NAME IS TAKEN OR EMPTY");
                    ImGui::CloseCurrentPopup();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("DELETE THIS SET (the files stay)")) {
                    if (app.DeleteSampleSet(setMenu_)) {
                        // review of PR 37: set_ is shared by every target (another one may remember Source::Set): always follow the shift
                        if (set_ == setMenu_) { set_ = -1; if (src_ == Source::Set) src_ = Source::Favorites; } else if (set_ > setMenu_) --set_;
                        dirty_ = true; sel_ = 0;
                    }
                }
                if (interaction::KeyCancel()) ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
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
    if (n > 0 && !fieldTyping && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
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
        const bool lr = list_ && src_ == Source::Library && !fieldTyping && (!searchActive || search_[0] == '\0') && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
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
    auto editTags = [&](int i, const Item& it) {   // right-click: the menu of this file (GENRE / TAGS, ADD TO SET, SHOW IN FINDER)
        sel_ = i;
        menuItem_ = it; menuWant_ = true;
    };
    const bool showBpm = src_ == Source::Library || src_ == Source::Favorites || src_ == Source::Recent || src_ == Source::Set;
    // THIS PROJECT copies have no type or tempo: WHERE IT PLAYS gets the rest of the row (the whole text is in the info line too)
    // Columns, left to right: NAME, FOLDER (THIS PROJECT copies: WHERE IT PLAYS), TYPE, GENRE · TAGS, BPM. The library and the
    // collections show all five; THIS PROJECT copies have no type or tempo, so WHERE IT PLAYS gets the rest of the row (the whole
    // text is in the info line too); other sources show NAME, FOLDER, TYPE.
    const float xName = 26.0f;
    const float xFolder = fullW * (copies ? 0.40f : showBpm ? 0.36f : 0.46f);
    const float xType = copies ? fullW : fullW * (showBpm ? 0.56f : 0.76f);
    const float xGenre = showBpm ? fullW * 0.66f : fullW;
    const float xBpm = showBpm ? fullW * 0.90f : fullW;
    if (list_) {   // column titles: a click sorts by that column, a second click turns the order around
        const ImVec2 h0 = ImGui::GetCursorScreenPos();
        const char* const kCols[5] = {"NAME", copies ? "WHERE IT PLAYS" : "FOLDER", "TYPE", "GENRE \xC2\xB7 TAGS", "BPM"};
        const int kSortOf[5] = {0, 1, 2, 4, 3};   // SortItems' column numbers
        const float xs[6] = {xName, xFolder, xType, xGenre, xBpm, fullW};
        for (int c = 0; c < 5; ++c) {
            if (xs[c + 1] - xs[c] < 1.0f && c < 4) continue;   // a column that has no room here
            if (c == 4 && !showBpm) continue;
            const int sc = kSortOf[c];
            ImGui::SetCursorScreenPos(ImVec2(h0.x + xs[c], h0.y));
            ImGui::PushID(c);
            if (ImGui::InvisibleButton("##col", ImVec2(std::max(10.0f, xs[c + 1] - xs[c] - 4.0f), 20.0f)) && src_ != Source::Recent) {
                if (sortCol_ == sc) sortDesc_ = !sortDesc_; else { sortCol_ = sc; sortDesc_ = false; }
                dirty_ = true;
            }
            if (ImGui::IsItemHovered()) Hint(src_ == Source::Recent ? "RECENT is always newest first." : "Sort by this column (click again = the other way round)");
            ImGui::PopID();
            std::string lab = kCols[c];
            if (sortCol_ == sc && src_ != Source::Recent) lab += sortDesc_ ? "  v" : "  ^";
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
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) editTags(i, it);
                    if (hov && !onStar && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { sel_ = i; pick = true; }
                    auto cell = [&](float x0, float x1, const std::string& s, ImU32 col) {
                        ImGui::PushClipRect(ImVec2(p0.x + x0, p0.y), ImVec2(p0.x + x1 - 6.0f, p1.y), true);
                        dl->AddText(ImVec2(p0.x + x0, p0.y + 3), col, s.c_str());
                        ImGui::PopClipRect();
                    };
                    cell(xName, xFolder, it.name, vis::kText);
                    cell(xFolder, xType, it.folder, vis::kTextDim);
                    cell(xType, xGenre, it.type, vis::kTextDim);
                    if (showBpm) cell(xGenre, xBpm, GenreTagsText(it), vis::kTextDim);
                    if (showBpm && it.bpm > 0.0) { char b[16]; std::snprintf(b, sizeof b, "%.0f", it.bpm); cell(xBpm, fullW, b, vis::kTextDim); }
                    if (hov) Hint("%s  -  %s\nclick = select   Space / HEAR = listen   Return / double-click = use for %s%s", it.name.c_str(), it.folder.c_str(), who.c_str(), "\nright-click = menu: genre / tags, add to set, show in Finder");
                } else {
                    const ImVec2 p0(rowO.x + cc * (cardW + gap), rowO.y), p1(p0.x + cardW, p0.y + cardH);
                    ImGui::SetCursorScreenPos(p0);
                    ImGui::InvisibleButton("##card", ImVec2(cardW, cardH));
                    const bool hov = ImGui::IsItemHovered();
                    dl->AddRectFilled(p0, p1, hov ? vis::kCellHover : vis::kCell, 6.0f);
                    dl->AddRect(p0, p1, i == sel_ ? vis::kAccent : vis::kLine, 6.0f, 0, i == sel_ ? 2.0f : 1.0f);
                    const bool onStar = drawStar(it, ImVec2(p1.x - 14.0f, p0.y + 14.0f), hov);
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) clickItem(i, it, onStar);
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) editTags(i, it);
                    if (hov && !onStar && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { sel_ = i; pick = true; }
                    ImGui::PushClipRect(ImVec2(p0.x + 8, p0.y), ImVec2(p1.x - 26, p1.y), true);
                    dl->AddText(ImVec2(p0.x + 10, p0.y + 7), vis::kText, it.name.c_str());
                    dl->AddText(ImVec2(p0.x + 10, p0.y + 27), vis::kTextDim, (it.type.empty() ? it.folder : it.type + "  " + it.folder).c_str());
                    ImGui::PopClipRect();
                    if (hov) Hint("%s  -  %s\nclick = select   Space / HEAR = listen   Return / double-click = use for %s%s", it.name.c_str(), it.folder.c_str(), who.c_str(), "\nright-click = menu: genre / tags, add to set, show in Finder");
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
        const bool narrowed = search_[0] || ((type_ >= 0 || genre_ >= 0 || tagMask_ || bpmLo_ > 0 || bpmHi_ > 0) && (src_ == Source::Library || src_ == Source::Favorites || src_ == Source::Recent || src_ == Source::Set)) || (src_ == Source::Library && !folder_.empty());
        const char* msg = src_ == Source::Kits && !kitScanDone_ ? "Reading the kit folders ..."
                        : waitKit_ ? "Reading the kit ..."
                        : waitLib_ ? "Reading the library ..."
                        : narrowed ? "Nothing here matches - clear the search, TYPE, GENRE, TAGS or BPM."
                        : src_ == Source::Project && target_ == AppState::LibTarget::Drum ? "This kit has no sample for this drum. Try KITS or MY LIBRARY."
                        : copies && inUseOnly_ && copiedCount_ > 0 ? "No sample plays in this project yet. ALL COPIED shows every file copied into it."
                        : copies ? "This project has no samples yet. Take one from MY LIBRARY or FROM FINDER..."
                        : src_ == Source::Favorites && favs_.empty() ? "No favorites yet - click the star in a row."
                        : src_ == Source::Recent && recent_.empty() ? "Nothing taken yet."
                        : src_ == Source::Set ? "This set is empty (or its files are gone). Right-click a file > ADD TO SET."
                        : src_ == Source::Library && libList && libList->empty() ? "The library is empty. Put .wav / .aif files in the samples folder (Library > Show Samples Folder) and use Library > Rescan Samples."
                        : "Nothing here matches - clear the search, TYPE, GENRE, TAGS or BPM.";
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
        if (const std::string gt = GenreTagsText(*cur); !gt.empty()) meta += (meta.empty() ? "" : "\n") + gt;
        if (!meta.empty()) ImGui::TextDisabled("%s", meta.c_str());
        ImGui::TextDisabled("%s", (cur->src == Source::Library ? "My library / " + cur->folder : cur->folder).c_str());
        ImGui::PopTextWrapPos();
        if (cur->src == Source::Library) {
            if (MiniButton("GENRE / TAGS...##sbTagsBtn")) { tagsWant_ = cur->key; tagsName_ = cur->name; }
            if (ImGui::IsItemHovered()) Hint("Correct the genre and tags of this file (also: right-click the row > GENRE / TAGS...).\nKept in Library/sample_tags.txt; the file itself is not changed.");
            ImGui::SameLine(0, 6);
            ImGui::TextDisabled("%s", cur->ownTags ? "your own" : "guessed");
        }
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

    // ---- MOVE UNUSED TO TRASH: the question (Esc = cancel; Return does nothing, a move to the Trash is never one key away) ----
    if (trashStage_ == 2 && !app.TrashPlanStillValid()) {   // another project was opened meanwhile (a dropped file): the list is not its own
        trashStage_ = 0; trashList_.clear(); app.SetStatus("NOTHING MOVED: ANOTHER PROJECT IS OPEN NOW");
    }
    if (trashStage_ == 2 && !ImGui::IsPopupOpen("##sbTrashAsk")) ImGui::OpenPopup("##sbTrashAsk");
    if (ImGui::BeginPopupModal("##sbTrashAsk", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) {
        const int nT = static_cast<int>(trashList_.size());
        ImGui::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f), "Move %d unused cop%s to the Trash?", nT, nT == 1 ? "y" : "ies");
        ImGui::TextDisabled("Nothing in this project, its Undo, its saved versions or the other projects in its folder uses %s.", nT == 1 ? "it" : "them");
        ImGui::TextDisabled("You can take %s back out of the Trash.", nT == 1 ? "it" : "them");
        ImGui::Spacing();
        for (int i = 0; i < std::min(nT, 10); ++i) ImGui::Text("  %s", trashList_[static_cast<size_t>(i)].c_str());
        if (nT > 10) ImGui::TextDisabled("  ... and %d more", nT - 10);
        if (trashKept_ > 0) { ImGui::Spacing(); ImGui::TextDisabled("%d more %s kept: a saved version or another project uses %s.", trashKept_, trashKept_ == 1 ? "is" : "are", trashKept_ == 1 ? "it" : "them"); }
        ImGui::Spacing();
        if (ImGui::Button("MOVE TO TRASH", ImVec2(150, 0))) {
            trashStage_ = 3;
            const unsigned tok = trashToken_;
            AppState* ap = &app;
            app.TrashSampleFiles(trashList_, [this, tok, ap](int moved, std::string err) {
                ap->SetStatus(err.empty() ? "MOVED " + std::to_string(moved) + " UNUSED SAMPLE" + (moved == 1 ? "" : "S") + " TO THE TRASH"
                              : moved == 0 ? "NOTHING MOVED: " + err
                                           : "MOVED " + std::to_string(moved) + " TO THE TRASH; NOT MOVED: " + err);
                if (tok != trashToken_) return;
                trashStage_ = 0; dirty_ = true; sel_ = 0;
            });
            trashList_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("CANCEL", ImVec2(110, 0)) || interaction::KeyCancel() || trashStage_ != 2) { if (trashStage_ == 2) trashStage_ = 0; trashList_.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    // ---- the right-click menu of a file (C4b v3): GENRE / TAGS..., ADD TO SET, REMOVE FROM THIS SET, SHOW IN FINDER ----
    if (menuWant_ && trashStage_ == 0) ImGui::OpenPopup("##sbRowMenu");
    menuWant_ = false;
    menuOpen_ = false;
    if (ImGui::BeginPopup("##sbRowMenu")) {
        menuOpen_ = true;
        const Item& mi = menuItem_;
        ImGui::TextDisabled("%s", mi.name.c_str());
        ImGui::Separator();
        if (mi.src == Source::Library && ImGui::MenuItem("GENRE / TAGS...")) { tagsWant_ = mi.key; tagsName_ = mi.name; }
        const SampleSetList* sets = app.SampleSetsIfReady();
        if (ImGui::BeginMenu("ADD TO SET", sets != nullptr && !mi.favKey.empty())) {
            for (int si = 0; sets && si < static_cast<int>(sets->size()); ++si) {
                const SampleSet& st = (*sets)[static_cast<size_t>(si)];
                const bool in = std::find(st.keys.begin(), st.keys.end(), mi.favKey) != st.keys.end();
                char lab[80]; std::snprintf(lab, sizeof lab, "%s##sbAdd%d", st.name.c_str(), si);
                if (ImGui::MenuItem(lab, in ? "in it" : nullptr, false, !in) && app.AddToSampleSet(si, mi.favKey)) { if (src_ == Source::Set && set_ == si) dirty_ = true; }
            }
            if (sets && !sets->empty()) ImGui::Separator();
            if (ImGui::MenuItem("NEW SET")) {
                const int ns = app.NewSampleSet(TodaySetName(), mi.favKey);
                if (ns < 0) app.SetStatus("CANNOT MAKE A NEW SET (64 AT MOST)");
                else app.SetStatus("ADDED TO " + (*app.SampleSetsIfReady())[static_cast<size_t>(ns)].name + " (right-click the set to rename it)");
            }
            ImGui::EndMenu();
        }
        if (src_ == Source::Set && sets && set_ >= 0 && set_ < static_cast<int>(sets->size())) {
            const std::string lab = "REMOVE FROM " + (*sets)[static_cast<size_t>(set_)].name;
            if (ImGui::MenuItem(lab.c_str()) && app.RemoveFromSampleSet(set_, mi.favKey)) dirty_ = true;
        }
        std::string where;   // the file on disk, when there is one
        if (mi.src == Source::Library) where = app.LibraryPath(mi.key);
        else if (mi.src == Source::Kits) where = mi.key;
        else if (mi.src == Source::Project && mi.riser < 0 && mi.variant < 0) where = app.SamplesDir() + "/" + mi.key;
        if (ImGui::MenuItem("SHOW IN FINDER", nullptr, false, !where.empty())) { if (!userdirs::Reveal(where)) app.SetStatus("CANNOT OPEN FINDER"); }
        if (interaction::KeyCancel()) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    // ---- GENRE / TAGS of one library file (C4b v2): every click is kept at once in Library/sample_tags.txt ----
    if (!tagsWant_.empty() && trashStage_ == 0) { tagsKey_ = tagsWant_; tagsWant_.clear(); ImGui::OpenPopup("##sbTags"); }
    else tagsWant_.clear();
    if (ImGui::BeginPopupModal("##sbTags", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) {
        const SampleTagMap* own = app.SampleTagsIfReady();
        const auto& g = Guess(tagsKey_);
        const auto o = own ? own->find(tagsKey_) : SampleTagMap::const_iterator();
        const bool mine = own && o != own->end();
        SampleTagOverride now = mine ? o->second : SampleTagOverride{g.first, g.second};
        bool changed = false;
        ImGui::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f), "GENRE / TAGS");
        ImGui::SameLine(0, 10);
        ImGui::TextUnformatted(tagsName_.c_str());
        ImGui::TextDisabled("%s", mine ? "Your own choice (kept in Library/sample_tags.txt; the file is not changed)." : "Guessed from the folder and file names. A click makes it your own.");
        ImGui::Dummy(ImVec2(1, 4));
        ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("GENRE"); ImGui::SameLine(64.0f);
        if (ToggleButton("NONE##sbtgN", now.genre.empty(), ImVec2(0, 0), ToggleTab())) { now.genre.clear(); changed = true; }
        for (int i = 0; i < kNumSampleGenres; ++i) {
            if (i == 5) { ImGui::Dummy(ImVec2(1, 0)); ImGui::SameLine(64.0f); } else ImGui::SameLine(0, 4);
            char lab[40]; std::snprintf(lab, sizeof lab, "%s##sbtg%d", kSampleGenres[i], i);
            if (ToggleButton(lab, now.genre == kSampleGenres[i], ImVec2(0, 0), ToggleTab())) { now.genre = kSampleGenres[i]; changed = true; }
        }
        ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("TAGS"); ImGui::SameLine(64.0f);
        for (int t = 0; t < kNumSampleTags; ++t) {
            if (t) ImGui::SameLine(0, 4);
            char lab[40]; std::snprintf(lab, sizeof lab, "%s##sbtt%d", kSampleTags[t], t);
            if (ToggleButton(lab, (now.tags >> t) & 1u, ImVec2(0, 0), ToggleTab())) { now.tags ^= 1u << t; changed = true; }
        }
        if (ImGui::IsItemHovered()) Hint("Tags add up: click again to take one off.");
        ImGui::Dummy(ImVec2(1, 6));
        if (changed && app.SetSampleTagOverride(tagsKey_, &now)) { dirty_ = true; selKeep_ = LibFavKey(tagsKey_); }
        ImGui::BeginDisabled(!mine);
        if (ImGui::Button("BACK TO THE GUESS", ImVec2(170, 0)) && app.SetSampleTagOverride(tagsKey_, nullptr)) { dirty_ = true; selKeep_ = LibFavKey(tagsKey_); }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) Hint("Forget your own choice for this file: the guess is shown again.");
        ImGui::SameLine();
        if (ImGui::Button("DONE", ImVec2(110, 0)) || interaction::KeyCancel() || interaction::KeyConfirm() || trashStage_ != 0) { tagsKey_.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    } else tagsKey_.clear();
    const bool trashBusy = trashStage_ != 0 || tagsBusy;   // a question (or its answer) owns the keys

    // ---- footer ----
    ImGui::TextDisabled("%d shown   |   click = select   Space / HEAR = listen   Up / Down = browse   Left / Right = folders   Return / double-click = take   Esc = %s",
                        n, search_[0] ? "clear search" : "close without a change");

    if (trashBusy) { wantHear = false; pick = false; }
    if (wantHear && cur) Hear(app, *cur);
    if (!trashBusy && !fieldTyping) {
        const bool hadSearch = search_[0] != '\0';
        if (browser::EscapeClosesOrClears(search_, &sel_, &scrollToSel_)) keepOpen = false;
        if (hadSearch && search_[0] == '\0') dirty_ = true;
    }
    if (pick && cur) { Take(app, *cur); keepOpen = false; }
    if (!keepOpen) { kitWanted_.clear(); app.CancelLibraryAudition(); ImGui::CloseCurrentPopup(); open_ = false; }
    ImGui::EndPopup();
}

} // namespace t8::desktop
