// trbrick_daw_desktop: the Mac mouse+keyboard DAW. One borderless ImGui shell
// fills the window; the LIVE screen (header, scenes+chain, sequencer + mixer /
// piano roll, context block, random/kit + live) is the whole performance
// surface. Keyboard shortcuts live in ui_desktop/Shortcuts.cpp.
#include <SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "core/AudioEngine.h"
#include "core/Sequencer.h"
#include "core/Persistence.h"
#include "core/SampleLibrary.h"
#include "app/AppState.h"
#include "app/AudioSettings.h"
#include "ui_desktop/DesktopTheme.h"
#include "ui_desktop/HeaderPanel.h"
#include "ui_desktop/MenuBar.h"
#include "ui_desktop/ChainPanel.h"
#include "ui_desktop/ChordLabPanel.h"
#include "ui_desktop/BricksPanel.h"
#include "ui_desktop/SequencerPanel.h"
#include "ui_desktop/MixerPanel.h"
#include "ui_desktop/MixLabPanel.h"
#include "ui_desktop/LanePanel.h"
#include "ui_desktop/GroovePanel.h"
#include "ui_desktop/PianoRollPanel.h"
#include "ui_desktop/ContextPanel.h"
#include "ui_desktop/SoundPanel.h"
#include "ui_desktop/BottomPanel.h"
#include "ui_desktop/LiveLayout.h"
#include "ui_desktop/Shortcuts.h"
#include "ui_desktop/MixerSync.h"
#include "ui_desktop/InfoLine.h"
#include "ui_desktop/HelpScreen.h"
#include "ui_desktop/DocumentDialogs.h"
#include "imgui_internal.h"
#include "AppBootstrap.h" // FileExists/IsDir/CurrentDir/ChdirToProjectRoot/AudioCallback

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <mutex>
#include <algorithm>
#include <fstream>

using namespace t8;
using namespace t8::desktop;

