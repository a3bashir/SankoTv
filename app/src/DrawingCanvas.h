#pragma once

#include "LayerTiles.h"
#include "PerspectiveTool.h"
#include "SankoPaintHostAdapter.h"
#include "camera/CameraModel.h"

#include <functional>

#include <QuickShape/quickshape_session.h>

#include <QJsonObject>

#include <QColor>
#include <QElapsedTimer>
#include <QCursor>
#include <QHash>
#include <QImage>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QPolygonF>
#include <QSet>
#include <QTransform>
#include <QPoint>
#include <QPointF>
#include <QVector>
#include <QWidget>
#include <memory>
#ifdef SANKOTV_DEV_RECORDER
#include <QVariantMap> // TEMP: dev-recorder camera probe
#endif

struct Layer;
struct Panel;
class QContextMenuEvent;
class QDragEnterEvent;
class QMenu;
class QUndoStack;
class QDropEvent;
class QAbstractButton;
class QPushButton;
class QSlider;
class QTimer;
class QThreadPool;

// Freehand drawing surface for a single storyboard panel. Composites the
// panel's layer stack scaled-to-fit (letterboxed) and edits the ACTIVE layer's
// QImage in place via mouse events.
class DrawingCanvas : public QWidget
{
    Q_OBJECT

public:
    ~DrawingCanvas() override;
    // Brush (the stamp-based pressure engine) is the single drawing tool;
    // pen-like behaviour is a brush preset. Eraser is the SAME engine with
    // the erase composite (pressure, tips, decimated preview, commit undo);
    // QuickShape stays Brush-only. Shapes stamps geometric primitives (see
    // ShapeKind). SelectRect/SelectEllipse/Lasso/SelectPoly define a mask on
    // the ACTIVE layer (marching ants); Move drags the selected pixels within
    // that layer. Camera is the SHOT CAMERA's tool (Phase 1B): it draws the
    // scene camera's frame at the playhead and its handles move, scale and
    // turn it; it never paints (see setShotCameraProvider).
    //   Lasso     = freehand: drag to trace the selection boundary.
    //   SelectPoly = polygon:  click to drop vertices, double-click/Enter to
    //                close them into the selection (Esc cancels).
    enum Tool { Brush, Eraser, Shapes, Fill, SelectRect, SelectEllipse, Lasso,
                SelectPoly, Move, Camera, Perspective };

    // Shapes-tool primitives. Rectangle/Triangle/Circle/Line commit on drag
    // release; Polygon collects clicked vertices until double-click/Enter
    // closes it (Esc cancels).
    enum ShapeKind { ShapeRectangle, ShapeTriangle, ShapeCircle, ShapeLine, ShapePolygon };

    // How a freshly-drawn selection shape combines with the existing one; the
    // Selection Modifier toolbar's Add / Remove buttons drive this.
    enum SelectionOp { SelReplace, SelAdd, SelSubtract };

    // Move tool transform-box interaction mode; the Move Modifier toolbar's
    // Pivot Point / Skew / Distort / Perspective / Warp buttons drive this.
    // Default = the classic move/scale/rotate box. Switching modes keeps the
    // same live transform session (commit/cancel ends it).
    enum XformUiMode { XformDefault, XformPivot, XformSkew, XformDistort,
                       XformPerspective, XformWarp };

    explicit DrawingCanvas(QWidget *parent = nullptr);

    // Fixed 16:9 working resolution for every panel layer.
    // The document's canvas size — derived from the ACTIVE PANEL's layers
    // (pixels are the runtime authority; the manifest shaped them at
    // create/load). INVALID QSize() when no panel is set: there is no
    // fallback size, deliberately — a 960x540 stand-in here is the defect
    // class the resolution epic removed. Document-space operations guard
    // with SANKO_REQUIRE_PANEL and refuse to run rather than compute
    // against an invalid size.
    QSize canvasSize() const;

    void setActivePanel(Panel *panel);

    // Onion skin: the selected layer's frames before and after, display-only. (Until OS-1,
    // 2026-11-27, also the previous panel's ghost - the Light Table's now.)
    void setOnionSkinEnabled(bool enabled);
    bool isOnionSkinEnabled() const { return m_onionSkin; }
    // THE DRAWINGS BEFORE AND AFTER (batch I-2, the user's answer 5): on a layer with a run,
    // the ghosts of its neighbouring drawings, each already in its colour, painted in the
    // order given with its opacity, over the picture. Not painted while the Move tool poses
    // an animated layer (the box is the layer's pose then, and the drawings are not what is
    // being worked on).
    struct OnionGhost
    {
        QPixmap pixmap;       // stage-sized, tinted (drawingGhost)
        double opacity = 0.3;
        int drawing = -1;     // which drawing of the run it is (the checks')
        bool before = true;
    };
    void setDrawingGhosts(const QVector<OnionGhost> &ghosts);
    const QVector<OnionGhost> &drawingGhostsForTest() const { return m_drawingGhosts; }
    // A drawing's own pixels as a ghost: `tint` wherever it has anything, as opaque as it is.
    static QPixmap drawingGhost(const QImage &pixels, const QColor &tint);
    // A ghost in another colour, made where it lies (2026-11-27): every pixel takes `tint`
    // and keeps its alpha. One fill - no source picture is read - so a colour dragged in
    // Preferences > Onion Skin or > Light Table costs no flatten and no tint loop.
    static void recolourGhost(QPixmap &ghost, const QColor &tint);
    // A layer of `panel` shows this drawing of its run ("" when it has none): what a pixel
    // step remembers, so that undo and redo land on the drawing they were made on.
    QString shownDrawingIdOf(const Panel *panel, const QString &layerId) const;
    void showDrawingForUndo(Panel *panel, const QString &layerId, const QString &drawingId);
    // "Seagull · drawing 3 of 10 · 2 frames": the active layer's drawing under the playhead,
    // for the status bar; empty for a layer without a run.
    QString runNoteText() const;

    // Light table: the neighbouring panels behind the current drawing - the panel before as
    // it ends, the panel after as it begins (OS-1, 2026-11-27), each in its colour at its
    // opacity (SankoLightTable.h: red and green at 35 % unless Preferences > Light Table says
    // another). Display-only (never written to any layer, flattenedPixmap, export, or save).
    void setLightTableEnabled(bool enabled);
    bool isLightTableEnabled() const { return m_lightTable; }
    // Raw flattened pixmaps of the previous / next panel (null = none). Tinted
    // ghosts are built internally. `...OnStage` (batch 6c): the picture's place on this
    // panel's stage, so panels with different stages overlay by the frame; empty, the whole.
    void setLightTablePixmaps(const QPixmap &previous, const QPixmap &next,
                              const QRect &previousOnStage = QRect(),
                              const QRect &nextOnStage = QRect());
    // The two ghosts in Preferences > Light Table's colours as they are now, coloured again
    // where they lie (recolourGhost): a colour dragged there flattens and tints nothing.
    void recolourLightTable();
    // The checks': a ghost of the Light Table is up; the ghosts built from a flatten so far;
    // a side's ghost as it is.
    bool lightTableShownForTest() const
    {
        return m_lightTable && (!m_ltPrevGhost.isNull() || !m_ltNextGhost.isNull());
    }
    int lightTableBuildsForTest() const { return m_ltBuilds; }
    QImage lightTableGhostForTest(bool before) const
    {
        return (before ? m_ltPrevGhost : m_ltNextGhost).toImage();
    }

    // Import an image file as a NEW image-type layer above the active layer
    // (scaled to fit the canvas, transparent padding). Returns true on success.
    // Shared by the button, Ctrl+I shortcut, and file drop.
    bool importImage(const QString &filePath);

    // Viewport overlays (display-only, never in flattenedPixmap()).
    void setCameraFrameEnabled(bool enabled);
    void setSafeAreaEnabled(bool enabled);
    void setTitleSafeEnabled(bool enabled);
    bool isCameraFrameEnabled() const { return m_cameraFrame; }
    bool isSafeAreaEnabled() const { return m_safeArea; }
    bool isTitleSafeEnabled() const { return m_titleSafe; }
    // VIEW > DRAWING OVERLAYS (the user's point 3, 2026-11-21): what a DRAWING tool - every
    // tool but the Camera - shows of the camera and the stage; each an entry of its own, the
    // same for every project. The Camera tool always shows its own frame, whatever these
    // say, and none of them changes the camera, its keys or its settings.
    //  - OverlayDepthOutline (off): the Move tool's dashed box where the shot puts a layer
    //    with depth, its tag, and the chip's "dashed: ..." wording.
    //  - OverlayCameraFrame (off): the camera's frame - its purple or amber line - and, with
    //    View > Camera Move Arrows, its arrows.
    //  - OverlayMarginVeil (on): the light veil over the stage margin - the margin only,
    //    fixed to the stage (until 2026-11-21 it lay outside the camera's frame, following it).
    //  - OverlayStageBorder (on): the dashed white border of the base framing, in a project
    //    with a stage margin.
    //  - OverlayDepthNote (on): the chip "Drawing view · depth shows with the Camera tool".
    enum DrawingOverlay {
        OverlayDepthOutline,
        OverlayCameraFrame,
        OverlayMarginVeil,
        OverlayStageBorder,
        OverlayDepthNote,
        DrawingOverlayCount
    };
    static bool drawingOverlayDefault(DrawingOverlay which)
    {
        return which == OverlayMarginVeil || which == OverlayStageBorder || which == OverlayDepthNote;
    }
    void setDrawingOverlayShown(DrawingOverlay which, bool shown);
    bool drawingOverlayShown(DrawingOverlay which) const { return m_drawingOverlays[which]; }
    // What the paint draws of them, widget px (the checks): the veil's area - empty when none
    // is drawn; is the camera's frame line drawn.
    QPainterPath marginVeilForTest() const { return drawingToolVeil(shotCameraView()); }
    bool cameraFrameDrawnForTest() const { return shotCameraFrameShown(shotCameraView()); }

    // SYMMETRY (batch H, 2026-11-22; Figma H-1 657:735, the user's answers of that day): a
    // switch; while it is on, a Brush or Eraser stroke is painted a second time, mirrored
    // across a vertical axis, live and as one undo step with the stroke.
    //  - THE AXIS belongs to the paper (it turns, zooms, pans and flips with the view) and
    //    to the SHOT: it is kept as whole px from the frame's left edge, so it is the same
    //    place of the shot on every panel, whatever a panel's own margin. The middle of the
    //    frame until it is moved, and again after a double-click on its grip. Saved with
    //    the project (StoryboardPage::symmetryToJson), with the switch.
    //  - THE GRIP, the round handle above the paper: a drag moves the axis left or right.
    //  - ON SCREEN ONLY: the axis and the grip are drawn by the canvas's own paint, with the
    //    Brush or the Eraser while the switch is on - never in a thumbnail, the preview, an
    //    export or the camera's shot. With any other tool, in Stage mode and without a panel
    //    nothing shows and nothing mirrors (symmetryLive); the switch stays as it is.
    //  - A STROKE keeps the mirror it began with (the adapter's setMirrorAxis); a shape
    //    held out of a stroke (QuickShape) keeps it until it is baked.
    void setSymmetry(bool on);
    bool symmetry() const { return m_symmetry; }
    void setSymmetryAxis(int framePx); // whole px from the frame's left edge
    void resetSymmetryAxis();          // the middle of the frame again
    bool symmetryAxisMoved() const { return m_symmetryAxisSet; }
    int symmetryAxis() const;          // px from the frame's left edge (the middle unless moved)
    bool symmetryLive() const;         // the axis shows and a stroke begun now is mirrored
    int symmetryAxisOnCanvas() const;  // canvas px on this panel's stage, kept on the paper
    // The checks: the axis's line and the grip's centre, widget px (empty / null when not
    // drawn); is the grip under a place; the second circle of the pointer.
    QLineF symmetryAxisLineForTest() const { return symmetryLive() ? symmetryAxisLine() : QLineF(); }
    QPointF symmetryGripForTest() const { return symmetryLive() ? symmetryGrip() : QPointF(); }
    bool symmetryGripAtForTest(const QPointF &widgetPos) const { return symmetryGripAt(widgetPos); }
    bool pointerMirrorShownForTest() const;
    QPointF pointerMirrorCentreForTest() const;

