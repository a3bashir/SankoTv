cmake_minimum_required(VERSION 3.21)

project(SankoTV LANGUAGES CXX)

# --- C++ standard ---------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Qt automatic tooling -------------------------------------------------
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# --- Locate a locally installed (official installer) Qt6 ------------------
# Prefer an explicit -DCMAKE_PREFIX_PATH=... on the command line. If none is
# given, fall back to the default official-installer location so the project
# configures out of the box. This intentionally does NOT use vcpkg for Qt.
if(NOT CMAKE_PREFIX_PATH AND WIN32)
    set(CMAKE_PREFIX_PATH "C:/Qt/6.11.1/msvc2022_64" CACHE PATH
        "Path to the local Qt6 installation")
endif()

# --- Find Qt6 -------------------------------------------------------------
# Multimedia is needed for the animatic's scratch-audio track, and the MP4
# export uses it to DECODE that track (QAudioDecoder). The MP4 itself is
# made by Windows (Media Foundation, see src/export/MfMovieEncoder): no
# encoder, codec or FFmpeg binary is added for the export.
# LICENSING: HANDOFF "BEFORE SELLING" and docs/licensing/.
# Svg is required for QIcon to render .svg icon resources (without it QIcon
# silently returns a null icon for SVG files).
# NOT here any more: Network and MultimediaWidgets. Both were used by the
# Generation page alone (its fal.ai / Anthropic requests and its take
# player) and left with it. The comment that stood here said Network was
# for the Script Editor's Claude call - the Script Editor parses on-device
# and has made no network call for a long time. The application now
# contains no network code. (Qt6Network.dll still ships: Qt Multimedia
# depends on it. Nothing of ours links or calls it.)
find_package(Qt6 REQUIRED COMPONENTS
    Core Concurrent Gui GuiPrivate Widgets Multimedia ShaderTools Svg)

# (Qt Advanced Docking System was removed in the native-docking migration:
# the reusable src/docking/ classes on stock QDockWidget replace it, so no
# third-party docking library is fetched, linked, or deployed.)

# --- QuickShapeKit (hold-to-shape recognition for the drawing canvas) ------
# Static recognition/session library; owns no canvas, brush engine, or undo
# history. The canvas feeds it document-space samples and replays commits
# through the existing brush engine.
add_subdirectory(third_party/QuickShapeKit)
add_subdirectory(third_party/SankoPaintEngine)

# --- Executable -----------------------------------------------------------
qt_standard_project_setup()

