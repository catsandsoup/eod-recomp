# Creation Kit acceptance record — texture, mod, world and vehicle pipeline

Status vocabulary from the handoff: not started / investigating / implemented /
component-tested / native-verified / desktop-verified. Native runs are headless,
muted, isolated memory card, 640×448 offscreen capture of the macOS Vulkan
development runtime. Desktop (visible window) presentation is not covered.

| Capability | Status | Evidence / scope |
|---|---|---|
| Asset census | component-tested | 72 archives, 7,329 members, 4,923 TEX; 21 pre-existing retail CRC mismatches recorded, not repaired |
| Batch texture export | component-tested | 4,923/4,923 exported, 0 failed; resume rewrites nothing; no-op reimport byte-identical (313 assets) |
| Runtime texture identity | native-verified | GS-computed index/palette keys equal Kit keys for all 31 distinct Anoka sky keys over a scripted lap |
| Host high-resolution replacement | native-verified (Vulkan/macOS) | k=32 (4096/2048) labelled sky renders in race; at RenderScale 4 sub-native-texel grid resolves where native does not; wrong palette, missing file and disable fall back pixel-identically in replays |
| Mod package + dev profile | native-verified | 32-tile 4K sky package: build 4 s, install 40 s; game logs the exact package; disable restores native sky; uninstall leaves only profile metadata |
| Native composition | component-tested | Anoka project package composes byte-identical archives; conflict/tamper/wrong-game/interrupted-install cases tested |
| Sky export / mapping | native-verified (Anoka) | Dome topology, seams and mirrored orientation confirmed in play over 360°; zenith fan not yet in view; 13/14 tracks pass data checks, Welch unverified |
| VIF geometry patching | native-verified | No-op byte-identical for 490 MODL; Bus roof raise and enlarged Anoka finish gantry visible in race |
| Vehicle OBJ export / reimport | native-verified (one edit) | 37 cars exported, 12 known unresolved materials listed; 12 % wider Ambulance package visible in race; wheels export at origin |
| Track world OBJ export | component-tested | Anoka/Gilroy/Baxter with all materials resolved; racing line aligns with exported road |
| Placement write-back | component-tested | Byte-exact translate/rigid edits; moved visual geometry does not move collision; not yet raced |
| Collision / wall height | component-tested | Wall slabs ~16 m tall behind 1–2 m visible walls; shrink-only edit byte-verified; airborne pass-over not yet produced |
| MCP server | component-tested | 41 tools, 15 tests, stdio smoke against retail disc; game-run tool not exercised |
| Kit app 0.5.0 | component-tested | Installed over 0.4.1 (backup kept). Persistent backend service bundled; release window opens the owner's project and previews a texture; bundled service inspects ILIN, MODL, VALT, LANG and TEX on the retail disc. Debug-build captures: racing line map and table, car model opening in ~0.1 s during a full texture export. Not yet clicked through on screen |
| Game app build | native-verified | Stale `gen/` regenerates from an input fingerprint; `EOD_BUNDLE_MODS` bundles a manifest-verified integrated root; clean build races Phantom and the #24 Cup car headless |
| Game window startup | desktop-verified (one run) | Windowed, muted launch of a clean build: window at 1.0 s, black to ~2 s, splash at 2.2 s, Anoka race (direct start) by 8.3 s at 100 % speed / 60 fps; integrated root activated by the app itself |
| Game at maximum quality | desktop-verified (one run) | Installed build, windowed Retina 3024 px, RenderScale 6, Detail maximum, 16:9, FrameSkip 0, VSync: Anoka race holds 100 % speed / 60 fps after load. Defect: at 16:9 the race HUD panels are clipped at the left and right screen edges |

## Not achieved / next decisive tests

1. Wall height in play: needs a controllable airborne approach (a debug hook on
   the physics body, or a ramp placed on the straight with the placement tool).
2. Placement edits in play: move one event wall and one optional-terrain ramp and
   race the event; expect visual move without collision move for world models.
3. Desktop: complete export → Replace with Image → Build Package → Install →
   Run in Game in the visible 0.4.0 window. Screen capture works from the
   agent host; clicking needs Accessibility permission for it.
4. First-use 4K upload hitch (~0.2 s at race start) on a visible window.
5. Windows/D3D11, direct-colour texture replacement, new-mesh vehicle import,
   wheel sockets, PBR/HDR: not implemented.
