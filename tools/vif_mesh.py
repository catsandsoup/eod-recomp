#!/usr/bin/env python3
"""MODL v9 (Monster Games PS2 engine: TDEOD 2004 / NASCAR Dirt to Daytona 2002)
VIF geometry decoder + OBJ/MTL exporter + tiny software renderer.

Pure Python 3 (no numpy). Run with `python3 -I` on untrusted disc data.

File layout (both games)
------------------------
0x00 u32 sub_version          0x04 u32 header size (0x58)
0x08 u32 1                    0x0C u32 file offset of the part-table slot that holds the TJRM chunk
0x10 u32 runtime record count (50)   0x14 u32 offset of runtime record table (0xE8)
0x18 f32 radius, f32 centre[3], f32 bbox min[3]?? (stored as minx,maxx,miny,maxy,minz,maxz)
0x40 u32 file size            0x44.. 0xE3 padding
0x58 u32 part offsets[(hdr[0x14]-0x58)/4] (36 slots, 0xE3E3E3E3 = empty)
hdr[0x14] u32 offsets[50] -> 0x80-byte runtime records (zero in files)

4SPS part ("SPS4"):
+0x00 '4SPS'  +0x04 u32 size  +0x08 char texture[16]  +0x18 u32 group
      (low 16 bits = damage/visibility group 0..31, bit 16 = body panel that uses the
       paintable base texture, bit 18 seen on NASCAR SPlate)
+0x1C..+0x37 unknown (counts / a float), +0x38 VIF stream to part end.

VIF stream, per batch (one VU1 call), retail TDEOD MSCAL 0x5C, NASCAR MSCAL 0x2D:
  [UNPACK V1-32 @0]    vertex count | 0x8000 (written before the batch's MSCAL)
  STCYCL cl=3 wl=1, STMOD offset, STMASK 0x40404040, STROW (256,256,1024,1)
  UNPACK V3-16 usn @2  -> position:  f32(rowbits + u16) per axis  (stride 3 qwords)
  STROW (65536,65536,65536,0x7F80)
  UNPACK V4-8  usn @1  -> normal xyz (u8, 128 = 0) + w bit7 = ADC (no-kick, strip restart)
       (NASCAR: V4-5 @1, 5-bit per channel, bit15 = ADC)
  STMASK 0x50505050, STROW (2^k,16,1,0x8000)
  UNPACK V2-16 usn @3  -> texcoord st: f32(rowbits + u16)
  STROW (0,0,32768,32768), STCYCL 1/1
  UNPACK V4-8 usn @150 -> per-vertex skin: bone_a, bone_b, weight_a, weight_b (wa+wb ~ 254)
  STMOD normal
  UNPACK V4-32 @198    -> qword198 = (st offset s, t offset, flags, 1)
                          qword199 = (position offset x, y, z, 0)
  MSCAL / MSCNT
The VU1 program (config/vu/vu1_fd0ce30ab0ec263c.bin @0x388) does
  pos = mem[2+3i].xyz + mem[199].xyz ; st = mem[3+3i].xy + mem[198].xy
then the model matrix. So local position = row_float + offset (metres, +Y up, +Z forward).
Primitive: triangle strip; a vertex with ADC set does not emit a triangle.
"""

import argparse
import json
import math
import os
import struct
import sys
import zlib

EMPTY = 0xE3E3E3E3


def f32(u):
    return struct.unpack('<f', struct.pack('<I', u & 0xFFFFFFFF))[0]


# --------------------------------------------------------------------------- header

def parse_modl(data):
    if data[:4] == b'MODL':
        raise ValueError('RST0-wrapped MODL not expected; pass the raw .mod')
    sub_ver, hdr_size, one, tjrm_slot_ptr, nrec, rec_tbl = struct.unpack_from('<6I', data, 0)
    radius = struct.unpack_from('<f', data, 0x18)[0]
    center = struct.unpack_from('<3f', data, 0x1C)
    b = struct.unpack_from('<6f', data, 0x28)
    bbox = {'min': (b[0], b[2], b[4]), 'max': (b[1], b[3], b[5])}
    nslots = (rec_tbl - 0x58) // 4
    offs = struct.unpack_from('<%dI' % nslots, data, 0x58)
    parts, markers = [], []
    for slot, o in enumerate(offs):
        if o == EMPTY or o + 8 > len(data):
            continue
        magic = data[o:o + 4]
        size = struct.unpack_from('<I', data, o + 4)[0]
        if magic == b'4SPS':
            tex = data[o + 8:o + 24].split(b'\0')[0].decode('latin1')
            group = struct.unpack_from('<I', data, o + 24)[0]
            extra = struct.unpack_from('<7I', data, o + 0x1C)
            parts.append({'slot': slot, 'offset': o, 'size': size, 'texture': tex,
                          'group': group, 'hdr_extra': [hex(x) for x in extra]})
        elif magic == b'TJRM':
            markers = parse_tjrm(data, o)
    return {'sub_version': sub_ver, 'radius': radius, 'center': center, 'bbox': bbox,
            'parts': parts, 'markers': markers}


