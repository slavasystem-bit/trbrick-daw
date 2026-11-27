// t8_uitest (audit v44.58 TEST-5): runs the REAL screen of the program with no window and clicks around in it.
//
// Before, the "UI tests" only searched the source code for text, so a panel could crash on the second click while every test was
// green (that is how the CHORD LAB crash of v44.44-45 got through). This test draws real frames with the very same function the
// program uses (DrawShellFrame, src/ui_desktop/Shell.cpp), with the same panels, the same AppState and the real sound engine
// (one buffer of audio is rendered after every frame, as the sound card would ask for it).
//
//   1. TOUR: every tab, every tool (MIX LAB, CHORD LAB, MASTER FX), every track selected, NOTES / SOUND / MIX on TRACK, the groove
//      window, the help screen, an empty and a filled track, playing and stopped, at three window sizes.
//   2. SWEEP: in 11 screen setups every control with a label is pressed once (the setup is made again before each press).
//   3. MONKEY: random clicks, double clicks, right clicks, drags, the wheel and keys (with and without Cmd / Shift / Option) all
//      over the screen, a few hundred frames per seed.
//
// It fails on: a crash (the process dies, CI shows it), any ImGui assertion (kept on here even in Release, see uitest_imconfig.h:
// unbalanced Begin/End, Push/Pop, ID clashes ...), the scene lock not held where the scene is changed, a NaN in the audio.
// The test is run by tools/run_tests.sh in a throw-away folder; HOME points into it and PATH is emptied, so a click on "Reveal in
// Finder" or "Choose folder" cannot open anything on the machine and every file the program writes stays in that folder.
//
//   t8_uitest [--seeds N] [--frames N]     (default 6 seeds x 500 frames)
#include "imgui.h"
#include "imgui_internal.h"
#include "AppBootstrap.h"
#include "core/Persistence.h"
#include "core/SampleLibrary.h"
#include "ui_desktop/Shell.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using namespace t8;
using namespace t8::desktop;

namespace {
std::string g_phase = "start";
int g_frame = 0;
}

// ---- ImGui test-engine hooks: every control drawn in the current frame, with its rectangle and label ----
namespace {
struct SeenControl { ImGuiID id = 0; ImRect bb; std::string label; };
std::vector<SeenControl> g_seen;        // this frame
std::vector<SeenControl> g_lastFrame;   // the frame before (what is on screen now)
}
void ImGuiTestEngineHook_ItemAdd(ImGuiContext*, ImGuiID id, const ImRect& bb, const ImGuiLastItemData*) {
    if (id != 0) g_seen.push_back({id, bb, std::string()});
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags) {
    if (!label) return;
    for (auto it = g_seen.rbegin(); it != g_seen.rend(); ++it) if (it->id == id) { it->label = label; return; }
}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }

void T8UiAssertFailed(const char* expr, const char* file, int line) {
    std::fprintf(stderr, "\nUITEST FAIL: ImGui assertion `%s` (%s:%d)\n  while: %s, frame %d\n", expr, file, line, g_phase.c_str(), g_frame);
    std::fflush(stderr);
    std::_Exit(3);
}

namespace {

struct Runner {
    Runner(AppState& a, ShellPanels& p, const DesktopFonts& f) : app(a), panels(p), fonts(f) {}
    AppState& app;
    ShellPanels& panels;
    DesktopFonts fonts;
    ImVec2 size{1440.0f, 900.0f};
    int lockMisses = 0, badAudio = 0, frames = 0, spaceFails = 0;
    int clicks = 0, clicksOnItems = 0, swept = 0;   // how many clicks landed on a button / field / grid cell (the test must really press things)
    std::vector<ImGuiID> pressed;        // every control that became active at least once
    std::vector<float> audio;

    // No renderer: the font textures are "uploaded" by marking them ready, the way a backend would after copying them.
    static void FakeTextures() {
        for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
            if (tex->Status == ImTextureStatus_WantCreate) { tex->SetTexID(static_cast<ImTextureID>(1)); tex->SetStatus(ImTextureStatus_OK); }
            else if (tex->Status == ImTextureStatus_WantUpdates) tex->SetStatus(ImTextureStatus_OK);
            else if (tex->Status == ImTextureStatus_WantDestroy) { tex->SetTexID(ImTextureID_Invalid); tex->SetStatus(ImTextureStatus_Destroyed); }
        }
    }

