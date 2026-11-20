#!/usr/bin/env python3
"""R6 Modern look guardrails (docs/MODERN_LOOK_GUARDRAILS.md), numbers only.

usage: modern_guardrails.py DIR [DIR ...]

Each DIR holds side-by-side captures named *_classic_left_modern_right.png (left half Classic,
right half Modern of the same or the next frame). Per scene prefix (text before the first '_')
it prints (pairs whose halves are identical are skipped):
  row 1    *_native_only.png (EOD_NATIVE_ONLY=1 headless captures, if present): non-black coverage
           (>= 50% nonzero pixels, mean luminance >= 5) and zero black-hole regressions
  row 4/5  mean luminance Modern/Classic for the whole frame and the worst of a 3x3 grid of regions
           (pass: every ratio within 0.85..1.15; regions darker than 8/255 in both are skipped)
  row 6    pixels at luminance 1.0 in Modern that are below 1.0 in Classic (% of the frame;
           pass: <= 0.05%)
  row 7    16x16 blocks that are flat in Modern (std < 2) but textured in Classic (std > 8) and
           brighter by more than 40 (an opaque card): pass when 0.1% of blocks or fewer
  row 8    [r6] GPU timing lines in *.log (EOD_GPU_TIMING=1, if present): Modern - Classic GPU ms
           <= 2.0 ms and Modern API draws/frame <= Classic + 5% (Classic splits items per TEX1.K run)
"""
import sys, glob, os, re
from collections import defaultdict
import numpy as np
from PIL import Image


def lum(a):
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]


def check(path):
    a = np.asarray(Image.open(path).convert('RGB'), dtype=np.float32)
    w = a.shape[1] // 2
    c, m = lum(a[:, :w]), lum(a[:, w:2 * w])
    if np.abs(a[:, :w] - a[:, w:2 * w]).mean() < 0.05:
        return None  # both halves are the same image (the capture missed the toggle): not a pair
    h = c.shape[0]
    whole = m.mean() / max(c.mean(), 1e-3)
    worst = 1.0
    for i in range(3):
        for j in range(3):
            rc = c[i * h // 3:(i + 1) * h // 3, j * w // 3:(j + 1) * w // 3].mean()
            rm = m[i * h // 3:(i + 1) * h // 3, j * w // 3:(j + 1) * w // 3].mean()
            if rc < 8 and rm < 8:
                continue
            r = rm / max(rc, 1e-3)
            if abs(r - 1) > abs(worst - 1):
                worst = r
    sat = float(((m >= 254.5) & (c < 254.5)).mean() * 100)
    bh, bw = h // 16, w // 16
    cb = c[:bh * 16, :bw * 16].reshape(bh, 16, bw, 16)
    mb = m[:bh * 16, :bw * 16].reshape(bh, 16, bw, 16)
    flat = (mb.std(axis=(1, 3)) < 2) & (cb.std(axis=(1, 3)) > 8) & (mb.mean(axis=(1, 3)) - cb.mean(axis=(1, 3)) > 40)
    return whole, worst, sat, float(flat.mean() * 100)


RE_GPU = re.compile(
    r"\[r6\] GPU timing: Classic mean ([0-9.]+) ms \(max ([0-9.]+), shadow ([0-9.]+), (\d+) frames, ([0-9.]+) draws/f\) \| "
    r"Modern mean ([0-9.]+) ms \(max ([0-9.]+), shadow ([0-9.]+), (\d+) frames, ([0-9.]+) draws/f\) \| delta ([+-]?[0-9.]+) ms"
)


def check_native_only(path):
    a = np.asarray(Image.open(path).convert('RGB'), dtype=np.float32)
    l = lum(a)
    nz = float((a > 0).any(axis=-1).mean() * 100.0)
    return float(l.mean()), float(l.std()), nz


def main():
    scenes = defaultdict(list)
    native_only_frames = defaultdict(list)
    gpu_rows = []
    for d in sys.argv[1:]:
        for p in sorted(glob.glob(os.path.join(d, '*_classic_left_modern_right.png'))):
            r = check(p)
            if r is not None:
                scenes[os.path.basename(p).split('_')[0]].append(r)
        for p in sorted(glob.glob(os.path.join(d, '*_native_only.png'))):
            native_only_frames[os.path.basename(p).split('_')[0]].append(check_native_only(p))
        for lp in sorted(glob.glob(os.path.join(d, '*.log'))):
            last = None
            for line in open(lp, encoding='utf-8', errors='replace'):
                m = RE_GPU.search(line)
                if m:
                    last = m
            if last:
                gpu_rows.append((os.path.basename(lp), last))
    ok_all = True
    for s, rows in sorted(scenes.items()):
        r = np.array(rows)
        wl = r[np.argmax(np.abs(r[:, 0] - 1)), 0]
        wr = r[np.argmax(np.abs(r[:, 1] - 1)), 1]
        ok = (abs(wl - 1) <= .15, abs(wr - 1) <= .15, r[:, 2].max() <= .05, r[:, 3].max() <= .1)
        ok_all &= all(ok)
        print(f'{s}: {len(rows)} pairs | lum M/C whole worst {wl:.3f} mean {r[:, 0].mean():.3f} | region worst {wr:.3f} '
              f'| new 1.0 px max {r[:, 2].max():.4f}% | opaque blocks max {r[:, 3].max():.3f}% | '
              + ' '.join('PASS' if o else 'FAIL' for o in ok))
    for s, rows in sorted(native_only_frames.items()):
        r = np.array(rows)
        min_mean, min_nz = float(r[:, 0].min()), float(r[:, 2].min())
        ok1 = (min_mean >= 5.0) and (min_nz >= 50.0)
        ok_all &= ok1
        print(f'{s} [row 1 native-only]: {len(rows)} frames | lum mean min {min_mean:.2f} avg {r[:, 0].mean():.2f} '
              f'| nonzero min {min_nz:.2f}% avg {r[:, 2].mean():.2f}% | ' + ('PASS' if ok1 else 'FAIL'))
    for name, m in gpu_rows:
        cm, cmax, csh, cf, cd = float(m.group(1)), float(m.group(2)), float(m.group(3)), int(m.group(4)), float(m.group(5))
        mm, mmax, msh, mf, md = float(m.group(6)), float(m.group(7)), float(m.group(8)), int(m.group(9)), float(m.group(10))
        delta = float(m.group(11))
        # Classic issues one API draw per VU1 TEX1.K run of an item; Modern one per item, so Modern
        # may draw fewer but never more (same item list)
        draw_ratio = md / max(cd, 1.0) if (cf > 0 and mf > 0) else 1.0
        ok8 = (cf > 0 and mf > 0 and delta <= 2.0 and draw_ratio <= 1.05)
        ok_all &= ok8
        print(f'{name} [row 8 GPU ms]: Classic {cm:.3f} ms ({cf}f, {cd:.0f} draws/f) | Modern {mm:.3f} ms ({mf}f, {md:.0f} draws/f) '
              f'| delta {delta:+.3f} ms (<= 2.0 ms) | ' + ('PASS' if ok8 else 'FAIL'))
    print('ALL PASS' if ok_all else 'SOME FAIL')
    return 0 if ok_all else 1


if __name__ == '__main__':
    sys.exit(main())
