Read and follow docs/MASTER_SPEC.md strictly.

Subagent and context-budget rules (full text: docs/MASTER_SPEC.md §6, rules 8–10):
- No upfront bulk reads: `git status --short` + `git diff --stat` first, then only the needed line ranges of the 2–4 files the micro-task touches.
- Every stage is two runs: Phase A (spec, decompile, Plume feed, compile, one headless EOD_DIFF smoke test, findings on disk) and Phase B (multi-scene headless suite, success/STAR docs, commit to main, install with backup).
- Delta-only resumes: after a usage limit, check `git diff` and `docs/`, list the unfinished delta in <= 5 bullets, do only that.
