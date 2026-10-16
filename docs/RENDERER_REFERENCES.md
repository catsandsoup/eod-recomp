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

### Independent M2 Pro oracle

A separate scratch executable now builds the pinned paraLLEl-GS/Granite code
with Road Trip's existing Darwin timer shim and initializes it headlessly through
MoltenVK on the M2 Pro. Seven generated 64×64 fixtures import canonical register
state and VRAM, replay GIF packets and inspect the resulting VRAM. This does not
import arbitrary retail replay state or prove whole-game parity.

The oracle confirms the corrected Gouraud truncation and fog arithmetic. It
also confirms hardware filter rounding (the UV bucket fixture gives 96, where
EOD software gives 95) and a rounded varying fog coefficient. Matching software
alone would have encouraged incorrect changes in both cases. A blended interior
pixel gives `(14,28,44)` in paraLLEl-GS versus `(15,29,45)` in the existing Vulkan
backend, establishing a real fixed-function blend precision gap.

Keep this independent oracle for future generated texture/blend/depth fixtures.
Any production exact-blend path must handle ordered destination reads, alpha
tests, depth tests and writes together; an RGB bias cannot fix the general case.

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

## Readback-cost follow-up and standing reuse rule — October 16, 2026

The owner explicitly requires checking existing projects online, Context7 and
SDK sources before designing replacements. This is now in root `AGENTS.md`.
Rechecked Road Trip's live third-party listing: PS2Recomp still pins
`818f279be3f9cdecaa9c7b4a3a7cbe8fe44fd6e4`.

