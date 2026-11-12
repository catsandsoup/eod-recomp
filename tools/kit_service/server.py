#!/usr/bin/env python3
"""EOD Creation Kit service: newline-delimited JSON-RPC 2.0 over a Unix socket (or stdio).

    python3 -I tools/kit_service/server.py [--workspace DIR] [--detach] [--stdio] [--idle SECONDS]

One socket server per workspace (server.lock). Clients auto-start it
(kit_service/client.py, the Swift app) and it exits after --idle seconds with
no requests and no active jobs, when its workspace disappears, or when its own
source files change (so the next client starts the new code). See docs/KIT_SERVICE.md.
"""
import argparse
import fcntl
import json
import os
import socket
import sys
import threading
import time
import traceback
from pathlib import Path

sys.path.append(str(Path(__file__).resolve().parents[1]))

from kit_service import VERSION  # noqa: E402
from kit_service import common as C  # noqa: E402
from kit_service.common import KitError  # noqa: E402

DEFAULT_IDLE = float(os.environ.get("EOD_KIT_SERVICE_IDLE", "900"))
CACHE_IDLE = float(os.environ.get("EOD_KIT_CACHE_IDLE", "300"))   # drop caches after this long unused
LANE_IDLE = float(os.environ.get("EOD_KIT_LANE_IDLE", "120"))     # close lane processes unused this long


def memory_pressure():
    """macOS VM pressure: 1 normal, 2 warning, 4 critical (0 if unknown)."""
    try:
        import subprocess
        out = subprocess.run(["/usr/sbin/sysctl", "-n", "kern.memorystatus_vm_pressure_level"],
                             capture_output=True, text=True, timeout=2).stdout
        return int(out.strip() or 0)
    except (OSError, ValueError, subprocess.SubprocessError):
        return 0


def _source_stamp():
    files = []
    for folder in (C.PACKAGE, C.TOOLS, C.TOOLS / "formats", C.TOOLS / "eod_mcp"):
        if folder.is_dir():
            files += [p for p in folder.glob("*.py")]
    out = {}
    for p in files:
        try:
            out[str(p)] = p.stat().st_mtime_ns
        except OSError:
            pass
    return out


