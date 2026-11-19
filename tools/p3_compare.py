#!/usr/bin/env python3
"""P3 Classic parity: live Plume frames vs the offline GS replay of the same frames (numbers only).

usage: p3_compare.py --live DIR --dump BASE --vblanks V1,V2,... [--exe EOD] [--ini SW_INI]
                     [--region x0,y0,x1,y1] [--search -2..3] [--csv OUT]

The live run (EOD_NATIVE=1, RenderScale=1, Classic) writes DIR/frame_NNNNN.png
(--screenshot-every 1 with EOD_SCREENSHOT_START/END around each V) and records the GS command
stream with EOD_GS_DUMP=BASE EOD_GS_DUMP_AT=V1,V2,... EOD_GS_DUMP_N=1 (files BASE.V; BASE when
only one V). For each V this tool replays BASE.V in the software GS (eod --gs-replay with
Renderer=software) three times: plain (reference), with EOD_GS_REPLAY_FOGCOL changed (pixels that
move are fog pixels) and with EOD_GS_REPLAY_ZSHOT (pixels whose final Z equals the frame's most
common far value with no geometry are sky pixels). It then picks the live frame in V+search that
matches best (GS presentation trails the EE vblank counter) and prints:
  per-channel MAE (1/255 units), p99 of the per-pixel max channel error, SSIM (luma, 8x8 windows,
  stride 4), and the MAE over fog pixels and over sky pixels.
Gate (docs/MASTER_SPEC.md P3): MAE <= 1, p99 <= 8, SSIM >= 0.98, fog and sky <= 2 (all /255).
"""
import argparse
import os
import subprocess
import sys

import numpy as np
from PIL import Image