qt_add_executable(SankoTV
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/DashboardPage.cpp
    src/NewProjectDialog.cpp
    src/ProjectIO.cpp
    src/ProjectIO.h
    # Media the project owns (the audio track) is copied into its
    # "<name>_assets" folder: ProjectMedia does the copy, MediaCopyProgress
    # is its progress window.
    src/ProjectMedia.cpp
    src/ProjectMedia.h
    src/MediaCopyProgress.cpp
    src/MediaCopyProgress.h
    src/NewProjectDialog.h
    src/RecentProjects.cpp
    src/RecentProjects.h
    src/RecentProjectsView.cpp
    src/RecentProjectsView.h
    src/RecentThumbnails.cpp
    src/RecentThumbnails.h
    src/ProjectSettingsDialog.cpp
    src/ProjectSettingsDialog.h
    src/ResizeProjectDialog.cpp
    src/ResizeProjectDialog.h
    src/ProjectResize.cpp
    src/ProjectResize.h
    src/FramelessDialogDrag.h
    src/DashboardPage.h
    src/ScriptEditorPage.cpp
    src/ScriptEditorPage.h
    src/CodeEditor.cpp
    src/CodeEditor.h
    src/StoryboardPage.cpp
    src/StoryboardPage.h
    src/StoryboardModel.h
    src/DrawingCanvas.cpp
    src/DrawingCanvas.h
    src/SankoPaintHostAdapter.cpp
    src/SankoPaintHostAdapter.h
    src/AnimaticPage.cpp
    src/AnimaticPage.h
    src/AnimaticTimeline.cpp
    src/AnimaticTimeline.h
    # File > Export: MP4 / PNG / PDF. MovieEncoder is the seam the MP4
    # encoder sits behind; the Windows implementation (MfMovieEncoder,
    # Media Foundation called directly) is added below, for WIN32 only.
    src/export/MovieEncoder.cpp
    src/export/MovieEncoder.h
    src/export/Mp4Probe.cpp
    src/export/Mp4Probe.h
    src/export/ProjectExport.cpp
    src/export/ProjectExport.h
    src/ConsistencyBoard.cpp
    src/ConsistencyBoard.h
    src/FloatingToolWindow.cpp
    src/FloatingToolWindow.h
    src/SankoScrollBarStyle.h
    src/SankoTheme.h
    # SankoDockOverlay.cpp/.h stay on disk but are ADS-specific and left out
    # of the build since the native docking migration (src/docking/).
    src/docking/DockController.cpp
    src/docking/DockController.h
    src/docking/DockOverlay.cpp
    src/docking/DockOverlay.h
    src/docking/DockTitleBar.cpp
    src/docking/DockTitleBar.h
    src/SankoSlider.cpp
    src/SankoSlider.h
    src/ZoomToolbar.cpp
    src/ZoomToolbar.h
    src/ColorPanel.cpp
    src/ColorPanel.h
    src/PerspectiveTool.cpp
    src/PerspectiveTool.h
    src/brushlib/BrushPreset.h
    src/brushlib/BrushPresetCodec.cpp
    src/brushlib/BrushPresetCodec.h
    src/brushlib/BuiltinRoster.cpp
    src/brushlib/BuiltinRoster.h
    src/brushlib/BrushLibraryModel.cpp
    src/brushlib/BrushLibraryModel.h
    src/brushlib/BrushPreviewRenderer.cpp
    src/brushlib/BrushPreviewRenderer.h
    src/brushlib/BrushWidthRatio.cpp
    src/brushlib/BrushWidthRatio.h
    src/brushlib/BrushImporter.h
    src/brushlib/AbrImporter.cpp
    src/brushlib/AbrImporter.h
    src/brushlib/BrushLibraryPanel.cpp
    src/brushlib/BrushLibraryPanel.h
    src/brushlib/StudioControls.cpp
    src/brushlib/StudioControls.h
    src/brushlib/ScratchCanvas.cpp
    src/brushlib/ScratchCanvas.h
    src/brushlib/BrushSettingsStudio.cpp
    src/brushlib/BrushSettingsStudio.h
    resources.qrc
    brush_assets.qrc
)

target_link_libraries(SankoTV PRIVATE Qt6::Widgets Qt6::Multimedia
    Qt6::Svg QuickShape::Kit SankoPaint::Engine)
# src on the include path: subdirectory units (src/brushlib/) include
# top-level headers like "SankoPaintHostAdapter.h" without ../ paths.
target_include_directories(SankoTV PRIVATE src)

# --- The MP4 encoder on Windows: Media Foundation, called directly ---------
# Windows system libraries only - nothing is redistributed. mfplat and
# mfreadwrite are DELAY-LOADED, and that is not an optimisation: a Windows
# "N" edition without the Media Feature Pack does not have them, and an
# ordinary import of a missing DLL stops the whole application from
# starting. Delay-loaded, the app starts, the export asks whether the
# libraries exist (MfMovieEncoder::unavailableReason) and says what to
# install. A function, because the lifecycle family links the same sources
# and needs exactly the same treatment.
function(sanko_link_media_foundation target)
    if(WIN32)
        target_sources(${target} PRIVATE
            src/export/MfMovieEncoder.cpp
            src/export/MfMovieEncoder.h)
        target_link_libraries(${target} PRIVATE
            mfplat mfreadwrite mfuuid ole32 delayimp)
        target_link_options(${target} PRIVATE
            /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll)
    endif()
endfunction()

# The lifecycle family builds the whole application (it constructs
# MainWindow); reuse the executable's own source list minus its main().
# (Read BEFORE the Media Foundation sources are added to SankoTV: the
# function adds them to each target itself.)
get_target_property(_sankotv_lifecycle_srcs SankoTV SOURCES)
list(REMOVE_ITEM _sankotv_lifecycle_srcs "src/main.cpp")
sanko_link_media_foundation(SankoTV)

# Permanent brush pixel-lock test (tests/PixelLockTest.cpp): asserts the
# committed R16 baseline hash, seeded CPU determinism, and the CPU/GPU
# tolerances (R16 3/255, RGBA16 2/255, dual 3/255). The fixture is fully
# pinned in the test file so the lock ships with the code.
# Run: build/<config>/SankoPaintPixelLock.exe (exit code = failure count).
qt_add_executable(SankoPaintPixelLock
    tests/PixelLockTest.cpp
    src/SankoPaintHostAdapter.cpp
    src/SankoPaintHostAdapter.h
)
target_include_directories(SankoPaintPixelLock PRIVATE src)
target_link_libraries(SankoPaintPixelLock PRIVATE SankoPaint::Engine)

