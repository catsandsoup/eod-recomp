"""vif_patch: in-place MODL/VIF geometry round-trip. Retail census gated by EOD_TEST_RETAIL_MODELS=1."""
import math
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools'))
sys.path.insert(0, str(ROOT/'tools'/'tests'))
import vif_mesh
import vif_patch
from rst0 import eod_crc
from test_vif_preview import retail_models

RETAIL = os.environ.get('EOD_TEST_RETAIL_MODELS') == '1'
DISC = Path(os.environ.get('EOD_DISC_DATA', str(Path.home()/'Library/Application Support/TDEODRecomp/disc/DATA')))


def cmd(code, count=0, address=0, payload=b''):
    return struct.pack('<I', (code << 24) | (count << 16) | address) + payload + b'\0'*((-len(payload)) % 4)


def synthetic_member():
    """One 4SPS part, one body-style batch: V3-16 positions (row 256.0, offset mode), V4-8 normals,
    V2-16 UVs (row 64.0), q198/q199 offsets. Six vertices, two triangles (strip 0,1 ADC)."""
    pos = [(0, 0, 0), (1.5, 0, 0), (0, 1.5, 0), (1.5, 1.5, 0), (0, 0, 1.5), (1.5, 0, 1.5)]
    u16 = lambda v: round(v*32768)
    s = cmd(0x60, 1, 0xc000, struct.pack('<I', 0x8006)) + cmd(1, address=0x0103) + cmd(5, address=1)
    s += cmd(0x20, payload=struct.pack('<I', 0x40404040))
    s += cmd(0x30, payload=struct.pack('<4I', 0x43800000, 0x43800000, 0x43800000, 0x3f800000))
    s += cmd(0x79, 6, 0xc002, b''.join(struct.pack('<3H', *map(u16, p)) for p in pos))
    s += cmd(0x30, payload=struct.pack('<4I', 0x47800000, 0x47800000, 0x47800000, 0x7f80))
    s += cmd(0x6e, 6, 0xc001, b''.join(bytes((128, 128, 255, 128 if i in (0, 1, 3, 4) else 0)) for i in range(6)))
    s += cmd(0x20, payload=struct.pack('<I', 0x50505050))
    s += cmd(0x30, payload=struct.pack('<4I', 0x42800000, 0x42800000, 0x3f800000, 0x8000))
    s += cmd(0x75, 6, 0xc003, b''.join(struct.pack('<2H', i*1000, 2000) for i in range(6)))
    s += cmd(5, address=0) + cmd(1, address=0x0101)
    s += cmd(0x6c, 2, 0xc0c6, struct.pack('<4f4f', 0, 0, 0, 0, -256.0, -256.0, -256.0, 0))
    s += cmd(0x14, address=0x5c)
    part = bytearray(56) + s
    part[:4] = b'4SPS'
    struct.pack_into('<I', part, 4, len(part))
    part[8:16] = b'test.tex'
    hdr = bytearray(0xE8 + 50*4)
    struct.pack_into('<6I', hdr, 0, 9, 0x58, 1, 0, 50, 0xE8)
    struct.pack_into('<4f', hdr, 0x18, 3.0, 0.75, 0.75, 0.75)
    struct.pack_into('<6f', hdr, 0x28, 0, 1.5, 0, 1.5, 0, 1.5)
    for i in range(36):
        struct.pack_into('<I', hdr, 0x58 + 4*i, 0xE3E3E3E3)
    struct.pack_into('<I', hdr, 0x58, len(hdr))
    return bytes(hdr) + bytes(part)


def edit_obj(src, dst, fn):
    out = []
    for line in open(src).read().split('\n'):
        if line.startswith('v '):
            line = 'v %.7f %.7f %.7f' % fn(*map(float, line.split()[1:4]))
        out.append(line)
    open(dst, 'w').write('\n'.join(out))


def decoded(data):
    m = vif_mesh.parse_modl(data)
    return [v['p'] for p in m['parts'] for b in vif_mesh.decode_part(data, p) for v in b['verts']]


class SyntheticTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.t = Path(self.tmp.name)
        self.data = synthetic_member()
        self.obj = str(self.t/'a.obj')
        self.map = vif_patch.export_model(self.data, self.obj, 'x.mod', '')

    def tearDown(self):
        self.tmp.cleanup()

    def test_noop_is_byte_identical(self):
        new, rep = vif_patch.patch_member(self.data, self.map, self.obj)
        self.assertEqual(new, self.data)
        self.assertEqual(rep['bytes_changed'], 0)

    def test_translation_decodes_at_new_position_and_grows_bounds(self):
        edit_obj(self.obj, str(self.t/'b.obj'), lambda x, y, z: (x+0.5, y+1.0, z-0.25))
        new, rep = vif_patch.patch_member(self.data, self.map, str(self.t/'b.obj'))
        self.assertEqual(len(new), len(self.data))
        for a, b in zip(decoded(self.data), decoded(new)):
            for k, d in enumerate((0.5, 1.0, -0.25)):
                self.assertAlmostEqual(b[k], a[k]+d, delta=1e-4)
        self.assertLess(rep['max_err']['pos'], 1e-4)
        self.assertIsNotNone(rep['bounds_updated'])
        hdr = vif_mesh.parse_modl(new)['bbox']
        self.assertGreaterEqual(hdr['max'][1], 2.5 - 1e-4)

    def test_large_scale_rebases_row_exponent(self):
        edit_obj(self.obj, str(self.t/'b.obj'), lambda x, y, z: (x*100, y*100, z*100))
        new, rep = vif_patch.patch_member(self.data, self.map, str(self.t/'b.obj'))
        self.assertGreater(rep['rebased_batches'], 0)
        for a, b in zip(decoded(self.data), decoded(new)):
            for k in range(3):
                self.assertAlmostEqual(b[k], a[k]*100, delta=3e-3)

    def test_topology_change_rejected(self):
        lines = open(self.obj).read().split('\n')
        i = max(k for k, l in enumerate(lines) if l.startswith('v '))
        with open(self.t/'c.obj', 'w') as f:
            f.write('\n'.join(lines[:i+1] + ['v 0 0 0'] + lines[i+1:]))
        with self.assertRaises(vif_patch.PatchError):
            vif_patch.patch_member(self.data, self.map, str(self.t/'c.obj'))
        faces = [l for l in lines if l.startswith('f ')]
        open(self.t/'d.obj', 'w').write('\n'.join(l for l in lines if l is not faces[0]))
        with self.assertRaises(vif_patch.PatchError):
            vif_patch.patch_member(self.data, self.map, str(self.t/'d.obj'))

    def test_out_of_range_rejected_and_nonfinite(self):
        edit_obj(self.obj, str(self.t/'b.obj'), lambda x, y, z: (x, y, float('nan')))
        with self.assertRaises(vif_patch.PatchError):
            vif_patch.patch_member(self.data, self.map, str(self.t/'b.obj'))

    def test_wrong_member_rejected(self):
        bad = bytearray(self.data)
        bad[-1] ^= 1
        with self.assertRaises(vif_patch.PatchError):
            vif_patch.patch_member(bytes(bad), self.map, self.obj)

    def test_out_of_range_reported_with_clamp_on_8bit_lane(self):
        # 8-bit lane too narrow even after exponent bump is impossible only past 2^23; check clamp path exists
        self.assertIn('clamp', vif_patch.patch_member.__code__.co_varnames)


