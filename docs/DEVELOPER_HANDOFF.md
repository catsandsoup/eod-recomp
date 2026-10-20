# Mac port contribution handoff — 2026-10-16

This fork extends pareidolico/eod-recomp with a native Apple Silicon port.
The upstream game recompilation is the foundation; the Mac work does not replace
that contribution. Source is published in catsandsoup/eod-recomp. No game data,
generated game C/C++, retail command captures or compiled apps are distributed.

## Upstream 1.0 merged (2026-10-16)

`main` now contains pareidolico/eod-recomp 1.0 (044bdf8 + release workflow 2eed33f).
Upstream won every conflict in shared code. Windows files stay as upstream ships them:
`runtime/platform/win32/platform_win32.cpp`, `runtime/gs/d3d11/gs_shaders.hlsl` and
`gs_d3d11.cpp` are byte-identical to 1.0. The fork's extra platform hooks
(`platform_configure_display`, `platform_thread_fast`, `platform_stop_ticker`,
`platform_list_pads`) are non-Windows only, with inline no-ops on Windows (`platform.h`).
The Vulkan shaders now build from their own copy, `runtime/gs/vulkan/gs_shaders_vk.hlsl`
(same SPIR-V as before). Windows keeps upstream's input order (pump after the
vblank IRQ) and host->local transfer reporting; SDL3 builds keep the fork's.

Widescreen follows upstream: `AspectRatio=auto|4:3|16:9|stretch` drives the game's
own display mode (`src/game_patches.cpp`); SDL3 `platform_set_aspect` feeds the
per-frame composition. The fork's race-only wide camera and 16:10 option are retired.
Not on Mac yet: the imgui F1 overlay (Direct3D 11 only; stubbed by upstream's
`overlay.h` outside Windows). Regenerate `gen/` after pulling (hle.txt/VU changed).

Checked: build-mac builds, `ctest` 43/43, headless boot to title and the scripted
2P race (`tools/test_2p_race.sh`), and `race2p-5000.gsd` replays byte-identical
(display + VRAM) to the pre-merge binary at RenderScale 1 and 6.

## One maintained branch

Use **`main`** in catsandsoup/eod-recomp for all work and developer review.
The former `macos`, `mac-app`, `rockcandy-pad`, `vulkan-gs`,
`macos-cpu-preservation` and `macos-gpu-transfers` branches are consolidated into
its history; no branch assembly is required. Their remote branch names are retired.

Main includes exact CPU-write preservation and the complete-block GPU upload
implementation. The latter is **off by default**: `EOD_GPU_UPLOADS=1` opts into
it for comparison. Both paths preserve partial/masked data conservatively and
have procedural full-VRAM comparisons at 1x and 6x. Experimental alpha-range
helpers have CPU tests but are not used to change production draw decisions.
The alternate CPU texture-source shortcut is gated by the same opt-in switch,
so it cannot change default scaled render-to-texture selection. Do not enable an
optimization just because its source is included.

Review `main` against pareidolico/eod-recomp `main`. Suggested subsystem order:
platform/arm64, app/disc handling, input/saves, then graphics fidelity/performance.
No upstream approval or Windows regression result is implied. Git history retains
the earlier component checkpoints without requiring separate maintained branches.

## Reproduce a personal Mac build

Use Apple Silicon, Xcode command line tools, Homebrew and your own North American
v1.00 disc (SLUS_209.10: 3,368,996 bytes, CRC32 75BB93EE). Do not upload the disc.
From a clone checked out to `main`:

```sh
brew install cmake ninja libusb molten-vk vulkan-loader vulkan-headers
python3 -m venv .venv
.venv/bin/pip install rabbitizer
cp '/path/to/your/disc/SLUS_209.10' .
.venv/bin/python tools/analyze.py
.venv/bin/python tools/recomp.py
.venv/bin/python tools/vurecomp.py
cmake -S . -B build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE="$PWD/.venv/bin/python" \
  -DEOD_VULKAN=ON -DEOD_MAC_APP=ON
cmake --build build-mac -j3
EOD_HEADLESS=1 EOD_MUTE=1 ctest --test-dir build-mac --output-on-failure
```

