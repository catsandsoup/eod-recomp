"""Automated verification suite for Sprint 3F-2: Additive First-Party NASCAR Vehicle Lifecycle.

Uses the REAL NASCAR: Dirt to Daytona ISO (STOCK.CAR + TRUCK.CAR) and REAL retail EOD disc
to verify 3D uicar_/uicars_ icons, 5 unpainted Sweeney's primer skins, full-panel PAINTKIT
UV mapping + UIBIG CANV spray-painting, zero retail overwrite, UI controls, and < 1.5 s Tracy budget.
"""

from __future__ import annotations

import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

EDITOR_DIR = Path(__file__).resolve().parent
TOOLS_DIR = EDITOR_DIR.parent
for _p in (str(EDITOR_DIR), str(TOOLS_DIR)):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import numpy as np
from PIL import Image
from PySide6.QtWidgets import QApplication

from import_nascar_cars import find_entry, transcode_modl
from iso9660_reader import NascarSource
import nascar_firstparty as nfp
import nascar_gen as gen
from nascar_import_panel import NascarImportPanel
import nascar_parts
import nascar_roster
from perf_profiler import get_profiler
import rst0
import tex
import valt

REAL_NASCAR_ISO = Path("/Users/monty/Downloads/NASCAR - Dirt to Daytona (USA).iso")
REAL_EOD_DISC = Path("/Users/monty/Library/Application Support/TDEODRecomp/disc")
REAL_EOD_DATA = REAL_EOD_DISC / "DATA"


def _extract_nascar_car_assets(
    staged: Path, car_archive: str, paint_res: str, paint_name: str, family: str, style: str, body_tex: str
) -> tuple[bytes, Image.Image]:
    """Extract & transcode Low1.mod (with panel splits) + 512x256 RGBA livery from staged NASCAR archives."""
    with tempfile.TemporaryDirectory(prefix="eod-fp-src-") as td:
        td_path = Path(td)
        car_dir = td_path / "car"
        paint_dir = td_path / "paint"
        with contextlib.redirect_stdout(io.StringIO()):
            rst0.unpack_archive(str(staged / car_archive), str(car_dir))
            rst0.unpack_archive(str(staged / "RES" / paint_res), str(paint_dir))
        raw_mod = find_entry(car_dir, "Low1.mod").read_bytes()
        mod_blob = transcode_modl(raw_mod)
        model = gen.read_modl(mod_blob)
        nascar_roster.remap_family_groups(model, family, style, body=body_tex)
        nascar_parts.split_model(model, {body_tex.lower()})
        mod_bytes = gen.write_modl(model)
        try:
            livery_raw = find_entry(paint_dir, paint_name).read_bytes()
        except FileNotFoundError:
            cands = sorted(p for p in paint_dir.iterdir() if p.name.lower().startswith("car") and p.name.lower().endswith(".tex"))
            if not cands:
                raise
            livery_raw = cands[0].read_bytes()
        w, h, rgba, _ = tex.decompile_tex(livery_raw)
        livery_img = Image.frombytes("RGBA", (w, h), rgba).resize((512, 256), Image.Resampling.LANCZOS)
        return mod_bytes, livery_img


