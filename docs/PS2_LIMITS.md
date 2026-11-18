# PS2 limits

Every fixed pool, buffer or table the game had on the PS2, who owns it, and its state. Rule
(docs/MASTER_SPEC.md section 3): a subsystem that is decompiled replaces its caps with host storage
that grows on demand and is released when unused. Runtime watcher: `runtime/core/limit_tracker.h`
(`[limit]` log lines: 75% of the PS2 cap, past it, scene high-water marks).

States: **removed** (host storage grows and is released), **tracked** (still a guest-side fixed
size because the guest layout itself is the limit; every run logs its use), **open** (not touched
yet).

| limit | owner | size on the PS2 | state | evidence |
| --- | --- | --- | --- | --- |
| Draw record pool `0x4C96C4` / `0x4C96C8` | `0x34B140` (src/render_queue.cpp) | `0xC00` records | open | docs/EE_RENDER_SUBMISSION.md |
| Shadow render target `0x34C7D0` -> `0x363218` | `0x35DA30` | 320x512 | open (R4) | |
| Plume texture cache | runtime/render/native_renderer.cpp | 256 MB soft limit | open | |
| Frame renderer list `0x20F4A0` / `0x3D1C90` | `0x20F4A0` | 20 entries | open | |
| Engine pools (heap, mip ring, chassis, car pool, debris, weapons) | see docs/MASTER_SPEC.md 3.5 | | open | |
| Contacts per object, obj+0x190 / count obj+0x1D0 | `0x17B918` ContactRecord (src/car_damage.cpp) | 16 | tracked (`[limit] contacts per object`): every reader of obj+0x190 must be decompiled before the list can move to host storage | docs/NATIVE_RENDERER.md R3 |
| Contact record pool `0x3CE3F0` (used `0x3CE3EC`, size `0x3CE3E8`) | `0x17B918` (src/car_damage.cpp) | `*(0x3CE3E8)` records of 0x24 bytes | tracked (`[limit] contact records`) | same |
| Detached wheel debris `0x484E68` | `0x1B3068` DetachedWheelSpawn | 16 | open (R3: owner still recompiled) | |
| Detached panel debris `0x494D60` | `0x1E0DC8` / `0x1DF9A0` | 16 | open (R3: owners still recompiled) | |
| Detachable panels per car, table `0x44AE68`, states car+0x13E8 | `0x18AFE0`, `0x18CA98` (src/car_damage.cpp) | 8 (data table, not a pool) | kept: the count is the panel table of the retail data | docs/NATIVE_RENDERER.md R3 |
| Native world leaf list (one texture record's REFs) | runtime/render/native_feed.cpp `g_leaves` | none on the PS2 (packet space below) | removed: `std::vector`, released when a pass ends with more than 4096 kept | |
| Native water mesh | runtime/render/native_feed.cpp | ImmediateBegin request `0x3000` vertices, 60-vertex packet chunks | removed: the native mesh grows with the surface (`[limit] water surface vertices`), transient GPU meshes released 8 vblanks after use | docs/NATIVE_RENDERER_SUCCESS.md R2 |
| Native camera-facing parts (VU1 `0x350`) | runtime/render/native_feed.cpp | VU1 memory (2048 qwords per batch) | removed: expanded on the CPU per draw, transient meshes | |
| mlist visibility bytes, scratchpad `0x70000000..0x70001BFF` | `0x3582B8` (src/native_world.cpp) | `0x1C00` spheres + groups + models | tracked (`[limit] mlist visibility bytes`): the 16 KB scratchpad is the guest's own memory map; moving it needs every reader (`0x358BD0`, `0x359198`, `0x357E60`, display objects) decompiled | docs/NATIVE_RENDERER_SUCCESS.md R2 |
| World matrix table, scratchpad `0x70001C00` | `0x3582B8` / `0x358BD0` | 256 matrix packets (retail tracks use entry 0 only) | tracked (`[limit] world matrix table`) | same |
| DMA packet buffer of manager `*(0x3E5978)` | `0x366D30` / `0x366CD0` (`0x35CBB8`) | fixed guest buffers | tracked: a full buffer drops a world texture record; counted as `dropped (no packet space)` in `[native] ee` lines (0 in every R2 run) | same |
