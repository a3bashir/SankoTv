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
find_package(Qt6 REQUIRED COMPONENTS Widgets Network Multimedia MultimediaWidgets)

# --- Qt Advanced Docking System (LGPL v2.1) --------------------------------
# LGPL compliance: MUST be linked as a shared DLL (BUILD_STATIC OFF), and its
# license text is deployed next to the executable (post-build step below).
include(FetchContent)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE) # ADS demo apps not needed
set(BUILD_STATIC OFF CACHE BOOL "" FORCE)   # shared library, per LGPL
FetchContent_Declare(ads
    GIT_REPOSITORY https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System.git
    GIT_TAG 4.3.1
)
FetchContent_MakeAvailable(ads)
# windeployqt assumes every dependency named qt6*.dll is a Qt module living in
# Qt's bin directory and hard-fails on qt6advanceddocking.dll. Renaming the
# output DLL sidesteps the heuristic (windeployqt ignores non-qt DLLs).
set_target_properties(qt6advanceddocking PROPERTIES OUTPUT_NAME "advanceddocking")

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
    resources.qrc
)

target_link_libraries(SankoTV PRIVATE Qt6::Widgets Qt6::Network Qt6::Multimedia Qt6::MultimediaWidgets
    ads::qt6advanceddocking)

# Deploy the ADS DLL and its LGPL license text next to SankoTV.exe.
add_custom_command(TARGET SankoTV POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:qt6advanceddocking> $<TARGET_FILE_DIR:SankoTV>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${ads_SOURCE_DIR}/LICENSE
        $<TARGET_FILE_DIR:SankoTV>/LICENSE.Qt-Advanced-Docking-System.txt
    COMMENT "Deploying Qt Advanced Docking System DLL + license"
    VERBATIM
)

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
