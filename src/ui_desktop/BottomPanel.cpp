#include "BottomPanel.h"
#include "core/Scales.h"
#include "Visual.h"
#include "LiveWidgets.h"
#include "ValueField.h"
#include "MixerSync.h"
#include "InstrumentSwitches.h"
#include "core/DrumBreaks.h"
#include "core/DrumPatterns.h"
#include "core/KitInfo.h"
#include "imgui.h"
#include "core/UserDirs.h"
#include "InfoLine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <random>
#include <string>

namespace t8::desktop {

namespace {

constexpr ImVec4 kAmberV(0.95f, 0.64f, 0.24f, 1.0f);

void VSep() {
    ImGui::SameLine(0, 8);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetContentRegionAvail().y;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y), ImVec2(p.x, p.y + h), vis::kLine, 1.0f);
    ImGui::SameLine(0, 18);
}

// category of a typical beat by its name (the list has no category field)
const char* BeatCategory(const std::string& n) {
    static const char* const house[] = {"FOUR ON THE FLOOR", "HOUSE", "DEEP HOUSE", "TECH HOUSE"};
    static const char* const techno[] = {"TECHNO", "MINIMAL TECHNO", "ACID TECHNO", "DUB TECHNO", "INDUSTRIAL TECHNO", "EBM"};
    static const char* const electro[] = {"ELECTRO", "ELECTRO FUNK"};
    static const char* const breaks[] = {"BREAKS", "AMEN", "BREAKBEAT", "JUNGLE", "DRUM AND BASS", "DUBSTEP", "UK GARAGE"};
    static const char* const hiphop[] = {"HIP HOP", "TRAP", "BOOM BAP", "LO-FI HIP HOP"};
    for (const char* k : house) if (n == k) return "HOUSE";
    for (const char* k : techno) if (n == k) return "TECHNO";
    for (const char* k : electro) if (n == k) return "ELECTRO";
    for (const char* k : breaks) if (n == k) return "BREAKS";
    for (const char* k : hiphop) if (n == k) return "HIP HOP";
    return "GROOVE";   // disco, funk, afrobeat, reggaeton
}

} // namespace

void BottomPanel::Draw(AppState& app) {
    SceneLock lock(app.sequencer.PatternMutex(), "BROWSERS");
    ImGui::PushID("BottomPanel");
    DrawRandomKit(app);
    VSep();
    DrawLive(app);
    DrawBrowsers(app);
    ImGui::PopID();
}

// v44.26: the kit browser (and the audition) belong to every screen that has a KIT picker, not only to LIVE: the SOUND view of a drum
// on TRACK asks for it with app.kitBrowserRequest too. Called once per frame by whichever screen is shown.
void BottomPanel::DrawBrowsers(AppState& app) {
    SceneLock lock(app.sequencer.PatternMutex(), "BROWSERS");
    ImGui::PushID("BottomPanelBrowsers");
    if (app.kitBrowserRequest) { app.kitBrowserRequest = false; OpenKitBrowser(app); }
    if (app.libraryBrowserRequest != AppState::LibTarget::None) { const auto t = app.libraryBrowserRequest; app.libraryBrowserRequest = AppState::LibTarget::None; OpenLibraryBrowser(app, t, app.libraryBrowserBrick); }
    if (app.kitAuditionRequest) { app.kitAuditionRequest = false; auditionKit_.clear(); ScheduleKitAudition(); }
    RunKitAudition(app);
    {   // the two big browser windows (they are modal popups, so they are drawn once per frame outside any group)
        const auto& kits = app.KitFolders();
        const std::string& curFolder = app.TrackKitFolder(app.project.currentPattern, app.seqCursorTrack);   // a drum with a kit of its own browses that kit
        int curKit = -1;
        for (size_t i = 0; i < kits.size(); ++i) if (kits[i] == curFolder) curKit = static_cast<int>(i);
        kitBrowser_.Draw(curKit,
            [&](const ListBrowser::Item&) {},   // C4b: a click only selects (try kits while it plays with < > in the KIT row)
            [&](const ListBrowser::Item& it) { if (it.id >= 0 && it.id < static_cast<int>(kits.size())) LoadKit(app, kits[static_cast<size_t>(it.id)]); },   // Return / double-click = take
            [&](const ListBrowser::Item& it) {   // HEAR: the short phrase from the SELECTED kit, without loading it
                if (it.id < 0 || it.id >= static_cast<int>(kits.size())) return;
                const std::string& k = kits[static_cast<size_t>(it.id)];
                auditionKit_ = k == app.TrackKitFolder(app.project.currentPattern, app.seqCursorTrack) ? std::string() : k;
                auditionKitAsked_ = ImGui::GetTime();
                if (!auditionKit_.empty()) (void)app.KitBank(auditionKit_);   // starts reading it (loader thread)
                ScheduleKitAudition();
            });
        beatBrowser_.Draw(app.rndGenreIndex,
            [&](const ListBrowser::Item& it) { app.rndGenreIndex = it.id; },
            [&](const ListBrowser::Item& it) { app.rndGenreIndex = it.id; ApplyBeat(app, it.id); });
        sampleBrowser_.Draw(app);
        styleBrowser_.Draw(app.styleIndex,
            [&](const ListBrowser::Item& it) { app.styleIndex = it.id; },
            [&](const ListBrowser::Item& it) { app.styleIndex = it.id; app.ApplyStyle(it.id); });
    }
    ImGui::PopID();
}

void BottomPanel::LoadKit(AppState& app, const std::string& kitPath) { app.LoadKitFolder(kitPath); }

void BottomPanel::ScheduleKitAudition() {
    // kick, hat, snare, hat, kick, open hat, snare: short enough to judge a kit, long enough to hear its character
    const double t = ImGui::GetTime();
    audition_.clear();
    static const struct { double dt; int slot; float v; } seq[] = {
        {0.00, static_cast<int>(DrumSlot::Kick), 1.0f}, {0.28, static_cast<int>(DrumSlot::ClosedHat), 0.8f}, {0.42, static_cast<int>(DrumSlot::Snare), 1.0f},
        {0.56, static_cast<int>(DrumSlot::ClosedHat), 0.8f}, {0.70, static_cast<int>(DrumSlot::Kick), 1.0f}, {0.84, static_cast<int>(DrumSlot::OpenHat), 0.8f},
        {0.98, static_cast<int>(DrumSlot::Snare), 1.0f}};
    for (const auto& h : seq) audition_.push_back({t + h.dt, h.slot, h.v});
}

