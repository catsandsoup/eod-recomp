#!/usr/bin/env python3
"""Phase A2 generators for NASCAR -> TDEOD car imports (pure Python, no numpy).

Everything here is derived from retail TDEOD data and the TDEOD VU1/EE code:

* MODL container read/write (part table, 50 runtime records, 4SPS parts, TJRM helpers).
* VIF batch walker that records where each UNPACK payload lives, so positions can be
  decoded and the per-vertex damage skin (UNPACK V4-8 @150) can be rewritten in place.
* Damage skin + Bone.mvl generator.  Verified facts (see A2_REPORT.md):
    - VU1 program fd0ce30a..., entry 0x2E0 (MSCAL 0x5C) reads the skin of vertex k from
      mem[150 + count-1-k]: the skin array is stored in REVERSE vertex order per batch.
    - skin = (bone_a, bone_b, w_a, w_b); displacement = w_a/256*D[a] + w_b/256*D[b], where
      D[i] = mem[143+i] is the bone table of the part's bonesurf (local index i =
      global bone - first_bone).  On Algar Low1, bone_a is the nearest Bone.mvl bone of the
      part's bonesurf for 96% of vertices, bone_b the 2nd nearest, and
      w_a = 254*d_b/(d_a+d_b) to within 2/254 (mean error 0.007).
    - Rigid vertices use (0, 0, 255, 0).
    - [bounds] min_g/max_g = bounding box of part-group g in Low1 (exact on Algar, 22/22).
    - bonesurf_K: first_bone/last_bone/group_mask; surf 2 always starts at 80, so each surf
      has <= 80 bones; unused slots are dummy bones at the origin.
    - link_center/travel in inches (x 0.0254 at load), dmg_dist in metres.
* Shadow volume meshes (shadow1-3.mod, part type 7, MSCAL 4, inward face normals).
"""

import math
import struct

EMPTY = 0xE3E3E3E3
INCH = 39.37007874


def f32(u):
    return struct.unpack('<f', struct.pack('<I', u & 0xFFFFFFFF))[0]


def fbits(f):
    return struct.unpack('<I', struct.pack('<f', f))[0]


# ----------------------------------------------------------------------------- MODL

def read_modl(data):
    h = struct.unpack_from('<6I', data, 0)
    n = h[0]
    offs = struct.unpack_from('<%dI' % n, data, 0x58)
    parts = []
    for o in offs:
        if data[o:o + 4] != b'4SPS':
            raise ValueError('part table entry %#x is not 4SPS' % o)
        size = struct.unpack_from('<I', data, o + 4)[0]
        parts.append({'hdr': bytearray(data[o:o + 0x38]), 'stream': bytes(data[o + 0x38:o + size]),
                      'trailer': bytes(data[o + size:o + size + 8]).ljust(8, b'\xe3')})
    tjrm = None
    if h[2]:
        t = struct.unpack_from('<I', data, h[3])[0]
        size = struct.unpack_from('<I', data, t + 4)[0]
        tjrm = bytes(data[t:t + size]).ljust(size, b'\0')
    return {'head': bytearray(data[:0x58]), 'parts': parts, 'tjrm': tjrm, 'nrec': h[4]}


