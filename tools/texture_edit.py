"""Layout-preserving edits of native EOD indexed TEX files.

PNG import accepts same-size, noninterlaced 8-bit RGBA PNGs (tex.read_png).
It quantizes to 16/256 shared colors using frequency then nearest RGBA distance,
without dithering, and rebuilds existing mips by nearest-neighbor downsampling.
This is deliberately ordinary indexed texture editing, not HDR lighting.
Direct-color import and aliased native upload layouts are unsupported.
"""
from collections import Counter
import math
from pathlib import Path
import struct

from tex import read_png, write_png
from texture_preview import _source_lanes, decode_preview


def _layout(data: bytes):
    width, height, _, metadata = decode_preview(data)
    bits = metadata['bits_per_pixel']
    if bits not in (4, 8):
        raise ValueError('Editing supports only native 4-bit/8-bit indexed TEX')
    levels = metadata['levels']
    if len(data) < 128:
        raise ValueError('Truncated native TEX header')
    palette = struct.unpack_from('<I', data, 24)[0]
    if palette < 128:
        raise ValueError('TEX palette overlaps header')
    extents = [(palette, palette+1024)]
    mips = []
    for level in range(levels):
        w, h = max(1,width>>level), max(1,height>>level)
        offset = struct.unpack_from('<I',data,28+4*level)[0]
        end = offset + (w*h*bits+7)//8
        if offset < 128 or end > len(data):
            raise ValueError(f'Invalid TEX mip {level} extent')
        if any(offset < b and end > a for a,b in extents):
            raise ValueError(f'Overlapping TEX mip {level} extent')
        extents.append((offset,end))
        mips.append((w,h,offset))
    return width,height,bits,palette,mips


def export_png(data: bytes, destination: Path) -> None:
    """Export the native mip-zero pixels, retaining their palette alpha."""
    width,height,pixels,_ = decode_preview(data)
    write_png(str(destination),width,height,pixels)


def tint_texture(original: bytes, rgb_factors: tuple[float,float,float]) -> bytes:
    """Multiply palette RGB channels; keep alpha, mip indices and metadata exact.

    Factors must be three finite nonnegative values; output channels clamp to
    255. Shared palette edits automatically apply to every existing mip level.
    """
    if len(rgb_factors)!=3 or any(not math.isfinite(v) or v<0 for v in rgb_factors):
        raise ValueError('RGB factors must be three finite nonnegative numbers')
    _,_,bits,palette,_ = _layout(original)
    result=bytearray(original)
    for index in range(1<<bits):
        for channel,factor in enumerate(rgb_factors,1):
            offset=palette+index*4+channel
            result[offset]=round(min(255,original[offset]*factor))
    return bytes(result)


def import_png(original: bytes, source: Path) -> bytes:
    """Replace indexed pixels in-place, preserving all native offsets and flags.

    An unchanged export returns the original bytes exactly. Edited imports use
    a shared quantized palette and regenerate every declared mip. Unsupported
    upload layouts fail before any output is returned. Source files are read
    only; callers decide where the returned standalone TEX bytes are saved.
    """
    width,height,bits,palette,mips=_layout(original)
    w,h,pixels=read_png(str(source))
    if (w,h)!=(width,height):
        raise ValueError('PNG dimensions must match the original TEX dimensions')
    if pixels==decode_preview(original)[2]:
        return original
    # Native very narrow uploads can alias samples. They decode for preview,
    # but cannot represent arbitrary independent replacement pixel values.
    lanes=[]
    for mw,mh,_ in mips:
        lane=range(mw*mh) if mh<32 else _source_lanes(mw,mh,bits)
        if len(set(lane))!=mw*mh or max(lane)>=mw*mh:
            raise ValueError('TEX upload aliases pixels; arbitrary PNG import is unsupported')
        lanes.append(lane)
    colors=Counter(pixels[i:i+4] for i in range(0,len(pixels),4))
    selected=[color for color,_ in colors.most_common(1<<bits)]
    cache={color:index for index,color in enumerate(selected)}
    for color in colors:
        if color not in cache:
            cache[color]=min(range(len(selected)), key=lambda i:sum((a-b)**2 for a,b in zip(color,selected[i])))
    indices=bytes(cache[pixels[i:i+4]] for i in range(0,len(pixels),4))
    result=bytearray(original)
    for index,color in enumerate(selected):
        r,g,b,a=color
        result[palette+index*4:palette+index*4+4]=bytes((a,r,g,b))
    previous_w=width
    previous_h=height
    for level,((mw,mh,offset),lane) in enumerate(zip(mips,lanes)):
        if level:
            indices=bytes(indices[min(previous_h-1,y*2)*previous_w+min(previous_w-1,x*2)] for y in range(mh) for x in range(mw))
        for index,target in zip(indices,lane):
            if bits==8:
                result[offset+target]=index
            else:
                byte=offset+(target>>1)
                shift=(target&1)*4
                result[byte]=(result[byte]&~(15<<shift))|(index<<shift)
        previous_w,previous_h=mw,mh
    return bytes(result)