void BottomPanel::RunKitAudition(AppState& app) {
    if (audition_.empty()) return;
    const double now = ImGui::GetTime();
    DrumKit::BankPtr bank;
    if (!auditionKit_.empty()) {   // C4b: another kit than the sounding one: wait for its bank (at most 3 s), then play from it
        bank = app.KitBank(auditionKit_);
        if (!bank) {
            if (now - auditionKitAsked_ > 3.0) { audition_.clear(); app.SetStatus("COULD NOT READ THAT KIT IN TIME"); return; }
            const double shift = ImGui::GetIO().DeltaTime;
            for (auto& h : audition_) h.at += shift;   // the phrase starts when the kit is there
            return;
        }
    }
    for (size_t i = 0; i < audition_.size();) {
        if (audition_[i].at <= now) {
            if (bank) {
                const auto& v = bank->variants[static_cast<size_t>(std::clamp(audition_[i].slot, 0, kNumDrumTracks - 1))];
                if (!v.empty() && v[0] && v[0]->frames() >= 2) {
                    auto sd = std::make_shared<SampleData>();
                    sd->data = v[0]->data; sd->channels = std::clamp(v[0]->channels, 1, 2); sd->sampleRate = v[0]->sampleRate;
                    auditionKeep_.push_back(sd);
                    while (auditionKeep_.size() > 16) auditionKeep_.erase(auditionKeep_.begin());
                    app.engine.AuditionSample(sd, 0.8f * audition_[i].vel);
                }
            } else {
                DrumVoiceParams p;
                app.engine.TriggerDrum(static_cast<DrumSlot>(audition_[i].slot), audition_[i].vel, 0, p);
            }
            audition_.erase(audition_.begin() + static_cast<long>(i));
        } else ++i;
    }
}

void BottomPanel::OpenKitBrowser(AppState& app) {
    ListBrowser::Config cfg;
    cfg.title = "DRUM KITS";
    cfg.categories = {"CLASSIC", "ELECTRONIC", "HARD", "GROOVE", "MY KITS"};
    cfg.tags = {"SUB", "PUNCHY", "DRIVEN", "DISTORTED", "LONG", "SHORT", "CRUSHED"};
    cfg.favFile = userdirs::FavoritesDir() + "/favorites_kits.txt";
    cfg.footer = "click = select   HEAR / Space = listen   arrows = browse   Enter / double-click = load";
    cfg.liveSelect = false;   // C4b: the same keys as every browser (try kits while it plays with < > in the KIT row)
    std::vector<ListBrowser::Item> items;
    const auto& kits = app.KitFolders();
    int cur = -1;
    for (size_t i = 0; i < kits.size(); ++i) {
        ListBrowser::Item it; it.id = static_cast<int>(i);
        it.name = kits[i].rfind("kits/", 0) == 0 ? kits[i].substr(5) : kits[i];
        const KitInfo* ki = FindKitInfo(it.name);
        it.category = ki ? ki->category : "MY KITS";
        if (ki) it.tags = ki->tags;
        it.favKey = it.name;
        items.push_back(std::move(it));
        if (kits[i] == app.TrackKitFolder(app.project.currentPattern, app.seqCursorTrack)) cur = static_cast<int>(i);
    }
    kitBrowser_.Open(cfg, std::move(items), cur);
}

void BottomPanel::OpenLibraryBrowser(AppState& app, AppState::LibTarget target, int brick) {
    sampleBrowser_.Open(app, target, brick);   // C4b: the one SAMPLES browser
}

void BottomPanel::OpenBeatBrowser(AppState& app) {
    ListBrowser::Config cfg;
    cfg.title = "TYPICAL BEATS";
    cfg.categories = {"HOUSE", "TECHNO", "ELECTRO", "BREAKS", "HIP HOP", "GROOVE", "MY BEATS"};
    cfg.tags = {"4/4", "BROKEN", "SWING", "BUSY", "SPARSE", "LONG"};
    cfg.favFile = userdirs::FavoritesDir() + "/favorites_beats.txt";
    cfg.footer = "click = select   Enter / double-click = apply to the drum tracks, a LONG one also lengthens the scene (Undo brings it all back)";
    cfg.liveSelect = false;
    std::vector<ListBrowser::Item> items;
    {
        ListBrowser::Item r; r.id = 0; r.name = "RANDOM (ANY)"; r.category = ""; r.favKey = "random"; items.push_back(std::move(r));
    }
    const auto& pats = AllGenrePatterns();
    for (int i = 0; i < kNumGenrePatterns; ++i) {
        const GenrePattern& g = pats[static_cast<size_t>(i)];
        ListBrowser::Item it; it.id = i + 1; it.name = g.name; it.category = BeatCategory(it.name); it.favKey = it.name;
        const auto& kick = g.hitSteps[static_cast<size_t>(DrumSlot::Kick)];
        auto hasStep = [&](int st) { return std::find(kick.begin(), kick.end(), st) != kick.end(); };
        const int nb = std::max(1, g.bars);   // v44.47: busy / sparse are counted per bar, so a 4-bar pattern is not "busy" just for being long
        int hits = 0; for (const auto& v : g.hitSteps) hits += static_cast<int>(v.size());
        hits /= nb;
        const int hats = static_cast<int>(g.hitSteps[static_cast<size_t>(DrumSlot::ClosedHat)].size() + g.hitSteps[static_cast<size_t>(DrumSlot::OpenHat)].size()) / nb;
        if (hasStep(0) && hasStep(4) && hasStep(8) && hasStep(12)) it.tags.push_back("4/4"); else it.tags.push_back("BROKEN");
        if (g.swing > 0.02f) it.tags.push_back("SWING");
        if (hats >= 12) it.tags.push_back("BUSY");
        if (hits <= 14) it.tags.push_back("SPARSE");
        if (nb > 1) it.tags.push_back("LONG");
        items.push_back(std::move(it));
    }
    const auto& customs = app.CustomPatternNamesCached();
    for (size_t i = 0; i < customs.size(); ++i) {
        ListBrowser::Item it; it.id = 1 + kNumGenrePatterns + static_cast<int>(i); it.name = customs[i]; it.category = "MY BEATS"; it.favKey = "my:" + customs[i];
        items.push_back(std::move(it));
    }
    beatBrowser_.Open(cfg, std::move(items), app.rndGenreIndex);
}

