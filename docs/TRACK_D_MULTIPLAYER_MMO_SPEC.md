# Track D — Stage 1 Private 16-Player Adversarial Multiplayer + Co-Op Career Eves → Stage 2 MMO Server Specification (Milestones 5–6)

> **Status:** Authoritative Engineering & Decompilation Specification (Milestones 5–6)
> **Scope:** Complete retirement of BitTorrent/P2P rendezvous (`src/netplay_online.cpp`) in favor of `eod-server` (QUIC + unreliable datagrams / raw UDP + built-in relay + HTTPS Content-Addressed Mod CDN), full 16-player rigid-body impulse & damage/dent/panel/wheel/chicken/trailer/chain/Career-Eve synchronization (Stage 1), 64+ player Forza Horizon / `nfsu2.xyz` style MMO Free-Roam Hub with instant event instancing (Stage 2), and the 16-client heterogeneous-latency adversarial test harness.
> **Target Binary:** `SLUS_209.10` (*Test Drive: Eve of Destruction*, NTSC-U PS2, SHA-1 verified)

---

## 1. Executive Architecture & `nfsu2.xyz` (`th0mk/nfsu2-web`) Comparative Analysis

### 1.1 Architectural Audit of `nfsu2.xyz` (`th0mk/nfsu2-web` & `server-rs`)

To design an online platform for *Test Drive: Eve of Destruction* that scales from zero-config private 16-player demolition lobbies and co-op Career Eves to a 64+ player persistent free-roam hub, we audited the complete source tree of `nfsu2.xyz` (`th0mk/nfsu2-web`), spanning both its Rust backend (`web/server-rs/`) and its recompiled C runtime (`runtime/roam*.c`, `runtime/racenet.c`, `runtime/mprace.c`, `runtime/mpcredit.c`, `runtime/damage.c`).

#### A. Virtual LAN Switch & Privacy-Preserving Transport (`web/server-rs/crates/lan/src/lib.rs`)
1. **Single-Socket Virtual Subnet (`10.77.0.0/17`):**
   - Clients connect over a single WebSocket (`/lan` or `/lan?resume`) and are assigned a deterministic virtual IPv4 address from `96` `/24` subnets (`10.77.0-95.2..251`, supporting `24,000` concurrent clients per node).
   - Two reserved server-actor VIPs live inside the virtual switch:
     - `10.77.0.1` (`VHOST_VIP`): Emulates the retail EA Online Aries / lobby matchmaking server (`crates/online/src/lib.rs`).
     - `10.77.0.253` (`ROAM_HUB_VIP`, port `9944`): The MMO Free-Roam Hub actor (`crates/roam/src/lib.rs`).
2. **Zero Peer IP Exposure & Keyed Identity Grouping:**
   - Real client IPv4/IPv6 addresses are never transmitted to peers. Instead, the server computes a keyed HMAC `group_id` (per IPv4 `/32` or IPv6 `/56`) and `site_key` (per IPv6 `/48`) for anti-griefing and Sybil/vote-brigading protection.
   - Reconnect tokens (`128-bit` random resume token, `reserve_ms = 60,000 ms`) preserve a player's virtual IP, active race slot, and free-roam state across transient connection drops.
3. **Dual-Cadence Framing, Compression & Lag Shedding (`crates/lan/src/{lib,fpred,lag}.rs`):**
   - **Active Races (`lan_ms = 4 ms`):** Structured race packets bypass batching and use field-level delta prediction (`KIND_COMPACT = 33`, `fpred.rs`) to strip redundant UDP/IP headers and predict kinematic fields.
   - **Free-Roam MMO (`bundle_ms = 50 ms`):** High-fanout free-roam snapshots are coalesced into `KIND_BUNDLE = 32` frames every `50 ms`.
   - **Adaptive Queue Shedding (`lag.rs`):** Per-client outbound queues monitor WebSocket ping/echo RTT; when a client's outbound buffer exceeds `roam_queue = 128 KB` (or `shed_queue = 256 KB`), older unreliable free-roam snapshots are dropped before reliable lobby/control traffic can head-of-line block.

#### B. Public MMO Free-Roam Worlds vs. Structured Lobbies (`crates/roam/` & `crates/online/`)
`nfsu2.xyz` cleanly decouples **Public MMO Free-Roam Worlds** from **Structured Instanced Lobbies**, while bridging them seamlessly in-world:

| Architectural Dimension | `nfsu2.xyz` Public MMO Free-Roam (`crates/roam/` + `runtime/roam_*.c`) | `nfsu2.xyz` Structured Lobbies (`crates/online/` + `runtime/mprace.c`) | EOD 2026 Adaptation & Advancement (`eod-server` + `runtime/net/`) |
| :--- | :--- | :--- | :--- |
| **World Sharding & Rooms** | Players join `World 1..4` (`room = 0..3`) or custom UTF-8 case-folded FNV-1a named rooms (`rc_room_hash`). Driving into a venue mixes `track_room(base, key) = base ^ (key * 0x9E3779B1)`. | Instanced 2–6 player race sessions created via EA Online lobby UI or in-world Free-Roam Race Beacons (`RC_MSG_LOBBY`). | **Stage 1:** Private/Public 16-player room-code lobbies (`EVE-XXXX`) & Co-Op 5-Event Career Eves.<br>**Stage 2:** 64+ player Fairgrounds / Open-World County Hub (`room = 0..N` + custom crew shards) with drive-in Event Gates (`track_room` venue shards). |
| **Spatial Interest Management (AOI)** | `crates/roam/src/links.rs` ticks every `500 ms` across up to `24,576` players. Rooms `<= 256` use exact radix sort; larger rooms use a 2D spatial grid (`MIN_CELL = 5 m`) + lazy ring expander with `25.0x` vertical overpass separation penalty (`slack = 5 m`, `grade = 0.2`) and `1 s` / `4 s` kinematic lookahead + `150 m` camera nose lookahead. | Full mesh broadcast among all `N` race participants (`N <= 6` in NFSU2). | **Stage 1:** Authoritative 60 Hz full-mesh star broadcast across all `16` active cars + dynamic debris/trailers/chains/balls.<br>**Stage 2:** `eod-server` spatial grid AOI (`degree = 32` active 60 Hz physics peers + `16` one-way visual peers + `4,096`-player live map stream `EOD_MSG_MAPD`). |
| **Priority Peering & Crews** | Strict link budget (`degree = 64`, dynamically scaled down to `min_degree = 8` if server CPU/quota exceeds `85%` over `10 s`):<br>1. Active Outrun Duel opponent<br>2. Active Free-Roam Lobby host/joiners<br>3. Crewmates (`HF_CREW`, up to 4 crew hashes, server marks `HF_HUBCREW` without leaking crew hashes)<br>4. Nearest spatial neighbors. | Locked to lobby roster for the duration of the event. | Retains the 4-tier priority hierarchy: (1) Active Wreck Duel / Chain-partner / Gauntlet Hearse, (2) Active Eve / Lobby Queue participants, (3) Demolition Crewmates (`up to 4` crew tags), (4) Nearest spatial neighbors with vertical ramp/figure-8 jump awareness. |
| **Client Car Injection & Smart Ghosting** | `runtime/roam_game.c` allocates `NSLOT = 64` extra `Car` structs outside `World::IndexCars` (preserving ambient AI). Cars are solid within `SOLID_M = 20 m`, but auto-ghost (`RC_BIT_PASS`) for `CLEAR_M = 6 m` on spawn, after `> 5 m` jumps, when stationary `> 20 s`, within `20 m` of shop doors (`DOOR_M`), or when anti-griefing `block_hit` detects `4+` pins from `2+` cars in `60 s`. | Full car-vs-car collision (or host-toggled collision off) across the race grid. | **Stage 1:** 100% solid authoritative demolition combat (no anti-pin ghosting in Derby/Push-Off/Gauntlet; spawn protection only during `2.5 s` Battle respawn fade).<br>**Stage 2 Free-Roam Hub:** Smart Ghosting near Garage/Swap-Meet menus, Event Gate triggers, and newly spawned cars (`6 m` clearance), plus toggleable **Passive Cruiser** vs. **Full-Contact Pit** zones. |
| **Damage, Dent & Cosmetic Sync** | `RC_HF_STATEX` (30 B) syncs 4 wheel heights (`wz`), NOS, hydraulics, tire slip/surface, authentic engine RPM + 3 synth voices, and hood/trunk/doors (`showoff.c`). `RC_MSG_DENTS (20)` syncs up to `66` vertex dents (`11 B` each: 3×`i16` mm pos, 3×`i8` normal, `u8` depth, `u8` strength). | Visual damage only; no fatal wrecking, detached wheels, or constraint physics. | **Full Authoritative Combat Replication:** Syncs scalar damage (`car+0x1BEC`), wreck state (`car+0x1BE8`), 157-bone `Bone.mvl` deformation vectors (`car+0x4B8`), 8 detachable panels (`car+0x13E8` + `car+0x2AC` visibility mask), 4 detachable wheels & collapsed suspension (`car+0x1A00`), trailer hitches, chain constraints, and chicken explosives. |
| **In-World Lobbies & Career Credit** | `runtime/mprace.c` lets any player drop a race beacon (`RC_MSG_LOBBY`) with `LOBBY_PRIVATE`, `LOBBY_STAGED`, and `LOBBY_COUNTS_ON`. Upon completion, `runtime/mpcredit.c` calls native `RaceWon (0x541420)` to award Career bank cash, reputation, sponsor progress, and unlocks! | Standard non-career unranked/ranked multiplayer matches. | **Co-Op & Adversarial Career Eves:** Players can run any 5-event Career Eve (`RaceDayDo` `0x0011F640`) or custom Eve Playlist online with 1–16 humans + AI fill. `eod-server` validates event results and credits authentic Career cash (`0x448008`), Respect Points (`0x44800C`), unlocks, and ambulance/repair bills! |
| **Asset Streaming & Cloud Saves** | `/data-blocks/v2/` serves 1 MiB `zstd`/`brotli`-compressed game chunks with `Cache-Control: immutable`. `/vault` stores cloud saves keyed by SHA-256 bearer token with `If-Match: "<rev>"` ETag optimistic concurrency. | Shares `/data-blocks/v2/` and `/vault`. | `/v1/blobs/<blake3>` serves content-addressed `.eodmod` v2 chunks (`zstd` compressed, Ed25519 signed, SQLite LRU cached) so custom cars/liveries/tracks stream on demand during lobby load or free-roam approach. `/v1/vault` provides ETag-versioned Career cloud saves. |

