#!/bin/sh
# tools/test_2p_race.sh <disc folder> [outdir]
#
# GOALS: 2-player split screen with two independent controllers. Without two
# physical pads this proves it with two scripted pads, headless and muted:
# ACTION > RACE > MULTI, both players join on their own ports, DENNISON,
# event 1. Both players accelerate from vblank 4300; only player 2 steers
# left for the first two seconds. The contact sheet must show distinct paths:
# P1 continues straight while P2 turns. A rolling start means an idle player's
# nonzero speed alone cannot prove input; leaving P2 idle also traps it against
# a barrier before a delayed throttle press can visibly move the car.
set -e
DISC=${1:?usage: tools/test_2p_race.sh <disc folder> [outdir]}
OUT=${2:-work/test_2p}
cd "$(dirname "$0")/.."
PY=${PYTHON:-python3}
mkdir -p "$OUT/mc"
P0=$($PY tools/padscript.py 400:start 1500:start 1800:x 2100:x 2350:down 2450:x 2600:x 2900:x \
     3200:x 3500:x 3950:x --hold 4300-6600:x)
P1=$($PY tools/padscript.py 2750:x 3050:x --hold 4300-6600:x --hold 4300-4420:left)
# Optional renderer/config overrides make identical software and native-scale
# hardware benchmark runs repeatable without changing the user's eod.ini.
set -- --headless --data "$DISC" --frames 6600
if [ -n "${EOD_TEST_RENDERER:-}" ]; then set -- "$@" --renderer "$EOD_TEST_RENDERER"; fi
if [ -n "${EOD_TEST_CONFIG:-}" ]; then set -- "$@" --config "$EOD_TEST_CONFIG"; fi
EOD_HEADLESS=1 EOD_MUTE=1 EOD_FPS=1 EOD_MEMCARD="$PWD/$OUT/mc" \
EOD_PAD_SCRIPT="$P0" EOD_PAD_SCRIPT1="$P1" \
  ./build-mac/eod "$@" \
  --screenshot-every 150 --screenshot-dir "$OUT" > "$OUT/out.log" 2>&1
$PY tools/contact_sheet.py "$OUT" 4200 16 4
echo "contact sheet: $OUT/sheet.png (top = player 1, bottom = player 2)"