The [accurate backend](https://github.com/silentsudin/PS2Recomp/blob/818f279be3f9cdecaa9c7b4a3a7cbe8fe44fd6e4/ps2xRuntime/src/lib/gs/gs_pgs_backend.cpp)
mirrors GIF packets through `gif_transfer`; CPU primitive/image-upload callbacks
are empty. `BeginTransfer` pulls GPU VRAM for local-to-host reads; `ReadVram`
and snapshots also synchronize. Reuse this packet-mirror/CPU-read boundary as
the next comparison target, building on the existing local paraLLEl-GS oracle.
It avoids designing another complete GS. Arbitrary EOD replay-state import,
M2 Pro performance and the required resolution still need verification.

The [hardware backend](https://github.com/silentsudin/PS2Recomp/blob/818f279be3f9cdecaa9c7b4a3a7cbe8fe44fd6e4/ps2xRuntime/src/lib/gs/hw/gs_hw_backend.cpp)
reports render-target local-to-local copies and local-to-host reads as
unsupported in `BeginTransfer`. Its CPU upload/version-hash approach is useful
reference, but it cannot establish EOD transfer fidelity. The slower candidate
is therefore retained separately rather than replaced blindly by this path.

Context7 `/khronosgroup/moltenvk` was queried again for submission/readback
behavior. Its [configuration reference](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Configuration_Parameters.md)
explains synchronous queue processing and prefilled command-buffer limits.
These affect scheduling; they do not remove required data dependencies.
EOD's current `vk_compat.h::Map(Texture*)` submits and waits per map. Changing
configuration alone is not evidence that 58 required readbacks become cheap.
Do not tune blind, skip preservation or start DLSS work to conceal this cost.

Downloaded public source references are ignored under `work/references/`;
no third-party implementation was copied into the production renderer here.

## Command state reuse — October 16 continuation

Rechecked Road Trip's pinned hardware GS above: it records through Granite's
`CommandBuffer` abstraction, rather than EOD's small compatibility context.
[Granite command buffer](https://github.com/Themaister/Granite/blob/fb178c8080d163419e8d20f10715c61c53c1ec9b/vulkan/command_buffer.cpp)
(`fb178c8080d163419e8d20f10715c61c53c1ec9b`) supplies the applicable wheel:
`flush_render_state` binds only changed pipelines and emits viewport/scissor
only when dirty. Adapt that pattern locally rather than importing Granite's
device/resource architecture. No third-party source was copied.

Context7 `/khronosgroup/vulkan-docs` and the official
[pipeline state rules](https://docs.vulkan.org/spec/latest/chapters/pipelines.html)
confirm dynamic state persists in a command buffer until overwritten or
invalidated by a pipeline that makes it static. EOD uses one shared pipeline
layout and makes viewport/scissor/blend constants dynamic for every pipeline;
no multiview or secondary command buffers are used.
[Push descriptors](https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html)
are command-buffer state with compatible pipeline layouts. Cache actual image
view/sampler handles; deferred resource destruction keeps them alive until the
submission completes. Invalidate every cache at `vkBeginCommandBuffer`, including
after either in-flight command pool is recycled. Rendering begin/end and transfer
barriers do not invalidate these bindings. Future layout or dynamic-state changes
must revisit these assumptions.

Inspection rejected the proposed uniform-buffer-to-push-constant optimization:
EOD already uses a 128-byte push range; `Map(Buffer*)` exposes CPU vector storage,
and `Draw` already calls `vkCmdPushConstants`.

The existing pre-alpha native CPU sample
(`work/consolidated-native-race/race-cpu-sample.txt`) includes
`MVKCommandEncoder::encode`, `MVKCmdPushDescriptorSet::setContent` and descriptor
write cleanup, supporting command encoding as a candidate. It predates the
alpha fix and cannot quantify its current share. The cache's same-binary
on/off replay comparison establishes the measured improvement.

## Per-draw command cost and GPU-time check — October 16 continuation

Measurement correction first: an earlier Metal System Trace reading of "~42 ms
GPU busy per replay" summed the whole system's GPU intervals (WindowServer,
browser and desktop-app helpers were in the same trace) and nested interval
depths. Filtering `metal-gpu-intervals` to the `eod` process and depth 0 gives
~6.5 ms GPU per warm 2-game-frame race replay, 10% of wall time; surfaces in that
capture are 640x512 and 320x512, so full-surface render areas are not the cost.
The GS worker is bound by CPU recording/encoding and synchronous readback round
trips, not by GPU fragment throughput.

Applied wheels (concepts only, no source copied):

- PCSX2 Vulkan hardware renderer, pinned
  [`685637d`](https://github.com/PCSX2/pcsx2/blob/685637d30b5eaa8e5d7a587673a17577a92dcb88/pcsx2/GS/Renderers/Vulkan/GSDeviceVK.cpp):
  the alpha-test second pass is a different pipeline selector, and constant
  buffers are re-uploaded only when their cached contents change; vertices are
  written once to a stream buffer and draws select them with `firstVertex`.
  EOD now does the same: `ps_draw` has `[[vk::constant_id(0)]] c_atest_select`
  OR'd into `c_flags` (Vulkan build only, D3D11 unchanged), the pass/fail half is
  a pipeline specialization, push constants are skipped when byte-identical, and
  each batch's vertices are uploaded and bound once (`SetVertexWindow`).
- Vulkan spec, [pipeline layout compatibility](https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html#descriptorsets-compatibility):
  push constants stay valid across binds of pipelines with an identical push
  range/layout; vertex buffer bindings are command-buffer state. Both caches are
  invalidated at every `vkBeginCommandBuffer`; the vertex window is also
  invalidated at every submit (its upload chunk is recycled with the flight).
- Vulkan spec, [specialization constants](https://docs.vulkan.org/spec/latest/chapters/pipelines.html#pipelines-specialization-constants).

Ruled out with measurements on the race capture (same binary, alternating):

- MoltenVK 1.4.2 config: `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=1/2`,
  `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0` and
  `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0` were within noise (async submits only
  move ~9 ms of Metal encoding into the fence wait).
- Dolphin-style early command-buffer kicks (async submit after N draws, at pass
  boundaries or mid-pass): at most ~1.5 ms better at pass boundaries; mid-pass
  kicks every 250–1000 draws were slower (59–64 ms) because each extra Metal
  command buffer costs a ~0.2–0.3 ms round trip on this M2 Pro. Not shipped.
- `EOD_GPU_UPLOADS=1` reduces readbacks 19 to 7 but not wait time (bigger
  batches per sync); see `GS_CPU_WRITE_PRESERVATION.md`.