### 1.2 Why BitTorrent/P2P (`src/netplay_online.cpp`) Is Permanently Retired

In the current codebase, [src/netplay_online.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_online.cpp#L40-L64) and [src/netplay_online_wire.h](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_online_wire.h#L16-L108) implement "Garage Code" discovery by abusing public UDP BitTorrent trackers (`tracker.opentrackr.org:1337`, `open.stealth.si:80`, etc.) via BEP 15 `info_hash` announces (`SHA-1("TDEOD-EVE-NET-1/{H|J|T}/<secret>")`), coupled with raw UDP STUN and UPnP/NAT-PMP/PCP port mapping.

This architecture fails EOD 2026's engineering requirements for five fundamental reasons:
1. **Hard Failure on Symmetric NAT & Carrier-Grade NAT (CGNAT):** When STUN detects destination-dependent port allocation (`rep.symmetric == true` at [src/netplay_online.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_online.cpp#L215-L217)) and UPnP/PCP fails or returns an RFC 6598 CGNAT address (`100.64.0.0/10` at [src/netplay_online_wire.h](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_online_wire.h#L16-L20)), [src/netplay_session.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_session.cpp#L1305-L1308) aborts with `"your internet connection is too strict for direct play"`. Modern cellular, university, fiber CGNAT, and dual-router households cannot host or join.
2. **Public IP Leakage & DDoS Vulnerability:** BitTorrent `announce` replies broadcast every participant's raw public IPv4 and UDP port to anyone who guesses or scrapes the 20-byte room hash.
3. **1.8s–30s Tracker Polling Latency & Public Infrastructure Fragility:** `drain_tsock()` ([src/netplay_online.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_online.cpp#L366-L433)) waits up to `1,800 ms` per connect/announce phase across third-party torrent trackers that routinely rate-limit, drop non-torrent peer IDs (`-EO0001-`), or get blocked by ISP parental/firewall filters.
4. **Host-Asymmetry & Brick-Wall Puppet Physics:** Pure P2P host-star topology without a dedicated authoritative physics/impulse arbitrator causes asymmetric host advantage and lacks bandwidth for 16-player / 64-player state fanout on residential upload links.
5. **No Content-Addressed Mod Delivery:** Whole-tree FNV-1a hashing in [src/netplay_content.h](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_content.h#L16-L71) rejects any client whose `mods/` directory differs by a single byte (`K_NACK` `R_CONTENT` at [src/netplay_session.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_session.cpp#L1011-L1017)), rather than serving missing `.eodmod` blobs over HTTPS.

**Replacement Mandate:** All BEP 15 tracker, UPnP SOAP, NAT-PMP, PCP, and raw STUN hole-punching code in `src/netplay_online.{h,cpp}` and `src/netplay_online_wire.h` is replaced by **`eod-server`**—a standalone, self-hostable Rust/C++ authoritative QUIC + UDP dedicated server and relay with an embedded HTTPS Content-Addressed Storage (CAS) Mod CDN (`/v1/blobs/<blake3>`).

---

## 2. Problems Faced in Current Codebase (`src/netplay_*.cpp` & PS2 Engine) & Exact Solutions

Every limitation in the current `src/netplay_*` implementation and the underlying `SLUS_209.10` PS2 engine has been audited down to exact source lines and guest virtual addresses, paired with its **Anti-Archivist Mutation Strategy** (*Expand & Relocate*, *Hook & Redirect*, or *Bypass to Native*).

| # | Problem / Subsystem Bottleneck | Exact Source File & Lines | Exact `SLUS_209.10` Guest Addresses | Anti-Archivist Strategy & Exact Engineering Solution |
| :- | :--- | :--- | :--- | :--- |
| **P1** | **BitTorrent Tracker Rendezvous, UPnP/STUN Fragility & Peer IP Exposure:** Fails on Symmetric NAT / CGNAT, leaks public IPs via BEP 15 trackers, and stalls on DNS/tracker timeouts. | [src/netplay_online.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_online.cpp#L40-L539)<br>[src/netplay_online_wire.h](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_online_wire.h#L16-L246)<br>[src/netplay_session.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_session.cpp#L1275-L1336) | N/A (Host runtime networking) | **Bypass to Native (`eod-server` QUIC/UDP):** Retire `netplay_online.{h,cpp}` & `netplay_online_wire.h`. Connect all clients to `eod-server` over QUIC (RFC 9000 reliable streams + RFC 9221 unreliable datagrams, with raw UDP + built-in token-authenticated server relay fallback). Zero port forwarding, zero STUN failure, zero peer IP exposure. |
| **P2** | **EE Simulation Thread Network Polling & Blocking `wait_until` Loops:** Socket `recv()`/`send()` and `45s..300s` blocking loops run directly on the EE vblank/simulation thread. | [src/netplay_session.h](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_session.h#L3-L4)<br>[src/netplay_session.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_session.cpp#L949-L959)<br>[src/netplay_session.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_session.cpp#L1338-L1697) | `0x0011F640` (`RaceDayDo`)<br>`0x00177718` (`WorldSimulationStep`)<br>`0x00124F48` (`EventPreStartWait`) | **Bypass to Native (Dedicated I/O Thread + Lock-Free SPSC Ring Buffers):** Move all socket I/O, QUIC crypto, packet pacing, and Opus voice encoding/decoding to a dedicated `NetIoThread`. The EE simulation thread communicates exclusively via two lock-free wait-free SPSC queues (`SimToNetQueue`, `NetToSimQueue`) with `< 50 µs` deterministic tick budget. |
| **P3** | **Brick-Wall Puppet Physics & Zero Car-vs-Car Impulse Arbitration:** Remote cars (`g_puppets[16]`) have their `0xB0` rigid body and `Car` pose overwritten both *before* (`step_begin`) and *after* (`step_end`) `WorldSimulationStep`, making them infinite-mass brick walls that absorb hits without recoiling or notifying the remote owner. | [src/netplay_car.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_car.cpp#L84-L95)<br>[src/netplay_session.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_session.cpp#L1677-L1694)<br>[src/netplay_puppet.h](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_puppet.h#L27-L63) | `0x00177718` (`WorldSimulationStep`)<br>`0x00189CD0` (`CarCollisionResponse`)<br>`0x0018CA98` (`CarApplyCollisionDamage`)<br>`*(0x00494D30) + idx*0xB0` (RigidBody pool) | **Hook & Redirect (Authoritative Bilateral Impulse Solver):**<br>1. Let `WorldSimulationStep (0x177718)` execute full finite-mass rigid-body response on remote puppets during the tick (do **not** clobber post-collision velocity in `step_end`; instead blend predicted post-impact velocity with incoming authoritative snapshots via Hermite + spring-damper correction).<br>2. Hook `0x00189CD0` / `0x0018CA98` to capture the exact world-space contact point, normal, relative velocity, and impulse vector $\vec{J}$, emitting `EODN_PKT_COLLISION_IMPULSE (0x04)` to `eod-server` for bilateral arbitration and credit attribution (`0x00202DA8`). |
| **P4** | **Dropped Damage Scalar & Missing Wreck / Smoke / Totaled State Sync:** `netplay_car::read()` reads `car + 0x1BEC` (`damage`), and `netplay_wire.h` packs it into `K_SNAP`, but `netplay_car::write()` **never writes `s.damage` back to guest memory**, nor does it sync wreck state (`car + 0x1BE8`), engine smoke emitters, or impair credit (`0x001DA468`). | [src/netplay_car.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_car.cpp#L58)<br>[src/netplay_car.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_car.cpp#L84-L95)<br>[src/netplay_wire.h](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_wire.h#L54-L64)<br>[src/car_damage.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/car_damage.cpp#L74-L165) | `car + 0x1BEC` (`float damage`)<br>`car + 0x1BE8` (`i32 wreck_state`)<br>`0x0017CF28` (`CarUpdateSmokeLevel`)<br>`0x0018C9E8` (`CarTriggerTotaledState`)<br>`0x001DA468` (`ContestNotifyCarImpaired`) | **Hook & Redirect:** Write `car + 0x1BEC` and `car + 0x1BE8` in `netplay_car::apply_damage_delta()`. When `wreck_state` crosses thresholds `1..4`, invoke `0x0017CF28` (`CarUpdateSmokeLevel`) on the remote puppet; when `wreck_state` transitions to `10/11/12` (totaled), invoke `0x0018C9E8` and `0x001DA468` so all clients see identical fire/smoke plumes, elimination banners, and wreck scoring. |
| **P5** | **Missing 157-Bone Procedural Vertex Denting (`Bone.mvl`), 8 Detachable Panels & 4 Detachable Wheels Sync:** Remote cars stay showroom-pristine even after taking 100% damage because `car + 0x4B8` (bones), `car + 0x13E8` (panels), `car + 0x2AC` (part visibility mask), and `car + 0x1A00` (wheels/suspension) are never replicated, and `debris_pool::expanded()` disables expanded debris when `online == true`. | [src/car_damage.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/car_damage.cpp#L168-L412)<br>[src/bone_pose.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/bone_pose.cpp#L1-L95)<br>[src/debris_pool.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/debris_pool.cpp#L110-L113) | `car + 0x4B8` (`157 × 12B` bone offsets)<br>`car + 0x13E8` (`8 × 0x18B` panels)<br>`car + 0x2AC` (`u32` visibility mask)<br>`car + 0x1A00` (`4 × 0x54B` wheels)<br>`0x001E0DC8` (`SpawnPanelDebris`)<br>`0x001B3068` (`SpawnWheelDebris`)<br>`0x001E06A8` (`HalveSuspensionStiffness`) | **Hook & Redirect + Expand & Relocate:**<br>1. Remove `!online` restriction in [src/debris_pool.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/debris_pool.cpp#L112) so all clients run the expanded 64-panel (`0x01C00000`) and 32-wheel (`0x01C10000`) pools.<br>2. Replicate `EODN_PKT_CAR_DAMAGE_DELTA (0x03)` containing quantized bone deformation deltas (`u8 bone_idx`, `3×i8` mm offset, `u8 shade`), the 8-panel 2-bit state vector (`16 bits`), and the 4-wheel detachment mask (`4 bits`).<br>3. On state transition (`panel -> 3` or `wheel & 0x80`), trigger `0x001E0DC8` / `0x001B3068` + `0x001E06A8` deterministically on every client. |
| **P6** | **Zero Chicken Weapon / Explosive Projectile / Shockwave Sync & 16-Weapon Registry Cap:** Firing or detonating chicken rockets in Battle/Chicken modes is purely local; `g_WeaponRegistry` holds at most 16 weapons and `0x20FD48` only attaches weapons to `0x50` (local human) cars unless patched. | [src/game_patches.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/game_patches.cpp#L306-L330)<br>[src/game_patches.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/game_patches.cpp#L458-L492)<br>[src/battle_bots.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/battle_bots.cpp#L130-L154) | `0x003CE3F8` (`g_WeaponRegistry[16]`)<br>`0x003CE4B8` (`g_WeaponCount`)<br>`0x0020FD48` (`SceneCreateCarAndWeapons`)<br>`0x0017E000` (`ChickenWeaponTrigger`)<br>`0x0017E718` (`ChickenWeaponDetonate`)<br>`0x0017DA58` (`ChickenExplosionShockwave`) | **Expand & Relocate + Hook & Redirect:**<br>1. Relocate `g_WeaponRegistry` from `0x003CE3F8` (`16 × 12B`) to high guest memory `0x01C30000` (`64 × 12B`) so all 16 cars in Stage 1 (and 64 in Stage 2) can carry dual weapon pods.<br>2. Extend the `0x0020FD48` wrapper so remote human puppets receive weapon instances in any weapon-enabled event.<br>3. Hook `0x0017E000` (fire) and `0x0017E718` (detonate) into `EODN_PKT_EVENT_MODE_STATE (0x05)` (`SUB_WEAPON_FIRE` / `SUB_WEAPON_DETONATE`). |
| **P7** | **Multi-Event Eve Unwind Bug (`EveHasMoreEvents` Forced to 0) & No Career Co-Op Sync:** `f_00120FB8` explicitly returns `0` when `netplay::active()`, terminating every online session after Event 1 and preventing 5-event Career Eves (`RaceDayDo`) from running online. | [src/session_start.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/session_start.cpp#L1000-L1007)<br>[src/netplay_session.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_session.cpp#L1525-L1630)<br>[src/hle_career.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/hle_career.cpp#L1-L120) | `0x0011F640` (`RaceDayDo`)<br>`0x00120650` (`EventSetupSelectInitial`)<br>`0x00120FB8` (`EveHasMoreEvents`)<br>`0x003C4BF8` (`EveTracker` struct)<br>`0x00447F4C` (`CareerSaveInfo`) | **Hook & Redirect:** Remove the forced `ctx.ret_s32(0)` early-exit in `f_00120FB8`. Instead, when `EveHasMoreEvents (0x00120FB8)` returns `1`, synchronize inter-event advancement via `EODN_PKT_CAREER_EVE_PROGRESSION (0x06)` (`event_index 0..4`, cumulative Eve points table at `EveTracker + 0x20`, repair/ambulance deductions, and post-Eve Career cash/respect payout). |
| **P8** | **Battle Mode 12-Car Cap (`kCapacity = 12`, `kMaxCars = 10`) & Retail 20-Car Pool Limits:** `BattleMaster` (`0x003CF194`) was only expanded from 4 to 12 slots (`kMaxCars = 10`), blocking 16-player Battle matches; retail car pools are capped at 20 (`16` in `netplay_car::slot_count()`) and `7` unique chassis per Eve (`0x00123EE0`). | [src/battle_layout.h](file:///Users/monty/Documents/GitHub/eod-recomp/src/battle_layout.h#L19-L65)<br>[src/battle_driver.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/battle_driver.cpp#L1-L120)<br>[src/netplay_car.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_car.cpp#L12-L16) | `0x003CF194` (`BattleMaster`)<br>`0x0012C158` (`BattleGetMaxCars`)<br>`0x00485230` (`g_DriverTable[20]`)<br>`0x00447458` (`g_ParticipantCarTable[20]`)<br>`0x003CE4F8` (`g_CarEntityList[20]`)<br>`0x00123EE0` (7-chassis cap) | **Expand & Relocate:**<br>1. **Stage 1 (16-Player Lobbies):** Expand `battle_layout::kCapacity` from `12 -> 16` (`kMaxCars = 16`, `kTotalSize = 0x2B4`) in high guest arena `0x01C20000`, and bypass the 7-unique-chassis check at `0x00123EE0` using our expanded 128 MB host-backed heap (`0x02000000+`).<br>2. **Stage 2 (64+ Player MMO Hub):** Relocate all 6 vehicle/driver/rigid-body pools (`0x485230`, `0x447458`, `0x3CE4F8`, `*(0x494D30)`, `0x3D1C90`, `0x494C60`) to `0x02400000` with capacity `N = 64` active cars. |
| **P9** | **Blocking FNV-1a Whole-Folder Content Hash & Hard Mod Mismatch Rejection:** Walks `mods/` synchronously on the main thread and aborts join (`R_CONTENT`) if hashes differ instead of streaming missing mods. | [src/netplay_content.h](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_content.h#L16-L71)<br>[src/netplay_session.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_session.cpp#L1011-L1017) | N/A (Host filesystem / mod loader) | **Bypass to Native (BLAKE3 `.eodmod` v2 Manifest + HTTPS CAS Streaming):** Replace FNV-1a directory walk with pre-indexed BLAKE3 content hashes per `.eodmod` v2 package. During `0x01 Handshake`, clients fetch any missing BLAKE3 blobs from `https://<eod-server>/v1/blobs/<blake3>` into `~/.local/share/eod/mod_cache/blobs/` and hot-mount them into `VirtualDisc` before grid spawn. |
| **P10** | **Unsynchronized Special Event Mechanics (Trailers, Chains, Soccer Ball, Flag Carrier, Gauntlet Hearse, Detention, Red Rover, Push-Off):** `K_SNAP` only sends 16 car transforms, leaving trailer hitches, chain constraints, the Soccer ball rigid body, Capture the Flag holder/timers, and Gauntlet target state unsynchronized. | [src/netplay_wire.h](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_wire.h#L17-L29)<br>[src/netplay_session.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_session.cpp#L1640-L1697) | Contest controllers created by `0x001D97B0` & `0x001D99A8` (`0x0013F3F0..0x00174710`) | **Hook & Redirect:** Add `EODN_PKT_EVENT_MODE_STATE (0x05)` with union sub-payloads for every one of EOD's 26 contest modes (detailed in Section 3.4 & Section 4.6). |


---

## 3. Decompiled Subsystem Reference & Memory Map (`SLUS_209.10`)

### 3.1 Vehicle Pool, Driver Table & Rigid-Body Physics Memory Map

In `SLUS_209.10`, active vehicles are cross-referenced across six parallel structures initialized by `EveRosterPopulate (0x00123EA0)`, `RaceDaySetupGrid (0x0013D2C0)`, and `SceneCreateCarAndWeapons (0x0020FD48)`:

| Guest Address / Symbol | Retail Capacity & Stride | Stage 1 (16-Player) Relocation | Stage 2 (64-Player MMO) Relocation | Purpose & Decompiled Layout |
| :--- | :--- | :--- | :--- | :--- |
| `0x00447450` (`g_ParticipantCount`) | `i32` (`1..20`, `netplay_car` clamped to `16`) | `1..16` | `1..64` | Total active vehicles in the current simulation world. |
| `0x00447458` (`g_ParticipantCarTable`) | `20 × 4 B` (`u32 car_ptr[]`) | In-place (`16 <= 20`) | `0x02400000` (`64 × 4 B`) | Primary indexed array of `Car*` pointers (`slot 0..N-1`) read by [src/netplay_car.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_car.cpp#L20). |
| `0x00485230` (`g_DriverTable`) | `20 × 0x78 B` (`120 B` per slot) | In-place (`16 <= 20`) | `0x02400200` (`64 × 0x78 B`) | Session driver descriptors (`+0x00` type `0x50`=Human / `0x51`=AI, `+0x04` chassis ID, `+0x34` driver name, `+0x58` grid slot, `+0x68` Eve score). |
| `0x003CE4F8` (`g_CarEntityList`) | `20 × 4 B` + count at `0x003CE578` | In-place (`16 <= 20`) | `0x02402000` (`64 × 4 B`) | Active scene `Car*` entity array iterated by damage (`0x0018CA98`), shockwaves (`0x0017DA58`), and AI targeters. |
| `*(0x00494D30)` (`g_RigidBodyPool`) | `N × 0xB0 B` (`176 B` per rigid body) | In-place (`16 <= 20`) | `0x02402400` (`128 × 0xB0 B`) | Master rigid-body physics array stepped by `WorldSimulationStep (0x00177718)`. Includes cars, trailers, and dynamic balls. |
| `0x0048D2B4` (`g_PhobSpatialGrid`) | `100 × 100` cells (`20 m` cell = `±1000 m`) | In-place | `0x02410000` (`512 × 512` cells = `±5120 m`) | Broadphase 2D spatial hash (`phoblist`) used by `0x00177718` for car-vs-car and car-vs-prop collision detection. |

#### Decompiled `Car` (`~0x1C40` bytes) & `RigidBody` (`0xB0` bytes) Kinematic Offsets
From [src/netplay_car.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_car.cpp#L41-L95) and our disassembly of `CarSyncFromRigidBody (0x00189D08)` and `CarApplyCollisionDamage (0x0018CA98)`:

```cpp
// Master RigidBody Record at *(uint32_t*)0x00494D30 + (rb_index * 0xB0)
struct GuestRigidBody_0xB0 {
    /* +0x00 */ float    rot_3x3[9];      // 3x3 world rotation matrix (orthonormalized via 0x002122D0)
    /* +0x24 */ float    pos[3];          // World position (X, Y, Z in meters)
    /* +0x30 */ float    lin_vel[3];      // Linear velocity (m/s) -> copied to car+0x294 and car+0x104
    /* +0x3C */ uint32_t flags_pad;       // +0x40 uint16_t surface/contact flags -> copied to car+0x94
    /* +0x44 */ float    ang_vel[3];      // Angular velocity (rad/s)
    /* +0x50 */ float    inv_mass;        // Inverse mass (1/kg)
    /* +0x54 */ float    inv_inertia[9];  // World-space inverse inertia tensor
    /* +0x78 */ float    force_accum[3];  // Accumulated force for current 60 Hz integrator step
    /* +0x84 */ float    torque_accum[3]; // Accumulated torque for current 60 Hz integrator step
    /* +0x90 */ uint8_t  reserved[0x20];
}; // sizeof == 0xB0 (176 bytes)

// Key Offsets inside Guest Car Object (car_ptr = *(uint32_t*)(0x00447458 + slot * 4))
struct GuestCarOffsets {
    /* +0x0064 */ float    world_mat[12];      // 3x3 rotation (+0x64..+0x84) + Vec3 position (+0x88, +0x8C, +0x90)
    /* +0x0094 */ uint16_t contact_flags;      // Copied from rb+0x40 in 0x00189D9C
    /* +0x0104 */ float    lin_vel[3];         // World linear velocity (m/s)
    /* +0x0110 */ float    ang_vel[3];         // World angular velocity (rad/s)
    /* +0x0160 */ uint32_t driver_slot_index;  // Index into g_DriverTable (0..N-1)
    /* +0x01A8 */ uint32_t wheel_count;        // Typically 4
    /* +0x01AC */ uint32_t susp_array_ptr;     // Pointer to 4 * 0x80 suspension structs (stiffness at +0x70)
    /* +0x01D8 */ uint32_t rb_pool_index;      // Index into *(0x00494D30) RigidBody array (stride 0xB0)
    /* +0x01DC */ uint32_t physics_ proxy_ptr; // Passed to 0x001E0648 on reset
    /* +0x01E8 */ uint32_t controller_ptr;     // Controller object (+0x64 = type: 0x50 human, 0x51 AI)
    /* +0x025C */ uint32_t bone_mvl_ptr;       // Pointer to Bone.mvl vertex deformation envelope table
    /* +0x0294 */ float    prev_lin_vel[3];    // Pre-step linear velocity cached by 0x00189D5C
    /* +0x02AC */ uint32_t part_vis_mask;      // 32-bit visibility bitmask for 4SPS detachable mesh groups
    /* +0x04B8 */ uint32_t bone_offsets[157][3];// 157 bones * 12B {float dx, float dy, float dz_with_shade_u8}
    /* +0x13E8 */ uint8_t  panels[8][0x18];    // 8 detachable body panels (hood, trunk, doors, bumpers, fenders)
    /* +0x1A00 */ uint8_t  wheels[4][0x54];    // 4 wheel records (+0x04 spin, +0x3C steer, +0x44 travel, +0x50 flags)
    /* +0x1B5C */ float    forward_speed;      // Dot(lin_vel, forward_axis) computed at 0x00189DB0..0x00189DC8
    /* +0x1B94 */ float    throttle;           // 0.0 .. 1.0
    /* +0x1B98 */ float    brake;              // 0.0 .. 1.0
    /* +0x1B9C */ float    steer;              // -1.0 .. +1.0
    /* +0x1BA8 */ float    handbrake;          // 0.0 .. 1.0
    /* +0x1BE4 */ float    inv_damage_mult;    // Vulnerability scalar (~1.0f; 0.0f = invulnerable)
    /* +0x1BE8 */ int32_t  wreck_state;        // 0=intact, 1..4=smoke stages, 10=engine fire, 11=wheel loss, 12=totaled
    /* +0x1BEC */ float    damage;             // Cumulative normalized damage (0.0f = fresh, >= 1.0f = wrecked)
    /* +0x1C04 */ int32_t  wheel_toss_side;    // 0=Front-Left, 1=Front-Right, 3=Rear
    /* +0x1C14 */ uint32_t dirty_part_vis_mask;// Bitmask of newly detached panel groups awaiting 0x0018C990 commit
};
```

---

### 3.2 Car Damage, 157-Bone Mesh Denting (`Bone.mvl`), Detachable Panels & Wheels (`0x0018CA98`)

Disassembly of `CarCollisionResponse (0x00189B00..0x00189CD8)` and [src/car_damage.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/car_damage.cpp#L1-L433) (`f_0018CA98`) reveals the complete four-stage damage pipeline executed on every car-vs-car or car-vs-world impact:

1. **Impact Vector Transform & Credit Attribution (`0x00189C00..0x00189CD4` -> `0x0018CA98`):**
   - `0x00189B00` computes the world-space contact point and impulse vector between two colliding bodies, transforms both into the victim car's local object space (`0x50($sp)` = local impact position, `0x60($sp)` = local force/impulse vector), and calls:
     ```cpp
     CarApplyCollisionDamage_0018CA98(
         uint32_t victim_car_ptr,   // $a0
         uint32_tlocal_hit_pos_ptr,// $a1
         uint32_t local_force_ptr,  // $a2
         int32_t  is_weapon_hit,    // $a3 (0 = collision, 1 = chicken explosion shockwave from 0x0017DA58)
         uint32_t attacker_car_ptr, // $t0 (0 if world/wall)
         uint32_t hit_part_code     // $t1
     );
     ```
   - Inside `0x0018CA98`, the damage delta `raw_dmg = |force| * 0.00018181818f * *(float*)(car + 0x1BE4)` is scaled by the career armor multiplier (`career_upgrades::damage_taken_mult`) and directional zone multiplier (`0.70x` front bumper `y > 1.0 m`, `1.20x` rear trunk `y < -1.0 m`, `1.35x` T-bone side doors `|x| > 0.7 m`).
   - If `attacker_car_ptr != 0`, `0x0018CA98` calls `ContestAwardDamageCredit (0x00202DA8)` with `(contest_ptr, victim_slot, attacker_slot, scaled_dmg, is_kill)` to award wreck points, cash bonuses, and HUD hit notifications.

2. **Scalar Health, Smoke Stages & Totaled Elimination (`car + 0x1BEC`, `car + 0x1BE8`):**
   - `car + 0x1BEC` (`damage`) accumulates `0.0f -> 1.0f`.
   - Crossing `0.20, 0.40, 0.60, 0.80` increments `car + 0x1BE8` (`wreck_state` `1..4`) and calls `CarUpdateSmokeLevel (0x0017CF28)`.
   - Crossing `1.00` transitions `car + 0x1BE8` to `10` (engine explosion), `11` (catastrophic wheel shear), or `12` (chassis total), calling `CarTriggerTotaledState (0x0018C9E8)` and `ContestNotifyCarImpaired (0x001DA468)`.

3. **157-Bone Procedural Vertex Denting (`car + 0x4B8` & `Bone.mvl` at `*(car + 0x25C)`):**
   - Each car mesh is skinned against up to `157` deformation bones (`car + 0x4B8`, stride `12 B`: `float dx, dy, dz`).
   - Low byte of the 3rd float (`car + 0x4B8 + bone*12 + 8`) stores a packed `uint8_t` crease-darkening shade (`0x00..0xFF`, where `0xFF` = unblemished and lower values darken the vertex color in VU1 via [src/bone_pose.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/bone_pose.cpp#L48-L82) at `0x0034CA18 -> 0x0035DD28`).
   - On impact, `0x0018CA98` iterates `Bone.mvl` envelopes within `radius = 1.65 m` of `local_hit_pos`, displacing `dx, dy, dz` inward along the impact vector (clamped to `±0.38 m` max displacement) and darkening the low byte shade proportional to dent depth.

4. **8 Detachable Body Panels (`car + 0x13E8`) & 4 Detachable Wheels (`car + 0x1A00`):**
   - **Panels (`car + 0x13E8 + i * 0x18`, `i = 0..7`):**
     - `+0x00`: `u32 scene_node_ptr`
     - `+0x04`: `i32 hinge_bone_a`, `+0x08`: `i32 hinge_bone_b`
     - `+0x0C`: `float strain_a`, `+0x10`: `float strain_b`
     - `+0x14`: `u32 state` (`0` = intact, `1` = dented, `2` = unlatched/flapping on hinge, `3` = detached)
     - When `strain_a + strain_b` exceeds threshold, `state` advances to `3`, setting bit `1u << (part_group & 31)` in `car + 0x1C14` (`0x0018C990` clears that bit in `car + 0x2AC` `part_vis_mask` so the attached 4SPS submesh disappears) and calling `SpawnPanelDebris (0x001E0DC8)` into the `Panl` rigid-body debris pool (`0x00494D60`, relocated to `0x01C00000` with 64 slots).
   - **Wheels (`car + 0x1A00 + w * 0x54`, `w = 0..3`):**
     - `+0x04`: `float spin_angle`, `+0x3C`: `float steer_angle`, `+0x44`: `float susp_travel`, `+0x50`: `u32 flags`.
     - When a severe corner hit or `wreck_state == 11` occurs, bit `0x80` is set in `wheel + 0x50`, `HalveSuspensionStiffness (0x001E06A8)` drops that corner's spring constant at `*(car + 0x1AC) + w * 0x80 + 0x70` so the chassis drags sparks on the dirt/asphalt, and `SpawnWheelDebris (0x001B3068)` launches a free-rolling `WHLP` wheel rigid body (`0x00484E68`, relocated to `0x01C10000` with 32 slots).

---

### 3.3 Trailer Hitch Constraints (`TrailerMaster` `0x00174710`) & Chain Race Constraints (`0x0013F3F0`)

1. **Trailer Race / Egg Trailer / Figure-8 Trailer (`TrailerMaster` at `0x00174710`, tag `'TrailerMaster'` `0x003F8A50`, size `0x250`, vtable `0x003F8A60`):**
   - Each participant car (`slot 0..N-1`) pulls a secondary trailer rigid body allocated in `*(0x00494D30)` (`rb_index = N + slot`) linked by a spherical ball-and-socket hitch constraint.
   - `TrailerMaster` (`0x250` bytes) tracks per-car trailer state (`+0x0C + slot * 0x20`):
     - `+0x00`: `u32 tow_car_ptr`
     - `+0x04`: `u32 trailer_entity_ptr` (with its own rigid body index and breakable cargo/boat/camper mesh)
     - `+0x08`: `u32 hitch_attached` (`1` = hitched, `0` = trailer broken free / detached!)
     - `+0x0C`: `float trailer_integrity` (`1.0f -> 0.0f`; in `egg_trailer_race` ID `4`, losing the trailer or cracking the egg cargo penalizes or disqualifies the driver; in `trailer_race` ID `2` and `figure8_trailer_race` ID `6`, crossing the finish line without your trailer refuses lap credit!).
2. **Chain Race (`chain_race` ID `37`, flags `0x00008125`, bit `0x8000` = paired chain constraint):**
   - Cars `(2*k, 2*k + 1)` (up to 8 pairs = 16 cars) are tethered tail-to-nose by a distance-limited spring-damper chain constraint (`max_length = 6.5 m`, `stiffness = 48,000 N/m`) solved inside `WorldSimulationStep (0x00177718)`.
   - **Multiplayer Requirement:** Because paired cars in Chain Race exert huge bilateral tension impulses on each other every frame whenever their distance exceeds `6.5 m`, both cars in a pair `(2*k, 2*k + 1)` **must** be simulated with authoritative server-arbitrated spring tension (`EODN_SUB_CHAIN_TENSION`), and in Stage 1 co-op/adversarial lobbies two human players can drive the front and rear car of the same chained pair!

---

### 3.4 All 26 Event Modes & Contest Controller Factory (`0x001D97B0`)

Disassembly of `ContestControllerFactory (0x001D97B0)` and the 12-byte Master Event Table at `0x003C3A70` (`39` entries, `26` distinct playable contest modes) yields the complete controller map required for `EODN_PKT_EVENT_MODE_STATE (0x05)`:

| Event ID(s) | Internal Name(s) (`0x003C3A70`) | Event Flags (`0x9A4($a0)`) | Constructor & Vtable | Allocation Tag & Struct Size | Authoritative Netplay State to Replicate (`0x05 EventModeState`) |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **16, 5, 8, 9, 10, 11, 12, 13, 14, 15, 18, 24, 25, 31** | `normal_race`, `figure8_race`, `fwd_bwd_race`, `bwd_race`, `suicide_race`, `shortcut_race`, `flagpole_race`, `reverse_race`, `jump_race`, `figure8_jump_race`, `stop_and_go_race`, `last_man_race`, `whip_around_race`, `school_bus` | `0x00002110`..`0x02002109` (`flags & 0x00080000 == 0`, race bit `0x2000`) | `0x0013F3F0`<br>Vtable: `0x003F3F30` | `'RaceMaster'` (`0x003F3D50`)<br>Size: `0x94` + `N × 0x10` | Per-car lap counter, checkpoint index, `fwd_bwd` / `whip_around` direction flip state, `flagpole` orbit completion bit, `stop_and_go` stop-box dwell timer, `suicide_race` opposing-direction cohort bit (`slot & 1`), `last_man_race` lap-elimination mask. |
| **37** | `chain_race` (Chain Race) | `0x00008125` (bit `0x8000` chain constraint) | `0x0013F3F0`<br>Vtable: `0x003F3F30` | `'RaceMaster'` (`0x003F3D50`)<br>Size: `0x94` + `8` chain links | All `'RaceMaster'` fields + 8 chain tether constraints (`car_a`, `car_b`, `rest_len`, `current_tension`, `broken_flag`). |
| **2, 4, 6** | `trailer_race`, `egg_trailer_race`, `figure8_trailer_race` | `0x00002B02`, `0x00003904`, `0x01002906` | `0x00174710`<br>Vtable: `0x003F8A60` | `'TrailerMaster'` (`0x003F8A50`)<br>Size: `0x250` | Per-car trailer rigid-body pose (`pos`, `rot_quat`, `lin_vel`, `ang_vel`), hitch attachment bit (`0/1`), and trailer/egg integrity (`0..100%`). |
| **19** | `red_rover` (Red Rover) | `0x08882213` | `0x0013F3F0`<br>Vtable: `0x003F3F30` | `'FootballMaster'` (`0x003F3D20`)<br>Size: `0x94` + `N × 0x10` | End-zone wave number, active defender/blocker team mask, runner survival mask, and score per driver. |
| **20** | `gauntlet` (Gauntlet) | `0x00004214` | `0x00147920`<br>Vtable: `0x003F4B08` | `'GauntletMaster'` (`0x003F49C8`)<br>Size: `0x18` | Hearse/Target driver slot (`+0x08`), target laps completed, runner pack wreck attribution bonuses, and Hearse health. |
| **21** | `soccer` (Soccer Match) | `0x18000215` | `0x0014B1F0`<br>Vtable: `0x003F5230` | `'SoccerMaster'` (`0x003F5218`)<br>Size: `0xF18` | Giant Soccer Ball rigid-body state (`pos[3]`, `rot_quat[4]`, `lin_vel[3]`, `ang_vel[3]`, last-touch driver slot), Team A / Team B score, goal reset timer. |
| **22** | `king_of_ring` (King of the Ring) | `0x18800216` | `0x00157AA8`<br>Vtable: `0x003F5B78` | `'KingOfRingMaster'` (`0x003F5B60`)<br>Size: `0xB4` | Ring center & radius, per-driver ring occupancy bitmask (`u16`), cumulative ring-hold points (`float[16]`), and out-of-bounds timer. |
| **23** | `pointtopoint` (Point-To-Point) | `0x08000217` | `0x001523B0`<br>Vtable: `0x003F58D8` | `'P2PMaster'` (`0x003F58B0`)<br>Size: `0x6F18` | Active random beacon coordinate (`Vec3`), current beacon index, and per-driver beacon capture score (`u16[16]`). |
| **26** | `wrangle` (Wrangling) | `0x0880421A` | `0x001597E8`<br>Vtable: `0x003F5E28` | `'WrangleMaster'` (`0x003F5E08`)<br>Size: `0x60` + `N × 0x0C` | Quarry/prey car slot(s), tag/capture progress timers per hunter, and wrangle score table. |
| **27, 29, 3** | `stuntman` (Stunt Man), `accuracy_jump`, `jump_for_dough` | `0x0C00011B`, `0x0800041D`, `0x00000403` | `0x0015C708`, `0x00145EE8`, `0x00145B78` | `'Stuntman'` (`0x5B0`), `'acc_master'` (`0x5C`), `'stunt_mstr'` (`0x158`) | Airborne stunt telemetry (distance, height, roll/flip revolutions, landing accuracy target distance) and awarded stunt cash/points. |
| **28** | `save_ring` (Survival) | `0x1800421C` | `0x001608F8`<br>Vtable: `0x003F6D88` | `'SaveRingMaster'` (`0x003F6D68`)<br>Size: `0x1294` | Shrinking safe-zone ring state, survival elimination order, and active hazard timers. |
| **30, 32, 33, 1, 7, 17** | `kill_derby`, `demo_derby`, `kill_push`, `pushoff`, `kill_knock`, `knockout` | `0x1880221E`, `0x18882201`, `0x18802220`, `0x18882207`, `0x18802221`, `0x18882211` | `0x0013F780`<br>Vtable: `0x003F3DF0` | `'KillDerbyMaster'` (`0x003F3D40`)<br>Size: `0x5C4` | Alive/eliminated bitmask (`u16`), ring-out / out-of-bounds elimination bitmask (for Push-Off & Knock-Out), per-driver hit score, kill count, and 60s inactivity warning timers. |
| **34** | `capture` (Capture the Flag / Downtown) | `0x18006222` | `0x001645B0`<br>Vtable: `0x003F7238` | `'CaptureMaster'` (`0x003F7228`)<br>Size: `0x8860` | Current flag carrier slot (`i8`, `-1` if loose), loose flag world position (`Vec3`), active drop/delivery goal position (`Vec3`), carrier immunity cooldown (`1.5 s`), and per-driver flag score. |
| **35** | `detention` (Detention) | `0x09002223` | `0x00165D20`<br>Vtable: `0x003F7448` | `'DententionMaster'` (`0x003F73D8`)<br>Size: `0x24` + `N × 0x10` | Per-driver penalty detention state, time-in-zone / tag transfer cooldowns, and standings. |
| **36** | `battle` (Battle / Chicken Mode) | `0x18000224` | `0x00167F30` -> `0x00166C60`<br>Vtable: `0x003F7770` | `'BattleMaster'` (`0x003F7620`)<br>Expanded Size: `0x2B4` (`16` slots) | Per-slot kills, deaths, suicidal deaths, active state (`1=alive, 3=dying, 4=respawning`), respawn timer (`+0x234`), weapon ammo/cooldown, plus Chicken projectile fire (`0x0017E000`) and detonation (`0x0017E718`) events. |
| **38** | `home` (Driving Around / Free-Roam Hub!) | `0x08000226` | `0x0016EF58`<br>Vtable: `0x003F8390` | `'HomeMaster'` (`0x003F8350`)<br>Size: `0xC04` | **Foundation of Stage 2 MMO Free-Roam Hub:** Retail engine's open-world driving controller (`home`). Extended to 64 active cars + spatial AOI + drive-in Eve portals! |

---

### 3.5 Multi-Event Career "Eve" Progression (`RaceDayDo` `0x0011F640`, `EveTracker` `0x003C4BF8`, `CareerSaveInfo` `0x00447F4C`)

Disassembly of `RaceDayDo (0x0011F640)`, `EventSetupSelectInitial (0x00120650)`, `EveHasMoreEvents (0x00120FB8)`, and [src/hle_career.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/hle_career.cpp#L1-L140) reveals how a 3-to-5-event Career Eve (usually 2 qualifying races + 1 special event + 1 semi-final + 1 final Demolition Derby / Gauntlet / Trailer / Chain event) is orchestrated:

1. **`EveTracker` Global Struct (`0x003C4BF8`..`0x003C5480`):**
   - `0x003C4BDC`: `i32 is_raceday_active` (set to `1` at `0x0011F740`)
   - `0x003C4BF8`: `u32 eve_descriptor_ptr` (points to the active Eve's event list, venue ID, entry fee, and purse table)
   - `0x003C4BFC`: `i32 current_event_index` (`0..4`, incremented by `EveAdvanceNextEvent`)
   - `0x003C4C00`: `i32 total_events_in_eve` (`1` for Single Action, `3..5` for Career Eve / Custom Tournament)
   - `0x003C4C18`: `i32 participant_points[20]` (cumulative Eve points across events `0..4`, sorted between events to determine the starting grid of the next event and the final Eve podium payout!)
   - `0x004494E0`: `i32 game_flow_mode` (`4` = Career RaceDay / Eve mode checked at `0x0011F6C4`)

2. **`CareerSaveInfo` Global Struct (`0x00447F4C`, size `0xD570`):**
   - `0x00448008` (`+0xBC`): `i32 career_cash` (bank balance in dollars)
   - `0x0044800C` (`+0xC0`): `i32 career_respect_points` (determines Career Rank `1..15` and Eve unlock gating)
   - `0x00448010` (`+0xC4`): `i32 career_wins`, `i32 career_eves_entered`, `i32 career_wrecks_inflicted`
   - `0x00448120` (`+0x1D4`): Garage car slots (`up to 5` owned cars in retail; each storing chassis ID, current health/damage carryover between Eve events, paint/decal index, and upgrade bits).

3. **Why Online Multi-Event Eves Currently Break & How We Fix It:**
   - At [src/session_start.cpp](file:///Users/monty/Documents/GitHub/eod-recomp/src/session_start.cpp#L1000-L1007), `f_00120FB8` (`EveHasMoreEvents`) contains:
     ```cpp
     if (netplay::active()) {
         ctx.ret_s32(0); // <-- Forces Eve termination after Event 0!
         return;
     }
     ```
   - **Solution:** In Stage 1, `f_00120FB8` queries `netplay::session_has_more_events()`. When `current_event_index + 1 < total_events_in_eve`, `eod-server` broadcasts `EODN_PKT_CAREER_EVE_PROGRESSION (0x06)` containing the authoritative post-event `participant_points[16]`, inter-event car damage carryover (with optional 30-second pit-repair window where players can spend Career cash to repair damage before the next heat), and the next event's grid seed, advancing all 16 clients synchronously into Event `k + 1`!

---

## 4. Binary Wire Protocol v2 (`EODN` over QUIC / UDP)

All online communication between `eod-recomp` clients and `eod-server` uses **Wire Protocol v2 (`EODN`)**, replacing the 4-byte `"EOD2"` P2P packets in [src/netplay_wire.h](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_wire.h#L15-L29).

### 4.1 Transport Mapping (QUIC + Unreliable Datagrams / Raw UDP Fallback)

- **Primary Transport:** QUIC v1 (RFC 9000) with Unreliable Datagram Extension (RFC 9221) over a single UDP port (`default: 20910/udp` + HTTPS CAS CDN on `20910/tcp` or `443/tcp`).
  - **QUIC Reliable Bidirectional Streams:** Used for `0x01 Handshake / ModManifest`, `0x03 CarDamageDelta` (panel/wheel detach transitions & full bone sync on join), `0x05 EventModeState` (critical state transitions: goal scored, flag captured, driver eliminated, chicken detonated), and `0x06 CareerEveProgression`.
  - **QUIC Unreliable Datagrams (RFC 9221, zero head-of-line blocking):** Used for `0x02 CarPose60Hz`, `0x04 CollisionImpulse` (with 3-tick redundant piggyback ACK), `0x07 VoiceOpusFrame`, and `0x08 FreeRoamMmoAoi`.
- **Fallback Transport:** Raw UDP with a lightweight 16-byte `EodnHeader` (sequence number + 32-bit ack bitfield + 64-bit session token) for self-hosted LAN/dedicated servers without TLS certificates.

### 4.2 Common Packet Header (`EodnHeader`, 16 Bytes, Little-Endian)

```cpp
#pragma pack(push, 1)

enum EodnPacketType : uint8_t {
    EODN_PKT_HANDSHAKE_MANIFEST     = 0x01, // Lobby join, auth, roster, BLAKE3 .eodmod manifest
    EODN_PKT_CAR_POSE_60HZ          = 0x02, // 60 Hz kinematic + wheel/input/engine-audio snapshot
    EODN_PKT_CAR_DAMAGE_DELTA       = 0x03, // Scalar health, wreck stage, 157-bone dents, 8 panels, 4 wheels
    EODN_PKT_COLLISION_IMPULSE      = 0x04, // Bilateral car-vs-car / car-vs-prop impulse & wreck credit
    EODN_PKT_EVENT_MODE_STATE       = 0x05, // Mode-specific state (Trailer, Chain, Soccer, CTF, Battle/Chicken)
    EODN_PKT_CAREER_EVE_PROGRESSION = 0x06, // 5-event Eve heat advancement, standings, pit repair, career payout
    EODN_PKT_VOICE_OPUS_FRAME       = 0x07, // Spatial 20ms Opus voice frame with 3D world pos & channel mask
    EODN_PKT_FREEROAM_MMO_AOI       = 0x08, // Stage 2 MMO spatial AOI roster, smart-ghost bits, live map stream
};

struct EodnHeader {
    uint8_t  magic[4];       // Always {'E', 'O', 'D', 'N'} (0x4E444F45 LE)
    uint8_t  version;        // Protocol version = 2
    uint8_t  pkt_type;       // EodnPacketType (0x01 .. 0x08)
    uint16_t payload_len;    // Length of payload following this 16-byte header
    uint32_t sim_tick;       // Authoritative 60 Hz simulation tick counter
    uint16_t seq;            // Monotonic per-channel packet sequence number
    uint8_t  sender_slot;    // 0..63 player slot (0xFF = authoritative server)
    uint8_t  flags;          // Bit 0: server_authoritative, Bit 1: compressed_zstd, Bit 2: ack_req
};
static_assert(sizeof(EodnHeader) == 16, "EodnHeader must be 16 bytes");
```

### 4.3 `0x01 Handshake / ModManifest` (`EodnHandshakeManifest`)

```cpp
struct EodnModBlobEntry {
    uint8_t  blake3_hash[32]; // 256-bit BLAKE3 digest of the .eodmod v2 archive
    uint32_t size_bytes;      // Compressed blob size on /v1/blobs/<blake3>
    uint8_t  mod_kind;        // 1=Car/Chassis, 2=Livery/TexPack, 3=Track/Arena, 4=Audio/Playlist
    uint8_t  mount_slot;      // Target virtual chassis/track ID
    char     mod_id[26];      // Null-terminated human-readable slug (e.g. "70_monte_carlo_derby")
};
static_assert(sizeof(EodnModBlobEntry) == 64, "EodnModBlobEntry must be 64 bytes");

struct EodnPlayerSlotInfo {
    uint8_t  slot_index;      // 0..15 (Stage 1) or 0..63 (Stage 2)
    uint8_t  driver_type;     // 0x50 = Human, 0x51 = Server-Authoritative Bot, 0x00 = Empty
    uint8_t  chassis_id;      // 0..32 retail or custom mounted chassis ID
    uint8_t  paint_livery_id; // Livery index
    uint8_t  team_id;         // 0=FFA, 1=Red, 2=Blue (Red Rover / Soccer / Team Battle)
    uint8_t  chain_pair_id;   // 0..7 pair index for Chain Race (0xFF if unchained)
    uint16_t ping_ms;         // Smoothed RTT in milliseconds
    uint32_t upgrade_mask;    // Career upgrade bitfield (Engine/Armor/Tires/Susp tiers)
    uint32_t career_respect;  // Player's verified Career Respect Points
    char     nickname[24];    // UTF-8 sanitized callsign
    uint8_t  livery_blake3[32]; // Zero if stock livery, else BLAKE3 of custom skin blob
};
static_assert(sizeof(EodnPlayerSlotInfo) == 72, "EodnPlayerSlotInfo must be 72 bytes");

struct EodnHandshakeManifest {
    uint8_t  sub_cmd;         // 1=CLIENT_HELLO, 2=SERVER_WELCOME, 3=ROSTER_UPDATE, 4=MOD_READY, 5=START_LOCK
    uint8_t  stage_mode;      // 1=Stage1_16P_Lobby, 2=Stage1_CoOp_Career_Eve, 3=Stage2_64P_MMO_Hub
    uint8_t  assigned_slot;   // Client's authoritative slot (0..63)
    uint8_t  max_slots;       // 16 (Stage 1) or 64 (Stage 2)
    char     room_code[12];   // e.g. "EVE-7K4M" or "WORLD-1"
    uint64_t reconnect_token; // 60-second session resume token (modeled on nfsu2-lan /lan?resume)
    uint32_t rng_seed;        // Deterministic session RNG seed (feeds 0x00447434)
    uint16_t venue_id;        // Track/Arena venue ID
    uint8_t  eve_event_count; // 1 for single event, 3..5 for Career Eve
    uint8_t  eve_event_ids[5];// Event IDs from 0x003C3A70 (e.g. {16, 5, 2, 20, 30})
    uint16_t active_players;  // Number of EodnPlayerSlotInfo records following
    uint16_t mod_blob_count;  // Number of EodnModBlobEntry records following
    // Followed by: EodnPlayerSlotInfo players[active_players];
    // Followed by: EodnModBlobEntry   mods[mod_blob_count];
};
static_assert(sizeof(EodnHandshakeManifest) == 44, "EodnHandshakeManifest fixed header must be 44 bytes");
```

### 4.4 `0x02 CarPose60Hz` (`EodnCarPose60Hz`, 64 Bytes per Car)

Replaces the 62-byte `K_SNAP` entry in [src/netplay_wire.h](file:///Users/monty/Documents/GitHub/eod-recomp/src/netplay_wire.h#L50-L71) with a quaternion-compressed, suspension/audio-enriched 64-byte record that also carries engine RPM, surface contact flags, and a rolling `damage_revision` counter so receivers immediately detect if they missed a `0x03 CarDamageDelta`:

```cpp
struct EodnCarPoseRecord {
    uint8_t  slot_index;        // +0x00: 0..63 car slot
    uint8_t  physics_flags;     // +0x01: Bit 0=airborne, Bit 1=ghosted, Bit 2=handbrake, Bit 3=horn, Bit 4..7=wreck_state(0..15)
    uint16_t contact_flags;     // +0x02: Guest car+0x94 surface contact bitmask
    float    pos[3];            // +0x04: World position X, Y, Z (meters, full IEEE-754 float32 for large Stage 2 maps)
    int16_t  rot_quat[4];       // +0x10: Unit quaternion (x, y, z, w) quantized * 32767.0f (orthonormalized on unpack)
    int16_t  lin_vel[3];        // +0x18: World linear velocity (m/s) * 256.0f (range ±128 m/s = ±460 km/h)
    int16_t  ang_vel[3];        // +0x1E: World angular velocity (rad/s) * 1024.0f (range ±32 rad/s)
    int8_t    throttle_q7;       // +0x24: Throttle (-127..+127)
    int8_t   brake_q7;          // +0x25: Brake (0..127)
    int8_t   steer_q7;          // +0x26: Steer (-127..+127)
    uint8_t  wheel_detached_mask;// +0x27: Bits 0..3 = wheel[0..3] tossed flag (0x80 at car+0x1A00+w*0x54+0x50)
    int16_t  wheel_susp_z[4];   // +0x28: 4 wheel suspension travels (car+0x1A00+w*0x54+0x44) * 4000.0f
    uint16_t wheel_spin[4];     // +0x30: 4 wheel spin angles (rad / 2pi * 65535)
    int16_t  wheel_steer[2];    // +0x38: Front L/R steer angles (car+0x1A00+w*0x54+0x3C) * 8000.0f
    uint16_t damage_q15;        // +0x3C: Scalar damage (car+0x1BEC) * 32767.0f (ALWAYS written to car+0x1BEC!)
    uint8_t  engine_rpm_q8;     // +0x3E: Normalized engine RPM (0..255) for remote Doppler audio synthesis
    uint8_t  damage_revision;   // +0x3F: Incremented on every 0x03 CarDamageDelta (triggers re-sync if mismatched)
};
static_assert(sizeof(EodnCarPoseRecord) == 64, "EodnCarPoseRecord must be 64 bytes");
```

### 4.5 `0x03 CarDamageDelta` (`EodnCarDamageDelta`)

Transmits exact structural deformation (`car + 0x4B8` 157-bone offsets), 8 detachable panels (`car + 0x13E8`), 32-bit part visibility mask (`car + 0x2AC`), and 4 wheel detach states (`car + 0x1A00`):

```cpp
struct EodnBoneDentDelta {
    uint8_t  bone_index;        // 0..156 index into car + 0x4B8 (157 * 12B)
    int8_t   dx_mm4;            // dx in units of 3.0 mm (range ±0.381 m, matching 0x0018CA98 clamp)
    int8_t   dy_mm4;            // dy in units of 3.0 mm
    int8_t   dz_mm4;            // dz in units of 3.0 mm
    uint8_t  crease_shade;      // Low-byte vertex darkening shade (0x00..0xFF) stored in dz float low byte
};
static_assert(sizeof(EodnBoneDentDelta) == 5, "EodnBoneDentDelta must be 5 bytes");

struct EodnCarDamageDelta {
    uint8_t  slot_index;        // Target car slot (0..63)
    uint8_t  attacker_slot;     // Attacker car slot (0..63, or 0xFF for world/wall)
    uint8_t  damage_revision;   // Matches EodnCarPoseRecord::damage_revision
    uint8_t  wreck_state;       // car + 0x1BE8 (0=intact, 1..4=smoke, 10=fire, 11=wheel_shear, 12=totaled)
    float    damage_scalar;     // car + 0x1BEC exact float32 health/damage
    uint32_t part_vis_mask;     // car + 0x2AC 32-bit 4SPS part visibility bitmask
    uint16_t panel_states_2bit; // 8 panels * 2 bits each (0=intact, 1=dented, 2=loose, 3=detached)
    uint8_t  wheel_toss_mask;   // Bits 0..3: wheel 0..3 detached; Bits 4..5: wheel_toss_side (car+0x1C04)
    uint8_t  bone_delta_count;  // Number of modified EodnBoneDentDelta records following (0..157)
    float    last_hit_local_pos[3];   // Local-space impact coordinates (for spawning sparks/debris velocity)
    float    last_hit_local_force[3]; // Local-space impact force vector (for 0x001E0DC8 / 0x001B3068 launch)
    // Followed by: EodnBoneDentDelta bones[bone_delta_count];
};
static_assert(sizeof(EodnCarDamageDelta) == 40, "EodnCarDamageDelta header must be 40 bytes");
```

### 4.6 `0x04 CollisionImpulse` (`EodnCollisionImpulse`, 48 Bytes)

Solves Problem **P3** (brick-wall puppet collisions) by transmitting bilateral rigid-body collision impulses and authoritative damage attribution:

```cpp
struct EodnCollisionImpulse {
    uint32_t collision_id;      // Deterministic hash of (tick >> 1, min(a,b), max(a,b)) for deduplication
    uint8_t  slot_a;            // First colliding vehicle slot (0..63)
    uint8_t  slot_b;            // Second colliding vehicle slot (0..63, or 0xFE=trailer, 0xFD=soccer_ball)
    uint8_t  hit_zone_a;        // 0=Front, 1=Rear, 2=Left(T-Bone), 3=Right(T-Bone), 4=Roof/Undercarriage
    uint8_t  hit_zone_b;        // Hit zone on body B
    float    world_contact_pt[3];   // World-space contact point (X, Y, Z in meters)
    float    world_contact_norm[3]; // Unit contact normal pointing from B -> A
    float    impulse_vec_a[3];  // Authoritative delta linear velocity (m/s) applied to Car A's RigidBody
    float    impulse_vec_b[3];  // Authoritative delta linear velocity (m/s) applied to Car B's RigidBody
    float    ang_impulse_a[3];  // Authoritative delta angular velocity (rad/s) applied to Car A
    float    ang_impulse_b[3];  // Authoritative delta angular velocity (rad/s) applied to Car B
    float    damage_dealt_to_a; // Authoritative damage credited to B against A (feeds 0x00202DA8)
    float    damage_dealt_to_b; // Authoritative damage credited to A against B (feeds 0x00202DA8)
};
static_assert(sizeof(EodnCollisionImpulse) == 84, "EodnCollisionImpulse must be 84 bytes");
```

### 4.7 `0x05 EventModeState` (`EodnEventModeState`)

Covers every one of EOD's 26 contest modes via a tagged union payload:

```cpp
enum EodnEventSubType : uint8_t {
    EODN_SUB_RACE_PROGRESS   = 0x01, // Laps, checkpoints, direction flip (Fwd/Bwd, Whip-Around), Last-Man elim
    EODN_SUB_TRAILER_STATE   = 0x02, // 16 trailer rigid bodies + hitch attachment + egg/cargo integrity
    EODN_SUB_CHAIN_TENSION   = 0x03, // 8 chain pair constraints (tension, max_len, broken state)
    EODN_SUB_SOCCER_BALL     = 0x04, // Giant soccer ball rigid body + team goals + last touch slot
    EODN_SUB_CAPTURE_FLAG    = 0x05, // Flag carrier slot, loose flag pos, goal beacon pos, hold timers
    EODN_SUB_GAUNTLET_HEARSE = 0x06, // Hearse slot, Hearse lap/health, runner hit bounties
    EODN_SUB_DERBY_SURVIVAL  = 0x07, // Alive/ring-out mask (Derby, Push-Off, Knock-Out, King of Ring, Survival)
    EODN_SUB_BATTLE_CHICKEN  = 0x08, // Chicken rocket fire (0x17E000), detonate (0x17E718), kills/respawns
    EODN_SUB_STUNT_TELEMETRY = 0x09, // Stunt Man / Accuracy Jump / Jump for Dough score award
};

struct EodnTrailerEntry {
    uint8_t  owner_slot;        // 0..15 tow car slot
    uint8_t  hitch_attached;    // 1 = hitched to tow car, 0 = detached free rigid body
    uint16_t integrity_q15;     // Trailer / Egg health (0..32767)
    float    pos[3];            // Trailer world position
    int16_t  rot_quat[4];       // Trailer rotation quaternion * 32767
    int16_t  lin_vel[3];        // Trailer linear velocity * 256
    int16_t  ang_vel[3];        // Trailer angular velocity * 1024
};
static_assert(sizeof(EodnTrailerEntry) == 36, "EodnTrailerEntry must be 36 bytes");

struct EodnChainPairState {
    uint8_t  lead_slot;         // 2*k
    uint8_t  tail_slot;         // 2*k + 1
    uint8_t  chain_intact;      // 1 = tethered, 0 = snapped
    uint8_t  reserved;
    float    current_length_m;  // Current separation distance (m)
    float    tension_force_n;   // Spring-damper tension magnitude (N) applied along tether axis
};
static_assert(sizeof(EodnChainPairState) == 12, "EodnChainPairState must be 12 bytes");

struct EodnSoccerState {
    float    ball_pos[3];       // World position of Soccer ball rigid body
    int16_t  ball_quat[4];      // Orientation * 32767
    float    ball_lin_vel[3];   // Linear velocity (m/s)
    float    ball_ang_vel[3];   // Angular velocity (rad/s)
    uint8_t  last_touch_slot;   // Last car slot that struck the ball
    uint8_t  goal_state;        // 0=in_play, 1=team_a_goal_celebration, 2=team_b_goal_celebration
    uint16_t team_score[2];     // Team A / Team B goals
};
static_assert(sizeof(EodnSoccerState) == 48, "EodnSoccerState must be 48 bytes");

struct EodnCaptureFlagState {
    int8_t   carrier_slot;      // -1 if flag is on ground, 0..15 if carried
    uint8_t  delivery_index;    // Active downtown goal drop-off index
    uint16_t carrier_shield_ms; // Remaining post-steal immunity timer (ms)
    float    flag_world_pos[3]; // Loose flag position when carrier_slot == -1
    float    goal_world_pos[3]; // Active delivery beacon world position
    uint16_t driver_scores[16]; // Points / deliveries per slot
};
static_assert(sizeof(EodnCaptureFlagState) == 60, "EodnCaptureFlagState must be 60 bytes");

struct EodnBattleChickenEvent {
    uint8_t  event_kind;        // 1=WEAPON_FIRE (0x17E000), 2=WEAPON_DETONATE (0x17E718), 3=RESPAWN, 4=SCORE_SYNC
    uint8_t  shooter_slot;      // 0..15
    uint8_t  weapon_pod_index;  // 0..63 index in relocated g_WeaponRegistry (0x01C30000)
    uint8_t  victim_slot;       // Primary target slot (or 0xFF)
    float    origin_pos[3];     // Launch position or detonation epicenter (passed to 0x0017DA58 shockwave)
    float    launch_dir[3];     // Initial velocity vector for projectile
    int16_t  kills[16];         // Authoritative BattleMaster kills per slot
    int16_t  deaths[16];        // Authoritative BattleMaster deaths per slot
    uint16_t alive_mask;        // Bitmask of active (non-respawning) cars
};
static_assert(sizeof(EodnBattleChickenEvent) == 94, "EodnBattleChickenEvent must be 94 bytes");
```

### 4.8 `0x06 CareerEveProgression` (`EodnCareerEveProgression`, 96 Bytes)

Synchronizes 5-event Career Eves (`RaceDayDo` `0x0011F640`) across all human participants:

```cpp
struct EodnCareerEveProgression {
    uint8_t  phase;             // 1=HEAT_FINISHED, 2=PIT_REPAIR_WINDOW, 3=ADVANCE_NEXT_HEAT, 4=EVE_FINAL_PAYOUT
    uint8_t  current_heat_idx;  // 0..4 (EveTracker 0x003C4BFC)
    uint8_t  total_heats;       // 3..5 (EveTracker 0x003C4C00)
    uint8_t  next_event_id;     // Event ID from 0x003C3A70 for heat + 1
    uint32_t next_rng_seed;     // Deterministic RNG seed for heat + 1
    int16_t  heat_points[16];   // Points earned in the just-completed heat
    int16_t  total_points[16];  // Cumulative Eve points (written to 0x003C4C18)
    uint8_t  next_grid_order[16];// Starting grid slot assignment for heat + 1 (reverse or points-ordered)
    uint16_t carryover_dmg_q15[16]; // Carryover car+0x1BEC damage into heat + 1 after pit repair deductions
    int32_t  career_cash_delta[16]; // Prize purse minus entry/ambulance/repair fees (credited to 0x00448008)
    int16_t  respect_delta[16]; // Career Respect Points earned (credited to 0x0044800C)
};
static_assert(sizeof(EodnCareerEveProgression) == 216, "EodnCareerEveProgression must be 216 bytes");
```

### 4.9 `0x07 VoiceOpusFrame` (`EodnVoiceOpusFrame`)

```cpp
struct EodnVoiceOpusFrame {
    uint8_t  speaker_slot;      // 0..63
    uint8_t  voice_channel;     // 0=Proximity_3D_Spatial, 1=Lobby_Global, 2=Team_Or_Chain_Partner, 3=Crew_Radio
    uint16_t frame_seq;         // Monotonic 20ms Opus frame counter
    float    world_pos[3];      // Speaker's car world coordinates for 3D panning & distance attenuation
    float    world_vel[3];      // Speaker's car velocity for subtle Doppler / CB-radio squelch effect
    uint16_t opus_len;          // Length of Opus payload (typically 24..60 bytes at 24 kbps mono 48 kHz)
    // Followed by: uint8_t opus_bytes[opus_len];
};
static_assert(sizeof(EodnVoiceOpusFrame) == 30, "EodnVoiceOpusFrame header must be 30 bytes");
```

#pragma pack(pop)
