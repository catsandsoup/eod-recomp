# macOS PS2 takeover implementation plan

> **For agentic workers:** Continue the already dispatched component tasks in their existing isolated worktrees. Parent integrates and verifies on `macos`.

**Goal:** Resume the three interrupted Claude tasks and produce a locally usable Mac PS2 recomp with verified two-player input, packaging, and accelerated graphics.

**Architecture:** Preserve the upstream static recomp and GS hardware interface. Vulkan through MoltenVK ports the D3D11 renderer; SDL3 with libusb handles pads; a native app layer handles import, settings and bundling. Keep original game timing.

**Tech Stack:** C++17, Objective-C++, CMake/Ninja, SDL3, libusb, Vulkan/MoltenVK.

**Spec:** Existing `docs/MACOS.md`, user request to replace the Claude agents, and recovered component briefs in Claude session `b4429b0e-6e89-4541-98db-f44f17ae5fb7`.

## Global constraints

- Target PS2 SLUS_209.10 NTSC-U v1.00 on Apple Silicon; Xbox work stays parked.
- Tests headless and muted. No visible windows or sound during automation.
- Never commit disc files, generated recomp sources, captures or binaries. No publishing.
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

- [ ] Review inherited diffs; finish diagnostics and controller mapping.
- [ ] Build pinned SDL with libusb, run virtual-pad and pad RPC tests.
- [ ] Verify diagnostics without disc data; identify whether physical pad is connected.
- [ ] Commit verified source and documentation only.

## Task 2: Mac app takeover (`mac-app`)

Files: `runtime/app/*`, `tests/iso9660_test.cpp`, app CMake resources/scripts, config/main startup and SDL settings event hook.

Consumes: existing `Config`, SLUS CRC check, platform initialization.
Produces: local .app, ISO/folder importer, native settings and persistent app paths.

- [ ] Finish inherited importer, bound traversal and preserve existing install on failure.
- [ ] Implement native startup/settings and local signed bundle packaging.
- [ ] Run synthetic importer tests and real ISO import into scratch; verify matching ELF.
- [ ] Run bundled executable headless; inspect dependency paths and signature.
- [ ] Commit verified source and documentation only.

## Task 3: Vulkan takeover (`vulkan-gs`)

Files: `runtime/gs/vulkan/*`, shader compiler, GS routing, renderer config, minimal SDL window hooks, CMake Vulkan linkage.

Consumes: existing `gs_hw.h` API, D3D11 surface/cache semantics, offline shader modules.
Produces: offscreen/windowed Vulkan renderer, hardware readback and display composition.

- [ ] Implement backend preserving GS surface ownership and blending/depth semantics.
- [ ] Build and run offscreen GS replay, inspect output against software.
- [ ] Correct demonstrated parity failures before selecting Vulkan by default.
- [ ] Measure frames per capture correctly: existing race capture contains nine game frames.
- [ ] Commit verified source; document measured performance and outstanding presentation limits honestly.

## Task 4: Parent integration

- [x] Read recovered Claude main and subagent histories; verify branch/worktree map and preserve existing changes.
- [x] Dispatch three replacements into the original component worktrees.
- [ ] Complete independent baseline race and inspect player-specific movement screenshots.
- [ ] Review each component diff, integrate commits with deliberate resolution of CMake/config/main/platform overlap.
- [ ] Build integrated app, run input/import tests and headless two-player regression.
- [ ] Inspect bundle dylibs/signature and Vulkan replay image/performance without concurrent builds.
- [ ] Record exact verified results, remaining limits and local launch path in `docs/STATUS.md`.

No new plan approval pause: the user expressly requested replacement subagents and autonomous continuation; the recovered design already established these three tasks.
