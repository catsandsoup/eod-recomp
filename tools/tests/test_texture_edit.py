"""Native texture edits preserve storage and agree with independent GS sampling."""
from pathlib import Path
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from test_texture_preview import fixture, oracle, retail_entry
import tex
try:
    import texture_edit
except ImportError:
    texture_edit = None


def mip_fixture(w, h, fmt, levels=4):
    original = bytearray(fixture(w, h, fmt))
    struct.pack_into('<I', original, 8, levels)
    # Nonzero metadata, holes and footer must survive edits.
    original[100:120] = bytes(range(20))
    for level in range(1, levels):
        original.extend(b'GAP!')
        struct.pack_into('<I', original, 28 + level*4, len(original))
        w, h = max(1, w//2), max(1, h//2)
        original.extend(fixture(w, h, fmt)[1152:])
    original.extend(b'FOOTER')
    return bytes(original)


def as_mip(data, level):
    out = bytearray(data)
    w, h = struct.unpack_from('<2I', data)
    struct.pack_into('<2I', out, 0, max(1,w>>level), max(1,h>>level))
    struct.pack_into('<I', out, 28, struct.unpack_from('<I',data,28+level*4)[0])
    return bytes(out)


class TextureEditTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(texture_edit, 'texture_edit module must implement native edits')
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.png = Path(self.directory.name)/'edit.png'

    def test_export_and_unchanged_import_are_byte_exact(self):
        for fmt in (0x200, 0x30201):
            raw = mip_fixture(128,128,fmt)
            texture_edit.export_png(raw,self.png)
            self.assertEqual(tex.read_png(str(self.png))[2],oracle(raw))
            self.assertEqual(texture_edit.import_png(raw,self.png),raw)

    def test_edit_preserves_header_gaps_footer_and_updates_every_mip(self):
        for fmt in (0x200,0x201):
            raw = mip_fixture(128,128,fmt)
            color = bytes((33,78,123,99))
            tex.write_png(str(self.png),128,128,color*(128*128))
            edited = texture_edit.import_png(raw,self.png)
            self.assertEqual(len(edited),len(raw))
            self.assertEqual(edited[:128],raw[:128])
            self.assertTrue(edited.endswith(b'FOOTER'))
            for level in range(4):
                expected = color*((128>>level)**2)
                self.assertEqual(oracle(as_mip(edited,level)),expected)
                if level:
                    off=struct.unpack_from('<I',raw,28+level*4)[0]
                    self.assertEqual(edited[off-4:off],b'GAP!')

    def test_spatial_replacement_matches_native_oracle_and_nearest_mips(self):
        for fmt in (0x200,0x201):
            raw = mip_fixture(128,128,fmt)
            colors=[bytes((n*17,n*9,255-n*13,255)) for n in range(16)]
            pixels=b''.join(colors[(x//8+y//8)%16] for y in range(128) for x in range(128))
            tex.write_png(str(self.png),128,128,pixels)
            edited=texture_edit.import_png(raw,self.png)
            for level in range(4):
                size=128>>level
                expected=b''.join(pixels[((y<<level)*128+(x<<level))*4:((y<<level)*128+(x<<level))*4+4] for y in range(size) for x in range(size))
                self.assertEqual(oracle(as_mip(edited,level)),expected)

    def test_more_than_palette_capacity_is_deterministic_and_bounded(self):
        raw=mip_fixture(32,32,0x200)
        pixels=b''.join(bytes((x*8,y*8,(x+y)*4,255)) for y in range(32) for x in range(32))
        tex.write_png(str(self.png),32,32,pixels)
        edited=texture_edit.import_png(raw,self.png)
        self.assertEqual(edited,texture_edit.import_png(raw,self.png))
        sampled=oracle(edited)
        self.assertLessEqual(len({sampled[i:i+4] for i in range(0,len(sampled),4)}),16)
        self.assertTrue(all(sampled[i]==255 for i in range(3,len(sampled),4)))
        saturated=texture_edit.tint_texture(raw,(1e308,1e308,1e308))
        self.assertEqual(len(saturated),len(raw))

    def test_tint_updates_shared_palette_only_preserving_alpha_all_mips(self):
        raw=mip_fixture(128,128,0x200)
        edited=texture_edit.tint_texture(raw,(0.5,1.0,0.0))
        self.assertEqual(edited[:128],raw[:128])
        self.assertEqual(edited[1152:],raw[1152:])
        for level in range(4):
            old=oracle(as_mip(raw,level))
            expected=b''.join(bytes((round(old[i]*.5),old[i+1],0,old[i+3])) for i in range(0,len(old),4))
            self.assertEqual(oracle(as_mip(edited,level)),expected)
        for factors in ((-1,1,1),(float('nan'),1,1),(1,2)):
            with self.assertRaises(ValueError):
                texture_edit.tint_texture(raw,factors)

    def test_reject_dimensions_bad_mips_direct_color_and_aliasing(self):
        raw=mip_fixture(128,128,0x201)
        tex.write_png(str(self.png),64,64,bytes((3,4,5,255))*4096)
        with self.assertRaisesRegex(ValueError,'dimensions'):
            texture_edit.import_png(raw,self.png)
        bad=bytearray(raw)
        struct.pack_into('<I',bad,32,1152)
        with self.assertRaises(ValueError):
            texture_edit.tint_texture(bytes(bad),(1,1,1))
        bad=bytearray(raw)
        struct.pack_into('<I',bad,12,3)
        with self.assertRaises(ValueError):
            texture_edit.tint_texture(bytes(bad),(1,1,1))
        raw=fixture(32,128,1)
        tex.write_png(str(self.png),32,128,bytes((3,4,5,255))*4096)
        with self.assertRaisesRegex(ValueError,'alias'):
            texture_edit.import_png(raw,self.png)

    @unittest.skipUnless(os.environ.get('EOD_TEST_RETAIL_TEXTURES')=='1','Owner-supplied retail opt in')
    def test_retail_four_bit_environment_export_tint_and_import(self):
        raw=retail_entry('TRACKS/MANKATO.TRK','at-aprn1.tex')
        texture_edit.export_png(raw,self.png)
        self.assertEqual(tex.read_png(str(self.png))[2],oracle(raw))
        self.assertEqual(texture_edit.import_png(raw,self.png),raw)
        tinted=texture_edit.tint_texture(raw,(0.8,0.9,1.0))
        self.assertNotEqual(oracle(tinted),oracle(raw))
        self.assertEqual(len(tinted),len(raw))
        texture_edit.export_png(tinted,self.png)
        replaced=texture_edit.import_png(raw,self.png)
        self.assertEqual(oracle(replaced),oracle(tinted))
        self.assertEqual(replaced[:128],raw[:128])


if __name__=='__main__':
    unittest.main()
