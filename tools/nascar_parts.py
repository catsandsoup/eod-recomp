#!/usr/bin/env python3
"""Split an imported NASCAR body into TDEOD's detachable panel groups.

Why this exists
---------------
The EE keeps a table of eight detachable panels (0x44AE68, built by 0x18DBF8 and read
every frame by 0x18AFE0 / 0x18CFE8).  Each row names a TJRM helper in the car's MODL and
a MODL part group:

    row  panel                  helper looked up in TJRM        group
    0    left door              "left door hinge"                6
    1    right door             "right door hinge"               5
    2    hood                   "right hood hinge"               2
    3    decklid                "right decklid hinge"           11
    4    front bumper           "left front bumper"             12
    5    rear bumper            "left rear bumper"              13
    6    left fender            "left door hinge" (+0.61 m z)    4
    7    right fender           "right door hinge" (+0.61 m z)   3

A row is live only if its helper exists ("car: can't find helper %s in %s (%s)" otherwise)
and the nearest Bone.mvl bone to that helper moves further than the row's threshold
(0.076 m doors/bumpers/fenders, 0.127 m hood, 0.038 m decklid).  The panel is then drawn
as debris from the part(s) of its group and the group bit is cleared on the car.  The
imported Cup body has only the groups 1, 2, 9 and 10, and only hood helpers, so before
this module five of the eight rows never existed and the car could not shed them.

This module cuts the NASCAR triangles (never donor geometry) into the native groups
with axis-aligned regions measured on the Cup body.  Triangles that straddle a door
edge are clipped.  The original strip batches stay in place with the moved triangles
switched off through the vertex ADC bit; moved or clipped triangles are re-emitted as
independent 3-vertex strips inside new batches that reuse the source batch's
quantisation rows, so no new VU program, row or format is introduced.
"""

import math
import struct

import nascar_gen as gen

# --- regions (Cup body, TDEOD model space: +x right, +y up, +z forward) -------------------
FRONT_BUMPER_Z = 2.05        # front fascia / valance starts here
FENDER_X = 0.62              # |x| beyond this on the front clip is the fender
REAR_BUMPER_Z = -2.55        # rear fascia lower panel ends here
REAR_BUMPER_Y = 0.55         # lower bar only; tail lamps and the number panel stay on the body
DECKLID_Y = 0.80             # rear clip surfaces above this are the decklid
DECKLID_X = 0.88
DOOR_Z = (-0.75, 0.72)       # door rear / front edge
DOOR_X = 0.80                # side surface (window glass, side panel) beyond |x|
DOOR_Y_MAX = 1.22

BODY_TEX = None              # set by caller: paintable body texture (lower case)
SPLIT_TEXTURES = None        # filled with the body texture plus window glass names


def _lerp(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(len(a)))


def _vert_lerp(v0, v1, t):
    return dict(p=_lerp(v0['p'], v1['p'], t), n=_lerp(v0['n'], v1['n'], t), uv=_lerp(v0['uv'], v1['uv'], t), adc=False)


def clip_polygon(poly, axis, value):
    """Split a convex polygon (list of vertex dicts) at p[axis] == value -> (below, above)."""
    lo, hi = [], []
    n = len(poly)
    for i in range(n):
        a, b = poly[i], poly[(i + 1) % n]
        da, db = a['p'][axis] - value, b['p'][axis] - value
        (lo if da < 0 else hi).append(a)
        if (da < 0) != (db < 0) and da != db:
            t = da / (da - db)
            v = _vert_lerp(a, b, t)
            v['p'] = tuple(value if k == axis else v['p'][k] for k in range(3))
            lo.append(v)
            hi.append(dict(v))
    return (lo if len(lo) >= 3 else None), (hi if len(hi) >= 3 else None)


def _area2(poly):
    s = 0.0
    p0 = poly[0]['p']
    for i in range(1, len(poly) - 1):
        a, b = poly[i]['p'], poly[i + 1]['p']
        e1 = [a[k] - p0[k] for k in range(3)]
        e2 = [b[k] - p0[k] for k in range(3)]
        c = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
        s += math.sqrt(c[0] ** 2 + c[1] ** 2 + c[2] ** 2)
    return s


