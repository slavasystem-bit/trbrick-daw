#include "DocumentDialogs.h"
#include "InfoLine.h"
#include "Interaction.h"
#include "Visual.h"
#include "imgui.h"
#include "core/UserDirs.h"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>
#include <vector>

namespace t8::desktop {

namespace {
const ImVec4 kAmber(0.95f, 0.64f, 0.24f, 1.0f);

std::string DocName(const AppState& app) {
    return app.IsUntitled() ? std::string("Untitled") : userdirs::ProjectDisplayName(app.projectPath);
}

void CenterNext() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.38f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
}

struct OpenItem { std::string path, name; long long mtime = 0; };
}

void DrawDocumentDialogs(AppState& app) {
    ImGui::PushFont(nullptr, 14.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));
    const ImGuiWindowFlags kModal = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;

    // ---- crash recovery ------------------------------------------------------------------------------------------------
    if (!app.docRecoverPath.empty() && !ImGui::IsPopupOpen("Recovered")) ImGui::OpenPopup("Recovered");
    CenterNext();
    if (ImGui::BeginPopupModal("Recovered", nullptr, kModal)) {
        ImGui::TextColored(kAmber, "TR-Brick did not close properly");
        ImGui::Text("There are unsaved changes of \"%s\" from the last session.", DocName(app).c_str());
        ImGui::TextDisabled("Restore them, or go on with the file as you saved it.");
        ImGui::Spacing();
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        // Return restores; Esc does nothing here on purpose (cancelling would have to mean throwing the work away)
        if (ImGui::Button("RESTORE", ImVec2(120, 0)) || t8::desktop::interaction::KeyConfirm()) { app.RestoreRecovery(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("DISCARD", ImVec2(120, 0))) { app.DiscardRecovery(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    // ---- Save / Don't Save / Cancel ---------------------------------------------------------------------------------------
    if (app.docAskUnsaved && !ImGui::IsPopupOpen("Unsaved")) ImGui::OpenPopup("Unsaved");
    CenterNext();
    if (ImGui::BeginPopupModal("Unsaved", nullptr, kModal)) {
        const char* what = app.docPending == AppState::DocAction::Quit ? "before quitting"
                         : app.docPending == AppState::DocAction::New ? "before making a new project"
                         : app.docPending == AppState::DocAction::Revert ? "before going back to the saved version" : "before opening another project";
        ImGui::TextColored(kAmber, "Save the changes to \"%s\" %s?", DocName(app).c_str(), what);
        ImGui::TextDisabled("If you do not save, the changes are lost.");
        ImGui::Spacing();
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        if (ImGui::Button("SAVE", ImVec2(110, 0)) || t8::desktop::interaction::KeyConfirm()) { ImGui::CloseCurrentPopup(); app.DocAnswerSave(); }
        ImGui::SameLine();
        if (ImGui::Button("DON'T SAVE", ImVec2(110, 0))) { ImGui::CloseCurrentPopup(); app.DocAnswerDiscard(); }
        ImGui::SameLine();
        if (ImGui::Button("CANCEL", ImVec2(110, 0)) || t8::desktop::interaction::KeyCancel()) { ImGui::CloseCurrentPopup(); app.DocAnswerCancel(); }
        ImGui::EndPopup();
    } else if (app.docAskUnsaved && !ImGui::IsPopupOpen("Unsaved")) {
        // the popup was closed from outside (a modal below it): do not leave the question hanging
        app.DocAnswerCancel();
    }

    // ---- name: Save As / Rename / Duplicate / the first Save ----------------------------------------------------------------
    static char nameBuf[96] = "";
    if (app.docNameOpen && !ImGui::IsPopupOpen("Name")) {
        std::string initial = DocName(app);
        if (app.docNameMode == AppState::NameMode::Duplicate) initial += " copy";
        if (app.IsUntitled() && app.docNameMode != AppState::NameMode::Duplicate) initial.clear();
        std::snprintf(nameBuf, sizeof nameBuf, "%s", initial.c_str());
        ImGui::OpenPopup("Name");
    }
    CenterNext();
    if (ImGui::BeginPopupModal("Name", nullptr, kModal)) {
        const char* title = app.docNameMode == AppState::NameMode::Rename ? "Rename the project"
                          : app.docNameMode == AppState::NameMode::Duplicate ? "Duplicate the project as"
                          : app.IsUntitled() ? "Name the project" : "Save the project as";
        ImGui::TextColored(kAmber, "%s", title);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(340);
        const bool enter = ImGui::InputTextWithHint("##docname", "project name", nameBuf, sizeof nameBuf, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::TextDisabled("Goes to %s", userdirs::Tilde(userdirs::ProjectsDir()).c_str());
        ImGui::TextDisabled("A project is never replaced: a name that exists gets (2).");
        ImGui::Spacing();
        const bool hasName = nameBuf[0] != '\0' && std::string(nameBuf).find_first_not_of(' ') != std::string::npos;
        ImGui::BeginDisabled(!hasName);
        const bool ok = ImGui::Button("SAVE", ImVec2(110, 0)) || (enter && hasName);
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool cancel = ImGui::Button("CANCEL", ImVec2(110, 0)) || t8::desktop::interaction::KeyCancel();
        if (ok) { app.DocNameConfirm(nameBuf); if (!app.docNameOpen) ImGui::CloseCurrentPopup(); }
        if (cancel) { app.DocNameCancel(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    // ---- Open: the projects, newest first -----------------------------------------------------------------------------------
    static std::vector<OpenItem> items;
    static int pick = -1;
    if (app.docOpenDialog && !ImGui::IsPopupOpen("Open")) {
        items.clear();
        for (const std::string& p : ListProjectFiles(DirNameOf(app.projectPath))) {
            OpenItem it; it.path = p; it.name = userdirs::ProjectDisplayName(p);
            struct stat st{};
            if (::stat(p.c_str(), &st) == 0) it.mtime = static_cast<long long>(st.st_mtime);
            items.push_back(it);
        }
        std::sort(items.begin(), items.end(), [](const OpenItem& a, const OpenItem& b) { return a.mtime > b.mtime; });
        pick = -1;
        ImGui::OpenPopup("Open");
    }
    CenterNext();
    if (ImGui::BeginPopupModal("Open", nullptr, kModal)) {
        ImGui::TextColored(kAmber, "Open a project");
        ImGui::TextDisabled("%s", userdirs::Tilde(userdirs::ProjectsDir()).c_str());
        if (!items.empty()) {   // the keyboard works the list too: Up / Down move, Return opens, Esc cancels
            const int n = static_cast<int>(items.size());
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) pick = std::min(n - 1, pick + 1);
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) pick = pick < 0 ? 0 : std::max(0, pick - 1);
        }
        ImGui::BeginChild("##openlist", ImVec2(460, 220), ImGuiChildFlags_Borders);
        if (items.empty()) ImGui::TextDisabled("(no projects yet: Save puts them here)");
        std::string chosen;
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            const OpenItem& it = items[static_cast<size_t>(i)];
            char when[32] = "";
            std::time_t t = static_cast<std::time_t>(it.mtime);
            std::tm tmv{};
            localtime_r(&t, &tmv);
            std::strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tmv);
            const bool open = it.path == app.projectPath;
            ImGui::PushID(i);
            if (ImGui::Selectable(it.name.c_str(), pick == i, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, 0))) {
                pick = i;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) chosen = it.path;
            }
            ImGui::SameLine(330);
            ImGui::TextDisabled("%s%s", open ? "open  " : "", when);
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::BeginDisabled(pick < 0);
        if (ImGui::Button("OPEN", ImVec2(110, 0)) || (pick >= 0 && t8::desktop::interaction::KeyConfirm())) chosen = items[static_cast<size_t>(pick)].path;
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool cancel = ImGui::Button("CANCEL", ImVec2(110, 0)) || t8::desktop::interaction::KeyCancel();
        if (!chosen.empty()) {
            app.docOpenDialog = false;
            ImGui::CloseCurrentPopup();
            if (chosen != app.projectPath) app.RequestDoc(AppState::DocAction::Open, chosen);
        } else if (cancel) { app.docOpenDialog = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    ImGui::PopStyleVar(2);
    ImGui::PopFont();
}

} // namespace t8::desktop
