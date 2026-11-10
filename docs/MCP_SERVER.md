# eod-creation-kit MCP server

Stdio MCP server (JSON-RPC 2.0, protocol 2025-06-18, stdlib only) that lets Claude Code drive the
Creation Kit backends. Design: `MCP_SERVER_SPEC.md`. Code: `tools/eod_mcp/`
(`server.py` transport, `tools.py` tools, `jobs.py` + `job_worker.py` child-process jobs,
`safety.py` guards/plans/audit, `content.py` resources and prompts).

## Register with Claude Code

Project scope (copy the example, then approve the server once in Claude Code):

    cp .mcp.json.example .mcp.json

or from the repo root:

    claude mcp add eod-creation-kit -s project \
      -e EOD_DISC="/path/to/extracted/disc" \
      -e EOD_KIT_WORKSPACE="$HOME/Library/Application Support/EODCreationKit" \
      -- python3 -I tools/eod_mcp/server.py

`${VAR}` and `${VAR:-default}` expansion in `.mcp.json` is a Claude Code feature; the example has no
machine-specific paths. Use `python3 -I` so a stray `json.py` or `tools/dis.py` cannot shadow the stdlib.
Check with `claude mcp list`.

## Environment

| Variable | Meaning |
|---|---|
| `EOD_DISC` | Extracted game folder (`DATA`, `SLUS_209.10`). Read-only, never a write target. |
| `EOD_KIT_WORKSPACE` | Workspace root (default `~/Library/Application Support/EODCreationKit`; `EOD_KIT_HOME` is accepted). All writes live here: `projects/ exports/ jobs/ profiles/ captures/ cache/ packages/ audit.jsonl`. Must not contain or sit inside the disc. |
| `EOD_KIT_ALLOW_RUN` | `1` enables `eod_test_run_start`. Default off: the tool is hidden and calls are refused. |
| `EOD_GAME_EXECUTABLE` | Built game binary used for runs (`EOD_GAME_EXE` accepted). Never taken from tool arguments. |
| `EOD_KIT_IMPORT_DIRS` | Optional extra read-only folders (colon separated) from which PNG/OBJ inputs may be read. |
| `EOD_MCP_HIDE_MODULES` | Test aid: comma list of backend modules to treat as absent. |

## Safety model (enforced in the server)

- The disc, installed apps (`/Applications`, `*.app`), other apps' support folders and anything
  outside the workspace are never written. Paths are workspace-relative; `..`, symlinks and
  case-fold collisions are refused (`UNSAFE_PATH`). Imports (PNG/OBJ) may be read from the workspace
  or `EOD_KIT_IMPORT_DIRS`.
- Profiles: names `dev-[a-z0-9-]+` with an `.eod-dev-profile` marker made by `eod_profile_create`.
- Every `[W:*]` tool defaults to `dry_run:true` and returns `plan_id`; applying needs that id
  (missing: `NEEDS_CONFIRM`; inputs changed, expired after 30 min or unknown: `STALE_PLAN`).
  Destructive tools (`eod_mod_uninstall`, `eod_profile_delete`) also need `confirm:"<exact name>"`.
- Game runs: only with `EOD_KIT_ALLOW_RUN=1` and `EOD_GAME_EXECUTABLE`; headless, muted, isolated
  memcard, hard timeout, one at a time, process group killed on cancel. A run succeeds only if a
  capture PNG exists; a clean exit alone is reported as failed.
- Every write (and job completion) appends to `audit.jsonl` with before/after fingerprints.
- Long work (census, texture export, sky/world/vehicle export, test run) is a child-process job with
  `progress.jsonl`; `eod_job_status` long-polls and mirrors `notifications/progress` when the call
  has a `progressToken`; `eod_job_cancel` sends SIGTERM then SIGKILL after 5 s (exports keep a
  resumable manifest). Jobs left running by a dead server are marked `failed: server_restart`.

## Tools

Exposure is capability-gated: a tool is listed only if every backend module it needs imports and
has the required functions; `eod_kit_status` reports `missing` with reasons.
Always present when their modules exist: orientation (`eod_kit_status eod_catalog eod_entries
eod_inspect eod_track_environment`), census (`eod_census_start eod_asset_search eod_asset_get
eod_asset_preview`), textures (`eod_texture_export_start eod_texture_replace eod_texture_tint
eod_texture_compare`), project/run (`eod_project_validate eod_launch_plan eod_test_run_start
eod_capture_get eod_capture_diff eod_log_tail`), jobs, packages (`eod_package_build/inspect/validate`),
profiles (`eod_profile_create/describe/delete`, `eod_conflicts_check`, `eod_mod_install/enable/disable/
set_order/uninstall`), and module-backed `eod_sky_export_start`, `eod_world_export_start`,
`eod_vehicle_export_start`, `eod_vehicle_list`, `eod_collision_query`, `eod_collision_edit`,
`eod_vif_patch_check`, `eod_vif_patch`.

Resources: `eod://status census audit docs/{name} jobs/{id} manifest/{run} export/{run}/{path}
asset/{archive}/{member}[/preview.png] project/{n} profile/{n} package/{p} run/{id}/{capture.png|game.log|meta.json}`.
Images are returned as base64 PNG (max 1024 px, 1.5 MB) plus resource links.
Prompts: `texture_pack_workflow share_mod new_livery debug_missing_replacement cleanup_profile`
(and `remove_boundary`, `adjust_props` when the world modules exist).

## Limitations

- Not exposed (backend absent or unverified): `eod_world_overview`, props list/transform, vehicle
  params/preview/import, `eod_texture_replace` host mode needs a build with host textures to be
  proven in game. `eod_collision_edit` and `eod_vif_patch` stage files under `exports/` and do not
  yet feed `eod_package_build`; results are `verified:false`.
- Plans live in server memory; restart the server and repeat the dry run.
- `eod_texture_export_start` dry run inventories the whole disc (hashes every file under `DATA`),
  which takes tens of seconds on retail data.
- Tests: `cd build && EOD_HEADLESS=1 EOD_MUTE=1 python3 ../tools/tests/test_eod_mcp.py`
  (`EOD_TEST_RETAIL_TEXTURES=1` adds the read-only retail census + ANOKA sky export).
