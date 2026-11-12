"""Pools of long-lived backend lane processes (kit_service/lane.py).

Each lane has its own processes, so its work never shares an interpreter (or the
GIL) with the service threads that answer interactive reads, nor with other lanes.
A process is started on first use, reused while it lives, and replaced if it dies.
"""
import json
import os
import queue
import subprocess
import sys
import threading
import time

from .common import PACKAGE

_CORES = os.cpu_count() or 8
# Most processes per lane (EOD_KIT_LANE_MODEL / EOD_KIT_LANE_WRITE override). None exist until
# work arrives; each one exits after EOD_KIT_LANE_IDLE seconds unused (server.py watchdog).
LANE_PROCS = dict(model=max(2, _CORES // 4), write=2)


class LaneDied(Exception):
    pass


class _Proc:
    def __init__(self, lane):
        env = {k: v for k, v in os.environ.items() if k in ("PATH", "HOME", "TMPDIR", "LANG", "LC_ALL")
               or k.startswith("EOD_")}
        self.popen = subprocess.Popen([sys.executable, "-I", str(PACKAGE / "lane.py"), lane],
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env, close_fds=True)
        self.seq = 0
        self.last_used = time.time()

    def call(self, request):
        self.seq += 1
        try:
            self.popen.stdin.write(json.dumps(dict(id=self.seq, request=request)).encode() + b"\n")
            self.popen.stdin.flush()
            header = self.popen.stdout.readline()
            envelope = self.popen.stdout.readline()
        except (OSError, ValueError) as error:
            raise LaneDied(str(error)) from error
        if not header or not envelope:
            raise LaneDied(f"lane process {self.popen.pid} exited ({self.popen.poll()})")
        return json.loads(header), envelope.rstrip(b"\n")

    def alive(self):
        return self.popen.poll() is None

    def close(self):
        try:
            self.popen.stdin.close()
        except OSError:
            pass
        try:
            self.popen.wait(2)
        except subprocess.TimeoutExpired:
            self.popen.kill()


class Lane:
    def __init__(self, name, size):
        self.name, self.size = name, size
        self.idle = queue.LifoQueue()
        self.started = 0
        self.calls = 0
        self.lock = threading.Lock()
        self.all = []

    def _take(self):
        try:
            return self.idle.get_nowait()
        except queue.Empty:
            pass
        with self.lock:
            if self.started < self.size:
                self.started += 1
                proc = _Proc(self.name)
                self.all.append(proc)
                return proc
        return self.idle.get()

    def call(self, request):
        proc = self._take()
        try:
            self.calls += 1
            return proc.call(request)
        except LaneDied:
            with self.lock:
                self.started -= 1
                self.all = [p for p in self.all if p is not proc]
            proc = None
            raise
        finally:
            if proc is not None:
                proc.last_used = time.time()
                self.idle.put(proc)

    def clear(self):
        """Drop the caches of every idle process (busy ones keep theirs until their next clear)."""
        procs = []
        while True:
            try:
                procs.append(self.idle.get_nowait())
            except queue.Empty:
                break
        for proc in procs:
            try:
                proc.popen.stdin.write(b'{"clear": true}\n')
                proc.popen.stdin.flush()
                proc.popen.stdout.readline()
                proc.popen.stdout.readline()
                self.idle.put(proc)
            except (OSError, ValueError):
                with self.lock:
                    self.started -= 1
                    self.all = [p for p in self.all if p is not proc]

    def reap(self, idle_seconds):
        """Close processes unused for idle_seconds; the lane starts new ones on demand."""
        keep = []
        while True:
            try:
                proc = self.idle.get_nowait()
            except queue.Empty:
                break
            if time.time() - proc.last_used >= idle_seconds:
                with self.lock:
                    self.started -= 1
                    self.all = [p for p in self.all if p is not proc]
                proc.close()
            else:
                keep.append(proc)
        for proc in keep:
            self.idle.put(proc)

    def stats(self):
        return dict(processes=sum(1 for p in self.all if p.alive()), size=self.size, calls=self.calls,
                    pids=[p.popen.pid for p in self.all if p.alive()])

    def close(self):
        for proc in list(self.all):
            proc.close()


class Lanes:
    def __init__(self):
        self.lanes = {name: Lane(name, int(os.environ.get("EOD_KIT_LANE_" + name.upper(), str(n))))
                      for name, n in LANE_PROCS.items()}

    def call(self, lane, request):
        """Run one request in a lane process. Returns (header, envelope bytes). Retries once if the
        process died before answering a read; a write is never repeated."""
        try:
            return self.lanes[lane].call(request)
        except LaneDied:
            if lane == "write":
                raise
            return self.lanes[lane].call(request)

    def clear(self):
        for lane in self.lanes.values():
            lane.clear()

    def reap(self, idle_seconds):
        for lane in self.lanes.values():
            lane.reap(idle_seconds)

    def stats(self):
        return {name: lane.stats() for name, lane in self.lanes.items()}

    def close(self):
        for lane in self.lanes.values():
            lane.close()
