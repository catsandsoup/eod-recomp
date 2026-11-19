#!/bin/bash
# P3 gate run (docs/NATIVE_RENDERER.md "Stage R5"): one headless, muted race with
# EOD_NATIVE_ONLY=2 (Plume draws all 3D, the GS only sprites), the GS command stream recorded at
# each vblank of $V (6 vblanks each), screenshots around V+6, camera views cycled with Triangle,
# then tools/p3_compare.py against the software GS replay of the same windows.
#
# usage: EXE=build/eod.exe DATA="<disc dir>" OUT=<dir> tools/p3_run.sh NAME TRACK EVENT CAR [env...]
#   V=v1,v2,...  vblanks (default 8 spread over 850..2250); FR frames (default 2260)
#   Names exactly as the [CATALOG] list prints them. Do not combine with EOD_DIFF on hot functions:
#   a slow EE leaves replay windows without a whole GS frame (the tool skips those).
set -u
N=$1; T=$2; E=$3; C=$4; shift 4
ROOT=$(cd "$(dirname "$0")/.." && pwd)
EXE=${EXE:-$ROOT/build/eod.exe}
OUT=${OUT:?set OUT to a scratch directory}
V=${V:-850,1050,1250,1450,1650,1850,2050,2250}
VS=$(python -c "import sys; print(','.join(str(int(x) + 6) for x in sys.argv[1].split(',')))" "$V")
PAD="500:4000"
for vb in 700 900 1100 1300 1500 1700 1900 2100; do PAD="$PAD,$vb:5000,$((vb + 8)):4000"; done
D="$OUT/$N"
rm -rf "$D"; mkdir -p "$D/mc"
for r in vulkan software; do
  cat > "$OUT/$N.$r.ini" <<INI
[Video]
Detail=maximum
DrawDistance=8
Renderer=$r
Fullscreen=0
Width=1280
Height=960
VSync=0
AspectRatio=16:9
RenderScale=1
FrameSkip=0
ModernRenderer=0
[Audio]
Enabled=0
[Cheats]
UnlockAll=1
INI
done
env EOD_HEADLESS=1 EOD_MUTE=1 EOD_SKIP_MOVIES=1 EOD_MEMCARD="$D/mc" EOD_NATIVE=1 EOD_NATIVE_ONLY=${ONLY:-2} \
  EOD_CAMERA_TRACE=1 "EOD_PAD_SCRIPT=$PAD" EOD_GS_DUMP="$OUT/$N.gsd" EOD_GS_DUMP_AT="$V" EOD_GS_DUMP_N=6 \
  EOD_SCREENSHOT_AT="$VS" "$@" "EOD_DIRECT_START=mode=action;track=$T;event=$E;car=$C" \
  "$EXE" --headless --config "$OUT/$N.vulkan.ini" --data "${DATA:?set DATA to the game disc directory}" \
  --frames "${FR:-2260}" --screenshot-every 1 --screenshot-dir "$D" > "$D/game.log" 2>&1
echo "exit $?"
grep "\[SESSION\] start" "$D/game.log"
python "$ROOT/tools/p3_compare.py" --exe "$EXE" --live "$D" --dump "$OUT/$N.gsd" --vblanks "$V" \
  --ini "$OUT/$N.software.ini" --search 3..9 --csv "$OUT/$N.csv"
