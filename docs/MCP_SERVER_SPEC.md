# `eod-creation-kit` MCP server: specification

Status: design, 2026-11-10. Target: Phase 1 implementable in about a week by one
worker. Tools that wrap modules still being built (`world_*`, `vehicle_*`,
`vif_patch`, `mod_install`, HOSTTEX) are **Phase 2** and must be registered only
when the module imports successfully (capability gating, section 3.1). Companion
user-journey doc: `MODDER_JOURNEYS.md`. Frozen data contracts (assetId, indexKey,
export manifest, `eodmod.json`, dev profile layout) are in
`CREATION_KIT_TEXTURE_CONTRACTS.md` and are not redefined here.

## 1. Goals and non-goals

Goals: let Claude Code drive the Kit's existing Python backends as small,
composable, schema-checked tools with a dry-run before every write, long jobs
behind handles, and images returned so the model (and human) can see results.

Non-goals: no new format knowledge, no arbitrary code execution tool (unlike
Blender MCP's `execute_blender_code`, which that project itself warns is
dangerous and gates behind an optional safe mode), no network, no GUI control,
no editing of the disc or installed apps.

## 2. SDK choice: minimal JSON-RPC, not the official `mcp` package (for now)

| | Official `mcp` Python SDK | Minimal stdlib JSON-RPC (recommended) |
|---|---|---|
| Install | `pip install mcp`; Python 3.11+; pulls pydantic, anyio, httpx, starlette etc. Not installed in this environment (`import mcp` fails; Python 3.13.7 is). | zero deps; the repo's tools are already stdlib-first (`json`, `struct`, `hashlib`) |
| Effort | decorators from type hints; progress/context helpers | about 350 lines: newline-delimited JSON-RPC over stdio |
| Risk | dependency/version churn in a repo shipped to modders; venv bootstrap | we must track the spec ourselves (small surface) |
| Protocol features needed | tools, resources, prompts, progress, cancel, logging | same; all are plain notifications |

Recommendation: **minimal implementation**, protocol version `2025-06-18`
(negotiate down to `2024-11-05` if the client asks), structured so that all tool
logic lives in `tools/eod_kit/` plain functions with JSON-schema dicts and
`server.py` is only the transport. Swapping to the SDK later is then a ~100-line
adapter (Phase 3, only if we need Streamable HTTP, OAuth or sampling).

Minimal server must implement: `initialize` / `notifications/initialized`,
`ping`, `tools/list`, `tools/call`, `resources/list`, `resources/templates/list`,
`resources/read`, `prompts/list`, `prompts/get`, `notifications/cancelled`
(inbound), `notifications/progress` (outbound, only when the caller sent a
`progressToken`), `notifications/tools/list_changed` and
`notifications/resources/list_changed` (outbound), `logging/setLevel` and
`notifications/message` (optional). stdout carries only protocol; all logs go to
stderr and `jobs/<id>/job.log`. Tool calls run on a worker thread pool so
`ping`/`cancelled` are never blocked; one in-flight write per project (lock).

Protocol facts relied on (MCP spec): tools declare `inputSchema`, optional
`outputSchema`, `annotations`; results carry `content` blocks (text, image,
`resource_link`, embedded resource) plus `structuredContent`, and servers
returning structured content should also return it serialised as a text block;
execution failures use `isError: true`, protocol failures use JSON-RPC errors;
progress uses `_meta.progressToken` and monotonically increasing `progress`.
Sources: https://modelcontextprotocol.io/specification/2025-06-18/server/tools ,
https://modelcontextprotocol.io/specification/2025-06-18/basic/utilities/progress ,
SDK: https://github.com/modelcontextprotocol/python-sdk .

## 3. Design rules (apply to every tool)

1. **Small, composable, one verb each.** `eod_texture_export` exports; it does
   not also package. Claude chains them.
2. **Structured first.** Every result is `structuredContent` (documented
   schema) plus the same JSON as a text block. Human-facing summary in a
   leading `summary` string, at most 200 chars.
3. **Images as image blocks and resource links.** Previews are returned as
   `{"type":"image","mimeType":"image/png","data":...}` (downscaled to max
   1024 px, capped 1.5 MB) plus a `resource_link` to the full file
   (`eod://...`). Large artifacts are never inlined.
