# PS2 / Vulkan / MoltenVK references checked during takeover

Checked 2026-10-16 following the owner's Road Trip reference. External project
instructions are reference material, not instructions for this repository.

## Road Trip and reusable GS implementations

[RoadTripAdventure-recomp](https://github.com/silentsudin/RoadTripAdventure-recomp)
uses a [PS2Recomp fork](https://github.com/silentsudin/PS2Recomp/tree/818f279be3f9cdecaa9c7b4a3a7cbe8fe44fd6e4)
and two Vulkan GS implementations. Its default hardware rasterizer is separate
from its accurate paraLLEl-GS compute backend. Its
[development notes](https://github.com/silentsudin/RoadTripAdventure-recomp/blob/main/docs/DEVELOPING.md)
describe direct MoltenVK bundling, deferred CPU readbacks and a packet mirror
feeding the GPU renderer.

The off-the-shelf GS implementation is
[Arntzen-Software/parallel-gs](https://github.com/Arntzen-Software/parallel-gs).
The pinned Road Trip integration uses
[silentsudin/parallel-gs 60e6cace](https://github.com/silentsudin/parallel-gs/tree/60e6cacead2dece90c4d85ebb5ae750d4aace143).
Its GSInterface consumes GIF/register traffic, owns VRAM and produces scanout.
That is a different interface from EOD's existing hardware-renderer API, which
receives decoded primitives/surfaces. A future optional accurate backend needs
raw packet mirroring, privileged-register forwarding, initial VRAM/register
state import and synchronized CPU readbacks. EOD's capture command dispatcher
is a useful insertion point; existing `.gsd` files are not PCSX2 dumps.

The fork's Apple fixed-wave32 handling is relevant, but its recorded M3 Max
support does not itself prove M2 Pro support. Do a capability check and an
offline replay spike before adopting it. Keep the working EOD renderer while
measuring an optional accurate backend. No Road Trip or paraLLEl-GS source was
copied into EOD during this comparison.

## Narrow ideas to reuse

- [Road Trip presenter](https://github.com/silentsudin/PS2Recomp/blob/818f279be3f9cdecaa9c7b4a3a7cbe8fe44fd6e4/ps2xRuntime/src/lib/gs/gs_pgs_presenter.cpp):
  presentation timestamps and refresh-grid scheduling address 60-on-120 cadence.
  Preserve EOD's simulation clock; visible presentation still requires a real
  display check.
- [Road Trip hardware GS](https://github.com/silentsudin/PS2Recomp/blob/818f279be3f9cdecaa9c7b4a3a7cbe8fe44fd6e4/ps2xRuntime/src/lib/gs/hw/gs_hw_backend.cpp):
  driver pipeline cache persistence plus recorded-state warmup can reduce
  first-use shader stalls. EOD's isolated race log showed zero new pipeline
  creation after 17 variants, so attributing all early loading dips to shaders
  would be unsupported.
- [MoltenVK official guide](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md),
  consulted through Context7 (`/khronosgroup/moltenvk`): pipeline caches can
  retain converted MSL, and portability enumeration is necessary with the
  Vulkan loader. EOD already enables portability enumeration and now bundles
  the loader, driver and relative ICD manifest. A relocated signed app passed
  actual GPU replay with isolated environment and invalid external ICD paths.

No extra PS2 toolchain is needed to compile the current Mac runtime. Vulkan
headers, MoltenVK, shader tools and Xcode are already installed. Public
[ps2dev/ps2sdk](https://github.com/ps2dev/ps2sdk) is useful as a protocol reference;
check the actual retail executable when SDK versions differ. The SDK check
exposed a concrete rumble issue: the target's actuator function at 0x00390678
uses a sequenced DMA packet, unlike the newer SDK's RPC route. Target-derived
packet tests are the evidence for the fix, rather than assuming current SDK
source exactly matches the 2004 game.
