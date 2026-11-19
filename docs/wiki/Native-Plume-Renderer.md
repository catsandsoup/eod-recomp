# Native Plume Renderer — Stages R0–R8, VIF Mesh Decoder, Classic/Modern Looks & Acceptance Gates

The **Native Plume Renderer** (`runtime/render/native_renderer.cpp`, `runtime/render/native_feed.cpp`, `src/render_queue.cpp`, `src/native_world.cpp`, `src/bone_pose.cpp`, `src/car_damage.cpp`, `src/native_shadows_flares.cpp`, `src/native_effects_imm.cpp`, `src/native_fog.cpp`, `src/native_texstate.cpp`) replaces PlayStation 2 VU1 microcode execution and GS rasterization with a commercial-grade native GPU pipeline built on **[Plume](https://github.com/renderbag/plume)** (the MIT render interface behind *RT64* and *Unleashed Recompiled*, supporting Vulkan on Windows/Linux and Metal on macOS).

> **Why Plume Instead of Filament?**
> Filament was evaluated and rejected (`docs/NATIVE_RENDERER.md`): it reorders draws internally (breaking the PS2's strict submission-order transparency), its material model cannot express all PS2 GS alpha-blend equations `(A - B) * C + D` (including reverse-subtract shadow volume passes), it only builds with MSVC on Windows (incompatible with our LLVM-MinGW toolchain), and no shipped game uses it. Plume gives us direct, low-overhead command-list control over Vulkan and Metal on a single shared device.

![Remastered 6x Gameplay](https://raw.githubusercontent.com/catsandsoup/eod-recomp/main/docs/img/world_editor/remaster_6x_gameplay.jpg)

---

## 1. One Renderer, Two Live-Switchable Looks (`F9`)

Every Plume draw originates directly in a **decompiled EE C++ function (`src/*.cpp`)** verified to 0 mismatches by `EOD_DIFF`—never by guessing cameras or geometry from raw VU1/GS traffic. From the same per-frame decompiled scene feed, Plume renders two looks switchable mid-race in **1 frame (`<= 33 ms`)** via **`F9`** (or controller **`L1 + R1 + R3`**):

| Feature | **PS2 Classic Look (`ModernRenderer=0`)** | **Modern Remastered Look (`ModernRenderer=1`)** |
|---|---|---|
| **Draw Order & State** | Strict game submission order; exact GS `TEST_1`, `ALPHA_1`, `ZBUF_1`, `CLAMP_1`, and `TEX1` state reconstructed from decompiled EE setters (`ee_state`). | Opaque/alpha passes fed by the same decompiled scene records; blend equations always come from the game's render state so `fx` / `emissive_light` cards never render as solid walls. |
| **Lighting & Materials** | Exact VU1 microcode lighting (`min(TOPS+5 + max(0, n·vf12)*TOPS+4, 128)`), car body `TFX HIGHLIGHT2` specular (`max(0, 1152(n·h) - 1056)`), and prelit `V4-5` world vertex colors (`Cv * Ct >> 7`). | Filament's published BRDF & IBL math in our own HLSL (`native_shaders.hlsl`) driven by **2,728 tagged textures** in `material_table.json`: clear-coat car paint, chrome, glass, rubber, dirt, asphalt, sky IBL, and screen-space reflections (SSR). |
| **Shadows** | Full-resolution extruded 3D shadow volumes (`MSCAL 4`, `0x34C208`/`0x34C4B0`): pass 1 additive front-faces (`Cd + Cs`), pass 0 subtractive back-faces (`Cd - Cs`), composited at `intensity / 128.0` (PS2 `320×512` cap removed!). | Cascaded directional sun shadows + soft contact grounding. |
| **Particles & VFX** | Exact PS2 camera-aligned rotated quads (`VU1 0x08E0` particles, `0x01B8` simple sprites, `0x05E0` atlas sprites, `0x304D50` lens flares). | Depth-faded soft GPU particles, multi-layered volumetric fire/smoke for exploding chickens & exhaust flames, GPU embers/sparks, HDR emissive bloom, and dynamic point-light flashes on nearby cars and terrain. |
| **Water (`AFTON.TRK`)** | Decompiled `WaterDraw` (`0x325728`) & `WaterSimulate` (`0x325E40`) wave ripple strips in 1P and 2P/4P split-screen. | Animated wave surface with fresnel reflections and depth refraction. |
| **Textures & Scale** | Native CLUT textures with exact `TEX1` mipmap LOD (`LOD = -log2approx(Q) << L + K`) **plus** support for 4K `.eodhtx` `HOSTTEX` overrides at `RenderScale` 1x–8x+. | Full trilinear/anisotropic mip chains, 4K/8K `HOSTTEX` overrides, and PBR roughness/metallic/normal parameterization. |

---

## 2. C++17 VIF Mesh Decoder (`runtime/render/vif_mesh.cpp`)

All 3D geometry in *Test Drive: Eve of Destruction* is stored on disc as prebuilt PlayStation 2 **VIF1 DMA command streams** inside `4SPS` chunks (`.mod` standalone `MODL` v9 files and `track.grf` `GRAF` v18 embedded `mrModel` records).

Our standalone C++17 decoder (`runtime/render/vif_mesh.{h,cpp}`, ported from `tools/vif_mesh.py` and verified by `tools/tests/vif_mesh_test.cpp`) decodes the **entire retail disc in 157–160 ms** on a single thread (**12.3 million vertices/second**, 180× faster than Python):

- **Retail Census Verified (`0` errors, `0` mismatches vs. Python reference):**
  - **72** `0TSR` v6 archives · **505** `MODL`/`GRAF` members · **34,564** models · **44,584** `4SPS` parts · **67,551** VU1 batches · **1,969,433** vertices · **1,113,759** triangles.

### `4SPS` Part & VU1 Batch Memory Layout

Each `4SPS` part has its byte size at `+0x04`, 16-byte null-terminated texture name at `+0x08`, damage/visibility group at `+0x18` (bits `0..15` = group `0..31`, bit `16` = `0x10000` body livery paintable flag), VIF qword count `qwc` at `+0x34`, and raw VIF1 stream starting at `+0x38`.

Inside VU1 data memory, each batch (terminated by an `MSCAL` or `MSCNT` kick) unpacks:

| VU1 Qword Address | VIF `UNPACK` Format | Decoded Attribute & Mathematical Formula |
|---|---|---|
| **`qword 0`** | `V1-32` | Vertex count `n = word0 & 0x7FFF` (bit 15 set). |
| **`qword 2 + 3i`** | `V3-16` / `V3-8` / `V3-32` (offset mode) | **3D Position:** $\vec{P} = \text{f32}(\text{STROW} + \vec{u}) + \text{q199}_{xyz}$. For world geometry (`track.grf`), `q199.xyz` holds the world-space translation offset! |
| **`qword 1 + 3i` (Normals)** | `V4-8` offset, `STROW = (65536.0f, 65536.0f, 65536.0f, 0x7F80)` | **Signed Unit Normal** (recognised by unpacked X lane `& 0xFFFFFF00 == 0x47800000`; **601,688** vertices across cars, props, drivers): $\vec{n} = (b - 128) / 128.0$, `ADC` strip-restart flag = byte 3 bit 7. |
| **`qword 1 + 3i` (Prelit Colors)** | `V4-5` (16-bit `R5G5B5A1`) or non-`0x47800000` `V4-8` | **Prelit Vertex Color** (**1,367,745** vertices across all `track.grf` `MSCAL 0x4E`/`MSCNT` world geometry, `paint.mod`, `envsky`, `sci_wall`): $R = (v \ \& \ 31) \ll 3$, $G = ((v \gg 5) \ \& \ 31) \ll 3$, $B = ((v \gg 10) \ \& \ 31) \ll 3$ on PS2 scale (`0x80` = `1.0×` neutral brightness, `248` = `1.9375×` highlight), `ADC` = bit 15. Decoded with **`fix_winding = false`** and drawn with **no backface culling**! |
| **`qword 3 + 3i`** | `V2-16` (offset mode) | **Texture UV:** $(s, t) = \text{f32}(\text{STROW} + \vec{u}) + \text{q198}_{xy}$. |
| **`qword 150..`** | `V4-8` offset (`STROW = (0, 0, 32768.0f, 32768.0f)`) or `V1-8` | **Dual-Bone Skin Word** (read in **reverse vertex order** at `TOP + 149 + n - k`): `bone_a`, `bone_b` (VU1 qword bases), weights $w_a = f_z - 32768.0$, $w_b = f_w - 32768.0$. |
| **`qword 198 / 199`** | `V4-32` | `q198` = `(u_offset, v_offset, k_lod_delta, flags)`; `q199` = `(pos_x_off, pos_y_off, pos_z_off, w)`. |

---

## 3. Decompiled EE Feed into Plume (All 9 Native Render Kinds)

Every 3D and effect primitive drawn by Plume is captured at its decompiled EE entry point:

| Native Kind | Decompiled EE Entry Points (`src/*.cpp`) | VU1 Program Replaced | What Plume Draws |
|---|---|---|---|
| **`1. queue`** (Cars, Props, Characters, Cockpit) | `0x34BDA8` (`ModelSubmitVu1Packets`), `0x34B140`, `0x34C9C0`, `0x34C930`, `0x34C860`, `0x34AF98` (`RenderQueueFlush` in `src/render_queue.cpp`) | `0x02A8`, `0x02E0` (`MSCAL 0x5C`), `0x0318` (`MSCAL 0x63`), `0x24B0` | Static and posed 3D models sorted by layer/texture bucket with exact camera projection `screen_matrix` (`1.3e-6` max error vs. VU1 `vf4..vf7`). |
| **`2. world`** (Track Scene & Sky Dome) | `0x214B68` (`WorldFrameRender`), `0x3582B8` (`MlistVisibilityCalc`), `0x358BD0` (`WorldRenderPass` 0..4), `0x35DEE0`, `0x35CBB8`, `0x223858` (`SkyDraw`), `0x223800` (`SkyDestroy`) in `src/native_world.cpp` | `0x0270` (`MSCAL 0x4E`), `0x0618`, `0x0920`, `0x0350` (`MSCAL 0x6A` sun-facing quads) | All visible track leaves per pass (`mlist` scratchpad visibility bytes `& 0x7F == req`), 32-part sky dome with cone culling, and CPU-expanded camera-facing foliage/billboard quads (`0xAC..0xAF` sun quad corners). |
| **`3. water`** (Woodly Water Body) | `0x326168` (`WaterDrawIfPresent`), `0x325728` (`WaterDraw`), `0x325E40` (`WaterSimulate`) in `src/native_world.cpp` | `0x0270` via immediate API | Animated wave surface (`*(0x3DC134)`) on Woodly (`AFTON.TRK`) with car splash simulation ($v > 2.222\text{ m/s}$), drawn in 1P and 2P/4P split-screen. |
| **`4. shadow`** (Extruded 3D Shadow Volumes) | `0x34C7D0` (`ShadowBegin`), `0x34C798` (`ShadowEnd`), `0x34C208` (`ShadowSubmit`), `0x34C4B0` (`ShadowSubmitMasked`), `0x35D170` (`ShadowEmitBatchPass`), `0x35D298` (`ShadowEmitBatch`) in `src/native_shadows_flares.cpp` | `0x0020` (`MSCAL 4`, `vu1_0x3BE4C0`) | Model-space vertex extrusion along light vector `vf1 = -M^T * L` up to `extrudeLen` (`0x4D7D18`), rendered into a full-resolution host shadow buffer (pass 1 additive, pass 0 subtractive) and composited at `intensity / 128.0`. |
| **`5. flare`** (Sun Glare & Light Flares) | `0x304D50` (`FlareRender`), `0x20F2D8` (`ViewportRenderFinish`) in `src/native_shadows_flares.cpp` | `0x0270` (`0x303CB8` 2D quads) | Sun glare (`0x3DBB40`) and point-light lens flares (`0x3DBB48`, up to 32 entries/bucket at `0x3D94F4`), with the PS2 single-viewport restriction lifted in 2P/4P (`SplitScreenParity=1`). |
| **`6. particle`** (Smoke, Dust, Sparks, Dirt) | `0x363B98` (`ParticleBegin`), `0x3639D0` (`ParticleAdd`), `0x363B68` (`ParticleEnd`), `0x363FF8` (`ParticlePacketReserve`) in `src/native_effects_imm.cpp` | `0x08E0` (`vu1_029b295eacf3f666.bin`) | Camera-aligned rotated quads using the 79-entry `(cos, sin)` table (`0x4D87B4`) and vertical sky-gradient shading `rgb * (1 - 0.5*scale*(1 - up_dot))`. |
| **`7. sprite`** (Simple & Atlas Effect Sprites) | `0x363F48`/`0x363DE8`/`0x363F18`/`0x364548` (Simple) & `0x363D78`/`0x363C18`/`0x363D48`/`0x3642B8` (Atlas) in `src/native_effects_imm.cpp` | `0x01B8` (Simple), `0x05E0` (Atlas) | Explosion fireballs (chicken detonations), exhaust flames, and effect cards with per-sprite UV atlas rectangles. |
| **`8. immediate`** (General & Sphere-Tested Immediate Prims) | `0x35DF10` (`ImmediateBegin`), `0x361468`, `0x361708` (`ImmediateVertex`), `0x361BC8` (`ImmediateEnd`), `0x364D18`, `0x34EE70`/`0x361ED8`/`0x361C30`, `0x35CAA0` in `src/native_effects_imm.cpp` | `0x0270` (`MSCAL 0x4E`) | Dynamic triangle strips and indexed triangles (`0x35CAA0`), plus sphere-cull-tested static immediate VIF refs (`0x361ED8`). |
| **`9. cockpit`** (Interior Overlay Quad) | `0x219C48` → `0x34DB70` → `0x35CAA0` (`ra == 0x21A064`) in `src/native_effects_imm.cpp` | `0x0270` | Non-queue cockpit stack quad when `0x2114A8() != -1` (while main cockpit interior `car+0x228` and posed gauges draw via `queue`). |

---

## 4. Roadmap Stages R0–R8 & R9 Status Summary

| Stage | Scope & Technical Milestone | Verification Status & Evidence (`docs/NATIVE_RENDERER_SUCCESS.md`) |
|---|---|---|
| **R0** | **Plume Device Ownership:** `runtime/render/plume_device.cpp` creates and owns the single Vulkan 1.3 instance/device; legacy GS borrows handles for zero-copy composition. | **DONE:** Offline GS replay MD5s (`1ae45b16...` at 1x, `8a86f217...` at 6x) unchanged. |
| **R1** | **Queued 3D Objects (`0x34AF98`):** Cars, props, and characters drawn by Plume from decompiled `src/render_queue.cpp`. | **DONE:** 798,780/798,780 race records drawn (P1 = 100%); max projection error `1.3e-6` vs. VU1 `vf4..vf7`; GPU frame time dropped from `12.8 ms` to `9.8 ms` at 6x. |
| **R2** | **Track World, Sky & Water (`0x214B68`):** 10 functions in `src/native_world.cpp` (`WorldFrameRender`, `MlistVisibilityCalc`, `WorldRenderPass`, `SkyDraw`, `WaterDraw`, `WaterSimulate`). | **DONE:** `EOD_DIFF` `0` mismatches across 18,432+ checked calls; 100% P1 leaf & water equality across Woodly 1P, Career Free Roam, Dundas 1P/2P, and Anoka Battle; Woodly water verified (`1,232/1,232`). |
| **R3** | **Skinned Characters & 157-Bone Car Deformation:** `src/bone_pose.cpp` (`0x34C9F8`, `0x35DBD8`, `0x35A810`, `0x35DD28`, `0x34CA18`), `src/car_damage.cpp` (`0x17B918`, `0x18CA98`, `0x18AFE0`), `src/debris_pool.cpp` (`0x1B3068`, `0x1E1E88`, `0x1E29C0`, `0x1B3180`, `0x1B3190`). | **DONE:** **P2 bit-identical** bone palette words (`241,920/241,920`) and VU1 dent words (`581,652/581,652`); **99.91%** of 4.67M non-clip posed vertices within `<= 1/16 px` of GS (`p99 = 0.0625 px`); wheel/panel debris pools expanded (`105` live wheels, `34` live panels). |
| **R4** | **Shadows, Flares, Particles, Sprites & Immediate Geometry:** 28 live-reachable functions in `src/native_shadows_flares.cpp` and `src/native_effects_imm.cpp`; VU1 `0x25F0 -> 0x2630` traced to `0x3620A8` GIFtag template setup. | **DONE:** `>= 1,000` `EOD_DIFF` checks on every live function (`0` mismatches); 100% P1 equality across **all 9 native kinds** and all 10 camera views; PS2 `320×512` shadow RT cap removed; `EOD_NATIVE_ONLY=1` dropped GS primitives reduced by **90.8%** (`133,823 → 12,337`, leaving only 2D/HUD for R7). |
| **R5** | **Classic Visual Parity (`P3`) & Unthrottled LOD (`P10`):** `tools/p3_compare.py` (MAE, p99, SSIM, fog/sky error vs. software GS replay), `src/native_fog.cpp` (`0x33ECB8`, `0x3653F8`, `0x365428`, `0x365540`), `src/native_texstate.cpp` (`0x3652A8`, `0x3654E0`, per-run `q198.z` `K` kicks, GS `log2approx(Q)` mipmap sampler, 2-pass `AFAIL FB_ONLY/ZB_ONLY`). | **Phase A1 & A2 Landed:** Proved retail PS2/Xbox never enable `PRIM.FGE` (`0` fogged pixels in retail replays); exact EE state match across 1.86M world draws; Gilroy Night SSIM improved from `0.9338` to **`0.9561`** (`MAE 2.44`, `sky MAE 1.79`). Shadow composite & near-plane depth clamp polish in progress for final `SSIM >= 0.98`. |
| **R6** | **Modern PBR & GPU VFX:** Connect all 2,728 `material_table.json` records (resolving 318 low-confidence textures) and replace flat 2004 billboard quads in Modern mode with depth-faded volumetric GPU fire/smoke/sparks & dynamic point-light flashes. | **Specified & In Progress:** `material_table.json` tagged; Modern PBR shaders & GPU particle VFX pipeline designed. |
| **R7** | **Native 2D / HUD & Emulator Removal (`S4 = 0`):** Move PATH2/PATH3 GS transfers (`MenuRun`, `HudDrawLeftRaceRows`, `HudDrawRightSpeedRows`, `GTEX` v4 fonts, `CANV` v1 canvases, `.PSS` movies) to Plume and cut VU1/GS emulation from the frame loop (eliminating all 58 synchronous VRAM readbacks/frame). | **Planned (Next after R5/R6):** Targets the final ~35 2D GS primitives/frame remaining after R4. |
| **R8** | **macOS Metal, Speed Sign-Off (`S1–S3`), Photo Mode (`P11`) & Creation Kit (`P13`):** `plume_metal.cpp`, 8K Photo Mode (`< 2 s`), fullscreen Tracy + `EOD_GPU_PROF` sign-off at 60+ FPS. | **Planned.** |
| **R9** | **Creation Kit & World Editor (`R9a–R9e`):** Standalone 3D World Editor (`tools/world_editor/`) with 37 Cars & Wrecks, Ghost Place, Scatter Brush, SOBL barrier flattening, ILIN road editor, 4K `HOSTTEX` swap, and embedded PIE. | **Phase 1 & Priority P1–P10 Implemented:** Verified by `tools/world_editor/test_editor.py`. |

---

## 5. Master Acceptance Gates (P1–P13, S1–S4, D1–D5)

Nothing counts as complete without measured numerical proof against these gates across the **11-Scene Verification Suite** (*1. Afton Jump Race 1P · 2. Afton/Dennison/Dundas 2P & 4P Split-Screen · 3. Career Free Roam Day · 4. Career Free Roam Night / Anoka Night · 5. Car Dealer · 6. Garage / Paint / Upgrades · 7. Front-End Menus · 8. Loading Screen · 9. Memory Card Screen · 10. `.PSS` Movie · 11. Heavy Demolition Crash Replay*):

### Player & Visual Gates (`P1–P13`)
- **P1 (Object Parity):** Per frame, objects submitted by the game (`queue`, `world`, `water`, `shadow`, `flare`, `particle`, `sprite`, `immediate`, `cockpit`) == objects drawn by Plume on 100% of frames.
- **P2 (Animation & Damage Parity):** Bone palettes (`0x34C9F8`) and 157-bone car dent offsets (`Bone.mvl` / `0x35DD28`) are bit-identical to EE outputs; posed vertices within `<= 1/16 px` (`0.0625 px`) of GS reference at 1x.
- **P3 (Classic Parity):** Plume PS2 Classic look at 1x vs. offline software GS replay of the same frame (`tools/p3_compare.py`): per-channel `MAE <= 1/255`, `p99 <= 8/255`, `SSIM >= 0.98`, fog and sky pixels within `<= 2/255`.
- **P4 (Clean Modern Look):** Filament BRDF/IBL math in native HLSL with cascaded sun shadows, sky IBL, SSR, clear-coat paint, chrome, glass, and water refraction from `material_table.json`. No unrequested vignette, color grade, or haze; fog/sky within `<= 4/255` of the game's setters.
- **P5 (Instant Toggle):** `F9` switches Classic ↔ Modern in **1 frame (`<= 33 ms`)** with **0 MB** GPU memory growth over 100 toggles.
- **P6 (Reference Integrity):** Offline GS replay of recorded race dumps reproduces reference VRAM MD5s (`f30d64c0...` / `1ae45b16...` at 1x, `8a86f217...` at 6x).
- **P7 (Determinism):** Solo replay and 2-instance netplay produce identical lap times, physics, and results with **0 desyncs**.
- **P8 (Split-Screen Parity):** 2P/4P views match 1P object lists, world passes 0..4, draw distance, LOD, sun glare/flares (`0x304D50`), and Woodly (`AFTON.TRK`) water (`0x325728`).
- **P9 (Share-Worthy Remaster):** 6x and 8x Modern captures show dynamic sun shadows, reflections, and sky IBL with zero shimmer or pop-in.
- **P10 (Unthrottled Maximum Detail):** `Detail=maximum` forces highest car (`Low1.mod`) and world LOD at every visible distance across 1P and split-screen.
- **P11 (8K Photo Mode):** Pause, free camera (orbit, fly, roll, FOV, DOF, no HUD), and export `7680×4320` PNG in `< 2 s`.
- **P12 (Video-Ready Pacing):** 5-minute Tracy race capture with video recording active: `0` missed vblanks, `p99 <= 16.7 ms`.
- **P13 (Modded Art Parity):** Creation Kit 4K/8K car and track textures (`.eodhtx`) render in both Classic and Modern looks with no seams or fallbacks.

### Speed Gates (`S1–S4`)
- **S1 (Target Floor — i5-7600K + Radeon Pro 580):** `RenderScale 6`, max LOD, Modern look, fullscreen vsync: locked **60 FPS**, `0` missed vblanks, `p99 CPU <= 12 ms`, `p99 GPU <= 14 ms` over 5-min race + 2-min free roam.
- **S2 (Minimum Floor — Intel UHD 620-class Vulkan iGPU):** `RenderScale 1–2`, PS2 Classic look: locked **60 FPS**, `0` missed vblanks.
- **S3 (Modern Hardware / Apple Silicon M2 Pro):** `RenderScale 6–8+`, max LOD: **60–240+ FPS** with zero synchronous VRAM readback stalls.
- **S4 (Zero Emulated Graphics in Frame Path):** `0` VU1 microprogram runs, `0` emulated GS primitives rasterized, and `0` synchronous VRAM readbacks per frame across the entire 11-scene suite.

### Decompilation Gates (`D1–D5`)
- **D1:** Readable C++ replaces every function in the subsystem (`config/hle.txt` `wrap` / `diff_run`).
- **D2:** `EOD_DIFF` proves **0 mismatches** over `>= 1,000` checked calls on every code path reached by the scene suite.
- **D3:** P6 and P7 hold with the C++ replacement active.
- **D4:** `ninja -C build eod` boots through menus, race, and career, exiting cleanly with **0 heap errors** (`EOD_HEAP_CHECK=1`).
- **D5:** Documented in `docs/<SUBSYSTEM>.md` with guest addresses, struct layouts, call graphs, and open edges.