4. **Side-effect class** is declared in `annotations` and in the tool
   description prefix: `[R]` read-only, `[W:project]` writes the workspace,
   `[W:profile]` writes a dev profile, `[RUN]` runs the game. MCP annotations:
   `readOnlyHint`, `destructiveHint`, `idempotentHint`, `openWorldHint:false`.
   Annotations are hints only (clients must treat them as untrusted); real
   enforcement is server-side (section 7).
5. **Two-phase writes.** Every `[W:*]` tool takes `dry_run` (default **true**).
   A dry run returns `plan` (exact files/rows to change, before/after hashes,
   warnings) and a `plan_id` (hash of normalised args + source hashes). To
   apply: call again with `dry_run:false, plan_id:<id>`. The server rejects an
   apply whose `plan_id` is missing, stale (inputs changed) or older than 30
   minutes. Destructive tools (`uninstall`, `profile_delete`, `project_reset`)
   additionally need `confirm:"<exact name>"` echoing the target name.
6. **Long tasks are jobs** (section 5). Tools ending `_start` return at once.
7. **Idempotent and resumable** where the backend allows (export resumes by
   manifest, per the contracts: never overwrite a file whose hash differs
   unless resuming the same asset).
8. **Errors.** `isError:true` with `{code, message, hint, details}`; codes:
   `NOT_CONFIGURED`, `DISC_NOT_FOUND`, `UNSAFE_PATH`, `STALE_PLAN`,
   `NEEDS_CONFIRM`, `UNSUPPORTED`, `CONFLICT`, `JOB_NOT_FOUND`,
   `CAPABILITY_UNAVAILABLE`, `BACKEND_ERROR`. Hints say the next tool to call.
9. **Descriptions teach.** First line states what and the side-effect class;
   second line states preconditions and the next likely tool.

### 3.1 Capability gating

At startup the server probes for each backend module (`asset_registry`,
`texture_export`, `sky_export`, `mod_package`, `mod_install`, `world_graf`,
`world_export`, `world_collision`, `vehicle_export`, `vehicle_import`,
`vif_patch`, game executable, HOSTTEX support flag) and exposes only tools whose
dependencies are present. `eod_kit_status` reports `available` and `missing`
with reason. Updating the set emits `notifications/tools/list_changed`. This
lets Phase 1 ship before Phase 2 modules land.

## 4. Configuration and workspace

Environment (set in `.mcp.json`): `EOD_DISC` (extracted game folder with `DATA`
and `SLUS_209.10`; read-only), `EOD_KIT_HOME` (default `~/Library/Application
Support/EODCreationKit`), `EOD_GAME_EXE` (built macOS binary, for `[RUN]`),
`EOD_KIT_ALLOW_RUN` (`1` to enable `[RUN]`; default `0` until the user opts in).

Workspace layout under `EOD_KIT_HOME`:
```
projects/<name>/project.json     # existing backend project file (edits)
exports/<run-id>/                # texture/world/vehicle exports, manifests
jobs/<job-id>/{job.json,job.log,progress.jsonl,result.json}
profiles/<name>/...              # per contracts section 6; marker .eod-dev-profile
captures/<run-id>/{capture.png,game.log,meta.json}
cache/census.json  cache/census.meta.json (disc slusSha256 + tool version)
```

## 5. Jobs

Long tools (`census`, `texture_export`, `world_export`, `vehicle_export`,
`package_build` of large packs, `test_run`) are `*_start` and return
`{job_id, kind, state:"queued"}`. Jobs run in a child process (so cancel is a
real signal and a crash cannot take the server down), write
`progress.jsonl` lines `{ts, done, total, message, current}`, and finish with
`result.json`. States: `queued running succeeded failed cancelled`.

- `eod_job_status {job_id, wait_seconds?<=25, since?}` returns state, `done`,
  `total`, `message`, last 20 log lines, and `result_link` when finished.
  `wait_seconds` long-polls so Claude does not spin.
- `eod_job_cancel {job_id}` sends SIGTERM, then SIGKILL after 5 s; partial
  outputs are kept and the manifest marks them; resumable.
