#pragma once
// The TRACK screen (v44.9): the whole track as ONE strip of bricks. (v44.10: it sits on top of the scene editor on one screen and
// carries the player: BRICK / TRACK mode, LOOP, FROM START; a click on the ruler plays from that brick.)
//
//   - a brick is a chain step; its width is its length in bars, its colour is the role of its scene;
//   - under the bricks a table says who plays on each brick (click / drag to switch an instrument off and on);
//   - under that one line of dynamics (level start -> end per brick; drag the dots) with the HPF / LPF sweeps as thin lines;
//   - no waveforms, no notes: double-click a brick to open its scene (the step editor of the LIVE tab).
//
// Everything here is a front-end onto Project::songChain (ChainStep: repeats, trimSteps, muteMask, automation) and the
// Sequencer's song mode, so nothing in the engine changed for it.
#include "app/AppState.h"

namespace t8::desktop {

class ChainPanel;

class BricksPanel {
public:
    // showDetails: the table "who plays" and the dynamics line under the bricks (false = bricks and ruler only, for short windows).
    void Draw(AppState& app, ChainPanel& chain, bool showDetails);
    // v44.11: the two rows under the strip with everything about the selected brick (role, repeats, trim, who plays, dynamics, fill,
    // transition, copy / repeat / remove). The caller gives it a child of InspectorHeight().
    void DrawInspector(AppState& app, ChainPanel& chain);
    static float InspectorHeight();

private:
    enum class Drag { None, Resize, Point, Paint, FillLen, TransLen, TrackLevel, FillMove, TransMove };
    int sel_ = -1;
    bool marquee_ = false; float marqueeX0_ = 0.0f; int marqueeLo_ = -2, marqueeHi_ = -2;   // U6: rubber band over the bricks (content x of the press)
    int anchor_ = -1;   // U6: the brick a Shift+click range starts from (the last plainly selected one)
    int page_ = 0;           // v44.34: the open page of the inspector: 0 = LEVEL, 1 = FILL, 2 = TRANSITION, 3 = PARAM (v44.38)
    int parTrack_ = 6, parParam_ = 5;   // v44.38: the parameter row the PARAM page and the fourth lane row under the bricks work on (bass, cutoff)
    char parSearch_[32] = {};
    bool transOut_ = false;  // v44.34: the TRANS page edits the transition out of this brick (into the next one) instead of the one into it
    bool whoPaint_ = false;  // v44.34: a click-drag over the WHO PLAYS bars of the inspector is going on
    int lanePick_ = -1, lanePickAt_ = -1;   // v44.29: the inspector's LEVEL OF shows MIX / FILTER (kLaneMix / kLaneFilter) or, at -1, the selected instrument
    Drag drag_ = Drag::None;
    int dragBrick_ = -1;
    bool dragEnd_ = false;       // TrackLevel: false = the start of the brick, true = its end
    int dragLane_ = 0, dragPt_ = 0, dragRow_ = 0;   // Point: the lane (instrument 0..9, kLaneMix, kLaneFilter), the point and the row it is drawn in
    Lane dragLane0_;                                // Point: the lane as it was, for Esc
    float dragYOff_ = 0.0f;                         // Point: where inside the dot the mouse took hold
    double lastAddT_ = -10.0;                       // when a point was last added (a double-click right after it must not delete it)
    bool paintMuted_ = false;    // Paint: the state every touched cell gets
    int menuBrick_ = -1;
    bool menuOpen_ = false;
    bool addOpen_ = false;
    // Zoom and navigation of a long track (v44.10)
    float ppbUser_ = 0.0f;       // pixels per bar chosen by the author; 0 = fit the whole track in the window
    float ppbCur_ = 18.0f;       // the one used in the last frame
    float scrollX_ = 0.0f;       // sideways scroll of the strip in the last frame
    float viewW_ = 0.0f, contentW_ = 0.0f, childLeftX_ = 0.0f;
    // The FILLS / TRANSITIONS lane (v44.10.2)
    int fxMenuBrick_ = -1; int fxMenuKind_ = 0; bool fxMenuOpen_ = false;   // kind 0 = fill, 1 = transition
    int dragTrack_ = -1; float dragY0_ = 0.0f; int dragDb0_ = 0;          // v44.13: Shift + drag on a cell of WHO PLAYS = the level of that instrument on that brick
    int lvlMenuBrick_ = -1, lvlMenuTrack_ = -1; bool lvlMenuOpen_ = false;
    int dynMenuBrick_ = -1; bool dynMenuOpen_ = false;   // right-click menu of the DYNAMICS block (v44.10.2)
    FillRule dragFill_{}; float dragRightX_ = 0.0f;
    // v44.25: moving / removing a fill or a transition block on the lane (a click without moving removes it)
    float dragX0_ = 0.0f; bool dragMoved_ = false; int dragTarget_ = -1; FillRule dragFill0_{}; FillMode dragMode0_ = FillMode::Scene;
    float pendingScroll_ = -1.0f; int pendingFrames_ = 0;   // scroll requested by a zoom / the overview (kept 2 frames: the content grows first)
    void ZoomTo(float newPpb, float anchorScreenX);
    void ScrollTo(float x) { pendingScroll_ = std::max(0.0f, x); pendingFrames_ = 2; }
};

} // namespace t8::desktop