    // One frame in the order of main.cpp: what arrived from the loader, the screen, then one buffer of sound.
    void Frame() {
        ++g_frame; ++frames;
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = size;
        io.DeltaTime = 1.0f / 60.0f;
        std::string msg, path;
        if (app.recorder.TakeResult(msg, path) || app.engine.Multitrack().TakeResult(msg, path)) app.SetStatus(msg);
        app.loader.Poll();
        app.PollSamplePick();
        if (app.statusMessageTimer > 0.0f) app.statusMessageTimer -= io.DeltaTime;
        app.SyncSceneKits();
        if (app.engine.ConsumeDspFault()) app.engine.ResetDsp();
        app.quitRequested = false;   // Cmd+Q / File > Quit only stop the program's loop; here the test goes on

        g_seen.clear();
        ImGui::GetCurrentContext()->TestEngineHookItems = true;
        ImGui::NewFrame();
        DrawShellFrame(app, panels, fonts);
        ImGui::Render();
        g_lastFrame.swap(g_seen);
        if (const ImGuiID id = ImGui::GetCurrentContext()->ActiveId; id != 0 && std::find(pressed.begin(), pressed.end(), id) == pressed.end()) pressed.push_back(id);
        FakeTextures();
        app.TickDocument(io.DeltaTime);

        const int n = std::max(64, app.engine.SampleRate() / 60);
        audio.assign(static_cast<size_t>(n) * 2, 0.0f);
        AudioCallback(&app, reinterpret_cast<Uint8*>(audio.data()), n * 2 * static_cast<int>(sizeof(float)));
        for (float v : audio) if (!std::isfinite(v)) { ++badAudio; std::fprintf(stderr, "UITEST FAIL: NaN / inf in the audio while: %s, frame %d\n", g_phase.c_str(), g_frame); break; }

        const int miss = app.sequencer.SceneLockViolations();
        if (miss > 0) {
            ++lockMisses;
            std::fprintf(stderr, "UITEST FAIL: the scene was changed without the scene lock while: %s, frame %d\n", g_phase.c_str(), g_frame);
            app.sequencer.ResetSceneLockViolations();
        }
    }
    void Frames(int n) { for (int i = 0; i < n; ++i) Frame(); }

    // ---- input, as the SDL backend would send it ----
    static void Mods(bool cmd, bool shift, bool alt) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, cmd);
        io.AddKeyEvent(ImGuiMod_Shift, shift);
        io.AddKeyEvent(ImGuiMod_Alt, alt);
    }
    void Key(ImGuiKey k, bool cmd = false, bool shift = false, bool alt = false) {
        ImGuiIO& io = ImGui::GetIO();
        Mods(cmd, shift, alt);
        io.AddKeyEvent(k, true);
        Frame();
        io.AddKeyEvent(k, false);
        Mods(false, false, false);
        Frame();
    }
    void Click(ImVec2 at, int button = 0, bool twice = false) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(at.x, at.y);
        Frame();
        ++clicks;
        if (ImGui::GetCurrentContext()->HoveredId != 0) ++clicksOnItems;
        for (int i = 0; i < (twice ? 2 : 1); ++i) {
            io.AddMouseButtonEvent(button, true); Frame();
            io.AddMouseButtonEvent(button, false); Frame();
        }
    }
    void Drag(ImVec2 from, ImVec2 to, int steps) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(from.x, from.y); Frame();
        io.AddMouseButtonEvent(0, true); Frame();
        for (int i = 1; i <= steps; ++i) {
            const float t = static_cast<float>(i) / steps;
            io.AddMousePosEvent(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t);
            Frame();
        }
        io.AddMouseButtonEvent(0, false); Frame();
    }
    // Leaves no popup, text box, tool or help screen open (Esc twice, then a click on an empty spot of the tab row).
    void Calm() {
        Key(ImGuiKey_Escape);
        Key(ImGuiKey_Escape);
        app.helpOpen = false;
    }
};

