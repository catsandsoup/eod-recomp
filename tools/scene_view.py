#!/usr/bin/env python3
"""Test viewer for the decoded-batch emitter (EOD_SCENE_EMIT, docs/SCENE_CAPTURE.md).

Reads the emitter's stream (FILE and FILE.tex) and renders frames of the live race from the
captured camera with the game's own light direction, next to the game's screenshot of the
same frame.

  python3 tools/scene_view.py stats FILE
  python3 tools/scene_view.py verify FILE              # reprojection error on the EMIT_VERIFY frame
  python3 tools/scene_view.py render FILE --frame N --disc GAME --screenshot frame_N.png --out DIR
  python3 tools/scene_view.py strip FILE --frames A B C --disc GAME --shots DIR --out strip.png

The viewer only uses what the emitter wrote: meshes (model space), per-batch model matrix,
world-space light, the frame's camera and the texture keys. Textures come from the disc
through the keys (tools/scene_capture.py load_textures).
"""
import argparse
import os
import struct
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scene_capture as sc  # noqa: E402


class Frame:
    def __init__(self, n):
        self.n, self.cam, self.batches, self.kicks, self.rows = n, None, [], [], []
        self.ns, self.gs_ns, self.fog, self.new_geoms = 0, 0, 0, 0


def tag(s):
    return struct.unpack('<I', s.encode())[0]


def load(path):
    """-> (frames {n: Frame}, geoms {id: dict})"""
    import numpy as np
    d = open(path, 'rb').read()
    assert d[:8] == b'EODEMIT1', 'not an EOD_SCENE_EMIT stream'
    frames, geoms, cur, i = {}, {}, [], 8
    T = {k: tag(k) for k in ('GEOM', 'BATC', 'CAMR', 'KICK', 'VBLK')}
    while i + 8 <= len(d):
        t, n = struct.unpack_from('<II', d, i)
        p = d[i + 8:i + 8 + n]
        i += 8 + n
        if t == T['GEOM']:
            gid, cnt = struct.unpack_from('<QI', p, 0)
            v = np.frombuffer(p, dtype=np.dtype([('p', '<f4', 3), ('uv', '<f4', 2), ('a', 'u1', 3), ('fl', 'u1')]),
                              count=cnt, offset=16)
            normal = bool(v['fl'][0] & 2) if cnt else False
            a = v['a'].astype(np.int8 if normal else np.uint8).astype(float)
            geoms[gid] = dict(n=cnt, pos=v['p'].astype(float), uv=v['uv'].astype(float), adc=(v['fl'] & 1).astype(bool),
                              nrm=a / 127.0 if normal else None, col=None if normal else a / 128.0)
        elif t == T['BATC']:
            pc, itop, k0, kn, gid = struct.unpack_from('<IIIIQ', p, 0)
            f = struct.unpack_from('<12f4f4f16f', p, 24)
            cur.append(('b', dict(pc=pc, itop=itop, kick0=k0, kicks=kn, geom=gid,
                                  model=np.vstack([np.array(f[:12]).reshape(3, 4), [0, 0, 0, 1]]),
                                  light=np.array(f[12:16]), dir2=np.array(f[16:20]),
                                  mvp=np.array(f[20:36]).reshape(4, 4).T)))
        elif t == T['CAMR']:
            cur.append(('c', np.array(struct.unpack_from('<16f', p, 8)).reshape(4, 4).T))
        elif t == T['KICK']:
            run, qwc = struct.unpack_from('<II', p, 0)
            cur.append(('k', p[8:8 + qwc * 16]))
        elif t == T['VBLK']:
            fn, nb, ng, ns = struct.unpack_from('<QIIQ', p, 0)
            fr = Frame(fn)
            fr.ns, fr.new_geoms = ns, ng
            for kind, x in cur:
                if kind == 'b':
                    fr.batches.append(x)
                elif kind == 'c':
                    fr.cam = x
                else:
                    fr.kicks.append(x)
            frames[fn] = fr
            cur = []
    try:
        t = open(path + '.tex', 'rb').read()
    except OSError:
        t = b''
    rows, i = [], 8
    while i + 8 <= len(t):
        tg, n = struct.unpack_from('<II', t, i)
        p = t[i + 8:i + 8 + n]
        i += 8 + n
        if tg == tag('TROW'):
            k, tbp, psm, tw, th, fbp, ik, pk, xyo = struct.unpack_from('<6I3Q', p, 0)
            rows.append(dict(kick=k, tbp=tbp, psm=psm, w=tw, h=th, fbp=fbp, index_key='%016x' % ik,
                             palette_key='%016x' % pk, xyoffset=((xyo & 0xFFFF) / 16.0, ((xyo >> 32) & 0xFFFF) / 16.0)))
        elif tg == tag('TVBL'):
            fn, fog, ns = struct.unpack_from('<3Q', p, 0)
            if fn in frames:
                frames[fn].rows, frames[fn].fog, frames[fn].gs_ns = rows, fog, ns
            rows = []
    return frames, geoms