Default app output:
`~/Library/Application Support/TDEODRecomp/local-build/Eve of Destruction.app`.
The bundle contains its linked non-system libraries, MoltenVK ICD, generated
TDEOD icon and an ad-hoc signature. It requires no Homebrew at runtime, but the
minimum OS depends on the build machine's libraries. It is not notarized.
First GUI launch offers a disc picker; an installed valid disc skips it. Settings
and saves live in Application Support/TDEODRecomp. F1/Cmd+, opens settings;
F2 explains controls and joining P2. See MACOS.md for detailed setup/diagnostics.

For a bounded native test, create a scratch INI with Renderer=vulkan,
RenderScale=1, AspectRatio=4:3, FrameSkip=0 and VSync=1 under [Video], then run:

```sh
EOD_HEADLESS=1 EOD_MUTE=1 build-mac/eod --headless \
  --config /path/to/native.ini --data '/path/to/your/disc' --frames 600
```

`tools/test_2p_race.sh` supplies reproducible scripted race input. Retail captures
mentioned in historical logs are local diagnostics, not prerequisites shipped in
this repo. Public procedural GS fixtures on the experimental branch do not need
a retail command capture; a full game build still requires the owner's executable.

## Evidence and remaining acceptance

Consolidated main: Release Mac build and all 37 CTests passed. The suite includes
CPU preservation and opt-in GPU upload modes at both 1x and 6x, each comparing
22 complete 4-MiB VRAM fixtures against software. It also includes alpha-range,
LOD/HUD, input, save and worker lifecycle tests. No test was skipped.

Previous branded app packaging passed icon/plist and strict signature validation,
a relocated bundled-MoltenVK replay and a native 600-frame startup. Those earlier
app checks precede consolidation; rerun them on the consolidated output before
claiming installed-app acceptance.

Actual games reached race completion/clean exit in bounded headless runs; LOD
loader tracing validated 20 tables in each baseline/2x run. Camera comparisons
support Hor+ proportions in the tested scene. HUD anchor contracts cover solo
and split-screen state restoration. None proves exhaustive 1:1 rendering.

Open: thin black HUD/world lines, visible keyboard latency, native full-game
fidelity/speed acceptance, physical controller/motor checks, native dialog journey,
and original movie/audio accuracy. Heavy 6x multiplayer previously ran roughly
12–29 drawn FPS; the accurate preservation branch was slower. The opt-in GPU
texture-validity change is not yet retail-benchmarked. Do not describe these as
flawless, full-speed 4K or an established transfer speedup. Windows/D3D11 was not
run during this Mac session. Achievements are not implemented by this work.

Owner order: 1:1 native 1x/4:3, original timing, stable full speed without frame
skipping; LOD/draw distance and proper widescreen; higher resolutions sequentially;
higher FPS; DLSS last. Car LOD expansion is opt-in via EOD_CAR_LOD_MULTIPLIER;
leave it unset for the native baseline. EOD_HUD_EDGE_ANCHORS=0 disables wide HUD
anchor adjustment for same-binary comparison. Leave enhancements off while
establishing the original baseline.

Before new renderer work, read RECOMP_RENDERER_RECON.md and
RENDERER_REFERENCES.md: pinned PCSX2, Road Trip/PS2Recomp/paraLLEl-GS, Play! and
SDK references include applicability limits. Reuse proven approaches and verify
against this game's actual outputs; another project's success is not correctness
proof for this runtime.

## Native alpha-outcome optimization — October 16 continuation

