# Remaster work queue

New user messages append to this queue. They do not cancel earlier items unless the owner explicitly says so. “Implemented” requires fresh build evidence; “verified” requires the stated scenario. Release and install are separate checkpoints.

## Assigned work

- iso_setup — GPT-6.1 Sol, medium. Owns cross-platform ISO import, first launch and importer tests.
- driver_grid — GPT-6.1 Sol, medium. Owns human control, unique grid seats, initial pose handshake and stale physics-body state.
- voice_music — GPT-6.1 Sol, medium. Owns voice codec/playback, silence recovery, 16-peer support and independent soundtrack mixing.
- Root — nickname identity, shared online rules, UI/catalog/version integration, full build and cross-platform acceptance, packaging and release.

Agents have explicit file/region ownership. Root owns final integration and full build runs. Player-count claims require evidence for the relevant layer: admission, grid occupancy and voice are separate bounds.

## Current batch — 1.0 release

- [ ] Clear current Mac and Windows compiler errors, then run the suite. Last public preview is commit 735e9b3.
- [ ] Windows first launch: native ISO chooser, shared transactional importer, cancellation/error/retry, automatic reuse on second launch. Test correct ISO, wrong disc, protected destination, Unicode paths, interruption and insufficient space. Do not ask players to extract raw disc files.
- [ ] Mac first launch: choose the ISO directly; preserve the in-app recovery route. Test clean app-support state.
- [ ] Nicknames: random session default, editable saved nickname, host resolves duplicates with (1), local roster says YOU. Test two peers asking for the same name, rename, reconnect and host change.
- [ ] Driver ownership: host and guest each control their selected car. Trace native driver, controller pointer, human flag, roster owner and world-slot mapping before GO. Test identical and different cars, both hosting roles and rematch.
- [ ] Starting grid: host establishes distinct valid grid poses before either client simulates. Test standing and rolling starts. Prevent guests overwriting locally owned human poses after GO.
- [ ] Car portraits: load native UICAR tm0 portraits, respect the integrated overlay and preserve alpha. Test stock, special, NASCAR and paint changes; never show the body UV texture.
- [ ] Fair online rules: all optional gameplay advantages off by default, independent of saved offline settings. Host opts in through shared lobby controls. Every client applies the same locked race rules; local combos and env/config cannot override. Verify turbo, chicken fight, invincibility, durability, recovery and multipliers.
- [ ] Mac online catalog stuck loading: trace title → native main menu → catalog → lobby defaults. Check hosting after catalog was cached and hosting before title was passed.
- [ ] Two-way voice: trace capture → encode → UDP → remote decode → actual mixer. A local speaking icon is insufficient evidence. Test silence/resume, mute/deafen, microphone permission/device loss and both host roles. Keep microphone toggle and default-off preference.
- [ ] Independent music volume/mute: stock and custom soundtrack only; engine, effects, spotter and voice remain audible. Each machine's playlist stays local.
- [ ] Version 1.0 everywhere: single VERSION source, app title, plist, Windows metadata, GitHub tag, release notes and README.
- [ ] Fresh glamour captures and feature/benefit README after batch verification.
- [ ] Package personal Mac content, public retail Mac ZIP and Windows ZIP, verify signatures/archive contents, push catsandsoup/eod-recomp and publish matching source revision.

## Carry-forward investigations

- [ ] Reproduce the 5,100-frame remote-car failure: invalid terrain coordinates, stalled snapshot age and live steering. Two short rematches do not establish a stable long race.
- [ ] Reported 19–20 FPS online alone / Career loading state: preserve exact settings and path; profile native game state and renderer, voice enabled vs off, host alone vs offline. No causal claim yet.
- [ ] Owner's Windows hardware result: 2017 27-inch 5K iMac, 16 GB DDR4, Radeon Pro 580 8 GB, highest graphics, reported steady 60 FPS. Record as owner evidence; confirm OS, renderer, actual internal resolution and frame-time measurement before broader parity claims.
- [ ] Complete host/guest journey matrix: natural finish, host ends race, guest leaves, closes lobby, abrupt host loss, cancellation at every load phase, lost packets, stale messages and repeated rematches.
- [ ] Targeted kick: named guest, confirmation, reason, seat cleanup and remaining-player continuity.
- [ ] Capacity design: protocol admits 16 including host; race seat limits vary; Battle total cars ≤10; voice now handles IDs 0–15. Room-size customization is implemented and protocol-tested; high-occupancy physical races remain open.
- [ ] Mod parity: compare real gameplay content, show incompatibility before Ready, reject unsafe fallback. No automatic mod transfer implemented.
- [ ] Couch UI / NN/g: code-entry-only keyboard exception, controller confirmation, discoverable actions, stable focus as roster changes, readable pinned identity/status, long car-list browsing, safe destructive actions, useful bounded recovery.
- [ ] Physical Mac ↔ Windows pads, microphones and WAN joining. Keep CrossOver loopback evidence distinct from native Windows hardware.
- [ ] Restore remaining earlier project priorities after online stabilization: performance/accuracy baseline, NASCAR roster/track fidelity, barrier edits and all-map gameplay acceptance. Historical progress claims need current source and play evidence.

