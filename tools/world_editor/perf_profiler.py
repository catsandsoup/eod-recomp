"""Live GPU + CPU Performance Profiler, GL_TIME_ELAPSED Pass Timer & HUD for EOD World Editor.

Provides:
  - High-resolution `time.perf_counter_ns()` phase timers for `app_startup`,
    `track_load`, `mod_compile`, and `pie_launch` (plus custom phases).
  - Double-buffered OpenGL 3.3 Core `GL_TIME_ELAPSED` pass timer queries
    (`terrain_ms`, `props_ms`, `water_ms`, `overlays_ms`, `total_gpu_ms`)
    with safe automatic fallback when query objects are unavailable or not yet ready.
  - One-click `cProfile` trace capture & dump (saved to %LOCALAPPDATA%\\EODWorldEditor\\profiles
    and formatted for the Output Log).
  - Lightweight translucent `PerfHudOverlay` anchored in the 3D viewport (toggle via F3).
"""

from __future__ import annotations

import cProfile
import io
import os
import pstats
import threading
import time
from collections import deque
from contextlib import contextmanager
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterator

from PySide6.QtCore import Qt
from PySide6.QtGui import QFont
from PySide6.QtWidgets import QFrame, QGridLayout, QHBoxLayout, QLabel, QVBoxLayout, QWidget


PASS_NAMES: tuple[str, ...] = ("terrain", "props", "water", "overlays")
CORE_PHASES: tuple[str, ...] = ("app_startup", "track_load", "mod_compile", "pie_launch")


def profiler_appdata_dir() -> Path:
    """Return canonical writable per-user editor directory (respects EOD_EDITOR_APPDATA override)."""
    from world_loader import editor_appdata_dir

    return editor_appdata_dir()


@dataclass
class PhaseRecord:
    name: str
    last_ms: float = 0.0
    min_ms: float = 0.0
    max_ms: float = 0.0
    count: int = 0
    detail: str = ""
    updated_at: float = 0.0


@dataclass
class GpuFrameTimings:
    terrain_ms: float = 0.0
    props_ms: float = 0.0
    water_ms: float = 0.0
    overlays_ms: float = 0.0
    total_gpu_ms: float = 0.0
    gpu_queries_active: bool = False


