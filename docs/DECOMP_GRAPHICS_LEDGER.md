# EE Graphics Subsystem Decompilation Ledger (`SLUS_209.10`)

Master function-by-function ledger of the PlayStation 2 Emotion Engine (`SLUS_209.10`) graphics subsystem, verified directly against `SLUS_209.10` disassembly (`tools/ee_dis.py`) and statically recompiled C (`gen/rec_*.c`).

All new decompilations follow the `EOD_DIFF` equivalence standard (`runtime/core/diff_harness.h`, `src/render_queue.cpp`, `src/bone_pose.cpp`, `src/car_damage.cpp`): exact guest memory side effects, struct offsets, EE FPU rounding (`fadd_ps2`/`fmul_ps2`/`frsqrt_ps2`/etc.), stack frame sizes, callee `$ra`, and `$v0`/`$f0` return values.

---

## Status Legend

- **Verified (`src/...`)**: Already wired into `config/hle.txt` & `CMakeLists.txt` with `diff_run`.
- **Decompiled — Ready (`src/decomp_graphics_env.cpp`)**: Complete, syntax-checked, behaviorally exact C++ ready to wire into `config/hle.txt` & `CMakeLists.txt` and verify with `EOD_DIFF`.
- **Disassembled — Queued**: Instruction-level disassembly, struct offsets, and call graph verified below; queued for next C++ decompilation pass.

---

## Cluster A — `GRAF` Environment, Lighting & Display Object Subtypes

### Vtable Map (`0x408100..0x408348`)

All `GRAF` environment objects share the display-object vtable layout:
- `+0x08` (slot 2): Destructor
- `+0x10` (slot 4): `Init(self, graf_chunk)`
- `+0x18` (slot 6): `Apply(self)` (scene activation)
- `+0x20` (slot 8): `Draw(self)` (panics on non-drawable config objects)

| Object | Size | Vtable | Destructor (`+0x08`) | `Init` (`+0x10`) | `Apply` (`+0x18`) | `Draw` (`+0x20`) |
| --- | --- | --- | --- | --- | --- | --- |
| `FlareSource` | `0x04` | `0x408100` | `0x222510` | `0x2224C0` | `0x223CE0` (nop) | `0x2224B8` (nop) |
| `FogColor` | `0x0C` | `0x408258` | `0x2234F8` | `0x2234E0` | `0x223488` | `0x223478` (panic) |
| `SunIntensity` | `0x0C` | `0x408288` | `0x2235B0` | `0x223548` | `0x223520` | `0x223510` (panic) |
| `ShadowColor` | `0x0C` | `0x4082B8` | `0x223610` | `0x2235F8` | `0x2235D8` | `0x2235C8` (panic) |
| `AmbientColor` | `0x0C` | `0x4082E8` | `0x223690` | `0x223678` | `0x223638` | `0x223628` (panic) |
| `Sun` | `0x14` | `0x408318` | `0x223800` | `0x2237D8` | `0x2236B0` | `0x2236A8` |
| `Sky` | `0x40` | `0x408348` | `0x223A58` | `0x223A50` | `0x223CE0` (nop) | `0x223858` |

### Function Ledger (Cluster A)

