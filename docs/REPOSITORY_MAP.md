# Repository guide

One repository contains the game, shared asset tools and Creation Kit.

| Branch | Purpose |
| --- | --- |
| main | Published game baseline |
| dev/creation-kit | Creation Kit and isolated test-runtime development |

## Downloads

- [Eve of Destruction Remaster](https://github.com/catsandsoup/eod-recomp/releases/tag/v1.0)
- [EOD Creation Kit preview](https://github.com/catsandsoup/eod-recomp/releases/tag/creation-kit-v0.3.2-preview)

The Kit is a separate application. It does not replace the game. Read each
release's requirements and feature limits before installing.

## Source layout

- src/, runtime/, config/: game and native runtime.
- tools/editor_macos/: macOS Creation Kit.
- tools/modding_backend.py and native-format helpers: asset reading and editing.
- tools/creation_kit/: editor checks.
- docs/: public build and format references.

Follow AGENTS.md for contribution and validation requirements. Keep runtime
experiments isolated; do not merge an entire experimental branch to ship an
editor-only fix. Historical archive tags are recovery references, not supported
product versions. Private planning and handoff documents are distributed locally.
