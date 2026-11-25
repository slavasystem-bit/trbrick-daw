#pragma once
// v44.41 MIX LAB: where do the instruments of the played scene fight for the same frequencies, and what could be turned to make room.
// Frequency map (one curve per instrument + the whole mix), "who lives where" strips, overlap cards with a concrete fix (SHOW IT = hear only
// the ones that collide, TRY IT = apply the fix and flip A / B), tonal balance against a typical track, and the priority list that decides
// who is asked to make room. The analysis runs on a private engine in a worker thread (app/MixLab.h); nothing here touches the live sound
// except SHOW IT (a temporary solo, put back when you leave) and TRY IT (a real, undoable change to the instrument's channel EQ / level / pan).
#include "app/AppState.h"
#include "app/MixLab.h"
#include <functional>

namespace t8::desktop {

class MixLabPanel {
public:
    void Draw(AppState& app, float width, float height);
    void Leave(AppState& app);   // called while the tab is not shown: puts a SHOW IT solo back

private:
    void StartAnalysis(AppState& app);
    void Refresh(AppState& app);
    void Rebuild(AppState& app);   // reports from the stored spectra (after the priority list changed)
    void DrawMap(AppState& app, float w, float h);
    void DrawCards(AppState& app, float w, float h);
    void DrawBalance(AppState& app, float w, float h);
    void DrawPriority(AppState& app, float w, float h);
    void DrawAB(AppState& app, float w, float h);
    void DrawChannel(AppState& app, float w, float h);   // v44.42: level, pan and the channel EQ of one instrument, against its spectrum
    void EditStrip(AppState& app, int track, const std::function<void(Track&)>& fn);
    void ApplyAll(AppState& app, int onlyTrack);         // the strips (level, pan, EQ) into every other scene / brick
    void TryFix(AppState& app, int spot);
    void UndoAll(AppState& app);
    void ShowSpot(AppState& app, int spot);
    void ReleaseSolo(AppState& app);
    void SetAB(AppState& app, bool b);
    void ApplySnaps(AppState& app, bool after);
    const MixSpectra& ShownSpectra() const { return (lab_.showB && !lab_.TriedFixes().empty() && haveCur_) ? cur_ : base_; }
    const MixReport& ShownReport() const { return (lab_.showB && !lab_.TriedFixes().empty() && haveCur_) ? curRep_ : baseRep_; }

    MixLab lab_;
    MixSpectra cur_, base_;          // cur_ = the latest analysis of the live state, base_ = before any fix
    MixReport curRep_, baseRep_;
    bool haveCur_ = false, haveBase_ = false;
    int seenGen_ = 0;
    int lastFrame_ = -10;
    int filter_ = 0;                 // 0 ALL, 1 DRUMS, 2 MELODIC
    bool masking_ = true;
    int soloSpot_ = -1;
    bool soloSaved_ = false;
    std::array<bool, kNumTracks> savedMute_{}, savedSolo_{};
    int dragFrom_ = -1;
    float cardScroll_ = 0.0f;
    int chan_ = -1;                  // the instrument shown in CHANNEL
    int eqDrag_ = -1;
    bool applyChainOnly_ = false;    // APPLY TO ALL: false = every scene and brick, true = only the scenes the chain uses
    bool reanalyse_ = false;         // an edit was finished: listen again as soon as the worker is free
};

} // namespace t8::desktop
