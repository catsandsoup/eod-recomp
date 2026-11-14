#!/usr/bin/env python3
"""Run a named game scenario headless and summarise it: one command per test situation.

  python3 tools/scenario.py dealer                 # Career from a copy of your save, warped to the dealer
  python3 tools/scenario.py garage --seconds 20 --pad 900:x
  python3 tools/scenario.py eve --eve 3
  python3 tools/scenario.py list

A scenario is: a memory card (a fresh copy of your real card, never the original), an EOD_DIRECT_START request,
the developer time scale (EOD_TIME_SCALE, default 8), optional scripted pad input (tools/padscript.py syntax),
and screenshots. The output folder gets game.log, the screenshots, sheet.png (a contact sheet) and summary.txt
(the session / warp / menu lines and any panic). Runs take the shared game-run lock, so only one game runs at once.
See docs/CAREER_WARP.md.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SUPPORT = Path.home() / "Library/Application Support/TDEODRecomp"
LOCK = SUPPORT / ".agent-game-run.lock"

CAREER_PLACES = ["here", "home", "dealer", "garage", "troy's place", "tj's", "time challenge 1", "time challenge 2", "next eve"]
SCENARIOS = {name.replace("'", "").replace(" ", "-"): f"mode=career;place={name}" for name in CAREER_PLACES}
SCENARIOS["eve"] = "mode=career;place=here"   # with --eve N

INI = """[Video]
Detail=maximum
Renderer=vulkan
Fullscreen=0
Width=1280
Height=960
VSync=1
AspectRatio=16:9
RenderScale=4
FrameSkip=1
ShowFps=1
[Audio]
Enabled=1
Volume=0
MusicVolume=0
"""


def pad_script(presses):
    if not presses:
        return ""
    return subprocess.check_output([sys.executable, str(REPO / "tools/padscript.py"), *presses], text=True).strip()


def contact_sheet(out: Path):
    try:
        from PIL import Image
    except ImportError:
        return None
    frames = sorted(out.glob("frame_*.png"))
    if not frames:
        return None
    step = max(1, len(frames) // 9)
    pick = frames[::step][:8] + [frames[-1]]
    w, h = 320, 224
    sheet = Image.new("RGB", (w * 3, h * ((len(pick) + 2) // 3)))
    for i, f in enumerate(pick):
        sheet.paste(Image.open(f).resize((w, h)), ((i % 3) * w, (i // 3) * h))
    path = out / "sheet.png"
    sheet.save(path)
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenario", help="scenario name, or 'list'")
    ap.add_argument("--seconds", type=int, default=15, help="real seconds to run (default 15)")
    ap.add_argument("--scale", type=int, default=8, help="EOD_TIME_SCALE (default 8; 1 = real time)")
    ap.add_argument("--pad", nargs="*", default=[], help="pad presses, e.g. 900:x 1200:down")
    ap.add_argument("--eve", type=int, help="run this Career Eve once free roam starts")
    ap.add_argument("--card", default=str(SUPPORT / "memcard"), help="memory card folder to copy (default: your card)")
    ap.add_argument("--bin", default=str(REPO / "build/eod"), help="game executable")
    ap.add_argument("--out", help="output folder (default: a new temporary folder)")
    ap.add_argument("--shot-every", type=int, default=300, help="screenshot every N frames")
    ap.add_argument("--env", nargs="*", default=[], help="extra VAR=value settings")
    a = ap.parse_args()

    if a.scenario == "list":
        for k, v in SCENARIOS.items():
            print(f"{k:18} {v}")
        return 0
    if a.scenario not in SCENARIOS:
        print(f"unknown scenario '{a.scenario}' (python3 tools/scenario.py list)", file=sys.stderr)
        return 2
    request = SCENARIOS[a.scenario] + (f";eve={a.eve}" if a.eve is not None else "")
    out = (Path(a.out) if a.out else Path(tempfile.mkdtemp(prefix=f"eod-scenario-{a.scenario}-"))).resolve()
    out.mkdir(parents=True, exist_ok=True)
    card = out / "card"
    if card.exists():
        shutil.rmtree(card)
    if Path(a.card).is_dir():
        shutil.copytree(a.card, card)          # a copy: the game may write to it
    else:
        card.mkdir()
    (out / "eod.ini").write_text(INI)
    (out / "appsupport").mkdir(exist_ok=True)

    env = dict(os.environ, EOD_HEADLESS="1", EOD_MUTE="1", EOD_SKIP_MOVIES="1", EOD_MEMCARD=str(card),
               EOD_APP_SUPPORT=str(out / "appsupport"), EOD_DIRECT_START=request, EOD_TIME_SCALE=str(a.scale),
               EOD_MENU_RUN_TRACE="1")
    script = pad_script(a.pad)
    if script:
        env["EOD_PAD_SCRIPT"] = script
    for kv in a.env:
        k, _, v = kv.partition("=")
        env[k] = v
    cmd = ["/usr/bin/lockf", "-k", str(LOCK), a.bin, "--data", str(SUPPORT / "disc"), "--config", str(out / "eod.ini"),
           "--headless", "--screenshot-every", str(a.shot_every), "--screenshot-dir", str(out)]
    print(f"[scenario] {a.scenario}: {request} at {a.scale}x for {a.seconds}s -> {out}")
    with open(out / "game.log", "w") as log:
        p = subprocess.Popen(cmd, env=env, stdout=log, stderr=subprocess.STDOUT, cwd=out)
        start = time.time()
        while p.poll() is None and time.time() - start < a.seconds:
            time.sleep(0.5)
        if p.poll() is None:
            p.kill()
            p.wait()
    keys = ("[SESSION]", "[CAREER_WARP] teleport", "[CAREER_EVE]", "[MENU_RUN]", "panic", "fatal", "exceeded")
    lines = [l.rstrip() for l in open(out / "game.log", errors="replace") if any(k in l for k in keys)]
    (out / "summary.txt").write_text("\n".join(lines) + "\n")
    print("\n".join(lines[-15:]))
    sheet = contact_sheet(out)
    print(f"[scenario] contact sheet: {sheet}" if sheet else "[scenario] no screenshots")
    return 1 if any(("panic" in l or "fatal" in l) for l in lines) else 0


if __name__ == "__main__":
    sys.exit(main())
