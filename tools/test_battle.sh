#!/bin/sh
# Reproducible scripted-input evidence, not a substitute for hands-on acceptance.
# Usage: tools/test_battle.sh DISC vanilla|weapon|single|mixed [OUTPUT] [FRAMES] [Dennison|Home|Mankato|Woodly|Anoka]
set -eu
DISC=${1:?disc folder required}
MODE=${2:?vanilla, weapon, single, or mixed required}
OUT=${3:-work/battle-$MODE}
FRAMES=${4:-9000}
TRACK=${5:-Dennison}
cd "$(dirname "$0")/.."
PY=${PYTHON:-python3}
mkdir -p "$OUT/mc"
OUT=$(cd "$OUT" && pwd)
BOTS=0
SP=0
UNLOCK=1
case "$MODE" in
  vanilla|weapon) UNLOCK=0 ;;
  single) BOTS=1; SP=1 ;;
  mixed) BOTS=1 ;;
  *) echo 'mode must be vanilla, weapon, single, or mixed' >&2; exit 2 ;;
esac
cat > "$OUT/config.ini" <<INI
[Video]
Renderer=vulkan
RenderScale=1
AspectRatio=auto
FrameSkip=0
VSync=0
[Audio]
Enabled=0
[Gameplay]
UnlockAll=$UNLOCK
CareerCheats=0
TurboBoost=0
ChickenFight=0
CarDurability=0
ManualRecovery=0
TurboScale=1
ChickenForceScale=1
ChickenRadiusScale=1
SinglePlayerBattle=$SP
BattleBots=$BOTS
INI
if [ "$MODE" = single ]; then
  P0=$($PY tools/padscript.py 350:start 1400:start 1650:x 1900:x 2200:x 2500:x 2800:x \
    3000:up 3500:x \
    4400:l2 4700:l2 5000:l2 5300:l2 5600:l2 5900:l2 6200:l2 6500:l2 6800:l2 7100:l2 7400:l2 7700:l2 8000:l2 8300:l2 \
    5500:select 6100:select 6700:select 7300:select 7900:select 8500:select 9100:select 9700:select 10300:select 10900:select \
    --hold 4300-4420:x+left --hold 5000-5120:x+right --hold 6000-6200:x)
  unset EOD_PAD_SCRIPT1
else
  P0=$($PY tools/padscript.py 400:start 1500:start 1800:x 2100:x 2350:down 2450:x 2600:x 2900:x 3200:x 3500:x \
    4000:up 4030:up 4060:up 4090:up 4120:up 4150:up 4180:up 4210:up 4240:up 4270:up 4500:x \
    5200:l2 5500:l2 5800:l2 6100:l2 6400:l2 6700:l2 7000:l2 7300:l2 7600:l2 7900:l2 8200:l2 8500:l2 \
    6500:select 7100:select 7700:select 8300:select 8900:select 9500:select 10100:select 10700:select 11300:select \
    --hold 5300-5400:x+left --hold 5700-5800:x 6000:r1 --hold 6200-6300:x+right)
  EOD_PAD_SCRIPT1=$($PY tools/padscript.py 2750:x 3050:x 5250:l2 5550:l2 5850:l2 6150:l2 6450:l2 6750:l2 7050:l2 7350:l2 7650:l2 7950:l2 8250:l2 \
    6550:select 7150:select 7750:select 8350:select 8950:select 9550:select 10150:select 10750:select 11350:select \
    --hold 5400-5500:x+right --hold 5800-5900:x 6050:r1 --hold 6350-6450:x+left)
  export EOD_PAD_SCRIPT1
fi
# Track selection uses native unlocked menu order; no track/state memory pokes.
# Keep the solo smoke route on Dennison until larger arenas pass navigation tests.
if [ "$MODE" = single ] && [ "$TRACK" != Dennison ]; then
  echo 'Non-Dennison map tests currently use mixed mode/native multiplayer layouts.' >&2
  exit 2
fi
if [ "$MODE" = weapon ] && [ "$TRACK" != Dennison ]; then
  echo 'The native weapon scenario currently targets Dennison.' >&2
  exit 2
fi
P0=$($PY tools/battle_test_inputs.py "$P0" "$TRACK" "$FRAMES" 0 "$MODE")
if [ "$MODE" != single ]; then
  EOD_PAD_SCRIPT1=$($PY tools/battle_test_inputs.py "$EOD_PAD_SCRIPT1" "$TRACK" "$FRAMES" 1 "$MODE")
  export EOD_PAD_SCRIPT1
fi
printf '%s\n' "$P0" > "$OUT/player1-input.txt"
printf '%s\n' "${EOD_PAD_SCRIPT1:-disconnected}" > "$OUT/player2-input.txt"
EOD_HEADLESS=1 EOD_MUTE=1 EOD_BATTLE_TRACE=1 EOD_MEMCARD="$OUT/mc" \
  EOD_PAD_SCRIPT="$P0" EOD_DUMP_RAM="$OUT/ram.bin" \
  ./build-mac/eod --headless --config "$OUT/config.ini" --data "$DISC" --frames "$FRAMES" \
  --screenshot-every 60 --screenshot-dir "$OUT" > "$OUT/out.log" 2>&1
# Require the named native resource as well as combat. Menu wrap must not
# silently turn an output directory name into evidence for the wrong map.
case "$TRACK" in
  Dennison) RESOURCE=Dennison.trk ;;
  Home) RESOURCE=homebase.trk ;;
  Mankato) RESOURCE=Mankato.trk ;;
  Woodly) RESOURCE=Afton.trk ;;
  Anoka) RESOURCE=Anoka.trk ;;
esac
if ! grep -Fq "load_resource_set($RESOURCE) normal" "$OUT/out.log"; then
  echo "Requested $TRACK but expected resource $RESOURCE was not loaded; inspect captures." >&2
  exit 1
fi
# Combat alone is not player acceptance: inspect attacks, recovery and results.
grep -q '\[BATTLE\].*phase=1' "$OUT/out.log"
printf 'Battle evidence: %s\n' "$OUT"