void BottomPanel::OpenStyleBrowser(AppState& app) {
    ListBrowser::Config cfg;
    cfg.title = "STYLES";
    cfg.categories = {"HOUSE", "TECHNO", "ELECTRO", "BREAKS", "HIP HOP", "MY STYLES"};
    cfg.tags = {"LAID BACK", "DRIVING", "DARK", "SWUNG", "SLOW", "FAST"};
    cfg.favFile = userdirs::FavoritesDir() + "/favorites_styles.txt";
    cfg.footer = "click = select   Enter / double-click = apply with the parts below (one Undo step)";
    cfg.liveSelect = false;
    cfg.detailH = 136.0f;
    cfg.detail = [this, &app](int id, float w) { DrawStyleDetail(app, id, w); };
    std::vector<ListBrowser::Item> items;
    const auto& styles = AllStyles();
    for (int i = 0; i < static_cast<int>(styles.size()); ++i) {
        const StyleDef& sd = styles[static_cast<size_t>(i)];
        ListBrowser::Item it; it.id = i; it.name = sd.name; it.category = sd.category; it.favKey = sd.name;
        it.tags.push_back(std::to_string(sd.bpm));
        const int gi = FindGenrePatternIndex(sd.pattern);
        const int bars = gi >= 0 ? std::max(1, AllGenrePatterns()[static_cast<size_t>(gi)].bars) : 1;
        it.tags.push_back(std::to_string(bars) + (bars == 1 ? " BAR" : " BARS"));
        std::string t = sd.tags;
        for (size_t pos = 0; !t.empty();) { pos = t.find('|'); it.tags.push_back(t.substr(0, pos)); if (pos == std::string::npos) break; t = t.substr(pos + 1); }
        items.push_back(std::move(it));
    }
    styleBrowser_.Open(cfg, std::move(items), app.styleIndex);
}

// The strip under the style cards: the four part switches and, next to each, what APPLY writes for the selected style.
void BottomPanel::DrawStyleDetail(AppState& app, int styleId, float width) {
    const auto& styles = AllStyles();
    if (styleId < 0 || styleId >= static_cast<int>(styles.size())) { ImGui::TextDisabled("Pick a style."); return; }
    const StyleDef& sd = styles[static_cast<size_t>(styleId)];
    const int gi = FindGenrePatternIndex(sd.pattern);
    const GenrePattern* gp = gi >= 0 ? &AllGenrePatterns()[static_cast<size_t>(gi)] : nullptr;
    const int bars = gp ? std::max(1, gp->bars) : 1;
    const int pi = FindProgressionIndex(sd.progression);
    ImGui::TextColored(kAmberV, "%s", sd.name);
    ImGui::SameLine(0, 8);
    ImGui::TextDisabled("WHAT APPLY DOES  -  %d BPM, %d %s (%d steps).  Parts switched off are not touched.", sd.bpm, bars, bars == 1 ? "bar" : "bars", bars * 16);
    struct Row { const char* chip; bool* flag; const char* tip; std::string text; };
    char b1[200], b2[200], b3[240], b4[100];
    std::snprintf(b1, sizeof b1, "%s beat%s%s, kit %s%s%s", sd.pattern, gp ? (", swing " + std::to_string(static_cast<int>(gp->swing * 100.0f + 0.5f)) + "%").c_str() : "", "",
                  sd.kit[0] == 'k' ? sd.kit + 5 : sd.kit, sd.groove[0] ? ", groove " : "", sd.groove);
    std::snprintf(b2, sizeof b2, "bass %s, synth 2 %s, synth 3 %s, chord %s", sd.bass, sd.syn2, sd.syn3, sd.chord);
    std::snprintf(b3, sizeof b3, "chords %s, one per %d beat%s; bass line of %d notes per bar; in your KEY and SCALE (%s %s now), the key is never changed", pi >= 0 ? AllChordProgressions()[static_cast<size_t>(pi)].name : "-",
                  sd.chordBeats, sd.chordBeats == 1 ? "" : "s", static_cast<int>(sd.bassLine.size()), kKeyNames[std::clamp(app.project.randomizerKeyRoot, 0, kNumKeys - 1)], kScaleNames[std::clamp(app.project.randomizerScale, 0, kNumScales - 1)]);
    std::snprintf(b4, sizeof b4, "%d BPM (this scene: %d)", sd.bpm, static_cast<int>(app.CurrentPattern().bpm + 0.5f));
    Row rows[4] = {
        {"BEAT", &app.styleBeat, "BEAT: the drum rhythm with its swing, the groove (baked into the steps), the kit and the scene length (it grows to the typical length, never shrinks).", b1},
        {"SOUND", &app.styleSound, "SOUND: the presets of bass, synth 2, synth 3 and chord.", b2},
        {"NOTES", &app.styleNotes, "NOTES: a bass line and a chord progression, written in the key and scale you have set.", b3},
        {"TEMPO", &app.styleTempo, "TEMPO: the BPM of the style. Off by default, so a style never changes the tempo of a set by surprise.", b4},
    };
    for (int i = 0; i < 4; ++i) {
        ImGui::PushID(i);
        if (ToggleButton(rows[i].chip, *rows[i].flag, ImVec2(64, 20), ToggleTab())) *rows[i].flag = !*rows[i].flag;
        if (ImGui::IsItemHovered()) Hint("%s", rows[i].tip);
        ImGui::SameLine(0, 10);
        ImGui::AlignTextToFramePadding();
        if (*rows[i].flag) ImGui::TextUnformatted(rows[i].text.c_str()); else ImGui::TextDisabled("%s", rows[i].text.c_str());
        ImGui::PopID();
    }
    (void)width;
}

void BottomPanel::ApplyBeat(AppState& app, int idx) {
    const auto& customNames = app.CustomPatternNamesCached();
    if (idx <= 0) {
        std::uniform_int_distribution<int> pick(0, kNumGenrePatterns - 1);
        app.ApplyGenrePattern(pick(AppState::RngEngine()));
    } else if (idx - 1 < kNumGenrePatterns) {
        app.ApplyGenrePattern(idx - 1);
    } else {
        const int ci = idx - 1 - kNumGenrePatterns;
        if (ci >= 0 && ci < static_cast<int>(customNames.size())) app.ApplyCustomPattern(customNames[static_cast<size_t>(ci)]);
    }
}

// ===========================================================================
// RANDOM / KIT
// ===========================================================================