def parse_tjrm(data, o):
    """TJRM: u32 count, then count x 80 bytes: name[32], 3x3 matrix, position."""
    n = struct.unpack_from('<I', data, o + 8)[0]
    out = []
    p = o + 12
    for _ in range(n):
        if p + 80 > len(data):
            break
        name = data[p:p + 32].split(b'\0')[0].decode('latin1')
        m = struct.unpack_from('<9f', data, p + 32)
        pos = struct.unpack_from('<3f', data, p + 68)
        out.append({'name': name, 'rot': m, 'pos': pos})
        p += 80
    return out


# --------------------------------------------------------------------------- VIF

class VifState:
    def __init__(self):
        self.cl, self.wl = 1, 1
        self.mode = 0
        self.mask = 0
        self.row = [0, 0, 0, 0]
        self.col = [0, 0, 0, 0]
        self.mem = {}          # addr -> [u32 x4] (TOPS-relative; reset per batch)
        self.raw = {}          # addr -> raw unpacked ints before row/mask (for byte fields)
        self.mscal = []


def _elements(data, p, vn, vl, n, usn):
    """Yield per-element lists of (value_int) for UNPACK formats."""
    comps = vn + 1
    out = []
    if vl == 3:  # V4-5 (only valid with vn==3)
        for i in range(n):
            v = struct.unpack_from('<H', data, p + 2 * i)[0]
            out.append([(v & 0x1F) << 3, ((v >> 5) & 0x1F) << 3, ((v >> 10) & 0x1F) << 3, (v >> 15) << 7])
        return out
    size = [4, 2, 1][vl]
    fmt = {4: 'I', 2: 'H', 1: 'B'}[size]
    sfmt = {4: 'i', 2: 'h', 1: 'b'}[size]
    for i in range(n):
        vals = []
        for c in range(comps):
            off = p + (i * comps + c) * size
            v = struct.unpack_from('<' + (fmt if usn else sfmt), data, off)[0]
            vals.append(v & 0xFFFFFFFF)
        out.append(vals)
    return out


