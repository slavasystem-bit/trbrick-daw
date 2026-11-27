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
    if (abBefore_) ListenAB(app, false);   // C2: a change always goes into the draft
    if (app.project.chordBankOn) app.PushUndo(); else app.StartChordDraft();
    ++labEdits_;
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

// C2: the palette is always scored against the notes under the place WHAT FITS is about (a block of the loop, or the room after the
// last one). Cheap (a few hundred steps), so it follows every edit of the notes.
void ChordLabPanel::RefreshMatch(AppState& app, int blockStart, int blockLen) {
    const Pattern& pat = app.project.patterns[static_cast<size_t>(SceneShown(app))];
    mel_ = LabMelodyFromRange(pat, SrcTracks(), blockStart, blockStart + std::max(1, blockLen));
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
    if (abBefore_) ListenAB(app, false);   // C2: leaving on A BEFORE puts the draft back
    opened_ = false;
    labEdits_ = 0;
}

// C2: A BEFORE plays the scene's own chords, B DRAFT the draft slot. Only what is heard changes (no undo step, nothing is written):
// the bank is switched off and on again and the panel shows the scene's chords read-only while A is on.
void ChordLabPanel::ListenAB(AppState& app, bool before) {
    if (before == abBefore_) return;
    if (before && jam_ < 0) return;
    std::lock_guard<SceneMutex> lock(app.sequencer.PatternMutex());
    if (before) {
        abSlot_ = jam_;
        app.project.chordBankOn = false;
        app.sequencer.SetChordSceneNow(0);
    } else {
        app.project.chordBankOn = true;
        app.project.chordSceneEdit = std::clamp(abSlot_, 0, kNumChordScenes - 1);
        app.sequencer.SetChordSceneNow(app.project.chordSceneEdit);
    }
    abBefore_ = before;
    app.RefreshChordLive(true);
}

// C2: UNDO ALL takes back every change made in CHORD LAB since it was opened (the draft included), one Undo at a time.
void ChordLabPanel::UndoAll(AppState& app) {
    if (abBefore_) ListenAB(app, false);
    int n = 0;
    {
        SceneLock lock(app.sequencer.PatternMutex(), "CHORD LAB");
        while (labEdits_ > 0 && app.Undo()) { --labEdits_; ++n; }
    }
    labEdits_ = 0;
    selBlock_ = -1;
    app.SetStatus(n > 0 ? "CHORD LAB: " + std::to_string(n) + " CHANGE" + (n == 1 ? "" : "S") + " TAKEN BACK" : "CHORD LAB: NOTHING TO TAKE BACK");
}


// ---- drawing ------------------------------------------------------------------------------------------------------------------------
namespace {

// C2: the palette and the loop are coloured by FUNCTION: home dark, away middle, tension bright (legend under the palette).
ImU32 FnFill(LabFn f) { return f == LabFn::Home ? IM_COL32(36, 33, 58, 255) : (f == LabFn::Away ? IM_COL32(66, 55, 122, 255) : IM_COL32(122, 96, 226, 255)); }
ImU32 FnEdge(LabFn f) { return f == LabFn::Home ? IM_COL32(78, 70, 120, 255) : (f == LabFn::Away ? IM_COL32(110, 94, 186, 255) : IM_COL32(160, 134, 250, 255)); }

void DashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float th = 1.2f) {
    const float dash = 5.0f, gap = 4.0f;
    for (float x = a.x; x < b.x; x += dash + gap) { const float x2 = std::min(b.x, x + dash); dl->AddLine(ImVec2(x, a.y), ImVec2(x2, a.y), col, th); dl->AddLine(ImVec2(x, b.y), ImVec2(x2, b.y), col, th); }
    for (float y = a.y; y < b.y; y += dash + gap) { const float y2 = std::min(b.y, y + dash); dl->AddLine(ImVec2(a.x, y), ImVec2(a.x, y2), col, th); dl->AddLine(ImVec2(b.x, y), ImVec2(b.x, y2), col, th); }
}

void Bar(ImDrawList* dl, ImVec2 a, float w, float h, float v, ImU32 col) {
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), IM_COL32(255, 255, 255, 22), h * 0.5f);
    if (v > 0.0f) dl->AddRectFilled(a, ImVec2(a.x + std::max(h, w * std::clamp(v, 0.0f, 1.0f)), a.y + h), col, h * 0.5f);
}

// The note names under a stretch of the scene, in time order (lowest track first on a step), at most `max` of them.
std::string NotesUnder(const Pattern& pat, const std::vector<int>& tracks, int from, int to, int max) {
    std::string out; int n = 0;
    for (int i = std::max(0, from); i < to && n < max; ++i) {
        for (int ti : tracks) {
            if (ti < 0 || ti >= kNumTracks) continue;
            const Track& t = pat.tracks[static_cast<size_t>(ti)];
            if (t.type == TrackType::Chord || t.type == TrackType::Drum || t.type == TrackType::Sampler) continue;
            const int len = std::max(1, t.useOwnLength ? t.length : pat.masterLength);
            const Step& s = t.steps[static_cast<size_t>((i % len) % kMaxSteps)];
            if (!s.active || s.chordSize > 0) continue;
            if (n < max) { if (!out.empty()) out += " "; out += LabPcName(s.note); ++n; }
        }
    }
    return out;
}

}  // namespace

ChordLabPanel::Slot ChordLabPanel::AdviceSlot(const AppState& app, const Track& jam, const std::vector<Blk>& blocks) const {
    (void)jam;
    Slot s;
    if (selBlock_ >= 0 && selBlock_ < static_cast<int>(blocks.size())) {
        s.start = blocks[static_cast<size_t>(selBlock_)].start; s.len = blocks[static_cast<size_t>(selBlock_)].len; s.block = selBlock_;
        return s;
    }
    s.start = blocks.empty() ? 0 : blocks.back().start + blocks.back().len;
    s.len = DefaultLen(app);
    if (s.start + s.len > kMaxSteps) { s.start = blocks.empty() ? 0 : blocks.back().start; s.len = blocks.empty() ? s.len : blocks.back().len; s.block = blocks.empty() ? -1 : static_cast<int>(blocks.size()) - 1; }
    return s;
}

