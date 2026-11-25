#pragma once
// The menu bar (v44.19, U8c): File / Edit / View / Library / Settings across the top of the window, the way every Mac program has
// it. It replaces the old FILE drawer: one place for the document, for Undo / Copy / Paste, for the size of the interface, for the
// user's library and for the audio settings. Nothing here is a performance control. Call DrawMenuBar inside the shell window
// (created with ImGuiWindowFlags_MenuBar).
#include "app/AppState.h"
#include <string>
#include <vector>

namespace t8::desktop {

class MenuBar {
public:
    void Draw(AppState& app);
    // true while stems or MIDI capture are armed (the File menu is tinted so a running capture is never forgotten).
    static bool AnyCaptureActive(AppState& app);
    // "Cmd+" on macOS, "Ctrl+" elsewhere: the prefix of the shortcut texts shown in the menus.
    static const char* CmdPrefix();

private:
    void DrawVersionsDialog(AppState& app);
    void DrawBeatsDialog(AppState& app);

    bool openVersions_ = false, openBeats_ = false;
    std::vector<std::string> backups_;
    std::string restoreArmed_;
    char newBeat_[48] = "";
    char renameBuf_[48] = "";
    std::string renameTarget_, deleteArmed_;
};

} // namespace t8::desktop
