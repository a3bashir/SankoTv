# QuickShapeKit for Qt 6

QuickShapeKit is the reusable recognition and interaction core extracted from the
Quick Shape demo. It is designed to be embedded in an existing Qt 6 C++ drawing
application. It does not own a canvas, layer, brush engine, tablet event handler,
document, or Undo/Redo stack.

## What the kit provides

- line, angled-line, zigzag, arc, circle, ellipse, elliptical-arc, triangle,
  rectangle, quadrilateral, pentagon, hexagon, and structural polygon recognition
- rejection and confidence values
- 420 ms configurable hold detection
- 220 ms configurable cubic ease-in-out correction morph
- hold-to-scale/rotate behavior
- temporary `QPainterPath` overlay output
- pressure values resampled onto the corrected path
- explicit commit, cancel, tap-away commit, and edited-path replacement APIs

## Add it to an existing CMake project

Copy `QuickShapeKit` into your repository, for example under `third_party/QuickShapeKit`,
then add:

```cmake
add_subdirectory(third_party/QuickShapeKit)
target_link_libraries(YourCanvasTarget PRIVATE QuickShape::Kit)
```

Include the session:

```cpp
#include <QuickShape/quickshape_session.h>

quickshape::QuickShapeSession m_quickShape;
```

## Required host integration

1. Convert mouse/tablet positions to document coordinates before calling
   `pointerPress`, `pointerMove`, and `pointerRelease`.
2. Continue drawing the ordinary live brush stroke through your existing engine,
   but keep it in the engine's reversible preview/scratch transaction until the
   hold decision is known. Do not permanently mutate the layer on every move.
3. Draw `overlayPathChanged` above the raster canvas as a temporary vector.
4. When `shapeRecognized` fires, discard the rough preview transaction and display
   the corrected vector. When `freehandStrokeFinished` fires, finalize the ordinary
   preview as one normal brush/Undo transaction.
5. Connect `commitRequested` to one transaction in the existing brush engine.
   Render `commit.points` with the current brush and matching `commit.pressures`.
6. Put that brush transaction into the app's existing Undo/Redo system.
7. Call `requestCommit()` on tool change, layer change, tap away, save, or any
   action that should bake the temporary vector.
8. Call `cancelActiveShape()` when the user explicitly cancels the vector.

## Brush-engine contract

`QuickShapeCommit::points` and `pressures` have matching lengths. The points are
in the same coordinate space supplied by the host. The brush engine should walk
the corrected polyline by arc length and apply its normal spacing, texture, grain,
flow, opacity, blending, and pressure rules. QuickShapeKit never creates brush dabs.

## Important coordinate rule

Use document/layer coordinates, not screen coordinates. Zooming, panning, DPI,
and device-pixel ratio must not change recognition thresholds during one stroke.
If the canvas stores layer-local coordinates, convert input into layer-local space
before passing it to the session and render the returned path in that same space.
Set the dwell radius at stroke start to the document-space equivalent of roughly
8 screen pixels (for example, `8.0 / canvasScale`) so pen jitter tolerance feels
consistent at different zoom levels.

## Minimal signal wiring

```cpp
connect(&m_quickShape, &quickshape::QuickShapeSession::overlayPathChanged,
        this, [this](const QPainterPath &path) {
            m_quickShapeOverlay = path;
            update();
        });

connect(&m_quickShape, &quickshape::QuickShapeSession::shapeRecognized,
        this, [this](const quickshape::QuickShapeResult &) {
            m_brushPreview.discard();
        });

connect(&m_quickShape, &quickshape::QuickShapeSession::freehandStrokeFinished,
        this, [this] { m_brushPreview.commitToUndoHistory(); });

connect(&m_quickShape, &quickshape::QuickShapeSession::commitRequested,
        this, [this](const quickshape::QuickShapeCommit &shape) {
            auto transaction = m_document.beginUndoTransaction("QuickShape");
            m_brushEngine.renderPath(shape.points, shape.pressures, activeBrush());
            transaction.commit();
        });
```

The names on the right side are placeholders for the host application's existing
APIs; Claude Code must map them to the real brush preview and Undo transaction types.

## Editing

The host may use its existing vector-node editor. After editing, call:

```cpp
m_quickShape.setEditedShape(shapeName, editedPoints);
```

The next commit will use the edited points and the original resampled pressure profile.

## Input ownership

QuickShapeKit accepts normalized samples and intentionally contains no `QTabletEvent`
or `QMouseEvent` code. Your app remains the single owner of Wacom/Windows Ink input,
which avoids duplicate synthetic mouse events and makes pointer-capture handling
consistent with the rest of the canvas.