def write_modl(m):
    parts, tjrm, nrec = m['parts'], m['tjrm'], m['nrec']
    n = len(parts)
    ns = (n + 3) // 4 * 4
    table_end = 0x58 + 4 * ns
    h3 = table_end
    h5 = table_end + 0x10 if tjrm else table_end
    pos = h5 + 4 * nrec
    rec0 = pos + ((8 - pos) % 16)
    recsize = 4 * ns
    part0 = rec0 + nrec * recsize
    out = bytearray(part0)
    head = bytearray(m['head'])
    struct.pack_into('<6I', head, 0, n, 0x58, 1 if tjrm else 0, h3, nrec, h5)
    out[:0x58] = head
    p = part0
    offs = []
    blobs = bytearray()
    for part in parts:
        if len(part['stream']) % 16:
            raise ValueError('part stream not quadword aligned')
        hdr = bytearray(part['hdr'])
        struct.pack_into('<I', hdr, 4, 0x38 + len(part['stream']))
        struct.pack_into('<I', hdr, 0x34, len(part['stream']) // 16)
        offs.append(p)
        blob = bytes(hdr) + part['stream'] + part['trailer'][:8].ljust(8, b'\xe3')
        blobs += blob
        p += len(blob)
    struct.pack_into('<%dI' % n, out, 0x58, *offs)
    for i in range(n, ns):
        struct.pack_into('<I', out, 0x58 + 4 * i, EMPTY)
    if tjrm:
        struct.pack_into('<4I', out, h3, p, EMPTY, EMPTY, EMPTY)
    struct.pack_into('<%dI' % nrec, out, h5, *[rec0 + k * recsize for k in range(nrec)])
    for a in range(pos, rec0, 4):
        struct.pack_into('<I', out, a, EMPTY)
    for k in range(nrec):
        r = rec0 + k * recsize
        for i in range(ns):
            struct.pack_into('<I', out, r + 4 * i, 0 if i < n else EMPTY)
    out += blobs
    if tjrm:
        out += tjrm
    struct.pack_into('<I', out, 0x40, len(out))
    for a in range(0x44, 0x58, 4):
        struct.pack_into('<I', out, a, EMPTY)
    return bytes(out)


def set_modl_bounds(m, pts):
    """Recompute radius/centre/bbox header floats from a point list."""
    lo = [min(q[i] for q in pts) for i in range(3)]
    hi = [max(q[i] for q in pts) for i in range(3)]
    c = [(lo[i] + hi[i]) / 2 for i in range(3)]
    r = max(math.dist(c, q) for q in pts)
    struct.pack_into('<4f', m['head'], 0x18, r, *c)
    struct.pack_into('<6f', m['head'], 0x28, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2])


def part_group(part):
    return struct.unpack_from('<I', part['hdr'], 0x18)[0]


def part_texture(part):
    return bytes(part['hdr'][8:24]).split(b'\0')[0].decode('latin1')


def part_type(part):
    return struct.unpack_from('<I', part['hdr'], 0x1C)[0]


# ----------------------------------------------------------------------------- TJRM

def tjrm_records(tjrm):
    n = struct.unpack_from('<I', tjrm, 8)[0]
    out = []
    for i in range(n):
        p = 12 + 80 * i
        name = tjrm[p:p + 32].split(b'\0')[0].decode('latin1')
        rot = struct.unpack_from('<9f', tjrm, p + 32)
        pos = struct.unpack_from('<3f', tjrm, p + 68)
        out.append((name, rot, pos))
    return out


def build_tjrm(records):
    body = bytearray()
    for name, rot, pos in records:
        nb = name.encode('latin1')[:31]
        body += nb + b'\0' * (32 - len(nb)) + struct.pack('<9f', *rot) + struct.pack('<3f', *pos)
    return b'TJRM' + struct.pack('<II', 12 + len(body), len(records)) + bytes(body)


IDENTITY = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)


# ----------------------------------------------------------------------------- VIF walk

def _unpack_bytes(cmd, num):
    vn, vl = (cmd >> 2) & 3, cmd & 3
    n = num or 256
    if vl == 3:
        return (n * 2 + 3) & ~3
    return (n * [4, 2, 1][vl] * (vn + 1) + 3) & ~3


