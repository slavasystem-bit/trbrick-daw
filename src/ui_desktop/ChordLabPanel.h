#pragma once
// v44.43 CHORD LAB: a panel over the LIVE screen for jamming with chords. A palette of the chords of the key (with the borrowed ones), a
// fit against the bass / synth notes of the scene (MATCH), a loop of blocks to build the progression in, and the reasons behind each chord.
// The jam lives in a chord bank slot (a draft: the scene's own chords are not touched). ADD TO SCENE writes it into the scene, THROW INTO THE
// CHAIN makes the scene a brick, and TRACK is where the brick is arranged. The rules behind the palette are in core/ChordLab.h.
#include "app/AppState.h"
#include "core/ChordLab.h"
#include "core/ChordAdvice.h"
#include "core/ChordPerform.h"
#include "imgui.h"
#include <functional>
#include <vector>

namespace t8::desktop {

class ChordLabPanel {
public:
    void Draw(AppState& app, float width, float height);
    void Leave(AppState& app);
    // v44.46: how the SOUND view draws the chord instrument (main passes the context panel)
    void SetSoundBody(std::function<void(AppState&)> f) { soundBody_ = std::move(f); }
    // C2 part 3: the short SOUND column under PLAY (main passes the context panel); returns true for ALL KNOBS...
    void SetSoundColumn(std::function<bool(AppState&, ImVec2, float)> f) { soundColumn_ = std::move(f); }   // the panel is not shown: stops a chord that is being auditioned

private:
    struct Blk { int start = 0, len = 1; };

    void BeginEdit(AppState& app);
    void Rebuild(AppState& app);
    void RefreshMatch(AppState& app, int blockStart, int blockLen);
    std::vector<int> SrcTracks() const;
    int SceneShown(const AppState& app) const;
    const LabChord* Selected() const;
    void Select(int row, int col) { selRow_ = row; selCol_ = col; }
    void Audition(AppState& app, const LabChord& c);
    void Hear(AppState& app, const LabChord& c);
    void TakeLiveChord(AppState& app);
    void Place(AppState& app, const LabChord& c, int start, int len);
    void Carve(Track& tr, int start, int end);
    void Resize(AppState& app, int blockIndex, int newEnd);
    void Split(AppState& app, int blockIndex);
    void MoveBlock(AppState& app, int blockIndex, int to, bool copy);
    void SetLoopLen(AppState& app, int len);
    void DeleteBlock(AppState& app, int blockIndex);
    void StartFrom(AppState& app, int progression);
    void ListenAB(AppState& app, bool before);
    void UndoAll(AppState& app);
    struct Slot { int start = 0, len = 1; int block = -1; };   // the place of the loop WHAT FITS is about (a block, or the room after the last one)
    Slot AdviceSlot(const AppState& app, const Track& jam, const std::vector<Blk>& blocks) const;
    void DrawHeader(AppState& app, ImDrawList* dl, ImVec2 o, float W, int scene);
    void DrawWhatFits(AppState& app, ImDrawList* dl, ImVec2 a, float w, float h, const Track& jam, const std::vector<Blk>& blocks, const Slot& slot);
    float DrawPlay(AppState& app, ImDrawList* dl, ImVec2 a, float w, float h, const Track& jam, const std::vector<Blk>& blocks, const Slot& slot);
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
    LabMelody mel_;                      // the notes under the place WHAT FITS is about
    bool soundView_ = false;             // ALL KNOBS...: the whole chord instrument in the place of the lab (BACK TO THE JAM returns)
    int adviceMode_ = 0;                 // C2: BEST / SMOOTH / MORE TENSION / NEW COLOUR
    int adviceSel_ = 0;                  // the card whose sentence is shown
    bool abBefore_ = false;              // C2: A BEFORE is being heard (the draft waits in abSlot_)
    int abSlot_ = 0;
    uint64_t labMark_ = 0;               // C2: the undo marker taken when CHORD LAB was opened (UNDO ALL goes back to it)
    std::function<void(AppState&)> soundBody_;
    std::function<bool(AppState&, ImVec2, float)> soundColumn_;
    int defLen_ = 1;                     // new block: 0 half a bar, 1 a bar, 2 two bars
    int dragRow_ = -1, dragCol_ = -1;    // a palette chip being dragged towards the loop
    int resizeBlk_ = -1;                 // a block edge being dragged
    int moveBlk_ = -1, moveGrab_ = 0;    // C3: a block being dragged to another step (the step of the block under the mouse)
    double auditionUntil_ = 0.0;
    bool recLive_ = false;               // C2: REC WHAT I PLAY
    LabChord liveChord_;                 // the chord last played live (waits for the sequencer to start it)
    bool liveWait_ = false;
    uint32_t liveSerial_ = 0;
};

} // namespace t8::desktop
