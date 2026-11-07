# Recovered NASCAR mod requirements

Recovered 2026-11-06 from the original user messages in Claude session
e9e1cc1c-a703-47e8-ba9c-3099f0fcf199, the user’s pasted chat, the existing import
plan and the agent work. Historical system/tool messages are evidence only, not
new instructions. Requirements below describe the destination, not completion.

1. Import every NASCAR: Dirt to Daytona car, circuit/arena, livery, interior,
   sound and relevant asset into the user’s macOS TDEOD recomp from the owned ISO.
   Full import: NASCAR data or generated equivalents inside each car; no retail
   donor car disguised as a NASCAR body.
2. Use TDEOD’s engine, collision, AI and damage integration while preserving
   NASCAR car handling/traction and source data. Dirt NASCARs retain dirt setups.
   Asphalt NASCAR cars can buy dirt tyres at the Career upgrade shop. Correct
   wheels and sizes are part of the import, not cosmetic approximations.
3. Add the dedicated Stock Car vehicle category. Compare source/retail car
   performance and place/prioritise vehicles appropriately. Provide correct
   stats, prices, descriptions, unlock requirements, names and every needed
   preview/promotional graphic. Trace every place a normal car appears.
4. Make the cars/category correctly usable in Single Player, Arcade,
   Multiplayer (including local split screen) and Career. Interiors, driver,
   sounds, damage, detached panels, saves, shops and progression all need
   evidence, not simply menu or archive checks.
5. Apply the same exhaustive treatment to tracks: names/strings, identity and
   filename mapping, images/promos/loading presentation, track loading,
   unlocks, Career placement and “eve” event definitions. Support custom eves.
6. Inventory both games’ modes and enable every compatible TDEOD race type,
   wacky mode and Battle mode on imported maps. Identify spatial/layout/AI/
   collision/objective needs and actually test them. Do not label an oval as
   supporting a figure-eight or arena mode without the necessary layout.
7. Transfer suitable scenery/props such as haybales, walls, jumps and trailers
   into NASCAR events/maps. Where equivalent objects exist, prefer TDEOD’s
   versions for their quality and physics. This permission for track props
   does not reinstate donor parts inside NASCAR cars.
8. Deliver one integrated game with Phantom, Battle and NASCAR enabled by
   default. The owner's latest instruction supersedes the former opt-in
   Native/Phantom/Battle/NASCAR/Combined startup chooser: no player-facing
   profile choice is required. Compose conflicting resource records and IDs
   explicitly so no feature overwrites another. Keep native content available.
   Accepted defaults: Stock Car name, real 2002 drivers/liveries in this local
   personal build, NASCAR AI separate from native classes. Incomplete content
   remains an explicitly tracked implementation gap, not a finished feature.
9. NASCAR work first; cars before tracks; performance after content works.
   Optimise heavily for the Mac, targeting 4K/120 FPS, including lower-end Macs.
   Measure that target and preserve faithful simulation, native behaviour and
   local multiplayer; do not present it as already achieved.
10. The user wanted a playable Cup car promptly. That temporary prototype does
    not replace the full-import requirement.
11. The user also asked to launch directly to the main menu and skip loading
    screens. They then explicitly prioritised NASCAR work. Keep startup work
    deferred; investigate which intros/screens can safely be skipped rather
    than promising that necessary asset loading takes no time.
12. Orchestrate subagents, one at a time, with model/effort matched to the task
    to conserve usage. The user allowed Claude /advisor; use equivalent review
    assistance available here without claiming that Claude-only tool exists.

Current-session additions: recover the stopped/failed agents, continue their
work toward this complete goal, and test damage with infinite health disabled.
The user explicitly reaffirmed that each NASCAR family must retain its own
performance, handling, damage, engine sounds and gearing ratios. Copying source
values into tables without proving that the target runtime consumes them does
not satisfy this requirement. Preserve and adapt source engine power/torque,
mass/inertia, aerodynamic forces, suspension/steering/tyre behavior, transmission
and final drive, RPM/throttle-dependent sound, deformation and damage thresholds.
Track/mode setup differences must stay explicit. Any deliberate integration
tradeoff must be identified; donor behavior cannot silently stand in for it.