| Address | Size (bytes / insns) | Name | Callers / Callees | Key Struct / Global Offsets | Status |
| --- | --- | --- | --- | --- | --- |
| `0x221DC8` | `0x38` / 14 | `SunAlloc` | `GRAF` factory -> `0x12E870` | `0x14` bytes: `+0x00` vtable `0x408318`, `+0x04..+0x0C` pos, `+0x10` color (`0`) | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x2237D8` | `0x28` / 10 | `SunInit` | vtable `0x408318+0x10` | `chunk+0x2C -> self+0x10` (RGB), `chunk+0x30..+0x38 -> self+0x04..+0x0C` (pos xyz) | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x2236B0` | `0x128` / 74 | `SunApply` | vtable `0x408318+0x18` -> `0x211150`, `0x34E8A8`, `0x3053C0`, `0x224778`, `0x34E880`, `0x30AA10`, `0x305350` | Day default `(0, 250, -150)` / `0x00FDFEE7`; Night (`tod==2`) `(0, 100, 30)` / `0x0075889B`; negates & normalizes pos -> ray dir | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x2236A8` | `0x08` / 2 | `SunDraw` | vtable `0x408318+0x20` -> `0x304F58` | Tail-calls `0x304F58(self + 0x04)` | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x223800` | `0x4C` / 19 | `SkyDestroy` (`Sun` dtor) | vtable `0x408318+0x08` -> `0x34E8A8(-1)`, `0x223D00` | Resets directional light color to white (`-1`) | Verified (`src/native_world.cpp`) |
| `0x224778` | `0x100` / 64 | `SunClampMinElevation` | `0x2236B0` -> `atanf` (`0x39B7A8`), `cosf` (`0x39BB28`), `sinf` (`0x39BD68`) | Clamps `atan(-dir.y / hypot(dir.x, dir.z))` to `*(float*)0x496480` | Disassembled — Queued |
| `0x34E880` | `0x28` / 10 | `SetSunDirection` | `0x2236B0`, `0x218850`, `0x2AF44C` -> `0x34E868` | Copies `vec3` to `0x4CD2E8..0x4CD2F0` | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x34E868` | `0x18` / 6 | `CommitSunDirectionAndIntensity` | `0x34E880`, `0x34F570` -> `0x35E3F0` | Reads `*(float*)0x3DD648`, passes `&0x4CD2E8` to `0x35E3F0` | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x35E3F0` | `0x2C` / 11 | `Vu1SetLightDirection` | `0x34E868` | Writes `dir.xyz` to `0x4D7C80..0x4D7C88`, `-intensity` to `0x4D7C8C` (VU1 `TOPS+6`) | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x34E8A8` | `0x1C` / 7 | `SetDirectionalLightColor` | `0x2236B0`, `0x223800` -> `0x35E388` | Unpacks `0xRRGGBB` to `(r, g, b)` | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x35E388` | `0x64` / 25 | `Vu1SetDirectionalLightColor` | `0x34E8A8` | Writes `rgb * (128/255)` (`0x3F008081`) to `0x4D7C90..0x4D7C98`, `0` to `0x4D7C9C` (VU1 `TOPS+4`) | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x30AA10` | `0x54` / 21 | `ShadowSetDirection` | `0x2236B0` | Normalizes `vec3` via `rsqrt.s` into `0x4AE970..0x4AE978` | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x3053C0` | `0x18` / 6 | `SunFlareSetColor` | `0x2236B0` | `*(0x3DBB3C) = rgb & 0x00FFFFFF` | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x305350` | `0x0C` / 3 | `SunFlareSetEnabled` | `0x2236B0` | `*(0x3DBB40) = enabled` | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x221D88` | `0x3C` / 15 | `AmbientColorAlloc` | `GRAF` factory -> `0x12E870` | `0x0C` bytes: `+0x00` vtable `0x4082E8`, `+0x04` valid (`0`), `+0x08` color (`0`) | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x223678` | `0x14` / 5 | `AmbientColorInit` | vtable `0x4082E8+0x10` | `self+0x04 = 1`, `self+0x08 = *(chunk+0x2C)` | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x223638` | `0x40` / 16 | `AmbientColorApply` | vtable `0x4082E8+0x18` -> `0x211150`, `0x34F5A0` | Ignores `self+0x08`; passes `0xFF646464` (night) or `0xFF909090` (day) | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x34F5A0` | `0x48` / 18 | `SetAmbientColor` | `0x223638`, `0x2189C8`, `0x218E68` (15 sites) -> `0x35E420` | Swaps `*(0x4CD478)` (returns prev), unpacks `rgb` to `0x35E420` | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x35E420` | `0x64` / 25 | `Vu1SetAmbientColor` | `0x34F5A0` | Writes `rgb * (128/255)` (`0x3F008081`) to `0x4D7C70..0x4D7C78`, `0` to `0x4D7C7C` (VU1 `TOPS+5`) | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x221D48` | `0x3C` / 15 | `ShadowColorAlloc` | `GRAF` factory -> `0x12E870` | `0x0C` bytes: `+0x00` vtable `0x4082B8`, `+0x04` valid, `+0x08` color | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x2235F8` | `0x14` / 5 | `ShadowColorInit` | vtable `0x4082B8+0x10` | `self+0x04 = 1`, `self+0x08 = *(chunk+0x2C)` | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x2235D8` | `0x20` / 8 | `ShadowColorApply` | vtable `0x4082B8+0x18` -> `0x30AA00` | Uses `self+0x08` if `self+0x04 != 0`, else `0x80111C28` | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x30AA00` | `0x10` / 4 | `ShadowSetAlphaFromColor` | `0x2235D8` | `*(0x3DC064) = argb >> 24` | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x221D08` | `0x3C` / 15 | `SunIntensityAlloc` | `GRAF` factory -> `0x12E870` | `0x0C` bytes: `+0x00` vtable `0x408288`, `+0x04` valid, `+0x08` intensity | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x223548` | `0x5C` / 23 | `SunIntensityInit` | vtable `0x408288+0x10` | `self+0x04 = 1`, `self+0x08 = float(*(chunk+0x2C) & 0xFF) * (1/255)` (`0x3B808081`) | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x223520` | `0x24` / 9 | `SunIntensityApply` | vtable `0x408288+0x18` -> `0x305360` | Passes `*(float*)(self+0x08)` if `self+0x04 != 0`, else `1.0f` | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x305360` | `0x5C` / 23 | `SunFlareSetIntensity` | `0x223520` | `*(0x3DBB38) = f12`; `s = (f12 < 0.6f) ? f12*(5/3) : 1.0f`; `*(0x3DBB10) = s*s` | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x34F570` | `0x30` / 12 | `SetLightIntensity` | `0x2189C8`, `0x218E68` (15 sites) -> `0x34E868` | Swaps `*(float*)0x3DD648` (returns prev in `$f0`), commits via `0x34E868` | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x221CC8` | `0x3C` / 15 | `FogColorAlloc` | `GRAF` factory -> `0x12E870` | `0x0C` bytes: `+0x00` vtable `0x408258`, `+0x04` valid, `+0x08` rgb | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x2234E0` | `0x14` / 5 | `FogColorInit` | vtable `0x408258+0x10` | `self+0x04 = 1`, `self+0x08 = *(chunk+0x2C)` | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x223488` | `0x58` / 22 | `FogColorApply` | vtable `0x408258+0x18` -> `0x211150`, `0x34E848` | Uses `self+0x08` if set, else `0x000000` (night) / `0xC9C9EB` (day) | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x34E848` | `0x1C` / 7 | `FogSetColorPacked` | `0x223488` -> `0x365428` | Unpacks `0xRRGGBB` to `(r, g, b)` and tail-calls `0x365428` | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x365428` | `0x48` / 18 | `FogSetColour` | `0x34E848` | Writes `0x4D8B80` (`FOGCOL`) and `*(0x4D8B74) |= 0x10000` | Verified (`src/native_fog.cpp`) |
| `0x221E00` | `0x80` / 32 | `SkyAlloc` | `GRAF` factory -> `0x12E870` | `0x40` bytes: `+0x00` vtable `0x408348`, `+0x08=1`, `+0x0C..+0x38` identity 3x4, `+0x3C=0` | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x223A50` | `0x08` / 2 | `SkyInit` | vtable `0x408348+0x10` | `*(self + 0x3C) = a1` (sky model/chunk ptr) | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x223858` | `0x1F8` / 126 | `SkyDraw` | vtable `0x408348+0x20` -> `0x34BDA8` | Draws sky dome parts centered on camera | Verified (`src/native_world.cpp`) |
| `0x221B18` | `0x34` / 13 | `FlareSourceAlloc` | `GRAF` factory -> `0x12E870` | `0x04` bytes: `+0x00` vtable `0x408100` | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x2224C0` | `0x48` / 18 | `FlareSourceInit` | vtable `0x408100+0x10` -> `0x211150`, `0x3052F0` | **Night-only** (`*(tod+0x0C) == 2`): calls `0x3052F0(a1+0x2C, f12=*(a1+0x38), f13=*(a1+0x3C))` | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x3052F0` | `0x5C` / 23 | `FlareSourceAppend` | `0x2224C0` | Appends to `0x3DBB48[count++]` (`0x2C` stride, max `24`); pos at `+0x04..+0x0C`, `f13` at `+0x20`, `0` at `+0x24`, `f12` at `+0x28` | Decompiled — Ready (`decomp_graphics_env.cpp`) |

