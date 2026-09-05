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
[DECIDED AND DONE 2026-08-27 — see the sankoSettings() entry below.]

## sankoSettings(): the settings choke point (2026-08-27)

DECIDED: fix app-wide, before the eraser library, so new features are
written against the choke point instead of adding sites to the pattern
being replaced.

src/SankoSettings.h — sankoSettings() returns EXACTLY what the
two-argument form opened (org "SankoTV", app "SankoTV", native format),
so every existing setting stays where it is: ZERO migration. Tests call
sankoSettingsSetOverrideForTest(<scratch>.ini) first and every read and
write thereafter goes to that INI. Org names deliberately unchanged —
the split-brain (settings under org "SankoTV", brush shelf under org
"Sanko") stays as recorded, its own decision.

ALL 30 SITES WENT THROUGH. No exceptions needed. The three sites with
existing override machinery LAYER on top rather than being replaced:
NewProjectDialog's recents override and BrushLibraryModel's shelf
override keep their own seams (their fallbacks now route through the
helper), and the Dev Recorder keeps SANKOTV_DEVREC_DIR with priority
over settings (it is read in the singleton's constructor, before any
override could be set, and it is the recorder's documented escape
hatch). The only raw two-argument constructions left in the tree are
the helper's own fallback and TWO DELIBERATE ones in the lifecycle
test, which exist to READ the real store as the verification
instrument — they must stay raw or the check verifies nothing.

EVERY family sets the override in main() now, including the four that
had no scratch root at all (PixelLock, CanvasBrushLock, EdgeLock,
QuickShape — their canvases DO construct settings-reading widgets, so
they were reading the real store on every run and nobody knew).

GATE, lifecycle section (l), both halves as demanded: a helper write
LANDS in the scratch ini, AND is absent from the real store — plus the
stronger form: the real store's ENTIRE key/value map (82 keys),
snapshotted before the family runs, is IDENTICAL after it — MainWindow
teardowns, StoryboardPage per-tool persistence, floating toolbars,
recorder and all. The non-empty-store control proves the comparison
can see keys. The real-store half is the one that would have caught
the probe contamination: a redirect that silently fails now fails a
check instead of a user.

RULE the helper encodes: the scratch guarantee must hold WITHOUT anyone
checking. Settings access added in future goes through sankoSettings()
— a new two-argument QSettings site is a review reject.

## The engine erase composite: the Eraser is a brush now (2026-08-28)

THE ERASER WAS NEVER A BRUSH. It was the classic QPainter path whole and
entire — a round-cap drawLine with CompositionMode_Clear, two parameters
(size 1..200, opacity), hard-edged always, snapshot undo, and PRESSURE-
BLIND: tabletEvent opened with `if (m_tool != Brush) ignore()`, so pen
erasing fell back to synthesized mouse events at fixed width. On a
Cintiq that was a defect lived with unknowingly for months, and it is
what decided this design (Requirement 0, two passes back).

THE COMPOSITE. Both render paths already converge on ONE publication
boundary (accumulated UNORM16 coverage -> wet transfer -> byte
quantisation -> sourceAlpha -> source-over). Erase replaces only the
final blend, symmetrically: CPU eraseOut in PixelCompositor
(dst.a *= 1 - sourceAlpha, colour untouched), GPU an eraseMode branch in
publish.frag riding the spare uniform slot at offset 92. Everything
upstream — stamps, tips, hardness, pressure curves, flow, wet edges, the
opacity ceiling, the decimated preview — is blend-agnostic and unchanged.
Brush::eraseMode wins over smudge and forces the mask path
(usesColorStrokeBuffer returns false): smudge/dual/colour-jitter erasers
are a contradiction, excluded at the Brush level. Codec: v11 block
(eraseMode bool); old builds refuse v11 files cleanly at the wrapper,
existing files load unchanged (fresh Brush defaults to false).

ZERO-ALPHA CANONICALISATION, found by the gate: CPU kept a fully-erased
pixel's RGB bytes (straight alpha, inert) while the GPU's premultiplied
render-target roundtrip zeroes them — invisible pixels, 229/255 apart.
eraseOut now writes ALL-ZERO bytes when alpha reaches 0, and the two
paths are BYTE-IDENTICAL for erase (maxDelta 0, better than the 3/255
brush bound). New pin: kBaselineErase 0bc243812b43... (PixelLock check
6), same hash both configs. Partial pixels keep their colour channels —
proven with a half-strength fixture, because the hardness-1.0 fixture
produces only 0/255 coverage and the first "partial pixels" check found
NOTHING to check (a vacuous check caught before it shipped).

THE QUIET HAZARD, confirmed and closed: the engine preview holds the
stroke as pixels drawn OVER the layer — the OPPOSITE of what erasing
shows. The canvas now carves instead: during an erase stroke (live or
pending-publish) the carve region is clipped OUT of the active layer's
plain draw and the carved copy (layer minus the preview's coverage,
selection-masked) is drawn in its place — the same "layer minus capped
stroke" the classic eraser previewed. Works at k=1 and through the
decimated (k>1) preview unchanged; the frozen release-bridge stores
COVERAGE for erase strokes (m_pendingPreviewErase) and carves too.

SELECTION + PARTIAL OPACITY (the piece expected to break quietly)
survive BY CONSTRUCTION and are pinned: coverage accumulates UNMASKED
in UNORM16 with the opacity ceiling applied once at publication, and
render() applies the selection mask as a single lerp — the classic
cap-once semantics exactly. BrushLibrary (b7): masked-out pixels
untouched, selected pixels to 0, a soft 50% band erases HALFWAY (127),
50% opacity leaves 127, and crossing the same pixels twice in ONE
stroke does not double-erase (127 == 127). Mask convention note: the
selection mask is LUMINANCE (render() converts Grayscale8; the canvas
mask is white-on-transparent premultiplied = luminance-consistent) —
the first version of the b7 fixture encoded softness in ALPHA and
erased to 0, a wrong-convention trap worth remembering.

CANVAS REWIRING: the Eraser presses/moves/releases through the SAME
engine path as the Brush (m_brushStroke covers both; m_drawing,
finishEraseStroke, the StrokeMaskErase preview branch and drawSegment's
erase branches are RETIRED). The eraser Brush (hard-round, hardness 1.0,
eraseMode, engine-default pressure response) is swap-restored around the
stroke — the engine has one brush slot and the studio/library own its
usual occupant; restorePaintBrushAfterStroke() runs at finishStrokeWork
and cancelStroke. QuickShape stays Brush-only. Undo entries read
"Eraser Stroke". The eraser joins the 1..5000 log slider (per-tool
restore max raised; erasing a 4K canvas at 200 px was the old misery).

THE DEAD HARDNESS RETIRED, NOT MIGRATED: the per-tool eraser hardness
persisted for months and was applied by NOTHING (confirmed: only
tc->brush.hardness is ever applied; the mirror writes only the brush
struct). Honouring the stale knob now that hardness WORKS would soften
erasing based on a value the user never saw act. The keys
(toolCtl/eraser/hardness + hardnessTicks) are actively REMOVED at load
so no future reader resurrects them.

TWO TIMING BEHAVIOURS ARRIVE FROM THE BRUSH PATH — flagged for hand
checking, they are new to erasing and will be FELT before any test
fails: (1) the erase bakes ASYNCHRONOUSLY (frozen-bridge preview until
the publish lands; the classic eraser baked synchronously at release);
(2) the DROPPED-PRESS window now applies — an eraser press during a
pending publish is refused, and the eraser's usage pattern (short rapid
scrubs) hits that window MORE often than brush strokes do. Measured
windows: ~13-14 ms per stroke at 960x540, 63-71 ms at 4K. Queuing the
press remains the known deferred fix (recorded at the stroke-path pass);
if scrubbing feels like it drops strokes at 4K, that fix moves up.

GATE: PixelLock check 6 (erase trio: pinned CPU hash, determinism,
GPU byte-identity, alpha-exactly-0, untouched corner, colour channels —
controls throughout); BrushLibrary (b7) as above; SizeLock (k) — the
engine eraser through REAL synthetic events: erases, MID-STROKE preview
shows REMOVAL at k=1 AND k>1 (QWidget::render sampling, pre-stroke
controls), undo/redo byte-exact titled "Eraser Stroke", tablet erasing
works (alpha 26 at pressure 0.9 — pressure genuinely modulates);
Lifecycle (j) eraser range updated to 1..5000. Totals: SizeLock 165,
Lifecycle 138. Pinned hashes 193847fa / 666f7b45 / cafcec7f unchanged —
verified by the full gate, both configs, plus the SankoTV target.

NEXT (its own pass): the library UI — builtinEraserRoster() with its
OWN pinned SHA (never added to builtinRoster(), which is what keeps
193847fa fixed), category tag "Eraser" in the panel, preview swatches
rendered over a colour band showing the carved hole (the smudge
pattern), preset save/load through the existing codec.
[DONE 2026-08-28 — the entry below.]

## The Eraser Library UI (2026-08-28)
[SUPERSEDED same day by the MIRROR - dedicated presets retired; see
"The mirrored Eraser Library" below.]

EIGHT BUILT-IN ERASERS, NOT TEN — ten was the brush categories' bar,
inherited from a check written for brushes, not a design number. Each
has a distinct mechanical identity on the mask path: Hard Round (the
default; the classic edge), Soft Round, Pressure Eraser (steep
size+opacity curves), Airbrush Eraser (low flow, builds up), Pencil
Eraser (small, light grain), Chalk Eraser (deep grain-on-coverage),
Chisel Eraser (compressed roundness + angle), Scatter Eraser
(scatter+jitter). builtinEraserRoster() is a SEPARATE function with its
own pinned combined swatch SHA
(96baaccf7c4eac88613a19b51a47f1d178e59c85ec78ab521d8bf604b474c5e9, same
in both configs); the model concatenates at ITS call sites and
builtinRoster()/builtinCategories() were never touched — which is what
kept 193847fa... fixed, verified by the gate.

