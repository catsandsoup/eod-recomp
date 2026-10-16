# Mac port contribution handoff — 2026-10-16

This fork extends pareidolico/eod-recomp with a native Apple Silicon port.
The upstream game recompilation is the foundation; the Mac work does not replace
that contribution. Source is published in catsandsoup/eod-recomp. No game data,
generated game C/C++, retail command captures or compiled apps are distributed.

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
