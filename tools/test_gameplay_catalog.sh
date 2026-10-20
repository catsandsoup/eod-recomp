#!/bin/sh
# Normal-input Welch acceptance probe; no selection/match RAM injection.
set -eu
# Usage: tools/test_gameplay_catalog.sh DISC MOD single|mixed [OUT] [FRAMES]
# Verified normal-input Welch journey for build_gameplay_mod's native-name catalog.
# Run sequentially with other games/builds. Captures prove only the tested route.
DISC=${1:?installed disc folder required}
MOD=${2:?generated gameplay mod root required}
MODE=${3:?single or mixed required}
OUT=${4:-work/welch-$MODE}
FRAMES=${5:-9000}
RIGHT=-5
cd "$(dirname "$0")/.."
DISC=$(cd "$DISC" && pwd)
MOD=$(cd "$MOD" && pwd)
mkdir -p "$OUT/mc"
OUT=$(cd "$OUT" && pwd)
cat > "$OUT/config.ini" <<'INI'
[Video]
Renderer=vulkan
RenderScale=1
AspectRatio=4:3
FrameSkip=0
VSync=0
[Audio]
Enabled=0
[Cheats]
UnlockAll=1
CareerCheats=0
TurboBoost=0
ChickenFight=0
Invincible=0
SinglePlayerBattle=1
BattleBots=1
[Gameplay]
CarDurability=0
ManualRecovery=0
INI
unset EOD_GODMODE
MENU=''
if [ "$MODE" = single ]; then
  MENU='350:start 1400:start 1650:x 1900:x 2200:x 2500:x'
  MAPSTART=2650
  CONFIRM=3400
  START=4100
  MENU="$MENU 3800:up"
  unset EOD_PAD_SCRIPT1
elif [ "$MODE" = mixed ]; then
  MENU='400:start 1500:start 1800:x 2100:x 2350:down 2450:x 2600:x 2900:x 3200:x'
  MAPSTART=3300
  CONFIRM=4000
  START=4700
  EOD_PAD_SCRIPT1=$(python3 tools/padscript.py 2750:x 3050:x 5250:l2 6000:l2 6700:l2 7400:l2 8100:l2 --hold 5500-5600:x+right)
  export EOD_PAD_SCRIPT1
else exit 2; fi
DIRECTION=right
if [ "$RIGHT" -lt 0 ]; then
  DIRECTION=left
  RIGHT=$((-RIGHT))
fi
N=0
while [ "$N" -lt "$RIGHT" ]; do
  MENU="$MENU $((MAPSTART + N * 45)):$DIRECTION"
  N=$((N + 1))
done
P0=$(python3 tools/padscript.py $MENU "$CONFIRM:x" "$START:x" 5200:l2 5900:l2 6600:l2 7300:l2 8000:l2 8700:l2 9400:l2 5600:select 6500:select 7400:select 8300:select --hold 5300-5500:x+left --hold 6100-6300:x)
printf '%s\n' "$P0" > "$OUT/player1-input.txt"
EOD_MOD_DIR="$MOD" EOD_HEADLESS=1 EOD_MUTE=1 EOD_INVINCIBLE=0 \
EOD_BATTLE_TRACE=1 EOD_MEMCARD="$OUT/mc" EOD_PAD_SCRIPT="$P0" EOD_DUMP_RAM="$OUT/ram.bin" \
./build-mac/eod --headless --config "$OUT/config.ini" --data "$DISC" --frames "$FRAMES" \
--screenshot-every 60 --screenshot-dir "$OUT" > "$OUT/out.log" 2>&1

# Require the actual map and active Battle; no output-folder-name inference.
if ! grep -Fq 'load_resource_set(Welch.trk) normal' "$OUT/out.log"; then
  echo 'Welch was not loaded; inspect the screenshots and normal-input route.' >&2
  exit 1
fi
if ! grep -q '\[BATTLE\].*phase=1' "$OUT/out.log"; then
  echo 'Active Battle was not reached; inspect the screenshots and log.' >&2
  exit 1
fi
printf 'Bounded Welch Battle evidence: %s\n' "$OUT"
