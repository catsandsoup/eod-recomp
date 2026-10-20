# NASCAR import: player success and evidence

## Player success, before implementation

“As a player using my own NASCAR: Dirt to Daytona and Test Drive: Eve of
Destruction discs, I can select an imported car, see its materials correctly,
complete a race with responsive handling and working damage, and purchase/save
it in Career. AI competitors use the same cars without missing assets or crashes.”

That requires visible original-reference comparisons, real driving and collision
checks, dealer purchase/save/reload, AI races and solo/local multiplayer checks.
An extracted archive, converted table, mesh bounds OBJ or clean startup alone
cannot prove those outcomes. Faithful native EOD rendering/timing remains the
repository priority; this overlay is an opt-in experiment.

## Supported tool scope

`tools/import_nascar.py` constructs six candidate CAR archives (stock, truck,
modified, dirt, hobby, pace car), synthetic liveries/thumbnails, and edited
COMMON/CT/UICAR resource overlays from owner-supplied inputs. Prices, menu stats,
driver names and liveries are mod choices, not an authenticated reproduction.
Source VALT values are preserved during conversion; previous guessed physics
key overrides were removed because cross-game units and key compatibility are
unverified. Retexturing targets body references while preserving cockpit/detail
materials. Model geometry and deformation remain in the source representation;
this tool does not implement VIF mesh compilation or prove EOD compatibility.

The importer checks required inputs and rejects overlapping source/output trees
before writing. It uses isolated temporary work directories and publishes a
complete candidate only after packaging succeeds. Choose a new or empty output
directory so older overlays cannot be mistaken for the new result.

```sh
python3 tools/import_nascar.py \
  --nascar-data /Volumes/SLUS_20441/DATA \
  --eod-data "$HOME/Library/Application Support/TDEODRecomp/disc/DATA" \
  --output mods/nascar-candidate
```

Track import is **not implemented**. The optional `--export-research-spline`
attempts to serialize a 19-to-15-float candidate into `research/`, outside the
mounted DATA tree; actual Daytona data currently fails the strict layout check. NASCAR field meanings, pointers, units and EOD compatibility remain
unverified. The importer no longer inserts a Daytona line into unrelated Afton
geometry. Existing older `mods/nascar/DATA/TRACKS/AFTON.TRK` files were not rebuilt
or validated by this change; use a fresh output directory.

`tools/trk.py` preserves unchanged EOD ILIN node bytes, including packed pointer
bits, and rejects malformed offsets/node sizes. OBJ export uses the EOD planar
X/Z coordinates at offsets +0/+4, with elevation left at zero; next-pointer data
at +20 is not a coordinate. This agrees with the native reader documented in
`src/battle_navigation.cpp`. Collision SOL records remain an experimental view,
not verified surfaces. Track retexturing edits typed MODL texture fields only;
opaque GRF files are not byte-patched.

## Verification and STAR handoff — 2026-10-20

**Situation:** the recovered Antigravity conversation reported six vehicles and
track portability, but its checklist did not establish visible player acceptance.
An ignored candidate package and extraction workspace already existed locally.
The NASCAR source disc was initially unmounted; the owner mounted it during
the audit, allowing archive, table and model metadata checks to run.

**Task:** finish reproducible tooling, remove unsupported production claims, and
separate binary/package evidence from the unproven gameplay outcome.

**Action:** added portable preflight/livery/ILIN regression tests, explicit CLI
paths, isolated build workspaces and publication, preserved source physics,
limited material rebinding and made spline export opt-in research. Runtime tests
require `EOD_RUN_NASCAR_GAME=1`, use headless/muted execution and isolated memory
cards, and must be coordinated with other GPU workloads.

```sh
python3 -m unittest tools.tests.test_nascar_import -v
```

`NASCAR_DISC_DATA`, `EOD_DISC_DATA` and `NASCAR_MOD_DIR` can select fixtures.
Missing retail fixtures skip their own tests; they no longer suppress portable
checks. The real Daytona record extent is rejected because the old guessed
layout leaves 68 unaccounted bytes; this is a guard, not a conversion success. Existing package inspection checks six archive files, class membership,
section counts, unique vehicle numbers and generated texture entries. Package presence alone does not establish a rebuild or playable result.

**Result:** a fresh mounted-disc build succeeded at
`work/nascar-candidate-20261020`; its full suite ran 17 tests in 12.495 seconds
and passed with only the explicitly disabled runtime smoke test skipped. The
existing ignored package also passed structural inspection. Source STOCK table
names/pairs survive explicit conversion unchanged; conversion leaves the source
model untouched. Eight
retail EOD Afton ILIN files independently round-tripped byte for byte; synthetic
checks cover mixed pointer/NaN bits and position export. No native game or C++ build workload
was run by the NASCAR audit. Vehicle appearance, mesh/VIF compatibility, deformation,
physics fidelity, Career persistence, AI driving and playable NASCAR circuits
remain unverified. Generated retail-derived archives, extractions and binaries
are deliberately excluded from Git.

**Open questions:** are the two games' model VIF packets, damage skeletons and
physics schemas compatible without semantic translation? Which NASCAR ILIN
fields are pointers, coordinates and event metadata? Does every imported chassis
have all dependencies expected by EOD's loader?

**Next decisive test:** inspect all six fresh archive dependencies, then run a coordinated
headless/muted overlay smoke test. Compare one car's visible materials and damage
against both originals, complete a real race and Career purchase/save/reload,
and only then expand acceptance across all types and multiplayer. A playable
track requires matching geometry/collision/event data and a verified semantic
spline translation; truncating float records is not that translation.