    // THE SHOT CAMERA ON THE CANVAS (Phase 1B, 2026-10-25). The workspace
    // tells the canvas, through this provider, what the scene's camera is
    // at the playhead for the panel on the canvas; the canvas never holds a
    // key. With the Camera tool active it draws the frame (the canvas's
    // aspect, 1.5 px accent), four 8 x 8 corner handles, a centre cross and
    // a 40 % dim outside, and a mode chip at the top centre; with any other
    // tool it draws the outline only, and only when the scene has keys.
    // Past the sharp limit the frame, handles and chips turn amber.
    // HANDLES ARE SCREEN-SIZED and follow the view's zoom, rotation and
    // flip: the frame is mapped through viewTransform() and every drag is
    // mapped back through toCanvasF(). A press inside the frame moves it, a
    // corner scales it about its centre, a press within 22 px outside a
    // corner turns it (Shift: 15-degree steps). NOTHING IS WRITTEN UNTIL
    // THE RELEASE: one drag asks for one edit (shotCameraEditRequested);
    // the workspace decides, and makes it one undo command. Panning (Space,
    // middle drag), Ctrl+wheel zoom and the plain wheel are view input and
    // are handled before the tool ever sees the press: they never ask.
    // When the safe-area guides are on and the camera is not neutral, they
    // are inset from the camera's frame instead of the canvas.
    struct ShotCameraView
    {
        bool valid = false;           // a scene and a panel are there
        shotcam::CameraState state;   // the camera at the playhead
        bool sceneHasKeys = false;
        bool animate = false;
        bool keyAtPlayhead = false;
        bool editable = true;         // false: a press is refused (Animate off, 2+ keys)
        bool stayInside = true;       // clamp drags to the artwork
        double sharpLimit = 1.0;      // past it: soft (amber)
        QString timecode;             // the frame a drag writes, HH:MM:SS:FF
        // A QUICK MOVE under the pointer (Phase 1D, Figma 1D-2): its START
        // drawn solid and its END dashed (a pan or tilt with an arrow from
        // centre to centre). Nothing is changed until the click.
        bool previewValid = false;
        shotcam::CameraState previewStart;
        shotcam::CameraState previewEnd;
        // What the segment at the playhead does (2026-11-06, Figma CAM-2):
        // drawn as glyphs on the frame, an editing guide only.
        shotcam::Motion motion;
        // Stage mode (batch 6c): this panel's key frames, on its stage.
        QVector<shotcam::CameraState> panelKeys;
    };
    enum ShotCameraEditKind { ShotCameraMove = 0, ShotCameraZoom = 1,
                              ShotCameraRotate = 2 };
    void setShotCameraProvider(std::function<ShotCameraView()> provider);
    void refreshShotCamera() { update(); }
    // STAGE MODE (batch 6c, Figma MARGIN-1): the Camera tool's other mode, the
    // page's session. Eight white handles outside the stage's edge propose new
    // margins for this panel; nothing changes until the page applies them.
    void enterStageMode(const QMargins &margins);
    void leaveStageMode();
    bool stageModeOn() const { return m_stageMode; }
    QMargins stageModeMargins() const { return m_stageLive; }
    void setStageModeMargins(const QMargins &margins);
    void setStageModeLinked(bool on) { m_stageLinked = on; }
    void stageModeKey(bool enter); // Enter or Esc, wherever the main window's focus is
    // The frame stays where it is on screen around a change of this panel's stage.
    void stageWillChange() { viewHoldBegin(); }
    void stageChanged();
    QPointF clipPosForTest() const { return m_clipPos; }
    QPointF floatPosForTest() const { return m_floatPos; }
    void cancelFloatingPasteForTest() { cancelFloatingPaste(); }
    QVector<QPolygonF> stageHandlesForTest() const { return m_stageMode ? stageHandles() : QVector<QPolygonF>(); }
    int stageHandleAtForTest(const QPointF &widgetPos) const { return stageHandleAt(widgetPos); }
    QString stageChipForTest(bool *warns = nullptr) const { return stageChipText(warns); }
    QString stageKeysChipForTest() const
    {
        return m_stageMode ? stageKeysChipText(shotCameraView()) : QString();
    }
    QPainterPath stageArtOutlineForTest() const { return m_stageArtOutline; }
    QRect stageLiveRectForTest() const { return stageLiveRect(); }
    int stageLayersHidingForTest() const { return stageLayersHiding(); }
    // Test hooks (read state; no test-only behaviour). The frame as drawn,
    // in widget px (empty: nothing drawn); what a press here would grab
    // (-1 nothing, 0 body, 1-4 corners TL TR BR BL, 5-8 their rotate
    // zones); the handle centres as drawn; the mode chip's text and rect
    // (empty: not drawn); whether a drag is under way; the cursor shown.
    QPolygonF shotCameraQuadForTest() const;
    // The Quick Move preview as drawn (empty: none): START and END, widget px.
    QPolygonF shotCameraPreviewStartForTest() const;
    QPolygonF shotCameraPreviewEndForTest() const;
    // THE CAMERA GUIDES (2026-11-06, Figma CAM-1 / CAM-2), as drawn: the
    // base framing's fixed dashed line in widget px (empty: not drawn), and
    // the movement glyphs - drawn (bool), zoom (+1 / -1), rotate (+1 / -1),
    // panX / panY (the pan arrow's widget-space unit direction), onEnd (on
    // the Quick Move preview's END rather than the frame), big (the Camera
    // tool's size).
    QPolygonF shotCameraBaseQuadForTest() const;
    QVariantMap shotCameraGuidesForTest() const;
    // The glyphs' strokes exactly as the canvas paints them (widget px), and
    // their pen's width; empty when no glyph is drawn.
    QPainterPath shotCameraGlyphPathForTest(double *penWidth = nullptr) const;
    // Leaves the glyphs out of the paint (and only them), so the gate can
    // tell their pixels from everything else's.
    void setMotionGlyphsHiddenForTest(bool hidden) { m_glyphsHiddenForTest = hidden; update(); }
    int shotCameraHitForTest(const QPointF &widgetPos) const;
    QVector<QPointF> shotCameraHandlesForTest() const;
    QString shotCameraChipForTest() const;
    // DEPTH (Phase 2B): the drawing-view chip's text (empty: not drawn), and
    // whether the Camera tool is showing the layers placed for their depth.
    QString drawingViewChipForTest() const;

    // THE NOTE IN THE STATUS BAR (batch I-1b round 1, 2026-11-25: the user's change after hand
    // test I1b-1 - "the notes are still distracting over the canvas, even at the top centre").
    // Every note SankoTV shows is said in ONE place, the bottom status bar, between Import Image
    // and "Recovery copy saved"; nothing is drawn over the canvas any more. The canvas works
    // out what the bar says and tells the page when it changes (noteBarChanged):
    //   * the active tool's own note - the Camera tool's ("SHOT CAMERA - ANIMATE ON - ..."),
    //     the Move tool's on an animated layer ("Boat - key 2 of 3 - frame 93 - ..."),
    //     "Drawing view - ..." with a drawing tool - live, also while the Move box or the
    //     camera's frame is dragged;
    //   * a note SAID by the workspace, the timeline or the Layers panel, for its time (one at
    //     a time, the newest), then the tool's own note again. A refusal or a warning
    //     ("Layer 2 is locked") is said in amber (warning = true), any other note in grey.
    // While a drawing tool's stroke is under way (Brush, Eraser, Fill, Shapes; the mouse or the
    // pen) the bar's words do not change: a note said meanwhile waits for the release, and
    // the bar is brought up to date then. The readouts that follow the pointer through a drag
    // (the camera's zoom or turn, the depth layer that holds a frame, the stage's size) stay
    // at the pointer.
    // The Camera tool's note is grey but for its "Soft 1.43x" warning, which is amber like the
    // other warnings (batch I-1b round 2, the user's answer): noteBarAmberFrom is where the
    // amber part of the words starts (-1: none).
    // THE ONE EXCEPTION, the user's deliberate choice (2026-11-25, after hand test I1b-2): the
    // Perspective tool's "Tap anywhere to create the first Vanishing Point." is painted at the
    // canvas's centre, as before round 1, never in the bar (perspectivePromptRect).
    static constexpr int kNoteMs = 4000;
    void showNote(const QString &text, int ms = kNoteMs, bool warning = false);
    enum class NoteKind { Tool, Said, Warning };
    QString noteBarText() const { return m_noteBarText; } // what the bar says now ("" nothing)
    // What the bar says is worked out again after this event (the drawing under the playhead
    // changed, batch I-2: no tool and no paint need have).
    void refreshNoteBar() { queueNoteBarSync(); }
    NoteKind noteBarKind() const { return m_noteBarKind; }
    QString noteBarTip() const { return m_noteBarTip; }   // more than the words: the passed layers
    int noteBarAmberFrom() const { return m_noteBarAmberFrom; }
    QString toolNoteText() const; // the active tool's own note, worked out now
    QRectF perspectivePromptRectForTest() const { return perspectivePromptRect(); }
    static QString perspectivePromptText();
    bool noteSaidShownForTest() const { return m_noteBarKind != NoteKind::Tool; } // a said note
    bool notesHeldForTest() const { return m_notesHeld; }
    QString notePendingForTest() const { return m_notePending; }
    void expireNoteForTest() { expireNote(); } // the said note's time is up, now
    // Batch B2: the depth outline (canvas px; empty: none) and whether the last
    // paint drew it.
    QPolygonF depthOutlineForTest() const
    {
        QPolygonF quad;
        double depth = 1.0;
        QString name;
        return depthOutline(&quad, &depth, &name) ? quad : QPolygonF();
    }
    bool depthOutlinePaintedForTest() const { return m_depthOutlinePainted; }
    bool shotCameraDepthViewForTest() const;
    // Phase 2C: the drag chip naming the depth layer that holds a frame
    // drag in ("Edge of 'Ground'"; empty: none shown).
    QString shotCameraEdgeChipForTest() const { return shotCameraEdgeText(); }
    bool shotCameraDraggingForTest() const { return m_camDrag >= 0; }

    // Safe-area guide opacities in percent (Preferences > Camera). Defaults
    // are loaded from sankoSettings() in the constructor.
    void setActionSafeMaskOpacity(int percent); // action-safe amber
    void setTitleSafeMaskOpacity(int percent);  // title-safe blue
    // ...and their lines' thickness in screen pixels, 1 to kSafeLineMax (Preferences > Camera,
    // 2026-11-27; 1, the line they always had, unless set). The labels are not made heavier.
    static constexpr int kSafeLineMax = 6;
    void setActionSafeLineWidth(int px);
    void setTitleSafeLineWidth(int px);
    int actionSafeMaskOpacity() const { return m_actionSafeMaskPct; }
    int titleSafeMaskOpacity() const { return m_titleSafeMaskPct; }
    int actionSafeLineWidth() const { return m_actionSafeLinePx; }
    int titleSafeLineWidth() const { return m_titleSafeLinePx; }

    // Canvas VIEW transforms (display only; never touch layer pixels/export).
    void setViewZoom(double zoom);        // 0.25..4, centred on the view
    void setViewRotation(double degrees); // -180..180, around the view centre
    void toggleFlipH();                   // horizontal flip of the view
    void resetViewRotation();             // rotation back to 0
    double viewZoom() const { return m_zoom; }
    double viewRotation() const { return m_viewRotation; }
    bool viewFlipH() const { return m_viewFlipH; }
    QPointF viewPanOffset() const { return m_panOffset; }

    // A NEW PROJECT is arriving (open, new, or close): put the camera and
    // the per-drawing view aids back to what a freshly launched app shows.
    //
    // One long-lived canvas serves every project, so without this the view
    // simply carries over — which looked like zoom and rotation being
    // "saved" into a project and "leaking" between projects, when in truth
    // neither was ever written to disk at all.
    //
    // NOT resetView(): that is the Reset button's contract and lands at
    // zoom 1.0 (exact fit), whereas a launched app starts at kStartupZoom.
    // Opening a project should look exactly like starting the program.
    //
    // Grid and safe-area guides are deliberately NOT touched: they are
    // app-wide preferences persisted in QSettings, not project view state.
    void resetViewForNewProject();

    // Light alignment grid (View > Grid), display-only like the overlays.
    void setGridEnabled(bool enabled);
    bool isGridEnabled() const { return m_grid; }

    // Perspective guides + snap (display-only overlay; the settings panel and
    // project save/load talk to the same instance). Call update() after edits.
    PerspectiveTool *perspective() { return &m_perspective; }
    // Batch 6c: the tool holds them on the stage of the panel on the canvas; the
    // file and the undo steps take the keys' one space.
    QJsonObject perspectiveInKeySpace() const;
    void perspectiveFollowPanel(); // that panel's stage changed, or the panel did
    void perspectiveLoaded();      // the tool was just given the keys' space

    // QuickShape: hold-to-shape assist for the Brush tool. The session sees
    // the same canvas-space samples the brush engine paints; a recognized
    // shape becomes a temporary vector overlay that is only rasterized by
    // replaying it through the normal brush engine (one undo entry).
    void setQuickShapeEnabled(bool enabled);
    bool quickShapeEnabled() const { return m_quickShapeEnabled; }
    // THE dwell-chrome state. The progress ring and the post-recognition
    // hint are VALUES OF ONE ENUM, not two visibility flags: "never both
    // at once" is then a property of the type rather than a timing
    // coincidence two independent bools happen to honour. paintEvent
    // switches on this once, so drawing both is unrepresentable.
    // Both occupy quickShapeOverlayCentre(), so the hint lands exactly
    // where the ring was — the handover is a value change, not a move.
    enum class QsOverlay { None, Dwell, Hint };
    QsOverlay quickShapeOverlay() const;
    QPointF quickShapeOverlayCentre() const;
    // 0..1 across the VISIBLE window only (the final 1 - ringRevealFraction
    // of the hold, remapped to a full sweep). Meaningless unless the state
    // is Dwell.
    double quickShapeDwellSweep() const;
    quickshape::QuickShapeSession *quickShape() { return &m_quickShape; }
    void commitQuickShape(); // resolve any pending temporary shape (lifecycle)
    void cancelQuickShape(); // explicit cancel: discard the temporary vector
    // PERMANENT test surface (SankoQuickShapeGeometryLock): the exact
    // point stream the engine replay consumes for the current shape, and
    // the settled preview raster. Read-only; no test-only behaviour.
    QVector<StrokePoint> quickShapeReplayStreamForTest() const;
    quickshape::QuickShapeGeometry quickShapeCanonicalGeometryForTest() const
    {
        return m_qsGeometry;
    }
    QImage quickShapePreviewForTest() const { return m_qsPreview; }
    quint64 quickShapePreviewShownGenForTest() const
    {
        return m_qsPreviewShownGen;
    }
    bool quickShapePreviewBusyForTest() const
    {
        return m_qsPreviewInFlight || m_qsPreviewDirty
               || (m_qsPreviewTimer && m_qsPreviewTimer->isActive());
    }
    // The pen-tap check (clean-up batch, 2026-10-30): a stroke's commit
    // still in flight, and the inked pixels of the live preview - what the
    // artist sees before the release.
    bool paintCommitPendingForTest() const { return m_paintCommitPending; }
    int previewInkPixelsForTest() const;

