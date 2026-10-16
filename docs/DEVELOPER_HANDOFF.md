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
