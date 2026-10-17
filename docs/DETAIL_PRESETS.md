# Optional detail presets — October 17 continuation

## Player success and scope

Original must preserve the game's image, memory changes and native settings.
High should delay car simplification; Maximum should retain the highest permitted
loaded car model throughout the supported visible race range, including split
screen. Maximum's optional 2P projection/fog range restoration must show complete
geometry without new holes and retain playable native 1x speed; 4x is a separate
performance gate. This does not promise new assets, infinite terrain, persistent
debris, or unchanged performance on every track/hardware.

## Situation and recovered evidence

The stopped Claude job left investigation only. Its saved 2P trace (`work/t2p`)
shows far=575 with configured draw distance1150 and native selected car indexes
1..4. Function00211318 indexes `{1,1,2,2}` using view-count minus1, so the car
LOD multiplier is the same for1P and2P, not doubled in2P. Camera builder00212B58
divides configured distance by view-count and derives both projection and fog
from that quotient. These are distinct causes of detail/range reduction.

Native minimum index is1 for the observed cars. Index0 is not their highest
complete playable car; force-LOD0 would lose geometry/parts. The existing loader
hook already validates table ownership, levels, minimum, model handles and
ordered finite thresholds, respecting clone sharing and native register effects.

## Implementation and research

Reuse the existing `EOD_CAR_LOD_MULTIPLIER` mechanism, also published as
[bf6a575](https://github.com/catsandsoup/eod-recomp/commit/bf6a575) on the optional
`upstream/car-lod-multiplier` branch. A numeric environment override takes
precedence over the whole Detail preset, including range, allowing exact Original
comparison via value1. No change is posted to the upstream repository.

`[Video] Detail=original|high|maximum` defaults to Original and requires restart.
High multiplies positive car thresholds by3. Maximum scales the first native
permitted threshold to at least1,000,000 scaled distance units (>800x the largest
native far plane1150), preserving ordered finite distances and native minimum,
count, handle arrays, flags, wheel/damage logic and shadow tables. Invalid tables
remain unchanged. This retains the highest permitted model for normal finite
visible race distances; it does not bypass culling or force a numeric model0.

Maximum also scopes a2x configured-distance input only around native camera
builder00212B58 when view-count is exactly2 and the input is finite in(0,1150].
Native projection and fog both receive the original1P range. The source distance
word is then restored exactly, preserving guest registers and unrelated callers.
Cinematic overrides and unsupported view counts remain native. No global view
count is forged, no frustum test is bypassed, and no streaming pool is expanded.

Rechecked RoadTripAdventure-recomp's
[overrides.cpp](https://github.com/silentsudin/RoadTripAdventure-recomp/blob/0f6d3613e8e32ccda5305227a578b1fb659983a8/src/game/overrides.cpp): its camera/frustum changes
are scoped to verified consumers. Its runtime ABI and game addresses differ, and
it has no transferable generic EOD car-LOD implementation. Follow the scoped
consumer pattern using the existing EOD hook and native camera/fog calculation.

Debris persistence and extra1P-only visual passes are excluded: the prior trace
found a per-view12.5/count budget and fixed pool initialization around00310000,
not a proven safe lifetime-only switch. A1P-only call at00218D24 also has
unresolved semantics. Changing global view count to unlock these would affect
simulation/resource limits. Persistent debris needs pool ownership/recycling,
collision, lifetime and exhaustion evidence first. Existing debris/physics,
texture promotion and track traversal remain native.

## Validation

Results follow the bounded, headless/muted build and game gates; no physical
hardware/player acceptance is implied by CPU contracts or one scripted scene.

- Release build and signed Mac app succeeded. All 47 CTest cases pass, including
  eight LOD runtime modes, full guest-memory Original identity, native minimum
  boundaries, invalid thresholds and camera range/view-count boundaries.
- Maximum completed the scripted 6600-vblank two-player race at both 1x and 4x,
  headless and muted. Native 640x448 race windows: median59.9, minimum59.3 FPS.
  At4x (2560x1792 internal), last six five-second race windows: median58.55,
  minimum49.3 FPS. Loading/countdown also dips into the low50s. Maximum therefore
  does NOT meet a locked60FPS 4x gate; it remains optional and Original remains
  the default. These are window averages, not instantaneous minimums.
- Native Maximum trace:640 model-selection samples retained native minimum1,
  visible distances through431 units, far plane1150;20 valid car tables loaded.
  Both players take distinct scripted paths. Reviewed captures show complete
  nearby cars/damage and more complete distant cars; no obvious holes or fog
  seams in this bounded scene. Other tracks/physical play remain unverified.
- Replaying the identical baseline GS capture on unmodified8e74d28 and candidate
  produced byte-identical4MiB VRAM and2560x1792 screenshots. Separate live
  Original runs did not produce identical hashes; full live-run byte identity
  is not claimed. CPU contracts prove the disabled hook's memory behavior;
  same-capture replay establishes renderer parity, not game-hook execution.
- Evidence: work/lod-continuation/{ctest-final.log,maximum-1,maximum-4,
  replay-results.json,parity-original/final.ppm}. Comparison images in the
  recovered Claude notes are actual game captures at the same scripted moment,
  with minor separate-run timing variation possible. No5x/6x tests were run:
  the user's final instruction limited validation to their4x setting.
