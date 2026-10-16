# Native camera aspect hook

The host display aspect and the game's camera must agree. The PS2 game already
has native widescreen projection and associated display handling. `src/hle_camera.cpp`
wraps its display-mode setup rather than transforming rendered GS coordinates.

## Behavior

The wrapper snapshots `g_config.aspect` on its first call. Aspect settings apply
on the next launch, including when the native options menu later requests another
mode. Host 4:3 forces native mode 0; host 16:9 and 16:10 select native mode 1.
Stretch preserves the native requested mode and its original scales. The wrapper
calls the original function in every case, retaining its option, border, and
other register side effects. For wide output it then writes both base scales:

| Host aspect | Base scale |
| --- | --- |
| 4:3 | 1 (native mode 0) |
| 16:9 | 0.75 |
| 16:10 | 5/6 |

The general relation is `(4/3) / outputAspect`. The native 16:9 border policy is
preserved. There is no change to game time, physics, graphics resolution, or
rendering frequency. This hook is shared across host platforms; builds without a
16:10 config option still support their existing 4:3/16:9/Stretch selections.

## Verified target evidence

The supported 3,368,996-byte `SLUS_209.10` has ordinary CRC32 `75BB93EE`. Applying
PCSX2's documented ELF word-XOR checksum gives `5D0244D3` for that same file;
these are different checksums, not two different revisions. The
[official PCSX2 patch](https://github.com/PCSX2/pcsx2_patches/blob/main/patches/SLUS-20910_5D0244D3.pnach)
uses this same scale address. PCSX2's
[ELF checksum implementation](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/Elfheader.cpp)
explains the identifier difference.

Local disassembly (`.venv/bin/python tools/dis.py -f ADDRESS`) establishes:

- `0034D328`: native `SetDisplayMode`. Writes mode to `003DD660`; initializes
  `003DD658` and `003DD65C` to 1. Mode 1 sets both to 0.75 and the widescreen
  border flag `003DD64C` to 1. Mode 2 is letterbox. Saves `display_type` through
  the game's options code when available.
- `0034E530`: projection setup. At `0034E5C4`–`0034E5E0` its FOV adjustment is
  `2 * atan(tan(inputFov / 2) / xScale)`. It also consults both base scales when
  deriving the projection. For a 67.5 degree input, this first adjustment gives
  approximately 83.396 degrees at 16:9 and 77.446 at 16:10.
- `003634EC`–`00363520`: the enclosing `00363218` setup function saves the old
  x-scale, derives `oldScale * (baseWidth/baseHeight) / (targetWidth/targetHeight)`,
  and writes it using setter `0034F488`. Later calls restore the saved value.
  However, this function computes power-of-two surface dimensions, and its only
  discovered direct caller is `0035DA7C` in `0035DA30`, which clamps dimensions
  to 320×512. This is evidence of temporary render-target aspect handling, likely
  render-to-texture, **not proof of multiplayer split-screen camera setup**.
  The hook leaves these writes untouched; actual multiplayer projection still
  requires separate scene evidence.
- `0034F478`/`0034F498`: scale accessors. Native display/render-target code consumes
  these scales too. Visual HUD behavior remains subject to in-game verification.
- `0032EC58`: boot console-widescreen query; hooking only this query would miss
  subsequent saved-option/display-mode calls (`0034FCDC`, `002A39E4`).

The PCSX2 patch also disables generic menu increment/decrement stores at
`002A419C` and `002A41C0`. These are not aspect-specific functions in this ELF;
this port does not apply those NOPs. Its optional border-removal patch warns of
menu-edge artifacts, so border removal is also omitted.

## Validation and limits

`NativeCamera-*` CTest cases call the production wrapper using a stubbed original
native function and real EE RAM/register accessors. They check mode selection,
both scale writes, original side effects, preservation of Stretch mode changes,
and caching across later config mutations. The pass-through wrapper was first
shown to fail the wide-mode test before implementation. No retail game runs or
visible windows were used for this change.

Regenerate recompiled sources after changing `config/hle.txt`:

```sh
.venv/bin/python tools/recomp.py
cmake -S . -B build-mac
cmake --build build-mac --target eod_native_camera_test -j2
ctest --test-dir build-mac -R NativeCamera --output-on-failure
```

Parent integration must still compare matched single- and two-player scenes at
4:3/16:9/16:10, with aspect-correct display composition, to verify framing, HUD
proportions, borders, and menu coverage. Source and CPU tests establish the hook
mechanism; they are not proof that every game screen is visually correct.