class TestNascarFirstPartyLifecycle(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = QApplication.instance() or QApplication(["test_nascar_firstparty"])
        if not REAL_NASCAR_ISO.is_file() or not (REAL_EOD_DATA / "RES" / "UIBIG.RES").is_file():
            raise unittest.SkipTest("Requires real NASCAR ISO and retail EOD DATA/RES/")
        disc = NascarSource(REAL_NASCAR_ISO)
        cls._stage_cm = disc.stage(["CAR/STOCK.CAR", "CAR/TRUCK.CAR", "RES/PAINT256.RES", "RES/TRUCKSET.RES"])
        staged = cls._stage_cm.__enter__()
        cls.stock_mod, cls.stock_livery = _extract_nascar_car_assets(
            staged, "STOCK.CAR", "PAINT256.RES", "Car24.tex", "stock", "chevy", "Stock1.tex"
        )
        cls.truck_mod, cls.truck_livery = _extract_nascar_car_assets(
            staged, "TRUCK.CAR", "TRUCKSET.RES", "Car1.tex", "truck", "all", "Truck1.tex"
        )

    @classmethod
    def tearDownClass(cls) -> None:
        if hasattr(cls, "_stage_cm"):
            cls._stage_cm.__exit__(None, None, None)

    def test_1_render_and_encode_3d_uicar_and_uicars_icons(self) -> None:
        for label, mod_b, liv in (("STOCK", self.stock_mod, self.stock_livery), ("TRUCK", self.truck_mod, self.truck_livery)):
            uicar_im, uicars_im = nfp.render_vehicle_ui_icons(mod_b, liv, size=256)
            for tag, im in ((f"{label}_uicar", uicar_im), (f"{label}_uicars", uicars_im)):
                self.assertEqual(im.size, (256, 256), tag)
                tm0_b, tex_b = nfp.encode_ui_icon_pair(im)
                w, h, rgba, meta = tex.decompile_tex(tm0_b)
                self.assertEqual((w, h), (256, 256), tag)
                self.assertEqual(meta["levels"], 8, tag)
                arr = np.frombuffer(rgba, dtype=np.uint8).reshape(h, w, 4)
                opaque_ratio = float((arr[:, :, 3] > 16).mean())
                self.assertGreaterEqual(opaque_ratio, 0.10, f"{tag} too sparse: {opaque_ratio:.3f}")
                self.assertLessEqual(opaque_ratio, 0.70, f"{tag} not alpha-masked: {opaque_ratio:.3f}")
                # Verify transparent corner pixels (alpha == 0)
                self.assertEqual(int(arr[0, 0, 3]), 0, f"{tag} top-left corner must be transparent")
                # Verify 16x16 .tex pair also decodes cleanly
                sw, sh, _, _ = tex.decompile_tex(tex_b)
                self.assertEqual((sw, sh), (16, 16), f"{tag}.tex")

    def test_2_five_unpainted_sweeneys_primer_skins_clean_and_encodable(self) -> None:
        regions = nfp.derive_paintkit_regions(self.stock_mod)
        skins = nfp.generate_sweeneys_primer_skins(self.stock_livery, regions)
        self.assertEqual(set(skins.keys()), set(nfp.SWEENEYS_PRIMERS.keys()))
        self.assertEqual(len(skins), 5)

        src_arr = np.asarray(self.stock_livery, dtype=np.float32)
        hx0, hy0, hx1, hy1 = nfp.parse_panel_rect(regions["hood"])
        dx0, dy0, dx1, dy1 = nfp.parse_panel_rect(regions["ldoor"])
        src_hood_std = float(src_arr[hy0:hy1, hx0:hx1, :3].std(axis=(0, 1)).max())
        self.assertGreater(src_hood_std, 18.0, "Source Car24 hood should have high sponsor variance")

        for skin_name, target_rgb in nfp.SWEENEYS_PRIMERS.items():
            im = skins[skin_name]
            self.assertEqual(im.size, (512, 256))
            arr = np.asarray(im, dtype=np.float32)
            for pname, (x0, y0, x1, y1) in (("hood", (hx0, hy0, hx1, hy1)), ("ldoor", (dx0, dy0, dx1, dy1))):
                patch = arr[y0 : y1 + 1, x0 : x1 + 1, :3]
                std = float(patch.std(axis=(0, 1)).max())
                self.assertLess(std, 8.0, f"{skin_name} {pname} still has sponsor/number noise (max_ch_std={std:.2f})")
                mean_rgb = patch.mean(axis=(0, 1))
                for ch in range(3):
                    self.assertLess(abs(mean_rgb[ch] - target_rgb[ch]), 22.0, f"{skin_name} {pname} ch{ch} drift")
            # Encode to CT.RES 512x256 .tm0 + 256x128 .tex and decode cleanly
            tm0_b, tex_b = nfp.encode_livery_tex_pair(im)
            w, h, _, _ = tex.decompile_tex(tm0_b)
            self.assertEqual((w, h), (512, 256))
            sw, sh, _, _ = tex.decompile_tex(tex_b)
            self.assertEqual((sw, sh), (256, 128))

    def test_3_paintkit_7_panels_and_uibig_canv_spray_all_panels(self) -> None:
        uibig = nfp.FastRst0Archive(REAL_EOD_DATA / "RES" / "UIBIG.RES")
        algar_cvs = uibig.read("Algar-o.cvs")
        self.assertEqual(len(algar_cvs), nfp.CANV_SIZE)

        for label, mod_b, liv in (("Cupcar", self.stock_mod, self.stock_livery), ("NTruck", self.truck_mod, self.truck_livery)):
            regions = nfp.derive_paintkit_regions(mod_b, 512, 256)
            rects: dict[str, tuple[int, int, int, int]] = {}
            for pname in nfp.PAINTKIT_PANELS:
                self.assertIn(pname, regions, f"{label} missing panel {pname}")
                x0, y0, x1, y1 = nfp.parse_panel_rect(regions[pname])
                self.assertTrue(0 <= x0 < x1 <= 511, f"{label} {pname} X invalid: {(x0, x1)}")
                self.assertTrue(0 <= y0 < y1 <= 255, f"{label} {pname} Y invalid: {(y0, y1)}")
                self.assertGreaterEqual((x1 - x0) * (y1 - y0), 100, f"{label} {pname} area too small")
                rects[pname] = (x0, y0, x1, y1)
            # Pairwise non-overlapping verification
            pnames = list(nfp.PAINTKIT_PANELS)
            for i in range(len(pnames)):
                for j in range(i + 1, len(pnames)):
                    ax0, ay0, ax1, ay1 = rects[pnames[i]]
                    bx0, by0, bx1, by1 = rects[pnames[j]]
                    overlap = (min(ax1, bx1) > max(ax0, bx0)) and (min(ay1, by1) > max(ay0, by0))
                    self.assertFalse(overlap, f"{label} overlap between {pnames[i]}={rects[pnames[i]]} and {pnames[j]}={rects[pnames[j]]}")

            primers = nfp.generate_sweeneys_primer_skins(liv, regions)
            cvs = nfp.build_uibig_canvas(primers["primer_gray"], algar_cvs)
            self.assertEqual(len(cvs), nfp.CANV_SIZE)
            self.assertEqual(cvs[: nfp.CANV_HEADER_SIZE], algar_cvs[: nfp.CANV_HEADER_SIZE])

            # Simulate spray-painting every single one of the 7 panels with distinct bright colors
            spray_colors = [
                (250, 20, 20, 255),
                (20, 240, 20, 255),
                (20, 40, 250, 255),
                (250, 240, 20, 255),
                (250, 20, 240, 255),
                (20, 240, 240, 255),
                (250, 128, 10, 255),
            ]
            cur_cvs = cvs
            for pname, col in zip(nfp.PAINTKIT_PANELS, spray_colors):
                next_cvs = nfp.paint_uibig_canvas_panel(cur_cvs, regions[pname], col)
                before_px = np.frombuffer(cur_cvs[nfp.CANV_HEADER_SIZE :], dtype=np.uint8).reshape(256, 512, 4)
                after_px = np.frombuffer(next_cvs[nfp.CANV_HEADER_SIZE :], dtype=np.uint8).reshape(256, 512, 4)
                x0, y0, x1, y1 = rects[pname]
                changed = int(np.any(before_px[y0 : y1 + 1, x0 : x1 + 1] != after_px[y0 : y1 + 1, x0 : x1 + 1], axis=2).sum())
                self.assertGreater(changed, 50, f"{label} spraying {pname} modified too few pixels: {changed}")
                cur_cvs = next_cvs

    def test_4_additive_registration_zero_retail_loss_and_tracy_budget(self) -> None:
        prof = get_profiler()
        prof.clear_zones()
        with tempfile.TemporaryDirectory(prefix="eod-fp-reg-") as tmp:
            out_data = Path(tmp) / "DATA"
            res = nfp.register_additive_firstparty_vehicle(
                eod_data=REAL_EOD_DATA,
                output_data=out_data,
                chassis="Cupcar",
                livery_id=663,
                modl_bytes=self.stock_mod,
                livery_rgba=self.stock_livery,
                driver_name="Jeff Gordon",
                display_number="24",
                stats=(10, 8, 6),
                price=18000,
            )
            total_ms = res["timings_ms"]["total_ms"]
            print(f"\n[Sprint 3F-2 Tracy Benchmark] Cupcar #24 -> {res['profile_summary']}")
            self.assertLess(total_ms, 1500.0, f"Single-car first-party build exceeded 1.5s budget: {total_ms:.1f}ms")

            # Verify Tracy zones + Chrome Trace export
            zstats = prof.zone_stats()
            for stage in ("paintkit_uv", "sweeneys_primer", "render_icons", "uibig_canvas", "pack_res"):
                self.assertIn(stage, zstats, f"Missing Tracy zone {stage}")
            trace_path = prof.dump_chrome_trace(Path(tmp) / "nascar_firstparty_trace.json")
            trace_json = json.loads(trace_path.read_text(encoding="utf-8"))
            self.assertGreaterEqual(len(trace_json["traceEvents"]), 5)

            # Verify additive COMMON.RES: NASCAR Class 4 + 26 retail buycar chassis + 130 retail vehicles preserved
            out_common = nfp.FastRst0Archive(out_data / "RES" / "COMMON.RES")
            cc = valt.parse_binary_valt(out_common.read("carclass.val"))
            self.assertTrue(any(s["name"] == "Nascar" for s in cc["sections"]))
            cls4 = next(s for s in cc["sections"] if s["name"] == "$0004$chassis")
            self.assertTrue(any(k == "Cupcar" for k, _ in cls4["pairs"]))

            bc = valt.parse_binary_valt(out_common.read("buycar.val"))
            cars = [s for s in bc["sections"] if s["name"] == "car"]
            vehs = [s for s in bc["sections"] if s["name"] == "vehicle"]
            self.assertEqual(len(cars), 27)       # 26 retail + 1 Cupcar
            self.assertEqual(len(vehs), 135)      # 130 retail + 5 unpainted Sweeney's
            self.assertEqual(len(res["dealer_ids"]), 5)

            # Verify UICAR.RES has BOTH uicar_ & uicars_ for driver + all 5 dealer IDs
            out_uicar = nfp.FastRst0Archive(out_data / "RES" / "UICAR.RES")
            for vid in [663, *res["dealer_ids"]]:
                for prefix in ("uicar", "uicars"):
                    tm0_w, tm0_h, _, _ = tex.decompile_tex(out_uicar.read(f"{prefix}_{vid}.tm0"))
                    tex_w, tex_h, _, _ = tex.decompile_tex(out_uicar.read(f"{prefix}_{vid}.tex"))
                    self.assertEqual((tm0_w, tm0_h), (256, 256))
                    self.assertEqual((tex_w, tex_h), (16, 16))

            # Verify PAINTKIT.RES & UIBIG.RES additive entries & retail count preservation
            out_pk = nfp.FastRst0Archive(out_data / "RES" / "PAINTKIT.RES")
            reg_tbl = valt.parse_binary_valt(out_pk.read("regions.val"))
            self.assertEqual(len(reg_tbl["sections"]), 27)  # 26 retail + Cupcar
            cup_sec = next(s for s in reg_tbl["sections"] if s["name"] == "Cupcar")
            cup_map = dict(cup_sec["pairs"])
            for p in nfp.PAINTKIT_PANELS:
                self.assertIn(p, cup_map)

            out_uibig = nfp.FastRst0Archive(out_data / "RES" / "UIBIG.RES")
            self.assertEqual(len(out_uibig.read("Cupcar-o.cvs")), nfp.CANV_SIZE)

            # Layer a second chassis (NTruck) additively onto out_data/RES and verify zero collision!
            res2 = nfp.register_additive_firstparty_vehicle(
                eod_data=REAL_EOD_DATA,
                output_data=out_data,
                existing_res_dir=out_data / "RES",
                chassis="NTruck",
                livery_id=710,
                modl_bytes=self.truck_mod,
                livery_rgba=self.truck_livery,
                driver_name="Ted Musgrave",
                display_number="1",
                stats=(9, 6, 8),
                price=14000,
            )
            print(f"[Sprint 3F-2 Tracy Benchmark] NTruck #1  -> {res2['profile_summary']}")
            self.assertLess(res2["timings_ms"]["total_ms"], 1500.0)
            self.assertTrue(set(res["dealer_ids"]).isdisjoint(set(res2["dealer_ids"])))

    def test_5_nascar_import_panel_firstparty_options_and_telemetry_label(self) -> None:
        with tempfile.TemporaryDirectory(prefix="eod-fp-ui-") as tmp:
            fake_game = Path(tmp) / "game"
            (fake_game / "DATA" / "CAR").mkdir(parents=True, exist_ok=True)
            panel = NascarImportPanel(fake_game)
            self.addCleanup(panel.close)
            self.assertTrue(panel.chk_firstparty_icons.isChecked())
            self.assertTrue(panel.chk_sweeneys_skins.isChecked())
            self.assertTrue(panel.chk_paintkit_support.isChecked())
            self.assertEqual(
                panel.firstparty_options,
                {"enable_icons": True, "enable_sweeneys": True, "enable_paintkit": True},
            )
            panel.chk_sweeneys_skins.setChecked(False)
            self.assertFalse(panel.firstparty_options["enable_sweeneys"])
            panel._on_import_finished(1, ([], {"total_ms": 412.5, "render_icons": 55.0}, "Tracy: 412 ms (render_icons 55ms)"))
            self.assertIn("412 ms", panel.lbl_profile_summary.text())
            self.assertIn("412 ms", panel.status.text())


if __name__ == "__main__":
    unittest.main(verbosity=2)
