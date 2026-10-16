# Vulkan alpha-test pass ordering

The GS applies the alpha test and AFAIL action per primitive, in draw order.
The Vulkan backend implements tests that can still write on failure with two
passes. Rendering all passing fragments of an entire batch followed by all
failing fragments reorders primitives. In particular, a font shadow's failed
FB_ONLY fragments can overwrite a foreground glyph drawn later in the batch.

The backend now emits passing and failing draws consecutively for each
triangle, line or point. Sprite quads are already expanded into two triangles;
their nonoverlapping triangle coverage retains the same order. Vertex upload,
target acquisition and binding remain batched. Ordinary one-pass draws retain
their existing batching. Two-pass batches issue more GPU draw calls; gameplay
performance remains a separate integration check.

The asset-free `alpha_overlap` case in `gs_pixel_fixture.cpp` reproduces the
problem with two overlapping rectangles sharing state and Z=1: black with alpha
64 fails GEQUAL 96 and writes FB_ONLY, then cream `(200,200,160)` with alpha 128
passes. Both use depth GEQUAL in a Z16S buffer. The expected interior is cream.
The previous Vulkan batch order produced `(100,100,80)`; software and corrected
Vulkan both produce `(200,200,160)`. `VulkanGSPixels` asserts this result using
actual Vulkan and fails rather than accepting software fallback.

A local, uncommitted retail menu replay independently reproduced hollow font
interiors. Its glyphs use a 256×256 PSMT4 atlas, MODULATE/TCC, source-alpha
blending, alpha GEQUAL 96 with FB_ONLY failure, depth GEQUAL Z16S at Z 115, and
DATE disabled. Corrected Vulkan produces solid glyph interiors. Within the
matched title/instruction text region, 691 bright software-reference pixels
had mean absolute RGB error 19.43 before and 0.90 after; pixels with error over
40 fell from 147 to 0. These measurements establish this alpha-order defect and
its font reproduction, not complete renderer equivalence. UNORM blend rounding
and other GS limitations remain separately documented/tested. No retail data,
replay or screenshots are committed.

Run with an available Vulkan device:

```sh
EOD_HEADLESS=1 EOD_MUTE=1 ctest --test-dir build-mac -R '^VulkanGSPixels$' --output-on-failure
```