# Permanent brush-library test (tests/BrushLibraryTest.cpp): roster
# integrity, codec round-trips through the seeded render path, the RGBA16
# and tilt audits, and the preview renderer's determinism, caching,
# cancellation and shutdown behaviour. Fixtures are pinned in the test file.
# Run: build/<config>/SankoBrushLibraryTest.exe (exit code = failure count).
qt_add_executable(SankoBrushLibraryTest
    tests/BrushLibraryTest.cpp
    src/SankoPaintHostAdapter.cpp
    src/SankoPaintHostAdapter.h
    src/brushlib/BrushPresetCodec.cpp
    src/brushlib/BrushPresetCodec.h
    src/brushlib/BuiltinRoster.cpp
    src/brushlib/BuiltinRoster.h
    src/brushlib/BrushLibraryModel.cpp
    src/brushlib/BrushLibraryModel.h
    src/brushlib/AbrImporter.cpp
    src/brushlib/AbrImporter.h
    src/brushlib/BrushPreviewRenderer.cpp
    src/brushlib/BrushPreviewRenderer.h
    src/brushlib/BrushWidthRatio.cpp
    src/brushlib/BrushWidthRatio.h
    brush_assets.qrc
    tests/test_fixtures.qrc
)
target_include_directories(SankoBrushLibraryTest PRIVATE src src/brushlib)
target_link_libraries(SankoBrushLibraryTest PRIVATE SankoPaint::Engine)

# Permanent canvas brush-ASSEMBLY lock (tests/CanvasBrushLockTest.cpp): the
# pixel lock guards the render path with its own Brush; this guards the
# DrawingCanvas slider slots that assemble the working paint brush. Fixture
# and baseline hash are pinned in the test file.
# Run: build/<config>/SankoCanvasBrushLock.exe (exit code = failure count).
qt_add_executable(SankoCanvasBrushLock
    tests/CanvasBrushLockTest.cpp
    src/DrawingCanvas.cpp
    src/DrawingCanvas.h
    src/PerspectiveTool.cpp
    src/PerspectiveTool.h
    src/SankoPaintHostAdapter.cpp
    src/SankoPaintHostAdapter.h
)
target_include_directories(SankoCanvasBrushLock PRIVATE src)
target_link_libraries(SankoCanvasBrushLock PRIVATE Qt6::Widgets Qt6::Svg
    Qt6::Concurrent QuickShape::Kit SankoPaint::Engine)

# Permanent QuickShape GEOMETRY lock (tests/QuickShapeGeometryLockTest.cpp):
# raster preview-vs-commit equality once passed while Edit Shape conversions
# were geometrically wrong (both consumed the same too-sparse stream). This
# locks the deeper invariant: canonical geometry == displayed guide == the
# exact engine replay stream, across every vertex type/count, conversion
# chain, rotated frame, and the async stale-result race.
# Run: build/<config>/SankoQuickShapeGeometryLock.exe (exit = failure count).
qt_add_executable(SankoQuickShapeGeometryLock
    tests/QuickShapeGeometryLockTest.cpp
    src/DrawingCanvas.cpp
    src/DrawingCanvas.h
    src/PerspectiveTool.cpp
    src/PerspectiveTool.h
    src/SankoPaintHostAdapter.cpp
    src/SankoPaintHostAdapter.h
)
target_include_directories(SankoQuickShapeGeometryLock PRIVATE src)
target_link_libraries(SankoQuickShapeGeometryLock PRIVATE Qt6::Widgets
    Qt6::Svg Qt6::Concurrent QuickShape::Kit SankoPaint::Engine)

# Permanent canvas-border lock (tests/CanvasEdgeLockTest.cpp): the layer
# pixels reached the boundary while the SCREEN showed a one-pixel gap, twice
# (frame pen centred on the boundary path; paper leaking between separately-
# antialiased content quads on fractional device coordinates). This samples
# real screen pixels with QScreen::grabWindow at fractional zooms, both
# workspace colourings, rotation, and the corners. Requires a desktop session.
# Run: build/<config>/SankoCanvasEdgeLock.exe (exit code = failure count).
qt_add_executable(SankoCanvasEdgeLock
    tests/CanvasEdgeLockTest.cpp
    src/DrawingCanvas.cpp
    src/DrawingCanvas.h
    src/PerspectiveTool.cpp
    src/PerspectiveTool.h
    src/SankoPaintHostAdapter.cpp
    src/SankoPaintHostAdapter.h
)
target_include_directories(SankoCanvasEdgeLock PRIVATE src)
target_link_libraries(SankoCanvasEdgeLock PRIVATE Qt6::Widgets Qt6::Svg
    Qt6::Concurrent QuickShape::Kit SankoPaint::Engine)