int main(int argc, char** argv) {
    ChdirToProjectRoot(argv[0]);

    // v44.14: the user's files live in Documents/TR-Brick (Projects, Recordings), not next to the program. Older versions kept
    // them in the program folder: copy them over once (nothing is deleted there).
    userdirs::EnsureDirs();
    {   // U8c: beats, synth presets and favourites move into Documents/TR-Brick/Library
        const userdirs::LibraryMigration lm = userdirs::MigrateLibrary(".");
        if (lm.ran && lm.beats + lm.presets + lm.favorites > 0)
            std::fprintf(stderr, "Copied to %s: %d beats, %d synth presets, %d favourite lists.\n", userdirs::LibraryDir().c_str(), lm.beats, lm.presets, lm.favorites);
    }
    {
        const userdirs::MigrationResult mig = userdirs::MigrateLegacy(".");
        if (mig.ran && mig.projects + mig.recordings + mig.backups > 0)
            std::fprintf(stderr, "Copied earlier files to %s: %d projects, %d recordings/exports, %d backups.\n",
                         userdirs::Root().c_str(), mig.projects, mig.recordings, mig.backups);
    }

    std::string projectPath;
    std::string kitFolder = "kits/acid_v2";
    bool kitFolderExplicit = false;
    bool projectExplicit = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--project" && i + 1 < argc) { projectPath = argv[++i]; projectExplicit = true; }
        else if (a == "--kit" && i + 1 < argc) { kitFolder = argv[++i]; kitFolderExplicit = true; }
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // A real resizable desktop window.
    //
    // Sharpness (HiDPI / Retina): the whole UI is rendered at `uiScale`x into an
    // offscreen texture (ImGui rasterises glyphs at that density too) and that
    // texture is then stretched over the window's real drawable. This keeps the
    // layout correct whatever SDL reports about the backing scale (an earlier
    // attempt that trusted io.DisplayFramebufferScale drew the UI at half size
    // in a corner on one Mac): the UI always fills the window, and text is
    // crisp as long as uiScale >= the real backing scale. Default 2 on macOS
    // (Retina), otherwise the reported pixel/point ratio (>=1). Override with
    // --scale N / T8_SCALE=N (1..4; 1 = old soft behaviour, fastest).
    int uiScaleArg = 0;
    if (const char* es = std::getenv("T8_SCALE")) uiScaleArg = std::atoi(es);
    for (int i = 1; i + 1 < argc; ++i) if (std::string(argv[i]) == "--scale") uiScaleArg = std::atoi(argv[i + 1]);
    SDL_WindowFlags windowFlags = static_cast<SDL_WindowFlags>(
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Window* window = SDL_CreateWindow("TR-Brick DAW", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                           1440, 900, windowFlags);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetWindowMinimumSize(window, 800, 520);
    SDL_Renderer* sdlRenderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!sdlRenderer) sdlRenderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!sdlRenderer) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    {
        int ww = 0, wh = 0, pw = 0, ph = 0, wpw = 0, wph = 0;
        SDL_GetWindowSize(window, &ww, &wh);
        SDL_GetRendererOutputSize(sdlRenderer, &pw, &ph);
#if SDL_VERSION_ATLEAST(2, 26, 0)
        SDL_GetWindowSizeInPixels(window, &wpw, &wph);
#endif
        std::fprintf(stderr, "window %dx%d points, renderer output %dx%d, window pixels %dx%d \n",
                     ww, wh, pw, ph, wpw, wph);
    }

    int uiScale = 1;
    {
        int ww = 0, wh = 0, pw = 0, ph = 0;
        SDL_GetWindowSize(window, &ww, &wh);
        SDL_GetRendererOutputSize(sdlRenderer, &pw, &ph);
        float ratio = (ww > 0) ? static_cast<float>(pw) / ww : 1.0f;
        uiScale = std::max(1, static_cast<int>(std::ceil(ratio - 0.01f)));
#ifdef __APPLE__
        uiScale = std::max(uiScale, 2);
#endif
        if (uiScaleArg >= 1) uiScale = uiScaleArg;
        uiScale = std::min(uiScale, 4);
        std::fprintf(stderr, "UI render scale %dx (offscreen target, stretched to the window)\n", uiScale);
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_Texture* uiTarget = nullptr;
    int uiTargetW = 0, uiTargetH = 0;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // Keyboard navigation is deliberately OFF: the LIVE screen gives the
    // arrow keys a job of their own (LEFT / RIGHT select a parameter, UP /
    // DOWN change it -- see ContextPanel), and ImGui's own focus navigation
    // would otherwise move a highlight around the widgets at the same time.
    // No docking anymore (see this file's top comment) -- the shell is one
    // fixed-layout window, not user-rearrangeable panels, so there's
    // nothing meaningful for imgui.ini to persist between runs. Leaving it
    // off also means a stale .ini from the old dockable-windows layout
    // can't resurrect itself and confuse a rebuild.
    io.IniFilename = nullptr;

    DesktopFonts fonts = LoadDesktopFonts(io);
    ApplyDarkDawTheme(ImGui::GetStyle());

    ImGui_ImplSDL2_InitForSDLRenderer(window, sdlRenderer);
    ImGui_ImplSDLRenderer2_Init(sdlRenderer);

    // ---- engine/project/kit setup ----------------------------------------
    AppState app;
    bool hasDetailSetting = false;
    std::string lastProject;   // v44.14: the project that was open when the program was closed
    {   // remembered interface size (View > Interface Size)
        std::ifstream cf("ui_settings.cfg");
        std::string line;
        while (std::getline(cf, line)) {
            if (line.rfind("zoom=", 0) == 0) app.uiZoomIdx = std::clamp(std::atoi(line.c_str() + 5), 0, AppState::kNumUiZoom - 1);
            if (line.rfind("detail=", 0) == 0) { app.detailOpen = std::atoi(line.c_str() + 7) != 0; hasDetailSetting = true; }
            if (line.rfind("nextbar=", 0) == 0) app.quantizeSwitches = std::atoi(line.c_str() + 8) != 0;   // v44.54
            if (line.rfind("pitchrange=", 0) == 0) app.pitchRange16 = std::atoi(line.c_str() + 11) >= 16;   // v44.57 (the fader itself is not saved)
            if (line.rfind("lastproject=", 0) == 0) lastProject = line.substr(12);
            if (line.rfind("clickopen=", 0) == 0) app.clickOpens = std::atoi(line.c_str() + 10) != 0;   // v44.59: CLICK: LAUNCH | OPEN of the SCENES row
            if (line.rfind("audiobuf=", 0) == 0) app.audioBufferChoice = audiosettings::ValidBuffer(std::atoi(line.c_str() + 9));   // N1: Settings > Audio
            if (line.rfind("audioout=", 0) == 0) app.audioOutputChoice = line.substr(9);
        }
    }
    // v44.8: the very first launch of this version starts with the instrument block folded and the starter groove in scene 1
    // (a blank project only); the choice is remembered in ui_settings.cfg.
    const bool firstRunSettings = !hasDetailSetting;
    if (firstRunSettings) app.detailOpen = false;
    // v44.14: with no --project the program continues with the last open project (before, every start was a blank project that
    // silently overwrote project.t8proj); a first start gets a new "Untitled" in the Projects folder.
    if (!projectExplicit) {
        const std::string migrated = userdirs::ProjectsDir() + "/project.t8proj";
        if (!lastProject.empty() && FileExists(lastProject)) { projectPath = lastProject; projectExplicit = true; }   // also an Untitled that was never saved
        else if (FileExists(migrated)) { projectPath = migrated; projectExplicit = true; }
        else projectPath = userdirs::UntitledPath();   // U8b: a new project is Untitled (service folder) until it is saved with a name
    }
    std::string savedProject = lastProject;   // differs on the first frame when the setting is missing or stale, so it gets written
    int savedZoomIdx = app.uiZoomIdx;
    bool savedDetail = app.detailOpen;
    bool savedNextBar = app.quantizeSwitches;
    bool savedClickOpens = app.clickOpens;
    int savedAudioBuf = app.audioBufferChoice;
    std::string savedAudioOut = app.audioOutputChoice;
    auto writeUiSettings = [&]() {
        std::ofstream cf("ui_settings.cfg", std::ios::trunc);
        cf << "zoom=" << app.uiZoomIdx << "\ndetail=" << (app.detailOpen ? 1 : 0) << "\nnextbar=" << (app.quantizeSwitches ? 1 : 0) << "\npitchrange=" << (app.pitchRange16 ? 16 : 8) << "\nclickopen=" << (app.clickOpens ? 1 : 0) << "\naudiobuf=" << app.audioBufferChoice << "\naudioout=" << app.audioOutputChoice << "\nlastproject=" << app.projectPath << "\n";
    };
    if (firstRunSettings) writeUiSettings();
    float uiZoom = 1.0f;   // layout zoom of the previous frame (mouse positions are divided by it)
    app.projectPath = projectPath;
    if (projectExplicit && FileExists(projectPath)) {
        if (!LoadProject(app.project, projectPath)) {
            // DATA-2: say it on screen (the dialog offers the newest dated copy) and go on with a new Untitled project, so that Cmd+S
            // can never write an empty project where the damaged file was (it is kept as <name>.t8proj.corrupt).
            std::fprintf(stderr, "Failed to load %s, starting a new Untitled project\n", projectPath.c_str());
            app.NoteLoadFailed(projectPath);
            app.project = Project{};
            app.project.name = "Untitled";
            app.project.kitFolder = kitFolder;
            projectPath = userdirs::UntitledPath();
            std::remove(projectPath.c_str());
            app.projectPath = projectPath;
            projectExplicit = false;
        } else {
            app.docNewerFormat = LastLoadNewerFormat();   // DATA-1: Cmd+S saves a copy instead of writing over a newer file
            app.NoteLoadResult(projectPath);
            if (!LastLoadNote().empty()) { app.SetStatus(LastLoadNote()); app.statusMessageTimer = 8.0f; }
        }
        if (kitFolderExplicit) app.project.kitFolder = kitFolder;
    } else {
        app.project.kitFolder = kitFolder;
    }
    // U8b: an Untitled that was left in the service folder has never been saved (it counts as edited); for a named project a
    // recovery copy newer than the file means the last session ended with unsaved changes: the dialog asks what to do.
    const bool resumedUntitled = projectExplicit && FileExists(projectPath) && userdirs::IsUntitledPath(projectPath);
    if (!userdirs::IsUntitledPath(projectPath)) {
        const std::string rec = userdirs::RecoveryPathFor(projectPath);
        struct stat rs{}, ps{};
        if (::stat(rec.c_str(), &rs) == 0) {
            if (::stat(projectPath.c_str(), &ps) != 0 || rs.st_mtime > ps.st_mtime) app.docRecoverPath = rec;
            else std::remove(rec.c_str());   // older than the saved file: nothing to recover
        }
    }

    // Open the audio device FIRST, letting it pick its own sample rate (44.1 / 48 kHz ...),
    // and run the whole engine at that rate -- no resampling by SDL, no pitch / tempo drift.
    // The device starts paused; it is un-paused after the engine is set up (below).
    // N1 (LIVE-5 / LIVE-6): the output and the buffer size come from Settings > Audio (default: the system output, 128 frames =
    // ~2.9 ms at 44.1 kHz, a tight live feel). A chosen output that is not plugged in falls back to the system default.
    auto refreshAudioOutputs = [&]() {
        app.audioOutputs.clear();
        const int n = SDL_GetNumAudioDevices(0);
        for (int i = 0; i < n; ++i) if (const char* nm = SDL_GetAudioDeviceName(i, 0)) app.audioOutputs.emplace_back(nm);
    };
    auto openAudio = [&](int freq, int allowedChanges, SDL_AudioSpec& have) -> SDL_AudioDeviceID {
        refreshAudioOutputs();
        SDL_AudioSpec want{};
        want.freq = freq; want.format = AUDIO_F32SYS; want.channels = 2;
        want.samples = static_cast<Uint16>(audiosettings::ValidBuffer(app.audioBufferChoice));
        want.callback = AudioCallback; want.userdata = &app;
        const std::string name = audiosettings::OutputToOpen(app.audioOutputChoice, app.audioOutputs);
        SDL_AudioDeviceID dev = 0;
        if (!name.empty()) dev = SDL_OpenAudioDevice(name.c_str(), 0, &want, &have, allowedChanges);
        app.audioOpenedName = dev != 0 ? name : std::string();
        if (dev == 0) dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, allowedChanges);
        app.audioOnFallback = !app.audioOutputChoice.empty() && app.audioOpenedName.empty();
        app.audioOpenedRate = dev != 0 ? have.freq : 0;
        app.audioOpenedFrames = dev != 0 ? have.samples : 0;
        return dev;
    };
    SDL_AudioSpec have{};
    SDL_AudioDeviceID audioDev = openAudio(kSampleRate, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE, have);
    int engineRate = kSampleRate;
    if (audioDev == 0) {
        std::fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        // Keep running headless (UI still works, just silent) rather than
        // bailing out entirely -- handy when testing without a sound card.
    } else {
        if (have.freq >= 8000 && have.freq <= 192000) engineRate = have.freq;
        if (engineRate != kSampleRate) std::fprintf(stderr, "Audio device runs at %d Hz -- engine follows it.\n", engineRate);
        if (app.audioOnFallback) app.SetStatus("AUDIO: CHOSEN OUTPUT NOT AVAILABLE, PLAYING ON THE SYSTEM OUTPUT");
    }

    SyncLibraryKits(SamplesRoot(), "kits");   // v44.51: kit folders from <samples>/Kits/ appear in the kit list (copied, never changed)
    app.engine.Init(engineRate);
    app.recorder.Init(engineRate);
    int loadedSlots = app.engine.GetKit()->LoadFromFolder(app.project.kitFolder, engineRate);
    if (loadedSlots < kNumDrumTracks) {
        std::fprintf(stderr,
            "WARNING: only %d/%d drum samples loaded from \"%s\" -- drums will be silent.\n"
            "  (Current directory: %s)\n  Check that \"%s\" exists and has kick.wav/snare.wav/"
            "clap.wav/chh.wav/ohh.wav/perc.wav, or pass an explicit --kit /full/path/to/kits/808.\n",
            loadedSlots, kNumDrumTracks, app.project.kitFolder.c_str(), CurrentDir().c_str(),
            app.project.kitFolder.c_str());
    }
    int loadedRisers = app.engine.Risers().LoadFromFolder("kits/risers", engineRate);
    if (loadedRisers < RiserPlayer::kMaxRisers) {
        std::fprintf(stderr, "NOTE: only %d/%d riser samples loaded from \"kits/risers\".\n",
                      loadedRisers, RiserPlayer::kMaxRisers);
    }
    app.StartSound();   // patches, samples, kick and kit of the played scene (the sequencer is given the engine first)
    if (firstRunSettings && !(projectExplicit && FileExists(projectPath)) && app.CurrentPatternIsEmpty()) {
        ApplyStarterGroove(app.CurrentPattern(), app.StepTrack(app.CurrentPattern(), kChordTrackIndex));   // v44.8 first launch: something to hear at once
        app.SyncMixer(app.CurrentPattern());
        // v44.9: the seed is already the first brick of the track (4 bars). DATA-7: the brick is a COPY of scene 1 in a brick slot of
        // its own (as every thrown brick is since v44.22); before, it pointed at scene 1 itself, so editing the track changed the jam.
        uint16_t mm = 0; StepParams spr;
        const int slot = app.CopySceneToBrickSlot(0, true, &mm, &spr);
        app.project.songChain[0] = ChainStep{slot >= 0 ? slot : 0, 4};
        app.project.songChain[0].muteMask = mm;
        app.project.songChain[0].params = spr;
        app.project.songChainLength = 1;
    }
    if (resumedUntitled) app.MarkUnsaved(); else app.MarkSaved();   // U8b: what the document is compared with from now on
    app.sequencer.SetSongFollowsEdit(false); // desktop LIVE: the chain moves playback only, the editor stays where the user put it
    app.loader.Start();   // N1: from here on samples, kits and folder scans are read off the scene lock (the first sound above is already loaded)

    // The audio device was opened above (paused); only now start the callback.
    if (audioDev != 0) SDL_PauseAudioDevice(audioDev, 0);

    // v44.3 audio recovery. After the laptop sleeps (or the output device changes: headphones, a display's speakers, Bluetooth)
    // the old device can go dead while the sequencer keeps counting, so the playhead moves and nothing is heard. The watchdog
    // below reopens the device (at the engine's own rate, SDL converts if the new device differs) and clears the effect state.
    auto restartAudio = [&](const char* why) {
        std::fprintf(stderr, "Restarting audio (%s)\n", why);
        if (audioDev != 0) { SDL_CloseAudioDevice(audioDev); audioDev = 0; }
        const bool wasFallback = app.audioOnFallback;
        SDL_AudioSpec h2{};
        audioDev = openAudio(engineRate, 0, h2);   // at the engine's own rate: SDL converts if the new device differs
        app.engine.ResetDsp();
        if (audioDev != 0) {
            SDL_PauseAudioDevice(audioDev, 0);
            if (app.audioOnFallback) app.SetStatus("AUDIO: CHOSEN OUTPUT NOT AVAILABLE, PLAYING ON THE SYSTEM OUTPUT");
            else if (wasFallback && !app.audioOpenedName.empty()) app.SetStatus("AUDIO: BACK ON " + app.audioOpenedName);
            else app.SetStatus("AUDIO RESTARTED");
        }
        else app.SetStatus("AUDIO: NO OUTPUT DEVICE FOUND");
    };
    unsigned lastBlocks = app.engine.GetMixer().audioBlocks.load();
    double stallTimer = 0.0, deviceChangeTimer = -1.0, audioRetryTimer = 0.0;
    unsigned blocksAtDeviceChange = 0;

    HeaderPanel headerPanel;
    MenuBar menuBar;
    ChainPanel chainPanel;
    BricksPanel bricksPanel;
    SequencerPanel seqPanel;
    MixerPanel mixPanel;
    MixLabPanel mixLabPanel;
    ChordLabPanel chordLabPanel;
    LanePanel lanePanel;
    GroovePanel groovePanel;
    PianoRollPanel pianoRollPanel;
    ContextPanel contextPanel;
    SoundPanel soundPanel;
    BottomPanel bottomPanel;
    int lastSelTrack = -1;

    bool running = true;
    Uint64 prevCounter = SDL_GetPerformanceCounter();
    double autosaveTimer = 0.0;
    std::string titlePath;
    bool titleEdited = false;
    double folderRescanTimer = 0.0;

    while (running) {
        Uint64 nowCounter = SDL_GetPerformanceCounter();
        double dt = static_cast<double>(nowCounter - prevCounter) / SDL_GetPerformanceFrequency();
        prevCounter = nowCounter;

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (uiZoom != 1.0f) {   // the UI is laid out in zoomed units: bring mouse positions into them
                if (e.type == SDL_MOUSEMOTION) { e.motion.x = static_cast<Sint32>(e.motion.x / uiZoom); e.motion.y = static_cast<Sint32>(e.motion.y / uiZoom); }
                else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) { e.button.x = static_cast<Sint32>(e.button.x / uiZoom); e.button.y = static_cast<Sint32>(e.button.y / uiZoom); }
            }
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) app.RequestDoc(AppState::DocAction::Quit);   // U8b: asks Save / Don't Save / Cancel when something is unsaved
            // N1 (LIVE-6): a device that appears (AirPods, an HDMI screen, a USB box) no longer restarts the sound. A removed one is
            // checked after a moment, and the sound is restarted only if OUR output stopped (see the watchdog below).
            if (e.type == SDL_AUDIODEVICEREMOVED && e.adevice.iscapture == 0) { deviceChangeTimer = 0.6; blocksAtDeviceChange = app.engine.GetMixer().audioBlocks.load(); app.audioOutputsWanted = true; }
            // N1: the output chosen in Settings > Audio is plugged in again while the sound plays on the system output: go back to it.
            if (e.type == SDL_AUDIODEVICEADDED && e.adevice.iscapture == 0) {
                app.audioOutputsWanted = true;
                const char* nm = SDL_GetAudioDeviceName(static_cast<int>(e.adevice.which), 0);
                if (app.audioOnFallback && nm && app.audioOutputChoice == nm) app.audioRestartRequested = true;
            }

            if (e.type == SDL_DROPFILE && e.drop.file) {   // a WAV dropped on the window -> the selected drum track
                const std::string dropped = e.drop.file;
                SDL_free(e.drop.file);
                std::string ext = dropped.size() > 4 ? dropped.substr(dropped.size() - 4) : std::string();
                for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                std::string ext8 = dropped.size() > 7 ? dropped.substr(dropped.size() - 7) : std::string();
                for (char& c : ext8) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (ext8 == ".t8proj") app.RequestDoc(AppState::DocAction::Open, dropped);   // U8b: a project dropped on the window opens
                else if (IsSampleFileName(dropped)) {   // v44.56: .wav, .wave, .aif, .aiff, .aifc
                    const int dropTrack = std::clamp(app.seqCursorTrack, 0, kNumTracks - 1);
                    const int dropTgt = AppState::SampleTargetOfTrack(dropTrack);
                    const bool synthSample = dropTgt >= 0 && app.EditedPatch(dropTgt >= kNumSynthTracks, dropTgt).engineType == SynthEngineType::Sample;   // v44.53: a synth / chord on the SAMPLE engine takes it
                    if (app.CurrentPattern().tracks[static_cast<size_t>(dropTrack)].type == TrackType::Sampler) app.AssignSampleToSampler(dropped);
                    else if (synthSample) app.AssignSampleToSampler(dropped, dropTgt);
                    else app.AssignSampleToSlot(dropped);
                }
                else app.SetStatus("DROP A .WAV OR .AIF FILE ON A DRUM TRACK OR THE SAMPLER");
            }
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
                e.window.windowID == SDL_GetWindowID(window)) {
                app.RequestDoc(AppState::DocAction::Quit);
            }
            // Escape closes popups (ImGui); quitting is via the window close button, Cmd+Q or File > Quit.
        }

        {   // N1: a recording is closed on its own thread; its "SAVED ..." arrives here
            std::string msg, path;
            if (app.recorder.TakeResult(msg, path) || app.engine.Multitrack().TakeResult(msg, path)) {
                app.SetStatus(msg);
                if (!path.empty()) userdirs::SetLastOutput(path);
            }
        }
        app.loader.Poll();      // N1: files read on the loader thread are put in place here (outside every panel's scene lock)
        app.PollSamplePick();   // v44.50: a file chosen in the native window (takes the scene lock itself, after the file is read)
        if (app.statusMessageTimer > 0.0f) app.statusMessageTimer -= static_cast<float>(dt);
        {
            Mixer& mixer = app.engine.GetMixer();
            float hold = mixer.masterClipHold.load();
            if (hold > 0.0f) mixer.masterClipHold.store(std::max(0.0f, hold - static_cast<float>(dt)));
        }
        if (app.quitRequested) running = false;
        {   // ---- audio watchdog (v44.3) ----
            const char* why = nullptr;
            // N1 (LIVE-6): a long frame is a sleep only if the audio callback did not run through it either (a slow screen
            // frame, a project load, used to restart the device and cut the reverb tails too).
            const unsigned blocksNow = app.engine.GetMixer().audioBlocks.load();
            const int bufFrames = std::max(1, app.engine.GetMixer().audioBufferFrames.load());
            const double expectedBlocks = dt * engineRate / bufFrames;
            if (dt > 2.5 && audioDev != 0 && static_cast<double>(blocksNow - lastBlocks) < 0.25 * expectedBlocks) why = "woke up after a long pause";   // the laptop slept
            else if (app.audioRestartRequested) why = "requested";
            else if (deviceChangeTimer >= 0.0 && (deviceChangeTimer -= dt) < 0.0) {
                deviceChangeTimer = -1.0;
                const bool stillPlaying = audioDev != 0 && SDL_GetAudioDeviceStatus(audioDev) == SDL_AUDIO_PLAYING && app.engine.GetMixer().audioBlocks.load() != blocksAtDeviceChange;
                if (!stillPlaying) why = "output device removed";   // our output is gone: reopen the default one
            }
            else if (audioDev != 0) {
                const unsigned blocks = app.engine.GetMixer().audioBlocks.load();
                if (blocks == lastBlocks) { if ((stallTimer += dt) > 1.0) why = "the audio callback stopped"; } else stallTimer = 0.0;
                lastBlocks = blocks;
                if (!why && SDL_GetAudioDeviceStatus(audioDev) != SDL_AUDIO_PLAYING) SDL_PauseAudioDevice(audioDev, 0);
            } else if ((audioRetryTimer += dt) > 3.0) { audioRetryTimer = 0.0; why = "no device yet, trying again"; }
            if (why) {
                app.audioRestartRequested = false; stallTimer = 0.0; audioRetryTimer = 0.0;
                restartAudio(why);
                lastBlocks = app.engine.GetMixer().audioBlocks.load();
            }
            app.SyncSceneKits();   // v44.5.4: a scene's kit changed (picked, undone, swapped, loaded)
            if (app.engine.ConsumeDspFault()) { app.engine.ResetDsp(); app.SetStatus("AUDIO: EFFECTS RESET AFTER A BAD SIGNAL"); }
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(window, &ww, &wh);
            {   // interface zoom: AUTO only ever shrinks, to keep every block on a small window
                float z = AppState::kUiZoomValues[std::clamp(app.uiZoomIdx, 0, AppState::kNumUiZoom - 1)];
                if (z <= 0.0f) z = std::clamp(std::min(ww / 1440.0f, wh / 880.0f), 0.55f, 1.0f);   // v44.45: the header needs 1440 points (at 1400 the UNDO field was cut)
                uiZoom = z;
                io.DisplaySize = ImVec2(ww / uiZoom, wh / uiZoom);
            }
            io.DisplayFramebufferScale = ImVec2(static_cast<float>(uiScale) * uiZoom, static_cast<float>(uiScale) * uiZoom);
            int tw = std::max(1, ww * uiScale), th = std::max(1, wh * uiScale);
            if (!uiTarget || tw != uiTargetW || th != uiTargetH) {
                if (uiTarget) SDL_DestroyTexture(uiTarget);
                uiTarget = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_TARGET, tw, th);
                if (uiTarget) SDL_SetTextureScaleMode(uiTarget, SDL_ScaleModeLinear);
                else { uiScale = 1; io.DisplayFramebufferScale = ImVec2(uiZoom, uiZoom); }   // fall back to direct drawing
                uiTargetW = tw; uiTargetH = th;
            }
        }
        ImGui::NewFrame();
        HandleShortcuts(app);
        app.TickKeyFollow();
        app.TickTransport();
        app.RefreshChordLive();

        ImGui::PushFont(fonts.uiRegular);

        // ---- the one app screen: a borderless shell filling the client
        // area. LIVE is the whole performance surface. ----------------------
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
        ImGuiWindowFlags shellFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_MenuBar;
        ImGui::Begin("##Shell", nullptr, shellFlags);
        ImGui::PopStyleVar();
        menuBar.Draw(app);   // U8c: File / Edit / View / Library / Settings

        // v44.10: the scene editor (grid | mixer or piano roll, the instrument block, RANDOM / KIT + LIVE) is drawn the same on the TRACK
        // screen (under the bricks) and on the LIVE screen (under the scene pads); `aboveH` is what the screen already used above it.
        // v44.12: STATIC layout. Nothing in here changes size when the author clicks something (no fold-away blocks, no toggles); the
        // only thing that follows the window is the row height of the grid, inside fixed limits. ctxMin = 0 / bottomH = 0 leave
        // the instrument block / the RANDOM-KIT + LIVE block out (TRACK).
        // the instrument block (everything about the selected instrument): under the editor on LIVE, as the SOUND view on TRACK
        auto drawContextBlock = [&](float ctxH) {
            DpadKeys keys;
            {
                bool blocked = io.WantTextInput || io.KeyCtrl || io.KeySuper || app.rollKeysFrame >= ImGui::GetFrameCount() - 1 ||
                    ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
                if (!blocked) {
                    keys.up = ImGui::IsKeyPressed(ImGuiKey_UpArrow, true);
                    keys.down = ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
                    keys.left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true);
                    keys.right = ImGui::IsKeyPressed(ImGuiKey_RightArrow, true);
                    keys.coarse = io.KeyShift;
                }
            }
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
            ImGui::BeginChild("ContextChild", ImVec2(0, ctxH), ImGuiChildFlags_Borders, ImGuiWindowFlags_None);   // scrolls when the window is short
            contextPanel.Draw(app, keys);
            ImGui::EndChild();
            ImGui::PopStyleVar();
        };

        auto drawSceneEditor = [&](float availY, float aboveH, float sp, float minRow, float maxRow, float ctxMin, float bottomH) {
            app.detailOpen = true;   // the instrument block is always there (the fold-away "start here" card is gone)
            const float gaps = (2.0f + (ctxMin > 0.0f ? 1.0f : 0.0f) + (bottomH > 0.0f ? 1.0f : 0.0f)) * sp;
            LiveLayout layout;
            float midMax = availY - aboveH - bottomH - ctxMin - gaps;
            float overhead = layout.padY * 2 + layout.headerH + layout.footerH + 4.0f;
            layout.rowH = std::clamp((midMax - overhead) / kNumTracks, minRow, maxRow);
            float midH = overhead + layout.rowH * kNumTracks;
            float ctxH = ctxMin > 0.0f ? std::max(ctxMin, availY - aboveH - bottomH - midH - gaps) : 0.0f;
            const ImVec2 avail(ImGui::GetContentRegionAvail().x, availY);
            const ImGuiWindowFlags fixedFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
            // 3. sequencer | mixer-or-piano-roll (same height, row-aligned)
            bool showRoll = false;
            {
                SceneLock lock(app.sequencer.PatternMutex(), "LAYOUT");
                int sel = std::clamp(app.seqCursorTrack, 0, kNumTracks - 1);
                if (sel != lastSelTrack) { lastSelTrack = sel; app.deskRollClosed = false; app.deskLanesClosed = false; }
                showRoll = app.CurrentPattern().tracks[sel].type != TrackType::Drum && !app.deskRollClosed;
            }
            float seqW = std::min(660.0f, avail.x * 0.45f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(layout.padX, layout.padY));
            ImGui::BeginChild("SeqChild", ImVec2(seqW, midH), ImGuiChildFlags_Borders, fixedFlags);
            seqPanel.Draw(app, layout);
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("MixerOrRollChild", ImVec2(0, midH), ImGuiChildFlags_Borders, fixedFlags);
            bool showLanes = false;
            {
                SceneLock lock(app.sequencer.PatternMutex(), "LAYOUT");
                showLanes = app.CurrentPattern().tracks[std::clamp(app.seqCursorTrack, 0, kNumTracks - 1)].type == TrackType::Drum && !app.deskLanesClosed;
            }
            if (app.grooveViewOpen) groovePanel.Draw(app, layout);   // v44.3: the groove window replaces the mixer on any track
            else if (showRoll) pianoRollPanel.Draw(app, layout);
            else if (showLanes) lanePanel.Draw(app, layout);
            else mixPanel.Draw(app, layout);
            ImGui::EndChild();
            ImGui::PopStyleVar();

            // 4. context block for the selected instrument (LIVE only)
            if (ctxH > 0.0f) drawContextBlock(ctxH);

            // 5. RANDOM / KIT + LIVE, pinned to the bottom (LIVE only)
            if (bottomH > 0.0f) {
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
                ImGui::BeginChild("BottomChild", ImVec2(0, bottomH), ImGuiChildFlags_Borders, fixedFlags);
                bottomPanel.Draw(app);
                ImGui::EndChild();
                ImGui::PopStyleVar();
            }
        };

        {   // v44.57: the PITCH strip at the right end of the tab row (drawn first, the tab bar then starts where it did)
            const ImVec2 here = ImGui::GetCursorScreenPos();
            const float pitchLeft = headerPanel.DrawPitch(app, ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - 8.0f);
            ImGui::SetCursorScreenPos(here);
            headerPanel.DrawDropouts(app, pitchLeft - 24.0f);   // N1: always visible (the header row has no room at 1440 px)
            ImGui::SetCursorScreenPos(here);
        }
        if (ImGui::BeginTabBar("##MainTabs", ImGuiTabBarFlags_None)) {
            const int tabReq = app.uiTabRequest;   // a request made during this frame (THROW ONTO THE TRACK) waits for the next one
            const ImGuiTabItemFlags trackFlags = tabReq == 1 ? ImGuiTabItemFlags_SetSelected : 0;
            const ImGuiTabItemFlags liveFlags = tabReq == 2 ? ImGuiTabItemFlags_SetSelected : 0;
            const float headerH = 42.0f, liveChainH = 104.0f, liveCtxH = 214.0f, liveBottomH = 132.0f, trackInspH = BricksPanel::InspectorHeight();
            const ImGuiWindowFlags fixedFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
            // LIVE (the program opens here): jam. Scene pads + chord bank, the editor, the instrument block, RANDOM / KIT + LIVE.
            // "where am I": LIVE is amber, TRACK is teal (tab, line under it and the label of the editor)
            auto tabHue = [](const ImVec4& bg, const ImVec4& line) {
                ImGui::PushStyleColor(ImGuiCol_TabSelected, bg); ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, bg); ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, line);
            };
            // v44.51: the CHORD LAB child, shared by LIVE and TRACK (`rest` = the height it gets)
            auto drawChordLabChild = [&](float rest) {
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
                    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.30f, 0.72f, 0.65f, 1.0f));
                    ImGui::BeginChild("ChordLabChild", ImVec2(0, std::max(250.0f, rest)), ImGuiChildFlags_Borders, fixedFlags);
                    ImGui::PopStyleColor();
                    chordLabPanel.SetSoundBody([&](AppState& a) {   // the SOUND view: the instrument block of the chord track, drawn as everywhere else
                        DpadKeys keys;
                        const bool blocked = io.WantTextInput || io.KeyCtrl || io.KeySuper || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
                        if (!blocked) {
                            keys.up = ImGui::IsKeyPressed(ImGuiKey_UpArrow, true); keys.down = ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
                            keys.left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true); keys.right = ImGui::IsKeyPressed(ImGuiKey_RightArrow, true);
                            keys.coarse = io.KeyShift;
                        }
                        contextPanel.ShowChordSound();
                        contextPanel.Draw(a, keys);
                    });
                    chordLabPanel.Draw(app, ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
                    ImGui::EndChild();
                    ImGui::PopStyleVar();
            };
            // C1 (UI-1): MIX LAB, CHORD LAB and MASTER FX open the same way: in the room under the header of LIVE or TRACK, with one
            // strip on top (name, what it does, key, x). The tab under them stays selected; Esc, the same key or x closes.
            auto toolColor = [](AppState::Tool t) {
                return t == AppState::Tool::MixLab ? ImVec4(0.70f, 0.55f, 0.95f, 1.0f) : t == AppState::Tool::ChordLab ? ImVec4(0.30f, 0.72f, 0.65f, 1.0f) : ImVec4(0.95f, 0.64f, 0.24f, 1.0f);
            };
            auto drawTool = [&](float rest) {
                const AppState::Tool t = app.tool;
                const char* name = t == AppState::Tool::MixLab ? "MIX LAB" : t == AppState::Tool::ChordLab ? "CHORD LAB" : "MASTER FX";
                const char* what = t == AppState::Tool::MixLab ? "where instruments fight for the same frequencies"
                                 : t == AppState::Tool::ChordLab ? "the chords of the key: hear them, try them, build a loop"
                                 : "project-wide delay, reverb, distortion, sidechain and the master chain";
                const char* key = t == AppState::Tool::MixLab ? "M" : t == AppState::Tool::ChordLab ? "C" : "F";
                const float stripH = 30.0f;
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 4));
                ImGui::PushStyleColor(ImGuiCol_Border, toolColor(t));
                ImGui::BeginChild("ToolStripChild", ImVec2(0, stripH), ImGuiChildFlags_Borders, fixedFlags);
                ImGui::PopStyleColor();
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(toolColor(t), "%s", name);
                ImGui::SameLine(0, 12);
                ImGui::TextDisabled("%s", what);
                const float xw = 26.0f, right = ImGui::GetWindowContentRegionMax().x;
                ImGui::SameLine(right - xw - 120.0f);
                ImGui::TextDisabled("%s or Esc closes", key);
                ImGui::SameLine(right - xw);
                if (ImGui::Button("x##toolClose", ImVec2(xw, 0))) app.OpenTool(AppState::Tool::None);
                if (ImGui::IsItemHovered()) Hint(t == AppState::Tool::ChordLab ? "Close CHORD LAB (key C or Esc). A draft stays in its chord slot." : "Close (the same key or Esc).");
                ImGui::EndChild();
                ImGui::PopStyleVar();
                const float bodyH = std::max(200.0f, rest - stripH - ImGui::GetStyle().ItemSpacing.y);
                if (t == AppState::Tool::MixLab) {
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
                    ImGui::BeginChild("MixLabChild", ImVec2(0, bodyH), ImGuiChildFlags_None, fixedFlags);
                    mixLabPanel.Draw(app, ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
                    ImGui::EndChild();
                    ImGui::PopStyleVar();
                } else if (t == AppState::Tool::ChordLab) {
                    drawChordLabChild(bodyH);
                } else {
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));
                    ImGui::BeginChild("MasterFxChild", ImVec2(0, bodyH), ImGuiChildFlags_Borders, ImGuiWindowFlags_None);   // scrolls on a short window
                    headerPanel.DrawMasterFx(app);
                    ImGui::EndChild();
                    ImGui::PopStyleVar();
                }
            };
            // v44.51: a pulsing dot on the tab whose music is playing (LIVE = the jam, TRACK = the bricks). The tab bar also says it in words.
            auto tabPlayDot = [&](int tab) {
                const AppState::PlaySource ps = app.PlayingSource();
                if (ps == AppState::PlaySource::None || (tab == 2) != (ps == AppState::PlaySource::Live)) return;
                const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                const float pulse = 0.65f + 0.35f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.0f);
                const ImU32 col = tab == 2 ? IM_COL32(246, 163, 61, static_cast<int>(255 * pulse)) : IM_COL32(94, 224, 201, static_cast<int>(255 * pulse));
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(b.x - 12.0f, (a.y + b.y) * 0.5f), 3.6f, col);
            };
            tabHue(ImVec4(0.23f, 0.17f, 0.08f, 1.0f), ImVec4(0.95f, 0.64f, 0.24f, 1.0f));
            const bool liveOpen = ImGui::BeginTabItem("LIVE   ###tabLive", nullptr, liveFlags);
            tabPlayDot(2);
            ImGui::PopStyleColor(3);
            if (liveOpen) {
                ShowTab(app, 2);
                ImGui::PushFont(nullptr, 14.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 4));
                const float sp = ImGui::GetStyle().ItemSpacing.y;
                const float availY = ImGui::GetContentRegionAvail().y - kInfoLineHeight - 4.0f;   // the info line (U2) sits under everything
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 4));
                ImGui::BeginChild("HeaderChild", ImVec2(0, headerH), ImGuiChildFlags_Borders, fixedFlags);
                headerPanel.Draw(app);
                ImGui::EndChild();
                ImGui::PopStyleVar();
                if (app.tool != AppState::Tool::None) {
                    drawTool(availY - headerH - sp);   // C1: a tool takes the room under the header
                } else {
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
                    ImGui::BeginChild("ChainChild", ImVec2(0, liveChainH), ImGuiChildFlags_Borders, fixedFlags);
                    chainPanel.Draw(app);
                    ImGui::EndChild();
                    ImGui::PopStyleVar();
                    drawSceneEditor(availY, headerH + liveChainH, sp, 18.0f, 34.0f, liveCtxH, liveBottomH);
                }
                ImGui::PopStyleVar(2);
                ImGui::PopFont();
                ImGui::EndTabItem();
            }
            // TRACK: build the finished track from the bricks: header, the strip, the inspector, the scene editor. No RANDOM / KIT, no LIVE block.
            tabHue(ImVec4(0.07f, 0.20f, 0.19f, 1.0f), ImVec4(0.37f, 0.88f, 0.79f, 1.0f));
            const bool trackOpen = ImGui::BeginTabItem("TRACK   ###tabTrack", nullptr, trackFlags);
            tabPlayDot(1);
            ImGui::PopStyleColor(3);
            if (trackOpen) {
                ShowTab(app, 1);
                ImGui::PushFont(nullptr, 14.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 4));
                const float sp = ImGui::GetStyle().ItemSpacing.y;
                const float availY = ImGui::GetContentRegionAvail().y - kInfoLineHeight - 4.0f;   // the info line (U2) sits under everything
                // the strip has a fixed height (the editor under it wants about 60 + 10 rows of 26 px); it only gives way on a short window
                const float modeRowH = 34.0f;   // v44.24: NOTES | SOUND | MIX
                // v44.45: the room the editor needs is worked out from its own layout (10 rows of 20 + header + footer + padding); before, a
                // fixed 236 was too little and the bottom of the sequencer ran into the info line
                const bool emptyTrack = app.project.songChainLength == 0;
                const float editorNeed = LiveLayout{}.padY * 2 + LiveLayout{}.headerH + LiveLayout{}.footerH + 4.0f + kNumTracks * 18.0f + 2 * sp;   // v44.50: 11 rows of 18 (the sampler took 20 px from the strip above, which made its rows unreadable)
                const float stripH = emptyTrack ? std::clamp(availY - headerH - 190.0f - 2 * sp, 250.0f, 420.0f) : std::clamp(availY - headerH - trackInspH - modeRowH - 5 * sp - editorNeed, 170.0f, 420.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 4));
                ImGui::BeginChild("HeaderChildT", ImVec2(0, headerH), ImGuiChildFlags_Borders, fixedFlags);
                headerPanel.Draw(app);
                ImGui::EndChild();
                ImGui::PopStyleVar();
                if (app.tool != AppState::Tool::None) {   // C1: a tool takes the room under the header, over the brick that is open
                    drawTool(availY - headerH - sp);
                    chainPanel.DrawEditors(app);
                    bottomPanel.DrawBrowsers(app);
                    ImGui::PopStyleVar(2);
                    ImGui::PopFont();
                    ImGui::EndTabItem();
                } else {
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
                ImGui::BeginChild("BricksChild", ImVec2(0, stripH), ImGuiChildFlags_Borders, fixedFlags);
                bricksPanel.Draw(app, chainPanel, true);
                ImGui::EndChild();
                ImGui::PopStyleVar();
                if (emptyTrack) {   // v44.45: no bricks, nothing to edit here: the LIVE scene is edited on LIVE only
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));
                    ImGui::BeginChild("EmptyTrackChild", ImVec2(0, std::max(120.0f, availY - headerH - stripH - 2 * sp)), ImGuiChildFlags_Borders, fixedFlags);
                    ImGui::TextColored(ImVec4(0.37f, 0.88f, 0.79f, 1.0f), "THE TRACK IS EMPTY");
                    ImGui::Dummy(ImVec2(1, 4));
                    ImGui::TextDisabled("A track is made of bricks. Make a loop on LIVE and throw it onto the track, or add a scene with + on the strip.");
                    ImGui::Dummy(ImVec2(1, 10));
                    if (ImGui::Button("GO TO LIVE", ImVec2(130, 30))) app.uiTabRequest = 2;
                    if (ImGui::IsItemHovered()) Hint("Back to the jam. Scenes are edited on LIVE.");
                    ImGui::SameLine(0, 8);
                    if (ImGui::Button("THROW THE LIVE SCENE ONTO THE TRACK", ImVec2(300, 30))) {
                        SceneLock lock(app.sequencer.PatternMutex(), "LAYOUT");
                        app.ThrowIntoChain();
                    }
                    if (ImGui::IsItemHovered()) Hint("Makes the scene you are playing the first brick of the track.");
                    ImGui::EndChild();
                    ImGui::PopStyleVar();
                    chainPanel.DrawEditors(app);
                    bottomPanel.DrawBrowsers(app);
                    ImGui::PopStyleVar(2);
                    ImGui::PopFont();
                    ImGui::EndTabItem();
                } else {
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 5));
                ImGui::BeginChild("InspectorChild", ImVec2(0, trackInspH), ImGuiChildFlags_Borders, fixedFlags);
                bricksPanel.DrawInspector(app, chainPanel);
                ImGui::EndChild();
                ImGui::PopStyleVar();
                chainPanel.DrawEditors(app);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 4));
                ImGui::BeginChild("ModeRowChild", ImVec2(0, modeRowH), ImGuiChildFlags_Borders, fixedFlags);
                soundPanel.DrawModeRow(app);
                ImGui::EndChild();
                ImGui::PopStyleVar();
                const float aboveEditor = headerH + stripH + trackInspH + modeRowH + 2 * sp;
                const int mode = std::clamp(app.trackEditMode, 0, 2);
                if (mode == 0) {
                    drawSceneEditor(availY, aboveEditor, sp, 20.0f, 30.0f, 0.0f, 0.0f);
                } else {
                    const float rowH = 34.0f;
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 4));
                    ImGui::BeginChild("ScopeRowChild", ImVec2(0, rowH), ImGuiChildFlags_Borders, fixedFlags);
                    if (mode == 1) soundPanel.DrawScopeRow(app); else soundPanel.DrawMixNote(app);
                    ImGui::EndChild();
                    ImGui::PopStyleVar();
                    const float restH = std::max(120.0f, availY - aboveEditor - rowH - sp);
                    if (mode == 1) {
                        drawContextBlock(restH);
                    } else {
                        LiveLayout layout;
                        const float overhead = layout.padY * 2 + layout.headerH + layout.footerH + 4.0f;
                        layout.rowH = std::clamp((restH - overhead) / kNumTracks, 18.0f, 34.0f);
                        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(layout.padX, layout.padY));
                        ImGui::BeginChild("MixViewChild", ImVec2(0, restH), ImGuiChildFlags_Borders, fixedFlags);
                        mixPanel.Draw(app, layout);
                        ImGui::EndChild();
                        ImGui::PopStyleVar();
                    }
                }
                bottomPanel.DrawBrowsers(app);   // v44.26: the kit browser opened from the KIT picker of a drum on TRACK (it used to exist on LIVE only)
                ImGui::PopStyleVar(2);
                ImGui::PopFont();
                ImGui::EndTabItem();
                }
                }
            }
            // C1 (UI-1): the three tools, one look, the key next to the name; lit while open (one at a time)
            if (app.tool != AppState::Tool::MixLab) mixLabPanel.Leave(app);
            if (app.tool != AppState::Tool::ChordLab) chordLabPanel.Leave(app);
            auto toolButton = [&](const char* label, const char* key, AppState::Tool t, bool dot, const char* hint) {
                const bool on = app.tool == t;
                const ImVec4 c = toolColor(t);
                ImGui::PushStyleColor(ImGuiCol_Tab, on ? ImVec4(c.x * 0.30f, c.y * 0.30f, c.z * 0.30f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_Tab));
                ImGui::PushStyleColor(ImGuiCol_Text, on ? c : ImGui::GetStyleColorVec4(ImGuiCol_Text));
                char lb[48];
                std::snprintf(lb, sizeof lb, "%s         ###tool%s", label, key);   // the spaces leave room for the key box
                const bool hit = ImGui::TabItemButton(lb, ImGuiTabItemFlags_NoTooltip);
                ImGui::PopStyleColor(2);
                const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 ks = ImGui::CalcTextSize(key);
                const float kx = a.x + ImGui::GetStyle().FramePadding.x + ImGui::CalcTextSize(label).x + (dot ? 26.0f : 12.0f), ky = (a.y + b.y) * 0.5f;
                dl->AddRect(ImVec2(kx - 4.0f, ky - ks.y * 0.5f - 1.0f), ImVec2(kx + ks.x + 4.0f, ky + ks.y * 0.5f + 1.0f), vis::kLineHi, 3.0f);
                dl->AddText(ImVec2(kx, ky - ks.y * 0.5f), vis::kTextDim, key);
                if (dot) dl->AddCircleFilled(ImVec2(kx - 13.0f, ky), 3.5f, IM_COL32(94, 224, 201, 255));
                if (on) dl->AddLine(ImVec2(a.x + 2.0f, a.y + 1.0f), ImVec2(b.x - 2.0f, a.y + 1.0f), ImGui::GetColorU32(c), 2.0f);
                if (hit) app.ToggleTool(t);
                if (ImGui::IsItemHovered()) Hint("%s  Key %s; the same key or Esc closes.", hint, key);
            };
            toolButton("MIX LAB", "M", AppState::Tool::MixLab, false, "MIX LAB: where the instruments of the played scene fight for the same frequencies, and what to turn to make room.");
            toolButton("CHORD LAB", "C", AppState::Tool::ChordLab, false, "CHORD LAB: jam with the chords of the key over the running scene (LIVE) or the open brick (TRACK).");
            toolButton("MASTER FX", "F", AppState::Tool::MasterFx, app.project.fx.master.chainOn, "MASTER FX: project-wide delay / reverb / distortion / sidechain and the master chain (the dot: the chain is on).");
            {   // v44.51: what plays, in words, at the right end of the tab bar; when the tab on screen is not the one that plays, a click moves the music here
                const AppState::PlaySource ps = app.PlayingSource();
                if (ps != AppState::PlaySource::None) {
                    char lb[96];
                    const bool live = ps == AppState::PlaySource::Live;
                    if (live) std::snprintf(lb, sizeof(lb), "PLAYING: LIVE %02d", app.sequencer.PlayPatternIndex() + 1);
                    else std::snprintf(lb, sizeof(lb), "PLAYING: TRACK %d/%d", app.sequencer.SongChainPos() + 1, std::max(1, app.project.songChainLength));
                    const bool elsewhere = app.PlayingElsewhere();
                    if (elsewhere) std::snprintf(lb + std::strlen(lb), sizeof(lb) - std::strlen(lb), "  -  CLICK: PLAY %s", live ? "TRACK" : "LIVE");
                    ImGui::PushStyleColor(ImGuiCol_Text, live ? ImVec4(0.96f, 0.64f, 0.24f, 1.0f) : ImVec4(0.37f, 0.88f, 0.79f, 1.0f));
                    if (ImGui::TabItemButton((std::string(lb) + "###tabNow").c_str(), ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip) && elsewhere) {
                        // what the shown tab holds starts; the beat does not jump
                        app.PlayHere();
                    }
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) Hint(elsewhere ? "The music you hear is %s. The tab on screen only shows and edits. Click: play what this tab holds instead."
                                                               : "The music you hear is %s.", live ? "the LIVE jam (a scene)" : "the TRACK (bricks)");
                }
            }
            ImGui::EndTabBar();
            if (app.uiTabRequest == tabReq) app.uiTabRequest = 0;
        }

        DrawDocumentDialogs(app);   // U8b: Save changes? / name / Open / recovery
        {   // the info line: what is under the mouse, and how to use it (U2). Drawn into the bottom strip of the shell.
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
            const ImVec2 a(wp.x + 6.0f, wp.y + ws.y - 4.0f - kInfoLineHeight), b(wp.x + ws.x - 6.0f, wp.y + ws.y - 4.0f);
            ImGui::PushFont(nullptr, 14.0f);
            DrawInfoLine(dl, a, b, ImGui::GetTime());
            ImGui::PopFont();
        }
        ImGui::End(); // ##Shell
        DrawHelpScreen(app);

        ImGui::PopFont();

        // U2: one cursor rule for the whole program -- the hand on everything that can be pressed, unless the control chose its
        // own cursor (resize arrows on a number, the text cursor in a field). Menus and lists keep the arrow, like macOS menus.
        {
            ImGuiContext* g = ImGui::GetCurrentContext();
            if (ImGui::GetMouseCursor() == ImGuiMouseCursor_Arrow && g->HoveredId != 0 && !g->HoveredIdIsDisabled && !io.WantTextInput &&
                !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }

        ImGui::Render();
        if (uiTarget) {
            SDL_SetRenderTarget(sdlRenderer, uiTarget);
            SDL_SetRenderDrawColor(sdlRenderer, 21, 22, 28, 255);
            SDL_RenderClear(sdlRenderer);
            // The SDLRenderer2 backend only scales clip rects by FramebufferScale;
            // vertex positions are scaled by SDL itself, so the renderer scale
            // MUST be set (this was missing = the "half size in a corner" bug).
            SDL_RenderSetScale(sdlRenderer, static_cast<float>(uiScale) * uiZoom, static_cast<float>(uiScale) * uiZoom);
            ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), sdlRenderer);
            SDL_RenderSetScale(sdlRenderer, 1.0f, 1.0f);
            SDL_SetRenderTarget(sdlRenderer, nullptr);
            SDL_SetRenderDrawColor(sdlRenderer, 21, 22, 28, 255);
            SDL_RenderClear(sdlRenderer);
            SDL_RenderCopy(sdlRenderer, uiTarget, nullptr, nullptr);
        } else {
            // Offscreen target unavailable: draw straight (old behaviour).
            SDL_SetRenderDrawColor(sdlRenderer, 21, 22, 28, 255);
            SDL_RenderClear(sdlRenderer);
            ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), sdlRenderer);
        }
        SDL_RenderPresent(sdlRenderer);

        if (app.projectPath != titlePath || app.edited != titleEdited) {   // the window is named after the open project (+ "Edited"); recordings take its name too
            titlePath = app.projectPath;
            titleEdited = app.edited;
            const std::string name = app.IsUntitled() ? std::string("Untitled") : userdirs::ProjectDisplayName(titlePath);
            userdirs::SetProjectName(name);
            const std::string title = name + (titleEdited ? " \xE2\x80\x94 Edited" : "") + " \xE2\x80\x94 TR-Brick";
            SDL_SetWindowTitle(window, title.c_str());
        }
        if (app.audioOutputsWanted) { app.audioOutputsWanted = false; refreshAudioOutputs(); }
        if (app.uiZoomIdx != savedZoomIdx || app.detailOpen != savedDetail || app.quantizeSwitches != savedNextBar || app.clickOpens != savedClickOpens || app.projectPath != savedProject
            || app.audioBufferChoice != savedAudioBuf || app.audioOutputChoice != savedAudioOut) {
            savedAudioBuf = app.audioBufferChoice;
            savedAudioOut = app.audioOutputChoice;
            savedNextBar = app.quantizeSwitches;
            savedClickOpens = app.clickOpens;
            savedZoomIdx = app.uiZoomIdx;
            savedDetail = app.detailOpen;
            savedProject = app.projectPath;
            writeUiSettings();
        }
        folderRescanTimer += dt;
        if (folderRescanTimer > 3.0) { folderRescanTimer = 0.0; app.RescanFoldersInBackground(); }   // N1 (LIVE-10): read off the scene lock
        app.TickDocument(dt);   // U8b: "Edited" = the open project differs from the saved one
        autosaveTimer += dt;
        if (autosaveTimer > 10.0) {
            autosaveTimer = 0.0;
            if (app.edited && app.docRecoverPath.empty()) app.WriteRecovery();   // never over the project file: Cmd+S does that
        }
    }
    // (no save at exit: the window close / Cmd+Q asked "Save / Don't Save / Cancel" when something was unsaved)

    if (audioDev != 0) SDL_CloseAudioDevice(audioDev);
    app.recorder.Shutdown();               // N1: a take still running at quit is closed properly (the file is already on disk)
    app.engine.Multitrack().Shutdown();
    app.loader.Stop();
    app.FlushSampleTags();                 // a sample-tags write still waiting in the loader was dropped by Stop(): write it now
    app.FlushSampleSets();                 // the same for the performance sets of the SAMPLES browser
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    if (uiTarget) SDL_DestroyTexture(uiTarget);
    SDL_DestroyRenderer(sdlRenderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
