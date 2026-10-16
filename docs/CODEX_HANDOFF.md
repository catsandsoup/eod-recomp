# Fresh-chat continuation — 2026-10-16

**New owner steering after this handoff:** LOD/draw-distance overhaul and correct
widescreen/ultrawide culling are now HIGH priority, exempt from the otherwise
deferred mods policy. Read the new priority section in MODDING_SCOPE.md. Aim for
very far highest-available detail and minimal visible pop-in, scalable to stronger
Macs. A dedicated fresh specialist is appropriate. Do not promise literal zero
pop-in before tracing streaming, submission, LOD and frustum/visibility limits;
do not just disable all culling or confuse 4K raster scale with new asset detail.

Read this and README first. This is a compact handoff, not a requirement to load
old chat history. The owner explicitly requested fresh chats and stopped old
agents to save context. All previous agents are stopped; no game/build/GPU job
should remain after the handoff checkpoint finishes.

## Owner's current outcome

Finish the PS2 native Apple Silicon Mac port. Prioritize solo custom games and
local multiplayer, original physics/timing, 100% game speed and full 4K-or-higher
internal rendering at genuinely proportioned 16:10/16:9. The owner tests career.
Defer 120 Hz and mods. WASD/arrows driving, Space brake, Enter menu Accept,
Esc back/pause. Give occasional real screenshots. Frequently commit and push
verified source to origin/macos, keeping README and player journey current.
Periodically hand off to fresh chats rather than growing old agents forever.

The latest request specifically asks what “Press X for player 2” means on Mac.
README now traces device assignment and Cross/Enter/bottom-face-button mapping.
The original game still displays PS2 artwork. Device-aware in-game Mac hints and
clear missing-P2-device guidance are unfinished; trace the journey, not only the
pad packet implementation. One keyboard is currently assigned to one player.

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

1. `vulkan-gs` WIP `62652c5`: actual-chunk CPU transfer preservation. Read its
   `docs/GS_CPU_WRITE_WIP.md` in the vk worktree. Moves notification/barriers to
   bytes actually supplied, coalesces IMAGE payload, flushes prior GPU draws,
   preserves partial/masked pages, skips preservation only for actual complete
   storage coverage. CPU GSPageCoverage520 + GSTransferChunks5000 passed;
   candidate objects and generator compile. Eleven public fixtures exist.
   NO full executable/GPU/full-VRAM comparison/performance validation. Test
   pending-order, partial, full, split, interrupted, CT24/high8/high4 cases.
   Rebase/cherry-pick carefully: its gs.cpp predates the main shutdown changes.
2. `mac-app` WIP `2608d02`: alpha_range.h and CPU proofs only. Whole decoded
   texture alpha bounds across all active mip levels can prove an alpha test
   always passes and avoid the expensive two-pass fallback. NO production
   integration/GPU validation. Read the helpers/tests, incorporate TEXA/AEM,
   palette, TCC/TFX and vertex-alpha bounds. Unknown rendered textures retain
   0..255. Require same-binary switch-off/on pixel equality at1x/6x, procedural
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
  fix/measure alpha and CPU-write WIPs; one isolated6x MP6600 with few captures
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