THE CONTRADICTION MADE UNCONSTRUCTIBLE (gate demand: "the roster check
proves today's eight are clean; that check proves the ninth cannot be
dirty"). Found while building it: dualBrushEnabled() ignored eraseMode,
and the DUAL publication shader has no erase branch — an erase+dual
brush would have DEPOSITED COLOUR. The accessor now forces the mask
path (!m_eraseMode && ...), same shape as the smudgeActive guard, which
also keeps the codec from serialising a secondary payload for erasers.
BrushLibrary (b8) pins all three at the Brush level with before/after
controls: eraseMode forces smudge off, forces dual off, forces the mask
path over the colour buffer. FORCING, not refusal — setters stay
order-independent.

THE SWATCH is the structural INVERSE of a brush swatch: a solid neutral
band (0x8a8a92) with the S-wave carved through it (Source-composited so
the reduced-alpha carve replaces the band; the UI checkerboard shows in
the hole) versus a coloured mark on transparency. Gate asserts every
swatch shows band AND carve. The studio's ScratchCanvas test-draws
erase presets over the same band. No kSwatchRevision bump: the eraser
rendering is a new code path and existing brush swatch bytes are
untouched.

PANEL: "Eraser" is a PANEL-side sidebar row (between the brush
categories and "Imported") — BrushLibraryModel::categories() is a
pinned baseline and was not touched.

TWO V1 UX DECISIONS [BOTH SUPERSEDED 2026-08-28 - the tool-scoped
library entry below. Recorded here exactly so the reversal could start
from the reasoning; it did.]:
1. PRESET ACTIVATION SWITCHES THE TOOL (Procreate precedent): picking
   an eraser preset states intent — it lands in the ERASER brush and
   the tool switches to Eraser; a brush preset switches to Brush. Each
   tool keeps its own selection (m_activeBrushPresetId /
   m_activeEraserPresetId); switching TOOLS changes neither. If this
   fights the user's actual workflow, the reversal is: drop the two
   setTool calls in the brushActivated lambda (StoryboardPage) and the
   Lifecycle (m) tool-switch assertions — everything else stands.
2. "RECENT" STAYS BRUSH-ONLY: eraser activations would churn the brush
   Recent list for no benefit (the eraser roster is eight rows away,
   not thirty). Enforced in BrushLibraryModel::recordUsage. This is a
   DELIBERATE exclusion, not an oversight — revisit if it feels wrong.

STUDIO: opens for eraser presets; Mixing, Dual Brush and Color sections
disappear (plus the grain-affects-colour toggle) — everything left
genuinely drives erasing. eraseMode is the preset's identity, no toggle
anywhere; Save Variation of an eraser stays an eraser (the codec
carries the flag). If the studio is open on a hidden section when an
eraser loads, it falls back to Stroke.

WIRING: DrawingCanvas::setEraserPreset (forces eraseMode, mirrors
size/opacity, emits eraserBrushChanged) + eraserBrush() accessor; the
Size CTL mirrors the eraser preset via m_syncEraserCtl the way
paintBrushChanged mirrors the brush. The panel gained
activatePresetForTest() — emits the REAL brushActivated signal so the
gate drives the page's routing lambda end to end.

GATE: BrushLibrary (b8) — roster discipline (exactly 8, ids unique and
eraser/-prefixed, all eraseMode, none smudge/dual/colour-buffer), the
three contradiction pins, swatch render/determinism/band+carve, and the
eraser SHA pin. Lifecycle (m) — activation through the real panel
signal: preset lands in the eraser brush, tool switches, paint brush
untouched, bar mirrors it, both selections survive a tool round-trip.
Totals: Lifecycle 145. All pins intact: 193847fa (brush swatches),
666f7b45/cafcec7f (canvas locks), 0bc24381 (erase render), plus the new
96baaccf (eraser swatches). Full gate green, both configs, both SankoTV
builds.

## The tool-scoped library: the v1 model inverted back (2026-08-28)

THE USER'S CORRECTION: the Eraser Library belongs to the Eraser tool the
way the Brush Library belongs to the Brush tool - open the library under
the Eraser and see erasers, under the Brush and see brushes, never both.
The shipped v1 (erasers as a sidebar category inside the brush panel,
activation switching the tool) was the inversion of that.

ONE PANEL WITH A SCOPE, not a second instance - the m_importedRow
precedent decided it: sidebar rows were already toggled by visibility
with a fallback when the current category vanishes, so
setToolScope(Brush|Eraser) is that pattern generalised. A second panel
would have duplicated the FloatingToolWindow persistence identity, the
anchor machinery, the studio suppression wiring - all cost, no gain.

WHAT SHIPPED: scope follows toolChanged in both directions, live while
the panel is open; the sidebar shows Recent + brush categories +
Imported under the Brush, the Eraser row alone under the Eraser
("Imported" stays brush-only - ABR imports are paint brushes; an
eraser-Recent is DEFERRED ON SIZING, not excluded by rule: eight presets
fit one screen, revisit when saved variations lengthen the list); the
panel title flips (user-renameable library name / fixed "Eraser
Library"); the panel anchors to the SCOPE's own tool button, and the
Eraser button gained the Brush button's press-to-toggle wiring; the View
menu has ONE "Library" entry. Activation NEVER touches the tool - the
eraser-vs-brush routing by preset kind stays, minus the two setTool
calls, exactly the reversal recipe the v1 entry wrote down.

BOTH V1 DECISIONS DISSOLVED rather than reversed piecemeal: with the
panel scoped to the tool, switch-on-activation has nothing to switch and
Recent-brush-only stops being an exclusion. THE VINDICATION worth
recording: the reversal was executed FROM the written reasoning and its
recorded recipe - nothing had to be reconstructed or re-litigated. That
is the return on writing decisions down with their reversal recipes, and
it is why the practice continues.

CAUGHT BY THE REWRITTEN GATE ON ITS FIRST RUN: the sidebar rows are
built VISIBLE and setToolScope early-returns on a no-op change, so the
Eraser row showed inside the brush sidebar until the first tool switch -
the constructor now applies the default scope's visibility explicitly.
(The stranding fallback the user flagged as most likely to be subtly
wrong passed as designed, in both directions, asserted by what the panel
SHOWS: strand "Sketching" under the Eraser -> the view lands on
"Eraser"; strand "Eraser" under the Brush -> it lands on "Recent".)

LIFECYCLE (m) REWRITTEN, not extended (it pinned the v1 tool switch by
name): now pins scope-follows-tool both directions with the panel open,
activation-never-touches-the-tool under both scopes, the stranding
fallback by displayed category and visible rows, the bar mirror, and
both selections surviving. Panel seams added: toolScope(),
currentCategoryForTest(), visibleCategoriesForTest(),
selectCategoryForTest(). Lifecycle is now 154 checks.

ALL FIVE PINS UNTOUCHED, as a UI reorganisation must leave them - 
193847fa (brush swatches), 666f7b45 / cafcec7f (canvas locks), 0bc24381
(erase render), 96baaccf (eraser swatches) - verified by the full gate,
both configs, both SankoTV builds.

## The mirrored Eraser Library: one definition, referenced twice
## (2026-08-28)

THE USER'S CORRECTION, second and final shape: the Eraser Library
MIRRORS the Brush Library. Every mirrorable brush is available as an
eraser with identical character - same tip, texture, spacing, dynamics,
jitter, pressure, opacity - differing only in knocking pixels out.
Gouache erases like Gouache. The eight dedicated eraser presets are
RETIRED (every one had a mirror equivalent; they were exactly the
second-copy class the correction forbids). The out-of-box default
eraser is unchanged - the canvas constructor's hard-round-1.0 brush is
code, not a preset.

MECHANICS - a scope-keyed view, no second data structure: both scopes
share the category rows; the eraser scope filters the LIST
(presetFitsScope), requests ERASE-VARIANT swatches (preset id +
"#erase", a copy with eraseMode forced - settingsHash covers the v11
flag so the two variants can never collide in the cache), and routes
activation through the eraser door (setEraserPreset, which applies
eraseMode to ITS COPY - the stored preset is never mutated). The
"Eraser" sidebar row survives only as the home of LEGACY user eraser
presets and hides when none exist (the Imported pattern). Recent is now
ONE list shared by both scopes - the same ids flow through either door.
Shared category selection SURVIVES scope flips (the old stranding
applied to disjoint sidebars; only legacy-Eraser/Imported can strand
now).

THE EXCLUSIONS - 6 of 62, split by observability: the 3 smudge
(Blender, Smudge Soft, Wet-on-Wet) and 3 dual (Ink Line & Splatter,
Bristle, Glitch) presets do NOT mirror - eraseMode forces the mask path
and their names would lie about the character. The 6 colour-buffer
presets (Confetti, Chromatic, Sparkle, Granulating Wash, Bleed Edge,
Salt Texture) DO mirror: colour is unobservable in erase mode, so
nothing VISIBLE is lost - and they are deliberately NOT badged (noise
explaining an invisible difference).

SHARED EDITING, by decision: the studio edits THE DEFINITION - editing
Gouache from the eraser scope changes Gouache everywhere, because there
is only one Gouache. Save Variation is the fork mechanism and needed
nothing new. The studio's eraser section-hiding keys off
session.eraseMode() and therefore applies only to LEGACY eraser
presets; mirrored presets show every section (Colour edits the painting
side of the same definition; erase ignores colour by construction).

>>> THE COUPLED PINS - READ THIS BEFORE TOUCHING EITHER SHA <<<
The eraser swatch SHA (kEraserSwatchSha in BrushLibraryTest, currently
124f9ba0b843...) pins renders DERIVED FROM builtinRoster(), so it is
COUPLED to the brush preview SHA (193847fa...). The asymmetry is the
part that will not be obvious later:
  * 193847fa MOVED (legitimate roster/fixture change): the eraser pin
    moves WITH it - re-baseline BOTH in the SAME commit, one reason,
    stated once. A commit that re-baselines only one of them is
    incomplete.
  * THE ERASER PIN MOVED ALONE (193847fa intact): that is a DEFECT in
    the erase composite, the mirror plumbing, or the banded fixture -
    NEVER a re-baseline. Find the cause. The brush SHA is the source;
    the eraser SHA is derived; a derivative cannot legitimately move
    while its source stands still.
The same note lives in the test next to the pin, so whoever hits a
failure sees it at the failure site.

RETIRED WITH THE EIGHT: builtinEraserRoster(), its 96baaccf... pin
(superseded BY DESIGN with the user's explicit approval - the fixture
was removed, not the failure worked around), and the b8
roster-discipline checks - replaced by the mirror census (56 of 62
mirror; exactly 6 excluded, each smudge or dual), the ONE-DEFINITION
IDENTITY (toggling eraseMode on and off leaves every mirrorable preset
byte-identical through the codec), the three Brush-level contradiction
guards (unchanged), and the roster-sized swatch pin. One check
calibrated during the pass: Soft Wash erases at very low strength and
its faint carve missed an alpha<200 detector - the carve claim is now
"the erase render did something to the band" (alpha<255), because a
faint eraser making a faint carve is CORRECT mirroring.

Lifecycle (m) rewritten a second time: the mirror census through the
REAL panel (every mirrorable preset appears under the eraser scope, no
excluded preset leaks), the both-doors identity (eraser-Gouache with
the flag cleared is BYTE-IDENTICAL to Gouache through the codec),
scope-follows-tool with the shared category SURVIVING the flip,
activation never touching the tool, bar mirror, both selections
surviving. Lifecycle is now 152 checks.

Pins after this pass: 193847fa (brush swatches), 666f7b45 / cafcec7f
(canvas locks), 0bc24381 (erase render) - all four UNTOUCHED, verified
by the gate - plus 124f9ba0 (the mirrored eraser swatches, coupled per
the note above).

## Erase swatches: the coverage as white ink (2026-08-28)

THE BANDED CARVE IS GONE from the library previews. Under the mirror the
grey plate with dark holes read as broken next to white-on-transparency
brush swatches (the "holes" showed the dark panel row through the carve).
The erase swatch now depicts THE COVERAGE - the preset's stroke rendered
as white ink on transparency, pixel for pixel the removal footprint.
Same fixture, same tip/grain/scatter/taper as the brush swatch; the
mirror's claim made visible: same character, no colour. Mechanically:
the RENDER COPY clears eraseMode, forces white ink, and neutralises the
colour dynamics (real erasing forces the mask path, so a faithful
depiction must too - Confetti's hue jitter recoloured the first attempt
and the gate caught it). The stored preset is untouched, as always.

THE DISTINGUISHABILITY TRADE, weighed with eyes open - NOT an oversight,
and not a regression from the banded design: the inverted-swatch
argument was built for the pre-mirror world where eraser rows sat INSIDE
the brush panel as a sibling category and needed per-row distinction.
Under the tool-scoped mirror the two libraries are never on screen at
once - title, sidebar and anchor button all say which one is open - so
the per-row signal was redundant three times over. WHAT WAS GIVEN UP: a
swatch seen out of context (a cropped screenshot, a glimpse mid-scroll
during a scope flip) no longer self-identifies as an eraser, and the
one-frame moment during a live scope switch before swatches re-render is
visually silent. Edge-glances, accepted as the price.

kSwatchRevision bumped r2->r3 (the disk cache would otherwise serve
stale banded images for erase-variant keys). The bump re-keys ALL
swatches; brush swatch BYTES are unchanged and the gate PROVED it:
193847fa... came through byte-identical in both configs after the
re-render - the check that shows the bump changed cache validity only.
kEraserSwatchSha re-baselined WITH APPROVAL as a deliberate change of
depiction: 124f9ba0 -> 32f0dfff (identical across configs before
pinning). The coupled-pin asymmetry note stands unchanged - the new pin
is still derived from builtinRoster(), and it moving alone is still a
defect. The studio ScratchCanvas KEEPS its band: interactive erasing
needs something to erase - different surface, different job.

b8 rewritten to the new depiction with controls: every mirrored swatch
is a white mark on transparency - corners alpha 0, at least one
white-ish stroke pixel, NO coloured pixels (an eraser has no colour) -
deterministic, combined into the new pin. Four pins untouched:
193847fa, 666f7b45, cafcec7f, 0bc24381.

## Promotion: overrides fold into recipes as reviewable diffs (2026-08-28)

BUILT-INS STAY AS CODE - the decision that carries everything: the wire
format is binary, so a committed .sankobrush is an unreviewable blob,
while a recipe edit reads "curve2(0.6) -> curve2(0.1)" in a diff. The
studio's OVERRIDE mechanism is the no-rebuild tuning workflow (it
already existed; the user had been running a tuned Gouache for four
weeks without knowing), and PROMOTION folds an override into the recipe
as a commit.

THE INSTRUMENT: SankoPresetDiff (permanent tool, tools/PresetDiff.cpp).
Given an override file it prints every differing field against the
stock recipe in recipe vocabulary - and PROVES the diff complete by
reconstruction: its entire field vocabulary applied to a stock copy
must reproduce the override byte-for-byte through the codec, else it
exits STOP. Image-bearing fields (custom tip/grain) and dual-brush
secondaries are declared unpromotable up front. "If a tuned field
cannot be expressed in the recipe's vocabulary, stop" is therefore
mechanical, not a promise.

[AMENDED 2026-08-29, user-approved - see "code plus versioned assets"
below. Images remain inexpressible AS CODE, and SankoPresetDiff's STOP
on image diffs stands; but an image can now ship as a VERSIONED ASSET
(brush_assets.qrc) that the recipe loads, which is how the HB Pencil
promotion carried its custom tip. The two rules do not conflict: the
diff tool stops because it cannot make an image reviewable as
parameters; the asset route makes it reviewable as a committed file
instead. Both routes end in a commit or not at all.]

THE WORKFLOW: user says "promote my <preset> override" -> run
SankoPresetDiff -> report the field diff for confirmation BEFORE any
edit (the user may not want every value they touched) -> recipe edit ->
full gate -> BOTH swatch SHAs re-baselined in the same commit with the
tuning as the stated reason (the coupled-pin procedure's legitimate
case) -> report -> commit on approval -> delete THAT override only,
after the user approves the committed result. CLAUDE.md's gate section
carries the current preview SHA and moves in the same commit.

FIRST PROMOTION, Gouache (this commit): the four-week-old override held
exactly ONE change - sizePressureCurve floor 0.6 -> 0.104167 (a light
pen paints at ~10% size instead of 60%; a much deeper pressure taper).
Promoted rounded to 0.1 by the user's decision. SHAs: preview 193847fa
-> 7cd8d084, eraser swatches 32f0dfff -> e7ce9d6b, both configs
identical, moved together per the coupled-pin note. Canvas/engine pins
untouched (666f7b45, cafcec7f, 0bc24381).

OVERRIDE FOLDER CENSUS at promotion time: Gouache was the ONLY
override. Answered before it could matter, per the user's ask.

QUEUED, its own pass with Requirement 0 first: override DISCOVERABILITY
+ the STALE-OVERRIDE HAZARD, treated as one problem - nothing in the UI
shows a built-in is shadowed (hasBuiltinOverride has no UI caller; the
user found their Gouache only because Claude looked), and
loadBuiltinOverrides replaces the recipe brush unconditionally, so an
override silently beats any future recipe improvement. Scope to cover:
the row's modified mark + reset-to-stock; whether the app should detect
that a recipe CHANGED UNDERNEATH an override (an override matching
current stock is harmless; one diverging from a recipe that has moved
is the hazard) - the model could store the stock settingsHash at
override save and compare at load; and the census surfaced in-app
rather than by inspection.

## Override marks + the stale-hazard detector (2026-08-28)

THE ONE PROBLEM, both halves fixed: nothing showed a built-in was
shadowed (the user ran a tuned Gouache for a month unknowingly), and an
override silently beat any future recipe improvement.

FOUR STATES, computed by BrushLibraryModel::overrideState():
  None         no override, OR one byte-equal to current stock
               (harmless residue - no mark, and NOT auto-deleted:
               user files are never destroyed uninvited).
  Modified     diverges from stock; stock unchanged since the save
               (the stored fingerprint matches). Teal DOT on the row.
  StockChanged THE HAZARD - the recipe moved underneath the override.
               Orange TRIANGLE: different SHAPE and hue, spottable
               while scanning, per the user's explicit demand that the
               two marks not be two shades of one thing.
  Unknown      an override with NO stored fingerprint (pre-dating this
               machinery). Grey "?" - an honest question mark, NEVER
               defaulted to a reassuring state the app cannot support
               (the user's explicit demand). Empty on their machine
               today; the code outlives that.

THE FINGERPRINT: updateBrush's override branch records
settingsHash(stock recipe AT SAVE TIME) in the shelf settings (through
sankoSettings - scratch-safe by construction); overrideState compares
it against the CURRENT stock recipe's hash. A recipe edit in a normal
commit therefore flips the mark to the triangle at next load - the
silence is gone.

RESET TO STOCK: per-preset, in the row's context menu, only when an
override exists, with a confirm (it DESTROYS the override file - unlike
built-in delete, which hides). Deletes EXACTLY that file (the promotion
pass's only-that-one rule, now encoded in resetBuiltinToStock and
pinned by the sibling-untouched check), restores the stock brush
byte-exact, drops the fingerprint, and - via the panel - re-applies
stock through the scope door when the reset preset is the current
selection. "Restore Default Brushes" remains the nuclear option.

RESOLUTION PATHS for the triangle: Reset to Stock (take the new
recipe), or promotion via Claude (keep the tuning and make it official
- which ends the staleness by construction).

GATE: BrushLibrary (b9), 12 checks - all four states constructed and
asserted on a scratch-rooted model, including the two user-demanded
pins (StockChanged via a moved fingerprint, exactly what a recipe edit
does to it; Unknown for a missing fingerprint, never a reassuring
default), and reset restoring stock byte-exact through the codec while
the SIBLING override stays untouched. Lifecycle (n), 7 checks - the
user-visible loop through the real panel: tune -> teal mark appears ->
reset -> mark clears -> activation applies stock. Totals: Lifecycle
159. All five pins byte-identical: 7cd8d084 / e7ce9d6b (coupled) /
666f7b45 / cafcec7f / 0bc24381.

## The Tip Shape preview: the engine's own tip, live (2026-08-29)

WHAT: the studio Tip section's 44x26-ish thumbnail is replaced by the
Tip Shape preview panel (Figma 358:22; StudioTipShapePreview in
StudioControls, 320x210 - the TipRing's documented width divergence,
same #3c3c42/#595964/radius-8 chrome tokens). The tip is displayed
centred in the well, rendered at a FIXED 160 px (x devicePixelRatio),
as kDragger ink over the panel.

THE ONE-DEFINITION RULE, AGAIN: the image is produced by
StrokeBuilder::shapedTipForStamp - the engine's own per-stamp tip
function (the CPU stamp path's, mirrored term for term by the GPU
instance build) - called on a working copy with a neutral stamp (no
tilt, no rotation, no jitters; effectiveSize = 160, effectiveHardness =
base hardness, stamp.roundness = 1). Hardness falloff, custom tip
image, static angle/roundness/flips all compose INSIDE the engine call.
No tip maths exists in the studio. NO ENGINE CODE CHANGED - the
function was already public.

THE RETIRED DUPLICATE HAD ALREADY DRIFTED: tipTransformThumbnail
reimplemented the falloff and affine inline. Measured at retirement by
operation-for-operation transcription (scratchpad drift_check.py,
recorded here because the script is ephemeral): the procedural
falloff+affine agreed EXACTLY (max |diff| = 0.0 over 2.95M samples),
but the custom-tip sampling had diverged - nearest-neighbour with no
texel-centre against the engine's texel-centred bilinear
(the same-source fix's convention) - up to 132/255 per pixel along a
hard edge, mean 1.17/255 over a 64x64 hard-edge mask. The thumbnail
misdrew custom-tip edges for as long as both implementations existed.
That is the drift class a second implementation invites, and why the
preview renders THROUGH the engine rather than describing it.

COST DISCIPLINE (the 19-119 ms tip-cache trap): the render is fixed at
160 px regardless of brush size - the procedural falloff is
radius-normalized and custom tips sample [0,1] UVs, so the depiction is
size-invariant BY THE ENGINE'S OWN MATHS, and a full-size tip build
(119 ms at 5000, measured) can never be triggered by the preview.
Per-refresh cost is sub-ms (one 160^2 stamp loop; the engine's
incidental shape(160, h) cache build lands in the StrokeBuilder's own
brush copy and dies with it - it cannot evict the canvas brush's
full-size tips, which live in a different Brush instance's cache).

REFRESH PATH: pushToScratch - the single funnel every edit runs
(slider per-move, applyInstant, ring drags, undo restores, session
loads) - plus a syncer for the A/B scope switch, which syncs without
pushing. The preview shows the SCOPE brush. setBrush skips the repaint
when the engine returns byte-identical pixels, so edits to non-tip
parameters cost one 160 px render and no paint.

SURVEYED, LEFT ALONE: StudioTipRing::tipSpace mirrors the affine for
POINTER hit-testing (geometry in, no pixels out) - it cannot call an
image-producing engine function and its 2x2 transform is asserted
against the renderer's formula by its own seam. The Shape Control panel
(345:99) is that ring, already built in the statics pass - NOT part of
this pass, untouched.

GATE: Lifecycle (o), 12 checks - engine-format render, coverage
positive control, the ONE-DEFINITION identity (preview bytes == the
test's own shapedTipForStamp call, asserted at TWO session states),
hardness edit re-renders through the REAL edit path, size 5000 changes
NOTHING (the fixed-resolution claim, pinned), roundness+angle reach the
preview, custom tip reaches it, Flip X changes an asymmetric tip and
flipping back restores byte-exact (involution control), and the session
never touches the model. Totals: Lifecycle 171. All five pins
byte-identical both configs: 7cd8d084 / e7ce9d6b (coupled) / 666f7b45 /
cafcec7f / 0bc24381 - a studio-UI-only pass, confirmed rather than
assumed.

TWO FAILURE MODES FROM ONE ABSENCE (recorded at the user's direction,
grain-pass survey 2026-08-29): with no true preview in the studio, the
tip and grain took different wrong turns. The TIP got a hand-rolled
depiction - tipTransformThumbnail - which DUPLICATED the engine maths
and silently DRIFTED (132/255 on custom-tip edges, above). GRAIN got
nothing: the Texture section showed no grain pixels at all - verified
by grepping app/src for every grain-sampling identifier (grainTexture,
proceduralGrain, GrainField, wrapped, modulation, shapedSample; the
same patterns find the real engine implementations, the positive
control) - so it never lied, and it never informed either. Duplicated-
and-drifted versus silent-and-blind: both are what "no engine-rendered
preview" costs, and both end the same way - a preview the ENGINE
renders. That is the standing rule for any future studio preview: no
depiction code, only engine calls.

## The Grain Preview: the engine's carve, live (2026-08-29)

WHAT: the Texture section's Grain popup (Paper/Canvas/Chalk/Charcoal/
Custom StudioChoiceRow) is replaced by the Grain Preview panel (Figma
359:47, the Tip Shape panel's design sibling - same chrome, one column
up; StudioGrainPreview in StudioControls, 320x210, the documented width
divergence). The popup's job SPLIT: the four procedural presets moved
to a segmented row under the well; Custom stopped being a menu item and
became the STATE the new Load... button puts you in, shown as NO lit
segment (StudioSegmentedRow::setCurrentIndex(-1), a small extension) -
the engine agrees, setGrainPreset(Custom) was always a no-op, so Custom
was never a choice, only a consequence of loading. Load... imports an
image as the active grain immediately; the codec embeds the bytes in
the preset (unconditional image slot), so the source file on disk is
never referenced again - no missing-file failure mode exists.

THE DEPICTION (approved): a neutral SOFT disc (hardness 0.6) rendered
through the REAL stamp path - StrokeBuilder::addRawPoint -> placeStamp
-> strokeMask() on a render copy with the tip half neutralized (the
erase-swatch pattern) and every grain field kept - so the well shows
the engine's own coverage modulation: Scale at true 1:1 canvas pixels,
Rotation, Contrast, Depth, Motion anchoring, and all nine blend modes
through the engine's textureBlendCoverage. The raw texture was
rejected: it is blind to Depth, Contrast, Blend and Motion, most of
the section. The two preview panels are deliberately orthogonal: every
Texture control moves exactly the grain well, every Tip control exactly
the tip well.

TWO TRAPS THE GATE CAUGHT ON FIRST RUN (why the gate exists):
- rasterizePreview=false makes placeStamp record only the affected
  rect and rasterize NOTHING - it is BrushPreviewRenderer's mode, which
  rasterizes from the stamp list via the host adapter. The preview's
  StrokeBuilder must pass TRUE or strokeMask() is empty. The flatness
  control caught the all-zero mask immediately.
- At FULL coverage most texture blend modes collapse to the same value
  (Multiply, Subtract and Darken are identical at tip=1), so the
  original hardness-1 patch was blend-mode-BLIND. The soft disc's
  falloff band gives the nine modes coverage values they differ over.
  "Texture Blend mode reaches the preview" failed until it did.

COST: there is NO grain analog of the tip cache - grain is a wrapped
128x128 (or custom) texture sampled per pixel with scale/rotation/
contrast/depth as per-sample parameters, so no control rebuilds
anything full-size, ever. A refresh is one GrainField construction
(COW) + one 176px stamp - sub-ms. The patch is a fixed canvas window
independent of brush size, rendered at logical resolution deliberately
(a DPR upscale would widen the window and change WHAT is shown, not
how sharply). Refresh path: pushToScratch (live during drags) + a
scope syncer, byte-identical renders skip the repaint - identical to
the tip preview.

GATE: Lifecycle (p), 14 checks - engine-format render, coverage
positive control, the ONE-DEFINITION identity (preview == the test's
own transcribed engine render) asserted at THREE states (stock, depth
0, custom grain), depth-0 difference proving the delta WAS grain,
preset/scale/rotation/blend edits through the real edit path, loaded
grain active immediately (setCustomGrain flips preset to Custom
atomically), size-5000 invariance, session never touches the model.
Totals: Lifecycle 185. All five pins byte-identical both configs:
7cd8d084 / e7ce9d6b (coupled) / 666f7b45 / cafcec7f / 0bc24381.

## The suppression fix: holder-ship becomes machinery-owned (2026-08-29)

THE REGRESSION (user-reported, hand-hit): open Brush Settings, use any
modal inside it (Load..., the Done-failure warning), close the studio
- the floating bars stay gone with no UI path back; only an app
restart recovers (suppression is memory-only, startup show()s are
unconditional). Reproduced deterministically by probe, both configs.

ROOT CAUSE - a TEMPORAL imbalance, not a missing restore: the studio
was both a suppression HOLDER (visibilityChanged -> suppress/restore
in StoryboardPage) and a suppression TARGET (the generic modal filter
suppresses everything over the canvas, no except; the studio's anchor
is the canvas). The modal walk hid the studio; its hideEvent fired the
holder restore REENTRANTLY, mid-walk, BEFORE the walk pushed its own
capture - popping the one capture that held the true intents. Every
capture from then on recorded already-suppressed bars. Three
suppresses, three restores, state destroyed at capture time.

Candidate (A) (push before the walk) was traced and REJECTED before
coding: it only changes which capture the reentrant restore eats, then
strands the true capture under a duplicate. (C) (except the studio
from the modal suppress) resurrects the painted-across-the-dialog bug
for the largest window in the app.

THE STRUCTURAL FIX: FloatingToolWindow::setModalSurface(true) - the
studio declares the role in its constructor; holder logic runs in
applyEffectiveVisibility, keyed on the transition's CAUSE
(g_suppressionWalking: transitions the walk itself produces never
trigger holder actions), with m_holdingSuppression making push/pop
strictly alternate per holder. StoryboardPage's manual wiring is
deleted (only raise() remains on visibilityChanged). Do NOT wire
show/hide signals to suppress/restore again - the header says why.
Page-switch/minimize transparency preserved EXACTLY: host-driven
transitions still fire the holder, so the transient
restore-and-recapture round trip (captures re-read LIVE intent, the
drift-proofing) is unchanged and now PINNED.

TWO MORE DEFECTS FOUND BY THE PERMANENT GATE ITSELF:
- FloatingToolWindowManager::watch() never pruned destroyed objects
  from m_watched; a later anchor allocated at a recycled address read
  as "already watched" and got NO event filter. Unhittable with the
  app's single MainWindow; the gate's seventeen windows per process
  hit it deterministically. Fixed: prune on QObject::destroyed.
- DockController took settingsOrg/settingsApp and constructed the raw
  two-argument QSettings form - a sankoSettings() CHOKE-POINT BYPASS
  that wrote dock layout into the USER'S REAL STORE on every gate run
  that closed a MainWindow (it made Lifecycle (l) flap: consecutive
  identical runs converged and passed, interleaved different-window
  runs failed). Fixed: the org/app parameters are DELETED so the
  bypass cannot be reconstructed; the three sites go through
  sankoSettings(). Same store and keys for the user; scratch under
  tests. NOTE: gate runs before this fix DID write test-window dock
  layouts into the real store - the user's next launch may restore an
  odd dock layout once; View > Reset Layout recovers the default.

RECOVERY (its own pass, queued): even fixed, vanished tools deserve an
escape hatch. Survey result: the bars have NO user-facing close, and
placements are clamp-validated, so the only independent no-way-back
class WAS this bug; the derivative shape is chain dependency (Colors
panel and Brush Library reopen through buttons hosted on the brush
bar). A "Show All Tool Bars" action - the placement reset's sibling -
covers both. UX placement is the user's call.

GATE - PERMANENT, at the user's direction (second defect to hide in a
path only temporary seams covered: the studio seam never drove a
modal, the modal seam never opened the studio): Lifecycle (q), 14
checks - plain open/close, the full holder x target composition via
synthetic WindowBlocked/Unblocked (studio hides as target, bars STAY
hidden mid-modal, capture survives, close restores, stack balanced),
and the TRANSPARENCY PIN driving the anchor's hide/show (the
page-switch path, window-manager-independent) with the protected
semantics stated inside the check itself. Totals: Lifecycle 199. All
five pins byte-identical both configs: 7cd8d084 / e7ce9d6b (coupled) /
666f7b45 / cafcec7f / 0bc24381.

## The custom-image cap + the encoded-image memo (2026-08-29)

THE INCIDENT: the user loaded an 18-megapixel camera photo (5184x3456)
as custom grain and a 1.3 MP image as custom tip - a 14 MB preset. The
studio froze for seconds per gesture and the recording showed two
40-55 s input-queue grinds while drawing.

A DIAGNOSIS THAT DIED ON MEASUREMENT (recorded at the user's
direction, because this is the project's recurring pattern): the
recording review blamed per-pixel grain sampling over the 18 MB
texture - a cache-miss theory, plausible enough to survive its own
review. The Requirement-0 benchmark killed it: sampling cost is FLAT
across source sizes (grain ~7.6 ms, tip ~4.6 ms per 615 px stamp,
whether 128 px or 18 MP - the access pattern is sequential enough that
the prefetcher wins), and an end-to-end stroke through the user's
actual preset changed by <1% with the images capped and got SLOWER
with them removed. The DRAWING lag was the variation's spacing 0.015
(8x denser than default) - a fact that would have been missed entirely,
and the cap shipped as a phantom stroke fix, had the cap been built on
the reviewed diagnosis. Measure before building; the tip-drift pass
learned the same lesson from the other direction.

WHAT THE IMAGES DID CAUSE (all confirmed): 428 ms PNG encode per
serialization at 18 MP, x2 per studio gesture (applyInstant snapshots
before+after), 15 MB undo snapshots (the recording's ~900 MB working
set), a 14 MB preset file. The pass fixes exactly this class.

THE CAP: Brush::kMaxCustomImageDim = 2048, applied in setCustomShape /
setCustomGrain (SmoothTransformation, aspect preserved; under-cap
images pass byte-untouched). 2048 is visually free by measurement:
grain keeps >= 1 texel per canvas px at every reachable Grain Scale
(slider max 2048), and a tip source beyond the stamped size ALIASES
under plain bilinear - filtered downsampling is sharper for stamps at
or below 2048 (soft only above, 2.4x at the 5000 brush cap). Setter
placement means every path inherits it: studio Load..., ABR import,
and pre-cap files at load. 2048 was approved CONDITIONAL on the memo
below shipping in the same pass - without it the 69 ms encode would
still run per gesture and 1024 would have been the right number.

THE MEMO: BrushPresetCodec's encodePng memoises encoded bytes on
QImage::cacheKey (mutex-guarded; BrushPreviewRenderer's worker calls
saveBrush off-thread). A gesture's serialization now encodes ZERO
images. The stale-memo hazard - a replaced same-dimension image
serialising the OLD bytes - is pinned from BEHAVIOUR, not assumed from
the docs: (b10) proves distinct same-dimension images key differently
and that a swapped grain serialises the new bytes. Determinism: the
memo returns exactly the bytes it produced (b1/b2 idempotence stand
guard). NOTE the memo saves TIME, not undo MEMORY - QDataStream copies
the bytes into each snapshot; at the 2048 cap a snapshot is ~2.5 MB,
acceptable where 15 MB was not.

TOLD, NEVER SILENT: importing an image the cap reduces shows a modal
AT THE IMPORT with the numbers and "your original file is unchanged"
(a deliberate, rare moment - a passive caption would be missed by
exactly the person who needs it). A PRE-CAP FILE (made before this
pass) loads capped in memory, flagged transient on the BrushPreset
(imagesCappedOnLoad, codec-detected, never serialised); Done on such a
preset says the rewrite out loud before the file is rewritten in
capped form, and the model clears the flag after the write. The user's
own HB Pencil Variation is the one such file in existence: their
decision was (a) leave the file alone and flag the re-save - it is NOT
rewritten by this pass.

QUEUED, NOT BUILT: a spacing x size performance guard (refuse, clamp,
or warn is undecided - the user wants that design discussion before
anyone builds one; their variation is the only case that has ever hit
it). The user raises their own spacing by hand.

GATE: BrushLibrary (b10), 9 checks - both caps with aspect, the
under-cap byte-untouched control, cacheKey distinctness AS A TEST,
memo identity + zero-encode + the stale-memo replacement hazard, and a
spliced pre-cap file (a big PNG spliced into a real save's
length-prefixed image slot) loading capped with the flag raised, plus
the unspliced flag-down control. Lifecycle (r), 7 checks - the import
notice with numbers, the under-cap silence control, and the flagged
Done-rewrite flow end to end with the flag clearing after the write.
Totals: Lifecycle 206. All five pins byte-identical both configs:
7cd8d084 / e7ce9d6b (coupled) / 666f7b45 / cafcec7f / 0bc24381. (One
build-hygiene note: the Debug tree produced a corrupt SankoTV.exe
(LNK1136) and a zero-output segfaulting SankoCanvasSizeLock in the
same session - both cured by deleting intermediates and relinking;
the sources were proven innocent by Release passing throughout.)


## HB Pencil: the first user-preset promotion, and "code plus versioned assets" (2026-08-29)

WHAT: the built-in HB Pencil recipe (BuiltinRoster.cpp) is replaced
WHOLESALE by the user's "HB Pencil Variation" - the tuned pencil from
the image-cap pass (spacing 0.08, flow 0.2, opacity 0.55, Paper grain
scale 40 / depth 0.55 / depth-minimum 1.0 / Static, noise 0.15,
custom 1177x1102 tip, stock size/opacity pressure curves) - with
exactly ONE approved delta: size 4 -> 36. Opacity was the user's
explicit deliberate choice: 0.55 KEPT (the HB ceiling - strokes top
out at mid-grey and the paper shows through; 1.0 was offered and
declined). There was never a rename: the built-in was always named
"HB Pencil"; "4px" was its size. The user's variation file is LEFT
ALONE (their call: they delete it in-app once satisfied).

THE RULE CHANGE, user-approved: "built-ins are code" becomes "CODE
PLUS VERSIONED ASSETS". Reason: the variation carries a custom tip
image, and images cannot be expressed in recipe vocabulary (the
promotion pass declared them unpromotable AS PARAMETERS - that entry
is amended above so the two rules read together, not against each
other). The tip's exact PNG bytes were EXTRACTED from the saved
.sankobrush (never re-encoded) into assets/brushes/hb_pencil_tip.png,
served via brush_assets.qrc. THE CONSTRAINT THAT MUST HOLD: the qrc
must compile into EVERY target that builds BuiltinRoster.cpp (SankoTV,
SankoBrushLibraryTest, SankoPresetDiff; lifecycle inherits the app
list) - a target without it loads a null tip and renders a DIFFERENT
roster than the app. (b11)'s asset-loaded check trips if a new roster
target forgets it.

SOURCE-OF-TRUTH DISCIPLINE: the file was hashed at approval
(d1884843...) and re-verified unchanged at build end - the promotion
is provably of the version the user has, not one they have not seen.
The values in the recipe were read from the file, never retyped;
(b11) makes that claim mechanical (below).

GATE: BrushLibrary (b11), 5 checks, the user's demanded form verbatim:
the variation file is COMMITTED as a test fixture
(tests/test_fixtures.qrc, frozen at promotion - later edits to the
live variation must not move the gate); the check constructs BOTH
brushes, applies the single size delta, and compares saveBrush()
byte-for-byte - and on inequality a full-vocabulary comparator NAMES
THE FIELD (every scalar, enum, colour, both images, all 15 curves and
source/minimum pairs). Passed on the first run: the comparator never
fired. Plus the asset-loaded check and the opacity-ceiling control.
BOTH swatch SHAs re-baselined in the same commit per the coupled-pin
procedure, cross-config identical: preview 7cd8d084 -> c1fbee31,
eraser e7ce9d6b -> b602355f. CLAUDE.md updated with the reason. Canvas
locks and the erase baseline unmoved. Totals: BrushLibrary 79 checks,
Lifecycle 206.

## The Size CTL opacity becomes a MULTIPLIER (2026-08-29)

THE SEMANTICS CHANGE, user-approved: the bar's opacity now means "how
much of the preset's own ceiling", not an absolute that overwrites the
preset. engine opacity = preset base x bar multiplier, both tools
(setBrushOpacity / setEraserOpacity), general across every preset - no
special case for HB Pencil, which merely exposed the old mirror by
being the first built-in whose base is not ~1.0. Selecting HB Pencil
now reads Size 36 / Opacity 100 on the bar while the engine holds the
promoted 0.55.

WHY THE MIGRATION IS LOW-RISK - the property the whole change rests
on: AT MULTIPLIER 100 THE PRODUCT IS THE PRESET, BYTE FOR BYTE. Every
byte-identity check in the gate runs at 100 and held unchanged; the
stored bar values reset to 100 under a NEW key (opacityMult; the old
absolute-percent keys are actively removed beside the retired eraser
hardness), because an old absolute value cannot be faithfully
converted - it never recorded which preset it was overwriting - and
"full preset strength" is the only honest reading of the old intent
under the new meaning.

THE COMPOUNDING CONSEQUENCE, recorded at the user's direction so it is
never rediscovered as a bug: 50 percent on HB Pencil now paints at
0.55 x 0.5 = 0.275 - MUCH fainter than 50 percent meant before, and
correct. If a brush feels unexpectedly faint at a low bar setting,
this is why, by design. Lifecycle (j) pins the exact number.

THE REMEMBERED BASE - new state, audited for staleness up front:
m_brushBaseOpacity / m_eraserBaseOpacity live on DrawingCanvas,
in-memory only, captured EXCLUSIVELY at setPaintBrush /
setEraserPreset. The only writers of engine opacity are those two
sites plus the two bar slots, and all four recompute the product, so
base and engine cannot drift apart. Survey of the other paths:
QuickShape captures/restores the FULL product-carrying brush plus the
scalar multiplier and never recomputes one from the other - a preset
change mid-shape updates base for the live brush while the bake uses
its captured copy, both correct. The studio never writes the canvas
brush directly (Done -> presetCommitted -> setPaintBrush recaptures).
Stroke undo never touches the brush. Project load leaves the brush
alone. App restart: base starts at 1.0 until the first selection,
multiplier restores from settings - the pre-first-selection brush is
the default (base 1.0), where multiplier IS the absolute, same as
before. Unlike the override fingerprint there is no persisted copy to
go stale: base is rederived at every activation.

SEQUENCING, per the user's directive: CanvasBrushLock ran FIRST, both
configs, immediately after the canvas change and before any page
wiring - its setBrushOpacity(100)/(50) calls act on the default brush
(base 1.0), where product == old absolute, and both pins held
byte-identical (666f7b45 / cafcec7f). Measured, not reasoned, before
the rest was built.

GATE: Lifecycle (j) extended (not rewritten - its size assertions
stand): bar reads multiplier default 100; selecting HB Pencil leaves
the bar at 100 while the engine holds 0.55; the compounding rule at
the exact number 0.275; at 100 the product equals the preset byte for
byte through the codec (colour neutralized - selection adopts identity
colours by design); eraser symmetric against Gouache base 0.95.
Totals: Lifecycle 213. All five pins byte-identical both configs:
c1fbee31 / b602355f (coupled) / 666f7b45 / cafcec7f / 0bc24381.

## Pencil stamp promotion, batch one: 4H / 2B / 6B (2026-08-30)

WHAT: the user's scanned pencil stamps (G:\Brush_SankoTV_DEF) become
the tip shapes of the Sketching pencils, with per-grade tuning. NINE
brushes are in scope (H exists in the roster, so the H stamp is the
ninth); THIS batch ships the calibration three - 6B, 4H, 2B - per the
agreed iteration: the user draws with these before the remaining six
(H, 2H, 4B, Mechanical, Blue, Charcoal) are tuned, so their feedback
calibrates the rest. The proposed v1 tunings for all nine are in the
2026-08-30 report; expect them to move.

TWO FINDINGS RECORDED AT THE USER'S DIRECTION:
- POLARITY: tip masks are WHITE-ON-BLACK (coverage = brightness). The
  scans were probed pixel-level before use: corners 0, marks bright -
  correct convention, nothing inverted. A dark-on-white scan loaded as
  a tip renders INVERTED; check polarity before blaming the engine.
- HARDNESS IS INERT WITH A CUSTOM TIP: the procedural falloff is the
  ONLY consumer of hardness/effectiveHardness in tip rendering, so
  every custom-tip brush's hardness value does nothing at render time.
  The pencil recipes KEEP their hardness ladder values with an INERT
  comment at each site (the user's rule: never leave a number that
  reads as meaningful when it is not). Edge character for stamp tips
  comes from the stamp itself plus Noise.
- TILT ELONGATION IS INERT WITH A CUSTOM TIP TOO (2026-09-04, Drawing
  batch two): Graphite Block stroke width measured 39 / 39 / 38 px with
  tilt off / on-flat / on-tilted 0.8. Two instances is a pattern -
  **levers that go dead with a custom tip: hardness, tilt elongation**
  (both consumed only by the procedural tip path). Kept at the site
  with INERT comments like hardness. Levers that stay LIVE with a
  custom tip, measured: flow, opacity, spacing, size curve, scatter
  (perpendicular/along/count + its pressure curve), size/angle/
  roundness jitter, grain (preset/depth/contrast/scale), noise.
  Whoever adds the next custom-tip brush: check this list before
  tuning a dead lever.

ASSETS: pencil_4h_tip.png (822 KB), pencil_2b_tip.png (915 KB),
pencil_6b_tip.png (799 KB) - 2.5 MB this batch, projecting ~7.5 MB for
all nine. Converted RGB -> Grayscale8 with QT'S OWN conversion (a temp
tool, archived), so shipping the pre-converted asset is byte-identical
to converting at load, minus the repo weight. No downsampling: every
stamp is under the 2048 cap. All under-cap, all untouched.

TUNING v1 SHAPE (details in the recipes): all three move to Static
grain, grain-depth minimum 1.0, and low flow, aligned with the
promoted HB reference. The ladder: 4H = faint ceiling 0.30 reached
QUICKLY (flow 0.4 - hard pencils do not build), light tooth, zero
jitter. 2B = ceiling 0.70, flow 0.22, deeper tooth, slight angle
jitter. 6B = ceiling 0.92, slowest flow 0.18 (darkness earned over
passes), coarser tooth (scale 48), noise 0.22, deep size swell
(curve floor 0.2), tilt elongation kept.

GATE: (b12) stamp-tip census - every asset-bearing built-in (HB + the
three) must hold its tip at source dimensions, which also catches a
roster-building target missing brush_assets.qrc. Coupled pins
re-baselined together, cross-config identical: preview c1fbee31 ->
10c3106c, eraser b602355f -> c9f2ff70. Second batch will move both
again - expected, one reason, one commit. All other pins unmoved.
Totals: BrushLibrary 84, Lifecycle 213.

## Pencil promotion complete: v2 from the hand-test + batch two (2026-08-30)

THE FEEDBACK LOOP CLOSED: the user drew with the calibration three.
Verdicts: 2B right as-is (it becomes the soft-end reference); 4H right
in character but "texture too soft"; 6B "too similar to 2B - a bit
darker". All three default to size 25 (the ~1254px stamps at 25px
undersample-alias at stamp time - reads as pencil sparkle in practice,
per the HB precedent at 36; no performance cost, measured flat).

v2 MOVES, with the reasoning that survived:
- 4H: tooth deepened AND refined - depth 0.25 -> 0.40, scale 40 -> 30,
  noise 0.06 -> 0.08. Finer scale keeps the added texture reading as
  hard-pencil precision; the machined feel lives in the untouched line
  geometry.
- 6B: the naive lever (flow) was DECLINED - faster deposit makes 6B
  build like 2B and collapses the instrument distinction. Instead the
  opacity CURVE FLOOR rose 0.25 -> 0.4 (soft graphite bites dark at
  first touch - the felt "darker"), ceiling 0.92 -> 0.97, and the
  texture gap widened in the same move (scale 48 -> 56, noise -> 0.26).
  Flow stays 0.18: darkness is still earned over passes.

BATCH TWO shipped with feedback-shifted tunings: the hard end was
under-textured family-wide (H depth 0.45/scale 32, 2H 0.40/30), the
soft-end spread widened into ladders (scale 40/48/56 and opacity-floor
0.25/0.32/0.40 across 2B/4B/6B), Charcoal takes the 6B direction
further (scale 64, noise 0.30, floor 0.4), Mechanical and Blue
unchanged from the v1 table (the feedback does not reach them). Every
recipe carries the INERT hardness comment. All nine stamps verified
under-cap and white-on-black before use.

ASSETS: nine tips, 7.82 MB total (batch two adds 5.29 MB). Repo was
~127 MB packed - a one-time ~6%. The additive-history caveat is on
record: replacing an asset keeps the old blob forever; wholesale
re-scanning as a habit would be the point to consider LFS.

BUILD-TREE WARNING, third corruption event in app/build Debug in two
days (corrupt SankoTV.exe LNK1136, a zero-output SizeLock segfault,
now a SizeLock wrong-render - each cured by deleting intermediates
and rebuilding; sources proven innocent by Release passing and by
clean rebuilds going green). This pass finished on a FULLY WIPED and
rebuilt Debug config. If a fourth event lands, suspect the disk or
the toolchain, not the code - and check this list before debugging a
Debug-only failure as real.

GATE: (b12) census now covers all ten asset-bearing built-ins (nine
pencils + HB) at source dimensions. Coupled pins re-baselined together
(fourth legitimate movement), cross-config identical: preview 10c3106c
-> 6aee3d7f, eraser c9f2ff70 -> d7264fcb. All other pins unmoved.
Totals: BrushLibrary 90, Lifecycle 213.

## Pencil v3: measured, not guessed (2026-08-30)

Two v2 rounds under-delivered by hand-test; the user ordered
measurement over adjustment. A probe rendered real strokes with the
roster brushes (archived: seam_pencilprobe_20260830_2.cpp). Both v2
theories died on the numbers, and two ENGINE FACTS came out, recorded
here at the user's direction:

FINDING 1 - STAMP SPARSITY SILENTLY GOVERNS DEPOSIT RATE. A custom
stamp's coverage density is a per-dab deposit multiplier that shows up
in NO setting. The 6B scan is far sparser than the 2B scan, so v2's 6B
was LIGHTER than 2B at every pressure and pass despite ceiling 0.97 vs
0.70 and a higher opacity floor - measured: 2B darker by 38/255 at
full-pressure pass 1, 63/255 at pass 3. Ceilings never mattered:
nothing approaches them in real drawing (2B pass-3 full pressure hits
178/255 = exactly its 0.70 ceiling only in the extreme row). Whoever
tunes the next custom-tip brush: compare STAMP MEAN COVERAGE first,
and compensate with flow. v3 6B: flow 0.18 -> 0.45; measured shipped
ratios 6B/2B = 1.86 / 1.58 / 1.21 (pass 1) and 1.83 / 1.43 / 1.06
(pass 3) across pressures 0.15 / 0.5 / 1.0.

FINDING 2 - THE PAPER PRESET CANNOT PRODUCE VISIBLE TOOTH. Its texels
cluster ~0.8: contrast CLAMPS it flatter (values sit far above the 0.5
midpoint), and grain depth cannot help because depth is the valley
FLOOR (modulation lives in [1-depth, 1]) - a texture with no dark
texels never lets paper show through. Measured maximum tooth spread
with Paper: 5/255 at ANY depth/contrast/spacing/flow combination.
Anyone reaching for Grain Depth on a light brush must know the PRESET
is the ceiling, not the setting. Visible tooth needs a texture with
real dark texels + high depth: Charcoal preset at fine scale 30,
depth 0.90, contrast 3.0 measured spread 27/255 with valleys at 4/255
(near paper-white) - shipped for 4H, scaled for 2H (0.85/2.5) and H
(0.80/2.2, scale 32). Third contributor: overlap saturation (size 25 x
spacing 0.08 = a stamp every 2 px) flattens whatever survives; v3
de-saturates with flow 0.25-0.30 and spacing 0.11-0.12. Caveat on
H/2H: at size 3 the probe's core-band metric mixes line-edge falloff
with tooth, so their numbers are not comparable to 4H's - the
treatment is applied on the shared diagnosis and the user's hand
judges it.

WALL TEXTURE, MEASURED AND NOT SHIPPED (the user's one-change-at-a-
time rule): the 5184x3456 paper photo as custom grain at the v3 4H
settings measured spread 18 max at a much dimmer mean (6.8 vs the
shipped preset's 21.6 at spread 27) - NOT materially better than the
Charcoal preset. Recommendation stands: keep the preset.

SHIPPED == MEASURED: the probe's (e) section re-rendered the ROSTER
brushes after the edit and matched the candidate numbers exactly (4H
21.6/27/p5=4; 6B ratios above). Coupled pins re-baselined together,
cross-config identical: preview 6aee3d7f -> 159eeaa5, eraser d7264fcb
-> 159b1786. v2 was never committed, so the calibration commit
carries v3 - the known-wrong intermediate exists only in HANDOFF and
the report record.

## Identity colour design (b) + Charcoal v2, both measured (2026-08-30)

THE COLOUR BUG (general, not Blue-specific): activating a preset with a
non-black stored colour wrote it into the SHARED app colour and nothing
ever restored it - every pencil after Blue Pencil painted blue. Four
roster presets carry identity colours (Blue Pencil #4a90d9, Chalk
#f2f2f2, Sanguine #c05a3a, Sepia #70421e - Chalk the sneakiest:
near-white ink without noticing), plus any coloured user/ABR preset.
Colour IS codec-serialised preset data; the defect was the UI-state
handling (adopt-without-restore, deliberate at the time).

DESIGN (b), user-approved: identity colour applies WHILE ACTIVE; a
black-ink preset restores the pre-adoption colour; an explicit
setColor() while adopted WINS AND PERSISTS (restore state drops).
Captured only on the user->identity transition, so identity->identity
switches restore the ORIGINAL colour. Rejected: (a) permanent adoption
(the bug as felt) and (c) preset-colour-without-touching-app-colour
(the colour panel would show black while the pen paints blue - a worse
surprise). STALENESS AUDIT (the fingerprint/suppression lesson, done
up front): m_colorBeforeIdentity is in-memory only, never persisted
(colour resets at launch - no restart leak), untouched by project
load, eraser round-trips (setEraserPreset never writes m_color), undo
(stroke undo is pixel-level), and the QuickShape bake swap (which
round-trips m_color exactly). The only writers of m_color are
setColor, the adopt/restore block, and the QS transient pair.
Gate: Lifecycle (s), 10 checks - adopt, THE restore, identity->
identity->black restoring the original, the explicit-pick override,
and the eraser round-trip pair.

THE STAMP-COVERAGE CENSUS - check this FIRST when a custom-tip brush
feels wrong; two brushes (6B, then Charcoal) were misdiagnosed for
the same invisible reason and this table catches both in one look.
Mean coverage of each shipped stamp (probe-measured; deposit per dab
is proportional to it, and NO setting shows it):

  4H          157.5 / 255
  2H          119.8
  H           108.1
  HB           97.3
  Blue         70.2
  Mechanical   64.9
  2B           46.6
  4B           33.2
  Charcoal     29.2
  6B           18.5

The scans encode grade darkness IN the stamps: the ladder is real and
deliberate, and flow must compensate wherever a sparse stamp should
still deposit hard.

CHARCOAL v2, from the census: sparse stamp (29/255) at HALF 6B's flow
with the heaviest grain eating each deposit measured 36/255 at full
pressure vs 6B's 104. Fix mirrors 6B's: flow 0.25 -> 0.85, spacing
0.09 -> 0.07; measured shipped ratios Charcoal/6B = 0.38 / 0.86 /
1.12 across pressures. The light end deliberately stays lighter -
charcoal skates on the tooth until leaned on (the user's own words) -
and every roughness lever is untouched. Coupled pins re-baselined
together, cross-config identical: preview 159eeaa5 -> 0a5fc34d,
eraser 159b1786 -> 3cd942ee. Totals: Lifecycle 223.

## Family default size 25 - and size re-opens deposit ratios (2026-08-30)

Six pencils moved to the size-25 family default (H, 2H, 4B, Mechanical,
Blue, Charcoal; 4H/2B/6B already there). Mechanical and Blue were
checked for character damage before shipping: the fixed-width curve and
near-zero grain ARE the mechanical identity and hold at any size; both
ship at 25 without behavioural change beyond width.

THE FINDING the user's pre-build check caught - SIZE RE-OPENS MEASURED
DEPOSIT RATIOS. The naive invariance argument (overlap count = 1/spacing
regardless of size) is wrong for custom tips: STAMP DOWNSAMPLING is part
of the sparsity mechanism. Rendering the sparse charcoal scan at 6 px
under-resolves its coverage; at 25 px it deposits far more per dab. The
size-6-calibrated Charcoal (flow 0.85) measured Ch/6B = 2.18 at LIGHT
pressure at size 25 - out-darking 6B where charcoal must skate.
RECALIBRATED AT THE SHIPPED SIZE: flow 0.50, size-curve floor 0.22,
opacity-curve floor 0.28, spacing 0.07 - measured shipped ratios
Ch/6B = 0.89 / 0.94 / 1.06 (skates light, parity mid, darker leaned
on). RULE FOR NEXT TIME: a default-size change on a custom-tip brush
re-opens its measured ratios; re-measure at the shipped size, always.

BONUS from the same size change: H and 2H tooth metrics became valid
(the size-3 edge confound is gone) and land right beside 4H - spread
25 / 28 / 27 for H / 2H / 4H at means 36.6 / 26.7 / 21.6. The hard-end
tooth ladder is now measured, not scaled-by-analogy. 6B/2B ratios
unchanged (their sizes did not move).

Coupled pins re-baselined together, cross-config identical: preview
0a5fc34d -> d55d579f, eraser 3cd942ee -> 83db7f66. Lifecycle 223.

## Drawing-list stamps, calibration batch: Soft Pastel / Compressed Charcoal / Grease Pencil (2026-08-30)

The Drawing category's ten-brush stamp promotion, run measured-first
(census -> targets -> probe sweep at the APPROVED shipped sizes -> ship
the winning row -> re-verify the ROSTER). Three brushes implemented per
the approved calibration order; the other seven are proposed on paper
and wait for the user's hand-test of these three.

**Stamp census** (mean coverage of the Grayscale8 tip, 0-255; all ten
scans are 1254x1254, white-on-black polarity confirmed, under the 2048
cap). Coverage governs deposit rate - this table is why the three
brushes needed different flows:

| Stamp | mean | | Stamp | mean |
|---|---|---|---|---|
| Soft Pastel | 102.2 | | Chalk | 46.7 |
| Hard Pastel | 62.7 | | Sepia | 43.5 |
| Graphite Block | 58.7 | | Compressed Ch. | 42.3 |
| Charcoal Stick | 51.1 | | Conte Crayon | 37.6 |
| Sanguine | 50.2 | | Grease Pencil | 20.4 |

**Shipped tunings** (probe: 160-pt stroke, 1000x240, core band +/-4 px,
x in [200,800); p0.5x1 mean / build x3-over-x1 / p1.0 mean / spread
p95-p5):

- Soft Pastel (size 35, flow 0.14, Chalk depth 0.7 scale 56 con 2.0):
  42.4 / x2.54 / 108.2 / spread 25 - powdery, wide, builds up.
- Compressed Charcoal (size 30, flow 0.55, Charcoal depth 0.6): 72.3 /
  x2.16 / 171.3 / spread 87 - dense and dark from the first pass;
  grain depth LOWERED vs the Stick because compressed charcoal fills
  the tooth. Distinctness vs Charcoal Stick preserved by that pairing.
- Grease Pencil (size 25, flow 0.85, spacing 0.03, Paper depth 0.15):
  165.0 / x1.47 / 228.3 / spread 78 - waxy, heavy, low build-up
  (flat flow curve).

**Grease stamp limitation (measured cause, item-10 rule).** The brief
wants waxy-smooth (spread <= 8). With grain OFF entirely the spread is
still ~71: the variance IS the stamp's own internal texture, not any
tuning. Tightest useful spacing (0.03) only averages it to ~78. Fix,
if the hand-test wants it smoother: a denser/smoother re-scan of the
grease tip, NOT another tuning round.

Mechanics: three assets added to `brush_assets.qrc` (2.88 MB total);
(b12) census extended to pin all three tip dimensions; probe seams
archived (`seam_drawingprobe_20260830.cpp`,
`seam_stampconvert_20260830.cpp`). Old Soft Pastel roundnessJitter 0.2
deliberately dropped (unmeasured under the new stamp).

Coupled pins re-baselined together, cross-config identical: preview
d55d579f -> 0bc662dc, eraser 83db7f66 -> 1bf25d5b. Lifecycle 223,
BrushLibrary 93.

## Soft Pastel edge/tooth: pressure-inverted scatter (2026-08-30)

Hand-test verdicts on the Drawing calibration three, and what followed:

- **Grease Pencil: STAMP IS THE CEILING, confirmed by the user.** No
  further tuning. A wax-reading replacement stamp needs: one connected
  blob, interior mean >= 110/255, interior variation (p95-p5, central
  60%) <= ~25, no interior hole > ~2% of width, edge falling solid to
  nothing over 3-6% of diameter, grayscale white-on-black ~1254px.
  User is re-scanning (flat single press, smooth hot-press paper).
  Census any candidate before wiring it.
- **Compressed vs Stick: measured, deferred to batch two by the user.**
  Stick reads darker because it still runs the procedural drawBase
  recipe (flow 1.0, opacity 1.0, full-coverage disc): grain-off
  first-touch 128 vs the stamp's 87. Compressed already wins at full
  pressure (171 vs 150). Flow ~0.85 would close the first-touch gap,
  but Stick is scheduled for its own scan at size 35 in batch two -
  the pair calibrates JOINTLY then (polarity: Compressed denser/darker
  at first-touch AND full pressure). No interim bump.
- **Soft Pastel: "soft-density mark with a hard silhouette" - fixed,
  measured.** The stamp's own rim is hard (radial census: 145->32->1
  across 0.83R..0.98R, ~1-2px at stroke scale), so the cliff at dy12
  was the stamp outline. Engine findings that generalize:
  - **scatterPerpendicular is the "runs out" lever**: at 0.75-1.0 it
    produces a genuinely granular tail (occupancy grading ~50/30/15/5%,
    not a veil) but thins the full-pressure spine badly (p1.0 108->60
    at 0.75).
  - **scatterPressureCurve() can INVERT that cost**: curve2(1.0, 0.15)
    = full fray at light pressure, dense tight spine at full pressure.
    Control sources default to Pressure; no setControlSource needed.
  - **scatterCount N multiplies deposit ~N x** - pair it with a flow
    cut (0.14 -> 0.10 compensated count 2 exactly).
  - **Grain preset tooth ceiling at depth 1.0**: Chalk spread ~35,
    custom coarse tile ~38, Charcoal ~53 with real bare valleys (only
    preset that opens holes). Charcoal at depth 1.0 is the deepest
    available tooth.
  Shipped: scatterPerp 0.75, count 2, inverted curve, flow 0.10,
  Charcoal grain d1.0 c3.0 s48. Measured: first-touch 36.5 / build
  x2.53 / p1.0 108.9 (build character preserved vs 42.4/x2.54/108.2),
  interior spread 25 -> 57, cross-section cliff replaced by graded
  sputter (18.2/13.8/8.7/4.9 mean over dy 12-18, zero at 20). At p1.0
  the edge stays tight (37.9 -> 12.6 -> 0.5 over dy 16-20).

Coupled pins re-baselined together, cross-config identical: preview
0bc662dc -> 5304f43b, eraser 1bf25d5b -> 48dc5641. Lifecycle 223.
Probe archived as seam_drawingprobe_20260830_{2,3}.cpp.

## Drawing batch two: the remaining seven + the joint charcoal pair (2026-09-04)

Hard Pastel, Charcoal Stick, Chalk, Graphite Block, Conte Crayon,
Sanguine, Sepia promoted to their scans; Compressed Charcoal re-flowed
against the new Stick. Requirement-0 pass first (census + sweeps into
the scratchpad, report, approval), then implemented; shipped ==
measured for all eight, roster-verified.

**Census findings that generalize:**
- Every one of the seven stamps has a HARD rim (50% -> 10% of interior
  within 0.025R) and a granular interior (pixel std ~= mean). So: no
  Drawing stamp can fray its own edge (scatter is the only route), and
  interior spread floors near ~57-60 regardless of grain depth (Hard
  Pastel depth 0.8 -> 0.45 moved spread only 70 -> 58).
- **"Build-up" is NOT a tuning lever.** The x3-over-x1 ratio is
  compositing arithmetic on first-touch density: with a = first-touch
  / 255, build = 3 - 3a + a^2 (Soft Pastel a=0.143 -> 2.59 predicted /
  2.53 measured; Compressed a=0.284 -> 2.23 / 2.16). A target like
  "first-touch 55-70 AND build 1.7-2.0" is self-contradictory (build
  2.0 needs first-touch ~97). Retired family-wide: build is REPORTED,
  never targeted. (Brush::setBuildUp is a separate, unmeasured lever.)
- Tilt elongation inert with custom tips - see the hardness entry.

**Shipped** (size; first-touch / full / spread / light-stroke end):
Hard Pastel 35: 56.6/125.1/62/16 (scatter .35, Charcoal d.6, flow .28).
Charcoal Stick 35: 49.0/117.8/92/17 (scatter .50, d1.0, flow .40; old
rotationAffectsShape dropped unmeasured). Compressed 30: flow .55 ->
.70 = 86.0/192.8/96/10 - 1.76x the Stick at first touch, 1.64x at full
pressure; the user rejected a technically-correct 1.48x as "not
unmistakable" after the hand had read the pair inverted once. Chalk
30: 45.2/129.9/79/18 (full Soft Pastel technique: scatter .75 count 2
d1.0, flow .28, #f2f2f2 kept). Graphite Block 40: 51.3/139.4/60/17
(no scatter, Charcoal d.3 scale 30, flow .30, spacing .08; flatTip
and tilt widening gone). Conte 25: 59.7/163.8/86/11 (scatter .20,
d.5, flow .65, opacity .85). Sanguine 25: 51.6/125.5/67/12 and Sepia
25: 52.1/135.5/69/12 (scatter .35, d.6; flows .40/.48 differ only to
cancel coverage 50.2/43.5 - same density by design, colour is the
distinction).

**Scatter inheritance:** full - Chalk; moderated - Stick .50, Hard
Pastel .35, Sanguine/Sepia .35, Conte .20; none - Compressed (its
filled hard edge IS the pair polarity), Graphite Block (a film, not a
powder), Grease (untouched, awaiting re-scan).

**Measured stamp limitation (item-10), Graphite Block:** the "even
sheen" target (spread 20-35) is unreachable - floors at ~57-60 with
grain nearly off and spacing halved; it is the scan's interior
texture. Milder than Grease; the user chose to draw it and will
re-scan alongside Grease if the interior bothers the hand.

Seven assets (~7.2 MB) in brush_assets.qrc; (b12) census now pins 20
asset-bearing built-ins. Coupled pins re-baselined together,
cross-config identical: preview 5304f43b -> 3fb8f4d8, eraser 48dc5641
-> 1a68e706. Lifecycle 223. Probes archived as
seam_drawingprobe_20260904.cpp (sweeps) and _20260904_2.cpp (verify).

## Custom tips render at TRUE ASPECT + five re-scans + (b12) content pin (2026-09-04)

**Trigger.** "The Drawing tips all look the same in the Tip preview."
Diagnosed before touching anything: all ten assets were distinct
(ten SHAs, ten means matching the census), the qrc/roster paths were
one-to-one, and the preview renders each tip faithfully (ten distinct
renders, mean alpha tracking the census). The scans are simply the
same KIND of object - hard-rimmed discs of binary speckle differing
only in speckle density, which the eye cannot read off a 160 px
point-sampled disc. Meanwhile the user had re-scanned six sources on
G: AFTER the batch-two conversion (Conte and Grease became the same
file - Grease held), four of them non-square.

**The engine change.** The custom-tip path used to map the whole
image into the unit disc, stretching non-square scans square (and
had been doing so to HB 1177x1102, H 1296x1214, 2H 1295x1215 all
along, ~7% vertical stretch). Now `Brush::customTipExtent()` (longer
axis = 1, shorter = aspect; (1,1) for square/procedural; derived, not
serialised) bounds the image to |local| <= extent in tip-local space
and divides the sample coordinates by it. Sites: the CPU sampler in
`StrokeBuilder::shapedTipForStamp` (rectangle clip + divide),
`stamp.frag` and `color_stamp.frag` (same two lines each, including
the exact-sampling branch), and the two Globals uniform writers
(`vec2 tipExtent` at std140 offset 80 in the mono block, 72 in the
colour block - both fit the existing buffers). Angle, roundness,
tilt and flips compose BEFORE the extent, so the rectangle rotates
rigidly, squashes along local-x and mirrors within itself; the disc
clip still cuts the rectangle's corners as it cut a square's.

**Confinement proof (seam_tipaspect_20260904.cpp).** 30 built-ins
with custom tips rendered through shapedTipForStamp across 10 configs
(3 sizes x angle x roundness x flips x tilt), hashed BEFORE the change
and diffed AFTER: 22 square stamps byte-identical in every config;
exactly HB/H/2H (and, after promotion, the five re-scans) differ.
Extent maths direct: 384x192 tip at size 100 -> 100x50 footprint,
50x100 rotated 90 deg (100x100 before - the positive control). CPU vs
GPU on a non-square tip through SankoPaintHostAdapter::render: <= 1/255
plain, rotated, squashed+flipped, tilted - the rectangle clip now
exercised on both paths (today's square roster never could).

**Pre-existing finding, NOT this change:** a HARD-EDGED custom tip
(1 px texel edges) diverges CPU/GPU by up to ~180-250/255 on a tiny
pixel fraction (0.07% at 30 deg) at angles where an edge lands on a
sampler rounding boundary - deterministic, angle-dependent, identical
on square tips, present on the untouched engine. It is the class the
stamp.frag comments already name (fixed-point sampler weights vs
double bilinear); smooth tips (every shipped scan) agree at 1/255.
Recorded, not fixed; the seam asserts < 0.5% bad pixels for the hard
case so the class stays visible.

**Deltas, measured, NOT compensated (user draws and decides):**
| Brush | before (p0.5 / p1.0 / spread / ends) | after |
|---|---|---|
| HB Pencil (aspect only) | 43.0 / 107.2 / 14 / 10 | 43.4 / 106.2 / 14 / 10 |
| H Pencil (aspect only) | 34.7 / 79.2 / 27 / 8 | 35.0 / 79.5 / 27 / 7 |
| 2H Pencil (aspect only) | 25.0 / 65.3 / 27 / 9 | 24.9 / 64.2 / 27 / 9 |
| Chalk (re-scan, 1254 sq) | 45.2 / 129.9 / 79 / 18 | 58.1 / 145.2 / 95 / 18 |
| Charcoal Stick (992x1585) | 49.0 / 117.8 / 92 / 17 | 27.9 / 74.7 / 65 / 17 |
| Compressed (1024x1536) | 86.0 / 192.8 / 96 / 10 | 70.2 / 169.6 / 89 / 11 |
| Conte (1536x1024) | 59.7 / 163.8 / 86 / 11 | 71.5 / 153.5 / 77 / 8 |
| Sepia (1774x887) | 52.1 / 135.5 / 69 / 12 | 56.2 / 158.8 / 90 / 8 |
The three pencils moved within noise. Stick/Compressed polarity holds
(now 2.5x at first touch, 2.3x at full). Sepia no longer matches
Sanguine (56.2 vs 51.6, spread 90 vs 67) - the coverage-matched pair
is broken by the re-scan; flagged, not re-tuned.

**(b12) content pin.** Each asset-bearing built-in now pins dims +
sha256(Grayscale8 scanlines, 12 hex) + census mean (+-0.05), plus one
check that all 20 hashes are distinct. It caught all five re-scans
(Chalk by content alone - same dims). It cannot catch look-alike
scans, the G: folder changing under the repo, wrong tuning on a right
asset, or anything the preview does.

**Chalk loses its identity colour (2026-09-05).** Hand-test: "Chalk is
invisible". Diagnosed before changing: mask coverage normal (58.1 light
/ 145.2 full, in line with the visible Sanguine), so not opacity/flow;
the adopted #f2f2f2 over white paper caps contrast at 13/255 at full
alpha and gives 3/255 at the measured light-pressure alpha (black ink:
58/255). The white was removed - chalk is defined by being dry, not by
being white; white chalk is now a colour the user picks. Sanguine and
Sepia KEEP theirs (the colour is the medium). Mechanism untouched: the
rule is per-preset `color != black`, so Chalk simply moves from the
adopt side to the restore side (selecting it after Blue/Sanguine/Sepia
restores the user's colour, like 2B). Lifecycle (s) drives Blue Pencil
and Sepia - still two identity presets, no third case needed; three
identity built-ins remain (Blue, Sanguine, Sepia). Tuning untouched.

## Inking Part A: "Ink Bleed" -> "Rich Ink", id kept as a fossil (2026-09-05)

Built-in ids are slug(name), so a code rename silently moves the id -
and the shelf keys FIVE stores by id: Recent (the user's list holds
builtin/inking/ink-bleed today), hidden, favourites, UI renames, and
the Overrides folder (an override whose id vanished is dropped as
stale). Saved projects hold no brush ids (ProjectIO) and user
duplicates are self-contained user/<uuid> files, so those two are safe
either way. Decision: a `make(category, name, keptId, recipe)`
overload keeps the original id - "Rich Ink" IS builtin/inking/ink-bleed
- rather than migrating five stores for nothing visible. The proof that
the rename changed nothing: the preview and eraser swatch SHAs do NOT
move on this commit.

"Calligraphy Copy" is NOT a built-in: it is the user's own duplicate
(user/a0f8bf34..., Inking) differing from Calligraphy in exactly one
field (scatterPerpendicular 0.254 vs 0, identical tip). Not renamed:
Part B creates builtin/inking/rough-calligraphy fresh with its stamp
and a measured roughness mechanism; the user deletes the copy.

## Inking Part B, first two: Dry Ink + Brush Pen; Rough Calligraphy HELD (2026-09-05)

Requirement-0 census of the eight ink scans (G:\Brush_SankoTV_DEF\Ink
Brushes): all 1254x1254 except Rough Calligraphy 1024x1536 and Rich Ink
1230x1278; seven are near-binary (0-6% midtones) - genuinely hard-
edged; Brush Pen is the exception (23% midtones, soft rim). Measured
engine limits, all favourable: every stamp gives a 1 px stroke edge at
its proposed size (the procedural hardness-1.0 reference is 1 px) -
undersampling a hard scan keeps it hard; Brush Pen and Rich Ink soften
above ~1.5x size (their rims). None trips the hard-edge CPU/GPU fault
(<= 1/255, 0.000% at angles 0/20/30/45).

**Taper lever = the size-pressure FLOOR.** inkBase's 0.2 is what blunts
pen-down (3-5 px); 0.02 gives 1-2 px starts and lifts on every stamp,
slow (2 px point gaps) and fast (25 px) alike - the resampler
interpolates pressure. inkBase is left at 0.2; floors are set PER
BRUSH so nothing outside the stamped brushes changes (Studio Pen,
Technical Pen, Fine Liner untouched). Blob metric now measured from
the TRUE pen-down column: "visible from +N px" separates a late start
from a blob.

**Shipped (== measured from the roster):**
- Brush Pen @14: core 253 / occupancy 100% / edge 1 px (per-column
  0.0); taper 1/1/1/2 px at +2/+4/+8/+16 from pen-down, no blob, slow
  and fast. Floor 0.02 (0.05 and 0.08 gave 2 px blobs), spacing 0.03.
- Dry Ink @12: the scan alone is NOT broken at full pressure -
  overlapping dabs union to occupancy 100% at any spacing, and sparse
  spacing (0.15-0.25) makes a dab-string. Breakup comes from Charcoal
  grain depth 1.0 contrast 3.0 scale 30 in STATIC mode (fixed paper
  holes; Rolling averages across dabs into a soft 1.0 px transition).
  Core 155 / occupancy 65% / per-column edge 0.3 px; half pressure
  occupancy 27%; taper 1/1/1/1, no blob.

**Rough Calligraphy HELD - stamp limitation of the Grease class.** The
scan's CENTRE is empty (centre pixel 0, central 6% mean 98; 1-2 px
dabs deposit nothing), so a ramped stroke deposits nothing for the
first ~27-29 px and then appears at 2-4 px wide - a late start, with
the mirror at pen-up. No tuning fixes a hollow centre; it needs a
re-scan with the nib centred and solid at the centre. Its roughness
mechanism is measured and ready for when the scan arrives: scatter
0.25 gives 1.18 px edge raggedness at full pressure with a 0.0 px
per-column transition (ragged, not blurred); sizeJitter 0.3 alone
0.69 px. Asset not added; builtin/inking/rough-calligraphy not created.

**Default pressure curves are the IDENTITY** (PressureCurve() ->
{0,0}->{1,1}): every dynamic scales with pressure unless a recipe says
otherwise, so scatter already vanishes on a hairline. An "ascending"
scatter curve is therefore a no-op (measured); the Soft Pastel
INVERTED curve (1 -> 0.15) is real and re-verified: 108.9 at full
pressure vs 61.4 with the curve flat (1,1).

**Proposed, not implemented (await the hand-test):** Calligraphy 16
(stamp, rotation-following, floor 0.02), Ink Line & Splatter 8 (stamp
on the PRIMARY only; procedural dot secondary kept), Splatter 10,
Marker 18, Rich Ink 10 (crisp at 10, softens above 15). Assets for the
five stay in the scratchpad until their batch.

Coupled pins re-baselined together, cross-config identical: preview
b7f4179f -> e7d41822, eraser a06a968b -> a42203dc. (b12) +2 content
pins. Probe archived as seam_inkprobe_20260905{,_2}.cpp.

## SCAN SESSION BRIEF: three stamp-blocked brushes (2026-09-05)

Three brushes are held on their SCANS, not their tunings - each one
was measured to the cause, and the cause is structure the scan lacks.
Work from this at the scanner. Common to all three: grayscale, white
mark on black (the engine reads brightness as coverage; a dark-on-white
scan renders inverted), ~1254 px on the long side, mark centred in the
frame, no paper texture in the black (smooth hot-press paper, levels
pushed so the paper is 0). Non-square is fine since 2026-09-04 (true
aspect). Hand a candidate over and it gets a census before wiring;
the acceptance numbers below are what that census checks.

**1. Grease Pencil - a solid wax dab.** Press a grease/china marker
FLAT, once, on smooth paper: a single filled blob, not a stroke. Wax
fills the tooth, so the interior must be continuous.
- interior mean >= 110/255 (current scan: 20 - speckle);
- interior variation (p95-p5 over the central 60%) <= 25;
- no interior hole wider than ~2% of the blob;
- edge: solid -> nothing over 3-6% of the diameter (40-75 px at
  1254) - soft but short; no long fringe;
- one connected blob, round or oval.

**2. Rough Calligraphy - a nib mark, centred and solid at the centre.**
Press a broad/rough nib once, or a very short (< 1 nib width) drag.
The current scan's CENTRE is empty, so dabs under 3 px deposit nothing
and every stroke starts ~28 px late and ends early.
- the centre pixel and the central 6% of the frame >= 200/255 (current:
  0 and 98) - THE criterion; the taper lives or dies here;
- 1-2 px dabs must deposit: the census renders the tip at 1, 2, 3, 4 px
  and wants > 128 at every size;
- edge crisp (midtones < 10%), rough outline welcome - roughness is
  added by scatter (measured: 0.25 -> 1.18 px raggedness, 0.0 px
  per-column blur), it does not need to be in the scan;
- portrait or landscape both fine; the mark should fill >= 70% of the
  frame's long axis.

**3. Dry Ink - BRISTLE BANDS, not a blot.** Drag a dry brush 1-2 cm on
smooth paper; scan; crop a square where the streaks run LEFT-TO-RIGHT.
The engine rotates the tip with the stroke heading, so horizontal
bands in the scan become path-following streaks in any direction.
The current scan is an isotropic blot (dark-run ratio 0.85) and unions
to solid at any spacing; no setting turns a blot into bristles.
- 6-10 distinct bands across the crop (across = the vertical axis of
  the crop), each band >= 1/12 of the crop width so it survives
  size 12 (>= 1 px on canvas);
- 30-40% of the width un-inked (dark bands between bristles);
- bands span the FULL width of the crop left-to-right - a band that
  stops halfway becomes a gap that closes;
- dark-run ratio (mean dark run along x / along y in the central 60%)
  >= 4 - the census number that says "stripes", not "blot" (the
  synthetic proof tip measured 11.3);
- crisp band edges (midtones < 10%).

**4. Calligraphy - a FLAT NIB bar. HELD by decision (2026-09-05), not
overlooked.** The Calligraphy scan is not a flat nib: ink bounding box
947x1058 (aspect 0.90, 36% fill). Thick/thin - THE calligraphic
property - at size 16, horizontal vs vertical stroke width: stock
flatTip(0.22) 4 vs 16 px (4:1); the scan at angle 0 gives 12 vs 11
(none), at angle 45 8 vs 16 (2:1). Shipping the scan would DOWNGRADE
the brush; the procedural flat tip is currently the better
calligraphy pen and stays until a flat-nib scan exists. Do not promote
the existing Calligraphy.png later on the assumption it was missed.
Re-scan: press a flat nib straight down once (or a < 1 nib-width drag)
so the mark is a thin bar, aspect >= 4:1, fill >= 80% of its bounding
box, crisp. Census check: bbox aspect and the H/V width ratio at 16.

**Reading the inking brief on taper (decision 2026-09-05):** "all
Inking brushes taper" means the PEN-LIKE ones. Marker, Technical Pen
and Fine Liner are constant-width by identity (uniform size curve)
and stay so; do not read the brief literally and taper them later.

## Inking Part B, the four: Ink Line & Splatter, Splatter, Marker, Rich Ink (2026-09-05)

Requirement-0 structural screen of the five remaining scans found ONE
stamp-limited brush (Calligraphy, held - see the scan brief) and four
sound ones, shipped == measured from the roster:
- Ink Line & Splatter @8: scan on the PRIMARY only; the procedural
  dot-splatter secondary is the brush's identity and stays. Primary:
  core 252 / occupancy 100% / edge 1 px; taper 1/1/1/1, no blob. The
  scan is a sparse blot (17% fill) that unions to a clean line at 8
  and breaks up above 12 - do not enlarge it.
- Splatter @10: recipe unchanged + scan + angleJitter 1.0. The
  procedural disc measured as ONE merged 13,416 px blob over 600 px of
  stroke (scattered solid discs overlap); the scan gives 669 droplets,
  median 1 px, largest 464, spread 32 rows - real spatter.
- Marker @18: CONSTANT WIDTH by decision (uniform size curve), opacity
  0.6: core 151, occupancy 100%, edge 1 px.
- Rich Ink @10: core 254 / 100% / 1 px, taper 1/1/1/1; the old opacity
  ramp (0.4 -> 0.9 -> 1.0) IS the "rich"; hardness 0.55 was the
  "bleed" and is inert.

**The dual secondary, MEASURED through the real adapter** (the one
place a primary-tip swap could have an effect nobody looked for):
shipped vs a stock-equivalent (same preset, primary tip cleared, 0.2
floor restored). Secondary stamp lists byte-identical (738 stamps,
identical position and size sums). Controlled pixel comparison over
the region outside BOTH primary-only lines (512,372 px): 1,171
secondary-inked pixels in each, 0 differing, max alpha delta 0 -
positive control satisfied. Two probe gotchas recorded: a "residual"
defined against the primary alone changes when the primary's WIDTH
changes (112 -> 158 droplets - the same droplets, no longer covered),
and SankoPaintHostAdapter::render returns early on EMPTY primary
stamps, so "render the secondary alone" is vacuous - the positive
control caught it (0 inked px).

Six ink assets in brush_assets.qrc (3.0 MB); (b12) +4 content pins
(20 -> 26 asset-bearing built-ins). Coupled pins re-baselined together,
cross-config identical: preview e7d41822 -> 39f656b0, eraser a42203dc
-> a80dd7e0. Probe archived as seam_inkprobe_20260905_5_four.cpp.

## ENGINE FACT: streaks need tip-internal structure + heading-following rotation (2026-09-05)

Path-following breakup (dry brush, bristle streaks) cannot come from
grain or from any dab-to-dab lever. Measured on the shipped Dry Ink
scan, core band of a 601 px stroke, threshold 128:
- Static grain d1.0 c3.0 s30 (shipped): 441 gaps, median 2 px, max 8,
  run-length along/across 1.11 - isotropic pepper = "digital noise".
- Rolling grain: 100% solid (per-dab re-sampling averages the holes
  away). Stamp alone: 100% at any spacing. sizeJitter .5, scatter .3,
  angleJitter .5, noise .6: 100% or pinholes. spacing .15 + spacing
  jitter .8: 86% as 1 px holes; wider spacing = a DASHED line
  (full-width breaks), never bristles. Flow fade: saturates at inking
  spacing (flow accumulates across overlapping dabs).
- The engine has no per-dab opacity/flow jitter, and it would not help:
  a dab is a disc, and varying discs make dashes or pinholes.
- THE MECHANISM: a tip with stripes along its local x, plus
  `setControlSource(DynamicProperty::AngleJitter, ControlSource::
  Direction)` (the engine's orientation driver: the smoothed heading
  rotates the tip). Striped-tip CONTROL: static angle gives 60%
  occupancy as one 601 px x 2 px streak-gap on a horizontal stroke and
  100% solid on a vertical one (stripes cross the path and union-fill);
  with the heading driver, horizontal 60% and vertical 40%, gaps
  601 px long by 1.5-2 px - streaks that follow the path. Holds at
  size 18. Caveat to measure on a real scan: the heading is smoothed,
  so the first dabs orient to the initial estimate.
- Dry Ink stays SHIPPED as is (crisp, tapered, peppery): stamp-blocked,
  not tuning-blocked - the user chose crisp-and-peppery over a round
  that could only make it worse. Probe archived as
  seam_inkprobe_20260905_3_gapstructure.cpp.

**Follow-up (recorded, not done): StudioTipRing.** The angle/roundness
ring draws a circle/ellipse regardless of image aspect. Now that tips
can be rectangular, a ring that always shows an ellipse misrepresents
the tip - the thumbnail-drift class. The ring should carry the extent.

Coupled pins re-baselined together, cross-config identical: preview
3fb8f4d8 -> cd6fa6cb, eraser 1a68e706 -> a06a968b. Canvas locks and
erase baseline unmoved (procedural fixtures). Lifecycle 223. The
seam's before-file was written by the Release binary and matched by
the Debug binary: tip renders are cross-config identical too.

Gate note: run back-to-back, EdgeLock failed 52/51 ("SCREEN CAPTURE
FAILED - no undisturbed capture in 10 attempts"), QuickShape 5-14
("button 'N' missing") and Debug SizeLock (j) once ("corners inked:
0") - every one passed run ALONE with no rebuild. That is the
"runs spaced apart" rule in rule 4, not the Debug-tree corruption
class (which needs a rebuild to clear); count it as a reminder, not
as the 4th corruption event. Recurred identically on 2026-09-05 (both
inking gates): EdgeLock 49-51 "no undisturbed capture", Debug SizeLock
(j) once - every time green alone, no rebuild. (j) is now a repeat
offender specifically when SizeLock runs right after the other GUI
families in Debug; run it alone if it trips.
