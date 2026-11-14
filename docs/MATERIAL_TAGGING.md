# Material tagging for the modern renderer

## Player success

In the remastered renderer every surface reacts to light like what it is: dirt is
matte, chrome and bare steel reflect, painted metal has a coat, glass is glossy,
tyres are rubber, foliage and fences stay cut out, signs stay readable, and the
sky, crowds and fake shadow decals are never shaded as solid materials. No
surface shows an obviously wrong material in a drive around any track.

Deliverable: one machine-readable material table (`material_table.json`, one
record per unique texture) that the renderer and the look test read, plus a
**replacement index** ranking which textures most need new artwork.

## Scope (census, 13 Nov 2026)

| Source | Unique textures | Notes |
|---|---|---|
| 15 `.TRK` world exports (incl. HomeBase) | 1,859 | 344 shared across tracks |
| 37 `.CAR` exports | 52 | one livery per car only |
| **Total (deduplicated by pixels)** | **1,911** | 2,602 references; 540 with cut-out alpha |

Sizes: 2 at 16 px, 54 at 32 px, 441 at 64 px, 1,318 at 128 px, 62 at 256 px, 34 at 512 px.

Known gaps to close in step 0: the remaining car liveries (`CT.RES` paint
`.tm0` images, one per driver) and `RES` prop models that are not placed by
`track.grf`.

## Label schema (one record per texture)

| Field | Values |
|---|---|
| `surface` | asphalt, gravel, dirt, mud, sand, rock, grass_ground, foliage_cutout, bark, wood, concrete, brick, metal_bare, metal_painted, metal_rusty, chrome, rubber, glass, car_paint, car_interior, fabric, plastic, water, sky, sign_decal, crowd_sprite, emissive_light, fake_shadow, fx, mixed_atlas, other |
| `metalness`, `roughness` | 0–1 |
| `detail` | CC0 detail material to layer, or `none` |
| `alpha` | opaque, cutout, blend |
| `emissive` | true/false |
| `baked_lighting` | none, light, heavy (shadows or highlights painted into the texture) |
| `regions` | for `mixed_atlas`: a short list of the materials in it and where |
| `confidence` | 0–1 |
| `reason` | one line of evidence |

## Method

### Votes per texture

1. **Tagger A** (Sonnet) and **Tagger B** (Haiku) label each contact sheet
   independently. Each sheet holds 36 labelled tiles (alpha shown on a checker)
   plus a JSON sidecar with names, owners, size, alpha and geometry context.
2. **Rule vote** (no agent): the name/geometry heuristic the look test already
   uses, extended with the geometry features below. It is free and fully
   reproducible, and it breaks ties.
3. **Judge** (main model) sees only disputed textures: both proposals, the rule
   vote, the tile and the geometry context, and decides or sends the texture to
   human review.

Agreement rule: A and B agree on `surface` (and metalness within 0.35) → accept
and average the numbers. Otherwise → judge. A judge confidence below 0.6 →
`needs_human`.

### Fallback evidence for hard textures

These are computed deterministically from the exported OBJ files, not guessed:

- **Orientation**: share of each texture's surface area facing up, sideways or down
  (ground versus wall versus underside).
- **Size in the world**: total area in square metres, height above the track, and
  the names of objects/parts that use it (e.g. car part `C_Detail`, world
  object group).
- **Texel density**: texels per metre on the surface = √(texels × UV area ÷
  world area). This also drives the replacement index.
- **Pixel statistics**: mean colour, saturation, contrast and alpha coverage.
  Very low saturation and high contrast suggest metal or concrete; green with
  noise suggests grass.
- **In-context render** (second pass, hard cases only): render the meshes that
  use the texture in the look test with the proposed material and attach the
  crop to the judge.
- **Human review queue**: whatever remains goes to the owner as a short contact
  sheet in the Kit, never silently defaulted.

## Pass/fail gates

| Gate | Pass | Fail action |
|---|---|---|
| Coverage | 1,911/1,911 records, all schema-valid | re-run missing sheets |
| Gold accuracy (surface) | ≥ 90% on the gold set | fix the prompt or schema, re-run the pilot |
| Gold accuracy (metal yes/no) | ≥ 95% | same |
| Tagger agreement | ≥ 75% per sheet on `surface` | re-run that sheet once, then judge all of it |
| Calibration | records with confidence ≥ 0.8 are ≥ 95% right on gold | treat confidence as advisory and send more to the judge |
| Human queue size | ≤ 5% of textures | improve evidence (in-context renders) before handing over |
| Visual check | look test on a day dirt track, a night track and HomeBase shows no obviously wrong material (sky shaded as metal, signs shaded as dirt, crowds lit as solid) | fix the records, add them to gold |

The gold set is 60 textures chosen across every class and the hard cases
(atlases, baked shadows, tiny, sprites). It is labelled by the main model with
pixel and geometry evidence and **confirmed by the owner** before it is used as
the reference. Until then gold accuracy is reported as provisional.

## Run order

0. Close the census gaps (liveries, `RES` props); compute geometry and pixel
   features; add them to each sheet's sidecar.
1. **Pilot**: 3 sheets (one track mix, one sky/crowd/sign mix, one car sheet) plus
   the gold set. Measure all gates. Fix prompts. No full run until the pilot passes.
2. Full run over all sheets (≈ 54 sheets × 2 taggers + 1 judge per sheet with
   disputes, about 110–160 agents).
