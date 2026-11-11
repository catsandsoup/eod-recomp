# Format coverage ledger

Status of every native asset type on the retail disc against
[FORMAT_VIEW_CONTRACT.md](FORMAT_VIEW_CONTRACT.md). Nothing on the disc is
encrypted. Every member opens in the Creation Kit and through MCP as a labelled
view; a type without a decoder shows its byte ranges as named unknown blocks
(plus readable text), never a hex dump. Raw bytes are an expert toggle.

Source of the counts: the asset census (72 archives, 7,329 members, 24 types).
Descriptions are best current knowledge and are marked where unverified; the
older catalogue in `SEARCHABLE_FUNCTION_AND_ASSET_CATALOGUE.md` was written
before the census and some of its size and purpose notes disagree with it (see
LANG and XDSP).

Decoder status values:

- **labelled view**: dedicated preview or decoded fields in the app.
- **generic blocks**: contract-compliant fallback (named unknown blocks and text); read-only.
- **in progress**: a decoder module is being written; shown as generic blocks until it lands.

## Priority order

1. LANG, 2. SETU, 3. ILIN, 4. IMAG, 5. LAYT, 6. STAB, 7. open files whose
checksum differs, 8. SFX0 / ESFX / speech bank, 9. GTEX / CANV / STMP / ICON,
10. Anim / Anm0, 11. BOPT / XDSP, 12. BPPT / BSPT, then the types that already
have previews.

## Archive members, by priority

