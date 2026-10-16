# Fresh-chat continuation — 2026-10-16

**New owner steering after this handoff:** LOD/draw-distance overhaul and correct
widescreen/ultrawide culling are now HIGH priority, exempt from the otherwise
deferred mods policy. Read the new priority section in MODDING_SCOPE.md. Aim for
very far highest-available detail and minimal visible pop-in, scalable to stronger
Macs and high-end PCs/RTX 5090-class hardware at 8K. Maximum/Extreme quality tiers
may exceed the owner's full-speed hardware; retain a playable Mac preset and
faithful baseline. Distinguish CPU submission/streaming from GPU pixel cost.
DLSS is explicitly LAST per the owner's latest instruction. Do not spend work
on it until the core port and quality work are complete. A dedicated fresh specialist
is appropriate. Do not promise literal zero
pop-in before tracing streaming, submission, LOD and frustum/visibility limits;
do not just disable all culling or confuse 4K raster scale with new asset detail.

Read this and README first. This is a compact handoff, not a requirement to load
old chat history. The owner explicitly requested fresh chats and stopped old
agents to save context. All previous agents are stopped; no game/build/GPU job
should remain after the handoff checkpoint finishes.

## Standing reuse rule

Read root `AGENTS.md`. Before designing new fixes, check similar recomps online,
especially RoadTripAdventure-recomp, Context7 and official SDK/MoltenVK sources.
Reuse applicable work first; record concrete reasons when it does not fit.
`RENDERER_REFERENCES.md` now records the verified CPU-read boundary in Road Trip
accurate backend and unsupported render-target transfers in its hardware path.
Build on the existing oracle rather than invent another whole GS. DLSS is last.

## Owner's current outcome

Finish the PS2 native Apple Silicon Mac port. Prioritize solo custom games and
local multiplayer, original physics/timing, 100% game speed and full 4K-or-higher
internal rendering at genuinely proportioned 16:10/16:9. The owner tests career.
Defer 120 Hz and other mods; LOD/draw-distance/culling is explicitly prioritized.
WASD/arrows driving, Space brake, Enter menu Accept,
Esc back/pause. Give occasional real screenshots. Frequently commit and push
verified source to origin/macos, keeping README and player journey current.
Periodically hand off to fresh chats rather than growing old agents forever.

The latest request specifically asks what “Press X for player 2” means on Mac.
README now traces device assignment and Cross/Enter/bottom-face-button mapping.
The original game still displays PS2 artwork. F2 now opens Controls and player 2
with current SDL device assignments, Accept bindings and missing-P2 guidance;
menu window titles and Settings mention it. Reopen after hot-plugging. The native
dialog needs a hands-on visual check and automatic PS2 prompt replacement remains
unfinished. One keyboard is currently assigned to one player.

## Workspace and safety preferences

- Main `/Users/monty/Documents/GitHub/eod-recomp`, branch `macos`, personal fork
  `catsandsoup/eod-recomp`; upstream `pareidolico/eod-recomp`.
- Existing component worktrees: `../eod-recomp-vk` (`vulkan-gs`),
  `../eod-recomp-app` (`mac-app`), `../eod-recomp-pad` (`rockcandy-pad`).
- Old Xbox repo `/Users/monty/Documents/GitHub/TDEOD RECOMP` stays parked.
- Automated game tests MUST be headless and muted: `EOD_HEADLESS=1 EOD_MUTE=1`
  plus `--headless`. User-authorized interactive play is separate. Avoid
  competing GPU tests or builds during performance measurements.
- Owner's disc: `/Users/monty/Library/Application Support/TDEODRecomp/disc`.
  The supported SLUS_209.10 is 3,368,996 bytes, CRC32 75BB93EE.
- Personal config: Application Support/TDEODRecomp/eod.ini. Current personal
  trial uses Vulkan, render scale 6, 16:10, VSync, FrameSkip, Racing keyboard P1.
  Do not silently lower quality/opponents/simulation to hide defects.
- Canonical personal signed app: Application Support/TDEODRecomp/local-build/
  Eve of Destruction.app. Rebuilding can rewrite it: do not rebuild while the
  owner is playing. No game currently runs at handoff.
- Never commit disc, generated `gen/`, captures, binaries or screenshots.
  Source push authorized; no public binary release. Source changes need checks.

## Integrated work and current evidence

See STATUS.md, GS_ALPHA_ORDER.md, NATIVE_CAMERA_ASPECT.md,
VULKAN_SYNC_DIAGNOSTICS.md and WORKER_SHUTDOWN.md for exact proofs/limits.

