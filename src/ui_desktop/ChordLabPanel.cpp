#include "ChordLabPanel.h"
#include "Visual.h"
#include "LiveWidgets.h"
#include "InfoLine.h"
#include "ValueField.h"
#include "core/ChordProgressions.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

namespace t8::desktop {

namespace {

constexpr ImU32 kMajFill = IM_COL32(30, 52, 44, 255), kMajEdge = IM_COL32(60, 130, 100, 255), kMajText = IM_COL32(159, 240, 200, 255);
constexpr ImU32 kMinFill = IM_COL32(44, 38, 72, 255), kMinEdge = IM_COL32(110, 90, 176, 255), kMinText = IM_COL32(205, 184, 255, 255);
constexpr ImU32 kOthFill = IM_COL32(38, 40, 48, 255), kOthEdge = IM_COL32(74, 78, 94, 255), kOthText = IM_COL32(195, 198, 211, 255);

ImU32 KindFill(int k) { return k == 0 ? kMajFill : (k == 1 ? kMinFill : kOthFill); }
ImU32 KindEdge(int k) { return k == 0 ? kMajEdge : (k == 1 ? kMinEdge : kOthEdge); }
ImU32 KindText(int k) { return k == 0 ? kMajText : (k == 1 ? kMinText : kOthText); }

void Text(ImDrawList* dl, ImVec2 p, ImU32 col, const char* s, float size) {
    ImGui::PushFont(nullptr, size);
    dl->AddText(p, col, s);
    ImGui::PopFont();
}
float TextW(const char* s, float size) { ImGui::PushFont(nullptr, size); const float w = ImGui::CalcTextSize(s).x; ImGui::PopFont(); return w; }

void Wrap(ImDrawList* dl, ImVec2 p, float maxW, ImU32 col, const std::string& text, float size, float lineH, int maxLines) {
    std::string line; int lines = 0; size_t i = 0;
    while (i < text.size() && lines < maxLines) {
        size_t j = text.find(' ', i); if (j == std::string::npos) j = text.size();
        const std::string word = text.substr(i, j - i);
        const std::string tryLine = line.empty() ? word : line + " " + word;
        if (!line.empty() && TextW(tryLine.c_str(), size) > maxW) { Text(dl, ImVec2(p.x, p.y + lines * lineH), col, line.c_str(), size); ++lines; line = word; }
        else line = tryLine;
        i = j + 1;
    }
    if (!line.empty() && lines < maxLines) Text(dl, ImVec2(p.x, p.y + lines * lineH), col, line.c_str(), size);
}

int PerBar(const Pattern& pat) { return std::max(1, 4 * pat.stepsPerBeat); }
int PerBeat(const Pattern& pat) { return std::max(1, pat.stepsPerBeat); }

}  // namespace

// ---- the jam ------------------------------------------------------------------------------------------------------------------------
int ChordLabPanel::SceneShown(const AppState& app) const {
    return app.project.currentPattern;   // v44.51: on TRACK the jam is over the scene of the brick that is open, on LIVE over the live scene
}

// The chords the panel shows and edits: the edited chord slot while the bank is on, the scene's own chord track while it is off
// (then nothing is written until BeginEdit starts a draft).
Track& ChordLabPanel::JamTrack(AppState& app) { return app.StepTrack(app.project.patterns[static_cast<size_t>(SceneShown(app))], kChordTrackIndex); }

// Every change goes through here first. Bank on: an ordinary undo step. Bank off: the draft starts (the scene's chords are copied into a
// free slot and the bank goes on) as ONE undo step together with the change that follows. v44.44: opening the panel changes nothing.
void ChordLabPanel::BeginEdit(AppState& app) {
    if (app.project.chordBankOn) app.PushUndo(); else app.StartChordDraft();
    jam_ = std::clamp(app.project.chordSceneEdit, 0, kNumChordScenes - 1);
}

int ChordLabPanel::DefaultLen(const AppState& app) const {
    const Pattern& pat = app.project.patterns[static_cast<size_t>(SceneShown(app))];
    const int bar = PerBar(pat);
    return defLen_ == 0 ? std::max(PerBeat(pat), bar / 2) : (defLen_ == 2 ? bar * 2 : bar);
}

std::vector<ChordLabPanel::Blk> ChordLabPanel::Blocks(const Track& tr) const {
    std::vector<Blk> v;
    for (int j = 0; j < kMaxSteps; ++j) {
        const Step& s = tr.steps[static_cast<size_t>(j)];
        if (s.active && s.chordSize > 0) v.push_back({j, std::max(1, static_cast<int>(s.lengthSteps))});
    }
    return v;
}

void ChordLabPanel::Rebuild(AppState& app) {
    const int key = app.project.randomizerKeyRoot, sc = app.project.randomizerScale;
    if (key == palKey_ && sc == palScale_ && nine_ == palNine_ && !pal_.empty()) return;
    pal_ = BuildLabPalette(key, sc, nine_);
    palKey_ = key; palScale_ = sc; palNine_ = nine_;
}

std::vector<int> ChordLabPanel::SrcTracks() const {
    std::vector<int> tracks;
    for (int i = 0; i < kNumSynthTracks; ++i) if (srcMask_ & (1 << i)) tracks.push_back(kNumDrumTracks + i);
    return tracks;
}

// MATCH on: the first read, with a message. From then on RefreshMatch runs every frame.
void ChordLabPanel::Match(AppState& app) {
    RefreshMatch(app, -1, 0);
    app.SetStatus(mel_.notes > 0 ? "CHORD LAB: " + std::to_string(mel_.notes) + " NOTES READ, THE CHORDS ARE MARKED BY HOW WELL THEY FIT"
                                 : "CHORD LAB: NO BASS OR SYNTH NOTES TO MATCH YET");
}

// v44.46: with a block selected the palette is scored against the notes under THAT block (what could stand here instead); with none,
// against the bars chosen next to MATCH. Cheap (a few hundred steps), so it follows every edit of the notes.
void ChordLabPanel::RefreshMatch(AppState& app, int blockStart, int blockLen) {
    const Pattern& pat = app.project.patterns[static_cast<size_t>(SceneShown(app))];
    const std::vector<int> tracks = SrcTracks();
    if (blockStart >= 0) {
        mel_ = LabMelodyFromRange(pat, tracks, blockStart, blockStart + std::max(1, blockLen));
        scopeBlock_ = true;
    } else {
        const int bars = std::max(1, (std::max(1, pat.masterLength) + PerBar(pat) - 1) / PerBar(pat));
        const int to = barsMode_ == 0 ? 0 : (barsMode_ == 1 ? std::min(1, bars - 1) : bars - 1);
        mel_ = LabMelodyFromPattern(pat, tracks, 0, to);
        scopeBlock_ = false;
    }
    ScoreLabChords(pal_, mel_);
}

const LabChord* ChordLabPanel::Selected() const {
    for (const LabChord& c : pal_) if (c.row == selRow_ && c.col == selCol_) return &c;
    return nullptr;
}

void ChordLabPanel::Audition(AppState& app, const LabChord& c) {
    const std::vector<int> n = c.Notes(48);
    int8_t notes[kMaxChordVoices];
    const int cnt = std::min<int>(kMaxChordVoices, static_cast<int>(n.size()));
    for (int i = 0; i < cnt; ++i) notes[i] = static_cast<int8_t>(n[static_cast<size_t>(i)]);
    app.engine.ChordAuditionOn(notes, cnt, 0.85f);   // its own voices: the loop's chord keeps sounding (v44.44)
    auditionUntil_ = ImGui::GetTime() + 0.9;
}

// Takes the room [start, end) away from the blocks that are in it: a block that starts inside goes, one that reaches into it is shortened.
void ChordLabPanel::Carve(Track& tr, int start, int end) {
    for (const Blk& b : Blocks(tr)) {
        const int bEnd = b.start + b.len;
        if (b.start >= start && b.start < end) tr.steps[static_cast<size_t>(b.start)] = Step{};
        else if (b.start < start && bEnd > start) tr.steps[static_cast<size_t>(b.start)].lengthSteps = static_cast<uint8_t>(start - b.start);
    }
}

static void FitLoop(Track& tr, int perBar) {
    int end = 0;
    for (int j = 0; j < kMaxSteps; ++j) { const Step& s = tr.steps[static_cast<size_t>(j)]; if (s.active && s.chordSize > 0) end = std::max(end, j + std::max(1, static_cast<int>(s.lengthSteps))); }
    int len = std::max(perBar, ((end + perBar - 1) / perBar) * perBar);
    tr.useOwnLength = true; tr.length = std::min(len, kMaxSteps);
}

void ChordLabPanel::Place(AppState& app, const LabChord& c, int start, int len) {
    SceneLock lock(app.sequencer.PatternMutex(), "CHORD LAB");
    const Pattern& pat = app.project.patterns[static_cast<size_t>(SceneShown(app))];
    start = std::clamp(start, 0, kMaxSteps - 1);
    len = std::clamp(len, 1, kMaxSteps - start);
    BeginEdit(app);
    Track& tr = JamTrack(app);
    Carve(tr, start, start + len);
    Step s; s.active = true; s.lengthSteps = static_cast<uint8_t>(len);
    const std::vector<int> n = c.Notes(48);
    s.chordSize = static_cast<uint8_t>(std::min<int>(kMaxChordVoices, static_cast<int>(n.size())));
    for (int i = 0; i < kMaxChordVoices; ++i) s.chordNotes[static_cast<size_t>(i)] = static_cast<int8_t>(i < s.chordSize ? n[static_cast<size_t>(i)] : 0);
    tr.steps[static_cast<size_t>(start)] = s;
    FitLoop(tr, PerBar(pat));
    const std::vector<Blk> bl = Blocks(tr);
    for (size_t i = 0; i < bl.size(); ++i) if (bl[i].start == start) selBlock_ = static_cast<int>(i);
}

void ChordLabPanel::Resize(AppState& app, int bi, int newEnd) {
    SceneLock lock(app.sequencer.PatternMutex(), "CHORD LAB");
    const Pattern& pat = app.project.patterns[static_cast<size_t>(SceneShown(app))];
    Track& tr = JamTrack(app);
    const std::vector<Blk> bl = Blocks(tr);
    if (bi < 0 || bi >= static_cast<int>(bl.size())) return;
    const Blk b = bl[static_cast<size_t>(bi)];
    const int beat = PerBeat(pat);
    newEnd = std::clamp(newEnd, b.start + beat, kMaxSteps);
    if (bi + 1 < static_cast<int>(bl.size())) {
        const Blk nb = bl[static_cast<size_t>(bi) + 1];
        const int nbEnd = nb.start + nb.len;
        if (b.start + b.len == nb.start) {                       // neighbours: the edge between them moves
            newEnd = std::min(newEnd, nbEnd - beat);
            if (newEnd != nb.start) {
                Step moved = tr.steps[static_cast<size_t>(nb.start)];
                tr.steps[static_cast<size_t>(nb.start)] = Step{};
                moved.lengthSteps = static_cast<uint8_t>(nbEnd - newEnd);
                tr.steps[static_cast<size_t>(newEnd)] = moved;
            }
        } else newEnd = std::min(newEnd, nb.start);
    }
    tr.steps[static_cast<size_t>(b.start)].lengthSteps = static_cast<uint8_t>(newEnd - b.start);
    FitLoop(tr, PerBar(pat));
}

void ChordLabPanel::Split(AppState& app, int bi) {
    SceneLock lock(app.sequencer.PatternMutex(), "CHORD LAB");
    const Pattern& pat = app.project.patterns[static_cast<size_t>(SceneShown(app))];
    Track& tr = JamTrack(app);
    const std::vector<Blk> bl = Blocks(tr);
    if (bi < 0 || bi >= static_cast<int>(bl.size())) return;
    const Blk b = bl[static_cast<size_t>(bi)];
    const int beat = PerBeat(pat);
    if (b.len < 2 * beat) { app.SetStatus("CHORD LAB: THE BLOCK IS TOO SHORT TO SPLIT (ONE BEAT IS THE SMALLEST)"); return; }
    const int h = std::max(beat, (b.len / 2 / beat) * beat);
    BeginEdit(app);
    Track& et = JamTrack(app);   // the draft slot when this change started the draft
    Step second = et.steps[static_cast<size_t>(b.start)];
    et.steps[static_cast<size_t>(b.start)].lengthSteps = static_cast<uint8_t>(h);
    second.lengthSteps = static_cast<uint8_t>(b.len - h);
    et.steps[static_cast<size_t>(b.start + h)] = second;
}

void ChordLabPanel::DeleteBlock(AppState& app, int bi) {
    SceneLock lock(app.sequencer.PatternMutex(), "CHORD LAB");
    const Pattern& pat = app.project.patterns[static_cast<size_t>(SceneShown(app))];
    Track& tr = JamTrack(app);
    const std::vector<Blk> bl = Blocks(tr);
    if (bi < 0 || bi >= static_cast<int>(bl.size())) return;
    BeginEdit(app);
    Track& et = JamTrack(app);
    et.steps[static_cast<size_t>(bl[static_cast<size_t>(bi)].start)] = Step{};
    FitLoop(et, PerBar(pat));
    selBlock_ = -1;
}

void ChordLabPanel::ClearJam(AppState& app) {
    SceneLock lock(app.sequencer.PatternMutex(), "CHORD LAB");
    BeginEdit(app);
    Track& tr = JamTrack(app);
    for (Step& s : tr.steps) s = Step{};
    tr.useOwnLength = false;
    selBlock_ = -1;
}

void ChordLabPanel::StartFrom(AppState& app, int pi) {
    const auto& all = AllChordProgressions();
    if (pi < 0 || pi >= static_cast<int>(all.size())) return;
    const Pattern& pat = app.project.patterns[static_cast<size_t>(SceneShown(app))];
    const int len = DefaultLen(app);
    const int row = app.chdComplex ? 1 : 0;   // the CHORDS tab's 7TH / TRIAD choice carries over
    {
        SceneLock lock(app.sequencer.PatternMutex(), "CHORD LAB");
        BeginEdit(app);
        Track& tr = JamTrack(app);
        for (Step& s : tr.steps) s = Step{};
        int at = 0;
        for (int d : all[static_cast<size_t>(pi)].degrees) {
            const LabChord* pick = nullptr;
            for (const LabChord& c : pal_) if (c.row == row && c.degree == d) pick = &c;
            if (!pick || at + len > kMaxSteps) continue;
            Step s; s.active = true; s.lengthSteps = static_cast<uint8_t>(len);
            const std::vector<int> n = pick->Notes(48);
            s.chordSize = static_cast<uint8_t>(std::min<int>(kMaxChordVoices, static_cast<int>(n.size())));
            for (int i = 0; i < kMaxChordVoices; ++i) s.chordNotes[static_cast<size_t>(i)] = static_cast<int8_t>(i < s.chordSize ? n[static_cast<size_t>(i)] : 0);
            tr.steps[static_cast<size_t>(at)] = s;
            at += len;
        }
        FitLoop(tr, PerBar(pat));
    }
    selBlock_ = -1;
    app.SetStatus(std::string("CHORD LAB: STARTED FROM ") + all[static_cast<size_t>(pi)].name);
}

void ChordLabPanel::Leave(AppState& app) {
    if (auditionUntil_ > 0.0) { app.engine.ChordAuditionOff(); auditionUntil_ = 0.0; }
    opened_ = false;
}

// ---- drawing ------------------------------------------------------------------------------------------------------------------------
void ChordLabPanel::Draw(AppState& app, float W, float H) {
    if (!opened_) { opened_ = true; Rebuild(app); matched_ = false; selBlock_ = -1; }
    {   // which chords are shown: the edited slot (bank on) or the scene's own (bank off, nothing changed yet)
        const int now = app.project.chordBankOn ? std::clamp(app.project.chordSceneEdit, 0, kNumChordScenes - 1) : -1;
        if (now != jam_) { jam_ = now; selBlock_ = -1; }
    }
    Rebuild(app);
    if (auditionUntil_ > 0.0 && ImGui::GetTime() > auditionUntil_) { app.engine.ChordAuditionOff(); auditionUntil_ = 0.0; }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const int scene = SceneShown(app);
    const Pattern& pat = app.project.patterns[static_cast<size_t>(scene)];
    const int perBar = PerBar(pat), perBeat = PerBeat(pat);
    const float leftW = std::clamp(W * 0.50f, 560.0f, 760.0f);
    const float rx = leftW + 16.0f, rw = std::max(200.0f, W - rx - 6.0f);
    const int key = app.project.randomizerKeyRoot, scale = app.project.randomizerScale;
    ImGui::PushFont(nullptr, 14.0f);

    // header
    Text(dl, ImVec2(o.x + 4, o.y + 2), vis::kTeal, "CHORD LAB \xC2\xB7 JAM", 15.0f);
    {
        char hb[200];
        if (jam_ >= 0) std::snprintf(hb, sizeof hb, "draft in chord slot %d over scene %02d  -  the scene keeps its own chords until ADD TO SCENE", jam_ + 1, scene + 1);
        else std::snprintf(hb, sizeof hb, "showing the chords of scene %02d  -  nothing is changed yet: your first change starts a draft in a chord slot", scene + 1);
        Text(dl, ImVec2(o.x + 150, o.y + 4), vis::kTextDim, hb, 12.0f);
    }
    {   // v44.46: the sound of the chords is one switch away, in the place of the whole jam (the loop keeps playing)
        ImGui::SetCursorScreenPos(ImVec2(o.x + W - 30 - 142, o.y));
        int v = soundView_ ? 1 : 0;
        const char* vl[2] = {"JAM", "SOUND"};
        if (Segmented("##lview", &v, vl, 2, 68.0f)) {
            soundView_ = v == 1;
            if (soundView_) app.seqCursorTrack = kChordTrackIndex;
        }
        if (ImGui::IsItemHovered()) Hint("JAM: the palette and the loop.  SOUND: the sound of the chord instrument (preset, oscillators, filter, envelope, EQ, FX) while the loop keeps playing.");
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x + W - 30, o.y));
    if (MiniButton("X", 26.0f)) { app.chordLabOpen = false; }
    if (ImGui::IsItemHovered()) Hint(jam_ >= 0 ? "Close CHORD LAB. The draft stays in its chord slot." : "Close CHORD LAB. Nothing was changed.");