    Tool tool() const { return m_tool; } // active tool (per-tool sliders)
    // Selection changed while Move is active: commit the running transform
    // and re-lift around the NEW target (layer or group) — no tool toggle.
    void refreshTransformBox();
    // A SCRUB DRAG ON THE TIMELINE (batch I-1b, the user's note 1 and answer 6): while the
    // playhead is dragged, each panel it crosses is selected, and the Move tool's box - a
    // lift of the layer's pixels, the costly part of a crossing (64-71 ms on the 4K song
    // copy, more than a frame) - waits for the drag's end; the canvas shows the panel's
    // layers meanwhile.
    void setScrubbing(bool on);
    bool scrubbingForTest() const { return m_scrubbing; }
    bool liftWaitsForTest() const { return m_liftAfterScrub; }
    // The layer model changed UNDERNEATH a Move session (e.g. layers were
    // merged): the lifted buffers are stale, so drop them without committing
    // and lift a fresh box around the new active target.
    void resetTransformBox();
    // The Layers panel mirrors its row multi-selection here: with several
    // rows selected, the Move tool lifts them ALL behind one box and the
    // same transform lands on each, in place.
    void setSelectedLayerIds(const QStringList &ids)
    {
        m_selectedLayerIds = ids;
    }
    // Fit Screen: centre the SHOT in the viewport and fit it fully with a
    // small margin, whatever the current zoom/pan (rotation/flip untouched).
    // Without a stage margin the shot is the whole canvas, as it always was;
    // with one (Phase 2A, the user's word) it is the frame, and Fit Stage
    // fits the whole stage.
    void fitToScreen();
    void fitStageToScreen();
    // THE STAGE MARGIN (Phase 2A): px each side of the shot (0 x 0: none).
    // With one, every tool shows the frame - a 1 px outline and the margin
    // veiled 20 % with a drawing tool, the Camera tool's own frame and dim
    // with it - and the camera's limits and guides use the frame.
    void setStageMargin(const QSize &margin);
    shotcam::Stage stage() const;

    // Standalone Sanko Paint engine settings. The engine receives only
    // canvas-space samples and one stable layer key; SankoTV retains all
    // Scene/Panel/group semantics around this boundary.
    ::Brush &paintBrush() { return m_paintEngine.brush(); }
    const ::Brush &paintBrush() const { return m_paintEngine.brush(); }
    // The layer stack changed outside the canvas (visibility, opacity,
    // reorder, undo, ...): drop the below/above composite caches so the next
    // paint recomposites. Cheap to call; the rebuild is lazy.
    void invalidateComposite()
    {
        m_compValid = false;
        m_lastCompositeInvalidatedRect = QRect(QPoint(), canvasSize());
    }
    void invalidateCompositeRegion(const QRect &canvasRect)
    {
        // The caches deliberately exclude the active layer, so its pixel edit
        // requires no cache rebuild. Record and repaint only the changed area.
        m_lastCompositeInvalidatedRect = canvasRect.intersected(
            QRect(QPoint(), canvasSize()));
        updateBrushRegion(m_lastCompositeInvalidatedRect);
    }
    QRect lastCompositeInvalidatedRect() const
    {
        return m_lastCompositeInvalidatedRect;
    }
    int eraserSize() const { return m_eraserSize; }
    int eraserOpacity() const { return qRound(m_eraserOpacity * 100.0); }

public slots:
    void setTool(Tool tool);
    void setColor(const QColor &color);
    // Gate seam: the shared app colour (identity-colour adoption and
    // restore are asserted against this, not against the engine brush).
    QColor currentColorForTest() const { return m_color; }
    void setBrushSize(int size);
    void undo();   // app-wide history (Ctrl+Z / toolbar Undo)
    void redo();   // app-wide history (Ctrl+Y / toolbar Redo)
    void clearCanvas();

    // Brush engine settings (the stamp-based Brush tool only).
    void setBrushToolSize(int px);       // 1..5000, canvas pixels
    void setBrushOpacity(int percent);   // 0..100
    void setBrushHardness(int percent);  // 0..100 (100 = sharp edge)

    // Brush Library: replace the working paint brush with a preset's FULL
    // parameter set (a copy — canvas edits never write back into the
    // preset). Slider mirrors and pressure toggles resync; emits
    // paintBrushChanged() so the Brush Options panel can follow.
    void setPaintBrush(const ::Brush &brush);

    // Eraser Library: the preset's full parameter set becomes the working
    // ERASER brush (a copy; eraseMode is forced true - it is the preset's
    // identity, not an editable knob). Size CTL mirrors resync via
    // eraserBrushChanged. The paint brush is untouched.
    void setEraserPreset(const ::Brush &brush);
    const ::Brush &eraserBrush() const { return m_eraserBrush; }

    // APP-LEVEL stroke stabilization (Brush Settings studio, Smoothing
    // section — option (c)): an input-position EMA applied to LIVE brush
    // strokes only, before QuickShape and the engine see the points. 0 (the
    // default) bypasses the filter entirely — the legacy input path stays
    // byte-identical. Never per-brush, never serialized into presets; the
    // QuickShape replay and undo replay paths are untouched.
    void setStrokeStabilization(double amount); // 0..1, QSettings-backed

    // Eraser settings — independent of the brush (per-tool Size CTL).
    void setEraserSize(int px);          // 1..5000, canvas pixels
    void setEraserOpacity(int percent);  // 0..100 (erase strength)
    void setPressureToSize(bool on);
    void setPressureToOpacity(bool on);

    // Shapes tool settings (Shapes options panel).
    void setShapeKind(ShapeKind kind);   // cancels any in-progress shape
    void setShapeStrokeWidth(int px);    // 1..100, canvas pixels
    void setShapeFill(bool on);          // filled with the current colour

    void setSelectionOp(SelectionOp op);
    SelectionOp selectionOp() const { return m_selOp; }

    // Move Modifier toolbar: transform-box interaction mode (see XformUiMode).
    void setXformUiMode(XformUiMode mode);
    XformUiMode xformUiMode() const { return m_xformUiMode; }

    // Selection Modifier "Move": while on, dragging with a selection tool
    // translates ONLY the selection outline (marching ants) — artwork pixels
    // are never lifted or altered.
    void setSelectionOutlineMove(bool on);
    bool selectionOutlineMove() const { return m_selOutlineMove; }

    // Selection + canvas clipboard (ACTIVE layer only). Copy reads even a
    // locked layer (not an edit); cut/paste/move require an editable one.
    bool hasSelection() const { return !m_selectionPath.isEmpty(); }
    // An edit the user is in the middle of. A project resize REFUSES while
    // one is running rather than committing it: committing an in-flight
    // stroke or a half-placed transform writes pixels the user never
    // decided to keep, into an undo stack the resize is about to clear, so
    // it could not be taken back. Names what is active for the message.
    // The canvas changed size under the view: re-fit and re-centre, so the
    // artist lands on the whole new canvas instead of an old pan offset.
    void resetViewAfterResize() { resetView(); }
    bool hasActiveEdit(QString *what = nullptr) const
    {
        // Quick Shape is checked FIRST because it rides on a brush press:
        // both flags are set during a held recognition, and "a Quick Shape"
        // is the one that tells the artist what they are actually holding.
        if (m_qsHeld || quickShapePreviewBusyForTest()) {
            if (what)
                *what = QStringLiteral("a Quick Shape");
            return true;
        }
        // m_brushStroke covers BOTH the Brush and the (engine) Eraser -
        // one in-flight flag since the erase composite pass.
        if (m_brushStroke || m_shapeDrag) {
            if (what)
                *what = QStringLiteral("a brush stroke");
            return true;
        }
        if (m_moveActive || !m_transformBuf.isNull()) {
            if (what)
                *what = QStringLiteral("a transform");
            return true;
        }
        return false;
    }
    // Is the pointer holding something - an edit (above), a selection or
    // camera drag, a pan, a floating bar - or is a stroke's commit still in
    // flight? The recovery copy (2026-11-04) waits for the hold to end.
    bool inputHeld() const
    {
        return hasActiveEdit() || m_camDrag >= 0 || m_selDrag || m_selOutlineDrag
            || m_panning || m_floatDragging || m_paintCommitPending;
    }
    // The Move tool keeps its box up all the time. One nobody has changed holds no
    // edit: committing it writes nothing, dropping it loses nothing (the trial of
    // 2026-11-09: Grow to fit was refused for a box that had never been touched).
    bool transformUntouched() const;
    // THE CANVAS'S KEYS GO WHERE THE POINTER IS (the trial of 2026-11-09): Space,
    // Enter and Esc pressed while the pointer is over the canvas are handed here by
    // the page, whatever widget holds the keyboard. True when the canvas used the key.
    bool routedKey(QKeyEvent *event);
    // A Space held through the page and never released here (the window lost
    // activation): the pan modifier does not stay on.
    void dropSpaceHold();
    bool spaceHeld() const { return m_spaceHeld; }
    bool spaceHeldForTest() const { return m_spaceHeld; }
    // R HELD (batch C, Photoshop's rotate-view key): a left drag turns the view around the
    // drawing's centre, the angle under the pointer kept; let go of R and the tool is
    // back. Routed like Space (the page hands it over while the pointer is over the
    // canvas), and dropped the same way when the window loses activation.
    bool rotateHeld() const { return m_rotateHeld; }
    void dropRotateHold();
    bool rotatingForTest() const { return m_rotating; }
    // THE ROTATION INDICATOR while R is held (the user's design, Tile 3, Figma 608:702): its
    // arcs turn with the canvas, at 70 % with a white outline inside them at 70 %; the angle
    // in the middle stays upright at 100 % with a soft shadow under it; half the design's
    // size, on the point the view turns about. For the gate: shown, where, its text, its
    // scale, and the indicator alone at an angle (arcs, number or both) on a transparent
    // 300 x 300 square with its centre in the middle.
    bool rotationIndicatorShownForTest() const { return m_rotateHeld || m_rotating; }
    QPointF rotationIndicatorCentreForTest() const { return rotationIndicatorCentre(); }
    static QString rotationIndicatorText(double degrees);
    static double rotationIndicatorScaleForTest();
    // shadow: 1 as shown (with the number's soft shadow), 0 without it, 2 the shadow alone.
    QImage rotationIndicatorImageForTest(double degrees, bool arcs, bool number,
                                         const QString &text = QString(), int shadow = 1) const;
    // The arcs' parts at an angle, on the same square: 0 as drawn, 1 the fills' cover alone
    // (100 %), 2 the outline's whole 3-unit stroke (100 %), 3 the outline as drawn (the inside
    // half, 70 %).
    QImage rotationArcsPartForTest(double degrees, int part) const;
    // Where the number's design box (131, 135, 102 x 56 in the tile) lands on that square;
    // and, in the tile's own units, the outline box of a number's glyphs as laid out.
    static QRectF rotationNumberBoxForTest();
    static QRectF rotationNumberInkForTest(const QString &text);
    // THE KEYS' ZOOM (batch C): Ctrl+= / Ctrl+- multiply the zoom (1.25) about the view's
    // centre; Ctrl+1 shows one drawing pixel per screen pixel (within the zoom's 0.25 to
    // 4 times the fit).
    void zoomStepBy(double factor);
    void zoomToActualPixels();
    double actualPixelsZoom() const;
    // View > Camera Move Arrows (batch C, the user's choice: off by default): the glyphs
    // for the move at the playhead on the camera frame. A hovered Quick Move's, on its
    // end, shows whatever this says.
    void setCameraMoveArrowsShown(bool shown);
    bool cameraMoveArrowsShown() const { return m_moveArrowsShown; }
    bool penSideButtonHeldForTest() const { return m_penSideButtonHeld; }
    // Batch B1: the active layer's art, kept art included, in canvas px.
    QRect layerArtBoundsForTest(const Layer &layer) const { return layerArtBounds(layer); }
    QImage layerArtImageForTest(const Layer &layer, const QRect &rect) const { return layerArtImage(layer, rect); }
    // The Move tool's box as it stands (empty: none), a pasted drawing still
    // floating, and the press gate's "on the paper as shown".
    QPolygonF transformQuadForTest() const { return m_xformActive ? m_quad : QPolygonF(); }
    bool floatingPasteForTest() const { return m_floatActive && m_floatFromPaste; }
    QRectF floatBoundsForTest() const { return m_floatActive ? floatBounds() : QRectF(); }
    bool onPaperForTest(const QPointF &widgetPos) const { return onPaper(widgetPos); }
    bool panningForTest() const { return m_panning; }
    bool hasCanvasClipboard() const { return !m_clipImg.isNull(); }
    // Closing a project drops the copied pixels with it: they belong to the
    // project that was closed, and would otherwise be pasteable into the
    // next one. (Pasting is already impossible with no panel, so this is
    // about not carrying one project's artwork into another, not a crash.)
    void clearCanvasClipboard() { m_clipImg = QImage(); }
    void copySelection();                 // selected pixels -> internal clipboard
    void cutSelection();                  // copy, then clear to transparent (undoable)
    void pasteClipboard(bool atOriginalPos); // floating paste; commit on click-away/Enter
    // Edit > Paste (batch D): the copied drawing placed at once on the active layer - the view's
    // centre, or (atOriginalPos) exactly where it was copied from - no float, no history (the
    // page's "Paste" step holds it). False: nothing copied, or the layer cannot take it.
    bool placeClipboardOnActiveLayer(bool atOriginalPos);
    // Before the layer stack changes under the canvas (Edit > Paste's new layer): a pending
    // stroke commit, a QuickShape and a floating paste land, a live transform commits - as
    // Undo resolves them first.
    void settleForLayerChange();
    // THE EYEDROPPER (batch D; the user's D-2, approved 2026-11-14). armColorPick(true) - the
    // Colors panel's button, the I key - takes the next press on the canvas as ONE pick and is
    // off again after it: the tool never changes. Alt held with the Brush picks for as long as
    // it is held. A press shows the sampling ring (the colour under the pointer above, the
    // colour you had below, a neutral grey ring round both) until it is let go; the colour
    // under the pointer is then colorPicked. A pick is not a stroke. The dropper - the pointer
    // and the ring's centre - is always the user's black one with a thin white outline (D-3).
    void armColorPick(bool on);
    bool colorPickArmed() const { return m_pickArmed; }
    // A left press on the canvas now would be a pick - armed, or Alt with the Brush - and not
    // a pan, a turn or an Edit Shape press: what mousePressEvent decides, for who must know
    // before it (the page, 2026-11-21: a pick never closes the Colors panel).
    bool pressWouldPickColor(Qt::KeyboardModifiers modifiers) const
    {
        return m_panel && !m_qsEditing && !m_spaceHeld && !m_rotateHeld && pickWanted(modifiers);
    }
    void setAltHeld(bool held); // the page reports the Alt key, wherever it went
    // The colour shown at a widget point - the paper and every visible layer at its
    // effective opacity, as the canvas composites them; never an overlay (the margin veil,
    // the onion skin, the camera frame, guides, the grid). False off the stage.
    bool colorSeenAt(const QPointF &widgetPos, QColor *out);
    bool pickingForTest() const { return m_picking; }
    QColor pickRingTopForTest() const { return m_pickColor; }
    QColor pickRingBottomForTest() const { return m_pickBefore; }
    QPointF pickRingCentreForTest() const { return m_pickPos; }
    bool pickCursorShownForTest() const { return m_pickCursorOn; }
    // FLIP H / V (batch D): part of the open Move box - its lifted pixels are mirrored inside
    // the box (outline and handles stay), shown at once; Enter lands it with the rest, Esc
    // takes it back with the rest. False with no box open.
    bool canFlipTransform() const { return m_xformActive && !m_transformBuf.isNull(); }
    bool moveBoxOpen() const { return m_xformActive; } // the Move tool's box is up: Enter lands it
    bool flipTransform(bool horizontal);
    void clearSelection();                // Esc equivalent
    void deselect();                      // clearSelection + selection-history entry
    void selectAll();                     // select the whole canvas
    void invertSelection();               // canvas minus current selection

