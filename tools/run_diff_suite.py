#!/usr/bin/env python3
"""One-command multi-scene headless EOD_DIFF + EOD_HEAP_CHECK verification runner.

Usage:
  python tools/run_diff_suite.py --exe d:/eod-build/main/eod.exe \
    --addrs 001779C0,00177B40,00178258 --scenes afton,dundas,battle --frames 1800
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

DEFAULT_GAME_DIR = Path(r"D:\Test Drive - Eve of Destruction (USA)")

SCENES = {
    "afton": "mode=action;track=Afton;event=#0;car=Moth;paint=0;humans=1",
    "dundas": "mode=action;track=Dundas;event=#0;car=Moth;paint=0;humans=1",
    "battle": "mode=action;track=Dennison;event=Battle (8 AI);car=Moth;paint=0;humans=1",
    "detention": "mode=action;track=Dennison;event=Detention;car=Moth;paint=0;humans=1",
    "career": "mode=career;place=dealer",
}

DIFF_LINE_RE = re.compile(
    r"\[diff\]\s+([0-9a-fA-F]{8}):\s+(\d+)\s+calls,\s+(\d+)\s+checked.*,\s+(\d+)\s+mismatched"
)


def run_scene(
    exe: Path,
    game_dir: Path,
    sw_ini: Path,
    scene_name: str,
    direct_start: str,
    addrs: str,
    frames: int,
    every: int,
    heap_check: bool,
    log_dir: Path,
) -> tuple[int, dict[str, tuple[int, int, int]], list[str]]:
    env = os.environ.copy()
    env["EOD_HEADLESS"] = "1"
    env["EOD_MUTE"] = "1"
    env["EOD_SKIP_MOVIES"] = "1"
    env["EOD_DIRECT_START"] = direct_start
    env["EOD_DIFF"] = addrs
    env["EOD_DIFF_EVERY"] = str(every)
    if heap_check:
        env["EOD_HEAP_CHECK"] = "1"

    log_path = log_dir / f"diff_{scene_name}.log"
    cmd = [
        str(exe),
        "--headless",
        "--config",
        str(sw_ini),
        "--frames",
        str(frames),
    ]
    with log_path.open("w", encoding="utf-8", errors="replace") as lf:
        proc = subprocess.run(
            cmd,
            cwd=str(game_dir),
            env=env,
            stdout=lf,
            stderr=subprocess.STDOUT,
            timeout=300,
        )

    totals: dict[str, tuple[int, int, int]] = {}
    errors: list[str] = []
    for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = DIFF_LINE_RE.search(line)
        if m:
            totals[m.group(1).upper()] = (int(m.group(2)), int(m.group(3)), int(m.group(4)))
        if any(k in line for k in ("HEAP_CHECK", "heap error", "MISMATCH", "panic:")):
            if "0 heap errors" not in line.lower():
                errors.append(line.strip())
    return proc.returncode, totals, errors


def main() -> int:
    ap = argparse.ArgumentParser(description="Run multi-scene headless EOD_DIFF verification")
    ap.add_argument("--exe", type=Path, required=True, help="Path to eod.exe")
    ap.add_argument("--addrs", required=True, help="Comma-separated hex addresses for EOD_DIFF")
    ap.add_argument("--scenes", default="afton,battle", help=f"Comma-separated scenes ({','.join(SCENES)})")
    ap.add_argument("--frames", type=int, default=1800, help="Frames per scene (default: 1800)")
    ap.add_argument("--every", type=int, default=1, help="EOD_DIFF_EVERY sampling interval (default: 1)")
    ap.add_argument("--no-heap-check", action="store_true", help="Disable EOD_HEAP_CHECK=1")
    ap.add_argument("--game-dir", type=Path, default=DEFAULT_GAME_DIR)
    args = ap.parse_args()

    log_dir = args.exe.parent / "diff_logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    sw_ini = log_dir / "headless_sw.ini"
    sw_ini.write_text("[Graphics]\nRenderer=software\nRenderScale=1\nFullscreen=0\n", encoding="utf-8")

    scenes = [s.strip() for s in args.scenes.split(",") if s.strip()]
    any_fail = False

    print(f"Running EOD_DIFF suite on {args.exe} | addrs={args.addrs} | frames={args.frames}")
    print("-" * 86)
    for sc in scenes:
        ds = SCENES.get(sc, sc)
        rc, totals, errors = run_scene(
            args.exe,
            args.game_dir,
            sw_ini,
            sc,
            ds,
            args.addrs,
            args.frames,
            args.every,
            not args.no_heap_check,
            log_dir,
        )
        print(f"Scene: {sc:10} | exit={rc} | log={log_dir / f'diff_{sc}.log'}")
        if rc != 0:
            any_fail = True
        for addr in sorted(totals):
            calls, checked, mism = totals[addr]
            tag = "PASS" if mism == 0 else "FAIL"
            if mism > 0:
                any_fail = True
            print(f"  0x{addr}  calls={calls:6d}  checked={checked:6d}  mismatched={mism:4d}  [{tag}]")
        if not totals:
            print("  (no [diff] calls recorded)")
        for e in errors[:10]:
            print(f"  [ERR] {e}")
            any_fail = True
    print("-" * 86)
    print("RESULT:", "FAIL" if any_fail else "ALL PASS (0 mismatches)")
    return 1 if any_fail else 0


if __name__ == "__main__":
    sys.exit(main())
