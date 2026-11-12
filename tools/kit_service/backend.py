"""modding_backend actions served in-process.

`call(request)` returns exactly the envelope `modding_backend.py` prints for the
same request on stdin ({ok, result} or {ok: false, error, detail}), so clients
that decoded the spawn-per-call helper keep working unchanged. Differences are
speed (warm imports, caches) plus three service duties:

* read-only responses (catalog, entries, environment, inspect) are cached by the
  identity of every file they depend on;
* write actions take the project / profile lock and pass the state-machine guard;
* export_batch_start/status/cancel run as `texture_export` jobs in the shared
  queue (same response fields), so app exports appear in the MCP job list.
"""
import hashlib
import json
import os
import time
from pathlib import Path

from . import cache, states
from .common import KitError, canonical, ensure_tools_path

ensure_tools_path()
import modding_backend as mb  # noqa: E402

cache.install(mb)

CACHED = ("catalog", "entries", "environment", "inspect")
PROJECT_WRITES = ("save", "save_format", "import_host", "remove_host", "package_project")
PROFILE_WRITES = ("profile_install", "profile_enable", "profile_disable", "profile_uninstall", "profile_accept_conflict")
EXPORT_ACTIONS = ("export_batch_start", "export_batch_status", "export_batch_cancel")
MANIFEST = "export-manifest.json"


def error_envelope(error):
    """modding_backend.main()'s error mapping (kept in step by test_kit_service parity checks)."""
    if isinstance(error, KitError):
        message = error.message + (" " + error.hint if error.hint else "")
        return dict(ok=False, error=message, detail=error.code)
    if isinstance(error, json.JSONDecodeError):
        return dict(ok=False, error="The Creation Kit sent a request its helper couldn't read. Restart the app and try again.",
                    detail=str(error))
    if isinstance(error, ValueError):
        return dict(ok=False, error=str(error), detail=getattr(error, "detail", None))
    if isinstance(error, OSError):
        return dict(ok=False, error="A file couldn't be read or written. Check that the folder is available and has free space, then try again.",
                    detail=f"{type(error).__name__}: {error}")
    return dict(ok=False, error="The Creation Kit couldn't finish this action. Nothing was changed. Try again, or reopen the project.",
                detail=f"{type(error).__name__}: {error}")


def _data_signature(disc):
    digest = hashlib.blake2b(digest_size=16)
    for root, dirs, files in os.walk(Path(disc) / "DATA"):
        dirs.sort()
        for name in sorted(files):
            try:
                st = os.stat(os.path.join(root, name))
            except OSError:
                continue
            digest.update(f"{root}/{name}\0{st.st_size}\0{st.st_mtime_ns}\0{st.st_ino}\n".encode())
    return digest.hexdigest()


def _images(result):
    if not isinstance(result, dict):
        return []
    out = [result["image"]] if isinstance(result.get("image"), str) else []
    for part in result.get("parts") or ():
        if isinstance(part, dict) and isinstance(part.get("texture"), str):
            out.append(part["texture"])
    preview = (result.get("format") or {}).get("preview") if isinstance(result.get("format"), dict) else None
    if isinstance(preview, dict) and isinstance(preview.get("png"), str):
        out.append(preview["png"])
    return out