- SDL Racing context controls, menu/title/driving mappings, analog scripted
  pressures, virtual controllers, SDK pad DMA/rumble, memory-card persistence,
  movie helper and signed bundled MoltenVK app were integrated previously.
- Native keyboard polling now occurs before pad DMA publication. Mac GS queue
  backlog reduced 12→2. Launch VSync stays stable rather than recreating the
  swapchain when catch-up flags change. No measured event-to-visible latency
  claim: headless tests cannot establish the owner's reported ~0.5 second lag.
- Explicit wide mode now pairs 4:3 menus/movies with native mode0, and race
  mode1 with camera base 5/6 for16:10 or .75 for16:9. Per-vblank tagged aspect
  crosses EE/GS threads in order. Actual race trace confirms recomputation.
- Matched rendered static-background geometry supports real Hor+ (X/Y scale
  ratio1.001 vs stretched1.20). Native VIF matrix separately compensates Y by
  1/xscale. This is evidence for the tested MP scene, not all HUD/screens.
- Font bug reproduced with exact same captured GS commands: alpha pass batching
  reordered shadows over glyph fills. Ordered per-primitive PASS/FAIL fixed it
  but severely increased race draw calls. A proven constant-Z FB_ONLY fast path
  restores menu draw count and identical corrected pixels. Conservative
  disjoint-tile grouping restores another ~9% in the limited race replay;
  performance remains inadequate. Latest grouping commit integrated as2aa0c07.
- Exact physical requested pages replace an overbroad page-range estimate for
  CPU reads and writes. Removes reads of wholly unrequested neighbouring pages
  and preserves their GPU ownership. All requested-block overlap remains read.
- Normal-exit crash reproduced: GS worker accessed a texture unordered_map
  during static destruction. Integrated acfc357 drains/joins ticker, offload,
  GS and raster workers. CPU lifecycle regression passes; repeat full6600 game
  exit on combined build before claiming real crash fixed.

The previous 6x MP run BEFORE ordered-alpha fixes reached6600, then crashed on
exit; final sample59.9vblank/s,59.5gameframes/s,44.8drawnframes/s. Full-speed
rendering is NOT met. Capture-excluded worst pump gap35.6ms, GS block26.1ms,
pump+DMA59us. A367ms spike overlapped intentional dump readbacks. Subsequent
ordered-alpha-only4:3 run fell to10.8drawn/29.4game FPS; never ship that as fast.
Later constant-Z/tiled improvements need a fresh full-game measurement.

Thin black HUD/world lines are still open. Lost pixels after partial CPU writes
to GPU-owned pages are proven with synthetic fixtures, but not established as
the specific black-line cause. Transparent cars near the green flag may be
normal game ghosting; do not classify from appearance alone.

## Saved, UNVERIFIED next work — do not blindly merge

1. `vulkan-gs` WIP `62652c5` was tested carefully in this continuation,
   preserving main shutdown/presentation/alpha changes. Its verified but slower
   successor is saved on `macos-cpu-preservation`, NOT merged into `macos`. Full Mac build and all
   25 CTests pass, including complete 4 MiB software/Vulkan comparisons for 12
   public transfer fixtures at 1x/6x. Font and five-frame race images are unchanged.
   Read `GS_CPU_WRITE_PRESERVATION.md`: isolated 6x replay grows 211–229ms to
   325–330ms with 58 additional CPU readbacks. Exact-page grouping did not reduce
   the cost and was removed. Correctness is verified for those fixtures;
   efficient preservation remains open. Point scaling at 6x is a separate newly
   recorded limitation. This is not proof of the black-line cause.
2. `mac-app` WIP `2608d02`: alpha_range.h and CPU proofs only. Whole decoded
   texture alpha bounds across all active mip levels can prove an alpha test
   always passes and avoid the expensive two-pass fallback. NO production
   integration/GPU validation. Read the helpers/tests, incorporate TEXA/AEM,
   palette, TCC/TFX and vertex-alpha bounds. Unknown rendered textures retain
   0..255. Require same-binary switch-off/on pixel equality at 1x/6x, procedural
   strict/variable-Z fallbacks and measured race improvement. Do not approximate.

## Local fixtures and practical next checks

- Main CLI `build-mac/eod`; build with CMake Ninja `EOD_MAC_APP=ON EOD_VULKAN=ON`.
  `tools/recomp.py` was already regenerated for new camera HLE wrappers. Generated
  sources ignored. Run regeneration only if HLE config changes or missing.
- Main `tools/test_2p_race.sh` documents reproducible ACTION→RACE→MULTI scripts
  to6600; its automated pads are NOT physical-controller journey proof.