void Tour(Runner& r) {
    AppState& app = r.app;
    const ImVec2 sizes[] = {{1440.0f, 900.0f}, {800.0f, 520.0f}, {2560.0f, 1440.0f}};
    const AppState::Tool tools[] = {AppState::Tool::None, AppState::Tool::MixLab, AppState::Tool::ChordLab, AppState::Tool::MasterFx};
    for (const ImVec2& sz : sizes) {
        r.size = sz;
        for (int pass = 0; pass < 2; ++pass) {   // pass 0: the track is as the project left it; pass 1: with a brick, playing
            if (pass == 1) {
                SceneLock lock(app.sequencer.PatternMutex(), "UITEST");
                if (app.project.songChainLength == 0) app.ThrowIntoChain();
            }
            if (pass == 1 && !app.sequencer.IsPlaying()) app.sequencer.Play();
            for (int tab : {2, 1}) {
                app.uiTabRequest = tab;
                r.Frames(2);
                for (AppState::Tool t : tools) {
                    app.OpenTool(t);
                    for (int tr = 0; tr < kNumTracks; ++tr) {
                        char ph[160];
                        std::snprintf(ph, sizeof ph, "tour %gx%g %s tool %d track %d%s", sz.x, sz.y, tab == 2 ? "LIVE" : "TRACK", static_cast<int>(t), tr, pass ? " playing" : "");
                        g_phase = ph;
                        app.seqCursorTrack = tr;
                        r.Frames(2);
                        if (t == AppState::Tool::None && tab == 1)
                            for (int mode = 0; mode < 3; ++mode) { app.trackEditMode = mode; r.Frames(2); }
                        app.trackEditMode = 0;
                    }
                }
                app.OpenTool(AppState::Tool::None);
                g_phase = std::string("tour groove window / help / closed roll, ") + (tab == 2 ? "LIVE" : "TRACK");
                app.grooveViewOpen = true; r.Frames(3); app.grooveViewOpen = false;
                app.deskRollClosed = true; app.deskLanesClosed = true; r.Frames(3); app.deskRollClosed = false; app.deskLanesClosed = false;
                app.helpOpen = true; r.Frames(3); app.helpOpen = false;
                r.Frames(2);
            }
        }
        app.sequencer.Stop();
        r.Frames(2);
    }
    r.size = sizes[0];
    g_phase = "tour keys: tab, tools, help";
    for (ImGuiKey k : {ImGuiKey_Tab, ImGuiKey_M, ImGuiKey_M, ImGuiKey_C, ImGuiKey_Escape, ImGuiKey_F, ImGuiKey_F, ImGuiKey_Tab, ImGuiKey_Space, ImGuiKey_Space})
        r.Key(k);
}

// Every labelled control on the screen, pressed once each, in each screen setup (TEST-5 review: random clicks alone found the
// missing scene locks only after ~60 seeds). Before each press the setup is made again, after it Esc twice closes what opened.
void Sweep(Runner& r) {
    AppState& app = r.app;
    struct Setup { const char* name; int tab; AppState::Tool tool; int mode; int track; };
    const Setup setups[] = {
        {"LIVE", 2, AppState::Tool::None, 0, 0}, {"LIVE synth", 2, AppState::Tool::None, 0, 7},
        {"LIVE MIX LAB", 2, AppState::Tool::MixLab, 0, 0}, {"LIVE CHORD LAB", 2, AppState::Tool::ChordLab, 0, 9},
        {"LIVE MASTER FX", 2, AppState::Tool::MasterFx, 0, 0},
        {"TRACK NOTES", 1, AppState::Tool::None, 0, 0}, {"TRACK NOTES synth", 1, AppState::Tool::None, 0, 7},
        {"TRACK SOUND drum", 1, AppState::Tool::None, 1, 0}, {"TRACK SOUND synth", 1, AppState::Tool::None, 1, 7},
        {"TRACK MIX", 1, AppState::Tool::None, 2, 0}, {"TRACK CHORD LAB", 1, AppState::Tool::ChordLab, 0, 9},
    };
    r.size = ImVec2(1440.0f, 900.0f);
    for (const Setup& su : setups) {
        auto setUp = [&]() {
            if (su.tab == 1 && app.project.songChainLength == 0) { SceneLock lock(app.sequencer.PatternMutex(), "UITEST"); app.ThrowIntoChain(); }
            app.helpOpen = false;
            app.uiTabRequest = su.tab;
            app.OpenTool(su.tool);
            app.trackEditMode = su.mode;
            app.seqCursorTrack = su.track;
            app.grooveViewOpen = false;
            r.Frames(2);
        };
        setUp();
        std::vector<SeenControl> controls;
        for (const SeenControl& c : g_lastFrame) {
            if (c.label.empty() || c.label.compare(0, 2, "##") == 0) continue;
            if (c.bb.GetWidth() < 2.0f || c.bb.GetHeight() < 2.0f || c.bb.Max.x < 0 || c.bb.Max.y < 0 || c.bb.Min.x > r.size.x || c.bb.Min.y > r.size.y) continue;
            bool dup = false;
            for (const SeenControl& d : controls) if (d.id == c.id) dup = true;
            if (!dup) controls.push_back(c);
        }
        for (const SeenControl& c : controls) {
            const std::string shown = c.label.substr(0, c.label.find("##"));
            g_phase = std::string("sweep ") + su.name + ": press \"" + shown + "\"";
            setUp();
            const SeenControl* now = nullptr;
            for (const SeenControl& d : g_lastFrame) if (d.id == c.id) now = &d;
            if (!now) continue;   // gone after an earlier press changed the screen
            r.Click(now->bb.GetCenter());
            ++r.swept;
            r.Calm();
        }
    }
    app.OpenTool(AppState::Tool::None);
    app.trackEditMode = 0;
    r.Frames(2);
}

