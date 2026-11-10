# Mod packages and development profiles

Implements sections 5 and 6 of `CREATION_KIT_TEXTURE_CONTRACTS.md`.
Tools: `tools/mod_package.py` (build/verify/zip), `tools/mod_install.py` (profiles).
Both print JSON; failures are `{"ok": false, "error": "..."}` with exit code 1.
The same functions are importable (dict results, `ValueError` with user-facing text).

## Package commands

    mod_package.py from-project PROJECT.eodproject --disc DISC --id ID --version V --out DIR [--name N]
    mod_package.py init DIR --disc DISC --id ID --version V
    mod_package.py add-host DIR --disc DISC --asset 'DATA/TRACKS/X.TRK#ground.tex' --image big.png [--any-palette]
    mod_package.py inspect|verify DIR_OR_EODMOD [--disc DISC]
    mod_package.py zip DIR OUT.eodmod  |  unzip IN.eodmod DIR

`from-project` diffs each built overlay archive against retail per member and stores only changed
members (with source archive/member hashes); it reports whether re-composing reproduces the project's
build byte for byte. `add-host` requires an indexed 4/8-bit retail TEX and a PNG of orig*k pixels
(k a power of two, at most 4096/side); keys come from `texture_identity`. Direct-colour textures are refused.
Unzip rejects traversal, absolute paths, symlinks, case-folded duplicates and hash mismatches.

## Profile commands

    mod_install.py install PKG --disc DISC [--profile NAME] [--root DIR]
    mod_install.py enable|disable|uninstall ID | order ID... | accept-conflict 'ARCHIVE#MEMBER' WINNER
    mod_install.py status | compose | launch-env

Default root `~/Library/Application Support/EODCreationKit/profiles`; default profile `default`.
`--disc` is stored in `profile.json` at first install. Every mutating command takes the profile lock,
builds a new `active/` in a sibling temp folder, validates it and swaps it by rename, then writes
`profile.json`; any failure restores the previous `active/` and `profile.json`.

- Each compose re-verifies installed package hashes, the SLUS hash and retail source hashes.
- Native members are replaced per member in the retail archive: same size patches payload and
  directory CRC in place, otherwise the archive is repacked by `rst0`. All other members are
  verified byte-identical.
- The same member with different bytes in two enabled packages blocks (nothing changes) until
  `accept-conflict` names a winner. The acceptance covers the byte variants seen when it was made;
  a new variant blocks again. Result does not depend on `order`.
- Host entries become `active/HOSTTEX/replacements.tsv` plus `.eodhtx` files (RGBA8, full mip chain,
  2x2 box filter per channel on straight alpha). `active/PROFILE.TXT` lists packages and output hashes.
- Uninstall deletes only files recorded as owned inside the profile root and prunes emptied folders.

Run the game with `EOD_MOD_DIR` from `launch-env`.
