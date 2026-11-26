#include "ValueField.h"
#include "SequencerPanel.h"
#include "Visual.h"
#include "LiveWidgets.h"
#include "TrackStyle.h"
#include "MixerSync.h"
#include "InstrumentMenu.h"
#include "StepEditPopup.h"
#include "StepGlyphs.h"
#include "BrickGlyphs.h"
#include "imgui.h"
#include "InfoLine.h"
#include <algorithm>
#include <cstdio>
#include <mutex>

namespace t8::desktop {

namespace {

// Explicit-value counterpart to AppState::ToggleStep -- a click-drag PAINT
// gesture must set every cell it crosses to the SAME on/off value the drag
// started with. Mirrors ToggleStep's "seed a sensible default note/chord so
// a freshly-activated Synth/Chord step isn't silent" behaviour.
void SetStepActiveExplicit(AppState& app, int trackIndex, int stepIndex, bool active) {
    Pattern& pat = app.CurrentPattern();
    Track& t = app.StepTrack(pat, trackIndex);
    Step& s = t.steps[stepIndex];
    if (s.active == active) return;
    app.PushUndo();
    s.active = active;
    if (s.active && IsNoteTrackType(t.type) && s.note == 0) s.note = 60;
    if (s.active && t.type == TrackType::Chord && s.chordSize == 0) {
        s.chordSize = 3;
        s.chordNotes = {60, 64, 67, 71, 0, 0};
    }
}

ImU32 StepColor(int row, const Step& s, bool shadedGroup) {
    if (!s.active) return shadedGroup ? vis::kLine : vis::kCellHover;
    ImU32 c = TrackColor(row);
    return s.accent ? ColorLighten(c, 0.35f) : c;   // accent: brighter + white bar (DrawStepFace)
}

} // namespace

void SequencerPanel::Draw(AppState& app, const LiveLayout& layout) {
    SceneLock lock(app.sequencer.PatternMutex(), "SEQUENCER");
    Pattern& pat = app.CurrentPattern();

    ImGui::PushID("SequencerPanel");

    // ---- geometry: exactly 16 steps always fit; paging brings the rest ----
    const int gridSpb = ClampGrid(pat.stepsPerBeat);
    const int kPageSteps = PageStepsFor(gridSpb), kGroup = GroupStepsFor(gridSpb);
    const float kLabelW = 152.0f, kCellGap = 2.0f, kGroupGap = 6.0f;
    ImVec2 origin = ImGui::GetCursorPos();
    float innerW = ImGui::GetContentRegionAvail().x;
    float cellW = CellWidthFor(innerW, kLabelW, kCellGap, kGroupGap, kPageSteps, kGroup);
    cellW = std::max(10.0f, cellW);
    const float rowH = layout.rowH;
    const float rowsTop = origin.y + layout.headerH;

    int maxLen = pat.masterLength;
    for (int ti = 0; ti < kNumTracks; ++ti) { const Track& t = app.StepTrack(pat, ti); if (t.useOwnLength) maxLen = std::max(maxLen, t.length); }
    int numPages = std::max(1, (maxLen + kPageSteps - 1) / kPageSteps);
    app.deskStepPage = std::clamp(app.deskStepPage, 0, numPages - 1);
    const int base = app.deskStepPage * kPageSteps;

    // ---- header: SEQUENCER label + page switcher --------------------------
    {
        ImGui::SetCursorPos(ImVec2(origin.x, origin.y + (layout.headerH - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::AlignTextToFramePadding();
        // v44.23 "where am I": the same colours as the tabs. LIVE (amber) edits a scene of the jam pad, TRACK (teal) the notes of a brick.
        if (app.activeTab == 1 && app.project.songChainLength > 0) {
            const int b = app.BrickForTransport();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(vis::kTeal), "BRICK %d  NOTES", (b >= 0 ? b : std::clamp(app.selBrick, 0, app.project.songChainLength - 1)) + 1);
            if (ImGui::IsItemHovered()) Hint("You are editing the notes of this brick of the track.\nThe LIVE scenes are a separate sketch pad and are not touched.");
        } else if (app.activeTab == 1) {
            ImGui::TextDisabled("SEQUENCER");
        } else {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(vis::kAccent), "LIVE  SCENE %02d", (AppState::IsBrickSlot(app.project.currentPattern) ? app.project.liveScene : app.project.currentPattern) + 1);
            if (ImGui::IsItemHovered()) Hint("You are on the LIVE sketch pad. Press THROW ONTO THE TRACK to copy this scene into the track as a brick.");
        }
        ImGui::SameLine(origin.x + kLabelW + 2.0f);
        bool canPrev = app.deskStepPage > 0, canNext = app.deskStepPage < numPages - 1;
        if (!canPrev) ImGui::BeginDisabled();
        if (ImGui::ArrowButton("##pgPrev", ImGuiDir_Left)) app.deskStepPage--;
        if (!canPrev) ImGui::EndDisabled();
        ImGui::SameLine(0, 6);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("STEPS %d-%d", base + 1, std::min(base + kPageSteps, std::max(maxLen, base + 1)));
        ImGui::SameLine(0, 6);
        if (!canNext) ImGui::BeginDisabled();
        if (ImGui::ArrowButton("##pgNext", ImGuiDir_Right)) app.deskStepPage++;
        if (!canNext) ImGui::EndDisabled();
        if (numPages > 1) {
            ImGui::SameLine(0, 10);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("page %d/%d", app.deskStepPage + 1, numPages);
        }
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 win = ImGui::GetWindowPos();

    // ---- drag the right edge of a note to set its length ------------------
    // Pitched tracks only (a drum hit has no length). Grabbing the grip at the
    // end of a note's bar and moving sideways snaps to whole steps; the list in
    // the right-click popup stays as a quick way to enter long values.
    const PageGeom geom{origin.x + kLabelW, cellW, kCellGap, kGroupGap, base, kPageSteps, kGroup};
    const float gripW = std::min(9.0f, cellW * 0.4f);
    bool gripPressed = false;
    {
        const ImGuiIO& io = ImGui::GetIO();
        const float sx = ImGui::GetScrollX(), sy = ImGui::GetScrollY();
        if (resizing_) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && resizeTrack_ >= 0) {
                Track& rt = app.StepTrack(pat, resizeTrack_);
                Step& rs = rt.steps[resizeStep_];
                int rLen = rt.useOwnLength ? rt.length : pat.masterLength;
                int endCol = base + geom.ColAtX(io.MousePos.x - win.x + sx);
                int len = std::clamp(endCol - resizeStep_ + 1, 1, std::max(1, rLen - resizeStep_));
                if (len != rs.lengthSteps) {
                    if (!resizeUndone_) { app.PushUndo(); resizeUndone_ = true; }
                    rs.lengthSteps = static_cast<uint8_t>(len);
                }
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                char tip[24]; std::snprintf(tip, sizeof(tip), "length %d", len);
                Hint("%s", tip);
            } else {
                resizing_ = false;
            }
        } else if (ImGui::IsWindowHovered() && !moveCandidate_) {
            for (int row = 0; row < kNumTracks && !gripPressed; ++row) {
                const Track& t = app.StepTrack(pat, row);
                if (t.type == TrackType::Drum) continue;
                int trackLen = t.useOwnLength ? t.length : pat.masterLength;
                float y0 = win.y + rowsTop + row * rowH - sy + 1.0f, y1 = y0 + rowH - 4.0f;
                if (io.MousePos.y < y0 || io.MousePos.y > y1) continue;
                for (int c = 0; c < kPageSteps; ++c) {
                    int col = base + c;
                    if (col >= trackLen || col >= kMaxSteps) break;
                    const Step& st = t.steps[col];
                    if (!st.active) continue;
                    int ec = c + StepSpan(st, col, trackLen) - 1;
                    if (ec > kPageSteps - 1) continue;                       // end is on a later page
                    float x1 = win.x + geom.X(ec) + cellW - sx, x0 = x1 - gripW;
                    if (io.MousePos.x >= x0 && io.MousePos.x <= x1 + 1.0f) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                            resizing_ = true; resizeUndone_ = false;
                            resizeTrack_ = row; resizeStep_ = col;
                            app.seqCursorTrack = row; app.seqCursorStep = col;
                            gripPressed = true;
                        }
                        break;
                    }
                }
            }
        }
    }
    const bool cellsLocked = gripPressed || resizing_;

    for (int row = 0; row < kNumTracks; ++row) {
        Track& t = app.StepTrack(pat, row);
        auto& mt = app.EditMixer().tracks[row];
        float ry = rowsTop + row * rowH;
        // v44.38: on a brick M and S are WHO PLAYS (one mute for everything); on a live scene the flags of the scene
        const bool onBrick = app.BrickOfSlot(app.project.currentPattern) >= 0;
        const bool rowMuted = onBrick ? app.TrackIsMuted(row) : mt.mute;
        const bool rowSolo = onBrick ? app.TrackIsSolo(row) : mt.solo;
        ImGui::PushID(row);

        // selected-row backdrop, spanning label + cells
        if (app.seqCursorTrack == row) {
            ImVec2 a(win.x + origin.x - 2.0f, win.y + ry - ImGui::GetScrollY());
            dl->AddRectFilled(a, ImVec2(a.x + innerW + 4.0f, a.y + rowH - 1.0f), vis::kAccentGlow, 4.0f);
        }

        // ---- label: type bar + name (click selects) + M + S ------------
        const float btnW = 22.0f, btnH = rowH - 6.0f;
        float nameW = kLabelW - btnW * 2 - 10.0f;
        ImGui::SetCursorPos(ImVec2(origin.x, ry));
        if (ImGui::InvisibleButton("##name", ImVec2(nameW, rowH - 1.0f))) {
            app.seqCursorTrack = row;
            app.detailOpen = true;      // v44.8: clicking an instrument name opens its sound controls
            app.deskRollClosed = false; // clicking a synth / chord name always brings its roll back
        }
        if (ImGui::BeginPopupContextItem("##trackctx")) {
            app.seqCursorTrack = row;
            if (ImGui::MenuItem("Copy track", "Cmd+C")) app.CopyTrack(row);
            if (ImGui::MenuItem("Paste track", "Cmd+V", false, app.hasTrackClipboard)) app.PasteTrack(row);
            ImGui::Separator();
            DrawInstrumentItems(app, app.project.currentPattern, row);
            ImGui::Separator();
            if (ImGui::MenuItem("Copy whole scene", "Cmd+Shift+C")) { app.CopyPattern(); app.SetStatus("SCENE COPIED"); }
            if (ImGui::MenuItem("Paste whole pattern", "Cmd+Shift+V", false, app.hasPatternClipboard))
                app.SetStatus(app.PastePattern() ? "SCENE PASTED" : "CLIPBOARD EMPTY");
            if (t.type == TrackType::Drum) {
                ImGui::Separator();
                if (ImGui::MenuItem("Rhythm from the next genre")) app.SwapDrumSlotNext(t.slotIndex);
                if (ImGui::MenuItem("Random rhythm")) app.SwapDrumSlotRandom(t.slotIndex);
            }
            ImGui::EndPopup();
        }
        {
            ImVec2 a = ImGui::GetItemRectMin();
            {   // v44.40: the coloured stripe is the level of the instrument (post fader), so you can see who is sounding
                const float sy0 = a.y + 3.0f, sy1 = a.y + rowH - 4.0f;
                dl->AddRectFilled(ImVec2(a.x + 1, sy0), ImVec2(a.x + 5, sy1), IM_COL32(32, 34, 43, 255), 1.5f);
                const float pk = std::clamp(app.engine.GetMixer().tracks[row].peak.load(std::memory_order_relaxed), 0.0f, 1.0f);
                const float lvl = (pk > 0.01f && !rowMuted) ? std::clamp(std::sqrt(pk), 0.08f, 1.0f) : 0.0f;
                dl->AddRectFilled(ImVec2(a.x + 1, sy1 - (sy1 - sy0) * lvl), ImVec2(a.x + 5, sy1), rowMuted ? ColorAlpha(TrackColor(row), 60) : TrackColor(row), 1.5f);
                if (lvl > 0.0f) dl->AddRectFilled(ImVec2(a.x - 1, sy1 - (sy1 - sy0) * lvl - 1.0f), ImVec2(a.x + 7, sy1), ColorAlpha(TrackColor(row), 40), 2.5f);
                else dl->AddRectFilled(ImVec2(a.x + 1, sy1 - 2.0f), ImVec2(a.x + 5, sy1), ColorAlpha(TrackColor(row), rowMuted ? 40 : 110), 1.0f);   // a sliver, so every row keeps its colour
            }
            char nameBuf[16];
            std::snprintf(nameBuf, sizeof(nameBuf), "%.8s", TrackDisplayName(t).c_str());
            ImVec2 ts = ImGui::CalcTextSize(nameBuf);
            bool sel = app.seqCursorTrack == row;
            DrawInstrumentGlyph(dl, row, ImVec2(a.x + 16.0f, a.y + (rowH - 1.0f) * 0.5f), std::clamp(rowH - 8.0f, 11.0f, 15.0f), rowMuted ? ColorAlpha(TrackColor(row), 90) : TrackColor(row));   // v44.37: the instrument's pictogram
            dl->AddText(ImVec2(a.x + 28.0f, a.y + (rowH - 1.0f - ts.y) * 0.5f),
                        rowMuted ? IM_COL32(110, 114, 130, 255) : (sel ? vis::kAccentHi : vis::kText),
                        nameBuf);
        }
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        ImGui::SetCursorPos(ImVec2(origin.x + nameW + 2.0f, ry + 3.0f));
        if (ToggleButton("M", rowMuted, ImVec2(btnW, btnH), ToggleRed())) ToggleTrackMute(app, row);
        if (ImGui::IsItemHovered()) Hint(onBrick ? "M: this instrument is off in this brick - the same switch as its bar in WHO PLAYS." : "M: mute this instrument in this scene.");
        ImGui::SetCursorPos(ImVec2(origin.x + nameW + 2.0f + btnW + 4.0f, ry + 3.0f));
        if (ToggleButton("S", rowSolo, ImVec2(btnW, btnH))) ToggleTrackSolo(app, row);
        if (ImGui::IsItemHovered()) Hint(onBrick ? "S: only this instrument plays in this brick (all the others go off in WHO PLAYS). Press again to bring back what was playing." : "S: solo this instrument in this scene.");
        ImGui::PopStyleVar();

        // ---- 16 step cells of the current page ------------------------
        int trackLen = t.useOwnLength ? t.length : pat.masterLength;
        int playStep = app.sequencer.StepForTrack(row);
        bool playing = app.sequencer.IsPlaying() && app.EditIsPlaying();
        float cx = origin.x + kLabelW;
        for (int c = 0; c < kPageSteps; ++c) {
            int col = base + c;
            if (c > 0) cx += kCellGap + ((c % kGroup == 0) ? kGroupGap : 0.0f);
            if (col >= kMaxSteps) { cx += cellW; continue; }
            bool inRange = col < trackLen;
            Step& s = t.steps[col];
            bool shadedGroup = ((c / kGroup) % 2) == 1;

            ImGui::SetCursorPos(ImVec2(cx, ry + 1.0f));
            ImVec2 size(cellW, rowH - 4.0f);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::PushID(c);
            ImGui::InvisibleButton("cell", size);
            bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            ImGui::PopID();

            if (hovered && !cellsLocked) { hoverTrack_ = row; hoverStep_ = col; }
            if (cellsLocked) {
                // a length drag owns the mouse; no painting / moving meanwhile
            } else if (inRange && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && s.active) {
                // press on a lit step: wait to see if it becomes a move
                app.seqCursorTrack = row;
                app.seqCursorStep = col;
                moveCandidate_ = true;
                moveActive_ = false;
                moveSrcTrack_ = row;
                moveSrcStep_ = col;
            } else if (inRange && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                app.seqCursorTrack = row;
                app.seqCursorStep = col;
                bool newValue = !s.active;
                SetStepActiveExplicit(app, row, col, newValue);
                dragPainting_ = true;
                dragPaintValue_ = newValue;
                dragLastTrack_ = row;
                dragLastStep_ = col;
            } else if (inRange && dragPainting_ && hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                       !(dragLastTrack_ == row && dragLastStep_ == col)) {
                SetStepActiveExplicit(app, row, col, dragPaintValue_);
                dragLastTrack_ = row;
                dragLastStep_ = col;
            }
            if (!cellsLocked && inRange && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                app.seqCursorTrack = row;
                app.seqCursorStep = col;
                popupTrack_ = row;
                popupStep_ = col;
                popupOpenRequest_ = true;
            }

            ImVec2 p1(p0.x + size.x, p0.y + size.y);
            if (inRange && s.active) {
                DrawStepFace(dl, p0, p1, StepColor(row, s, shadedGroup), s.probability, StepIsAccent(s), 4.0f);
                DrawStepBadges(dl, p0, p1, s.ratchet, s.probability);
                DrawMicroBadges(dl, p0, p1, s.nudge, StepIsGhost(s));
                if (s.slide && t.type != TrackType::Drum)
                    DrawTieGlyph(dl, ImVec2(p0.x + 6.0f, p1.y - 6.0f), IM_COL32(18, 20, 26, 230));
            } else {
                dl->AddRectFilled(p0, p1, inRange ? StepColor(row, s, shadedGroup) : vis::kInset, 4.0f);
            }
            if (playing && inRange && playStep == col) {
                dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 230), 4.0f, 0, 2.0f);
            }
            if (moveActive_ && hovered && inRange && !(row == moveSrcTrack_ && col == moveSrcStep_)) {
                dl->AddRect(p0, p1, vis::kTeal, 4.0f, 0, 2.0f);
            }
            if (moveActive_ && row == moveSrcTrack_ && col == moveSrcStep_) {
                dl->AddRect(p0, p1, IM_COL32(242, 163, 61, 200), 4.0f, 0, 2.0f);
            }
            cx += cellW;
        }
        // ---- note-length bars (pitched tracks): translucent tail across the
        //      cells a held note covers, a grip at its end, a link where tied
        if (t.type != TrackType::Drum) {
            const float ty0 = win.y + ry + 1.0f - ImGui::GetScrollY();
            const float ty1 = ty0 + rowH - 4.0f;
            const float sx = ImGui::GetScrollX();
            const ImU32 base32 = TrackColor(row);
            for (int col = 0; col < std::min(trackLen, base + kPageSteps); ++col) {
                const Step& st = t.steps[col];
                if (!st.active) continue;
                int span = StepSpan(st, col, trackLen);
                int sc = col - base, ec = col + span - 1 - base;
                if (ec < 0) continue;
                const bool tied = StepIsTied(t, st, trackLen);
                if (span > 1) {
                    int from = std::max(sc, 0), to = std::min(ec, kPageSteps - 1);
                    float fx = (sc >= 0) ? geom.X(sc) + cellW : geom.X(0);
                    float tx = geom.X(to) + cellW;
                    if (tx > fx + 0.5f) {
                        float a = (st.probability < 100) ? 60.0f : 105.0f;
                        dl->AddRectFilled(ImVec2(win.x + fx - sx - 1.0f, ty0 + 3.0f), ImVec2(win.x + tx - sx, ty1 - 3.0f),
                                          ColorAlpha(base32, static_cast<int>(a)), 3.0f);
                    }
                    (void)from;
                }
                if (ec <= kPageSteps - 1) {
                    // grip: light bar at the very end of the note
                    float gx = win.x + geom.X(ec) + cellW - sx;
                    bool hot = resizing_ && resizeTrack_ == row && resizeStep_ == col;
                    dl->AddRectFilled(ImVec2(gx - 3.0f, ty0 + 5.0f), ImVec2(gx - 1.0f, ty1 - 5.0f),
                                      hot ? IM_COL32(255, 255, 255, 255) : ColorAlpha(ColorLighten(base32, 0.6f), 230), 1.0f);
                    if (tied && !st.slide && ec >= 0) {   // full-loop note: ties into its own restart
                        DrawTieGlyph(dl, ImVec2(gx - 10.0f, ty1 - 6.0f), IM_COL32(18, 20, 26, 230));
                    }
                } else {
                    float gx = win.x + geom.X(kPageSteps - 1) + cellW - sx;
                    dl->AddTriangleFilled(ImVec2(gx - 6.0f, ty0 + 6.0f), ImVec2(gx - 6.0f, ty1 - 6.0f), ImVec2(gx - 1.0f, (ty0 + ty1) * 0.5f),
                                          ColorAlpha(ColorLighten(base32, 0.6f), 220));
                }
            }
        }
        ImGui::PopID();
    }

    // ---- footer hint ------------------------------------------------------
    ImGui::SetCursorPos(ImVec2(origin.x, rowsTop + kNumTracks * rowH + 4.0f));
    if (app.CurrentPatternIsEmpty()) {   // v44.12: an empty scene offers a ready groove in place of the hint (the line keeps its height)
        if (MiniButton("STARTER GROOVE")) app.LoadStarterGroove();
        if (ImGui::IsItemHovered()) Hint("A slow, warm beat at 72 BPM to start from. Change anything.");
        ImGui::SameLine(0, 8);
        ImGui::TextDisabled("or click the squares to place hits  -  Space plays");
    } else {
        ImGui::TextDisabled("click / drag = paint  -  drag a lit step = move (%s copies)  -  drag a note's right edge = length  -  right-click = options", interaction::OptionName());
    }

    // ---- drag-to-move resolution -----------------------------------------
    if (moveCandidate_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (hoverTrack_ != moveSrcTrack_ || hoverStep_ != moveSrcStep_) moveActive_ = true;
            if (moveActive_) {
                if (interaction::IsCopy()) ValueTip("copy step"); else ValueTip("move step  (%s = copy)", interaction::OptionName());
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
        } else {
            // released
            if (!moveActive_) {
                SetStepActiveExplicit(app, moveSrcTrack_, moveSrcStep_, false);   // a plain click erases
            } else if (hoverTrack_ >= 0 && hoverStep_ >= 0 &&
                       !(hoverTrack_ == moveSrcTrack_ && hoverStep_ == moveSrcStep_)) {
                Pattern& mp = app.CurrentPattern();
                Track& st = app.StepTrack(mp, moveSrcTrack_);
                Track& dt = app.StepTrack(mp, hoverTrack_);
                int dLen = dt.useOwnLength ? dt.length : mp.masterLength;
                if (st.type == dt.type && hoverStep_ < dLen) {
                    app.PushUndo();
                    Step tmp = st.steps[moveSrcStep_];
                    if (!interaction::IsCopy()) st.steps[moveSrcStep_] = Step{};
                    dt.steps[hoverStep_] = tmp;
                    app.seqCursorTrack = hoverTrack_;
                    app.seqCursorStep = hoverStep_;
                }
            }
            moveCandidate_ = false;
            moveActive_ = false;
        }
    }
    hoverTrack_ = hoverStep_ = -1;

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        dragPainting_ = false;
        dragLastTrack_ = -1;
        dragLastStep_ = -1;
    }

    if (popupTrack_ >= 0 && popupStep_ >= 0) {
        DrawStepEditPopup("SeqStepPopup", popupOpenRequest_, app, popupTrack_, popupStep_);
    }
    popupOpenRequest_ = false;

    ImGui::PopID();
}

} // namespace t8::desktop
