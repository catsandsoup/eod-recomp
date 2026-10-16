# LOD, distance and visibility investigation

Static investigation, 2026-10-16. No gameplay changes, builds, game runs or GPU
measurements were made for this document. Addresses refer to the supported
`SLUS_209.10` (3,368,996 bytes, ordinary CRC32 `75BB93EE`). Evidence is local
retail ELF disassembly, generated recompilation and the owner's PS2 disc read
in place. This is a bounded first-hook investigation, not a completed streaming
or visibility audit.

## Outcome and scope

A concrete first implementation target exists: an opt-in **car LOD threshold
multiplier after native car loading at `0021ABF8`**, preceded by a read-only
trace of the loaded threshold/model arrays. The engine already loads each car's
available model levels during initialization; its visual selectors compare
camera distance with an array of thresholds. This is more precise than
increasing an option value or disabling visibility tests. Preserve the original
mode as the default and leave simulation, physics, AI and timing untouched.

Treat car LOD retention, track submission range, projection/fog distance and
frustum edge correctness as separate settings/investigations. This pass supports
improving car model retention; it does **not** establish unlimited track distance
or zero pop-in. Highest available detail means an existing valid game asset,
not extra polygons or texels synthesized by rendering at 4K/8K.

The quality ceiling must accommodate much faster future Macs and PCs, including
RTX 5090 class hardware at 8K, rather than stop at the owner's M2 Pro. Keep an
efficient playable Mac preset and allow explicitly expensive Maximum/Extreme
presets. Profile EE CPU selection/submission, guest memory/residency, texture
loading and GPU raster cost separately. Extra GPU throughput and upscaling
cannot remove CPU submission or absent/residency limitations. DLSS is explicitly the very last priority per the owner; no DLSS investigation
or Windows implementation is part of this work.

## The original draw-distance option is not an unbounded multiplier

Disassemble with `.venv/bin/python tools/dis.py -f 00212808`.

`00212808` defaults the option to 2, reads `draw_distance`, initializes a float
factor to zero, and branches only for options 0, 1 and 2:

| Raw option | Intermediate factor | Final float at `00495D58` |
| --- | --- | --- |
| 0 | 0.5 | 1150 |
| 1 | 0.25 | 725 |
| 2 (default) | 0 | 300 |
| Other integer | 0 | 300 |

The final expression is `1700 * factor + 300` (`002128C8`–`00212900`). These
are engine world-distance values; this pass does not assert metres. Raising the
integer past 2 cannot extend the range. This corrects the historical suggestion
in MODDING_SCOPE §1.3. The option ordering also must not be inferred from its
integer magnitude.

The setup calls native projection `0034E530` with near=1, far=`00495D58` and
FOV approximately 0.654498 radians (`00212930`–`00212948`). Initialization at
`002127B0` separately uses near=1, far=1000, FOV=67.5 degrees. Actual race calls
can replace these values; the camera trace documented in NATIVE_CAMERA_ASPECT
already observes other FOVs.

`00212B58` reads the same distance, divides it by the result of `00211378`
clamped to [1,4], and passes the quotient to projection at `00212C04`.
`00211378` reads `003D1A74`, a view/player-count-related global also used by
car LOD scaling. The fog-related call `0033ECB8` receives distance-dependent
arguments at `00212C34`. Thus a far-plane-only experiment can leave fog or other
range consumers inconsistent. Two views can use a reduced native distance;
verify actual values per view rather than assuming one-player settings carry
unchanged into split screen.

`00212DB0` and `00212DC0` are identical getters for `00495D58`. The simple
`tools/dis.py -x` JAL search finds no callers; this is **not** proof that there
are no tail-call/indirect/data references. A write after setup is a candidate
range experiment only after logging later projection/fog updates and other
scene limits. Do not patch a getter and assume the whole engine changed.

## Asset availability and the distinction between disk and live residency

A fresh in-place directory scan of all 37 `DATA/*/*.CAR` archives found:

- 31 contain `Low0.mod` through `Low4.mod`.
- Five contain `Low0.mod` through `Low3.mod`.
- One contains `Low0.mod` through `Low6.mod`.
- Every one of these Low model entries lies before the archive's extractable
  boundary (`dir_offset + header[0x1C]`), in its resident prefix.
