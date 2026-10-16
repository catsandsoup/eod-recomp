# Mac port contribution handoff — 2026-10-16

This fork extends pareidolico/eod-recomp with a native Apple Silicon port.
The upstream game recompilation is the foundation; the Mac work does not replace
that contribution. Source is published in catsandsoup/eod-recomp. No game data,
generated game C/C++, retail command captures or compiled apps are distributed.

## Source branches

| Branch | Purpose | Integration status |
| --- | --- | --- |
| `macos` | Native arm64 runtime, SDL input/audio, Vulkan/MoltenVK, app/disc importer, saves, camera/HUD and opt-in car LOD | Main Mac contribution; incomplete fidelity/performance acceptance |
| `macos-cpu-preservation` | Correct preservation around actual CPU VRAM writes | Verified procedural cases; slower, not merged |
| `macos-gpu-transfers` | Full physical-block GPU patches and CPU-validity tracking | Experimental continuation; not merged or a proven speedup |
| `vulkan-gs` | Earlier preservation WIP checkpoint | Historical, superseded by preservation branches |
| `mac-app` | Alpha-range proof helpers/tests | WIP, no production integration; name predates current work |
| `rockcandy-pad` | Input/camera component history | Much already integrated; not a separate finished feature set |

Review `macos` against upstream `main`. Review experimental branches separately;
do not merge all branches together. The broad Mac port should be reviewed in
subsystems: platform/arm64, app/disc handling, input/saves, then graphics fidelity
and performance. No upstream approval or Windows regression result is implied.

## Reproduce a personal Mac build

Use Apple Silicon, Xcode command line tools, Homebrew and your own North American
v1.00 disc (SLUS_209.10: 3,368,996 bytes, CRC32 75BB93EE). Do not upload the disc.
From a clone checked out to `macos`:

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

Branded app checkpoint: all 30 main CTests passed, icon representations and plist
validated, ad-hoc signature passed strict verification, and a relocated bundle
replayed on the GPU using its own MoltenVK despite invalid external ICD overrides. The transfer branch
has 26 CTests, including 22 procedural full-VRAM software/Vulkan comparisons at
both 1x and 6x. These test different branches and are not additive coverage.
The source includes exact tests and commands; rerun on your machine.

Actual games reached race completion/clean exit in bounded headless runs; LOD
loader tracing validated 20 tables in each baseline/2x run. Camera comparisons
support Hor+ proportions in the tested scene. HUD anchor contracts cover solo
and split-screen state restoration. None proves exhaustive 1:1 rendering.

Open: thin black HUD/world lines, visible keyboard latency, native full-game
fidelity/speed acceptance, physical controller/motor checks, native dialog journey,
and original movie/audio accuracy. Heavy 6x multiplayer previously ran roughly
12–29 drawn FPS; the accurate preservation branch was slower. The latest GPU
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