def walk_vif(stream):
    """Return batches: {'count', 'unpacks': {addr: info}, 'q198', 'q199', 'mscal', 'mscal_off'}.
    info = {'cmd', 'num', 'off' (payload offset in stream), 'row', 'mode', 'mask', 'cl', 'wl'}."""
    batches = []
    cur = {}
    row, mode, mask, cl, wl = [0] * 4, 0, 0, 1, 1
    p = 0
    while p + 4 <= len(stream):
        w = struct.unpack_from('<I', stream, p)[0]
        cmd, num, imm = (w >> 24) & 0x7F, (w >> 16) & 0xFF, w & 0xFFFF
        if cmd >= 0x60:
            nb = _unpack_bytes(cmd, num)
            cur[imm & 0x3FF] = {'cmd': cmd, 'num': num or 256, 'off': p + 4, 'row': list(row), 'mode': mode,
                                'mask': mask, 'cl': cl, 'wl': wl, 'code_off': p}
            p += 4 + nb
            continue
        if cmd == 0x01:
            cl, wl = imm & 0xFF, (imm >> 8) & 0xFF
        elif cmd == 0x05:
            mode = imm & 3
        elif cmd == 0x20:
            mask = struct.unpack_from('<I', stream, p + 4)[0]
            p += 8
            continue
        elif cmd in (0x30, 0x31):
            if cmd == 0x30:
                row = list(struct.unpack_from('<4I', stream, p + 4))
            p += 20
            continue
        elif cmd in (0x14, 0x15, 0x17):
            c0 = cur.get(0)
            count = struct.unpack_from('<I', stream, c0['off'])[0] & 0x7FFF if c0 else 0
            u = cur.get(198)
            q = struct.unpack_from('<8I', stream, u['off']) if u else (0,) * 8
            batches.append({'count': count, 'count_word': struct.unpack_from('<I', stream, c0['off'])[0] if c0 else 0,
                            'unpacks': cur, 'q198': list(q[:4]), 'q199': list(q[4:]),
                            'mscal': imm, 'mscal_off': p})
            cur = {}
        elif cmd in (0x4A,):
            p += 4 + (num or 256) * 8
            continue
        elif cmd in (0x50, 0x51):
            p += 4 + imm * 16
            continue
        p += 4
    return batches


def batch_positions(stream, b):
    u = b['unpacks'][2]
    off = [f32(x) for x in b['q199'][:3]]
    out = []
    for i in range(b['count']):
        vals = struct.unpack_from('<3H', stream, u['off'] + 6 * i)
        out.append(tuple(f32((u['row'][c] + vals[c]) if u['mode'] == 1 else vals[c]) + off[c] for c in range(3)))
    return out


def part_points(part):
    pts = []
    for b in walk_vif(part['stream']):
        pts.extend(batch_positions(part['stream'], b))
    return pts


def set_batch_position(stream, b, i, xyz):
    """Re-encode vertex i of batch b at a new position (must stay inside the batch's row range)."""
    u = b['unpacks'][2]
    off = [f32(x) for x in b['q199'][:3]]
    vals = []
    for c in range(3):
        base = f32(u['row'][c])
        ulp = f32(u['row'][c] + 1) - base
        k = round((xyz[c] - off[c] - base) / ulp)
        if not 0 <= k <= 0xFFFF:
            raise ValueError('position outside the batch row range')
        vals.append(k)
    struct.pack_into('<3H', stream, u['off'] + 6 * i, *vals)


# ----------------------------------------------------------------------------- transcoding helpers

MSCAL_MAP = {0x2D: 0x5C, 0x26: 0x55}   # NASCAR VU1 entry -> TDEOD VU1 entry (type 3/4 -> 0x5C, type 1 -> 0x55)


# ----------------------------------------------------------------------------- skin

def compute_skin(p, bones):
    """bones: list of (local_index, xyz). Nearest two; weights by inverse-distance ratio."""
    best = sorted(((math.dist(p, q), li) for li, q in bones))[:2]
    (da, a), (db, b) = best
    if da + db <= 1e-9:
        return (a, b, 254, 0)
    wa = int(round(254.0 * db / (da + db)))
    wa = max(0, min(254, wa))
    return (a, b, wa, 254 - wa)