// Name of the draft, the key, A / B, KEEP, UNDO ALL and THROW (or BRICK n PLAYS THIS JAM on TRACK).
void ChordLabPanel::DrawHeader(AppState& app, ImDrawList* dl, ImVec2 o, float W, int scene) {
    const int key = app.project.randomizerKeyRoot, scale = app.project.randomizerScale;
    char hb[96];
    if (abBefore_) std::snprintf(hb, sizeof hb, "A \xC2\xB7 SCENE %02d'S OWN CHORDS", scene + 1);
    else if (jam_ >= 0) std::snprintf(hb, sizeof hb, "DRAFT \xC2\xB7 SLOT %d OVER SCENE %02d", jam_ + 1, scene + 1);
    else std::snprintf(hb, sizeof hb, "SCENE %02d \xC2\xB7 ITS OWN CHORDS", scene + 1);
    float x = o.x + 2;
    auto pill = [&](const char* t, ImU32 bg, ImU32 fg) {
        const float w = TextW(t, 12.0f) + 18.0f;
        dl->AddRectFilled(ImVec2(x, o.y + 3), ImVec2(x + w, o.y + 25), bg, 11.0f);
        Text(dl, ImVec2(x + 9, o.y + 7), fg, t, 12.0f);
        x += w + 8.0f;
    };
    pill(hb, IM_COL32(50, 42, 86, 255), IM_COL32(205, 184, 255, 255));
    std::string kn = LabKeyName(key, scale);
    for (char& ch : kn) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    pill(kn.c_str(), IM_COL32(24, 60, 52, 255), vis::kTeal);
    {   // v44.46: the sound of the chords is one switch away, in the place of the whole lab (the loop keeps playing)
        ImGui::SetCursorScreenPos(ImVec2(x, o.y));
        int v = soundView_ ? 1 : 0;
        static const char* const kView[2] = {"JAM", "SOUND"};
        if (Segmented("##lview", &v, kView, 2, 64.0f)) {
            soundView_ = v == 1;
            if (soundView_) app.seqCursorTrack = kChordTrackIndex;
        }
        if (ImGui::IsItemHovered()) Hint("JAM: the palette, the loop and what fits.  SOUND: the sound of the chord instrument (preset, oscillators, filter, envelope, EQ, FX) while the loop keeps playing.");
        x += 2 * 64.0f + 14.0f;
    }
    if (jam_ < 0 && !abBefore_ && !soundView_) Text(dl, ImVec2(x + 4, o.y + 7), vis::kTextDim, "nothing is changed yet: your first change starts a draft", 12.0f);

    const bool has = (jam_ >= 0 || abBefore_) && !app.ChordSceneEmpty(abBefore_ ? abSlot_ : jam_);
    const bool onTrack = app.activeTab == 1 && app.project.songChainLength > 0;
    const float throwW = onTrack ? 190.0f : 176.0f;
    float rx = o.x + W - throwW - 2.0f - 92.0f - 8.0f - 150.0f - 8.0f - 170.0f;
    ImGui::SetCursorScreenPos(ImVec2(rx, o.y));
    {   // A / B
        ImGui::BeginDisabled(jam_ < 0 && !abBefore_);
        int ab = abBefore_ ? 0 : 1;
        static const char* const kAB[2] = {"A BEFORE", "B DRAFT"};
        if (Segmented("##ab", &ab, kAB, 2, 82.0f)) ListenAB(app, ab == 0);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) Hint(jam_ < 0 && !abBefore_ ? "A / B compares the draft with the scene's own chords. There is no draft yet." : "A BEFORE: hear the scene's own chords.  B DRAFT: hear the draft. Nothing is written; a change goes into the draft (B).");
    }
    ImGui::SameLine(0, 8);
    ImGui::BeginDisabled(!has);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.40f, 0.34f, 1.0f));
    char keep[40]; std::snprintf(keep, sizeof keep, "KEEP IN SCENE %02d", scene + 1);
    if (ImGui::Button(keep, ImVec2(150, 0))) { if (abBefore_) ListenAB(app, false); app.CommitChordSlotToScene(jam_, scene); }
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) Hint(has ? "Write the draft into this scene's own chord track, in the key it was built in. One undo step; the chord slot keeps its copy." : "Nothing to keep yet: change the loop and a draft starts.");
    ImGui::SameLine(0, 8);
    ImGui::BeginDisabled(labEdits_ == 0);
    if (ImGui::Button("UNDO ALL", ImVec2(92, 0))) UndoAll(app);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) Hint("Take back every change made in CHORD LAB since it was opened (%d step%s), the draft included.", labEdits_, labEdits_ == 1 ? "" : "s");
    ImGui::SameLine(0, 8);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.86f, 0.50f, 0.12f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.10f, 0.07f, 0.03f, 1.0f));
    if (onTrack) {   // v44.51: on TRACK the jam is over the open brick: put it on the brick instead of throwing a scene
        const int bi = std::clamp(app.selBrick, 0, app.project.songChainLength - 1);
        ImGui::BeginDisabled(!has);
        char use[48]; std::snprintf(use, sizeof use, "BRICK %d PLAYS THIS JAM", bi + 1);
        if (ImGui::Button(use, ImVec2(throwW, 0))) {
            if (abBefore_) ListenAB(app, false);
            SceneLock lock(app.sequencer.PatternMutex(), "CHORD LAB");
            app.PushSoundUndo();
            if (!app.project.chordBankOn) app.SetChordBank(true);
            app.project.songChain[static_cast<size_t>(bi)].chordScene = static_cast<int8_t>(jam_);
            app.sequencer.RefreshArrangement();
            app.SetStatus("BRICK " + std::to_string(bi + 1) + " PLAYS CHORD SLOT " + std::to_string(jam_ + 1));
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) Hint(has ? "The open brick plays this chord slot (the chord bank goes on). The scene's own chords stay as they are. Undo takes it back." : "Build a loop of chords first.");
    } else {
        if (ImGui::Button("THROW ONTO THE TRACK", ImVec2(throwW, 0))) {
            if (abBefore_) ListenAB(app, false);
            SceneLock lock(app.sequencer.PatternMutex(), "CHORD LAB");   // TEST-5 found it unlocked
            if (app.ThrowIntoChain()) { app.selBrick = std::max(0, app.project.songChainLength - 1); app.uiTabRequest = 1; }
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) Hint("The scene becomes the next brick of the track and TRACK opens. With the chord bank on, the brick plays the chord slot you are hearing (and keeps the scene's own chords too).");
    }
}