void Monkey(Runner& r, unsigned seed, int frames) {
    std::mt19937 rng(seed);
    auto R = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
    auto P = [&]() { return ImVec2(static_cast<float>(R(static_cast<int>(r.size.x))), static_cast<float>(R(static_cast<int>(r.size.y)))); };
    static const ImGuiKey keys[] = {
        ImGuiKey_Space, ImGuiKey_Escape, ImGuiKey_Tab, ImGuiKey_Enter, ImGuiKey_Delete, ImGuiKey_Backspace,
        ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_UpArrow, ImGuiKey_DownArrow,
        ImGuiKey_A, ImGuiKey_C, ImGuiKey_D, ImGuiKey_F, ImGuiKey_M, ImGuiKey_R, ImGuiKey_V, ImGuiKey_Z, ImGuiKey_X,
        ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4, ImGuiKey_5, ImGuiKey_6, ImGuiKey_7, ImGuiKey_8, ImGuiKey_9, ImGuiKey_0,
        ImGuiKey_Q, ImGuiKey_W, ImGuiKey_E, ImGuiKey_T, ImGuiKey_Y, ImGuiKey_U, ImGuiKey_I, ImGuiKey_O, ImGuiKey_P,
        ImGuiKey_Slash, ImGuiKey_Minus, ImGuiKey_Equal, ImGuiKey_LeftBracket, ImGuiKey_RightBracket,
    };
    const int start = r.frames;
    int action = 0;
    while (r.frames - start < frames) {
        char ph[96];
        std::snprintf(ph, sizeof ph, "monkey seed %u action %d", seed, action++);
        g_phase = ph;
        const int a = R(100);
        if (a < 40) r.Click(P());
        else if (a < 48) r.Click(P(), 0, true);
        else if (a < 54) r.Click(P(), 1);
        else if (a < 64) { const ImVec2 f = P(); r.Drag(f, ImVec2(f.x + static_cast<float>(R(301) - 150), f.y + static_cast<float>(R(301) - 150)), 2 + R(6)); }
        else if (a < 70) { const ImVec2 at = P(); ImGui::GetIO().AddMousePosEvent(at.x, at.y); ImGui::GetIO().AddMouseWheelEvent(0.0f, static_cast<float>(R(5) - 2)); r.Frame(); }
        else if (a < 92) {
            const ImGuiKey k = keys[R(static_cast<int>(sizeof keys / sizeof keys[0]))];
            const int m = R(10);   // mostly plain keys; some with Cmd, Shift or Option
            r.Key(k, m == 7 || m == 9, m == 8 || m == 9, m == 6);
        }
        else if (a < 97) { r.app.uiTabRequest = 1 + R(2); r.Frame(); }
        else r.Calm();
        if (R(40) == 0) { r.size = ImVec2(static_cast<float>(800 + R(1761)), static_cast<float>(520 + R(921))); }   // the window is resized
    }
    r.Calm();
}

