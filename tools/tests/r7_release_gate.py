#!/usr/bin/env python3
"""Check linked R7 backends, native gameplay scanout and the whole-run lifecycle audit."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))
from r7_audit import FIELDS, SUMMARY
from tex import read_png

DEFAULT_SCENES = (
    "menu",
    "mode=action;track=Anoka;event=No-Rules Race;car=Palo;paint=0;humans=1",
    "mode=action;track=Dundas;event=Battle;car=Bus;ai=12",
    "mode=action;track=Hampton;event=Battle;car=Postal",
)
DEFAULT_EOD = REPO_ROOT / "build-mac-r7-release" / "eod"

# Symbols (demangled substrings in text/code sections) that MUST be physically
# absent from an EOD_R7_RELEASE binary.
FORBIDDEN_SYMBOLS: list[tuple[str, str]] = [
    ("D3D11 GS backend", "gs::hw::d3d11::"),
    ("legacy batch emitter", "scene_emit::vu1_start("),
    ("legacy GS batch producer", "modern::post_draw("),
    ("legacy GS batch draw", "modern::gs_draw("),
    # Legacy Vulkan GS primitive rasterizer (gs_vulkan.cpp)
    ("gs_vulkan:draw_triangle", "gs::hw::draw_triangle("),
    ("gs_vulkan:draw_sprite", "gs::hw::draw_sprite("),
    ("gs_vulkan:draw_line", "gs::hw::draw_line("),
    ("gs_vulkan:draw_point", "gs::hw::draw_point("),
    ("gs_vulkan:build_state", "gs::hw::(anonymous namespace)::build_state("),
    ("gs_vulkan:ensure_state", "gs::hw::(anonymous namespace)::ensure_state("),
    ("gs_vulkan:draw_batch", "gs::hw::(anonymous namespace)::draw_batch("),
    ("gs_vulkan:push_sprite_slivers", "gs::hw::(anonymous namespace)::push_sprite_slivers("),
    ("gs_vulkan:vram_texture", "gs::hw::(anonymous namespace)::vram_texture("),
    ("gs_vulkan:texture_for", "gs::hw::(anonymous namespace)::texture_for("),
    # Legacy EOD_MODERN Vulkan GS pass (modern_vk.inc)
    ("modern_vk:modern_init", "gs::hw::(anonymous namespace)::modern_init("),
    ("modern_vk:modern_prepare", "gs::hw::(anonymous namespace)::modern_prepare("),
    ("modern_vk:modern_batches", "gs::hw::(anonymous namespace)::modern_batches("),
    ("modern_vk:modern_upload", "gs::hw::(anonymous namespace)::modern_upload("),
    ("modern_vk:mpipeline", "gs::hw::(anonymous namespace)::mpipeline("),
    # Legacy CPU GS software rasterizer (gs_draw.cpp)
    ("gs_draw:tri_spans", "gs::(anonymous namespace)::tri_spans"),
    ("gs_draw:sprite_rows", "gs::(anonymous namespace)::sprite_rows("),
    ("gs_draw:fill_rows", "gs::(anonymous namespace)::fill_rows("),
    ("gs_draw:shade_px", "gs::(anonymous namespace)::shade_px("),
    ("gs_draw:raster_worker", "gs::(anonymous namespace)::raster_worker("),
    ("gs_draw:run_job", "gs::(anonymous namespace)::run_job("),
    ("gs_draw:decode_tile", "gs::(anonymous namespace)::decode_tile("),
    ("gs_draw:tex_lookup", "gs::(anonymous namespace)::tex_lookup("),
    # General per-instruction and per-program execution, including the offline benchmark.
    ("VU0/VU1 interpreter loop", "vu_interp("),
    ("VU0/VU1 generic dispatch", "vu_execute("),
    ("VU0/VU1 software backend", "vu_dispatch_software("),
    ("VU0 macro interpreter", "vu0_macro"),
    ("VU0 reference control read", "vu0_cfc2"),
    ("VU0 reference control write", "vu0_ctc2"),
    ("VU0/VU1 benchmark", "vu_bench("),
    ("VU0/VU1 opcode decoder", "decode_pair("),
    # Legacy VU1 software interpreter (vu.cpp)
    ("vu:vu1_interp_execute", "vu1_interp_execute"),
]

# Positive sanity check: the Plume bridge and native vehicle solver must remain present
# (guards against running `nm` on an empty/stripped/corrupt binary).
REQUIRED_SYMBOLS: list[tuple[str, str]] = [
    ("plume_bridge:native_begin", "gs::hw::native_begin("),
    ("plume_bridge:native_display_copy", "gs::hw::native_display_copy("),
    ("native vehicle solver", "vehicle_dynamics_step("),
]


def check_symbols(eod_path: Path, nm: str) -> list[str]:
    failures: list[str] = []
    if not eod_path.is_file():
        return [f"binary not found: {eod_path}"]

    # Inspect all defined text symbols (local + external, demangled).
    proc = subprocess.run(
        [nm, "-C", str(eod_path)],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        return [f"nm -C failed ({proc.returncode}): {proc.stderr.strip()}"]

    text_lines: list[str] = []
    for line in proc.stdout.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[1] in ("T", "t"):
            text_lines.append(parts[2])

    for label, pattern in REQUIRED_SYMBOLS:
        if not any(pattern in sym for sym in text_lines):
            failures.append(f"required symbol missing ({label}): {pattern}")

    for label, pattern in FORBIDDEN_SYMBOLS:
        if pattern in {"vu0_macro", "vu0_cfc2", "vu0_ctc2"}:
            hits = [sym for sym in text_lines if sym.removeprefix("_") == pattern]
        else:
            hits = [sym for sym in text_lines if pattern in sym]
        if hits:
            failures.append(
                f"forbidden legacy symbol present ({label}): {hits[0]}"
                + (f" (+{len(hits) - 1} more)" if len(hits) > 1 else "")
            )

    if not failures:
        print(
            f"[r7-gate] PASS symbol exclusion: {len(FORBIDDEN_SYMBOLS)} forbidden legacy "
            f"GS/modern_vk/VU symbols absent, {len(REQUIRED_SYMBOLS)} native symbols present"
        )
    return failures


def check_map(path: Path) -> list[str]:
    contents = path.read_text(errors="replace")
    missing = [name for name in ("gs_vulkan.cpp", "native_renderer.cpp", "plume_vulkan.cpp",
                                 "vehicle_dynamics.cpp", "unretired.cpp")
               if name not in contents]
    forbidden = [name for name in ("gs_d3d11.cpp", "gs_hw_select.cpp", "gs_hw_stub.cpp", "scene_emit.cpp",
                                    "reference_control.cpp", "reference_interpreter.inc")
                 if name in contents]
    return ([f"link map lacks {name}" for name in missing] +
            [f"legacy backend in link map: {name}" for name in forbidden])


def check_build_graph(path: Path) -> list[str]:
    """CMake writes the actual eod target sources and both targets' definitions."""
    lines = path.read_text().splitlines()
    failures = []
    for target in ("eod", "eod_gen"):
        prefix = target + " definitions: "
        if not any(line.startswith(prefix) and "EOD_R7_RELEASE=1" in line[len(prefix):].split()
                   for line in lines):
            failures.append(f"{target} missing strict-release definition in CMake graph")
    sources = {line.replace("\\", "/").split("/")[-1] for line in lines}
    for required in ("unretired.cpp", "vehicle_dynamics.cpp", "vu.cpp"):
        if required not in sources:
            failures.append(f"CMake graph lacks {required}")
    for legacy in ("reference_control.cpp", "gs_hw_select.cpp", "gs_d3d11.cpp", "scene_emit.cpp"):
        if legacy in sources:
            failures.append(f"reference backend in CMake graph: {legacy}")
    return failures