    // App-wide undo plumbing. The shared QUndoStack (owned by MainWindow)
    // receives every mutating action as a QUndoCommand; the commands call
    // back into these apply methods.
    void setUndoStack(QUndoStack *stack) { m_undoStack = stack; }
    // `tiles`: the layer's tiles to set as well (batch B1: an edit that kept art
    // past the stage); null: the tiles are not touched.
    void applyLayerRegionForUndo(Panel *panel, const QString &layerId,
                                 const QRect &region, const QImage &pixels,
                                 const layertiles::TilesPtr *tiles = nullptr);
    void applyBrushCommitForUndo(Panel *panel, const QString &layerId,
                                 const BrushStrokeCommit &commit, bool after);
    bool flushPaintCommit(int timeoutMs = 5000);
    void ensurePanelCpuCoherent(Panel *panel, BrushCoherenceTrigger trigger);
#ifdef SANKOTV_DEV_RECORDER
    // TEMP dev-recorder camera probe (read-only, no behaviour change;
    // compiled out and removed together with the recorder).
    QVariantMap devCameraState() const;
#endif
    void applySelectionPathForUndo(const QPainterPath &path);

    // Perspective undo plumbing: every completed perspective gesture (VP
    // create/move/delete, settings edit) pushes ONE command holding full
    // before/after model snapshots. begin/end bracket toolbar slider drags.
    void applyPerspectiveForUndo(const QJsonObject &state);
    void beginPerspectiveEdit();
    void endPerspectiveEdit(const QString &text);

    // --- Workspace grid (gutter-only, screen-space) -----------------------
    // View furniture drawn behind the OPAQUE paper, anchored to the viewport:
    // constant 25 screen-px spacing whatever the camera does. Pure view
    // state: never touches layer pixels, undo, save/load, or thumbnails.
    // (public:, not slots — the enum/constant may not live in a moc slots
    // section, and everything here is connected through lambdas.)
public:
    enum class GridStyle { Square = 0, Cross = 1, Dot = 2, Dashed = 3 };
    static constexpr int kGridSpacing = 25; // screen px, fixed
    bool gridVisible() const { return m_gridVisible; }
    GridStyle gridStyle() const { return m_gridStyle; }
    QColor gridColor() const { return m_gridColor; }

    // PERMANENT test surface (SankoCanvasEdgeLock): exact view placement and
    // workspace colours without persisting to QSettings. m_zoom is
    // fit-relative and the pan interacts with rotation/flip, so tests cannot
    // reach an exact on-screen scale through the public view setters alone.
    // No test-only rendering behaviour — these only set existing view state.
    void placeViewForTest(double onScreenScale, const QPointF &canvasPt,
                          const QPointF &widgetPt);
    QTransform viewTransformForTest() const { return viewTransform(); }
    void setWorkspaceForTest(const QColor &gutter, bool gridVisible,
                             GridStyle style, const QColor &gridColor);
    QColor gutterColor() const { return m_gutterColor; }
    void setGridVisible(bool on);
    void setGridStyle(GridStyle style);
    void setGridColor(const QColor &color);
    void setGutterColor(const QColor &color);
    // THE STAGE BACKGROUND (batch E, Figma E-1; the user's answer: on screen only): the
    // paper's colour ON THE CANVAS - laid over the white Background layer by multiplying, so
    // no layer's pixels change - while thumbnails, the strip, the film and every export keep
    // white paper. A view setting the app keeps, like the gutter's: not in the project, no
    // undo. The presets and Custom... in View > Stage Background and on the canvas's menu.
    QColor paperColor() const { return m_paperColor; }
    void setPaperColor(const QColor &color);
    void addStageBackgroundMenu(QMenu *parent); // the submenu, wherever it is offered
    void setPaperColorPickForTest(std::function<QColor(const QColor &)> pick) { m_paperPickForTest = std::move(pick); }
    // THE COLOUR PICKER (COLOUR-1, 2026-11-18): every Custom... on the canvas's menus - Stage
    // Background, Grid Colour, Gutter Background - asks this, titled with what it sets; the page
    // gives it the SankoTV picker. Unset (the lock tests' canvases), Custom... picks nothing.
    using ColorPicker = std::function<QColor(const QColor &initial, const QString &title)>;
    void setColorPicker(ColorPicker picker) { m_colorPicker = std::move(picker); }

    // CONTEXTUAL CURSORS (batch G; Figma G-1 645:702, approved 2026-11-19 with the user's
    // answers). The cursor says what a press will do. The Brush and the Eraser: an outline
    // circle the size of the tip - the Size bar's number times the zoom, 1 px black inside
    // 1 px white - drawn while the pen hovers and while it draws (answer 1: A); under 6 px
    // across today's crosshair instead, from 100 px a small cross at its centre (answer 2).
    // Fill: the toolbar's bucket. The selection tools: a crosshair with + (Add) or - (Remove),
    // the four arrows for the outline's Move. The others as before. Over a locked or hidden
    // layer, wherever a press would do nothing because of it: Windows' "not allowed", and
    // the press says why (editRefused; answer 3) - and still changes nothing.
    // The Size bar's number for a tool - its VISIBLE width (BrushWidthRatio), what the circle
    // is the size of. 0 (no bar: the lock tests' canvases): the engine size.
    void setShownToolSize(bool eraser, int px);
    // Something the cursor depends on changed away from the canvas (the page: a layer's
    // lock or eye, the active layer): the cursor and the circle follow at once.
    void refreshToolCursor();
    // Called by the application-wide pointer watch (see PointerWatch) for every real pointer
    // event, at the widget it reaches first: over the canvas or something on it, or not.
    void notePointerEvent(QObject *receiver, QEvent *event);
    // The circle is drawn on a see-through widget of its own just above the canvas (its
    // sibling), never into the canvas: grab() and every check that reads the canvas's pixels
    // see the picture as it always was. Null until first needed, and with no parent.
    QWidget *pointerOutlineForTest() const;
    bool pointerOutlineShownForTest() const;
    QPointF pointerOutlineCentreForTest() const;   // canvas coordinates
    double pointerOutlineDiameterForTest() const;  // screen px
    // "outline" (no pointer, the circle), "cross", "bucket", "add", "remove", "move",
    // "forbidden": the tool's own idle cursor where the pointer is.
    QString toolCursorKindForTest() const { return toolCursorKind(); }
    QString layerRefusalForTest() const { return layerRefusal(); }
    bool pointerOverCanvasForTest() const { return m_pointerIn; }
    // The gate's pointer: with real ones ignored, only events of the test's own device move
    // the circle (the machine's mouse, wherever it rests, is not part of a check).
    void setRealPointerIgnoredForTest(bool ignored);
    void setPointerDeviceForTest(qint64 uniqueId);