def pair_textures(fr):
    """Texture row per batch: the last PATH1 draw-state row at or before the batch's last kick."""
    rows = sorted(fr.rows, key=lambda r: r['kick'])
    fbps = Counter(r['fbp'] for r in rows)
    disp = fbps.most_common(1)[0][0] if fbps else None
    xyo = Counter(r['xyoffset'] for r in rows if r['fbp'] == disp)
    j, last = 0, None
    for b in fr.batches:
        end = b['kick0'] + b['kicks']
        while j < len(rows) and rows[j]['kick'] <= end:
            last = rows[j]
            j += 1
        b['tex'] = last
        b['fbp'] = last['fbp'] if last else None
    return disp, (xyo.most_common(1)[0][0] if xyo else (1728.0, 1824.0))


def role(b, disp):
    import numpy as np
    m = b['model']
    if b['fbp'] is not None and disp is not None and b['fbp'] != disp:
        return 'offscreen'
    if np.abs(m - np.eye(4)).max() < 1e-3:
        return 'world'
    if b['pc'] == 0x270 and b['itop'] == 0x200:
        return 'hud'
    if b['itop'] == 0x280 and np.abs(m[:3, :3] - np.eye(3)).max() < 1e-3:
        return 'sky'
    return 'object'


def cmd_stats(a):
    import numpy as np
    frames, geoms = load(a.file)
    ns = np.array([f.ns for f in frames.values()]) / 1e6
    gns = np.array([f.gs_ns for f in frames.values()]) / 1e6
    nb = np.array([len(f.batches) for f in frames.values()])
    print('frames %d (%d..%d), meshes %d, batches/frame mean %.0f max %d' %
          (len(frames), min(frames), max(frames), len(geoms), nb.mean(), nb.max()))
    for name, v in (('producer', ns), ('gs', gns)):
        print('%s ms/frame: mean %.3f p50 %.3f p95 %.3f max %.3f' %
              (name, v.mean(), np.percentile(v, 50), np.percentile(v, 95), v.max()))
    size = os.path.getsize(a.file)
    print('stream %.1f MB (%.1f KB/frame); new meshes after the first 60 frames: %.1f/frame' %
          (size / 2**20, size / 1024 / len(frames),
           np.mean([f.new_geoms for n, f in sorted(frames.items())[60:]] or [0])))
    if a.frame is not None:
        fr = frames[a.frame]
        disp, _ = pair_textures(fr)
        print(Counter(role(b, disp) for b in fr.batches))
        print(Counter('%04x/%x' % (b['pc'], b['itop']) for b in fr.batches).most_common())


def screen(cam, model, pos):
    import numpy as np
    P = np.hstack([pos, np.ones((len(pos), 1))]) @ model.T
    C = P @ cam.T
    return C[:, 0] / C[:, 3], C[:, 1] / C[:, 3], P, C