`main` enables conservative whole-batch all-pass/all-fail alpha proofs by default;
`EOD_ALPHA_RANGE=0` keeps the same-binary baseline. Cached decoded bounds include
all active mips; rendered texture bounds remain unknown. Native retail replay
median falls 230.4 to 184.3 ms (20.0%) across alternating 3×4 repetitions, with
143,373 to 102,931 draw calls and exact off/on race/font images and complete VRAM
at both 1x and 6x. All 39 CTests pass. Readbacks/submits remain 58/65; full speed
and visible input latency are not established by these replay checks. See
`docs/GS_ALPHA_ORDER.md` for proof, reuse source, fixtures and measurement scope.
The candidate also reached 6,600 native 1x/4:3 two-player vblanks, exit 0 in
110.23 s; race samples were 17.3–32.0 drawn/game FPS, final 20.2, GS 97–99% and
EE waiting 56–75%. This remains below player acceptance; original-reference
cadence, hands-on latency/physics/fidelity, solo acceptance and thin-line
comparisons are still open. Full-loop evidence: `work/alpha-native-race/`.

## Native Vulkan command encoding — October 16 continuation

**Situation/task:** heavy native 1x/4:3 races still miss original-speed player
acceptance. Reduce renderer CPU command overhead while preserving every draw,
transfer, image and timing rule; do not infer hardware GPU utilization from the
GS worker's busy percentage.

**Action:** reuse Granite's changed-state emission pattern in `vk_compat.h`.
Pipeline, viewport, scissor, blend constants and pushed image/sampler descriptors
are emitted only when changed. One common pipeline layout and dynamic-state
configuration makes these states reusable across rendering breaks. Every fresh
command buffer invalidates all cached bindings, including the two-flight pool
swap. Resource handles stay alive through deferred fence-based destruction.
`EOD_VK_STATE_CACHE=0` restores the same-binary command-emission baseline.
Push constants already existed; shaders and primitive ordering are unchanged.
See `RENDERER_REFERENCES.md` for pinned upstream/spec evidence.

**Result:** alternating three runs per mode, four repetitions each with first
replay excluded, reduced the fixed five-game-frame native race median from
180.8 to 152.2 ms (15.8%). Samples: baseline 178.1–188.3 ms, candidate
148.2–155.9 ms. Both modes issue 102,931 draws. Race/font displayed images and
complete 4-MiB VRAM are byte-identical off/on at both 1x and 6x. Local evidence:
`work/vk-state-cache/verification.json` and logs. The prior alpha optimization
remains enabled in both modes. These are controlled replay measurements, not a
claim of full-game original speed or exhaustive fidelity.

Frame-rate log caveat: `GS busy` is the CPU renderer worker's occupancy, not
hardware GPU utilization. The logged game-frame counter is a framebuffer-flip
heuristic, not a simulation-update hook; the host vblank ticker staying near
59.9 Hz does not establish original simulation/physics cadence. A matched
original-game reference and hands-on latency/physics comparison remain required.

Same-binary native two-player checks both reached 6,600 vblanks and exited 0,
with 1x/4:3, FrameSkip=0 and enhancements off: cache off 110.214 s, race drawn
counter 17.5–36.5 FPS (final 36.5); cache on 110.209 s, 22.4–33.8 FPS
(final 32.0). Candidate GS worker busy was 97–99%, EE waiting 50–64%.
The ticker bounds wall time and scene variation prevents a precise native
speedup claim. These counters remain below player acceptance evidence.
Local runs: `work/vk-state-cache/native-0/` and `native-1/`.

Final validation: all 41 CTests passed (none skipped), including cache off/on
full VRAM/display comparisons at 1x and 6x, varied target/scissor/filter/texture
state and two-circuit constant-alpha composition across repeated submissions.
The canonical app was rebuilt, replaced in `~/Applications`, passed strict deep
signature verification and completed a headless muted retail Vulkan replay.
The native GUI journey, visible thin lines/input latency and physical controllers
remain unverified; packaging and headless replay do not complete those criteria.

## Native GS per-draw command cost — October 16 continuation (STAR)

**Situation:** the native 1x/4:3 two-player race stutters because the GS worker
needs ~25 ms per game frame. The fixed capture `work/claude-native/race2p-5000.gsd`
(2 game frames, 36,365 draws) replayed in ~49 ms. An earlier "~42 ms GPU busy per
replay" reading was wrong: it summed every process's Metal intervals. Filtered to
`eod`, depth 0, the GPU is busy ~5 ms per warm replay (10% of wall time);
surfaces are only 640x512/320x512. The cost is CPU recording, MoltenVK encoding at
`vkQueueSubmit` and 19 synchronous CPU-write-preservation readbacks.

