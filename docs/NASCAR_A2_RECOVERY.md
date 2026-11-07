# NASCAR Cup-car A2 recovery — 2026-11-06

## Player acceptance

The player can select the Cup car, see its original body/livery/driver and cockpit,
crash it with infinite health off, see local body deformation rather than unrelated
panels moving, and see a stable silhouette at the far LOD. Shadows and roof lining
must come from NASCAR geometry. Native resources and the normal memory card stay
unchanged. This checkpoint is still a prototype, not full-import acceptance.

## Situation

Claude session e9e1cc1c-a703-47e8-ba9c-3099f0fcf199 stopped at its usage limit.
Recovered seven subagent JSONL logs from ~/.claude/projects and working files from
/private/tmp/claude-501. The A2 agent left code, two generated overlays and a clean
10,400-frame run but no completed report. The project plan incorrectly still said
no game test had finished and listed already-resolved owner decisions as pending.
A durable source checkout now lives at work/nascar/recomp beside the earlier plan.

## Task

Continue the accepted cars-first full-import plan. Delivery remains an opt-in
NASCAR profile, ultimately with Stock Car as a fifth class, real 2002 drivers and
liveries, separate NASCAR AI, source tyre/traction data and a Career dirt-tyre
upgrade. Tracks, modes and performance follow car acceptance. Do not equate this
one-car prototype with that complete goal.

## Action

Recovered A2 MODL/VIF tools, generated mesh deformation bones/skin, generated
shadow volumes, hood hinge helpers and true NASCAR Low4 support. Car geometry is
transcoded to TDEOD VU1 entries, not copied blindly. Bone skin is stored in reverse
vertex order per batch; each bonesurf uses local indices.

Fixed physics merge: retain all seven NASCAR source sections and 219 source
keys, including previously dropped source-only sections/gauges; neutral grip and
damage_scale=1.0; FrontDirt/RearDirt use the NASCAR source axle tyre values.
Four regression tests observed the old failures before the fix and now pass.

The first new cockpit test showed the previous liner adjustment was insufficient.
Its height followed low window/pillar vertices from group9 rather than the local
roof ceiling. Two regression tests reproduced that wrong-height choice; the
candidate now selects the local upper envelope minus0.02m. A normal-LOD runtime capture now hides the large exterior roof number; green
paint remains around the upper frame. Retaining source lining width is covered
by a real-cockpit regression. Full cockpit fidelity remains open.

## Verified result and limits

Before the physics change, a headless muted 1x/4:3 run with EOD_INVINCIBLE=0 and
EOD_GODMODE=0 showed yellow damage, HITS27% and a crushed roof after a hard impact.
It exited0 at9,000frames. Low-speed wall contact before that showed no damage.
The normal installed configuration has Invincible=1; it has not been changed.

Retail Algar/Prince comparisons reproduce all32 group bounds, and nearest-bone
inference matches96.9%/97.7% of their primary skin indices. This is approximate
reconstruction, not byte-exact reproduction of their original authoring process.
Generated Low0–4 indices and weights pass offline range/bounds checks.
Read-only review found no confirmed archive/skin corruption. Low4 has unskinned
Detail.tex batches in group9; damage persistence there requires a specific test.
A forced-Low4 overlay renders the actual source lower-detail body up close and
exits0 at6,900frames; its far-LOD geometry is not valid for judging the near cockpit.

The complete Python unittest modules pass76 tests with6 opt-in skips, including
source-backed fixtures and the new archive regressions. Broad
unittest discovery also tries importing five command-line GS scripts and reports
missing-argument errors: test_gs_alpha_range, test_gs_capture_replay,
test_gs_cpu_writes, test_gs_pixels, test_gs_state_cache. Those are CTest-driven
CLI harnesses, not unittest modules. No C++/renderer changes were made or claimed
validated by this Python suite.

Remaining donor adapters: transmission gearing, wheel meshes/class, engine audio,
effects, LOD distances, speedometer/menu preview. No fifth-class hooks, Career
circuit, dirt-tyre shop upgrade, imported track or4K/120 acceptance yet. No race
finish, buy/save/reload or split-screen acceptance yet. Doors/decklid/bumpers were
not detachable at this checkpoint; see "Detachable panels and shadow volumes" below.

## Next decisive tests

