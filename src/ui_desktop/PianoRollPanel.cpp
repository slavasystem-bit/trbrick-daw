#include "BrickGlyphs.h"
#include "EuclidWidget.h"
#include "PianoRollPanel.h"
#include "Visual.h"
#include "StepEditPopup.h"
#include "MixerPanel.h"
#include "LiveWidgets.h"
#include "ValueField.h"
#include "TrackStyle.h"
#include "StepGlyphs.h"
#include "Interaction.h"
#include "core/Scales.h"
#include "imgui.h"
#include "InfoLine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace t8::desktop {

namespace {

std::string NoteName(int midiNote) {
    static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    int n = ((midiNote % 12) + 12) % 12;
    int octave = midiNote / 12 - 1;
    return std::string(names[n]) + std::to_string(octave);
}
bool IsBlackKey(int note) {
    int n = ((note % 12) + 12) % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}
bool IsC(int note) { return ((note % 12) + 12) % 12 == 0; }

// 0 = outside the scale, 1 = in the scale, 2 = the tonic.
int ScaleDegreeClass(int note, int keyRoot, int scaleIdx) {
    int rel = (((note - keyRoot) % 12) + 12) % 12;
    if (rel == 0) return 2;
    for (int i = 0; i < kScaleDegreeCount[scaleIdx]; ++i)
        if (kScaleIntervals[scaleIdx][i] == rel) return 1;
    return 0;
}

// Whether `rowNote` is currently sounding at this step -- Synth tracks
// use the single Step::note field, Chord uses the whole chordNotes[0..
// chordSize-1] stack (several rows can be lit on the same column at once).
bool NoteActiveAtRow(const Track& t, const Step& s, int rowNote) {
    if (IsNoteTrackType(t.type)) {
        return s.active && s.note == rowNote;
    }
    for (int i = 0; i < s.chordSize; ++i) {
        if (s.chordNotes[i] == rowNote) return true;
    }
    return false;
}

// Applies one (row, col) click/drag-paint cell to `value` (true = turn
// this note on, false = off). For a Synth track this is just Step::note +
// active, same "a synth step only ever has one note" model
// SequencerPanel's SetStepActiveExplicit already assumes. For the Chord
// track, turning a note ON adds it to the chord stack (up to 4, kept
// sorted ascending purely so the grid reads as a stacked chord rather
// than an arbitrary order) and turning the LAST remaining note off
// deactivates the whole step -- same "chordSize==0 means nothing to
// play" convention as everywhere else chordSize is read (see Types.h's
// Step comment).
void ApplyNoteAtRow(AppState& app, int trackIndex, int col, int rowNote, bool value) {
    Track& t = app.StepTrack(app.CurrentPattern(), trackIndex);
    Step& s = t.steps[col];
    if (IsNoteTrackType(t.type)) {
        bool wasOn = s.active && s.note == rowNote;
        if (wasOn == value) return;
        app.PushUndo();
        if (value) {
            s.active = true;
            s.note = static_cast<int8_t>(rowNote);
            if (s.lengthSteps == 0) s.lengthSteps = 1;
        } else {
            s.active = false;
        }
        return;
    }
    // Chord track.
    int idx = -1;
    for (int i = 0; i < s.chordSize; ++i) {
        if (s.chordNotes[i] == rowNote) { idx = i; break; }
    }
    bool present = (idx >= 0);
    if (present == value) return;
    app.PushUndo();
    if (value) {
        if (s.chordSize < kMaxChordVoices) {
            s.chordNotes[s.chordSize] = static_cast<int8_t>(rowNote);
            s.chordSize++;
            std::sort(s.chordNotes.begin(), s.chordNotes.begin() + s.chordSize);
            s.active = true;
        }
    } else {
        for (int i = idx; i < s.chordSize - 1; ++i) s.chordNotes[i] = s.chordNotes[i + 1];
        s.chordSize--;
        if (s.chordSize == 0) s.active = false;
    }
}


// Every distinct pitch used anywhere on the track (within its own length),
// highest first -- the rows ACTIVE mode shows.
std::vector<int> UsedPitches(const Track& t, int trackLen) {
    std::set<int, std::greater<int>> notes;
    for (int i = 0; i < trackLen && i < kMaxSteps; ++i) {
        const Step& s = t.steps[i];
        if (IsNoteTrackType(t.type)) {
            if (s.active) notes.insert(s.note);
        } else {
            for (int k = 0; k < s.chordSize; ++k) notes.insert(s.chordNotes[k]);
        }
    }
    return std::vector<int>(notes.begin(), notes.end());
}

} // namespace

