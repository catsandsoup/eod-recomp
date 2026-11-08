# 1.0 release acceptance

## Player success

Install by choosing the owner's North American PS2 ISO; drive a locally controlled
car in the selected online event; start and return to the same party without
Career menus, overlapping grid seats or a permanent return banner. Keep invite,
roster, rules and recovery messages available with a controller. Optional online
advantages belong to the host; personal music belongs to each machine.

This release's source is identified by the `v1.0` tag on
`catsandsoup/eod-recomp`. Published binaries and checksum inventory accompany that
tag. Marketing copy and screenshots are in README.md; this file records evidence.

## Verification

- Recompiled SLUS_209.10 into a separate output directory: 13,481 functions,
  zero generation errors. All 119 output files matched the compiled generated
  tree byte for byte. Generated code and disc files remain outside Git.
- Native Mac build and all 83 CTests passed, including AssetTools. After the final
  portrait-only changes, 12 focused frontend, ISO, content, rule, music, voice and
  network checks passed again. The publication script repeats the complete suite.
- Windows x64 game, setup and all test executables built. A fresh run of all
  40 executable CTests through CrossOver passed. Test-only runtime DLLs are not
  dependencies of either shipped executable; package imports are Windows system
  libraries only.
- Final Mac host + packaged Windows guest: two actual GOs, two races and two native
  lobby returns on both machines, normal exits. Reverse Windows host + Mac guest:
  the same result. Both use the final protocol, content fingerprint, grid and
  ownership code. These are CrossOver/loopback checks, not native Windows/WAN
  measurements.
- Long native pair: 8,700 host / 9,000 guest simulation ticks, separate throttle,
  brake and left/right steering blocks, valid positions and fresh snapshots after
  the earlier 5,100-frame failure point. Same-car rolling and different-car
  standing Battle starts were exercised. The 14-car grid's minimum initial center
  separation was 5.248 m.
- Actual host protocol tests: 15 guest admissions, overflow refusal, duplicate
  names, rename, same-endpoint reconnect, host-only rule changes, capacity clamp,
  targeted kick and stale-generation rejection. A 16-person party attempting a
  14-seat event received no GO and returned to the lobby with a visible reason.
  Those guests were protocol clients rather than 15 human-driven game instances.
- Exact packaged Windows ISO checkpoint: Unicode executable/ISO/LocalAppData paths,
  128 installed files / 1,939,523,956 bytes matching the owner's source SHA-256.
  Return launch succeeded without the ISO and without rewriting installed files.
  A later portrait-only package revision was rebuilt and integrity-checked;
  its distinction is retained in ISO_SETUP_ACCEPTANCE.md.
- Personal Mac app bundled with its dependency closure, version 1.0, verified
  signature, and preserved owner content. Installed-app smoke uses a PATH without
  Homebrew, isolated configuration/saves and a 300-frame headless launch.
- Native Moth portrait visually checked in a 1920×1344 game-rendered Online screen.
  Missing Trailer2 and custom Cupcar swatches explicitly show no preview rather
  than a body texture or a misleading track picture.

Local evidence lives under the current chat workspace's `work/`: final-v1-mac-host,
final-v1-windows-host, driver-grid-long-input, driver-grid-standing-different,
lobby-protocol-v5-final, lobby-protocol-v5-visual, car-portraits-final-3x,
windows-release-final, installed-v1.0-smoke and physical-audio.

## Audio evidence boundary

Both Mac roles captured the physical built-in microphone through actual game
transport into the remote game mixer. A separate acoustic mic→Opus→UDP→speaker→mic
probe passed. An independent acoustic test of the actual game's delayed speaker
output detected the signal but did not meet the chosen isolation criterion against
other game audio. It remains inconclusive. Native Windows audio hardware, a fresh
packaged-app permission journey, external device loss and WAN audio are unverified.
See VOICE_AND_MUSIC_ACCEPTANCE.md; packet counters are not speaker evidence.

## Remaining acceptance

No claim of perfect netplay or universal performance parity is made. Natural finish
for every event, results/collision authority, three-plus physical clients, high
occupancy race performance, network impairment, native Windows peripherals, and
the original 19–20 FPS report remain on INVESTIGATION_QUEUE.md. Native car portrait
assets are incomplete; runtime 3D thumbnail generation and automatic mod transfer
are absent. Content is assumed read-only during a session. The compatibility
checksum detects accidental differences, not a malicious modified client.

## STAR handoff

Situation: prior progress reports mixed implemented code, short tests and physical
acceptance, while long races and native menu transitions still failed.
Task: ship a coherent 1.0 from one source revision and retain an honest evidence
boundary for remaining work.
Action: scoped driver/grid, ISO and audio agents; integrated nickname/rule/content/
capacity/moderation state; reviewed rendered output and corrected missing/false
portraits; verified native/cross-platform starts and returns; packaged both OSes.
Result: the specifically listed checks passed. The full release script publishes
both archives together, with checksums, after source commit and push. Next decisive
acceptance is a three-machine natural-finish race with network impairment, followed
by native Windows microphone/speaker and controller checks.