def centroid(poly):
    return tuple(sum(v['p'][k] for v in poly) / len(poly) for k in range(3))


def classify(group, c):
    """Target TDEOD group for a piece of body in `group` (after remap_stock_groups) at centroid c."""
    x, y, z = c
    if group == 1:                                  # front clip
        if z > FRONT_BUMPER_Z and y < 0.62:
            return 12
        if abs(x) > FENDER_X and z <= FRONT_BUMPER_Z:
            return 3 if x > 0 else 4
        return 1
    if group == 10:                                 # rear clip
        if y >= DECKLID_Y and abs(x) < DECKLID_X:
            return 11
        if z < REAR_BUMPER_Z and y < REAR_BUMPER_Y:
            return 13
        return 10
    if group == 9:                                  # body, greenhouse, glass
        if abs(x) > DOOR_X and DOOR_Z[0] <= z <= DOOR_Z[1] and y <= DOOR_Y_MAX:
            return 5 if x > 0 else 6
        return 9
    return group


def _cut_planes(group, poly):
    """z planes that a side-body triangle must be cut at (only triangles lying wholly on the side)."""
    if group != 9 or min(abs(v['p'][0]) for v in poly) < DOOR_X:
        return []
    zs = [v['p'][2] for v in poly]
    return [z for z in DOOR_Z if min(zs) < z < max(zs)]


# --- batch decode ------------------------------------------------------------------------

def decode_batches(part):
    """[{'info': walk_vif batch, 'verts': [...], 'tris': [(i0, i1, i2, emit_vertex)]}]."""
    st = part['stream']
    out = []
    for b in gen.walk_vif(st):
        u = b['unpacks']
        for a in (0, 1, 2, 3, 150, 198):
            if a not in u:
                raise ValueError('batch lacks UNPACK @%d; not a skinned TDEOD body batch' % a)
        if b['mscal'] != 0x5C:
            raise ValueError('unexpected MSCAL %#x in a body part' % b['mscal'])
        n = b['count']
        pos_off = [gen.f32(x) for x in b['q199'][:3]]
        st_off = [gen.f32(x) for x in b['q198'][:2]]
        verts = []
        for i in range(n):
            pv = struct.unpack_from('<3H', st, u[2]['off'] + 6 * i)
            p = tuple(gen.f32(u[2]['row'][c] + pv[c]) + pos_off[c] for c in range(3))
            nb = st[u[1]['off'] + 4 * i:u[1]['off'] + 4 * i + 4]
            sv = struct.unpack_from('<2H', st, u[3]['off'] + 4 * i)
            uv = tuple(gen.f32(u[3]['row'][c] + sv[c]) + st_off[c] for c in range(2))
            verts.append(dict(p=p, n=tuple(float(x) for x in nb[:3]), uv=uv, adc=bool(nb[3] & 0x80)))
        tris = []
        start = 0
        for i, v in enumerate(verts):
            if v['adc'] and (i == 0 or not verts[i - 1]['adc']):
                start = i
            if i >= 2 and not v['adc']:
                tris.append((i - 2, i - 1, i, i) if (i - start) % 2 == 0 else (i - 1, i - 2, i, i))
        out.append(dict(info=b, verts=verts, tris=tris))
    return out


def _batch_range(stream, prev_end, b):
    return prev_end, b['mscal_off'] + 4


def _tokens(stream, start, end):
    """Split a batch's VIF code into ('raw', bytes) and ('unpack', addr, cmd, imm, payload) tokens."""
    p = start
    toks = []
    while p < end:
        w = struct.unpack_from('<I', stream, p)[0]
        cmd, num, imm = (w >> 24) & 0x7F, (w >> 16) & 0xFF, w & 0xFFFF
        if cmd >= 0x60:
            nb = gen._unpack_bytes(cmd, num)
            toks.append(('unpack', imm & 0x3FF, cmd, imm, bytes(stream[p + 4:p + 4 + nb]), num))
            p += 4 + nb
        elif cmd == 0x20:
            toks.append(('raw', bytes(stream[p:p + 8])))
            p += 8
        elif cmd in (0x30, 0x31):
            toks.append(('raw', bytes(stream[p:p + 20])))
            p += 20
        else:
            toks.append(('raw', bytes(stream[p:p + 4])))
            p += 4
    return toks


