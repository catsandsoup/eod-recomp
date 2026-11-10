# Creation Kit development boundary

Player success: the accepted EOD Modding Suite and game continue to work while development builds use their own app identity, preferences, project and output folders. No UI redesign or engine modernization is part of this separation.

## Products in this repository

- Game: `src/`, `runtime/`, `config/`, generated-code and game build tools. Gameplay/rendering/startup experiments are reviewed independently.
- Shared native format tools: `tools/rst0.py`, `valt.py`, `tex.py`, `texture_preview.py`, `vif_mesh.py`, and their tests. Preview decoding is not arbitrary geometry compilation.
- Creation Kit: `tools/editor_macos`, `tools/modding_backend.py`, `tools/build_modding_app.py`, and `tools/creation_kit`. The JSON request/result boundary is currently an internal interface, not a promised stable public API.
- Legacy C++ editor: `tools/editor`. Useful components remain available; this experimental prototype is opt-in and does not supply the Swift app's backend.
- Research: `docs/`. Proposed interfaces and completion claims require source/runtime corroboration.

## Build and check separately

From the Kit checkout:

```sh
cmake -S tools/creation_kit -B build/creation-kit/checks
EOD_HEADLESS=1 EOD_MUTE=1 ctest --test-dir build/creation-kit/checks --output-on-failure
EOD_HEADLESS=1 EOD_MUTE=1 python3 tools/build_modding_app.py --game-executable /absolute/path/to/existing/eod
```

The packager does not build/copy the game, install an app, open it, or modify user defaults. Output is `build/creation-kit/EOD Creation Kit Development.app`; Swift build products are under `build/creation-kit/swift`. It rejects symlinks in these output paths. Rerunning replaces only the development bundle, retaining one previous development bundle.

Development bundle and preference suite: `org.eod-recomp.creationkit.development`.
Default project: `mods/CreationKitDevelopment/project.eodproject` within the Kit checkout. A game executable must be provided explicitly when packaging. An unbundled development process has no automatic game executable; use the existing chooser. Original extracted assets are read as sources; compiled overlays, caches, test configuration and isolated test memory cards live beside the chosen project. A user can explicitly open another project, so use a copy when testing someone else's work.

The Kit's launch plan uses existing overlay/direct-start contracts. The track-environment development launcher now opts into `EOD_EDITOR_TEST` on its separate runtime; the accepted game is unchanged. See TRACK_ENVIRONMENT_WORKFLOW.md for native image evidence and desktop acceptance limits. Skipping videos is not skipping resource loading. Native code changes, faster loading and new geometry/event compilers require separate review and acceptance.

Normal fresh game configuration defaults `EOD_BUILD_EDITOR=OFF`; existing cached CMake values persist until explicitly changed. To work on the legacy editor independently:

```sh
cmake -S tools/editor -B build/legacy-editor
cmake --build build/legacy-editor
```

## Development and promotion

Keep one repository with `main` as published source, a Kit branch/worktree, and a separate branch/worktree for game runtime experiments. Never deploy a development bundle over the accepted Modding Suite. Never merge unrelated runtime experiments to make an editor change look complete. Preserve known working app/game/project snapshots before promotion; they contain retail-derived output and belong in ignored local storage, not commits.

The copied recovery UI is a candidate, unchanged by this isolation task. Packaging and tests do not make its UI accepted. The currently accepted app stays installed; no development bundle is launched by this task. Windows packaging/UI remains future work.

## Handoff (STAR)

Situation: accepted binaries coexisted with mixed uncommitted editor and game experiments.
Task: separate development while preserving the current app, UI and game.
Action: isolate editor/shared-tool candidates; separate build, identity, preferences and default project; keep runtime edits outside the Kit branch; preserve rollback snapshots and verify hashes.
Result: see the dated verification report for executed tests, native evidence and remaining limits. Next decisive acceptance is an explicitly chosen development-app trial; this separation does not authorize replacing the accepted app.
