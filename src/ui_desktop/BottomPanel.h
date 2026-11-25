#pragma once
// The LIVE screen's bottom strip -- always on screen, full width, RANDOM / KIT
// first and LIVE second:
//
//   RANDOM / KIT   drum kit browser, typical-drum-pattern browser (both the big card windows),
//                  one RANDOM + SIMPLE/COMPLEX + CLEAR column per part
//                  (drums, bass, synth 2, synth 3, chords), CLEAR PATTERN
//   LIVE           master filter XY pad, the MUTE PAD grid (KICK DRUMS HATS CHORD / BASS SYN2 SYN3 DROP),
//                  MUTE ALL, STUTTER (+ density),
//                  FILL with its own break picker, RISER with its own sample
//                  picker (both "RANDOM" or a specific one; both bar-quantized
//                  and cancellable while queued)
//
// Mouse front-end onto the AppState generators / live-break / riser calls.
#include "app/AppState.h"
#include "ListBrowser.h"
#include <vector>

namespace t8::desktop {

class BottomPanel {
public:
    void Draw(AppState& app);
    void DrawBrowsers(AppState& app);   // v44.26: the kit browser, the beat browser and the kit audition (also needed on TRACK)

private:
    void DrawRandomKit(AppState& app);
    void DrawLive(AppState& app);
    void DrawMutePads(AppState& app);
    void OpenKitBrowser(AppState& app);
    void OpenBeatBrowser(AppState& app);
    void OpenStyleBrowser(AppState& app);
    void OpenLibraryBrowser(AppState& app, AppState::LibTarget target, int brick);   // v44.51: the user's sample library
    void DrawStyleDetail(AppState& app, int styleId, float width);
    void LoadKit(AppState& app, const std::string& kitPath);
    void ApplyBeat(AppState& app, int beatIndex);   // 0 = random, 1.. = typical beats, then the saved ones
    void RunKitAudition(AppState& app);
    void ScheduleKitAudition();

    ListBrowser kitBrowser_, beatBrowser_, styleBrowser_, libBrowser_;
    AppState::LibTarget libTarget_ = AppState::LibTarget::None;   // what Enter / double-click in the library browser does
    int libBrick_ = -1;
    std::vector<int> libIds_;   // browser item id -> index into AppState::LibraryList()
    struct AuditionHit { double at; int slot; float vel; };
    std::vector<AuditionHit> audition_;
};

} // namespace t8::desktop
