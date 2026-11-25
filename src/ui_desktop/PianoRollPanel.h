#pragma once
// Piano Roll for the selected Synth / Chord track. Swaps in for the Mixer on
// the LIVE screen's right half and is exactly as tall as the Sequencer next
// to it (same LiveLayout), with the MASTER strip kept underneath.
//
//   - always 16 steps wide, paged together with the Sequencer
//     (AppState::deskStepPage) -- no scrolling
//   - ALL NOTES shows one octave (12 pitch rows) that the mouse wheel scrolls (the OCT buttons are gone: right-click a note = OCTAVE UP / DOWN);
//     ACTIVE ONLY lists just the pitches that actually have a note on this
//     track, so a sparse bassline collapses to a handful of tall rows
//   - the selected track's VOLUME / PAN sit in the header, since the Mixer
//     (which normally owns them) is hidden while this is showing
//   - SCALE+USED: all notes of the key/scale together with every pitch the track plays (out-of-key
//     ones in red), several octaves, wheel to scroll, degree numbers, ghosts of other tracks
//   - SCALE: OFF / MARK / ONLY follows the project key + scale from the header
//   - drag the right edge of a note to set its length
//   - click / drag-paint notes; right-click opens the shared step popup
#include "app/AppState.h"
#include "core/NoteEdit.h"
#include "LiveLayout.h"
#include "StepLane.h"

namespace t8::desktop {

class PianoRollPanel {
public:
    void Draw(AppState& app, const LiveLayout& layout);

private:
    int topNote_ = 59;       // MIDI note on the top row in ALL NOTES mode
    int lastTrack_ = -1;     // re-centres topNote_ when the selected track changes
    bool activeOnly_ = false;
    bool scaleUsed_ = false; // SCALE+USED: every scale note plus every played pitch (multi-octave, scrollable)
    float humanizePct_ = 15.0f;   // HUMANIZE amount, % of the lane range
    int popupRow_ = -1;           // the pitch that was right-clicked (octave shift of a single chord note)
    bool ghost_ = true;      // faint outlines of the other melodic tracks' notes
    int scrollIdx_ = 0;      // first visible row of the SCALE+USED pool
    bool recenter_ = true;   // re-centre the SCALE+USED window on the played notes
    int scaleView_ = 0;      // 0 = OFF, 1 = MARK (shade rows outside the key/scale), 2 = ONLY (list scale notes)
    // note-length drag (grip at the right end of a note's bar)
    bool resizing_ = false;
    bool resizeUndone_ = false;
    int resizeStep_ = -1;
    unsigned long long usedHash_ = 0; // detects pitch-set changes made outside the roll

    bool dragPainting_ = false;
    bool dragPaintValue_ = false;
    int dragLastRow_ = -1;
    int dragLastCol_ = -1;

    // value lane under the notes (velocity / nudge of the steps), same widget as the drum LANES panel
    bool laneOn_ = true;
    LaneKind laneKind_ = LaneKind::Velocity;
    LaneState laneState_;

    // U6 (v44.30): several notes at once. Shift+drag = rubber band, Shift / Cmd+click = add / remove one, Cmd+A = all; then drag a selected
    // note (Option = copy), arrows (Shift = octave / beat), Delete, Cmd+D, drag the right edge of one of them = the length of all.
    std::vector<NoteRef> sel_;
    int selTrack_ = -1, selPattern_ = -1;
    StepSnapshot snap_{};                 // the steps when a move / length gesture began
    bool moving_ = false, moveCopy_ = false, moveUndone_ = false, moveMoved_ = false;
    int moveCol0_ = 0, movePitch0_ = 0;
    std::vector<NoteRef> moveSel0_;
    bool marquee_ = false; ImVec2 marqueeP0_{0, 0}; std::vector<NoteRef> marqueeBase_, marqueeNew_;
    bool shiftPress_ = false; NoteRef shiftNote_{-1, -1}; bool shiftOnNote_ = false;   // a Shift / Cmd press: a click toggles the note, a drag is a band
    bool resizeGroup_ = false; int resizeOrig_ = 1;
    bool SelHas(int col, int pitch) const { return std::find(sel_.begin(), sel_.end(), NoteRef{col, pitch}) != sel_.end(); }
    void SelToggle(int col, int pitch) {
        auto it = std::find(sel_.begin(), sel_.end(), NoteRef{col, pitch});
        if (it == sel_.end()) sel_.push_back({col, pitch}); else sel_.erase(it);
    }

    int popupStep_ = -1;
    bool popupOpenRequest_ = false;
};

} // namespace t8::desktop
