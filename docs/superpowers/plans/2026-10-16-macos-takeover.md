# macOS PS2 takeover implementation plan

> **For agentic workers:** Continue the already dispatched component tasks in their existing isolated worktrees. Parent integrates and verifies on `macos`.

**Goal:** Resume the three interrupted Claude tasks and produce a locally usable Mac PS2 recomp with verified two-player input, packaging, and accelerated graphics.

**Architecture:** Preserve the upstream static recomp and GS hardware interface. Vulkan through MoltenVK ports the D3D11 renderer; SDL3 with libusb handles pads; a native app layer handles import, settings and bundling. Keep original game timing.

**Tech Stack:** C++17, Objective-C++, CMake/Ninja, SDL3, libusb, Vulkan/MoltenVK.

**Spec:** Existing `docs/MACOS.md`, user request to replace the Claude agents, and recovered component briefs in Claude session `b4429b0e-6e89-4541-98db-f44f17ae5fb7`.

## Global constraints

- Target PS2 SLUS_209.10 NTSC-U v1.00 on Apple Silicon; Xbox work stays parked.
- Tests headless and muted. No visible windows or sound during automation.
- Never commit disc files, generated recomp sources, captures or binaries.
  The owner authorized periodic source commits/pushes; no binary publication.
- Reuse branches/worktrees `vulkan-gs`, `rockcandy-pad`, `mac-app`.
- Two independent player ports, keyboard + controller and two controllers; Rock Candy 360 is the priority physical pad.
- Mod implementation remains parked; completed PS2 scope report is retained.

## Review focus

- Missing/wrong disc and cancelled import: friendly error and no damaged existing install.
- Bundle on a clean machine: all non-system dylibs resolve inside bundle.
- Keyboard/pad assignment after hotplug: existing player retains its port.
- Vulkan readback and GS memory synchronization: compare replay output to software.
- Renderer unavailable or explicit software selection: preserve working software path.

## Task 1: Controller takeover (`rockcandy-pad`)

Files: `CMakeLists.txt`, `runtime/platform/sdl3/sdl_input.*`, platform implementations, `runtime/iop/pad.cpp`, input tests.

Consumes: `HostPadState` and existing pad-script overrides.
Produces: libusb-capable SDL build, diagnostics, tested input/rumble interface.

- [x] Review inherited diffs; finish diagnostics and controller mapping.
- [x] Build pinned SDL with libusb, run virtual-pad and pad RPC tests.
- [x] Verify diagnostics without disc data; identify whether physical pad is connected.
- [x] Commit verified source and documentation only.

## Task 2: Mac app takeover (`mac-app`)

Files: `runtime/app/*`, `tests/iso9660_test.cpp`, app CMake resources/scripts, config/main startup and SDL settings event hook.

Consumes: existing `Config`, SLUS CRC check, platform initialization.
Produces: local .app, ISO/folder importer, native settings and persistent app paths.

- [x] Finish inherited importer, bound traversal and preserve existing install on failure.
- [x] Implement native startup/settings and local signed bundle packaging.
- [x] Run synthetic importer tests and real ISO import into scratch; verify matching ELF.
- [x] Run bundled executable headless; inspect dependency paths and signature.
- [x] Commit verified source and documentation only.

## Task 3: Vulkan takeover (`vulkan-gs`)

Files: `runtime/gs/vulkan/*`, shader compiler, GS routing, renderer config, minimal SDL window hooks, CMake Vulkan linkage.

Consumes: existing `gs_hw.h` API, D3D11 surface/cache semantics, offline shader modules.
Produces: offscreen/windowed Vulkan renderer, hardware readback and display composition.

- [x] Implement backend preserving GS surface ownership and blending/depth semantics.
- [x] Build and run offscreen GS replay, inspect output against software.
- [x] Correct demonstrated parity failures before selecting Vulkan by default.
- [x] Measure frames per capture correctly: existing race capture contains nine game frames.
- [x] Commit verified source; document measured performance and outstanding presentation limits honestly.

## Task 4: Parent integration

- [x] Read recovered Claude main and subagent histories; verify branch/worktree map and preserve existing changes.
- [x] Dispatch three replacements into the original component worktrees.
- [x] Complete independent baseline race and inspect player-specific movement screenshots.
- [x] Review each component diff, integrate commits with deliberate resolution of CMake/config/main/platform overlap.
- [x] Build integrated app, run input/import tests and headless two-player regression.
- [x] Inspect bundle dylibs/signature and Vulkan replay image/performance without concurrent builds.
- [x] Record exact verified results, remaining limits and local launch path in `docs/STATUS.md`.

No new plan approval pause: the user expressly requested replacement subagents and autonomous continuation; the recovered design already established these three tasks.


## Follow-on journey verification

The takeover tasks above are integrated; they do not establish a flawless port.
The owner's later product documents are applied through `docs/PLAYER_JOURNEY.md`.

- [x] Demonstrate distinct two-player steering through the real game.
- [x] Correct target SDK memory-card filename offsets and pad replies; test persistence across processes.
- [x] Correct double volume attenuation through the actual offline audio pipeline.
- [x] Correct demonstrated GS fog/interpolation errors and GPU capture synchronization.
- [ ] Save a named career in the game, quit, and load it in a new process.
- [ ] Offer and verify usable analog trigger driving without losing original control mappings.
- [ ] Optimize measured intro bitstream bottleneck while preserving full guest state and interrupt behavior.
- [ ] Compare procedural GS output with an independent accurate renderer.
- [ ] Rebuild and verify the final relocated local app after follow-on fixes.
- [ ] Validate native UI and physical controller behavior when permitted/available.

High-refresh presentation and mods remain deferred. Known GS blending and audio
model approximations prevent a complete 1:1 claim.
