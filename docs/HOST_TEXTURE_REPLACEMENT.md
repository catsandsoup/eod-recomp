# Host high-resolution texture replacement (Vulkan)

Implements sections 2 and 4 of `CREATION_KIT_TEXTURE_CONTRACTS.md` in the
runtime. A replacement is bound in place of a native indexed texture the game
actually samples; GS memory, UV meaning and wrap/clamp modes are unchanged and
the native decode remains the fallback.

## Player-visible result and acceptance

- With an overlay whose `HOSTTEX/replacements.tsv` lists a texture, that surface
  shows the replacement at its own resolution. At render scale 1 detail is
  limited by screen pixels; at higher render scales detail finer than one native
  texel resolves (render scale and texture scale are independent).
- Without an overlay, with `EOD_HOST_TEXTURES=0`, or with a non-matching key the
  picture is pixel-identical to the build without this feature.

## Configuration

| Variable | Effect |
| --- | --- |
| `EOD_MOD_DIR` | Overlay root. `PROFILE.TXT` lines are logged at startup; `HOSTTEX/replacements.tsv` is loaded (case-insensitive lookup). |
| `EOD_HOST_TEXTURES=0` | Do not load replacements. |
| `EOD_HOST_TEXTURE_BUDGET_MB` | GPU residency budget for replacements (default 1536). |
| `EOD_HOST_TEXTURE_CPU_BUDGET_MB` | Bytes prefaulted into memory at startup (default 2048). |
| `EOD_HOST_TEXTURE_LOG=<file>` | Appends `indexKey paletteKey psm tw th tbp0 frame` (psm hex, tbp0 hex block address, GS frame) for every newly decoded indexed texture, once per key pair and address. Works without replacements. |

## Implementation

- `runtime/gs/texture_replacements.{h,cpp}`: manifest parsing, `.eodhtx`
  validation (magic, flags 0, full mip chain, exact size, `orig*k` with one
  power-of-two `k`, at most 4096), path safety (relative, no `..`), identity
  keys, alpha conversion, CPU residency. Files are memory-mapped; a background
  thread prefaults them in manifest order within the CPU budget. Malformed
  lines and missing files are logged with the reason and skipped. Lines with
  identical keys are reported as ambiguous; the earlier line wins. An exact
  `paletteKey` wins over `*`.
- `gs_vulkan.cpp` `vram_texture()`: only on a texture-cache miss, and only when
  replacements are loaded (and an entry has these native dimensions) or the key
  log is enabled, the mip-0 index plane is read from GS memory (PSMT8, PSMT4,
  PSMT8H, PSMT4HL, PSMT4HH) and the keys are computed. `paletteKey` hashes R,G,B
  of the expanded CLUT `e.pal` (byte 0 = R, as uploaded to R8G8B8A8).
- Alpha: replacement alpha is straight 0..255. If the native decoded texture's
  largest alpha is <= 0x80 the texture is in the GS domain and replacement alpha
  becomes `(a*128+127)/255`; otherwise it is used unchanged. The decision is
  logged per replacement. Anoka sky tiles decode with alpha 0x80 (GS domain).
- GPU: replacements are uploaded on first use through a dedicated staging
  buffer freed with its command buffer (`vk_compat.h UploadLevels`), so large
  uploads do not permanently grow the per-flight upload chunks. A separate LRU
  cache keyed by (entry, alpha domain) is bounded by the GPU budget; eviction
  skips textures used in the current frame and also drops every texture-cache
  entry borrowing the evicted view. Texture-cache entries hold their own
  reference, so the native 256 MB cache eviction never double-releases.
- Shader: `F_TEX_HOST` (0x8000). The bound texture is `k` times the GS size with
  `c_tex_scale = k` and `c_tex_size` in GS texels. The existing path would keep
  the GS mip level, which counts GS texels and would alias a 4096 texture, so the
  host path selects the level from the screen footprint (`SampleGrad`, or
  nearest `Load` at the footprint level), keeping the GS mag/min filter choice
  and wrap modes.
- Stats line (`--log gs`): `host textures N hits N misses N fallbacks, N uploads
  (ms) N evictions, N MB resident`. Misses count newly keyed textures with
  candidate dimensions and no entry.

## Limitations

- Direct-colour textures have no key (contract v1). Textures whose GS size
  differs from the TEX file size do not match.
- CLUT-animated textures: each palette is a different `paletteKey`; use `*` to
  bind one replacement for all palettes of an index plane.
- Atlases/sub-rectangles sampled from a larger GS texture are keyed by the GS
  texture as a whole.
- Upload happens on the GS thread at first use (measured ~23 ms CPU per
  4096x4096 chain), which can hitch the first frame showing new replacements.
- D3D11 renderer: not implemented.

## Verification (2026-11-10, Anoka, headless and muted)

`tools/tests/texture_replacements_test.cpp` (ctest `HostTextureReplacements`)
checks keys against `tools/texture_identity.py` reference values, header and
manifest validation, alpha conversion and the CPU budget. Native evidence used
a race capture (`EOD_GS_DUMP_AT=600`) replayed with `--gs-replay` for
deterministic comparisons: no overlay, `EOD_HOST_TEXTURES=0`, a wrong
`paletteKey` and the previous build are pixel-identical at render scale 1 and 4;
an identity (`k=1`) replacement differs by at most 2/255 per channel (host
sampling path), confirming orientation, palette and alpha domain.
