#pragma once
// Shared row metrics for the LIVE screen's middle band. The Sequencer on the
// left and the Mixer / Piano Roll on the right are separate child windows,
// but they must line up row-for-row (one sequencer row == that track's mixer
// row) and be exactly the same height, so both read their vertical sizes from
// this one struct, computed once per frame in main_desktop.cpp.
namespace t8::desktop {

struct LiveLayout {
    float rowH = 26.0f;      // one track row (10 of them)
    float headerH = 28.0f;   // strip above the rows (paging / column labels)
    float footerH = 28.0f;   // strip below the rows (hint / master volume)
    float padX = 8.0f;       // child-window inner padding, horizontal
    float padY = 6.0f;       // child-window inner padding, vertical
};

} // namespace t8::desktop
