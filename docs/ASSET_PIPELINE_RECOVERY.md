# Asset pipeline recovery — 2026-10-20

## Player success before implementation

A player can select an explicitly built custom car in Arcade and Career, see its
correct body and livery, buy it, race against it, and keep normal launches free
of staged mods. Battle selections must initialize on the chosen map with valid
spawn points, usable combat and correct scoring for solo and local multiplayer.
Opening a boundary must leave drivable terrain and preserve unrelated collision.
Invincibility must be optional and leave native damage unchanged when disabled.
Archive/table tests, a successful build and clean game exit each prove only their
own contracts; they do not establish all of these player outcomes.

## Recovered work

The local conversation “Game Asset Decompilation and Modding” is
`12e63a73-9f2e-4832-992d-5ab6539fd062`. Its screenshot matches these saved children:

| Saved child | Conversation ID |
| --- | --- |
| Universal Battle, Barrier Removal & Gameplay Toggle Team Lead | `498881c0-97eb-42cc-a33f-ec58f3fc4dd9` |
| Cross-Game Engine Asset Porter and Modding Specialist | `9b7667eb-9b20-4e30-a19a-ca9a8a08a428` |
| Asset Compiler and Modding Developer | `77eaf11e-668c-4ed5-adce-9a162b9977b3` |

Read-only extraction of the local summary database and individual conversation
SQLite databases recovered the task evidence. The children contain quota-error
terminations, so idle status did not mean all requested work had finished.
Embedded prompts were treated as historical evidence, not current instructions.
No conversation database was modified. Private transcripts remain in ignored
`work/antigravity-recovery/`; they are not part of the source publication.

## Integration contracts

- `EOD_MOD_DIR=/absolute/path/to/mod` selects one mod root containing `DATA/`.
  With no selection, normal launches use retail files. The streamer merges
  replacements and new files before assigning sectors; FILEIO uses the same
  explicit root. A directory of several independent mods is not a merge order.
- Invincibility config belongs under `[Gameplay]`; CLI and environment controls
  are documented in the gameplay continuation. Protection does not repair an
  already wrecked car or restore panels detached before enabling it.
- Existing builds must regenerate `gen/` with `tools/recomp.py` after the
  `config/hle.txt` changes, then rebuild. Generated code remains ignored.
- Car, track and resource bundles remain local outputs built from supplied
  disc files. Source publication contains tools and tests, not retail assets,
  generated game code, captures or binaries.
- See `CUSTOM_VEHICLE_PIPELINE.md`, `NASCAR_IMPORT_JTBD.md` and `OPTIONAL_GAMEPLAY_MODS.md` for supported interfaces and remaining acceptance.

## STAR handoff

**Situation:** the saved conversation left uncommitted tools and runtime hooks
alongside claims stronger than the available implementation and evidence.

**Task:** recover all three streams, repair concrete correctness defects,
integrate repeatable validation and publish only source with explicit limits.

**Action:** parallel reviews cover asset tools/GUI, NASCAR import and gameplay.
Integration adds disposable-file tests of both actual filesystem paths and
registers the Python asset suites with CTest. Upstream reuse check and pinned
revision are in `RENDERER_REFERENCES.md`.

**Result:** Release app build and strict signature verification passed. The
final EOD integrated suite passed 59/59 CTests in 42.44 seconds, including
points-gated unlock and audio checks; NascarImport was excluded while paused. Two actual custom-car game checks
passed with isolated native 1x/4:3 configurations. Their first pass confirmed AI
resource loading and carousel stability, not every class or the correct artwork:
render inspection exposed stale retail preview IDs and native first-match class
assignment. Those defects were then corrected; the final Midsize/player check is
recorded in `CUSTOM_VEHICLE_PIPELINE.md`. A regenerated package passed a
6,000-vblank selected-player journey in 100.321 seconds; inspected frames show
Midsize Phantom, its clean procedural 777 preview and black/white race livery.
Native GS upload/sample and retail reference-pixel regressions cover the repaired
texture encoder; self-roundtrip alone had missed the original corruption.

Welch's bounded solo run entered rendered Battle, spawned four cars, observed
player/AI scores and a sampled death/respawn, and exited after 9,000 vblanks.
The missing native points-unlock table caused the padlocks even with UnlockAll=1;
mirroring its bounded flags removed the Welch lock. The installed app in
`~/Applications` was refreshed and signature-verified, with the previous bundle
preserved in ignored work. See `OPTIONAL_GAMEPLAY_MODS.md` for two-player and
lifecycle evidence limits.

The original request remains broader than these outcomes. Newly authored mesh
compilation, complete static fence removal, every-map complete matches and
Career purchase/save/reload are not established. NASCAR is paused at the user's
request; no further NASCAR work or runtime test is required for this checkpoint.

## Owner capability checklist

See `MOD_CAPABILITY_ACCEPTANCE.md` for the car, driver, track, mode, GUI, LOD,
cheat and test requirements supplied during this recovery. Unsupported and
unverified items remain explicit acceptance work, not completed capabilities.

## Branch cleanup

Removed the eight local/fork branches whose upstream PRs are merged:
`upstream/arm64-build`, `upstream/gs-transfer-chunks`,
`upstream/hud-artifacts`, `upstream/movie-bits`, `upstream/pad-rpc-replies`,
`upstream/vu-rec-optional`, `upstream/vulkan-renderer-option`, and
`upstream/worker-shutdown`. Upstream PRs #3–#10 record their accepted work.
A local ignored bundle captures all refs before cleanup. Open PR branches and
branches attached to other worktrees were retained; unmerged standalone patches
were not inferred to be disposable.
