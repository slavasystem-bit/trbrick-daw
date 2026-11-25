#pragma once
// Horizontal mixer for the LIVE screen. One row per track, lined up
// row-for-row with the Sequencer on its left (same LiveLayout::rowH, same
// header height), so each row IS that track's channel: a VOLUME slider with a
// thin live peak meter along its bottom edge, and a PAN slider. Mute / solo
// are NOT repeated here -- the Sequencer's own M / S buttons on the same row
// already own them. The MASTER volume strip sits underneath (see
// DrawMasterStrip, also used under the Piano Roll so master level never
// goes away when the Mixer is swapped out).
//
// Edits write both copies of the data (Mixer::TrackState for the audio
// thread, Track for saving) like MixerSync.h's helpers, but push ONE undo
// entry per drag instead of one per frame.
#include "app/AppState.h"
#include "LiveLayout.h"

namespace t8::desktop {

class MixerPanel {
public:
    void Draw(AppState& app, const LiveLayout& layout);
};

// MASTER volume + L/R meters + CLIP lamp, one row, at local cursor-Y `y`
// of the window currently being drawn into.
void DrawMasterStrip(AppState& app, const LiveLayout& layout, float y);

// Same row, but for ONE track's fader (name, slider with its live peak, value).
// Used under the piano roll so the track you are editing can be balanced
// without leaving the roll; the MASTER strip stays on the MIXER view.
void DrawTrackVolumeStrip(AppState& app, const LiveLayout& layout, float y, int trackIndex);

} // namespace t8::desktop
