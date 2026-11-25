#pragma once
// v44.43 CHORD LAB: a panel over the LIVE screen for jamming with chords. A palette of the chords of the key (with the borrowed ones), a
// fit against the bass / synth notes of the scene (MATCH), a loop of blocks to build the progression in, and the reasons behind each chord.
// The jam lives in a chord bank slot (a draft: the scene's own chords are not touched). ADD TO SCENE writes it into the scene, THROW INTO THE
// CHAIN makes the scene a brick, and TRACK is where the brick is arranged. The rules behind the palette are in core/ChordLab.h.
#include "app/AppState.h"
#include "core/ChordLab.h"
#include <functional>
#include <vector>

namespace t8::desktop {

class ChordLabPanel {
public:
    void Draw(AppState& app, float width, float height);
    void Leave(AppState& app);
    // v44.46: how the SOUND view draws the chord instrument (main passes the context panel)
    void SetSoundBody(std::function<void(AppState&)> f) { soundBody_ = std::move(f); }   // the panel is not shown: stops a chord that is being auditioned

private:
    struct Blk { int start = 0, len = 1; };

    void BeginEdit(AppState& app);
    void Rebuild(AppState& app);
    void Match(AppState& app);
    void RefreshMatch(AppState& app, int blockStart, int blockLen);
    std::vector<int> SrcTracks() const;
    int SceneShown(const AppState& app) const;
    const LabChord* Selected() const;
    void Select(int row, int col) { selRow_ = row; selCol_ = col; }
    void Audition(AppState& app, const LabChord& c);
    void Place(AppState& app, const LabChord& c, int start, int len);
    void Carve(Track& tr, int start, int end);
    void Resize(AppState& app, int blockIndex, int newEnd);
    void Split(AppState& app, int blockIndex);
    void DeleteBlock(AppState& app, int blockIndex);
    void StartFrom(AppState& app, int progression);
    void ClearJam(AppState& app);
    Track& JamTrack(AppState& app);
    std::vector<Blk> Blocks(const Track& tr) const;
    int DefaultLen(const AppState& app) const;

    bool opened_ = false;
    int jam_ = -1;                       // the chord bank slot of the draft; -1 = bank off, the scene's own chords are shown and nothing is changed yet
    std::vector<LabChord> pal_;
    int palKey_ = -1, palScale_ = -1; bool palNine_ = false;
    bool nine_ = false;                  // 9THS instead of 7THS in the second row
    int selRow_ = -1, selCol_ = -1;      // the chord shown in WHY
    int selBlock_ = -1;
    int srcMask_ = 1;                    // bit 0 BASS, bit 1 SYNTH 2, bit 2 SYNTH 3
    int barsMode_ = 2;                   // 0 bar 1, 1 bars 1-2, 2 all bars of the scene
    LabMelody mel_;
    bool matched_ = false;
    bool scopeBlock_ = false;            // the notes MATCH reads are those under the selected block (not the whole scene)
    bool soundView_ = false;             // JAM | SOUND switch
    std::function<void(AppState&)> soundBody_;
    int defLen_ = 1;                     // new block: 0 half a bar, 1 a bar, 2 two bars
    int dragRow_ = -1, dragCol_ = -1;    // a palette chip being dragged towards the loop
    int resizeBlk_ = -1;                 // a block edge being dragged
    double auditionUntil_ = 0.0;
    int startFromSel_ = 0;
};

} // namespace t8::desktop
