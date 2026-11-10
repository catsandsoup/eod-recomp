"""Preview pixels are checked against independent GS upload/sample tables."""
import hashlib
import os
from pathlib import Path
import re
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from texture_preview import decode_preview


def native_tables():
    source = (ROOT / 'runtime/gs/gs_mem.cpp').read_text()
    tables = {}
    for name, rows, cols, body in re.findall(r'(\w+)\[(\d+)\]\[(\d+)\] = \{(.*?)\};', source, re.S):
        values = [int(x) for x in re.findall(r'\d+', body)]
        tables[name] = [values[i * int(cols):(i + 1) * int(cols)] for i in range(int(rows))]
    return tables


def fixture(w, h, fmt):
    bpp = 8 if fmt & 255 == 1 else 4
    header = bytearray(128)
    struct.pack_into('<8I', header, 0, w, h, 1, fmt, 0x320600, 0, 128, 1152)
    # ARGB on disc. Distinct alpha and channels catch palette permutations.
    palette = b''.join(bytes((255-i, i, (i*3)%256, (i*7)%256)) for i in range(256))
    body = bytes((i*73 + i//17 + 19) % 256 for i in range(w*h*bpp//8))
    return bytes(header) + palette + body


def oracle(data):
    """Replay f_0035EEA8 transfer into GS memory, then GS indexed sampling.

    No production preview functions, addressing constants, or tex compiler used.
    Transfer branches use the native internal-format 4=8bpp / 8=4bpp.
    """
    t = native_tables()
    w, h, _, fmt = struct.unpack_from('<4I', data)
    pal, mip = struct.unpack_from('<2I', data, 24)
    bits = 8 if fmt & 255 == 1 else 4
    if h < 32:
        indices = list(data[mip:mip+w*h]) if bits == 8 else [
            (data[mip+i//2] >> (4*(i%2))) & 15 for i in range(w*h)]
    else:
        # Native large-mip transfers use PSMCT32. In the 4-bit narrow case,
        # width/height of the upload rectangle are exchanged, NOT the image.
        if bits == 8:
            tw, th, pitch = w//2, h//2, ((w+63)//64)//2
        elif w >= 128 and h >= 128:
            tw, th, pitch = w//2, h//4, (((w+63)//64)+1)//2
        else:
            tw, th, pitch = h//2, w//4, (((h+63)//64)+1)//2
        memory = bytearray(4*1024*1024)
        for y in range(th):
            for x in range(tw):
                page = (y//32)*pitch + x//64
                block = t['blockTable32'][(y//8)%4][(x//8)%8]
                address = ((page*32+block)*64+t['columnTable32'][y%8][x%8])*4
                off = mip+(y*tw+x)*4
                memory[address:address+4] = data[off:off+4]
        indices = []
        pitch = ((w+63)//64)//2
        for y in range(h):
            for x in range(w):
                if bits == 8:
                    page = (y//64)*pitch+x//128
                    block = t['blockTable8'][(y//16)%4][(x//16)%8]
                    address = (page*32+block)*256+t['columnTable8'][y%16][x%16]
                    indices.append(memory[address])
                else:
                    page = (y//128)*pitch+x//128
                    block = t['blockTable4'][(y//16)%8][(x//32)%4]
                    nibble = (page*32+block)*512+t['columnTable4'][y%16][x%32]
                    indices.append((memory[nibble//2] >> ((nibble%2)*4)) & 15)
    return b''.join(bytes((data[pal+i*4+1], data[pal+i*4+2], data[pal+i*4+3], data[pal+i*4])) for i in indices)


def retail_entry(archive, name):
    root = Path(os.environ.get('EOD_DISC_DATA', str(Path.home()/'Library/Application Support/TDEODRecomp/disc/DATA')))
    with (root/archive).open('rb') as f:
        header = f.read(40)
        directory, count = struct.unpack_from('<2I', header, 20)
        f.seek(directory)
        entries = f.read(count*68)
        for i in range(count):
            row = entries[i*68:(i+1)*68]
            if row[:32].split(b'\0')[0].decode('latin1').lower() == name.lower():
                _, size, offset, _ = struct.unpack_from('<4I', row, 36)
                f.seek(directory+offset)
                return f.read(size)
    raise ValueError(name)


def direct_fixture(w, h, fmt):
    bits = 16 if fmt & 255 == 2 else 32
    header = bytearray(128)
    struct.pack_into('<8I', header, 0, w, h, 1, fmt, 0x320600, 0, 0, 128)
    body = bytes((i*53+i//7) % 256 for i in range(w*h*bits//8))
    return bytes(header)+body


def direct_oracle(data):
    # Native direct-color path transfers PSMCT16/32 unchanged and samples the
    # same format. Replay both steps with canonical runtime tables, independent
    # of the production decoder's linear path. RGB5 expansion follows shader
    # unpack_color, alpha is opaque for RGB555 preview (TEXA is material state).
    tables = native_tables()
    w,h,_,fmt = struct.unpack_from('<4I',data)
    bits = 16 if fmt & 255 == 2 else 32
    mip = struct.unpack_from('<I',data,28)[0]
    memory = bytearray(4*1024*1024)
    def address(x,y):
        pitch = (w+63)//64
        if bits == 16:
            page = (y//64)*pitch+x//64
            block = tables['blockTable16'][(y//8)%8][(x//16)%4]
            return ((page*32+block)*128+tables['columnTable16'][y%8][x%16])*2
        page = (y//32)*pitch+x//64
        block = tables['blockTable32'][(y//8)%4][(x//8)%8]
        return ((page*32+block)*64+tables['columnTable32'][y%8][x%8])*4
    stride = bits//8
    for y in range(h):
        for x in range(w):
            o=mip+(y*w+x)*stride
            a=address(x,y)
            memory[a:a+stride]=data[o:o+stride]
    result=bytearray()
    for y in range(h):
        for x in range(w):
            a=address(x,y)
            if bits==16:
                v=struct.unpack_from('<H',memory,a)[0]
                result.extend(((v%32)*8, ((v//32)%32)*8, ((v//1024)%32)*8, 255))
            else:
                result.extend(memory[a:a+4])
    return bytes(result)


class TexturePreviewTests(unittest.TestCase):
    def test_native_transfer_and_sampling_for_4_and_8_bit_rectangles(self):
        # Wrong nibble order, swapping the image dimensions, or applying the
        # 8-bit swizzle to 4-bit data changes these oracle pixels.
        for fmt in (0, 0x200, 0x30000, 1, 0x201, 0x30201):
            for w, h in ((16,16),(32,16),(32,32),(32,64),(64,32),(64,64),
                         (32,128),(32,256),(64,128),(128,32),(128,64),(128,128),(256,128),(128,256),(256,256)):
                with self.subTest(fmt=hex(fmt), size=(w,h)):
                    data = fixture(w,h,fmt)
                    width, height, pixels, meta = decode_preview(data)
                    self.assertEqual((width,height), (w,h))
                    self.assertEqual(pixels, oracle(data))
                    self.assertEqual(meta['format'], fmt)
                    self.assertEqual(meta['bits_per_pixel'], 8 if fmt & 255 == 1 else 4)
                    self.assertTrue(all(isinstance(v, (int,float)) for v in meta.values()))

    def test_direct_color_matches_native_transfer_and_sample(self):
        for fmt in (2,0x203):
            for w,h in ((16,16),(32,16),(32,32),(64,64),(32,128),(128,64)):
                with self.subTest(fmt=fmt,size=(w,h)):
                    raw=direct_fixture(w,h,fmt)
                    width,height,pixels,meta=decode_preview(raw)
                    self.assertEqual((width,height),(w,h))
                    self.assertEqual(pixels,direct_oracle(raw))
                    self.assertEqual(meta['bits_per_pixel'],16 if fmt==2 else 32)

    def test_direct_color_channel_values_are_not_palette_or_argb(self):
        raw=bytearray(direct_fixture(2,2,2))
        struct.pack_into('<4H',raw,128,0x001f,0x03e0,0x7c00,0xffff)
        self.assertEqual(decode_preview(raw)[2],bytes((248,0,0,255,0,248,0,255,0,0,248,255,248,248,248,255)))
        raw=bytearray(direct_fixture(1,1,0x203))
        raw[128:132]=bytes((12,34,56,78))
        self.assertEqual(decode_preview(raw)[2],bytes((12,34,56,78)))

    def test_uncovered_narrow_4bit_upload_is_rejected(self):
        # A 16-wide large-height CT32 transfer leaves sampled nibbles unwritten;
        # standalone preview cannot infer prior VRAM contents. No retail TEX in
        # the installed census uses this arrangement.
        for h in (64,256):
            with self.assertRaises(ValueError):
                decode_preview(fixture(16,h,0))

    @unittest.skipUnless(os.environ.get('EOD_TEST_RETAIL_TEXTURES') == '1', 'Opt in to owner-supplied textures')
    def test_all_three_retail_direct_color_resources(self):
        for archive,names in [('RES/COMMON.RES',('envmap.tex','cenvmap.tex')),('RES/GENESIS.RES',('loadspin.tex',))]:
            for name in names:
                with self.subTest(name=name):
                    raw=retail_entry(archive,name)
                    pixels=decode_preview(raw)[2]
                    self.assertEqual(pixels,direct_oracle(raw))
                    self.assertGreater(len(set(pixels[i:i+4] for i in range(0,len(pixels),4))),4)

    def test_small_nibbles_and_argb_palette_are_not_swapped(self):
        data = bytearray(fixture(16,16,0x200))
        data[1152:1154] = b'\x21\x43'
        self.assertEqual(decode_preview(data)[2][:16], bytes((1,3,7,254,2,6,14,253,3,9,21,252,4,12,28,251)))

    def test_truncation_unsupported_and_bad_extents_fail(self):
        valid = fixture(32,32,0x200)
        bad_header = bytearray(valid)
        struct.pack_into('<I', bad_header, 28, 0xfffffff0)
        unknown = bytearray(valid)
        struct.pack_into('<I', unknown, 12, 0x207)
        for raw in (b'',valid[:31],valid[:-1],bytes(bad_header),bytes(unknown)):
            with self.subTest(length=len(raw)), self.assertRaises(ValueError):
                decode_preview(raw)

    @unittest.skipUnless(os.environ.get('EOD_TEST_RETAIL_TEXTURES') == '1', 'Opt in to owner-supplied textures')
    def test_retail_reported_failures_match_native_oracle(self):
        for archive, names in [('TRACKS/MANKATO.TRK',('b1.tex','at-aprn1.tex')),
                               ('TRACKS/WELCH.TRK',('we-fld05.tex',))]:
            for name in names:
                with self.subTest(archive=archive,name=name):
                    raw = retail_entry(archive,name)
                    w,h,pixels,meta = decode_preview(raw)
                    self.assertEqual((w,h,meta['bits_per_pixel']), (128,128,4))
                    self.assertEqual(pixels, oracle(raw))
                    self.assertGreater(len(set(pixels[i:i+4] for i in range(0,len(pixels),4))), 4)

    @unittest.skipUnless(os.environ.get('EOD_TEST_RETAIL_TEXTURES') == '1', 'Opt in to owner-supplied textures')
    def test_retail_8bit_matches_reviewed_reference_hash(self):
        raw = retail_entry('RES/UICAR.RES','uicar_296.tm0')
        pixels = decode_preview(raw)[2]
        self.assertEqual(hashlib.sha256(pixels).hexdigest(), '25ed48fa7b205ce45cb12972b9354bc8ef4fdc3e72df027c87827e29c03add31')


if __name__ == '__main__':
    unittest.main()
