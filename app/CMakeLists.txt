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
find_package(Qt6 REQUIRED COMPONENTS Widgets Network Multimedia MultimediaWidgets Svg)

# (Qt Advanced Docking System was removed in the native-docking migration:
# the reusable src/docking/ classes on stock QDockWidget replace it, so no
# third-party docking library is fetched, linked, or deployed.)

# --- QuickShapeKit (hold-to-shape recognition for the drawing canvas) ------
# Static recognition/session library; owns no canvas, brush engine, or undo
# history. The canvas feeds it document-space samples and replays commits
# through the existing brush engine.
add_subdirectory(third_party/QuickShapeKit)

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
    resources.qrc
)

target_link_libraries(SankoTV PRIVATE Qt6::Widgets Qt6::Network Qt6::Multimedia Qt6::MultimediaWidgets
    Qt6::Svg QuickShape::Kit)

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