class Service:
    def __init__(self, ws, idle=DEFAULT_IDLE, mode="socket"):
        self.ws = Path(ws)
        self.mode = mode
        self.idle = float(idle)
        self.started = time.time()
        self.last_activity = time.time()
        self.active = 0
        self.requests = 0
        self.alock = threading.Lock()
        self.stopping = threading.Event()
        self.stop_reason = None
        C.service_dir(self.ws).mkdir(parents=True, exist_ok=True)
        from kit_service import jobs as J
        self.J = J
        self.store = J.Store(C.db_path(self.ws))
        self.jobs = J.Jobs(self.ws, self.store)
        self.locks = J.Locks(self.store)
        self._backend = None
        self.block = threading.Lock()
        from kit_service.lanes import Lanes
        # Lane processes (3D models, writes); EOD_KIT_LANES=0 keeps everything in this process.
        self.lanes = Lanes() if os.environ.get("EOD_KIT_LANES", "1") != "0" else None
        self.stamp = _source_stamp()

    @property
    def backend(self):
        with self.block:
            if self._backend is None:
                from kit_service.backend import Backend
                self._backend = Backend(self.ws, self.store, self.jobs, self.locks, self.lanes)
            return self._backend

    def close(self):
        self.jobs.stop()
        if self.lanes is not None:
            self.lanes.close()

    # ------------------------------------------------------------------ dispatch
    def handle_line(self, line, client="rpc"):
        try:
            msg = json.loads(line)
        except ValueError:
            return C.error_response(None, C.PARSE_ERROR, "Parse error")
        if not isinstance(msg, dict) or msg.get("jsonrpc") != "2.0" or not isinstance(msg.get("method"), str):
            return C.error_response(msg.get("id") if isinstance(msg, dict) else None, C.INVALID_REQUEST, "Invalid Request")
        rid, method, params = msg.get("id"), msg["method"], msg.get("params")
        with self.alock:
            self.active += 1
            self.requests += 1
            self.last_activity = time.time()
        try:
            result = self.dispatch(method, params if params is not None else {}, client)
            return None if "id" not in msg else C.response(rid, result)
        except KitError as error:
            return C.error_response(rid, C.APP_ERROR, error.message, error.payload())
        except _MethodNotFound:
            return C.error_response(rid, C.METHOD_NOT_FOUND, "Method not found: " + method)
        except (TypeError, KeyError) as error:
            return C.error_response(rid, C.INVALID_PARAMS, f"Invalid params: {type(error).__name__}: {error}")
        except Exception as error:  # noqa: BLE001
            traceback.print_exc(file=sys.stderr)
            return C.error_response(rid, C.INTERNAL, f"Internal error: {type(error).__name__}: {error}")
        finally:
            with self.alock:
                self.active -= 1
                self.last_activity = time.time()

    def dispatch(self, method, p, client):
        if method == "backend":
            return C.Raw(self.backend.call(p, client))
        if method == "service.ping":
            return dict(pong=True, pid=os.getpid(), version=VERSION, workspace=str(self.ws), mode=self.mode,
                        uptime=round(time.time() - self.started, 3), python=sys.version.split()[0])
        if method == "service.status":
            from kit_service import cache
            return dict(pid=os.getpid(), version=VERSION, workspace=str(self.ws), uptime=round(time.time() - self.started, 1),
                        requests=self.requests, active=self.active, idleSeconds=self.idle, caches=cache.stats(),
                        jobs={s: n for s, n in self.store.conn().execute("SELECT state, count(*) FROM jobs GROUP BY state")},
                        backendCalls=self._backend.calls if self._backend else 0, db=str(C.db_path(self.ws)),
                        lanes=dict(read=dict(threads=threading.active_count()),
                                   **(self.lanes.stats() if self.lanes else {})),
                        jobLanes=self.jobs.slots)
        if method == "service.shutdown":
            self.stop("shutdown requested")
            return dict(stopping=True)
        if method == "cache.clear":
            from kit_service import cache
            cache.clear()
            if self.lanes is not None:
                self.lanes.clear()
            return dict(cleared=True)
        if method.startswith("index."):
            return self.index_method(method[6:], p)
        if method.startswith("job."):
            return self.job_method(method[4:], p)
        if method.startswith("lock."):
            return self.lock_method(method[5:], p)
        if method.startswith("state."):
            from kit_service import states
            if method == "state.get":
                return states.describe(self.store.conn(), p["kind"], p)
            if method == "state.check":
                machine = states.MACHINES.get(p["kind"])
                if machine is None:
                    raise KitError("INVALID_ARGUMENT", "kind must be project, profile or package")
                return dict(target=machine.check(p["state"], p["action"], p.get("name", p["kind"]), p, p.get("extra")))
        raise _MethodNotFound()

    def index_method(self, name, p):
        from kit_service import index
        conn = self.store.conn()
        if name == "build":
            disc = str(Path(p["disc"]).expanduser().resolve())
            params = dict(disc=disc, jobs=int(p.get("jobs") or max(1, (os.cpu_count() or 2) - 2)), full=bool(p.get("full")))
            for key in ("census_out", "meta_out"):
                if p.get(key):
                    params[key] = str(Path(p[key]).expanduser().resolve())
            return self.jobs.submit("index_build", params, label=p.get("label") or ("Rebuild index" if params["full"] else "Index disc"),
                                    source=p.get("source", "rpc"), audit=bool(p.get("audit")))
        if name == "search":
            return index.search(conn, p.get("disc"), p)
        if name == "get":
            return index.get(conn, p.get("disc"), p["assetId"], self.ws)
        if name == "status":
            return index.status(conn, p.get("disc"), C.db_path(self.ws))
        if name == "census":
            return index.census(conn, p.get("disc"))
        if name == "thumbnail":
            return index.thumbnail(conn, self.ws, p.get("disc"), p["assetId"], int(p.get("size", 128)))
        if name == "tag":
            return index.tag(conn, p.get("disc"), p["assetId"], p.get("add") or (), p.get("remove") or ())
        raise _MethodNotFound()

    def job_method(self, name, p):
        if name == "submit":
            return self.jobs.submit(p["kind"], p.get("params") or {}, p.get("label", ""), p.get("source", "rpc"),
                                    bool(p.get("audit")), p.get("resources"), int(p.get("max_attempts", 3)))
        if name == "status":
            return self.jobs.status(p["job_id"], p.get("wait_seconds", 0), p.get("since"))
        if name == "list":
            return dict(jobs=self.jobs.list(p.get("state"), p.get("kind"), int(p.get("limit", 200))))
        if name == "cancel":
            return self.jobs.cancel(p["job_id"])
        if name == "events":
            return dict(events=self.jobs.events(p["job_id"]))
        raise _MethodNotFound()

    def lock_method(self, name, p):
        if name == "acquire":
            resource = p["resource"]
            pid = int(p.get("pid") or 0)
            if not pid:
                raise KitError("INVALID_ARGUMENT", "lock.acquire needs the pid of the process that holds the lock.")
            holder = f"pid:{pid}:{p.get('owner', 'client')}"
            if resource == "game":
                busy = self.jobs._game_holder(self.store.conn())
                if busy and busy != holder:
                    raise KitError("CONFLICT", "A game test run is already in progress, so a second one was not started.",
                                   "Wait for it to finish or stop it, then start the new run.", dict(holder=busy))
            self.locks.acquire([resource], holder, pid, wait=float(p.get("wait", 0)), note=p.get("note", ""))
            return dict(resource=resource, holder=holder)
        if name == "release":
            self.locks.release([p["resource"]], f"pid:{int(p['pid'])}:{p.get('owner', 'client')}")
            return dict(released=True)
        if name == "list":
            return dict(locks=self.locks.list())
        raise _MethodNotFound()

    # ------------------------------------------------------------------ lifecycle
    def stop(self, reason):
        if not self.stopping.is_set():
            self.stop_reason = reason
            print(f"kit-service: stopping ({reason})", file=sys.stderr, flush=True)
            self.stopping.set()

    def release_memory(self, idle_for):
        """Give memory back: shed caches under pressure or after an idle spell, close idle lanes."""
        from kit_service import cache
        now = time.time()
        if now - getattr(self, "_pressure_checked", 0) >= 5:
            self._pressure_checked = now
            level = memory_pressure()
            if level >= 4:
                cache.clear()
                if self.lanes is not None:
                    self.lanes.clear()
                    self.lanes.reap(0)
                self.memory_events = getattr(self, "memory_events", 0) + 1
            elif level >= 2:
                cache.shrink(0.5)
                if self.lanes is not None:
                    self.lanes.clear()
                self.memory_events = getattr(self, "memory_events", 0) + 1
        if idle_for >= CACHE_IDLE and cache.total_bytes():
            cache.clear()
            if self.lanes is not None:
                self.lanes.clear()
        if self.lanes is not None:
            self.lanes.reap(LANE_IDLE)

    def watchdog(self, lock_path=None, lock_ino=None):
        while not self.stopping.wait(1.0):
            if lock_path is not None:
                try:
                    if os.stat(lock_path).st_ino != lock_ino:
                        self.stop("workspace lock replaced")
                except OSError:
                    self.stop("workspace removed")
            if _source_stamp() != self.stamp:
                self.stop("source files changed")
            with self.alock:
                idle_for = time.time() - self.last_activity
                busy = self.active > 0
            self.release_memory(idle_for)
            if self.mode == "socket" and not busy and idle_for > self.idle:
                try:
                    if not self.jobs.busy():
                        self.stop(f"idle for {int(idle_for)} s")
                except Exception:  # noqa: BLE001
                    pass


