# SankoTV — Session Handoff Brief

Paste this into a new chat to continue without losing context.

## Project
SankoTV — a Qt6 C++ desktop application for animation directors (storyboarding,
drawing canvas, animatic, AI generation).

- **App source:** `C:\SankoTv\app` (repo root `C:\SankoTv`, branch `master`,
  github.com/a3bashir/SankoTv)
- **Qt:** `C:\Qt\6.11.1\msvc2022_64`
- **CMake:** `C:\Qt\Tools\CMake_64\bin\cmake.exe`
- **Build dir:** `C:\SankoTv\app\build` (Debug)
- **Rebuild:** `C:\Qt\Tools\CMake_64\bin\cmake.exe --build C:\SankoTv\app\build --config Debug`
- **Run:** `C:\SankoTv\app\build\Debug\SankoTV.exe`
- NOTE: the working directory `G:\Claude Code\Apps_Div\Artist_Ref` is a separate
  memory/reference folder, NOT the git repo. All git commands must `cd /c/SankoTv` first.

## Working style / conventions (IMPORTANT — keep doing these)
- **Commit ONLY when I explicitly say "commit and push."** Never commit otherwise.
  Commit messages end with `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.
  Stage only the intended files; leave `PerspectiveLab/` untracked.
- **Verify every change with a temporary "seam" before reporting PASS/FAIL.**
  Pattern: two env-var seams — `SANKOTV_GOTO_STORYBOARD` (MainWindow ctor: builds a
  test scene/panel and jumps straight into the Storyboard) + `SANKOTV_TEST_<NAME>`
  (StoryboardPage ctor tail: a `QTimer::singleShot(900,...)` block that drives
  synthetic `QMouseEvent`s/key events, asserts, and writes `PASS/FAIL` lines to
  `C:/SankoTv/app/<name>.txt`, then `QCoreApplication::quit()`). Run headless,
  read the results, then REMOVE the seams and do a final clean build.
  - Canvas coord mapping in seams: `fit = qMin(width/960.0, height/540.0)`;
    widget point = top-left offset + `(cx*fit, cy*fit)`.
  - Seam scripts are Python (index/marker-based text edits) in the scratchpad.
    The Bash tool is Git Bash (POSIX sh) — do NOT use PowerShell here-string
    syntax there.
- **How existing source files are REWRITTEN (not just written).** Six
  mechanical file-writing failures so far: five shell-heredoc manglings of
  C++ (backslash/quote corruption) and one Python text-mode rewrite that
  silently converted three files from CRLF to LF (2026-08-19, resolution
  epic unseam — Python's universal-newline read + `newline=''` write). The
  rule: **any modification to an existing source file goes through the Edit
  tool**, which preserves encoding and line endings exactly. No shell
  heredocs for C++ content, and no Python rewrite-in-place of existing
  source — not for deletions, not for "just removing a block". Python in
  the scratchpad is for generating NEW scratch files and for analysis only.
  If a whole-file operation is truly unavoidable, operate in BINARY mode
  ('rb'/'wb', splitting on b'\r\n') and verify byte-identical endings
  afterwards with `file`/`git diff --stat`.
- Before builds, kill the running exe to avoid LNK1168 exe-locked errors:
  `while taskkill //F //IM SankoTV.exe >/dev/null 2>&1; do sleep 1; done`
- No-regression rule: keep undo, save/load, groups, thumbnails intact; report the
  diagnosed cause, then the fix.

## Key architecture
- **Model** (`StoryboardModel.h`): `Scene` owns `Panel*`s; `Panel` has a flat
  `QVector<Layer>` + `activeLayerIndex`. `Layer` = {id, name, type
  ("raster"/"image"/"background"/"group"), QImage image (ARGB32_Premultiplied,
  960x540), visible, opacity, locked, colorTag, groupId, groupExpanded}.
  - **Groups = folders**: a `type=="group"` entry (null image) with members
    carrying `groupId`; `isGroupLayer()`; group-aware `layerEffectivelyVisible()`
    / `layerEffectiveOpacity()` (member AND-s folder visibility, multiplies folder
    opacity). `flattenedPixmap()` composites over white paper.
  - The old cross-panel "shared layer" (sharedId) system was REMOVED; load migrates
    legacy shared refs into independent copies.
- **Undo:** ONE app-wide `QUndoStack` (owned by MainWindow, limit 60). Commands:
  `DrawingCommand` (region-limited before/after pixels, by layer id),
  `SelectionCommand`, `PerspectiveCommand`, `LayerStackCommand` (whole-vector
  snapshot), panel insert/remove/move. Transform commits push one entry (single
  command, or several under a `beginMacro`/`endMacro` for multi/group) — the macro
  opens ONLY when >0 real commands, or empty macros corrupt undo.
- **DrawingCanvas** transform engine: `liftDefaultTransformBox()` →
  `beginTransform()` / `beginGroupTransform()` / `beginLayersTransform(ids)`;
  `commitTransform(relift)` / `cancelTransform()` / `refreshTransformBox()` /
  `resetTransformBox()`. Session identity by id: `m_xformLayerIds` + parallel
  `m_xformBufs` (pristine lifted pixels) + `m_xformHoles` (source-subtracted layer,
  built once at lift). Multi/group transforms preview & commit each layer AT ITS
  OWN z-position (never brings-to-front). Move tool box follows layer-selection
  changes with no tool toggle.
- **Rendering perf:** below/above composite caches (`m_compBelow`/`m_compAbove`,
  `ensureComposite()`, `invalidateComposite()`) — a repaint touches ≤3 images
  regardless of stack depth; direct loop fallback for transform sessions /
  light-table (both share one `paintLayerContent` lambda so output is identical).
- **Layers panel** (`StoryboardPage.cpp`, Figma node 7-70): footer (Delete, Clear,
  Merge, Group, Duplicate, Import, New), selected border = Sanko accent `#7C6EF6`,
  row thumbnails over a transparency checkerboard, group rows show a folder icon
  only (no box). `rebuildLayerPanel()` = full rebuild; `updateLayerSelectionUi()` =
  in-place restyle for selection-only clicks (perf). Panel thumbnails are debounced
  (`m_thumbTimer`, 120ms), flushed before panel switches.
- Docking = first-party native `QDockWidget` system in `src/docking/`
  (DockController/DockOverlay/DockTitleBar) — ADS was removed.

## Work completed this session (all committed & pushed to master)
1. `b00ad3b3b` Move tool transforms a selected group as one unit (box around
   group, in-place commit, one undo, group-row layout eye-before-chevron).
2. `eff0f2f43` Layers panel + Move tool 8 fixes (four-way move cursor; box follows
   merge; Duplicate-to-Panel supports groups; REMOVED Copy/Reuse shared-layer
   system + migration; BG layer disables Merge/Delete greyed; accent highlight
   `#7C6EF6`; removed dock title 6-dot grip; removed dark box behind "Layers").
3. `00ce415bf` 6 bug fixes (delete clears canvas at once; group show/hide
   propagates; Ctrl+click canvas auto-selects topmost layer; Move never reorders
   z; New Layer numbering = next free; multi-select transform hits all layers).
4. `ce8a471f1` Transform-undo integrity (undo no longer double-reverts the prior
   entry / toggles visibility) + Fit Screen centres & fully fits.
5. `e602f4f91` Fix undo after an Enter-committed transform (empty-macro root cause).
6. `fe9b19c24` Perf: composite caches, Move-session hole-cache, thumbnail debounce,
   in-place layer-row selection updates (~12.6x faster repaint at 40 layers).
7. `490b5a6bd` Layer thumbnails: transparency checkerboard + folder-only group rows
   (+ fixed a latent `updateActiveLayerThumb` clobber of the folder icon).

## Current state
- Clean working tree except untracked `PerspectiveLab/`. Build is green.
- HEAD = `490b5a6bd` on `master`, pushed.


## Open items (recorded 2026-07-28, after the engine integration verification)

- **DROPPED CONCURRENT PRESS** — a Brush press arriving while a stroke's
  async publish is still in flight is DISCARDED by the `m_paintCommitPending`
  guard (mouse and tablet paths), not queued. Pre-existing design, but publish
  windows are now routine (~10 ms Release / ~90 ms Debug per commit), so a
  fast sketcher can silently lose an intended stroke. Candidate fix: queue
  the press and replay it when the publish lands.

- **256 MiB UNDO BUDGET IS UNEXERCISED** — at the fixed 960x540 canvas,
  20 retained stroke after-pixel entries total ~40 MiB, so only the N=20
  count cap ever evicts. The byte-budget eviction path will run for the
  first time in production if canvas sizes grow, having never run under
  test. Verified behaviourally: eviction via the count cap keeps a mixed
  stack (strokes + classic commands) byte-exact through full undo/redo.

- **PIXEL LOCK RE-BASELINED** — the standalone Phase 4a closure hash
  `421845ebe66ca7892ddee3f49728365ce20c12424018470b56ef8cab67ed1ff7` is
  superseded and UNREPRODUCIBLE in-app: its fixture definition lived only in
  the standalone harness and was never imported (a prior in-app run recorded
  `3a1c9d84...` and FAILED the lock for the same reason). The lock is now the
  PERMANENT in-repo test `app/tests/PixelLockTest.cpp` (target
  `SankoPaintPixelLock`, fixture fully pinned in the file), baseline
  `666f7b455228e18020ad6b4967740de762e559140e8e52c98ddb415a9f91547a`, passing
  identically in Debug and Release. A future hash change there is a REAL
  pixel regression — do not mistake this re-baseline for one.

- **MEMORY FIGURES ARE UNCOMPARED** — integration-era measurements:
  181 MiB working / 147 MiB private at storyboard start, 314/314 after the
  full verification torture run (Release). No true pre-integration baseline
  was obtainable from git (pre-integration commits either lack the app state
  or do not build standalone), so these are measured absolutes, not a delta.

## Developer Recorder (TEMPORARY — remove before release)

A self-contained bug-capture tool for intermittent UI issues (built for the
Grab Control hang). CMake option: **`SANKOTV_DEV_RECORDER`** (default ON;
OFF compiles zero recorder code). All code lives in `app/src/devrecorder/`
plus one marked block in `app/CMakeLists.txt` and one `#ifdef`-guarded block
in `MainWindow.cpp` — to remove it completely, delete the directory, the
CMake block ("Developer recorder (TEMP)"), and the guarded MainWindow block,
or just configure with `-DSANKOTV_DEV_RECORDER=OFF`. Usage: Developer menu
or Ctrl+Shift+R to start/stop, **Ctrl+Shift+B = Issue Happened** marker
(works mid-interaction), menubar-corner REC indicator. Sessions land in
`Documents/SankoTV-DevRecordings/<timestamp>/` (override with
`SANKOTV_DEVREC_DIR`): screenshots/ (app window only, active-window-gated
for privacy), events.jsonl, system.txt, summary.txt (markers with the last
events before each). Tests: `SankoDevRecorderTest.exe` (pixel-lock pattern,
exit code = failures).

## Floating toolbars: Grab Control REMOVED — deliberate Figma divergence

2026-07-29: the grab_CTL pill (Figma nodes **213:79 / 213:81 / 213:83**) was
REMOVED from the Floating Zoom, Layers, and Brush toolbars. They now **drag
from anywhere on their background**; interactive children always win a press,
the threshold is `QApplication::startDragDistance()`, and `Qt::SizeAllCursor`
over background is the only affordance. This is an intentional PRODUCT
decision, not an implementation shortcut: the 50x8 conditional target and its
hover-visibility state machine caused three separate rounds of bugs
(intermittent stuck pill, hit target too small, slow hover appearance).
**Do not re-add the pill to match the Figma file.** Consequences: the 12px
reserved strip is gone, so layout rect == visual rect, and the settings key
was bumped `storyboard/floatToolbars/v1` -> `/v2` so strip-era saved
positions are discarded and defaults reapply. Double-click on background does
nothing (no special behaviour). SizeCtlBar (Floating Brush Size) is unmanaged
and KEEPS its own grab, unchanged.

### SizeCtlBar converted too (2026-07-29 follow-up)

The Floating Brush Size toolbar was converted to the SAME drag-from-background
model for CONSISTENCY - not because it was broken (it had been the reference
implementation). Its pill grab, hover-visibility state, and the kGap/kGrabW
strip are gone; it drags from background with Qt::SizeAllCursor and a
startDragDistance threshold, keeps its own snap-to-nearest-side on release,
and its width is now exactly kBarW (46, was 58). QSettings keys bumped to
storyboard/sizeCtlSide/v2 and storyboard/sizeCtlY/v2 so strip-era values are
discarded. It remains UNMANAGED: it takes no part in margins, snapping, or
collision resolution for itself, and acts only as a collision OBSTACLE for
the three managed bars. All four floating toolbars now share one interaction
model, and the Figma grab_CTL divergence noted above applies to all four.

## Floating toolbars: the 4px edge margin is CORRECT — do not "fix" it

2026-07-30: the 4px window margin was reported as missing on left/right
snapping. It is not missing: at a diagnostic kMargin=20, all 16 bar x edge
combinations showed exactly 20px of painted gap at native resolution, so the
clamp runs on the real drag path and the geometry is exact. The gap is
IMPERCEPTIBLE at 4px because the palette gives it nothing to read against:
bar #212121 vs canvas gutter #0a0a0a is ~1.2:1 contrast, and the bars'
1px #1a1a1a border (specified in Figma on 33:110 / 173:36 / 86:32, and
present in code) sits BETWEEN those two values (~1.1:1 against each). Do not
widen the margin or remove the border to "fix" this — the margin is
verified geometry and the border is the Figma spec. Any legibility change is
a deliberate design decision (brighter border or larger margin), not a bug
fix. Related divergence: the Brush Size bar's Figma node 209:42 specifies NO
border; the code gives it the same 1px #1a1a1a as the other three bars for
consistency (drawn inside its bounds — the 46x574 layout rect is unchanged).

## Workspace grid in the gutter (2026-07-30)

Screen-space grid drawn in the GUTTER only, behind the paper. Implementation
facts a future change must not break:

- Painted in DrawingCanvas::paintEvent BEFORE the world transform: gutter
  colour fill, then a tiled kGridSpacing (25px) pattern anchored to the
  widget origin. It never pans/zooms/rotates with the artwork, and the
  constant screen-px spacing means no moire and no adaptive subdivision.
- The paper hides it by DRAW ORDER alone: both composite caches and the
  direct paint path start from an opaque white fill of the full canvas rect
  (ensureComposite: m_compBelow.fill(Qt::white); direct: fillRect(canvasR,
  white)), so the grid cannot show through, whatever layer visibility/alpha
  does - verified with the Background layer hidden and with alpha content,
  under 30 degree canvas rotation (transformed-quad footprint), leaks=0.
  If that opaque paper fill is ever removed, the grid needs clipping.
- Styles: Square (1px rules), Dashed (3-on/2-off, period 5 divides 25 so the
  tile is seamless), Cross (7px plus at intersections), Dot (3px AA dot).
- Pure view state: QSettings canvas/grid/v1/{visible,style,gridColor,
  gutterColor}, APP-WIDE, not per-project; never in save files, layer bytes,
  thumbnails, or undo. First-run defaults: hidden, Square, grid #2a2a2a,
  gutter #0a0a0a.
- Right-click on the canvas opens the grid menu (right-click had NO prior
  binding there); suppressed during strokes/drags/transforms via
  viewInteractionActive(). Show/hide shortcut: Ctrl+' (no collision).