void BottomPanel::DrawRandomKit(AppState& app) {
    ImGui::BeginGroup();
    ImGui::TextColored(kAmberV, "RANDOM / KIT");
    const float btnH = 24.0f;

    // ---- column A: style, kit and typical beat (each a name button that opens a big browser) ----
    ImGui::BeginGroup();
    ImVec2 styleChipsEnd(0, 0);
    {   // v44.49: STYLE = beat + kit + groove + sound + bass line + chords (+ tempo) of one genre, as ONE undo step; B S N T say which parts
        const auto& styles = AllStyles();
        const int ns = static_cast<int>(styles.size());
        app.styleIndex = std::clamp(app.styleIndex, 0, ns - 1);
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("STYLE");
        ImGui::SameLine(x0 + 40.0f);
        if (ImGui::Button("<##stylePrev", ImVec2(24, btnH - 2))) app.styleIndex = (app.styleIndex - 1 + ns) % ns;
        ImGui::SameLine(0, 3);
        if (NameButton("##styleOpen", 120.0f, styles[static_cast<size_t>(app.styleIndex)].name, ImU32(vis::kAccent))) OpenStyleBrowser(app);
        if (ImGui::IsItemHovered()) Hint("Open the style browser: a whole style (beat, kit, groove, sound, bass line, chords) in one place. Nothing changes until APPLY.");
        ImGui::SameLine(0, 3);
        if (ImGui::Button(">##styleNext", ImVec2(24, btnH - 2))) app.styleIndex = (app.styleIndex + 1) % ns;
        ImGui::SameLine(0, 6);
        if (ImGui::Button("APPLY##style", ImVec2(50, btnH - 2))) app.ApplyStyle(app.styleIndex);
        if (ImGui::IsItemHovered()) Hint("Write the style into this scene with the parts that are lit (B S N T). One Undo step brings everything back.");
        ImGui::SetCursorPosX(x0 + 40.0f);
        struct Chip { const char* label; bool* flag; const char* tip; };
        const Chip chips[4] = {
            {"B", &app.styleBeat, "BEAT: drums, swing, groove, kit and the scene length of the style"},
            {"S", &app.styleSound, "SOUND: presets of bass, synth 2, synth 3 and chord"},
            {"N", &app.styleNotes, "NOTES: bass line and chords, in your KEY and SCALE (the key is never changed)"},
            {"T", &app.styleTempo, "TEMPO: the BPM of the style (off by default)"},
        };
        for (int i = 0; i < 4; ++i) {
            if (i > 0) ImGui::SameLine(0, 3);
            ImGui::PushID(i);
            if (ToggleButton(chips[i].label, *chips[i].flag, ImVec2(36, btnH - 6), ToggleTab())) *chips[i].flag = !*chips[i].flag;
            if (ImGui::IsItemHovered()) Hint("%s", chips[i].tip);
            ImGui::PopID();
            styleChipsEnd = ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMin().y);
        }
    }
    {
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("KIT");
        ImGui::SameLine(x0 + 40.0f);
        const auto& kits = app.KitFolders();
        if (kits.empty()) {
            ImGui::TextDisabled("(no kit folders)");
        } else {
            const int n = static_cast<int>(kits.size());
            // the picker follows the kit that is actually loaded (new project, opened project)
            for (int i = 0; i < n; ++i) if (kits[static_cast<size_t>(i)] == app.SceneKitFolder(app.project.currentPattern)) { app.menuKitIndex = i; break; }
            const int cur = std::clamp(app.menuKitIndex, 0, n - 1);
            std::string name = kits[static_cast<size_t>(cur)];
            if (name.rfind("kits/", 0) == 0) name = name.substr(5);
            const KitInfo* ki = FindKitInfo(name);
            if (ImGui::Button("<##kitPrev", ImVec2(24, btnH - 2))) LoadKit(app, kits[static_cast<size_t>((cur - 1 + n) % n)]);
            ImGui::SameLine(0, 3);
            if (NameButton("##kitOpen", 120.0f, name, ki ? ImU32(vis::kAccent) : ImU32(vis::kTextDim))) OpenKitBrowser(app);
            if (ImGui::IsItemHovered()) Hint("Open the kit browser (categories, search, favorites; click a kit to load it and hear it)");
            ImGui::SameLine(0, 3);
            if (ImGui::Button(">##kitNext", ImVec2(24, btnH - 2))) LoadKit(app, kits[static_cast<size_t>((cur + 1) % n)]);
            ImGui::SameLine(0, 3);
            if (ImGui::Button("=ALL##kitScenes", ImVec2(40, btnH - 2))) app.SetKitAllScenes(app.SceneKitFolder(app.project.currentPattern));
            if (ImGui::IsItemHovered()) Hint("The kit belongs to the scene you are editing.\n=ALL: use this scene's kit in every scene.");
        }
    }
    {
        const auto& customNames = app.CustomPatternNamesCached();
        const int patCount = 1 + kNumGenrePatterns + static_cast<int>(customNames.size());
        if (app.rndGenreIndex >= patCount) app.rndGenreIndex = patCount - 1;
        std::string label = app.rndGenreIndex <= 0 ? "RANDOM"
            : (app.rndGenreIndex - 1 < kNumGenrePatterns ? AllGenrePatterns()[static_cast<size_t>(app.rndGenreIndex - 1)].name
                                                           : customNames[static_cast<size_t>(app.rndGenreIndex - 1 - kNumGenrePatterns)] + " (SAVED)");
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("BEAT");
        ImGui::SameLine(x0 + 40.0f);
        if (ImGui::Button("<##beatPrev", ImVec2(24, btnH - 2))) app.rndGenreIndex = (app.rndGenreIndex - 1 + patCount) % patCount;
        ImGui::SameLine(0, 3);
        if (NameButton("##beatOpen", 120.0f, label, app.rndGenreIndex <= 0 ? ImU32(vis::kTextDim) : ImU32(vis::kTeal))) OpenBeatBrowser(app);
        if (ImGui::IsItemHovered()) Hint("Open the beat browser (categories, search, favorites; Enter or double-click applies)");
        ImGui::SameLine(0, 3);
        if (ImGui::Button(">##beatNext", ImVec2(24, btnH - 2))) app.rndGenreIndex = (app.rndGenreIndex + 1) % patCount;
        ImGui::SameLine(0, 6);
        if (ImGui::Button("APPLY", ImVec2(50, btnH - 2))) ApplyBeat(app, app.rndGenreIndex);
        if (ImGui::IsItemHovered()) Hint("Replace the drum tracks with the browsed typical beat");
        ImGui::SameLine(0, 4);
        static char patName[48] = "";
        if (ImGui::Button("SAVE", ImVec2(44, btnH - 2))) { patName[0] = '\0'; ImGui::OpenPopup("BeatSaveName"); }
        if (ImGui::IsItemHovered()) Hint("Save the current drum beat under a name (it joins the BEAT list)");
        if (NamePrompt("BeatSaveName", patName, sizeof(patName), "BEAT NAME")) app.SaveCurrentAsCustomPattern(patName);
    }
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.16f, 0.16f, 1.0f));
    ImGui::SetCursorScreenPos(ImVec2(styleChipsEnd.x + 8.0f, styleChipsEnd.y));   // v44.49: beside the B S N T chips (the panel has no room for a row of its own)
    if (ImGui::Button("CLEAR ALL", ImVec2(104, btnH - 6))) { app.ClearPattern(); app.SetStatus("SCENE CLEARED (ALL INSTRUMENTS)"); }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) Hint("Clear every track of this scene (Undo brings it back)");
    ImGui::EndGroup();

    ImGui::SameLine(0, 12);

    // ---- columns B..: one per part: RANDOM / SIMPLE-COMPLEX / CLEAR ----------
    // melodic parts get a fourth button, OCT: the octaves the randomizer may use (a set; opens a small window above the button)
    auto octLabel = [](unsigned mask) {
        int lo = -1, hi = -1, n = 0;
        for (int o = 0; o <= kMaxRndOctave; ++o) if ((mask >> o) & 1u) { if (lo < 0) lo = o; hi = o; ++n; }
        char b[24];
        if (n == 0) std::snprintf(b, sizeof b, "OCT -");
        else if (n == hi - lo + 1) { if (lo == hi) std::snprintf(b, sizeof b, "OCT %d", lo); else std::snprintf(b, sizeof b, "OCT %d-%d", lo, hi); }
        else std::snprintf(b, sizeof b, "OCT %d+%d..", lo, n - 1);   // a gap: first octave and how many more
        return std::string(b);
    };
    auto NoteName = [](int n) { return std::string(kKeyNames[((n % 12) + 12) % 12]) + std::to_string(n / 12 - 1); };
    auto column = [&](const char* id, const char* title, int melodicIndex, bool* complexFlag,
                      auto&& onRandom, auto&& onClear) {
        ImGui::PushID(id);
        ImGui::BeginGroup();
        if (ImGui::Button(title, ImVec2(68, btnH))) onRandom();
        if (melodicIndex >= 0) {
            // v44.4: the density button cycles SIMPLE -> COMPLEX -> RHYTHM (a figure with a chosen number of notes per bar; set up under OCT)
            int& mode = app.rndDensityMode[melodicIndex];
            mode = std::clamp(mode, 0, 2);
            static const char* kModeLabel[3] = {"SIMPLE", "COMPLEX", "RHYTHM"};
            const bool rhythm = mode == 2;   // TEST-5: the click below changes `mode`; push and pop must follow the same value
            if (rhythm) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.36f, 0.10f, 1.0f));
            if (ImGui::Button(kModeLabel[mode], ImVec2(68, btnH - 2))) mode = (mode + 1) % 3;
            if (rhythm) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) Hint("What RANDOM writes on this part:\nSIMPLE / COMPLEX = a melody (fewer / more notes)\nRHYTHM = a rhythmic figure with a chosen number of notes per bar, even one (a bass line).\nSet it up in the OCT window. Click to switch.");
        } else if (complexFlag != nullptr) {
            if (ImGui::Button(*complexFlag ? "COMPLEX" : "SIMPLE", ImVec2(68, btnH - 2))) *complexFlag = !*complexFlag;
        } else if (std::string(id) == "drums") {
            // melody randomizers: follow the chords (chord tones on strong beats) or just the key
            if (ToggleButton(app.rndChordAware ? "CHORDS" : "KEY", app.rndChordAware, ImVec2(68, btnH - 2), ToggleTeal())) app.rndChordAware = !app.rndChordAware;
            if (ImGui::IsItemHovered()) Hint("Melody randomizers (BASS / SYNTH 2 / SYNTH 3):\nCHORDS = chord tones of the sounding chord on the strong beats (bass: the root)\nKEY = any note of the key / scale");
        } else {
            ImGui::Dummy(ImVec2(68, btnH - 2));
        }
        if (melodicIndex >= 0) {
            uint8_t& mask = app.project.rndOctaves[static_cast<size_t>(melodicIndex)];
            const std::string lab = octLabel(mask) + "##oct";
            if (ImGui::Button(lab.c_str(), ImVec2(68, btnH - 2))) ImGui::OpenPopup("OctPopup");
            const ImVec2 bmin = ImGui::GetItemRectMin();
            if (ImGui::IsItemHovered()) Hint("Octaves this part may use when you press RANDOM.\nClick to choose (several octaves, with gaps if you like).");
            ImGui::SetNextWindowPos(ImVec2(bmin.x, bmin.y - 6.0f), ImGuiCond_Appearing, ImVec2(0.0f, 1.0f));
            if (ImGui::BeginPopup("OctPopup")) {
                static const char* kPartName[3] = {"BASS", "SYNTH 2", "SYNTH 3"};
                ImGui::TextColored(kAmberV, "%s  -  RANDOM OCTAVES", kPartName[melodicIndex]);
                const ImVec2 cell(40.0f, 46.0f);
                for (int o = 0; o <= kMaxRndOctave; ++o) {
                    if (o > 0) ImGui::SameLine(0, 4);
                    ImGui::PushID(o);
                    const bool on = (mask >> o) & 1u;
                    char b[16]; std::snprintf(b, sizeof b, "%d\nC%d", o, o);
                    if (ToggleButton(b, on, cell, ToggleTab())) {
                        const uint8_t nm = static_cast<uint8_t>(mask ^ (1u << o));
                        if (nm != 0) mask = nm;   // at least one octave stays on
                    }
                    if (ImGui::IsItemHovered()) Hint("Octave %d: C%d - B%d (MIDI %d - %d)", o, o, o, 12 * o + 12, 12 * o + 23);
                    ImGui::PopID();
                }
                static const struct { const char* label; int lo, hi; } kPre[] = {{"LOW 1-2", 1, 2}, {"MID 3-4", 3, 4}, {"HIGH 4-6", 4, 6}, {"WIDE 2-5", 2, 5}};
                for (int k = 0; k < 4; ++k) {
                    if (k > 0) ImGui::SameLine(0, 4);
                    if (ImGui::Button(kPre[k].label, ImVec2(74.0f, 0))) mask = static_cast<uint8_t>(OctaveMaskRange(kPre[k].lo, kPre[k].hi));
                }
                const std::vector<int> notes = BuildScaleNotesMask(app.project.randomizerKeyRoot, app.project.randomizerScale, mask);
                if (!notes.empty())
                    ImGui::TextDisabled("%d notes of %s %s, from %s to %s", static_cast<int>(notes.size()), kKeyNames[std::clamp(app.project.randomizerKeyRoot, 0, kNumKeys - 1)],
                                        kScaleNames[std::clamp(app.project.randomizerScale, 0, kNumScales - 1)], NoteName(notes.front()).c_str(), NoteName(notes.back()).c_str());
                ImGui::TextDisabled("Used by the next RANDOM press. Saved with the project.");
                // ---- RHYTHM settings (v44.4): used when the mode button says RHYTHM -------------------------------------
                ImGui::Separator();
                ImGui::TextColored(kAmberV, "RHYTHM  -  notes per bar, pitch, length");
                RhythmRnd& rh = app.project.rndRhythm[static_cast<size_t>(melodicIndex)];
                int mn = rh.minNotes, mx = rh.maxNotes;
                ImGui::TextUnformatted("NOTES PER BAR"); ImGui::SameLine(0, 8);
                {
                    float f = static_cast<float>(mn);
                    ValueSpec sp; sp.lo = 1.0f; sp.hi = static_cast<float>(kRhythmMaxNotes); sp.step = 0.1f; sp.wheel = 1.0f; sp.def = 1.0f; sp.integer = true; sp.fmt = "min %.0f";
                    if (ValueField("##rmin", &f, sp, 62).changed) { rh.minNotes = static_cast<uint8_t>(f); rh = NormalizeRhythm(rh); if (rh.maxNotes < rh.minNotes) rh.maxNotes = rh.minNotes; }
                    ImGui::SameLine(0, 4);
                    f = static_cast<float>(mx); sp.fmt = "max %.0f";
                    if (ValueField("##rmax", &f, sp, 62).changed) { rh.maxNotes = static_cast<uint8_t>(f); rh = NormalizeRhythm(rh); }
                }
                if (ImGui::IsItemHovered()) Hint("Every bar gets a number of notes between min and max (1 = a single note per bar).");
                ImGui::TextUnformatted("PITCH        "); ImGui::SameLine(0, 8);
                for (int k = 0; k < kRhythmPitchCount; ++k) {
                    if (k > 0) ImGui::SameLine(0, 4);
                    ImGui::PushID(100 + k);
                    if (ToggleButton(RhythmPitchName(static_cast<RhythmPitch>(k)), static_cast<int>(rh.pitch) == k, ImVec2(70, 0), ToggleTab())) rh.pitch = static_cast<RhythmPitch>(k);
                    ImGui::PopID();
                }
                if (ImGui::IsItemHovered()) Hint("ROOT = the key's root (mostly its lowest octave)\nROOT+5TH = root, now and then the fifth\nCHORD = the sounding chord (bass: its root), the key root where there is no chord\nSCALE = any note of the key / scale, a small walk");
                ImGui::TextUnformatted("LENGTH       "); ImGui::SameLine(0, 8);
                for (int k = 0; k < kRhythmLenCount; ++k) {
                    if (k > 0) ImGui::SameLine(0, 4);
                    ImGui::PushID(200 + k);
                    if (ToggleButton(RhythmLenName(static_cast<RhythmLen>(k)), static_cast<int>(rh.length) == k, ImVec2(70, 0), ToggleTab())) rh.length = static_cast<RhythmLen>(k);
                    ImGui::PopID();
                }
                if (ImGui::IsItemHovered()) Hint("SHORT = one step, HELD = until the next note, MIX = anything in between");
                if (ToggleButton("FIRST ON THE ONE", rh.onTheOne, ImVec2(140, 0), ToggleTab())) rh.onTheOne = !rh.onTheOne;
                ImGui::SameLine(0, 4);
                if (ToggleButton("SAME EVERY BAR", rh.sameEveryBar, ImVec2(130, 0), ToggleTab())) rh.sameEveryBar = !rh.sameEveryBar;
                if (ImGui::IsItemHovered()) Hint("ON: one bar is drawn and repeated through the scene.\nOFF: every bar gets a figure of its own.");
                ImGui::SameLine(0, 10);
                if (ImGui::Button("ROLL", ImVec2(60, 0))) { app.rndDensityMode[melodicIndex] = 2; app.RandomizeRhythm(melodicIndex); }
                if (ImGui::IsItemHovered()) Hint("Switch this part to RHYTHM and write a figure now");
                ImGui::EndPopup();
            }
        } else {
            ImGui::Dummy(ImVec2(68, btnH - 2));
        }
        if (ImGui::Button("CLEAR", ImVec2(68, btnH - 2))) onClear();
        ImGui::EndGroup();
        ImGui::PopID();
    };
    column("drums", "DRUMS", -1, nullptr,
           [&] { app.RandomizeDrums(); app.SetStatus("RANDOMIZED DRUMS"); },
           [&] { app.ClearAllDrums(); app.SetStatus("ALL DRUMS CLEARED"); });
    static const char* kMel[3] = {"BASS", "SYNTH 2", "SYNTH 3"};
    for (int i = 0; i < 3; ++i) {
        ImGui::SameLine(0, 6);
        column(kMel[i], kMel[i], i, nullptr,
               [&, i] { app.RandomizeMelodicMode(i, app.rndDensityMode[i]); if (app.rndDensityMode[i] < 2) app.SetStatus(std::string("RANDOMIZED ") + kMel[i]); },
               [&, i] { app.ClearSynth(i); app.SetStatus(std::string(kMel[i]) + " CLEARED"); });
    }
    ImGui::SameLine(0, 6);
    {   // the chord column has no octave set (its chords are built around octave 3)
        bool chdCx = app.chdComplex;
        column("chord", "CHORD", -1, &chdCx,
               [&] { app.RandomizeChordProgression(); },
               [&] { app.ClearChordTrack(); app.SetStatus("CHORD TRACK CLEARED"); });
        app.chdComplex = chdCx;
    }
    ImGui::EndGroup();
}

