cmake_minimum_required(VERSION 3.16)
project(trbrick_daw CXX)

# TR-Brick DAW -- macOS desktop app (Dear ImGui + SDL2) on top of the t8core
# engine. The handheld / muOS build was retired in v31; see CHANGELOG.md.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---- warnings and sanitizers (first-party code only, never third_party/) ----
# Warnings are on for everyone. CI also turns on T8_WERROR, which makes the warning
# classes that once hid a broken test (a loop swallowed by a comment) fatal in tools/.
# T8_SANITIZE=address,undefined or T8_SANITIZE=thread builds the checking variants
# that CI runs t8_selftest / t8_fuzz under (see docs/AUDIT_v44.43.md, section 5).
option(T8_WERROR "Fail the build on warnings that hide bugs in tools/ (used by CI)" OFF)
set(T8_SANITIZE "" CACHE STRING "Sanitizers to build with, e.g. address,undefined or thread")
set(T8_WARN_FLAGS -Wall -Wextra)
set(T8_TOOL_WERROR_FLAGS -Werror=unused-variable -Werror=misleading-indentation -Werror=unused-value)
if(T8_SANITIZE)
    add_compile_options(-fsanitize=${T8_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${T8_SANITIZE})
    add_compile_definitions(T8_SANITIZED=1)   # lets the selftest skip its CPU-time checks
endif()
# T8_WARNINGS_AS_ERRORS (CI only): any compiler warning in our own code (not in third-party libraries such as ImGui)
# fails the build, so new warnings cannot pile up. gcc's -Wstringop-overflow stays a warning: it reports a false
# positive in ArrangeTypes.h (Lane::Normalize, where n >= 2 is guaranteed). Local builds are not affected.
option(T8_WARNINGS_AS_ERRORS "Treat every warning in first-party code as an error (used by CI)" OFF)
function(t8_first_party target)
    target_compile_options(${target} PRIVATE ${T8_WARN_FLAGS})
    if(T8_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE -Werror)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${target} PRIVATE -Wno-error=stringop-overflow)
        endif()
    endif()
endfunction()
function(t8_tool target)
    t8_first_party(${target})
    if(T8_WERROR)
        target_compile_options(${target} PRIVATE ${T8_TOOL_WERROR_FLAGS})
    endif()
endfunction()

# ---- SDL2 ------------------------------------------------------------
# Point CMake at a prebuilt install via SDL2_ROOT, or rely on a system SDL2
# (brew install sdl2 on a Mac).
if(DEFINED SDL2_ROOT)
    set(CMAKE_PREFIX_PATH "${SDL2_ROOT}" ${CMAKE_PREFIX_PATH})
endif()
find_package(SDL2 QUIET CONFIG)
if(NOT SDL2_FOUND)
    find_package(SDL2 REQUIRED)
endif()

# ---- core engine (no UI dependency) -----------------------------------
add_library(t8core STATIC
    src/core/Types.cpp
    src/core/WavFile.cpp
    src/core/DrumEngine.cpp
    src/core/SubtractiveSynth.cpp
    src/core/FMSynth.cpp
    src/core/Wavetables.cpp
    src/core/WavetableSynth.cpp
    src/core/Chords.cpp
    src/core/ChordProgressions.cpp
    src/core/Effects.cpp
    src/core/Scales.cpp
    src/core/DrumPatterns.cpp
    src/core/Styles.cpp
    src/core/DrumBreaks.cpp
    src/core/SynthPresets.cpp
    src/core/AudioEngine.cpp
    src/core/Sequencer.cpp
    src/core/Persistence.cpp
    src/core/UserDirs.cpp
    src/core/Recorder.cpp
    src/core/WavStream.cpp
    src/core/MultitrackRecorder.cpp
    src/core/ZipWriter.cpp
    src/core/Risers.cpp
    src/core/NotesExport.cpp
    src/core/MixAnalysis.cpp
    src/core/SampleLibrary.cpp
)
target_include_directories(t8core PUBLIC src/core)
t8_first_party(t8core)
if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(t8core PUBLIC Threads::Threads)
endif()

# ---- offline self-test (no audio device needed) -----------------------
add_executable(t8_selftest tools/selftest_main.cpp src/app/SynthParams.cpp)
target_link_libraries(t8_selftest PRIVATE t8core)
target_include_directories(t8_selftest PRIVATE src third_party/imgui)   # ValueField.h rules + widget ratchet
t8_tool(t8_selftest)

# Random sequences of UI operations with audio in between; run it under the sanitizers (see docs/AUDIT_v44.43.md).
add_executable(t8_fuzz tools/fuzz_main.cpp src/app/SynthParams.cpp)
target_link_libraries(t8_fuzz PRIVATE t8core)
target_include_directories(t8_fuzz PRIVATE src third_party/imgui)
t8_tool(t8_fuzz)

