# Texture batch export

`tools/texture_export.py` writes mip zero of native TEX members as PNG plus an
`export-manifest.json` (contract: `docs/CREATION_KIT_TEXTURE_CONTRACTS.md` section 3).
`tools/asset_registry.py` produces the underlying census of every archive and
member. Both only read the disc and refuse destinations inside it.

```
python3 tools/asset_registry.py --disc DISC --out census.json [--jobs N]
python3 tools/texture_export.py --disc DISC --dest DIR
    [--select GLOB ...] [--archive PATH ...] [--role ROLE] [--dry-run] [--resume] [--jobs N]
python3 tools/texture_export.py --reimport-check DIR/export-manifest.json ASSETID edited.png
```

- `--select` is a case-insensitive glob on `assetId` (`DATA/TRACKS/ANOKA.TRK#sky*`);
  repeated values are a union. `--archive` takes `DATA/...` or a path relative to
  `DATA`. `--archive`, `--role` and `--select` combine with AND.
- Progress is JSON lines on stderr (`start`, `progress` with `done`/`total`/`assetId`,
  `finish`); the final summary is JSON on stdout. Exit status 0 all exported, 1 some
  failed or unsupported, 2 refused, 130/143 after SIGINT/SIGTERM.
- The manifest is checkpointed atomically (every 50 assets or 2 s). After Ctrl-C,
  SIGTERM, a crash or `kill -9`, `--resume` re-verifies finished outputs by hash, skips
  them, and continues. Records from earlier runs outside the current selection are kept.
- A destination holding a manifest requires `--resume`. An existing file whose content
  differs is never overwritten, except for the same asset (same output path) on `--resume`.
  Output paths are checked case-folded and Unicode-normalised; all colliding assets are
  refused rather than letting order choose a winner. Symlinked path components are refused.
- Library: `texture_export.export_batch(disc, dest, ..., progress=callback,
  cancel=threading.Event())` returns the manifest dict; `classify_reimport(manifest,
  assetId, png)` reports eligibility only (it writes nothing):
  `native-same-size` needs equal dimensions and a natively editable indexed TEX;
  `host-replacement` needs an indexed TEX (runtime keys) and `origW*k` by `origH*k` with
  `k` a power of two and each side at most 4096.

## Manifest additions beyond the contract

`run` (`status` running/complete/cancelled, totals, resumed count, selection), `updated`,
per-asset `roleReason`, `decode`, `bitsPerPixel`, `crc`, `written`
(`written|unchanged|overwritten|skipped-resumed`) and `indexKeyShared` (same indexKey,
different palette). `runtimeKeyShared` lists assets with the same indexKey and paletteKey
(a binding would be ambiguous). `alpha` also has `straight-0-255-source` for 32-bit
direct-colour TEX; the contract names only two values.

## Retail census (this disc)

72 archives (37 CAR, 20 RES, 15 TRK), 7,329 members, 4,923 TEX (all decode), 54 other files
under `DATA` (video, audio, IOP modules) that are not 0TSR. 21 members fail their directory
CRC (`roadrace.ili` in 13 tracks and 8 `HOMEBASE.TRK` option files); they are recorded,
not rejected. 4,920 TEX are indexed (4-bit 2,946, 8-bit 1,974), two are RGB555, one is RGBA8.
Raster-looking types the exporter does not cover: IMAG 264, GTEX 62, CANV 35, STMP 4 (name
guess), ICON 1; none of their formats has been analysed.

## Resolution recommendation per role

Native sizes are small (sky 64-256, track mostly 128-256, vehicle paint 256x128 and 512x256),
so a uniform 4K is never needed. Memory for an RGBA8 mip chain: 4096x4096 = 64 MiB for mip0
(about 85 MiB with the full chain); 2048x2048 = 16 MiB (21 MiB); 1024x1024 = 4 MiB; 512x512 = 1 MiB.
Use the smallest power-of-two scale that fixes visible blur; textures are not guaranteed to be
sampled at their full size in game.

| Role (native sizes) | Start at | Ceiling | Reasoning |
|---|---|---|---|
| sky (64-256 tiles, 448 files) | 4x (256-1024) | 8x only for the 256 tiles visible near the horizon | 448 tiles at 1024x1024 RGBA8 is 1.75 GiB before mips, so scale only tiles that are visibly soft. |
| vehicle-paint (256x128, 512x256) | 2x-4x (1024x512 or 2048x1024) | 4x of the 512x256 body maps | One car on screen (2-4 with split screen/AI); 2048x1024 is 8 MiB. Keep body, glass and lights consistent in scale. |
| vehicle (parts, 64-512) | 2x-4x | 4x | Small parts gain little; match the paint scale. |
| track (128-256, 2,998 files) | 2x (256-512) | 4x for road surface and hero walls | Thousands of textures: 2x everywhere is about 4x the memory of the originals, 4x is about 16x. |
| ui / font (16-256) | 2x-4x, nearest-neighbour style unless redrawn | 4x | Pixel-aligned layouts; larger sources do not help unless the UI is redrawn. |
| other | 2x | 4x | Unclassified, treat conservatively. |

Role is a name-based guess with a confidence value in the manifest; only `sky` (track member
`skyNNA|B.tex`) is high. Direct-colour TEX (3 here) and non-indexed assets cannot be host
replaced in contract v1. Replacements are capped at 4096 per side; validate any 4x or larger batch in game before extending it.