// WHAT FITS IN BAR n: four cards (the best of the chosen mode) with the three measured bars, and the sentence for the chosen card.
void ChordLabPanel::DrawWhatFits(AppState& app, ImDrawList* dl, ImVec2 a, float w, float h, const Track& jam, const std::vector<Blk>& blocks, const Slot& slot) {
    const int key = app.project.randomizerKeyRoot, scale = app.project.randomizerScale;
    const Pattern& pat = app.project.patterns[static_cast<size_t>(SceneShown(app))];
    const int perBar = PerBar(pat);
    // the chords around the place (the loop comes round)
    LabChord prevC, nextC; const LabChord* prev = nullptr; const LabChord* next = nullptr;
    LabSlot ls;
    const int nb = static_cast<int>(blocks.size());
    if (nb > 0) {
        int pi = -1, ni = -1;
        if (slot.block >= 0) { if (nb > 1) { pi = (slot.block - 1 + nb) % nb; ni = (slot.block + 1) % nb; } }
        else { pi = nb - 1; ni = 0; }
        if (pi >= 0) { const Step& st = jam.steps[static_cast<size_t>(blocks[static_cast<size_t>(pi)].start)]; prevC = LabChordFromNotes(st.chordNotes.data(), st.chordSize); prev = &prevC; }
        if (ni >= 0) { const Step& st = jam.steps[static_cast<size_t>(blocks[static_cast<size_t>(ni)].start)]; nextC = LabChordFromNotes(st.chordNotes.data(), st.chordSize); next = &nextC; }
        for (const Blk& b : blocks) { const Step& st = jam.steps[static_cast<size_t>(b.start)]; ls.loopMasks.push_back(LabMask(LabChordFromNotes(st.chordNotes.data(), st.chordSize))); }
    }
    ls.prev = prev; ls.next = next; ls.melody = mel_;
    const std::vector<LabAdvice> adv = LabAdvise(pal_, ls, key, scale, static_cast<LabAdviceMode>(adviceMode_));

    char hd[64]; std::snprintf(hd, sizeof hd, "WHAT FITS IN BAR %d", slot.start / perBar + 1);
    Text(dl, a, vis::kTextDim, hd, 12.0f);
    dl->AddLine(ImVec2(a.x + TextW(hd, 12.0f) + 8, a.y + 7), ImVec2(a.x + w - 3 * 52.0f - 60.0f, a.y + 7), vis::kLine, 1.0f);
    {
        std::string ctx;
        if (prev) ctx += "after " + prev->name;
        if (next) ctx += std::string(ctx.empty() ? "" : " \xC2\xB7 ") + "before " + next->name + (slot.block < 0 || slot.block == nb - 1 ? " (the loop comes round)" : "");
        const std::string notes = NotesUnder(pat, SrcTracks(), slot.start, slot.start + slot.len, 8);
        ctx += std::string(ctx.empty() ? "" : " \xC2\xB7 ") + (notes.empty() ? std::string("no bass / synth notes here") : "notes here: " + notes);
        Wrap(dl, ImVec2(a.x, a.y + 18), w, IM_COL32(210, 213, 224, 255), ctx, 12.0f, 15.0f, 2);
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + 50));
    {
        static const char* const kModes[4] = {"BEST", "SMOOTH", "MORE TENSION", "NEW COLOUR"};
        if (Segmented("##advmode", &adviceMode_, kModes, 4)) adviceSel_ = 0;
        if (ImGui::IsItemHovered()) Hint("BEST: function, voices and notes together.  SMOOTH: the voices move least.  MORE TENSION: chords that pull.  NEW COLOUR: chords that are not in the loop yet (sus, 7ths, borrowed).");
    }
    {   // whose notes are compared (small, at the right end of the title row)
        const char* names[3] = {"BASS", "SYN 2", "SYN 3"};
        const float bx = a.x + w - 3 * 52.0f;
        if (bx > a.x + TextW(hd, 12.0f) + 80.0f) {
            Text(dl, ImVec2(bx - TextW("notes of", 11.0f) - 6, a.y + 1), vis::kTextDim, "notes of", 11.0f);
            ImGui::SetCursorScreenPos(ImVec2(bx, a.y - 4));
            for (int i = 0; i < kNumSynthTracks; ++i) {
                if (i) ImGui::SameLine(0, 3);
                bool on = (srcMask_ >> i) & 1;
                if (Check(names[i], &on, 49.0f)) srcMask_ = on ? (srcMask_ | (1 << i)) : (srcMask_ & ~(1 << i));
                if (ImGui::IsItemHovered()) Hint("Tracks whose notes the chords are compared with (MELODY). The bass counts double on the beat.");
            }
        }
    }
    const float cardsTop = a.y + 84.0f;
    const float sentenceH = 92.0f;
    const int cards = std::clamp(static_cast<int>((h - (cardsTop - a.y) - sentenceH) / 76.0f), 1, 4);
    const float cardH = std::min(80.0f, (h - (cardsTop - a.y) - sentenceH) / cards - 6.0f);
    adviceSel_ = std::clamp(adviceSel_, 0, std::max(0, std::min(cards, static_cast<int>(adv.size())) - 1));
    const bool haveMel = mel_.notes > 0;
    for (int i = 0; i < cards && i < static_cast<int>(adv.size()); ++i) {
        const LabAdvice& ad = adv[static_cast<size_t>(i)];
        const ImVec2 ca(a.x, cardsTop + i * (cardH + 6.0f)), cb(a.x + w, ca.y + cardH);
        ImGui::SetCursorScreenPos(ca);
        ImGui::PushID(5000 + i);
        ImGui::InvisibleButton("##card", ImVec2(w, cardH));
        const bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(0)) {
            adviceSel_ = i;
            Select(ad.chord->row, ad.chord->col);
            Audition(app, *ad.chord);
            if (ImGui::IsMouseDoubleClicked(0)) Place(app, *ad.chord, slot.start, slot.len);
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 5.0f) && dragRow_ < 0) { dragRow_ = ad.chord->row; dragCol_ = ad.chord->col; }
        const bool sel = i == adviceSel_;
        dl->AddRectFilled(ca, cb, hov ? IM_COL32(40, 42, 54, 255) : IM_COL32(28, 30, 38, 255), 6.0f);
        dl->AddRect(ca, cb, sel ? vis::kAccent : vis::kLine, 6.0f, 0, sel ? 2.0f : 1.0f);
        const float chipW = std::min(78.0f, w * 0.18f), chipH = std::min(40.0f, cardH - 16.0f);
        const ImVec2 pa(ca.x + 10, ca.y + (cardH - chipH) * 0.5f), pb(pa.x + chipW, pa.y + chipH);
        dl->AddRectFilled(pa, pb, FnFill(ad.fn), 5.0f);
        if (ad.chord->degree == 0) DashedRect(dl, pa, pb, FnEdge(ad.fn)); else dl->AddRect(pa, pb, FnEdge(ad.fn), 5.0f);
        Text(dl, ImVec2(pa.x + chipW * 0.5f - TextW(ad.chord->name.c_str(), 15.0f) * 0.5f, pa.y + chipH * 0.5f - 9), vis::kText, ad.chord->name.c_str(), 15.0f);
        const float lx = pb.x + 10, labW = 62.0f, barW = std::max(40.0f, (cb.x - lx - labW) * 0.42f), tx = lx + labW + barW + 10;
        const float rowH = (cardH - 12.0f) / 3.0f;
        char vt[64], mt[64];
        if (ad.haveIn && prev) std::snprintf(vt, sizeof vt, "move %d semitone%s", ad.in.semitones, ad.in.semitones == 1 ? "" : "s");
        else std::snprintf(vt, sizeof vt, "no chord before");
        if (haveMel) std::snprintf(mt, sizeof mt, "%d of %d notes", ad.covered, ad.total); else std::snprintf(mt, sizeof mt, "no notes here");
        const char* labels[3] = {"FUNCTION", "VOICES", "MELODY"};
        const float vals[3] = {ad.fnFit, ad.voiceFit, haveMel ? ad.melFit : 0.0f};
        const std::string texts[3] = {ad.fnWords, vt, mt};
        for (int r = 0; r < 3; ++r) {
            const float ry = ca.y + 6.0f + r * rowH;
            Text(dl, ImVec2(lx, ry + rowH * 0.5f - 7), vis::kTextDim, labels[r], 11.0f);
            Bar(dl, ImVec2(lx + labW, ry + rowH * 0.5f - 2), barW, 4.0f, vals[r], r == 2 ? vis::kTeal : vis::kViolet);
            Text(dl, ImVec2(tx, ry + rowH * 0.5f - 7), IM_COL32(210, 213, 224, 255), texts[r].c_str(), 11.5f);
        }
        if (hov && !ImGui::IsMouseDown(0)) Hint("%s (%s)  -  click: hear it and read why  -  double-click: put it in bar %d  -  drag it into the loop", ad.chord->name.c_str(), ad.chord->roman.c_str(), slot.start / perBar + 1);
        ImGui::PopID();
    }
    if (!adv.empty()) {   // the sentence for the chosen card
        const float sy = cardsTop + cards * (cardH + 6.0f) + 4.0f;
        dl->AddLine(ImVec2(a.x, sy), ImVec2(a.x + w, sy), vis::kLine, 1.0f);
        const std::string why = LabAdviceSentence(adv[static_cast<size_t>(adviceSel_)], ls, key, scale);
        Wrap(dl, ImVec2(a.x, sy + 8), w, IM_COL32(220, 222, 232, 255), why, 12.5f, 16.0f, std::max(1, static_cast<int>((a.y + h - sy - 8) / 16.0f)));
    }
}