def cmd_verify(a):
    """Projects every emitted mesh through camera x model matrix (emitted data only) and compares with
    the screen XY the game actually kicked for that batch (the EMIT_VERIFY frame's KICK records)."""
    import numpy as np
    frames, geoms = load(a.file)
    fr = next((f for f in frames.values() if f.kicks), None)
    if fr is None:
        sys.exit('no EOD_SCENE_EMIT_VERIFY frame in the stream')
    disp, xyo = pair_textures(fr)

    class R:
        pass
    errs, direct, onscreen, rigid, n1, used = [], [], [], [], 0, Counter()
    for b in fr.batches:
        r = R()
        r.kicks = fr.kicks[b['kick0']:b['kick0'] + b['kicks']]
        ov = sc.kick_vertices(r)
        g = geoms[b['geom']]
        rl = role(b, disp)
        if len(ov) != g['n'] or rl in ('hud', 'offscreen'):
            continue
        n1 += 1
        used[rl] += 1
        x, y, _, _ = screen(fr.cam, b['model'], g['pos'])
        o = np.array([(v[0], v[1]) for v in ov])
        e = np.hypot(x - o[:, 0], y - o[:, 1])
        errs.extend(e[np.isfinite(e)])
        # on screen: in front of the camera and inside the 640x448 display (GS offset applied)
        ox, oy = xyo
        on = np.isfinite(e) & (x - ox >= 0) & (x - ox < 640) & (y - oy >= 0) & (y - oy < 448)
        Cw = (np.hstack([g['pos'], np.ones((g['n'], 1))]) @ b['model'].T) @ fr.cam.T
        on &= Cw[:, 3] > 0
        onscreen.extend(e[on])
        if b['pc'] not in (0x24b0, 0x318):  # the skinned driver (open question 1)
            rigid.extend(e[on])
        # the same through the batch's own matrix (isolates the camera / model decomposition)
        C = np.hstack([g['pos'], np.ones((g['n'], 1))]) @ b['mvp'].T
        e2 = np.hypot(C[:, 0] / C[:, 3] - o[:, 0], C[:, 1] / C[:, 3] - o[:, 1])
        direct.extend(e2[np.isfinite(e2)])
    errs, direct, onscreen, rigid = np.array(errs), np.array(direct), np.array(onscreen), np.array(rigid)
    print('verify frame %d: %d batches with 1:1 kicks (%s), %d vertices' %
          (fr.n, n1, dict(used), len(errs)))
    for name, e in (('camera x model', errs), ('batch matrix', direct), ('on screen', onscreen), ('  w/o driver', rigid)):
        print('  %-15s %6d verts: median %.3f px, p95 %.3f, p99 %.3f, max %.3f; under 0.5 px %.1f%%' %
              (name, len(e), np.median(e), np.percentile(e, 95), np.percentile(e, 99), e.max(), 100 * (e < 0.5).mean()))
    return fr.n, errs


# ------------------------------------------------------------------- rendering
def render(fr, geoms, textures, size=(640, 448), lit=True):
    """z-buffered rasterizer through the emitted camera; texel x vertex colour, or texel x
    (ambient + diffuse * max(0, n . L)) with the game's world light and ambient (q17.w)."""
    import numpy as np
    W, H = size
    img = np.zeros((H, W, 3), np.uint8)
    zb = np.full((H, W), np.inf)
    disp, (ox, oy) = pair_textures(fr)
    order = {'sky': 0, 'world': 1, 'object': 2}
    for b in sorted(fr.batches, key=lambda b: order.get(role(b, disp), 9)):  # stable: draw order within a role
        rl = role(b, disp)
        if rl not in order:
            continue
        g = geoms[b['geom']]
        m = b['model']
        P = np.hstack([g['pos'], np.ones((g['n'], 1))]) @ m.T
        C = P @ fr.cam.T
        shade = np.ones((g['n'], 3))
        if g['nrm'] is not None and lit:
            Nw = g['nrm'] @ m[:3, :3].T
            Nw /= np.maximum(np.linalg.norm(Nw, axis=1, keepdims=True), 1e-9)
            L = b['light'][:3]
            amb = b['light'][3]
            d = np.clip(Nw @ L, 0, 1)
            shade = np.repeat((amb + (1 - amb) * d)[:, None], 3, 1)
        elif g['col'] is not None:
            shade = np.clip(g['col'][:, :3], 0, 2)
        tex = None
        t = b.get('tex')
        if t:
            tex = textures.get((t['index_key'], t['palette_key']))
            if tex:
                tex = (tex[0], tex[1], np.frombuffer(tex[2], np.uint8).reshape(tex[1], tex[0], 4))
        tris = sc.strip_tris(list(g['adc']), 4)
        for tri in tris:
            at = np.hstack([g['uv'][list(tri)], shade[list(tri)]])
            for c, att in sc.clip_near(C[list(tri)], at):
                raster(img, zb, c, att, rl, tex, ox, oy, W, H)
    return img


