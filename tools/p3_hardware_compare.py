#!/usr/bin/env python3
"""Score manually synchronized PS2 and native Classic frames. See docs/SCENE_CAPTURE.md."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import tempfile

import numpy as np
from PIL import Image

from p3_compare import metrics


def required_text(record, field):
    value = record.get(field)
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{field} must be nonempty text")
    return value


def load_png(path):
    with path.open("rb") as file:
        header = file.read(26)
        if header[:16] != b"\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR" or header[24:26] != b"\x08\x02":
            raise ValueError(f"{path}: expected an 8-bit RGB PNG")
        file.seek(0)
        digest = hashlib.sha256()
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
        file.seek(0)
        with Image.open(file) as image:
            if image.format != "PNG" or image.mode != "RGB" or getattr(image, "n_frames", 1) != 1:
                raise ValueError(f"{path}: expected a single 8-bit RGB PNG")
            return np.asarray(image, dtype=np.int16), digest.hexdigest()


def score_pair(pair, root, stage):
    fields = ("id", "scene", "ps2", "native", "ps2_field", "sync")
    result = {field: required_text(pair, field) for field in fields}
    vblank = pair.get("native_vblank")
    if type(vblank) is not int or vblank < 0:
        raise ValueError("native_vblank must be a nonnegative integer")
    reference, reference_hash = load_png(root / result["ps2"])
    native, native_hash = load_png(root / result["native"])
    height, width = reference.shape[:2]
    crop = pair.get("ps2_crop")
    if crop is not None:
        if not isinstance(crop, list) or len(crop) != 4 or any(type(coord) is not int for coord in crop):
            raise ValueError("ps2_crop must contain four integer pixel coordinates")
        left, top, right, bottom = crop
        if not (0 <= left < right <= width and 0 <= top < bottom <= height):
            raise ValueError("ps2_crop is outside the reference frame")
        reference = reference[top:bottom, left:right]
    if reference.shape != native.shape:
        raise ValueError(f"{result['id']}: input frames must have the same dimensions after explicit PS2 crop")
    if min(native.shape[:2]) < 8:
        raise ValueError("comparison frames must be at least 8×8 pixels for SSIM")
    no_mask = np.zeros(native.shape[:2], dtype=bool)
    mae, p99, ssim, *_ = metrics(native, reference, no_mask, no_mask)
    difference = np.abs(native - reference)
    pair_id = result["id"]
    difference_file = f"{pair_id}-difference.png"
    sheet_file = f"{pair_id}-comparison.png"
    Image.fromarray(difference.astype(np.uint8)).save(stage / difference_file)
    sheet = np.concatenate((reference, native, np.minimum(difference * 4, 255)), axis=1)
    Image.fromarray(sheet.astype(np.uint8)).save(stage / sheet_file)
    return dict(result, native_vblank=vblank, ps2_crop=crop, ps2_source_size=[width, height],
                measured_size=[native.shape[1], native.shape[0]], ps2_sha256=reference_hash, native_sha256=native_hash,
                metrics=dict(mae_rgb=mae.tolist(), p99=p99, ssim=ssim),
                difference=difference_file, comparison=sheet_file)


def compare(manifest, output):
    capture = json.loads(manifest.read_text(encoding="utf-8"))
    if not isinstance(capture, dict):
        raise ValueError("expected a capture manifest object")
    report = {field: required_text(capture, field) for field in ("source", "capture_chain", "native_build")}
    pairs = capture.get("pairs")
    if not isinstance(pairs, list) or not pairs:
        raise ValueError("pairs must be a nonempty list")
    ids = set()
    for pair in pairs:
        if not isinstance(pair, dict):
            raise ValueError("each pair must be an object")
        pair_id = required_text(pair, "id")
        if not re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,63}", pair_id) or pair_id in ids:
            raise ValueError(f"invalid or duplicate pair id {pair_id!r}")
        ids.add(pair_id)
    if output.exists():
        raise ValueError(f"output directory already exists: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        stage = Path(temporary) / "report"
        stage.mkdir()
        report["pairs"] = [score_pair(pair, manifest.parent, stage) for pair in pairs]
        (stage / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        stage.rename(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--out", required=True, type=Path, help="new directory for the report and PNGs")
    args = parser.parse_args()
    try:
        compare(args.manifest, args.out)
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