def write_part_skin(part, skin_fn):
    """skin_fn(position) -> (a, b, wa, wb) or int (V1-8 parts). Writes reversed per batch."""
    stream = bytearray(part['stream'])
    for b in walk_vif(stream):
        u = b['unpacks'].get(150)
        if not u:
            continue
        pts = batch_positions(stream, b)
        n = b['count']
        fmt = u['cmd'] & 0x6F
        for k, pt in enumerate(pts):
            s = skin_fn(pt)
            j = n - 1 - k
            if fmt == 0x6E:      # V4-8
                if isinstance(s, int):
                    s = (s, 0, 255, 0)
                struct.pack_into('4B', stream, u['off'] + 4 * j, *s)
            elif fmt == 0x62:    # V1-8 single bone (part type 4)
                if not isinstance(s, int):
                    s = s[0]
                struct.pack_into('B', stream, u['off'] + j, s)
            else:
                raise ValueError('unexpected skin unpack format %#x' % u['cmd'])
    part['stream'] = bytes(stream)


def read_part_skin(part):
    """[(position, skin tuple)] using the reversed per-batch layout."""
    out = []
    st = part['stream']
    for b in walk_vif(st):
        u = b['unpacks'].get(150)
        if not u:
            continue
        pts = batch_positions(st, b)
        n = b['count']
        for k, pt in enumerate(pts):
            j = n - 1 - k
            if (u['cmd'] & 0x6F) == 0x6E:
                out.append((pt, struct.unpack_from('4B', st, u['off'] + 4 * j)))
            else:
                out.append((pt, (st[u['off'] + j], 0, 255, 0)))
    return out


# ----------------------------------------------------------------------------- bones

def _cluster(points, cell):
    """Voxel clustering on x-folded points; returns representative points (on the mesh)."""
    cells = {}
    for p in points:
        q = (abs(p[0]), p[1], p[2])
        key = (int(math.floor(q[0] / cell + 0.5)), int(math.floor(q[1] / cell)), int(math.floor(q[2] / cell)))
        cells.setdefault(key, []).append(q)
    reps = []
    for key, qs in sorted(cells.items()):
        c = [sum(q[i] for q in qs) / len(qs) for i in range(3)]
        r = min(qs, key=lambda q: math.dist(q, c))
        reps.append((0.0 if key[0] == 0 else r[0], r[1], r[2]))
    return reps


def _mirror(reps):
    out = []
    for r in reps:
        if r[0] < 0.02:
            out.append((0.0, r[1], r[2]))
        else:
            out.append((-r[0], r[1], r[2]))
            out.append((r[0], r[1], r[2]))
    return out


def place_bones(points, max_bones=79):
    """Pick <= max_bones mirror-symmetric bone positions covering `points`."""
    lo, hi = 0.05, 2.0
    best = None
    for _ in range(40):
        mid = (lo + hi) / 2
        bones = _mirror(_cluster(points, mid))
        if len(bones) <= max_bones:
            best, hi = bones, mid
        else:
            lo = mid
    return best, hi


def bone_params(pos, zc, front_rear_z=1.72):
    """Travel/dmg_dist pattern of retail Bone.mvl (inches). The outward-facing limit is the
    small one (4 in minimum); inward/up travel is 30 in. dmg_dist 0.102 m beyond +-1.72 m."""
    x, y, z = (v * INCH for v in pos)
    end_zone = abs(pos[2] - zc) >= front_rear_z
    small_z = 8.0 if end_zone else 4.0
    tmin = [-30.0, -4.0, -30.0]
    tmax = [30.0, 30.0, 30.0]
    if x < -0.05:
        tmax[0] = 4.0
    else:
        tmin[0] = -4.0
    if pos[2] - zc > 0:
        tmin[2] = -small_z
    else:
        tmax[2] = small_z
    return {'dmg_dist': 0.102 if end_zone else 0.051, 'link_center': (x, y, z),
            'travel_min': tmin, 'travel_max': tmax, 'weakness': 1.5}