class Backend:
    def __init__(self, ws, store, jobs, locks, lanes=None):
        self.ws, self.store, self.jobs, self.locks = Path(ws), store, jobs, locks
        self.lanes = lanes  # kit_service.lanes.Lanes, or None to run everything in this process
        self.calls = 0
        self.mb = mb

    # ------------------------------------------------------------------ entry point
    def call(self, request, client="rpc"):
        self.calls += 1
        try:
            if not isinstance(request, dict) or "action" not in request:
                raise ValueError("The Creation Kit sent a request without an action. Restart the app and try again.")
            key = self._cache_key(request)
            if key is not None:
                hit = cache.RESPONSES.get(key)
                if hit is not None and all(os.path.exists(p) for p in hit[1]):
                    return hit[0]
            data, images, ok = self._run(request, client)
            if key is not None and ok:
                cache.RESPONSES.put(key, (data, images), len(data) + 256)
            return data
        except Exception as error:  # noqa: BLE001 - every failure becomes the helper's error envelope
            return json.dumps(error_envelope(error)).encode()

    def lane(self, request):
        """Which lane serves this request: "read" (service threads), "model" or "write" (lane processes)."""
        action = request.get("action")
        if self.lanes is None:
            return "read"
        if action in PROJECT_WRITES or action in PROFILE_WRITES:
            return "write"
        if action == "inspect" and request.get("disc") and request.get("archive") and request.get("asset"):
            try:
                disc = Path(request["disc"]).expanduser().resolve()
                for entry in mb.directory(mb.archive_path(disc, request["archive"])):
                    if entry["name"] == request["asset"]:
                        return "model" if entry["type"] == "MODL" else "read"
            except (ValueError, KeyError, OSError, TypeError):
                return "read"
        return "read"

    def _cache_key(self, request):
        action = request.get("action")
        if action not in CACHED or not request.get("disc"):
            return None
        try:
            disc = Path(request["disc"]).expanduser().resolve()
            if action == "catalog":
                return ("catalog", str(disc), _data_signature(disc))
            archive = mb.archive_path(disc, request["archive"])
            ident = cache.file_key(archive)
            if ident is None:
                return None
            if action in ("entries", "environment"):
                return (action, str(disc), request["archive"], ident)
            project = Path(request["project"]).expanduser().resolve() if request.get("project") else None
            edits = hashlib.blake2b(canonical({k: request.get(k) for k in ("textureEdit", "textureEdits", "formatEdits")})
                                    .encode(), digest_size=16).hexdigest()
            return ("inspect", str(disc), request["archive"], request.get("asset"), ident, str(project),
                    cache.file_key(project) if project else None, edits, _data_signature(disc))
        except (ValueError, KeyError, OSError, TypeError):
            return None

    def _run(self, request, client):
        """Returns (envelope bytes, image paths, ok)."""
        action = request["action"]
        if action in EXPORT_ACTIONS:
            disc = self._disc(request)
            return self._ok(getattr(self, action)(request, disc))
        lane = self.lane(request)
        if lane == "model":
            header, data = self.lanes.call("model", request)
            return data, header.get("images") or [], bool(header.get("ok"))
        conn = self.store.conn()
        resources, holder = self._resources(request), f"rpc:{os.getpid()}:{client}:{time.time_ns()}"
        game_holder = self._game_on_profile(request) if action in states.PROFILE_ACTIONS else None
        guard = states.Guard(conn, request, game_holder).check()
        if resources:
            self.locks.acquire(resources, holder, os.getpid(), wait=float(request.get("lockWait", 60)), note=action)
        try:
            if lane == "write":
                header, data = self.lanes.call("write", request)
                if not header.get("ok"):
                    return data, [], False
                result = json.loads(data)["result"]
            else:
                result = mb.dispatch(request)
                data = None
        finally:
            if resources:
                self.locks.release(resources, holder)
        guard.done(result)
        return (data, _images(result), True) if data is not None else self._ok(result)

    @staticmethod
    def _ok(result):
        return json.dumps(dict(ok=True, result=result), allow_nan=False).encode(), _images(result), True

    def _disc(self, request):
        disc = Path(request["disc"]).expanduser().resolve()
        if not (disc / "DATA").is_dir():
            raise ValueError("Select the extracted game folder containing DATA and SLUS_209.10")
        return disc

    def _resources(self, request):
        action = request["action"]
        if action in PROJECT_WRITES and request.get("project"):
            return ["project:" + str(Path(request["project"]).expanduser().resolve())]
        if action in PROFILE_WRITES:
            _, folder = states.profile_location(request)
            return ["profile:" + str(folder)]
        return []

    def _game_on_profile(self, request):
        _, folder = states.profile_location(request)
        row = self.store.conn().execute(
            "SELECT l.holder FROM locks l JOIN jobs j ON l.job_id=j.id WHERE l.resource=? AND j.state='running' "
            "AND j.kind='test_run'", ("profile:" + str(folder),)).fetchone()
        return row["holder"] if row else None

    # ------------------------------------------------------------------ batch export as queue jobs
    def _export_job(self, dest):
        rows = self.jobs.list(kind="texture_export", dest=str(dest), limit=1)
        return self.jobs.get(rows[0]["job_id"]) if rows else None

    def export_batch_start(self, request, disc):
        dest = mb._dest_checked(disc, request["destination"])
        if self.export_batch_status(request, disc)["state"] == "running":
            raise ValueError("An export to this folder is still running. Wait for it to finish, or choose another folder.")
        resume = bool(request.get("resume"))
        if (dest / MANIFEST).exists() and not resume:
            raise ValueError("This folder already contains an export. Resume it, or choose another folder.")
        params = dict(disc=str(disc), dest=str(dest), select=list(request.get("select") or []),
                      archives=list(request.get("archives") or []), role=request.get("role"), resume=resume,
                      jobs=int(request.get("jobs") or 1))
        job = self.jobs.submit("texture_export", params, label="Export textures", source="app")
        return dict(jobId=job["job_id"], pid=job.get("pid") or 0, destination=str(dest), resumed=resume)

    def export_batch_status(self, request, disc):
        dest = mb._dest_checked(disc, request["destination"])
        row = self._export_job(dest)
        if row is None:
            return mb.export_batch_status(request, disc)  # exports started by the spawn-per-call helper
        manifest = dest / MANIFEST
        out = dict(jobId=row["id"], destination=str(dest), state="idle", done=row["progress_done"] or 0,
                   total=row["progress_total"] or 0, assetId=row["progress_current"], resumable=manifest.is_file(),
                   manifest=str(manifest) if manifest.is_file() else None)
        state = row["state"]
        if state in ("queued", "running"):
            out.update(state="running", cancelRequested=bool(row["cancel_requested"]))
            return out
        summary, failures = {}, []
        try:
            document = json.loads(manifest.read_text())
            summary = document.get("summary", {})
            failures = [dict(assetId=a["assetId"], error=a.get("error")) for a in document.get("assets", [])
                        if a.get("status") != "exported"][:50]
            run = document.get("run", {})
            out.update(done=run.get("done", out["done"]), total=run.get("total", out["total"]))
        except (OSError, ValueError):
            pass
        result = json.loads(row["result"] or "null") or {}
        status = result.get("status")
        if state == "succeeded" or (state == "cancelled" and status in ("complete", "cancelled")) or \
                (state == "failed" and status in ("complete", "cancelled")):
            final = "complete" if status == "complete" else "cancelled"
            out.update(state=final, summary=dict(status=final, manifest=str(manifest), **summary),
                       exported=summary.get("exported", 0), failed=summary.get("failed", 0),
                       unsupported=summary.get("unsupported", 0), failures=failures, resumable=True)
            if final == "complete":
                out["done"] = out["total"] or out["done"]
            return out
        out.update(state="cancelled" if state == "cancelled" else "failed",
                   error=row["error"] or "The export stopped before it finished. Resume it to continue.")
        return out

    def export_batch_cancel(self, request, disc):
        dest = mb._dest_checked(disc, request["destination"])
        row = self._export_job(dest)
        if row is None:
            return mb.export_batch_cancel(request, disc)
        if row["state"] not in ("queued", "running"):
            return self.export_batch_status(request, disc)
        self.jobs.cancel(row["id"])
        return dict(self.export_batch_status(request, disc), cancelRequested=True)
