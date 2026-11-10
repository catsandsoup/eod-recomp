# Repository map and safe handoff

Updated 10 November 2026, Australia/Sydney. One repository, two active branches.

| Branch | Purpose | Start here |
| --- | --- | --- |
| `main` | Published game source baseline; preserve working app/game | `AGENTS.md`, then `docs/DEVELOPER_HANDOFF.md` for game research |
| `dev/creation-kit` | Creation Kit and its isolated development runtime | `docs/handoffs/2026-11-10-claude-opus/START_HERE.md` on that branch |

[Open the Kit source and current handoff](https://github.com/catsandsoup/eod-recomp/tree/dev/creation-kit/docs/handoffs/2026-11-10-claude-opus).
The Kit has its own app identity and build paths. It does not need another repo.
Do not merge the whole development branch into main simply to tidy GitHub.
Promote specific verified changes only through a reviewed integration decision.

## Releases

- [Game v1.0](https://github.com/catsandsoup/eod-recomp/releases/tag/v1.0): existing game download; unchanged by this cleanup.
- [Creation Kit 0.3.1 development preview](https://github.com/catsandsoup/eod-recomp/releases/tag/creation-kit-v0.3.1-preview): separate macOS arm64 app; packaged runtime requires macOS 27. Source/package checkpoint `aac177aed793158fda91397a46e431963b42a29e`. Subsequent documentation commits clarify the handoff.

Older preview releases remain historical artifacts. Their existence is not an
instruction to use them or evidence that current features are complete. Read
release-specific limitations; the Kit preview is not the stable game release.

## Local checkouts

- `/Users/USER/Documents/GitHub/eod-recomp`: clean `main`; this is the checkout previously showing 11 changed files in GitHub Desktop.
- `/Users/USER/.codex/worktrees/creation-kit-isolation/eod-recomp`: clean `dev/creation-kit`. Its directory name remains unchanged to preserve build caches, installed app configuration and ongoing project paths.

Both worktrees are intentional. Do not delete or move the Kit worktree as
“cleanup”: ignored projects/builds and the running app depend on its location.
Ignored files are not automatically disposable. Never run a blanket git clean.

## Archived runtime experiments

The former `codex/game-runtime-experiments` branch contained 11 uncommitted
source/test files. They are preserved byte-for-byte in commit `72d6f7e`, tagged
and pushed as `archive/runtime-experiments-2026-11-10`. This is an **unvalidated
historical snapshot**, not a third active development branch. Parts were
superseded by later Kit runtime work; do not merge it wholesale.

Inspect without changing a checkout:

    git show archive/runtime-experiments-2026-11-10
    git diff main...archive/runtime-experiments-2026-11-10

For recovery, make a new branch from that tag. The old branch label was removed
only after confirming every file matched the archive. Local patch, untracked
file copies, source hashes and a Git bundle also live outside the repo at:
`/Users/USER/Documents/Codex/2026-11-10/read/outputs/github-cleanup/`.
The bundle predates the documentation/branch-name cleanup and contains the
archive commit and then-existing refs. It contains source history, not ignored
projects, extracted disc data or app binaries.

## Claude preflight

1. Read `AGENTS.md` and this map; run `git status --short --branch` and `git worktree list`.
2. For Kit work, use `dev/creation-kit` and read its dated START_HERE, HANDOFF, AGENT_BRIEFS and ACCEPTANCE documents.
3. If there are unexpected changes, inspect and preserve them. Never reset or overwrite another agent's work.
4. Preserve accepted binaries, disc assets, projects and saves. Work only in separate Kit outputs and test profiles.
5. Distinguish supported behavior from proposals and historical claims. Report evidence and remaining gaps.

This cleanup changes Git organisation and documentation. It does not validate or
promote the archived runtime code, rebuild the game, or alter the working app.
