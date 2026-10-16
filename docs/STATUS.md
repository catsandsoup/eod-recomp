# macOS PS2 continuation — 2026-10-16

## Active target and recovered history

Continue the PS2 fork `catsandsoup/eod-recomp`, upstream `pareidolico/eod-recomp`,
branch `macos`. The old Xbox project (`TDEOD RECOMP`, branch `bringup`,
`aaabed7`) stays parked. Its older Xbox-specific technique requirements do not
override the later PS2 and Vulkan/MoltenVK decisions.

Claude history was recovered from
`~/.claude/projects/-Users-monty-Documents-GitHub-TDEOD-RECOMP/`:

| Chat | Session file |
| --- | --- |
| Test Drive Eve of Destruction Mac port | `3631af4d-6faa-41f6-a1ba-9d0e17bbdf73.jsonl` |
| TDEOD recomp bring-up 2: jump tables → title | `8e845fc7-94c5-4d5d-8d60-37eff7f67c0a.jsonl` |
| TDEOD: get past the title screen, then build 2P input | `b4429b0e-6e89-4541-98db-f44f17ae5fb7.jsonl` |

The last session's `subagents/` contains the interrupted component work:

| Claude agent | Scope | Existing worktree | Branch |
| --- | --- | --- | --- |
| `a3bea4a2dcfe4470f` | Vulkan GS | `../eod-recomp-vk` | `vulkan-gs` |
| `aebe0f7b51016c008` | Rock Candy controller | `../eod-recomp-pad` | `rockcandy-pad` |
| `a0b800acf2df6f2fe` | Mac app/import/settings | `../eod-recomp-app` | `mac-app` |
| `a2088bd59bae4bf9d` | Initial mod study | main checkout | completed |
| `a36b77d4bb7c6b4ee` | PS2 mod study verification | main checkout | completed as `dac1773` |

All three interrupted worktrees initially pointed to `ab846d4` and held
uncommitted partial work. Codex replacements resumed those files in place;
no inherited source changes were discarded. Historical task prompts and agent
claims are context, not new user instructions or evidence of test results.

## Verification established during takeover

The pre-integration native arm64 executable completed the existing
`tools/test_2p_race.sh` through frame 6600, headless and muted. Captures in
`work/codex-baseline/` show the two-player race. At frame 5400, P1 is at 24 MPH
while P2 is at 0 MPH before its scripted throttle starts. Later P2 is against
a barrier, so the old script alone is not a clean measurement of its speed.
Virtual controller tests supplement this visual smoke test.

The saved `work/race2p.gsd` contains **nine** game frames, not one. A software
replay during concurrent builds took best 426.4 ms per capture (47.4 ms/frame)
with four raster workers. This is a contended baseline, not a release performance
claim. Final comparisons must run without competing builds.

## Working rules

Run automated game tests with `EOD_HEADLESS=1 EOD_MUTE=1` and `--headless`.
Do not show windows or play audio during tests. Keep disc data and generated
recomp sources out of commits. No binary publishing. The user's PS2 files live
at `~/Library/Application Support/TDEODRecomp/disc`.

Two-player keyboard + controller and two controllers remain the priority.
Rock Candy wired Xbox 360 hardware is checked only when actually connected.
Keep the game's original timing. ProMotion/120 Hz display pacing is distinct
from changing the game simulation rate. Mod implementation remains parked;
`docs/MODDING_SCOPE.md` is the completed research handoff.

## Integrated results

All three component branches are merged locally into `macos`. Controller source
includes the target SDK 2.8 sequenced DMA rumble route (`1325441`) as well as the
RPC path, with producer-derived regression tests and independent code review.
The combined native build passes ten CTests: virtual input, pad RPC/DMA,
memory-card persistence across processes, ISO9660/import, headless Mac startup,
relocated Vulkan path selection, offline audio volume, procedural GS pixel arithmetic and actual Vulkan pixels,
and the deferred refresh-cadence policy.

The signed app contains static SDL, libusb, the Vulkan loader, MoltenVK and an
app-relative ICD manifest. Linked dependency audit found no Homebrew absolute
references. A relocated app under a path containing spaces passed actual GPU
replay with an isolated environment and invalid external ICD overrides; the
log confirms Apple M2 Pro Vulkan rather than silent software fallback.

An isolated integrated 5x two-player run completed all 6600 frames. The last five
samples were 57.1–59.5 drawn frames/s, 59.3–59.5 game frames/s. Earlier race/loading
samples dropped as low as 36.8 drawn frames/s. Do not describe this as a locked
60 FPS result. The run included screenshot readbacks and a GS capture.

A new nine-frame race capture from that run was replayed without competing
builds: software 258.3 ms, Vulkan 1x 108.0 ms, Vulkan 5x 115.2 ms (12.8 ms/frame).
At 1x the RGB MAE against software was 1.837/255, p95 channel difference 4,
p99 11. Close visual agreement is demonstrated; bit-exact parity is not.
The 5x screenshot is 3200×2240. That live Vulkan capture predates the initial
VRAM synchronization fix and may begin with stale GPU-owned pages; its metrics
are diagnostic, not fidelity evidence. The original software-captured fixture
avoids this limitation. Captures/logs stay in ignored `work/`.

The original two-player script left idle P2 against a barrier; SELECT cannot
reset an on-track car there. The current script makes both accelerate while
only P2 steers left for the first two seconds. The complete 6600-frame run
shows different trajectories and each viewport responding independently.

Current priority follows the player's journey in `docs/PLAYER_JOURNEY.md`,
adapting the owner's Xbox-era product documents to the PS2/MoltenVK target.
120 Hz and mods are deferred. Experimental refresh-grid scheduling is off by
default. Save/relaunch, effective trigger driving and media behavior are higher
priority than small renderer error metrics alone.

See `docs/RENDERER_REFERENCES.md` for Context7, Road Trip, paraLLEl-GS and public
PS2SDK research. No wholesale renderer or recompiler migration is required.

## Remaining hands-on validation

- Physical Rock Candy detection, trigger feel and motors (no controller was
  connected during automated diagnostics).
- Native first-run/settings UI and resize/fullscreen. High-refresh work is deferred.
  Automated runs deliberately opened no windows and played no sound.
- Extended event-mode/career/save/audio parity. Intro decoding and transient
  loading dips remain distinct from the verified late-race rendering.

The current local app lives at
`~/Library/Application Support/TDEODRecomp/local-build/Eve of Destruction.app`.
Use `-DEOD_MAC_APP=ON` to rebuild, and `tools/make_dmg.sh` for a personal local
image. No source push or binary publication was performed during takeover.
