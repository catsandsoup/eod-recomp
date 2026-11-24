# Online play: state sync (Wreckfest / FlatOut style)

Each player's game owns and simulates its own car from its own controller (full native physics,
zero input delay, own full-screen view). Every machine sends its car's state 30 times a second to the
host, the host relays it, and the other machines drive **puppet cars** from the received states.
No lockstep, no determinism requirement, no rollback. The host is authoritative for the event flow
and for the AI cars. Every car is always solid: there is no ghosting or collision pause, ever.

An earlier input-lockstep attempt (deterministic virtual time, tick authority) was abandoned; its
evidence patch is outside the repo (`online-build/abandoned-determinism-experiment.patch`).

## Car state (verified, SLUS_209.10)
* `0x48522C` participant count, `0x447458[slot]` car by description slot (= driver table `0x485230` order).
* car object: pose 4x3 at `+0x64` (right, up, forward, position), linear velocity `+0x104`, angular
  velocity `+0x110` (world space: omega x forward = dF/dt, fits to 0.008), controls `+0x1B6C` steer,
  `+0x1B70` gas, `+0x1B74` brake, `+0x1B7C` handbrake, `+0x1B90` gear, damage `+0x1BEC`, type `+0x1E0`
  (0x50 human, 0x51 AI).
* **The car's pose is a copy.** The physics master is a contiguous `0xB0`-byte body array at
  `*(0x494D30)`, count `*(0x494D28)`. Native `0x185390` allocates a body through `0x1DD2F8`,
  then appends the car to `0x3CE4F8` (count `0x3CE548`); that registry index selects its body.
  The previous global-pointer scan resolved only body zero. Do not cache mappings across worlds.
  Body layout: `+0x00` position, `+0x10/+0x20/+0x30`
  rotation rows, `+0x40` quaternion (x y z w, standard matrix convention), `+0x50` linear velocity,
  `+0x60` angular velocity. A puppet is imposed by writing both the body record and the car copies
  before and after every world step (`netplay_car::write`). Proof (N1): replaying a recorded car onto
  an AI slot in another process drifts 1.3 mm per native step on average
  (`EOD_NETPLAY_REC` / `EOD_NETPLAY_PLAY`).
* Human ownership is verified through the actual native controller chain: `car+0x1C1C` driver
  wrapper (vtable `0x3FA4B0`) → `+0x10` HumanDriver (`0x3FA450`) → `+0x38` owning car and
  `+0x3C` DriverInput, whose `+0x34` pad index must be zero on each online machine.
* A puppet's native controller is skipped (`f_001A2D20` returns early for puppets, which also stops
  Battle bots driving it).

## Event flow (all idempotent state, repeated until acknowledged by the peer's heartbeat)
1. Lobby (host): JOIN / WELCOME compare the effective gameplay-content fingerprint before admission;
   differing archives/overrides and Battle alternates are rejected. Offline/custom music stays local.
   LOBBY state4Hz, guest HEARTBEAT (ready, car, phase). LAN beacons on UDP27014.
2. START: the lobby presses ACTION on the native main menu (the menu is a blocking loop, so the pad
   read hook injects Up/Cross using the highlighted-row word `0x3D73FC`), then GameDoSingle runs with
   the **native car screen shown for real** on every machine and the **native track screen real on the
   host**; guests answer theirs with the host's pick (TRACK). The event is chosen by name from the lobby.
3. Roster: the host waits until everyone has picked a car, puts each remote player into an AI seat of the
   description (before `0x1116E0` resolves names), and broadcasts ROSTER (driver, vehicle, owner per seat).
   Guests overwrite their seat numbers with it, force their own seat to human (0x50) and all others to AI.
   Native controller bits (`record+0x70`, low two bits) are explicitly pad zero for the local human.
   Grid indices (`record+0x72`) are a unique slot permutation; native vehicle-ID swaps cannot make
   identical selected cars share a starting seat. Battle sizes add one car per remote human (`0x12C158`).
   Events without enough physical seats are refused and unwind to the lobby.
4. Loaded: each machine verifies every car/body and its one local HumanDriver before simulation.
   Host captures all native starting states (including rolling velocities), rejects coincident seats,
   and resends event-scoped GRID until every guest applies it once to every car, including its own,
   and sends GRID_ACK. Only then does the host issue GO. Grid/GO waits are bounded and failures abort.
5. During the race, each guest sends its driven car to the host at 30 Hz. The host sends its car, AI cars and the other drivers to each guest. The receiver checks the event, seat owner and snapshot tick. Protocol 10 includes full-precision cumulative damage, the native damage stage, ranked Battle scores and ordered guest lobby heartbeats. Protocol 11 adds the native damage flash and camera shake to the owning driver’s impact checkpoint. Multiple contact points in one checkpoint retain the strongest native pulse. Both peers check the protocol version at admission. Remote cars take these values from the latest accepted state.
6. The host collects the event outcome and sends the official standings for covered modes. Returning to the native menu restores the party lobby.

