"""Python client for the Kit service with auto-start.

    from kit_service.client import Client
    Client().call("index.search", {"disc": DISC, "role": "sky"})

Connects to the socket named in <workspace>/service/service.json. If nothing
answers, it takes <workspace>/service/start.lock (so concurrent clients start
one server), starts `server.py --detach` and waits for it to become ready.
One connection per call keeps long polls from blocking other callers.
"""
import fcntl
import json
import os
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

from . import common as C
from .common import KitError, ServiceUnavailable

SERVER = C.PACKAGE / "server.py"


class Client:
    def __init__(self, workspace=None, autostart=True, python=None, start_timeout=60.0):
        self.ws = C.workspace(workspace)
        self.autostart = autostart
        self.python = python or sys.executable
        self.start_timeout = start_timeout
        self.ids = 0
        self.lock = threading.Lock()
        self.started_server = False

    def _next_id(self):
        with self.lock:
            self.ids += 1
            return self.ids

    def _connect(self):
        info = C.read_service_info(self.ws)
        if not info or not info.get("socket"):
            raise ServiceUnavailable("no service.json")
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            s.settimeout(5)
            s.connect(info["socket"])
            s.settimeout(None)
        except OSError as error:
            s.close()
            raise ServiceUnavailable(str(error)) from None
        return s

    def ensure(self):
        try:
            self._connect().close()
            return
        except ServiceUnavailable:
            if not self.autostart:
                raise
        sdir = C.service_dir(self.ws)
        sdir.mkdir(parents=True, exist_ok=True)
        with open(sdir / "start.lock", "a+") as lock:
            deadline = time.time() + self.start_timeout
            while True:
                try:
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except OSError:
                    if time.time() > deadline:
                        raise ServiceUnavailable("timed out waiting for another client to start the service") from None
                    time.sleep(0.05)
            try:
                try:
                    self._connect().close()
                    return
                except ServiceUnavailable:
                    pass
                env = dict(os.environ, EOD_KIT_WORKSPACE=str(self.ws))
                proc = subprocess.run([self.python, "-I", str(SERVER), "--workspace", str(self.ws), "--detach"],
                                      stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                      env=env, timeout=self.start_timeout + 60)
                if proc.returncode != 0:
                    raise ServiceUnavailable(f"service failed to start (see {sdir / 'service.log'})")
                self.started_server = True
                self._connect().close()
            finally:
                fcntl.flock(lock, fcntl.LOCK_UN)

    def call(self, method, params=None, timeout=None):
        """Result of one JSON-RPC call. Raises KitError (application) or ServiceUnavailable (transport)."""
        line = self.raw(method, params, timeout)
        msg = json.loads(line)
        if "error" in msg:
            err = msg["error"]
            data = err.get("data") or {}
            raise KitError(data.get("code") or str(err.get("code")), data.get("message") or err.get("message", ""),
                           data.get("hint"), data.get("details"))
        return msg.get("result")

    def raw(self, method, params=None, timeout=None):
        payload = C.encode(dict(jsonrpc="2.0", id=self._next_id(), method=method, params=params or {}))
        for attempt in (0, 1):
            try:
                s = self._connect()
            except ServiceUnavailable:
                if attempt or not self.autostart:
                    raise
                self.ensure()
                continue
            try:
                s.settimeout(timeout)
                s.sendall(payload)
                reader = s.makefile("rb")
                line = reader.readline()
                reader.close()
            except OSError as error:
                raise ServiceUnavailable(f"connection failed during {method}: {error}") from None
            finally:
                s.close()
            if not line:
                # The server stopped (idle/code change) after accepting; requests are only retried
                # when nothing could have been applied, so retry reads only.
                if attempt or not _read_only(method, params):
                    raise ServiceUnavailable(f"service closed the connection during {method}")
                self.ensure()
                continue
            return line
        raise ServiceUnavailable("unreachable")


READ_ONLY = {"service.ping", "service.status", "index.search", "index.get", "index.status", "index.census",
             "job.status", "job.list", "job.events", "lock.list", "state.get"}


READ_ONLY_ACTIONS = {"catalog", "entries", "environment", "inspect", "load", "validate", "profile_status",
                     "export_batch_status", "reimport_check"}


def _read_only(method, params):
    if method == "backend":
        return isinstance(params, dict) and params.get("action") in READ_ONLY_ACTIONS
    return method in READ_ONLY


def stdio_client(workspace=None, python=None):
    """A stdio-mode server process (for scripts that must not leave a daemon)."""
    env = dict(os.environ, EOD_KIT_WORKSPACE=str(C.workspace(workspace)))
    return subprocess.Popen([python or sys.executable, "-I", str(SERVER), "--stdio", "--workspace", str(C.workspace(workspace))],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env)