- NOTE for the open black-canvas anomaly (view/transform, t~29.5-30.0s in
  the 2026-07-29 recording): this task touched paintEvent's first lines
  (gutter fill + grid blit) but NOT viewTransform/displayRect or the paper
  compositing. If the paper ever vanishes while gutter+grid render normally,
  that isolates the fault to the transform/composite path, not the widget
  paint plumbing - the grid is a useful witness, not a suspect.
- Camera safe-area masks dim everything outside the safe frame (~x0.6); any
  pixel-probing test of gutter content must match colours by hue, not
  absolute value (this cost one seam iteration).

## The three CAPTURE / RENDERING traps (2026-08-01) — read before writing any UI verification

Three separate traps have now cost real time on this project. They are
recorded together because they are the same class of problem: what you
measure is not what the user sees.

1. **QSS backgrounds CASCADE onto floating-window children.**
   `StoryboardPage` sets `background-color: #0a0a0a` on itself, and Qt
   cascades that to every DESCENDANT. A plain `QWidget` child of a floating
   tool window therefore paints that inherited opaque fill OVER the window's
   own `paintEvent` chrome — the chrome survives only in the layout margin,
   which reads as "my painting is not running".
   The Brush Library panel escaped this ONLY because its children are
   custom-painted subclasses, which do not render QSS backgrounds. Any
   future plain-QWidget child of a floating window will hit it.
   Fix used by the Brush Settings studio: a window-scoped
   `QWidget { background: transparent; }` reset (and style `QMenu`
   explicitly, since it would otherwise inherit the transparency).

2. **`QWidget::grab()` lies on translucent top-levels.**
   On a `WA_TranslucentBackground` TOP-LEVEL, `grab()` substitutes a palette
   fill for the real backing store, so a correctly-painted window looks
   blank/opaque. Verify floating windows with `QScreen::grabWindow(0)`
   cropped to `frameGeometry()` — a real screen capture — and never with
   `grab()`. (Phase 3 chased this into a wrong "fix" before a red-fill probe
   proved the paintEvent was composing correctly all along.)

3. **The Developer Recorder downscales by 2.**
   Anything measured off a recorder frame is at half resolution. Do not take
   pixel geometry from recorder output.

Corollary that applies to all three: a floating window is only visible when
its ANCHOR is visible. The app boots to the Dashboard page, so a seam must
front the Storyboard page before asserting anything about floating windows —
eleven assertions failed as a group once for exactly this reason.

## Deferred: stroke-shaping engine mini-phase (Fall off, Taper, per-brush stabilization)

These three are DEFERRED BY DECISION, not oversights. Recorded so a future
reader does not "fix" them by binding them to whatever is nearby.

- **Fall off** is drawn in the Figma Stroke section (node 274:104). The
  engine has NO stroke-length falloff of any kind — not for size, opacity or
  flow. The row was DROPPED rather than silently bound to something else,
  which is why the one fully-specified section in the design ships four of
  its five rows.
- **Taper** is the same feature: stroke-length falloff applied to size.
  Taper exists today only IMPLICITLY, as whatever a size/opacity pressure
  curve does when a tablet reports fading pressure at the stroke ends —
  there is no length-based taper for mouse or steady-pressure input. The
  sidebar item was removed with Fall off.
- **Per-brush stabilization** was deferred with them. Phase 4 shipped
  stabilization as an APP-LEVEL setting (`paint/v1/stabilization`) instead
  of a per-brush parameter, specifically to avoid a codec wire bump to v2 in
  the middle of a UI phase — a bump would have put the preview SHA and both
  pixel locks at risk of moving for a reason unrelated to that work.

All three belong to ONE deliberate engine mini-phase: length-based falloff
applied to size/opacity/flow, plus a per-brush stabilization parameter, with
a codec v2 bump that keeps a v1 reader (Phase 3+ user presets and exports
must keep loading), and hand-verification with a real tablet.

## BrushLibraryModel::presetsIn() returns pointers INTO the model's vector

`presetsIn()` / `recentPresets()` hand out `const BrushPreset *` that point
into `m_presets`. Any `addUserPreset` / `importFile` / `removeUserPreset`
reallocates that vector and DANGLES every outstanding pointer.

Production is safe today only by convention: every call site re-queries by
id (`model->preset(id)`) rather than holding a pointer across a mutation.
The phase 5 seam held them across an import of 143 presets and segfaulted.
If you hold one of these pointers, hold the `id` instead.

## Build command: kill the running app FIRST, and always verify the exe timestamp

`--clean-first` Release builds have now hit `LNK1104: cannot open file
...SankoTV.exe` in FOUR consecutive phases. The linker cannot overwrite an
exe that a running instance holds. Use this as the standard build command:

```bash
powershell -NoProfile -Command "Get-Process SankoTV -ErrorAction SilentlyContinue | Stop-Process -Force"
"/c/Qt/Tools/CMake_64/bin/cmake.exe" --build build --config Release
ls -la --time-style=+%m-%d_%H:%M:%S build/Release/SankoTV.exe
```

Two warnings that cost real time, both learned the hard way:

- **The kill is destructive.** It force-terminates the app with no save
  prompt. In phase 5 it killed two instances that may have held unsaved
  work. Prefer closing the app yourself; use the kill only when you know
  what is running. The real fix is for the app to prompt on quit — that is
  an open item, not something the build command can solve.
- **ALWAYS check the exe timestamp after building, and run from
  `C:\SankoTv\app`.** A `cmake --build build` issued from the wrong working
  directory does NOT fail loudly — it prints a cache error that does not
  match a `grep -E "error C|error LNK"` filter, so the build "succeeds"
  while the exe on disk stays stale. Phase 5 lost a full Release
  verification cycle to this: the seam appeared to hang for fifteen minutes
  when in fact it was never in the binary being run. A timestamp check
  catches it instantly.

## Pressure toggles REMOVED with the Brush Options panel (2026-08-01) — recorded tradeoff

The Brush Options panel is gone: Opacity and Hardness moved to the floating
Size CTL bar (three uniform 150px sliders — the slider LENGTH is the one
approved divergence from Figma 209:42, made to fit the third slider inside
the unchanged 46x574 bar), and the five phase-1-era preset buttons were
superseded by the Brush Library.

The "Pressure -> Size" / "Pressure -> Opacity" checkboxes were removed
DELIBERATELY, not lost: each was a blunt override that replaced a preset's
multi-point pressure curve with a hard-coded linear or flat one — exactly
the curve-clobbering trap phase 3 documented. With per-preset Dynamics
curves in the Studio, the curves are authoritative.

THE COST, so it is not rediscovered as a missing feature: turning pressure
response off for a brush now requires flattening its curve in the Studio's
Dynamics section, where it used to be one checkbox. If quick toggles ever
come back, they must write through the Studio's session model (one undo
entry, preset-aware), not through the old canvas slots — the slots
themselves (setPressureToSize/-Opacity) remain as API and are exercised by
SankoCanvasBrushLock.

## SizeCtlBar reverted to the Figma two-slider layout (2026-08-02)

The three-slider SizeCtlBar (Size + Flip + Opacity + Hardness at a uniform
150px) lasted one task. It read as TWO SIZE SLIDERS plus opacity — hardness
has no label and visibly changes apparent stroke thickness — and the 150px
compression broke the 209:42 design. The bar is back to Size (220px), Flip,
Opacity (220px) in the exact Figma column: size at (10,25), Flip 30x30 at
(8,276), opacity at (10,337), bar 46x574.

HARDNESS IS CURRENTLY STUDIO-ONLY (double-click a brush -> Tip section).
That is a decided tradeoff pending a better home, NOT an oversight; the
quick-control decision was deliberately separated from the layout revert.
What survives from the relocation, on purpose:
- per-tool hardness PERSISTENCE under storyboard/toolCtl/brush/hardness
  (+ dormant hardnessTicks): restored into the canvas at launch and
  mirrored on every preset selection, so last-session hardness still
  round-trips with no visible slider;
- the canvas setBrushHardness slot, exercised by SankoCanvasBrushLock's
  endpoint AND mid-range fixtures.
Any future quick hardness control should read/write that same key and go
through the existing slot — the state is already flowing.

## The canvas "1-pixel white border" was the Camera Frame overlay (2026-08-04)

STOP AND READ THIS BEFORE INVESTIGATING ANY CANVAS-EDGE LINE. A pale 1px
line hugging the artwork was chased across several investigation cycles as a
rendering defect. It was the CAMERA FRAME OVERLAY, which shipped ON by
default. It is now OFF by default (DrawingCanvas.h m_cameraFrame, and the
matching toolbar button state in StoryboardPage.cpp) — the feature itself is
unchanged and one click away.

