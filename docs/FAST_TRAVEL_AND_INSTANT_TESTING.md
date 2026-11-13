# Fast travel and instant testing: remaster plan

The Career warp (docs/CAREER_WARP.md) showed that the game's own teleport, its Career driver and its screens can
be driven directly. The same pieces give players a remaster-grade way around the Career world, and give
development a way to reach any scenario in seconds. This plan defines both from the player's side first.

## Player outcomes

| Job | Today (original game) | Remaster |
|---|---|---|
| Go to a place in Career (dealer, Malt Shop, Pete's...) | Drive there, or pause -> Jump To -> a short list of codenames; you arrive at the door and still drive in | **Travel** screen: every place by its real name, what you do there ("Buy and sell cars"), and why a place is locked ("Opens at 50th"). Choose it and you are *inside*: the dealer's Buy screen, the Paintshop, the Eve's event list. |
| Get back into my Career | Title -> main menu -> Career -> memory card "Loading... do not turn the power off" -> hub -> Drive Around | **Continue** on the main menu: one press, straight to where you saved, no memory card wait, no hub detour. |
| Not repeat beginner content | Intro fly-by and first-visit tutorial notes | Returning players skip them; a new Career still gets them (setting: Show tutorials). |
| Not lose progress | Save from the hub only | Autosave after a purchase or an Eve (on by default), Save from the pause menu. |
| Try something again | Replay the route | Quick Save / Quick Load anywhere (opt-in; off online). |

Rules: plain player wording (AGENTS.md product language); controller, keyboard and mouse all work; nothing
blocks the game thread; online play unaffected; the original Jump To still works for players who want it.

## Developer outcomes

| Job | Now | Plan |
|---|---|---|
| Start in Career at a place | `EOD_DIRECT_START="mode=career;place=..."` + a copy of a real save (done) | Arrive inside the place, every place (incl. Highway, TJ's, Troy's), each Eve and event |
| Run faster than real time | `EOD_TIME_SCALE=N` (done, ~6.5x at 8) | Unchanged |
| Reach a hard scenario (moving challenger, dealer page 5, 1 h of play then quit) | Script it every time | **Snapshots**: save the whole game state once, load it in under a second, any number of times |
| Prove a fix | Hand-built pad scripts and screenshots | `tools/scenario.py <name>`: snapshot or warp + script + contact sheet + log checks, one command |

## Phases

1. **Arrive inside (warp hand-off).** Reproduce the pause menu Jump To hand-off (`0x386C90`: teleport, then
   `0x386D98`, `0x369840`, `0x384478`) so the Career drive loop opens the place after a teleport. Map the
   remaining places (Highway `hiway`, TJ's, Troy's Place, Bug's Place) to their areas and screens.
   Done when each place opens its screen from a cold boot with a copy of a real save, at 8x, with screenshots.
2. **Travel screen for players.** In free roam (pause menu entry and its own button), built on phase 1.
   Lists every place with name, purpose, lock reason; arrive inside; works with pad, keyboard and mouse.
   Done when every place is reached from every other in the real game, and locked places explain themselves.
3. **Continue and no waiting.** Main-menu Continue; skip the memory card loading screen where the write is
   already done (NVMe); returning players skip the intro fly-by and tutorial notes.
4. **Snapshots.** Full guest state (EE RAM, only the pages in use; scratchpad; VU; GS registers and VRAM;
   IOP and sound state; host-side HLE state), versioned and compressed. Developer first (`EOD_SNAPSHOT_SAVE`,
   `EOD_SNAPSHOT_LOAD`), then player Quick Save / Quick Load once restores are proven identical
   (state hash after load matches state hash at save, and the next 600 frames match).
5. **Scenario library.** Named scenarios built from saves, warps and snapshots, used by every Career fix
   (dealer lot, challengers, quit-to-menu, sound in the garage).

## Risks

- A teleport outside the pause path can leave the Career loop waiting (seen: car stops, no screen). Phase 1.
- Snapshots must capture every piece of state or a restore drifts; restores are checked by hashing, not by eye.
- Saves: Continue and autosave write the player's real memory card; they reuse the game's own save code only.