void ChordLabPanel::Draw(AppState& app, float W, float H) {
    if (!opened_) { opened_ = true; Rebuild(app); selBlock_ = -1; labEdits_ = 0; abBefore_ = false; }
    if (!abBefore_) {   // which chords are shown: the edited slot (bank on) or the scene's own (bank off, nothing changed yet)
        const int now = app.project.chordBankOn ? std::clamp(app.project.chordSceneEdit, 0, kNumChordScenes - 1) : -1;
        if (now != jam_) { jam_ = now; selBlock_ = -1; }
    } else if (app.project.chordBankOn) abBefore_ = false;   // something else switched the bank on (undo, a pad): B again
    Rebuild(app);
    if (auditionUntil_ > 0.0 && ImGui::GetTime() > auditionUntil_) { app.engine.ChordAuditionOff(); auditionUntil_ = 0.0; }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const int scene = SceneShown(app);
    const Pattern& pat = app.project.patterns[static_cast<size_t>(scene)];
    const int perBar = PerBar(pat), perBeat = PerBeat(pat);
    const int key = app.project.randomizerKeyRoot, scale = app.project.randomizerScale;
    ImGui::PushFont(nullptr, 14.0f);

    DrawHeader(app, dl, o, W, scene);
    const float top = 36.0f;
    if (soundView_) {
        ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + top));
        ImGui::BeginChild("##chordsound", ImVec2(W, std::max(60.0f, H - top - 2.0f)), ImGuiChildFlags_None, ImGuiWindowFlags_None);
        if (soundBody_) soundBody_(app);
        ImGui::EndChild();
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(o.x, o.y));
        ImGui::Dummy(ImVec2(W, H));
        return;
    }
    // C2: HARMONY and THE LOOP on the left, WHAT FITS on the right (PLAY and a short SOUND column follow in the next parts of C2)
    const float leftW = std::clamp(W * 0.54f, 520.0f, 900.0f);
    const float midX = leftW + 20.0f, midW = std::max(300.0f, W - midX - 2.0f);
    dl->AddLine(ImVec2(o.x + midX - 10, o.y + top), ImVec2(o.x + midX - 10, o.y + H - 4), vis::kLine, 1.0f);

    Track& jt = JamTrack(app);   // A BEFORE: the bank is off, so this is the scene's own chord track (shown, not edited)
    const std::vector<Blk> blocks = Blocks(jt);
    if (selBlock_ >= static_cast<int>(blocks.size())) selBlock_ = -1;
    const Slot slot = AdviceSlot(app, jt, blocks);
    RefreshMatch(app, slot.start, slot.len);
    const bool readOnly = abBefore_;

    // what sounds now (the white frame)
    unsigned nowMask = 0; int nowRoot = -1;
    const int loopSteps = std::max(perBar, jt.useOwnLength ? jt.length : pat.masterLength);
    if (app.sequencer.IsPlaying()) {
        const int pos = app.sequencer.ChordLoopPos(loopSteps);
        for (const Blk& b : blocks) if (pos >= b.start && pos < b.start + b.len) {
            const Step& st = jt.steps[static_cast<size_t>(b.start)];
            const LabChord lc = LabChordFromNotes(st.chordNotes.data(), st.chordSize);
            nowMask = LabMask(lc); nowRoot = lc.root;
        }
    }

    // ---- 1 HARMONY: the palette
    float y = top;
    Text(dl, ImVec2(o.x + 2, o.y + y + 4), vis::kTextDim, "HARMONY \xC2\xB7 WHAT SOUNDS", 12.0f);
    {
        ImGui::SetCursorScreenPos(ImVec2(o.x + 170, o.y + y));
        int nine = nine_ ? 1 : 0;
        static const char* const kNine[2] = {"7THS", "9THS"};
        if (Segmented("##nine", &nine, kNine, 2, 58.0f)) nine_ = nine == 1;
        if (ImGui::IsItemHovered()) Hint("The second row: 7th chords (four voices) or 9th chords (five). A chord can use up to six voices.");
    }
    y += 30.0f;
    const float rowLabelW = 70.0f, gap = 5.0f;
    const float chipW = (leftW - rowLabelW - gap * 6) / 7.0f;
    for (int c = 0; c < 7; ++c) {
        const char* rn = LabRomanDegree(c + 1, true);
        Text(dl, ImVec2(o.x + rowLabelW + c * (chipW + gap) + chipW * 0.5f - TextW(rn, 11.0f) * 0.5f, o.y + y), vis::kTextDim, rn, 11.0f);
    }
    const float palTop = y + 16.0f;
    const float loopMinH = 150.0f;
    const float chipH = std::clamp((H - palTop - 26.0f - loopMinH - gap * 3) / 4.0f, 24.0f, 48.0f);
    static const char* kRowName[4] = {"TRIADS", "7THS", "SUS \xC2\xB7 ADD", "BORROWED"};
    const char* rowNames[4] = {kRowName[0], nine_ ? "9THS" : kRowName[1], kRowName[2], kRowName[3]};
    int clickedRow = -1, clickedCol = -1; bool dbl = false;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    for (int r = 0; r < 4; ++r) {
        const float cy = palTop + r * (chipH + gap);
        Text(dl, ImVec2(o.x + 2, o.y + cy + chipH * 0.5f - 7), vis::kTextDim, rowNames[r], 11.0f);
        for (int c = 0; c < 7; ++c) {
            const LabChord* chip = nullptr;
            for (const LabChord& x : pal_) if (x.row == r && x.col == c) chip = &x;
            if (!chip) continue;
            const LabFn fn = LabFunctionOf(*chip, key, scale);
            const ImVec2 a(o.x + rowLabelW + c * (chipW + gap), o.y + cy), b(a.x + chipW, a.y + chipH);
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(r * 10 + c);
            ImGui::InvisibleButton("##chip", ImVec2(chipW, chipH));
            const bool hov = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked(0)) { clickedRow = r; clickedCol = c; if (ImGui::IsMouseDoubleClicked(0)) dbl = true; }
            if (!readOnly && ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 5.0f) && dragRow_ < 0) { dragRow_ = r; dragCol_ = c; }
            const bool sel = r == selRow_ && c == selCol_;
            const bool sounding = nowRoot >= 0 && chip->root == nowRoot && LabMask(*chip) == nowMask;
            ImU32 fill = FnFill(fn);
            if (hov) fill = IM_COL32(std::min(255, static_cast<int>((fill >> IM_COL32_R_SHIFT) & 0xFF) + 22), std::min(255, static_cast<int>((fill >> IM_COL32_G_SHIFT) & 0xFF) + 22), std::min(255, static_cast<int>((fill >> IM_COL32_B_SHIFT) & 0xFF) + 22), 255);
            dl->AddRectFilled(a, b, fill, 5.0f);
            if (chip->degree == 0) DashedRect(dl, a, b, FnEdge(fn)); else dl->AddRect(a, b, FnEdge(fn), 5.0f);
            if (sounding) dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), IM_COL32(255, 255, 255, 235), 5.0f, 0, 2.0f);
            if (sel) dl->AddRect(ImVec2(a.x - 2, a.y - 2), ImVec2(b.x + 2, b.y + 2), vis::kAccent, 6.0f, 0, 2.0f);
            Text(dl, ImVec2(a.x + 7, a.y + chipH * 0.5f - (chipH >= 32 ? 11 : 9)), IM_COL32(236, 232, 255, 255), chip->name.c_str(), chipH >= 32 ? 15.0f : 13.0f);
            const float rw2 = TextW(chip->roman.c_str(), 10.0f);
            Text(dl, ImVec2(b.x - rw2 - 5, a.y + 3), IM_COL32(200, 196, 230, 200), chip->roman.c_str(), 10.0f);
            if (mel_.notes > 0) Bar(dl, ImVec2(a.x + 7, b.y - 6), (chipW - 14) * 0.62f, 3.0f, chip->fit, IM_COL32(236, 236, 245, 230));
            if (hov && !ImGui::IsMouseDown(0)) {
                char fit[80] = "";
                if (mel_.notes > 0) std::snprintf(fit, sizeof fit, "  -  holds %d of %d notes in bar %d", chip->covered, mel_.notes, slot.start / perBar + 1);
                Hint("%s (%s, %s)%s%s  -  click: hear it  -  drag it into the loop  -  double-click: put it in bar %d",
                     chip->name.c_str(), chip->roman.c_str(), LabFnName(fn), chip->degree == 0 ? ", borrowed" : "", fit, slot.start / perBar + 1);
            }
            ImGui::PopID();
        }
    }
    if (clickedRow >= 0) {
        Select(clickedRow, clickedCol);
        for (const LabChord& c : pal_) if (c.row == clickedRow && c.col == clickedCol) {
            Audition(app, c);
            if (dbl && !readOnly) Place(app, c, slot.start, slot.len);
        }
    }
    {   // legend
        const float ly = palTop + 4 * (chipH + gap) + 2.0f;
        float lx = o.x + 2;
        const LabFn fns[3] = {LabFn::Home, LabFn::Away, LabFn::Tension};
        for (LabFn f : fns) {
            dl->AddRectFilled(ImVec2(lx, o.y + ly + 2), ImVec2(lx + 26, o.y + ly + 13), FnFill(f), 3.0f);
            Text(dl, ImVec2(lx + 31, o.y + ly), vis::kTextDim, LabFnName(f), 11.0f);
            lx += 31 + TextW(LabFnName(f), 11.0f) + 14;
        }
        Text(dl, ImVec2(lx + 4, o.y + ly), vis::kTextDim, "dashed = borrowed    white frame = sounding now    bar = notes it holds", 11.0f);
    }

    // ---- 2 THE LOOP
    const float loopTop = palTop + 4 * (chipH + gap) + 24.0f;
    const int ghostEnd = slot.block < 0 ? slot.start + slot.len : 0;
    const int showSteps = std::min(kMaxSteps, std::max(loopSteps, ghostEnd));
    {
        char hd[64]; std::snprintf(hd, sizeof hd, "THE LOOP \xC2\xB7 %d BAR%s", std::max(1, loopSteps / perBar), loopSteps / perBar == 1 ? "" : "S");
        Text(dl, ImVec2(o.x + 2, o.y + loopTop + 4), vis::kTextDim, hd, 12.0f);
        ImGui::SetCursorScreenPos(ImVec2(o.x + 130, o.y + loopTop));
        ImGui::BeginDisabled(readOnly);
        const auto& progs = AllChordProgressions();
        ImGui::SetNextItemWidth(130.0f);
        if (ChoiceBegin("##startfrom", "START FROM...")) {
            for (int i = 0; i < static_cast<int>(progs.size()); ++i) if (ImGui::Selectable(progs[static_cast<size_t>(i)].name, false)) StartFrom(app, i);
            ChoiceEnd();
        }
        if (ImGui::IsItemHovered()) Hint("Start the loop from a typical progression, in this key.");
        ImGui::SameLine(0, 4);
        if (MiniButton("EMPTY", 58.0f)) ClearJam(app);
        if (ImGui::IsItemHovered()) Hint("Take every chord out of the loop (undoable). The scene's own chords are not touched.");
        ImGui::SameLine(0, 4);
        const char* dls[3] = {"1/2 BAR", "1 BAR", "2 BARS"};
        ImGui::SetNextItemWidth(84.0f);
        Choice("##newlen", &defLen_, dls, 3, 84.0f);
        if (ImGui::IsItemHovered()) Hint("Length of a chord that is dropped into the loop. Drag the orange edge of a block to change it afterwards.");
        ImGui::EndDisabled();
    }
    const float bx0 = o.x + 2, bx1 = o.x + leftW - 2, by0 = o.y + loopTop + 28.0f, bh = std::max(80.0f, H - loopTop - 28.0f - 4.0f);
    const float unit = (bx1 - bx0) / static_cast<float>(showSteps);
    const float lineTop = by0 + 14.0f, lineH = std::min(30.0f, bh * 0.22f);   // the tension line above the blocks
    const float blkTop = lineTop + lineH + 6.0f, blkBot = by0 + bh - 4.0f;
    dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by0 + bh), IM_COL32(20, 22, 28, 255), 5.0f);
    dl->AddRect(ImVec2(bx0, by0), ImVec2(bx1, by0 + bh), vis::kLine, 5.0f);
    for (int s = 0; s <= showSteps; s += perBeat) {
        const bool bar = (s % perBar) == 0;
        dl->AddLine(ImVec2(bx0 + s * unit, by0), ImVec2(bx0 + s * unit, bar ? by0 + bh : by0 + 6.0f), bar ? vis::kLineHi : vis::kGrid, 1.0f);
        if (bar && s < showSteps) { char n[12]; std::snprintf(n, sizeof n, "%d", s / perBar + 1); Text(dl, ImVec2(bx0 + s * unit + 4, by0 + 1), vis::kTextDim, n, 10.0f); }
    }
    Text(dl, ImVec2(bx1 - TextW("tension", 10.0f) - 4, lineTop - 12), vis::kTextDim, "tension", 10.0f);
    Text(dl, ImVec2(bx1 - TextW("home", 10.0f) - 4, lineTop + lineH - 12), vis::kTextDim, "home", 10.0f);
    {   // the tension line: one level per block (home low, tension high)
        ImVec2 last(-1, -1);
        for (const Blk& b : blocks) {
            const Step& st = jt.steps[static_cast<size_t>(b.start)];
            LabChord lc = LabChordFromNotes(st.chordNotes.data(), st.chordSize);
            int deg = 0; for (const LabChord& pc : pal_) if (pc.root == lc.root && LabMask(pc) == LabMask(lc)) { deg = pc.degree; break; }
            lc.degree = deg;
            const float lv = LabFnLevel(LabFunctionOf(lc, key, scale));
            const float yy = lineTop + lineH * (1.0f - lv);
            const ImVec2 p0(bx0 + b.start * unit, yy), p1(bx0 + (b.start + b.len) * unit, yy);
            if (last.x >= 0) dl->AddLine(last, ImVec2(p0.x, p0.y), vis::kViolet, 2.0f);
            dl->AddLine(p0, p1, vis::kViolet, 2.0f);
            last = p1;
        }
    }
    const bool overLoop = !readOnly && mouse.x >= bx0 && mouse.x <= bx1 && mouse.y >= by0 && mouse.y <= by0 + bh;
    int newSel = -2;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const Blk& b = blocks[i];
        const Step& st = jt.steps[static_cast<size_t>(b.start)];
        LabChord lc = LabChordFromNotes(st.chordNotes.data(), st.chordSize);
        std::string roman;
        for (const LabChord& pc : pal_) if (pc.root == lc.root && LabMask(pc) == LabMask(lc)) { lc.degree = pc.degree; roman = pc.roman; break; }
        const LabFn fn = LabFunctionOf(lc, key, scale);
        const ImVec2 a(bx0 + b.start * unit + 2, blkTop), e(bx0 + (b.start + b.len) * unit - 2, blkBot);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(1000 + static_cast<int>(i));
        ImGui::InvisibleButton("##blk", ImVec2(std::max(4.0f, e.x - a.x - 9.0f), e.y - a.y));
        const bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(0)) newSel = static_cast<int>(i);
        if (!readOnly && ImGui::IsItemClicked(1)) { selBlock_ = static_cast<int>(i); ImGui::OpenPopup("##blkmenu"); }
        const bool sel = selBlock_ == static_cast<int>(i);
        dl->AddRectFilled(a, e, FnFill(fn), 5.0f);
        if (lc.degree == 0) DashedRect(dl, a, e, FnEdge(fn)); else dl->AddRect(a, e, FnEdge(fn), 5.0f);
        if (sel) dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(e.x + 1, e.y + 1), vis::kAccent, 5.0f, 0, 2.0f);
        Text(dl, ImVec2(a.x + 8, a.y + 5), vis::kText, lc.name.c_str(), 18.0f);
        {
            char sub[64]; const float bars = static_cast<float>(b.len) / perBar;
            const std::string rn = roman.empty() ? std::string("") : roman + " \xC2\xB7 ";
            if (bars >= 1.0f && std::fabs(bars - std::round(bars)) < 0.01f) std::snprintf(sub, sizeof sub, "%s%d bar%s", rn.c_str(), static_cast<int>(std::round(bars)), std::round(bars) == 1.0f ? "" : "s");
            else std::snprintf(sub, sizeof sub, "%s%d/%d bar", rn.c_str(), std::max(1, b.len), perBar);
            if (e.y - a.y > 44.0f) Text(dl, ImVec2(a.x + 8, a.y + 27), IM_COL32(200, 196, 230, 220), sub, 11.0f);
        }
        {   // the notes under THIS block that THIS chord holds
            LabChord tmp = lc;
            const LabMelody under = LabMelodyFromRange(pat, SrcTracks(), b.start, b.start + b.len);
            LabFitOne(tmp, under);
            const float w = e.x - a.x - 12.0f;
            if (under.notes > 0 && e.y - a.y > 56.0f) {
                Bar(dl, ImVec2(a.x + 8, e.y - 12), std::min(60.0f, w * 0.3f), 3.0f, tmp.fit, vis::kTeal);
                char fb[32]; std::snprintf(fb, sizeof fb, "%d of %d notes", tmp.covered, under.notes);
                if (w > 140.0f) Text(dl, ImVec2(a.x + 14 + std::min(60.0f, w * 0.3f), e.y - 18), IM_COL32(214, 210, 236, 230), fb, 11.0f);
            }
        }
        if (hov && !ImGui::IsMouseDown(0)) Hint("%s (%s)  -  click: select it (WHAT FITS shows what could stand here)  -  drag the orange edge: length  -  right-click: length, split, delete", lc.name.c_str(), LabFnName(fn));
        if (!readOnly) {   // the length handle
            const ImVec2 ha(e.x - 7, a.y), hb(e.x, e.y);
            ImGui::SetCursorScreenPos(ha);
            ImGui::InvisibleButton("##edge", ImVec2(7.0f, e.y - a.y));
            const bool hh = ImGui::IsItemHovered() || resizeBlk_ == static_cast<int>(i);
            if (ImGui::IsItemHovered()) { ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW); if (!ImGui::IsMouseDown(0)) Hint("Drag to make the chord longer or shorter. The next chord gives way (one beat is the smallest)."); }
            dl->AddRectFilled(ImVec2(ha.x + 1, ha.y + 10), ImVec2(hb.x - 1, hb.y - 10), hh ? vis::kAccentHi : vis::kAccent, 3.0f);
            if (ImGui::IsItemActivated()) { resizeBlk_ = static_cast<int>(i); BeginEdit(app); }
            if (resizeBlk_ == static_cast<int>(i) && ImGui::IsItemActive()) {
                const int target = static_cast<int>(std::round((mouse.x - bx0) / unit / perBeat)) * perBeat;
                if (target != b.start + b.len) Resize(app, static_cast<int>(i), target);
            }
            if (resizeBlk_ == static_cast<int>(i) && !ImGui::IsMouseDown(0)) resizeBlk_ = -1;
        }
        ImGui::PopID();
    }
    if (slot.block < 0 && !readOnly) {   // the place WHAT FITS is about: the room after the last chord
        const ImVec2 a(bx0 + slot.start * unit + 2, blkTop), e(bx0 + (slot.start + slot.len) * unit - 2, blkBot);
        DashedRect(dl, a, e, vis::kAccent);
        Text(dl, ImVec2(a.x + 8, a.y + 5), vis::kAccent, blocks.empty() ? "first chord" : "next chord ?", 15.0f);
        if (e.y - a.y > 44.0f) Text(dl, ImVec2(a.x + 8, a.y + 27), vis::kTextDim, "drop here", 11.0f);
    }
    if (newSel >= 0) {
        selBlock_ = newSel == selBlock_ ? -1 : newSel;   // a second click lets go: WHAT FITS goes back to the room after the loop
        adviceSel_ = 0;
        if (selBlock_ >= 0) {
            const Blk& b = blocks[static_cast<size_t>(newSel)];
            const Step& st = jt.steps[static_cast<size_t>(b.start)];
            const LabChord lc = LabChordFromNotes(st.chordNotes.data(), st.chordSize);
            for (const LabChord& c : pal_) if (LabMask(c) == LabMask(lc) && c.root == lc.root) { Select(c.row, c.col); break; }
            int8_t nn[kMaxChordVoices]; const int cnt = std::min<int>(kMaxChordVoices, st.chordSize);
            for (int i = 0; i < cnt; ++i) nn[i] = st.chordNotes[static_cast<size_t>(i)];
            app.engine.ChordAuditionOn(nn, cnt, 0.85f); auditionUntil_ = ImGui::GetTime() + 0.9;
        }
    }
    if (blocks.empty() && readOnly) Text(dl, ImVec2(bx0 + 10, blkTop + 10), vis::kTextDim, "The scene has no chords of its own.", 13.0f);
    if (app.sequencer.IsPlaying()) {   // the playhead: the chord loop runs on its own length
        const int pos = app.sequencer.ChordLoopPos(loopSteps);
        dl->AddLine(ImVec2(bx0 + pos * unit, by0), ImVec2(bx0 + pos * unit, by0 + bh), IM_COL32(255, 255, 255, 190), 2.0f);
    }
    if (ImGui::BeginPopup("##blkmenu")) {
        const bool okSel = selBlock_ >= 0 && selBlock_ < static_cast<int>(blocks.size());
        if (okSel) {
            const int b0 = blocks[static_cast<size_t>(selBlock_)].start;
            auto setLen = [&](int len) { BeginEdit(app); Resize(app, selBlock_, b0 + len); };
            if (ImGui::MenuItem("1/2 bar")) setLen(std::max(perBeat, perBar / 2));
            if (ImGui::MenuItem("1 bar")) setLen(perBar);
            if (ImGui::MenuItem("2 bars")) setLen(perBar * 2);
            ImGui::Separator();
            if (ImGui::MenuItem("Split in two")) Split(app, selBlock_);
            if (ImGui::MenuItem("Delete this chord")) DeleteBlock(app, selBlock_);
        }
        ImGui::EndPopup();
    }
    if (dragRow_ >= 0) {   // a chord dragged towards the loop (from the palette or a WHAT FITS card)
        const LabChord* dc = nullptr; for (const LabChord& c : pal_) if (c.row == dragRow_ && c.col == dragCol_) dc = &c;
        if (dc) {
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            const LabFn fn = LabFunctionOf(*dc, key, scale);
            const ImVec2 a(mouse.x - chipW * 0.5f, mouse.y - chipH * 0.5f);
            fg->AddRectFilled(a, ImVec2(a.x + chipW, a.y + chipH), FnFill(fn), 5.0f);
            fg->AddRect(a, ImVec2(a.x + chipW, a.y + chipH), vis::kAccent, 5.0f, 0, 2.0f);
            ImGui::PushFont(nullptr, 15.0f); fg->AddText(ImVec2(a.x + 7, a.y + 4), vis::kText, dc->name.c_str()); ImGui::PopFont();
            const int s = std::clamp(static_cast<int>(std::floor((mouse.x - bx0) / unit / perBeat)) * perBeat, 0, std::max(0, showSteps - perBeat));
            const int ln = std::min(DefaultLen(app), kMaxSteps - s);
            if (overLoop) {
                fg->AddRectFilled(ImVec2(bx0 + s * unit, blkTop), ImVec2(bx0 + (s + ln) * unit, blkBot), IM_COL32(242, 163, 61, 50), 5.0f);
                fg->AddRect(ImVec2(bx0 + s * unit, blkTop), ImVec2(bx0 + (s + ln) * unit, blkBot), vis::kAccent, 5.0f, 0, 2.0f);
            }
            if (!ImGui::IsMouseDown(0)) {
                if (overLoop) { Audition(app, *dc); Place(app, *dc, s, ln); Select(dragRow_, dragCol_); }
                dragRow_ = dragCol_ = -1;
            }
        } else dragRow_ = dragCol_ = -1;
    }

    // ---- 3 WHAT FITS
    DrawWhatFits(app, dl, ImVec2(o.x + midX, o.y + top + 4.0f), midW, H - top - 8.0f, jt, blocks, slot);

    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y));
    ImGui::Dummy(ImVec2(W, H));
}

}  // namespace t8::desktop
