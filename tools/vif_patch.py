#!/usr/bin/env python3
"""In-place geometry patching of native MODL/VIF data (positions, normals, UVs).

export: native MODL member -> OBJ + sidecar map (vertex -> byte offsets/format).
apply:  edited OBJ + map -> same-length member with only the touched UNPACK
        slots re-encoded (same format/precision); topology changes are refused.
apply-archive: same, then patch the member inside a 0TSR archive in place and
        update its directory CRC (like modding_backend.save_single_archive).
Decoding conventions follow vif_mesh.py (imported, not modified).
"""
import argparse
import hashlib
import json
import math
import os
import re
import struct
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vif_mesh as vm
from rst0 import eod_crc

MAP_FORMAT = 'eod-vif-patch-1'
POS_TOL, UV_TOL, NRM_TOL = 2e-6, 2e-6, 1e-4   # OBJ text round-off ignored below these
ROLES = {(0, 0, 0): 'count', (2, 2, 1): 'pos', (2, 2, 2): 'pos', (2, 2, 0): 'pos', (1, 3, 2): 'nrm', (3, 1, 1): 'uv',
         (3, 1, 2): 'uv', (3, 1, 0): 'uv', (150, 3, 2): 'skin', (198, 3, 0): 'off'}
UF = {32: '<%dI', 16: '<%dH', 8: '<%dB'}
HEADER_BOUNDS = (0x18, 0x40)   # radius, centre, bbox: the only non-VIF bytes apply may touch


class PatchError(ValueError):
    pass


def u32(data, o):
    return struct.unpack_from('<I', data, o)[0]


def fbits(bits):
    return vm.f32(bits)