# ---- offline render + metrics (t8_render) and engine audit (t8_audit) ------
add_executable(t8_render tools/render_main.cpp)
target_link_libraries(t8_render PRIVATE t8core)
target_include_directories(t8_render PRIVATE src tools)
t8_tool(t8_render)
add_executable(t8_kitgen tools/kitgen_main.cpp)
target_link_libraries(t8_kitgen PRIVATE t8core)
t8_tool(t8_kitgen)

add_executable(t8_presetaudit tools/preset_audit_main.cpp)
target_link_libraries(t8_presetaudit PRIVATE t8core)
target_include_directories(t8_presetaudit PRIVATE src tools)
t8_tool(t8_presetaudit)

add_executable(t8_audit tools/audit_main.cpp)
target_link_libraries(t8_audit PRIVATE t8core)
target_include_directories(t8_audit PRIVATE src tools)
t8_tool(t8_audit)

# ---- the app ----------------------------------------------------------
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
add_library(t8imgui STATIC
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp"
)
target_include_directories(t8imgui PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
# every panel of the screen: the program and the UI test (TEST-5) build the same files
set(T8_UI_SOURCES
    src/ui_desktop/DesktopTheme.cpp
    src/ui_desktop/HeaderPanel.cpp
    src/ui_desktop/MenuBar.cpp
    src/ui_desktop/DocumentDialogs.cpp
    src/ui_desktop/ChainPanel.cpp
    src/ui_desktop/BricksPanel.cpp
    src/ui_desktop/SequencerPanel.cpp
    src/ui_desktop/MixerPanel.cpp
    src/ui_desktop/LanePanel.cpp
    src/ui_desktop/GroovePanel.cpp
    src/ui_desktop/ContextPanel.cpp
    src/ui_desktop/SoundPanel.cpp
    src/ui_desktop/PresetBrowser.cpp
    src/ui_desktop/ListBrowser.cpp
    src/ui_desktop/SampleBrowser.cpp
    src/ui_desktop/Shortcuts.cpp
    src/ui_desktop/BottomPanel.cpp
    src/ui_desktop/FxPanel.cpp
    src/ui_desktop/PianoRollPanel.cpp
    src/ui_desktop/MixLabPanel.cpp
    src/ui_desktop/ChordLabPanel.cpp
    src/ui_desktop/Shell.cpp
)
add_executable(trbrick_daw
    src/main.cpp
    src/app/SynthParams.cpp
    ${T8_UI_SOURCES}
)
target_include_directories(trbrick_daw PRIVATE src)
target_link_libraries(trbrick_daw PRIVATE t8imgui)
t8_first_party(trbrick_daw)
# imgui's SDL backends need the SDL2 headers; the app links SDL itself below
if(TARGET SDL2::SDL2-static)
    target_link_libraries(t8imgui PUBLIC SDL2::SDL2-static)
elseif(TARGET SDL2::SDL2)
    target_link_libraries(t8imgui PUBLIC SDL2::SDL2)
else()
    target_include_directories(t8imgui PUBLIC ${SDL2_INCLUDE_DIRS})
endif()
if(TARGET SDL2::SDL2main)
    target_link_libraries(trbrick_daw PRIVATE SDL2::SDL2main)
endif()
if(TARGET SDL2::SDL2-static)
    target_link_libraries(trbrick_daw PRIVATE t8core SDL2::SDL2-static)
elseif(TARGET SDL2::SDL2)
    target_link_libraries(trbrick_daw PRIVATE t8core SDL2::SDL2)
else()
    target_include_directories(trbrick_daw PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(trbrick_daw PRIVATE t8core ${SDL2_LIBRARIES})
endif()

# ---- t8_uitest (audit TEST-5): the real screen (DrawShellFrame) drawn with no window, toured and clicked at random ----
# Its own copy of ImGui with every IM_ASSERT on, also in Release (tools/uitest_imconfig.h); the panels are compiled with it.
add_library(t8imgui_checked STATIC
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
)
target_include_directories(t8imgui_checked PUBLIC "${IMGUI_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/tools")
target_compile_definitions(t8imgui_checked PUBLIC IMGUI_USER_CONFIG="uitest_imconfig.h")
add_executable(t8_uitest tools/uitest_main.cpp src/app/SynthParams.cpp ${T8_UI_SOURCES})
target_include_directories(t8_uitest PRIVATE src)
target_link_libraries(t8_uitest PRIVATE t8imgui_checked)
t8_tool(t8_uitest)
if(TARGET SDL2::SDL2-static)
    target_link_libraries(t8_uitest PRIVATE t8core SDL2::SDL2-static)
elseif(TARGET SDL2::SDL2)
    target_link_libraries(t8_uitest PRIVATE t8core SDL2::SDL2)
else()
    target_include_directories(t8_uitest PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(t8_uitest PRIVATE t8core ${SDL2_LIBRARIES})
endif()