# Permanent variable-resolution lock (tests/CanvasSizeLockTest.cpp): the
# other six families pin 960x540 fixtures, so they gate the legacy size
# only. This family draws REAL pixels at the far edge of five canvas sizes
# (960x540, 1920x1080, 2048x1080, 3840x2160, 777x1013) through the
# synthetic-event path, and locks persistence round-trips, the
# migration/mismatch behaviour, cross-size staleness, and the
# flatten-thumb cache at a non-legacy size. Needs a GUI session but never
# samples screen pixels (no grabWindow).
# Run: build/<config>/SankoCanvasSizeLock.exe (exit code = failure count).
# Section (g) — project settings — brings the Project Settings dialog, the
# shared studio controls, and the animatic timeline (whose .cpp calls into
# AnimaticPage, hence the Multimedia link) into this family: FPS must
# re-derive frame counts, and the dialog must keep edits pending.
qt_add_executable(SankoCanvasSizeLock
    tests/CanvasSizeLockTest.cpp
    src/ProjectIO.cpp
    src/ProjectIO.h
    src/ProjectMedia.cpp
    src/ProjectMedia.h
    src/MediaCopyProgress.cpp
    src/MediaCopyProgress.h
    src/ProjectResize.cpp
    src/ProjectResize.h
    src/DrawingCanvas.cpp
    src/DrawingCanvas.h
    src/PerspectiveTool.cpp
    src/PerspectiveTool.h
    src/SankoPaintHostAdapter.cpp
    src/SankoPaintHostAdapter.h
    src/ProjectSettingsDialog.cpp
    src/ProjectSettingsDialog.h
    src/AnimaticTimeline.cpp
    src/AnimaticTimeline.h
    src/AnimaticPage.cpp
    src/AnimaticPage.h
    src/brushlib/StudioControls.cpp
    src/brushlib/StudioControls.h
)
target_include_directories(SankoCanvasSizeLock PRIVATE src)
target_link_libraries(SankoCanvasSizeLock PRIVATE Qt6::Widgets Qt6::Svg
    Qt6::Concurrent Qt6::Multimedia
    QuickShape::Kit SankoPaint::Engine)

# Permanent project-LIFECYCLE lock (tests/ProjectLifecycleTest.cpp): the one
# family that constructs MainWindow, because opening a project runs behind a
# native file dialog and nothing else in the suite covered the lifecycle at
# all. It performs REAL loads through loadFromPath so the real teardown
# deletes the real panels - the free-then-load sequence where two
# use-after-frees hid (the canvas's active panel, and the animatic timeline's
# scene list rebuilt by a frame-rate change). A check that keeps both panels
# alive cannot catch these; this one lets them die.
# Run: build/<config>/SankoProjectLifecycle.exe (exit code = failure count).
qt_add_executable(SankoProjectLifecycle
    tests/ProjectLifecycleTest.cpp
    ${_sankotv_lifecycle_srcs}
)
target_include_directories(SankoProjectLifecycle PRIVATE src)
target_link_libraries(SankoProjectLifecycle PRIVATE Qt6::Widgets
    Qt6::Multimedia Qt6::Svg Qt6::Concurrent
    QuickShape::Kit SankoPaint::Engine dbghelp)
sanko_link_media_foundation(SankoProjectLifecycle)

# --- Preset promotion instrument (permanent tool) ---------------------------
# SankoPresetDiff <override.sankobrush>: prints the field-by-field diff of a
# built-in override against its stock recipe, in recipe vocabulary, and
# PROVES the diff complete by reconstructing the override from it (byte
# equality through the codec). The promotion workflow's first step - see
# HANDOFF "Promotion".
qt_add_executable(SankoPresetDiff
    tools/PresetDiff.cpp
    src/brushlib/BuiltinRoster.cpp
    src/brushlib/BrushPresetCodec.cpp
    brush_assets.qrc
)
target_include_directories(SankoPresetDiff PRIVATE src)
target_link_libraries(SankoPresetDiff PRIVATE Qt6::Gui SankoPaint::Engine)
# --- end promotion instrument ------------------------------------------------