    if (soundView_) {
        ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + 28.0f));
        ImGui::BeginChild("##chordsound", ImVec2(W, std::max(60.0f, H - 30.0f)), ImGuiChildFlags_None, ImGuiWindowFlags_None);
        if (soundBody_) soundBody_(app);
        ImGui::EndChild();
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(o.x, o.y));
        ImGui::Dummy(ImVec2(W, H));
        return;
    }
    // the loop's blocks (also needed above them: MATCH reads the notes under the selected block)
    Track& jt = JamTrack(app);
    const std::vector<Blk> blocks = Blocks(jt);
    const bool haveBlk = selBlock_ >= 0 && selBlock_ < static_cast<int>(blocks.size());
    if (!haveBlk) selBlock_ = -1;
    if (matched_) RefreshMatch(app, haveBlk ? blocks[static_cast<size_t>(selBlock_)].start : -1, haveBlk ? blocks[static_cast<size_t>(selBlock_)].len : 0);

    // ---- MATCH strip
    float y = 28.0f;
    dl->AddRectFilled(ImVec2(o.x, o.y + y), ImVec2(o.x + leftW, o.y + y + 54), IM_COL32(24, 26, 33, 255), 5.0f);
    dl->AddRect(ImVec2(o.x, o.y + y), ImVec2(o.x + leftW, o.y + y + 54), vis::kLine, 5.0f);
    Text(dl, ImVec2(o.x + 8, o.y + y + 7), vis::kTextDim, "MATCH TO", 12.0f);
    {
        const char* names[3] = {"BASS", "SYNTH 2", "SYNTH 3"};
        ImGui::SetCursorScreenPos(ImVec2(o.x + 78, o.y + y + 4));
        for (int i = 0; i < kNumSynthTracks; ++i) {
            if (i) ImGui::SameLine(0, 3);
            bool on = (srcMask_ >> i) & 1;
            if (Check(names[i], &on, i == 0 ? 56.0f : 70.0f)) srcMask_ = on ? (srcMask_ | (1 << i)) : (srcMask_ & ~(1 << i));
            if (ImGui::IsItemHovered()) Hint("Tracks whose notes the chords are compared with. The bass counts double on the beat.");
        }
        ImGui::SameLine(0, 10);
        const char* bl[3] = {"BAR 1", "BARS 1-2", "ALL"};
        Segmented("##bars", &barsMode_, bl, 3, 60.0f);
        if (ImGui::IsItemHovered()) Hint("How much of the scene is read: the first bar, the first two, or all of it.");
        ImGui::SameLine(0, 10);
        if (ToggleButton("MATCH", matched_, ImVec2(64.0f, 0.0f))) { matched_ = !matched_; if (matched_) Match(app); }
        if (ImGui::IsItemHovered()) Hint("Read the bass / synth notes and mark how well every chord fits them (the bar under a chord). With a block of the loop selected it reads only the notes under that block. Nothing is chosen for you.");
    }
    {
        char cap[160];
        if (matched_ && scopeBlock_) std::snprintf(cap, sizeof cap, "%d notes under block %d  -  bar under a chord = fit, teal outline = best", mel_.notes, selBlock_ + 1);
        else if (matched_) std::snprintf(cap, sizeof cap, "%d notes in the scene%s  -  select a block to read only its notes", mel_.notes, (srcMask_ & 1) ? " (bass on the beat counts double)" : "");
        else std::snprintf(cap, sizeof cap, "Press MATCH: every chord gets a fit bar, and so does every block. Nothing is chosen for you.");
        Text(dl, ImVec2(o.x + 8, o.y + y + 34), vis::kTextDim, cap, 12.0f);
    }

    // ---- palette
    y = 90.0f;
    {
        char hd[96]; std::snprintf(hd, sizeof hd, "PALETTE \xC2\xB7 %s %s", kKeyNames[((key % 12) + 12) % 12], kScaleNames[std::clamp(scale, 0, kNumScales - 1)]);
        Text(dl, ImVec2(o.x + 4, o.y + y), vis::kTextDim, hd, 12.0f);
        ImGui::SetCursorScreenPos(ImVec2(o.x + leftW - 52, o.y + y - 3));
        Check("9THS", &nine_, 52.0f);
        if (ImGui::IsItemHovered()) Hint("The second row shows 9th chords (five voices) instead of 7th chords. A chord can use up to six voices.");
    }
    const float rowLabelW = 58.0f, gap = 4.0f;
    const float chipW = (leftW - rowLabelW - gap * 6) / 7.0f;
    const float loopH = 92.0f;
    const float palTop = y + 30.0f;
    const float chipH = std::clamp((H - palTop - loopH - 8.0f - gap * 3) / 4.0f, 22.0f, 38.0f);
    static const char* kRowName[4] = {"TRIADS", "7THS", "SUS \xC2\xB7 ADD", "BORROWED"};
    const char* rowNames[4] = {kRowName[0], nine_ ? "9THS" : kRowName[1], kRowName[2], kRowName[3]};
    for (int c = 0; c < 7; ++c) {
        const float cx = rowLabelW + c * (chipW + gap);
        const char* rn = LabRomanDegree(c + 1, true);
        Text(dl, ImVec2(o.x + cx + chipW * 0.5f - TextW(rn, 11.0f) * 0.5f, o.y + y + 15), vis::kTextDim, rn, 11.0f);
    }
    int clickedRow = -1, clickedCol = -1; bool dbl = false;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    for (int r = 0; r < 4; ++r) {
        const float cy = palTop + r * (chipH + gap);
        Text(dl, ImVec2(o.x + 2, o.y + cy + chipH * 0.5f - 6), vis::kTextDim, rowNames[r], 11.0f);
        for (int c = 0; c < 7; ++c) {
            const LabChord* chip = nullptr;
            for (const LabChord& x : pal_) if (x.row == r && x.col == c) chip = &x;
            if (!chip) continue;
            const ImVec2 a(o.x + rowLabelW + c * (chipW + gap), o.y + cy), b(a.x + chipW, a.y + chipH);
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(r * 10 + c);
            ImGui::InvisibleButton("##chip", ImVec2(chipW, chipH));
            const bool hov = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked(0)) { clickedRow = r; clickedCol = c; if (ImGui::IsMouseDoubleClicked(0)) dbl = true; }
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 5.0f) && dragRow_ < 0) { dragRow_ = r; dragCol_ = c; }
            const bool sel = r == selRow_ && c == selCol_;
            const float dim = chip->inKey ? 1.0f : 0.78f;
            ImU32 fill = KindFill(chip->kind);
            dl->AddRectFilled(a, b, hov ? IM_COL32(52, 56, 70, 255) : fill, 5.0f);
            dl->AddRect(a, b, sel ? vis::kAccent : KindEdge(chip->kind), 5.0f, 0, sel ? 2.0f : 1.0f);
            ImU32 tc = KindText(chip->kind);
            if (dim < 1.0f) tc = (tc & 0x00FFFFFF) | (static_cast<ImU32>(190) << 24);
            Text(dl, ImVec2(a.x + 6, a.y + 3), tc, chip->name.c_str(), chipH >= 30 ? 15.0f : 13.0f);
            const float rw2 = TextW(chip->roman.c_str(), 9.0f);
            Text(dl, ImVec2(b.x - rw2 - 5, a.y + 3), vis::kTextDim, chip->roman.c_str(), 10.0f);
            if (chipH >= 30.0f) {
                std::string ns; for (int pc : chip->Pcs()) { ns += LabPcName(pc); ns += ' '; }
                Text(dl, ImVec2(a.x + 6, a.y + chipH - 14), vis::kTextDim, ns.c_str(), 10.0f);
            }
            const ImVec2 dot(b.x - 8, b.y - 8);
            if (chip->inKey) dl->AddCircleFilled(dot, 3.0f, vis::kTextDim); else dl->AddCircle(dot, 3.0f, vis::kTextDim, 12, 1.2f);
            if (matched_) {
                dl->AddRectFilled(ImVec2(a.x + 4, b.y - 4), ImVec2(a.x + 4 + (chipW - 8) * chip->fit, b.y - 2), chip->fit >= 0.75f ? vis::kTeal : (chip->fit >= 0.5f ? vis::kAccent : vis::kLineHi));
                if (chip->best) dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), vis::kTeal, 5.0f, 0, 2.0f);
            }
            if (hov && !ImGui::IsMouseDown(0)) {
                char fit[64] = "";
                if (matched_) std::snprintf(fit, sizeof fit, "  -  fits %d %% of the notes %s", static_cast<int>(chip->fit * 100.0f + 0.5f), scopeBlock_ ? "under the block" : "of the scene");
                Hint("%s (%s)%s%s\nClick: hear it and see why  -  Drag it into the loop  -  Double-click: add it after the last chord",
                     chip->name.c_str(), chip->roman.c_str(), fit, chip->inKey ? "" : "  -  outside the key (hollow dot)");
            }
            ImGui::PopID();
        }
    }
    if (clickedRow >= 0) {
        Select(clickedRow, clickedCol);
        selBlock_ = -1;
        for (const LabChord& c : pal_) if (c.row == clickedRow && c.col == clickedCol) {
            Audition(app, c);
            if (dbl) {
                const std::vector<Blk> bl = Blocks(JamTrack(app));
                const int at = bl.empty() ? 0 : bl.back().start + bl.back().len;
                if (at < kMaxSteps) Place(app, c, at, DefaultLen(app));
            }
        }
    }

    // ---- the loop
    const float loopTop = palTop + 4 * (chipH + gap) + 6.0f;
    const int loopSteps = std::max(perBar, jt.useOwnLength ? jt.length : pat.masterLength);
    {
        char hd[64]; std::snprintf(hd, sizeof hd, "THE JAM LOOP \xC2\xB7 %d BAR%s", std::max(1, loopSteps / perBar), loopSteps / perBar == 1 ? "" : "S");
        Text(dl, ImVec2(o.x + 4, o.y + loopTop), vis::kTextDim, hd, 12.0f);
        float bx = o.x + leftW;
        ImGui::SetCursorScreenPos(ImVec2(bx - 290, o.y + loopTop - 3));
        const auto& progs = AllChordProgressions();
        ImGui::SetNextItemWidth(130.0f);
        if (ChoiceBegin("##startfrom", "START FROM...")) {
            for (int i = 0; i < static_cast<int>(progs.size()); ++i) if (ImGui::Selectable(progs[static_cast<size_t>(i)].name, false)) StartFrom(app, i);
            ChoiceEnd();
        }
        if (ImGui::IsItemHovered()) Hint("Start the loop from a typical progression, in this key (7TH / TRIAD of the CHORDS tab is used).");
        ImGui::SameLine(0, 4);
        if (MiniButton("EMPTY", 52.0f)) ClearJam(app);
        if (ImGui::IsItemHovered()) Hint("Take every chord out of the jam loop (undoable). The scene's own chords are not touched.");
        ImGui::SameLine(0, 4);
        const char* dls[3] = {"1/2 BAR", "1 BAR", "2 BARS"};
        ImGui::SetNextItemWidth(78.0f);
        Choice("##newlen", &defLen_, dls, 3, 78.0f);
        if (ImGui::IsItemHovered()) Hint("Length of a chord that is dropped into the loop. Drag the edge of a block to change it afterwards.");
    }
    const float bx0 = o.x + 4, bx1 = o.x + leftW - 4, by0 = o.y + loopTop + 20.0f, bh = std::max(38.0f, H - loopTop - 20.0f - 18.0f);
    const float unit = (bx1 - bx0) / static_cast<float>(loopSteps);
    dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by0 + bh), IM_COL32(20, 22, 28, 255), 4.0f);
    for (int s = 0; s <= loopSteps; s += perBeat) {
        const bool bar = (s % perBar) == 0;
        dl->AddLine(ImVec2(bx0 + s * unit, by0), ImVec2(bx0 + s * unit, by0 + (bar ? bh : 6.0f)), bar ? vis::kLineHi : vis::kGrid, 1.0f);
        if (bar && s < loopSteps) { char n[12]; std::snprintf(n, sizeof n, "%d", s / perBar + 1); Text(dl, ImVec2(bx0 + s * unit + 3, by0 + 1), vis::kTextDim, n, 10.0f); }
    }
    bool overLoop = mouse.x >= bx0 && mouse.x <= bx1 && mouse.y >= by0 && mouse.y <= by0 + bh;
    // blocks
    int newSel = -2;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const Blk& b = blocks[i];
        const Step& st = jt.steps[static_cast<size_t>(b.start)];
        const LabChord lc = LabChordFromNotes(st.chordNotes.data(), st.chordSize);
        const ImVec2 a(bx0 + b.start * unit + 1, by0 + 12), e(bx0 + (b.start + b.len) * unit - 1, by0 + bh - 3);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(1000 + static_cast<int>(i));
        ImGui::InvisibleButton("##blk", ImVec2(std::max(4.0f, e.x - a.x - 8.0f), e.y - a.y));
        const bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(0)) newSel = static_cast<int>(i);
        if (ImGui::IsItemClicked(1)) { selBlock_ = static_cast<int>(i); ImGui::OpenPopup("##blkmenu"); }
        const bool sel = selBlock_ == static_cast<int>(i);
        dl->AddRectFilled(a, e, KindFill(lc.kind), 4.0f);
        dl->AddRect(a, e, sel ? vis::kAccent : KindEdge(lc.kind), 4.0f, 0, sel ? 2.0f : 1.0f);
        Text(dl, ImVec2(a.x + 6, a.y + 3), KindText(lc.kind), lc.name.c_str(), 16.0f);
        {
            std::string ns; for (int pc : lc.Pcs()) { ns += LabPcName(pc); ns += ' '; }
            char sub[96]; const float bars = static_cast<float>(b.len) / perBar;
            if (bars >= 1.0f && std::fabs(bars - std::round(bars)) < 0.01f) std::snprintf(sub, sizeof sub, "%s\xC2\xB7 %d bar%s", ns.c_str(), static_cast<int>(std::round(bars)), std::round(bars) == 1.0f ? "" : "s");
            else std::snprintf(sub, sizeof sub, "%s\xC2\xB7 %d/%d bar", ns.c_str(), std::max(1, b.len), perBar);
            if (e.y - a.y > 34.0f) Text(dl, ImVec2(a.x + 6, e.y - 15), vis::kTextDim, sub, 11.0f);
        }
        if (matched_) {   // v44.46: the fit of THIS chord with the notes under THIS block
            LabChord tmp = lc;
            const LabMelody under = LabMelodyFromRange(pat, SrcTracks(), b.start, b.start + b.len);
            LabFitOne(tmp, under);
            const float w = e.x - a.x - 8.0f;
            if (under.notes > 0) {
                dl->AddRectFilled(ImVec2(a.x + 4, e.y - 5), ImVec2(a.x + 4 + (w - 4) * tmp.fit, e.y - 2), tmp.fit >= 0.75f ? vis::kTeal : (tmp.fit >= 0.5f ? vis::kAccent : vis::kLineHi));
                char fb[32]; std::snprintf(fb, sizeof fb, "%d/%d notes", tmp.covered, under.notes);
                const float tw = TextW(fb, 11.0f);
                if (tw + 80.0f < w) Text(dl, ImVec2(a.x + w - tw - 4, a.y + 5), vis::kTextDim, fb, 11.0f);
            } else if (w > 150.0f) Text(dl, ImVec2(a.x + w - TextW("no notes", 11.0f) - 4, a.y + 5), vis::kTextDim, "no notes", 11.0f);
        }
        if (hov && !ImGui::IsMouseDown(0)) Hint("%s  -  click to select (see WHY, change its SIZE)  -  drag the orange edge to change its length  -  right-click: delete", lc.name.c_str());
        // the length handle
        const ImVec2 ha(e.x - 8, a.y), hb(e.x, e.y);
        ImGui::SetCursorScreenPos(ha);
        ImGui::InvisibleButton("##edge", ImVec2(8.0f, e.y - a.y));
        const bool hh = ImGui::IsItemHovered() || resizeBlk_ == static_cast<int>(i);
        if (ImGui::IsItemHovered()) { ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW); if (!ImGui::IsMouseDown(0)) Hint("Drag to make the chord longer or shorter. The next chord gives way (one beat is the smallest)."); }
        dl->AddRectFilled(ha, hb, hh ? vis::kAccentHi : vis::kAccent, 3.0f);
        if (ImGui::IsItemActivated()) { resizeBlk_ = static_cast<int>(i); BeginEdit(app); }
        if (resizeBlk_ == static_cast<int>(i) && ImGui::IsItemActive()) {
            const int target = static_cast<int>(std::round((mouse.x - bx0) / unit / perBeat)) * perBeat;
            if (target != b.start + b.len) Resize(app, static_cast<int>(i), target);
        }
        if (resizeBlk_ == static_cast<int>(i) && !ImGui::IsMouseDown(0)) resizeBlk_ = -1;
        ImGui::PopID();
    }
    if (newSel >= 0) {
        selBlock_ = newSel;
        const Blk& b = blocks[static_cast<size_t>(newSel)];
        const Step& st = jt.steps[static_cast<size_t>(b.start)];
        const LabChord lc = LabChordFromNotes(st.chordNotes.data(), st.chordSize);
        unsigned m = 0; for (int pc : lc.Pcs()) m |= 1u << pc;
        for (const LabChord& c : pal_) { unsigned pm = 0; for (int pc : c.Pcs()) pm |= 1u << pc; if (pm == m && c.root == lc.root) { Select(c.row, c.col); break; } }
        int8_t nn[kMaxChordVoices]; const int cnt = std::min<int>(kMaxChordVoices, st.chordSize);
        for (int i = 0; i < cnt; ++i) nn[i] = st.chordNotes[static_cast<size_t>(i)];
        app.engine.ChordAuditionOn(nn, cnt, 0.85f); auditionUntil_ = ImGui::GetTime() + 0.9;
    }
    if (blocks.empty()) Text(dl, ImVec2(bx0 + 10, by0 + bh * 0.5f - 7), vis::kTextDim, "Drag a chord here, double-click one, or START FROM a progression.", 13.0f);
    // the playhead: the chord loop runs on its own length
    if (app.sequencer.IsPlaying()) {
        const int pos = app.sequencer.ChordLoopPos(loopSteps);
        dl->AddLine(ImVec2(bx0 + pos * unit, by0), ImVec2(bx0 + pos * unit, by0 + bh), IM_COL32(255, 255, 255, 190), 2.0f);
    }
    if (ImGui::BeginPopup("##blkmenu")) {
        if (ImGui::MenuItem("Delete this chord")) DeleteBlock(app, selBlock_);
        if (ImGui::MenuItem("Split in two")) Split(app, selBlock_);
        ImGui::EndPopup();
    }
    // a chip dragged towards the loop
    if (dragRow_ >= 0) {
        const LabChord* dc = nullptr; for (const LabChord& c : pal_) if (c.row == dragRow_ && c.col == dragCol_) dc = &c;
        if (dc) {
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            const ImVec2 a(mouse.x - chipW * 0.5f, mouse.y - chipH * 0.5f);
            fg->AddRectFilled(a, ImVec2(a.x + chipW, a.y + chipH), KindFill(dc->kind), 5.0f);
            fg->AddRect(a, ImVec2(a.x + chipW, a.y + chipH), vis::kAccent, 5.0f, 0, 2.0f);
            ImGui::PushFont(nullptr, 15.0f); fg->AddText(ImVec2(a.x + 6, a.y + 4), KindText(dc->kind), dc->name.c_str()); ImGui::PopFont();
            if (overLoop) {
                const int s = std::clamp(static_cast<int>(std::floor((mouse.x - bx0) / unit / perBeat)) * perBeat, 0, std::max(0, loopSteps - perBeat));
                const int ln = std::min(DefaultLen(app), kMaxSteps - s);
                fg->AddRectFilled(ImVec2(bx0 + s * unit, by0 + 12), ImVec2(bx0 + (s + ln) * unit, by0 + bh - 3), IM_COL32(242, 163, 61, 50), 4.0f);
                fg->AddRect(ImVec2(bx0 + s * unit, by0 + 12), ImVec2(bx0 + (s + ln) * unit, by0 + bh - 3), vis::kAccent, 4.0f, 0, 2.0f);
            }
            if (!ImGui::IsMouseDown(0)) {
                if (overLoop) {
                    const int s = std::clamp(static_cast<int>(std::floor((mouse.x - bx0) / unit / perBeat)) * perBeat, 0, kMaxSteps - perBeat);
                    Audition(app, *dc);
                    Place(app, *dc, s, std::min(DefaultLen(app), kMaxSteps - s));
                    Select(dragRow_, dragCol_);
                }
                dragRow_ = dragCol_ = -1;
            }
        } else dragRow_ = dragCol_ = -1;
    }

    // ---- WHY (right)
    const LabChord* sel = Selected();
    const float whyW = std::max(220.0f, rw * 0.60f);
    float ry = 28.0f;
    {
        char hd[96];
        std::snprintf(hd, sizeof hd, "WHY  \xC2\xB7  %s", sel ? (sel->name + "  (" + sel->roman + ")").c_str() : "pick a chord");
        Text(dl, ImVec2(o.x + rx, o.y + ry), vis::kTextDim, hd, 12.0f);
        dl->AddLine(ImVec2(o.x + rx + TextW(hd, 12.0f) + 8, o.y + ry + 7), ImVec2(o.x + rx + whyW, o.y + ry + 7), vis::kLine, 1.0f);
    }
    if (sel) {
        Text(dl, ImVec2(o.x + rx, o.y + ry + 16), KindText(sel->kind), sel->name.c_str(), 30.0f);
        std::string sub; for (int pc : sel->Pcs()) { sub += LabPcName(pc); sub += ' '; }
        sub += sel->degree == 0 ? "\xC2\xB7 borrowed" : "\xC2\xB7 in the key";
        Text(dl, ImVec2(o.x + rx + TextW(sel->name.c_str(), 30.0f) + 10, o.y + ry + 30), vis::kTextDim, sub.c_str(), 12.0f);
        // keyboard
        const float kx = o.x + rx, ky = o.y + ry + 56, kw = 11.0f, kh = 44.0f;
        unsigned lit = 0; for (int pc : sel->Pcs()) lit |= 1u << pc;
        static const int wk[7] = {0, 2, 4, 5, 7, 9, 11};
        for (int i = 0; i < 14; ++i) {
            const int pc = wk[i % 7];
            const bool on = (lit >> pc) & 1;
            dl->AddRectFilled(ImVec2(kx + i * kw, ky), ImVec2(kx + (i + 1) * kw - 1, ky + kh), on ? vis::kViolet : IM_COL32(226, 228, 236, 255));
        }
        static const int bkPos[5] = {0, 1, 3, 4, 5}, bkPc[5] = {1, 3, 6, 8, 10};
        for (int oct = 0; oct < 2; ++oct) for (int j = 0; j < 5; ++j) {
            const bool on = (lit >> bkPc[j]) & 1;
            const float x = kx + (oct * 7 + bkPos[j] + 1) * kw - 3.5f;
            dl->AddRectFilled(ImVec2(x, ky), ImVec2(x + 7.0f, ky + kh * 0.6f), on ? vis::kAccent : IM_COL32(26, 28, 36, 255));
        }
        // fit
        const float fx = kx + 14 * kw + 14;
        if (matched_ && mel_.notes > 0) {
            char pc[16]; std::snprintf(pc, sizeof pc, "%d %%", static_cast<int>(sel->fit * 100.0f + 0.5f));
            Text(dl, ImVec2(fx, ky), vis::kTextDim, scopeBlock_ ? "fit under this block" : "fit with the scene", 11.0f);
            Text(dl, ImVec2(fx, ky + 11), vis::kTeal, pc, 24.0f);
            char cv[32]; std::snprintf(cv, sizeof cv, "%d of %d notes", sel->covered, mel_.notes);
            Text(dl, ImVec2(fx, ky + 38), vis::kTextDim, cv, 11.0f);
        }
        // the reasons
        const std::vector<Blk>& bl = blocks;
        const LabChord* prevPtr = nullptr; LabChord prevChord;
        int pi = -1;
        if (selBlock_ >= 0 && selBlock_ < static_cast<int>(bl.size())) pi = selBlock_ - 1; else pi = static_cast<int>(bl.size()) - 1;
        if (pi >= 0 && pi < static_cast<int>(bl.size())) { const Step& ps = jt.steps[static_cast<size_t>(bl[static_cast<size_t>(pi)].start)]; prevChord = LabChordFromNotes(ps.chordNotes.data(), ps.chordSize); prevPtr = &prevChord; }
        const std::vector<std::string> why = LabWhy(*sel, prevPtr, key, scale, matched_ ? mel_ : LabMelody{}, scopeBlock_ ? "under this block" : "in the scene");
        float wy = o.y + ry + 108.0f;
        const float lineH = 15.0f;
        for (size_t i = 0; i < why.size(); ++i) {
            if (wy > o.y + H - 98.0f) break;
            dl->AddCircleFilled(ImVec2(o.x + rx + 4, wy + 7), 3.0f, i + 1 == why.size() && why.size() > 3 && matched_ ? vis::kTeal : vis::kTeal);
            Wrap(dl, ImVec2(o.x + rx + 14, wy), whyW - 18, IM_COL32(210, 213, 224, 255), why[i], 12.0f, lineH, 2);
            wy += lineH * (TextW(why[i].c_str(), 12.0f) > whyW - 18 ? 2 : 1) + 5.0f;
        }
    } else {
        Wrap(dl, ImVec2(o.x + rx, o.y + ry + 22), whyW, vis::kTextDim, "Click a chord of the palette to hear it and see what it holds of the key and of your notes. Drag it into the loop to build a progression.", 12.0f, 16.0f, 4);
    }

    // ---- SIZE of the selected block (right column; v44.46: where the NEXT list was, it was a formula and is gone)
    {
        const float nx = o.x + rx + whyW + 14.0f, nw = std::max(130.0f, rw - whyW - 14.0f);
        const float sy = o.y + ry;
        Text(dl, ImVec2(nx, sy), vis::kTextDim, "SIZE", 12.0f);
        dl->AddLine(ImVec2(nx + 34, sy + 7), ImVec2(nx + nw, sy + 7), vis::kLine, 1.0f);
        const bool haveSel = selBlock_ >= 0 && selBlock_ < static_cast<int>(blocks.size());
        ImGui::BeginDisabled(!haveSel);
        ImGui::SetCursorScreenPos(ImVec2(nx, sy + 16));
        const float bw = std::max(40.0f, (nw - 12.0f) / 4.0f);
        const int b0 = haveSel ? blocks[static_cast<size_t>(selBlock_)].start : 0;
        auto setLen = [&](int len) { BeginEdit(app); Resize(app, selBlock_, b0 + len); };
        if (MiniButton("1/2 BAR", bw)) setLen(std::max(perBeat, perBar / 2));
        ImGui::SameLine(0, 4); if (MiniButton("1 BAR", bw)) setLen(perBar);
        ImGui::SameLine(0, 4); if (MiniButton("2 BARS", bw)) setLen(perBar * 2);
        ImGui::SameLine(0, 4); if (MiniButton("SPLIT", bw)) Split(app, selBlock_);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) Hint("Select a block of the loop, then set its length. SPLIT cuts it in two halves (same chord) to change the second one.");
        Wrap(dl, ImVec2(nx, sy + 44), nw, vis::kTextDim, "or drag the orange edge of a block. A chord is held as a stack of up to six voices.", 11.0f, 14.0f, 3);
    }

    // ---- FROM JAM TO TRACK
    {
        const float by = H - 84.0f;
        const ImVec2 a(o.x + rx, o.y + by), b(o.x + W - 6.0f, o.y + H - 2.0f);
        dl->AddRect(a, b, vis::kLine, 5.0f);
        Text(dl, ImVec2(a.x + 8, a.y + 3), vis::kTextDim, "FROM JAM TO TRACK", 11.0f);
        const bool has = jam_ >= 0 && !app.ChordSceneEmpty(jam_);
        ImGui::SetCursorScreenPos(ImVec2(a.x + 8, a.y + 20));
        ImGui::BeginDisabled(!has);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.40f, 0.34f, 1.0f));
        char add[40]; std::snprintf(add, sizeof add, "ADD TO SCENE %02d", scene + 1);
        if (ImGui::Button(add, ImVec2(150, 28))) app.CommitChordSlotToScene(jam_, scene);
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) Hint(has ? "Write the jam into this scene's own chord track, in the key it was built in. One undo step; the chord slot keeps its copy." : (jam_ < 0 ? "Nothing to add yet: the loop shows this scene's own chords. Change something and a draft starts." : "Build a loop of chords first."));
        ImGui::SameLine(0, 8);
        if (app.activeTab == 1 && app.project.songChainLength > 0) {   // v44.51: on TRACK the jam is over the open brick: put it on the brick instead of throwing a scene
            const int bi = std::clamp(app.selBrick, 0, app.project.songChainLength - 1);
            ImGui::BeginDisabled(!has);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.86f, 0.50f, 0.12f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.10f, 0.07f, 0.03f, 1.0f));
            char use[48]; std::snprintf(use, sizeof use, "BRICK %d PLAYS THIS JAM", bi + 1);
            if (ImGui::Button(use, ImVec2(176, 28))) {
                app.PushSoundUndo();
                if (!app.project.chordBankOn) app.SetChordBank(true);
                app.project.songChain[static_cast<size_t>(bi)].chordScene = static_cast<int8_t>(jam_);
                app.sequencer.RefreshArrangement();
                app.SetStatus("BRICK " + std::to_string(bi + 1) + " PLAYS CHORD SLOT " + std::to_string(jam_ + 1));
            }
            ImGui::PopStyleColor(2);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) Hint(has ? "The open brick plays this chord slot (the chord bank goes on). The scene's own chords stay as they are. Undo takes it back." : "Build a loop of chords first.");
            ImGui::SameLine(0, 8);
            if (ImGui::Button("CLOSE", ImVec2(70, 28))) app.chordLabOpen = false;
            if (ImGui::IsItemHovered()) Hint("Back to the brick: inspector and notes.");
        } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.86f, 0.50f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.10f, 0.07f, 0.03f, 1.0f));
        if (ImGui::Button("THROW ONTO THE TRACK", ImVec2(176, 28))) { if (app.ThrowIntoChain()) { app.selBrick = std::max(0, app.project.songChainLength - 1); app.uiTabRequest = 1; } }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) Hint("The scene becomes the next brick of the track and TRACK opens. With the chord bank on, the brick plays the chord slot you are hearing (and keeps the scene's own chords too).");
        ImGui::SameLine(0, 8);
        if (ImGui::Button("TRACK", ImVec2(70, 28))) app.uiTabRequest = 1;
        if (ImGui::IsItemHovered()) Hint("Open TRACK: arrange the bricks (the chord scene each brick plays is set in its right-click menu).");
        }
        Wrap(dl, ImVec2(a.x + 8, a.y + 52), b.x - a.x - 16, vis::kTextDim, "1 JAM with the palette over the running scene.  2 ADD TO SCENE writes the loop into this scene's chord track.  3 THROW makes the scene a brick; TRACK is where it is arranged.", 10.5f, 13.0f, 2);
    }
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y));
    ImGui::Dummy(ImVec2(W, H));
}

}  // namespace t8::desktop