- No `lod_hi.val` entry occurs in the scanned CAR/TRK/RES archives.
- All 15 GRAF track scene entries also lie in resident prefixes. Their sizes
  range from the 1,788-byte empty track to 3,993,276 bytes for Homebase;
  Afton's GRAF is 3,435,712 bytes.
- The archive scan finds 1,547 `.tm0` texture variants. Unlike the Low models,
  these belong to the known extractable/on-demand texture path.

These facts establish on-disc availability and archive grouping, **not** that
all tracks/cars stay live simultaneously. Resource sets, model instances and
texture budgets can still be released or bounded at runtime. The recomp retains
the original guest memory model; faster host hardware alone does not expand
its allocators. No evidence here supports streaming an arbitrarily large world.

For a concrete example, `ALGAR.CAR` contains Low0/1/2/3/4 sizes of
14,516 / 161,380 / 155,348 / 36,084 / 10,820 bytes. Do not force index zero on
the assumption it is the highest-detail playable car. Its `lod.val` says:

```
low:     lod0="0 n", lod1="10 y", lod2="15 y", lod3="30 y", lod4="500 y"
detail5: min=1, mirror=3, scale=1.0, use=low
shadow:  lod0=0, lod1=10, lod2=70, model=shadow
```

Whitespace in the retail values contains tabs. Other vehicles have exceptions
(e.g. bus 12/17/30/500, some 40 instead of 30, trailers with fewer levels or
1010). All 37 have a `detail5` table with min=1 and scale=1.0; mirror/shadow
fields differ. Use the loaded table per vehicle, never one hard-coded global
array or a presumed five-level count. Packet byte sizes are not polygon counts
and do not establish which visual elements each model contains.

The loader `0021ABF8` checks `high_detail_car` through `00212DF0`, probes
`%s.car/lod_hi.val`, and falls back to `%s.car/lod.val` when the high table
is absent (`0021AC40`–`0021AC98`). This option cannot conjure absent models.
The loader selects an available `detailN` table from `detail_level`, resolves
its `use` table, reads min/mirror, and parses each `lodN` with `%f %s`
(`0021B014`–`0021B098`). The `y` flag becomes a separate record field; changing
it is outside the proposed LOD-distance experiment.

The loader iterates all parsed levels and calls model loader `0034CEF0` for
`%s%d.mod` at `0021B640`–`0021B6F0`, storing per-level model handles. This is
positive static evidence for initializing the complete selected model chain,
rather than loading only the currently visible mesh on every transition.
Live handle/allocator traces are still needed. Texture helper `003545B8`
constructs a `.tm0` name, loads it via `001323E8`, checks TEX version 10 and
feeds `003537F0`; texture promotion/eviction timing has not been traced.

## Actual LOD selection and submission

The visual car object passed as a0 to `0021ABF8` has these observed fields:

| Offset | Evidence-backed role |
| --- | --- |
| +0x1F8 | Parsed LOD count |
| +0x220 | Pointer to records, stride 16 bytes |
| record+0 | Float threshold |
| record+4 | Parsed `y` flag |
| record+8 / +12 | Model-part capability flags from loaded models |
| +0x74 | Per-level model handle array (indexed with stride 4) |
| +0x548 | Camera distance multiplied by view/FOV factors |
| +0x54C | Selected LOD index |
| +0x550 | Minimum permitted index from selected detail table |

`0021C6E0` performs a single-object selection. `00218E68` begins with an
equivalent loop over the car pointers at `003D1C90`, up to 20 objects. Both:

1. Compute Euclidean camera distance with `0034FA98` from object+0x28.
2. Multiply by the FOV compensation returned by `0034F620` and the count-related
   factor from `00211318`. The four retail factors at `004062A8` are 1,1,2,2.
3. Start with the last available index, choose the first record whose threshold
   is strictly greater than scaled distance, then clamp to the minimum and count.
4. Store the result at object+0x54C.

For Algar this retains index1 until scaled distance reaches10, then index2
until15, index3 until30 and index4 afterward. This is a model-quality switch,
separate from the 300/725/1150 projection range. Wider FOV also affects the
LOD distance compensation; merely extending the far plane does not retain
higher model detail.