DUMMY_BONE = {'dmg_dist': 0.051, 'link_center': (0.0, 0.0, 0.0), 'travel_min': [-30.0, -4.0, -30.0],
              'travel_max': [29.8, 30.0, 8.2], 'weakness': 1.5}


def _fmt3(v):
    return '%.1f, %.1f, %.1f' % tuple(v)


def build_mvl_model(template, group_bounds, surfs, bones):
    """template: a parsed retail Bone.mvl (for vtables); group_bounds: {g: (lo, hi)} metres;
    surfs: [(first, last, mask)]; bones: list (index 1..N) of param dicts."""
    vt = template['sections'][0]['sec_vtable']
    secs = []
    bp = []
    for g in range(32):
        lo, hi = group_bounds.get(g, ((0.0, 0.0, 0.0), (0.0, 0.0, 0.0)))
        bp.append(('max_%d' % g, '%.2f, %.2f, %.2f' % tuple(hi)))
        bp.append(('min_%d' % g, '%.2f, %.2f, %.2f' % tuple(lo)))
    bp.sort(key=lambda kv: kv[0])
    secs.append({'name': 'bounds', 'parent': '', 'pairs': bp, 'sec_vtable': vt, 'tool_ptr': 0})
    for k, (first, last, mask) in enumerate(surfs, 1):
        secs.append({'name': 'bonesurf_%d' % k, 'parent': '', 'sec_vtable': vt, 'tool_ptr': 0,
                     'pairs': [('first_bone', str(first)), ('group_mask', '%X' % mask), ('last_bone', str(last))]})
    for i, b in enumerate(bones, 1):
        secs.append({'name': 'bone_%d' % i, 'parent': '', 'sec_vtable': vt, 'tool_ptr': 0, 'pairs': [
            ('dmg_dist', '%.3f' % b['dmg_dist']), ('link_center', _fmt3(b['link_center'])),
            ('travel_max', _fmt3(b['travel_max'])), ('travel_min', _fmt3(b['travel_min'])),
            ('weakness', '%.1f' % b['weakness'])]})
    model = dict(template)
    model['sections'] = secs
    model['unk1'] = len(secs)
    model['orig_sec_count'] = len(secs)
    return model


# ----------------------------------------------------------------------------- shadows

def convex_hull(points, eps=1e-7):
    """Incremental 3D convex hull. Returns (vertices, faces) with outward CCW faces."""
    pts = [tuple(p) for p in dict.fromkeys(tuple(round(c, 5) for c in p) for p in points)]
    if len(pts) < 4:
        raise ValueError('too few points for a hull')

    def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])

    def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])

    def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]

    # initial tetrahedron
    i0 = 0
    i1 = max(range(len(pts)), key=lambda i: math.dist(pts[i], pts[i0]))
    i2 = max(range(len(pts)), key=lambda i: sum(c * c for c in cross(sub(pts[i1], pts[i0]), sub(pts[i], pts[i0]))))
    nrm = cross(sub(pts[i1], pts[i0]), sub(pts[i2], pts[i0]))
    i3 = max(range(len(pts)), key=lambda i: abs(dot(nrm, sub(pts[i], pts[i0]))))
    faces = [(i0, i1, i2), (i0, i2, i3), (i0, i3, i1), (i1, i3, i2)]
    cen = [sum(pts[i][k] for i in (i0, i1, i2, i3)) / 4 for k in range(3)]

    def orient(f):
        a, b, c = f
        n = cross(sub(pts[b], pts[a]), sub(pts[c], pts[a]))
        return f if dot(n, sub(pts[a], cen)) > 0 else (a, c, b)

    faces = [orient(f) for f in faces]
    for i, p in enumerate(pts):
        if i in (i0, i1, i2, i3):
            continue
        vis = []
        for f in faces:
            a, b, c = f
            n = cross(sub(pts[b], pts[a]), sub(pts[c], pts[a]))
            ln = math.sqrt(dot(n, n)) or 1
            if dot(n, sub(p, pts[a])) / ln > eps:
                vis.append(f)
        if not vis:
            continue
        edges = {}
        for a, b, c in vis:
            for e in ((a, b), (b, c), (c, a)):
                edges[e] = edges.get(e, 0) + 1
        horizon = [e for e in edges if (e[1], e[0]) not in edges]
        vs = set(vis)
        faces = [f for f in faces if f not in vs] + [(e[0], e[1], i) for e in horizon]
    used = sorted({i for f in faces for i in f})
    remap = {o: n for n, o in enumerate(used)}
    return [pts[i] for i in used], [tuple(remap[i] for i in f) for f in faces]


