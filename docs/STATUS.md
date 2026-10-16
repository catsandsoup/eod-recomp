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
The fresh-chat handoff build passes all 22 CTests: virtual input, pad RPC/DMA,
memory-card persistence across processes, ISO9660/import, headless Mac startup,
relocated Vulkan path selection, offline audio volume, procedural GS pixel arithmetic and actual Vulkan pixels,
native camera selection (four aspect cases), keyboard context, scripted analog
pressure, movie full-state differential behavior, display composition and the
deferred refresh-cadence policy, exact physical-page coverage, alpha ordering/
disjoint grouping, and worker shutdown. This does not replace full-game speed
or clean-exit validation of the combined changes.

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
default. Custom single-player/MP matches, normal racing keyboard controls, 100% game
speed and 4K-or-higher 16:10/16:9 rendering are the latest priority. A basic
changed-name career save/load completed across processes; further career testing
belongs to the owner. Effective trigger driving and match presentation have
priority over further career exploration.

See `docs/RENDERER_REFERENCES.md` for Context7, Road Trip, paraLLEl-GS and public
PS2SDK research. No wholesale renderer or recompiler migration is required.

## Remaining hands-on validation

### Owner-reported defects under active investigation

The owner's October 16 playtest exposed stretched menus/gameplay, hollow font
interiors, thin black lines, slowdown when opponent cars appear, and delayed
keyboard response. None of these reports is dismissed as original artwork or
accepted as complete based on unit tests alone.

Matched software/Vulkan replay reproduced the font defect. Vulkan grouped all
alpha-pass primitives before all alpha-fail primitives, allowing earlier shadow
edges to overwrite later glyph fills. Ordered passes restore the captured font.
A proven constant-depth FB_ONLY optimization retains that exact corrected output
with the original menu draw count; variable-depth race batching remains expensive.
See `GS_ALPHA_ORDER.md` for the regression and performance limits.

The new 6x, 3840x2400 diagnostic MP run reached vblank 6600. Its last sample was
59.9 vblanks/s, 59.5 game frames/s and 44.8 drawn frames/s, before the ordered-alpha
fix. This is not full rendering speed. That run also exposed a normal-exit race:
the detached GS worker accessed a texture map during static destruction. Worker
shutdown now drains and joins producers/workers; the full game clean-exit check
must still be repeated on the integrated build.

Native projection tracing confirms race-mode recomputation with scale 5/6 for
16:10. Menu artwork uses 4:3 composition; actual world and HUD proportions still
require matched visual confirmation. Headless scripted input cannot establish
keyboard-event-to-visible-image latency. Excluding screenshot/dump intervals,
the diagnostic race's worst observed pump gap was 35.6 ms, GS block 26.1 ms and
pump-plus-pad-DMA 59 microseconds. The 367 ms peak coincided with intentional GS
capture and is not evidence of ordinary input delay.

Exact physical-page enumeration removes unrequested neighbour-page reads and
ownership drops. A separate procedural test demonstrates lost pixels when a CPU
write modifies part of a GPU-owned page; preservation of the remaining bytes is
still being repaired. This is evidence of a renderer bug, not yet proof of the
specific black-line cause. Keep the full quality target while diagnosing these
issues; do not hide them by reducing opponents, resolution or simulation speed.

- Physical Rock Candy detection, trigger feel and motors (no controller was
  connected during automated diagnostics).
- Native first-run/settings UI and resize/fullscreen. High-refresh work is deferred.
  Automated runs deliberately opened no windows and played no sound.
- Extended event-mode/career/save/audio parity. Intro decoding and transient
  loading dips remain distinct from the verified late-race rendering.

The current local app lives at
`~/Library/Application Support/TDEODRecomp/local-build/Eve of Destruction.app`.
Use `-DEOD_MAC_APP=ON` to rebuild, and `tools/make_dmg.sh` for a personal local
image. The owner subsequently authorized periodic source checkpoints; verified changes
are pushed to the personal fork's `macos` branch. No binary publication was
performed.
