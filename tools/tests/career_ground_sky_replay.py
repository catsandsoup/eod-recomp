#!/usr/bin/env python3
"""Verify the Gate 1 ground/sky regression on a paired developer capture.

Capture the same EOD_SCENE_EMIT and EOD_GS_DUMP_AT window with EOD_MODERN=0,
EOD_NATIVE=0 and EOD_GS_DUMP_LIVE=1. Supply the resulting producer file and GS
dump, a Vulkan developer executable and the extracted retail disc. Requires
NumPy and Pillow. The script reads the paired live image beside the GS dump.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mod_install import eodhtx
from modding_backend import payload
from scene_view import load, pair_textures
from texture_identity import texture_identity
from texture_preview import decode_preview

GROUND = "dirt01.tex"
PROGRAM_MODE = (0x270, 0x280)


def check(condition, message):
    if not condition:
        raise ValueError(message)


def source(archive, name):
    data, _ = payload(archive, name)
    return data, texture_identity(data)


def overlay(folder, name, data, key, color, *, wrong=False):
    host = folder / "HOSTTEX"
    host.mkdir(parents=True)
    w, h, native, _ = decode_preview(data)
    rgba = np.frombuffer(native, dtype=np.uint8).reshape(h, w, 4).copy()
    rgba[:, :, :3] = color
    source_png = host / "swatch.png"
    Image.fromarray(rgba).save(source_png)
    (host / "swatch.eodhtx").write_bytes(eodhtx(source_png))
    palette = key["paletteKey"]
    if wrong:
        palette = f"{int(palette, 16) ^ 1:016x}"
    fields = (key["indexKey"], palette, str(w), str(h), "swatch.eodhtx", name, "career-proof")
    (host / "replacements.tsv").write_text("eod-host-textures 1\n" + "\t".join(fields) + "\n")


def producer_evidence(producer, terminal, identities):
    frames, _ = load(str(producer))
    complete = [frame for n, frame in frames.items() if n <= terminal and frame.batches]
    check(complete, "No geometry in the captured producer window")
    frame = max(complete, key=lambda f: f.n)
    pair_textures(frame)
    counts = {}
    for name, key in identities.items():
        matrices = {}
        for batch in frame.batches:
            tex = batch["tex"]
            if (batch["pc"], batch["itop"]) != PROGRAM_MODE or not tex:
                continue
            if (tex["index_key"], tex["palette_key"]) != (key["indexKey"], key["paletteKey"]):
                continue
            check(batch["sky_dome"] is not None, "Capture predates the recorded sky classification")
            check(batch["sky_dome"] == (name != GROUND), f"Wrong sky classification for {name}")
            matrix = np.ascontiguousarray(batch["mvp"].T, dtype="<f4").tobytes()
            digest = hashlib.sha256(matrix).hexdigest()
            matrices[digest] = matrices.get(digest, 0) + 1
        check(matrices, f"No 0x270/0x280 batch for {name} in producer frame {frame.n}")
        counts[name] = matrices
    dirt, sky = counts.values()
    check(dirt.keys().isdisjoint(sky), "Ground and sky used the same projection matrix")
    return {"frame": frame.n, "program": "0x270", "itop": "0x280", "batches_by_matrix_sha256": counts}


def run(args, name, overlay_root, config, *, disabled=False):
    path = args.out / name
    path.mkdir()
    capture = path / "scene.gsd"
    shutil.copyfile(args.dump, capture)
    env = {k: v for k, v in os.environ.items() if not k.startswith("EOD_")}
    env.update(EOD_MOD_DIR=str(overlay_root), EOD_NATIVE="0", EOD_MODERN="0", EOD_MUTE="1")
    if disabled:
        env["EOD_HOST_TEXTURES"] = "0"
    with (path / "run.log").open("w") as log:
        subprocess.run([args.exe, "--headless", "--config", config, "--gs-replay", capture,
                        "--repeat", "1"], cwd=path, env=env, stdout=log, stderr=log, timeout=90, check=True)
    capture.unlink()
    text = (path / "run.log").read_text()
    check(re.search(r"per replay: [1-9]\d* prims, \d+ pixels, [1-9]\d* game frames", text),
          f"{name} did not draw a new game frame")
    image_path = path / "scene.gsd.ppm"
    with Image.open(image_path) as image:
        image = image.convert("RGB")
        image.save(path / "scanout.png")
        rgb = np.array(image)
    image_path.unlink()
    return rgb


def mask_stats(mask):
    y, x = np.where(mask)
    return {"pixels": len(x), "bounds_xyxy_exclusive":
            [int(x.min()), int(y.min()), int(x.max()) + 1, int(y.max()) + 1]}


def verify(args):
    expected = Path(str(args.dump) + ".live.png")
    terminal = int(Path(str(args.dump) + ".live.vblank").read_text())
    archive = args.disc / "DATA/TRACKS/HOMEBASE.TRK"
    ground, g_key = source(archive, GROUND)
    sky, s_key = source(archive, args.sky)
    provenance = producer_evidence(args.producer, terminal, {GROUND: g_key, args.sky: s_key})
    args.out.mkdir(parents=True, exist_ok=False)
    empty = args.out / "empty"
    empty.mkdir()
    ground_root = args.out / "ground-overlay"
    sky_root = args.out / "sky-overlay"
    wrong_root = args.out / "wrong-overlay"
    overlay(ground_root, GROUND, ground, g_key, (252, 2, 250))
    overlay(sky_root, args.sky, sky, s_key, (1, 250, 3))
    overlay(wrong_root, GROUND, ground, g_key, (252, 2, 250), wrong=True)
    config = args.out / "replay.ini"
    config.write_text("[Video]\nRenderer=vulkan\nRenderScale=1\nAspectRatio=4:3\n")
    baseline = run(args, "control", empty, config)
    with Image.open(expected) as live:
        check(np.array_equal(baseline, np.array(live.convert("RGB"))), "Replay differs from paired live scanout")
    for name, root, disabled in (("repeat", empty, False), ("wrong-key", wrong_root, False),
                                 ("disabled", ground_root, True)):
        check(np.array_equal(baseline, run(args, name, root, config, disabled=disabled)), f"{name} drifted")
    dirt_pixels = np.any(baseline != run(args, "ground", ground_root, config), axis=2)
    sky_pixels = np.any(baseline != run(args, "sky", sky_root, config), axis=2)
    height, width = dirt_pixels.shape
    check(dirt_pixels[height // 2:].sum() > width * height // 10, "Insufficient visible ground response")
    check(sky_pixels.sum() > width * height // 1000, "Insufficient visible sky response")
    check(not sky_pixels[height // 2:].any(), "Sky texture affects pixels in the lower half")
    check(not (dirt_pixels & sky_pixels).any(), "Ground and sky response overlap")
    Image.fromarray(dirt_pixels).save(args.out / "ground-mask.png")
    Image.fromarray(sky_pixels).save(args.out / "sky-mask.png")
    result = {"display_vblank": terminal, "producer": provenance, "sources": {GROUND: g_key, args.sky: s_key},
              "ground": mask_stats(dirt_pixels), "sky": mask_stats(sky_pixels),
              "overlap_pixels": 0, "control_differences_pixels":
              {"paired_live": 0, "repeated": 0, "wrong_palette": 0, "replacement_disabled": 0},
              "dump_sha256": hashlib.sha256(args.dump.read_bytes()).hexdigest()}
    (args.out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("exe", "disc", "dump", "producer", "out"):
        parser.add_argument("--" + name, required=True, type=lambda p: Path(p).resolve())
    parser.add_argument("--sky", default="sky08A.tex", help="Visible HOMEBASE sky tile in the supplied frame")
    verify(parser.parse_args())