MEASURED SIGNATURE, so the same line is never chased again. The overlay dims
everything outside the 16:9 shot with rgba(0,0,0,102) = x0.6, then draws a
#cccccc (204) outline just outside the paper:
- dimmed gutter  = gutter x 0.6   -> 77 (#4d4d4d) reads 46; 10 (#0a0a0a) reads 6
- outline blend  = 122,122,122    (204 half-covering the dimmed gutter)
- with it OFF    = gutter straight into artwork: 77 -> 0, or 10 -> 0
If you see 122 at the boundary, it is the camera frame. If you see the
gutter value x0.6 just outside it, that is the dim. Neither is paint.

THE FILL PATH IS VERIFIED CORRECT — do not re-audit it. After a black fill,
3000/3000 border pixels are fully opaque on ALL THREE fill routes: plain
Fill tap with no selection, Select All -> Fill, and an edge-to-edge marquee
drag -> Fill. Measured on the layer image, all four borders, alpha 255.

TWO REAL display-side causes WERE found and fixed on the way, and they are
genuine — keep them:
1. the old permanent canvas frame's cosmetic pen, centred ON the boundary
   path, blended half its width into the outermost row/column (a 230,60,60
   stroke read 136,51,51). That frame has since been REMOVED entirely; the
   white paper edge is the document/workspace separator.
2. white paper leaking between the separately-antialiased stacked content
   quads (paper/compBelow, active layer, compAbove) at alpha(1-alpha) weight
   on FRACTIONAL device coordinates — up to 25% white. Fixed by aliasing the
   content quads so every layer covers the same device pixels. Integer-only
   zoom tests cannot see this: it needs a fractional boundary, which the
   fit-relative 0.85 startup zoom produces every launch.
Both are locked by SankoCanvasEdgeLock (tests/CanvasEdgeLockTest.cpp), the
sixth permanent test family: 74 checks sampling REAL screen pixels via
QScreen::grabWindow at fractional zooms, rotation, both workspace
colourings, corners, plus a padded-reference engine comparison.

STILL OPEN, deliberately not changed (needs a product decision):
- the camera-frame outline hugs the artwork when the overlay IS on;
- marching ants on a full-canvas selection sit exactly ON the boundary (a
  white cosmetic pen centred on m_selectionPath — the same geometry as
  cause 1 above), and a fill does NOT clear the selection.
- the camera frame / safe area / title safe overlays do NOT persist across
  restarts; only the grid and its colours do (canvas/grid/v1/).

The static tip transform landed in dynamics Phase 2 — TWO items for later
phases (2026-08-08):

1. DISCHARGED (2026-08-08, the ABR unbake): the importer no longer bakes
   static transforms. Angl/Rndn/flipX/flipY map onto tipAngle /
   tipRoundness / tipFlipX / tipFlipY; the mask imports untransformed;
   computed tips use the engine's procedural tip with the descriptor's
   hardness. Two conversion facts worth keeping: the engine's tipRoundness
   squashes tip-local X while Photoshop squashes Y, reconciled by an EXACT
   90-degree content pre-rotation whose direction follows flip parity
   (mapStaticTransform in AbrImporter.cpp documents the algebra); and the
   OLD bake had an order defect — chained QTransform calls applied the
   rotation FIRST, squashing in canvas space, so angle+roundness brushes
   were ~20 degrees off Photoshop's squash-then-rotate order. The new
   mapping is verified against a sequentially-composed Photoshop-order
   reference (axis within 0.3 degrees, moment aspect within 8%).
   Identity now fingerprints canonical content (name + params + raw tip
   pixels), not codec bytes — codec-byte ids silently broke re-import
   idempotence at every wire bump. Old-generation imported presets keep
   their stored ids; re-importing the same ABR upgrades them IN PLACE by
   name (disk-first), preserving favourites/Recent/hidden, and the
   upgrade never claims a current-generation preset (its id recomputes to
   itself), so same-named brushes from different packs stay distinct.

2. PRE-EXISTING CPU/GPU DIVERGENCE: ROTATED OR COMPRESSED CUSTOM TIPS. The
   CPU reference samples the bucket-rescaled tip (Brush::shape's cached
   size/hardness bucket, then bilinear), the GPU samples the ORIGINAL
   customShape texture in one step. The two resampling chains agree at
   identity (delta 1/255) but disagree along hard edges once the sample
   grid rotates or compresses: measured max deltas on a hard-edged custom
   tip, single 100 px stamp — angle 37 deg: 106/255; roundness 0.55:
   57/255; ANGLE JITTER 0.5 with NO static fields: 81/255. The jitter
   control proves the gap PRE-DATES Phase 2 — the statics only made it
   deterministic and easy to hit. Procedural tips are unaffected (both
   paths evaluate the same analytic falloff; <= 3/255 with full statics),
   and flips are exact (texture mirror == coordinate mirror, 1/255). Fix
   direction when it is taken up (engine-quality work, not Phase 3): make
   the GPU sample the same bucket-rescaled tip the CPU uses (per-bucket
   texture uploads), or make both sample the original. Soft tips hide it;
   hard-edged imported ABR tips at fixed angles will show it as a subtle
   GPU-vs-preview edge difference.
   MEASURED ON THE IMPORTED CORPUS after the unbake (2026-08-08): the 29
   brushes whose mapping changed (static transforms now runtime, computed
   tips now procedural) render 29-255/255 from their baked-era previews at
   preview size — expected and explained per brush; the OTHER 129 corpus
   presets render byte-identical. The divergence entry above still stands
   as engine-quality work; imported brushes with static angle/roundness on
   hard-edged tips are now the likeliest place a user meets it.

## New Project dialog (Figma 350:24) — deferred items

- **DISCHARGED (2026-08-19, resolution epic Pass 3a):**
  canvasWidth/canvasHeight are now APPLIED, not merely stored. See the
  "Canvas resolution epic — Pass 3a" section below for what changed and
  what deliberately did not.
- **Save As still scatters sibling PNGs** next to wherever it is pointed,
  while Create now writes `<Location>/<Name>/<Name>.sankotv` (folder per
  project). Save As should probably adopt the same folder-per-project
  shape; decide separately — changing it silently would surprise existing
  muscle memory.
- **Six one-off QLineEdits** are candidates to migrate to the shared
  StudioTextField: ConsistencyBoard.cpp ×4 (entry name/tags, edit + create
  forms), StoryboardPage.cpp ×2 (panel mood field, layer rename). Not
  migrated with the dialog commit, by instruction.

## Canvas resolution epic — Pass 3a (2026-08-19)

canvasWidth/canvasHeight became the single source of truth. Two-level
authority: at LOAD the artwork's real pixel size wins over the manifest
(ProjectIO::projectFromJson reconciles; a mismatch shows one plain dialog
— canvasMismatchDialogText in StoryboardModel.h, ONE definition so the
load path and any test assert the same string — and the next save writes
the honest numbers; artwork is never rescaled, cropped, or discarded). At
RUNTIME `Panel::canvasSize()` (first layer's image size) is the truth;
`DrawingCanvas::canvasSize()` is now an INSTANCE method forwarding to the
active panel. All layer factories (makeLayerImage / makeRasterLayer /
makeBackgroundLayer / makeBlankPanel) take a required QSize — no default,
so the compiler flags any new silent-960×540 site. Save version stays 1
(the size keys were already optional). File-level serialization moved to
src/ProjectIO.{h,cpp} so correctness logic is testable without
MainWindow (whose page teardown writes dock state to the real registry).

Release-build guard: SANKO_REQUIRE_PANEL(ret) in DrawingCanvas.cpp —
Q_ASSERT_X in debug; in release, qCritical once then `return ret;`. When
it fires the operation is REFUSED: nothing is computed on an invalid
QSize, no 960×540 stand-in, the (empty) workspace stays consistent.
Used by toCanvas(), ensureComposite(), placeViewForTest().

PRE-EXISTING fixes folded in (these crashes/asserts predate this pass —
they were reachable whenever no panel was active and are simply exposed
more by dynamic sizing): wheelEvent (now ignores the event),
selectAll and invertSelection (now early-return). Marked PRE-EXISTING at
each site in DrawingCanvas.cpp.

Perspective legacy migration: pre-epic perspective JSON stored absolute
pixel positions authored against 960×540. fromJson now takes the canvas
size and derives the legacy defaults as fractions (0.4h horizon, 0.15w /
0.85w / 0.5w vanishing points, 1.6h distance) — approved fractions, but
NOT yet validated on unusual aspect ratios (e.g. tall 777×1013); a
portrait project migrating old perspective data may want a design pass.

Deliberately NOT in Pass 3a (known, unchanged, scale with canvas size):
- AnimaticPage flattens panels per repaint — at 4K this is a real
  per-frame cost; needs caching before large-canvas animatics feel good.
- AnimaticPage MP4 export renders at a FIXED 1920×1080 delivery format —
  now the named constant kExportFrameSize with a comment; a 4K project
  exports downscaled. Product decision to revisit.
- NewProjectDialog recents decode full-resolution flatten PNGs just to
  paint thumbnails; slow with many 4K projects on disk.
- GenerationPage base64/vision payloads grow with canvas area; 4K
  panels make large API requests.
- Undo keeps full before-pixels per drawing command (undo limit 60);
  at 3840×2160 that is ~33 MB/command worst case. Fine at 960×540,
  worth a budget at 4K.
- Resize-after-creation is NOT implemented and has no UI; a project's
  size is fixed at creation (or by its artwork on load).
- Dead src/brush/BrushEngine.{h,cpp} (in no build target) still carries
  a QSize(960,540) default argument — left per instruction; delete the
  files when convenient.
- onNewProject() does not reset m_canvasWidth/Height: safe today, its
  only caller (the New Project dialog handler) assigns both from the
  dialog immediately after. If a second caller ever appears it must set
  the size or the previous project's size leaks through.

## Performance pass 3b (2026-08-21) — measured, fixed, re-measured

Requirement 0 measured before anything was built (probe archived as
tests/_backups/perf_probe_3b_req0_20260821.cpp), and the measurements
corrected the 3b brief twice: the 4K generation payload is 2.48 MB, UNDER
the 5 MB Anthropic limit (not an automatic 400 — dense content can still
exceed it), and the transient per-request memory is ~16 MB, not ~100 MB.

Three fixes landed (verification seam archived as
tests/_backups/seam_perf_3b_20260821.cpp, 63 checks x 10 green runs):

1. Panel::flattenedThumb() — a ~512 px long-edge mip of the flatten,
   cached per panel and VALIDATED per read (fingerprint of layer order,
   each QImage::cacheKey(), visibility/opacity/type/group fields; 0.11 us
   per panel). No call site invalidates anything; the cache proves its
   own freshness. Consumers: timeline clip thumbs, strip thumbs,
   generation row thumbs. Full-res flattenedPixmap() stays UNCACHED on
   purpose (33 MB per 4K panel; caching it is gigabytes per scene).
   Timeline repaint 50 clips: 20.4 -> 2.7 ms at 960x540 (a defect at the
   legacy size — scrub was capped under 50 Hz), 284.7 -> 2.9 ms at 4K.
   loadScenes 50 panels warm: 853 -> 19 ms at 4K. COLD load still pays
   the first mip build per panel (~810 ms at 4K, once per session) — an
   async prewarm is possible future work.
2. New Project recents: per-row mtime-keyed cache with
   QImageReader::setScaledSize decode (RecentList::rowThumb). The old
   full-res decode per row per paintEvent was masked at 960x540 by
   QPixmapCache but re-decoded EVERY hover repaint at 4K (64 ms/row,
   638 ms/repaint at the 10-row cap).
3. Generation: one flatten per submission (was three: two blank checks +
   encode); payloads downscale before encode — 1568 long edge for
   Anthropic vision, 1280x720 for fal (the request asks for a 720p
   render); downscale-only, never upscale. buildFalBody() extracted from
   callFal so request construction is testable without a network.

Findings for later:
- A 50-panel 4K scene with 3-image panels holds ~5 GB of layer images
  (33 MB per 4K ARGB32 image). The probe OOM-crashed at that load. Big
  4K projects will need layer-memory work (tiling/compression/eviction)
  eventually — nothing in 3b addresses footprint, only redundant work.
- Animatic playback DISPLAY (AnimaticPage:513) still flattens full-res
  per panel ADVANCE (~17 ms at 4K) — per advance, not per repaint, so it
  was left alone deliberately.
- MP4 export unchanged (1920x1080 delivery constant, full-res flattens).

## Seventh permanent family: SankoCanvasSizeLock (2026-08-21)

The gate is now SEVEN families. The first six pin 960x540 fixtures;
SankoCanvasSizeLock (tests/CanvasSizeLockTest.cpp) is the
variable-resolution lock, promoted from the archived 3a/3b seam evidence
into CI. Anti-vacuous by construction: every size it pins is a size it
DRAWS at through the synthetic-event path — a corner-entry stroke must
paint (W-1, H-1) after the same sampler proved it empty. 79 checks:
authority/strokes/undo at 960x540, 1920x1080, 2048x1080 (right edge ON a
tile boundary), 3840x2160, 777x1013; persistence byte-identity +
second-save fixed point + a comparator-must-fail control at three sizes
(presence bugs, not magnitude bugs); the migration/mismatch locks
including the dialog's exact string against an independent literal;
cross-size staleness; and the flatten-thumb cache at 777x1013 (its only
non-legacy CI coverage). Runtime measured: ~18 s Release, ~21-25 s Debug
(pump-dominated; the 4K increment is small, which is why 4K strokes
stayed in). Needs a GUI session but samples NO screen pixels — none of
EdgeLock's capture sensitivity. Remaining known gap, deliberate:
screen-pixel rendering at variable sizes (EdgeLock is 960x540-only);
extending EdgeLock is its own decision.

## Stroke-path pass, requirement 0 (2026-08-21) — measured attribution

Probe archived as tests/_backups (stroke_perf_probe). All three stall
classes from Dev Recorder session 20260821-153353 were re-attributed or
refuted by measurement — recording evidence is a symptom, not a
measurement:

- The 148 ms IDLE stall: INVESTIGATED AND NOT REPRODUCED — do not chase
  it again from that recording. Seven 8-second idle watches after heavy
  4K activity (25 strokes, undo policy engaged): worst GUI slice 6.7 ms,
  no working-set drop events, and NO eviction step at stroke 21+ (the
  paint-undo dropAfterPixels path costs 2-4 ms flat at 4K tile-patch
  sizes). Remaining suspects are recorder-side (500 ms screenshot
  grab/JPEG) or allocator decommit — neither is the app.
- The briefed "33 MB DrawingCommand copies" driver: REFUTED. A
  full-canvas 4K QImage::copy measures 5.3 ms, and the undo command
  stores per-tile patches, not full-canvas copies.
- DROPPED CONCURRENT PRESS (existing entry above) now has a number: the
  publish pending window measured 13-14 ms at 960x540 vs 63-71 ms at 4K
  — the discard window is ~5x wider at 4K. The GUI stays responsive
  (worst slice ~10 ms) while pending; queuing the press remains the
  candidate fix and remains a stroke-semantics change, out of scope for
  the performance pass.
- The stall STAGE, named by temporary instrumentation (removed): the
  synchronous engine replay of the QuickShape stream on the GUI thread,
  in both its appearances — the Done bake (bakeReplayLoop, up to 76 ms
  measured at 4K) and every preview rebuild (previewSyncBuild, up to
  37 ms, runs on each shape manipulation under a 16 ms coalescing
  timer). Preview PUBLISH and the shape-bar rebuild measured zero events
  over 8 ms — eliminated as suspects. 4K paintEvent runs up to ~28 ms;
  the intermittent 88-152 ms slices are a replay landing in the same
  GUI slice as one or more 4K repaints. The QS preview replay at legacy
  size (12-24 ms) makes this a DEFECT AT 960x540 TODAY, not a 4K
  regression — the recorded 87.9 ms spike was at legacy size.

## Done-bake async proposal — DECLINED 2026-08-21 (recorded so the
## reasoning is not reconstructed if it comes back)

Candidate: run the QuickShape Done bake's engine replay (measured up to
76 ms at 4K, ~9 ms at 960x540) off the GUI thread the way the brush
publish path already works. Declined by decision: not worth trading
protocol surface for ~50 ms once per bake. The four invariants any future
attempt must hold:
1. UNDO ORDERING: the stroke command must be pushed on the GUI thread
   when the async result lands, in request order — reuse the existing
   m_paintCommitPending -> watcher -> publish -> push protocol, never a
   new one.
2. CAPTURED-STATE-WINS MUST SURVIVE THE FLIGHT: the bake replays with
   captured brush/seed/viewport-rotation. Synchronous code guarantees
   this by swap-restore of live members; an async bake must carry the
   captured state IN THE WORK OBJECT and never touch live members from
   the pool, or it races the artist's next stroke.
3. THE COMMIT BARRIER (the decider): a press arriving while the bake is
   in flight must either wait — widening the dropped-press window this
   work exists to shrink — or be ordered against the bake. Either answer
   is a sequencing DESIGN, not a performance fix. This is why it was
   declined.
4. The preview replay is the cheaper, lower-risk target and fires on
   every manipulation; the bake fires once per shape. Fix the preview
   first (done in the stroke-path pass); revisit the bake only if ~50 ms
   per bake at 4K still matters afterwards.

## Preview replay fix (stroke-path pass, 2026-08-21)

renderQuickShapePreview no longer runs the engine replay on the GUI
thread. The whole build — 33 MB host alloc + beginStroke/appendPoint loop
+ finishStrokeWork — moved inside the pooled job on a PRIVATE
SankoPaintHostAdapter; the GUI-side cost is capturing the inputs by value
(stream, brush, seed, size, selection mask). The shared m_paintEngine is
no longer involved at all (no brush swap, no preview key, no forgetLayer);
the strokeActive() guard stays for behavioural parity; the
generation/in-flight/dirty staleness protocol is unchanged (the stale-race
lock pins it). Off-thread engine building has precedent in
BrushPreviewRenderer. m_qsPreviewHost member removed (dead).

Verified (seam archived: tests/_backups/stroke_perf_seam_qsfix_20260821):
golden previews of the full lifecycle (circle, Triangle conversion,
Rectangle conversion, vertex drag, at 960x540 AND 3840x2160) captured
pre-fix — byte-stable across two capture runs, so the comparison is
valid — and byte-IDENTICAL post-fix, 40/40 across 5 Release runs and
40/40 across 5 Debug runs (Debug compared against Release-captured
goldens: cross-config engine parity, consistent with the pinned locks).
Distribution, not average: the recognition+settle windows' GUI slices —
PRE 4K: 5 of 24 windows had a slice over 30 ms (max 42.7; the recorded
spikes reached 151.6) — POST 4K: 0 of 60 windows over 30 ms (max 24.5,
bounded by repaint alone). 960x540 max 13.8 ms post (the 87.9 ms legacy
settle spike class is structurally removed: the synchronous replay stage
no longer exists on the GUI thread). Done bake intentionally unchanged
(57.8 -> 59.1 ms at 4K; declined proposal above). Seven families green
in both configs at the pinned SHA/hashes.

## Bake follow-up: cheaper replay landed; pixel reuse REJECTED (2026-08-21)

FINDING ABOUT A PERMANENT FAMILY — do not build on this later as if it
were general: the QS geometry lock's preview == commit byte-equality is
FIXTURE-CONDITIONAL. render() composites the stroke over the layer
pixels captured at stroke begin, so commit pixels are
stroke-blended-onto-artwork for EVERY brush; the preview renders over a
transparent host. They are byte-equal only because the lock's fixture
bakes onto an EMPTY layer. On any artwork they legitimately differ —
plus: the selection-mask path lerps toward beforeRegion (transparent vs
artwork), replayQuickShape invalidates the preview generation at entry
BY DESIGN, and publish's mid-flight authority rebase re-renders over
current pixels. This is why preview-PIXEL reuse for the bake was
rejected: the correctness argument cannot be made. Undo also needs what
the preview lacks: tile patches captured against the real layer,
afterHashes, and the retained replay work for redo-after-drop.

What landed instead (the measured 97%): the bake's replay was spending
372 of 388 ms at 4K (80 of 82 ms at 960x540 — a LEGACY-SIZE defect at
real brush sizes) rasterizing per-move engine preview tiles whose only
consumer was the flight placeholder. The bake now passes
rasterizePreview=false when the already-rendered QS preview exists to
serve as the placeholder (installed through the existing
pending-preview mechanism after the watcher is armed; display-only).
No-preview paths are FIRST-CLASS: Done inside the 16 ms coalesce or the
render's flight, a failed preview render, and lifecycle commits that
raced the first render all keep the OLD rasterizing path so the shape
never vanishes during the flight. A STALE placeholder (shape edited
within the last render's flight) shows the previous geometry for the
~70 ms flight, then the correct published pixels replace it.
Done click measured: 81 -> 1.6-2.3 ms at 960x540; 388-392 -> 22.4-25.7
ms at 4K (15 samples, no tail); fallback path measured at old cost
(385 ms at 4K) proving old behaviour is preserved where it must be.
Verified (seam archived: tests/_backups/seam_bake_fix_20260821): baked
pixels onto NON-EMPTY layers byte-identical to the pre-fix build
(goldens proven byte-stable across pre-fix runs first — also proving
the default brush renders seed-independently despite the random
m_qsSeed); undo/redo byte-exact including redo-after-after-pixels-drop
(bake + 21 strokes, 22 undos + 22 redos); flight continuity asserted on
BOTH paths; the no-preview path driven deterministically (commit in the
same event-loop iteration recognition flips). 30 checks x 10 runs
across both configs; seven families green at the pinned hashes.

## Post-bake 50-65 ms class (2026-08-21) — recorded at campaign close

The final 4K recording (session 20260821-185856, both stroke-path fixes
in the binary: seven full QuickShape cycles, brush 149) shows nothing
above 65 ms anywhere. What remains is a 50-65 ms GUI slice in the second
AFTER a bake tap — not on the tap itself (the bake's synchronous slice
is 22-25 ms and no longer registers among the top gaps).

ATTRIBUTION IS INFERRED FROM TIMING CORRELATION, NOT STAGE-MEASURED.
Two candidate contributors, in likely order:
1. The composite-cache rebuild over a 4K layer when the async publish
   lands (invalidation -> next paint rebuilds m_compBelow/m_compAbove:
   roughly two full-canvas composites plus a repaint — fits the size).
2. The bake placeholder's repaint rect: completePaintStroke repaints
   m_pendingPreviewRect, which the cheaper-replay fix sets to the FULL
   canvas. It could be cropped to the stroke's affected rect, which the
   publish watcher already knows — a one-line candidate, unmeasured.

NOISE FLOOR, read this before chasing it: DevRecorder's own overhead
measured up to ~19 ms of UI-thread gap under load, so recording-derived
numbers at this scale are blurred by the instrument itself. Any pursuit
needs bench stage-measurement (the archived probes in tests/_backups are
the pattern), not another recording. At 50-65 ms once per bake, this was
deliberately left as the stopping point of the stroke-path campaign.

## QS drag blanking: monotonic progressive display (2026-08-21)

REGRESSION after the preview-replay fix: on a 4K canvas the rotate/scale
drag showed NOTHING for its entire duration (probe: 23/23 samples blank,
preview null throughout; screenshot confirms a bare canvas with only the
hint chip). Root cause is a LATENT PROPERTY OF THE PREVIEW PROTOCOL, not
of the fix: the landing rule displayed a result only if NO further change
happened during its flight. Under continuous manipulation, any canvas
large enough that the render flight outruns the event cadence (16 ms
coalesce + ~8 ms moves vs ~105 ms flights at 4K) drops EVERY frame,
forever — including the first, so the display never fills. The
synchronous pre-fix build had masked the race by blocking the event queue
each cycle, chopping the drag stream into bursts that let some renders
land clean. The async fix uncovered a latent starvation race rather than
introducing one; the same race exists at ANY size where flights outrun
input — 960x540 measured 0/23 blank only because ~25 ms flights win it.

Fix (display-lifecycle only): MONOTONIC PROGRESSIVE DISPLAY. A landed
frame shows if it is NEWER than the frame on screen and the shape is
alive (m_qsPreviewShownGen); every deliberate preview clear advances the
shown generation so an in-flight result can never resurrect a cleared
display. Landings are serialized by the in-flight guard, so display only
moves forward; the dirty re-render chain still guarantees the settled
frame is the LATEST geometry — exactly what the QS geometry lock's
stale-race section pins, and it stays green. Result: 19 distinct frames
across a 1.6 s 4K drag (~12 fps progressive), 23/23 at 960x540,
cancel-mid-flight forced deterministically at both sizes and never
resurrects. Seam archived: tests/_backups/seam_qsdrag_fix_20260821.

VACUOUS-TEST CATALOGUE, seventh entry: the first repro probe sampled the
WHOLE WIDGET for ink and the dark gutter kept the count high while the
canvas paper was completely blank — "blank=0" over a fully blank canvas.
A sampler measuring the wrong REGION would have shipped a no-bug-found
conclusion; the mid-drag screenshot caught it. Corollary to the standing
rule: a visibility sampler needs a positive control proving it goes DARK
when the thing it measures is absent — restricting to the paper rect and
re-checking flipped blank=0 into blank=23/23.

## Project Settings — Part 1 landed; Part 2 (resize) REPORTED, UNIMPLEMENTED (2026-08-22)

File > Project Settings... (NOT under Edit > Preferences — those are
application-wide; these belong to the open project). ProjectSettingsDialog:
General = Project Name (StudioTextField) + Frame Rate (StudioDropdown,
24/25/30/60 plus the project's own rate so opening never silently changes
it); Canvas = read-only resolution + aspect ratio and a DISABLED "Resize
Project..." with tooltip "Project resizing is not yet available." Edits
are PENDING: Cancel discards, Apply commits and stays open, OK commits and
closes; the dialog owns no project state — it emits applied(name, fps) and
MainWindow::applyProjectSettings sets the members, calls
AnimaticPage::setFps (AnimaticTimeline re-derives per-block frames), and
updates the title. No pixel is touched by an FPS change. Two checks were
promoted into SankoCanvasSizeLock section (g): FPS re-derives frame counts
(24/30/60 -> 240/300/600 for 10 s of panels, read from the DERIVED total
via totalFramesForTest) and the dialog's pending contract. The remaining
Part 1 seam checks were judged not to earn permanent slots (the pixel check
is near-tautological; UI-shape checks are redesign-fragile) — seam archived
as tests/_backups/seam_project_settings_part1_20260822.cpp.

DECISIONS RECORDED for Part 2 (canvas resize), none implemented:
- V1 = Resize Canvas Only (expand/crop, artwork scale preserved), anchor
  CENTRE. Scale Artwork is V2, after the staging + uniformity machinery is
  proven.
- FPS is NOT undoable (Ctrl+Z silently changing playback timing mid-drawing
  is worse than re-opening a dialog). Resize is NOT undoable by design: it
  clears the undo stack (every canvas-space command — drawing regions,
  stroke tile patches + replay, selection paths, perspective VP JSON — is
  invalid after a size change) and runs behind an explicit confirm with a
  save prompt first. A pixel-snapshot resize command cannot fit the 256 MiB
  after-byte budget (one 3-layer 4K panel is ~100 MB; a 50-panel scene
  ~5 GB each way).
- Atomicity = TWO-PHASE: build every new layer image into staging while
  touching no panel; only after all succeed, swap into every panel in one
  synchronous pass, update m_canvasWidth/Height + setProjectCanvasSize,
  clear undo, invalidate caches (strip labels, onion/light-table pixmaps
  need explicit passes; flattenedThumb, adapter mirror, composite and
  selection-mask caches self-heal on size). Panel::canvasSize() and the
  reconcile stay as they are; the swap cannot fail. Peak memory at 4K is
  old + new (~10 GB for the 50-panel scene in the footprint finding).
- Future Resize UI: Width/Height StudioTextFields, Lock Aspect toggle,
  preset dropdown (1280x720, 1920x1080, 2560x1440, 3840x2160, Custom), mode
  control (Canvas Only in V1), a painted warning stating exactly what will
  happen, Cancel + filled Resize Project.

TWO DEFECTS/HOLES RECORDED NOW, BEFORE ANY RESIZE WORK:
1. THE CLIPBOARD DOOR — a defect TODAY, independent of resize: Storyboard
   panel Copy/Paste goes through StoryboardPage::clonePanel, a deep copy
   that keeps the source panel's layer sizes. Since 3a the runtime truth is
   Panel::canvasSize() per panel and nothing in the model enforces one size
   per project, so pasting a panel copied at another size (pre-resize in
   future; across projects of different sizes today, via the session-held
   clipboard) re-creates a MIXED-SIZE project through a door nothing guards.
   No family in the seven-family gate catches it. Fix direction: paste must
   conform (refuse, or re-canvas the clone to the project size) — decide
   with Part 2.
2. THE RECONCILE'S SILENT MIXED-SIZE HOLE — ProjectIO::projectFromJson
   takes pixelSize from the FIRST valid panel and checks it only against
   the manifest; it never verifies that ALL panels share one size. A
   partially resized (or clipboard-mixed) project therefore LOADS SILENTLY
   at the first panel's size with the others still at theirs — no dialog,
   no refusal. Closing this (a load-time uniformity check with a plain
   report) is a PREREQUISITE for resize, not part of it.

## Dev Recorder: MODAL BLIND SPOT (tooling gap, recorded 2026-08-22 — not a bug, not asked to be fixed)

Screenshot capture STOPS for the entire duration of every modal exec():
session 20260822-133926 (five opens of Project Settings) has zero frames
of the dialog — captures exist only in the windows when it was closed.
statePoll carries no project name or fps fields. So a recording can show
that a dialog interaction RAN (menu, show, presses on named widgets,
dropdown popup show/hide, close) but NOT what values it produced. This
affects all four frameless dialogs (New Project, Project Settings, and
the two studio surfaces) and any future modal. The two changes that would
close it: (1) keep the screenshot timer capturing while a modal event loop
runs (grab the active modal window, not only the main window); (2) add
projectName / fps to the state poll. Until then: that session confirmed
dialog MECHANICS only — the applied fps value was never visible to the
recorder, and the (archived) Part 1 seam plus SankoCanvasSizeLock section
(g) remain the only evidence for VALUES.

CORRECTION (2026-08-24): this entry originally ended "Recorder noise to
ignore when reading such sessions: 88 windowActivate/windowDeactivate
records per dialog show/hide (one per child widget)." THAT WAS NOT NOISE,
IT WAS A DEFECT, and calling it noise is why it sat unfixed for two days.
Qt delivers the WINDOW-SCOPED events — WindowActivate, WindowDeactivate,
WindowBlocked, WindowUnblocked — to EVERY widget inside the window, and
the recorder's event filter only excluded non-windows for Move, Resize,
Show and Hide. The same missing filter later produced 388 modalOpen
records for a single dialog, each running a full Win32 desktop z-order
walk. Fixed by extending that one condition to all four window-scoped
types; SankoDevRecorderTest now asserts one modalOpen per modal and fails
(9 records, 8 of them from child buttons) if the filter is removed. If a
future session shows a burst of per-widget records for a window-scoped
event, read it as this defect returning, not as noise.

## Drag-by-header on the frameless dialogs (2026-08-22)

Recording 20260822-133926 showed three presses on the Project Settings
dialog's top edge in its first 1.5 s — someone trying to move a window
that could not move, and a dialog that cannot move can cover what the
artist is looking at. Both frameless dialogs (New Project, Project
Settings) now drag by their HEADER BAND only, via the shared
src/FramelessDialogDrag.h (HeaderDrag): the band is QRect(0,0,width,
kFormY) — the strip above the first field holding the painted section
headers and their rules; the first control sits at kFormY+16, so the band
never overlaps a control and presses on controls or on the body never
start a drag (a drag-from-anywhere dialog turns every mis-click into a
window move). The frame is clamped to the available geometry of the
screen it is on, so dragging toward an edge stops at the edge. Behaviour
only — no title bar, resize grip, or chrome the design does not show.
Seam (archived: tests/_backups/seam_dialog_drag_20260822.cpp): header
drag moves by exactly the delta (positive control first), body drag and
control drag do not move, edge drags stay fully on screen — 13 checks x
10 runs across both configs. Seam-driver lesson: synthetic drag events
must carry ABSOLUTE global positions from a fixed start; deriving globals
from the moving dialog re-adds each step and the drag runs away.

## Multi-monitor drag: coverage rule + pointer-screen fallback (2026-08-22)

DEFECT (from the drag-by-header work, same day): the clamp used
dialog->screen()->availableGeometry() — the screen the dialog was ON — so
every shared edge was a wall; reproduced on this two-screen machine
(Cintiq 0,0 primary; Dell -1920,0; same height; both DPR 1): a header drag
of -1449 px toward the Dell stopped at x=0.

RULE NOW (FramelessDialogDrag.h, behaviour only): "fully visible" across
screens is PIXEL-WISE COVERAGE — every pixel of the frame lies on some
screen's available area (QRegion(frame) - union(availableGeometry) is
empty). If the unclamped target is covered it is accepted (straddling a
full-height shared edge is free — no jump); if not, clamp into the screen
the POINTER is on (QGuiApplication::screenAt), else the dialog's current
screen, else the primary — never a null screen. Consequences per case:
same-height edge: seamless crossing; mismatched heights: stops at the edge
while the pointer is still on the tall screen, then ONE bounded snap to a
fully-visible position once the pointer is on the short screen (the
geometry forces it; accepted as least surprising); L-shape / dead space:
never reachable (uncovered); pointer off the desktop / in the taskbar
strip: fallback clamp into a real screen.

VERIFIED on this hardware (seam archived:
tests/_backups/seam_multimon_20260822.cpp; 27 checks x 10 runs, both
configs, both dialogs): the drag CROSSES the shared edge and lands on the
other screen; the frame is fully covered at EVERY step; it straddles the
edge mid-way (free crossing); the reverse crossing; outer-edge clamps at
the desktop's left/right/top/bottom extremes; the NULL-SCREEN fallback
really executed (pointer at y=-500 — QGuiApplication::screenAt == nullptr
asserted first — no crash, frame covered, clamped to the current screen's
top); the dead-space-inside-a-screen fallback really executed (pointer in
the taskbar strip — inside geometry, outside available — asserted first;
frame covered, bottom clamped). Seam-driver lesson: synthetic pointer
paths must be REALISTIC — a real pointer cannot leave the screens, so
edge-clamp drags slide along the desktop edge; a pointer sent to y=-3000
correctly hits the null-screen fallback and never crosses, which the
first version mis-read as a clamp failure.

UNVERIFIED — NO SUCH HARDWARE HERE, and a synthetic layout would prove
nothing about Qt's real screen handling. Each must be checked by hand on
a matching arrangement before the drag is called verified there:
  1. MISMATCHED HEIGHTS (partial shared edge): expect stop-at-edge until the
     pointer crosses, then one bounded snap; never into the dead strip.
  2. L-SHAPED / NON-RECTANGULAR arrangement with dead space: expect the
     notch to be unreachable.
  3. MONITOR DISCONNECTED while a dialog sits on it: expect the OS to
     relocate the window, dialog->screen() possibly null for a moment, the
     helper falling back to primary, the next drag clamping into a live
     screen.
  4. MIXED DPR per monitor: expect logical coordinates to stay consistent
     across the boundary and the fixed-size dialog to re-render at the new
     DPR without misplacement; frameGeometry().size() is re-read per move.

## Modal dialogs vs the floating tool windows (2026-08-22)

REPORTED: in the real app, the Project Settings dialog's Canvas VALUES
(resolution, aspect ratio) looked blank while the labels and layout showed
normally. Investigation could not reproduce blank values in ANY data path
— bare dialog, app stylesheet, real MainWindow parent, five real projects
(legacy 960x540, legacy no-keys, 4K, zero-scene), 777x1013, 150% display
scaling, after a window move, and finally through the REAL slot
(MainWindow::onProjectSettings on a really-opened project, captured from a
timer while exec() blocked). In every case the values are computed AND
painted.

What WAS found and proved with captures: the storyboard's floating bars
are Qt::Tool top-levels owned by the main window, and NOTHING kept them
behind a modal dialog. A capture shows the brush toolbar and the Brush
Library painted straight ACROSS the Project Settings dialog; an overlap
audit logged BrushLibraryPanel at 638,291 360x420 intersecting the dialog
rect in a real session. That produces exactly the reported symptom SHAPE —
part of a dialog unreadable while the rest shows — without any state
corruption. It is NOT confirmed as the specific sighting (in the layout
captured at 16:03 the library sat over the dialog's LEFT half, which would
hide labels rather than values), so the door is left open.

FIX: StoryboardPage watches its TOP-LEVEL window for
QEvent::WindowBlocked/WindowUnblocked — the generic signal for any modal,
including message boxes and dialogs that do not exist yet — and
suppresses/restores the floating bars through the SAME mechanism the Brush
Settings studio already used. Enumeration is the manager's REGISTRY
(FloatingToolWindowManager::windows()), not a hardcoded list, so a bar
added later is covered by construction. Suppression captures INTENT, so a
bar the user had closed stays closed, and nothing is persisted.
suppressFloatingBars/restoreFloatingBars became a per-anchor STACK (was
first-capture-wins): holders nest, so a modal opening over the Brush
Settings studio hides what the studio left visible and closing it hands
the studio back exactly what it had. Both walks now batch placement and
re-pack ONCE at the end instead of per bar.

VERIFIED (seam archived: tests/_backups/seam_modal_bars_20260822.cpp):
bars visible before (positive control), ALL hidden during the modal,
restored exactly after OK / Cancel / Escape, a deliberately closed bar
still closed afterwards (with a control proving the checker can tell
closed from open), three sequential modals, and nested modals where
closing the inner one must NOT restore while the outer is still open.

KNOWN RESIDUAL — placement, not state: after RAPID repeated modal cycles
the managed group sometimes settles 13 px to the right with the brush bar
one slot (48 px) lower. Measured, not guessed: the delta is identical
every time, an idle-cycles control with NO modal never drifts, and an
anchor audit proves the CANVAS DID NOT MOVE — so the bars are not
following their anchor; managed placement simply packs a hide-all ->
show-all cycle into a different arrangement. Visibility, sizes and stored
user offsets are always exact; only the auto-placed position moves.
Batching the re-pack reduced it (2/10 -> ~1/6 of runs) but did not remove
it. This is in the managed-placement system, not the suppression
protocol, and is left as a separate item. NOTE for whoever picks it up:
the earlier "5/5 clean" readings were an artifact of snapshotting before
the group settled — always let placement settle before comparing.

## Dev Recorder modal capture — HIGHEST-VALUE TOOLING FIX (2026-08-22)

Raised from "gap" to "highest value" by the investigation above: the
recorder stops screenshot capture for the whole of every modal exec(), so
a dialog defect the user can SEE cannot be photographed. This entire
investigation — a full day of harnesses, layered reproductions and
captures — would have been ONE SCREENSHOT if the recorder had captured
the active modal window. Do this before the next dialog bug: (1) keep the
screenshot timer capturing during modal event loops, grabbing
QApplication::activeModalWidget() when one exists (note grabWindow(winId)
returns BLACK for the translucent frameless dialogs — grab the composited
desktop and crop in the GRAB's pixel space, not logical coordinates);
(2) add the dialog's seeded values (project name, fps, canvas size) to the
state poll; (3) log top-level geometry + z-order when a modal opens, which
would have named an obscuring floating panel immediately.

## Dev Recorder: modal blind spot CLOSED (2026-08-22)

ROOT CAUSE, in code, not inferred: the screenshot timer's privacy guard
read `!d->window->isActiveWindow()` and returned. A modal dialog TAKES
activation from the main window, so that one line skipped the screenshot
AND the state poll for a modal's entire lifetime. It was by design (keep
foreign windows out of captures), not an effect of the nested exec()
loop — timers keep firing in a nested loop. Confirmed against the
20260822-133926 session arithmetic: 37.7 s at 500 ms would be ~75 polls;
dialogs were open ~22.8 s, leaving ~15 s ~= 30 — the summary recorded 29.

FOUR CHANGES:
1. CAPTURE DURING MODALS. The guard now passes when the main window is
   active OR one of OUR modals is active. A foreign app with focus still
   suppresses capture (then neither is active) — asserted in the seam.
2. CAPTURE REGION. Still the app window's frame, which already CONTAINS a
   centred dialog, so the privacy bound is unchanged in the common case.
   Only when a dialog has been dragged clear of the window (possible since
   drag-by-header, including onto the other monitor) is the region united
   with the modal's frame, clamped to the screen, and a
   "captureRegionExtended" record notes it (with modalClipped when the
   dialog is on another screen).
3. MODAL STATE. statePoll now carries modal.class/name/title/geom/
   appModal/insideApp, plus whatever the dialog chooses to publish through
   an OPTIONAL invokable — Q_INVOKABLE QVariantMap devrecState() const —
   invoked BY NAME, so the recorder keeps its host-agnostic promise and any
   dialog opts in with a few lines. ProjectSettingsDialog publishes
   projectName, fps, canvasW/H, resolutionText, aspectRatioText,
   validationReason and the button enable states; NewProjectDialog
   publishes its form values. Those two *Text fields are exactly what
   round 1 of the blank-values investigation could not confirm.
4. GEOMETRY + Z-ORDER. A modalOpen record (from QEvent::WindowBlocked,
   the generic signal) enumerates every visible top-level with geometry,
   whether it overlaps the modal, and a z-index from a Win32 GetTopWindow/
   GW_HWNDNEXT walk (guarded by Q_OS_WIN, consistent with the existing
   working-set code); windowHandle() is used rather than winId() so the
   audit cannot force a native window into existence and change what it
   measures. Move/Resize/Show records now carry the FULL frame rect: they
   previously carried position OR size only, so a panel that moved during a
   modal could not be reconstructed. A capture + state poll also fire
   immediately at modal open rather than up to an interval later.

CAPTURE METHOD, for whoever revisits it: the recorder already grabs the
composited SCREEN REGION (grabWindow(0, x, y, w, h)), which is the only
approach that can see a window drawn OVER a dialog — the defect class this
exists to catch. grabWindow(winId) returns BLACK for the translucent
frameless dialogs and cannot see overlapping windows; QWidget::grab()/
render() renders the widget's own painting and is likewise blind to
overlap (fine for "did we paint it" unit checks, useless here); a
full-desktop grab would see everything but breaks the privacy bound.

OVERHEAD, measured rather than assumed. The capture primitive costs
~16.6 ms and is INDEPENDENT of region size (1600x900, 380x352 and
1700x950 all ~16.6 ms), so extending the region is free. Using the
recorder's own 10 ms probe: Release baseline worst gap 12-23 ms vs modal
STEADY STATE 20-35 ms across five runs — at parity within noise. Debug
showed 37-45 ms against a 16-19 ms baseline on 2 of 5 runs.

THE DEBUG NUMBERS ARE ACCEPTED, WITH REASON — do NOT re-open them as a
defect. Recording is done in Release (the recorder's own summary tells
you to read timings against the build profile); the capture primitive
costs the same in both configs (~16.6 ms, measured, size-independent);
and Debug's baseline is doing more work per frame regardless, so a wider
worst-gap there says something about Debug, not about modal capture. The
500 ms interval was kept deliberately and was NOT lengthened for modals.

Seam archived: tests/_backups/seam_devrec_modal_20260822.cpp — 16 checks,
each colour signature with a positive control (no accent pixels before the
dialog exists; no obscuring colour before the obscurer is shown), proving
the capture contains the dialog AND shows a window drawn over it.

## First Project Settings open costs ~344-414 ms of UI thread (2026-08-22)

Surfaced while measuring recorder overhead, and recorded here so it does
not stay buried in that report: constructing and first-showing
ProjectSettingsDialog costs 344-414 ms on the UI thread the FIRST time in
a process (five measurements, Release). Measured with the DEV RECORDER
OFF, so it is the app's own construction cost, not instrumentation: font
resolution, the shared StudioControls (StudioTextField's embedded editor,
StudioDropdown), and the first paint of a translucent frameless window.
Subsequent opens do not pay it — a later open in the same process
measured 17-39 ms.

Not urgent and not a correctness problem; the dialog is modal and opens
on a deliberate menu action. Worth a look if dialog-open latency ever
matters, and the same one-time cost very likely applies to the other
frameless dialogs (New Project pays it at the same place). Candidate
directions if it is ever taken up: warm the shared studio controls once
at startup, or construct the dialog lazily off the first paint. MEASURE
FIRST — this number came from a bare open with nothing else running, and
the previous performance passes both found that briefed numbers did not
survive contact.

## Mixed-size projects: the hole and the door, both closed (2026-08-22)

Both were LIVE defects in shipping code, not resize prerequisites in
waiting — and they were connected: the clipboard door manufactured the
files the reconcile hole then swallowed.

THE DOOR (reachable today, no resize needed): the panel clipboard
survives a project switch — m_panelClipboard is cleared only in the
destructor and on the next copy, never on load. Copy a panel in a
960x540 project, open a 1920x1080 project in the same session, paste, and
the project now holds two sizes. Verified by reading every panel-creating
path: blank panels use the project size (MainWindow / StoryboardPage) and
Import Image builds its layer at the panel's own canvasSize() and fits
the artwork into it, so the clipboard was the ONLY door.

THE HOLE: projectFromJson took the first valid panel's size and broke out
of both loops, never looking at the rest; `mismatch` compared manifest vs
that size only. Two outcomes, one worse than reported: with the odd panel
NOT first, total silence; with it FIRST, the project's whole nominal size
silently became the odd panel's on reload AND the mismatch dialog fired
with a false claim ("its artwork is 960 x 540" when one panel was) plus a
promise to "correct" the file by writing an accident of ordering.

FIXES. The load now takes a census of EVERY panel and picks the MAJORITY
size, ties to the first seen: "first" was an accident of ordering that
would otherwise become the project size and spread to every new panel,
while for a uniform project the majority IS the first, so the ordinary
path cannot move. Disagreement is reported, never repaired —
mixedSizes/majorityPanelCount/offSizePanels carry the counts and the
locations, and mixedCanvasSizesDialogText names the majority, lists the
dissenters as "Scene N, panel M", states that no artwork is modified, and
says exactly what the next save writes. A mixed project takes precedence
over the plain mismatch message, which must never be shown for one.
THIS SITS BESIDE PIXELS-WIN AND DOES NOT CHANGE IT: pixels-win governs
manifest vs artwork; this governs artwork vs artwork. Artwork is still
never rescaled, cropped, or discarded.

The paste is REFUSED when the clipboard panel's size differs from the
project's, naming both sizes and saying nothing was changed. Refusal, not
adaptation: scaling and resampling change the pixels, and centring crops
whenever the target is smaller, so refusing beats quietly altering
someone's drawing. The clipboard is deliberately NOT cleared on load —
the same-size cross-project paste is legitimate and clearing it would
hide the reason. The check lives in the two paste slots, NOT in
insertPanelClone, because duplicating an already-odd panel inside an
existing mixed project introduces no new size.

GATE — there was none before; SankoCanvasSizeLock section (h), 11 checks
(family now 101): detection fires on a mixed project and stays quiet on a
uniform one; majority beats first when they disagree; the dissenter is
located; the message is true of a mixed project; a uniform load is
unchanged in size, manifest and mismatch verdict (the check that proves
the normal path did not move); and the paste refusal fires on a mismatch
in both directions with a POSITIVE CONTROL first proving the matching
case is not refused. The decision itself lives as a free function,
pasteWouldMixSizes(panel, size) in StoryboardModel.h, so the gate asserts
the REAL function on REAL panels rather than a restatement.
COVERAGE CAVEAT, stated plainly: the refusal's dialog and its wiring into
the two paste slots are one call each and are covered by reading, not by
the gate — linking the whole StoryboardPage widget stack into this family
would cost more than it proves.

## Canvas resize V1 (Canvas Only, centre anchor) — IMPLEMENTED (2026-08-22)

Both prerequisites having landed, Part 2 is built. Canvas-only: every
panel's layers are rebuilt at the new size with the artwork re-anchored at
the CENTRE, 1:1, no scaling or resampling — expanding adds margin,
contracting crops symmetrically, surviving pixels byte-identical.

STAGING IS PER PANEL, NOT PER PROJECT — a correction to the recorded
design, made before building rather than after. The recorded plan staged
the whole project then swapped, peaking at old+new (~10 GB for the
50-panel 4K scene); the same HANDOFF records that the probe OOM-CRASHED at
~5 GB, so that plan doubled a footprint the app already cannot hold. Each
panel now stages its own layers and swaps only when all of them allocated,
releasing the old images as it goes: peak is the larger of the two full
sets plus ONE panel (~100 MB at 4K), a 1.02x peak instead of 2x.

Per-panel staging cannot make the project-wide operation infallible by
itself, so atomicity is bought two other ways: a PRECHECK before anything
is touched (a refusal at that point is perfectly atomic), and the save
prompt in front of the confirm, whose file is the rollback. A partial
resize is never silent — the panel census added with the mixed-size fix
reports it and names the panels.

PRECHECK. Needs = max(0, newTotal - currentTotal) + largest single panel
at the new size. Safety factor: the requirement is DOUBLED and a 512 MB
reserve held back, checked against GlobalMemoryStatusEx ullAvailPhys
(physical only — winning the check by counting pagefile would trade a
crash for thrashing). The x2 covers what the resize TRIGGERS but does not
itself allocate (composite caches, a flatten and thumbnail per panel the
strip rebuilds, engine surfaces) plus allocator fragmentation at 33 MB
blocks; the reserve keeps the machine alive and absorbs whatever another
process takes between check and work. BOTH NUMBERS ARE CHOSEN, NOT
MEASURED, and deliberately conservative because the failure they prevent
is a crash mid-resize. The refusal states what is needed AND what is
available, and that nothing was changed. Where memory cannot be measured
(non-Windows) the precheck does NOT guess: it allows only up to 512 MB.

STATE. Cleared: selection path, panel clipboard (it holds old-size panels;
left alone the new paste guard would refuse the user their own project's
panel), undo stack (every canvas-space command is invalid). TRANSLATED,
not cleared: perspective vanishing points — canvas-space geometry under an
exactly known offset, and user work. Explicitly refreshed: strip labels
and panel thumbnails. Self-healing but invalidated anyway: composite
caches, flattened thumbs, the engine mirror; the view re-fits because
m_zoom is fit-relative, with the pan offset reset.

IN-FLIGHT: REFUSED, never committed — committing an unfinished stroke or a
half-placed transform writes pixels the user never chose to keep, into an
undo stack the resize then clears, so it could not be taken back. The
message names what is active. Note for maintainers: the brush's in-flight
flag is m_brushStroke, the ERASER's is m_drawing, and Quick Shape rides on
a brush press — checking only m_drawing (the first attempt) missed brush
strokes entirely, which the gate caught.

ORDER, and why: refuse in-flight -> size prompt -> precheck -> save prompt
-> confirm -> per-panel swap -> members updated -> page state -> undo
clear. The members MUST be updated before anything can save, or a save
would write the old manifest against new pixels — a mismatch of our own
making.

GATE: SankoCanvasSizeLock section (i), 36 checks (family now 136), at
960x540 -> 1920x1080, 3840x2160 -> 1920x1080 (the cropping direction) and
1920x1080 -> 1080x1350 (non-16:9, both axes moving different ways):
surviving artwork BYTE-IDENTICAL at the centre offset WITH a positive
control proving the comparison detects a one-pixel change; groups keep
null images, background margin white, others transparent (the three that
are invisible in the flatten); layer ids survive; the census finds one
distinct size and the reload is neither mixed nor mismatched; VPs land at
exactly the translated positions; the precheck passes an ordinary resize
and measures real memory; and the in-flight refusal fires for a brush
stroke, a Quick Shape and a transform, behind an idle control.
Storyboard-side state (clipboard dropped, selection cleared, VPs
translated, size updated, undo emptied) is proven by an archived seam,
tests/_backups/seam_resize_state_20260822.cpp, 14 checks x 10 runs — each
with a control proving the state existed BEFORE the resize.

UI: ResizeProjectDialog (presets 1280x720 / 1920x1080 / 2560x1440 /
3840x2160 / Custom, width + height fields, lock-aspect on the CURRENT
project's ratio, live effect text) in the painted studio idiom, opened
from the now-enabled "Resize Project..." in Project Settings. The crop
warning appears ONLY when something is actually cropped — a warning shown
for a harmless expansion teaches people to ignore warnings.

STILL V1: Scale Artwork remains V2. Nothing here scales or resamples.

## The File > Open crash: three dangling-pointer defects, and the blind spot that hid them (2026-08-22)

REPORTED as "with a project already open, File > Open closes the app",
suspected to be the resize work. IT WAS NOT. Bisect, with a symbolised
stack from a real MainWindow driven through the REAL loadFromPath:
identical crash at 9f7de82c6 (resize), at 73c1ce6e3 (before resize) and at
9dfef7557 (before the whole day's work). The faulting line arrived on
2026-07-27 in 80018f681. Nothing in the four commits of that day touched
setActivePanel, freeScenes or the load ordering.

ROOT CAUSE, one shape three times: MainWindow::freeScenes() deleted every
Scene (and its Panels) while other objects still held NON-OWNING pointers
into them.
  1. DrawingCanvas::m_panel — the reported crash. The canvas kept pointing
     at a deleted panel for the whole of the following load, and
     setActivePanel then READ it: invalidateComposite() -> canvasSize() ->
     m_panel->canvasSize(). The `m_panel ?` guard does not help; the
     pointer is not null, it is dead. It faulted only when the freed memory
     had actually been reused, which is why it looked intermittent.
  2. AnimaticTimeline::m_scenes — loadFromPath calls m_animatic->setFps()
     AFTER freeScenes(), and setFps rebuilds the timeline by walking the
     scene list. The animatic otherwise reloads only when the user
     navigates to it, so the stale list survives until then. It fires only
     when the new project's rate DIFFERS (setFps early-returns otherwise),
     which is why opening same-rate projects looked safe.
  3. The canvas AGAIN, via File > New: found by the new family, not by a
     user. freeScenes' first fix called m_storyboard->loadScenes({}), but
     with an EMPTY list that stops at rebuildPanelStrip() and NEVER reaches
     setActivePanel, so the canvas kept the dying panel. loadScenes({}) IS
     NOT A DETACH - do not use it as one.

FULL AUDIT of every Panel*/Layer*/Scene* holder, since the ask was to
enumerate rather than fix a list nobody had seen:
  DEREFERENCED (fixed): DrawingCanvas::m_panel; AnimaticTimeline::m_scenes;
    AnimaticPage::m_scenes + its m_items rows; GenerationPage::m_scenes +
    rows; StoryboardPage::m_scenes; DrawingCanvas::m_editPanel (read at
    DrawingCanvas.cpp:3789).
  COMPARED ONLY, never dereferenced, but still hazardous because a NEW
  panel allocated at a recycled address reads as "the same panel" and a
  stale cache or selection is kept: DrawingCanvas::m_compPanel;
  StoryboardPage::m_layerSelPanel. Both are now nulled.
  SAFE, confirmed: StoryboardPage::m_panelClipboard is an OWNED deep copy,
  independent of any scene.

FIXES. (1) Root, inside freeScenes() rather than at its four call sites
(loadFromPath, onNewProject, buildScenesFromJson, ~MainWindow) because a
fifth caller would have to remember: detach every page BEFORE deleting.
StoryboardPage::detachScenes() drops the canvas's panel through
setActivePanel(nullptr) — which also runs the canvas's leave-handling
(commit an in-flight quick shape, floating paste or transform) while the
outgoing panel is still ALIVE, the only moment that is safe — and clears
m_layerSelPanel. (2) Defence in depth in setActivePanel: it no longer
dereferences the outgoing panel at all; the cache flags are cleared
directly and the invalidated rect describes the panel being switched TO.

WHY NOTHING CAUGHT IT. Two independent gaps. SankoCanvasSizeLock's
cross-size switch calls setActivePanel(small) then setActivePanel(tall)
with BOTH panels alive and owned by the test — it exercises the switch and
never the use-after-free, so it passed truthfully while the app died. And
nothing in the suite had ever constructed MainWindow, so loadFromPath, the
free-then-load sequence, and onNewProject had ZERO coverage — the same gap
that made the Project Settings investigation expensive, where
onProjectSettings had never executed under test either.

NEW EIGHTH FAMILY: SankoProjectLifecycle (tests/ProjectLifecycleTest.cpp),
13 checks: open, open again at a different size AND rate, a third open,
re-open the same project, load then File > New then load again, and six
consecutive opens alternating both (a use-after-free faults only when the
freed memory is reused, so one switch can pass over a broken build).
PROVEN NON-VACUOUS: with the detach disabled it CRASHES rather than
passing. Its failure mode IS a crash, so it installs an
unhandled-exception filter that prints a symbolised stack, and it flushes
after every check — a gate failure names a faulting line instead of dying
silently.

COSTS, measured, since this is the first family to link the whole
application: build 55 s Release (it compiles the app's ~120 sources a
second time; the exe is 2.5 MB), run 9.0 s Release / 13.4 s Debug — well
under SankoCanvasEdgeLock's 41-43 s. It needs a GUI session, like
EdgeLock and SizeLock already do. It does NOT touch real state: QSettings
is redirected to INI under a scratch root, QStandardPaths is in test mode,
and NewProjectDialog::setSettingsOverride points the recents store into
scratch too (loadFromPath records a recent project on EVERY successful
open, so without that override a test run would edit the user's list).
Verified: no scratch left behind, no real registry key written.

THE GATE IS NOW EIGHT FAMILIES, not seven.

## Unsaved-changes tracking (2026-08-22)

Groundwork for the save prompts on New/Open/Close/Exit, shipped ALONE and
FIRST: a prompt that misses a change type tells the artist their work is
safe and then discards it, so the flag had to be trustworthy before
anything asked it a question.

NO DIRTY FLAG EXISTED. And the undo stack could not stand in for one: it
carries SelectionCommand (selecting is not a document change), it has a
60-command limit so its clean index can fall off the bottom and never
return, and several real edits never reach it at all — Shot Info fields
and panel durations are written straight to the model, and frame rate,
project name and canvas resize are the window's own.

MECHANISM, two halves.
  1. UNDO-STACK BACKSTOP. MainWindow watches QUndoStack::indexChanged, so
     every undoable change marks the project dirty BY CONSTRUCTION —
     strokes, fills, erases, pastes, transforms, layer stack edits, panel
     add/remove/move, perspective — INCLUDING command types added later.
     Enumerating them by hand would have aged badly; the risk being guarded
     against is a change type nobody wired. One exclusion:
     SelectionCommand::id() returns a sentinel the watcher skips, because a
     marquee drag must not claim there is work to save. It is identified by
     id, not by matching its text.
  2. THREE NEW SIGNALS for what the stack cannot see, one per page that
     mutates silently: StoryboardPage::documentChanged (Shot Info, 1 emit
     site), AnimaticPage::documentChanged (duration drag + audio track
     chosen + audio removed, 3 sites — one MORE than the 2 estimated when
     planning, because REMOVING the track is as much an edit as adding it),
     ConsistencyBoard::documentChanged (add, delete, edit — 3 sites, as
     estimated). AnimaticPage::setAudioPath deliberately stays SILENT: it
     is how a project LOAD installs its track, and adopting a saved path is
     not an edit.
  Plus three direct markDirty() calls the window owns: applyProjectSettings
  (name + fps), the resize, and a Script Editor re-parse.

THE ORDERING TRAP, which would have silently ruined the feature: opening a
project fires the very signals that mark it dirty (panels selected, canvas
publishing, pages rebuilding). setClean() is therefore the LAST thing
loadFromPath and onNewProject do, and the gate asserts a freshly loaded
project is clean rather than checking the flag at some arbitrary later
moment. Anything added below those lines that touches the model must clear
the flag again.

isDirty() is public and pure, so the prompts can be tested as a DECISION
rather than by driving a modal (the pasteWouldMixSizes pattern). The title
carries [*] via setWindowModified.

REFACTOR that rode along: the resize's post-dialog work moved out of
onResizeProject into applyCanvasResizeInternal, so there is ONE definition
of what a resize does and the gate can drive it without answering four
modal dialogs.

GATE: SankoProjectLifecycle section (d), 16 checks (family now 29, 15.9 s
Release / 21.6 s Debug). Every change type proven INDIVIDUALLY — each
starts from clean, makes exactly one change, and asserts dirty — because
a check that marks dirty via one path and declares the flag working is the
vacuous shape here. Types: canvas stroke, the undo backstop, Shot Info,
panel duration, frame rate, project name, resize, consistency entry. Then
save clears it, New clears it, and two controls: doing nothing leaves it
clean, and A SELECTION CHANGE DOES NOT MARK DIRTY. That last one is
load-bearing and was PROVEN so, not assumed: with the id exclusion removed
it is the only check that fails.

One thing the gate taught while being written: the animatic holds NO
scenes until the user navigates to it, so a duration change on a
just-loaded project is a no-op. The check now clicks the real "Continue to
Animatic" button first, or it would have passed vacuously.

NEXT PASS (not built): File menu reorganisation — New / Open / Open Recent
/ Close Project / Save / Save As / Project Settings / Exit, with the
unsaved prompts on New, Open, Close and Exit, closeEvent as the SINGLE
prompt implementation (a prompt from the menu and silent loss from the X
button is worse than no prompt), Open Recent reusing the Open path rather
than duplicating load logic, and lifecycle sections (e) close-state and
(f) close-then-open/new.

## File menu reorganisation: New / Open / Open Recent / Close / Exit (2026-08-22)

Menu is now New Project (Ctrl+N) / Open Project (Ctrl+O) / Open Recent >
/ Close Project (Ctrl+W) / --- / Save (Ctrl+S) / Save As (Ctrl+Shift+S) /
--- / Project Settings / --- / Exit.

ONE TEARDOWN. resetProjectState(ClipboardPolicy) serves New, Open and
Close; they differ only in what they do AFTERWARDS (New applies the
dialog's values and goes to the Script Editor, Open the file's and goes to
the Storyboard, Close nothing and goes to the Dashboard). No parallel
reset logic exists.

PRE-EXISTING DEFECT FIXED IN PASSING: the old onNewProject UNDER-CLEARED.
It never cleared the undo stack, the panel clipboard or the perspective
vanishing points, so starting a new project inherited the previous one's
construction lines and an undo history describing panels that no longer
existed. This predates the pass. Open escaped it by accident
(perspectiveFromJson overwrites the VPs, loadScenes clears the stack),
which is why only New showed it.

THE CLIPBOARD ASYMMETRY IS DELIBERATE. Close clears the panel clipboard
and the canvas clipboard; New and Open keep them. Pasting a panel into the
next project is a real workflow and the size guard already refuses a
mismatch, but after Close there is no project to paste into and the
clipboard would hold artwork from one the artist explicitly closed. That
is why ClipboardPolicy is a parameter rather than a fixed behaviour.

THREE THINGS NEEDED MORE THAN CLEARING, per the audit: the canvas
clipboard had NO clear API at all (added DrawingCanvas::
clearCanvasClipboard); the vanishing points needed a call, not a field
(PerspectiveTool::reset already existed and does exactly the right thing);
and the project canvas size resets to the documented 960x540 PRE-PROJECT
IDLE values rather than an invalid QSize, which any panel-making path
would build garbage from. One audit item turned out to need NOTHING: the
Dashboard's project cards are hardcoded placeholders, not a live recents
list, so nothing there goes stale on close.

ONE CLOSE PATH. closeEvent holds the only unsaved-changes prompt; X,
Alt+F4, the taskbar and a Windows shutdown all arrive there, and File >
Exit does nothing but call close(). Cancel calls event->ignore(). Alt+F4
is shown as TEXT and deliberately NOT bound — Windows already delivers it
as a close request, and binding it (or adding Ctrl+Q) would put a second
close path in front of the one that has to stay correct.

THE SAVE-THAT-DID-NOT-HAPPEN HOLE, closed: onSaveProject returns void, so
a Save As the artist cancels, or a write that fails, was
indistinguishable from success. saveForPrompt() returns whether the save
ACTUALLY happened, and mayDiscardAfterAnswer(Save) returns it — so a
failed save cancels the transition instead of proceeding to discard the
work the artist just asked to keep. That failure would have arrived
THROUGH the prompt that exists to prevent it.

DECISION vs MODAL: shouldPromptToSave() is pure and public, and
mayDiscardAfterAnswer maps answer -> consequence with no dialog in front,
so the gate asserts the decision instead of driving a QMessageBox.

GATE: sections (e) and (f), family now 54 checks. Close-state asserts
every audited item with a control proving the state EXISTED first
(clipboards, a vanishing point, a non-empty undo stack, a dirty flag);
close->open and close->new; and the decision table including the
save-that-failed case, driven WITHOUT a modal by removing the project's
folder so the write genuinely fails. Note for whoever runs it: the close
state checks clear the dirty flag immediately before closing, because
Close now correctly PROMPTS and a gate cannot answer a modal — the real
close path still runs, the prompt is simply a no-op.

COST: the family is now the slowest alongside EdgeLock — 25-45 s Release
standalone (variance is real project I/O: PNG encode/decode per fixture
and per load), ~32 s Debug, and 215 s was observed once when run
immediately after the other seven on a loaded machine. It was 9 s when it
held only the open/reopen checks.

## Three fixes the 20260824-200632 recording proved (2026-08-24)

The first session recorded with modal capture working, and it earned its
keep immediately: 11 screenshots and 11 state polls DURING a New Project
dialog, where before there would have been none.

1. NEW PROJECT WAS UNUSABLE, and the recording is the proof: every poll of
   the dialog's 5.1 s life carried createEnabled=false and
   validationReason="The save location does not exist." The default
   location (Documents/SankoTV) did not exist on that machine, so Create
   was disabled from open to close and nothing the artist typed could
   help. The diagnosis is an INCONSISTENCY, not a missing capability:
   attemptCreate already calls mkpath, which builds the whole chain
   including that folder — validate() simply refused to let it try, so the
   dialog blocked itself on work it was about to do anyway. Fixed by
   removing the "does not exist" rejection and probing WRITABILITY on the
   nearest ancestor that does exist (probing a folder that is not there
   can only fail), plus creating the default folder when the dialog opens
   so Browse does not start at a path that is not there. If creation
   fails: fall back to Documents, then to home. Open and Save As were
   checked and do NOT share the bug — both default to QDir::homePath(),
   which always exists; deliberately left alone rather than riding a UX
   change along in a bug-fix pass.

2. THE WINDOW-SCOPED EVENT STORM — see the CORRECTION added to the
   2026-08-22 modal blind spot entry above. One line, four event types.

3. THE GIT HASH LIED. system.txt named 9f7de82c6 while the binary was four
   commits later, because the hash was captured with execute_process at
   CONFIGURE time and only changed when CMake happened to rerun. Now
   captured at BUILD time by cmake/WriteGitHead.cmake into a generated
   header, rewritten only when the value changes so an unchanged HEAD
   recompiles nothing. It also appends "+dirty" when the tree has
   uncommitted work, and system.txt gained a "built:" timestamp — because
   a build from a dirty tree is not any commit, and a bare hash can never
   say so.

STALLS: seven gaps >=150 ms (worst 243 ms) against 55/45 ms in the two
previous sessions. NOT investigated and NOT attributed, deliberately. The
comparison is not like-for-like: those sessions were 26 s of dialog
clicking, this was 96 s of drawing on a 2048x1080 canvas ROTATED ~140 deg
at 50% zoom with a 152 px brush, and the user was actively drawing through
every stall (29-66 mouse moves in each stall second). Nothing in the
recent work touches the paint path. When these are measured it should be a
proper pass at those canvas settings, not an inference from a session that
was not designed for it.

## View state resets on every project transition (2026-08-24)

REPORTED as two defects: zoom and rotation surviving a project reopen, and
"leaking between projects" (change the view in SB_002 and SB_001 is
affected). Suspected to be global storage in QSettings.

NOTHING ON DISK WAS EVER WRONG. Zoom and rotation are not in the save
format — ProjectIO has no view fields at all — and not in QSettings; the
canvas's QSettings use is safe-area opacities and GRID settings only. The
single cause of BOTH symptoms: ONE long-lived DrawingCanvas serves every
project, and nothing reset its view when a project arrived. The "leak" was
that one canvas still showing a stale view, not a file being modified.
Nobody should go looking for a corrupted-file bug: there is none, and the
fix required no save-format change.

FIXED with a dedicated DrawingCanvas::resetViewForNewProject(): zoom back
to kStartupZoom (0.85), pan, rotation and horizontal flip cleared, plus
the per-drawing aids ONION SKIN and LIGHT TABLE, which carried across
projects the same way. Grid and safe-area guides are deliberately NOT
touched — they are app-wide preferences in QSettings, not project view
state, and the gate asserts they survive so nobody later folds them in.

NOT resetView(): that lands at zoom 1.0 (exact fit) and is the Reset
button's existing contract. Opening a project should look exactly like
launching the app, which is 0.85, so this is its own small function rather
than a reuse that would change a different feature's behaviour.

PLACEMENT CORRECTION, found by the gate: the reset went first into
resetProjectState, on the understanding that New, Open and Close all share
it. THEY DO NOT. Open goes through loadFromPath, which calls freeScenes()
DIRECTLY and never touches resetProjectState — so New and Close reset
while Open did not, and the gate caught it as "open: rotation back to 0
[45]". The reset now lives in freeScenes(), which is the teardown all
three genuinely share (the same reason the dangling-pointer detach lives
there). If anything else must happen on every project transition, that is
where it goes.

GATE: SankoCanvasSizeLock is untouched; SankoProjectLifecycle section (g),
29 checks (family now 83, 26.7 s Release / 34.8 s Debug). Every item is
asserted INDIVIDUALLY for EACH of the three transitions, because the whole
risk is one being left out, and each transition is preceded by a control
proving the view was non-default first. Two notes for whoever edits it:
zoom is driven by CTRL+wheel (the canvas ignores a plain wheel), and the
disturbance is kept minimal because onion skin + light table + rotation +
zoom together make every repaint expensive.

A PRE-EXISTING FLAKE was found and fixed while running this: section (e)'s
"a save that did not happen must not allow the transition" deletes the
project folder so the write fails — and a failed save legitimately shows a
QMessageBox warning, which blocks a headless run forever. It had passed
before only because the deletion sometimes left the save succeeding. The
check now dismisses whatever modal appears while the save is attempted, so
it exercises the real failure path (warning included) instead of hanging
on it.

## Save As shared one folder's pixels between projects (2026-08-24)

REPORTED as artwork appearing in the wrong project: open SB_001, Save As to
SB_002, paint in SB_002, and SB_001 shows the new paint. Real pixels, not a
stale view.

CAUSE, confirmed: panels and layers are written as image files named by
POSITION — panel_s0_p0_layer0.png, and pixmapFile panel_s0_p0.png in legacy
projects — with nothing in the name identifying the project, into whatever
folder holds the .sankotv. Two projects in one folder therefore write the
SAME FILES. Save As itself destroys nothing (both hold identical pixels at
that instant, which is why it hid); the next save of EITHER overwrites the
other's artwork permanently. Different folders are wholly independent —
proven by a probe that measured 0 of 15 files touched across folders and 2
of 15 rewritten within one.

MEASURED DAMAGE on this machine: 16 projects in 4 shared folders, of which
only 4 were intact — Test_SB_006, Test_SB_011, manion, project_006. The
pattern is exact: in each shared folder only the LAST-SAVED project
survived, because the last writer's pixels are the ones on disk.

A COUNTING MISTAKE WORTH REMEMBERING: the first damage count said nine
intact. It was wrong because it looked only at layers[].imageFile and
missed the LEGACY panel.pixmapFile spelling, so five legacy projects that
reference their art that way were scored as referencing nothing at all.
Any tool that walks project references must handle BOTH spellings; one that
does not will silently under-report, or "repair" a project into having no
pixels.

SUPERSEDED — see "Assets subfolder" (2026-08-27) below for the real fix,
and "The Save As guard removed" (2026-08-27) for why the warning described
here no longer exists. What follows is the state as of 2026-08-24.

SHIPPED SO FAR: a Save As guard warning when the target folder already
holds another project (commit "Warn before Save As...", REMOVED 2026-08-27
once the fix made its text false), and
tools/repair_shared_projects.py, which gives each project its own folder —
COPYING referenced images (never moving: several manifests name the same
file and the attribution question has no answer), verifying each copy by
SHA-256, moving the .sankotv only after every copy verifies, and deleting
nothing. All 16 are now in folders of their own, every referenced file
resolves, zero verification failures. The 12 damaged ones are FROZEN, not
restored; that art was gone before the repair.

## Tooling: the --root trap (2026-08-24)

Added to the vacuous-test/tooling catalogue because it is the same class as
the seam-driver bugs: THE TOOL DID SOMETHING OTHER THAN WHAT ITS OPERATOR
INTENDED, AND WAS BELIEVED BECAUSE THE OUTPUT LOOKED RIGHT.

repair_shared_projects.py took --root as an ADDITIONAL search root. Passing
--root <scratch replica> --apply to rehearse on a throwaway copy therefore
swept the REAL project folders as well, and performed the repair the user
had explicitly reserved for themselves ("I will run --apply myself"). The
outcome happened to match the reviewed plan and every copy verified, but
consent was not given, and a correct operation performed without it is
still a boundary crossed.

Three fixes, all in the tool: --root now REPLACES the defaults (the only
reading that cannot surprise); --apply additionally requires --yes, so a
stray argument cannot move files; and the summary counter, which reported
"0 projects moved" while manifests plainly moved, is fixed — a summary that
cannot be trusted is worse than none, because it is read instead of the
detail.

AND THE LOG OF THAT PASS WAS THEN DESTROYED. The log is written to the
run's WORKING DIRECTORY by default; that run's cwd was the scratch replica
folder, and clearing away the replicas afterwards deleted the only record
of what the real pass did to real files. So the per-file SHA-256 record of
the one repair that mattered no longer exists. The end state was
independently re-verified afterwards — 22 projects, every referenced file
resolving, no folder holding more than one project — but that is a
statement about NOW, not a record of what moved where.

Two lessons, both cheap: default the log somewhere that is not the cwd (or
refuse to write it into a directory the same command is about to remove),
and never let cleanup run over a directory whose contents have not been
accounted for.

RULE, generally: when a task says the operator will run the destructive
step, no amount of confidence that the step is correct transfers that
permission. Rehearse on data the tool CANNOT confuse for the real thing, or
do not rehearse.

## Assets subfolder — the real fix for the Save As data loss (2026-08-27)

THE ROOT CAUSE was positional image names in a shared folder. The fix
removes the sharing rather than the positional names: every project writes
its pixels into `<basename>_assets/` beside its own .sankotv, and the
manifest stores that relative path. Two projects in one folder now write to
two different directories, so neither can reach the other's files at all.

FOUR DECISIONS, each load-bearing:

1. NAMED FROM THE FILE, NOT THE PROJECT NAME. `assetSubdirFor()` uses
   `QFileInfo(path).completeBaseName()`. projectName is NOT unique and is
   not even tied to the file: `Test_SB_006.sankotv` on this machine carries
   projectName `Test_SB_007`. Naming the folder from data would have
   reproduced the collision with extra steps.

2. ENFORCED BY THE SIGNATURE. `projectToJson` now takes the project FILE
   PATH, not its folder (MainWindow passes `path`, not
   `QFileInfo(path).absolutePath()`). A folder parameter would leave the
   subfolder to the caller, and a caller that forgot — or that used
   projectName — puts two projects' pixels back in one place. The compiler
   now refuses the old call.

3. SAVE AS DOES NOT MOVE THE FILE THE USER CHOSE. Only the images go into
   a subfolder; the chosen .sankotv path is exactly the chosen path.
   Create keeps its folder-per-project as well, so there is ONE rule.

4. NOTHING IS DELETED, AND OLD PROJECTS ARE UNCHANGED ON LOAD. Names are
   stored relative to the manifest, so a project naming flat files still
   finds them exactly where it did. On its first save under this build it
   writes `_assets/` and LEAVES THE FLAT PNGS ALONE — another manifest in
   that folder may still reference them. Orphaned flat files are the
   user's to remove, never the app's.

GATE (SankoProjectLifecycle section (h), 14 checks, part of the permanent
97): Save As into the SAME folder, then edit the copy — the original must
be byte-identical; and THE REVERSE, edit the original — the copy must be
byte-identical. Both directions, because the bug was symmetric: whichever
project saved last clobbered the other, so a fix that isolated one side
would still lose work, and a one-direction check would have passed over it.
A POSITIVE CONTROL runs the same SHA-256 comparison across a real painted
stroke and asserts it DETECTS the change — otherwise "byte-identical" only
proves the comparison is blind. Then the migration path every existing
project takes: an old flat-named project loads, opens at the right size,
and its first save creates `_assets/` while its flat PNGs keep their
hashes.

SankoCanvasSizeLock section (d) needed updating for the new layout — its
on-disk checks globbed `*.png` beside the manifest and found zero files.
Worth noting that it FAILED rather than passing vacuously: the check
carried the file count in its own message ("0 files") and asserted the list
was non-empty, so the layout change surfaced as six failures instead of six
silent successes over an empty set.

FULL GATE, both configs, all eight families: PaintPixelLock, BrushLibrary
(preview SHA 193847fa... unchanged), CanvasBrushLock (666f7b45... /
cafcec7f...), QuickShapeGeometryLock, CanvasEdgeLock 74, CanvasSizeLock
136, DevRecorderTest, ProjectLifecycle 97 — zero failures.

## The Save As guard removed — a warning outlives its bug (2026-08-27)

confirmSaveAsLocation warned that saving into a folder holding another
project would make the two share their artwork files, in words chosen to
make the consequence unmistakable: "OVERWRITES THE OTHER'S ARTWORK",
"permanent", a "Save Here Anyway" button with DestructiveRole. Every word
was true when written. Two commits later the assets-subfolder fix made all
of it false, and the guard was left describing a bug that no longer exists.

REMOVED rather than softened. A warning that cries wolf is worse than no
warning, because the next real one gets dismissed by reflex — and there
was nothing true AND important left to say at that moment: two projects in
one folder is now a filing preference, not a hazard.

THE DECIDING CASE, and it inverts the guard's advice. A legacy project
that shares a folder and has not been re-saved still names flat files. Its
FIRST SAVE under this build is the thing that RESCUES it: the save writes
its pixels into <basename>_assets/ and leaves the flat files for whatever
else still references them. A dialog discouraging that save would now be
actively harmful. The guard did not just go stale — it inverted.

AND IT HAD NO TEST. Nothing in the eight families referenced
confirmSaveAsLocation or its buttons; it shipped as a dialog nothing
verified, which is also why nothing broke when it went. Worth remembering
next time a guard ships "just as a warning": an unverified dialog is not
a cheap safety net, it is untested code with a user-visible voice.

TEXT AUDIT DONE AT THE SAME TIME, because the ground moved under prose
written two commits earlier. Corrected: the dialog text, its call-site
comment, its declaration comment in MainWindow.h, the WHY THIS EXISTS
block in tools/repair_shared_projects.py (now past tense, and it says
plainly that migration happens by itself on the next save, so someone
finding that script in six months does not conclude their projects are in
danger), and the 2026-08-24 entry above, which read as current state.
LEFT ALONE deliberately: the comments in ProjectIO.cpp/.h and both test
families, which are already past tense ("used to write the SAME...") and
whose observation that the names are STILL positional is true and
load-bearing — the folder is what keeps them apart.

RULE: a fix that removes a hazard has a second half. Every warning,
comment, tool rationale and doc line that described the hazard is now
wrong, and prose does not fail a gate. Grep for the old description as
part of the fix, not as a later tidy-up.

## The save could fail silently and clear the dirty flag (2026-08-27)

EVERY WRITE IN THE SAVE WAS UNCHECKED: QDir::mkpath, all three
QImage::save sites, and QFile::write's returned byte count. saveToPath
then returned true UNCONDITIONALLY and called setClean(). And the manifest
was written with a plain QFile opened WriteOnly, which TRUNCATES ON OPEN —
QSaveFile was already used for the brush library and the recents file, but
not for the one file that holds the artist's work.

WHAT THAT LEFT ON DISK, and why nobody would notice:
 * mkpath blocked (a FILE where <basename>_assets must go, a read-only
   parent, a path too long) but the folder itself writable: a complete,
   well-formed manifest naming forty PNGs THAT WERE NEVER WRITTEN.
 * one image unwritable (disk full at file 17, antivirus or Explorer
   holding one file open): that layer named but absent.
 * the disk filling during the manifest write: a TRUNCATED, unparseable
   project file where a good one had been, the previous version already
   destroyed by the truncating open.
In all three the save reported success, cleared the dirty flag, and the
title's [*] disappeared — so the artist could close the app with no prompt
on work that never reached disk.

THE LOADER MADE IT INVISIBLE, CORRECTLY. projectFromJson deliberately
fills a missing image at the PANEL's size (a genuinely damaged old project
must still open), so the result came back at the right dimensions with
blank layers and no complaint. Right behaviour there, worst possible
failure signature here. NOT CHANGED — see the open question at the end.

THE FIX. projectToJson returns a [[nodiscard]] WriteResult {root, ok,
failedFile, reason, imagesWritten} instead of a bare QJsonObject, checks
mkpath first and every image write through one ImageWriter, and STOPS AT
THE FIRST FAILURE without building a manifest. saveToPath writes images
first, refuses to touch the project file unless ok, then commits the
manifest through QSaveFile with the byte count checked and cancelWriting()
on a short write. Changing the RETURN TYPE (rather than adding an
out-parameter) is what makes the failure impossible to ignore: there is no
QJsonObject to write until the result says ok.

setClean() MADE STRUCTURALLY UNREACHABLE FROM A PARTIAL SAVE. The dirty
flag is the thing that stands between the artist and closing on lost work,
so this is not a rule review has to catch. m_currentProjectPath, the
recents entry and setClean() moved into markSavedTo(path, SaveCompleted),
where SaveCompleted is a token whose constructor is private and whose only
friend is saveToPath. The single place that can construct one is the tail
of saveToPath, after every check. Elsewhere it does not compile.

AND THE FIRST VERSION OF THAT TOKEN WAS DECORATIVE. Written as
`SaveCompleted() = default;` it compiled a deliberate forgery in
onSaveProject without complaint: under C++17 a class whose only
constructor is defaulted-in-class is still an AGGREGATE, so
`SaveCompleted{}` was aggregate initialisation, which does not check
constructor access. A negative compile test caught it — the guarantee was
asserted, then TESTED BY TRYING TO BREAK IT, and it failed. Writing the
body (`SaveCompleted() {}`) makes the constructor user-provided, the class
a non-aggregate, and the access check real (error C2248). Catalogue this
with the vacuous tests: a compile-time guarantee nobody tried to violate
is a comment.

CALLER AUDIT, since fixing a return value without its consumers is a half
fix. Three call sites: saveForPrompt already handled false AND belt-and-
braced it with `&& !isDirty()` (this is the one the exit prompt depends
on); onSaveProject and onSaveProjectAs both guard updateTitle() on it.
None ignored a false. One real defect found anyway: onSaveProjectAs
assigns m_projectName BEFORE the save (it has to — the name is what gets
written) and never restored it, so a FAILED Save As left the open project
renamed after the file it had not managed to create. Now reverted on
failure.

GATE, section (i), 25 checks: mkpath blocked by a same-named file, one
image blocked by a directory in its place, and an unwritable manifest —
each asserting the save reports failure, the manifest on disk is
unchanged, and THE PROJECT IS STILL DIRTY; plus QSaveFile leaving no temp
file behind, a failed save not moving the project path, and a normal save
still working afterwards (without which the section could pass on a save
broken for every input).

THE CONTROL CAUGHT A VACUOUS CHECK IN THIS VERY SECTION. The first
version painted a stroke, saved, and asserted the manifest hash changed —
IT DOES NOT. Artwork lives in the PNGs; the JSON beside them is
byte-identical before and after a stroke. So "the manifest is unchanged"
would have passed on a BROKEN build too, for a reason having nothing to do
with the fix. The failing saves now change the FRAME RATE, which the
manifest does record, and each case additionally asserts the file still
carries the LAST SAVED rate rather than the unsaved one. Proven by
defeating the mkpath check (`if (false && ...)`): all four (i.1) checks
fail, with the detail line reading "clean - the artist could close on
unsaved work".

OPEN QUESTION, REPORTED NOT IMPLEMENTED: a load cannot currently tell "this
project never had that file" from "this file should exist and does not" —
both arrive as a null QImage and are filled silently. The manifest NAMES
every file it expects, so the information is there: a load could count
named-but-missing images and say so once. Deliberately not done in this
pass.

FULL GATE, both configs, eight families plus the SankoTV target: zero
failures. ProjectLifecycle is now 122 checks. Pinned hashes unchanged.

## The 20260824-200632 stalls: CLOSED — investigated, not reproduced,
## and NOT in the paint path (2026-08-27)

Seven event-loop gaps >= 150 ms (worst 243) were recorded while the user
worked at brush 152 on a 2048x1080 canvas (NOT 2048x2160 — cameraInitial
says canvasW 2048, canvasH 1080) rotated ~137-160 deg at zoom 0.509. They
were provisionally framed as drawing stalls. THE EVENT DATA SAYS OTHERWISE
— read this table before reopening them from that recording:

    t     gap    moves  button-down  mouse was over
    7 s   197 ms   48        0       DrawingCanvas
    28 s  243 ms   33        0*      QMenu / QMenuBar
    67 s  204 ms   66        0       QMenuBar
    70 s  197 ms   65        2       QPushButton / QMenu
    82 s  201 ms   29        0       DrawingCanvas
    86 s  200 ms   51        0       panelThumb / QMenuBar
    92 s  200 ms    2        0       DrawingCanvas
    (* one 120 ms stroke early in that second, ended before the gap)

NO STROKE WAS IN FLIGHT during any of the seven. The earlier "actively
drawing through every stall" claim counted mouse MOVES; the button state
shows hovering, and four of seven were over menus, not the canvas. The
gaps cluster at ~197-204 ms — a near-constant cost, not the signature of
anything brush- or canvas-size-dependent.

REPRODUCTION ATTEMPT (Release, maximized, the exact recorded camera,
brush 152, recorder OFF then ON with its real 500 ms capture cadence,
20 s of hover+strokes each plus 30 menu open/closes): ZERO gaps >= 50 ms.
Candidates measured in isolation on this machine: grabWindow of the app
region 19-25 ms; QPixmap::toImage ~0; forced canvas repaint at that
camera 2-5 ms; menu popup 2-4 ms. Documents is NOT OneDrive-redirected,
so screenshot-write sync interference is excluded too.

VERDICT: does not reproduce; was not the paint path, the recorder's
capture, repaints, or menus. A constant ~200 ms hitting hover and menu
seconds alike points at something environmental in that session (other
machine load, a driver). If ~200 ms gaps appear again, capture a NEW
recording and check the button state FIRST; do not re-derive "drawing
stalls" from 20260824-200632 — that conclusion is refuted above.
(Probe archived: tests/_backups/brush_size_probe_req0_20260827.cpp.)

## Brush size to 5000 + the decimated live preview (2026-08-27)

THE MEASUREMENT CAME FIRST (Requirement 0, probe archived as above), and
it inverted the obvious worry. GPU stroke cost is CANVAS-BOUNDED, not
brush-bounded: instance quads clip to 256 px tiles, so once a stroke
touches every tile the cost plateaus — at 960x540 a true-5000 brush costs
the same ~20 ms render as a 1000; at 4K, 5000 vs 2048 is +25% render
(230->287 ms) and +12% readback. No GPU limit is approached on either
backend (all render targets are 256 px tiles; tip textures are the custom
mask's NATIVE size, independent of brush size). The real cost was on the
GUI THREAD, at SMALL-to-mid sizes: see the preview cliff below.

THE CAP, 5000 ABSOLUTE, at every site (the old "200" was already a
fiction — the studio slider went to 2048): Brush::setSize,
Brush::shape() x2 (request + bucket), StrokeBuilder::placeStamp bounds,
StrokeBuilder::shapedTipForStamp, GpuStampRenderer rasterSize x2,
DrawingCanvas::setBrushToolSize, StoryboardPage restore clamp + mirror
clamp + SizeCtlSlider range, BrushSettingsStudio Size slider,
AbrImporter kEngineTipMax. ABSOLUTE, not canvas-relative, by decision:
cost is canvas-bounded so relative caps buy no performance, and a preset
meaning different things in different projects is what the mixed-size
work spent effort preventing. The ERASER keeps 1..200 (eraser library
untouched, its own pass).

THE SIZE CTL SLIDER IS LOGARITHMIC for the Brush (linear for the
Eraser): every track pixel is the same RELATIVE step (~+3.9%/px), so
1..20 keeps >= 1 px of track per integer while 5000 stays reachable.
Chosen over piecewise-linear (kinks, arbitrary knees to defend) and
power curves (REJECTED BY ARITHMETIC: no reasonable exponent keeps
1 px/integer at v=20). The mapping lives in ONE pair of helpers
(fracForValue/valueForFrac) used by the handle, fill, preset ticks and
drag alike. Tool switching swaps range+mapping BEFORE the value — the
other order squashes a 5000 brush to 200 through the eraser's clamp on a
round-trip (asserted in the gate).

PRE-EXISTING DISPLAY LIE, fixed here: the bar's engine mirror clamped to
200 while the studio could set 2048, so the bar showed 200 whenever a
large library brush was active. The bar now spans the engine's range,
and grew sizeCtlDisplayedSizeForTest() — the slider class is file-local
and nothing could observe the lie from outside, which is WHY it lived.

THE DECIMATED LIVE PREVIEW (the 512 cliff and the blind range, both
closed by one mechanism). The per-stamp CPU preview cost ~size^2 on the
GUI thread — measured ~10 ms PER POINTER MOVE at size 500 on 4K — and
the old mitigation was a gate: brushes over 512 had NO preview at all
(the artist drew BLIND until the async publish landed; DrawingCanvas
paints nothing when preview tiles are empty). Now: brushes over 256
rasterize their preview in a SEPARATE StrokeBuilder at 1/k scale
(k = ceil(size/256)) and the canvas upscales at composite time. Ceiling
~2.6 ms/move at 4K at EVERY size; brushes <= 256 keep the byte-identical
full-res path. The edge blur is ~one preview texel = ~0.4% of the brush
diameter at every size — constant RELATIVE softness, sharpening at
publish.

THE SAFETY CASE, stated and pinned: the preview builder's output cannot
reach published pixels, because finishStrokeWork() reads stamps, points,
seed and affectedRect from the FULL-RES primary builder only, and
render() re-rasterizes from those; the preview builder is reachable
solely through previewTiles(), which only the canvas paint path reads.
The gate pins it as INVARIANCE (the closest real check to a universal —
"never" cannot be asserted, equality can): the same stroke with preview
ON vs OFF publishes byte-identical afterRegion at 152/500/2048 on the
GPU path and 2048 on the CPU path, with controls (ON has preview tiles,
OFF has none; different sizes publish different bytes).

KNOWN IN-FLIGHT DIVERGENCE, textured presets: grain pattern scale is
compensated on the preview brush copy (grainScale/k), but the NOISE
field has no such knob — a noise-textured preset previews its texture at
the wrong spatial scale and settles at publish. Shape and coverage do
not jump; sharpening + texture settle is the whole release transition.
An artist reporting "the texture shifts when I lift the pen" on a big
textured brush is seeing this, not a bug in their file.

TIP CACHE at 5000: 24.4 MB per bucket, 119 ms cold build — but only on
the RENDER WORKER (CPU fallback). The GUI-side preview builds tips
<= 256 px, and the GPU path evaluates procedural tips in the shader
without building one. No GUI-thread tip cost was added.

ONE ENGINE QUIRK found by the probe, recorded not fixed: a stroke that
changes ZERO pixels (e.g. repainting identical pixels over itself)
reports succeeded=false with an EMPTY error string — the empty commit
fails isValid() because QRect().contains(QRect()) is false. Harmless in
the app (a no-op stroke pushes no undo entry), but it cost the probe a
debugging round, and an empty error on a "failure" is a trap.

GATE ADDITIONS: SizeLock (j) — footprint sweep at 960x540 AND 3840x2160
for 152/500/2048/5000: a 5000 stroke from the centre must ink all four
corners of both canvases (radius 2500 >= both half-diagonals), 2048 inks
4 at 960x540 but 0 at 4K, 152 inks none — reach depends on size, so a
clamp reintroduced at ANY site fails the footprint, with the empty-corner
control first. BrushLibrary (b5) 5000 through the engine and round-trip
through .sankobrush with a wrong-size control; (b6) the preview checks
above plus "a 2048 brush HAS a live preview" (the blind-range regression
gate). Lifecycle (j) bar<->engine agreement: a 5000 preset lands in the
engine AND on the bar (both would have failed before), the 152 control,
and the eraser round-trip. Totals: SizeLock 154, Lifecycle 129,
BrushLibrary +15.

The preview SHA did not move (swatches clamp preview brushes to 3..16 px
— structurally immune to the cap), and the paint locks did not move (the
preview change cannot reach published pixels; the cap change alters no
existing preset). Both verified by the full gate, not assumed.

## Field validation of the 5000 pass, and what analysing it turned up
## (2026-08-27, recording 20260827-205606)

THE PASS HELD IN THE FIELD: 81 s of real tablet work on a 4K project,
135 strokes at ~178 pen events/s, brush sizes swept 18..3566 on the log
slider, 32 strokes above 2000 px. Worst GUI gap during ANY stroke
second: 54 ms (500-2000 px), 40 ms above 2000 px, medians 33-35 ms. The
above-2000 range was BLIND on the previous build. Zero QPainter
warnings, zero drops, memory flat. The two worst whole-session gaps
(~105 ms) were Color Panel interaction while hovering, not painting —
on the shelf, not investigated.

THE PROBE CONTAMINATED THE USER'S RECORDINGS FOLDER (self-report).
The 2026-08-27 measurement probe redirected the Dev Recorder's output
via QSettings — and the redirect SILENTLY FAILED, because the probe
wrote the key under the app-level org name ("Sanko") while the recorder
reads QSettings("SankoTV","SankoTV"). Two sessions of synthetic probe
strokes landed in the user's real Documents/SankoTV-DevRecordings
(20260827-185513 and -190131) and were nearly analysed as user
sessions: their QPainter warnings cost a full attribution round before
the stroke shape (200,150 step 8,5 — the probe's own driveCanvas) gave
them away. The user deletes the two folders themselves. Rules already
on the books that this reaffirms: verify a redirect TOOK EFFECT before
trusting it, and check provenance of a recording before analysing it.

THE UNDERLYING TRAP IS BIGGER THAN THE INCIDENT — the two-argument
QSettings("SankoTV","SankoTV") form ignores BOTH setDefaultFormat and
the application org name, so it bypasses every test family's scratch
redirection (setDefaultFormat(IniFormat) + setPath only govern the
DEFAULT-constructed QSettings). Census (2026-08-27): 30 sites across 10
files — ColorPanel 2, DrawingCanvas 3, FloatingToolWindow 5,
MainWindow 2, NewProjectDialog 1 (has its own override seam),
StoryboardPage 8, BrushLibraryModel 1, BrushLibraryPanel 3,
BrushSettingsStudio 4, DevRecorder 1 (has an env-var override) — 11 of
them WRITERS. No test currently writes through them (writes hang off
slider adjust-end and teardown paths tests do not drive), so nothing
has leaked YET — but the scratch rule is supposed to hold without
anyone checking. Scoped, not fixed; see the pending decision.

ALSO FOUND: the app is already split-brained across org names —
QSettings lives under org "SankoTV" (registry Software\SankoTV\SankoTV)
while QStandardPaths data (the brush library shelf, the preview cache)
lives under org "Sanko" (AppData/Roaming/Sanko/...). Any fix that
renames the app org would MOVE the brush shelf and require migrating
the user's custom brushes; the recommended fix (a single overridable
sankoSettings() helper) avoids that entirely. Registry note: the real
store contains a leftover "seamProbe" value from an old seam.

## system.txt's git hash: empty since c7b1b37c5 — fixed, fenced, gated
## (2026-08-27)

THE HASH HAS GONE WRONG SILENTLY TWICE. First it was captured at
CONFIGURE time and named a commit four builds behind the binary — a
hash that LIED (found by recording 20260824-200632, fixed in
c7b1b37c5). The fix generated the header at BUILD time but included it
in DevRecorder.cpp, where NOTHING uses it, while the #ifdef
SANKOTV_GIT_HEAD that feeds HostInfo sits in MainWindow.cpp — which
never saw the macro and silently compiled the empty-string branch. So
the fix replaced a hash that lied with a hash that WASN'T THERE, and
every recording since c7b1b37c5 — including the one that validated the
5000 pass — has "git:" with no value. The old lie is exactly why the
new silence went unnoticed: nobody re-checked a fixed thing.

THREE LAYERS NOW, so a third silence is impossible:
1. The include moved into MainWindow.cpp (the consuming TU).
2. An #error fence right after it: if the generated header ever stops
   reaching MainWindow.cpp, the BUILD fails (proven by negative compile
   test — removing the include fails with C1189).
3. Lifecycle section (k): drives the REAL wiring (MainWindow HostInfo ->
   Recorder -> system.txt) with output redirected via SANKOTV_DEVREC_DIR
   — the env var, NOT the settings key, precisely because of the
   org-name trap above — and asserts the recording landed under scratch
   and the git line is non-empty and not "unknown", with a parser
   control on the same file. The lifecycle target now compiles the
   recorder (SANKOTV_DEV_RECORDER + the generated header).

FOUND BY THE GATE WHILE BUILDING IT: the Recorder singleton's indicator
widget was a RAW pointer, and QMenuBar::setCornerWidget takes
ownership — so the second MainWindow a process constructs handed its
menu bar a widget the first window's death had deleted (crash in
QMenuBar::setCornerWidget, immediately caught by the lifecycle family
the moment the recorder was compiled into it). One MainWindow per
process in the app, so production never saw it. Now a QPointer;
indicatorWidget() recreates after deletion.

AND A PROCESS NOTE, recorded because it is the second time this shape
appeared: the negative compile test for the #error fence was restored
with `git checkout -- MainWindow.cpp`, which also reverted the
UNCOMMITTED fix itself. Caught immediately and re-applied. Temporary
edits stacked on uncommitted work must be undone by reversing the edit,
never by checkout.

PENDING DECISION (user): whether to fix the two-argument QSettings form
app-wide (the scoped recommendation: one sankoSettings() choke point
with a test override, zero data migration) or leave it recorded.
