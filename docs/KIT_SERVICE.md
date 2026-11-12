# Creation Kit service

One long-lived Python process per Kit workspace serves every `modding_backend`
action, the SQLite asset index and the persistent job queue. The macOS app, the
MCP server (`tools/eod_mcp`) and scripts are clients of the same process, so they
see one index, one job list and one set of locks. Code: `tools/kit_service/`.
Standard library only; it runs on the macOS system Python (3.9) and newer.

| File | Role |
|---|---|
| `server.py` | JSON-RPC 2.0 server (Unix socket or `--stdio`), auto-start daemon, idle exit |
| `client.py` | Python client with auto-start (used by the MCP server and `cli.py`) |
| `cli.py` | `python3 -I tools/kit_service/cli.py <method> [json]` for scripts |
| `backend.py` | `modding_backend` actions in-process, response cache, locks, state guard |
| `cache.py` | LRU caches over archive directories, member payloads and decoded textures |
| `index.py` | asset index (built from `asset_registry`'s census functions) |
| `jobs.py` | persistent queue, scheduler, leases, retries, resource locks |
| `worker.py` | job subprocess; reuses `eod_mcp/job_worker.py` kinds |
| `lanes.py`, `lane.py` | long-lived backend lane processes (3D models, project/profile writes) |
| `states.py` | project, package and development-profile state machines |
| `db.py` | SQLite schema and numbered migrations |
| `../editor_macos/Sources/EODModdingSuite/Models/KitService.swift` | Swift socket client |

## Process model

- Workspace: `EOD_KIT_WORKSPACE` (default `~/Library/Application Support/EODCreationKit`).
  Service files live in `<workspace>/service/`: `kit.db` (+ WAL), `service.json`,
  `server.lock`, `start.lock`, `service.log`, `thumbs/`. Job logs: `<workspace>/jobs/<id>/job.log`.
- Socket: `<workspace>/service/kit.sock`, or `/tmp/eodkit-<uid>/<hash>.sock` when that path
  exceeds the AF_UNIX limit. Clients never derive it; they read `service.json`, which the
  server writes after `bind` (mode 0600, directory 0700).
- Start: a client that cannot connect takes an exclusive `flock` on `start.lock`, retries the
  connection, then runs `python3 -I server.py --workspace W --detach`. The server double-forks,
  takes `server.lock` for its lifetime, removes any stale socket (holding the lock proves no
  other server owns it), binds, writes `service.json` and reports readiness through a pipe.
  A second server for the same workspace exits once it sees the first answering.
- Stop: after `EOD_KIT_SERVICE_IDLE` seconds (default 900) with no request and no queued or
  running job; when `<workspace>/service` or `server.lock` disappears; when any of its Python
  sources change (the next client starts the new code); or on `service.shutdown`. In-flight
  requests are drained first. Job workers are separate processes and are not stopped.
- `--stdio` serves the same methods on stdin/stdout (no lock, no idle exit) for scripts that
  must not leave a daemon. It schedules jobs too; job claims are atomic in the database.

### macOS app

`Backend.call` sends `{"method":"backend","params":<the old request>}` and decodes the same
`{ok, result, error, detail}` envelope as before. Each call opens its own connection; task
cancellation shuts that socket down. If the service script is missing (for example a bundle
built without `tools/kit_service`), cannot start, or `EOD_KIT_SERVICE=0`, the call falls back to
one `/usr/bin/python3 -I modding_backend.py` per request and writes a line to standard error;
the service is retried after 60 s. A write whose connection drops after sending is reported
as unconfirmed rather than repeated. The app also claims the global `game` lock while its
test session runs (`lock.acquire` / `lock.release`).

### MCP server

`eod_mcp/jobs.py:make_job_manager` returns a `ServiceJobManager` (job tools, census, test
runs) when the service answers, else the previous local `JobManager`. `eod_census_start`
submits `index_build` and exports `cache/census.json` from the index; `eod_asset_search` and
`eod_asset_get` query the index; catalog, entries and inspect use the `backend` method.

## Protocol

Newline-delimited JSON-RPC 2.0. Application errors use code `-32000` with
`error.data = {code, message, hint?, details?}` (`message` says what happened, `hint` what to do).

| Method | Params → result |
|---|---|
| `backend` | a `modding_backend` request → its envelope, byte-compatible with the helper's stdout |
| `service.ping` / `service.status` / `service.shutdown` / `cache.clear` | liveness, cache and job counters |
| `index.build` | `{disc, jobs?, full?, census_out?, meta_out?}` → job (deduplicated) |
| `index.search` | `{disc, query?, text?, role?, type?, archive?, min_w?, min_h?, shares_palette_with?, duplicate_of?, tag?, limit?, cursor?}` → `{assets, total, next?}` |
| `index.get` | `{disc, assetId}` → census record + `tags`, `relations`, `fields`, `thumbnails` |
| `index.thumbnail` | `{disc, assetId, size: 64|128|256|512|1024}` → PNG path and sha256 (generated once) |
| `index.tag` | `{disc, assetId, add?, remove?}` |
| `index.status` / `index.census` | counts, last run, database bytes / census document |
| `job.submit` | `{kind, params, label?, source?, audit?, resources?, max_attempts?}` → job (`duplicate: true` when existing) |
| `job.status` | `{job_id, wait_seconds? (≤25), since?}` long poll → state, progress, result, log tail |
| `job.list` / `job.cancel` / `job.events` | |
| `lock.acquire` / `lock.release` / `lock.list` | `{resource, pid, owner, wait?}` |
| `state.get` | `{kind: project|package|profile, ...request fields}` → state, allowed actions, history |

`query` is a case-insensitive substring of the assetId (the MCP search semantics); `text` is
an FTS5 trigram match over names, tags and decoded fields (e.g. `mass=2800`).

## Caches

Bounded LRU, in the service process only: archive directories and member payloads keyed by
`(realpath, size, mtime_ns, inode, device)`; decoded textures keyed by a payload digest (edited
bytes decode separately); and whole `catalog` / `entries` / `environment` / `inspect` envelopes
keyed by the archive identity, the project file identity, the pending edits in the request and
a signature of every file under `DATA`. A cached envelope is used only while the preview PNGs it
names still exist. Scratch archives in the backend's temporary folders are never cached.
Sizing is adaptive: caches start empty and grow only with what is opened; nothing is reserved.
Each cache has a ceiling relative to the machine (payloads and decoded textures 1/16 of RAM,
responses 1/32, directories 1/256, with small floors), which is a limit, not a target; entry
counts are set above the ~7300 retail members so bytes decide. Memory is given back: the
watchdog reads the macOS VM pressure level every 5 s and halves the caches on a warning (and
clears lane caches) or clears everything and closes idle lane processes when critical; after
`EOD_KIT_CACHE_IDLE` seconds (default 300) without a request all caches are dropped; lane
processes exit after `EOD_KIT_LANE_IDLE` seconds unused (default 120) and restart on demand.
Overrides: `EOD_KIT_<NAME>_CACHE_MB`, `EOD_KIT_<NAME>_CACHE_ENTRIES` (`NAME` = `PAYLOADS`,
`DECODED`, `RESPONSES`, `DIRECTORIES`; the older `EOD_KIT_PAYLOAD_CACHE_MB` etc. still work).
`service.status` reports each cache's bytes, ceiling and hit rate. `cache.clear` also clears
the lane processes' caches.

## Concurrency lanes

No single thread or process does everything (AGENTS.md, "Creation Kit concurrency"). A long
export, build or model decode never delays the next inspect.

| Lane | Runs | Where | Default size |
|---|---|---|---|
| read | catalog, entries, environment, inspect of non-model assets, validate, previews, index queries, job/lock calls | service threads (one per connection), SQLite WAL readers | unbounded threads |
| model | `inspect` of `MODL` members (mesh decode, triangulation, materials) | `lane.py model` processes, started on demand | up to max(2, cores/4) |
| write | save, save_format, import_host, remove_host, package_project, profile_* | `lane.py write` processes, started on demand | up to 2 |
| export | `texture_export` jobs (app batch export, MCP) | job worker processes | max(2, cores/4) slots |
| build | `cli` jobs (package builds and other long tool runs) | job worker processes | max(2, cores/4) slots |
| index | `index_build`, `census` jobs | job worker processes | 1 slot (parallel inside, `jobs=N`) |
| game | `test_run` jobs | job worker process | 1 slot, plus the global `game` lock |

Lane process counts: `EOD_KIT_LANE_MODEL`, `EOD_KIT_LANE_WRITE`; job slots: `EOD_KIT_LANE_EXPORT`,
`EOD_KIT_LANE_BUILD`, `EOD_KIT_LANE_INDEX`, `EOD_KIT_LANE_GAME` (`EOD_KIT_MAX_WORKERS`, if set,
caps every job lane). `EOD_KIT_LANES=0` runs model and write actions in the service process.
Write locks are per project file and per profile folder, taken in the service before a write
is handed to its lane, so two writes to one project are serialised (or refused after
`lockWait`) while writes to different projects run side by side. Reads never take a lock.
Lane processes are started on first use and replaced if they die (a read is retried once; a
write is never repeated and reports that it may not have been applied). They exit when the
service does (their stdin closes). `service.status.lanes` and `job.list`/`job.status` (`lane`)
show where work runs.

In the app, `StudioState` tracks operations per lane (`running`, `begin(_:)`, `end(_:)`,
`isRunning(_:)`). Only conflicting writes exclude each other (`isWriting`); selecting,
inspecting and previewing stay available while an export, build, save or game run is in
progress, and a result that arrives after the selection moved on is dropped. `loading` drives
spinners; `busy` remains as a compatibility alias for `isWriting`. Service responses are decoded
off the main actor.

## Asset index schema (`kit.db`)

`PRAGMA user_version` is the schema version; `db.MIGRATIONS[n]` upgrades n → n+1 inside one
transaction. A database newer than the code is refused. Never edit a released migration.

- `files(disc, path, ord, size, mtime_ns, sha256, kind, members, parse_error)` — every file under `DATA`.
- `assets` — one row per archive member, identity per `CREATION_KIT_TEXTURE_CONTRACTS.md` §1–2:
  `asset_id, archive, member, archive_sha256, member_sha256, type, crc_ok, role, width, height,
  levels, format, flags, bits, index_key, palette_key, decode_status` plus `record`, the exact
  census record (JSON).
- `fields(asset, seq, section, key, value)` — decoded VALT tables.
- `tags(disc, asset_id, tag, source)` — `user` tags (index tags such as role are computed).
- `relations(disc, src, rel, dst)` — `duplicateOf`, `variantOf`, `runtimeKeyShared`,
  `indexKeyShared`; further relation names (track → model, event → prop) need no schema change.
- `thumbnails(member_sha256, size, path, sha256, width, height)` — content addressed, lazy.
- `assets_fts` — FTS5 (trigram) over name, tags and fields. `file_hashes` caches sha256 by file
  identity. `index_runs` records each build.

### Building and rebuilding

`index.build {disc}` (MCP: `eod_census_start`) runs an `index_build` job. Incremental: a file
whose size and mtime are unchanged is skipped unread; a changed file is hashed and skipped if
its sha256 is unchanged; only new content is analysed (CRC, TEX decode and runtime keys via
`asset_registry.analyze_member`, VALT fields), in a process pool. Each archive commits on its
own, so a cancelled or killed build resumes where it stopped. Relations are recomputed with
`asset_registry.link_records`. `full: true` deletes the disc's derived rows and reindexes
everything; the index is derived data, so deleting `kit.db` and building again is always safe
(user tags and job history are lost in that case).

## Jobs

States: `queued → running → succeeded | failed | cancelled`; a failed attempt with a transient
error returns to `queued` with `next_run_at` (backoff `EOD_KIT_BACKOFF_BASE·2^(attempt−1)`,
default base 2 s, capped at 60 s, up to `max_attempts`, default 3). Transient means timeout,
connection, `EAGAIN/EBUSY/EINTR/ETIMEDOUT/EMFILE/ENFILE/ENOLCK`, a locked database, or a worker
killed by a signal; everything else fails at once.

Kinds: `texture_export`, `census`, `cli`, `test_run` (the `eod_mcp/job_worker.py` functions),
`index_build`, and `stub` (tests). Batch texture export is the `texture_export` kind; the app's
`export_batch_start/status/cancel` actions submit and read it with their previous response fields.

- Idempotency key: sha256 of kind, canonical params and the content hashes of the job's disc
  inputs. A submit matching a queued or running job returns it; for `index_build` and `census`
  a finished job is returned while its recorded outputs still hash the same.
- Workers: `python3 -I worker.py <db> <job> <lease> <dir>` in a new session (cancel signals the
  whole group: SIGTERM, SIGKILL after 5 s). The worker renews its lease every 2 s
  (`EOD_KIT_HEARTBEAT_SECONDS`, lease `EOD_KIT_LEASE_SECONDS`, default 15 s) and writes progress,
  checkpoints and the result into the database; if its lease is taken it stops itself.
- Recovery: a service that finds a `running` job it did not start adopts the worker if it is
  alive and renewing; kills it if alive but silent; and otherwise finalises it (result present)
  or requeues it (`recoveries + 1`; `texture_export` resumes from `export-manifest.json`, whose
  checkpoint skips verified outputs and leaves byte-identical files untouched). Game runs are
  failed with `server_restart` instead of being relaunched.
- Concurrency: per-lane slots (see Concurrency lanes); a full export lane leaves index builds,
  package builds and game runs free to start. Resources are exclusive locks held in
  the `locks` table: `export:<folder>`, `index:<disc>`, `profile:<folder>`, `project:<file>`,
  `game`. Reads take no lock. A second game run (job or app session) is rejected while one is
  queued or running. Synchronous profile and project writes wait up to 60 s (`lockWait`) for
  the lock and are then refused with a plain-language message; `mod_install`'s own `flock`
  still guards writers outside the service.

## State machines (`states.py`)

States are derived from what `modding_backend.py`, `mod_package.py` and `mod_install.py`
already write, plus the last recorded transition; every guarded write is checked first and the
result re-derived and logged (`entity_state`, `state_log`).

| Machine | States | Transitions |
|---|---|---|
| Package (per profile) | draft, built, installed, enabled, disabled, uninstalled | draft→built (build); built→installed/enabled (install, by `enable`); installed→enabled/disabled; enabled⇄disabled; installed/enabled/disabled→uninstalled; uninstalled→installed/enabled or built |
| Project | new, saved, packaged | new→saved (save, import_host); saved→saved; saved→packaged (package); packaged→saved (any edit) |
| Dev profile | absent, empty, inactive, active, blocked, in-use | writes allowed unless absent (only install) or in-use; launch only from active |

`in-use` means a running game job holds the profile; its packages cannot change until the run
ends. Rejections name the problem and the next step, for example "hd is not installed in
profile dev, so it can't be enabled. Install the package into this profile first."

## Tests

`tools/tests/test_kit_service.py` (envelope parity with the helper, caches, index/census
parity, incremental and full rebuild, FTS, thumbnails, tags, migrations, concurrent auto-start
and stale socket, kill -9 restart, idle exit, stdio, state machines) and
`tools/tests/test_kit_service_jobs.py` (duplicate submit, retries, cancel, one game run with a
stub executable, profile lock serialisation and in-use refusal, kill -9 requeue from checkpoint,
adoption, texture export resume without duplicates) and `tools/tests/test_kit_service_lanes.py`
(model and write lanes, dead lane process replaced, idle lanes and caches released, independent job lanes, inspect latency of 20
assets while an export runs within 1.5x of idle). All three are registered in
`tools/creation_kit/CMakeLists.txt`, which also points the Swift tests at a build-folder workspace.