def run_vif(data, start, end, on_batch):
    st = VifState()
    p = start
    while p + 4 <= end:
        w = struct.unpack_from('<I', data, p)[0]
        cmd = (w >> 24) & 0x7F
        num = (w >> 16) & 0xFF
        imm = w & 0xFFFF
        p += 4
        if cmd >= 0x60:
            vn, vl = (cmd >> 2) & 3, cmd & 3
            masked = (cmd >> 4) & 1
            n = num or 256
            bpe = 2 if vl == 3 else [4, 2, 1][vl] * (vn + 1)
            nbytes = (n * bpe + 3) & ~3
            usn = (imm >> 14) & 1
            addr = imm & 0x3FF
            elems = _elements(data, p, vn, vl, n, usn)
            for i, e in enumerate(elems):
                if st.wl <= st.cl:
                    a = addr + (i // st.wl) * st.cl + (i % st.wl)
                    cyc = i % st.wl
                else:
                    raise NotImplementedError('fill-mode STCYCL')
                q = list(st.mem.get(a, [0, 0, 0, 0]))
                rawq = list(st.raw.get(a, [None] * 4))
                for c in range(4):
                    m = (st.mask >> (min(cyc, 3) * 8 + c * 2)) & 3 if masked else 0
                    if c >= len(e):
                        # components not present in data: PS2 leaves them = row when masked,
                        # otherwise undefined. Treat as row.
                        m = 1 if masked else m
                        if not masked:
                            continue
                    if m == 0:
                        v = e[c]
                        rawq[c] = v
                        if st.mode == 1:
                            v = (v + st.row[c]) & 0xFFFFFFFF
                        elif st.mode == 2:
                            v = (v + st.row[c]) & 0xFFFFFFFF
                            st.row[c] = v
                        q[c] = v
                    elif m == 1:
                        q[c] = st.row[c]
                    elif m == 2:
                        q[c] = st.col[min(cyc, 3)]
                    # m==3 write-protect
                st.mem[a] = q
                st.raw[a] = rawq
            p += nbytes
        elif cmd == 0x00:   # NOP
            pass
        elif cmd == 0x01:   # STCYCL
            st.cl, st.wl = imm & 0xFF, (imm >> 8) & 0xFF
        elif cmd == 0x05:   # STMOD
            st.mode = imm & 3
        elif cmd == 0x20:   # STMASK
            st.mask = struct.unpack_from('<I', data, p)[0]
            p += 4
        elif cmd == 0x30:
            st.row = list(struct.unpack_from('<4I', data, p))
            p += 16
        elif cmd == 0x31:
            st.col = list(struct.unpack_from('<4I', data, p))
            p += 16
        elif cmd in (0x14, 0x15, 0x17):  # MSCAL / MSCALF / MSCNT
            st.mscal.append(imm if cmd != 0x17 else st.mscal[-1] if st.mscal else -1)
            on_batch(st)
            # next batch goes to the other double buffer: keep only addr 0 written after
            st.mem, st.raw = {}, {}
        elif cmd in (0x02, 0x03, 0x04, 0x06, 0x07, 0x10, 0x11, 0x13):
            pass
        elif cmd == 0x4A:  # MPG
            p += (num or 256) * 8
        elif cmd in (0x50, 0x51):
            p += imm * 16
        else:
            raise ValueError('unknown VIF cmd %02x at %x' % (cmd, p - 4))
    return st


def decode_part(data, part):
    o, size = part['offset'], part['size']
    batches = []

    def on_batch(st):
        mem, raw = st.mem, st.raw
        if 0 not in mem:
            return
        count = mem[0][0] & 0x7FFF
        q198 = mem.get(198, [0, 0, 0, 0])
        q199 = mem.get(199, [0, 0, 0, 0])
        soff, toff = f32(q198[0]), f32(q198[1])
        pox, poy, poz = f32(q199[0]), f32(q199[1]), f32(q199[2])
        verts = []
        for i in range(count):
            nq = raw.get(1 + 3 * i)
            pq = mem.get(2 + 3 * i)
            tq = mem.get(3 + 3 * i)
            bq = raw.get(150 + i)
            if pq is None:
                break
            pos = (f32(pq[0]) + pox, f32(pq[1]) + poy, f32(pq[2]) + poz)
            st_ = (f32(tq[0]) + soff, f32(tq[1]) + toff) if tq else (0.0, 0.0)
            if nq is not None and nq[0] is not None:
                nrm = tuple((nq[c] - 128) / 127.0 for c in range(3))
                adc = bool(nq[3] & 0x80)
            else:
                nrm, adc = (0.0, 1.0, 0.0), False
            skin = tuple(bq) if bq is not None and bq[0] is not None else None
            verts.append({'p': pos, 'uv': st_, 'n': nrm, 'adc': adc, 'skin': skin})
        batches.append({'mscal': st.mscal[-1], 'verts': verts, 'q198_zw': (q198[2], q198[3])})

    run_vif(data, o + 0x38, o + size, on_batch)
    return batches


def strip_triangles(verts):
    """Triangle strip with ADC: vertex i (>=2) emits (i-2,i-1,i) unless ADC.
    Winding alternates with the parity of the position inside the strip."""
    tris = []
    start = 0
    for i, v in enumerate(verts):
        if v['adc'] and (i == 0 or not verts[i - 1]['adc']):
            start = i            # first vertex of a new strip (strips open with 2 ADC verts)
        if i >= 2 and not v['adc']:
            a, b, c = i - 2, i - 1, i
            if ((i - start) % 2) == 0:
                tris.append((a, b, c))
            else:
                tris.append((b, a, c))
    return tris


def fix_winding(verts, tris):
    """Orient each triangle so its geometric normal agrees with the stored vertex normals."""
    out = []
    fix_winding.flips = getattr(fix_winding, 'flips', 0)
    fix_winding.total = getattr(fix_winding, 'total', 0)
    for (a, b, c) in tris:
        fix_winding.total += 1
        pa, pb, pc = verts[a]['p'], verts[b]['p'], verts[c]['p']
        e1 = [pb[k] - pa[k] for k in range(3)]
        e2 = [pc[k] - pa[k] for k in range(3)]
        n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
        vn = [verts[a]['n'][k] + verts[b]['n'][k] + verts[c]['n'][k] for k in range(3)]
        if n[0] * vn[0] + n[1] * vn[1] + n[2] * vn[2] < 0:
            fix_winding.flips += 1
            out.append((a, c, b))
        else:
            out.append((a, b, c))
    return out


def load_mesh(path):
    data = open(path, 'rb').read()
    m = parse_modl(data)
    for part in m['parts']:
        batches = decode_part(data, part)
        verts, tris = [], []
        for bt in batches:
            base = len(verts)
            t = strip_triangles(bt['verts'])
            t = fix_winding(bt['verts'], t)
            # drop degenerate (repeated-index / zero-area) triangles
            for (a, b, c) in t:
                pa, pb, pc = bt['verts'][a]['p'], bt['verts'][b]['p'], bt['verts'][c]['p']
                if pa == pb or pb == pc or pa == pc:
                    continue
                tris.append((a + base, b + base, c + base))
            verts.extend(bt['verts'])
        part['verts'] = verts
        part['tris'] = tris
        part['mscal'] = sorted(set(b['mscal'] for b in batches))
        part['batches'] = len(batches)
    return m


# --------------------------------------------------------------------------- textures

def load_texture(name, search_dirs, tools_dir=None):
    """Return (w, h, rgba bytes) or None. Uses fork tools/tex.py decompile_tex (reads
    palette offset at +0x18 and mip0 offset at +0x1C, so both the NASCAR 0x60 header and
    the TDEOD 0x80 header work)."""
    if not name or name == '-':
        return None
    cand = None
    lname = name.lower()
    for d in search_dirs:
        if not os.path.isdir(d):
            continue
        for fn in os.listdir(d):
            if fn.lower() == lname:
                cand = os.path.join(d, fn)
                break
        if cand:
            break
    if not cand:
        return None
    if tools_dir and tools_dir not in sys.path:
        sys.path.insert(0, tools_dir)
    try:
        from tex import decompile_tex  # noqa
        w, h, rgba, meta = decompile_tex(open(cand, 'rb').read())
        return (w, h, rgba)
    except Exception as e:  # unsupported format (4-bit etc.)
        sys.stderr.write('texture %s: %s\n' % (cand, e))
        return None


def write_png(fn, w, h, rgba):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw.extend(rgba[y * w * 4:(y + 1) * w * 4])
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + \
        chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b'')
    open(fn, 'wb').write(png)


