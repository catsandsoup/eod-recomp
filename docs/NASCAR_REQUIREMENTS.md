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
8. Keep the import an opt-in mod (the final delivery decision, superseding the
   earlier “not a mod” wording). Startup chooser target: Native, Phantom,
   Battle, NASCAR and a combined profile. Native remains retail with mods off.
   Accepted defaults: Stock Car name, real 2002 drivers/liveries in this local
   personal build, NASCAR AI separate from native classes.
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