### Instruction-Level Proofs (Cluster A)

1. **`+Y` Up & `0x4CD2E8` Light-Ray Direction (`0x2236B0`):**
   - `0x2236D8..0x2236F8`: Day default sun world position is `(0.0f, +250.0f [0x437A0000], -150.0f [0xC3160000])`. Night default (`0x223718..0x223738`) is `(0.0f, +100.0f [0x42C80000], +30.0f [0x41F00000])`. Thus world `+Y` is UP.
   - `0x22374C..0x22379C`: `neg.s` is applied to all three components (`-pos.x, -pos.y, -pos.z`), then normalized via `mula.s`/`madda.s`/`madd.s` + `rsqrt.s $f4, 1.0f, len_sq`. Thus `0x4CD2E8` (and VU1 `TOPS+6` at `0x4D7C80`, and shadow dir at `0x4AE970`) stores the **unit light-ray travel direction (from sun toward origin, `y < 0`)**, NOT the to-sun vector.
   - In `0x35E3F0` (`0x35E3FC`), `$f12` (`intensity`) is negated (`neg.s`) into `0x4D7C8C` (`TOPS+6.w`). VU1 setup program `0x0000` computes `vf12 = R^T * L * L.w`, so `L * (-intensity)` flips the downward light-ray back to a positive to-sun diffuse vector scaled by `+intensity`.