def _qbase(lo, hi):
    """Pick a float row base 2^k (and offset) so [lo, hi] maps onto 16-bit increments."""
    span = max(hi - lo, 1e-6)
    k = math.ceil(math.log2(span * 128.0 + 1e-9))
    base = 2.0 ** max(k, -20)
    return base, lo - base


def encode_shadow_part(tris, first_q198_z, f30, tex=b'.tex'):
    """tris: list of ((p0,p1,p2), (n0,n1,n2)) with the per-vertex *stored* normals (retail stores
    the inward face normal; see shadow_volume_triangles).  Builds one type-7 shadow part as
    separate 3-vertex strips (ADC on the first two).  Like retail, only the first batch calls the
    shadow program with MSCAL 4; later batches continue it with MSCNT."""
    verts = []
    for t, ((a, b, c), (na, nb_, nc)) in enumerate(tris):
        # The shadow program flips the facing of the triangle drawn at every odd vertex index
        # (strip parity), so each 3-vertex strip sits at index 3t+2 and the odd ones are stored
        # with their first two vertices swapped.  Verified on retail shadow1/2: with this rule
        # all 64+28 solid triangles have a geometric normal exactly opposite their stored one.
        order = ((b, nb_), (a, na), (c, nc)) if t % 2 else ((a, na), (b, nb_), (c, nc))
        for k, (p, n) in enumerate(order):
            verts.append((p, n, k < 2))
    stream = bytearray()
    batches = [verts[i:i + 96] for i in range(0, len(verts), 96)]
    allp = [v[0] for v in verts]
    lo = [min(p[i] for p in allp) for i in range(3)]
    hi = [max(p[i] for p in allp) for i in range(3)]
    bases = [_qbase(lo[i], hi[i]) for i in range(3)]
    for bi, bv in enumerate(batches):
        n = len(bv)
        stream += struct.pack('<II', 0x60010000 | 0x8000 | 0x4000 | 0, 0x8000 | n)   # V1-32 @0 (flg|usn)
        stream += struct.pack('<I', 0x01000102)          # STCYCL cl=2 wl=1
        stream += struct.pack('<I', 0x05000001)          # STMOD offset
        stream += struct.pack('<II', 0x20000000, 0x40404040)
        stream += struct.pack('<I', 0x30000000) + struct.pack('<4I', *[fbits(b[0]) for b in bases], fbits(1.0))
        pos = bytearray()
        for p, _, _ in bv:
            vals = []
            for c in range(3):
                base, off = bases[c]
                ulp = f32(fbits(base) + 1) - base
                vals.append(max(0, min(0xFFFF, int(round((p[c] - off - base) / ulp)))))
            pos += struct.pack('<3H', *vals)
        while len(pos) % 4:
            pos += b'\0'
        stream += struct.pack('<I', 0x79000000 | (n << 16) | 0xC000 | 2) + pos   # V3-16 masked @2
        stream += struct.pack('<I', 0x30000000) + struct.pack('<4I', 0x47800000, 0x47800000, 0x47800000, 0x7F80)
        nb = bytearray()
        for _, nr, adc in bv:
            nb += bytes(max(0, min(255, int(round(nr[c] * 127.0)) + 128)) for c in range(3)) + bytes([0x80 if adc else 0])
        stream += struct.pack('<I', 0x6E000000 | (n << 16) | 0xC000 | 1) + nb    # V4-8 @1
        stream += struct.pack('<I', 0x05000000)          # STMOD normal
        stream += struct.pack('<I', 0x01000101)          # STCYCL 1/1
        q198 = (0, 0, first_q198_z if bi == 0 else 5, 0)
        q199 = (fbits(bases[0][1]), fbits(bases[1][1]), fbits(bases[2][1]), 0)
        stream += struct.pack('<I', 0x6C020000 | 0xC000 | 198) + struct.pack('<4I', *[x & 0xFFFFFFFF for x in q198]) \
            + struct.pack('<4I', *q199)
        stream += struct.pack('<I', 0x14000004 if bi == 0 else 0x17000000)   # MSCAL 4, then MSCNT
    while len(stream) % 16:
        stream += b'\0\0\0\0'
    hdr = bytearray(0x38)
    hdr[0:4] = b'4SPS'
    hdr[8:8 + len(tex)] = tex
    struct.pack_into('<7I', hdr, 0x18, 0, 0x001F0007, 0, 0, 0, len(verts) - 2, 0)
    struct.pack_into('<f', hdr, 0x30, f30)
    return {'hdr': hdr, 'stream': bytes(stream), 'trailer': b'\xe3' * 8}