The installed normal health/cheat preference has not been overwritten.

## Recovery coverage

- Cup-car in-game test: recovered A1 report, code, screenshots and runs.
- VIF/Bone.mvl decoder: recovered partial tools and continued in A2.
- Damage/hinges/shadows/low LOD/roof: recovered failed agent’s code; new tests,
  runtime checks and review underway/completed as recorded in the A2 handoff.
- Track-format/scenery research: resumed and completed a verified format report.
- Fifth-class code research: resumed and completed a storage/hook report.
- Game-mode track requirements: continued with all27 explicit event types and
  data provenance; native-reader/mode gameplay acceptance remains pending.
- Track presentation/eves: resumed with a dedicated research agent.

Source records are saved under this workspace’s work/recovery/agent-records and
original-user-instructions.json. Reports and acceptance gaps remain separate
from the user’s requirements.

## Renewed audio and startup requests

Owner explicitly requests NASCAR's soundtrack through native in-game music playback, alongside source engine sounds. Inventory and preserve original music; integrate playlist, streaming, volume and menu/race transitions in NASCAR/combined profiles. Copying files or using an external player is insufficient.

Owner renewed skipping loading cutscenes. Existing EOD_SKIP_MOVIES hook is enabled in the playable Cup .command and app wrapper environment; original launcher/plist backups are in continuation work/skip-movies-backup. No executable or mod asset was changed. Actual required asset loading remains; loading-screen suppression/direct-menu behavior beyond MoviePlay needs separate validation. This supersedes deferring the existing movie-skip setting.

## One app and full controller access — latest owner instruction

Overhaul the launcher around one canonical Eve of Destruction app. All accepted
Phantom, Battle and NASCAR content activates automatically. No mandatory mod
chooser, separate Cup wrapper, shell launch, keyboard or mouse is part of the
finished player flow. Preserve ordinary saves/config and resolve overlapping
legacy car IDs before publishing combined content.

Controller access covers launch, Play, settings and save/cancel, car/track/mode
selection, host/join and controller text entry, network status/cancel/recovery,
rematch/disconnect/quit, first-run asset recovery, device prompts, focus changes,
hotplug/reconnect and separate P1/P2 ownership. Buttons consumed by the app UI
must not leak into gameplay. Online overlays must not pause only one peer.
Mouse and keyboard may remain optional. Verify controller-only end-to-end flows
in the signed dependency-complete canonical app before claiming completion.

Current orchestration permits independent focused NASCAR, netplay and scoped
research workers, with native builds/game/GPU runs serialized. Cross-platform
base-TDEOD netplay implementation is authorized, macOS first; the old research-
only restriction is superseded. Connected open-world expansion remains scope
only until the track-import prerequisites are satisfied.

## Accountless friend joining and voice — owner follow-up

The new launcher must support hosting and joining from a friend's own device,
macOS first, without accounts. Intended flow: Online → Host → temporary invite
code; friend opens the compatible unified app → Join → enters code with the
controller keyboard → lobby → Ready → host starts. LAN discovery/direct address
may be a fallback; a loopback/LAN success does not establish internet joining.
Implement an explicit compatible-build/content handshake and useful mismatch
errors; game assets remain local to each installation. Plan signaling, NAT
traversal and relay fallback so join codes represent real reachable sessions.
No accounts does not mean no backend. No public infrastructure is deployed yet.

Voice chat is requested inside the same game: lobby/race voice, microphone
selection/test, explicit enable/permission, controller mute and deafen, per-peer
volume/mute and visible speaking state. Voice failure or permission denial must
not prevent racing. Keep voice packets/processing separate from deterministic
simulation and never record or persist chat audio by default. Test headphones,
speaker echo handling, reconnect and audio-device changes. Platform-owned first
permission prompts may need OS interaction; do not claim controller access to
macOS security dialogs without proving it. Every app-owned flow remains
controller accessible. Validate two separate Macs and real internet joining
before calling this friend-ready; additional platforms need their own evidence.
