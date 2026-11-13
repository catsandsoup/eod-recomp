#!/usr/bin/env python3
"""Read an EOD_SCENE_CAPTURE recording (runtime/vu/scene_capture.cpp).

usage:
  scene_capture.py stats CAPTURE
  scene_capture.py runs CAPTURE [--frame N] [--pc 0270]     per VU1 run: inputs and kicks
  scene_capture.py reconstruct CAPTURE --out DIR [--disc GAME] [--screenshot PNG]
      world-space OBJ + scene.json (camera, per-batch model matrix, role, texture keys) and a
      render through the captured camera; with --screenshot, compare.png next to the game's image
See docs/SCENE_CAPTURE.md.
"""
import argparse
import struct
import sys
from collections import Counter, OrderedDict

GIF_REG = {0: 'PRIM', 1: 'RGBAQ', 2: 'ST', 3: 'UV', 4: 'XYZF2', 5: 'XYZ2', 6: 'TEX0_1', 7: 'TEX0_2', 8: 'CLAMP_1',
           9: 'CLAMP_2', 10: 'FOG', 12: 'XYZF3', 13: 'XYZ3', 14: 'AD', 15: 'NOP'}
AD_REG = {0x00: 'PRIM', 0x06: 'TEX0_1', 0x07: 'TEX0_2', 0x14: 'TEX1_1', 0x15: 'TEX1_2', 0x16: 'TEX2_1',
          0x3B: 'TEXA', 0x3F: 'TEXFLUSH', 0x42: 'ALPHA_1', 0x47: 'TEST_1', 0x4C: 'FRAME_1', 0x4E: 'ZBUF_1',
          0x08: 'CLAMP_1', 0x18: 'XYOFFSET_1', 0x40: 'SCISSOR_1', 0x1A: 'PRMODECONT', 0x50: 'BITBLTBUF',
          0x51: 'TRXPOS', 0x52: 'TRXREG', 0x53: 'TRXDIR', 0x3D: 'FOGCOL', 0x46: 'COLCLAMP', 0x49: 'PABE',
          0x4A: 'FBA_1', 0x45: 'DTHE', 0x1C: 'TEX1_1?', 0x34: 'MIPTBP1_1', 0x36: 'MIPTBP2_1', 0x61: 'FINISH'}


class Run:
    """One VU1 micro program run: entry pc, TOP/ITOP, registers and data memory at entry, the GS
    packets it kicked (PATH1) in order."""

    def __init__(self, idx, frame, pc, top, itop, vf, vi, mem):
        self.idx, self.frame, self.pc, self.top, self.itop = idx, frame, pc, top, itop
        self.vf, self.vi, self.mem = vf, vi, mem
        self.kicks = []

    def q(self, a):
        """VU1 data memory qword a as 4 floats."""
        return struct.unpack_from('<4f', self.mem, (a & 0x3FF) * 16)

    def qi(self, a):
        return struct.unpack_from('<4I', self.mem, (a & 0x3FF) * 16)


def load(path):
    """Returns the stream as a list of ('vblank', n) / ('run', Run) / ('gif', path, bytes, run_or_None)."""
    d = open(path, 'rb').read()
    if d[:8] != b'EODSCN01':
        raise SystemExit('%s: not a scene capture' % path)
    p, frame, out, last, nruns = 8, None, [], None, 0
    while p + 8 <= len(d):
        tag, n = struct.unpack_from('<4sI', d, p)
        p += 8
        q, p = p, p + n
        if tag == b'VBLK':
            frame = struct.unpack_from('<Q', d, q)[0]
            out.append(('vblank', frame))
        elif tag == b'VU1R':
            pc, top, itop, _ = struct.unpack_from('<4I', d, q)
            vf = [struct.unpack_from('<4f', d, q + 16 + 16 * r) for r in range(32)]
            vi = struct.unpack_from('<16I', d, q + 16 + 512)
            last = Run(nruns, frame, pc, top, itop, vf, vi,
                       d[q + 16 + 512 + 64:q + n])
            out.append(('run', last))
            nruns += 1
        elif tag == b'GIFP':
            path_, qwc = struct.unpack_from('<II', d, q)
            data = d[q + 8:q + 8 + qwc * 16]
            if path_ == 1 and last is not None:
                last.kicks.append(data)
            out.append(('gif', path_, data, last if path_ == 1 else None))
    return out