**Task:** cut per-draw Vulkan command cost without changing any draw, ordering,
transfer or timing, pixel-exact.

**Action** (PCSX2 GSDeviceVK pattern, see `RENDERER_REFERENCES.md`): the two-pass
alpha-test half is now a `ps_draw` specialization constant (`c_atest_select`,
Vulkan build only) instead of push-constant flags; push constants are skipped when
byte-identical; each batch's vertices are uploaded and bound once
(`SetVertexWindow`, draws use `firstVertex`); the std::string pipeline key is a
POD key with a 4-entry recent cache. Per draw this removes ~35k push-constant and
~36k vertex-bind commands per replay. Kept the render-pass end-cause counters
(`render passes N, ended by ...` in the stats line).

**Result (measured):** alternating same-capture runs, 4x6 replays, first excluded:
median 49.05 -> 41.45 ms per replay (-15.5%); GPU busy ~5.0 -> ~4.1 ms per warm
replay (Metal System Trace, eod only). Draws/batches/readbacks/submits unchanged
(36,365/1,013/19/22). Race and font fixtures: display image and full 4 MiB VRAM
byte-identical to the pre-change binary at RenderScale 1 and 6, in default,
`EOD_GPU_UPLOADS=1` and `EOD_VK_STATE_CACHE=0` modes. All 43 CTests pass. No new
switch: the change is default because it is exact; `EOD_VK_STATE_CACHE=0` still
restores per-draw push/bind emission.

**Remaining:** 41 ms per 2 frames is still above the 33 ms needed for 60 fps.
Biggest cost is now the 19 sync readback fence waits (~17 ms, ~41% of replay):
mostly per-command-buffer round-trip latency (~0.3 ms minimum each) plus the
accumulated GPU work, not GPU throughput. Next: avoid or defer the
CPU-write-preservation readbacks into the PSMZ16 surface (see
`GS_CPU_WRITE_PRESERVATION.md`), then MoltenVK encode (~16%). Ruled out:
MVK prefill/argument-buffer/async-submit config, early draw-count kicks.
Native full-game run not repeated here.

## Native 2P race at 60 fps — October 16 continuation