def f32round(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


# ----------------------------------------------------------------------------- scan

def scan_part(data, part):
    """Walk the VIF stream; per MSCAL batch return the byte slots of count/pos/nrm/uv/q198-199."""
    cl = wl = 1
    mode = mask = 0
    row = [0] * 4
    recs, batches, users, row_off = [], [], Counter(), -1
    p, end = part['offset'] + 0x38, part['offset'] + part['size']
    while p + 4 <= end:
        w = u32(data, p)
        cmd, num, imm = (w >> 24) & 0x7F, (w >> 16) & 0xFF, w & 0xFFFF
        p += 4
        if cmd >= 0x60:
            vn, vl = (cmd >> 2) & 3, cmd & 3
            n = num or 256
            bpe = 2 if vl == 3 else [4, 2, 1][vl] * (vn + 1)
            recs.append(dict(addr=imm & 0x3FF, vn=vn, vl=vl, n=n, off=p, cl=cl, wl=wl, mode=mode,
                             mask=mask if (cmd >> 4) & 1 else 0, row=list(row), row_off=row_off))
            users[row_off] += 1
            p += (n * bpe + 3) & ~3
        elif cmd == 0x01:
            cl, wl = imm & 0xFF, (imm >> 8) & 0xFF
        elif cmd == 0x05:
            mode = imm & 3
        elif cmd == 0x20:
            mask = u32(data, p); p += 4
        elif cmd in (0x30, 0x31):
            if cmd == 0x30:
                row = list(struct.unpack_from('<4I', data, p))
                row_off = p
            p += 16
        elif cmd in (0x14, 0x15, 0x17):
            batches.append(_batch(data, recs, imm if cmd != 0x17 else -1))
            recs = []
        elif cmd == 0x4A:
            p += (num or 256) * 8
        elif cmd in (0x50, 0x51):
            p += imm * 16
        elif cmd not in (0x00, 0x02, 0x03, 0x04, 0x06, 0x07, 0x10, 0x11, 0x13):
            raise PatchError('unknown VIF cmd %02x at %x' % (cmd, p - 4))
    for b in batches:
        for role in ('pos', 'uv'):
            if role in b:
                b[role]['shared'] = users[b[role]['row_off']] > 1
    return batches


def _batch(data, recs, mscal):
    slots = {}
    for r in recs:
        role = ROLES.get((r['addr'], r['vn'], r['vl']))
        if role:
            slots[role] = r
    b = dict(mscal=mscal, count=0, unsupported=[])
    if 'count' not in slots or 'pos' not in slots:
        return b
    b['count'] = u32(data, slots['count']['off']) & 0x7FFF
    n = b['count']
    for role in ('pos', 'nrm', 'uv'):
        r = slots.get(role)
        if r is None:
            continue
        size = 4 if role == 'nrm' else (4, 2, 1)[r['vl']]        # bytes per u16 (vl1) / u8 (vl2) component
        stride = 4 if role == 'nrm' else size * (3 if role == 'pos' else 2)
        # offset mode (STMOD 1), full cl>=wl==1 skip-write, one element per vertex at addr+i*cl
        ok = r['mode'] == 1 and r['wl'] == 1 and r['n'] >= n and r['addr'] == {'pos': 2, 'nrm': 1, 'uv': 3}[role]
        if role != 'nrm':
            lanes = 3 if role == 'pos' else 2
            ok = ok and all(((r['mask'] >> (c * 2)) & 3) == 0 for c in range(lanes))
            ok = ok and all(r['row'][c] & ((1 << 8 * size) - 1) == 0 for c in range(lanes))
        if not ok:
            b['unsupported'].append(role)
            continue
        b[role] = dict(off=r['off'], stride=stride, cl=r['cl'], row=r['row'][:3 if role == 'pos' else 2], bits=8 * size, row_off=r['row_off'])
    if 'off' in slots and slots['off']['n'] >= 2 and slots['off']['mode'] == 0:
        b['q198'] = slots['off']['off']
        b['q199'] = slots['off']['off'] + 16
    if 'pos' not in b or 'q199' not in b:
        b['unsupported'].append('pos/offset')
        b['count'] = 0
    if 'uv' in b and 'q198' not in b:
        del b['uv']
        b['unsupported'].append('uv-offset')
    return b


def ulp_of(rowbits):
    return fbits(rowbits + 1) - fbits(rowbits)


# ----------------------------------------------------------------------------- decode

def read_batch(data, b):
    """Native-order vertices of one batch: dict(p, uv, n) lists from the slots."""
    n = b['count']
    P, UV, N = [], [], []
    if not n or 'pos' not in b or 'q199' not in b:
        return dict(p=P, uv=UV, n=N)
    o199 = struct.unpack_from('<3f', data, b['q199'])
    pb = b['pos']
    base = [fbits(r) for r in pb['row']]
    for j in range(n):
        u = struct.unpack_from(UF[pb['bits']] % 3, data, pb['off'] + j * pb['stride'])
        P.append(tuple(fbits(pb['row'][c] + u[c]) + o199[c] for c in range(3)))
    if 'uv' in b:
        o198 = struct.unpack_from('<2f', data, b['q198'])
        ub = b['uv']
        for j in range(n):
            u = struct.unpack_from(UF[ub['bits']] % 2, data, ub['off'] + j * ub['stride'])
            UV.append(tuple(fbits(ub['row'][c] + u[c]) + o198[c] for c in range(2)))
    if 'nrm' in b:
        for j in range(n):
            q = data[b['nrm']['off'] + j * 4: b['nrm']['off'] + j * 4 + 3]
            N.append(tuple((x - 128) / 127.0 for x in q))
    return dict(p=P, uv=UV, n=N)


def decode_member(data):
    """Parse a MODL member into parts -> batches (with byte slots) -> flat native vertices."""
    m = vm.parse_modl(data)
    parts = []
    for part in m['parts']:
        batches = scan_part(data, part)
        verts = dict(p=[], uv=[], n=[])
        for b in batches:
            b['vbase'] = len(verts['p'])
            r = read_batch(data, b)
            for k in verts:
                verts[k].extend(r[k])
        parts.append(dict(slot=part['slot'], offset=part['offset'], size=part['size'],
                          texture=part['texture'], group=part['group'], batches=batches, v=verts))
    return m, parts


def part_faces(data, part, batches):
    """Triangles (part-local native vertex indices) exactly as vif_mesh/export_obj emits them."""
    vbatches = vm.decode_part(data, part)
    faces, base = [], 0
    for bt in vbatches:
        for (a, b, c) in vm.triangulate_batch(bt):
            pa, pb, pc = bt['verts'][a]['p'], bt['verts'][b]['p'], bt['verts'][c]['p']
            if pa == pb or pb == pc or pa == pc:
                continue
            faces.append((a + base, b + base, c + base))
        base += len(bt['verts'])
    return faces


def part_object(p):
    return 'slot%02d_g%d_%s%s' % (p['slot'], p['group'] & 0xFFFF, os.path.splitext(p['texture'])[0] or 'none',
                                  '_body' if p['group'] & 0x10000 else '')


# ----------------------------------------------------------------------------- export

def export_model(data, obj_path, member='', archive=''):
    m, parts = decode_member(data)
    raw_parts = {p['slot']: p for p in m['parts']}
    lines = ['# vif_patch export: vertex order = native UNPACK order; do not add/delete/reorder vertices or faces']
    vbase = vtbase = 1
    vnbase = 1
    mp = dict(format=MAP_FORMAT, archive=archive, member=member, size=len(data), crc=eod_crc(data),
              sha256=hashlib.sha256(data).hexdigest(), header=dict(radius=m['radius'], center=m['center'],
              bbox=m['bbox']), notes='position=f32(rowbits+u16)+q199; uv=f32(rowbits+u16)+q198; '
              'normal=(byte-128)/127; vertex j offset = slot.off + j*slot.stride; batch vertices follow vbase',
              parts=[])
    for part in parts:
        if not part['v']['p']:
            continue
        v = part['v']
        lines.append('o ' + part_object(part))
        lines += ['v %.7f %.7f %.7f' % p for p in v['p']]
        has_uv, has_n = len(v['uv']) == len(v['p']), len(v['n']) == len(v['p'])
        if has_uv:
            lines += ['vt %.7f %.7f' % (s, 1.0 - t) for s, t in v['uv']]
        if has_n:
            lines += ['vn %.6f %.6f %.6f' % n for n in v['n']]
        for (a, b, c) in part_faces(data, raw_parts[part['slot']], part['batches']):
            lines.append('f ' + ' '.join('%d/%s/%s' % (i + vbase, i + vtbase if has_uv else '', i + vnbase if has_n else '')
                                        for i in (a, b, c)))
        mp['parts'].append(dict(slot=part['slot'], object=part_object(part), vertices=len(v['p']),
                                batches=[{k: b[k] for k in b if k != 'unsupported'} | dict(unsupported=b['unsupported'])
                                         for b in part['batches'] if b['count']]))
        vbase += len(v['p'])
        vtbase += len(v['p']) if has_uv else 0
        vnbase += len(v['p']) if has_n else 0
    with open(obj_path, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    return mp


# ----------------------------------------------------------------------------- OBJ parse

def parse_obj(path):
    """Objects -> {'v': [...], 'vt': {corner->..}, faces: [[(v,vt,vn)...]]} with part-local indices."""
    V, VT, VN, objs, cur = [], [], [], {}, None
    with open(path) as fh:
        lines = fh.read().split('\n')
    for line in lines:
        t = line.split()
        if not t:
            continue
        if t[0] in ('o', 'g'):
            cur = objs.setdefault(' '.join(t[1:]), dict(v0=len(V), nv=0, faces=[]))
        elif t[0] == 'v':
            if cur is None:
                cur = objs.setdefault('default', dict(v0=len(V), nv=0, faces=[]))
            V.append(tuple(float(x) for x in t[1:4]))
            cur['nv'] += 1
        elif t[0] == 'vt':
            VT.append((float(t[1]), float(t[2])))
        elif t[0] == 'vn':
            VN.append(tuple(float(x) for x in t[1:4]))
        elif t[0] == 'f':
            corners = []
            for c in t[1:]:
                a = (c.split('/') + ['', ''])[:3]
                corners.append(tuple(int(x) if x else 0 for x in a))
            if any(i < 0 for c in corners for i in c):
                raise PatchError('negative OBJ indices are not supported')
            cur['faces'].append(corners)
    return V, VT, VN, objs


def canon(tri):
    k = tri.index(min(tri))
    return tri[k:] + tri[:k]


# ----------------------------------------------------------------------------- encode

def _fit_lane(vals, row, off, bits, shared, clamp, what):
    """Re-encode one lane. Returns (u list, new_off|None, new_rowbits|None).
    Order: keep row+offset; else shift offset by whole ulps; else raise the row exponent (coarser ulp)."""
    top = (1 << bits) - 1
    if bits == 32:    # raw float lane (row 0): store f32(target - offset) directly
        return [struct.unpack('<I', struct.pack('<f', x - off))[0] for x in vals], None, None

    def enc(r, o):
        b, u = fbits(r), ulp_of(r)
        return [(x - o - b) / u for x in vals]
    us = enc(row, off)
    rnd = lambda us: [min(top, max(0, round(u))) for u in us]
    if all(-0.5 <= u < top + 0.5 for u in us):
        return rnd(us), None, None
    k = round(min(us))
    if max(us) - k < top + 0.5:
        no = f32round(off + k * ulp_of(row))
        return rnd(enc(row, no)), no, None
    if not shared:
        lo = min(vals)
        for e in range(((row >> 23) & 0xFF) + 1, 150):
            r = e << 23
            no = f32round(lo - ulp_of(r) - fbits(r))
            us = enc(r, no)
            if max(us) < top + 0.5 and min(us) > -0.5:
                return rnd(us), no, r
    if not clamp:
        raise PatchError('%s span %.4g cannot be encoded in a %d-bit slot%s; use --clamp to saturate'
                         % (what, max(vals) - min(vals), bits, ' (STROW shared)' if shared else ''))
    return rnd(us), None, None


def patch_member(data, mp, obj_path, channels=('pos', 'uv', 'nrm'), clamp=False):
    """Return (new_bytes, report). Raises PatchError on topology change or out-of-range data."""
    if mp.get('format') != MAP_FORMAT:
        raise PatchError('not a vif_patch map')
    if len(data) != mp['size'] or eod_crc(data) != mp['crc']:
        raise PatchError('map does not describe this member (size/CRC mismatch)')
    m, parts = decode_member(data)
    pby = {p['slot']: p for p in parts}
    V, VT, VN, objs = parse_obj(obj_path)
    out = bytearray(data)
    rep = dict(parts=0, vertices=0, changed_vertices=0, max_err=dict(pos=0.0, uv=0.0, nrm=0.0),
               out_of_range=[], refit_batches=0, rebased_batches=0, warnings=[], clamped=0)
    byslot = {}
    for name, o in objs.items():
        mm = re.search(r'slot(\d+)', name)
        if not mm:
            raise PatchError('OBJ object %r has no slotNN name' % name)
        byslot[int(mm.group(1))] = (name, o)
    expect = [e for e in mp['parts']]
    if set(byslot) != {e['slot'] for e in expect}:
        raise PatchError('OBJ parts %s differ from native parts %s (topology change)'
                         % (sorted(byslot), sorted(e['slot'] for e in expect)))
    allowed = [HEADER_BOUNDS]
    raw_parts = {p['slot']: p for p in m['parts']}
    for e in expect:
        part = pby[e['slot']]
        name, o = byslot[e['slot']]
        n = len(part['v']['p'])
        if o['nv'] != n:
            raise PatchError('%s: %d vertices in OBJ, %d native (topology change)' % (name, o['nv'], n))
        # faces must be the native triangle set (rotation/order/winding-agnostic)
        want = Counter(canon(t) for t in part_faces(data, raw_parts[e['slot']], part['batches']))
        got = Counter()
        for f in o['faces']:
            if len(f) != 3:
                raise PatchError('%s: non-triangle face (topology change)' % name)
            got[canon(tuple(c[0] - 1 - o['v0'] for c in f))] += 1
        if want != got:
            ws = Counter(tuple(sorted(t)) for t in want.elements())
            gs = Counter(tuple(sorted(t)) for t in got.elements())
            if ws != gs:
                raise PatchError('%s: face connectivity differs from native strips (topology change)' % name)
            rep['warnings'].append('%s: face winding/orientation differs; native strip winding kept' % name)
        # per-vertex uv / normal from face corners
        uvs, nrm = {}, {}
        for f in o['faces']:
            for (iv, it, inn) in f:
                j = iv - 1 - o['v0']
                if it and 'uv' in channels:
                    t = VT[it - 1]
                    t = (t[0], 1.0 - t[1])
                    if j in uvs and max(abs(t[0] - uvs[j][0]), abs(t[1] - uvs[j][1])) > 1e-5:
                        raise PatchError('%s: vertex %d has differing UVs per face (seam split = topology change)' % (name, j))
                    uvs[j] = t
                if inn and 'nrm' in channels:
                    nrm.setdefault(j, []).append(VN[inn - 1])
        pos = V[o['v0']:o['v0'] + n]
        if any(not math.isfinite(x) for p in pos for x in p):
            raise PatchError('%s: non-finite position' % name)
        rep['parts'] += 1
        rep['vertices'] += n
        for b in part['batches']:
            if not b['count']:
                continue
            allowed_b = _patch_batch(out, data, b, part, pos, uvs, nrm, channels, clamp, rep, name)
            allowed += allowed_b
    _update_bounds(out, data, rep)
    changed = [i for i in range(len(data)) if out[i] != data[i]]
    for i in changed:
        if not any(a <= i < z for a, z in allowed):
            raise PatchError('internal error: byte %#x changed outside patched slots' % i)
    rep['bytes_changed'] = len(changed)
    return bytes(out), rep


def _patch_batch(out, data, b, part, pos, uvs, nrm, channels, clamp, rep, name):
    n, vb = b['count'], b['vbase']
    v = part['v']
    spans = []
    for role, lanes, qkey, tgts in (('pos', 3, 'q199', {vb + j: pos[vb + j] for j in range(n)} if 'pos' in channels else {}),
                                    ('uv', 2, 'q198', uvs if 'uv' in channels and 'uv' in b else {})):
        if not tgts or role not in b:
            continue
        sl = b[role]
        old = v['p' if role == 'pos' else 'uv'][vb:vb + n]
        tol = POS_TOL if role == 'pos' else UV_TOL
        chg = [j for j in range(n) if (vb + j) in tgts and max(abs(tgts[vb + j][c] - old[j][c]) for c in range(lanes)) > tol]
        if not chg:
            continue
        if role == 'pos':
            rep['changed_vertices'] += len(chg)
        cs = set(chg)
        want = [tgts[vb + j] if j in cs else old[j] for j in range(n)]
        offs = list(struct.unpack_from('<%df' % lanes, data, b[qkey]))
        cols, new_off, new_row = [], list(offs), list(sl['row'])
        for c in range(lanes):
            us, no, nr = _fit_lane([w[c] for w in want], sl['row'][c], offs[c], sl['bits'], sl['shared'], clamp,
                                   '%s batch %s lane %s' % (name, role, ('xyz' if role == 'pos' else 'st')[c]))
            cols.append(us)
            new_off[c] = offs[c] if no is None else no
            new_row[c] = sl['row'][c] if nr is None else nr
        if new_off != offs:
            rep['refit_batches'] += 1
            struct.pack_into('<%df' % lanes, out, b[qkey], *new_off)
            spans.append((b[qkey], b[qkey] + 4 * lanes))
        if new_row != sl['row']:
            rep['rebased_batches'] += 1
            struct.pack_into('<%dI' % lanes, out, sl['row_off'], *new_row)
            spans.append((sl['row_off'], sl['row_off'] + 4 * lanes))
        full = new_off != offs or new_row != sl['row']
        for j in (range(n) if full else chg):
            struct.pack_into(UF[sl['bits']] % lanes, out, sl['off'] + j * sl['stride'], *(cols[c][j] for c in range(lanes)))
        spans.append((sl['off'], sl['off'] + n * sl['stride']))
        for j in chg:
            dec = [fbits(new_row[c] + cols[c][j]) + new_off[c] for c in range(lanes)]
            err = max(abs(dec[c] - want[j][c]) for c in range(lanes))
            rep['max_err'][role] = max(rep['max_err'][role], err)
            if err > 1.0001 * max(ulp_of(new_row[c]) for c in range(lanes)) and sl['bits'] < 32:
                rep['clamped'] += 1
                rep['out_of_range'].append(dict(part=name, vertex=vb + j, role=role))
    if 'nrm' in channels and 'nrm' in b and nrm:
        old = v['n'][vb:vb + n]
        for j in range(n):
            ns = nrm.get(vb + j)
            if not ns:
                continue
            avg = tuple(sum(x[c] for x in ns) / len(ns) for c in range(3))
            if max(abs(avg[c] - old[j][c]) for c in range(3)) <= NRM_TOL:
                continue
            ln = math.sqrt(sum(x * x for x in avg)) or 1.0
            avg = tuple(x / ln for x in avg)
            bts = bytes(min(255, max(0, round(avg[c] * 127.0 + 128))) for c in range(3))
            o = b['nrm']['off'] + j * 4
            out[o:o + 3] = bts
            rep['max_err']['nrm'] = max(rep['max_err']['nrm'], *(abs((bts[c] - 128) / 127.0 - avg[c]) for c in range(3)))
        spans.append((b['nrm']['off'], b['nrm']['off'] + n * 4))
    return spans


def _extent(parts_v):
    P = [p for pv in parts_v for p in pv]
    return ([min(p[k] for p in P) for k in range(3)], [max(p[k] for p in P) for k in range(3)]) if P else None


def _update_bounds(out, data, rep):
    """Grow header bbox/radius by exactly how far the decoded geometry grew (preserves retail slack)."""
    _, before = decode_member(data)
    _, after = decode_member(bytes(out))
    e0, e1 = _extent([p['v']['p'] for p in before]), _extent([p['v']['p'] for p in after])
    if not e0:
        return
    hdr = list(struct.unpack_from('<4f6f', data, 0x18))
    radius, centre, bb = hdr[0], hdr[1:4], hdr[4:10]   # file order minx,maxx,miny,maxy,minz,maxz
    new = list(bb)
    for k in range(3):
        if e1[0][k] < e0[0][k] - 1e-6:
            new[2 * k] = bb[2 * k] - (e0[0][k] - e1[0][k])
        if e1[1][k] > e0[1][k] + 1e-6:
            new[2 * k + 1] = bb[2 * k + 1] + (e1[1][k] - e0[1][k])
    r0 = max(math.dist(p, centre) for pv in before for p in pv['v']['p'])
    r1 = max(math.dist(p, centre) for pv in after for p in pv['v']['p'])
    nr = radius + (r1 - r0) if r1 > r0 + 1e-6 else radius
    if new != bb or nr != radius:
        struct.pack_into('<f', out, 0x18, nr)
        struct.pack_into('<6f', out, 0x28, *new)
        rep['bounds_updated'] = dict(radius=[radius, nr], bbox_old=bb, bbox_new=new)
    else:
        rep['bounds_updated'] = None
    # Radius/bbox match decoded vertex extents on all 490 retail MODL; no other bounds field is known.
    rep['warnings'].append('TJRM markers, part header words (+0x1C..+0x37) and any external track/sector '
                           'culling bounds are not recomputed')


# ----------------------------------------------------------------------------- archives

def list_models(archive):
    import modding_backend as mb
    from pathlib import Path
    return [e for e in mb.directory(Path(archive)) if e['type'] == 'MODL']


def read_member(archive, member):
    import modding_backend as mb
    from pathlib import Path
    data, entry = mb.payload(Path(archive), member)
    return data, entry


def patch_archive(archive, member, new_member, out_path):
    """Same-size in-place member patch + directory CRC (as save_single_archive)."""
    import modding_backend as mb
    from pathlib import Path
    src = Path(archive)
    original = src.read_bytes()
    entries = mb.directory(src)
    hit = [(i, e) for i, e in enumerate(entries) if e['name'] == member]
    if len(hit) != 1:
        raise PatchError('member not found: ' + member)
    i, e = hit[0]
    if len(new_member) != e['size']:
        raise PatchError('size changed; in-place archive patch requires identical length')
    arc = bytearray(original)
    arc[e['offset']:e['offset'] + len(new_member)] = new_member
    d_off = struct.unpack_from('<I', original, 20)[0]
    struct.pack_into('<I', arc, d_off + i * 68 + 48, eod_crc(new_member))
    Path(out_path).write_bytes(bytes(arc))
    return len(arc)


# ----------------------------------------------------------------------------- CLI

def _load(a):
    if a.archive:
        return read_member(a.archive, a.member)[0]
    return open(a.member, 'rb').read()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    for name in ('export', 'apply', 'apply-archive', 'list'):
        s = sub.add_parser(name)
        s.add_argument('--archive')
        if name != 'list':
            s.add_argument('--member', required=True, help='archive member name (or raw .mod path without --archive)')
        if name == 'export':
            s.add_argument('--obj', required=True)
            s.add_argument('--map', help='sidecar JSON (default OBJ path + .map.json)')
        elif name != 'list':
            s.add_argument('--obj', required=True)
            s.add_argument('--map', required=True)
            s.add_argument('--out', required=True)
            s.add_argument('--clamp', action='store_true')
            s.add_argument('--channels', default='pos,uv,nrm')
            s.add_argument('--report')
    a = ap.parse_args(argv)
    if a.cmd == 'list':
        for e in list_models(a.archive):
            print(e['name'], e['size'])
        return 0
    data = _load(a)
    if a.cmd == 'export':
        mp = export_model(data, a.obj, a.member, a.archive or '')
        json.dump(mp, open(a.map or a.obj + '.map.json', 'w'))
        print('exported %d parts, %d vertices' % (len(mp['parts']), sum(p['vertices'] for p in mp['parts'])))
        return 0
    mp = json.load(open(a.map))
    new, rep = patch_member(data, mp, a.obj, tuple(a.channels.split(',')), a.clamp)
    if a.cmd == 'apply':
        open(a.out, 'wb').write(new)
    else:
        patch_archive(a.archive, a.member, new, a.out)
    if a.report:
        json.dump(rep, open(a.report, 'w'), indent=1)
    print(json.dumps(rep))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except PatchError as e:
        sys.stderr.write('REJECTED: %s\n' % e)
        sys.exit(2)