    // --- LAYER MOTION (batch I-1, 2026-11-24; Figma I-1 693:755, approved) -------------
    // The page tells the canvas where the playhead is in the panel on it. The Drawing view
    // shows every layer as drawn (a stroke lands where the pen is), as with depth. THE MOVE
    // TOOL ON A LAYER WHOSE ANIMATE IS ON shows the panel's motion at the playhead, and its
    // box is the layer's pose there: a drag moves it, a corner sizes it (one size for both
    // directions), outside a corner turns it, about its pivot - and on release the page sets
    // the key at the playhead (layerPoseEdited). With it, and only then (the user's answer
    // c), the faint copies at the layer's other keys and the dotted path through them. The
    // Camera tool shows the motion with the camera's shot.
    struct MotionView
    {
        bool valid = false;
        double at = 0.0; // the place in the panel, 0 .. 1 (shotcam::panelAt)
        int frame = 0;   // the playhead's frame in the panel
        int frames = 1;  // the panel's length
        int fps = 24;    // the project's rate, what `frame` and `frames` are counted at (batch I-2)
    };
    void setMotionProvider(std::function<MotionView()> provider);
    void refreshMotion() { update(); }
    // The centre of a layer's drawn art, stage px (its pivot when it first takes a key);
    // the frame's centre for a layer with nothing drawn.
    QPointF artCentreOf(const Layer &layer) const;
    bool poseModeForTest() const { return poseMode(); }
    QPolygonF poseBoxForTest() const;   // widget px; empty when there is none
    QPointF posePivotForTest() const;   // widget px
    QVector<QPolygonF> poseGhostsForTest() const; // each faint copy's box, widget px
    QPolygonF posePathForTest() const;  // the dotted path, widget px
    QString poseChipForTest() const { return poseChipText(); }
    bool poseDraggingForTest() const { return m_poseDrag != PoseNone; }

signals:
    // The words the status bar's note says changed (batch I-1b round 1): noteBarText,
    // noteBarKind, noteBarTip.
    void noteBarChanged();
    // LAYER MOTION (batch I-1): the Move box let go on an animated layer - the pose it
    // made at the playhead, and the pivot it turned and sized about (stage px).
    void layerPoseEdited(int layerIndex, const shotcam::LayerPose &pose, const QPointF &pivot);
    // The eraser preset changed (library selection): the Size CTL bar
    // mirrors size/opacity the way paintBrushChanged mirrors the brush.
    void eraserBrushChanged();
    void contentChanged();
    void layersChanged(); // layer added/removed by the canvas (image import)
    // The eyedropper (batch D): a pick landed (the page gives the colour to the brush, the
    // swatch and the Colors panel); the one-pick mode went on or off.
    void colorPicked(const QColor &color);
    void colorPickArmedChanged(bool armed);
    // Ctrl+Left click: the topmost visible layer with opaque pixels under
    // the cursor — the Layers panel selects/highlights that row.
    void layerPickRequested(const QString &layerId);
    void viewZoomChanged(double zoom); // wheel/pan zoom -> sync the zoom slider
    void viewRotationChanged(double degrees); // R-drag, Shift+R (batch C) -> the zoom bar's dial
    // Committing a Warp resets the box to the default move/scale/rotate mode;
    // the Move Modifier toolbar listens and unchecks its mode buttons.
    void xformUiModeReset();
    // An undo/redo command rewrote this panel's layer data; the page
    // regenerates that panel's thumbnail from the committed model.
    void panelEdited(Panel *panel);
    // An undo or redo is about to change a drawing of a run that the layer does not show
    // (batch I-2): the page puts the playhead on it, so the step lands where it is seen.
    void drawingNeededForUndo(Panel *panel, const QString &layerId, const QString &drawingId);
    // The active tool switched (the per-tool Size CTL sliders re-sync).
    void toolChanged(int tool);
    // A library preset replaced the working brush (sliders re-sync).
    void paintBrushChanged();
    // A slider/toggle field-edited the working brush (dirty-state tracking;
    // colour is deliberately NOT included — it is app-global, not brush
    // identity, and must never mark a preset dirty).
    void paintBrushEdited();
    // A LIVE brush stroke began (pointer press with the Brush tool on the
    // canvas — mouse or tablet). Emitted only from the live-input sites,
    // never from the QuickShape replay, so panning, zooming, other tools
    // and programmatic strokes can never trigger listeners. The Brush
    // Library's draw-to-dismiss auto-hide hangs off this.
    void liveBrushStrokeStarted();
    // A perspective VP was created, selected, or removed: the Perspective
    // Modifier toolbar re-syncs its per-VP sliders.
    void perspectiveEdited();
    // SYMMETRY (batch H): the switch changed (whoever changed it); the axis was moved by
    // its grip - a drag let go, or the double-click that centres it.
    void symmetryChanged(bool on);
    void symmetryAxisEdited();
    // A Camera-tool drag was released with the frame changed: the camera
    // the artist made (kind: ShotCameraEditKind). The workspace records it.
    void shotCameraEditRequested(const shotcam::CameraState &state, int kind);
    // A Camera-tool press the view says is not editable (Animate off with
    // two or more keys): nothing moves; the workspace says why.
    void shotCameraEditRefused();
    // A move or a paste landed past the 8,192 px a layer may hold (batch B1):
    // the part past it is not kept; the workspace says so at the pointer.
    void keptArtLimitReached();
    // A press that does nothing because the active layer is locked or hidden (batch G): why
    // ("Layer 2 is locked"), and where (global): the workspace shows it by the pointer.
    void editRefused(const QString &why, const QPointF &globalPos);
    // Stage mode (batch 6c): a handle moved the proposed margins; Enter or Esc on
    // the canvas; the tool or the panel changed under the session.
    void stageModeEdited(const QMargins &margins, int activeSide);
    void stageModeFinished(bool apply);
    void stageModeAbandoned();

protected:
    bool eventFilter(QObject *object, QEvent *event) override; // toolbar grip drag
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override; // closes a polygon
    void tabletEvent(QTabletEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool event(QEvent *event) override; // UngrabMouse -> commit the stroke
    void focusOutEvent(QFocusEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override; // grid menu

private:
    // --- Workspace grid internals -----------------------------------------
    void loadGridSettings();            // ctor: versioned QSettings keys (v1)
    void saveGridSettings() const;
    void ensureGridTile();              // rebuild the tiled pattern on change
    bool viewInteractionActive() const; // stroke/drag/transform in progress
    QMenu *buildGridMenu();             // the right-click context menu
    bool m_gridVisible = false;
    GridStyle m_gridStyle = GridStyle::Square;
    QColor m_gridColor{0x2a, 0x2a, 0x2a};
    QColor m_gutterColor{0x0a, 0x0a, 0x0a};
    QColor m_paperColor{Qt::white};     // the stage background on the canvas (batch E)
    std::function<QColor(const QColor &)> m_paperPickForTest; // Custom...'s picker, in the gate
    ColorPicker m_colorPicker; // the SankoTV picker (COLOUR-1), from the page
    QColor pickColor(const QColor &initial, const QString &title)
    {
        return m_colorPicker ? m_colorPicker(initial, title) : QColor();
    }
    // The paper's colour over what is drawn so far in `rect` (the white Background layer and
    // the paper under it): Multiply, so white becomes the colour. Nothing when it is white.
    void tintPaper(QPainter &p, const QRectF &rect) const;
    QPixmap m_gridTile;                 // kGridSpacing^2 pattern, style+colour
    bool m_gridTileDirty = true;

    QRect displayRect() const;          // axis-aligned zoom+pan rect (pre-rotation)
    bool onPaper(const QPointF &widgetPos) const; // on the paper AS SHOWN, the view turned or not
    double scale() const;               // display px per canvas px (includes zoom)
    QTransform viewTransform() const;   // canvas -> widget (zoom+pan+rotate+flip)
    QPoint toCanvas(const QPoint &widgetPoint) const;
    void setZoom(double zoom, const QPointF &anchorScreen); // keeps anchor point fixed
    void resetView();                   // 100%, recentred
    int penWidth() const;               // brush size mapped into canvas space
    Layer *editableActiveLayer() const; // active layer if it accepts strokes, else nullptr
    // App-wide undo: snapshot the active layer before a mutating operation,
    // then diff and push ONE region-limited DrawingCommand when it finishes.
    void beginLayerEdit();
    void finalizeLayerEdit(const QString &text,
                           const QImage &beforeOverride = QImage());
    void drawSegment(const QPoint &from, const QPoint &to, const QColor &color);
    QString paintLayerKey(Panel *panel, const QString &layerId) const;
    void syncPaintBrushSettings();
    void pushPaintStroke(const SankoPaintHostAdapter::StrokeResult &result,
                         Panel *panel, const QString &layerId,
                         const QString &undoText);
    void enforcePaintUndoPolicy();
    void completePaintStroke(SankoPaintHostAdapter::StrokeResult result,
                             Panel *panel, const QString &layerId,
                             const QString &undoText);
    void floodFill(const QPoint &seed);

    // Shapes tool: preview geometry lives in canvas coords until committed.
    void paintShapeGeometry(QPainter &painter, bool closePolygon) const;
    void commitDragShape(); // rectangle/triangle/circle/line, on release
    void commitPolygon();   // on double-click/Enter; needs >= 3 vertices
    void cancelShape();     // clears drag + polygon state, no artifacts
    void closePolygonSelection(); // SelectPoly: vertices -> selection mask
    QPainterPath combinedSelection(const QPainterPath &shape) const; // Replace/Add/Sub

    // Selection rasterisation. The selection itself is VECTOR geometry
    // (m_selectionPath); a grayscale ANTIALIASED coverage mask is generated
    // from it only when a pixel operation needs one (brush, erase, fill,
    // lift, clear, copy/cut). cachedSelectionMask() is the full-canvas mask,
    // rebuilt lazily ONLY when the path has changed — a stroke of hundreds of
    // dabs rasterises the path exactly once; selectionMask() crops it.
    // DestinationIn with the mask KEEPS exactly the masked coverage and
    // DestinationOut CLEARS exactly the complement — alpha + (1-alpha)
    // partitions the pixels exactly, so cut/lift edges stay clean.
    const QImage &cachedSelectionMask() const;
    QImage selectionMask(const QRect &boundingRect) const;
    // Clamp used only to place a fresh PASTE fully on-canvas (positioning,
    // not clipping).
    QPointF clampFloatDelta(const QPointF &delta) const;
    // Move tool, deferred-commit model: the layer is written exactly ONCE,
    // on mouse up. beginMoveDrag copies the masked pixels + snapshots the
    // whole layer; commitMoveDrag rebuilds from that snapshot (clear source,
    // paste at the final offset) and pushes ONE undo entry.
    void beginMoveDrag(const QPointF &grabCanvasPt);
    void commitMoveDrag();
    void commitFloating();      // paste-floating commit (click-away/Enter)
    void cancelFloatingPaste(); // Esc on a floating paste: discard, no artifacts
    QRectF floatBounds() const; // floating image bounds in canvas coords
    void updateAntsTimer();     // marching ants animate only while needed

    // Stamp-based brush stroke pipeline (mouse pressure = 1.0; tablet =
    // real). `seed` pins the engine's per-stroke randomness (scatter,
    // jitter, colour dynamics): 0 = random (live strokes), nonzero =
    // deterministic — the QuickShape replay passes the captured seed.
    QPointF toCanvasF(const QPointF &widgetPoint) const; // float, unclamped
    // rasterizePreview: live strokes keep the engine's per-move preview
    // tiles (they ARE the on-screen stroke until publish); the QuickShape
    // bake opts out when the already-rendered QS preview can stand in as
    // the flight placeholder instead — the tiles were 97% of the bake's
    // synchronous cost at 4K (372 of 388 ms, measured) for pixels shown
    // only milliseconds.
    void beginBrushStroke(const QPointF &canvasPt, qreal pressure,
                          qreal tiltX = 0.0, qreal tiltY = 0.0,
                          qreal rotation = 0.0, quint64 timestamp = 0,
                          quint64 seed = 0, bool rasterizePreview = true);
    void moveBrushStroke(const QPointF &canvasPt, qreal pressure,
                         qreal tiltX = 0.0, qreal tiltY = 0.0,
                         qreal rotation = 0.0, quint64 timestamp = 0);
    void endBrushStroke(const QString &undoText = QStringLiteral("Brush Stroke"));
    void restorePaintBrushAfterStroke();
    // Canvas-edge fix: shared erase finalize + interruption commit (focus
    // loss / broken grab end a stroke exactly like a release).
    void finishInterruptedStroke();
    // Repaint only the widget region covering the given canvas-space bounds.
    void updateBrushRegion(const QRectF &canvasBounds);

    Panel *m_panel = nullptr;
    Tool m_tool = Brush;
    QColor m_color = Qt::black;
    int m_brushSize = 4;


    // Shapes tool state. Preview-only until committed to the active layer.
    ShapeKind m_shapeKind = ShapeRectangle;
    int m_shapeStroke = 4;         // stroke width, canvas px (1..100)
    bool m_shapeFill = false;      // OFF = outline only
    bool m_shapeDrag = false;      // drag-defined shape in progress
    QPointF m_shapeStartC;         // canvas coords
    QPointF m_shapeCurrentC;       // canvas coords (drag end / polygon rubber)
    QVector<QPointF> m_polygonPts; // in-progress polygon vertices, canvas coords

    // Selection state: resolution-independent VECTOR geometry on the ACTIVE
    // layer (canvas coords). Rasterised lazily via cachedSelectionMask().
    QPainterPath m_selectionPath;        // empty = no selection
    mutable QImage m_selMaskCache;       // AA coverage, canvas-sized, lazy
    mutable QPainterPath m_selMaskPath;  // the path the cache was built from
    QPainterPath m_selBase;        // selection before the current Add/Sub drag
    SelectionOp m_selOp = SelReplace;
    bool m_selDrag = false;        // dragging out a new selection
    QPointF m_selStartC;
    QPointF m_selCurrentC;
    // Selection Modifier "Move" (outline only, never pixels).
    bool m_selOutlineMove = false; // the toolbar's Move mode is on
    bool m_selOutlineDrag = false; // dragging the outline right now
    QPointF m_selOutlineStartC;    // canvas point where the drag grabbed
    QPainterPath m_selOutlineBase; // m_selectionPath at drag start (translated live)
    // Selection changes push SelectionCommands onto the shared stack.
    void recordSelectionChange(const QPainterPath &before);
    QPainterPath m_selGestureBase; // selection at gesture start (drag/polygon)
    // Shared app-wide undo stack (owned by MainWindow) + the in-flight layer
    // edit snapshot captured by beginLayerEdit().
    QUndoStack *m_undoStack = nullptr;
    Panel *m_editPanel = nullptr;
    QString m_editLayerId;
    QImage m_editBefore;
    QVector<QPointF> m_lassoPts;   // freehand outline while dragging
    QTimer *m_antsTimer = nullptr; // marching-ants animation
    int m_antsPhase = 0;

    // Floating pixels: a move drag (commits on release) or an un-committed
    // paste (commits on click-away/Enter). Display-only until committed.
    bool m_floatActive = false;
    bool m_floatFromPaste = false; // paste floats until click-away; move doesn't
    // The eyedropper (batch D): see armColorPick.
    bool m_pickArmed = false;      // one pick waiting (the Colors panel's button, I)
    bool m_altHeld = false;        // Alt down (with the Brush: the eyedropper)
    bool m_picking = false;        // a pick press under way: the ring shows
    bool m_pickOnStage = false;    // the ring's point is on the stage (a colour to take)
    QPointF m_pickPos;             // the ring's centre, widget coords
    QColor m_pickColor;            // under the pointer (the ring's top half)
    QColor m_pickBefore;           // the colour you had (its bottom half)
    bool m_pickCursorOn = false;   // the dropper cursor is up
    bool m_pickTrackingWas = false; // mouse tracking before the dropper cursor took it
    bool pickWanted(Qt::KeyboardModifiers modifiers) const
    {
        return m_pickArmed || (m_tool == Brush && (modifiers & Qt::AltModifier));
    }
    void beginColorPick(const QPointF &widgetPos);
    void moveColorPick(const QPointF &widgetPos);
    void endColorPick();
    void refreshPickCursor();
    void paintSamplingRing(QPainter &p) const;
    bool m_xformFlipH = false;     // Flip H / V applied to the open box (parity)
    bool m_xformFlipV = false;
    bool m_floatDragging = false;
    QImage m_floatImg;             // complete masked pixels (never cropped)
    QPointF m_floatPos;            // top-left, canvas coords
    QPointF m_floatDelta;          // drag offset since lift/paste
    QPointF m_floatGrabC;          // canvas point where the drag grabbed
    QPointF m_floatGrabDelta;      // m_floatDelta at grab time

    // Move tool deferred-commit state. During the drag the layer is NEVER
    // written; mouse up rebuilds it once from m_layerBackup.
    bool m_moveActive = false;     // between Move mouse-down and mouse-up
    QImage m_layerBackup;          // pristine full copy of the layer at move start
    QImage m_moveMask;             // selection mask, bounding-rect-local
    QRect m_moveSrcRect;           // selection bounding rect at move start

    // Non-destructive transform box (Move tool). Activating Move with a
    // selection lifts the masked pixels into m_transformBuf (the PRISTINE
    // source, never re-transformed) and shows a bounding box; every edit
    // re-renders the preview from m_transformBuf through a fresh QTransform,
    // so there is no cumulative resampling. The layer is written exactly once
    // on commit (Enter); Esc restores m_layerBackup.
    //
    // The box is a free QUAD (corners TL,TR,BR,BL in canvas coords) mapped
    // from the source rect with QTransform::quadToQuad — affine for
    // move/scale/rotate/skew, projective for distort/perspective. m_pivot is
    // the rotation/scale origin (Pivot Point mode relocates it; until then it
    // tracks the quad centre).
    enum XformMode { XNone, XMove, XRotate, XPivot, XWarpPt, XWarpBox,
                     XScaleTL, XScaleTR, XScaleBL, XScaleBR,
                     XScaleT, XScaleB, XScaleL, XScaleR };
    bool m_xformActive = false;
    bool m_xformAutoSel = false;   // box selection was synthesized from the
                                   // layer's artwork bounds (no user selection)
    QImage m_transformBuf;
    // Transform-session identity: the lifted layers' IDs + one buffer each
    // (single-layer sessions hold one entry; group sessions one per member).
    // Commit resolves by ID, so reordering/grouping mid-session can never
    // paste the result into the wrong layer.
    QStringList m_xformLayerIds;
    QVector<QImage> m_xformBufs;         // pristine lifted pixels (m_moveSrcRect-sized)
    // Each lifted layer's image with the source region already subtracted,
    // built ONCE at lift (the session never mutates layer pixels) — the
    // paintEvent used to make this full-frame copy per layer per FRAME.
    QVector<QImage> m_xformHoles;
    // Layers-panel row multi-selection (ids), mirrored via
    // setSelectedLayerIds: >1 selected rows lift together under one box.
    QStringList m_selectedLayerIds;
    QPolygonF m_quad;              // current box corners TL,TR,BR,BL, canvas coords
    QPointF m_pivot;               // rotate/scale origin (Pivot Point mode)
    bool m_pivotCustom = false;    // user moved the pivot off the box centre
    XformUiMode m_xformUiMode = XformDefault;
    XformMode m_xformMode = XNone; // active handle while dragging
    QPointF m_dragStartCanvas;     // pointer at press, canvas coords
    QPolygonF m_quad0;             // quad snapshot at press (drags recompute from it)
    QPointF m_pivot0;              // pivot snapshot at press
    qreal m_rotStart0 = 0.0;       // pointer angle at a rotate press, radians
    QCursor m_rotateCursors[4];    // curved-arrow cursors, oriented per corner
                                   // (TL, TR, BR, BL — each bends around its
                                   // own corner like Photoshop's)
    mutable int m_rotateCorner = 0; // corner whose rotate zone is hovered
    QPointF pivotPoint() const;    // m_pivot when custom, else the quad centre

    // Warp mode: an IRREGULAR mesh of draggable control points that smoothly
    // deform the buffer. Each point pairs a fixed SOURCE position (buffer
    // coords) with a draggable DESTINATION (canvas coords). The deformation
    // is a THIN-PLATE SPLINE interpolating the control points — globally
    // smooth (C1+), so clean line art stays smooth with no polygon faceting.
    // Rendering samples the TPS over a dense subdivided grid (fine cells:
    // sub-pixel deviation) drawn as seam-free micro-triangles. Seeded as a
    // kWarpGrid lattice; Ctrl+click adds/removes points (the 4 source-corner
    // anchors are protected). Quad edits in the other modes carry every
    // destination along through the incremental quad transform. All guides
    // are display-only overlays and never bake into the layer.
    struct WarpPt {
        QPointF src; // buffer coords, fixed after creation
        QPointF dst; // canvas coords, dragged by the user
    };
    static constexpr int kWarpGrid = 4;
    QVector<WarpPt> m_warp;        // control points
    QVector<WarpPt> m_warp0;       // snapshot at press
    QSet<int> m_warpSel;           // selected control points (multi-select)
    bool m_warpDirty = false;      // a control point was moved: render the mesh
    mutable int m_warpIdx = -1;    // control point under the cursor / dragged
    int m_warpHoverIdx = -1;       // hovered point (drawn slightly enlarged)
    bool m_warpMarquee = false;    // rubber-band point selection in progress
    QPointF m_marqueeStartW;       // marquee corners, WIDGET coords
    QPointF m_marqueeEndW;
    // Thin-plate-spline solve of src -> dst (weights per dimension, laid out
    // [w0..wN-1, a0, ax, ay]); recomputed on every mesh change.
    QVector<qreal> m_tpsX, m_tpsY;
    bool m_tpsValid = false;
    void solveWarpTps();                          // fit the spline to m_warp
    QPointF warpMap(const QPointF &src) const;    // evaluate the spline
    int warpPointAt(const QPointF &widgetPos) const;
    void addWarpPointAt(const QPointF &widgetPos);   // Ctrl+click on the mesh
    void removeWarpPoint(int index);                 // Ctrl+click on a point
    // Piecewise render of the TPS at the given source-space cell size (finer
    // for the commit than the interactive preview). overrideBuf: warp THAT
    // pristine buffer instead of m_transformBuf (per-layer sessions render
    // and commit each member's own pixels through the same mesh).
    void paintWarpedBuffer(QPainter &p, qreal cellPx,
                           const QImage &overrideBuf = QImage()) const;
    qreal luminanceBehind(const QPointF &canvasPt) const; // pivot contrast

    // Below/above composite caches: all layers UNDER the active one (over
    // the white paper) and all layers ABOVE it, each flattened once. A
    // normal repaint is then paper+below, the live active layer, above —
    // instead of re-drawing the whole stack every frame. Rebuilt lazily when
    // the keys change or after invalidateComposite().
    void ensureComposite();
    QImage m_compBelow;
    QImage m_compAbove;
    bool m_compValid = false;
    Panel *m_compPanel = nullptr;
    int m_compActive = -1;
    int m_compCount = -1;
    QRect m_lastCompositeInvalidatedRect;

    void liftDefaultTransformBox(); // Move tool: box around selection/artwork
    void beginTransform();         // lift selection -> box (source cleared on lift)
    // Lift EVERY visible, unlocked member of a group folder into per-layer
    // buffers behind ONE box: the whole group moves/scales/rotates as a
    // unit and commits back into each member's own image.
    void beginGroupTransform(Layer *group);
    // Shared lift core (group members OR multi-selected rows): every
    // candidate layer that is visible, unlocked and has artwork lifts into
    // its own pristine buffer behind ONE box; stacking order is untouched —
    // each buffer previews and commits at its layer's own z-position.
    void beginLayersTransform(const QStringList &candidateIds);
    // Bake once (one undo). relift: Photoshop-style — while the Move tool
    // stays active the box does not disappear; it RESETS to a fresh default
    // axis-aligned box around the committed artwork. setTool passes false
    // when the user is leaving Move.
    void commitTransform(bool relift = true);
    // Restore m_layerBackup exactly. relift (Esc): keep a fresh default box
    // up while the Move tool remains active.
    void cancelTransform(bool relift = false);
    // KEPT ART (batch B1, 2026-11-11; Figma B-1). What a move, scale, rotate, warp
    // or paste lands past the stage is kept in the layer's tiles - batch 6c's store
    // of art outside the stage - up to 8,192 px across and down, and the Move tool
    // lifts it with the rest. Not for a Background or a folder. Canvas px throughout.
    bool keepsArt(const Layer &layer) const;
    QRect layerArtBounds(const Layer &layer) const;                 // kept art included
    QImage layerArtImage(const Layer &layer, const QRect &rect) const;
    QRect keptArtWindow(const QRect &wanted) const;  // what of `wanted` a layer may hold
    struct KeptLanding
    {
        QImage image;                 // the new stage image
        layertiles::TilesPtr tiles;   // the new tiles, kept art included
        bool limited = false;         // part of `reach` was past the 8,192 px
    };
    // The layer's art over the stage and `reach`, `paint` drawn on it in canvas px.
    // `lifted`: where the art was lifted from; what of it lies past the 8,192 px
    // window `paint` could reach is emptied in the tiles too, so it is not left behind.
    KeptLanding landKeptArt(const Layer &layer, const QRect &reach,
                            const std::function<void(QPainter &)> &paint,
                            const QRect &lifted = QRect()) const;
    QRect landedBounds(const QImage &buffer) const; // where a lifted buffer lands now
    // Draws `draw` (canvas px) at full strength inside the stage and faint outside
    // it, where only kept art can be: what the Move tool shows of it (Q-B2).
    void paintStageAndKept(QPainter &painter, const QRectF &bounds,
                           const std::function<void()> &draw) const;
    QTransform boxTransform() const;                 // buffer -> canvas placement
    QVector<QPointF> boxHandlesCanvas() const;       // 8 handles, canvas coords
    XformMode hitTestBox(const QPointF &widgetPos) const;
    void applyXformDrag(const QPointF &canvasPos, bool proportional);
    void updateXformCursor(XformMode mode);
    // Idle cursor for the active tool where the pointer is (batch G): see toolCursorKind.
    // Before batch G: four-way move arrows for Move, the crosshair for everything else.
    QCursor toolCursor() const;
    QString toolCursorKind() const;
    // True while nothing else owns the pointer: no pick, no Space or R, no pan or turn, no
    // Move box (its handles name themselves), not the Camera or Perspective tool (their
    // hover does), not Edit Shape.
    bool toolCursorInCharge() const;
    // "Layer 2 is locked" / "... is hidden" / "... is in a hidden folder" - or empty: the
    // active layer takes edits, or it is not a lock or an eye that stops it (a folder, none).
    QString layerRefusal() const;
    // A press at this widget point would reach the editable-layer gate and stop there.
    bool pressRefusedAt(const QPointF &widgetPos) const;
    // The circle: its size on screen (the Size bar's number x the zoom) and whether it shows.
    double pointerOutlineDiameter() const;
    bool pointerOutlineWanted() const;
    // Where the pointer is (canvas coordinates), from the events that say it.
    bool pointerEventCounts(const QEvent *event) const;
    void notePointerAt(const QPointF &widgetPos);
    void notePointerAway();
    void syncPointerOutline();
    // While the pen is over the canvas, its cursor on the window too (see the .cpp).
    void syncWindowCursor();
    // Says why a press at widgetPos did nothing (editRefused), when it is a lock or an eye.
    void sayEditRefused(const QPointF &widgetPos);
    // The tools whose hover needs buttonless moves (mouse tracking) for as
    // long as they are active: Camera and Perspective.
    bool toolTracksHover() const { return m_tool == Camera || m_tool == Perspective; }

    // Visual debug: dump the move-pipeline stages to C:\SankoTv\app\debug\.
    // Off in shipped builds; flip to true to re-inspect the move stages.
    bool m_debugMove = false;
    void dumpMoveDebug(const QString &name, const QImage &img, bool checker) const;

    // Canvas clipboard (Edit menu Copy/Cut/Paste on the selection).
    QImage m_clipImg;              // copied pixels, tight bounding rect
    QPointF m_clipPos;             // where they came from, in frame space (batch 6c)

    // Stroke-level selection masking: while a selection is active, the live
    // brush/eraser stroke accumulates UNMASKED in a scratch buffer and the
    // cached antialiased mask is applied ONCE (composited each repaint for
    // the preview, baked into the layer on release). The mask is therefore a
    // CEILING the stroke can never exceed — overlapping dabs cannot saturate
    // the soft boundary back to a hard edge (Photoshop semantics).
    enum StrokeMaskMode { StrokeMaskNone, StrokeMaskPaint, StrokeMaskErase };
    StrokeMaskMode m_strokeMask = StrokeMaskNone;
    QImage m_strokeBuf; // canvas-sized scratch holding the live stroke

    // Sanko Paint owns the lazily allocated 256x256 tiled backing for each
    // paintable layer. Layer::image remains SankoTV's coherent CPU mirror for
    // thumbnails, composites and save/load.
    SankoPaintHostAdapter m_paintEngine;
    std::unique_ptr<QThreadPool> m_paintGpuPool;
    bool m_paintCommitPending = false;
    GpuStampRenderer::Result m_lastPaintRenderer;
    // Frozen copy of the live stroke preview, drawn from release until the
    // async publish lands in layer.image — without it the stroke vanished
    // for the render's flight time after every release.
    QImage m_pendingPreview; // canvas-space region, premultiplied
    QRect m_pendingPreviewRect;

    // App-level input stabilization (see setStrokeStabilization).
    QPointF stabilizeStrokePoint(const QPointF &raw, bool strokeBegin);
    double m_strokeStabilization = 0.0; // 0 = filter fully bypassed
    QPointF m_stabPoint;                // EMA state, reset at stroke begin

    // CONTEXTUAL CURSORS (batch G): where the pointer is, the Size bar's numbers, the
    // circle's widget (the canvas's sibling; see PointerOutline), the gate's pointer.
    QPointF m_pointerPos;            // canvas coordinates
    bool m_pointerIn = false;        // over the canvas itself (not a child, not elsewhere)
    int m_shownBrushSize = 0;        // the Size bar's number (visible width); 0 = engine size
    int m_shownEraserSize = 0;
    QPointer<QWidget> m_pointerOutline;
    bool m_realPointerIgnoredForTest = false;
    qint64 m_pointerDeviceForTest = -1;

    // SYMMETRY (batch H; see the public section).
    bool m_symmetry = false;
    bool m_symmetryAxisSet = false; // moved by its grip (else: the middle of the frame)
    int m_symmetryAxis = 0;         // px from the frame's left edge, when moved
    bool m_symmetryDrag = false;    // the grip is being dragged
    int m_symmetryDragFrom = 0;     // the axis (frame px) when the drag began
    bool m_symmetryDragWasSet = false;
    int m_strokeMirrorAxis = -1;    // canvas px: the mirror of the stroke under way; -1 none
    int m_qsMirrorAxis = -1;        // ...and of the shape held out of a stroke, until baked
    bool m_qsBaking = false;        // replayQuickShape is feeding the bake's stroke
    QLineF symmetryAxisLine() const;  // widget px: from the grip to past the paper's foot
    QPointF symmetryGrip() const;     // widget px: the grip's centre
    bool symmetryGripAt(const QPointF &widgetPos) const;
    void paintSymmetry(QPainter &painter) const;
    void updateSymmetryOverlay();     // repaint where the axis and the grip are
    QRect mirroredRect(const QRect &canvasRect, int axis) const
    {
        return QRect(2 * axis - (canvasRect.left() + canvasRect.width()), canvasRect.top(),
                     canvasRect.width(), canvasRect.height());
    }

    // Brush engine state. Defaults mirror the initial settings-panel values.
    int m_brushToolSize = 25;        // dab diameter, canvas px
    // E6 (2026-09-28, user-approved): the Size CTL scales a DUAL preset's
    // secondary WITH the primary, at the preset's own ratio. Captured at
    // setPaintBrush; at the preset's own size the secondary is exactly the
    // preset's (integer arithmetic), so every dual preset stays byte-
    // identical until the slider actually moves. 0 = the active preset is
    // not dual. Before this the slider moved the primary only, so Dry
    // Brush's 20 px mask carved an 11% band under a 286 px primary (the
    // preview had scaled both all along: BrushPreviewRenderer).
    int m_dualPresetPrimarySize = 0;
    int m_dualPresetSecondarySize = 0;
    // IDENTITY COLOUR, design (b) (2026-08-30, user-approved): a preset
    // with a non-black stored colour applies it WHILE ACTIVE; switching
    // to a black-ink preset restores the colour the user had before the
    // adoption; an explicit setColor() while adopted WINS AND PERSISTS
    // (the restore state drops there). Captured only on the
    // user->identity transition, so identity->identity switches keep the
    // ORIGINAL user colour. Never persisted; untouched by project load,
    // tool switches, undo, and the QuickShape bake swap (which
    // round-trips m_colour exactly) - the staleness audit is in HANDOFF.
    QColor m_colorBeforeIdentity;
    bool m_identityColorActive = false;

    // MULTIPLIER semantics (2026-08-29, user-approved): the Size CTL bar's
    // opacity is "how much of the preset's own ceiling", not an absolute.
    // engine opacity = base x multiplier; at multiplier 1.0 the product IS
    // the preset, byte for byte - which is what makes the migration safe
    // and every byte-identity gate check hold. The base is captured at
    // preset activation (setPaintBrush / setEraserPreset) and NOWHERE
    // else; the only writers of engine opacity are those two sites plus
    // setBrushOpacity/setEraserOpacity, all of which recompute the
    // product, so the pair cannot drift apart.
    double m_brushToolOpacity = 1.0;  // 0..1 bar multiplier
    double m_brushBaseOpacity = 1.0;  // the active preset's opacity
    // Eraser state (independent of the brush; per-tool Size CTL sliders).
    int m_eraserSize = 25;           // stroke width, canvas px
    double m_eraserOpacity = 1.0;     // 0..1 bar multiplier (see above)
    double m_eraserBaseOpacity = 1.0; // the active eraser preset's opacity
    // The Eraser IS an engine brush now (the erase composite pass): a
    // hard-round eraseMode Brush whose size/opacity the Size CTL drives.
    // During an eraser stroke it is swapped into the adapter and the
    // brush-tool Brush is saved and restored - the engine has ONE brush
    // slot and the studio/library own its usual occupant.
    ::Brush m_eraserBrush;
    ::Brush m_savedPaintBrush;       // brush-tool brush across the swap
    bool m_eraserStrokeSwap = false; // a swap is outstanding
    bool m_pendingPreviewErase = false; // frozen bridge shows REMOVAL
    double m_brushHardness = 0.8;    // 0..1
    bool m_pressureToSize = true;
    bool m_pressureToOpacity = false;
    bool m_brushStroke = false;      // brush stroke in progress
    QPointF m_lastBrushPt;           // canvas coords, float
    qreal m_lastBrushPressure = 1.0;

    bool m_onionSkin = false;
    QVector<OnionGhost> m_drawingGhosts; // the run's drawings before and after (batch I-2)
    QRect m_ltPrevRect;
    QRect m_ltNextRect;

    // Light table: the previous panel's ghost (red) and the next's (green) by default, drawn
    // on the paper behind the current drawing layers (display only; the switch is a setting
    // on this computer since OS-1).
    bool m_lightTable = false;
    QPixmap m_ltPrevGhost;   // previous panel, in the Light Table's "before" colour
    QPixmap m_ltNextGhost;   // next panel, in its "after" colour
    int m_ltBuilds = 0;      // (the checks': ghosts built from a flatten)
    void drawLightTable(QPainter &painter, const QRect &d) const;

    // Viewport: zoom + pan.
    // Startup zoom: 85% of the letterbox fit — THE one place the startup
    // camera is set. m_zoom is FIT-RELATIVE (scale = fit * m_zoom via
    // displayRect()), so this is geometry-independent: no layout pass,
    // resize, or show can recompute it, and nothing programmatic writes the
    // camera at startup (fitToScreen runs only from the user's Fit button).
    // Zoom is NOT persisted — every launch starts here; the ZoomToolbar
    // reads viewZoom() at construction so its readout shows 85%.
    static constexpr double kStartupZoom = 0.85;
    double m_zoom = kStartupZoom; // 0.25 .. 4.0
    QSize m_stageMargin = QSize(0, 0); // Phase 2A: px each side; none
    QPointF m_panOffset;         // screen-px offset from the centred position
    bool m_spaceHeld = false;    // spacebar pan modifier
    bool m_panning = false;      // mid-drag (space+left or middle button)
    QPoint m_panStartScreen;
    QPointF m_panStartOffset;
    bool m_rotateHeld = false;   // R down: a drag turns the view (batch C)
    bool m_rotating = false;     // mid-drag with R
    double m_viewRotStartAngle = 0.0; // the press's angle around the drawing's centre
    double m_viewRotStartView = 0.0;  // the view's rotation at the press
    double viewAngleAt(const QPointF &widgetPos) const;
    QPointF rotationIndicatorCentre() const;
    void paintRotationIndicator(QPainter &p, const QPointF &centre, double degrees, bool arcs,
                                bool number, const QString &text, int shadow) const;
    void paintRotationArcs(QImage &layer, const QPointF &centre, double degrees, int part) const;
    void paintRotationNumber(QPainter &p, const QPointF &centre, double degrees, const QString &text) const;
    mutable class QSvgRenderer *m_rotationArcs = nullptr;        // the fills; made on first use
    mutable class QSvgRenderer *m_rotationArcsOutline = nullptr; // the outline's stroke; the same
    bool m_moveArrowsShown = false; // View > Camera Move Arrows, off by default
    // View > Drawing Overlays, in DrawingOverlay's order (drawingOverlayDefault says the same).
    bool m_drawingOverlays[DrawingOverlayCount] = {false, false, true, true, true};

    // Canvas VIEW transforms (display only — the layer pixels, flattenedPixmap,
    // save, and export are never rotated/flipped/zoomed). Applied as one
    // QTransform in paintEvent; mouse coords invert it (see viewTransform()).
    double m_viewRotation = 0.0; // degrees, -180..180
    bool m_viewFlipH = false;    // horizontal flip
    // Canvas View Controls toolbar (grip, zoom slider, flip, rotate slider,
    // reset rotation), floating over the canvas.
    QWidget *m_viewToolbar = nullptr;
    QWidget *m_viewGrip = nullptr;
    QSlider *m_zoomSlider = nullptr;
    QSlider *m_rotateSlider = nullptr;
    bool m_syncingViewUi = false; // guards slider<->engine feedback
    bool m_viewDragging = false;  // dragging the toolbar by its grip
    QPoint m_viewDragStart;       // global cursor at grip press
    QPoint m_viewToolbarStart;    // toolbar pos at grip press
    void buildViewToolbar();
    void positionViewToolbar();
    void syncViewToolbar();       // push engine state into the sliders

    // Perspective guides (display-only; geometry in canvas coords).
    PerspectiveTool m_perspective;
    QPoint m_perspShift;    // the keyShift it holds them at (batch 6c)

    // STAGE MODE (batch 6c): the drag is an overlay; nothing changes until Apply.
    bool m_stageMode = false;
    bool m_stageLinked = false;      // every side the same share of the frame
    QMargins m_stageLive;            // the proposed margins, each side
    QMargins m_stageDragStart;
    int m_stageDrag = -1;            // the handle held, -1: none
    int m_stageHover = -1;
    QPointF m_stagePressCanvas;
    QPointF m_stagePointer;
    bool m_stageAlt = false;
    bool m_stageShift = false;
    QVector<layertiles::ArtCells> m_stageArt; // each visible layer's, as of entering
    QPainterPath m_stageArtOutline;  // all of it, frame space
    // A panel switch or a stage change keeps the frame in its place on screen.
    bool m_viewHold = false;
    QPointF m_viewHoldAt;
    double m_viewHoldScale = 1.0;
    shotcam::Stage m_viewHoldStage;
    int m_perspHandle = -1; // VP index being dragged (-1: none)

    // QuickShape session state (recognition math lives in the kit; the canvas
    // owns input feeding, the rough-stroke rollback, overlay painting, and
    // the brush-engine replay).
    quickshape::QuickShapeSession m_quickShape;
    QPainterPath m_quickShapeOverlay; // corrected vector, canvas coords
    bool m_quickShapeEnabled = true;
    bool m_qsHeld = false;            // pointer currently feeding the session
    void applyQuickShapeTiming();     // screen-px tuning -> document units
    void discardRoughStroke();        // roll the pending edit back
    void replayQuickShape(const quickshape::QuickShapeCommit &commit);

    // Brush state captured when the QuickShape stroke begins. The capture is
    // COMPLETE and deterministic: the full engine brush (every preset
    // parameter), the app colour, one nonzero per-stroke randomness seed,
    // and the viewport rotation — plus the session's own recorded pressure/
    // tilt/rotation samples. The commit consumes exactly this capture, so a
    // preset, slider, colour, or camera change while the shape is being
    // edited can never silently alter what Done bakes (captured-state-wins
    // policy; live controls affect the NEXT stroke). The QsBrushState mirror
    // feeds the legacy preview stamper until it is retired.
    struct QsBrushState {
        QColor color;
        int size = 8;
        qreal opacity = 1.0;
        qreal hardness = 1.0;
        bool pressureToSize = false;
        bool pressureToOpacity = false;
    };
    QsBrushState m_qsBrush;
    ::Brush m_qsFullBrush;        // complete engine brush copy at stroke start
    quint64 m_qsSeed = 0;         // nonzero; commit randomness is pinned
    qreal m_qsViewRotation = 0.0; // viewport rotation at stroke start
    void captureQuickShapeBrush();
    // One replay point stream shared by every consumer of the commit:
    // corrected points + resampled pressure/tilt/rotation channels, the
    // captured viewport rotation, and stable synthetic timestamps (1..n).
    QVector<StrokePoint> quickShapePointStream(
        const quickshape::QuickShapeCommit &commit) const;
    // Real-brush preview of the corrected path: the corrected point stream
    // runs through the SAME engine pipeline as the commit
    // (beginStroke/appendPoint/finishStrokeWork/render) against a dedicated
    // preview layer key and a transparent canvas-sized host — never the
    // layer, never the layer's engine mirror. The GPU render runs async on
    // the stroke pool; a generation counter drops stale results; every
    // render REPLACES m_qsPreview whole (regenerated from scratch, never
    // stacked). Regeneration is coalesced to ~one schedule per display frame.
    QImage m_qsPreview;

    // --- Tablet -> UI router (repair stage 5) ------------------------------
    // A pen press over a visible canvas-child CONTROL latches that control
    // and ACCEPTS the tablet events for the whole interaction, so Qt never
    // synthesizes the fallback mouse sequence: the 9-widget propagation walk
    // and the rapid-taps-become-mouseDblClick coalescing are gone, one tap
    // is exactly one activation, and no stroke can ever begin beneath a
    // control. Ownership order: (1) latched control, (2) QuickShape node /
    // transform interaction, (3) the drawing pipeline. QPointer: rebuilding
    // or tearing down the type bar can never leave the latch dangling. A
    // DISABLED control is an accepted dead zone. Real mouse input is never
    // touched — suppression covers only the tablet interaction the router
    // owns.
    QPointer<QAbstractButton> m_penUiTarget;
    bool m_penUiDeadZone = false;
    // A press made with a pen's side button (Qt's right or middle button) with
    // the tip up: the pen does nothing until its last button is up (tabletEvent).
    bool m_penSideButtonHeld = false;
    QAbstractButton *penUiButtonAt(const QPointF &widgetPos) const;
    void clearPenUiLatch();

    quint64 m_qsPreviewGen = 0;       // bumped to invalidate in-flight renders
    // MONOTONIC DISPLAY GUARD (drag-blanking fix): the generation of the
    // preview currently on screen. A landed render displays only if its
    // generation is NEWER — so during a continuous manipulation every
    // landed frame shows progressively (the old exact-match check dropped
    // them all: at 4K every ~105 ms flight overlapped another change and
    // the drag went blank), while an older result can never appear after a
    // newer one. Every deliberate clear of m_qsPreview advances this to
    // the current generation, so an in-flight result can never resurrect
    // a cleared display (shape death, bake, degenerate commit).
    quint64 m_qsPreviewShownGen = 0;
    bool m_qsPreviewInFlight = false;
    bool m_qsPreviewDirty = false;    // shape changed while a render was out
    QTimer *m_qsPreviewTimer = nullptr;
    QTimer *m_qsHoldTick = nullptr;   // repaints the hold ring while pen down
    bool m_qsCommitting = false;      // Done reentry guard
    void scheduleQuickShapePreview();
    void renderQuickShapePreview();

    // --- QuickShape Edit Shape mode -----------------------------------------
    // A released temporary shape shows an "Edit Shape" button (top-centre);
    // pressing it exposes draggable structural nodes and a compact shape-type
    // selector. All edit-mode input is fully isolated from the brush.
    bool m_qsEditing = false;
    quickshape::QuickShapeGeometry m_qsGeometry;
    int m_qsNode = -1;   // node being dragged (-1: none)
    int m_qsHover = -1;  // node under the idle cursor
    QPushButton *m_qsEditButton = nullptr;
    QPushButton *m_qsDoneButton = nullptr;
    QWidget *m_qsTypeBar = nullptr;
    QElapsedTimer m_qsTabletClock;    // pen double-tap detection on nodes
    qint64 m_qsLastTabletPressMs = -100000;
    int m_qsLastTabletNode = -1;
    void updateQuickShapeUi();
    void enterQuickShapeEdit();
    void rebuildQuickShapeTypeBar();
    void convertQuickShapeTo(const QString &typeName);
    void convertQuickShapeToPolygon(int sides); // explicit count (3,4,5,6,8)
    void applyQuickShapeGeometry();
    QVector<QPointF> quickShapeNodes() const;      // canvas coords
    int quickShapeNodeAt(const QPointF &widgetPos) const;
    bool quickShapeEditPress(const QPointF &widgetPos);
    void quickShapeEditMove(const QPointF &widgetPos);
    void quickShapeEditRelease();
    bool quickShapeEditDoubleClick(const QPointF &widgetPos);
    int m_perspHover = -1;  // VP handle under the idle cursor (grows on hover)
    // One undo command per completed gesture: snapshot at press, push at
    // release (or at begin/endPerspectiveEdit for toolbar edits).
    QJsonObject m_perspBefore;
    bool m_perspGesture = false;
    QString m_perspGestureText;
    void pushPerspectiveCommand(const QJsonObject &before, const QString &text);

    // Display-only overlays.
    bool m_grid = false;         // alignment grid, 40 canvas px (View > Grid)
    // 16:9 framing + dim outside. OFF by default: its #cccccc outline sits
    // just outside the paper and, over the dimmed gutter, reads as a bright
    // 1px ring hugging the artwork — it was mistaken for a canvas-edge
    // rendering defect across several investigation cycles (see HANDOFF.md).
    // The feature is unchanged and still one toolbar click away.
    bool m_cameraFrame = false;
    bool m_safeArea = false;     // action safe, 5% inset
    bool m_titleSafe = false;    // title safe, 10% inset
    int m_actionSafeMaskPct = 50; // guide opacity, percent (persisted)
    int m_titleSafeMaskPct = 50;
    int m_actionSafeLinePx = 1;   // the guide's line, screen pixels (persisted)
    int m_titleSafeLinePx = 1;

    // The shot camera's tool (Phase 1B; see setShotCameraProvider).
    std::function<ShotCameraView()> m_shotCamProvider;
    ShotCameraView shotCameraView() const;
    // LAYER MOTION (batch I-1): see setMotionProvider.
    enum PoseDrag { PoseNone, PoseMove, PoseScale, PoseRotate };
    std::function<MotionView()> m_motionProvider;
    MotionView motionView() const { return m_motionProvider ? m_motionProvider() : MotionView(); }
    bool poseMode() const;                       // the Move tool on a layer whose Animate is on
    const Layer *poseLayer() const;              // that layer, or null
    shotcam::LayerPose poseNow() const;          // its pose at the playhead (the drag's, live)
    QPointF posePivot() const;                   // stage px: its pivot (its art's centre before a key)
    QRect poseArt() const;                       // its drawn art, stage px (cached by content)
    QPolygonF poseQuad(const shotcam::LayerPose &pose) const; // the art's box posed, stage px
    PoseDrag poseHit(const QPointF &widgetPos) const;
    void paintPoseGhosts(QPainter &painter) const; // canvas space: copies at the other keys, the path
    void paintPoseBox(QPainter &painter) const;    // widget space: the box, the pivot, the chip
    QString poseChipText() const;
    PoseDrag m_poseDrag = PoseNone;
    shotcam::LayerPose m_poseStart, m_poseLive;
    QPointF m_posePressC;  // canvas px
    QPointF m_posePivotC;  // the pivot as posed at the press, canvas px
    mutable qint64 m_poseArtKey = 0;
    mutable QRect m_poseArtRect;
    shotcam::CameraState shotCameraShownState(const ShotCameraView &v) const;
    QPolygonF shotCameraWidgetQuad(const shotcam::CameraState &s) const;
    // The camera guides (CAM-1 / CAM-2): is the frame drawn at all; the base
    // framing's quad unless the frame sits on it; the glyphs to draw and
    // where; a canvas-px travel as a widget-space unit direction.
    bool shotCameraFrameShown(const ShotCameraView &v) const;
    QPolygonF shotCameraBaseQuad(const QPolygonF &frameQuad) const;
    // View > Drawing Overlays (2026-11-21): the dashed border the paint draws - the Camera
    // tool's base framing, or a drawing tool's stage border; a drawing tool's veil over the
    // stage margin, widget px (empty: none).
    QPolygonF shotCameraBorderQuad(const ShotCameraView &v, const QPolygonF &frameQuad) const;
    QPainterPath drawingToolVeil(const ShotCameraView &v) const;
    shotcam::Motion shotCameraGlyphMotion(const ShotCameraView &v, bool *onEnd) const;
    QPointF motionWidgetDir(const QPointF &canvasTravel) const;
    QPainterPath motionGlyphStrokes(const QPolygonF &quad, const shotcam::Motion &motion,
                                    bool big) const;
    void paintMotionGlyphs(QPainter &painter, const QPolygonF &quad,
                           const shotcam::Motion &motion, bool big) const;
    QVector<QPointF> shotCameraHandles(const QPolygonF &quad) const;
    int shotCameraHitTest(const QPointF &widgetPos) const;
    void shotCameraUpdateCursor(int hit);
    shotcam::CameraState shotCameraDragState(const QPointF &widgetPos,
                                             bool shift, int *stoppedBy = nullptr) const;
    bool shotCameraSoft(const ShotCameraView &v,
                        const shotcam::CameraState &s) const;
    QString shotCameraChipText(const ShotCameraView &v,
                               const shotcam::CameraState &s) const;
    // Phase 2B, Q5: a drawing tool, a panel with depth, the camera elsewhere.
    bool shotCameraDrawingViewChip() const;
    QString drawingViewChipText() const;
    // DEPTH WHILE MOVING (batch B2, Figma B-1, the user's Q-B3): with the Move tool's
    // box up on a layer at another depth than 100 %, where that box is in the shot at
    // the playhead (layerToStage), canvas px; false when there is none to show.
    bool depthOutline(QPolygonF *quad, double *depth, QString *name) const;
    mutable bool m_depthOutlinePainted = false; // the last paint drew it (the checks)
    // The note in the status bar (above, batch I-1b round 1).
    void syncNoteBar();       // works out what the bar says; noteBarChanged when it changed
    void queueNoteBarSync();  // ...once, after this event (a paint: whatever repainted may change it)
    void holdNotesForStroke(); // a drawing tool's press: the bar's words stay until...
    void releaseHeldNotes();   // ...its release
    void expireNote();
    QTimer *m_noteTimer = nullptr;
    QString m_noteText;           // the said note up now ("" none, or its time is up)
    bool m_noteWarning = false;   // ...a refusal or a warning: amber
    bool m_notesHeld = false;     // a drawing tool's stroke is under way
    QString m_notePending;        // said during it: shown at its release
    int m_notePendingMs = 0;
    bool m_notePendingWarning = false;
    QString m_noteBarText;        // what the bar says now (syncNoteBar)
    NoteKind m_noteBarKind = NoteKind::Tool;
    QString m_noteBarTip;
    int m_noteBarAmberFrom = -1;  // the Camera tool's "Soft" warning in its note: amber from here
    bool m_noteBarSyncQueued = false;
    QRectF perspectivePromptRect() const; // the Perspective tool's prompt chip (null: none)
    static QFont perspectivePromptFont();
    bool m_scrubbing = false;      // setScrubbing (batch I-1b)
    bool m_liftAfterScrub = false; // the Move tool's box waits for the drag's end
    void shotCameraPress(const QPointF &widgetPos);
    void shotCameraMove(const QPointF &widgetPos, Qt::MouseButtons buttons,
                        Qt::KeyboardModifiers mods);
    void shotCameraFinishDrag(); // release, or an interruption: one request
    void paintShotCameraGuides(QPainter &painter, const shotcam::Stage &stage,
                               const shotcam::CameraState &s);
    void paintShotCamera(QPainter &painter);
    // Stage mode (batch 6c, Figma MARGIN-1).
    void paintStageMode(QPainter &painter, const ShotCameraView &view);
    QRect stageLiveRect() const;     // the proposed stage, in this stage's px
    QVector<QPolygonF> stageHandles() const; // TL T TR R BR B BL L, widget px
    int stageHandleAt(const QPointF &widgetPos) const;
    void stagePress(const QPointF &widgetPos);
    void stageMove(const QPointF &widgetPos, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers);
    void stageRelease();
    void stageUpdateCursor(int handle);
    int stageLayersHiding() const;   // layers with art the proposed stage would hide
    QString stageChipText(bool *warns) const;
    QString stageKeysChipText(const ShotCameraView &view) const;
    void viewHoldBegin();
    QTransform viewTransformExact() const; // viewTransform without the display's whole px
    void viewHoldEnd();
    int m_camHover = -1;          // hit under the idle pointer
    int m_camDrag = -1;           // hit at the press; -1 = no drag
    bool m_glyphsHiddenForTest = false;
    bool m_camShift = false;
    shotcam::CameraState m_camStart; // at the press
    shotcam::CameraState m_camLive;  // during the drag
    // Stay inside with depth (Phase 2C): the panel's edge-reaching depth
    // layers, worked out at the press, and the one holding the frame in now
    // (-1: the stage or nothing) - the drag chip names it (Q3).
    QVector<shotcam::DepthEdge> m_camEdges;
    int m_camStoppedBy = -1;
    QString shotCameraEdgeText() const;
    QPointF m_camPressCanvas;
    QPointF m_camPressWidget;
    QPointF m_camPointer;         // widget px, for the drag chip
};