For a lap race, the host maps each driver's crossing to its monotonic clock. The earliest accepted crossing starts the 30-second finish window. A retry reporting an earlier crossing can shorten that window. At the cutoff the host allows 750 ms for reports of crossings already made. Finishes in the same clock millisecond tie by grid slot. Each client receives the same completed rows and host presentation time.

Run `tools/test_online_completion.py EXE DISC EMPTY_OUTPUT_DIR race-four --impaired-results` to exercise lost finish reports and standings over an asymmetric 150 ms round trip with jitter and background packet loss. The runner measures the time from the host's outcome to each driver's results and the spread between clients. It checks that the wrecked player's Totalled banner gives way to results, that everyone returns to the lobby, and that the next round repeats the journey.

For a live CPU race, run `tools/test_online_completion.py EXE DISC EMPTY_OUTPUT_DIR native-pack --public-garage relay --rotate-track`. Three copies of the game each drive their own network seat using the retail Hard racing AI. The host selects another venue after the first standings screen. The test checks each driver's controls, remote motion, competitive contact or passing, actual finish-line crossings, and both sets of standings. Add `--impaired-public-relay IPV4:PORT` to simulate latency and loss on the guest connections to the deployed relay. Add `--rendered-host` to use the host's normal display settings while the two guests run headless at 1×.

`tools/test_online_party.py EXE DISC EMPTY_OUTPUT_DIR --lose-loading-host` kills the host after all three games leave the main menu and before GO. Both guests must return to their native menus with the retryable host-loss reason. They then form a new party, finish a race and receive the same two places using the original processes.

With `--lose-results-guest`, all three drivers must first receive the same standings. Killing one guest during presentation leaves those places intact. The host and remaining guest must rematch in the same party, finish in a field with two human drivers, and return together.

## Driving and car contact

Beyond 50 m, remote cars use the configured interpolation buffer (six ticks by default). Inside 15 m they predict forward by the estimated one-way travel time, up to fifteen ticks. Between those distances the playback time changes continuously. The host measures distance against the whole field so two guests can trade a bumper hit while the host drives elsewhere. Guests account for both network legs when viewing another guest's car. After thirty world steps without a newer pose, the car coasts under native gravity and terrain contact with its last confirmed damage. If the host loses an owner's pose stream, it sends this coasting body to the other drivers. Fresh owner snapshots blend from the current body. The roster identifies remote human drivers during car construction so they receive the selected event's human damage tuning.

Run the impact fixture with `--snapshot-stall --observer --airborne-m 8 --rtt-ms 0 --max-damage-delta 0` to cut snapshots to the third driver during an airborne impact and withhold the colliding guest's checkpoint acknowledgements. It checks the observed descent and landing during the interruption, continued local driving, repeated checkpoints, recovery of the owner's position and matching damage on all three drivers.

At a damaging car-to-car hit, the host lets both native bodies carry their physical response through subsequent steps. It sends the affected guest an event- and seat-scoped post-contact body checkpoint. The guest keeps local controls and applies each newer checkpoint once. The host continues sending the final checkpoint until the owner echoes its revision in a snapshot. A guest that predicts contact first can render recoil and cosmetic dents while it waits for the authoritative competitive damage. It repeats its pre-impact approach for up to 600 ms to keep predicted recoil from changing the host's incoming collision.

The impact fixture runs two game processes on Anoka, seeds a post-countdown high-speed approach, and measures the native response. Pass `--rtt-ms`, `--guest-to-host-share`, `--jitter-ms` and `--loss` to introduce loopback impairment. Use `--lateral-offset-m`, `--host-car` and `--guest-car` to vary the geometry and masses. `--max-damage-delta 0` checks scalar damage and stage on both views at fixture tick 120. `--max-impulse-difference .045` checks longitudinal momentum change on the host's pair. `--wreck-derby` starts the selected driver (host by default, or `--wreck-driver guest`) with 94 percent wear and checks that a native collision totals it, the surviving driver wins, and both return to the party. With `--observer`, a third game drives its own car from the native grid using a held accelerator. The test checks its throttle before and after contact, movement, 116 consecutive native steps and both damaged cars on all three machines. Pass `--guest-pair` to make both guests collide and the host drive the independent car. Each guest must apply its own ordered host checkpoints. The Derby case keeps both survivors racing after one wreck. All processes finish the native countdown before the test releases the seeded pair together inside the selected arena. Panel shapes, loose debris and detached wheels still need independent replication and acceptance tests.

Use `--following follow` to place equal-speed cars 5.5 m apart at 72 km/h, `--following nudge` to close at 46 versus 36 km/h, or `--following ram` to close at 105 versus 36 km/h. The fixture seeds the approach once. The ordinary solver determines whether either bumper makes contact, how much momentum transfers and whether it causes damage. `--guest-pair` makes the two cars independently owned guests.

Pass `--pack-collision --rtt-ms 100 --jitter-ms 4` to drive three independently owned cars into the same point from three approaches on Anoka. Every pair must register reciprocal native damage-producing contacts on the same step, with reciprocal impulses within 4.5 percent, and each driver must approach at 95 km/h or more. Both guests must apply their own ordered host checkpoints. At step 120, every copy must agree with its owner's damage scalar and stage. The 16-driver capacity and prolonged side contact require separate fixtures.