void PianoRollPanel::Draw(AppState& app, const LiveLayout& layout) {
    SceneLock lock(app.sequencer.PatternMutex(), "PIANO ROLL");

    int ti = std::clamp(app.seqCursorTrack, 0, kNumTracks - 1);
    Pattern& pat = app.CurrentPattern();
    Track& t = app.StepTrack(pat, ti);
    auto& mt = app.EditMixer().tracks[ti];

    ImGui::PushID("PianoRollPanel");
    ImVec2 origin = ImGui::GetCursorPos();
    float innerW = ImGui::GetContentRegionAvail().x;
    const float rowsTop = origin.y + layout.headerH;

    if (t.type == TrackType::Drum) {
        ImGui::TextDisabled("(select a SYNTH or CHORD track to edit its notes here)");
        ImGui::PopID();
        return;
    }

    int trackLen = t.useOwnLength ? t.length : pat.masterLength;
    const int keyRoot = std::clamp(app.project.randomizerKeyRoot, 0, kNumKeys - 1);
    const int scaleIdx = std::clamp(app.project.randomizerScale, 0, kNumScales - 1);

    // Keep the visible octave on the track's notes: re-centre when the track
    // changes, and again when its set of pitches changes from outside the roll
    // (a chord progression applied, a randomizer, undo) and none of the new
    // pitches fall inside the current band. Edits made inside the band, or an
    // OCT shift the user chose, are left alone.
    std::vector<int> usedNow = UsedPitches(t, trackLen);
    unsigned long long usedHash = 1469598103934665603ull;
    for (int n : usedNow) usedHash = (usedHash ^ static_cast<unsigned long long>(n + 1)) * 1099511628211ull;
    auto octaveTopFor = [](int note) { return std::clamp((note / 12) * 12 + 11, 23, 127); };
    if (lastTrack_ != ti) {
        lastTrack_ = ti;
        int ref = usedNow.empty() ? (t.type == TrackType::Chord ? 60 : (ti == kNumDrumTracks ? 40 : 64)) : usedNow.front();
        topNote_ = octaveTopFor(ref);
        usedHash_ = usedHash;
        recenter_ = true;
        if (!usedNow.empty()) {
            int bestCount = 0;
            for (int top = 23; top <= 127; top += 12) {
                int c = 0;
                for (int n : usedNow) if (n <= top && n > top - 12) ++c;
                if (c > bestCount) { bestCount = c; topNote_ = top; }
            }
        }
    } else if (usedHash != usedHash_) {
        usedHash_ = usedHash;
        recenter_ = true;
        // Show the octave that holds the most of the track's pitches (the
        // current one wins ties, so a user's own OCT choice survives edits).
        auto countIn = [&](int top) { int c = 0; for (int n : usedNow) if (n <= top && n > top - 12) ++c; return c; };
        if (!usedNow.empty()) {
            int bestTop = topNote_, bestCount = countIn(topNote_);
            for (int top = 23; top <= 127; top += 12) {
                int c = countIn(top);
                if (c > bestCount) { bestCount = c; bestTop = top; }
            }
            topNote_ = bestTop;
        }
    }

    // ---- header ----------------------------------------------------------
    ImGui::SetCursorPos(ImVec2(origin.x, origin.y + (layout.headerH - ImGui::GetFrameHeight()) * 0.5f));
    if (ImGui::Button("X  MIXER")) app.deskRollClosed = true;
    if (ImGui::IsItemHovered()) Hint("Close the piano roll and bring the mixer back");
    ImGui::SameLine(0, 8);
    ImGui::AlignTextToFramePadding();   // UI-7: no ROLL word any more (the pictogram and the name say whose notes these are), so the row fits at 1440 px
    {   // v44.37: the instrument's pictogram before its name
        const ImVec2 p = ImGui::GetCursorScreenPos();
        DrawInstrumentGlyph(ImGui::GetWindowDrawList(), std::clamp(app.seqCursorTrack, 0, kNumTracks - 1), ImVec2(p.x + 8.0f, p.y + ImGui::GetFrameHeight() * 0.5f), 14.0f, TrackColor(std::clamp(app.seqCursorTrack, 0, kNumTracks - 1)));
        ImGui::Dummy(ImVec2(18.0f, 1.0f));
        ImGui::SameLine(0, 0);
    }
    ImGui::TextColored(ImVec4(0.95f, 0.64f, 0.24f, 1.0f), "%.8s", TrackDisplayName(t).c_str());
    // UI-7: the rows-to-show choices are ALL / ACTIVE / SCALE+USED (were ALL-NOTES / ACTIVE-ONLY), so EUCLID n/m and GHOST fit at 1440 px
    ImGui::SameLine(0, 10);
    if (ToggleButton("ALL", !activeOnly_ && !scaleUsed_, ImVec2(0, 0), ToggleTab())) { activeOnly_ = false; scaleUsed_ = false; }
    if (ImGui::IsItemHovered()) Hint("Show every pitch (all notes): click an empty row to add a new note.");
    ImGui::SameLine(0, 4);
    if (ToggleButton("ACTIVE", activeOnly_, ImVec2(0, 0), ToggleTab())) { activeOnly_ = true; scaleUsed_ = false; }
    if (ImGui::IsItemHovered()) Hint("Show only the pitches that play on this track.\nSwitch back to ALL to add a new pitch.");
    ImGui::SameLine(0, 4);
    if (ToggleButton("SCALE+USED", scaleUsed_, ImVec2(0, 0), ToggleTab())) { scaleUsed_ = !scaleUsed_; if (scaleUsed_) { activeOnly_ = false; recenter_ = true; } }
    if (ImGui::IsItemHovered()) Hint("Every note of the key / scale together with every pitch this track plays,\nacross several octaves (mouse wheel to scroll). Notes outside the scale are red,\nrows show the scale degree, other tracks' notes appear as ghosts.");
    ImGui::SameLine(0, 4);
    {
        static const char* kScaleViewLabel[3] = {"SCALE: OFF", "SCALE: MARK", "SCALE: ONLY"};
        if (ToggleButton(kScaleViewLabel[scaleView_], scaleView_ != 0, ImVec2(0, 0), ToggleTab())) scaleView_ = (scaleView_ + 1) % 3;
        if (ImGui::IsItemHovered())
            Hint("Notes of the project's key / scale (set in the header).\nMARK = shade scale rows, tonic highlighted.\nONLY = list just the scale notes (ALL view).");
    }
    ImGui::SameLine(0, 8);
    if (ToggleButton("LANE", laneOn_, ImVec2(0, 0), ToggleTab())) laneOn_ = !laneOn_;
    if (ImGui::IsItemHovered()) Hint("Velocity / nudge of every step as bars under the notes\nSeveral notes: Shift+drag = rubber band, Shift+click = add / remove, Cmd+A = all. Then drag them (Option = copy),\nUp / Down = semitone (Shift = octave), Left / Right = step (Shift = beat), Delete, Cmd+D = duplicate.");
    ImGui::SameLine(0, 4);
    if (ImGui::Button("HUMANIZE")) HumanizeLane(app, ti, laneKind_, humanizePct_);
    if (ImGui::IsItemHovered())
        Hint("Random jitter of the %s of every note on this track (%.0f %% of the range, drag the number to change).\nSwitch the lane under the notes between VEL and NUDGE to pick which. Undoable.",
                          laneKind_ == LaneKind::Velocity ? "velocity" : "timing", humanizePct_);
    ImGui::SameLine(0, 2);
    {
        ValueSpec sp; sp.lo = 2.0f; sp.hi = 60.0f; sp.step = 0.5f; sp.wheel = 1.0f; sp.def = 15.0f; sp.integer = true; sp.unit = "%";
        ValueField("##rollhum", &humanizePct_, sp, 46.0f);
        if (ImGui::IsItemHovered()) Hint("HUMANIZE amount, 2-60 %% of the range. Option+click = 15.");
    }
    ImGui::SameLine(0, 4);
    if (ImGui::Button("SNAP")) app.SnapTrackToScale(ti);
    if (ImGui::IsItemHovered()) Hint("Move every note of this track that is outside the header's key / scale\nto the nearest scale note (undoable).");
    ImGui::SameLine(0, 4);
    {   // UI-7: EUCLID n/m and GHOST are the last two on the row; when they would run past the edge (SCALE+USED with a spread on, a
        // narrow window) they are drawn smaller (not below 10 points) instead of being cut to "EUC"
        const float pad = ImGui::GetStyle().FramePadding.x * 2.0f;
        const float need = ImGui::CalcTextSize("EUCLID 16/64").x + pad + (scaleUsed_ ? ImGui::CalcTextSize("GHOST").x + pad + 4.0f : 0.0f);
        const float avail = origin.x + innerW - ImGui::GetCursorPosX();
        const float fs = ImGui::GetFontSize();
        const float fit = need > avail && need > 0.0f ? std::max(10.0f, fs * avail / need) : fs;
        DrawEuclidTools(app, ti, fit < fs ? fit : 0.0f);
        ImGui::PushFont(nullptr, fit);
        if (scaleUsed_) {
            ImGui::SameLine(0, 4);
            if (ToggleButton("GHOST", ghost_, ImVec2(0, 0), ToggleTab())) ghost_ = !ghost_;
            if (ImGui::IsItemHovered()) Hint("Show the notes of the other melodic tracks as faint outlines.");
        }
        ImGui::PopFont();
    }
    // The Mixer is hidden while the roll shows, so keep this track's VOL / PAN here.
    float usedX = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x; // end of the last button, window-local
    float remain = origin.x + innerW - usedX - 24.0f;
    if (remain > 200.0f) {
        float sw = (remain - 120.0f) * 0.5f;
        ImGui::SameLine(0, 14);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("VOL");
        ImGui::SameLine(0, 4);
        float vol = mt.volume;
        ImGui::SetCursorPosY(origin.y + (layout.headerH - (layout.rowH - 6.0f)) * 0.5f);
        {
            ValueSpec sp = BarSpec(0.0f, 1.0f, 0.8f, sw); sp.height = layout.rowH - 6.0f; sp.meter = mt.peak.load(); sp.color = TrackColor(ti); sp.scale = 100.0f;
            const ValueResult r = ValueField("rollvol", &vol, sp, sw);
            if (r.started) app.PushUndo();
            if (r.changed) { mt.volume = std::clamp(vol, 0.0f, 1.0f); t.volume = mt.volume; }
            if (r.cancelled) app.DropLastUndo();
        }
        ImGui::SameLine(0, 10);
        ImGui::SetCursorPosY(origin.y + (layout.headerH - ImGui::GetFrameHeight()) * 0.5f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("PAN");
        ImGui::SameLine(0, 4);
        float pan = mt.pan;
        ImGui::SetCursorPosY(origin.y + (layout.headerH - (layout.rowH - 6.0f)) * 0.5f);
        {
            ValueSpec sp = BarSpec(-1.0f, 1.0f, 0.0f, sw); sp.height = layout.rowH - 6.0f; sp.fromCenter = true; sp.color = TrackColor(ti); sp.scale = 100.0f;
            const ValueResult r = ValueField("rollpan", &pan, sp, sw);
            if (r.started) app.PushUndo();
            if (r.changed) { mt.pan = std::clamp(pan, -1.0f, 1.0f); t.pan = mt.pan; }
            if (r.cancelled) app.DropLastUndo();
        }
    }

    // ---- rows ------------------------------------------------------------
    std::vector<int> rows;
    // mouse wheel scrolls the pitch range of the plain views (SCALE+USED has its own scroll below)
    if (!activeOnly_ && !scaleUsed_ && ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
        const int d = static_cast<int>(std::lround(ImGui::GetIO().MouseWheel * 2.0f));
        topNote_ = std::clamp(topNote_ + (d != 0 ? d : (ImGui::GetIO().MouseWheel > 0 ? 1 : -1)), 23, 127);
    }
    const float laneH = laneOn_ ? 74.0f : 0.0f;          // the velocity / nudge lane takes the bottom of the body
    const float bodyH0 = layout.rowH * kNumTracks - laneH;
    int poolSize = 0, poolStart = 0;
    if (activeOnly_) {
        rows = usedNow;
    } else if (scaleUsed_) {
        std::set<int, std::greater<int>> pool;
        for (int n = 127; n >= 12; --n) if (ScaleDegreeClass(n, keyRoot, scaleIdx) != 0 && n >= 21 && n <= 108) pool.insert(n);
        for (int n : usedNow) pool.insert(n);
        std::vector<int> pv(pool.begin(), pool.end());
        poolSize = static_cast<int>(pv.size());
        const int maxRowsP = std::max(1, static_cast<int>(bodyH0 / 13.0f));
        const int winRows = std::min(maxRowsP, poolSize);
        if (recenter_) {
            int centre = usedNow.empty() ? (t.type == TrackType::Chord ? 60 : (ti == kNumDrumTracks ? 40 : 64))
                                         : (usedNow.front() + usedNow.back()) / 2;
            int bi = 0, bd = 1000;
            for (int i = 0; i < poolSize; ++i) { int d = std::abs(pv[i] - centre); if (d < bd) { bd = d; bi = i; } }
            scrollIdx_ = bi - winRows / 2;
            recenter_ = false;
        }
        if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f) scrollIdx_ -= static_cast<int>(ImGui::GetIO().MouseWheel * 2.0f);
        scrollIdx_ = std::clamp(scrollIdx_, 0, std::max(0, poolSize - winRows));
        poolStart = scrollIdx_;
        for (int i = 0; i < winRows; ++i) rows.push_back(pv[poolStart + i]);
    } else if (scaleView_ == 2) {
        for (int r = 0; r < 12; ++r)
            if (ScaleDegreeClass(topNote_ - r, keyRoot, scaleIdx) != 0) rows.push_back(topNote_ - r);
    } else {
        for (int r = 0; r < 12; ++r) rows.push_back(topNote_ - r);
    }
    const float bodyH = layout.rowH * kNumTracks - laneH;
    int maxRows = std::max(1, static_cast<int>(bodyH / (scaleUsed_ ? 13.0f : 9.0f)));
    if (static_cast<int>(rows.size()) > maxRows) rows.resize(maxRows);
    float rh = rows.empty() ? layout.rowH : std::clamp(bodyH / std::max<int>(rows.size(), 5), scaleUsed_ ? 13.0f : 9.0f, layout.rowH);

    const int gridSpb = ClampGrid(pat.stepsPerBeat);
    const int kPageSteps = PageStepsFor(gridSpb), kGroup = GroupStepsFor(gridSpb);
    const float kLabelW = 44.0f, kCellGap = 2.0f, kGroupGap = 6.0f;
    float cellW = std::max(8.0f, CellWidthFor(innerW, kLabelW, kCellGap, kGroupGap, kPageSteps, kGroup));
    const int base = std::clamp(app.deskStepPage, 0, std::max(0, (kMaxSteps / kPageSteps) - 1)) * kPageSteps;
    int playStep = app.sequencer.StepForTrack(ti);
    bool playing = app.sequencer.IsPlaying() && app.EditIsPlaying();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImVec2 win = ImGui::GetWindowPos();

    // ---- U6: the selection follows the track and the scene; keys act on it ----------------------------------------------------------
    if (selTrack_ != ti || selPattern_ != app.project.currentPattern) { sel_.clear(); selTrack_ = ti; selPattern_ = app.project.currentPattern; moving_ = marquee_ = false; keysFocus_ = false; }
    if (!moving_ && !marquee_) PruneNotes(t, sel_);
    // the roll keeps the keys after a click in it (or Cmd+A), not only while the mouse is over it; a click elsewhere gives them back
    const bool rollHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) keysFocus_ = rollHovered;
    const bool popupUp = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);   // a window on top keeps its own keys
    const bool rollHot = RollOwnsKeys(rollHovered, keysFocus_ && !popupUp, ImGui::GetIO().WantTextInput, moving_ || marquee_ || resizing_);
    const bool editHot = rollHot && rollHovered;   // Cmd+A / Delete / Cmd+D / Esc stay with the panel under the mouse (the bricks strip has them too)
    if (rollHot && !sel_.empty()) app.rollKeysFrame = ImGui::GetFrameCount();   // the arrow keys are for the selected notes, not for the instrument block
    // C1 (UI-8): with the mouse over the roll, Cmd+C copies the selected notes and Cmd+V pastes them (Shortcuts leaves both to the roll then)
    if (editHot && !sel_.empty()) app.rollCopyFrame = ImGui::GetFrameCount();
    if (editHot && !noteClip_.empty()) app.rollPasteFrame = ImGui::GetFrameCount();
    if (editHot && interaction::IsCmd() && !ImGui::GetIO().KeyShift) {
        if (!sel_.empty() && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            noteClip_ = CopyNotes(t, sel_);
            app.SetStatus(std::to_string(noteClip_.size()) + " NOTES COPIED (CMD+V PASTES WHERE THE MOUSE IS)");
        } else if (!noteClip_.empty() && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
            int at = noteClip_.front().relCol;   // the mouse column on this page; off the grid: where the notes were copied from
            {
                const PageGeom g{origin.x + kLabelW, cellW, kCellGap, kGroupGap, base, kPageSteps, kGroup};
                const float mx = ImGui::GetIO().MousePos.x - win.x + ImGui::GetScrollX();
                if (mx >= g.x0) at = base + g.ColAtX(mx);
            }
            app.PushUndo();
            std::vector<NoteRef> pasted = PasteNotes(t, noteClip_, at, trackLen);
            if (pasted.empty()) { app.DropLastUndo(); app.SetStatus("THE COPIED NOTES DO NOT FIT THIS TRACK"); }
            else { sel_ = pasted; keysFocus_ = true; app.SetStatus(std::to_string(sel_.size()) + " NOTES PASTED"); }
        }
    }
    if (rollHot) {
        const bool cmdKey = interaction::IsCmd(), shiftKey = ImGui::GetIO().KeyShift;
        if (editHot && cmdKey && ImGui::IsKeyPressed(ImGuiKey_A, false)) { sel_ = AllNotes(t, trackLen); keysFocus_ = true; app.SetStatus(std::to_string(sel_.size()) + " NOTES SELECTED"); }
        if (!sel_.empty()) {
            int dc = 0, dp = 0;
            if (!cmdKey) {
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) dp = shiftKey ? 12 : 1;
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) dp = shiftKey ? -12 : -1;
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) dc = shiftKey ? ClampGrid(pat.stepsPerBeat) : 1;
                if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) dc = shiftKey ? -ClampGrid(pat.stepsPerBeat) : -1;
            }
            if (dc != 0 || dp != 0) {
                ClampNoteDelta(sel_, trackLen, &dc, &dp);
                if (dc != 0 || dp != 0) {
                    app.PushUndo();
                    const StepSnapshot snap = t.steps;
                    sel_ = MoveNotes(t, snap, sel_, dc, dp, false, trackLen);
                    // the view follows the selection only when none of it is on screen any more (the middle pitch decides)
                    if (!activeOnly_ && !scaleUsed_) {
                        std::vector<int> ps; for (const NoteRef& n : sel_) ps.push_back(n.pitch);
                        std::sort(ps.begin(), ps.end());
                        bool seen = false; for (int pp : ps) if (pp <= topNote_ && pp > topNote_ - 12) seen = true;
                        if (!seen) topNote_ = octaveTopFor(ps[ps.size() / 2]);
                    }
                }
            }
            if (!editHot) {
                // only the arrows act while the mouse is away from the roll
            } else if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
                app.PushUndo(); RemoveNotes(t, sel_); app.SetStatus(std::to_string(sel_.size()) + " NOTES REMOVED"); sel_.clear();
            } else if (cmdKey && ImGui::IsKeyPressed(ImGuiKey_D, false)) {
                app.PushUndo();
                if (DuplicateNotes(t, sel_, trackLen)) app.SetStatus(std::to_string(sel_.size()) + " NOTES DUPLICATED");
                else { app.DropLastUndo(); app.SetStatus("NO ROOM TO DUPLICATE THE NOTES"); }
            } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) sel_.clear();
        }
    }

    // ---- drag a note's right edge to set its length (same gesture as the Sequencer)
    const PageGeom geom{origin.x + kLabelW, cellW, kCellGap, kGroupGap, base, kPageSteps, kGroup};
    const float gripW = std::min(9.0f, cellW * 0.4f);
    bool gripPressed = false;
    {
        const ImGuiIO& io = ImGui::GetIO();
        const float sx = ImGui::GetScrollX(), sy = ImGui::GetScrollY();
        if (resizing_) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                Step& rs = t.steps[resizeStep_];
                int endCol = base + geom.ColAtX(io.MousePos.x - win.x + sx);
                int len = std::clamp(endCol - resizeStep_ + 1, 1, std::max(1, trackLen - resizeStep_));
                if (len != rs.lengthSteps) {
                    if (!resizeUndone_) { app.PushUndo(); resizeUndone_ = true; }
                    if (resizeGroup_) ResizeNotes(t, snap_, sel_, len - resizeOrig_, trackLen);   // U6: the same change for every selected note
                    else rs.lengthSteps = static_cast<uint8_t>(len);
                }
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                ValueTip("length %d", len);
            } else {
                resizing_ = false;
            }
        } else if (ImGui::IsWindowHovered()) {
            for (size_t ri = 0; ri < rows.size() && !gripPressed; ++ri) {
                int rowNote = rows[ri];
                float y0 = win.y + rowsTop + ri * rh - sy, y1 = y0 + rh - 2.0f;
                if (io.MousePos.y < y0 || io.MousePos.y > y1) continue;
                for (int c = 0; c < kPageSteps; ++c) {
                    int col = base + c;
                    if (col >= trackLen || col >= kMaxSteps) break;
                    const Step& st = t.steps[col];
                    if (!st.active || !NoteActiveAtRow(t, st, rowNote)) continue;
                    int ec = c + StepSpan(st, col, trackLen) - 1;
                    if (ec > kPageSteps - 1) continue;
                    float x1 = win.x + geom.X(ec) + cellW - sx, x0 = x1 - gripW;
                    if (io.MousePos.x >= x0 && io.MousePos.x <= x1 + 1.0f) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                            resizing_ = true; resizeUndone_ = false; resizeStep_ = col;
                            resizeGroup_ = false;
                            if (sel_.size() > 1) for (const NoteRef& sn : sel_) if (sn.col == col) resizeGroup_ = true;
                            if (resizeGroup_) { snap_ = t.steps; resizeOrig_ = NoteLength(t, col); }
                            app.seqCursorTrack = ti; app.seqCursorStep = col;
                            gripPressed = true;
                        }
                        break;
                    }
                }
            }
        }
    }
    // ---- U6: moving the selection with the mouse, and the rubber band --------------------------------------------------------------
    {
        const ImGuiIO& io = ImGui::GetIO();
        const float sx = ImGui::GetScrollX(), sy = ImGui::GetScrollY();
        if (moving_) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                int colNow = base + geom.ColAtX(io.MousePos.x - win.x + sx);
                int ri = static_cast<int>(std::floor((io.MousePos.y - (win.y + rowsTop - sy)) / rh));
                ri = std::clamp(ri, 0, std::max(0, static_cast<int>(rows.size()) - 1));
                int dc = colNow - moveCol0_, dp = rows.empty() ? 0 : rows[static_cast<size_t>(ri)] - movePitch0_;
                ClampNoteDelta(moveSel0_, trackLen, &dc, &dp);
                if (dc != 0 || dp != 0 || moveMoved_) {
                    if (!moveUndone_) { app.PushUndo(); moveUndone_ = true; }
                    moveMoved_ = true;
                    sel_ = MoveNotes(t, snap_, moveSel0_, dc, dp, moveCopy_, trackLen);
                    ValueTip("%s%+d steps  %+d semitones", moveCopy_ ? "copy  " : "", dc, dp);
                }
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            } else {
                if (!moveMoved_) {   // a click on a selected note without moving: it becomes the only selected one
                    sel_.clear(); sel_.push_back({moveCol0_, movePitch0_});
                }
                moving_ = false;
            }
        }
        if (marquee_) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const ImVec2 a(std::min(marqueeP0_.x, io.MousePos.x), std::min(marqueeP0_.y, io.MousePos.y)), b(std::max(marqueeP0_.x, io.MousePos.x), std::max(marqueeP0_.y, io.MousePos.y));
                ImGui::GetWindowDrawList()->AddRectFilled(a, b, IM_COL32(255, 255, 255, 30), 2.0f);
                ImGui::GetWindowDrawList()->AddRect(a, b, IM_COL32(255, 255, 255, 200), 2.0f, 0, 1.2f);
                ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
            } else {
                marquee_ = false;
                if (!shiftOnNote_ || std::fabs(io.MousePos.x - marqueeP0_.x) + std::fabs(io.MousePos.y - marqueeP0_.y) > 4.0f) {} // band result already applied
                else SelToggle(shiftNote_.col, shiftNote_.pitch);
            }
        }
    }
    const bool cellsLocked = gripPressed || resizing_ || moving_ || marquee_;
    const ImU32 trackCol = TrackColor(ti);

    if (rows.empty()) {
        ImGui::SetCursorPos(ImVec2(origin.x + 8.0f, rowsTop + 8.0f));
        ImGui::TextDisabled("No notes on this track yet - switch to ALL and click a cell (mouse wheel scrolls the pitches).");
    }
    for (size_t ri = 0; ri < rows.size(); ++ri) {
        int rowNote = rows[ri];
        if (rowNote < 0 || rowNote > 127) continue;
        float ry = rowsTop + ri * rh;
        ImGui::PushID(static_cast<int>(ri));

        ImGui::SetCursorPos(ImVec2(origin.x, ry + (rh - ImGui::GetTextLineHeight()) * 0.5f));
        const int degClass = ScaleDegreeClass(rowNote, keyRoot, scaleIdx);
        ImVec4 labelCol = IsC(rowNote) ? ImVec4(0.95f, 0.64f, 0.24f, 1.0f) : ImVec4(0.72f, 0.73f, 0.80f, 1.0f);
        if (scaleView_ != 0) {
            if (degClass == 2) labelCol = ImVec4(1.0f, 0.86f, 0.35f, 1.0f);
            else if (degClass == 1) labelCol = ImVec4(0.90f, 0.92f, 0.97f, 1.0f);
            else labelCol = ImVec4(0.42f, 0.43f, 0.50f, 1.0f);
        }
        const bool usedHere = std::find(usedNow.begin(), usedNow.end(), rowNote) != usedNow.end();
        const bool outOfKey = degClass == 0;
        if (scaleUsed_) {
            if (outOfKey) labelCol = ImVec4(1.0f, 0.38f, 0.38f, 1.0f);
            else if (degClass == 1) labelCol = ImVec4(0.90f, 0.92f, 0.97f, 1.0f);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, labelCol);
        ImGui::TextUnformatted(NoteName(rowNote).c_str());
        ImGui::PopStyleColor();
        if (scaleUsed_ && rh >= 12.0f) {
            // scale degree (1..7) or "!" for a played note outside the scale
            char dg[4] = "!";
            if (!outOfKey) {
                int rel = (((rowNote - keyRoot) % 12) + 12) % 12, d = 0;
                for (int q = 0; q < kScaleDegreeCount[scaleIdx]; ++q) if (kScaleIntervals[scaleIdx][q] == rel) { d = q + 1; break; }
                std::snprintf(dg, sizeof dg, "%d", d);
            }
            ImVec2 tp(win.x + origin.x + kLabelW - 12.0f, win.y + ry + (rh - ImGui::GetTextLineHeight()) * 0.5f - ImGui::GetScrollY());
            dl->AddText(tp, outOfKey ? IM_COL32(255, 96, 96, 255) : IM_COL32(150, 154, 170, 200), dg);
            if (IsC(rowNote) && ri > 0)
                dl->AddLine(ImVec2(win.x + origin.x, win.y + ry - 1.0f - ImGui::GetScrollY()), ImVec2(win.x + origin.x + innerW, win.y + ry - 1.0f - ImGui::GetScrollY()), vis::kLineHi, 1.0f);
        }
        (void)usedHere;

        float cx = origin.x + kLabelW;
        for (int c = 0; c < kPageSteps; ++c) {
            int col = base + c;
            if (c > 0) cx += kCellGap + ((c % kGroup == 0) ? kGroupGap : 0.0f);
            if (col >= kMaxSteps) { cx += cellW; continue; }
            bool inRange = col < trackLen;
            Step& s = t.steps[col];
            bool on = NoteActiveAtRow(t, s, rowNote);

            ImGui::SetCursorPos(ImVec2(cx, ry));
            ImVec2 size(cellW, rh - 2.0f);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::PushID(c);
            ImGui::InvisibleButton("cell", size);
            bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);   // painting: the pressed cell blocks plain hover
            ImGui::PopID();

            if (cellsLocked) {
                // a length drag owns the mouse
            } else if (inRange && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && (ImGui::GetIO().KeyShift || interaction::IsCmd())) {
                // Shift / Cmd: a click adds / removes this note in the selection, a drag draws the rubber band (adds to the selection)
                app.seqCursorTrack = ti; app.seqCursorStep = col;
                marquee_ = true; marqueeP0_ = ImGui::GetIO().MousePos; marqueeBase_ = sel_; marqueeNew_.clear();
                shiftOnNote_ = on; shiftNote_ = {col, rowNote};
            } else if (inRange && hovered && on && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && SelHas(col, rowNote)) {
                // a plain press on a selected note: drag moves the whole selection (Option = a copy)
                app.seqCursorTrack = ti; app.seqCursorStep = col;
                moving_ = true; moveMoved_ = false; moveUndone_ = false; moveCopy_ = interaction::IsReset();
                snap_ = t.steps; moveSel0_ = sel_; moveCol0_ = col; movePitch0_ = rowNote;
            } else if (inRange && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                sel_.clear();   // a plain click elsewhere ends the selection and paints as before
                app.seqCursorTrack = ti;
                app.seqCursorStep = col;
                bool newValue = !on;
                ApplyNoteAtRow(app, ti, col, rowNote, newValue);
                dragPainting_ = true;
                dragPaintValue_ = newValue;
                dragLastRow_ = static_cast<int>(ri);
                dragLastCol_ = col;
            } else if (inRange && dragPainting_ && hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                       !(dragLastRow_ == static_cast<int>(ri) && dragLastCol_ == col)) {
                ApplyNoteAtRow(app, ti, col, rowNote, dragPaintValue_);
                dragLastRow_ = static_cast<int>(ri);
                dragLastCol_ = col;
            }
            if (!cellsLocked && inRange && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                app.seqCursorTrack = ti;
                app.seqCursorStep = col;
                popupStep_ = col;
                popupRow_ = rowNote;
                popupOpenRequest_ = true;
            }

            ImVec2 p1(p0.x + size.x, p0.y + size.y);
            if (inRange && on) {
                DrawStepFace(dl, p0, p1, StepIsAccent(s) ? ColorLighten(trackCol, 0.35f) : trackCol, s.probability, StepIsAccent(s), 3.0f);
                if (rh >= 13.0f) {
                    DrawStepBadges(dl, p0, p1, s.ratchet, s.probability);
                    DrawMicroBadges(dl, p0, p1, s.nudge, StepIsGhost(s));
                    if (s.slide) DrawTieGlyph(dl, ImVec2(p0.x + 6.0f, p1.y - 5.0f), IM_COL32(18, 20, 26, 230));
                }
            } else {
                ImU32 off = IsBlackKey(rowNote) ? vis::kInset : vis::kCellHover;
                if (!inRange) off = vis::kInset;
                else if (scaleUsed_) {
                    if (degClass == 2) off = ColorAlpha(trackCol, 70);
                    else if (degClass == 1) off = vis::kCellHover;
                    else off = IM_COL32(120, 40, 44, 90);
                }
                else if (scaleView_ != 0) {
                    if (degClass == 0) off = vis::kInset;
                    else if (degClass == 2) off = ColorAlpha(trackCol, 70);
                    else off = vis::kCellHover;
                }
                dl->AddRectFilled(p0, p1, off, 3.0f);
            }
            if (scaleUsed_ && ghost_ && inRange && !on) {
                for (int gt = kNumDrumTracks; gt < kNumTracks; ++gt) {
                    if (gt == ti) continue;
                    const Track& og = app.StepTrack(pat, gt);
                    const Step& os = og.steps[col];
                    if (!os.active || !NoteActiveAtRow(og, os, rowNote)) continue;
                    dl->AddRect(ImVec2(p0.x + 2.0f, p0.y + 2.0f), ImVec2(p1.x - 2.0f, p1.y - 2.0f), ColorAlpha(TrackColor(gt), 150), 2.0f, 0, 1.2f);
                }
            }
            if (scaleUsed_ && inRange && on && degClass == 0)
                dl->AddRect(p0, p1, IM_COL32(255, 96, 96, 255), 3.0f, 0, 1.6f);
            if (inRange && on && SelHas(col, rowNote)) dl->AddRect(ImVec2(p0.x - 1.0f, p0.y - 1.0f), ImVec2(p1.x + 1.0f, p1.y + 1.0f), IM_COL32(255, 255, 255, 255), 3.0f, 0, 2.0f);
            if (marquee_ && inRange && on) {
                const ImVec2 mp = ImGui::GetIO().MousePos;
                const float ax = std::min(marqueeP0_.x, mp.x), bx = std::max(marqueeP0_.x, mp.x), ay = std::min(marqueeP0_.y, mp.y), by = std::max(marqueeP0_.y, mp.y);
                if (p1.x > ax && p0.x < bx && p1.y > ay && p0.y < by) marqueeNew_.push_back({col, rowNote});
            }
            if (playing && inRange && playStep == col) dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 230), 3.0f, 0, 2.0f);
            cx += cellW;
        }
        // note-length bars: tail across the cells a held note covers + end grip
        for (int col = 0; col < std::min(trackLen, base + kPageSteps); ++col) {
            const Step& st = t.steps[col];
            if (!st.active || !NoteActiveAtRow(t, st, rowNote)) continue;
            int span = StepSpan(st, col, trackLen);
            int sc = col - base, ec = col + span - 1 - base;
            if (ec < 0) continue;
            const float sx = ImGui::GetScrollX();
            const float ty0 = win.y + ry - ImGui::GetScrollY(), ty1 = ty0 + rh - 2.0f;
            if (span > 1) {
                int to = std::min(ec, kPageSteps - 1);
                float fx = (sc >= 0) ? geom.X(sc) + cellW : geom.X(0);
                float tx = geom.X(to) + cellW;
                if (tx > fx + 0.5f)
                    dl->AddRectFilled(ImVec2(win.x + fx - sx - 1.0f, ty0 + 1.0f), ImVec2(win.x + tx - sx, ty1 - 1.0f),
                                      ColorAlpha(trackCol, st.probability < 100 ? 60 : 105), 3.0f);
            }
            if (ec <= kPageSteps - 1) {
                float gx = win.x + geom.X(ec) + cellW - sx;
                bool hot = resizing_ && resizeStep_ == col;
                dl->AddRectFilled(ImVec2(gx - 3.0f, ty0 + 2.0f), ImVec2(gx - 1.0f, ty1 - 2.0f),
                                  hot ? IM_COL32(255, 255, 255, 255) : ColorAlpha(ColorLighten(trackCol, 0.6f), 230), 1.0f);
                if (rh >= 13.0f && StepIsTied(t, st, trackLen) && !st.slide)
                    DrawTieGlyph(dl, ImVec2(gx - 10.0f, ty1 - 5.0f), IM_COL32(18, 20, 26, 230));
            } else {
                float gx = win.x + geom.X(kPageSteps - 1) + cellW - sx;
                dl->AddTriangleFilled(ImVec2(gx - 6.0f, ty0 + 2.0f), ImVec2(gx - 6.0f, ty1 - 2.0f),
                                      ImVec2(gx - 1.0f, (ty0 + ty1) * 0.5f), ColorAlpha(ColorLighten(trackCol, 0.6f), 220));
            }
        }
        ImGui::PopID();
    }

    if (laneOn_) {
        const float laneTop = rowsTop + bodyH0 + 6.0f;
        // lane selector in the label column
        ImGui::SetCursorPos(ImVec2(origin.x, laneTop + 12.0f));
        if (ToggleButton("VEL", laneKind_ == LaneKind::Velocity, ImVec2(kLabelW - 4.0f, 20), ToggleTab())) laneKind_ = LaneKind::Velocity;
        ImGui::SetCursorPos(ImVec2(origin.x, laneTop + 36.0f));
        ImGui::PushFont(nullptr, std::max(10.0f, std::min(ImGui::GetFontSize(), 12.0f)));   // UI-7: "NUDGE" fits the label column (was "NUDG")
        if (ToggleButton("NUDGE", laneKind_ == LaneKind::Nudge, ImVec2(kLabelW - 4.0f, 20), ToggleTab())) laneKind_ = LaneKind::Nudge;
        ImGui::PopFont();
        const float totalW = geom.X(kPageSteps - 1) + cellW - geom.x0;
        DrawStepLane(app, ti, laneKind_, laneState_, geom, laneTop, laneH - 8.0f, totalW, trackLen, playing ? playStep : -1);
    }

    if (scaleUsed_ && poolSize > static_cast<int>(rows.size()) && !rows.empty()) {
        const float bx = win.x + origin.x + innerW - 4.0f, by0 = win.y + rowsTop - ImGui::GetScrollY();
        const float bh = rh * rows.size();
        dl->AddRectFilled(ImVec2(bx, by0), ImVec2(bx + 3.0f, by0 + bh), vis::kInset, 1.5f);
        float f0 = static_cast<float>(poolStart) / poolSize, f1 = static_cast<float>(poolStart + rows.size()) / poolSize;
        dl->AddRectFilled(ImVec2(bx, by0 + bh * f0), ImVec2(bx + 3.0f, by0 + bh * f1), vis::kLineHi, 1.5f);
    }
    if (marquee_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        if (std::fabs(mp.x - marqueeP0_.x) + std::fabs(mp.y - marqueeP0_.y) > 4.0f) {
            sel_ = marqueeBase_;
            for (const NoteRef& nr : marqueeNew_) if (std::find(sel_.begin(), sel_.end(), nr) == sel_.end()) sel_.push_back(nr);
        }
        marqueeNew_.clear();
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        dragPainting_ = false;
        dragLastRow_ = -1;
        dragLastCol_ = -1;
    }
    if (popupStep_ >= 0) DrawStepEditPopup("PianoRollStepPopup", popupOpenRequest_, app, ti, popupStep_, popupRow_);
    popupOpenRequest_ = false;

    DrawTrackVolumeStrip(app, layout, rowsTop + kNumTracks * layout.rowH + 4.0f, ti);
    ImGui::PopID();
}

} // namespace t8::desktop