2. **`FlareSource` Layout (`0x3052F0`):**
   - `0x305324..0x305338`: `ldl`/`ldr` + `sdl`/`sdr` copy 8 bytes (`x, y`) from `pos+0..+7` to `entry+0x04..+0x0B`, and `lw`/`sw` copy 4 bytes (`z`) from `pos+8` to `entry+0x0C`. Entry `+0x00` is the visibility/active word, `+0x04..+0x0C` is world `xyz`, `+0x20` is `f13` (intensity), `+0x24` is `0`, `+0x28` is `f12` (size).

---

## Cluster B — Camera, Projection & Viewport Setup

| Address | Size (bytes / insns) | Name | Callers / Callees | Key Struct / Global Offsets | Status |
| --- | --- | --- | --- | --- | --- |
| `0x35B3D0` | `0x4E4` / 313 | `Vu1WriteCameraPacket` | `0x35BA10` -> `0x366D30`, `0x38AE78`, `0x38A5D0`, `0x366CD0` | 18-qword DMA CNT packet -> VU1 qwords `1..16`; reads `0x4D7C10..2C`, `0x4D7C60..64`, `0x3DD658`, `0x3E57A8..D4`; clears `*(0x3E57E4)` | Decompiled — Ready (`eod_env_0035B3D0` in `decomp_graphics_env.cpp`) |
| `0x34D8A0` | `0xE4` / 57 | `ViewportSet` | `0x20F2D8`, `0x20F4A0`, etc. -> `0x35E200`, `0x353A20` | Optional vertical scale around `*(0x3DC97C)/2` by `*(float*)0x3DD65C`; writes `0x4CD388..94`; tail-calls `0x353A20(w, *(float*)0x4CD378)` | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x35E200` | `0x100` / 64 | `Vu1SetViewport` | `0x34D8A0` -> `0x35B8B8` | Scales `y, h` by `14/15` (`0x3F6EEEF0`) when `*(0x3E57E8)==0`; writes `0x4D7C20..2C`, guard-band `0x4D7C60 = 0.75*w`, `0x4D7C64 = 0.825*h`; sets `0x3E57EC = 1`, `0x3E57E4 = 1`, `0x4D7C44 = 1`, clears `0x4D7C50` & `0x4D7C3C` | Decompiled — Ready (`decomp_graphics_env.cpp`) |

### Instruction-Level Proofs (Cluster B)

1. **Negative `sy` in `0x35B3D0` (`Vu1WriteCameraPacket`):**
   - `0x35B564..0x35B588`: Loads `0xBF6EEEF0` (`-14.0f / 15.0f = -0.9333334f`) into `$f22`, divides by `*(float*)0x3DD658` (`g_DisplayAspectXScale`).
   - `0x35B5D4`: Writes `$f22` (`sy_aspect < 0`) into `V_scale[1][1]` (`$sp + 0x14`), which is multiplied with `P` (`0x38A5D0`) to produce VU1 qwords `5..8` (`proj * viewport`).
   - `0x35B650..0x35B694`: Computes `vp_sy = ((fx * sy_aspect) * guard_y_near) / near_z` — strictly **negative** — and stores it at `pkt + 0x34` (VU1 `q2.y`). This flips `+Y`-up world/camera space into `+Y`-down PS2 GS screen coordinates.
2. **Exact GS Depth & Screen-Center Constants (`0x35B3D0`):**
   - `cx = vp_x + 2047.9375f [0x44FFFE00] + vp_w * 0.5f - 320.0f [0x43A00000]`
   - `cy = vp_y + 2047.9375f [0x44FFFE00] + vp_h * 0.5f - 224.0f [0x43600000]`
   - `V_scale[2][2] = (far * near * 65499.0f [0x477FDB00]) / (far - near)`
   - `V_scale[3][2] = (near * -65500.0f [0xC77FDC00] + far) / (far - near)`
   - `VP[2][2] = -32749.5f [0xC6FFDB00]`, `VP[3][2] = +32750.5f [0x46FFDD00]`
3. **View Matrix (`0x35B6A8..0x35B844`):**
   - Copies 3×3 camera rotation `R` from `0x3E57A8..0x3E57C8`, transposes in-place (`R^T`), computes `-R^T * t` with `t = 0x3E57CC..0x3E57D4` (`mula`/`madda`/`madd` + `neg.s`), writes to VU1 qwords `13..16` (`pkt + 0xE0..0x11F`), and overwrites `q16.w` (`pkt + 0x11C`) with `fov_scale * vp_w * 0.5f`.

---

## Cluster C — Texture Slot Manager & World Visibility

### `TextureSlot` Layout (`*(0x4D2D30)`, 512 entries × `0x44` bytes = `0x8800` bytes)

Verified from `0x3507F8`, `0x350F58`, `0x3514F0`, and `0x353470`:

| Offset | Type | Field |
| --- | --- | --- |
| `+0x00` | `int32` | Reference count (`0` = free slot) |
| `+0x04..+0x13` | `char[16]` | Texture name (max length 15; panics if `strlen >= 16`) |
| `+0x14` | `uint32` | Group / flags (`a1` in `0x350F58`) |
| `+0x18` | `uint32` | Base GS texture object pointer (returned by `0x350D78`) |
| `+0x1C` | `uint32` | Source texture header pointer (`0x352400` or caller `a2`) |
| `+0x20` | `uint32` | `is_inline` flag (`a2 != 0`) |
| `+0x24` | `uint32` | `is_static` flag (returned by `0x350C80`: `"team*"`, `0x3DD688` table, `"envmap"` exclusion) |
| `+0x28` | `uint32` | Has-alpha / format flag (`*(uint8*)(tex_hdr + 0x0D) != 0`) |
| `+0x2C` | `uint32` | Streaming state (`0`) |
| `+0x30` | `float` | Minimum scaled camera distance this frame (updated by `0x353470`, reset to `1e12f` [`0x5368D4A5`] by `0x3534D8`) |
| `+0x34` | `uint32` | High-res `.tm0` asset size (only if `>= 0x3E80`, else `0`; gates `0x353470`) |
| `+0x38` | `uint32` | Streamed high-res GS texture pointer (preferred over `+0x18` in `0x3514F0`) |
| `+0x3C` | `uint32` | Stream handle (`0`) |
| `+0x40` | `uint32` | Bind count this frame (incremented by `0x3514F0`, reset to `0` by `0x3534D8`) |

### Function Ledger (Cluster C)

| Address | Size (bytes / insns) | Name | Callers / Callees | Key Struct / Global Offsets | Status |
| --- | --- | --- | --- | --- | --- |
| `0x3507F8` | `0x280` / 160 | `TextureSlotTableInit` (contains `0x350810`) | Engine init -> `0x12E7D0(0x8800, "texlist")`, `0x352D28`, `0x3A5FD0`, `0x352520`, `0x3549B8` | Allocates `*(0x4D2D30)` (512 × `0x44`), zeroes all 512 slots, sets `*(0x4D2D38) = -2`, builds 32×32 white fallback texture at `0x4D2CE8` -> `*(0x4D2D34)`, `*(0x3DD678) = -1` | Disassembled — Queued |
| `0x350F58` | `0x3C8` / 242 | `TextureSlotAcquire` (contains `0x350FF4`) | Model/material loaders -> `0x354A98`, `0x3A72D8`, `0x352400`/`0x3521E8`, `0x1323E8` (`.tm0`), `0x350C80`, `0x350D78` | Scans 512 slots (4-way unrolled at `0x350FF4`) for matching `(name, flags)` or first free slot; panics `"Out of textures (%s)"`; loads header & `.tm0` size; invalidates `*(0x4D2D38) = -2` | Disassembled — Queued |
| `0x350C80` | `0x94` / 37 | `TextureIsStatic` | `0x350F58` | Checks `"team"` prefix + digit, static name table `0x3DD688`, `"envmap"` exclusion | Disassembled — Queued |
| `0x350D78` | `0x1E0` / 120 | `TextureCreateGsObject` | `0x350F58` -> `0x34F878`, `0x360408` | Computes mip count, fills `0x24`-byte descriptor on `$sp`, tracks static VRAM bytes at `*(0x4CD4AC)` | Disassembled — Queued |
| `0x3514F0` | `0xC8` / 50 | `TextureSlotBind` | `0x34F988`, `0x34C798` -> `0x351410`, `0x34E8C8`, `0x360680` | Caches bound slot in `*(0x4D2D38)`; `-1` binds fallback `*(0x4D2D34)`; else increments `slot+0x40` and binds `slot+0x38 ? slot+0x38 : slot+0x18` | Disassembled — Queued |
| `0x353470` | `0x68` / 26 | `TextureSlotNoteDistance` | `0x34B140`, `0x358BD0` | If `slot != -1 && *(0x3DD6C4) != 0 && slot+0x34 != 0`: `slot+0x30 = min(slot+0x30, *(0x3DD6CC) * max(depth, 1.0f) * batch_scale)` | Decompiled — Ready (`decomp_graphics_env.cpp`) |
| `0x3582B8` | — | `MlistVisibilityCalc` | `0x214B68` | Computes per-sphere / LOD group / model visibility bytes in scratchpad | Verified (`src/native_world.cpp`) |
| `0x358BD0` | — | `WorldRenderPass` | `0x214B68` | Emits DMA REFs for visible world batches of pass `0..4` | Verified (`src/native_world.cpp`) |

---

## Cluster D — Vehicle Visual Submission & Live Damage Dent Pipeline

| Address | Size (bytes / insns) | Name | Callers / Callees | Key Struct / Global Offsets | Status |
| --- | --- | --- | --- | --- | --- |
| `0x2189C8` | `0x270` / 156 | `VisualCarSubmitModel` | `0x218E68`, `0x218C38` -> `0x34F570`, `0x34F5A0`, `0x21C660`, `0x34F760`, `0x34DFE0(0x80)`, `0x34CA18`, `0x34DEC0(0x400)`, `0x34BDA8`, `0x34F710`, `0x34BF78` | Sets per-car light intensity `car+0x558` & ambient `car+0x55C`. Loops damage zones `0..car+0x200` (`car+0x204`, stride `0x0C`: `first_bone`, `end_bone`, `zone_mask`): uploads dent offsets `state+0x29C + 12*first_bone` (`count = end_bone - first_bone`) via `0x34CA18(1, ptr, count)` -> VU1 qword `0x90`, submits loose panels (`state+0xA2C`, stride `0x34`, mask `+0x30`) and remaining chassis via `0x34BDA8(lod_models[car+0x54C], mat, dent_pkt, mask)`. Sets render flag `0x400` when `car+0x548 < 30.0f` (`0x41F00000`). Submits interior/shadow model `car+0xF4` via `0x34BF78` if `!= -1`. | Disassembled — Queued |
| `0x218E68` | `0x6F8` / 446 | `VisualCarBatchSelectAndSubmit` | `0x20F0D8` (wrapped by `src/hle_lod.cpp`) -> `0x34FA98`, `0x34F620`, `0x211318`, `0x2112E8`, `0x34F570`, `0x34F5A0`, `0x218958`, `0x2189C8`, `0x220698`, `0x218C38` | Walks `g_VisualCarList` (`0x3D1C90`, max 20 cars): computes LOD distance `car+0x548` and LOD index `car+0x54C` against `car+0x220` thresholds, scales per-car ambient `car+0x55C` by `car+0x554` when `> 1.0f`, calls `0x218958` + `0x2189C8` + 4 wheels (`car+0x5E8..+0x5F4` -> `0x220698`), then `0x218C38` for player car, restores ambient & light intensity. | Disassembled — Queued (wrapped in `src/hle_lod.cpp`) |
| `0x21A9B0` | `0xDC` / 55 | `VisualCarSubmitReflection` | `0x20F4A0` -> `0x2112E8`, `0x34DFE0(0x80)`, `0x34BDA8`, `0x34FA48`, `0x220718` | Skips player car (`car+0x40 == player_idx`); clamps LOD `max(car+0x54C, car+0x1FC)`; submits body with `material = 0`, mask `state+0x25C`; if camera distance `< 10000.0f` (`0x461C4000`), submits 4 wheels via `0x220718`. | Disassembled — Queued |
| `0x220788` | `0x4A8` / 298 | `WheelSelectSlowOrFastModel` | `0x220698`, `0x220718` (wrapped by `src/hle_nascar_wheels.cpp`) -> `0x39BB28`/`0x39BD68`, `0x34F760`, `0x220F38`, `0x34BDA8`, `0x34F710` | Builds wheel spin/steer 3×3 matrix on `$sp`, scales X column by `*(float*)(wheel_def+0x24)`, selects slow (`+0x00`) vs fast (`+0x04`) mesh from `wheel_def + 0x28 + 0x10*lod` based on `wheel+0x04 & 0x10`, submits via `0x34BDA8`. | Disassembled — Queued (wrapped in `src/hle_nascar_wheels.cpp`) |
| `0x18CA98` | — | `CarApplyCollisionDamage` | `0x189CD0` -> `0x18C6B8` | Primary collision damage entry; finds nearest bones & calls `CarDentBone` (`0x18C6B8`) | Verified (`src/car_damage.cpp`) |
| `0x18C6B8` | `0x3E0` / 248 | `CarDentBone` | `0x18CA98` (recursive up to depth `< 3`) | **Writer of live dent buffer (`car+0x4B8`, stride `0x0C`, seen at `0x005BF288` in race):** scales impulse by `bone+0x2C * 6.854e-6f` (`0x36E5FAD5`), clamps Y `<= 0`, adds to `car+0x4B8 + 12*slot` and clamps to `[bone+0x04..+0x0C, bone+0x10..+0x18]`, then propagates damped impulse across bone spring links (`M+0x2108`, count `M+0x210C`, stride `0x14`) recursively. | Disassembled — Queued |
| `0x34CA18` / `0x35DD28` | — | `DentOffsetUpload` / `DentOffsetWrite` | `0x2189C8` -> `0x35A810` | Uploads `V3-32` dent offsets to VU1 qword `0x90` (`Skin::Offset` programs `0x2E0`/`0xD48`/`0x15A8`) | Verified (`src/bone_pose.cpp`) |

---

## Previously Verified Graphics Subsystems (Reference)

| File | Verified Functions |
| --- | --- |
| `src/render_queue.cpp` | `0x34BDA8`, `0x34B140`, `0x34BF90`, `0x34CC98`, `0x34C9C0`, `0x34C930`, `0x34C860`, `0x34AF98` |
| `src/bone_pose.cpp` | `0x34C9F8`, `0x35DBD8`, `0x35A810`, `0x35DD28`, `0x34CA18` |
| `src/native_world.cpp` | `0x214B68`, `0x3582B8`, `0x358BD0`, `0x35DEE0`, `0x35CBB8`, `0x223858`, `0x223800`, `0x326168`, `0x325728`, `0x325E40` |
| `src/native_fog.cpp` | `0x33ECB8`, `0x3653F8`, `0x365428`, `0x365540` |
| `src/native_shadows_flares.cpp` | `0x34C7D0`, `0x34C798`, `0x34C208`, `0x34C4B0`, `0x35D298`, `0x35D170`, `0x304D50`, `0x20F2D8` |
| `src/native_effects_imm.cpp` | `0x363FF8`, `0x363B98`, `0x3639D0`, `0x363B68`, `0x364548`, `0x363F48`, `0x363DE8`, `0x363F18`, `0x3642B8`, `0x363D78`, `0x363C18`, `0x363D48`, `0x35DF10`, `0x361468`, `0x361708`, `0x361BC8`, `0x364D18`, `0x34EE70`, `0x34EEB8`, `0x361ED8`, `0x361C30`, `0x35CAA0` |
| `src/native_texstate.cpp` | `0x3652A8`, `0x3654E0` |
| `src/car_damage.cpp` | `0x17B918`, `0x18CA98`, `0x18AFE0` |

---

## Wiring Checklist (Next Step — `EOD_DIFF` Verification)

To activate and `EOD_DIFF`-verify all 39 functions in `src/decomp_graphics_env.cpp`:
1. Add `src/decomp_graphics_env.cpp` to `CMakeLists.txt`.
2. In `src/render_trace.cpp`, forward `f_0035B3D0()` to `eod_env_0035B3D0()` (instead of calling `f_0035B3D0_orig()` directly).
3. Add `wrap` entries in `config/hle.txt` for the 38 non-traced addresses:
   `0x221DC8`, `0x2237D8`, `0x2236B0`, `0x2236A8`, `0x34E880`, `0x34E868`, `0x35E3F0`, `0x34E8A8`, `0x35E388`, `0x30AA10`, `0x3053C0`, `0x305350`, `0x221D88`, `0x223678`, `0x223638`, `0x34F5A0`, `0x35E420`, `0x221D48`, `0x2235F8`, `0x2235D8`, `0x30AA00`, `0x221D08`, `0x223548`, `0x223520`, `0x305360`, `0x34F570`, `0x221CC8`, `0x2234E0`, `0x223488`, `0x34E848`, `0x221E00`, `0x223A50`, `0x221B18`, `0x2224C0`, `0x3052F0`, `0x34D8A0`, `0x35E200`, `0x353470` (`0x35B3D0` is already `wrap`).

---

## Next Cluster Queued

1. **Cluster D C++ Decompilation (`src/decomp_vehicle_visual.cpp`):** `0x2189C8` (`VisualCarSubmitModel`), `0x21A9B0` (`VisualCarSubmitReflection`), `0x18C6B8` (`CarDentBone`), and `0x220788` (`WheelSelectSlowOrFastModel`, coordinated with `src/hle_nascar_wheels.cpp`).
2. **Cluster C C++ Decompilation (`src/decomp_texture_slots.cpp`):** `0x3507F8` (`TextureSlotTableInit`), `0x350F58` (`TextureSlotAcquire`), `0x350C80`, `0x350D78`, `0x3514F0` (`TextureSlotBind`), `0x3534D8`.
3. **Cluster E (VU1 Submission Core):** `0x35CD28` (`MatrixPacketBuild`), `0x35BA10` (`EmitCameraMatrixMaterial`), `0x34E308` (`FrustumTestSphere`), `0x224778` (`SunClampMinElevation`).
