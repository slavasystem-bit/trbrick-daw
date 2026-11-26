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
#include "SampleBrowser.h"
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

    ListBrowser kitBrowser_, beatBrowser_, styleBrowser_;
    SampleBrowser sampleBrowser_;   // C4b: the one SAMPLES browser (was the MY SAMPLES card browser)
    struct AuditionHit { double at; int slot; float vel; };
    std::vector<AuditionHit> audition_;
    // C4b: HEAR in the kit browser plays the selected kit without loading it (a click only selects): its bank is read on the
    // loader thread, then the short phrase is played from its buffers.
    std::string auditionKit_;      // the kit the phrase is waiting for / playing from ("" = the sounding kit through the drum voices)
    double auditionKitAsked_ = 0.0;
    std::vector<SamplePtr> auditionKeep_;
};

} // namespace t8::desktop