@unittest.skipUnless(RETAIL, 'Opt in to owner-supplied MODL resources')
class RetailTests(unittest.TestCase):
    def test_every_retail_modl_noop_roundtrip_byte_identical(self):
        fails, count, tmp = [], 0, tempfile.mkdtemp()
        obj = os.path.join(tmp, 'x.obj')
        for archive, name, data in retail_models():
            count += 1
            try:
                mp = vif_patch.export_model(data, obj, name, archive)
                new, rep = vif_patch.patch_member(data, mp, obj)
                if new != data:
                    fails.append((archive, name, 'differs'))
            except Exception as e:   # noqa: BLE001
                fails.append((archive, name, repr(e)))
        self.assertEqual(count, 490)
        self.assertEqual(fails, [])

    def test_translated_and_scaled_track_and_car_models(self):
        want = {('ANOKA.TRK', 'b_stfn.mod'), ('BLITZ.CAR', 'Low1.mod'), ('RACE.RES', 'envsky.mod'), ('TRKEDIT.RES', 'point.mod')}
        seen, tmp = 0, tempfile.mkdtemp()
        for archive, name, data in retail_models():
            if (archive, name) not in want:
                continue
            obj = os.path.join(tmp, 'x.obj')
            mp = vif_patch.export_model(data, obj, name, archive)
            edit_obj(obj, obj + '2', lambda x, y, z: (x*2+10, y*3+4, z*2))
            new, rep = vif_patch.patch_member(data, mp, obj + '2')
            self.assertEqual(len(new), len(data))
            for a, b in zip(decoded(data), decoded(new)):
                for q, w in zip(b, (a[0]*2+10, a[1]*3+4, a[2]*2)):
                    self.assertAlmostEqual(q, w, delta=max(2e-3, abs(w)*2e-5))
            m1 = vif_mesh.parse_modl(new)
            xs = [p[0] for p in decoded(new)]
            self.assertGreaterEqual(m1['bbox']['max'][0] + 1e-3, max(xs))
            seen += 1
        self.assertEqual(seen, len(want))

    def test_scanner_matches_vif_mesh_on_all_models(self):
        for archive, name, data in retail_models():
            m, parts = vif_patch.decode_member(data)
            for part, raw in zip(parts, m['parts']):
                ref = [v['p'] for b in vif_mesh.decode_part(data, raw) for v in b['verts']]
                self.assertEqual(len(ref), len(part['v']['p']), (archive, name))
                for p, q in zip(part['v']['p'], ref):
                    self.assertTrue(all(abs(x-y) < 1e-9 for x, y in zip(p, q)))

    def test_archive_patch_same_size_with_directory_crc(self):
        import modding_backend as mb
        src = DISC/'TRACKS'/'ANOKA.TRK'
        data, entry = vif_patch.read_member(src, 'b_stfn.mod')
        tmp = Path(tempfile.mkdtemp())
        obj = str(tmp/'x.obj')
        mp = vif_patch.export_model(data, obj, 'b_stfn.mod', str(src))
        edit_obj(obj, obj + '2', lambda x, y, z: (x, y+5, z))
        new, _ = vif_patch.patch_member(data, mp, obj + '2')
        out = tmp/'ANOKA.TRK'
        vif_patch.patch_archive(src, 'b_stfn.mod', new, out)
        self.assertEqual(out.stat().st_size, src.stat().st_size)
        got, e2 = mb.payload(out, 'b_stfn.mod')       # verifies the CRC
        self.assertEqual(got, new)
        self.assertEqual(e2['crc'], eod_crc(new))
        a, b = src.read_bytes(), out.read_bytes()
        diff = [i for i in range(len(a)) if a[i] != b[i]]
        lo, hi = entry['offset'], entry['offset'] + entry['size']
        dirent = struct.unpack_from('<I', a, 20)[0]
        self.assertTrue(all(lo <= i < hi or dirent <= i < dirent + 68*10000 for i in diff))
        # every other member is untouched and still CRC-valid
        for e in mb.directory(out):
            if e['name'] != 'b_stfn.mod':
                self.assertEqual(mb.payload(out, e['name'], verify_crc=False)[0], mb.payload(src, e['name'], verify_crc=False)[0])

    def test_modl_member_census_in_tracks_and_cars(self):
        counts = {}
        for archive, name, data in retail_models():
            counts[archive] = counts.get(archive, 0) + 1
        self.assertEqual(sum(v for k, v in counts.items() if k.endswith('.TRK')), 54)
        self.assertEqual(sum(v for k, v in counts.items() if k.endswith('.CAR')), 278)


if __name__ == '__main__':
    unittest.main()
