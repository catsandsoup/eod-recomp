# Native EOD PS2 texture contract

Player acceptance is recognizable custom livery and preview artwork in the
actual renderer. A compiler/decompiler self-roundtrip is insufficient: the old
pair agreed on an incorrect raw GS VRAM ordering and produced checker corruption
in the rendered Phantom selection preview.

## Verified retail layout

TEX v10 starts with a 0x80-byte header. Width, height and mip count are at offsets
0, 4 and 8. The tested 8-bit assets use format 0x201 at +0x0C and flags 0x320600
at +0x10. Preserve +0x14 as an auxiliary word: observed CT body textures use
0x100 and UICAR previews use zero. Palette offset +0x18 is 0x80; each of 256
palette entries is ARGB with 0–255 alpha. Native loading converts palette alpha
and palette order for GS use; input files must not perform that permutation.
Mip offsets start at +0x1C. Retail headers have a **zero sentinel** after the last
mip offset. Native `f_003521E8` relocates count+1 slots, explicitly skipping zero
entries. That loop does not mean the sentinel must contain the file-end address.

| Retail reference | Dimensions | Mip count | File bytes |
| --- | --- | --- | --- |
| UICAR.RES/uicar_296.tex | 16×16 | 4 | 1,668 |
| UICAR.RES/uicar_296.tm0 | 256×256 | 8 | 88,708 |
| CT.RES/20.tex | 256×128 | 7 | 44,936 |
| CT.RES/20.tm0 | 512×256 | 8 | 176,008 |

The observed mip chain stops when its smaller dimension reaches 2. Mips before
the last occupy at least 128 bytes. Full indexed payloads otherwise occupy
width×height bytes; the last 2×2 or 4×2 tail occupies 4 or 8 bytes.

## Upload order, distinct from absolute GS memory order

Native `f_0035EE38` computes mip dimensions, then tests whether **mip height is
less than 32** and branches to its direct linear upload path (`0x0035F140`).
Larger mips contain the compact byte ordering needed for a PSMCT32 upload whose
GS memory is subsequently sampled as PSMT8. Absolute `gs_addr8` addresses index
VRAM, and must not index a compact TEX payload.

For large mips the compiler uses `upload_addr8`. Small mip tails use linear
indices. The regression oracle independently uploads the compiled bytes using
PSMCT32 block/column tables from `runtime/gs/gs_mem.cpp`, then samples the VRAM
using that file's separate PSMT8 tables. It recovers every source pixel without
calling the compiler's upload addressing function.

A retail `uicar_296.tm0` was decoded and visually inspected as the recognizable
orange/teal number-296 car. Its decoded RGBA SHA-256 is:

`25ed48fa7b205ce45cb12972b9354bc8ef4fdc3e72df027c87827e29c03add31`

The retail resource is read locally for this regression and is not committed.
The old incorrect decoder fails this reference. Retail CT/20 small mips provide
additional evidence: 32×16 and 16×8 tails decode coherently as linear pixels,
consistent with the native height branch. The UICAR high-resolution reference
contains mostly empty lower mips, so it alone cannot establish their layout.

## Limits and next decisive test

The compiler regenerates nearest-neighbor mipmaps and quantizes to 256 colors;
it does not preserve arbitrary retail mip artwork or support PSMT4. Narrow-tall
textures with height greater than twice width are rejected explicitly: their
narrow large-height mip upload layout remains unverified. Header and
CPU GS equivalence establish the repaired format contract. They do not establish
rendered livery appearance, artistic quality, or full original visual parity.
Regenerate the package, inspect the Midsize Phantom carousel preview, and drive
the selected Phantom in the actual renderer to finish that acceptance.

The regenerated package subsequently reached rendered Midsize Phantom selection
and a selected-player race. The procedural 777 menu card is clean and the
black/white native-chassis body livery is visible. The bounded 6,000-vblank
journey passed; arbitrary artwork quality, every LOD and full native visual
parity remain outside that evidence.