class GpuPassTimer:
    """Double-buffered OpenGL 3.3 Core GL_TIME_ELAPSED pass timer with safe CPU fallback.

    Uses ping-pong query rings so we only ever query `GL_QUERY_RESULT_AVAILABLE` on
    the PREVIOUS frame's query objects — zero GPU pipeline stalls and zero GL errors.
    """

    def __init__(self) -> None:
        self._gl: Any = None
        self._supported: bool = False
        self._queries: list[dict[str, int]] = []  # 2 ping-pong sets of pass_name -> query_id
        self._active_set: int = 0
        self._frame_count: int = 0
        self._in_pass: str | None = None
        self._cpu_pass_start_ns: int = 0
        self._cpu_pass_ms: dict[str, float] = {k: 0.0 for k in PASS_NAMES}
        self._gpu_pass_ms: dict[str, float] = {k: 0.0 for k in PASS_NAMES}
        self._used_this_frame: list[set[str]] = [set(), set()]
        self.last_timings = GpuFrameTimings()

    @property
    def supported(self) -> bool:
        return self._supported

    def initialize(self, gl_module: Any) -> bool:
        """Allocate ping-pong GL_TIME_ELAPSED query objects inside a current GL context."""
        self._gl = gl_module
        self._supported = False
        self._queries = []
        try:
            if not hasattr(gl_module, "GL_TIME_ELAPSED") or not hasattr(gl_module, "glGenQueries"):
                return False
            # Probe support cleanly (some software drivers report 0 query bits)
            bits = gl_module.glGetQueryiv(gl_module.GL_TIME_ELAPSED, gl_module.GL_QUERY_COUNTER_BITS)
            if int(bits) <= 0:
                return False
            for _ in range(2):
                qids = gl_module.glGenQueries(len(PASS_NAMES))
                if isinstance(qids, int):
                    qlist = [int(qids)]
                else:
                    qlist = [int(x) for x in qids]
                self._queries.append(dict(zip(PASS_NAMES, qlist)))
            self._supported = True
            self._frame_count = 0
            self._active_set = 0
            self._used_this_frame = [set(), set()]
            return True
        except Exception:
            self._supported = False
            self._queries = []
            return False

    def cleanup(self) -> None:
        if self._gl is not None and self._queries:
            try:
                all_ids = [qid for qset in self._queries for qid in qset.values()]
                if all_ids:
                    self._gl.glDeleteQueries(len(all_ids), all_ids)
            except Exception:
                pass
        self._queries = []
        self._supported = False

    def begin_frame(self) -> None:
        """Harvest previous frame's completed GPU query results without stalling."""
        for k in PASS_NAMES:
            self._cpu_pass_ms[k] = 0.0
        self._used_this_frame[self._active_set].clear()

        if self._supported and self._frame_count >= 1 and self._gl is not None:
            prev_set = 1 - self._active_set
            prev_used = self._used_this_frame[prev_set]
            qmap = self._queries[prev_set]
            GL = self._gl
            any_gpu = False
            for name in PASS_NAMES:
                if name not in prev_used:
                    self._gpu_pass_ms[name] = 0.0
                    continue
                qid = qmap[name]
                try:
                    avail = GL.glGetQueryObjectiv(qid, GL.GL_QUERY_RESULT_AVAILABLE)
                    if int(avail):
                        ns = int(GL.glGetQueryObjectui64v(qid, GL.GL_QUERY_RESULT))
                        self._gpu_pass_ms[name] = ns / 1_000_000.0
                        any_gpu = True
                except Exception:
                    # Fallback silently if driver doesn't expose 64-bit query object getter
                    try:
                        avail = GL.glGetQueryObjectiv(qid, GL.GL_QUERY_RESULT_AVAILABLE)
                        if int(avail):
                            ns = int(GL.glGetQueryObjectuiv(qid, GL.GL_QUERY_RESULT))
                            self._gpu_pass_ms[name] = ns / 1_000_000.0
                            any_gpu = True
                    except Exception:
                        self._supported = False
                        break
            if any_gpu:
                self.last_timings.gpu_queries_active = True

    def begin_pass(self, pass_name: str) -> None:
        self._in_pass = pass_name
        self._cpu_pass_start_ns = time.perf_counter_ns()
        if self._supported and self._gl is not None and pass_name in self._queries[self._active_set]:
            try:
                qid = self._queries[self._active_set][pass_name]
                self._gl.glBeginQuery(self._gl.GL_TIME_ELAPSED, qid)
                self._used_this_frame[self._active_set].add(pass_name)
            except Exception:
                self._supported = False

    def end_pass(self, pass_name: str) -> None:
        elapsed_ms = (time.perf_counter_ns() - self._cpu_pass_start_ns) / 1_000_000.0
        self._cpu_pass_ms[pass_name] = self._cpu_pass_ms.get(pass_name, 0.0) + elapsed_ms
        if self._supported and self._gl is not None and pass_name in self._used_this_frame[self._active_set]:
            try:
                self._gl.glEndQuery(self._gl.GL_TIME_ELAPSED)
            except Exception:
                self._supported = False
        self._in_pass = None

    def end_frame(self) -> GpuFrameTimings:
        use_gpu = self._supported and self.last_timings.gpu_queries_active
        src = self._gpu_pass_ms if use_gpu else self._cpu_pass_ms
        t_ms = float(src.get("terrain", 0.0))
        p_ms = float(src.get("props", 0.0))
        w_ms = float(src.get("water", 0.0))
        o_ms = float(src.get("overlays", 0.0))
        self.last_timings = GpuFrameTimings(
            terrain_ms=t_ms,
            props_ms=p_ms,
            water_ms=w_ms,
            overlays_ms=o_ms,
            total_gpu_ms=t_ms + p_ms + w_ms + o_ms,
            gpu_queries_active=use_gpu,
        )
        self._active_set = 1 - self._active_set
        self._frame_count += 1
        return self.last_timings