def check_scene(output: str, scene: str) -> list[str]:
    failures = []
    if "[vulkan] device owned by plume" not in output or "EOD_R7_RELEASE: Plume native-only active" not in output:
        failures.append("missing native-only release or Plume device confirmation")
    if not re.search(r"^\[native\] gs frame \d+ .* GPU-recorded objects [1-9]\d*\b", output, re.MULTILINE):
        failures.append("no native frame recorded a draw into a GPU command buffer")
    mismatches = [line for line in output.splitlines() if line.startswith("[native] parity frame ")]
    if mismatches:
        failures.append(f"native submission mismatches: {len(mismatches)} reports, first: {mismatches[0]}")
    if scene == "menu":
        if "[netplay] main menu reached" not in output:
            failures.append("front end did not reach the main menu")
        if not re.search(r"\[native\] gs frame \d+ objects ([1-9]\d*) drawn \1 ", output):
            failures.append("no frame with a positive, complete menu draw count")
        return failures
    req = dict(field.split("=", 1) for field in scene.split(";") if field)
    if req.get("mode", "action") != "action":
        return ["the gameplay scanout gate requires an action scene"]
    # SLUS calls the AFTON.TRK event Woodly (find_eve in session_start.cpp).
    track_name = req["track"].removesuffix(".trk")
    track = re.escape("Woodly" if track_name.casefold() == "afton" else track_name)
    humans = int(req.get("humans", "1"))
    if humans == 1:
        selected = re.search(rf"^\[SESSION\] start: Eve \d+ '{track}' contest \d+ '([^']+)' vehicle \d+ humans 1$",
                             output, re.MULTILINE | re.IGNORECASE)
        # The short Battle request resolves to the retail roster-specific event.
        if not selected or not re.fullmatch(re.escape(req["event"]) + r"(?: \(\d+ AI\))?",
                                            selected[1], re.IGNORECASE):
            failures.append("requested track and event did not start")
    else:
        if not re.search(rf"\[SESSION\] head-to-head: live track Eve \d+ '{track}'", output, re.IGNORECASE):
            failures.append("requested split-screen track did not start")
        if not re.search(r"\[SESSION\] head-to-head: selected tracker position \d+ \(contest \d+\)", output):
            failures.append("split-screen contest selection was not confirmed")
        if len(set(re.findall(r"\[SESSION\] head-to-head: seat (\d+) joined", output))) != humans:
            failures.append("requested split-screen seats did not join")
        # A split run can successfully join and render the world while still
        # losing the projected-shadow strips for one or more viewports.
        views = [(0, 0), (0, 224)] if humans == 2 else [(0, 0), (320, 0), (0, 224), (320, 224)][:humans]
        strips = 20 if humans == 2 else 10
        for x, y in views:
            for stage in ("preparations", "composites"):
                if not re.search(rf"\[native\] EE shadow {stage} [1-9]\d* \({strips} strips, view x={x} y={y}\)", output):
                    failures.append(f"missing native shadow {stage} for viewport ({x}, {y})")
    for kind in ("world leaves", "queue records"):
        totals = re.findall(rf"\[native\] gs frame \d+ {kind} submitted (\d+) drawn (\d+)", output)
        if not any(int(submitted) > 0 and drawn == submitted for submitted, drawn in totals):
            failures.append(f"no frame with positive, complete native {kind} draws")
    if not re.search(r"hud P1 equal \d+/\d+ \(([1-9]\d*)/\1\)", output):
        failures.append("no positive, complete native HUD draw totals")
    return failures