// Space = play / stop on every screen, with every tool open, after chords were played live in CHORD LAB, and when the UI missed
// the key-up of an earlier Space (urgent bug 2026-11-27: on a Mac Space stopped working after CHORD LAB).
void SpaceKey(Runner& r) {
    AppState& app = r.app;
    auto expectToggle = [&](const char* what) {
        const bool was = app.sequencer.IsPlaying();
        r.Key(ImGuiKey_Space);
        if (app.sequencer.IsPlaying() == was) { ++r.spaceFails; std::fprintf(stderr, "UITEST FAIL: Space did not %s: %s\n", was ? "stop" : "start", what); }
    };
    const AppState::Tool tools[] = {AppState::Tool::None, AppState::Tool::MixLab, AppState::Tool::ChordLab, AppState::Tool::MasterFx};
    for (int tab : {2, 1}) {
        if (tab == 1 && app.project.songChainLength == 0) { SceneLock lock(app.sequencer.PatternMutex(), "UITEST"); app.ThrowIntoChain(); }
        for (AppState::Tool t : tools) {
            char ph[96]; std::snprintf(ph, sizeof ph, "space: %s, tool %d", tab == 2 ? "LIVE" : "TRACK", static_cast<int>(t));
            g_phase = ph;
            app.uiTabRequest = tab; app.OpenTool(t); r.Frames(3);
            expectToggle(ph); r.Frames(20); expectToggle(ph);
        }
        // chords played in CHORD LAB: heard while stopped, live while the loop plays
        app.uiTabRequest = tab; app.OpenTool(AppState::Tool::ChordLab); r.Frames(3);
        g_phase = std::string("space after chords in CHORD LAB, ") + (tab == 2 ? "LIVE" : "TRACK");
        std::vector<ImVec2> chips;
        for (const SeenControl& c : g_lastFrame) if (c.label == "##chip") chips.push_back(c.bb.GetCenter());
        if (chips.empty()) { ++r.spaceFails; std::fprintf(stderr, "UITEST FAIL: no chord chips in CHORD LAB\n"); }
        for (size_t i = 0; i < chips.size() && i < 3; ++i) { r.Click(chips[i]); r.Frames(5); }
        expectToggle(g_phase.c_str());
        for (size_t i = 3; i < chips.size() && i < 6; ++i) { r.Click(chips[i]); r.Frames(30); }
        expectToggle(g_phase.c_str());
        // a right-click on a chord of the loop opens its menu; after Esc Space works again (the menu used to be opened under the
        // wrong id: it never showed and every key stayed dead)
        app.sequencer.Stop(); r.Frames(2);
        if (!chips.empty()) { r.Click(chips[0], 0, true); r.Frames(3); }   // double-click: the chord goes into the loop
        std::vector<ImVec2> blocks;
        for (const SeenControl& c : g_lastFrame) if (c.label == "##blk") blocks.push_back(c.bb.GetCenter());
        if (blocks.empty()) { ++r.spaceFails; std::fprintf(stderr, "UITEST FAIL: no chord in the CHORD LAB loop to right-click\n"); }
        else {
            g_phase = std::string("space after the right-click menu of a loop chord, ") + (tab == 2 ? "LIVE" : "TRACK");
            r.Click(blocks[0], 1); r.Frames(2);
            bool shown = false;
            for (const ImGuiPopupData& pd : ImGui::GetCurrentContext()->OpenPopupStack) if (pd.Window && pd.Window->Active) shown = true;
            if (!shown) { ++r.spaceFails; std::fprintf(stderr, "UITEST FAIL: the right-click menu of a loop chord did not show\n"); }
            r.Key(ImGuiKey_Escape); r.Frames(2);
            expectToggle(g_phase.c_str());
        }
        app.sequencer.Stop(); app.OpenTool(AppState::Tool::None); r.Frames(2);
    }
    // the key-up of a Space got lost: the UI still holds Space, the window reports a new press
    g_phase = "space after a lost key-up";
    app.uiTabRequest = 2; r.Frames(2);
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_Space, true); ++app.spaceDownsRaw; r.Frame();   // starts
    r.Frames(3);
    const bool was = app.sequencer.IsPlaying();
    ++app.spaceDownsRaw; r.Frame();   // a new press, no key-up in between
    if (app.sequencer.IsPlaying() == was) { ++r.spaceFails; std::fprintf(stderr, "UITEST FAIL: Space after a lost key-up did nothing\n"); }
    io.AddKeyEvent(ImGuiKey_Space, false); r.Frame();
    app.sequencer.Stop(); r.Frames(2);
}

} // namespace

