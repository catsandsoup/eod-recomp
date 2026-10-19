# Battle mode, chicken weapons and AI contracts

Status: experimental bot policy; native CPU contracts and bounded live Battle evidence verified on
2026-10-19; complete-match and lifecycle acceptance are tracked below. This chapter supersedes conflicting weapon, input,
registry and respawn claims in [the historical gameplay notes](GAMEPLAY_MODS_RE.md).
It describes the North American `SLUS_209.10` executable only. Addresses are
guest EE addresses, not host pointers or portable APIs for another game.
Executable SHA-256: `4f60053dc141365f5ae50737622f8408be30ced513b4c109427c926461f2e2ae`.

## Player success and acceptance

The player can enter original two-player Battle with independent controls and
visible native chickens. A shot follows the car's orientation, collides and
explodes, attributes the hit to the correct player, and reloads on the original
timer and release/repress sequence. Optional bots must preserve every human
controller and complete the same native lifecycle: drive, attack, recover,
shoot, respawn, score and finish a match. Native track geometry and spawn
transforms remain authoritative.

Required evidence combines instruction/ABI inspection, focused native tests,
rendered gameplay, scripted inputs and RAM state. Passing a mock input test or
seeing a nonzero velocity does not prove visible player success. The bounded
recovery targets at most four participants; increasing the native participant
capacity requires separate structural work.

## What reverse engineering and recompilation establish

**Reverse engineering** here means recovering the executable's contracts from
instructions, call sites, memory writes and observed behavior. Labels such as
"body", "interface" and "owner" are descriptions supported by those uses;
they are not recovered original C++ declarations.

**Disassembly** exposes the original MIPS instructions. **Decompilation** would
reconstruct higher-level source from them, without guaranteeing the original
variable names, types or structure. This investigation uses the disassembler
and the project's generated translation as evidence; it is not a recovered
original-source release.

**Static recompilation** translates guest instructions into host-compiled code
which still operates on the guest register state and memory. Generated
`f_0017E000(void)` therefore receives its arguments through `g_ee`, not through
ordinary host C++ parameters. The generated function and native MIPS function
share a guest address identity, but successful compilation alone does not prove
correct runtime semantics.

**Modding** intentionally changes behavior around that baseline. Extra blast
force, custom bots or a different launch mode are modifications, not evidence
that the original weapon has been restored. Keep native acceptance and optional
mod acceptance distinct. Do not repair a guessed ABI by adding more guessed
state writes.

## Native address evidence

Evidence was inspected in locally generated `gen/rec_020.c`, `gen/rec_021.c` and
`gen/rec_023.c`, with the guest addresses below as stable lookup keys. Generated
file partitioning can change. Source checkout at the initial focused test run:
`67c972b0fed775994d1769382676bd94ebdc3bc7` plus the uncommitted recovery changes.
This is a provenance checkpoint, not a claim that this commit alone contains
the recovery.

| Guest function / site | Observed contract | Evidence scope |
| --- | --- | --- |
| `0x0017DE30` | Registers interface `$a0`, owner index `$a1`, slot `$a2` into a 12-byte row. | Generated native stores. |
| `0x0017DE78` | Queries owner `$a0` and slot `$a1`; returns the row's interface in `$v0`, or zero. | Generated comparison and return paths. |
| `0x0017EA10`, registration call near `0x0017EB98` | Initialization registers `body+0x1DC`, owner from `body+0x1E8`, slot zero. | Native initializer; not covered by the focused fixture. |
| `0x0017DFA8` | Allocates a `0x228`-byte body and performs constructor work. | Does not establish that allocation alone completes gameplay initialization. |
| `0x0017E000` | Trigger takes interface `$a0`, car body `$a1`, input pointer `$a2`; subtracts `0x1DC` from interface. | Executed by focused test. |
| `0x0017AF80` | Copies a 48-byte pose into body+`0x64`, then propagates scene/bounds state. | Native copy executed; downstream propagation stubbed. |
| `0x0017E4D8` | With master and weapon enabled, stores state zero and deadline `now+3`. | Executed by focused test. |
| `0x0017E5C8`, `0x0017E5F8`–`0x0017E624` | Native update advances expired cooldown to release-waiting state. | Timer path executed; downstream physics/render work stubbed. |
| `0x0018FC1C`–`0x0018FCB8` within `0x0018F2A8` | Human driver gates on master+weapon, queries by controller+`0x40` owner index, reads L2 and calls interface vtable+8 with car and input pointer. | Native caller inspection; full driver not executed by focused test. |
| `0x0017E718`, call at `0x0017E7FC` | Detonation passes body+`0x88` position and body+`0x1E8` attacker index to `0x0017DA58`. | Native caller inspection; damage/score acceptance pending. |
| `0x0017DA58` | Resolves attacker index through `0x00179A08` before the native radial impulse path. | Ownership flow inspected; not exercised by focused fixture. |
| `0x0018AA80` | Stores steering `$f12` at car+`0x1B6C`. | Native setter inspection. Writing car+`0x1B80` is not this steering contract. |