- `eod_job_list {state?}`.
- If the originating `tools/call` carried a `progressToken`, the server also
  mirrors progress as `notifications/progress` while that call is open; for
  `*_start` calls, which return immediately, progress is delivered via
  `job_status` and the `eod://jobs/<id>` resource (clients may subscribe).
- Jobs survive server restart: on startup, `running` jobs whose pid is gone are
  marked `failed: server_restart` and are resumable if the kind supports it.
- Client-side cancel (`notifications/cancelled` for a request id) cancels
  in-flight synchronous tools; for `*_start` the model must call `job_cancel`.
- One game process at a time (`test_run`); queue others.

## 6. Tools

Notation: `[R]` read-only, `[W:project]`, `[W:profile]`, `[RUN]`. Phase P1
(week 1), P2 (as modules land, weeks 2-4), P3 later. Backend column maps to
existing code. All inputs are objects with `additionalProperties:false`. `disc`
is never an input; it is `EOD_DISC`.

### 6.1 Orientation (P1, all [R])

| Tool | Input | Output | Backend |
|---|---|---|---|
| `eod_kit_status` | `{}` | `{kitVersion, disc:{path,slusSha256,ok}, workspace, available[], missing[{tool,reason}], runEnabled, activeProfile?, openProject?}` | probes; `environment` not used |
| `eod_catalog` | `{kind?: "car"\|"res"\|"trk"}` | `{archives:[{path,name,count,size}], assetCount, errors}` | `catalog` |
| `eod_entries` | `{archive}` | `{entries:[{name,type,size,crcOk}]}` | `entries` |
| `eod_inspect` | `{archive, member, image?:bool}` | `{type, size, sha256, header, fields?, preview?}` + image block for TEX/MODL | `inspect` (+`texture_preview`) |
| `eod_track_environment` | `{archive}` (e.g. `DATA/TRACKS/ANOKA.TRK`) | `{track, skyTextures[], ...}` | `environment` |

### 6.2 Census and search (P1 if `asset_registry` lands; [R] except start)

| Tool | Input | Output |
|---|---|---|
| `eod_census_start` | `{refresh?:bool, jobs?:int}` | job handle; writes `cache/census.json` |
| `eod_asset_search` | `{query?, role?: sky\|vehicle-paint\|vehicle\|track\|ui\|font\|other, type?, archive?, min_w?, min_h?, shares_palette_with?:assetId, duplicate_of?:assetId, limit?<=200, cursor?}` | `{assets:[{assetId,type,role,roleConfidence,width,height,format,duplicateOf[],runtimeKeyShared[]}], next?}` |
| `eod_asset_get` | `{asset_id}` | full census record per contracts section 1-3, plus `previewLink` |
| `eod_asset_preview` | `{asset_ids[<=12], sheet?:bool}` | image block(s); `sheet:true` returns one labelled contact sheet |

Search is over the cached census (never rescans the disc); if no cache,
`NOT_CONFIGURED` with hint `eod_census_start`.

### 6.3 Textures

| Tool | Class | P | Input | Output |
|---|---|---|---|---|
| `eod_texture_export_start` | [W:project] job | P1 | `{asset_ids?[], role?, archive?, all?:bool, out?:"exports/<id>" (relative only), resume?:run_id, dry_run?:true}` | dry-run: counts, estimated bytes, collisions; apply: job handle; result = `export-manifest.json` link + summary counts by status |
| `eod_texture_replace` | [W:project] | P1 | `{asset_id, png (workspace-relative path), mode:"native"\|"host", project?, dry_run, plan_id}` | plan: size check (native must match; host must be `orig*k`, k power of 2, <=4096), quantisation loss warning for indexed, alpha handling, identity keys; apply: staged edit in project |
| `eod_texture_tint` | [W:project] | P1 | `{asset_id\|asset_ids[]\|sky:true+archive, rgb:[r,g,b], strength?, dry_run, plan_id}` | wraps `prepare_texture` (tint, `allSky`); returns before/after image |
| `eod_texture_compare` | [R] | P1 | `{asset_id, png}` | image block side by side + PSNR; flags palette overflow |
| `eod_sky_export_start` | [W:project] job | P2 | `{archive, out?}` | job; `sky_export` output |

Notes: `mode:"host"` requires HOSTTEX capability; `mode:"native"` is the proven
path today (`prepare_texture` + `save`). Responses always say which path.