class _MethodNotFound(Exception):
    pass


# ---------------------------------------------------------------------- socket server

def _serve_connection(service, conn):
    conn.settimeout(None)
    reader = conn.makefile("rb")
    try:
        for line in reader:
            if not line.strip():
                continue
            reply = service.handle_line(line, client="sock")
            if reply is not None:
                conn.sendall(reply)
    except (OSError, ValueError):
        pass
    finally:
        try:
            reader.close()
            conn.close()
        except OSError:
            pass


def run_socket(ws, idle, ready_fd=None):
    sdir = C.service_dir(ws)
    sdir.mkdir(parents=True, exist_ok=True)
    os.chmod(sdir, 0o700)
    lock_path = sdir / "server.lock"
    lock = open(lock_path, "a+")
    deadline = time.time() + 90
    while True:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            break
        except OSError:
            info = C.read_service_info(ws)
            if info and _ping(info.get("socket")):
                _ready(ready_fd, b"1")  # another server is already serving this workspace
                return 0
            if time.time() > deadline:
                _ready(ready_fd, b"0")
                print("kit-service: another server holds the workspace lock", file=sys.stderr)
                return 1
            time.sleep(0.1)
    lock.seek(0)
    lock.truncate()
    lock.write(str(os.getpid()))
    lock.flush()
    service = Service(ws, idle, "socket")
    path = C.socket_path(ws)
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    try:
        path.unlink()  # we hold server.lock, so any socket here is stale
    except FileNotFoundError:
        pass
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    old = os.umask(0o077)
    try:
        listener.bind(str(path))
    finally:
        os.umask(old)
    listener.listen(64)
    listener.settimeout(0.5)
    service.jobs.start()
    info = dict(pid=os.getpid(), socket=str(path), started=service.started, version=VERSION, workspace=str(ws),
                python=sys.executable, tools=str(C.TOOLS), db=str(C.db_path(ws)))
    tmp = sdir / f"service.json.{os.getpid()}"
    tmp.write_text(json.dumps(info))
    os.replace(tmp, sdir / "service.json")
    threading.Thread(target=service.watchdog, args=(lock_path, os.stat(lock_path).st_ino), daemon=True).start()
    print(f"kit-service: serving {ws} on {path} (pid {os.getpid()})", file=sys.stderr, flush=True)
    _ready(ready_fd, b"1")
    threads = []
    try:
        while not service.stopping.is_set():
            try:
                conn, _ = listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            t = threading.Thread(target=_serve_connection, args=(service, conn), daemon=True)
            t.start()
            threads = [x for x in threads if x.is_alive()] + [t]
    finally:
        listener.close()
        drain = time.time() + 60
        while service.active and time.time() < drain:
            time.sleep(0.05)
        service.close()
        try:
            if os.stat(path).st_ino and (C.read_service_info(ws) or {}).get("pid") == os.getpid():
                path.unlink()
                (sdir / "service.json").unlink()
        except OSError:
            pass
        fcntl.flock(lock, fcntl.LOCK_UN)
        lock.close()
    return 0