Reinspect locally with a local executable and analysis metadata present:

```sh
.venv/bin/python tools/dis.py -f 0017DE78
.venv/bin/python tools/dis.py -f 0017E000
.venv/bin/python tools/dis.py -f 0017E4D8
.venv/bin/python tools/dis.py -f 0017E5C8
.venv/bin/python tools/dis.py -f 0017E718
.venv/bin/python tools/dis.py -f 0018AA80
```

Do not commit the executable, generated functions, RAM dumps, captures or
binaries. Commit authored tests, extraction tools and interpreted findings.

## Registry, body and input ABI

The registry starts at `0x003CE3F8`, with count at `0x003CE4B8`. A row contains:

| Row offset | Meaning |
| --- | --- |
| `+0x00` | Owner/car **index**, not a car pointer. |
| `+0x04` | Weapon slot. Native chicken initialization uses zero. |
| `+0x08` | Weapon interface pointer. For this chicken, `interface = body+0x1DC`. |

The trigger reads a 32-bit pressed value at `$a2`; the native human caller
provides a two-word input structure with the second word zero. Forward both
pressed and released input through the native call. A call to `0x0017E5C8`
does not replace the trigger, and writing body offsets relative to a registry
interface corrupts a different memory region.

Relevant body fields are:

| Body offset | Meaning used by the inspected routines |
| --- | --- |
| `+0x7C` | Forward vector after native pose copy. |
| `+0x88` | World position. |
| `+0x94` | Identifier copied from the car by the trigger. |
| `+0x104` | Launch/linear velocity vector. |
| `+0x1DC` | Weapon interface/vtable location. |
| `+0x1E0` | Trigger state: 0 cooldown, 1 awaiting release, 2 armed. |
| `+0x1E4` | Cooldown deadline, float. |
| `+0x1E8` | Registered owner index; preserved by firing. |
| `+0x1F4` | In-flight marker. |
| `+0x210`, `+0x214`, `+0x218` | Effect object pointers used across initialization/flight/detonation. |
| `+0x21C` | Local muzzle offset, three floats. |

The native initializer establishes more than the registry entry: it assigns
ownership, initializes scene state and attaches effects. Allocation followed by
handwritten registry rows and a fabricated ready flag is not an equivalent
initialization path. Use the existing native lifecycle and query its interface.

## Straight Battle, feat mortar and rain

The active mask is `0x003C4190`:

| Bits | Consequence in the inspected paths |
| --- | --- |
| Master `0x1` and weapon `0x8` | Native human weapon dispatch and three-second cooldown enabled. |
| Master and `0x10` | Feat mortar launch branch. This is not required for a straight Battle shot. |
| Master and `0x80` | Rain velocity branch, checked before the mortar branch. |

Let `F` and `U` be the pose's forward/up vectors and `V` the car velocity.
The native trigger computes:

- Straight Battle: `F * (length(V) + 28)`.
- Feat mortar: `(F * 0.707 + U * 0.4949) * 24.888889 + V`, with native float
  constants and instruction-order rounding.
- Rain branch: `(0, -19.62, 0)`.

