# Modder journeys (jobs-to-be-done) for the EOD Creation Kit

Status: product design, 2026-11-10. Not an acceptance record. Capability status
below is as understood from the repo at writing time:
`exists` = shipped in `tools/modding_backend.py` / Kit today;
`in progress` = being built by other workers (treat as a planned API, names may
move); `missing` = nobody is building it yet. Do not read "in progress" as
"works". Note: `docs/handoffs/2026-11-10-claude-opus/HANDOFF.md` was empty/absent
when this was written, so nothing here leans on it; texture contracts come from
`docs/CREATION_KIT_TEXTURE_CONTRACTS.md` and the backend from
`tools/modding_backend.py` (`catalog entries inspect environment prepare_texture
export_texture export_native save validate launch_plan`).

## 1. What modders in comparable scenes actually do

Patterns repeated across scenes (sources in section 5):

- **Extract, edit in a general DCC tool, repack.** FlatOut modders run a
  command-line W32/BGM tool to export cars and tracks to FBX, edit in Blender
  with documented import/export settings, then re-import; the tool's
  `-export_bgm` with no edits is used as a round-trip check. GTA San Andreas
  modders open `gta3.img` in an IMG tool, pull DFF/TXD, edit, and re-import;
  Blender add-ons (INU Tools, SA Map Tools) now cover model + collision +
  IDE/IPL placement in one place. Lesson: **byte-exact unchanged round trip, and
  a standard interchange format (OBJ/FBX/PNG), are what earn trust.**
- **Texture packs are the largest audience.** PCSX2 users dump textures,
  edit/upscale them and drop replacements in a per-game folder keyed by hash/name
  (hardware renderer only; people regularly hit "dumps but does not load" and
  duplicate/unstable names). Lesson: identity must be stable and the tool must
  say *why* a replacement did not bind (our HOSTTEX hit/miss log).
- **Placement and collision are edited as data tables, not meshes.** GTA IDE/IPL
  text and COL files; FlatOut track collision settings; Halo scenario tags
  edited in Sapien (placement) and Guerilla (tag fields) then compiled by Tool.
  Lesson: **expose placements and collision as structured records with a
  viewer**, and keep "compile/pack" a separate, repeatable step.
- **In-game tuning loops.** FlatOut 2 modders use an in-game DB editor (F5) to
  tweak handling/mass/grip live. Lesson: the value is the *edit-run-look* loop
  time, not the editor itself.
- **Mod managers solve distribution, not authoring.** Mod Organizer 2 uses a
  virtual file system so the real install stays clean, shows a Conflicts tab with
  winners, resolves loose-file conflicts by priority order, and warns on missing
  masters; Skyrim plugins declare masters and load order decides record
  conflicts; Vortex/CurseForge add one-click install. Lesson: our dev profile
  (`active/` overlay, explicit `order`, conflict-blocks-unless-accepted) is the
  right shape; the Kit must make conflicts legible.
- **Crash-prone big editors.** Halo's Guerilla/Sapien crash on bad tags and keep
  a debug log. Lesson: validate before launch, keep logs addressable.

## 2. Who the modders are (personas)

- **Tinkerer** (largest): wants one visible change in-game in under an hour
  (sky colour, car paint, one ramp). Will not read formats.
- **Texture artist**: upscales/repaints; needs export-all, edit, 4K back,
  proof in a race screenshot, and a shareable pack.
- **Track/prop tweaker**: moves/removes props, opens up barriers, adjusts
  ramps; needs a picture of the world and numbers they can trust.
- **Vehicle modder**: car swaps, paint, handling; later new meshes from Blender.
- **Distributor**: packages, versions, installs, dependency and conflict
  handling for other people.
- **Researcher** (us): reverse-engineering; wants census, hashes, diffs.

## 3. Ranked jobs-to-be-done

Rank = audience size x frequency x how well it shows off the Kit. "LLM-assisted"
is what the user types to Claude Code with the `eod-creation-kit` MCP server
(see `MCP_SERVER_SPEC.md`).

