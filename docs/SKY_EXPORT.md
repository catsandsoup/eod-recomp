# Track sky tile sets (`tools/sky_export.py`)

Player success: open a track's sky as one image, edit it (or edit tiles), and get
back per-tile images that land in the right place in the native game, at native
size or as `k` times host replacements. Status per track: see the registry below.

## Native sky topology (evidence)

| Fact | Evidence | Confidence |
| --- | --- | --- |
| Every retail `track.grf` except `EMPTYT_1` has exactly one type-4 leaf whose display record has type 1 / subtype 4 ("Sky"). | ELF table 0x406B40 (12-byte entries: name, constructor, size): 4 = Sky, ctor 0x221E00; scan of all 15 TRK files. | high |
| Record+8 is a record-relative offset to an embedded MODL (Anoka: record 0x15928 + 0x209A80 = 0x21F3A8). MODL word0 = part count, word1 = part-table offset. | `f_00214258` (rec_046.c) relocates record+8 relative to the record before `f_0034B530`; decoded table yields 4SPS parts. | high |
| The model is drawn at the camera position with identity rotation, so directions from the model origin are view directions. | `f_00221E00` writes an identity 3x3 at +0x0C; `f_00223858` copies camera+36..44 to +0x30 each frame. | high |
| Per-part cull: record+0x2C holds 32 cones (dir xyz, half-angle rad) indexed by 4SPS group; a group is drawn when `dot(camFwd, dir) > cos(angle + 0.625*viewParam)`. | `f_00223858` loop, `f_0039BB28` = cosf (pi/4 kernel split), `f_002111D0` viewport float. Each A tile lies inside its cone; B tiles exceed by at most 8 degrees (covered by the margin). | high |
| 16 azimuth sectors of ~22.5 degrees. `skyNNA` (128x128) is the horizon band, `skyNNB` (64x64) the cap wedge to the zenith. | Decoded mesh: Anoka A el 0.4..30.1, B el 30.1..90; A is a 3x3 quad grid, B is two quad rows plus a fan whose apex is texel (0,0). | high |
| Seams: NN.u1 = (NN+1).u0 (16 wraps to 01) for A and B; A.v0 = B.v1 of the same number with identical u along the seam. | Shared vertex positions; texel continuity across these seams equals within-tile neighbour difference (A 1.01 vs 1.14, B 0.34 vs 0.29) while the reversed pairing is worse (1.66, 1.27). | high |
| B uses only the part of its texture where v >= u/3 (about 16% of texels unused: 672 of 4096). | UV triangles. | high |
| Anoka view: azimuth atan2(x, z) increases to the viewer's right; tile u runs right-to-left on screen, so tile art appears mirrored and numbers decrease left to right (01A, 16A, 15A, 14A). | Native k=32 labelled host-replacement run (hosttex `x32_rs1/first-race.png`), consistent with model space +x right / +z forward. | high (Anoka) |
| `sky12B` and `sky13B` (Anoka) are byte-identical, so they share runtime keys. | `duplicateOf` / `runtimeKeyShared`; runtime key log. | high |

Other parts in the sky model (group 0, e.g. Anoka `treew4*.tex`) are horizon cards
at el -1..5 degrees; they are listed as `nonTileParts` and not exported as tiles.
The VU program for all sky parts is MSCAL 78; positions/UVs decode as exact grids,
normals in that layout are not meaningful and are not used.

## Commands

```
python3 -I tools/sky_export.py export   --disc DISC --track DATA/TRACKS/ANOKA.TRK --dest OUT [--panorama-width N]
python3 -I tools/sky_export.py reimport --manifest OUT/sky-manifest.json (--from-tiles DIR | --from-panorama PNG) --out DIR [--k K]
python3 -I tools/sky_export.py testart  --manifest OUT/sky-manifest.json --out DIR [--k 32]
python3 -I tools/sky_export.py registry --disc DISC --out registry.json
```

`export` writes `tiles/<member>.png` (lossless decode of mip 0, identical to
`texture_preview.decode_preview`), `sky-manifest.json` and `panorama.png`.
Per tile: contract identity fields (`assetId`, hashes, `crcOk`, header, `indexKey`,
`paletteKey`, `duplicateOf`, `runtimeKeyShared`), the decoded mesh (positions + uv),
`geometry` (azimuth at u0/u1, elevation range and at v0/v1, `upEdge`, `uvRect`,
`uvCoverage`, `apexAtV0`, native cull cone) and `neighbours` per edge
(`u0,u1,v0,v1` -> tile, edge, seam-parameter delta). `topologyChecks` records each
check; `topology` is `ring16-horizon-band-A-plus-cap-B` only if all pass.

Panorama (preview, resampled, lossy): equirectangular upper hemisphere, default
width = sum of A widths (2048 for Anoka, one texel per pixel horizontally) x
width/4, elevation 90 (top) to 0 (bottom), linear rows. x = 0 is the low-azimuth
edge of sky01A and x increases with azimuth (player view, mirrored relative to
the tile PNGs). Each pixel is ray-cast into the decoded triangles; the texture is
sampled bilinearly, clamped to the tile. Alpha 0 = no sky (Anoka: below 0.39 deg).
Native filtering/wrap state is not modelled.

Reimport:
- `--from-tiles`: native-size PNGs are passed through unchanged (lossless);
  `k`x inputs are box-averaged to native (lossy). `--k K` also writes `kK/` tiles,
  nearest-neighbour upscaled from native input or passed through from `K`x input.
- `--from-panorama`: every texel centre inside a UV triangle is mapped to its 3D
  point and sampled from the panorama (bilinear, azimuth wrap, alpha-0 pixels get
  no weight). Unused texels (B corner) keep the original. Lossy. With `--k`, tiles
  are sampled at the largest power of two <= k the panorama width supports
  (width / default width), then nearest-upscaled.
- `sky-reimport.json` lists files, hashes, `changedTexels`, `nativeColours` and
  whether the native output fits the 256-colour indexed import path.
- Measured Anoka roundtrip tiles -> panorama -> tiles: mean absolute error
  0.18/255 per channel (mean of tile means), max 42 (isolated stars).

Test artwork (`testart`): per tile, hue by sector number, 8 value bands along v,
borders v0 yellow, v1 blue, u0 red, u1 green, label `NNA/NNB`, black UP arrow
toward the zenith side, white arrow toward +u, decoded seam neighbours beside the
red/green borders, `K<k>` only on host-size files. Native files use 14 colours.
`testart-panorama.png` shows the expected player view.

Not written by this tool: native TEX (use the existing same-size indexed import)
and `.eodhtx` host files (contract section 4).

## Environment registry

`registry` classifies values per track: `verified-editable` (edited and observed
natively), `editable-unverified`, `baked`, `unknown`, `absent`. Today only Anoka
sky tiles (native same-size edit and k=32 host replacement) are `verified-editable`; sky geometry and cull cones are
`baked`; `shadow.val` global brightness and fog are `unknown`; GRAF Sun / Ambient /
Shadow / SunBright / FogColor nodes are counted, with payload semantics not
decoded. No fog, ambient or HDR control is offered.

## Other tracks

The topology checks pass on 13 of 14 tracks with skies. Several (Afton, Dennison,
Hastings, Kenyon, Bemidji) have the dome off the camera origin, so per-tile
azimuth/elevation differ from Anoka; the panorama uses the decoded geometry.
Welch fails (`unverified`): its tiles also carry a mirrored lower hemisphere.
Only Anoka orientation is natively verified.
