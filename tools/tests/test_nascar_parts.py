"""Detachable-panel split (nascar_parts) and shadow-volume (nascar_gen) contracts.

Synthetic fixtures only: no owner-supplied disc data is needed.
"""
import math
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import nascar_gen as gen
import nascar_parts as parts

ROW_POS = (0x43800000, 0x43800000, 0x44800000, 0x3F800000)     # 256, 256, 1024, 1
ROW_UV = (0x43000000, 0x43000000, 0x3F800000, 0x00008000)
OFFSET = (-257.0, -257.0, -1028.0)


def _u32(*v):
    return struct.pack('<%dI' % len(v), *v)


def synth_part(verts, group=9, tex=b'cupcar.tex'):
    """One skinned TDEOD body batch. verts: [(pos, adc)]; normals fixed, uv from x/z."""
    n = len(verts)
    pos = bytearray()
    for p, _ in verts:
        for c in range(3):
            base = gen.f32(ROW_POS[c])
            ulp = gen.f32(ROW_POS[c] + 1) - base
            pos += struct.pack('<H', round((p[c] - OFFSET[c] - base) / ulp))
    while len(pos) % 4:
        pos += b'\0'
    nrm = b''.join(bytes((128, 255, 128, 0x80 if a else 0)) for _, a in verts)
    uv = b''.join(struct.pack('<2H', int(max(0, min(1, p[0] * .25 + .5)) * 60000),
                              int(max(0, min(1, p[2] * .1 + .5)) * 60000)) for p, _ in verts)
    s = bytearray()
    s += _u32(0x60010000 | 0xC000, 0x8000 | n)
    s += _u32(0x01000103, 0x05000001, 0x20000000, 0x40404040, 0x30000000, *ROW_POS)
    s += _u32(0x79000000 | (n << 16) | 0xC002) + bytes(pos)
    s += _u32(0x30000000, 0x47800000, 0x47800000, 0x47800000, 0x7F80)
    s += _u32(0x6E000000 | (n << 16) | 0xC001) + nrm
    s += _u32(0x20000000, 0x50505050, 0x30000000, *ROW_UV)
    s += _u32(0x75000000 | (n << 16) | 0xC003) + uv
    s += _u32(0x30000000, 0, 0, 0x47000000, 0x47000000, 0x01000101)
    s += _u32(0x6E000000 | (n << 16) | 0xC096) + bytes(4 * n)
    s += _u32(0x05000000, 0x6C020000 | 0xC0C6, 0xC3000000, 0xC3000000, 0xFFFFFC1D, 0)
    s += _u32(*[gen.fbits(v) for v in OFFSET], 0)
    s += _u32(0x14000000 | 0x5C)
    while len(s) % 16:
        s += b'\0\0\0\0'
    hdr = bytearray(0x38)
    hdr[0:4] = b'4SPS'
    hdr[8:8 + len(tex)] = tex
    struct.pack_into('<I', hdr, 0x18, 0x10000 | group)
    struct.pack_into('<I', hdr, 0x1C, 0x1F0003)
    return {'hdr': hdr, 'stream': bytes(s), 'trailer': b'\xe3' * 8}


def tri_area(a, b, c):
    e1 = [b[k] - a[k] for k in range(3)]
    e2 = [c[k] - a[k] for k in range(3)]
    n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
    return math.sqrt(sum(x * x for x in n)) / 2


def decoded_triangles(part):
    out = []
    for bd in parts.decode_batches(part):
        for i0, i1, i2, _ in bd['tris']:
            out.append(tuple(bd['verts'][i]['p'] for i in (i0, i1, i2)))
    return out


