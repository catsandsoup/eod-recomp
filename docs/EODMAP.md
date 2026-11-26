# Portable creator maps

Use the numbered build palette in the World Editor to place a deck, then aim at its surface and place a Start Gate. The most recently designated start occupies grid index zero. Test (F5) writes a private temporary map. Export .eodmap writes a shareable file. The source editor project retains editable model instances and road plans.

## Version 1

The container is an uncompressed USTAR archive with exactly four regular files at its root. It can be inspected with `tar -tf MyTrack.eodmap`. The loader accepts up to 256 MiB per package. All binary numbers are little-endian. Positions are world metres in game coordinates with Y up. Heading zero points along +Z.

| Member | Contents |
| --- | --- |
| manifest.json | Integer `version`, strings `title`, `author`, `base_arena` and `sha256`, booleans `replace_retail_spawns` and `validated`. The editor exports authored maps with replacement enabled. New maps have `validated=false`. |
| spawns.json | Ordered arrays named `grid_starts`, `battle_respawns`, `checkpoints`, `finish_gates` and `ai_waypoints`. Each pose has a three-number `position`, a unit `normal` and a finite `heading_deg`. |
| scene.glb | GLB 2.0 with the authored meshes baked into world space. The v1 scene uses a single indexed triangle primitive, float32 POSITION and COLOR_0 and uint32 indices. Empty scenes have no mesh. Vertex colors come from each part’s material color. |
| collision.eodcol | Eight magic bytes (`EODCOL1` and a trailing zero), uint32 vertex count, uint32 index count, packed float32 XYZ positions, then uint32 triangle indices. Positions and indices exactly match the scene. |

The content hash covers the manifest with its `sha256` field omitted, followed by the exact bytes of spawns.json, scene.glb and collision.eodcol. Each segment is prefixed with its byte length as a uint64. The manifest uses compact, key-sorted JSON with ASCII escapes. Netplay identifies the complete archive by a separate SHA-256 over the bytes on disk.

The runtime opens a map with `eod --map MyTrack.eodmap`. It validates the package before publishing it to the VFS. In the creator playtest, Battle provides the driving session. The replacement spawn tables are host-side vectors. Grid selection wraps by participant index. When a map supplies just one of the two spawn lists, the other uses that list too. The base arena supplies the backdrop and existing surfaces. Authored geometry participates in wheel and chassis collision.

Export suggests the host lobby’s `mods/maps/` folder beside the game settings. In the multiplayer lobby, the host can select an exported file from the map picker. Joining clients that already have the same archive SHA-256 use their cached copy. Other clients download the package, verify it, then mount it before entering the session. Both the blank canvas and installed Battle arenas support this driving mode. The blank canvas creates its driving event in memory for the lifetime of the session.
