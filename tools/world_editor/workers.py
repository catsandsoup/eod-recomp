"""Non-blocking QThreadPool background worker infrastructure for EOD World Editor.

Thread-ownership contract:
  - Worker runnables NEVER touch QWidget, QPixmap, or OpenGL (GL.*) calls.
  - Worker runnables perform disk I/O, binary archive parsing, VIF decoding,
    PIL->QImage thumbnail rasterization, and mod compilation off the UI thread,
    delivering results back to the Qt main thread via queued signals.
  - CancelToken + monotonic generation IDs guarantee stale jobs are discarded
    cleanly with zero race conditions.
"""

from __future__ import annotations

import os
import threading
import time
import traceback
from typing import Any, Callable

from PySide6.QtCore import QObject, QRunnable, QThreadPool, Signal

# Keep strong Python references to running QRunnables + their WorkerSignals until the
# Qt MAIN thread drains all queued signals (avoids 75-95% PySide6 worker-thread GC drops!).
_LIVE_WORKERS_LOCK = threading.Lock()
_LIVE_WORKERS: set[QRunnable] = set()


class _WorkerJanitor(QObject):
    """Main-thread QObject that releases finished QRunnables AFTER queued signals drain."""

    releaseRequested = Signal(object)

    def __init__(self) -> None:
        super().__init__()
        self.releaseRequested.connect(self._on_release)

    def _on_release(self, worker: object) -> None:
        with _LIVE_WORKERS_LOCK:
            _LIVE_WORKERS.discard(worker)  # type: ignore[arg-type]


_JANITOR: _WorkerJanitor | None = None


def _get_janitor() -> _WorkerJanitor:
    global _JANITOR
    if _JANITOR is None:
        _JANITOR = _WorkerJanitor()
    return _JANITOR


def start_worker(worker: QRunnable, priority: int = 0) -> None:
    """Safely submit a QRunnable to the editor QThreadPool with main-thread lifetime cleanup."""
    _get_janitor()  # ensure janitor lives on the Qt main thread
    worker.setAutoDelete(False)
    with _LIVE_WORKERS_LOCK:
        _LIVE_WORKERS.add(worker)
    editor_thread_pool().start(worker, priority)


def _release_worker(worker: QRunnable) -> None:
    """Queue release onto the Qt main thread so all prior queued signals deliver first."""
    if _JANITOR is not None:
        _JANITOR.releaseRequested.emit(worker)
    else:
        with _LIVE_WORKERS_LOCK:
            _LIVE_WORKERS.discard(worker)


class CancelToken:
    """Thread-safe cooperative cancellation flag."""

    def __init__(self, generation: int = 0):
        self.generation = generation
        self._cancelled = threading.Event()

    def cancel(self) -> None:
        self._cancelled.set()

    @property
    def is_cancelled(self) -> bool:
        return self._cancelled.is_set()


class WorkerSignals(QObject):
    """Signals emitted by FunctionWorker back to the Qt UI thread."""

    started = Signal(int, str)          # generation, label
    progress = Signal(int, int, str)    # generation, percent (0..100), status
    finished = Signal(int, object)      # generation, result
    failed = Signal(int, str)           # generation, error message
    cancelled = Signal(int)             # generation


class FunctionWorker(QRunnable):
    """Generic cancellable background callable running on QThreadPool."""

    def __init__(
        self,
        fn: Callable[..., Any],
        *args: Any,
        token: CancelToken | None = None,
        label: str = "Working...",
        **kwargs: Any,
    ):
        super().__init__()
        self.setAutoDelete(False)
        self.fn = fn
        self.args = args
        self.kwargs = kwargs
        self.token = token or CancelToken(0)
        self.label = label
        self.signals = WorkerSignals()

    def report_progress(self, pct: int, msg: str = "") -> None:
        if not self.token.is_cancelled:
            self.signals.progress.emit(self.token.generation, max(0, min(100, int(pct))), msg)

    def run(self) -> None:
        gen = self.token.generation
        try:
            if self.token.is_cancelled:
                self.signals.cancelled.emit(gen)
                return
            self.signals.started.emit(gen, self.label)
            try:
                result = self.fn(*self.args, token=self.token, progress=self.report_progress, **self.kwargs)
                if self.token.is_cancelled:
                    self.signals.cancelled.emit(gen)
                else:
                    self.signals.finished.emit(gen, result)
            except Exception as exc:
                if self.token.is_cancelled:
                    self.signals.cancelled.emit(gen)
                else:
                    tb = traceback.format_exc(limit=4)
                    self.signals.failed.emit(gen, f"{exc}\n{tb}")
        finally:
            _release_worker(self)


class ThumbnailBatchSignals(QObject):
    """Signals emitted per rendered QImage thumbnail from a worker thread."""

    thumbnailReady = Signal(int, str, object)  # generation, model_key, QImage
    batchDone = Signal(int)                    # generation


class ThumbnailBatchWorker(QRunnable):
    """Off-thread 3D model thumbnail rasterizer emitting thread-safe QImages."""

    def __init__(
        self,
        items: list[tuple[str, Any]],
        textures: dict[str, Any],
        render_qimage_fn: Callable[[Any, dict[str, Any]], Any],
        token: CancelToken,
    ):
        super().__init__()
        self.setAutoDelete(False)
        self.items = items
        self.textures = textures
        self.render_qimage_fn = render_qimage_fn
        self.token = token
        self.signals = ThumbnailBatchSignals()

    def run(self) -> None:
        gen = self.token.generation
        try:
            for idx, (mkey, model) in enumerate(self.items):
                if self.token.is_cancelled:
                    return
                try:
                    qimg = self.render_qimage_fn(model, self.textures)
                    if not self.token.is_cancelled:
                        self.signals.thumbnailReady.emit(gen, mkey, qimg)
                except Exception:
                    continue
                # Cooperative GIL yield so UI + 3D viewport stay silky-smooth on low-end CPUs
                if (idx & 3) == 3:
                    time.sleep(0.002)
            if not self.token.is_cancelled:
                self.signals.batchDone.emit(gen)
        finally:
            _release_worker(self)


def editor_thread_pool() -> QThreadPool:
    """Return the shared QThreadPool sized gently so the UI thread always keeps a free core."""
    pool = QThreadPool.globalInstance()
    cpus = os.cpu_count() or 2
    target = max(2, min(4, cpus - 1))
    pool.setMaxThreadCount(target)
    return pool


def shutdown_editor_workers() -> None:
    """Cancel editor jobs and keep their Qt signal objects alive until they stop."""
    with _LIVE_WORKERS_LOCK:
        workers = tuple(_LIVE_WORKERS)
    for worker in workers:
        if isinstance(worker, (FunctionWorker, ThumbnailBatchWorker)):
            worker.token.cancel()
    if workers:
        editor_thread_pool().waitForDone()
        with _LIVE_WORKERS_LOCK:
            _LIVE_WORKERS.difference_update(workers)