Submission consumes the selection: `002189C8` indexes a per-level handle array
at +0x21C with +0x54C and submits through `0034BDA8` at `00218AEC`/`00218B10`.
The companion model arrays are initialized by the loader. `00218C38` also reads
selected-record flags for wheel/part rendering. Hence model identity, wheels,
damage and mirrors must be checked together; forcing a numeric index without
respecting these arrays would be unsafe.

## Frustum coupling and wide edges

Native `0034E530` changes the half-FOV tangent using xScale at `003DD658`,
then stores near/far at `004CD370`/`004CD374`, tan/cot half-FOV coefficients at
`004CD378`/`004CD37C` and additional sphere-test coefficients at
`004CD380`/`004CD384`. It also updates native packet projection through
`0035E120`. The matrix compensation already documented in
NATIVE_CAMERA_ASPECT remains relevant; GS output aspect alone is insufficient.

`0034E308` is a concrete **CPU sphere-versus-view test**, taking centre a0
and radius f12, with optional classification/distance outputs a1/a2. It subtracts
camera position `004CD31C`, dots with the camera basis, rejects before near or
after far including radius, then tests absolute side-axis components against a
bound derived from depth*`004CD378` plus radius*`004CD380`. It uses the same
broad bound for both side axes and additional conservative inside-classification
constants. Do not rewrite it as an assumed canonical six-plane test without
establishing the engine coordinate conventions.

There is direct scene-submission evidence: the call at `0034C0EC` is followed
by a zero-result branch at `0034C0F4` that skips the subsequent submission work.
`0034CCD4` likewise calls this test and gates `0034B140`; wrappers `0034F630`
and `0034F650` expose the test to other callers. Thus this is an appropriate
read-only visibility diagnostic hook. The CPU test consumes the same widened
FOV coefficient generated by native camera setup; there is no evidence that
it is simply stuck at a fixed 4:3 constant. It remains necessary to inspect
track traversal, additional rejection layers and VU clipping for edge loss.

**No culling bypass is justified.** Current 16:10 scene evidence supports
proportions/Hor+ but is not an ultrawide edge-visibility test. Per-view projection
state and temporary targets must be logged separately. Extending output aspect
requires the native scale relation `(4/3)/outputAspect`, correct presentation,
and these actual CPU/VU/scene tests to agree. Current support for named host
aspect modes does not itself implement arbitrary ultrawide output.

## Next implementation target and acceptance

First add bounded, opt-in read-only wrappers (preserve all native side effects):
`0021ABF8` records per-car thresholds/min/count and loaded handles after return;
`0021C6E0` records distance/index; `0034E308` samples input/output and current
near/far/FOV/camera state. Batch selector `00218E68` also needs before/after
sampling because it selects and submits within one call; a post-return LOD
forcing wrapper there would be too late. Reuse the existing projection trace.
No production behavior hook was added in this pass.

The first behavioral experiment should multiply **only positive car thresholds**
at the end of `0021ABF8` by a finite, validated setting >=1 (1 preserves the
baseline). Retain zero, all flags, model handles, count, min, shadow tables and
original physics data. This single load-time hook feeds both selectors and
avoids duplicated per-frame patches. A large multiplier keeps the highest
permitted model longer; do not use infinity or overwrite all thresholds with
one value. This is a supported candidate, not a verified safe implementation:
confirm array ownership/lifetime, float bounds and all handle entries in trace
before enabling it. A finite distance profile can subsequently set the first
permitted threshold beyond the intended draw range while preserving ordering.

Validate faithful/multiplier1 identity, distant opponents moving both ways across
thresholds, damage/detached panels/wheels, mirrors, trailers, one/two views and
4:3/16:10/16:9/ultrawide edge sweeps. Sample selected indexes and handles alongside
images, EE frame time, submission/primitive counts, texture I/O and guest/host
memory. Raise scene/projection/fog distance in a **separate** experiment after
tracing its other limits. Check wider scene bounds before judging absence as
frustum failure. Maximum presets may exceed full-speed performance on the M2
Pro; report that honestly without lowering simulation load or quality silently.

The investigation stops here because a concrete asset-backed load-time hook
and actual CPU visibility consumer are established. Outstanding work is live
residency/allocator verification, track traversal/range limits, texture
promotion, complete per-view/VU edge testing and measured performance. Literal
zero pop-in remains unproven.
