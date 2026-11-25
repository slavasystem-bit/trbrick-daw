#pragma once
// The questions of the document model (U8b): "Save changes?" (Save / Don't Save / Cancel), the name dialog (Save As, Rename,
// Duplicate, the first Save of an Untitled project), the Open list and the recovery question after a crash. One call per frame,
// from the shell; the state lives in AppState (docAskUnsaved, docNameOpen, docOpenDialog, docRecoverPath).
#include "app/AppState.h"

namespace t8::desktop {
void DrawDocumentDialogs(AppState& app);
} // namespace t8::desktop