def load_rgb(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def load_z(path):
    with open(path, "rb") as f:
        head = f.readline().split()
        w, h = int(head[1]), int(head[2])
        return np.frombuffer(f.read(w * h * 4), dtype="<u4").reshape(h, w)


def replay(exe, ini, dump, env_extra):
    env = dict(os.environ, EOD_HEADLESS="1", **env_extra)
    r = subprocess.run([exe, "--headless", "--config", ini, "--gs-replay", dump, "--repeat", "1"],
                       env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, errors="replace")
    if r.returncode != 0:
        sys.exit(f"replay of {dump} failed ({r.returncode})")
    replay.tris = 0
    for line in r.stderr.splitlines():
        if line.strip().startswith("triangles "):
            replay.tris += int(line.rsplit(":", 1)[1].split()[0])
    return dump + ".ppm"


def ssim_luma(a, b):
    ya = (a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114).astype(np.float64)
    yb = (b[..., 0] * 0.299 + b[..., 1] * 0.587 + b[..., 2] * 0.114).astype(np.float64)
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    vals = []
    h, w = ya.shape
    for y in range(0, h - 7, 4):
        ra, rb = ya[y:y + 8], yb[y:y + 8]
        # 8x8 windows along the row, vectorised
        n = (w - 8) // 4 + 1
        wa = np.stack([ra[:, x:x + 8] for x in range(0, 4 * n, 4)])
        wb = np.stack([rb[:, x:x + 8] for x in range(0, 4 * n, 4)])
        ma, mb = wa.mean(axis=(1, 2)), wb.mean(axis=(1, 2))
        va, vb = wa.var(axis=(1, 2)), wb.var(axis=(1, 2))
        cov = ((wa - ma[:, None, None]) * (wb - mb[:, None, None])).mean(axis=(1, 2))
        vals.append(((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2)))
    return float(np.concatenate(vals).mean()) if vals else 1.0


def metrics(live, ref, fog_mask, sky_mask):
    d = np.abs(live - ref)
    mae = d.reshape(-1, 3).mean(axis=0)
    p99 = float(np.percentile(d.max(axis=2), 99))
    fog = float(d[fog_mask].mean()) if fog_mask.any() else float("nan")
    sky = float(d[sky_mask].mean()) if sky_mask.any() else float("nan")
    return mae, p99, ssim_luma(live, ref), fog, int(fog_mask.sum()), sky, int(sky_mask.sum())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--live", required=True)
    ap.add_argument("--dump", required=True)
    ap.add_argument("--vblanks", required=True)
    ap.add_argument("--exe", default=os.path.join(os.path.dirname(__file__), "..", "build", "eod.exe"))
    ap.add_argument("--ini", required=True, help="eod.ini with Renderer=software for the replay")
    ap.add_argument("--region", help="x0,y0,x1,y1: report only this rectangle (display pixels)")
    ap.add_argument("--search", default="-2..3")
    ap.add_argument("--csv")
    ap.add_argument("--min-tri-px", type=int, default=100000,
                    help="triangle pixels the replay must draw (replay stats) for the frame to count")
    a = ap.parse_args()
    vbs = [int(v) for v in a.vblanks.split(",")]
    lo, hi = (int(x) for x in a.search.split(".."))
    rows = []
    for v in vbs:
        dump = a.dump if len(vbs) == 1 else f"{a.dump}.{v}"
        ref_p = replay(a.exe, a.ini, dump, {})
        ref = load_rgb(ref_p)
        # The dump starts from VRAM as the live renderer left it. Unless the window holds a whole
        # GS frame (its triangles, then the copy to the display buffer), the replay shows the live
        # native pixels again and the comparison is void: such windows are skipped.
        if replay.tris < a.min_tri_px:
            print(f"vb {v}: skipped, the replay window drew only {replay.tris} triangle pixels (no complete GS frame)")
            continue
        # FOGCOL complemented: fog pixels (F < 255 with FGE set) move, all others stay
        fog_p = dump + ".fog"
        os.replace(replay(a.exe, a.ini, dump, {"EOD_GS_REPLAY_FOGCOL": "00FF00FF"}), fog_p + ".ppm")
        fog2 = load_rgb(fog_p + ".ppm")
        os.replace(replay(a.exe, a.ini, dump, {"EOD_GS_REPLAY_FOGCOL": "0000FF00"}), fog_p + "2.ppm")
        fog3 = load_rgb(fog_p + "2.ppm")
        fog_mask = (np.abs(fog2 - ref).max(axis=2) > 0) | (np.abs(fog3 - ref).max(axis=2) > 0)
        z_p = dump + ".z"
        replay(a.exe, a.ini, dump, {"EOD_GS_REPLAY_ZSHOT": z_p})
        z = load_z(z_p)
        sky_mask = np.zeros(ref.shape[:2], bool)
        if z.shape == ref.shape[:2]:
            sky_mask = z == 0  # cleared depth (GS: larger Z is nearer; the frame clears Z to 0)
        best = None
        for off in range(lo, hi + 1):
            lp = os.path.join(a.live, f"frame_{v + off:05d}.png")
            if not os.path.exists(lp):
                continue
            live = load_rgb(lp)
            if live.shape != ref.shape:
                sys.exit(f"{lp}: {live.shape[1]}x{live.shape[0]} vs replay {ref.shape[1]}x{ref.shape[0]}")
            mae = float(np.abs(live - ref).mean())
            if best is None or mae < best[0]:
                best = (mae, off, live)
        if best is None:
            print(f"vb {v}: no live frame in {lo}..{hi}")
            continue
        live = best[2]
        r, f, s = ref, fog_mask, sky_mask
        if a.region:
            x0, y0, x1, y1 = (int(t) for t in a.region.split(","))
            live, r, f, s = live[y0:y1, x0:x1], r[y0:y1, x0:x1], f[y0:y1, x0:x1], s[y0:y1, x0:x1]
        mae, p99, ssim, fog, nfog, sky, nsky = metrics(live, r, f, s)
        rows.append((v, best[1], mae, p99, ssim, fog, nfog, sky, nsky))
        print(f"vb {v} (live offset {best[1]:+d}): MAE r/g/b {mae[0]:.2f}/{mae[1]:.2f}/{mae[2]:.2f} p99 {p99:.0f} "
              f"SSIM {ssim:.4f} fog {fog:.2f} ({nfog} px) sky {sky:.2f} ({nsky} px)")
    if rows:
        m = np.array([[x[2].mean(), x[3], x[4]] for x in rows])
        fog = [x[5] for x in rows if x[6]]
        sky = [x[7] for x in rows if x[8]]
        print(f"P3 over {len(rows)} frames: MAE {m[:, 0].mean():.2f} (worst {m[:, 0].max():.2f}) p99 {np.median(m[:, 1]):.0f} "
              f"(worst {m[:, 1].max():.0f}) SSIM {m[:, 2].mean():.4f} (worst {m[:, 2].min():.4f}) "
              f"fog {np.mean(fog) if fog else float('nan'):.2f} ({sum(x[6] for x in rows)} px) "
              f"sky {np.mean(sky) if sky else float('nan'):.2f} ({sum(x[8] for x in rows)} px)")
        ok = m[:, 0].max() <= 1 and m[:, 1].max() <= 8 and m[:, 2].min() >= 0.98 and \
            all(x <= 2 for x in fog) and all(x <= 2 for x in sky)
        print("P3 gate:", "PASS" if ok else "FAIL")
    if a.csv and rows:
        with open(a.csv, "w") as f:
            f.write("vblank,offset,mae_r,mae_g,mae_b,p99,ssim,fog_mae,fog_px,sky_mae,sky_px\n")
            for x in rows:
                f.write(f"{x[0]},{x[1]},{x[2][0]:.3f},{x[2][1]:.3f},{x[2][2]:.3f},{x[3]:.0f},{x[4]:.4f},"
                        f"{x[5]:.3f},{x[6]},{x[7]:.3f},{x[8]}\n")


if __name__ == "__main__":
    main()
