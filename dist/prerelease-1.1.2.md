Eve of Destruction Remaster 1.1.2 is a prerelease. You need your own North American game disc (SLUS_209.10). No retail game data, NASCAR disc data or personal mods are included.

## Changes in 1.1.2

### Unified PLAY Hub (new front end)
- Tabs are now `PLAY | MODS | GRAPHICS | INPUTS | SETTINGS`. Career, Solo Quick Play, 2–4P Split-Screen and Online share one three-column hub: setup, preview and a 16-slot roster.
- Visual **Mode Browser** and **Map Browser** (retail tracks and installed `.eodmap` maps), a four-section **Customize Rules** drawer, and a translucent **Quick Pause** rail.
- Custom game modes save to and load from `mods/gametypes/*.json`.
- 4-seat local split-screen with per-seat car and paint. Controller ports raised from 2 to 4.
- Fixed: `Enter` on **Launch Solo Event** only closed the menu. Keyboard hints now read `ARROWS`.
- New setting `[System] UnifiedFrontEnd=1`. **Classic PS2 Menus** switches back.

### Car selector and previews
- New **Visual Car Selector**, styled like the map selector. Classes: `ALL | COMPACT | MIDSIZE | MUSCLE | SPECIAL | TRAILERS | MODDED`.
- Trailers, campers and caravans can be chosen as playable vehicles. Installed modded cars appear under `MODDED`.
- Updated map and car preview art. Woodly now maps to its internal `afton` picture, and modded car portraits load.
- The centre preview switches to the selected car when you hover car or paint rows.

### Host and guest (online)
- Guests see the host's rules as a read-only **HOST RULES** screen. Guest edits are rejected.
- The host's mode and rule changes are sent to guests as soon as they happen, and to each guest when they join.
- Guests cannot press Ready while the host's custom map downloads. A progress percentage is shown, and the host's roster shows `SYNCING MAP`.
- The online roster now shows the lobby's real AI count and capacity.
- The MODS and SETTINGS tabs show online status. Host-controlled options are labelled, and Career cheats are marked offline only.
- `mods/maps` and `mods/gametypes` no longer appear as broken mods.
- Custom map metadata (base arena) is now read from `.eodmap` files.

### Under the hood
- Faster, cached map and game-mode catalog. Clean transitions between Career, Solo, Split-Screen and Online.

### Known issues
- After a race finishes or you quit, the game can land on the original event selector, and **Launch** is ignored until you pick `Exit` there.
- **Tips (Tag):** the `IN!` marker does not always transfer on contact, AI may not chase, and the `ALIVE` counter is misleading. A fix is planned.
- NASCAR cars and the modded Phantom are not bundled. Importing NASCAR requires your own disc.