def _quant(value, row_bits, offset):
    base = gen.f32(row_bits)
    ulp = gen.f32(row_bits + 1) - base
    return max(0, min(0xFFFF, int(round((value - offset - base) / ulp))))


def encode_batch(stream, prev_end, bd, tris):
    """Encode `tris` (list of 3-vertex lists) as independent strips reusing batch bd's rows."""
    b = bd['info']
    u = b['unpacks']
    pos_off = [gen.f32(x) for x in b['q199'][:3]]
    st_off = [gen.f32(x) for x in b['q198'][:2]]
    verts = []
    for tri in tris:
        for k, v in enumerate(tri):
            verts.append((v, k < 2))
    n = len(verts)
    pos = bytearray()
    nrm = bytearray()
    uvb = bytearray()
    for v, adc in verts:
        for c in range(3):
            pos += struct.pack('<H', _quant(v['p'][c], u[2]['row'][c], pos_off[c]))
        nrm += bytes(max(0, min(255, int(round(v['n'][c])))) for c in range(3)) + bytes([0x80 if adc else 0])
        for c in range(2):
            uvb += struct.pack('<H', _quant(v['uv'][c], u[3]['row'][c], st_off[c]))
    while len(pos) % 4:
        pos += b'\0'
    out = bytearray()
    for tok in _tokens(stream, *_batch_range(stream, prev_end, b)):
        if tok[0] == 'raw':
            out += tok[1]
            continue
        _, addr, cmd, imm, payload, num = tok
        if addr == 0:
            out += struct.pack('<I', (cmd << 24) | (num << 16) | imm) + struct.pack('<I', 0x8000 | n)
        elif addr in (1, 2, 3, 150):
            data = {1: bytes(nrm), 2: bytes(pos), 3: bytes(uvb), 150: bytes(4 * n)}[addr]
            out += struct.pack('<I', (cmd << 24) | ((n & 0xFF) << 16) | imm) + data
        else:
            out += struct.pack('<I', (cmd << 24) | (num << 16) | imm) + payload
    return bytes(out)


MAX_VERTS = 48


def split_model(model, body_textures):
    """Split model parts in place; returns {target_group: triangle count}."""
    new_parts = {}      # (group, tex, hdr tail) -> {'hdr': bytearray, 'chunks': []}
    kept = []
    moved = {}
    for part in model['parts']:
        g = gen.part_group(part) & 0xFFFF
        tex = gen.part_texture(part).lower()
        if g not in (1, 9, 10) or tex not in body_textures:
            kept.append(part)
            continue
        batches = decode_batches(part)
        st = bytearray(part['stream'])
        high = gen.part_group(part) & 0xFFFF0000
        prev_end = 0
        alive = 0
        for bd in batches:
            b = bd['info']
            pieces_by_group = {}
            for (i0, i1, i2, emit) in bd['tris']:
                tri = [bd['verts'][i] for i in (i0, i1, i2)]
                polys = [tri]
                for z in _cut_planes(g, tri):
                    nxt = []
                    for poly in polys:
                        lo, hi = clip_polygon(poly, 2, z)
                        nxt += [q for q in (lo, hi) if q]
                    polys = nxt
                pieces = [(classify(g, centroid(poly)), poly) for poly in polys if _area2(poly) > 1e-9]
                if len(polys) == 1 and pieces and pieces[0][0] == g:
                    alive += 1
                    continue                       # unchanged: stays in the original strip
                nrm = b['unpacks'][1]['off'] + 4 * emit + 3
                st[nrm] |= 0x80                    # switch the original triangle off (ADC)
                for tg, poly in pieces:
                    for k in range(1, len(poly) - 1):
                        pieces_by_group.setdefault(tg, []).append([poly[0], poly[k], poly[k + 1]])
            for tg, tris in sorted(pieces_by_group.items()):
                key = (tg, tex, high)
                ent = new_parts.setdefault(key, dict(hdr=bytearray(part['hdr']), chunks=[]))
                moved[tg] = moved.get(tg, 0) + len(tris)
                per = MAX_VERTS // 3
                for s in range(0, len(tris), per):
                    ent['chunks'].append(encode_batch(part['stream'], prev_end, bd, tris[s:s + per]))
            prev_end = b['mscal_off'] + 4
        if alive:
            part['stream'] = bytes(st)
            kept.append(part)
    parts = list(kept)
    for (tg, tex, high), ent in sorted(new_parts.items()):
        hdr = ent['hdr']
        struct.pack_into('<I', hdr, 0x18, high | tg)
        stream = b''.join(ent['chunks'])
        while len(stream) % 16:
            stream += b'\0\0\0\0'
        parts.append({'hdr': hdr, 'stream': stream, 'trailer': b'\xe3' * 8})
    if len(parts) > 36:
        raise ValueError('split model needs %d parts; the retail models hold at most 36' % len(parts))
    model['parts'] = parts
    return moved