### 6.4 World (P2; names follow the planned `world_graf/world_export/world_collision`)

| Tool | Class | Input | Output |
|---|---|---|---|
| `eod_world_export_start` | [W:project] job | `{track, out?, textures?:bool}` | OBJ/MTL path, counts, bounds, units |
| `eod_world_overview` | [R] | `{track, layer?: props\|collision\|boundaries\|ramps, highlight?[ids], size?}` | top-down annotated image + legend + ids |
| `eod_props_list` | [R] | `{track, name_like?, kind?, near?:{x,z,radius}, bbox?, limit?}` | `[{id, model, kind, kindConfidence, pos, rot, scale, bounds}]` |
| `eod_props_transform` | [W:project] | `{track, ids[]\|select:{kind\|name_like\|bbox}, op:{move?:[dx,dy,dz], set_pos?, rotate_y_deg?, scale?:[sx,sy,sz], anchor?:"base"\|"center"}, remove?:bool, dry_run, plan_id}` | per-id before/after, bounding-box delta, "collision follows: yes/no/unknown" |
| `eod_collision_query` | [R] | `{track, near?:{x,y,z,radius}, kinds?: wall\|boundary\|height_limit\|surface}` | records with ids, extents, flags, owner prop if any |
| `eod_collision_edit` | [W:project] | `{track, ids[], action:"remove"\|"disable"\|"lower"\|"raise"\|"move", params?, dry_run, plan_id}` | exact records changed; refuses `UNSUPPORTED` when semantics unverified |

### 6.5 Vehicles (P2)

| Tool | Class | Input | Output |
|---|---|---|---|
| `eod_vehicle_list` | [R] | `{}` | `[{id, name, archive, roles}]` (CAR_NAMES) |
| `eod_vehicle_get_params` | [R] | `{vehicle}` | curated fields with units (`mass lb, grip, damage_scale, power_max hp, torque_max lb-ft, redline rpm`) |
| `eod_vehicle_set_params` | [W:project] | `{vehicle, changes:{field:value}, dry_run, plan_id}` | before/after; `save` today supports a narrow case (see journeys job 8); otherwise `UNSUPPORTED` |
| `eod_vehicle_export_start` | [W:project] job | `{vehicle, out?}` | OBJ/MTL + textures + manifest |
| `eod_vehicle_preview` | [R] | `{vehicle, angle?}` | image |
| `eod_vif_patch_check` | [R] | `{asset_id, obj}` | topology compatibility report (counts, strips, material groups); never writes |
| `eod_vif_patch` | [W:project] | `{asset_id, obj, dry_run, plan_id}` | patched native member staged; refuses on any mismatch |
| `eod_vehicle_import` | [W:project] | `{vehicle, bundle, dry_run, plan_id}` | P3 |

### 6.6 Packages and profiles (P1 for build/inspect/profile; install P2)

| Tool | Class | Input | Output |
|---|---|---|---|
| `eod_package_build` | [W:project] | `{id, version, name, from:{project?, export_run?}, include?[], dry_run, plan_id}` | `eodmod.json` summary, file list, hashes, validation result, path of `.eodmod` |
| `eod_package_inspect` | [R] | `{path}` | validated manifest, game hash match, deps, what it touches |
| `eod_package_validate` | [R] | `{path}` | errors with paths (no `..`, symlinks, case dups, hash mismatch) |
| `eod_profile_create` | [W:profile] | `{name (^dev-[a-z0-9-]+$)}` | creates profile with `.eod-dev-profile` marker |
| `eod_profile_describe` | [R] | `{name}` | packages, enabled, order, composed hashes, `PROFILE.TXT` |
| `eod_conflicts_check` | [R] | `{profile, add?:package_path}` | per-member overlaps, winner under current order, which are blocking vs accepted |
| `eod_mod_install` | [W:profile] | `{profile, package, accept_conflicts?:[assetId], dry_run, plan_id}` | plan: files added, composed archives recomposed; apply: staged then rename-swapped (`active/`) |
| `eod_mod_enable` / `eod_mod_disable` | [W:profile] | `{profile, id, dry_run, plan_id}` | same plan/apply |
| `eod_mod_set_order` | [W:profile] | `{profile, order[], dry_run, plan_id}` | winner changes listed |
| `eod_mod_uninstall` | [W:profile] destructive | `{profile, id, confirm:"<id>", dry_run, plan_id}` | removes only owned files |
| `eod_profile_delete` | [W:profile] destructive | `{name, confirm:"<name>"}` | only dev profiles |

