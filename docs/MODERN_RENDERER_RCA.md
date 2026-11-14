# Modern renderer on Vulkan: root causes of the visual bugs and the lag

Status (2026-11-14): measured on Windows, Radeon Pro 580, i5-7600K (4 cores), headless,
muted. Woodly (the `track=Afton` direct start resolves to Eve 16), Anoka at night
(Battle, 8 AI) and Career free roam (Time Challenge gate 1, driven with a pad script
through jumps and roll-overs).

## Tools

| Tool | What it answers |
|---|---|
| Tracy (CMake `-DEOD_TRACY=ON`, v0.11.1, `runtime/core/trace.h`) | CPU zones on the EE, VU1 producer and GS threads; Vulkan GPU zones for the modern passes. `tracy-capture` + `tracy-csvexport -e` give self time per zone |
| GPU timer (`EOD_FPS` or `EOD_GPU_PROF`, `vk_compat.h`) | timestamp pairs per command buffer and per modern pass, read when the flight retires; GPU busy share of its own timeline; draws, descriptor sets, render passes, barriers and submits per frame; cycle cost of each section of `Context::Draw` |
| GS dump replay (`EOD_GS_DUMP_AT`, `--gs-replay --repeat`) | deterministic CPU cost of a GS backend on identical commands |
| `EOD_MODERN_DEBUG=1` | shadow factor (R), N.L (G), world path (B) |
| `EOD_MODERN_LOGPROGS=<vblank>` | every VU1 program run of one frame and whether the modern pass replaced it |
| `EOD_MODERN_LOGKEYS=1` | GS blend / texture / alpha-test state of each replaced program |
| `EOD_VU_STATS=1` | VU programs running interpreted (none in a race) |

Tracy 0.11.1's `tracy-csvexport` does not export GPU zones; the built-in GPU timer covers
that. RenderDoc (frame capture) and the Khronos validation layers with synchronization
validation are the next tools for draw-level correctness; Radeon GPU Profiler support for
Polaris is uncertain.

Measurement caution: a stray background process held one of the four cores for most of
the session and the GPU heat-soaks after long runs (the same Direct3D 11 run fell from 44
to 12 drawn frames/s). Compare only runs taken back to back; prefer the GS replay and GPU
timestamps over live frame rates.

## Fishbone

```
Visual bugs                                     Lag
├─ Data: sky decided by program, not matrix     ├─ Vulkan GS shim: per-draw CPU (68% over D3D11)
├─ Method: cascades sized by object median      ├─ GS: alpha-test split, ~20k draws/frame (modern off)
├─ Method: AO radius by object median           ├─ EE/VU1: compiled VU ~50% of the EE (baseline)
├─ Material: night lit as day                   ├─ Producer: emitter hashing per VU1 run
└─ Tone: ACES over baked vertex light (washed)  └─ GPU: 4x MSAA + scene + post at 6x
```

## Visual bugs: 7 whys (verified)

**Torn triangle shards, sliced pit walls, no shadows on the ground in Career (user
screenshots 8, 10).**
1. Why shards? GS-drawn and modern-drawn terrain interleave with unrelated depth.
2. Why is some terrain GS-drawn? Program 0x270 in ITOP mode 0x280 is never replaced.
3. Why? The emitter treated (0x270, 0x280) as "the sky dome, left to the GS".
4. Why was that wrong? It holds on Woodly (13 sky runs) but in free roam the same program
   and mode draw most of the ground (209 runs in one frame, `EOD_MODERN_LOGPROGS`).
5. Why was the rule by program? The capture document identified the sky from one race frame.
6. Why did nothing catch it? Tests ran on one race track.
7. Fix: the sky is the run(s) of 0x270/0x280 that share the first such run's matrix
   (camera translation) before any world geometry; every other run is world geometry and
   is replaced. Verified: the whole free-roam ground is modern-drawn and receives shadows.

**Blocky, washed-out or missing shadows (user screenshot 4).**
1. Why? The near cascade covered hundreds of units in free roam.
2. Why? Cascade half-size = median object distance x 1.2 x 0.35.
3. Why does that vary? The median was 32 units on Woodly and 574 in Career free roam
   (distant houses and traffic).
4. Fix: camera-relative cascades (near 21.6 units, far 162) and shadow casters culled per
   cascade by bounding sphere. The AO radius had the same dependency and is now fixed.

**Night tracks lit like day.** The game gives the same light direction and ambient (q17)
at night; only the fog colour changes (black at Anoka, 0.79/0.79/0.92 on Woodly).
Daylight now comes from the fog luminance and scales the sun, ambient, reflections and
the sun-shadow strength.

**Washed-out ground.** World geometry carries the game's baked vertex light; the pass
multiplied it by 1.35 and the ACES curve lifts low values ~1.45x. Sunlit world geometry
now lands at the original's brightness, shadowed ground keeps 43%.

**Batches with no GS state** (key `~0u`: the GS never drew them, e.g. a projected shadow
quad that samples the off-screen shadow buffer) are no longer drawn with default state.

False hypotheses (kept for the next investigator):
- "The white streak is the game's blob-shadow composite drawn white": no, it is the
  game's own dust/tyre-trail decal (program 0x2a8, ordinary alpha blend), present in the
  original too; it read white because the ground was over-exposed.
- "Shadows are broken on Vulkan": no; in the Woodly test frame the sun is behind the
  camera, so every shadow falls behind its caster. The shadow pass works.
- "The PCSS blocker search fails on linear-filtered D32": no; hard shadows behaved the same.

## Lag: measured

| Measure | Result |
|---|---|
| GS replay, 96 race frames, 1x | Direct3D 11 1067 ms (11.1 ms/frame), Vulkan 1789 ms (18.6 ms/frame); same at 6x |
| Vulkan, modern off, per frame | 14,000-23,000 draws, ~90 descriptor sets, 21-27 render passes, 28-50 full barriers |
| `Context::Draw` per frame | pipeline lookup ~1 ms, render pass + dynamic state ~2 ms, descriptors + constants ~0.9 ms, vertex upload + vkCmdDraw ~3.3 ms |
| Why 20k draws | `flush()` splits alpha-tested batches into tile-disjoint groups and draws each twice (pass, fail); strip neighbours overlap, so most groups are one triangle |
| Modern on, per frame | GS draws drop to ~25-40 (the replaced 3D is gone); the GS thread then spends its time recording the modern pass and waiting for the GPU |
| GPU at 6x, modern on | modern scene 5.4-6.7 ms, post 2.8-3.7 ms, shadows 0.5-0.7 ms, GS 4-6 ms (before the heat soak) |
| EE | compiled VU1 programs ~50% of the EE in every mode; no program runs interpreted |

## Next decisive work

1. Vulkan GS shim: cut per-draw cost (pipeline lookup by state pointers, fewer pipeline
   switches in the pass/fail pairs) and the 28-50 full barriers per frame; target the
   Direct3D 11 replay time.
2. GS per-vertex fog for modern world geometry (the original fades to the fog colour per
   vertex; the pass uses a distance haze).
3. MSAA off automatically when the render scale already supersamples the output.
4. Particles, fire and sparks rebuilt in the modern pass (see the effects audit in
   `docs/SCENE_CAPTURE.md`).
