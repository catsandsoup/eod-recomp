# Creation Kit texture pipeline contracts (v1)

Frozen contracts shared by batch export, sky export, mod packaging/installation,
the GS host-replacement path and the Kit UI. Change only by coordinated revision
of this file; record the reason. Status of each capability lives in the
acceptance record, not here.

## 1. Asset identity

- `assetId` = `<archive>#<member>`: `archive` is the disc-relative path as
  spelled on disc (for example `DATA/TRACKS/ANOKA.TRK`), `member` the exact
  0TSR member name. Comparison is case-insensitive; original spelling is kept.
- Every record also carries `archiveSha256`, `memberSha256` (payload bytes),
  `type` (FourCC), `crcOk` (payload CRC vs directory) and, for TEX, header
  `width/height/levels/format/flags`.
- Duplicate payloads across archives remain separate assets. `duplicateOf`
  lists other assetIds with identical `memberSha256`; nothing is merged.
- `.tex` and `.tm0` members with one stem are separate assets; the record links
  them with `variantOf` (stem-level relationship only).

## 2. Runtime texture identity (tools/texture_identity.py)

For indexed 4/8-bit TEX:

- `indexKey`: FNV-1a 64 over `"EODTEX1"`, u8 bits, u32le width, u32le height,
  then one byte per mip-zero pixel (palette index) in row-major order.
- `paletteKey`: FNV-1a 64 over R,G,B of logical palette entries 0..n-1
  (n = 16 or 256). Alpha is excluded because native loading converts it.

The GS renderer computes the same keys from GS memory when it decodes a texture
(index from PSMT8/PSMT4 sampling, RGB from the active CLUT). Hex is lowercase,
16 digits. Identical keys across different assets are an *ambiguity* that the
exporter reports (`runtimeKeyShared`). Direct-colour TEX has no key in v1.

## 3. Export manifest (`export-manifest.json`, schemaVersion 1)

Written to a destination outside the disc. Top level: `schemaVersion`, `tool`,
`toolVersion`, `disc` (`slusSha256`), `created`, `summary` (counts by status),
`assets` (list). Asset fields: identity fields above plus `role`
(`sky|vehicle-paint|vehicle|track|ui|font|other`, with `roleConfidence`),
`status` (`exported|failed|unsupported`), `warnings`, `error`, `output`
(relative POSIX path of the mip-zero PNG), `outputSha256`, `decodedRgbaSha256`,
`alpha` (`straight-0-255-from-palette` or `opaque-rgb555`), `indexKey`,
`paletteKey`, `reimport` (`native-same-size` and/or `host-replacement`).
Output paths: `textures/<archive path>/<member>.png`, case-folded collision
check before writing; never overwrite a file whose hash differs unless resuming
the same asset.

## 4. Host replacement runtime format

Profile composition writes `HOSTTEX/` inside the single active overlay root:

- `HOSTTEX/replacements.tsv`, UTF-8. First line `eod-host-textures 1`. Each
  further line, tab-separated:
  `indexKey paletteKey|* origWidth origHeight file assetId packageId`.
- `file` is relative to `HOSTTEX/`, a `.eodhtx` image:
  16-byte header `"EODHTX1\0"`, u16le width, u16le height, u16le levels,
  u16le flags (0); then RGBA8 levels, mip 0 first, each `max(1,w>>i) *
  max(1,h>>i) * 4` bytes. Alpha is straight 0..255 (PNG convention); the
  renderer converts to the GS domain observed for the native texture.
- Replacement dimensions are `origWidth*k` by `origHeight*k` for one integer
  power-of-two `k >= 1`, at most 4096 per side. Levels cover the full chain.
- The renderer binds a replacement only on matching indexKey (and paletteKey
  unless `*`), keeps the native texture as fallback, and never changes GS
  memory, UV meaning or wrap/clamp modes. `EOD_HOST_TEXTURES=0` disables it.
- Diagnostics: the log names each loaded replacement (assetId, package,
  dimensions, bytes, hit/miss/fallback reason). `EOD_HOST_TEXTURE_LOG=<file>`
  appends every newly decoded indexed texture's keys/psm/size for identity
  verification.

## 5. Mod package (`eodmod.json`, schemaVersion 1)

A package is a directory (or `.eodmod` zip of it):

```
eodmod.json   native/<archive path>/<member>   host/<file>.png
```

`eodmod.json`: `schemaVersion`, `id` (`[a-z0-9][a-z0-9._-]{0,63}`), `version`
(semver), `name`, `game.slusSha256`, `dependencies` [{`id`, `minVersion`}],
`native` [{`archive`, `member`, `sourceArchiveSha256`, `sourceMemberSha256`,
`file`, `sha256`}], `host` [{`assetId`, `sourceMemberSha256`, `indexKey`,
`paletteKey`, `origWidth`, `origHeight`, `file`, `sha256`, `width`, `height`}].
All paths are relative POSIX, no `..`, no symlinks, no case-folded duplicates.

## 6. Development profile

Default root `~/Library/Application Support/EODCreationKit/profiles/<name>/`:

- `profile.json`: installed packages, `enabled`, explicit `order` (later wins
  for an explicitly accepted same-member conflict; otherwise conflicts block).
- `packages/<id>/<version>/`: owned, verified copies.
- `active/`: the composed overlay root passed to the game as `EOD_MOD_DIR`.
  Native archives are recomposed per member from the retail archive, so two
  packages editing different members of one archive coexist. `active/HOSTTEX/`
  per section 4. `active/PROFILE.TXT` lists profile, packages and hashes; the
  runtime logs it at startup.
- Publication is staged in a sibling temporary directory and swapped by rename;
  failures leave the previous `active/` intact. Uninstall removes only files
  recorded as owned.