### 6.7 Validate, run, capture

| Tool | Class | P | Input | Output |
|---|---|---|---|---|
| `eod_project_validate` | [R] | P1 | `{project}` | `validate` result: overlay path, archives count, hash checks |
| `eod_launch_plan` | [R] | P1 | `{project\|profile, track?, headless?}` | argv/env that would run (wraps `launch_plan`); never executes |
| `eod_test_run_start` | [RUN] job | P1 (needs `EOD_KIT_ALLOW_RUN=1`) | `{profile\|project, track?, car?, headless:true, timeout_s?<=600, baseline?:run_id, env?:{EOD_HOST_TEXTURES?,EOD_HOST_TEXTURE_LOG?}}` | job; result `{capture_png, game_log, exit:{code,signal}, warnings[], hosttex:{loaded,hit,miss,fallback}, diff?:{pct,image}}` + image block of the capture |
| `eod_capture_get` | [R] | P1 | `{run_id}` | image block + meta |
| `eod_log_tail` | [R] | P1 | `{run_id\|job_id, lines?<=400, grep?}` | text; secrets/paths of $HOME redacted to `~` |
| `eod_capture_diff` | [R] | P2 | `{a, b}` | image of difference + percent changed |
| `eod_job_status` / `eod_job_cancel` / `eod_job_list` | [R]/[RUN-control] | P1 | section 5 | |

`eod_test_run_start` only ever launches `EOD_GAME_EXE` with `EOD_MOD_DIR`
pointing at a **dev profile `active/`** or a project overlay inside the
workspace, plus whitelisted env vars. It never passes the disc path as a write
target.

### 6.8 Expert fallback (P3, off by default)

`eod_native_fields` ([R]) and `eod_native_patch` ([W:project], needs
`EOD_KIT_EXPERT=1`): raw VALT row get/set with strong warnings. Not needed for
any ranked journey.

## 7. Safety rules (server-enforced)

1. **Disc is read-only.** Any path argument resolving (after `realpath`) inside
   `EOD_DISC` as a write target is `UNSAFE_PATH`. `export_native` keeps its
   existing `inside(destination, disc)` guard.
2. **Never touch installed apps.** Refuse writes under `/Applications`, any
   `*.app` bundle, `~/Library/Application Support/<game or app>` other than
   `EOD_KIT_HOME`, and any profile lacking `.eod-dev-profile`.
3. **Workspace-only paths.** Every path input is relative to `EOD_KIT_HOME`
   (or an explicitly allow-listed import dir in config for PNG/OBJ inputs,
   read-only). No `..`, no symlinks, no absolute write targets; case-folded
   collision checks.
4. **Dev profiles only.** Install/enable/etc. require a profile name matching
   `dev-*` with marker file; there is no tool that edits a "release" profile.
5. **Dry-run first, plan-id bound** (rule 3.5); destructive ops need typed
   `confirm`. The model cannot waive these; the server rejects, and the Claude
   Code permission prompt is a second layer.
6. **Staged atomic publication.** Writes go to a sibling temp dir then
   rename-swap, per contract section 6; a failure leaves the previous state.
   Every write is appended to `EOD_KIT_HOME/audit.jsonl` (ts, tool, args hash,
   plan_id, files, before/after sha).
7. **No arbitrary execution.** Only the whitelisted game exe and in-repo Python
   modules; no shell, no `eval`, no URL fetch. Env passthrough whitelist.
8. **Run gating.** `[RUN]` tools exist only if `EOD_KIT_ALLOW_RUN=1`; headless
   by default; hard timeout; one game process; process group killed on cancel.
9. **Copyright hygiene.** Exports and previews stay in the workspace; packages
   contain only modified members (per contract section 5), never retail
   archives. `package_build` refuses to include a member identical to retail.
10. **Prompt-injection hygiene.** Text read from game logs, package manifests
    and filenames is data; tools return it inside JSON fields, and
    `eod_package_inspect` flags description strings that look like instructions.