def check_live_audit(eod: Path, frames: int, scale: int, scene: str, run_dir: Path) -> list[str]:
    run_dir.mkdir()
    card = run_dir / "card"
    card.mkdir()
    config = run_dir / "game.ini"
    config.write_text("[Video]\nRenderer=software\nNativeRenderer=0\nFrameSkip=0\n[Cheats]\nUnlockAll=1\n")
    env = os.environ.copy()
    env.pop("EOD_NATIVE_ONLY", None)
    env.update(EOD_R7_AUDIT="1", EOD_SKIP_MOVIES="1", EOD_MUTE="1", EOD_HEADLESS="1",
               EOD_DIRECT_START=scene, EOD_MEMCARD=str(card), EOD_NATIVE_LOG="30", EOD_NATIVE_PARITY_LOG="1")
    if scene == "menu":
        env.pop("EOD_DIRECT_START")
    humans = int(dict(field.split("=", 1) for field in scene.split(";") if "=" in field).get("humans", 1))
    if humans > 1:
        env.update(EOD_PAD_SCRIPT="0:0", EOD_PAD_SCRIPT1="0:0")
        if humans > 2:
            env.update({f"EOD_PAD_SCRIPT_SLOT{seat}": "0:0" for seat in range(1, humans)})
    shot = max(1, frames * 4 // 5)
    env["EOD_SCREENSHOT_AT"] = str(shot)
    cmd = [str(eod), "--config", str(config), "--headless", "--scale", str(scale),
           "--frames", str(frames), "--screenshot-every", str(shot), "--screenshot-dir", str(run_dir)]
    with (run_dir / "game.log").open("wb") as log:
        proc = subprocess.run(cmd, cwd=REPO_ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=180)
    output = (run_dir / "game.log").read_text(errors="replace")
    if proc.returncode:
        return [f"live run exited {proc.returncode}:\n" + "\n".join(output.splitlines()[-12:])]
    failures = check_scene(output, scene)
    try:
        width, height, rgba = read_png(str(run_dir / f"frame_{shot:05d}.png"))
        # Sample the playfield interior, away from letterboxing and HUD edges.
        samples = {rgba[(y * width + x) * 4:(y * width + x) * 4 + 3]
                   for y in range(height // 4, 3 * height // 4, max(1, height // 100))
                   for x in range(width // 4, 3 * width // 4, max(1, width // 100))}
        if len(samples) <= 1:
            failures.append("native scanout playfield is blank or flat")
        print(f"[r7-gate] scanout {width}x{height}, {len(samples)} sampled playfield colors at frame {shot}")
    except (OSError, ValueError) as error:
        failures.append(f"missing or invalid native scanout: {error}")
    summaries = [line for line in output.splitlines() if line.startswith("[r7-audit]")]
    audit = SUMMARY.fullmatch(summaries[0]) if len(summaries) == 1 else None
    if not audit:
        failures.append("missing, duplicate, or malformed R7 shutdown summary")
    else:
        remaining = [f"{field}={value}" for field, value in zip(FIELDS, audit.groups()) if int(value)]
        if remaining:
            failures.append("nonzero lifecycle counters: " + " ".join(remaining))
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--eod", type=Path, default=DEFAULT_EOD)
    parser.add_argument("--link-map", type=Path, help="Linker map from the same build")
    parser.add_argument("--build-graph", type=Path, help="CMake target sources from the same release build")
    parser.add_argument("--nm", default="nm", help="Symbol reader for the target executable")
    parser.add_argument("--frames", type=int, default=300)
    parser.add_argument("--scale", type=int, default=2)
    parser.add_argument("--scene", action="append", help="Action EOD_DIRECT_START string or menu (repeatable)")
    parser.add_argument("--skip-live", action="store_true", help="Check only binary and link-map exclusion")
    parser.add_argument("--artifacts", type=Path, help="Retain a new run directory with logs and PNGs here")
    args = parser.parse_args()
    if args.frames < 2 or args.scale < 1:
        parser.error("frames must be at least two and scale must be positive")
    eod = args.eod.resolve()
    map_path = args.link_map or eod.parent / "eod-r7-Release.map"
    try:
        graph = args.build_graph or map_path.with_name(map_path.stem + "-sources.txt")
        failures = check_symbols(eod, args.nm) + check_map(map_path) + check_build_graph(graph)
        if not failures and not args.skip_live:
            scenes = args.scene or DEFAULT_SCENES
            with tempfile.TemporaryDirectory(prefix="eod-r7-") as temporary:
                root = Path(temporary)
                if args.artifacts:
                    args.artifacts.mkdir(parents=True, exist_ok=True)
                    root = Path(tempfile.mkdtemp(prefix="r7-", dir=args.artifacts.resolve()))
                    print(f"[r7-gate] artifacts: {root}", flush=True)
                for index, scene in enumerate(scenes):
                    print(f"[r7-gate] scene {index + 1}: {scene}", flush=True)
                    try:
                        result = check_live_audit(eod, args.frames, args.scale, scene, root / str(index + 1))
                    except (ValueError, KeyError, OSError, subprocess.TimeoutExpired) as error:
                        result = [str(error)]
                    failures.extend(f"scene {index + 1}: {item}" for item in result)
    except OSError as error:
        failures = [str(error)]
    if failures:
        for item in failures:
            print(f"[r7-gate] FAIL: {item}", file=sys.stderr)
        return 1
    print("[r7-gate] PASS binary, map and build graph exclusion" if args.skip_live else "[r7-gate] PASS checked scenes and exclusion")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