**Cause:** the GS worker was CPU-bound: 19 synchronous CPU-write-preservation
readbacks per two frames (HUD textures streamed into the split-screen Z16
surface's memory), each a submit + fence wait, then MoltenVK command encoding
inside `vkQueueSubmit`. The EE blocked on the GS queue and caught up missed
vblanks (the visible jumping).

**Changes (all exact, Vulkan/macOS only):**
- Deferred preservation (`deferred_schedule`/`deferred_resolve` in `gs_vulkan.cpp`):
  the readback pass and copy are recorded at the same command position into a
  staging slot without submitting; ownership metadata is unchanged. A per-page
  bit mask tracks CPU-rewritten storage bits; before any CPU read of a block
  still holding a GPU-owed bit (readback_pages consumers, surface uploads,
  texture decode of the exact texels) the copy is waited for and merged
  bit-exactly. Pages the CPU rewrites completely never wait (18 of 19 here).
  `EOD_DEFER_PRESERVE=0` restores the synchronous path.
- Pages whose last deferral needed a merge submit early (`EOD_DEFER_KICK=0` off).
- `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0` (unless the user set it): MoltenVK
  encodes on its own queue thread.

**Gate:** display PPM and 4 MiB VRAM byte-identical to the pre-change binary at
RenderScale 1 and 6 for race2p-5000, race2p, codex-native2p and three heavy
late-race captures; 43/43 CTests (the CPU-write fixtures exercise the merge).

**Result:** race2p-5000 warm replay 49 -> 23 ms per 2 frames at 1x (loaded
machine; ~38 ms idle baseline), 60 -> 33 ms at 4x, 64 -> 36 ms at 5x. Live
scripted 2P race (`tools/test_2p_race.sh` input, 1x): 5 s windows 30-39 ->
59.9 fps, vblank catch-ups ~11/s -> 0, worst vblank debt 77 -> <17 ms. At 4x
(~3024x1964) median 59.7 with dips to ~36-57 in heavy/start windows; at 5x
(~4K) median ~53: GPU fragment work dominates there. A one-off ~35 ms GS hitch
remains at race load (first decode of ~64 MB of textures). The stats line
prints `deferred preservation N (merged, waited, overwritten, kicked, open)`.

## Clean-start existing-renderer probe

The default-off `EOD_GS_DUMP_COLD_START=N` capture now permits an exact initialized
packet stream, bounded to 256 MiB and an explicit vblank count. The pinned Road
Trip paraLLEl-GS oracle consumed real EOD boot/movie/title traffic on M2 Pro and
produced actual images. An observed SetGsCrt(1,2,0), official PS2SDK mapping and
Road Trip's CMOD=2/LC=32 bootstrap enable CRTC output. The completed title prefix
is **not parity** and **not a speedup**: PGS warm 951.6 ms, EOD Vulkan 781.4 ms;
frame/VRAM differences remain. See `GS_COLD_PACKET_REPLAY.md` for software
comparison, scope and exact next experiment. All 41 CTests pass. The installed
app retains b15d0d9's two verified performance changes; the added source hook is
only a diagnostic. Player original-speed/latency acceptance remains incomplete.

## 2P race at 60 fps at RenderScale 4 and 5 — October 17 continuation

**Cause:** at 4x the race-start dip was not GPU throughput (GPU ~43% busy) but
serialization: MoltenVK encodes a whole command buffer before the GPU starts,
and the frame-end submit waited for the previous frame's encode + GPU time. At
5x the GPU itself was the limit: `ps_draw` (one shader branching on push-constant
flags) cost ~15 ms of fragment time per frame in heavy race frames; one draw of
large blended quads over P1's view took ~5.5 ms of it. Windowed play also never
got MoltenVK's async submits (SDL loaded MoltenVK before the setting was made).

**Changes (all exact, Vulkan/macOS only):**
- Mid-frame submits (`vk_compat.h`): submitted command buffers go to a queue of
  flights recycled when their fence signals; after 1024 draws
  (`EOD_VK_CHUNK_DRAWS`, 0 = off) a render-pass boundary submits without
  waiting. Frame ends still wait for the previous frame end (one frame in flight).
- Specialized `ps_draw` pipelines: spec constants `c_flags_value`/`c_flags_known`
  fold the flag tests; a low-QoS background thread builds the variant on first
  use while the generic pipeline draws (`EOD_VK_SPECIALIZE` 0 off, 1 background,
  2 synchronous). Pipeline creation is a function of the key (`create_pipeline`).
- `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0` is set by a static initializer.
- Diagnostic: `EOD_VK_PASS_LOG=1` prints one line per render pass/submit.

**Gate:** display PPM and 4 MiB VRAM identical to the pre-change build at
RenderScale 1, 4 and 6 for race2p-5000, race2p, codex-native2p, three heavy
late-race captures and race2p-start (specializer modes 0/1/2); ctest 44/44.

**Result (live scripted 2P race, headless muted, 5 s windows from the
countdown):** 1x 59.9 median / 56.5 min, 4x 59.9 / 56.3, 5x 59.9 / 56.9 (the
countdown windows run at ~57 at every scale: the game's own rate). Before: 4x
59.8 / 45, 5x 51.7 / 38.6. Windowed muted 4x: 59.9 median, countdown 57.
Not done: the one-off race-load texture decode (no longer causes vblank debt
> 2 ms), the two-pass alpha draws (still ~2x fragment work for alpha-tested
geometry), full-surface conversion passes.

## Collision/effects performance checkpoint — 2026-10-20

The player reports reduced drawn FPS during crashes, chicken explosions, sparks
and dust, while the title still shows 100% speed. That label is host-vblank based,
not proof of smooth game frames. Native 1x ordinary-race samples remain near 60;
4x effect-heavy runs reproduce drops. The retained Vulkan texture-key optimization
uses pinned header-only xxHash and reduces warmed CPU-limited replay wall time
by 12.4% (1.14x throughput), but only 1.5% at 4x. This is **not** completion of the
reported high-resolution carnage issue or step 1. A slower depth-batching
experiment was rejected and removed. Read COLLISION_PERFORMANCE.md for exact
profiles, parity gates, reproduction limits and the next decisive work. No
physics/effects/quality reduction or frame skipping was introduced; NASCAR stayed
paused.

## On-demand personal Mac app — 2026-10-21

The owner's personal signed app now includes Native, Phantom, all-map Battle
and Combined launch choices. F1 exposes existing cheats/damage options and
editable turbo/chicken multipliers. Profile selection precedes streaming;
resource changes require a restart. Native uses no overlay. Headless and explicit
EOD_MOD_DIR runs bypass the chooser. Personal disc-derived profiles can be
bundled with EOD_BUNDLE_MODS; normal source builds include none.

Verification: all 62 active CTests passed (45.69 s); NASCAR was excluded per the
owner's pause. Eight composer CPU cases cover independent edits, conflicts and
exclusive atomic publication. Strict app signing verification passes. All 58
merged COMMON entries were compared against the selected original source
payload/metadata with no loss. A 6,000-frame native-input Combined race selected
Phantom as the human car and passed the existing E2E assertions (100.20 s).
The clean Combined profile also passed a separate 6,000-frame scripted Welch
solo Battle check: actual Welch resource loaded, active phase 1, one human and
four total cars. This bounds startup acceptance; it does not prove a full match,
all-map AI navigation or every mode. Logs are in ignored
`work/on-demand-{clean-combined-car-e2e,combined-welch-result}.log` and
`work/on-demand-combined-welch/out.log`.

An initial Combined race crashed with `MemFree: Dennison.trk not in use` when
using an older Battle package that also contained an experimental Dennison
dynamic-barrier edit. Regenerating a pure Battle catalog excludes that track
override and the same race then passes. The clean and experimental Battle
COMMON archives are byte-identical; the experimental track remains excluded.
This does not claim to have repaired that barrier experiment.

Cleanup removed merged local branches `fix/input` and `mod/lod`, their obsolete
compiled checkouts, old test executables/app backups and an unused legacy game
library cache: 4,731,352,428 logical bytes. A verified all-refs Git bundle,
inventories and preserved work/gen research live in ignored
`work/cleanup-2026-10-20`. Remaining unmerged branches and the open recovery PR
were retained. Disc assets, saves and personal settings were preserved.

Visible chooser/F1 interaction and exhaustive event compatibility remain
hands-on acceptance gaps. Four-car Battle capacity, navigation limitations,
unfinished geometry import and the high-resolution effects slowdown remain.
See ON_DEMAND_MODS.md for usage and STAR handoff.

### Owner correction: invincibility protects humans only

The owner clarified that invincibility must apply to every human player, with
AI retaining normal damage. Both damage hooks now classify their target using
native car+0x1E0 == 0x50, covering P1–P4 regardless of world slot. The inline
timed-damage guard follows the native selected-driver array and suppresses the
feat only for a human car. Native source inspection confirms queued camera
changes do not alter that selected-driver index before the inline write.
Collision impulses, existing damage, repairs and respawn remain unchanged.

A failing-then-passing regression covers four humans, AI callback delegation,
invalid pointers and selected-human versus selected-AI timed damage. F1 and
saved-setting descriptions now explicitly say human players only. This
supersedes earlier statements that invincibility protects AI or prevents AI
Battle damage kills. Hands-on collision/deformation acceptance remains separate
from the callback/identity regression.
After this correction, all 62 active CTests pass again (50.02 s); NASCAR remains
excluded. The personal app and ZIP were rebuilt with the human-only behavior.