11. **Honest status.** Results include `"verified": false` for any edit whose
    in-game semantics are not in the acceptance record, and tool descriptions
    never claim a capability the Kit lacks (see CREATION_KIT_CAPABILITIES.md).
    A clean exit is never reported as success of a gameplay change; success
    requires the capture to be produced and returned.

## 8. Resources

Static and templated; all `eod://` URIs resolve under the workspace only.

| URI | MIME | Content |
|---|---|---|
| `eod://status` | application/json | `eod_kit_status` |
| `eod://census` | application/json | cached census summary (totals by role/type, not covered list) |
| `eod://census/assets{?role,type,archive}` | application/json | filtered asset records (paged) |
| `eod://asset/{archive}/{member}` | application/json | one record |
| `eod://asset/{archive}/{member}/preview.png` | image/png | mip-zero preview |
| `eod://manifest/{run_id}` | application/json | `export-manifest.json` |
| `eod://export/{run_id}/{path}` | by extension | exported file (PNG, OBJ, MTL) |
| `eod://project/{name}` | application/json | project edits |
| `eod://profile/{name}` | application/json | profile + `PROFILE.TXT` |
| `eod://package/{path}` | application/json | validated manifest |
| `eod://jobs/{job_id}` | application/json | live job state (subscribable) |
| `eod://run/{run_id}/capture.png` | image/png | first-race capture |
| `eod://run/{run_id}/game.log` | text/plain | game log (redacted) |
| `eod://run/{run_id}/meta.json` | application/json | exe hash, profile hash, env, timings |
| `eod://docs/{name}` | text/markdown | whitelisted repo docs: texture contracts, capabilities, track workflow |
| `eod://audit` | application/jsonl | last 200 audit entries |

Binary resources are returned as `blob` (base64). Resource links from tool
results use the same URIs. Emit `resources/list_changed` after exports/runs;
`resources/updated` for subscribed job URIs.

## 9. Prompts

Prompts are user-invoked templates (they appear as slash commands in Claude
Code as `/mcp__eod-creation-kit__<name>`). Each returns messages that teach the
tool order, the verification step and the stop conditions.

| Prompt | Args | Teaches |
|---|---|---|
| `texture_pack_workflow` | `scope` (role or archive), `scale` (2\|4), `pack_id` | status -> census -> search -> export (dry-run, job, poll) -> user's upscale step -> replace host (dry-run) -> package_build -> profile_create dev- -> install (plan, confirm) -> test_run -> read HOSTTEX hit/miss -> report unmatched |
| `remove_boundary` | `track`, `where` (free text or coords) | overview image with ids -> collision_query near -> ask user to confirm ids -> collision_edit dry-run -> apply -> test_run -> capture; warns about verification limits |
| `adjust_props` | `track`, `selector`, `change` | props_list -> props_transform dry-run -> apply -> test; ramps: check collision follows |
| `new_livery` | `vehicle`, `style` | locate paint role -> export -> edit -> compare -> replace -> test |
| `tune_vehicle` | `vehicle`, `intent` | get params -> propose numeric changes with units -> set (dry-run) -> test |
| `share_mod` | `project` | package_build -> validate -> inspect -> conflicts_check on a fresh dev profile |
| `debug_missing_replacement` | `run_id` | log_tail grep HOSTTEX, identity keys, runtimeKeyShared, EOD_HOST_TEXTURES |
| `cleanup_profile` | `profile` | describe -> disable/uninstall one by one with confirm |

## 10. Claude Code registration

Project-scoped `.mcp.json` at repo root (`$K/.mcp.json`), checked in so every
agent session in the repo gets it (Claude Code asks the user to approve
project-scoped servers once):

```json
{
  "mcpServers": {
    "eod-creation-kit": {
      "command": "python3",
      "args": ["-I", "tools/eod_kit/server.py"],
      "env": {
        "EOD_DISC": "${EOD_DISC}",
        "EOD_KIT_HOME": "${HOME}/Library/Application Support/EODCreationKit",
        "EOD_GAME_EXE": "${EOD_GAME_EXE:-}",
        "EOD_KIT_ALLOW_RUN": "${EOD_KIT_ALLOW_RUN:-0}"
      }
    }
  }
}
```
(`${VAR}` / `${VAR:-default}` expansion in `.mcp.json` is a Claude Code feature;
verify against https://docs.claude.com/en/docs/claude-code/mcp for the installed
version. Use `-I` so a stray `json.py` in the cwd cannot shadow stdlib.)
Alternative: `claude mcp add eod-creation-kit -s project -e EOD_DISC=... --
python3 -I tools/eod_kit/server.py`.