The roof and forced-LOD checks completed as described above. A freshly generated
package matches tested Low0/physics/Bone.mvl bytes before the archive fix. The final
source-complete package exited0 at9,000frames, loaded Cupcar without panic or
missing-car errors, and RAM confirms the sole human drives Cupcar. All four
profile files were installed and hash-verified, with the prior profile backed
up and the ordinary eod.ini byte-for-byte unchanged. Follow with controlled local-dent,
hood detachment, race-finish and Career/split-screen tests. Complete remaining
source assets/adapters before declaring a full car import.

Research references: this chat outputs TRACK_FORMAT_RECOVERY.md and
MODE_RECOVERY.md, STOCK_CLASS_RECOVERY.md and TRACK_PRESENTATION_RECOVERY.md.
All stopped research task areas now have recovered reports or implementation work. See the source
plan for the whole acceptance matrix. RoadTripAdventure-recomp was checked on
2026-11-06 (https://github.com/silentsudin/RoadTripAdventure-recomp); it does not
supply a Monster Games VALT physics converter. Existing rst0/valt compilers are
reused for serialization; no renderer replacement was introduced.

## Shared archive fix

Track research exposed a v2 extraction error: directory payload size excludes
the four-byte !IGM marker, while the old extractor subtracted it again. The
corrected extractor keeps all payload bytes, searches for the next marker only
after the complete payload, and rejects truncated payloads. Three portable
regressions failed before and pass after. Read-only review verified all297
entries across STOCK/TRUCK/HOBBY/MODIFIED/PACECAR/PAINT256/COMMON are marked and
in bounds with the corrected arithmetic. The original A1 claim that NASCAR TJRM
is missing4bytes was an extraction artifact; old un/n_* extracts are incomplete.

The source-complete package was tested and installed instead of the truncated
candidate. This restored real physics data: Rear stiff_c2=645.7 had been cut to64;
the corrected source value also feeds RearDirt. A fixture assertion now pins the
full value. Complete source helper coordinates are restored too. Native v6
packing/reading is unchanged.
No production networking code has been added: the user separately requested
a scoping-only online-multiplayer subagent for TDEOD base game, with UG2 as
inspiration. That scope is complete in ONLINE_MULTIPLAYER_SCOPE.md; no networking
implementation was performed. This is separate from NASCAR content and is not a prerequisite.

## Detachable panels and shadow volumes — 2026-11-07

**Panels.** The EE holds a table of eight detachable panels (`0x44AE68`, written by
`0x18DBF8`, read every frame by `0x18AFE0`/`0x18CFE8`): left/right door (groups 6/5), hood (2),
decklid (11), front/rear bumper (12/13) and left/right fender (4/3, hung from the door-hinge
helpers). A row exists only if its TJRM helper is found in the car's MODL ("car: can't find
helper %s in %s (%s)") and sheds when the Bone.mvl bone nearest that helper moves beyond the
row's threshold (0.076 m; hood 0.127 m; decklid 0.038 m). The imported Cup body had only the
groups 1/2/9/10 and only hood helpers, so five rows never existed. `tools/nascar_parts.py` now
cuts the NASCAR triangles into those groups (clipping the side panel at the door edges), writes
the helpers and `import_nascar_cars.py` rebuilds Bone.mvl for the new groups (two bone surfaces,
79+77 bones: more than 157 bones breaks the native VALT reader). Low1-3 are split; Low4 keeps
one body group. Verified in an Anoka Battle: all eight rows exist for Cupcar, the rear bumper
(row 5), the front bumper and a fender were shed and drawn as debris. Hood, doors and
decklid use the same mechanism but were not seen shedding in the short runs. The roster
builder (`nascar_roster.py`) still generates unsplit bodies for the other families.

**Shadows.** The shadow program (VU1 `0x0020`) pushes every vertex whose stored normal faces
the light along the light vector, so a shadow mesh is a closed volume only if every edge
carries a degenerate fin quad (the retail meshes' zero-area stitching triangles), and it flips
the facing of the triangle at every odd vertex index. The generator emitted a bare convex hull
whose lit faces flew off as detached triangles (the dark fans beside and behind the car) and
stored half of its triangles with the wrong parity. `nascar_gen.shadow_volume_triangles` adds
the fins, `encode_shadow_part` applies the parity rule (checked against all 92 solid triangles
of Algar's shadow1/2) and continues later batches with MSCNT like retail. Native cars render
the same scene cleanly; no renderer change.