- Fresh heavy-race fixture and log:
  `/private/tmp/eod-mp-diagnostics-20261016/race.gsd` (74MB,9vblanks/5gameframes),
  settings.ini,out.log,frame_02250/04500.ppm/png. Dump at5000 forces readbacks.
- Actual font fixture `/private/tmp/eod-font-baseline-20261016/font-menu.gsd`;
  software.png, vulkan.png and replay.ini. Fixed output /private/tmp/
  eod-alpha-overlap/font-after.png. Pre-font binary eod-old in baseline folder.
- 4:3 reference /private/tmp/eod-mp-4x3-reference-20261016/frame_04500.png.
- Diagnostic analysis script and JSON/CSV: old chat work directory
  `/Users/monty/Documents/Codex/2026-10-16/h/work/`.
- Real user-facing screenshots in old chat outputs/: font-before.png,
  font-after.png, widescreen-camera-comparison.png. Copy needed deliverables
  into the fresh chat's outputs before linking there. No fabricated game images.
- Short next validation: combined build/CTest; replay public fixtures and font;
  fix/measure alpha and CPU-write WIPs; one isolated 6x MP6600 with few captures
  to verify full exit, speed, proportions and lines. Then useful user play build
  with native keyboard-event timing if needed, keeping tests headless/muted.
- Read docs/RENDERER_REFERENCES.md for Context7, RoadTripAdventure-recomp,
  PS2Recomp/paraLLEl-GS and public PS2SDK research. Do not reinvent their loader/
  MoltenVK work; no SDK download needed currently. An independent GS oracle
  scratch project is in old chat work/pgs-oracle; no production migration chosen.

The handoff checkpoint's exact build/CTest outcome is in main ignored
work/codex-build-handoff.log and work/codex-ctest-handoff.log. Read the result;
do not inherit a passing claim without checking it. README and source must be
committed/pushed before this old chat finishes the transfer.

Candidate build/25 CTest logs: `work/codex-build-cpu-final.log` and
`work/codex-ctest-cpu-final.log`. Baseline 6x MP 6600 clean exit is recorded in
`work/continuation-6x/`; preservation-build run is in `work/cpu-write-mp6x/` (exit 0, late 8–14 drawn FPS).
The canonical app and main build are restored to `macos`; final main 22 CTest
logs are `work/codex-ctest-restored-main.log`. No game/GPU job remains.
F2 Controls/player2 is implemented; native dialog still needs a hands-on check.
LOD/culling static investigation: `LOD_CULLING_INVESTIGATION.md` (a6445db).
Start with read-only loaded-table/handle/selection/visibility traces before
enabling the load-time threshold multiplier. The correct draw_distance enum
mapping is 0→1150, 1→725, 2→300; raising it beyond 2 cannot extend distance.


## Current continuation checkpoint — LOD and HUD

- User explicitly flags HUD inset in widescreen: world Hor+ proof is not full
  HUD correctness. Native semantic left/right HUD row wrappers now shift only
  the relevant per-player anchor while producing draws, restore exact bits,
  preserve shapes/overscan and leave centered UI/menu/movie paths untouched.
  `EOD_HUD_EDGE_ANCHORS=0` is the same-binary baseline; actual game captures
  with `EOD_HUD_TRACE=1` are the immediate next acceptance.
- `LOD_RUNTIME.md` corrects previous layout assumptions: count pointer+0x224,
  active handle pointer+0x21C and cloned shared tables matter. Loader-only
  `EOD_CAR_LOD_MULTIPLIER` validates own storage and scales positive thresholds
  transactionally. Baseline and2x each ran5000 vblanks, exit0,20/20 valid tables.
  Captures differ in camera/motion, so do not claim a matched visual improvement.
- Main build30/30 CTests pass; `work/hud-build.log`, `work/hud-ctest.log`.
  Native wrappers generated with normal tools; unchanged output timestamps
  were retained to avoid recompiling unchanged generated translation units.
- Independent online recon in `RECOMP_RENDERER_RECON.md`: no proven matching
  Monster Games recomp found. PCSX2 Metal/emulator, upstream paraLLEl-GS, Play!
  Vulkan and OpenGOAL are distinct references with explicitly recorded limits.
  Road Trip is not assumed correct or counted as independent of its dependencies.
- Renderer work is isolated in `../eod-recomp-vk`, branch`macos-gpu-transfers`,
  based on the preservation candidate. Reusing GPU-authoritative uploads for
  proven complete physical blocks, with a strict fallback for partial/masked
  data. Actual readback trace shows CPU32 uploads alias16-bit color/depth owners.
  Direct uploads alone only moved the stalls; explicit CPU-known-block validity
  is being tested. No renderer change has been merged into main yet.
