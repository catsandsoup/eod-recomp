# Custom vehicle and asset tools

## Player acceptance, defined before this recovery

The player can select a renamed, retextured car with changed specs in Arcade and
Career in its chosen native class (Midsize by default), navigate other classes
without a panic, and race against AI
using that chassis. Required evidence is the built resource tables plus a current
headless startup/selection/race test and visible, hands-on menu/driving checks.
Asset tests alone do not establish this experience. Original retail assets remain
on the owner's computer and must never be committed.

## Supported workflow

```sh
python3 tools/create_custom_car.py \
  --disc-data "$HOME/Library/Application Support/TDEODRecomp/disc/DATA" \
  --output mods/phantom --name Phantom --base Algar --class midsize
EOD_MOD_DIR="$PWD/mods/phantom" ./build-mac/eod \
  --data "$HOME/Library/Application Support/TDEODRecomp/disc"
```

`EOD_MOD_DIR` explicitly selects a single overlay. Only enable it when testing the
package; normal native baseline testing should leave it unset. Do not combine
independently generated COMMON.RES overlays: each replaces the entire archive.

The native chassis-class lookup `f_00164218` tests Compact, then Midsize,
Muscle and Special; each successful membership branches directly to its return
(`gen/rec_016.c`, native returns 0/1/2/3). Registering one chassis name in three
lists therefore silently makes it Compact. The builder now accepts **exactly
one class**, defaults to Midsize to address the original P0 carousel request,
and the GUI uses a single class choice. To target other classes, generate a
separate uniquely named chassis variant with `--class compact` or `--class muscle`
and select its overlay independently. Independently built COMMON.RES files
cannot be combined by copying over one another. Supporting all three classes
simultaneously would require one coordinated overlay containing distinct chassis
identities; that broader multi-variant workflow is not implemented.

The builder clones the retail chassis geometry, generates a procedural livery,
rebinds its verified retail body texture identifiers, changes physics, adds class/stats/dealer/AI records, and
packages PHANTOM.CAR, COMMON.RES, CT.RES and UICAR.RES. It builds in a temporary
directory before copying a successful package into the selected output directory.
It checks racing-number collisions against retail drivers/dealer entries; the
selected number and the adjacent dealer number must both be free. Default racer
number is 777 and dealer number 778. Body/thumbnail resource IDs follow the chosen
racer number. Existing AI racers retain their own unique racing numbers.
The paint carousel previews use each actual driver/dealer number (for the
default package, 199, 296, 777 and 778); all receive custom UICAR previews.
The preview is a procedural number-card placeholder with the custom livery
colors and number, not an authored car cutout or rendered model thumbnail.
The Basher field in the selection screen is the player nickname, independent
of chassis and paint selection.

`python3 tools/eod_mod_gui.py` exposes name, base chassis, class selection,
performance, body color, stripe style, price, and description. The body-material
map is checked against actual Low1.mod fields (for example Ocelot uses ocey.tex),
with cockpit/engine/detail textures preserved. Exterior Low1–Low4 require actual
replacements; cockpit-only Low0 models can remain unchanged. All actual Low*.mod
files are examined; Prince distant Low5/Low6 use the verified arrow.tex body alias. It requires a Python
installation with Tk. UI worker results return through a main-thread queue.
The Gameplay tab writes recognized runtime keys to the native macOS configuration,
`~/Library/Application Support/TDEODRecomp/eod.ini` (or `build-mac/eod.ini` on other
platforms). It displays that path; changes apply on the next launch. The Career money action asks the player to choose a save and close the game.
It calls the existing native `--save-edit` CLI on a disposable copy, verifies
checksum and career presence, edits money, independently reloads and verifies
the result, then keeps an original backup and atomically replaces the selected
save. Invalid/missing-career inputs or failed verification preserve the original.
Barrier removal/reload-delay controls remain disabled.
The GUI has import/config smoke coverage, but this recovery has not visually
validated the native Tk window or its complete interaction flow.

## Format coverage and limits

| Tool | Verified scope | Limit |
| --- | --- | --- |
| `rst0.py` | EOD 0TSR v6 archive extraction/packing; CRC; safe resized-entry layout | Unedited v6 roundtrip preserves original non-payload bytes in manifest `original_layout` and can be byte exact. Edited archives are re-laid out; v2 packing is explicitly unsupported. |
| `valt.py` | EOD v11 section/key/value parsing and compilation | Logical equivalence, not byte-exact string-pool layout. v6 parsing exists; v6 compilation rejects explicitly. |
| `tex.py` | Indexed PS2 compact-upload textures with native linear small-mip tails; RGBA PNG conversion | Dimensions must be powers of two from 16 to 1024, with height≤2×width; taller shapes reject as unverified. PNG input must be noninterlaced 8-bit RGBA. Compilation quantizes to 256 colors, regenerates mips; it is not a lossless general texture compiler. Visual retail parity remains unverified. |
| `modl.py` | MODL metadata, bounded texture-field edits, JSON export, bounds OBJ | OBJ contains one bounding box, not decoded VIF mesh geometry. No arbitrary mesh importer/compiler exists. |
| `audio.py` | Mono SFX/WAV, SPU2 ADPCM codec, speech bank interfaces, stereo PSS extractor | ADPCM is lossy. Rate conversion is rejected; resample externally. Speech compilation requires mono 22050-Hz cues. General PSS demux parity is not established. |
| `trk.py` | EOD ILIN lines and SOBL collision inspection; see NASCAR handoff | No full track geometry compiler or proven cross-game geometry/physics conversion. |