def raster(img, zb, c, at, rl, tex, ox, oy, W, H):
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

    def interp(k):
        return (l0 * at[0, k] * iw[0] + l1 * at[1, k] * iw[1] + l2 * at[2, k] * iw[2]) / wsum
    depth = 1 / wsum if rl != 'sky' else np.full(l0.shape, 1e30)
    layered = rl == 'world'
    zs = zb[y0:y1 + 1, x0:x1 + 1]
    ok = inside & ((depth <= zs * (1 + 1e-4)) if layered else (depth < zs))
    sh = np.stack([interp(2), interp(3), interp(4)], -1)
    alpha = None
    if tex:
        tw, th, rgba = tex
        u, v = interp(0), interp(1)
        texel = rgba[(np.floor(v * th).astype(int)) % th, (np.floor(u * tw).astype(int)) % tw]
        if layered:
            alpha = texel[..., 3:4] / 255.0
            ok &= texel[..., 3] >= 8
        else:
            ok &= texel[..., 3] >= 64
        rgb = texel[..., :3] * sh
    else:
        rgb = 170.0 * sh
    rgb = np.clip(rgb, 0, 255)
    sub = img[y0:y1 + 1, x0:x1 + 1]
    if alpha is not None:
        rgb = alpha * rgb + (1 - alpha) * sub
        solid = ok & (alpha[..., 0] >= 0.5)
    else:
        solid = ok
    sub[ok] = rgb[ok].astype(np.uint8)
    zs[solid] = depth[solid]


def textures_for(frames, geoms, disc):
    wanted = set()
    for fr in frames:
        pair_textures(fr)
        for b in fr.batches:
            if b.get('tex') and b['tex']['index_key'] != '0' * 16:
                wanted.add((b['tex']['index_key'], b['tex']['palette_key']))
    found = sc.load_textures(disc, wanted) if disc else {}
    print('textures: %d wanted, %d found on the disc' % (len(wanted), len(found)))
    return found


def edges(img):
    import numpy as np
    g = img.astype(float).mean(-1)
    e = np.zeros_like(g)
    e[1:-1, 1:-1] = np.hypot(g[1:-1, 2:] - g[1:-1, :-2], g[2:, 1:-1] - g[:-2, 1:-1])
    return e


def cmd_render(a):
    import numpy as np
    from PIL import Image
    frames, geoms = load(a.file)
    fr = frames[a.frame]
    tex = textures_for([fr], geoms, a.disc)
    img = render(fr, geoms, tex)
    os.makedirs(a.out, exist_ok=True)
    Image.fromarray(img).save(os.path.join(a.out, 'view_%05d.png' % a.frame))
    if a.screenshot:
        shot = np.array(Image.open(a.screenshot).convert('RGB').resize((640, 448), Image.BILINEAR))
        # overlay: the viewer's edges (green) on the game's frame
        ov = shot.copy()
        e = edges(img)
        ov[e > 40] = (0, 255, 0)
        side = np.hstack([shot, img, ov])
        Image.fromarray(side).save(os.path.join(a.out, 'compare_%05d.png' % a.frame))
        # agreement of edge maps (both images blurred a little): a coarse structural score
        es, ev = edges(shot) > 40, e > 40
        print('frame %d: edge agreement %.1f%% of viewer edges within 1 px of a game edge' %
              (a.frame, 100 * near(ev, es)))
    print('wrote', a.out)


def near(a, b, r=1):
    import numpy as np
    d = b.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            d |= np.roll(np.roll(b, dy, 0), dx, 1)
    return (a & d).sum() / max(a.sum(), 1)


def cmd_strip(a):
    import numpy as np
    from PIL import Image
    frames, geoms = load(a.file)
    sel = [frames[n] for n in a.frames]
    tex = textures_for(sel, geoms, a.disc)
    rows = []
    for fr in sel:
        img = render(fr, geoms, tex)
        shot = os.path.join(a.shots, 'frame_%05d.png' % fr.n) if a.shots else None
        left = (np.array(Image.open(shot).convert('RGB').resize((640, 448), Image.BILINEAR))
                if shot and os.path.exists(shot) else np.zeros_like(img))
        rows.append(np.hstack([left, img]))
        print('rendered frame', fr.n)
    Image.fromarray(np.vstack(rows)).save(a.out)
    print('wrote', a.out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('stats')
    p.add_argument('file')
    p.add_argument('--frame', type=int)
    p = sub.add_parser('verify')
    p.add_argument('file')
    p = sub.add_parser('render')
    p.add_argument('file')
    p.add_argument('--frame', type=int, required=True)
    p.add_argument('--disc')
    p.add_argument('--screenshot')
    p.add_argument('--out', default='.')
    p = sub.add_parser('strip')
    p.add_argument('file')
    p.add_argument('--frames', type=int, nargs='+', required=True)
    p.add_argument('--disc')
    p.add_argument('--shots')
    p.add_argument('--out', default='strip.png')
    a = ap.parse_args()
    {'stats': cmd_stats, 'verify': cmd_verify, 'render': cmd_render, 'strip': cmd_strip}[a.cmd](a)


if __name__ == '__main__':
    main()