# --------------------------------------------------------------------------- OBJ

def export_obj(mesh, obj_path, tex_pngs=None, include_groups=None):
    tex_pngs = tex_pngs or {}
    base = os.path.splitext(os.path.basename(obj_path))[0]
    mtl_path = os.path.splitext(obj_path)[0] + '.mtl'
    lines = ['# vif_mesh.py export', 'mtllib %s' % os.path.basename(mtl_path)]
    mats = {}
    vbase = 1
    for part in mesh['parts']:
        if include_groups is not None and (part['group'] & 0xFFFF) not in include_groups:
            continue
        tex = part['texture'] or 'none'
        mat = os.path.splitext(tex)[0] or 'none'
        mats[mat] = tex
        lines.append('o slot%02d_g%d_%s%s' % (part['slot'], part['group'] & 0xFFFF, mat,
                                              '_body' if part['group'] & 0x10000 else ''))
        lines.append('usemtl %s' % mat)
        for v in part['verts']:
            lines.append('v %.5f %.5f %.5f' % v['p'])
        for v in part['verts']:
            lines.append('vt %.5f %.5f' % (v['uv'][0], 1.0 - v['uv'][1]))
        for v in part['verts']:
            lines.append('vn %.4f %.4f %.4f' % v['n'])
        for (a, b, c) in part['tris']:
            ia, ib, ic = a + vbase, b + vbase, c + vbase
            lines.append('f %d/%d/%d %d/%d/%d %d/%d/%d' % (ia, ia, ia, ib, ib, ib, ic, ic, ic))
        vbase += len(part['verts'])
    open(obj_path, 'w').write('\n'.join(lines) + '\n')
    ml = []
    for mat, tex in mats.items():
        ml += ['newmtl %s' % mat, 'Kd 1 1 1']
        if mat in tex_pngs:
            ml.append('map_Kd %s' % tex_pngs[mat])
    open(mtl_path, 'w').write('\n'.join(ml) + '\n')