class ClipAndClassifyTests(unittest.TestCase):
    def test_clip_keeps_area_and_interpolates(self):
        mk = lambda p, u: dict(p=p, n=(128.0, 255.0, 128.0), uv=u, adc=False)
        tri = [mk((1, 0, -1), (0, 0)), mk((1, 0, 1), (1, 0)), mk((1, 1, -1), (0, 1))]
        lo, hi = parts.clip_polygon(tri, 2, 0.0)
        area = lambda poly: sum(tri_area(poly[0]['p'], poly[k]['p'], poly[k + 1]['p']) for k in range(1, len(poly) - 1))
        self.assertAlmostEqual(area(lo) + area(hi), tri_area(*[v['p'] for v in tri]))
        self.assertTrue(all(v['p'][2] <= 1e-9 for v in lo) and all(v['p'][2] >= -1e-9 for v in hi))
        cut = [v for v in lo if v['p'][2] == 0.0 and v['p'][1] == 0.0][0]
        self.assertAlmostEqual(cut['uv'][0], 0.5)

    def test_regions(self):
        self.assertEqual(parts.classify(9, (0.95, 0.5, 0.0)), 5)         # right door
        self.assertEqual(parts.classify(9, (-0.95, 0.5, 0.0)), 6)        # left door
        self.assertEqual(parts.classify(9, (0.0, 1.2, 0.0)), 9)          # roof stays
        self.assertEqual(parts.classify(9, (0.95, 0.5, 1.5)), 9)         # ahead of the door
        self.assertEqual(parts.classify(1, (0.0, 0.3, 2.3)), 12)         # front bumper
        self.assertEqual(parts.classify(1, (0.9, 0.5, 1.3)), 3)          # right fender
        self.assertEqual(parts.classify(1, (-0.9, 0.5, 1.3)), 4)         # left fender
        self.assertEqual(parts.classify(1, (0.0, 0.5, 1.3)), 1)
        self.assertEqual(parts.classify(10, (0.0, 0.86, -2.0)), 11)      # decklid
        self.assertEqual(parts.classify(10, (0.0, 0.4, -2.7)), 13)       # rear bumper bar
        self.assertEqual(parts.classify(10, (0.9, 0.5, -2.4)), 10)       # quarter panel


class SplitModelTests(unittest.TestCase):
    def setUp(self):
        # Right-hand side panel x=0.98 from z=-1.5 to 1.0 (two triangles in one strip),
        # y 0.3..0.8: it must be cut at the door edges z=-0.75 and z=0.72.
        a, b, c, d = (0.98, 0.3, -1.5), (0.98, 0.8, -1.5), (0.98, 0.3, 1.0), (0.98, 0.8, 1.0)
        self.src = synth_part([(a, True), (b, True), (c, False), (d, False)])
        self.model = {'head': bytearray(0x58), 'parts': [self.src], 'tjrm': None, 'nrec': 50}
        self.before = decoded_triangles(self.src)

    def test_door_is_cut_out_of_the_side_and_area_is_kept(self):
        total = sum(tri_area(*t) for t in self.before)
        moved = parts.split_model(self.model, {'cupcar.tex'})
        self.assertIn(5, moved)
        groups = {gen.part_group(p) & 0xFFFF for p in self.model['parts']}
        self.assertEqual(groups, {5, 9})
        door = [p for p in self.model['parts'] if gen.part_group(p) & 0xFFFF == 5][0]
        body = [p for p in self.model['parts'] if gen.part_group(p) & 0xFFFF == 9][0]
        self.assertTrue(all(-0.75 - 1e-3 <= v[2] <= 0.72 + 1e-3 for t in decoded_triangles(door) for v in t))
        after = sum(tri_area(*t) for t in decoded_triangles(door)) + sum(tri_area(*t) for t in decoded_triangles(body))
        self.assertAlmostEqual(after, total, places=3)
        # the door piece covers exactly door_length x height
        self.assertAlmostEqual(sum(tri_area(*t) for t in decoded_triangles(door)), (0.72 + 0.75) * 0.5, places=3)

    def test_new_part_keeps_header_flags_and_batch_layout(self):
        parts.split_model(self.model, {'cupcar.tex'})
        for p in self.model['parts']:
            self.assertEqual(struct.unpack_from('<I', p['hdr'], 0x1C)[0], 0x1F0003)
            self.assertEqual(gen.part_group(p) & 0xFFFF0000, 0x10000)
            self.assertEqual(len(p['stream']) % 16, 0)
            for b in gen.walk_vif(p['stream']):
                self.assertEqual(b['mscal'], 0x5C)
                self.assertLessEqual(b['count'], parts.MAX_VERTS)
                self.assertEqual(sorted(b['unpacks']), [0, 1, 2, 3, 150, 198])

    def test_roundtrip_through_modl_writer(self):
        parts.split_model(self.model, {'cupcar.tex'})
        blob = gen.write_modl(self.model)
        again = gen.read_modl(blob)
        self.assertEqual(len(again['parts']), len(self.model['parts']))

    def test_untouched_texture_is_not_split(self):
        other = synth_part([((0.98, 0.3, -1.5), True), ((0.98, 0.8, -1.5), True), ((0.98, 0.3, 1.0), False)], tex=b'detail.tex')
        model = {'head': bytearray(0x58), 'parts': [other], 'tjrm': None, 'nrec': 50}
        self.assertEqual(parts.split_model(model, {'cupcar.tex'}), {})
        self.assertIs(model['parts'][0], other)