# --- Developer recorder (TEMP) -----------------------------------------------
# Temporary bug-capture tool; remove by deleting app/src/devrecorder/ and
# THIS block (plus the one guarded block in MainWindow.cpp).
option(SANKOTV_DEV_RECORDER "Build the temporary developer recorder" ON)
if(SANKOTV_DEV_RECORDER)
    # The git hash is captured at BUILD time, not configure time: a
    # recorded session proved that a configure-time hash names whatever
    # commit was current when CMake last ran, which had drifted four
    # commits behind the binary. See cmake/WriteGitHead.cmake.
    set(SANKOTV_GIT_HEADER ${CMAKE_CURRENT_BINARY_DIR}/SankoGitHead.h)
    add_custom_target(SankoGitHead ALL
        COMMAND ${CMAKE_COMMAND}
            -DOUT=${SANKOTV_GIT_HEADER}
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteGitHead.cmake
        BYPRODUCTS ${SANKOTV_GIT_HEADER}
        COMMENT "Capturing git HEAD for the dev recorder")
    add_dependencies(SankoTV SankoGitHead)
    target_include_directories(SankoTV PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_sources(SankoTV PRIVATE
        src/devrecorder/DevRecorder.cpp
        src/devrecorder/DevRecorder.h)
    target_compile_definitions(SankoTV PRIVATE SANKOTV_DEV_RECORDER
        SANKOTV_GIT_HEAD_GENERATED)
    qt_add_executable(SankoDevRecorderTest
        tests/DevRecorderTest.cpp
        src/devrecorder/DevRecorder.cpp
        src/devrecorder/DevRecorder.h)
    target_include_directories(SankoDevRecorderTest PRIVATE src)
    target_link_libraries(SankoDevRecorderTest PRIVATE Qt6::Widgets)

    # The lifecycle family gets the recorder too, so its section (k) can
    # assert PROVENANCE through the REAL wiring: MainWindow's HostInfo ->
    # Recorder -> system.txt, with a non-empty git hash. The hash has gone
    # silently wrong twice (configure-time staleness, then a generated
    # header included where nothing used it); the gate check plus the
    # #error fence in MainWindow.cpp make a third silence impossible.
    add_dependencies(SankoProjectLifecycle SankoGitHead)
    target_include_directories(SankoProjectLifecycle PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR})
    target_sources(SankoProjectLifecycle PRIVATE
        src/devrecorder/DevRecorder.cpp
        src/devrecorder/DevRecorder.h)
    target_compile_definitions(SankoProjectLifecycle PRIVATE
        SANKOTV_DEV_RECORDER SANKOTV_GIT_HEAD_GENERATED)
endif()
# --- end Developer recorder (TEMP) -------------------------------------------

# --- The gate harness: linked into every family, referred to by none --------
# tests/TestHarness.cpp installs itself (before main, and again when the
# application object is created), so no family's source names it - which is
# also why it can serve the two families whose sources are not to be edited.
# It does two things for an UNATTENDED gate on a machine someone is using:
# a test that cannot start prints why and exits instead of raising a dialog
# that waits for a click, and every test window opens on one chosen screen
# (SANKO_TEST_SCREEN, else the launcher's, else the primary) instead of on
# whichever monitor the mouse happens to be over. A NEW family must be added
# to this list, or it runs without either.
foreach(_gate_family
        SankoBrushLibraryTest SankoPaintPixelLock SankoCanvasBrushLock
        SankoQuickShapeGeometryLock SankoDevRecorderTest SankoCanvasEdgeLock
        SankoCanvasSizeLock SankoProjectLifecycle)
    if(TARGET ${_gate_family})
        target_sources(${_gate_family} PRIVATE tests/TestHarness.cpp)
    endif()
endforeach()

# Build as a GUI app on Windows (no console window).
set_target_properties(SankoTV PROPERTIES
    WIN32_EXECUTABLE ON
)

# --- Deploy Qt runtime next to the exe (Windows) --------------------------
if(WIN32)
    add_custom_command(TARGET SankoTV POST_BUILD
        COMMAND Qt6::windeployqt
            --no-translations
            --no-compiler-runtime
            "$<TARGET_FILE:SankoTV>"
        COMMENT "Deploying Qt runtime with windeployqt"
        VERBATIM
    )
endif()