// ===========================================================================
// LIVE
// ===========================================================================

// The performance mute pads: a 4 x 2 grid right next to the filter pad. Red = muted. DROP (amber) mutes everything except the
// kick and the bass and brings the rest back on the second press.
// v44.48: the instrument switches of LIVE. Ten bars (the same icons and colours as WHO PLAYS and the sequencer): click = mute, the S under
// a bar = solo; the group buttons (ALL DRUMS, DRUMS, HATS, DROP, MUTE ALL, ALL ON) stand to the right of them. All of it goes through
// AppState::Sw*(-1, ...), the flags of the edited scene: the same ones the M and S of the sequencer write.
void BottomPanel::DrawMutePads(AppState& app) {
    using namespace instrument_switches;
    constexpr int brick = -1;
    constexpr float bh = 68.0f;   // pad 84 = bars 68 + 3 + the S row 13
    // v44.58: while a scene launch waits for its boundary, these switches mean the scene that is COMING: a kick muted now is muted from its
    // first beat, and the scene that plays is not touched (see AppState::ArmedScene)
    AppState::ArmScope arm(app, app.ArmedScene());
    ImGui::BeginGroup();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int scene = app.project.currentPattern;
    const bool anySolo = app.SwAnySolo(brick);
    for (int t = 0; t < kNumTracks; ++t) {
        const ImVec2 p0(o.x + t * (kBarW + kBarGap), o.y), p1(p0.x + kBarW, p0.y + bh);
        const bool has = app.TrackHasNotes(scene, t), muted = app.SwMuted(brick, t), solo = app.SwSolo(brick, t);
        const bool silenced = anySolo && !solo;
        ImGui::PushID(t);
        ImGui::SetCursorScreenPos(p0);
        if (ImGui::InvisibleButton("##bar", ImVec2(kBarW, bh))) app.LiveSwitch(AppState::QueuedSwitch::Mute, static_cast<uint16_t>(t));   // v44.54: NEXT BAR may queue it
        const bool hov = ImGui::IsItemHovered();
        if (app.SwQueued(AppState::QueuedSwitch::Mute, static_cast<uint16_t>(t))) PendingOutline(p0, p1);
        DrawBar(dl, t, p0, p1, has, muted, silenced);
        if (hov) dl->AddRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), vis::kLineHi, 3.0f);
        if (hov) Hint("%s: %s.  Click = mute / unmute.  S under the bar = solo.", InstrumentName(t), StateWord(has, muted, silenced, solo));
        if (SoloCell("##solo", ImVec2(p0.x, p1.y + 3.0f), solo, !has)) app.LiveSwitch(AppState::QueuedSwitch::Solo, static_cast<uint16_t>(t));
        if (app.SwQueued(AppState::QueuedSwitch::Solo, static_cast<uint16_t>(t))) PendingOutline(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (ImGui::IsItemHovered()) Hint("%s solo: only this instrument plays. Press again to bring the others back.", InstrumentName(t));
        ImGui::PopID();
    }
    DrawGroups(app, brick, ImVec2(o.x + StripWidth() + 10.0f, o.y), 62.0f, 26.0f, 3.0f);
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(StripWidth() + 10.0f + 2 * 62.0f + 3.0f, bh + 3.0f + kSoloH));
    ImGui::EndGroup();
}

