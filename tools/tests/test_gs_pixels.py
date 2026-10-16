#!/usr/bin/env python3
"""Headless procedural GS regression: register packets and generated pixels only."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument("--eod", type=Path, required=True)
parser.add_argument("--generator", type=Path, required=True)
parser.add_argument("--work-dir", type=Path, required=True)
args = parser.parse_args()
eod, generator, work = args.eod.resolve(), args.generator.resolve(), args.work_dir.resolve()
work.mkdir(parents=True, exist_ok=True)
config = work / "pixels.ini"
config.write_text("[Video]\nRenderScale=1\n")
env = os.environ.copy()
env.update(EOD_HEADLESS="1", EOD_MUTE="1", EOD_GS_RASTER="0")
for key in ["EOD_GS_REPLAY_STOP", "EOD_GS_REPLAY_TRACE", "EOD_GS_DUMP", "EOD_GS_REPLAY_DUMP", "EOD_SCREENSHOT_SCALE", "EOD_SCREENSHOT_WIDTH", "EOD_SCREENSHOT_HEIGHT"]:
    env.pop(key, None)

config6 = work / "pixels6.ini"
config6.write_text("[Video]\nRenderScale=6\n")

def render(case, renderer, scale=1):
    fixture = work / (case + ".gsd")
    subprocess.run([str(generator), case, str(fixture)], check=True)
    run_env = dict(env, EOD_SCREENSHOT_SCALE=str(scale)) if scale > 1 else env
    result = subprocess.run([str(eod), "--headless", "--config", str(config6 if scale > 1 else config), "--renderer",
                             renderer, "--gs-replay", str(fixture), "--repeat", "1"], env=run_env,
                            capture_output=True, text=True, timeout=30, cwd=work)
    tag = case + "-" + renderer + ("-%dx" % scale if scale > 1 else "")
    (work / (tag + ".log")).write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(result.stderr)
    if renderer == "vulkan" and "replaying with vulkan" not in result.stderr:
        print("SKIP: Vulkan device unavailable; software fallback is not GPU verification")
        sys.exit(77)
    data = Path(str(fixture) + ".ppm").read_bytes()
    Path(work / (tag + ".ppm")).write_bytes(data)
    size = 64 * scale
    magic, dimensions, maximum, pixels = data.split(b"\n", 3)
    assert (magic, dimensions, maximum, len(pixels)) == (b"P6", b"%d %d" % (size, size), b"255", size*size*3)
    return lambda x,y: tuple(pixels[(y*size+x)*3:(y*size+x)*3+3])

for renderer in ["software", "vulkan"]:
    flat = render("flat", renderer)
    assert flat(16,16) == (37,83,126), (renderer, "flat control", flat(16,16))
    gouraud = render("gouraud", renderer)
    assert gouraud(9,9) == (10,21,31), (renderer, "RGBA must truncate", gouraud(9,9))
    fog = render("fog", renderer)
    assert fog(16,16) == (128,128,128), (renderer, "constant fog colour", fog(16,16))
    uv = render("uvsnap", renderer)
    assert uv(16,16) == uv(16,40), (renderer, "UVs within one 1/16-texel bucket", uv(16,16), uv(16,40))
    assert max(uv(16,16)) - min(uv(16,16)) == 0
    overlap = render("alpha_overlap", renderer)
    assert overlap(16,16) == (200,200,160), (renderer, "alpha-fail shadow must precede foreground fill", overlap(16,16))
    for case in ["alpha_depth_strict", "alpha_depth_varying"]:
        depth = render(case, renderer)
        assert depth(16,16) == (200,200,160), (renderer, case, "later depth must reject", depth(16,16))
    edge = render("edge_sliver", renderer)
    assert edge(63,63) == (200,40,40) and edge(63,30) == (200,40,40), (renderer, "GS fills the last scissor pixel")
    print(renderer + ": flat, RGBA, fog, UV bucket, alpha ordering, depth fallback and edge pixel passed")

# Upscaled, the last scissor column/row must be filled across the whole native
# pixel, not only the sub-pixels left of the sprite's fractional edge.
edge6 = render("edge_sliver", "vulkan", 6)
for x, y in [(383, 200), (200, 383), (383, 383), (378, 378), (53, 53)]:
    assert edge6(x, y) == (200,40,40), ("vulkan 6x edge sub-pixel", x, y, edge6(x, y))
assert edge6(47, 200) == (32,64,96), ("vulkan 6x: nothing before the first covered pixel", edge6(47, 200))
# A Z-only clear of a PSMZ16 buffer later shown as PSMCT16: the uncleared
# sub-pixels of Z column 63 would appear as a line at colour column 31.
for renderer in ["software", "vulkan"]:
    clear = render("zclear_relayout", renderer)
    assert all(clear(x, y) == (0,0,0) for x in (0, 31, 32, 63) for y in (0, 31, 32, 63)), (renderer, "Z16 clear")
clear6 = render("zclear_relayout", "vulkan", 6)
for x in range(0, 378):  # (the display filter reads past the 64-pixel buffer's last column)
    for y in (3, 100, 200, 370):
        assert clear6(x, y) == (0,0,0), ("vulkan 6x Z16 clear seen as CT16", x, y, clear6(x, y))
print("vulkan 6x: Z16 clear covers whole native pixels in the CT16 view passed")

# A diagnostic rather than a passing fidelity claim: hardware UNORM blending
# still rounds where the GS integer equation truncates (no edge ambiguity).
for renderer in ["software", "vulkan"]:
    blend = render("blend", renderer)
    print("known blend limitation:", renderer, "interior pixel", blend(16,16), "; GS integer reference (14,28,44)")
print("procedural GS pixel regression passed; this is not proof of full GS fidelity")