int main(int argc, char** argv) {
    int seeds = 6, frames = 500;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seeds") seeds = std::max(0, std::atoi(argv[++i]));
        else if (a == "--frames") frames = std::max(10, std::atoi(argv[++i]));
    }
    // Everything the program writes (projects, recordings, settings, Library) goes into the test's own folder; with no PATH, a
    // click on "Reveal" / "Choose folder" cannot start Finder, a file dialog or any other program.
    const std::string here = CurrentDir();
    const std::string home = here + "/uitest_home";
    setenv("HOME", home.c_str(), 1);
    setenv("TRB_DATA_DIR", (home + "/Documents/TR-Brick").c_str(), 1);
    setenv("TRB_SERVICE_DIR", (home + "/service").c_str(), 1);
    setenv("PATH", (here + "/uitest_no_programs").c_str(), 1);
    userdirs::EnsureDirs();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;   // FakeTextures() plays the renderer
    io.ConfigErrorRecoveryEnableAssert = true;
    DesktopFonts fonts = LoadDesktopFonts(io);
    ApplyDarkDawTheme(ImGui::GetStyle());

    auto appPtr = std::make_unique<AppState>();   // AppState is large: not on the stack
    AppState& app = *appPtr;
    app.projectPath = userdirs::UntitledPath();
    app.project.kitFolder = "kits/acid_v2";
    app.engine.Init(kSampleRate);
    app.recorder.Init(kSampleRate);
    if (app.engine.GetKit()->LoadFromFolder(app.project.kitFolder, kSampleRate) < kNumDrumTracks)
        std::fprintf(stderr, "uitest: the kit did not load (run from the repository root, see tools/run_tests.sh)\n");
    app.engine.Risers().LoadFromFolder("kits/risers", kSampleRate);
    app.StartSound();
    app.MarkSaved();
    app.sequencer.SetSongFollowsEdit(false);
    app.loader.Start();

    ShellPanels panels;
    Runner r{app, panels, fonts};
    r.Frames(3);
    SpaceKey(r);
    Tour(r);
    Sweep(r);
    const int tourFrames = r.frames;
    for (int s = 1; s <= seeds; ++s) Monkey(r, static_cast<unsigned>(s), frames);

    app.sequencer.Stop();
    app.recorder.Shutdown();
    app.engine.Multitrack().Shutdown();
    app.loader.Stop();
    ImGui::DestroyContext();

    std::printf("uitest: %d frames (tour + sweep %d, %d labelled controls pressed one by one; monkey %d seeds x %d), %d clicks (%d on a control), %zu different controls pressed, scene lock misses %d, bad audio blocks %d, Space failures %d\n",
                r.frames, tourFrames, r.swept, seeds, frames, r.clicks, r.clicksOnItems, r.pressed.size(), r.lockMisses, r.badAudio, r.spaceFails);
    // a test that clicks into nothing proves nothing: at least one click in five must land on a control and many different ones must be pressed
    const bool reached = r.swept >= 100 && (seeds == 0 || (r.clicksOnItems * 5 > r.clicks && r.pressed.size() >= 50));
    if (!reached) std::printf("UITEST FAIL: the clicks did not reach the controls (is the screen drawn?)\n");
    if (app.orphanPopupsClosed > 0) std::printf("UITEST FAIL: %d popup(s) opened under one id and drawn under another (see 'closed a popup that was never drawn' above)\n", app.orphanPopupsClosed);
    const bool ok = r.lockMisses == 0 && r.badAudio == 0 && r.spaceFails == 0 && app.orphanPopupsClosed == 0 && reached;
    std::printf("%s\n", ok ? "UI TEST PASSED" : "UI TEST FAILED");
    return ok ? 0 : 1;
}
