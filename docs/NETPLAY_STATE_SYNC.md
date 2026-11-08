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
5. Race: event-scoped SNAP at 30 Hz. Guests send their own car to the host; the host sends every other car (its own,
   the AI cars, other guests' cars) to each guest. 62 bytes per car. Guests never apply SNAP to their
   own roster owner. Older/duplicate ticks and invalid/nonfinite states are discarded. A previous
   event's datagram cannot populate a rematch timeline. Wire protocol 5 includes the rules extension.
6. End: when GameDoSingle returns to the main menu the lobby reopens (ONLINE tab).

## Smoothing (no ghosting)
`netplay_car::Remote`: snapshots keyed by the sender's tick; play-head 6 ticks behind the newest;
dead reckoning for up to 15 ticks when late, then hold; the shown car rides its own velocity and
closes the position error 30% per step (50% beyond 8 m), so big corrections are blended over a few
frames while the car stays solid. Contact is native on both machines: each game collides its own car
with the other's puppet, so no hit event is needed in v1 (an owner-side impulse event is the next step).

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
