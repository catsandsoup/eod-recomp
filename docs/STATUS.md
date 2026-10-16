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

## Integration in progress

See `docs/superpowers/plans/2026-10-16-macos-takeover.md`. Component branches
are reviewed and merged locally before a final integrated build, input/import
tests, offscreen GPU replay comparison, bundle audit and two-player run.