def hull_triangles(points):
    verts, faces = convex_hull(points)
    out = []
    for a, b, c in faces:
        pa, pb, pc = verts[a], verts[b], verts[c]
        e1 = [pb[i] - pa[i] for i in range(3)]
        e2 = [pc[i] - pa[i] for i in range(3)]
        n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
        ln = math.sqrt(sum(x * x for x in n))
        if ln < 1e-9:
            continue
        out.append(((pa, pb, pc), tuple(x / ln for x in n)))
    return out


def shadow_volume_triangles(tris):
    """Turn closed-hull triangles ((a,b,c), outward normal) into a TDEOD shadow volume.

    The shadow program (VU1 0x0020, config/vu/vu1_eb9855205ae4523b.bin) does not project a mesh:
    it pushes every vertex whose *stored normal* faces the light (n.L > 0) along the light vector
    and draws what is left.  A closed convex hull therefore tears apart as soon as whole faces are
    pushed away: the faces facing the light are flung to the horizon as detached triangles, which
    is the large dark triangle fans seen on the ground beside and behind the Cup car.  A volume
    only closes if every edge between two faces carries a *fin*, a quad that is degenerate in the
    rest pose and whose two vertex pairs carry the normals of the two adjacent faces, so that at
    a silhouette edge one pair is pushed out and the fin stretches into the volume wall.  That is
    what the retail meshes are (zero-area stitching triangles between the face triangles).

    Winding follows the retail meshes: faces keep the outward counter-clockwise order with the
    inward normal stored on all three vertices; a fin runs q1,p1,p2,q2 where face 1 contains the
    directed edge p->q (the quad closes the surface between the two faces).
    Returns [((p0,p1,p2), (n0,n1,n2)), ...]."""
    out = []
    edges = {}
    for (a, b, c), n in tris:
        inward = tuple(-x for x in n)
        out.append(((a, b, c), (inward, inward, inward)))
        for e in ((a, b), (b, c), (c, a)):
            edges[e] = inward
    for (p, q), n1 in sorted(edges.items()):
        n2 = edges.get((q, p))
        if n2 is None or (p, q) > (q, p):
            continue
        out.append(((q, p, p), (n1, n1, n2)))        # q1 p1 p2
        out.append(((q, p, q), (n1, n2, n2)))        # q1 p2 q2
    return out