# --------------------------------------------------------------------------- renderer

def render(mesh, out_png, textures=None, width=640, height=420, yaw_deg=35.0, pitch_deg=20.0,
           exclude_groups=(), bg=(235, 235, 240), colour_by=None, title=None):
    """Tiny z-buffered perspective rasterizer, Lambert shading, nearest texel, wrap UV."""
    textures = textures or {}
    yaw, pitch = math.radians(yaw_deg), math.radians(pitch_deg)
    bb = mesh['bbox']
    cx = (bb['min'][0] + bb['max'][0]) / 2
    cy = (bb['min'][1] + bb['max'][1]) / 2
    cz = (bb['min'][2] + bb['max'][2]) / 2
    ext = max(bb['max'][k] - bb['min'][k] for k in range(3))
    dist = ext * 1.9
    cyaw, syaw, cp, sp = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch)
    f = height * 2.1
    light = (0.4, 0.8, 0.45)
    ll = math.sqrt(sum(x * x for x in light))
    light = tuple(x / ll for x in light)

    def xf(p):
        x, y, z = p[0] - cx, p[1] - cy, p[2] - cz
        # yaw about Y, then pitch about X; camera looks down -Z from +Z side
        x1 = cyaw * x + syaw * z
        z1 = -syaw * x + cyaw * z
        y2 = cp * y - sp * z1
        z2 = sp * y + cp * z1
        zc = dist - z2
        return (width / 2 + f * x1 / zc, height / 2 - f * y2 / zc, zc)

    zbuf = [1e30] * (width * height)
    img = bytearray(bytes(bg) + b'\xff') * (width * height)
    palette = [(220, 80, 60), (70, 140, 220), (90, 180, 90), (230, 190, 60), (160, 90, 200),
               (60, 190, 190), (230, 120, 170), (140, 140, 140)]
    for part in mesh['parts']:
        if (part['group'] & 0xFFFF) in exclude_groups:
            continue
        mat = os.path.splitext(part['texture'])[0].lower()
        tex = textures.get(mat)
        P = [xf(v['p']) for v in part['verts']]
        V = part['verts']
        for (a, b, c) in part['tris']:
            pa, pb, pc = P[a], P[b], P[c]
            if min(pa[2], pb[2], pc[2]) <= 0.05:
                continue
            area = (pb[0] - pa[0]) * (pc[1] - pa[1]) - (pc[0] - pa[0]) * (pb[1] - pa[1])
            if abs(area) < 1e-9:
                continue
            # flat shading from geometry normal (two-sided)
            A, B, C = V[a]['p'], V[b]['p'], V[c]['p']
            e1 = [B[k] - A[k] for k in range(3)]
            e2 = [C[k] - A[k] for k in range(3)]
            n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
            nl = math.sqrt(n[0] ** 2 + n[1] ** 2 + n[2] ** 2) or 1
            lam = abs(n[0] * light[0] + n[1] * light[1] + n[2] * light[2]) / nl
            shade = 0.35 + 0.65 * lam
            if colour_by is not None:
                key = colour_by(part)
                base_col = palette[key % len(palette)]
            else:
                base_col = (200, 200, 200)
            minx = max(int(min(pa[0], pb[0], pc[0])), 0)
            maxx = min(int(max(pa[0], pb[0], pc[0])) + 1, width - 1)
            miny = max(int(min(pa[1], pb[1], pc[1])), 0)
            maxy = min(int(max(pa[1], pb[1], pc[1])) + 1, height - 1)
            if minx > maxx or miny > maxy:
                continue
            ua, ub, uc = V[a]['uv'], V[b]['uv'], V[c]['uv']
            iza, izb, izc = 1 / pa[2], 1 / pb[2], 1 / pc[2]
            inv = 1.0 / area
            for y in range(miny, maxy + 1):
                py = y + 0.5
                for x in range(minx, maxx + 1):
                    px = x + 0.5
                    w0 = ((pb[0] - px) * (pc[1] - py) - (pc[0] - px) * (pb[1] - py)) * inv
                    w1 = ((pc[0] - px) * (pa[1] - py) - (pa[0] - px) * (pc[1] - py)) * inv
                    w2 = 1 - w0 - w1
                    if w0 < 0 or w1 < 0 or w2 < 0:
                        continue
                    iz = w0 * iza + w1 * izb + w2 * izc
                    z = 1 / iz
                    idx = y * width + x
                    if z >= zbuf[idx]:
                        continue
                    if tex is not None and colour_by is None:
                        tw, th, trgba = tex
                        u = (w0 * ua[0] * iza + w1 * ub[0] * izb + w2 * uc[0] * izc) * z
                        v = (w0 * ua[1] * iza + w1 * ub[1] * izb + w2 * uc[1] * izc) * z
                        tx = int(u * tw) % tw
                        ty = int(v * th) % th
                        to = (ty * tw + tx) * 4
                        if trgba[to + 3] < 8:
                            continue
                        col = (trgba[to], trgba[to + 1], trgba[to + 2])
                    else:
                        col = base_col
                    zbuf[idx] = z
                    o = idx * 4
                    img[o] = min(255, int(col[0] * shade))
                    img[o + 1] = min(255, int(col[1] * shade))
                    img[o + 2] = min(255, int(col[2] * shade))
    write_png(out_png, width, height, img)