| # | Job | Observable success | Kit capability | Status |
|---|-----|--------------------|----------------|--------|
| 1 | **Reskin the sky / any texture** | Edited PNG shows in the first-race capture; unrelated textures unchanged | `prepare_texture`, `export_texture`, `save`, `validate`, `launch_plan` (sky edits proven on Anoka) | exists (native-size) |
| 2 | **4K texture pack** | Export all textures, replace with 2x/4x PNGs, race capture shows sharper art; HOSTTEX log lists hits, zero fallbacks for targeted assets; pack installs/uninstalls cleanly | `asset_registry` census, `texture_export` batch with manifest/resume, `texture_identity` keys, `mod_package`, HOSTTEX renderer path | in progress |
| 3 | **Share / install / disable a mod** | Another machine installs a `.eodmod`; Kit lists it, flags conflicts; disable returns to retail byte-for-byte; disc never touched | `mod_package.py`, `mod_install.py`, dev profiles, `active/PROFILE.TXT` | in progress |
| 4 | **Remove an invisible wall / open a boundary** ("jump the barn") | Car drives through where the wall was; capture proves it; other collision unchanged | `world_collision.py` (collision, boundaries, height limits) | in progress |
| 5 | **Move, remove or raise props / ramps** | Prop appears at the new place in the capture; placements diff shows only intended rows | `world_graf.py` placements + move props, `world_export.py` OBJ/MTL | in progress |
| 6 | **Recolour / new livery for a car** | Car paint texture replaced; in a race the car shows the livery; works as a package | vehicle-paint role in census, texture export/replace, `vehicle_export.py`, packages | in progress (native paint partly exists via `prepare_texture`) |
| 7 | **See the world** (track and car as OBJ/MTL + textures, screenshots) | Opens in Blender with textures, correct scale; contact-sheet preview without leaving the terminal | `world_export.py`, `vehicle_export.py`, `texture_preview`, headless capture | in progress |
| 8 | **Tune physics / handling / damage / power** | Changed `mass/grip/damage_scale/power_max/torque_max/redline` visibly changes a timed run; value diffs recorded | `inspect` curated vehicle fields, `save` (VALT edit; save presently single-archive and gated to `BUS.CAR` in `save()`) | exists (narrow) |
| 9 | **Swap a car's mesh** (same topology first) | New shape in race with correct textures and no gameplay regression | `vif_patch.py` same-topology OBJ -> native geometry, `vehicle_import.py` | in progress |
| 10 | **Test a change quickly and prove it** | One call returns a first-race capture PNG + game log; diff against baseline capture | headless test run (capture + log), `launch_plan(headless)` | exists (as plumbing) |
| 11 | **Find things** (which archive holds this texture, who else uses this palette, what changed vs retail) | Answer from census with assetIds, `duplicateOf`, `runtimeKeyShared` | `asset_registry` census, `catalog`/`entries` | in progress / exists |
| 12 | **Make a new track from an existing one** (retexture, re-prop, new boundaries; later new geometry) | Reskinned and re-propped copy loads as its own entry | combination of 2,4,5; arbitrary new geometry explicitly not achieved (CREATION_KIT_CAPABILITIES) | missing for geometry; partial via composition |
| 13 | **New vehicle from scratch** (registration + physics + controls) | Appears in car select and drives | "playable boat" class: not achieved | missing |
| 14 | **Audio / music / UI text swaps** | Replaced sound heard | see AUDIO_FORMAT_NOTES.md; no Kit surface | missing |
| 15 | **Stack many mods and trust the result** (order, accepted conflicts) | `conflicts` lists every overlapping member, winner explained | profile `order`, conflict block | in progress |

### 3.1 LLM-assisted versions

The agent is best at jobs that are **repetitive, numeric, cross-referencing, or
need a verify loop**. It must never be the only judge of "looks right": every
journey ends with a returned capture image the human can see.

1. *Sky reskin.* "Make Anoka's sky a sunset." Claude: `texture_list(role=sky,
   archive=...)` -> `texture_export` the sky tiles -> writes/edits PNGs with its
   own scripts or `texture_tint` -> `texture_replace(dry_run)` -> apply ->
   `test_run_start` -> shows the capture, asks "keep / tweak warmth?".
2. *4K pack.* "Upscale every vehicle paint texture 4x and package it as
   `eod-hd-paints`." Claude: `census_get`, `texture_export_start` (job, progress,
   resume), runs the user's upscaler (or asks which), `texture_replace` with
   `mode=host` per asset (dry-run reports unmatched keys), `package_build`,
   `profile_install`, `test_run_start`, reads HOSTTEX hit/miss log from the
   game-log resource and reports "212 of 214 bound, 2 fallbacks: reasons...".
3. *Share/install.* "Install this .eodmod into a fresh dev profile, tell me what
   conflicts." Claude: `package_inspect`, `profile_create(dev-...)`,
   `conflicts_check`, shows plan, waits for explicit yes, `mod_install`.
4. *Invisible wall.* "There's an invisible wall at the barn in Anoka; remove it
   so I can jump it." Claude: `collision_describe`/`collision_query(near=...)`
   from a user-given waypoint or a capture-based hint, lists candidate
   boundary volumes with ids and a top-down render, asks which, `collision_edit(
   dry_run)` shows exactly which records change, apply, `test_run_start` with a
   scripted drive (when input scripts exist) or a capture at the location.
5. *Ramps.* "Raise every ramp in Anoka by 20% and test it." Claude:
   `props_list(track=ANOKA, kind~ramp)` (names/models; flags low-confidence
   classification), `props_transform(dry_run, scale_y=1.2, anchor=base)`
   returning N rows and bounding-box before/after, apply, `test_run_start`,
   shows capture, warns that collision for a scaled prop may not follow unless
   `world_collision` says it does.