void BottomPanel::DrawLive(AppState& app) {
    auto& mixer = app.engine.GetMixer();
    ImGui::BeginGroup();
    ImGui::TextColored(kAmberV, "LIVE");
    if (const int armed = app.ArmedScene(); armed >= 0) {   // v44.58: the switches below are for the scene that is coming
        ImGui::SameLine(0, 10);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.45f, 1.0f), "NEXT: SCENE %02d", armed + 1);
        if (ImGui::IsItemHovered()) Hint("A scene is waiting for its launch: mute, solo and the group buttons below set THAT scene, and sound from its first beat. The scene that plays now is not touched.");
        ImGui::SameLine(0, 10);
    }
    // v44.54: NEXT BAR - lit: a click on a bar, an S or a group button waits and takes effect on the next bar line (like a FILL);
    // off: it acts at once. A waiting button pulses; a second click on it cancels it.
    ImGui::SameLine(0, 14);
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 0.0f));
        if (ToggleButton("NEXT BAR", app.quantizeSwitches, ImVec2(74, 16))) {
            app.quantizeSwitches = !app.quantizeSwitches;
            app.SetStatus(app.quantizeSwitches ? "NEXT BAR ON: MUTE, SOLO AND GROUPS WAIT FOR THE NEXT BAR" : "NEXT BAR OFF: MUTE, SOLO AND GROUPS ACT AT ONCE");
        }
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered()) Hint("NEXT BAR: mute, solo and the group buttons wait for the next bar line (like a fill) instead of acting at once.\nA waiting button pulses; click it again to cancel. Off = real time.  (STUTTER and scenes keep their own timing.)");
    }

    ImGui::BeginGroup(); // XY pad
    const float kPad = 84.0f;   // v44.48: 92 -> 84 so the instrument bars and their group buttons fit next to it
    ImGui::InvisibleButton("xy", ImVec2(kPad, kPad));
    bool dragging = ImGui::IsItemActive();
    ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, vis::kInset, 8.0f);
    dl->AddRect(p0, p1, vis::kLineHi, 8.0f);
    ImVec2 mid((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    dl->AddLine(ImVec2(mid.x, p0.y), ImVec2(mid.x, p1.y), vis::kCellHover, 1.0f);
    dl->AddLine(ImVec2(p0.x, mid.y), ImVec2(p1.x, mid.y), vis::kCellHover, 1.0f);
    if (dragging) {
        ImVec2 m = ImGui::GetIO().MousePos;
        app.SetMasterHpfAmount(std::clamp((m.x - p0.x) / kPad, 0.0f, 1.0f));
        app.SetMasterLpfAmount(std::clamp((m.y - p0.y) / kPad, 0.0f, 1.0f));
    }
    const float inset = 9.0f; // keep the dot (and its ring) inside the pad even at the corners
    ImVec2 dot(p0.x + inset + mixer.masterHpfAmount * (kPad - 2 * inset), p0.y + inset + mixer.masterLpfAmount * (kPad - 2 * inset));
    dl->AddCircleFilled(dot, 6.0f, vis::kAccent);
    dl->AddCircle(dot, 10.0f, vis::kAccentDim, 0, 2.0f);
    if (dragging || ImGui::IsItemHovered()) Hint("FILTER XY  HPF %.0f%%  LPF %.0f%%", mixer.masterHpfAmount * 100.0f, mixer.masterLpfAmount * 100.0f);
    ImGui::EndGroup();

    ImGui::SameLine(0, 10);
    DrawMutePads(app);

    ImGui::SameLine(0, 10);
    ImGui::BeginGroup();
    const float btnH = 24.0f;

    // row 1: STUTTER | DENS   (v44.48: MUTE ALL moved to the group buttons next to the instrument bars)
    if (ToggleButton("STUTTER", mixer.repeaterWantOn, ImVec2(70, btnH))) app.ToggleRepeater();
    if (mixer.repeaterWantOn != mixer.repeaterOn) {   // armed: waits for the next step / beat on the grid
        float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 10.0f);
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(255, 255, 255, static_cast<int>(90 + 140 * pulse)), 4.0f, 0, 2.0f);
    }
    if (ImGui::IsItemHovered()) Hint("Beat repeat of the master output.\nOn / off lands on the next step (next beat for windows of 4+ steps) so the loop stays on the grid.");
    ImGui::SameLine(0, 4);
    {
        // Density = how long a slice of the master the stutter loops, in steps
        // (1/16 ... 1 step = glitch, 2 / 4 / 6 / 8 steps = beat-repeat).
        char dens[24];
        std::snprintf(dens, sizeof(dens), "DENS %s", kRepeaterDensities[std::clamp(mixer.repeaterWantDensity, 0, kNumRepeaterDensities - 1)].label);
        ImGui::SetNextItemWidth(84);
        if (ChoiceBegin("##dens", dens, ImGuiComboFlags_HeightLargest)) {
            for (int k = 0; k < kNumRepeaterDensities; ++k) {
                int idx = kRepeaterDensityDisplayOrder[k];
                char item[32];
                std::snprintf(item, sizeof(item), "%s%s", kRepeaterDensities[idx].label, idx >= 5 ? "  steps" : (idx == 0 ? "  step" : "  step"));
                if (ImGui::Selectable(item, mixer.repeaterWantDensity == idx)) {
                    mixer.repeaterWantDensity = idx;
                    app.SetStatus(std::string("STUTTER DENSITY: ") + kRepeaterDensities[idx].label);
                }
            }
            ChoiceEnd();
        }
        if (ImGui::IsItemHovered()) Hint("Stutter density: length of the looped slice in steps\n(fractions of a step = glitchy, 2 / 4 / 6 / 8 = beat repeat)");
    }

    // row 2: FILL picker + trigger
    const auto& breaks = AllDrumBreaks();
    {
        ImGui::SetNextItemWidth(92);
        std::string cur = app.rndBreakIndex <= 0 ? "RANDOM" : breaks[std::min<int>(app.rndBreakIndex - 1, breaks.size() - 1)].name;
        if (ChoiceBegin("##fill", cur.c_str())) {
            if (ImGui::Selectable("RANDOM", app.rndBreakIndex <= 0)) app.rndBreakIndex = 0;
            for (int i = 0; i < kNumDrumBreaks; ++i) {
                if (ImGui::Selectable(breaks[i].name, app.rndBreakIndex == i + 1)) app.rndBreakIndex = i + 1;
            }
            ChoiceEnd();
        }
        ImGui::SameLine(0, 4);
        bool pending = app.sequencer.IsLiveBreakPending();
        bool active = app.sequencer.IsLiveBreakActive();
        const char* lbl = pending ? "QUEUED" : active ? "PLAYING" : "FILL";
        if (ToggleButton(lbl, pending || active, ImVec2(62, btnH - 2), ToggleTeal())) {
            if (pending) app.DeactivateLiveBreak(); else app.ArmLiveBreak();
        }
        if (ImGui::IsItemHovered()) Hint(pending ? "Queued for the next bar - click to cancel"
                                                              : "Queue the selected one-bar drum break at the next bar");
    }
    // row 3: RISER picker + trigger
    {
        int n = app.engine.Risers().Count();
        ImGui::SetNextItemWidth(92);
        if (app.rndRiserIndex > n) app.rndRiserIndex = n;
        std::string cur = app.rndRiserIndex <= 0 ? "RANDOM" : app.engine.Risers().NameAt(app.rndRiserIndex - 1);
        if (ChoiceBegin("##riser", cur.c_str())) {
            if (ImGui::Selectable("RANDOM", app.rndRiserIndex <= 0)) app.rndRiserIndex = 0;
            for (int i = 0; i < n; ++i) {
                if (ImGui::Selectable(app.engine.Risers().NameAt(i).c_str(), app.rndRiserIndex == i + 1)) app.rndRiserIndex = i + 1;
            }
            ChoiceEnd();
        }
        ImGui::SameLine(0, 4);
        {
            static const char* const kDropLbl[4] = {"1 BAR", "2 BARS", "4 BARS", "8 BARS"};   // v44.48: the word DROP is in the hint
            ImGui::SetNextItemWidth(66);
            if (ChoiceBegin("##riserdrop", kDropLbl[std::clamp(app.riserDropIdx, 0, 3)])) {
                for (int i = 0; i < 4; ++i) if (ImGui::Selectable(kDropLbl[i], i == app.riserDropIdx)) app.riserDropIdx = i;
                ChoiceEnd();
            }
            if (ImGui::IsItemHovered()) Hint("DROP: the riser's peak lands exactly on this bar line.\n1 BAR = the next bar line (at least half a second away).");
        }
        ImGui::SameLine(0, 4);
        bool pending = app.sequencer.IsRiserPending();
        bool active = app.engine.Risers().AnyActive();
        const char* lbl = pending ? "QUEUED" : active ? "PLAYING" : "RISER";
        if (ToggleButton(lbl, pending || active, ImVec2(62, btnH - 2), ToggleTeal())) {
            if (pending) app.CancelPendingRiser(); else app.TriggerRiser();
        }
        if (ImGui::IsItemHovered()) Hint(pending ? "Waiting for its start - click to cancel"
                                                              : "Build to the drop: the riser starts so that its peak lands on the chosen bar line");
    }
    ImGui::EndGroup();
    ImGui::EndGroup();
}

} // namespace t8::desktop
