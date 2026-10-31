#!/bin/bash
# fullgate.sh <tag> [serial] : THE GATE (the user's, 2026-10-31). Clean
# builds (with /MP), the nine families, Release and Debug.
#
# The normal mode overlaps what does not share a screen:
#   - the Debug clean build runs BESIDE the Release families (8 files at
#     once, so the tests keep cores);
#   - the window-free families (SankoBrushLibraryTest, SankoPaintPixelLock,
#     SankoCanvasBrushLock: no window) run BESIDE the window families, which
#     stay one at a time on the test screen, 2 s apart.
# "serial" is the old order: Release build, its families one at a time,
# then the Debug build and its families. IF A CHECK FAILS ONLY IN THE
# NORMAL MODE, rerun "serial" before treating it as a real failure, and
# note it in HANDOFF (the user's rule, 2026-10-31).
#
# Counting: each family's stdout (<family>_<config>.txt) and stderr
# (<family>_<config>.stderr.txt) are kept APART - FFmpeg logs to stderr and
# once landed in front of a PASS on the same line. PASS lines are counted
# on stdout only, FAIL lines on both, and checked against the family's own
# "checks=N" where it prints one. The exit code is the verdict.
#
# Output: app/build/_probe/split/fullgate_<tag>.log and fullgate_<tag>/.
export PATH="/c/Qt/6.11.1/msvc2022_64/bin:/usr/bin:/bin:$PATH"
A=/c/SankoTv/app
W="$A/build/_probe/split"
CM="/c/Qt/Tools/CMake_64/bin/cmake.exe"
TAG="${1:-run}"
MODE="${2:-normal}"
LOG="$W/fullgate_$TAG.log"
OUT="$W/fullgate_$TAG"
mkdir -p "$OUT"
: > "$LOG"
taskkill //F //IM SankoTV.exe > /dev/null 2>&1
T0=$(date +%s)
WINDOW_FREE="SankoBrushLibraryTest SankoPaintPixelLock SankoCanvasBrushLock"
WINDOWED="SankoQuickShapeGeometryLock SankoDevRecorderTest SankoCanvasEdgeLock SankoCanvasSizeLock SankoProjectLifecycle SankoCameraTest"
echo "mode: $MODE" >> "$LOG"

build() { # <config> [extra MSBuild arguments]
  local cfg=$1 s rc errs
  shift
  s=$(date +%s)
  "$CM" --build "$A/build" --config "$cfg" --clean-first "$@" > "$W/fullgate_${TAG}_build_${cfg}.log" 2>&1; rc=$?
  errs=$(grep -c -E "error C|error MSB|error LNK|Shader baking" "$W/fullgate_${TAG}_build_${cfg}.log")
  echo "build $cfg exit=$rc errors=$errs in $(( $(date +%s) - s ))s, done at +$(( $(date +%s) - T0 ))s  exe $(ls -la --time-style=+%H:%M:%S "$A/build/$cfg/SankoTV.exe" 2>/dev/null | awk '{print $6}')" >> "$LOG"
  return $rc
}

run() { # <family> <config>
  local t=$1 cfg=$2 s rc pass fail own n verdict
  s=$(date +%s)
  ( cd "$A" && "$A/build/$cfg/$t.exe" > "$OUT/${t}_${cfg}.txt" 2> "$OUT/${t}_${cfg}.stderr.txt" ); rc=$?
  pass=$(grep -a -c -E "^\s*PASS" "$OUT/${t}_${cfg}.txt")
  fail=$(cat "$OUT/${t}_${cfg}.txt" "$OUT/${t}_${cfg}.stderr.txt" | grep -a -c -E "^\s*(\*\*)?FAIL(\*\*| )")
  own=$(grep -a -E "RESULT|FAILURES [0-9]" "$OUT/${t}_${cfg}.txt" | tail -1 | cut -c1-70)
  n=$(echo "$own" | grep -o -E "checks=[0-9]+" | cut -d= -f2)
  verdict=""
  [ -n "$n" ] && [ "$n" != "$(( pass + fail ))" ] && verdict="  COUNT MISMATCH (checks=$n, pass+fail=$(( pass + fail )))"
  printf "%-30s %-8s exit=%-3s pass=%-4s fail=%-3s %4ss  (started +%ss)  %s%s\n" \
    "$t" "$cfg" "$rc" "$pass" "$fail" "$(( $(date +%s) - s ))" "$(( s - T0 ))" "$own" "$verdict" >> "$LOG"
}

families() { # <config>
  local cfg=$1 bg t
  if [ "$MODE" = "serial" ]; then
    for t in $WINDOW_FREE $WINDOWED; do run "$t" "$cfg"; sleep 2; done
    return
  fi
  ( for t in $WINDOW_FREE; do run "$t" "$cfg"; done ) &
  bg=$!
  for t in $WINDOWED; do run "$t" "$cfg"; sleep 2; done
  wait "$bg"
}

summary() { # <config>
  local cfg=$1 failing
  failing=$(grep -E "^Sanko[A-Za-z]+ +$cfg " "$LOG" | grep -v -c "exit=0 ")
  echo "FAMILIES FAILING ($cfg): $failing" >> "$LOG"
}

if [ "$MODE" = "serial" ]; then
  build Release && families Release
  summary Release
  build Debug && families Debug
  summary Debug
else
  build Release || { echo "BUILD FAILED - gate not run" >> "$LOG"; echo "ALL DONE" >> "$LOG"; exit 1; }
  ( build Debug -- -p:CL_MPCount=8 ) &   # "/p:" would be rewritten as a path by Git Bash
  DB=$!
  families Release
  summary Release
  wait "$DB"
  if grep -q -E "^build Debug exit=0 " "$LOG"; then
    families Debug
  else
    echo "BUILD FAILED - Debug families not run" >> "$LOG"
  fi
  summary Debug
fi
echo "TOTAL $(( $(date +%s) - T0 ))s" >> "$LOG"
echo "ALL DONE" >> "$LOG"