class ShadowVolumeTests(unittest.TestCase):
    def hull(self):
        pts = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1), (1, 1, 1)]
        return gen.hull_triangles(pts)

    def test_every_edge_gets_a_fin_between_its_two_faces(self):
        faces = self.hull()
        vol = gen.shadow_volume_triangles(faces)
        edges = {(a, b) for (a, b, c), _ in faces for a, b in ((a, b), (b, c), (c, a))}
        self.assertEqual(len(vol), len(faces) + 2 * len(edges) // 2)
        # solid faces carry one inward normal on all three vertices; fins mix the two neighbours'
        solid = [t for t in vol if len({tuple(n) for n in t[1]}) == 1 and len(set(t[0])) == 3]
        self.assertEqual(len(solid), len(faces))
        fins = [t for t in vol if len(set(t[0])) == 2]
        self.assertEqual(len(fins), len(edges))

    def test_encoded_parity_matches_retail_convention(self):
        vol = gen.shadow_volume_triangles(self.hull())
        part = gen.encode_shadow_part(vol, 0, 0.0)
        stream = part['stream']
        batches = gen.walk_vif(stream)
        self.assertEqual(struct.unpack_from('<I', stream, batches[0]['mscal_off'])[0], 0x14000004)
        solid = 0
        for b in batches:
            pts = gen.batch_positions(stream, b)
            nb = stream[b['unpacks'][1]['off']:b['unpacks'][1]['off'] + 4 * b['count']]
            for i in range(2, b['count']):
                if nb[4 * i + 3] & 0x80:
                    continue
                a, bb, c = pts[i - 2], pts[i - 1], pts[i]
                if len({tuple(round(x, 3) for x in p) for p in (a, bb, c)}) < 3:
                    continue
                tri = (a, bb, c) if i % 2 == 0 else (bb, a, c)
                e1 = [tri[1][k] - tri[0][k] for k in range(3)]
                e2 = [tri[2][k] - tri[0][k] for k in range(3)]
                n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
                stored = [(nb[4 * i + k] - 128) / 127 for k in range(3)]
                self.assertLess(sum(x * y for x, y in zip(n, stored)), 0)   # stored normal is inward
                solid += 1
        self.assertEqual(solid, len(self.hull()))

    def test_later_batches_continue_with_mscnt(self):
        big = []
        for k in range(40):
            o = k * 0.01
            big.append((((o, 0, 0), (1 + o, 0, 0), (0, 1, 0)), ((0, 0, 1),) * 3))
        stream = gen.encode_shadow_part(big, 0, 0.0)['stream']
        words = [struct.unpack_from('<I', stream, b['mscal_off'])[0] for b in gen.walk_vif(stream)]
        self.assertEqual(words[0], 0x14000004)
        self.assertTrue(all(w == 0x17000000 for w in words[1:]) and len(words) > 1)


class PanelHelperTests(unittest.TestCase):
    def test_helper_names_match_the_ee_panel_table(self):
        box = lambda x0, x1, y0, y1, z0, z1: [(x, y, z) for x in (x0, x1) for y in (y0, y1) for z in (z0, z1)]
        gpts = {2: box(-.8, .8, .5, .8, .9, 2.1), 5: box(.6, .98, .2, 1.2, -.75, .72), 6: box(-.98, -.6, .2, 1.2, -.75, .72),
                11: box(-.8, .8, .6, .9, -2.5, -1.7), 12: box(-.97, .97, -.06, .6, 2.05, 2.37),
                13: box(-.93, .93, .3, .55, -2.8, -2.55)}
        names = {h[0] for h in parts.panel_helpers(gpts)}
        for needed in ('left door hinge', 'right door hinge', 'right hood hinge', 'right decklid hinge',
                       'left front bumper', 'left rear bumper'):
            self.assertIn(needed, names)
        hinge = {h[0]: h[2] for h in parts.panel_helpers(gpts)}
        self.assertAlmostEqual(hinge['left door hinge'][0], -0.956)
        self.assertAlmostEqual(hinge['left door hinge'][2], 0.713)

    def test_absent_groups_add_no_helpers(self):
        self.assertEqual(parts.panel_helpers({9: [(0, 0, 0), (1, 1, 1)]}), [])


if __name__ == '__main__':
    unittest.main()
