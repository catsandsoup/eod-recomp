# Optional gameplay recovery — player success and STAR handoff

## Player success, defined before live acceptance

With an explicitly selected local mod root, a player can select any of the
fourteen named tracks in Action or 2–4-player Battle and enter a match with
valid ground-level respawns, working chickens, independent human controls,
optional AI and correct scores. Bounded Welch Battle startup, scoring and a participant respawn are verified
below in solo and two-player runs. The next decisive test is a complete match
and lifecycle coverage on every map, including three and four human players. Archive construction and
synthetic tests establish neither safe terrain nor complete player success.

For optional invincibility, a fresh vehicle takes no new mechanical or panel
damage while collisions and chicken knockback continue. With the option off,
the native damage callbacks run. Existing wrecks and detached panels do not
repair when it is enabled mid-event. Protection applies to AI too; consequently
damage-based Battle kills cannot work normally while every car is invincible.
Disable it for ordinary Battle scoring.

For the boundary work, the intended player outcome remains driving through
Dennison's highway access gap and clearing visible fences without invisible
collision. The implemented tool removes only explicitly selected **dynamic
object references** from authored layouts. The exact highway objects, static
SOL/BPP fence geometry and off-track ground have not been verified in play.
There is no claim that boundary acceptance is complete.

## Use

Build a separate, local optional overlay from the installed disc:

```sh
python3 tools/build_gameplay_mod.py \
  --disc "$HOME/Library/Application Support/TDEODRecomp/disc" \
  --output work/battle-catalog --battle-catalog
EOD_MOD_DIR="$PWD/work/battle-catalog" EOD_HEADLESS=1 EOD_MUTE=1 \
  build-mac/eod --headless
```

Use the actual built executable path if it differs. The output directory must
be new and separate from the disc. Its generated `DATA/RES/COMMON.RES` and track
archives are local disc-derived assets; do not commit them. Omit `EOD_MOD_DIR`
to return to the ordinary installation.

This overlay intentionally provides **Action catalogs with one native race followed by Battle, and Battle-only
multiplayer catalogs**. The solo loader requires its first contest to be a race
(`0x00124154`–`0x00124194`), so Action uses 28 contests across fourteen maps. Appending fourteen more contests to the
observed 312-contest Action catalog would exceed the native capacity of 320.
It retains the existing Career/challenge Eve sections and dependencies; no
Career season playthrough has verified that preservation. It preserves native
Battle terrain overrides (including HomeBase) and sets the native maximum to
four cars. The existing `BattleBots` option fills remaining slots up to four.
`UnlockAll=1` remains useful for native track progression gates.

For an explicitly selected dynamic-object experiment, add one or more
`--remove-dennison-dyno NAME` arguments. The native Dennison `c_battle.va0`
contains object references to `Semi_Trailer` and `wall2`; these names are
verified asset data. This switch removes matching references from **every**
Dennison `c_*.va0` layout, so review `gameplay-mod-report.json` and visually
inspect the intended event before describing it as highway access. It also
removes the selected objects' visible instances. No inferred primitive index,
world coordinate, collision extent or fence height is patched.

Enable invincibility with `EOD_INVINCIBLE=1`, `--invincible`, or `[Cheats]`
`Invincible=1` in the ordinary configuration. `GodMode`, `EOD_GODMODE` and
`--godmode` are aliases. `EOD_GODMODE`, when present, overrides
`EOD_INVINCIBLE`; configuration is parsed once at launch. Zero and false values
are respected. L1+R1+Square toggles protection live, once per press, with banner
and button suppression. Configuration saves include the option. It defaults off.

## Situation

The recovered Antigravity session promised universal Battle, open boundaries
and invincibility. The remaining files did not establish those outcomes:
`EOD_INVINCIBLE=0` activated the draft cheat by variable presence; the cheat
only intercepted an orphan scalar setter, reset one health field every frame,
was not saved, and omitted the tracked HLE registration. Assertions about
Dennison primitive indices were not backed by the recovered survey artifacts.

## Task

Finish reviewable, opt-in implementations using verified native contracts;
keep disc originals intact and separate verified outcomes from remaining
player acceptance. The user's renewed gameplay request authorizes these mods
alongside the repository's native baseline priority.

## Action and native evidence

The existing archive/VALT/ILIN wheel is reused rather than introducing another
asset stack. The repository's existing `src/battle_navigation.cpp` and native
`0x0013AF20` establish native course position X/Z at node +0/+4 and tangent at
+8/+12, with primary node count in the high halfword of header +0x1C. They do
not establish XYZ at the first three floats. The builder validates finite
positions, nonzero tangents and minimum planar separation, samples eight
primary course points, and writes native `pt`/`dir` layout sections for missing
Battle layouts. `0x00167A88` reads those sections and requires at least four;
its call to `0x001D8F58` performs native ground placement. The builder does not
invent Y or bypass a native spawn panic. The bounded Welch runs below verify observed grounded cars and one participant
respawn each; they do not cover all eight generated spawn candidates.