See TEXTURE_FORMAT_NOTES.md for the repaired native upload contract, retail
header/reference-pixel evidence, and independent CPU GS oracle.

The tools preserve existing SPS4/VIF geometry bytes when retexturing. They do
**not** satisfy the original request for a general, 1:1 mesh decompiler/compiler.
A bounds OBJ must never be presented as the game's actual exported mesh.

The original package draft incorrectly described `catalog.val` as a vehicle
name/biography catalog. Inspection of the retail table finds nine upgrade
sections (engine, suspension, exhaust across three classes), with no per-car
identity schema. The supported vehicle identity and dealer biography are the
existing `chassis` and `desc` fields of `buycar.val` car records; the builder writes
them and regression tests assert their contents. LOCALE.RES contains no Algar
name mapping. Custom chassis names are limited to eight characters for native
ISO lookup; long retail base names use the native six-character `_1` mapping.
Full visible menu display and AI behavior still require runtime evidence.

## Reuse references

The recovery retains the existing reverse-engineered tools rather than introducing
an unrelated asset pipeline. SPU2 filter coefficients match
`runtime/iop/spu2.cpp` (`kFilter`). Upstream
[RoadTripAdventure-recomp](https://github.com/silentsudin/RoadTripAdventure-recomp)
was inspected at `0f6d3613e8e32ccda5305227a578b1fb659983a8`: its texture-pack
interfaces do not implement EOD's MGI/0TSR format or virtual disc registration.
Context7/recovery and renderer references are recorded in RENDERER_REFERENCES.md.

## STAR recovery handoff — 2026-10-20

**Situation.** The recovered Antigravity work contained usable parsers and a
custom-car draft, but it overstated geometry support. The packer reused stale
entry offsets after resize, and the GUI ignored its custom settings, imported
nonexistent track functions, reported a fake save edit, and wrote unknown INI keys.

**Task.** Make the existing asset workflow reproducible and honest, retain source
assets locally, and leave concrete player acceptance separate from parser tests.

**Action.** Recompute edited archive offsets; preserve unchanged v6 layout; reject
unsupported versions/invalid names; validate image/audio input; fix tall texture
swizzling; parameterize and stage custom builds; fix GUI API calls and thread use;
wire recognized configuration keys, reuse the native save editor for career
money, and disable unsupported actions. Tests generate
their own temporary Phantom package from the owner's retail disc. Synthetic
regressions run even when no retail disc is present.

**Result.** CPU checks cover archive entry growth, exact unchanged ALGAR.CAR
roundtrip, logical VALT roundtrip, texture pixel preservation, model texture
bindings, generated physics/classes/stats/dealer/AI tables, bounds export, real
start.sfx conversion, and track inspection. Run:

```sh
python3 -m unittest tools.tests.test_custom_car -v
```

Two game workloads require explicit `EOD_RUN_GAME_TESTS=1`; they run headless and
muted and must be coordinated with other game/build/GPU work. The asset recovery
itself does not claim a new full-game or visual acceptance result. The next
decisive test is a current generated package reaching the Midsize carousel and a selected-Phantom
race in Arcade/Career, followed by visible selection and driving/AI verification.

## Rendered checkpoint — 2026-10-20

A freshly generated Midsize package using the repaired TEX encoder passed the
6,000-vblank normal-input selection/race journey in 100.321 seconds. Rendered
frame 3,000 shows MIDSIZE / PHANTOM, stats 9/8/8, and the clean custom 777
number-card preview. Frames 4,950 and 6,000 show the selected native-chassis
player car with the generated black/white livery in a 14-car race. The game
loaded Phantom.car and logged the expected mass 3200; no missing-car or duplicate
number panic occurred. The automated route is in test_08. A subsequent 6,000-vblank run passed in
100.227 seconds with an additional RAM assertion: exactly one human-controlled
car (type 0x50) has chassis Phantom at car+0x224, matching its parameter name
at u32[car+8]+0xAC. The assertion iterates the native world roster rather than
assuming the human occupies slot zero. This distinguishes player selection
from merely loading a Phantom AI. Menu and race images were inspected
separately; these local captures and RAM are not committed.

This bounded result establishes selection, race startup and visible custom
textures. It does not establish a complete race, driving feel, every LOD distance,
Career purchase/save/reload, every class or every mode.