These are initial velocities, not a promise of gravity-free or collision-free
flight. A sideways or reversing car contributes its speed magnitude to the
straight shot; mortar adds the velocity vector. Pitch affects the straight
shot because it follows the car's forward axis. The native trigger also
transforms the muzzle offset by the car pose before launching.

Default blast force and radius multipliers are now one; existing saved values
remain configurable and are not silently overwritten.

For the straight optional Chicken Fight mod, master+weapon is `0x09`; forcing
`0x19` selects mortar. If turbo is also enabled, the corresponding straight-shot
mask is `0x0B`, before unrelated flags. Preserve the distinction between flags
owned by the optional mod and flags established by native Battle/feat selection.

## Reload and release/repress

The native clock is a float at `0x003CDACC`. With master+weapon enabled, firing
sets state zero and deadline `now+3`. The update changes state zero to one only
when `now > deadline`; equality does not expire it. Held input leaves state one
waiting. Released input changes one to two without firing. A subsequent pressed
input in state two fires and starts a new cooldown.

Early release/repress during cooldown does not bypass the timer. In-flight state
is separate from trigger state: the original trigger does not use it as an
additional ready gate. Do not introduce a different reload model by equating
in-flight with cooldown or by manually resetting either field. Collision,
detonation and reset paths own the remaining projectile lifecycle.

## Focused tests and verified results

[`extract_native_weapon_reference.py`](../tools/extract_native_weapon_reference.py)
reuses the existing movie-bit reference extraction approach. At build time it
extracts locally generated `0x0017E000`, `0x0017E4D8`, `0x0017E5C8` and
`0x0017AF80` into the build directory. The checked-in
[`native_weapon_test.cpp`](../tests/native_weapon_test.cpp) contains synthetic
inputs and authored assertions, not copied retail functions.

Scene/bounds propagation `0x0017A498` and per-frame physics/render work
`0x0017F4E0` / `0x0017BBC0` are boundary stubs. None changes weapon state or
velocity. Unexpected owner lookup, rain spawn or hidden-pose update paths fail
the test. The native trigger, timer state transitions and pose copy stay real.

```sh
EOD_HEADLESS=1 EOD_MUTE=1 cmake --build build-mac --target native_weapon_test -j2
EOD_HEADLESS=1 EOD_MUTE=1 ctest --test-dir build-mac \
  -R '^native_chicken_weapon$' --output-on-failure -V
```

The initial coordinated run passed **138 checks in 0.26 seconds**. Cases cover
stationary launch, lateral/reverse speed, quarter turn, oblique yaw, pitch,
transformed muzzle position, flag gating/precedence, cooldown equality/expiry,
held suppression, release/repress, preserved owner and effect activation.

This is a native behavior characterization test. It does not itself execute the
production mod's flag-selection hook or full HumanDriver path. Separate wrapper
tests must prove those integrations. It also does not prove initialization,
rendered flight, collision, explosion damage/credit, reset, respawn or a complete
match. Retaining an owner integer in a fixture is weaker evidence than observing
correct scoring after a real hit.

## Prior art and evidence method

