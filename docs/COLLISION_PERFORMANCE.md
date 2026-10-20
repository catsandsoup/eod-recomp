# Collision, debris and effects performance — 2026-10-20

## Player success

Normal racing, collisions, detached parts, chicken explosions, sparks and dust
must retain smooth original motion without reducing physics/effects or hiding
stalls through frame skipping. Establish native 1x/4:3/FrameSkip=0 first, then
measure the player's higher-resolution workload separately. A 100% clock label
is not proof of completed frame cadence. Step 1 remains incomplete until the
collision/effects workload and visible play satisfy that outcome.

## Findings

The window title computes speed from delivered host vblanks divided by 59.94;
its FPS field uses completed framebuffer flips. The reported discrepancy is
therefore possible by construction. Neither counter is an authoritative physics
update or actual display-scanout measurement.

Exclusive headless/muted Apple M2 Pro runs used isolated configuration/memory
cards, native assets, no mod overlay, unchanged physics, and frame skipping off.
The ordinary 1x two-player Jump Race reached 6,600 vblanks in 110.280 seconds;
late five-second drawn samples were 59.5–59.9 FPS (the countdown window 57.9).
At 4x, the same input route reached 6,600 vblanks in 110.262 seconds, with sampled
49.9/52.3 FPS windows while host vblank remained near 60. CPU sampling and
screenshots add overhead; these are reproductions, not clean matched speedup
claims or proof of worst-case gameplay.

A native solo 14-car normal race with ChickenFight enabled generated actual
chicken blasts, heavy visible dust and sampled 52.5/54.2 FPS windows at 4x.
A second route supplied short 4x captures at vblanks 4,800, 5,100 and 5,400.
The attempted menu input did not establish Demo Derby selection: inspected HUD
shows a normal three-lap race. Directory labels containing derby/figure8 are
scratch names and must not be treated as event evidence. Later portions drove
off the valid course and emitted terrain-placement failures; they are not a
stable complete-match or collision-physics acceptance workload.

The renderer's CPU sample spends substantial time in texture-content hashing.
Assembly inspection places the hot samples in hash_level's serial multiply
chain over every 64-bit word of each 256-byte GS block. High-resolution work also
has expensive fragment intervals and submission/completion dependencies.
A 12-second Metal System Trace of repeated heavy 4x replay, filtered to the exact
launched game PID (shown as env after exec), reports 4,760 ms GPU busy over
9,829 ms. Fragment intervals total 4,347 ms; their 99th percentile is 5,899 us.
This does **not** demonstrate continuously saturated hardware GPU utilization.
It points to a mixed render/encoding/synchronization critical path, including
costly fragment bursts; CPU GS-worker occupancy must not be called GPU usage.

## Retained change and validation

The Vulkan texture key now uses upstream xxHash XXH3_64bits for each unchanged
256-byte input block, then the existing ordered level combine. Every byte read,
page-generation check, ownership decision, deferred read and texture decode is
unchanged. EOD_TEXTURE_HASH=0 restores the original serial implementation in the
same binary. No draw, effect, car, collision calculation or quality setting is
removed. Cache keys are internal and are not persistent file formats.

The dependency is header-only, pinned to xxHash v0.8.3 commit
[e626a72bc2321cd320e953a0ccf1584cad60f363](https://github.com/Cyan4973/xxHash/tree/e626a72bc2321cd320e953a0ccf1584cad60f363),
with a SHA-256-verified archive; its BSD license is included in Mac bundles.
[PCSX2 GSXXH](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/GS/GSXXH.h)
provides the existing texture-hash reuse precedent. The Road Trip/paraLLEl-GS
path was rechecked but its outstanding EOD parity and integration gaps make it
unsuitable as a quick production replacement; see GS_COLD_PACKET_REPLAY.md.

Alternating same-binary replay runs, four rounds and 32 warm samples per mode,
measured this candidate on the captured 5,100 interval:

| Internal scale | Original hash median | XXH3 median | Interpretation |
| --- | ---: | ---: | --- |
| 1x | 23.85 ms | 20.90 ms | 12.4% less wall time; 1.14x throughput |
| 4x | 36.40 ms | 35.85 ms | 1.5% less wall time; too small to resolve the reported issue |

These are fixed GS-render replay timings, not live FPS or physics benchmarks.
The pre-change performance gate failed, then the candidate exceeded its 10%
CPU-limited throughput threshold. The gate is local diagnostic evidence, not a
hardware-dependent timing assertion added to portable CTest.

Two new CTests reuse the existing procedural renderer oracle, toggling only the
hash path at 1x and 6x, comparing complete 4-MiB VRAM, displayed pixels and draw
counts across texture, mip, alpha and rendered-source cases. Broader tests cover
CPU/GPU transfer ordering. Retail capture/image data remain local and ignored.

Final verification: 61/61 EOD CTests passed in 45.77 seconds (NascarImport
excluded while paused). Nine additional retail gates, three captured heavy
intervals at each of 1x/4x/6x, produced identical displayed images and complete
4-MiB VRAM between hash modes. After removal of the rejected batching prototype,
the original AlphaPassTiles target was rebuilt and its focused test passed.
The signed app was rebuilt with the xxHash license included.

## Reproduction

For the existing native two-player baseline, create a separate INI containing
Renderer=vulkan, RenderScale=1 (then 4), AspectRatio=4:3 and FrameSkip=0 under
[Video], with native enhancements and cheats off. Set EOD_TEST_CONFIG to that
file, EOD_TIMING_TRACE=1 and run tools/test_2p_race.sh with the installed disc
folder and a new ignored work output. It supplies the normal controller route,
private memory card, headless/muted settings, FPS logs and captures. Run one game
or replay at a time; exclude loading, screenshot and sampling windows from
matched timing claims.

Use the existing --gs-replay FILE --repeat N with the same config for fixed
render-only comparisons; EOD_TEXTURE_HASH=0/1 changes only the hash implementation.
Discard warm-up iterations, alternate modes and record all samples. Ordinary
CTest deliberately avoids hardware-dependent timing thresholds.

## Rejected candidate and next decisive work

A second experiment grouped depth-ordered alpha-fail primitives. Independent
review required an actual-fragment depth enclosure, so varying-Z interpolation
was excluded. CPU differential tests passed, and one heavy capture dropped from
24,442 to 22,240 draw calls, but 4x median time worsened from 34.3 to 36.7 ms.
The entire candidate and its scaffolding were removed; its patch remains ignored
research evidence only. Fewer draw calls alone is not a performance result.

Next: capture a controlled, repeatable on-course explosion/figure-eight or
Detention pile-up, correlate per-frame critical-path GPU/encoding/fence intervals,
and reduce the measured render-pass/fragment cost with full image/VRAM parity.
Then repeat live solo/local multiplayer at native scale and the player's scale,
followed by visible input/audio/physical-controller and Career save/resume checks.
Do not declare step 1 or high-resolution carnage fixed from this checkpoint.

## STAR handoff

**Situation:** the player sees FPS drops during crashes and effects despite a
100% speed label, especially at the Mac's high resolution.

**Task:** reproduce, separate clocks from completed images, retain fidelity, and
publish only measured, reviewable improvements.

**Action:** native/high-resolution live probes, fixed GS captures, CPU and Metal
profiles, pinned xxHash reuse, differential cache tests, and rejection of a slower
batching experiment. Work stayed within step 1; NASCAR remained paused.

**Result:** texture-hash CPU overhead is reduced. The main high-resolution
collision/effects acceptance remains open; the measured 4x benefit is small.
