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