3. Merge, apply gates, build `material_table.json` and the replacement index.
4. Visual check in the look test; fix records and fold them into gold.

## Replacement index

Every texture gets an entry in `replacement_index.json` with reason codes so we
know what to replace later:

- `LOW_DENSITY`: texels per metre below 64 on a surface the camera sees up close
- `LARGE_AREA`: in the top 10% of world area
- `TINY`: 32 px or smaller
- `MIXED_ATLAS`: several materials in one image (needs region masks or a split)
- `BAKED_LIGHTING`: painted shadows or highlights that fight real lighting
- `FAKE_SHADOW`: shadow decal to remove once real shadows exist
- `SPRITE`: crowds, foliage cards, flat props to replace with geometry
- `DISPUTED` / `NEEDS_HUMAN`: unresolved label

Priority = area × proximity × (1 ÷ texel density), so big, close, blurry
surfaces come first (the dirt under the cars is the obvious first case).

## Results (full run, 14 Nov 2026)

The census grew to **2,728** unique textures once `RES` props and all 554 driver
liveries were added (1,859 track, 52 car, 263 `RES`, 554 liveries), on 77
contact sheets. The method changed partway to save usage (owner-approved):

- Sheets 1-25 and the pilot (8, 10, 30, 53): two taggers (Sonnet, Haiku) plus a
  judge on disagreements, as designed above.
- Sheets 26-61: **one Sonnet tagger per sheet**; only its labels with
  confidence < 0.6 go to the judge. Run one sheet at a time.
- Sheets 62-77 (liveries): labelled `car_paint` by rule, no agents.

Decided by: 719 agreed, 468 judge, 981 single tagger, 554 rule, 6 owner gold.

| Gate | Pass | Measured | Verdict |
|---|---|---|---|
| Coverage | all records, schema-valid | 2,728 / 2,728, 0 missing fields | **PASS** |
| Gold accuracy (surface) | ≥ 90% | on the 54 owner-confirmed gold: table 48/54 = **88.9%**; outside the human queue 46/50 = 92.0% | **FAIL** by one texture (the 6 misses are now fixed in the table from gold) |
| Gold accuracy (metal yes/no) | ≥ 95% | confirmed gold: 51/54 = **94.4%** | **FAIL** (narrowly) |
| Tagger agreement | ≥ 75% per sheet | 16 of 27 two-tagger sheets pass (range 50-100%); not measurable on single-tagger sheets | **FAIL** on 11 sheets (all their disputes were judged) |
| Calibration | conf ≥ 0.8 is ≥ 95% right on gold | confirmed gold: 31/32 = **96.9%** | **PASS** |
| Human queue | ≤ 5% | 318 = **11.7%** of all (14.7% of agent-tagged); all are judge verdicts below 0.6, mostly painted metal (84), concrete (42), wood (24), bare metal (21) | **FAIL** |
| Visual check | no obviously wrong material | Afton day, Anoka night, HomeBase day: no sky/sign/crowd shaded as a solid material after one renderer fix (below) | **PASS (provisional)** |

Gold check by the owner (14 Nov 2026): **54 confirmed right, 6 not sure, 0
wrong**; the 6 unsure are left out of scoring. The six misses on confirmed gold
(fig-85, btwd, Suspen, Spoke, cyl, h2o-2) are all metal versus wood, concrete or
backdrop, the same classes that fill the human queue; `merge.py` now applies
confirmed gold labels over the taggers (`decided_by: owner_gold`).

Visual-check finding: the PS2 draws light cones (`fade`), flares and dust as
additive cards. The tags call them `fx` / `emissive_light` correctly, but the
schema has **no blend-mode field**, so the look test first drew them as solid
glowing walls (Anoka night went white). The look test now hides tagged `fx`
and large emissive cards in the modern look, like fake shadows. The in-game
renderer should take blend mode from the captured GS state, not from the tag.
Also seen: day haze is strong on every track (lighting tuning, not materials),
and unlit backdrop cards read too bright at night.

Replacement index: 2,728 entries. Codes: `LOW_DENSITY` 1,080, `DISPUTED` 471,
`NEEDS_HUMAN` 320, `LARGE_AREA` 280, `BAKED_LIGHTING` 261, `SPRITE` 214,
`MIXED_ATLAS` 101, `FAKE_SHADOW` 94, `TINY` 93. The top of the list is the big
dirt fields and water (field4-6, we-fld04/05, water1), as predicted.

## STAR handoff

- **Situation**: the look test showed that once the lighting is modern, the
  2004 textures and models become the bottleneck, and lighting needs a material
  for every texture.
- **Task**: label all 2,728 unique textures and rank which ones to replace.
- **Action**: tagged all 77 sheets (two taggers + judge on sheets 1-25, one
  Sonnet tagger + judge on low confidence on 26-61, rule on liveries), merged
  into `material_table.json` and `replacement_index.json`, rendered the look
  test on Afton day, Anoka night and HomeBase. Data stays out of Git
  (disc-derived).
- **Result**: coverage and calibration pass; on owner-confirmed gold, surface
  88.9% and metal 94.4% narrowly fail, 11 sheets are below the agreement bar,
  and an 11.7% human queue fails. High-confidence labels (≥ 0.8) are reliable (97%); the weak spot is
  weathered metal versus wood/concrete on props. Next decisive steps: a cheap second pass on the 320 queued records
  with in-context renders (the planned fallback evidence) to bring the queue
  under 5%; add a blend-mode field (or take it from GS capture).
