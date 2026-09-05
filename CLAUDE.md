# SankoTV

SankoTV is a Qt6 / C++17 desktop application for animation directors — a
storyboarding and animatic tool with a real drawing engine. It is a mature
codebase (~5,200 commits): work inside its existing conventions, do not
propose fresh ones.

Recent work, discharged decisions, and deferred items live in the imported
handoff — keep that file current instead of duplicating its content here:

@HANDOFF.md

## What the app does

A director takes a screenplay through a pipeline of screens:
**Dashboard → Script Editor → Storyboard → Animatic → Generation**, with a
**Consistency Board** off the Storyboard for character/prop reference.
The Script Editor parses a screenplay into scenes; the Storyboard is where
the drawing happens (the heart of the app); the Animatic sequences panels
on a timeline with audio; Generation sends panels to fal.ai for AI video.

## Architecture

- **Model:** `Scene → QVector<Panel*> → QVector<Layer>`; a Layer holds an
  ARGB32_Premultiplied QImage. Layer groups are one level deep.
- **Canvas resolution is project-driven** (two-level authority): at load,
  the artwork's real pixel size wins over the manifest
  (`ProjectIO::projectFromJson` reconciles; a mismatch shows one dialog and
  the next save corrects the file — artwork is never rescaled, cropped, or
  discarded). At runtime `Panel::canvasSize()` (first layer's pixels) is
  the truth; `DrawingCanvas::canvasSize()` forwards to the active panel.
  Every layer factory takes a required QSize — never add a size default.
- **Save format:** `.sankotv` JSON plus sibling PNGs, serialized in
  `src/ProjectIO.{h,cpp}` (version stays 1). Two deliberate 960×540
  literals in ProjectIO are migration facts for pre-versioned files —
  do not remove them (each site's comment explains what breaks).
- **Drawing:** custom engine (`third_party/SankoPaintEngine`) — procedural
  and imported tips, pressure/tilt dynamics, dual brush, wet edges,
  build-up, colour dynamics, noise, texture. Presets are `.sankobrush`;
  Photoshop `.abr` import is supported.
- **QuickShape** (`third_party/QuickShapeKit`): hold the pen still after a
  stroke and it snaps to a recognised, editable vector shape.
- **Perspective tool:** tap-created vanishing points, derived horizon,
  stroke snapping onto guide directions.
- **UI is hand-written Qt Widgets** with custom `paintEvent` code; designs
  come from Figma, translated by hand. Use the shared component library
  (`src/brushlib/StudioControls.h`: StudioSlider, StudioChoiceRow,
  StudioToggleRow, StudioSegmentedRow, StudioTipRing, StudioTextField,
  StudioDropdown, `paintCapsule`). Do not use stock Qt widgets — no
  QComboBox, no unstyled QLineEdit, no default button styling.
- **Theme:** `SankoTheme.h` is the single source of colour truth. Accent
  `#7C6EF6` (`kAccent`); `kAccentLight` `#9E94F8` for accent text on dark.
  Do not add new colour literals.

## Environment

- Repo root `C:\SankoTv` — git commands must `cd /c/SankoTv` first.
- App at `C:\SankoTv\app`; Qt `C:\Qt\6.11.1\msvc2022_64`.
- CMake is not on PATH: use `C:\Qt\Tools\CMake_64\bin\cmake.exe`.
- The Bash tool needs `export PATH="/usr/bin:/bin:$PATH"`.
- GitHub: https://github.com/a3bashir/SankoTv

## Hard rules

Learned from real defects. Follow them exactly.

1. **Report before building.** When a task says "report, then WAIT" —
   investigate, report, and stop. When it says "propose before applying" —
   propose and stop. A stated checkpoint is a hard stop even when you are
   confident. Do not start coding past one.
2. **Verification seams.** Every substantive change gets a temporary test
   executable with lettered assertions covering the listed behaviours. Run
   it in both Debug and Release. Remove the seam and archive it to
   `app/tests/_backups/` before reporting done.
3. **Beware vacuous tests.** Every visibility or absence assertion needs a
   positive control that fails when it samples the wrong place or moment.
   "X is gone" proves nothing until the same sampler, at the same
   coordinates, has seen X while it was there. Real catches include: wrong
   hit region, stale archived artifact, transparent corner pixels instead
   of glyphs, an assertion validating itself, antialiasing sampled instead
   of flat fill.
4. **Eight-family gate.** Keep these green in both configs, runs spaced
   apart: SankoBrushLibraryTest, SankoPaintPixelLock, SankoCanvasBrushLock,
   SankoQuickShapeGeometryLock, SankoDevRecorderTest, SankoCanvasEdgeLock,
   SankoCanvasSizeLock, SankoProjectLifecycle. Preview SHA must stay
   `e7d418223357bcdfde0b64ecb254a624bc4ebf8fabc3812ff7f46e523088fe1b`
   (re-baselined 2026-09-05: Inking Part B, first two — Dry Ink and
   Brush Pen gain their scanned stamps with measured per-brush taper
   floors; shipped == measured; the eraser SHA
   `a42203dc…` moves WITH it, same commit, per the coupled-pin note in
   BrushLibraryTest); both pixel locks byte-identical
   (`666f7b45…`, `cafcec7f…`), erase baseline `0bc24381…`. The first
   six use 960×540 fixtures; SankoCanvasSizeLock is the variable-resolution
   lock — it draws real pixels at the far edge of five canvas sizes
   (960×540, 1920×1080, 2048×1080, 3840×2160, 777×1013) and locks
   persistence round-trips, migration/mismatch behaviour, cross-size
   staleness, and the flatten-thumb cache at a non-legacy size. What
   remains legacy-size only is SCREEN-PIXEL rendering: EdgeLock samples
   real screen pixels at 960×540 fixtures only — do not claim the gate
   proves variable-size display output.
5. **Build hygiene.** Kill any running `SankoTV.exe` before building.
   Verify the exe timestamp and the real exit code; grep build output for
   `error C|error MSB|error LNK|Shader baking`. Stale exes have produced
   false results before.
6. **Commit gate.** Do not commit or push without the user's explicit
   "commit and push". Build, verify, report, stop. Never stage
   `app/tests/CanvasEdgeLockTest.cpp` or
   `app/tests/QuickShapeGeometryLockTest.cpp`; if they show modified,
   leave them dirty and say so.
7. **Verification never touches real state.** Use a scratch settings key
   and scratch directories. Never write the real registry, the real brush
   library, or a real project. Assert the cleanup of everything a seam
   creates.
8. **Screen captures** use `QScreen::grabWindow` at native resolution.
   Never use `QWidget::grab()`.
9. **On-canvas chrome gets an opaque backing.** Anything drawn over
   artwork cannot rely on contrast against an assumed background — measure
   ratios over white paper AND over dark ink.
10. **Commit messages are prose that explains WHY** — decisions reversed,
    alternatives rejected, limitations accepted. Long is intentional.
11. **File rewrites.** Modify existing source with the Edit tool only. No
    shell heredocs for C++ content; no Python rewrite-in-place of existing
    files, not even for block deletions. Six incidents so far: five
    heredoc manglings, one Python text-mode rewrite that converted three
    files CRLF→LF. New files get CRLF endings to match the tree. If a
    whole-file operation is unavoidable, work in binary mode and verify
    endings afterwards with `file` and `git diff --stat`.

## Finding bugs

A built-in Dev Recorder captures real sessions to
`Documents/SankoTV-DevRecordings/<timestamp>/` as `events.jsonl` plus a
summary — every mouse/tablet event, widget state polls, perf samples. When
the user says "check the recordings", those events are the evidence.
