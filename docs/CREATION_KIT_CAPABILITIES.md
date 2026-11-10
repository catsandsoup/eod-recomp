# Creation Kit capability contract

The target is an integrated world/asset authoring and native playtest tool. Current development software is an incremental implementation, not completion of that target. The preserved EOD Modding Suite is a separate older app and intentionally retains its earlier code until a promotion is requested.

For each supported operation, establish the native resource type and version, references/ownership, load/use/unload paths, serialization, units/meaning, failure recovery and real gameplay acceptance. Generic native-field access is an expert fallback, not a replacement for verified semantic editors. Do not invent meanings for unknown bytes or label proposals as implemented.

Unchanged export/import should be byte-exact where promised. Edited output must preserve unrelated data and reproduce intended native semantics. Successful self-roundtrip, source labels and clean process exit do not establish those semantics alone.

## Proven / bounded in this development task

- Anoka track loads in the actual native runtime; 32 existing sky tile palettes are edited and saved to an isolated native archive overlay. Other payloads and original archive remain unchanged.
- Supported indexed4/8-bit textures export to PNG, accept same-size RGBA PNG replacement where upload layout is representable, preserve original metadata/layout and rebuild existing mips. Indexed palette quantization can be lossy. Direct-color import/HDR is not implemented.
- Existing VALT tables retain editable native rows. Friendly controls only carry verified meaning; arbitrary string fields are not a universal safe gameplay compiler.
- Real texture and recovered model previews exist. Complete vehicle assembly with all shared attachments/materials is not established by displaying one MODL.

## Not achieved

- Complete independent reconstruction of every resource type/version/lifecycle.
- Arbitrary custom track/world geometry, collision, terrain or placement authoring.
- Universal cross-game model conversion; component, rig, collision, material and behaviour mapping still need supported import pipelines.
- A playable boat merely because a boat-shaped model exists; vehicle registration and suitable physics/controls are separate requirements.
- Removal of 32-MiB guest addressing,4-MiB GS storage and all inherited pool/protocol constraints.
- Native4K replacement textures, PBR materials, HDR environment lighting or a modern renderer.
- Editable live3D world viewport. A native race capture is labelled as a capture; play runs in the real separate game.

## Main game promotion

Correctness fixes and proven capabilities may later be promoted independently. No automatic replacement of the working game/app is authorized by a research discovery. Startup presentation should be optional and separately tested; suppressing movies/loading presentation does not eliminate resource initialization.

The acceptance ledger for Anoka and current startup status is in TRACK_ENVIRONMENT_WORKFLOW.md. Never use this target specification as proof all rows are implemented.
