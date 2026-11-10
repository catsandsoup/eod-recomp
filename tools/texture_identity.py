"""Runtime-observable identity of native indexed EOD TEX resources.

Contract (docs/CREATION_KIT_TEXTURE_CONTRACTS.md): the GS renderer and the Kit
compute the same keys independently, so a host replacement can be bound to the
texture the game actually samples without trusting filenames.

  indexKey   FNV-1a 64 over b"EODTEX1" + u8 bits + u32le width + u32le height +
             one byte per mip-zero pixel (palette index), row-major.
  paletteKey FNV-1a 64 over R,G,B bytes of logical palette entries 0..n-1
             (n = 16 for 4-bit, 256 for 8-bit). Alpha is excluded: native
             loading converts palette alpha for the GS.

Direct-colour (16/32-bit) TEX resources have no key in contract v1.
"""
import struct

from texture_preview import _source_lanes, decode_preview

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK = (1 << 64) - 1


def fnv1a64(data, h=FNV_OFFSET):
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & MASK
    return h


def index_plane(data):
    """(bits, width, height, bytes of mip-zero palette indices, row-major)."""
    width, height, _, meta = decode_preview(data)
    bits = meta["bits_per_pixel"]
    if bits not in (4, 8):
        raise ValueError("Only indexed 4/8-bit TEX has a runtime identity key")
    mip = struct.unpack_from("<I", data, 28)[0]
    body = memoryview(data)[mip:mip + (width * height * bits + 7) // 8]
    if height < 32:
        lanes = range(width * height)
    else:
        lanes = _source_lanes(width, height, bits)
    if bits == 8:
        plane = bytes(body[i] for i in lanes)
    else:
        plane = bytes((body[i >> 1] >> ((i & 1) * 4)) & 15 for i in lanes)
    return bits, width, height, plane


def texture_identity(data):
    bits, width, height, plane = index_plane(data)
    head = b"EODTEX1" + struct.pack("<BII", bits, width, height)
    index_key = fnv1a64(plane, fnv1a64(head))
    pal = struct.unpack_from("<I", data, 24)[0]
    entries = 16 if bits == 4 else 256
    rgb = b"".join(data[pal + i * 4 + 1:pal + i * 4 + 4] for i in range(entries))  # stored A,R,G,B
    return dict(indexKey=f"{index_key:016x}", paletteKey=f"{fnv1a64(rgb):016x}",
                bits=bits, width=width, height=height)