6. *Livery.* "Make the Hurricane a red/white racing livery." Claude exports the
   paint texture, edits PNG (programmatic, or the user's art), previews as a
   texture image and on the car (`vehicle_preview`), replaces, tests.
7. *See the world.* "Export Anoka and give me a top-down overview with prop
   names." Claude: `world_export_start`, returns OBJ path + annotated preview.
8. *Physics.* "Make the Bulldog 15% heavier with more grip and run a lap."
   Claude: `vehicle_get_params`, `vehicle_set_params(dry_run)` showing before /
   after with units, apply, test; keeps a `CHANGELOG` in the project.
9. *Mesh swap.* "Replace the Digit body with this OBJ." Claude: runs
   `vif_patch_check` (topology compatibility report) first and refuses with the
   specific mismatch (vertex count, strip layout) instead of guessing; on pass,
   patch (dry-run), apply, test.
10. *Test and prove.* "Run the race and tell me if anything regressed vs
    baseline." Claude: `test_run_start(baseline=...)`, diff images, summarise
    log warnings (missing assets, HOSTTEX misses, crashes).
11. *Find things.* "Which textures share a palette with the Anoka sky?"
    Pure query over census.
12. *New track from existing.* "Clone Anoka as `Anoka Night`: night sky, lamps
    raised, barn boundary removed." Claude composes 1+4+5 into one package;
    **new track registration is missing** so this is delivered as an override
    of Anoka in a dev profile until registration exists. Claude must say so.
15. *Conflict resolution.* "Why does my sky look wrong?" Claude: `conflicts_check`,
    `profile_describe`, finds two packages touching the same member, explains
    winner, proposes order change as a dry-run.

### 3.2 What the LLM is bad at (design consequences)

- Judging visual correctness: always return images; never claim success from
  exit codes. (Repo rule: a clean process exit is not acceptance.)
- Inventing meanings for unknown bytes: tools expose only verified semantics;
  raw field access is an explicit expert tool with a warning in its description.
- Long jobs: must poll a handle, not block a turn; survive restart.
- Destruction: server enforces two-phase apply; the prompt cannot waive it.

## 4. Gaps worth building next (product view)

1. A **prop/ramp classifier with confidence** and a top-down annotated render
   (jobs 4, 5, 7 depend on the human recognising "the barn").
2. **Baseline capture + image diff** as a first-class result (job 10).
3. A scripted **drive path** for test runs ("drive from start through the
   barn"), otherwise collision edits can only be shown, not proven. Missing.
4. **Track/car registration** for new entries (jobs 12-13). Missing.
5. A **conflict explainer** that names the winning package and member (15).

## 5. Sources

Modding scenes
- FlatOut W32 & BGM Tool, FBX/Blender workflow, collision settings: https://modstalgia.com/flatout-2/mods/flatout-w32-and-bgm-tool and https://gaycoderprincess.github.io/project/flatout-w32
- FlatOut 2 In-Game DB Editor (live handling/mass/grip, F5): https://modstalgia.com/flatout-2/mods/flatout-2-in-game-db-editor
- FlatOut community mod pages noting BFS packer, DB tools, Blender track editing: https://www.moddb.com/mods/foj-community-mod
- GTA SA toolchain (IMG tools, TXD Workshop/Magic.TXD, COL editors, map editors, Blender add-ons INU Tools / SA Map Tools): https://libertycity.net/files/237474-inu-tools-2-2-0-blender-addon-for.html , https://libertycity.net/files/gta-san-andreas/224787-sa-map-tools-para-blender.html , https://gtastuff.com/
- Mod Organizer 2 (virtual file system, conflicts, load order, masters): https://stepmodifications.org/wiki/Guide:Mod_Organizer , https://modding.wiki/en/skyrim/users/infernalryan/mod-manager-fundamentals-mo2
- Halo Editing Kit (Tool / Guerilla / Sapien roles): https://www.halopedia.org/Halo:_Combat_Evolved_Mod_Tools , https://nikon.bungie.org/misc/hek_tutorial/references/level_design/mp_level_design/mp_ld_level_guerilla.html , https://learn.microsoft.com/en-us/halo-master-chief-collection/general/overview-tools
- PCSX2 texture replacement (hardware renderer, dump/replace folders, naming problems): https://www.moddb.com/tutorials/pcsx2-qt-texture-replacement-tutorial , https://forums.pcsx2.net/Thread-Texture-dumping-works-texture-loading-does-not?pid=627059
- Not fetched, from background knowledge only (verify before citing externally): Wreckfest/Blender modding guides, BeamNG documentation (https://documentation.beamng.com/), Vortex (https://www.nexusmods.com/about/vortex/), CurseForge, Skyrim Creation Kit (https://ck.uesp.net/).

Repo
- `docs/CREATION_KIT_TEXTURE_CONTRACTS.md`, `docs/CREATION_KIT_CAPABILITIES.md`, `tools/modding_backend.py`, `tools/asset_registry.py`, `tools/mod_package.py`.