## Already exercised in the prior preview

- [x] Mac host + Mac guest: Trailer2, Anoka, two starts and two returns.
- [x] Windows under CrossOver ↔ Mac: both hosting directions, two-race rematches.
- [x] Abrupt host process loss: guest reached offline native main menu in roughly 5.6 s.
- [x] Lobby admission: 15 simulated guests accepted, next refused as full.
- [x] Scripted controller host: open Online, host, start and return with guest.
- [x] Menu-model keyboard exception: type five code characters; controller confirms/readies.
- [x] Pinned code/roster, phase-specific controls and stable focus when roster grows.
- [x] Public Mac/Windows preview, illustrated README and host/guest plan at 735e9b3. Remaining defects above stay open.

Evidence and player acceptance details live in docs/ONLINE_PLAYER_JOURNEYS.md and the current chat workspace's work/ logs. Do not commit disc-derived assets, generated game code or binaries.

## Audio evidence boundary

The deterministic tone experiment proved generated PCM → Opus → UDP → remote decode → software mixer samples. It did not use a physical microphone or verify sound from speakers/headphones. Those are separate open acceptance checks on both Mac and Windows. A speaking icon is not evidence of either.

## 1.0 implementation and evidence ledger

The original queue above is retained; broad checkboxes remain open where their full
acceptance matrix has not run. The following narrower outcomes are verified:

- [x] Mac native build and all 83 CTests, including AssetTools (27 cases, three optional cases skipped).
- [x] Windows native cross-build; final Wine CTest result is recorded in release verification.
- [x] Whole-game Windows ISO import and second launch without the ISO: Unicode game,
  ISO and LocalAppData paths, 128 files / 1,939,523,956 bytes with matching SHA-256.
- [x] Mac importer transaction/Unicode/error/cancellation fixtures; native ISO route retained.
- [x] Host/guest independent human controls, distinct grid seats, rolling same-car
  long race beyond 5,100 frames and standing different-car Battle.
- [x] Native UICAR portrait loader replaces CT body texture lookup.
- [x] Random names, saved screen-name editor, duplicate suffixes and YOU identity;
  actual UDP rename/reconnect checks.
- [x] Default-off shared host rules, guest authority rejection and native cheat
  normalization; all 32 masks plus saved preference/native-bit truth tables.
- [x] Effective game-content handshake, differing archive rejection before admission,
  and no silent vehicle substitution. Music remains local; no automatic mod transfer.
- [x] Room limit 1–16, occupied-seat clamp, full-room refusal, occupant-bound kick
  confirmation, targeted removal and reused-slot protection.
- [x] Event-seat overflow refuses GO and returns to lobby; code/error/full roster
  visible together in a real 16-occupant screenshot.
- [x] Mac physical microphone reached remote actual-game mixer in both roles;
  separate real acoustic hardware roundtrip passed. This does not establish actual
  game's remote physical speaker output or native Windows hardware.
- [x] Stock/custom music gain isolation in the real SPU2 mixer, effects/speech
  preserved; 16-ID voice codec/mixing and silence recovery checks.

Release packaging, installation, exact commit/tag and cross-platform rematch evidence
are recorded with the final release verification. Existing broader performance,
WAN, natural-finish/event-matrix, physical Windows peripherals and earlier rendering/
NASCAR/barrier work remain investigations, not implied completion claims.

- [x] Final Mac↔Windows-through-CrossOver pairing in both hosting roles: two actual
  race starts and two native lobby returns per peer, normal exits.
- [x] Fresh Windows executable suite: 40/40.
- [x] Generation reproducibility: 119 generated files match, 13,481 functions,
  zero generation errors.

Full 1.0 evidence and remaining boundaries: docs/RELEASE_1_0_ACCEPTANCE.md.
