# Mod capability acceptance — 2026-10-20

## Player success

An added asset must appear correctly in selection, load with its own identity,
render correctly at each supported distance, behave correctly in play, survive
normal mode transitions and save/reload where relevant, and preserve the native
baseline when the overlay is disabled. More participants must complete a match
with correct scoring and lifecycle, not merely avoid a startup crash.

The owner supplied this checklist during recovery. It is an acceptance backlog,
not a statement that every capability is implemented. NASCAR remains paused.

| Capability | Evidence/status | Next decisive acceptance |
| --- | --- | --- |
| Additional native-chassis car | Package, roster, AI load and bounded selected-player race verified | Career buy/save/reload and complete race |
| Expanded Career/Arcade roster | One additional identity registered | Multiple added cars, scrolling, capacity boundaries and persistent ownership |
| Custom body and menu textures | Native GS oracle plus rendered menu/player livery verified | Other artwork, all LOD distances and modes |
| Arbitrary custom car mesh | Unsupported; current MODL OBJ is bounds only | Actual mesh decode, encode, render and collision/attachment validation |
| Custom performance | Native parameter edits and runtime mass load | Handling, damage, upgrades and AI comparisons |
| Career description | Native buycar.val desc field written and regression checked | Read description in dealer UI, purchase and reload |
| Custom AI by mode | Registration and a bounded AI resource load | Career, Arcade, 2P and Dare journeys with custom AI participation |
| Higher quality/extended LOD | Existing native LOD textures rebound; new geometry unsupported | Authored LOD meshes, transition and performance evidence |
| Fully supported custom driver | Driver records/numbers supported | Portrait, speech, selection and all-mode lifecycle |
| Arbitrary custom track geometry | Unsupported | Geometry, collision, terrain/material and streaming compiler |
| Racing lines/splines | Native ILIN inspection and roundtrip | Edited line with native-quality AI navigation and recovery |
| Native track modifications | Event/layout edits and selected dynamic-reference removal | Targeted rendered acceptance, including drivable ground/collision |
| Compatible-game track conversion | Research candidate only; paused | Proven per-format geometry, collision, materials, LOD and gameplay conversion |
| Native gates/checkpoints/grids/finish/spawns | Existing native contracts retained; Welch Battle spawn samples verified | Every authored/imported layout and complete event lifecycle |
| Universal pretested modes, Battle first | Fourteen-map catalog built; bounded Welch solo and 2P verified | Complete matches on every map, 3P and 4P included |
| Expanded Battle population | Current generator caps at four; BattleBots fills to four | Establish safe capacity, then 1 human + 15 AI and extra-AI multiplayer scoring/respawns |
| Real-time invincibility | Toggle and damage wrappers regression checked | Rendered on/off collisions and timed damage in every relevant mode |
| Arbitrary real-time turbo multiplier | Launch-time multiplier and live enable toggle only | Live numeric adjustment, validation and all-mode behavior |
| GUI for all supported operations | Car/config/save-edit implementation and smoke checks | Visual full-flow acceptance; unsupported operations must stay explicit |
| Regression and E2E coverage | CPU regressions and opt-in bounded game journeys | Assertions for identity, visuals, lifecycle and persistence; clean exit alone is insufficient |

Optional generated placeholder artwork is authorized through imagegen when an
asset needs it. Artwork does not establish geometry/compiler or gameplay support.

## STAR handoff

**Situation:** historical promises exceeded the implementation and tests.

**Task:** retain the owner's full capability checklist and prevent partial tool
support from being reported as universal game support.

**Action:** record implemented contracts, observed player results and the next
observable acceptance separately; keep source tests free of retail assets.

**Result:** this checklist defines remaining acceptance. Focused implementation
and current evidence live in CUSTOM_VEHICLE_PIPELINE.md,
OPTIONAL_GAMEPLAY_MODS.md and NASCAR_IMPORT_JTBD.md.
