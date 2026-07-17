# Claude Code integration handoff

Give this entire `QuickShapeKit` folder to Claude Code together with the relevant
canvas and brush-engine files from the host application.

## Implementation request for Claude Code

Integrate `QuickShape::Kit` into this Qt 6 C++ drawing application without replacing
the existing canvas, brush engine, layer model, tablet pipeline, or Undo/Redo system.

Requirements:

1. Add `QuickShapeKit` with `add_subdirectory()` and link the canvas target to
   `QuickShape::Kit`.
2. Create one `quickshape::QuickShapeSession` owned by the canvas/controller.
3. Feed the session the same accepted pen samples used by the brush engine, converted
   to stable document or layer coordinates. Do not add a second tablet-event pipeline.
4. Keep the ordinary live brush in the host engine's reversible preview/scratch
   transaction until the hold decision. On `shapeRecognized`, discard that rough
   transaction and display `overlayPathChanged`. On `freehandStrokeFinished`, commit
   it normally. If the host paints irreversibly during pointer moves, first identify
   the host's rollback/scratch-layer mechanism; do not fake erasure with background color.
5. Render the overlay above the active layer using the canvas's normal transform.
6. On `commitRequested`, start one host Undo transaction and replay `commit.points`
   and `commit.pressures` through the existing brush engine with the currently selected
   brush. Do not use a QPainter pen or a replacement dab engine.
7. Commit on tap-away, tool/layer change, explicit Commit, save, and document close.
   Cancel only for an explicit cancel action.
8. Preserve pointer capture and correctly handle focus loss, tablet cancel/proximity,
   and a release delivered outside the canvas by forwarding a final release/reset.
9. Do not copy the demo's raster QImage, mock brush dabs, or demo history system.
10. Add host-level tests proving freehand remains unchanged when released before the
    hold delay, a held stroke produces an overlay without raster mutation, commit makes
    exactly one brush/Undo transaction, and zoom does not alter recognition.

Before editing, identify and report:

- the canvas input entry points;
- the document/layer coordinate conversion;
- the brush engine path or dab API;
- the host Undo transaction API;
- where temporary overlays are painted.
- how an in-progress brush preview can be discarded without affecting existing pixels.

If any of those contracts are missing, stop and ask for the specific host file rather
than inventing a second canvas or brush implementation.