def gif_packets(data):
    """Yields (tag dict, list of (reg name, lo, hi)) for each GIF tag in a PATH data block."""
    p = 0
    while p + 16 <= len(data):
        lo, hi = struct.unpack_from('<QQ', data, p)
        p += 16
        nloop, eop, pre, prim = lo & 0x7FFF, (lo >> 15) & 1, (lo >> 46) & 1, (lo >> 47) & 0x7FF
        flg, nreg = (lo >> 58) & 3, (lo >> 60) & 15 or 16
        regs = [(hi >> (4 * i)) & 15 for i in range(nreg)]
        tag = dict(nloop=nloop, eop=eop, pre=pre, prim=prim, flg=flg, nreg=nreg, regs=regs)
        items = []
        if flg == 0:  # PACKED
            for _ in range(nloop):
                for r in regs:
                    a, b = struct.unpack_from('<QQ', data, p)
                    p += 16
                    if r == 14:
                        items.append((AD_REG.get(b & 0xFF, 'AD%02x' % (b & 0xFF)), a, b))
                    else:
                        items.append((GIF_REG.get(r, 'R%x' % r), a, b))
        elif flg == 1:  # REGLIST
            words = []
            for _ in range((nloop * nreg + 1) // 2):
                words += struct.unpack_from('<QQ', data, p)
                p += 16
            for i in range(nloop * nreg):
                items.append((GIF_REG.get(regs[i % nreg], 'R'), words[i], 0))
        else:  # IMAGE
            items.append(('IMAGE', nloop, 0))
            p += nloop * 16
        yield tag, items
        if eop:
            break


def cmd_stats(a):
    stats, cur = OrderedDict(), None
    for e in load(a.capture):
        if e[0] == 'vblank':
            cur = stats.setdefault(e[1], Counter())
        elif e[0] == 'run':
            cur['vu1 %04x' % e[1].pc] += 1
        else:
            cur['path%d packets' % e[1]] += 1
    for k, v in stats.items():
        print(k, dict(sorted(v.items())))


def cmd_runs(a):
    for e in load(a.capture):
        if e[0] != 'run':
            continue
        r = e[1]
        if a.frame is not None and r.frame != a.frame:
            continue
        if a.pc and r.pc != int(a.pc, 16):
            continue
        print('run %d pc %04x top %x itop %x kicks %d count@top %08x' % (r.idx, r.pc, r.top, r.itop, len(r.kicks),
                                                                          r.qi(r.top)[0]))
        for k in r.kicks[:a.kicks]:
            for tag, items in gif_packets(k):
                print('   tag nloop %d prim %x(pre %d) flg %d regs %s' % (tag['nloop'], tag['prim'], tag['pre'],
                                                                         tag['flg'], tag['regs']))
                for it in items[:a.items]:
                    print('      %-8s %016x %016x' % it)
        if a.limit and r.idx >= a.limit:
            break


# ----------------------------------------------------------------- reconstruction
# Batch input layout (docs/SCENE_CAPTURE.md): count at TOP.x & 0x7FFF, vertex k at TOP+1+3k
# (normal or colour, ADC in w), TOP+2+3k (position, plus TOP+199 offset), TOP+3+3k (s, t, plus
# TOP+198 offset). The micro program projects with [vf4 vf5 vf6 vf7] (model -> GS screen).

def adc_flag(w):
    return bool(w & 0x8000) or w == 0x80


def decode_batch(r):
    t = r.top
    n = r.qi(t)[0] & 0x7FFF
    if not 0 < n <= 120:
        return None
    off, uvo = r.q(t + 199), r.q(t + 198)
    pos, uv, nrm, col, adc = [], [], [], [], []
    for k in range(n):
        p = r.q(t + 2 + 3 * k)
        s = r.q(t + 3 + 3 * k)
        ni = r.qi(t + 1 + 3 * k)
        pos.append((p[0] + off[0], p[1] + off[1], p[2] + off[2]))
        uv.append((s[0] + uvo[0], s[1] + uvo[1]))
        if (ni[0] & 0xFFFFFF00) == 0x47800000:  # offset-mode V4-8 normal (STROW 65536)
            nrm.append(tuple(((v & 0xFF) - 128) / 127.0 for v in ni[:3]))
            col.append(None)
        else:  # V4-8 vertex colour (0x80 = 1.0)
            nrm.append(None)
            col.append(tuple(v & 0xFF for v in ni[:4]))
        adc.append(adc_flag(ni[3] & 0xFFFF))
    if not all(abs(v) < 1e6 for p in pos for v in p):  # not this layout (particle programs)
        return None
    return dict(n=n, pos=pos, uv=uv, nrm=nrm, col=col, adc=adc)


def kick_vertices(r):
    """Screen vertices of a run's kicks: (x, y, z, adc, prim)."""
    out, prim = [], 0
    for k in r.kicks:
        for tag, items in gif_packets(k):
            if tag['pre']:
                prim = tag['prim']
            for name, lo, hi in items:
                if name == 'PRIM':
                    prim = lo
                elif name in ('XYZF2', 'XYZ2', 'XYZF3', 'XYZ3'):
                    adc = name.endswith('3') or (tag['flg'] == 0 and (hi >> 47) & 1)
                    z = (hi >> 4) & 0xFFFFFF if name.startswith('XYZF') else hi & 0xFFFFFFFF
                    if tag['flg'] != 0:
                        z = lo >> 32 if not name.startswith('XYZF') else (lo >> 36) & 0xFFFFFF
                    out.append(((lo & 0xFFFF) / 16.0, ((lo >> 32) & 0xFFFF) / 16.0, z, bool(adc), prim))
    return out


def strip_tris(adc, prim_type=4):
    tris = []
    for i in range(2, len(adc)):
        if adc[i]:
            continue
        if prim_type == 5:
            tris.append((0, i - 1, i))
        else:
            tris.append((i - 2, i - 1, i) if i % 2 == 0 else (i - 1, i - 2, i))
    return tris


def read_tex_log(path):
    """file.tex -> list of (kick, path, tbp, psm, tw, th, index_key, palette_key) in order."""
    rows = []
    try:
        for line in open(path):
            f = line.split()
            if f and f[0] == 'draw':
                rows.append((int(f[3]), int(f[1]), int(f[4], 16), int(f[6], 16), int(f[7]), int(f[8]), f[11], f[12]))
    except OSError:
        pass
    return rows


def xyw(m):
    """The screen x, y, w rows of a model->screen matrix, completed to an invertible affine map."""
    import numpy as np
    return np.array([m[0], m[1], m[3], [0.0, 0.0, 0.0, 1.0]])


def scene(stream, tex_rows, vblank=None):
    """Rebuilds the 3D batches of one frame. Returns (camera VP 4x4, batches, info)."""
    import numpy as np
    frames = OrderedDict()
    cur = None
    for e in stream:
        if e[0] == 'vblank':
            cur = frames.setdefault(e[1], [])
        elif cur is not None:
            cur.append(e)
    if vblank is None:  # the interval with the most VU1 runs
        vblank = max(frames, key=lambda f: sum(1 for e in frames[f] if e[0] == 'run'))
    # kick numbering must count from the start of the recording (as the GS side does)
    kick_no, kicks_before = 0, {}
    for e in stream:
        if e[0] == 'gif' and e[1] == 1:
            kick_no += 1
            kicks_before.setdefault(id(e[3]), kick_no)
    # camera: the most common model->screen matrix among VU1 runs (every world batch shares it).
    # Batches of one camera differ in the depth row (per-layer depth ranges), so the screen x, y
    # and w rows identify it; model matrices are solved from those rows.
    mats, full = Counter(), {}
    for e in frames[vblank]:
        if e[0] == 'run' and e[1].kicks:
            k = tuple(round(e[1].vf[i][j], 2) for i in (4, 5, 6, 7) for j in (0, 1, 3))
            mats[k] += 1
            full.setdefault(k, e[1])
    cam = full[mats.most_common(1)[0][0]]
    vp = np.array([cam.vf[i] for i in (4, 5, 6, 7)], dtype=float).T
    vp_inv = np.linalg.inv(xyw(vp))
    batches, fbp, tex0, xyoff, disp = [], None, None, (0, 0), None
    row_i, cur_rows = 0, {}

    for e in frames[vblank]:
        if e[0] == 'gif':
            for tag, items in gif_packets(e[2]):
                for name, lo, hi in items:
                    if name == 'FRAME_1':
                        fbp = lo & 0x1FF
                    elif name == 'TEX0_1':
                        tex0 = lo
                    elif name == 'XYOFFSET_1':
                        xyoff = ((lo & 0xFFFF) / 16.0, ((lo >> 32) & 0xFFFF) / 16.0)
            continue
        if e[0] != 'run' or not e[1].kicks:
            continue
        r = e[1]
        b = decode_batch(r)
        if b is None:
            continue
        ov = kick_vertices(r)
        if not ov:  # state packets only (e.g. the TEX0 kicks of 0x2538)
            continue
        mvp = np.array([r.vf[i] for i in (4, 5, 6, 7)], dtype=float).T
        m = vp_inv @ xyw(mvp)
        rot = m[:3, :3]
        gram = rot.T @ rot
        rigid = np.abs(gram - np.eye(3) * gram.trace() / 3).max() < 2e-2 * max(gram.trace() / 3, 1e-9)
        prim = (ov[0][4] if ov else 4) & 7
        # authored strip connectivity (input ADC); the kicked ADC also carries the program's own
        # culling (off-screen, back-facing), which a renderer with its own camera must not inherit
        adc = b['adc']
        if len(ov) == b['n']:
            # projection check: input -> screen through [vf4..vf7]
            err = 0.0
            for p, o in zip(b['pos'], ov):
                c = mvp @ np.array([p[0], p[1], p[2], 1.0])
                if c[3]:
                    err = max(err, abs(c[0] / c[3] - o[0]), abs(c[1] / c[3] - o[1]))
        else:
            err = None
        first_kick = kicks_before.get(id(r))
        # the texture this run's geometry used: the GS logs a row when a packet sets up a textured
        # draw state, so it is the last row at or before the run's last kick
        # (with the TEX0 base pointer this run drew with: the game alternates two VRAM slots)
        last_kick = (first_kick or 0) + len(r.kicks) - 1
        while row_i < len(tex_rows) and tex_rows[row_i][0] <= last_kick:
            cur_rows[tex_rows[row_i][2]] = tex_rows[row_i]
            row_i += 1
        keys = cur_rows.get(tex0 & 0x3FFF) if tex0 is not None else None
        if disp is None and fbp is not None and r.itop & 0x200 == 0:
            disp = fbp
        expanded = len(ov) != b['n'] and (prim == 6 or len(ov) in (2 * b['n'], 4 * b['n']))
        batches.append(dict(run=r.idx, clipped=len(ov) != b['n'], expanded=expanded, pc=r.pc, itop=r.itop, fbp=fbp, n=b['n'], pos=b['pos'], uv=b['uv'],
                            nrm=b['nrm'], col=b['col'], tris=strip_tris(adc, prim), model=m.tolist(), rigid=rigid,
                            world=bool(np.abs(m - np.eye(4)).max() < 1e-3), proj_err=err, tex0=tex0,
                            tex=keys and dict(tbp=keys[2], psm=keys[3], w=keys[4], h=keys[5], index_key=keys[6],
                                              palette_key=keys[7])))
    info = dict(vblank=vblank, xyoffset=xyoff, display_fbp=disp)
    return vp, batches, info


def role(b, info):
    """offscreen (another frame buffer: shadow maps), hud (0x270 in ITOP mode 0x200 with its own 2D
    matrix), sky (ITOP mode 0x280, translated to the camera, unrotated), world (camera matrix itself), object
    (camera x model matrix: cars, props, wheels)."""
    import numpy as np
    if b['fbp'] != info['display_fbp']:
        return 'offscreen'
    if b['expanded']:
        return 'particles'
    if b['world']:
        return 'world'
    if b['pc'] == 0x270 and b['itop'] == 0x200:
        return 'hud'
    m = np.array(b['model'])
    if b['itop'] == 0x280 and np.abs(m[:3, :3] - np.eye(3)).max() < 1e-3:
        return 'sky'
    return 'object' if b['rigid'] else 'object-nonuniform'


def load_textures(disc, wanted):
    """(index_key, palette_key) -> (w, h, RGBA bytes) for keys found in the disc's TEX members."""
    import os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from pathlib import Path
    import modding_backend as mb
    from texture_identity import FNV_OFFSET, fnv1a64, texture_identity
    from texture_preview import decode_preview
    want_pk = {pk for _, pk in wanted}
    found = {}
    archives = sorted(Path(disc, 'DATA').rglob('*'))
    for a in archives:
        if a.suffix.upper() not in ('.TRK', '.CAR', '.RES'):
            continue
        try:
            entries = mb.directory(a)
        except Exception:
            continue
        raw = None
        for ent in entries:
            if ent['type'] != 'TEX':
                continue
            if raw is None:
                raw = a.read_bytes()
            data = raw[ent['offset']:ent['offset'] + ent['size']]
            try:
                pal = struct.unpack_from('<I', data, 24)[0]
            except struct.error:
                continue
            hit = False
            for n in (16, 256):
                rgb = b''.join(data[pal + i * 4 + 1:pal + i * 4 + 4] for i in range(n))
                if len(rgb) == 3 * n and '%016x' % fnv1a64(rgb) in want_pk:
                    hit = True
            if not hit:
                continue
            try:
                ident = texture_identity(data)
            except Exception:
                continue
            key = (ident['indexKey'], ident['paletteKey'])
            if key in wanted and key not in found:
                w, h, rgba, _ = decode_preview(data)
                found[key] = (w, h, bytes(rgba), '%s:%s' % (a.name, ent['name']))
    return found


def clip_near(c, attrs, near=0.5):
    """Clips a triangle (homogeneous screen rows c, per-vertex attribute rows) to w >= near.
    Returns a list of (c, attrs) triangles."""
    import numpy as np
    poly = list(zip(c, attrs))
    out = []
    for i in range(len(poly)):
        (a, aa), (b, ba) = poly[i], poly[(i + 1) % len(poly)]
        ina, inb = a[3] >= near, b[3] >= near
        if ina:
            out.append((a, aa))
        if ina != inb:
            t = (near - a[3]) / (b[3] - a[3])
            out.append((a + t * (b - a), aa + t * (ba - aa)))
    if len(out) < 3:
        return []
    return [(np.array([out[0][0], out[i][0], out[i + 1][0]]), np.array([out[0][1], out[i][1], out[i + 1][1]]))
            for i in range(1, len(out) - 1)]


def render(vp, batches, info, size=(640, 448), textures=None, roles=('world', 'object', 'object-nonuniform', 'sky')):
    """z-buffered rasterizer through the captured camera; textured where the texture was found,
    otherwise Lambert-shaded grey. Returns an RGB numpy image."""
    import numpy as np
    W, H = size
    img = np.full((H, W, 3), 0, np.uint8)
    zb = np.full((H, W), np.inf)
    ox, oy = info['xyoffset']
    light = np.array([0.3, 0.8, 0.5]) / np.linalg.norm([0.3, 0.8, 0.5])
    textures = textures or {}
    for b in batches:
        if role(b, info) not in roles:
            continue
        m = np.array(b['model'])
        P = np.array([list(p) + [1.0] for p in b['pos']]) @ m.T     # world space
        C = P @ vp.T                                                 # GS screen (homogeneous)
        tex = None
        if b['tex']:
            tex = textures.get((b['tex']['index_key'], b['tex']['palette_key']))
        if tex:
            tw, th, rgba = tex[0], tex[1], np.frombuffer(tex[2], np.uint8).reshape(tex[1], tex[0], 4)
        rl = role(b, info)
        for tri in b['tris']:
            uvw = np.array([list(b['uv'][i]) + list(P[i, :3]) for i in tri])
            for c, at in clip_near(C[list(tri)], uvw):
                raster(img, zb, c, at, rl, tex and (tw, th, rgba), light, ox, oy, W, H)
    return img


def raster(img, zb, c, at, rl, tex, light, ox, oy, W, H):
    import numpy as np
    if not np.isfinite(c).all() or not np.isfinite(at).all():
        return
    sx, sy = c[:, 0] / c[:, 3] - ox, c[:, 1] / c[:, 3] - oy
    x0, x1 = max(int(np.floor(sx.min())), 0), min(int(np.ceil(sx.max())), W - 1)
    y0, y1 = max(int(np.floor(sy.min())), 0), min(int(np.ceil(sy.max())), H - 1)
    if x0 > x1 or y0 > y1:
        return
    den = (sy[1] - sy[2]) * (sx[0] - sx[2]) + (sx[2] - sx[1]) * (sy[0] - sy[2])
    if abs(den) < 1e-9:
        return
    ys, xs = np.mgrid[y0:y1 + 1, x0:x1 + 1]
    px, py = xs + 0.5, ys + 0.5
    l0 = ((sy[1] - sy[2]) * (px - sx[2]) + (sx[2] - sx[1]) * (py - sy[2])) / den
    l1 = ((sy[2] - sy[0]) * (px - sx[2]) + (sx[0] - sx[2]) * (py - sy[2])) / den
    l2 = 1 - l0 - l1
    inside = (l0 >= 0) & (l1 >= 0) & (l2 >= 0)
    if not inside.any():
        return
    iw = 1 / c[:, 3]
    wsum = l0 * iw[0] + l1 * iw[1] + l2 * iw[2]
    # camera distance (w); GS z is not comparable across batches (per-layer depth rows).
    # The sky sorts behind everything.
    depth = 1 / wsum if rl != 'sky' else np.full(l0.shape, 1e30)
    # later coplanar layers (road/detail decals over the ground) win ties, as in draw order
    layered = rl == 'world'
    ok = inside & ((depth <= zb[y0:y1 + 1, x0:x1 + 1] * (1 + 1e-4)) if layered
                   else (depth < zb[y0:y1 + 1, x0:x1 + 1]))
    alpha = None
    if tex:
        tw, th, rgba = tex
        uv = at[:, :2]
        u = (l0 * uv[0, 0] * iw[0] + l1 * uv[1, 0] * iw[1] + l2 * uv[2, 0] * iw[2]) / wsum
        v = (l0 * uv[0, 1] * iw[0] + l1 * uv[1, 1] * iw[1] + l2 * uv[2, 1] * iw[2]) / wsum
        tu = (np.floor(u * tw).astype(int)) % tw
        tv = (np.floor(v * th).astype(int)) % th
        texel = rgba[tv, tu]
        if layered:
            alpha = texel[..., 3:4] / 255.0
            ok &= texel[..., 3] >= 8
        else:
            ok &= texel[..., 3] >= 64
        rgb = texel[..., :3]
    else:
        p3 = at[:, 2:5]
        nrm = np.cross(p3[1] - p3[0], p3[2] - p3[0])
        ln = np.linalg.norm(nrm)
        shade = 0.35 + 0.65 * abs(nrm @ light) / ln if ln else 0.6
        rgb = np.full(inside.shape + (3,), int(200 * shade))
    sub = img[y0:y1 + 1, x0:x1 + 1]
    if alpha is not None:
        rgb = (alpha * rgb + (1 - alpha) * sub).astype(np.uint8)
        solid = ok & (alpha[..., 0] >= 0.5)
    else:
        solid = ok
    sub[ok] = rgb[ok]
    zb[y0:y1 + 1, x0:x1 + 1][solid] = depth[solid]


def write_obj(path, vp, batches, info, textures=None):
    import os
    import numpy as np
    base = os.path.splitext(path)[0]
    textures = textures or {}
    mats = {}
    with open(path, 'w') as f:
        f.write('# EOD scene capture, vblank %d: world space (game units), one group per VU1 batch\n' % info['vblank'])
        f.write('mtllib %s.mtl\n' % os.path.basename(base))
        vi = vt = vn = 1
        for b in batches:
            rl = role(b, info)
            if rl not in ('world', 'object', 'object-nonuniform', 'sky'):
                continue
            m = np.array(b['model'])
            key = b['tex'] and (b['tex']['index_key'], b['tex']['palette_key'])
            mat = 'tex_%s_%s' % key if key else 'untextured'
            mats[mat] = key
            f.write('g run%d_%04x_%s\nusemtl %s\n' % (b['run'], b['pc'], rl, mat))
            for p in b['pos']:
                w = m @ np.array([p[0], p[1], p[2], 1.0])
                f.write('v %.5f %.5f %.5f\n' % tuple(w[:3]))
            for s, t in b['uv']:
                f.write('vt %.6f %.6f\n' % (s, 1 - t))
            has_n = all(n is not None for n in b['nrm'])
            if has_n:
                for n in b['nrm']:
                    w = m[:3, :3] @ np.array(n)
                    f.write('vn %.4f %.4f %.4f\n' % tuple(w))
            for tri in b['tris']:
                if has_n:
                    f.write('f %s\n' % ' '.join('%d/%d/%d' % (vi + i, vt + i, vn + i) for i in tri))
                else:
                    f.write('f %s\n' % ' '.join('%d/%d' % (vi + i, vt + i) for i in tri))
            vi += b['n']
            vt += b['n']
            if has_n:
                vn += b['n']
    with open(base + '.mtl', 'w') as f:
        for mat, key in mats.items():
            f.write('newmtl %s\nKd 0.8 0.8 0.8\n' % mat)
            if key and key in textures:
                png = '%s.png' % mat
                save_png(os.path.join(os.path.dirname(path) or '.', png), textures[key])
                f.write('map_Kd %s\n' % png)


def save_png(path, tex):
    from PIL import Image
    Image.frombytes('RGBA', (tex[0], tex[1]), tex[2]).save(path)


def cmd_reconstruct(a):
    import json
    import os
    import numpy as np
    from PIL import Image, ImageDraw
    stream = load(a.capture)
    vp, batches, info = scene(stream, read_tex_log(a.capture + '.tex'), a.vblank)
    os.makedirs(a.out, exist_ok=True)
    roles = Counter(role(b, info) for b in batches)
    errs = [b['proj_err'] for b in batches if b['proj_err'] is not None and role(b, info) in ('world', 'object', 'object-nonuniform')]
    wanted = {(b['tex']['index_key'], b['tex']['palette_key']) for b in batches
              if b['tex'] and b['tex']['index_key'] != '0000000000000000'}
    textures = load_textures(a.disc, wanted) if a.disc else {}
    write_obj(os.path.join(a.out, 'scene.obj'), vp, batches, info, textures)
    summary = dict(info, camera_vp=vp.tolist(), batches=len(batches), roles=roles,
                   triangles=sum(len(b['tris']) for b in batches if role(b, info) in ('world', 'object', 'object-nonuniform')),
                   projection_error_px=dict(n=len(errs), max=max(errs) if errs else None,
                                            median=float(np.median(errs)) if errs else None),
                   textures_wanted=len(wanted), textures_found=len(textures),
                   texture_sources=sorted({t[3] for t in textures.values()}))
    with open(os.path.join(a.out, 'scene.json'), 'w') as f:
        json.dump(dict(summary, batch_list=[{k: v for k, v in b.items() if k not in ('pos', 'uv', 'nrm', 'col', 'tris')}
                                            for b in batches]), f, indent=1, default=str)
    img = render(vp, batches, info, textures=textures)
    Image.fromarray(img).save(os.path.join(a.out, 'reconstruction.png'))
    print(json.dumps(summary, indent=1, default=str))
    if a.screenshot:
        shot = Image.open(a.screenshot).convert('RGB').resize((640, 448))
        rec = Image.fromarray(img)
        over = shot.copy()
        d = ImageDraw.Draw(over)
        ox, oy = info['xyoffset']
        for b in batches:
            if role(b, info) not in ('world', 'object', 'object-nonuniform'):
                continue
            m = np.array(b['model'])
            C = np.array([list(p) + [1.0] for p in b['pos']]) @ m.T @ vp.T
            for tri in b['tris']:
                for c, _ in clip_near(C[list(tri)], np.zeros((3, 1))):
                    pts = [(x / w - ox, y / w - oy) for x, y, _, w in c]
                    d.line(pts + [pts[0]], fill=(0, 255, 0), width=1)
        sheet = Image.new('RGB', (640 * 3, 448 + 24), (20, 20, 20))
        for i, (im, label) in enumerate(((shot, 'game screenshot'), (rec, 'reconstructed from captured 3D batches'),
                                         (over, 'reconstructed triangles over the screenshot'))):
            sheet.paste(im, (640 * i, 24))
            ImageDraw.Draw(sheet).text((640 * i + 6, 6), label, fill=(255, 255, 255))
        sheet.save(os.path.join(a.out, 'compare.png'))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('stats')
    s.add_argument('capture')
    s = sub.add_parser('runs')
    s.add_argument('capture')
    s.add_argument('--frame', type=int)
    s.add_argument('--pc')
    s.add_argument('--kicks', type=int, default=2)
    s.add_argument('--items', type=int, default=12)
    s.add_argument('--limit', type=int, default=0)
    s = sub.add_parser('reconstruct')
    s.add_argument('capture')
    s.add_argument('--out', required=True)
    s.add_argument('--vblank', type=int, help='frame interval to rebuild (default: the one with most VU1 runs)')
    s.add_argument('--disc', help='extracted game folder: textured render and OBJ textures')
    s.add_argument('--screenshot', help='the game screenshot of the same frame: writes compare.png')
    a = ap.parse_args()
    {'stats': cmd_stats, 'runs': cmd_runs, 'reconstruct': cmd_reconstruct}[a.cmd](a)


if __name__ == '__main__':
    sys.exit(main())
