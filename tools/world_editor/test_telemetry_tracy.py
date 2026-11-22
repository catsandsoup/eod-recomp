#!/usr/bin/env python3
"""Unit & integration tests for Tracy + Perfetto/Chrome-Trace CPU/GPU/RAM telemetry.

Covers both:
  1. World Editor Python telemetry (`perf_profiler.py`, `explorer_panel.py`, `viewport_gl.py`)
     and native Plume viewport library (`eod_editor_plume`).
  2. Game Runtime C++ telemetry (`runtime/core/eod_tracy.h`, `runtime/main.cpp`,
     `runtime/render/native_feed.cpp`, `runtime/render/native_renderer.cpp`) via
     headless 45-frame run with `EOD_TRACE_OUT`.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

EDITOR_DIR = Path(__file__).resolve().parent
REPO_ROOT = EDITOR_DIR.parent.parent
if str(EDITOR_DIR) not in sys.path:
    sys.path.insert(0, str(EDITOR_DIR))

from PySide6.QtWidgets import QApplication

import world_loader as wl
from explorer_panel import ExplorerPanel
from perf_profiler import (
    GpuFrameTimings,
    PerfProfiler,
    get_process_rss_mb,
    get_profiler,
)
from viewport_gl import ViewportGL
from workers import editor_thread_pool

DISC_DIR = Path("/Users/monty/Library/Application Support/TDEODRecomp/disc")
AFTON_TRK = DISC_DIR / "DATA" / "TRACKS" / "AFTON.TRK"
BUILD_DIR = REPO_ROOT / "build-mac-plume"
EOD_BIN = BUILD_DIR / "eod"
PLUME_LIB = BUILD_DIR / "eod_editor_plume.dylib"


class TestTelemetryTracy(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls._app = QApplication.instance() or QApplication(["test-telemetry-tracy"])

    def test_perf_profiler_zone_stats_rss_and_chrome_trace(self) -> None:
        rss_mb = get_process_rss_mb()
        self.assertGreater(rss_mb, 0.0, "get_process_rss_mb() must return positive process RSS")

        prof = PerfProfiler(max_events=20000)
        for i in range(25):
            with prof.zone("ui.sample_op", "ui"):
                # Tiny deterministic work so durations are measurable and ordered
                _ = sum(j * j for j in range(100 + (i % 5) * 200))
        for _ in range(5):
            with prof.zone("pick.ray", "pick"):
                time.sleep(0.0005)

        prof.record_frame(
            cpu_ms=3.5,
            gpu_timings=GpuFrameTimings(
                terrain_ms=0.8,
                props_ms=0.6,
                water_ms=0.2,
                overlays_ms=0.1,
                total_gpu_ms=1.7,
                gpu_queries_active=True,
            ),
            draw_calls=42,
            tris_drawn=18500,
        )

        stats = prof.zone_stats()
        self.assertIn("ui.sample_op", stats)
        self.assertIn("pick.ray", stats)

        s_ui = stats["ui.sample_op"]
        for key in ("count", "mean_ms", "p50_ms", "p95_ms", "p99_ms", "max_ms", "total_ms", "rss_mb_end"):
            self.assertIn(key, s_ui)
        self.assertEqual(s_ui["count"], 25)
        self.assertGreaterEqual(s_ui["p50_ms"], 0.0)
        self.assertLessEqual(s_ui["p50_ms"], s_ui["p95_ms"] + 1e-9)
        self.assertLessEqual(s_ui["p95_ms"], s_ui["p99_ms"] + 1e-9)
        self.assertLessEqual(s_ui["p99_ms"], s_ui["max_ms"] + 1e-9)
        self.assertGreater(s_ui["total_ms"], 0.0)
        self.assertGreater(s_ui["rss_mb_end"], 0.0)

        # Verify ring buffer bounding
        tiny = PerfProfiler(max_events=4)
        for _ in range(10):
            with tiny.zone("ring.test", "ui"):
                pass
        self.assertEqual(tiny.zone_stats()["ring.test"]["count"], 4)

        with tempfile.TemporaryDirectory() as td:
            out_path = prof.dump_chrome_trace(Path(td) / "editor_trace.json")
            self.assertTrue(out_path.exists())
            data = json.loads(out_path.read_text(encoding="utf-8"))
            self.assertIn("traceEvents", data)
            events = data["traceEvents"]
            x_events = [e for e in events if e.get("ph") == "X"]
            c_events = [e for e in events if e.get("ph") == "C"]
            c_names = {e.get("name") for e in c_events}
            self.assertEqual(len(x_events), 30)
            self.assertIn("ProcessRSS_MB", c_names)
            self.assertIn("GpuFrame_ms", c_names)
            self.assertIn("zoneStats", data.get("otherData", {}))

    def test_explorer_filter_and_viewport_picking_zones(self) -> None:
        self.assertTrue(AFTON_TRK.exists(), f"Missing retail track: {AFTON_TRK}")
        world = wl.load_track_world(AFTON_TRK)
        prof = get_profiler()
        prof.clear_zones()

        explorer = ExplorerPanel()
        viewport = ViewportGL()
        viewport.resize(640, 480)
        try:
            explorer.set_world(world)
            # Trigger Explorer search filter -> _rebuild_model_grid
            explorer.model_search.setText("tree")
            explorer._rebuild_model_grid()
            explorer.model_search.setText("")
            explorer._rebuild_model_grid()

            # Trigger viewport picking (screen + CPU raycast)
            viewport.set_world(world)
            viewport._pick_object_at_screen(320.0, 240.0)
            ray_o, ray_d = viewport.screen_ray(320.0, 240.0)
            viewport._pick_object_with_t(ray_o, ray_d)

            stats = prof.zone_stats()
            self.assertIn("explorer.rebuild_grid", stats)
            self.assertGreaterEqual(stats["explorer.rebuild_grid"]["count"], 2)
            self.assertIn("viewport.pick_screen", stats)
            self.assertIn("viewport.pick_cpu", stats)
            self.assertGreaterEqual(stats["viewport.pick_cpu"]["count"], 2)
            self.assertGreater(stats["viewport.pick_cpu"]["rss_mb_end"], 0.0)

            with tempfile.TemporaryDirectory() as td:
                trace_path = prof.dump_chrome_trace(Path(td) / "editor_ui_trace.json")
                parsed = json.loads(trace_path.read_text(encoding="utf-8"))
                names = {e.get("name") for e in parsed["traceEvents"] if e.get("ph") == "X"}
                self.assertIn("explorer.rebuild_grid", names)
                self.assertIn("viewport.pick_cpu", names)
                print(
                    f"\n[TELEMETRY_EDITOR] RSS={get_process_rss_mb():.1f} MB | "
                    f"explorer.rebuild_grid p50={stats['explorer.rebuild_grid']['p50_ms']:.2f} ms (n={stats['explorer.rebuild_grid']['count']}) | "
                    f"viewport.pick_cpu p50={stats['viewport.pick_cpu']['p50_ms']:.2f} ms (n={stats['viewport.pick_cpu']['count']})"
                )
        finally:
            explorer._cancel_thumb_worker()
            editor_thread_pool().waitForDone(2000)
            explorer.close()
            viewport.close()

    def test_game_runtime_chrome_trace_headless(self) -> None:
        self.assertTrue(EOD_BIN.exists(), f"Missing built eod binary: {EOD_BIN}")
        self.assertTrue(PLUME_LIB.exists(), f"Missing built eod_editor_plume library: {PLUME_LIB}")
        self.assertTrue(DISC_DIR.exists(), f"Missing retail disc data dir: {DISC_DIR}")

        with tempfile.TemporaryDirectory() as td:
            trace_out = Path(td) / "game_trace.json"
            env = os.environ.copy()
            env.update(
                {
                    "EOD_TRACE_OUT": str(trace_out),
                    "EOD_SKIP_MOVIES": "1",
                    "EOD_FAST_BOOT": "1",
                    "EOD_SKIP_MEMCARD": "1",
                    "EOD_DIRECT_START": "mode=action;track=Afton;event=Battle;car=Algar;paint=0;humans=1;ai=2",
                }
            )
            cmd = [
                str(EOD_BIN),
                "--headless",
                "--frames",
                "45",
                "--data",
                str(DISC_DIR),
            ]
            proc = subprocess.run(
                cmd,
                cwd=str(REPO_ROOT),
                env=env,
                capture_output=True,
                text=True,
                timeout=90,
            )
            self.assertEqual(
                proc.returncode,
                0,
                f"eod headless failed (rc={proc.returncode}):\nSTDOUT:\n{proc.stdout[-1000:]}\nSTDERR:\n{proc.stderr[-1000:]}",
            )
            self.assertTrue(trace_out.exists(), "game_trace.json was not written by eod app_exit()")

            data = json.loads(trace_out.read_text(encoding="utf-8"))
            self.assertIn("traceEvents", data)
            events = data["traceEvents"]
            self.assertGreater(len(events), 2)

            x_events = [e for e in events if e.get("ph") == "X"]
            rss_counters = [e for e in events if e.get("ph") == "C" and e.get("name") == "RSS_MB"]
            self.assertGreater(len(x_events), 0, "Expected runtime duration ('X') traceEvents")
            self.assertGreater(len(rss_counters), 0, "Expected RSS_MB counter ('C') traceEvents")
            self.assertGreater(rss_counters[-1]["args"]["rss_mb"], 0.0)

            other = data.get("otherData", {})
            self.assertIn("phaseSummary", other)
            self.assertGreater(other.get("rss_mb_end", 0.0), 0.0)
            self.assertGreater(other.get("wall_ms", 0.0), 0.0)
            print(
                f"\n[TELEMETRY_GAME] wall={other['wall_ms']:.1f} ms | RSS={other['rss_mb_end']:.1f} MB | "
                f"x_spans={len(x_events)} | rss_samples={len(rss_counters)} | "
                f"trk_load={other['phaseSummary']['trk_load_ms']:.1f} ms | car_load={other['phaseSummary']['car_load_ms']:.1f} ms"
            )


if __name__ == "__main__":
    unittest.main()