# --------------------------------------------------------------------------- CLI

def summary(mesh):
    rows = []
    for p in mesh['parts']:
        rows.append({'slot': p['slot'], 'texture': p['texture'], 'group': p['group'] & 0xFFFF,
                     'body': bool(p['group'] & 0x10000), 'group_raw': hex(p['group']),
                     'verts': len(p['verts']), 'tris': len(p['tris']), 'batches': p['batches'],
                     'mscal': p['mscal']})
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('mod')
    ap.add_argument('--obj', help='write OBJ (+MTL next to it)')
    ap.add_argument('--png', help='write a 3/4 software render')
    ap.add_argument('--png-groups', help='write a render coloured by group id')
    ap.add_argument('--tex-dir', action='append', default=[], help='dirs searched for textures')
    ap.add_argument('--tex-map', action='append', default=[],
                    help='NAME=FILE: substitute texture NAME (e.g. algar.tex=.../805.tex)')
    ap.add_argument('--tools', help='fork tools dir (for tex.py)')
    ap.add_argument('--json', help='write part summary JSON')
    ap.add_argument('--yaw', type=float, default=35.0)
    ap.add_argument('--pitch', type=float, default=20.0)
    ap.add_argument('--exclude-groups', default='')
    a = ap.parse_args()
    mesh = load_mesh(a.mod)
    for r in summary(mesh):
        print(r)
    textures, pngs = {}, {}
    if a.obj or a.png:
        names = sorted(set(p['texture'] for p in mesh['parts']))
        remap = dict(kv.split('=', 1) for kv in a.tex_map)
        remap = {k.lower(): v for k, v in remap.items()}
        for n in names:
            key = os.path.splitext(n)[0].lower()
            t = None
            if n.lower() in remap:
                src = remap[n.lower()]
                t = load_texture(os.path.basename(src), [os.path.dirname(src)], a.tools)
            else:
                t = load_texture(n, a.tex_dir, a.tools)
            if t:
                textures[key] = t
                if a.obj:
                    pfn = os.path.join(os.path.dirname(os.path.abspath(a.obj)), 'tex_%s.png' % key)
                    write_png(pfn, *t)
                    pngs[os.path.splitext(n)[0]] = os.path.basename(pfn)
    if a.obj:
        export_obj(mesh, a.obj, pngs)
    ex = set(int(x) for x in a.exclude_groups.split(',') if x)
    if a.png:
        render(mesh, a.png, textures, yaw_deg=a.yaw, pitch_deg=a.pitch, exclude_groups=ex)
    if a.png_groups:
        render(mesh, a.png_groups, None, yaw_deg=a.yaw, pitch_deg=a.pitch, exclude_groups=ex,
               colour_by=lambda p: p['group'] & 0xFFFF)
    if a.json:
        json.dump({'bbox': mesh['bbox'], 'parts': summary(mesh), 'markers': mesh['markers']},
                  open(a.json, 'w'), indent=1)


if __name__ == '__main__':
    main()