class PerfProfiler:
    """Thread-safe high-resolution CPU + GPU profiler and cProfile trace manager."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._active_starts: dict[str, int] = {}
        self.phases: dict[str, PhaseRecord] = {k: PhaseRecord(name=k) for k in CORE_PHASES}
        self.gpu = GpuFrameTimings()
        self.cpu_frame_ms: float = 0.0
        self.fps: float = 0.0
        self.draw_calls: int = 0
        self.tris_drawn: int = 0
        self._frame_history: deque[float] = deque(maxlen=60)
        self._cprof: cProfile.Profile | None = None
        self._cprof_active: bool = False

    def begin_phase(self, name: str) -> int:
        now = time.perf_counter_ns()
        with self._lock:
            self._active_starts[name] = now
        return now

    def end_phase(self, name: str, detail: str = "") -> float:
        now = time.perf_counter_ns()
        with self._lock:
            start = self._active_starts.pop(name, None)
            ms = (now - start) / 1_000_000.0 if start is not None else 0.0
            self._record_phase_locked(name, ms, detail)
            return ms

    def record_phase(self, name: str, ms: float, detail: str = "") -> PhaseRecord:
        with self._lock:
            return self._record_phase_locked(name, float(ms), detail)

    def _record_phase_locked(self, name: str, ms: float, detail: str) -> PhaseRecord:
        rec = self.phases.get(name)
        if rec is None:
            rec = PhaseRecord(name=name)
            self.phases[name] = rec
        rec.last_ms = ms
        if rec.count == 0:
            rec.min_ms = ms
            rec.max_ms = ms
        else:
            rec.min_ms = min(rec.min_ms, ms)
            rec.max_ms = max(rec.max_ms, ms)
        rec.count += 1
        rec.detail = detail
        rec.updated_at = time.time()
        return rec

    @contextmanager
    def phase(self, name: str, detail: str = "") -> Iterator[None]:
        t0 = time.perf_counter_ns()
        try:
            yield
        finally:
            ms = (time.perf_counter_ns() - t0) / 1_000_000.0
            self.record_phase(name, ms, detail=detail)

    def record_frame(
        self,
        cpu_ms: float,
        gpu_timings: GpuFrameTimings | None = None,
        draw_calls: int = 0,
        tris_drawn: int = 0,
    ) -> None:
        with self._lock:
            self.cpu_frame_ms = float(cpu_ms)
            self._frame_history.append(self.cpu_frame_ms)
            avg = sum(self._frame_history) / max(1, len(self._frame_history))
            self.fps = (1000.0 / avg) if avg > 1e-3 else 0.0
            self.draw_calls = int(draw_calls)
            self.tris_drawn = int(tris_drawn)
            if gpu_timings is not None:
                self.gpu = gpu_timings

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            return {
                "cpu_frame_ms": self.cpu_frame_ms,
                "fps": self.fps,
                "draw_calls": self.draw_calls,
                "tris_drawn": self.tris_drawn,
                "terrain_ms": self.gpu.terrain_ms,
                "props_ms": self.gpu.props_ms,
                "water_ms": self.gpu.water_ms,
                "overlays_ms": self.gpu.overlays_ms,
                "total_gpu_ms": self.gpu.total_gpu_ms,
                "gpu_queries_active": self.gpu.gpu_queries_active,
                "phases": {
                    k: {
                        "last_ms": v.last_ms,
                        "min_ms": v.min_ms,
                        "max_ms": v.max_ms,
                        "count": v.count,
                        "detail": v.detail,
                    }
                    for k, v in self.phases.items()
                },
            }

    # -------------------------------------------------------------------
    # cProfile Trace Capture & Dump
    # -------------------------------------------------------------------
    def start_cprofile(self) -> None:
        if self._cprof_active:
            return
        self._cprof = cProfile.Profile()
        self._cprof.enable()
        self._cprof_active = True

    def dump_cprofile_trace(
        self,
        out_dir: Path | None = None,
        sample_fn: Any = None,
        top_n: int = 25,
    ) -> tuple[Path, str]:
        """Dump active cProfile session (or run `sample_fn` under cProfile) to disk and return (txt_path, summary)."""
        prof = self._cprof
        was_active = self._cprof_active
        if prof is not None and was_active:
            prof.disable()
            self._cprof_active = False
        else:
            prof = cProfile.Profile()
            prof.enable()
            try:
                if callable(sample_fn):
                    sample_fn()
                else:
                    # Lightweight built-in snapshot so dump always produces valid pstats output
                    _ = self.snapshot()
            finally:
                prof.disable()

        target_dir = Path(out_dir) if out_dir is not None else (profiler_appdata_dir() / "profiles")
        target_dir.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime("%Y%m%d_%H%M%S")
        prof_path = target_dir / f"eod_editor_{stamp}.prof"
        txt_path = target_dir / f"eod_editor_{stamp}.txt"
        prof.dump_stats(str(prof_path))

        buf = io.StringIO()
        snap = self.snapshot()
        buf.write(f"=== EOD World Editor cProfile Trace ({stamp}) ===\n")
        buf.write(
            f"Viewport: CPU={snap['cpu_frame_ms']:.2f}ms | GPU={snap['total_gpu_ms']:.2f}ms "
            f"(terrain={snap['terrain_ms']:.2f} props={snap['props_ms']:.2f} "
            f"water={snap['water_ms']:.2f} overlays={snap['overlays_ms']:.2f}) | "
            f"draws={snap['draw_calls']} tris={snap['tris_drawn']:,}\n"
        )
        for pname in CORE_PHASES:
            p = snap["phases"].get(pname, {})
            if p.get("count", 0) > 0:
                det = f" [{p['detail']}]" if p.get("detail") else ""
                buf.write(
                    f"Phase {pname:12s}: last={p['last_ms']:8.2f}ms  "
                    f"min={p['min_ms']:8.2f}ms  max={p['max_ms']:8.2f}ms  (n={p['count']}){det}\n"
                )
        buf.write("\n--- Top Functions (cumulative) ---\n")
        stats = pstats.Stats(prof, stream=buf)
        stats.strip_dirs().sort_stats("cumulative").print_stats(top_n)
        summary = buf.getvalue()
        txt_path.write_text(summary, encoding="utf-8")

        if was_active:
            # Restart clean continuous profiler session
            self._cprof = cProfile.Profile()
            self._cprof.enable()
            self._cprof_active = True

        return txt_path, summary


# Shared module-level profiler instance
_GLOBAL_PROFILER = PerfProfiler()


def get_profiler() -> PerfProfiler:
    return _GLOBAL_PROFILER


class PerfHudOverlay(QFrame):
    """Lightweight top-right translucent GPU + CPU Performance Profiler HUD (toggle via F3)."""

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.setObjectName("PerfHudOverlay")
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        self.setStyleSheet(
            """
            QFrame#PerfHudOverlay {
                background-color: rgba(13, 17, 24, 218);
                border: 1px solid rgba(90, 145, 220, 140);
                border-radius: 6px;
            }
            QFrame#PerfHudOverlay QLabel {
                color: #dbe6f6;
                background: transparent;
                border: none;
            }
            """
        )

        mono = QFont("Consolas", 8)
        mono.setStyleHint(QFont.StyleHint.Monospace)
        hdr_font = QFont("Segoe UI", 8, QFont.Weight.Bold)

        lay = QVBoxLayout(self)
        lay.setContentsMargins(10, 7, 10, 7)
        lay.setSpacing(3)

        top = QHBoxLayout()
        top.setSpacing(8)
        self.lbl_title = QLabel("PERF PROFILER (F3)")
        self.lbl_title.setFont(hdr_font)
        self.lbl_title.setStyleSheet("color: #60a5fa;")
        self.lbl_mode = QLabel("GL_TIME_ELAPSED")
        self.lbl_mode.setFont(mono)
        self.lbl_mode.setStyleSheet("color: #94a3b8;")
        top.addWidget(self.lbl_title)
        top.addStretch(1)
        top.addWidget(self.lbl_mode)
        lay.addLayout(top)

        self.lbl_frame = QLabel("FPS: --  |  CPU: 0.00 ms  |  GPU: 0.00 ms")
        self.lbl_frame.setFont(mono)
        lay.addWidget(self.lbl_frame)

        self.lbl_draws = QLabel("Draws: 0  |  Tris: 0")
        self.lbl_draws.setFont(mono)
        self.lbl_draws.setStyleSheet("color: #a5b4fc;")
        lay.addWidget(self.lbl_draws)

        grid = QGridLayout()
        grid.setContentsMargins(0, 2, 0, 2)
        grid.setHorizontalSpacing(10)
        grid.setVerticalSpacing(1)
        self._pass_labels: dict[str, QLabel] = {}
        for row, (key, label) in enumerate(
            (
                ("terrain_ms", "GPU Terrain"),
                ("props_ms", "GPU Props"),
                ("water_ms", "GPU Water"),
                ("overlays_ms", "GPU Overlays"),
            )
        ):
            k_lbl = QLabel(label)
            k_lbl.setFont(mono)
            k_lbl.setStyleSheet("color: #94a3b8;")
            v_lbl = QLabel("0.00 ms")
            v_lbl.setFont(mono)
            v_lbl.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
            grid.addWidget(k_lbl, row, 0)
            grid.addWidget(v_lbl, row, 1)
            self._pass_labels[key] = v_lbl
        lay.addLayout(grid)

        sep = QFrame()
        sep.setFrameShape(QFrame.Shape.HLine)
        sep.setStyleSheet("background: rgba(148, 163, 184, 55); max-height: 1px;")
        lay.addWidget(sep)

        pgrid = QGridLayout()
        pgrid.setContentsMargins(0, 2, 0, 0)
        pgrid.setHorizontalSpacing(10)
        pgrid.setVerticalSpacing(1)
        self._phase_labels: dict[str, QLabel] = {}
        for row, (pkey, plabel) in enumerate(
            (
                ("app_startup", "Startup"),
                ("track_load", "Track Load"),
                ("mod_compile", "Mod Compile"),
                ("pie_launch", "PIE Launch"),
            )
        ):
            k_lbl = QLabel(plabel)
            k_lbl.setFont(mono)
            k_lbl.setStyleSheet("color: #94a3b8;")
            v_lbl = QLabel("—")
            v_lbl.setFont(mono)
            v_lbl.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
            pgrid.addWidget(k_lbl, row, 0)
            pgrid.addWidget(v_lbl, row, 1)
            self._phase_labels[pkey] = v_lbl
        lay.addLayout(pgrid)

        self.setMinimumWidth(258)
        self.hide()

    @staticmethod
    def _budget_color(ms: float) -> str:
        if ms <= 8.0:
            return "#4ade80"
        if ms <= 16.6:
            return "#fbbf24"
        return "#f87171"

    def refresh(self, snap: dict[str, Any]) -> None:
        gpu_active = bool(snap.get("gpu_queries_active", False))
        self.lbl_mode.setText("GPU Query" if gpu_active else "CPU Fallback")
        self.lbl_mode.setStyleSheet(f"color: {'#4ade80' if gpu_active else '#fbbf24'};")

        cpu_ms = float(snap.get("cpu_frame_ms", 0.0))
        gpu_ms = float(snap.get("total_gpu_ms", 0.0))
        fps = float(snap.get("fps", 0.0))
        c_col = self._budget_color(max(cpu_ms, gpu_ms))
        self.lbl_frame.setStyleSheet(f"color: {c_col};")
        self.lbl_frame.setText(f"FPS: {fps:5.1f} | CPU: {cpu_ms:5.2f}ms | GPU: {gpu_ms:5.2f}ms")
        self.lbl_draws.setText(
            f"Draws: {int(snap.get('draw_calls', 0))}  |  Tris: {int(snap.get('tris_drawn', 0)):,}"
        )

        for key, lbl in self._pass_labels.items():
            val = float(snap.get(key, 0.0))
            lbl.setText(f"{val:6.2f} ms")
            lbl.setStyleSheet(f"color: {self._budget_color(val * 2.0)};")

        phases = snap.get("phases", {})
        for pkey, lbl in self._phase_labels.items():
            p = phases.get(pkey)
            if p and p.get("count", 0) > 0:
                ms = float(p["last_ms"])
                det = f" ({p['detail']})" if p.get("detail") else ""
                lbl.setText(f"{ms:7.1f} ms{det}")
                lbl.setStyleSheet("color: #e2e8f0;")
            else:
                lbl.setText("—")
                lbl.setStyleSheet("color: #64748b;")
        self.adjustSize()