An audit of local retail `COMMON.RES` found native Battle descriptors in
thirteen of fourteen `*_eves.val` resources; Welch alone lacked one.
`eves.val` already includes all fourteen resources and `tracks.val` already
contains their metadata, so gratuitous changes to those tables are unnecessary.
Springfield's actual disc archive is `SPRING_1.TRK`. Native Battle layouts are
retained; only a missing one is derived. The builder records source archive
SHA-256 values and what it changed in a local JSON report.

Invincibility wraps native `0x0018AD20` and `0x0018CA98`. The first adds/clamps
the accumulated damage at car+7148. The second performs panel deformation,
damage state changes and destruction after the collision response caller
(`0x00189CD8` return site). Suppressing the damage callbacks leaves the caller's
physical response in place; this source evidence still needs rendered
collision/chicken comparisons. The callbacks delegate directly when disabled;
there are no per-frame health writes or silent weak native fallbacks.
The native world update `0x001B5840` also writes damage inline for the
master+`0x40` timed-damage feat. A third wrapper temporarily clears only that
bit during the complete native update, then restores just that bit while
retaining unrelated native flag changes. Initialization and respawn health
stores (`0x00187248`, `0x0018D878`) remain native.
`config/hle.txt` registers all three hooks so generated code is reproducible.

The earlier `track.sol` fixed-stride exporter does **not** establish a safe
editable SOBL schema. It is insufficient evidence to delete guessed records or
clamp heights. Authored VAL layout references are the verified modification
surface used here. The upstream-reference and Context7 review for this recovery
is recorded in `RENDERER_REFERENCES.md`; those generic recomp references do not
supply game-specific EOD collision IDs or damage contracts.

## Result and remaining acceptance

Focused `LiveCheats` testing covers native delegation when disabled, explicit
environment zero, protection callback suppression, no invented pre-existing
damage repairs, unchanged unrelated physics memory, chord press edges/button
suppression, timed-damage gate restoration and configuration persistence,
including environment options on first launch without a configuration file. Synthetic gameplay builder tests
cover bounded Action/multiplayer catalogs, native first-race ordering and VAL root
counts, retained Career Eve definitions,
actual primary X/Z+tangent mapping, invalid points and selective dynamic-object
references. These are CPU contract tests with native/platform calls stubbed;
they do not prove damage-free driving, rendered collision response or matches.

The optional Battle builder supplies a concrete all-track catalog and a Welch
layout exercised in the bounded live runs below. Full lifecycle coverage for
all fourteen maps and three/four human players remains outstanding. Invincibility needs fresh-event wall, car and
chicken impacts on/off, including impulses. Boundary work still needs actual
highway object identification and a decoded static collision schema before
fence changes; off-track ground cannot be guaranteed by removing a barrier.

Live investigation found and corrected two additional native VAL contracts:
new Eve root sections must precede child sections and remain within the header
root count; child contest types must use the canonical `$NNNN$contest` naming
convention. `compile_binary_valt` now accepts explicit validated `root_count`
metadata for generated hierarchies. The optional builder sets it directly,
including one root for the generated Welch respawn layout. Tests assert the
serialized header, rather than only inspecting a Python dictionary.

## Default track locks — recovered native gate

A rendered Welch menu probe exposed a real pre-existing `UnlockAll` bug: the
seven prize-mask bytes and feat unlocks were all set, yet Welch still showed a
padlock and normal confirmation could not launch it. The separate native
points unlock roster at `0x003CBF48` contained sixteen locked rows. Welch's
`TRACK` row required 70,000 points and its unlocked flag remained zero.

Native `0x0012CC40`, called by the track picker at `0x002C7610`, accepts a
matching track when native points reach its threshold **or** its row's explicit
unlock flag is set. `0x0012CAB8` sets exactly that flag at
`0x003CBF58 + index*0x10`. `0x0012C940` limits the roster to 32 entries.
`UnlockAll=1` (already the default) now mirrors that native flag operation for
the bounded loaded roster every vblank, alongside the existing prizes/feats.
It applies to ordinary launches and optional catalogs, including the native
points-gated cars. It preserves names, types, thresholds, adjacent rows, money
and season state. `UnlockAll=0` does not write these flags. As with other native
unlock cheats, flags already set are not relocked by disabling the option.

The regression first failed on missing default roster flags, then passed after
the change. It covers both loaded rows, unrelated fields, the unused next row,
native profile resets, disabled preservation and rejection of a count above 32.
Rendered verification and its limits are recorded in the final result below.

## Reproduce the bounded Welch journey

After building a fresh optional catalog, run each probe separately:

```sh
tools/test_gameplay_catalog.sh \
  "$HOME/Library/Application Support/TDEODRecomp/disc" \
  work/battle-catalog single work/welch-single 9000
tools/test_gameplay_catalog.sh \
  "$HOME/Library/Application Support/TDEODRecomp/disc" \
  work/battle-catalog mixed work/welch-mixed 9000
```