def run_stdio(ws):
    service = Service(ws, mode="stdio")
    service.jobs.start()
    out = sys.stdout.buffer
    sys.stdout = sys.stderr  # stray prints from backends must never corrupt the protocol
    wlock = threading.Lock()
    for line in sys.stdin.buffer:
        if not line.strip():
            continue
        reply = service.handle_line(line, client="stdio")
        if reply is not None:
            with wlock:
                out.write(reply)
                out.flush()
    service.close()
    return 0


def _ping(path):
    if not path:
        return False
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(2)
            s.connect(path)
            s.sendall(C.encode(dict(jsonrpc="2.0", id=1, method="service.ping")))
            return b'"pong":true' in s.makefile("rb").readline()
    except OSError:
        return False


def _ready(fd, value):
    if fd is not None:
        try:
            os.write(fd, value)
            os.close(fd)
        except OSError:
            pass


def daemonize(ws, idle):
    """Fork twice, detach from the caller's session and report readiness through a pipe."""
    r, w = os.pipe()
    if os.fork() > 0:
        os.close(w)
        deadline = time.time() + 120
        data = b""
        while time.time() < deadline:
            import select
            ready, _, _ = select.select([r], [], [], 0.5)
            if ready:
                data = os.read(r, 1)
                break
        return 0 if data == b"1" else 1
    os.close(r)
    os.setsid()
    if os.fork() > 0:
        os._exit(0)
    log = open(C.service_dir(ws) / "service.log", "ab", buffering=0)
    devnull = os.open(os.devnull, os.O_RDONLY)
    os.dup2(devnull, 0)
    os.dup2(log.fileno(), 1)
    os.dup2(log.fileno(), 2)
    sys.stdout = sys.stderr = open(log.fileno(), "w", buffering=1, closefd=False)
    try:
        code = run_socket(ws, idle, ready_fd=w)
    except BaseException:
        traceback.print_exc()
        _ready(w, b"0")
        code = 1
    os._exit(code)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--workspace")
    parser.add_argument("--stdio", action="store_true", help="serve JSON-RPC on stdin/stdout instead of the socket")
    parser.add_argument("--detach", action="store_true", help="daemonize; exit 0 once the socket is ready")
    parser.add_argument("--idle", type=float, default=DEFAULT_IDLE, help="exit after this many idle seconds")
    args = parser.parse_args(argv)
    ws = C.workspace(args.workspace)
    C.service_dir(ws).mkdir(parents=True, exist_ok=True)
    if args.stdio:
        return run_stdio(ws)
    if args.detach:
        return daemonize(ws, args.idle)
    return run_socket(ws, args.idle)


if __name__ == "__main__":
    sys.exit(main())