The retail airborne crash camera pauses world physics for about three seconds. Its native eligibility check counts viewports. Each online machine has one viewport, so a collision can start that pause independently on both drivers. Online events skip the dedicated crash orbit at `0x001D3FB8`. The incident dispatcher, damage and normal driving cameras continue running. Solo play uses the original orbit. To check the full path with a third driver under throttle, run the impact fixture with `--airborne-m 4 --observer --rtt-ms 100 --check-crash-camera`. The test requires a native incident to request the orbit, the pause flag to stay clear and 120 consecutive native physics advances on every game.

## Player gamertags

The host controls **Player Gamertags** in the lobby. Races start with names on and Demolition Derby starts with names off. The room remembers a separate choice for each and sends the selected rule with the event start. The HUD labels human drivers, including the host. A driver who leaves the event loses their tag.

The label follows the selected car’s roof and the local driving camera. The native world visibility pass at `0x3582B8` records the camera used by `WorldToScreen` before the HUD installs its own. Projected labels use the game viewport, a 150 m distance limit, and staggered sightline checks against active SCOL terrain and SOBL boxes and tubes. These collision shapes approximate visual occlusion. Decorative meshes can have no collision, and some physical barriers extend above the visible wall.

Run `python3 -B tools/test_online_driver_names.py EXE DISC EMPTY_OUTPUT_DIR race` and repeat with a fresh output directory and `derby`. Each run opens a host and guest on Anoka, checks their names against native roof and camera samples, attempts a guest override and a mid-event host change, then verifies the host’s new setting on the rematch. The geometry assertions run in `netplay_state_sync`.

## References used
* FlatOut / FlatOut 2 (Bugbear, closest genre match): host with standard dead reckoning of physics objects,
  accepting small visible jumps on remote hits. https://gamedev.net/forums/topic/394380-multiplayer-physics/
* Wreckfest (Bugbear): each client owns its car, remote cars are smoothed puppets (public descriptions only).
* Assetto Corsa / CSP: server relay, remote-car extrapolation, jump limit. We do NOT use its
  collision-pause-after-correction (JUMP_PAUSE_COLLISIONS_FOR): owner requirement, no ghosting.
  https://cup.acstuff.club/docs/csp/misc/server-extra-options
* iRacing: relay plus remote prediction; close contact is the known weak spot.
* SuperTuxKart (stk-code, ENet, server-authoritative with rewind): heavier than needed, code reference.
* Mario Kart 8: host + clients star, same topology.

## November 8 ownership/body correction evidence

The focused `netplay_state_test` covers contiguous second-body writes, reused car addresses with
new body storage, native HumanDriver/input ownership, corrupt states and delayed snapshot ordering.
Actual integrated-content Mac same-car testing reached two GO barriers and two native lobby returns
on both peers. Initial failure runs demonstrated pre-GO abort/unwind; the rematch harness now requires
GO, preventing two aborted loads from passing. Current long-input evidence and acceptance limits are
recorded in the chat workspace `work/DRIVER_GRID_STAR.md`.

Rechecked [RoadTripAdventure-recomp](https://github.com/silentsudin/RoadTripAdventure-recomp) on
November 8. Its public platform/control overview does not supply this game's native participant or
body-layout contract. Reused the existing state-sync topology and native EOD allocation/driver
factories; the correction is to their verified interfaces, rather than a replacement physics system.

## Lobby portrait availability

YOUR CAR/PAINT displays the selected car's `uicar_<number>.tm0` portrait, independently of the
track preview. The native retail archive is checked directly (bypassing mod overrides) to establish
which identities have verified portrait art; their effective overlay mip is decoded with alpha.
Native special vehicles without art (for example Trailer2/401), custom Cupcar/Phantom entries and
added NASCAR paint identities show an explicit unavailable message. Installed custom UICAR entries
can be flat livery/HUD swatches, so their existence alone does not qualify them as car portraits.
The CT body texture is never used. Correct rendered custom portraits remain a separate asset task.


### Joining a room with different game content

Compatibility is checked against the effective gameplay files before assigning a seat. A host with recovery support includes its content fingerprint and the type of local content it loaded when declining an incompatible join. For a composed custom profile it can also report up to three package IDs and versions from the local Mod Manager inventory. The fingerprint governs admission even when two profiles share a name or package list.

The joining game checks the player's disc, installed integrated packs and registered standalone profile in the background. If one matches, **Restart and Rejoin** starts a fresh instance with those files and repeats the original Garage Code, direct or crew join. The next normal launch uses the player's existing preferences. When no installed profile matches, the guest sees the host's setup requirements and keeps Retry and Back. New gameplay packages require the existing local installation path in World Studio. Only the compatibility metadata crosses the network.

The two-process runner `tools/test_online_content_rejoin.py` covers original and integrated hosts, a relayed Garage Code, and a missing custom package. It checks for admission only after the verified local restart.