The helper uses normal controller input, a private card/config, native 1x/4:3,
headless/muted rendering, screenshots every sixty vblanks, a final RAM dump and
Battle traces. It requires the actual `Welch.trk` load and active Battle phase,
so a mislabeled output directory cannot pass. Its fixed menu timeline is a
bounded retail journey, not a general menu automation API. Inspect screenshots
for correct map, controls and gameplay. It does not assert match completion.


## Final bounded live result — October 20

**Situation/task:** the original Welch map was absent from Battle. A new catalog
must be reachable through normal menus, obey native loader bounds and place
vehicles on real terrain. The separate default map padlock bug also needed an
ordinary runtime fix.

**Action:** first rendered probes caught and corrected root ordering/count,
canonical child-type names and the solo first-race requirement. The final
optional catalog retains native Eve display metadata, including movie names
such as Woodly/Hampton, and resolves terrain fields through native contest
inheritance. No native panic or track-selection state was patched. The normal
input route selects Welch with five LEFT presses from Dennison, then selects
Battle. Both runs use native 1x/4:3, muted/headless Vulkan, private cards and
configuration, screenshots every sixty vblanks and native Battle telemetry.

**Verified result:**

- Solo, 9,000 vblanks, exit 0: the actual `Welch.trk` resource loaded; active
  Battle reported one human and four cars. Inspected images show an unlocked
  Welch menu, then a grounded player car on the Welch course with native
  SCORE/DAMAGE and chicken HUD. The player reached score 1; another participant
  reached score 1 and completed one sampled dead-to-alive respawn.
- Two players, 9,000 vblanks, exit 0: the final v5 archive loaded Welch into
  native Battle (`0x18000224`) with two humans/four cars. Inspected images show
  two separate grounded cars, split-screen views, native HUDs and minimaps.
  Eleven native blast events were recorded; one participant reached score 1
  and another completed one sampled dead-to-alive respawn. Sampled car Y
  coordinates ranged from -9.649 to 5.355, consistent with observed terrain.
- Final RAM in both runs contained all sixteen loaded native points-roster
  unlock flags set to 1. The pre-fix rendered Welch padlock disappeared and
  normal confirmation launched Battle after the fix.
- Native solo RAM contained all fourteen requested maps, each with race+Battle,
  plus the original hidden EmptyTrack test entry (15 Eves/29 contests). The
  optional generator's final compiled Eve bytes for all fourteen maps were
  compared against the actual final v5 packed archive and matched exactly.
  This includes the terrain-fallback guard added after inspection; retail
  Welch has no explicit terrain mask in its Eve/race/default tables.
- Eight synthetic gameplay tests now cover root headers, first-race ordering,
  canonical contest type, native display names, direct/inherited terrain masks,
  invalid respawn candidates and selective dynamic references. The tracked
  reproduction shell script passes syntax validation. Focused LiveCheats
  tests pass with the default points-roster unlock regression.

**Limits:** neither run reached the match-complete phase 2. Scores and one
sampled participant respawn per run establish bounded lifecycle evidence, not
all player/bot journeys. Three/four human players, every track, all generated
spawn candidates and complete match/restart/exit still require acceptance.
Some weapon-body telemetry appears below the world; those records are not car
positions and a nonzero flight flag alone is not evidence of a fired shot.
Only actual native blasts and score/state transitions were counted as combat
evidence here. Idle/projectile-body lifecycle beyond these observations is
not established. Invincibility was off for these Battle runs; its physical
response and damage-free player acceptance still needs a dedicated rendered
on/off comparison. Highway/fence collision and off-track ground acceptance
remain open as described above.

Local evidence is ignored under `work/gameplay-live/welch-single-unlocked/`
and `work/gameplay-live/welch-mixed-unlocked/`, with exact controller scripts,
configuration, screenshots, logs and RAM. Solo used the v4 catalog before the
native preview-name preservation; two-player used the final v5 output.
Generated disc-derived archives and captures are not committed.

## Expanded Battle participant capacity — static analysis

Increasing maxcars beyond four is unsafe with the current native Battle state.
The factory at 0x00167F30 allocates 0x5AC bytes (0x00167F48/4C). Constructor
0x00166C60 initializes only four records, at master+0x46C, +0x4BC, +0x50C and
+0x55C, through 0x00167F20. Loader 0x001678A8 copies the description's count
(+0x98C) to master+0x5A8 and indexes participants at +0x454+0x50*i without
a greater-than-four guard. Participant five starts at +0x594, overlapping rank
zero. During update, 0x00166DA0 calls 0x00167908 per participant; the fifth
record's clears at +0x0C/+0x10/+0x14 reach master+0x5A0, +0x5A4 (spawn index)
and +0x5A8 (participant count). Ranking 0x00167658 uses +0x594+4*i, likewise
overlapping spawn/count with larger populations; finish 0x00167D58 consumes the
same layout.

These are source/disassembly findings, not a >4-car live test. A safe expansion
needs coordinated participant/rank storage, construction/allocation and every
native access (including finish and HUD) audited and adapted, followed by
complete scoring/respawn/match tests. Neither ordinary 14-car racing nor changing
one Battle catalog field establishes this. The generator retains maxcars=4.