| Type | Count | Extensions | Example asset | What it is | Decoder status | Editable | Owner / next step | Proof |
|---|---:|---|---|---|---|---|---|---|
| LANG | 2 | .lng | `DATA/RES/LOCALE.RES#usenglish_lang.lng` (38,751 B) | All game text: 877 `Group:Name` entries (the file the game loads); `GENESIS.RES#english.lng` (325) is not referenced by the executable. Loader 0x001342A8, lookup 0x00134620. | labelled view (`tools/formats/lang.py`) | yes: text, incl. longer; `%d` and Latin-1 enforced; keys fixed | Native check: edited main-menu label on screen. | `docs/LANG_GAME_TEXT.md`; retail round trip; Kit save_format rebuild |
| SETU | 64 | .gar | `DATA/TRACKS/ANOKA.TRK#default.gar` (196 B) | Leftover car setup sheets (49 floats: per-wheel spring/shock/camber/pressure, gears, final drive, fuel) from the engine this game was built on. The retail game never loads them: no SETU request, no `.gar` name in the executable; driving comes from each car's `physics.val`. | labelled view (`tools/formats/setu.py`) with a plain "not used by the game" warning | yes, but no in-game effect | Handling edits belong in `physics.val` (VALT). Optional negative-control race. | `docs/SETU_CAR_SETUP.md`; executable search; 64/64 round trip |
| ILIN | 211 | .ild (AI line), .ili (ghost line) | `DATA/TRACKS/ANOKA.TRK#oval.ild` | `.ild` AI racing line (f_00137A68, f_0013D9A8); `.ili` ghost line with lap record (f_001174D0). 21 track `.ili` members are 0-byte placeholders with a stale checksum; they open empty. | labelled view + path preview (`tools/formats/ilin.py`) | yes: node x/z; stored tangents/lengths do not update; no add/remove | Native check: AI follows a moved line. | `docs/ILIN_RACING_LINES.md`; matches `trk.py`/world_export |
| IMAG | 264 | .img | `DATA/RES/UIACTION.RES#b-events.img` | Interface images: 1–20 frames of 8-bit indexed RGBA (loader 0x00349078/0x0034A0F8, max 512 px). | labelled view + image preview (`tools/formats/imag.py`) | yes: replace a frame with a same-size PNG | Native check: replaced menu button on screen. | `docs/IMAG_INTERFACE_IMAGES.md`; 264/264 round trip; contact sheet viewed |
| LAYT | 200 | .lyt | `DATA/RES/COMMON.RES#yesnocan.lyt` (1,952 B) | UI menu layouts (widgets, positions, text ids). Loader `f_00270430` / `f_00270F10`. | generic blocks | no | Decode widget records from the loader; table preview. | `docs/UI_MENU_ENGINE.md` |
| STAB | 89 | .tab (74), .obt (15) | `DATA/RES/COMMON.RES#ugly_x.tab` (538 B) | Lookup and string tables (e.g. `ranked_list.tab`, `camera.tab`, `Rtire.tab`, per-track `track.obt` object table, which differs between platforms). `Rtire.tab` is a 5x2 table, 0x4c header, 33-byte cells. | generic blocks | no | Decode as table preview; edit cells with length limits. | `docs/NASCAR_CUP_WHEELS.md`; `docs/MODDING_SCOPE.md` |
| CRC-mismatch open | 21 | .ili | `DATA/TRACKS/HOMEBASE.TRK#opt6.ili` | The 21 members whose directory checksum differs are all empty (0 bytes) `.ili` files with one shared placeholder checksum (`roadrace.ili` in 9 more tracks, HOMEBASE `opt2` to `opt8`, `opt_whp`). | done: open with a notice, never blocked | n/a (empty) | None. Saved edits to any member recompute and verify the checksum. | `tools/tests/test_formats_registry.py` |
| SFX0 | 112 | .sfx (87), .sf_ (25) | `DATA/RES/OVERLAYS.RES#rd_press.sfx` (1,120 B) | Sound effects, PS2 ADPCM. `tools/audio.py` converts `.sfx` to and from WAV. | generic blocks | via WAV tool only | Header fields and waveform preview in the view. | `docs/AUDIO_FORMAT_NOTES.md`; `tools/audio.py` |
| ESFX | 6 | .ens | `DATA/RES/PART.RES#eng1e.ens` (356 B) | Engine sound definitions. | generic blocks | no | Decode with `tools/nascar_engine_audio.py` knowledge. | `tools/nascar_engine_audio.py` |
| Speech bank | 1 file | `DATA/SPEECH.BNK` (81 MB) | Not an archive. Speech audio bank. | none (file kind: other) | no | Index and extract clips via `tools/audio.py`. | `tools/audio.py`; `docs/AUDIO_FORMAT_NOTES.md` |
| GTEX | 62 | .gtx | `DATA/RES/OVERLAYS.RES#overlay.gtx` (38,912 B) | UI font / glyph sheets. | generic blocks | no | Decode glyph widths; image preview. | Catalogue only |
| CANV | 35 | .cvs | `DATA/RES/GENESIS.RES#gc_icon.cvs` (4,132 B) | UI background canvases (catalogue: 512x512 full-screen images; the example is smaller, so sizes vary). | generic blocks | no | Image preview. | Catalogue; census |
| STMP | 4 | .stp | `DATA/RES/PAINTKIT.RES#base_layer.stp` (135,452 B) | Paint-kit stamp layers. | generic blocks | no | Image preview; relate to paint workflow. | Census |
| ICON | 1 | .icn | `DATA/RES/GENESIS.RES#mem_icon.icn` (75,736 B) | Memory card icon. | generic blocks | no | Model preview. | Census |
| Anim | 18 | .anm | `DATA/RES/RACE.RES#Drv_winA.anm` (4,128 B) | Driver animations. | generic blocks | no | Track table preview. | Census |
| Anm0 | 15 | .an0 | `DATA/RES/RACE.RES#spec_03A.an0` (30,776 B) | Spectator animations. | generic blocks | no | Track table preview. | Census |
| BOPT | 2 | .opt | `DATA/RES/GENESIS.RES#tune.opt` (612 B) | Option presets (`tune.opt`, `default.opt`). | generic blocks | no | Name option fields from the loader. | Census |
| XDSP | 1 | .dsp | `DATA/RES/GENESIS.RES#xtalk.dsp` (4,068 B) | Audio DSP configuration (catalogue says video timing; unverified). | generic blocks | no | Confirm from the loader. | Census |
| BPPT | 15 | .bpp | `DATA/TRACKS/AFTON.TRK#track.bpp` (168,316 B) | Track partition data (collision partitions). | generic blocks | no | See COLLISION_AND_BOUNDARIES.md. | `docs/COLLISION_AND_BOUNDARIES.md` |
| BSPT | 15 | .bsp | `DATA/TRACKS/AFTON.TRK#track.bsp` (108 B) | Track spatial partition tree. | generic blocks | no | See COLLISION_AND_BOUNDARIES.md. | `docs/COLLISION_AND_BOUNDARIES.md` |
| GRAF | 15 | .grf | `DATA/TRACKS/AFTON.TRK#track.grf` (3.4 MB) | World scene graph. Parsed by `tools/world_graf.py`; not yet wired into the app. | generic blocks | via world tools | Wire parser output into the view. | `tools/world_graf.py`; `docs/WORLD_FORMAT.md` |
| SOBL | 15 | .sol | `DATA/TRACKS/AFTON.TRK#track.sol` (361,786 B) | Track collision surfaces. Decoded in `tools/world_collision.py` and `tools/trk.py`; surface meaning partly unverified. | generic blocks | via collision tools | Wire into the view. | `tools/world_collision.py`; `docs/COLLISION_AND_BOUNDARIES.md` |
| SCOL | 15 | .bsc | `DATA/TRACKS/AFTON.TRK#track.bsc` (495,000 B) | Track collision boundaries. Decoded in `tools/world_collision.py`. | generic blocks | via collision tools | Wire into the view. | `tools/world_collision.py` |
| TEX | 4,923 | .tex (3,309), .tm0 (1,547), .art (67) | `DATA/CAR/AMBULA_1.CAR#lite.tex` | Textures; all 4,923 decode. | labelled view (image) | yes (PNG import) | None. | `tools/tex.py`; `docs/TEXTURE_FORMAT_NOTES.md`; census |
| VALT | 755 | .val (354), .va0 (338), .mvl (36), .pln (27) | `DATA/CAR/ALGAR.CAR#physics.val` | Named value tables (physics, drivers, effects). | labelled view (table) | yes (except v6) | None. | `tools/valt.py` |
| MODL | 490 | .mod | `DATA/CAR/ALGAR.CAR#shadow3.mod` | 3D models (VIF geometry). Models with no previewable geometry show generic blocks. | labelled view (3D) | texture and vertex patch only | None. | `tools/vif_mesh.py`; `docs/VIF_GEOMETRY_PATCHING.md` |

## Files outside archives

| Kind | Count | Example | What it is | Status |
|---|---:|---|---|---|
| Speech bank | 1 | `DATA/SPEECH.BNK` | Speech audio bank (81 MB) | not opened in the app; see Speech bank above |
| Movies | 40 | `DATA/VIDEO/ATTRACT.PSS`, `DATA/VIDEO/ANOKA.M2V` | PSS (MPEG-2 with audio) and M2V (video only) movies | not opened in the app; audio extraction in `tools/audio.py` |
| IOP modules | 13 | `DATA/IOP/PADMAN.IRX`, `DATA/IOP/IOPRP280.IMG` | PS2 IOP drivers and image; not game content | not opened in the app; `tools/irx.py` |

## Rules

- A type is "done" per the acceptance section of the format view contract.
- No-edit encode must be byte-identical for every instance before a type is marked editable.
- Update this table in the same change as a decoder.