Checked [Road Trip Adventure's development guide at revision
`0f6d3613e8e32ccda5305227a578b1fb659983a8`](https://github.com/silentsudin/RoadTripAdventure-recomp/blob/0f6d3613e8e32ccda5305227a578b1fb659983a8/docs/DEVELOPING.md).
Its regression approach combines deterministic guest time, scripted pad input,
headless execution, exact frame capture and RAM observations. Reuse that method
of connecting an input to observable state and pictures. Its game structures,
addresses and driving model belong to a different executable and are not EOD
weapon or Battle contracts. No Road Trip game structure is copied here.

EOD already has `tools/padscript.py`, independent `EOD_PAD_SCRIPT` and
`EOD_PAD_SCRIPT1`, bounded `--frames` runs, screenshot output and `EOD_DUMP_RAM`
at exit. `tools/test_2p_race.sh` demonstrates independent scripted controllers;
its race navigation sequence is not a Battle acceptance test. Keep a dedicated
Battle script and exact configuration with each result. Do not call EOD runs
bit-identical merely because the input schedule repeats; establish that property
with measured state/frame comparisons first.

Automated builds and game runs must be coordinated, headless and muted. Use
`EOD_HEADLESS=1 EOD_MUTE=1`, plus `--headless` for the game. Start native baseline
validation at 1x PS2 resolution, 4:3, original timing and no frame skipping.
Archive local evidence under ignored work/build directories, not the repository.

## Native Battle construction and bot policy

Battle event ID is `0x18000224`. The current event is `0x003CF19C`, and the
master pointer is `0x003CF194` (Battle vtable `0x003F7770`). The master is
`0x5AC` bytes with **four** `0x50`-byte participant records starting at `+0x454`.
Rankings start at `+0x594`, spawn index at `+0x5A4`, and participant count at
`+0x5A8`. Increasing count beyond four overwrites adjacent state. Other race
arrays having twenty entries does not expand Battle's records, rankings or HUD.
Faster host hardware does not remove these structural constraints.

The optional `BattleBots` setting fills to four participants before native roster
construction (`0x0012C158`), allowing `0x0012AEF0`, `0x00111E10` and
`0x001116E0` to select valid native vehicles and driver records. Participant
records are at description+`0x2C`, stride `0x78`: type `0x50` identifies a human,
`0x51` identifies AI. **Human slots are not necessarily first.** Live solo
construction has put its human in slot 3; mixed construction has put humans in
slots 2 and 3. Human count and viewport count cannot identify a particular car.
The AI slot mask is latched at event construction, so UI changes take effect on
the next event.

After `0x0020FD48` builds the world, missing AI weapons are created through
`0x00227EE8(slot)`, the same scene factory used by native humans. It derives
muzzle offsets from the model, creates the explosive chicken scene and queues
physical construction through `0x00213578 / 0x001758B8`. `0x0017EA10` registers
the physical interface later. Capacity accounting must reserve queued entries;
checking only the current registry count is insufficient. Bounds are sixteen
weapon records and 1,200 world/visible object pointers. Existing scene/weapon
ownership and native suppression `0x002CE180` are respected.

Battle's native AI factory chooses a NullDriver. It does not register with the
normal driver update list; hooking its empty update function produced no live
inputs even though an isolated wrapper test passed. The policy therefore runs
after native AICar update `0x001A2D20`, once per actual car update. Native physics,
damage, scene updates and human drivers still run. The policy selects a living
opponent with hysteresis, leads moving targets, steers toward contact, slows for
turns, reverses when stuck and fires only when aim/range/elevation permit. It
uses native weapon input and cooldown instead of changing weapon state.
This is a custom pursuit/ramming policy, not recovered original Battle AI.

### Correct vehicle controls

| Car offset | Native setter | Meaning/evidence |
| --- | --- | --- |
| `+0x1B6C` | `0x0018AA78/80` | Steering target. Physics smooths into `+0x1B80`; that latter field is not the input setter. |
| `+0x1B70` | `0x0018AA70` | Throttle input. |
| `+0x1B74` | `0x0018AA68` | Brake input. |
| `+0x1B78` | `0x0018AA60` | Drivetrain engagement input: native Derby writes **1** through `0x001975B0 → 0x001988E8`. Setting zero prevented useful bot acceleration in a live test. “Clutch” is an inferred label, not an original symbol. |
| `+0x1B7C` | `0x0018AA58` | Rear brake/handbrake input. Physics multiplies it by eight and combines it with the rear braking terms. |
| `+0x1B90` | `0x0018AA50` | Requested gear; `-10` is native automatic forward selection, `-1` reverse. |

Earlier claims that `+0x1B78` is the handbrake are incorrect. Native Human and
Derby callers, physics consumers, and live RAM comparison agree on the corrected
separation. A nonzero throttle field alone did not establish working driving.

### Death, scoring and respawn

Common Car damage already reports death through
`0x0018D6C0 → 0x001DA468 → BattleMaster virtual +0x60 → 0x00166F48`.
This works for AI without NullDriver forwarding. Do not report it twice.
The participant record's dead flag is master+`0x458 + slot*0x50`, death time
`+0x45C`, kills `+0x460`, deaths `+0x464`, suicide penalties `+0x468`.
Displayed/net score is **kills minus suicide penalties**. `0x00167D58` changes
phase 1 (combat) to phase 2 (finished) at five net kills; phase 0 is countdown.

Humans see “Press the SELECT button to respawn.” Bots request the same native
operation, `0x001671E8(master,slot)`, while dead and combat is active. It checks
the native strict two-second delay before calling `0x001675C0`, which resets the
car through `0x00189F50`, chooses a native track spawn with `0x001673B8`, applies
it through the car's virtual reset, and resets event presentation. The bridge
does not invent spawn coordinates or directly clear death/score fields.

Initial placement also has a common native path:
`0x00185470 → 0x00187248 → 0x00187688 → 0x001D92A8 → 0x001DA418 → 0x00167C70`.
Do not infer that all AI need a manual teleport because their driver is NullDriver.
Native Derby uses a separate bound resource loaded through `0x00151BA0`;
Battle's bound pointer `0x003CD580` was zero in live runs. Substituting Derby
controllers without constructing their dependencies is unsafe.

### Solo menu exposure

`SinglePlayerBattle` appends a contest referencing an existing native VAL Battle
section after the outer `0x001244A8` loader returns. It preserves every existing
race, VAL key/value string and native `c_battle` track layout. The native caller
then sorts/remaps through `0x001248E0`. There is no world-origin arena, shared
string overwrite or replacement of a track's first race.

Native limits are forty Eves, thirty-two contests per Eve, and 320 total
contests. The observed solo resource set already contained 312 contests. The
bridge fills remaining capacity in display-name order; consequently Battle
availability is bounded by the loaded resource set, not guaranteed for every
track. Expanding the table or safely constructing a separate Battle menu is
future work. This capacity limit must not be silently bypassed. With the tested
resource set, the alphabetically allocated additions are Anoka, Ashland, Baxter,
Dennison, Dundas, Eagan, Gilroy and Hampton. Menu placement follows the native
contest list, not necessarily alphabetic event names; the scripted solo route
selects Battle one entry above Jump Race on Dennison.

Course following, its native ABI, solo eligibility decision and limitations
are documented in [Battle navigation](BATTLE_NAVIGATION.md).

## Native map and event unlocking

`UnlockAll` is enabled by default. It writes the existing feat masks at
`0x003D18E0` and `0x003D18E4`, seven track-prize bytes at
`0x004A8AF8..0x004A8AFE`, and event-selection override `0x003D8180=1`.
The latter two operations mirror native `0x002CBC18` and `0x002CD418`;
`0x002CD4A8` consumes the override. Only seven bytes belong to the prize
masks; the adjacent bytes must remain unchanged. The full career cheat
`0x002B6DB8` is not called because it also changes money, vehicles and
career state. With `UnlockAll=false`, these fields are left to native progression.

Home's `hidden=until_career_done` gate uses
`0x002C71B0 → 0x002E0AF8 → 0x00205368(7)` and the feat masks. Home has a
native multiplayer Battle descriptor in `homebase_eves.val`,
`$0069$contest`, with `max_cars=2`, `terrain_mask=64`, and
`terrain_disable_mask=128`. With its inherited terrain bits, the final
native mask is 341. This corrects the older claim that Home had no Battle data.
Unlocking a track does not by itself validate custom bot navigation on it.

The resource display-name list includes Anoka, Ashland, Baxter, Dennison, Dundas,
Eagan, Gilroy, Hampton, Hastings, Home, Kenyon, Mankato, Welch and Woodly.
Do not infer menu tap counts from that list: the actual multiplayer journey
uses nine RIGHT taps to reach Woodly; ten wrapped to Anoka in the captured run.
Home's sixteen native multiplayer contests put Battle last, while Woodly's
25-contest list places Battle at native index 15 (zero based). These menu
contracts are used by the scripted input journey, without selecting maps by
writing arbitrary runtime state.

## Live evidence and remaining validation

Automated runs use isolated memory cards, Vulkan 1x, FrameSkip=0, muted audio,
original force/radius scales of one, and all optional gameplay flags disabled
for the vanilla baseline. `tools/test_battle.sh` saves exact configuration,
controller scripts, captures, trace and final RAM under ignored local output.
Its exit check only establishes reaching combat, not full player acceptance.

- `vanilla-battle` (7,000 vblanks): two native humans, two cars, phase 0→1,
  flags `0x09`, independent movement, visible chickens and arming/reload UI.
  This does not establish exhaustive camera-angle or blast-credit fidelity.
- `mixed-2` (9,000 vblanks): two humans plus two AI, four valid weapon interfaces,
  native phase 0→1, bot movement/firing/ramming, and a bot credited two kills.
  Screenshots show both human wrecks and the native SELECT respawn prompt.
  The automated inputs in this run did not press SELECT, so no human respawns
  should be inferred. One bot remained near its initial area, motivating further
  recovery-policy validation.
- `single-3` (10,000 vblanks, exit 0): Battle selected from Dennison's solo
  menu, one human in slot 3 and three AI in slots 0–2. All bots moved over
  100 world units from their initial positions, fired native weapons, scored
  kills and completed two observed death→respawn cycles each. Human SELECT
  inputs produced two observed respawns. Maximum net scores were 2, 3 and 3
  for the AI. These are sampled trace counts, not an exhaustive shot log.
- `mixed-final` (18,000 vblanks, exit 0): two humans and two bots;
  both bots fired, scored and completed two observed respawns. Maximum
  displacements were 183.16 and 223.35 world units, with maximum net scores
  2 and 3. The match **did not reach phase 2**. Both human input schedules
  ended before the run ended, leaving wrecked humans unrespawned; the bots
  continued, but later showed poor progress near obstacles. This is evidence
  of a navigation limitation, not complete-match acceptance.
- `home-1` (12,000 vblanks, exit 0): rendered Home/native `homebase.trk`,
  two humans/two bots, phase 0→1, bot maximum alive-period displacements
  139.42/268.47, sampled fire transitions 2/6, maximum net scores 1/1,
  observed bot respawns 0/1. The longest throttled interval within three
  units was 7.17/2.33 seconds. This baseline precedes course following;
  its native course cache was empty, so Home uses direct-pursuit fallback.
  It establishes attacks and movement around the Home layout, not a finished
  match or obstacle-clearance guarantee.
- `woodly-route` (12,000 vblanks, exit 0): WOODLY remained selected in
  captures and native `Afton.trk` loaded. Two humans/two bots reached phase
  0→1. Bot maximum alive-period displacement was 234.42/190.36, sampled
  fire transitions 3/5, maximum net scores 2/1, and each bot respawned once.
  Course telemetry was valid in 114 samples per bot; only slot 1 used routing
  in five samples (native t=92.409–96.413), with direct interception elsewhere.
  The longest throttled interval within three units was 10.01/5.34 seconds.
  Captures show the farmyard, native chickens, moving opponents and fence pins.
  The run **did not reach phase 2**. This proves bounded course activation and
  combat on Woodly, not general obstacle avoidance or completed map acceptance.
- `dennison-route` (15,000 vblanks, exit 0): native `Dennison.trk`,
  two humans/two bots, phase 0→1. Bot maximum alive-period displacements
  were 164.20/206.12, sampled fire transitions 4/4, maximum net scores 3/2,
  and respawns 0/2. Course following activated in 3/27 of 171/168 sampled
  bot updates. Longest throttled intervals within three units were 11.34/3.67
  seconds. Human SELECT inputs produced 2/1 sampled respawns. The run did
  not reach phase 2, and pins remain. Different stimuli/frame bounds prevent
  treating these observations as a controlled performance comparison with
  `mixed-final`; primary-course following is not a complete obstacle solution.
- The navigation rebuild passed **54/54 CTests** (15.31 seconds, latest checkpoint rebuild), including the
  138 native weapon assertions. The local macOS app rebuilt successfully and
  passed `codesign --verify --deep --strict`.
- Earlier failed experiments are not acceptance: a NullDriver hook never ran;
  writing zero to drivetrain engagement left bots nearly stationary. Those
  failures explain why fixture-only success was insufficient.

The run named `woodly-1` was a menu-navigation mistake: native logs loaded
`Anoka.trk`, and captures show ANOKA before acceptance. Its 12,000-vblank
run nevertheless reached phase 2 at native t=104.438 with two humans and two
bots, slot 0 at five net kills, and visible native ranks/Eve standings. This
proves a bounded completed mixed match on Anoka with the previous direct
pursuit policy; it proves neither Woodly routing nor controlled restart.
The continued script entered post-event presentation. Keep this distinction
when interpreting the local directory name.

The following requirements remain pending until a named live run proves them.

| Pending check | Evidence needed |
| --- | --- |
| Additional original-reference coverage | Additional pitch/camera angles and direct original-hardware comparisons beyond the recorded vanilla run. |
| Reload in a real match | First shot, held input through expiry, release, second press; matching native state/deadline and visible second shot. |
| Collision, blast and ownership | Native impact/detonation path; correct attacker and victim; score changes consistent with the hit. |
| Complete mixed match with course routing | Two humans plus two bots through five net kills and results with the final course policy; the earlier direct-pursuit Anoka run reached this threshold. |
| Recovery and respawn | Observed wall recovery and native respawn transforms on the actual track; no invented world-origin circle or unsupported time guarantee. |
| Complete lifecycle | Countdown, combat, death, respawn, score threshold, results, restart and exit without stale state. |
| Additional regression journeys | Four-human Battle, restart, return-to-menu and normal-race visual checks beyond the passing fixture suite. |

## Pending acceptance journeys

The checkpoint intentionally does not claim the owner's entire Battle/chicken
request is complete. Preserve working builds and branches until the remaining
acceptance is verified; final source push and cleanup follow that acceptance.

The bounded next weapon journey is authored as:

```sh
tools/test_battle.sh "$DISC" weapon work/battle-weapon-reload 7000 Dennison
```

It uses original two-human Battle with bots/enhancements off, holds human 0's
L2 from vblank 5200 through 5549, releases at 5550 and presses again at 5580.
Human 1 remains neutral. Input-state checks establish the intended timeline;
real reload, visible flight/impact and score evidence remain pending until this
journey is actually inspected. `BATTLE_BLAST` records the native detonation
entry's attacker index, position, radius and force when tracing is enabled;
it does not itself prove which car was hit or why a later score changed.

Use an explicitly named `Anoka` mixed run for the next full-match/lifecycle
journey; the harness supports its ten RIGHT taps and requires `Anoka.trk`.
Control post-match input separately: demonstrate rank/results, select restart,
observe a fresh countdown/score reset, return to the menu and exit. Existing
fixed smoke scripts do not certify those post-match choices.

The major unresolved implementation gap is obstacle-aware pursuit: the current
reader only supplies primary-course segments, and no safe native wall/clearance
query ABI or secondary-branch traversal contract has been established. Resolve
that interface from native callers/consumers and test it before adding steering
heuristics or claiming general pathfinding. Recheck moving-target fence chases
on Home/Woodly and full mixed/solo matches after such a change.

## STAR handoff

**Situation:** historical implementation/notes mixed the weapon interface with
the body, treated update as fire, forced the mortar flag, and made unsupported
claims about synthetic registration, bots and arena relocation.

**Task:** recover the original weapon/control contract before extending Battle,
then prove the player acceptance above with live evidence.

**Action:** inspected native call sites and stores, documented ABI/flags/state,
added build-local extraction and focused native tests, and marked conflicting
historical documentation. Live validation caught two incorrect control assumptions and established native
weapon rendering, bot attacks and kill credit. Remaining acceptance is explicit.

**Result:** native CPU weapon characterization and mixed Battle attack/score
evidence are established. Solo Battle, bot/human respawns and the 54-test regression suite also pass.
The longer Dennison mixed run failed to reach the score threshold and exposed obstacle-navigation limits; the later mislabeled Anoka run did reach five kills/results. Home fallback and Woodly course activation are now measured, with obstacle pins still present. Next decisive checks are robust obstacle routing, controlled restart and exit. No claim of completed player acceptance follows from unit
tests alone.