# --- TJRM helpers for the panel table ----------------------------------------------------

def panel_helpers(gpts):
    """Helpers the EE looks up by name (see the table at the top), placed like Algar's:
    door hinge at the front edge / latch at the rear edge, decklid hinges at its forward edge,
    bumper helpers just inside each end, hood hinges on the rear corners with the latch at the nose."""
    out = []
    ident = gen.IDENTITY

    def box(g):
        pts = gpts.get(g)
        if not pts:
            return None
        return ([min(p[k] for p in pts) for k in range(3)], [max(p[k] for p in pts) for k in range(3)])

    hood = box(2)
    if hood:
        lo, hi = hood
        pts = gpts[2]
        rear = [p for p in pts if p[2] < lo[2] + 0.06]
        front = [p for p in pts if p[2] > hi[2] - 0.06]
        xmax = max(abs(p[0]) for p in pts)
        out += [('left hood hinge', ident, (-xmax, max(p[1] for p in rear), lo[2])),
                ('right hood hinge', ident, (xmax, max(p[1] for p in rear), lo[2])),
                ('hood latch', ident, (0.0, min(p[1] for p in front), hi[2]))]
    for g, side, name in ((6, -1, 'left'), (5, 1, 'right')):
        bx = box(g)
        if bx:
            lo, hi = bx
            xs = side * (max(abs(lo[0]), abs(hi[0])) - 0.024)
            out.append((name + ' door hinge', ident, (xs, lo[1] + 0.186, hi[2] - 0.007)))
            out.append((name + ' door latch', ident, (xs, lo[1] + 0.52, lo[2] + 0.009)))
    bx = box(11)
    if bx:
        lo, hi = bx
        top = max(p[1] for p in gpts[11])
        out += [('left decklid hinge', ident, (-0.588, top, hi[2])),
                ('right decklid hinge', ident, (0.588, top, hi[2])),
                ('decklid latch', ident, (0.0, lo[1], lo[2]))]
    bx = box(12)
    if bx:
        lo, hi = bx
        y = hi[1] - 0.054
        out += [('left front bumper', ident, (-(max(abs(lo[0]), hi[0]) - 0.105), y, hi[2] - 0.086)),
                ('right front bumper', ident, (max(abs(lo[0]), hi[0]) - 0.105, y, hi[2] - 0.086))]
    bx = box(13)
    if bx:
        lo, hi = bx
        y = hi[1] - 0.054
        out += [('left rear bumper', ident, (-(max(abs(lo[0]), hi[0]) - 0.118), y, lo[2] + 0.082)),
                ('right rear bumper', ident, (max(abs(lo[0]), hi[0]) - 0.118, y, lo[2] + 0.082))]
    return out
