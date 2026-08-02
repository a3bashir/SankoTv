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
# Network is needed for the Claude API call in the Script Editor screen.
# Multimedia is needed for the Animatic scratch-audio track.
# Svg is required for QIcon to render .svg icon resources (without it QIcon
# silently returns a null icon for SVG files).
find_package(Qt6 REQUIRED COMPONENTS
    Core Concurrent Gui GuiPrivate Widgets Network Multimedia
    MultimediaWidgets ShaderTools Svg)

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
    src/ConsistencyBoard.cpp
    src/ConsistencyBoard.h
    src/GenerationPage.cpp
    src/GenerationPage.h
    src/FloatingToolWindow.cpp
    src/FloatingToolWindow.h
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
    src/brushlib/BrushImporter.h
    src/brushlib/BrushLibraryPanel.cpp
    src/brushlib/BrushLibraryPanel.h
    src/brushlib/StudioControls.cpp
    src/brushlib/StudioControls.h
    src/brushlib/ScratchCanvas.cpp
    src/brushlib/ScratchCanvas.h
    src/brushlib/BrushSettingsStudio.cpp
    src/brushlib/BrushSettingsStudio.h
    resources.qrc
)

target_link_libraries(SankoTV PRIVATE Qt6::Widgets Qt6::Network Qt6::Multimedia Qt6::MultimediaWidgets
    Qt6::Svg QuickShape::Kit SankoPaint::Engine)
# src on the include path: subdirectory units (src/brushlib/) include
# top-level headers like "SankoPaintHostAdapter.h" without ../ paths.
target_include_directories(SankoTV PRIVATE src)

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
    src/brushlib/BrushPreviewRenderer.cpp
    src/brushlib/BrushPreviewRenderer.h
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

# --- Developer recorder (TEMP) -----------------------------------------------
# Temporary bug-capture tool; remove by deleting app/src/devrecorder/ and
# THIS block (plus the one guarded block in MainWindow.cpp).
option(SANKOTV_DEV_RECORDER "Build the temporary developer recorder" ON)
if(SANKOTV_DEV_RECORDER)
    execute_process(COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE SANKOTV_GIT_HEAD OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    target_sources(SankoTV PRIVATE
        src/devrecorder/DevRecorder.cpp
        src/devrecorder/DevRecorder.h)
    target_compile_definitions(SankoTV PRIVATE SANKOTV_DEV_RECORDER
        SANKOTV_GIT_HEAD="${SANKOTV_GIT_HEAD}")
    qt_add_executable(SankoDevRecorderTest
        tests/DevRecorderTest.cpp
        src/devrecorder/DevRecorder.cpp
        src/devrecorder/DevRecorder.h)
    target_include_directories(SankoDevRecorderTest PRIVATE src)
    target_link_libraries(SankoDevRecorderTest PRIVATE Qt6::Widgets)
endif()
# --- end Developer recorder (TEMP) -------------------------------------------

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
