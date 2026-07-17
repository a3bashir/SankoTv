# QuickShapeKit package manifest

Version: 1.0.0  
Language: C++17  
Dependencies: Qt 6 Core and Gui  
Tested toolchain: Qt 6.11.1 with MSVC x64

## Public files

- `include/QuickShape/quickshape_types.h` — recognition and brush-commit data
- `include/QuickShape/quickshape_recognizer.h` — synchronous pure recognizer API
- `include/QuickShape/quickshape_session.h` — hold/morph/overlay/commit controller
- `README.md` — host integration and brush-engine contract
- `CLAUDE_CODE_HANDOFF.md` — implementation instructions for Claude Code

## Validation completed

- standalone static library build without Qt Widgets
- direct `add_subdirectory()` consumption
- installed `find_package(QuickShapeKit)` consumption
- recognizer smoke tests for line, angled line, ellipse, rectangle, and polygon
- timed session test for hold, morph, overlay availability, pressure mapping, and commit
- recognizer implementation equivalence with the 28-check demo production algorithm

## Host work still required

This package cannot safely guess the host application's brush-preview transaction,
layer coordinates, overlay painter, pointer-capture policy, or Undo API. Claude Code
must map the documented signals to those existing host systems. The package contains
no substitute brush engine, raster canvas, tablet-event handler, or Undo stack.