Suggested `.claude/settings.json` permissions: allow `mcp__eod-creation-kit__eod_*`
read-only tools (`eod_kit_status`, `eod_catalog`, `eod_entries`, `eod_inspect`,
`eod_asset_*`, `eod_*_list`, `eod_*_get*`, `eod_job_status`, `eod_log_tail`,
`eod_capture_*`), and leave all `[W:*]`, `[RUN]` and destructive tools on
"ask", so the human sees each apply even though the server already requires a
plan id.

## 11. Companion skill / CLAUDE.md snippet

Ship `.claude/skills/eod-modding/SKILL.md` (and the short version in
`CLAUDE.md`). Draft:

```markdown
---
name: eod-modding
description: Use when the user wants to change Test Drive: Eve of Destruction (textures, skies, liveries, props, ramps, collision, car physics), package or install a mod, or test a change. Drives the eod-creation-kit MCP server.
---
# EOD modding workflow
Rules
- Use the eod_* MCP tools. Never edit game archives, the disc folder, or an installed app by hand or with shell commands.
- Start with eod_kit_status. If a tool is missing it is not built yet; say so, do not improvise a workaround that writes to the disc.
- Every write: dry run first, show the user the plan summary (counts, files, before/after), then apply with the plan_id. Destructive: ask the user to confirm by name.
- Only `dev-*` profiles. Create one if none exists.
- Prove every gameplay change: eod_test_run_start, then show the capture image. Never say "it works" from an exit code. If you cannot run the game (EOD_KIT_ALLOW_RUN off), say the change is unverified in game.
- Prefer eod_asset_search over browsing archives. Asset ids are `<archive>#<member>`.
- Long jobs: poll eod_job_status with wait_seconds; report progress; offer cancel. Resume rather than restart.
- Report only verified semantics. Fields flagged verified:false are experiments: say so.
Recipes: /mcp__eod-creation-kit__texture_pack_workflow, remove_boundary, adjust_props, new_livery, tune_vehicle, share_mod.
Texture rules: native replace must match size; host replace must be origW*k x origH*k (k = power of two, max 4096); indexed textures may quantise (check eod_texture_compare); sky tiles share palettes, change them as a set.
```

## 12. Implementation plan

Files (all new, under `tools/eod_kit/`; no edits to existing code):
`server.py` (transport, dispatch, cancel, progress), `schemas.py` (tool defs),
`guard.py` (path and profile safety, plan ids, audit), `jobs.py`
(subprocess jobs, progress jsonl), `tools_read.py`, `tools_texture.py`,
`tools_pkg.py`, `tools_run.py`, later `tools_world.py`, `tools_vehicle.py`,
`resources.py`, `prompts.py`. Backend access: `modding_backend.dispatch()` is
called in-process for fast reads and in a child for anything heavy; planned
modules are called through their argparse/`main()` CLIs or functions with
`progress`/`cancel` callbacks (as `asset_registry.census(disc, jobs, progress,
cancel)` already does).

Phase 1 (week 1): server.py, guard, jobs, status/catalog/entries/inspect/
environment, census start + search/get/preview, texture export (job) /
replace(native) / tint / compare, project_validate, launch_plan, test_run +
capture/log, package_build/inspect/validate, profile_create/describe,
resources (status, census, manifest, run/*, jobs), prompts (texture pack,
share), `.mcp.json`, skill.
Phase 2 (weeks 2-4, as modules land): mod_install family + conflicts,
HOSTTEX `mode:host`, sky_export, world_* and props/collision tools, vehicle
export/params/preview, vif_patch, capture_diff, remaining prompts.
Phase 3: vehicle_import, expert fallback, scripted drive path, track
registration, optional SDK/HTTP transport.

## 13. Acceptance tests

Automated (`tools/tests/test_eod_kit_mcp.py`, stdlib `unittest`, spawns the
server as a subprocess over stdio with a fake mini-disc fixture; no game
needed unless marked RUN):

1. Handshake: `initialize` returns capabilities `tools, resources, prompts`;
   `tools/list` schemas all valid JSON Schema with `additionalProperties:false`;
   every tool has a side-effect prefix and annotations.
2. stdout hygiene: server emits only valid JSON-RPC lines; a provoked backend
   exception goes to stderr and an `isError` result, server stays alive.
3. Protocol: unknown tool -> JSON-RPC -32602; bad args -> -32602 with field
   path; `ping` answered while a 5 s job is running.
4. Read tools match `modding_backend.dispatch` byte-for-byte (catalog, entries,
   inspect) on the fixture and on the real disc when `EOD_DISC` is set.
5. Safety: path traversal (`..`, absolute, symlink, case-fold collision),
   destination inside disc, `/Applications/...`, non-`dev-` profile,
   profile without marker: each returns `UNSAFE_PATH` and writes nothing
   (disc tree hash identical before/after).
6. Two-phase: apply without `plan_id` -> `NEEDS_CONFIRM`; stale plan after the
   input changed -> `STALE_PLAN`; dry run leaves tree unchanged (hash);
   `uninstall` without exact `confirm` refused.
7. Jobs: census/export on fixture reports monotonically increasing progress,
   `job_status` long-poll returns on change; cancel mid-run leaves a resumable
   manifest and `cancelled` state; resume completes without rewriting
   unchanged outputs; kill server mid-job -> restart marks `failed:
   server_restart`.
8. Progress: with `progressToken`, `notifications/progress` are monotonic and
   stop after the response.
9. Images: `eod_asset_preview` returns an image block that decodes as PNG with
   the right size, under 1.5 MB, and a working resource link; resource read of
   the same URI returns identical bytes.
10. Resources/prompts: list/read round trip for every URI template; prompts
    expand with args; docs whitelist rejects other files.
11. Capability gating: removing a backend module hides its tools and
    `eod_kit_status.missing` explains why; `list_changed` emitted on change.
12. Texture flow (fixture): export -> replace(native, same size) -> validate
    -> package_build -> package_validate: unrelated members byte-identical;
    wrong-size PNG rejected with the expected/actual sizes.
13. Redaction: log tail output contains no `$HOME` path.
14. Audit: every applied write has an `audit.jsonl` line with before/after hashes.

Manual / RUN (needs built game, `EOD_KIT_ALLOW_RUN=1`):
15. `eod_test_run_start` on a dev profile with the Anoka sky edit returns a
    capture PNG that differs from baseline in the sky region and a game log
    that lists the profile (`PROFILE.TXT`) at startup; with
    `EOD_KIT_ALLOW_RUN=0` the tool is absent.
16. Claude Code end-to-end: `claude mcp list` shows the server connected; the
    prompt "make the Anoka sky orange and show me" completes using only
    `eod_*` tools, shows an image, and the disc hash is unchanged.
17. HOSTTEX (Phase 2): 4x replacement of one vehicle paint asset binds
    (log hit, no fallback) and the capture shows it; disabling the mod
    restores the retail capture byte-for-byte (image hash equal to baseline).

## 14. Sources

- MCP tools, results, errors, annotations: https://modelcontextprotocol.io/specification/2025-06-18/server/tools
- MCP progress: https://modelcontextprotocol.io/specification/2025-06-18/basic/utilities/progress
- Official Python SDK (3.11+, `mcp[cli]`, stdio/HTTP/SSE): https://github.com/modelcontextprotocol/python-sdk
- Blender MCP (tool set incl. screenshot `look`, `get_scene_info`, `execute_blender_code`, optional safe mode, unauthenticated socket warning): https://github.com/ahujasid/blender-mcp
- Claude Code MCP configuration (`.mcp.json`, scopes): https://docs.claude.com/en/docs/claude-code/mcp (not fetched; verify)
- Unity / Godot MCP servers (tool-per-editor-action pattern; not fetched, from background knowledge): https://github.com/CoplayDev/unity-mcp , https://github.com/Coding-Solo/godot-mcp
- Modding-scene sources: see `MODDER_JOURNEYS.md` section 5.
