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
