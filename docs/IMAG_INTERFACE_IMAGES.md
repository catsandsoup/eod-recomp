# IMAG interface images (`*.img`)

264 members across COMMON, GENESIS, PAINTKIT, RACE, RACEDAY, UI3D, UIACTION, UIBIG,
UIEDIT, UIMEM and HOMEBASE.TRK: buttons, controller glyphs, arrows, HUD dials and LEDs,
logos and full-screen backdrops (up to 512 x 512). Decoder: `tools/formats/imag.py`.

## Layout (little endian)

| Offset | Size | Meaning |
|---|---|---|
| 0x00 | u32 | width (loader panics above 512) |
| 0x04 | u32 | height (same limit) |
| 0x08 | u32 | frame count N (1 to 20 in retail) |
| 0x0C | u32 | pixel layout, always 4 (indexed) |
| 0x10 | u8 + 3 | storage kind, always 1 (8-bit indexed); flags `E4 40 00` unread |
| 0x14 | u32 | bytes after the 64-byte header |
| 0x18, 0x1C | u32 x2 | build addresses; overwritten at load with the frame table bounds |
| 0x20 | N x u32 | frame offsets, `first + i * (1024 + w*h)` |
| ... | | padding to the first frame (offset copy, 256 x N, build bytes); first frame is 64-byte aligned |
| per frame | 1,024 | 256 RGBA palette entries, already in GS CLUT order (no reorder needed) |
| | w*h | palette indices, row-major, top row first (linear, no GS block swizzle) |

Alpha: 255 is opaque (not the GS 0x80 scale); 0 is transparent. Every retail file is
exactly header + frames with no trailing bytes.

## Evidence

- `ImageLoad` 0x00349078 and 0x0034A0F8 call `ResourceGet` (0x00132638) with type
  `0x494D4147` ("IMAG"), then the relocation pass 0x00349C18 (adds the load address to the
  N frame offsets at +0x20, stores the table bounds at +0x18/+0x1C) and the upload pass
  0x00349CA8 (gen/rec_097.c). Kind 1 goes to 0x00350210, which copies 0x400 palette bytes
  then uploads the index bytes; sizes above 512 panic (0x0034A1D0 rounds sizes for the GS).
- All 264 files satisfy `size == first + N * (1024 + w*h)` and frame offsets are exactly
  that stride; rendering with a linear palette and linear pixels gives recognisable
  artwork (checked with a contact sheet of mixed archives).
- Not yet run in the game: that an edited image displays correctly in a native run.

## Editing

`encode(data, {"image.png": path})` (or `"frame.N.png"`) accepts a same-size PNG. If its
pixels equal the current frame the file is returned unchanged (byte-identical). Otherwise
the frame is reduced to 256 colours with the same method as `tools/texture_edit.py`
(most frequent colours, then nearest RGBA, no dithering) and only that frame's palette and
pixels change; file size and layout stay identical. Images with at most 256 distinct
colours therefore come back pixel-identical.

## Not covered

Storage kinds other than 1 (direct colour, other depths) do not occur in retail; the
loader supports them (0x0034A258 converts them) and the decoder reports them as
unrecognised, so they open as generic blocks and save unchanged.
