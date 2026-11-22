"""Unit and headless Qt tests for NASCAR ISO auto-scanner, Ephemeral Test-Drive, and NascarImportPanel."""

from __future__ import annotations

import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

EDITOR_DIR = Path(__file__).resolve().parent
TOOLS_DIR = EDITOR_DIR.parent
for _p in (str(EDITOR_DIR), str(TOOLS_DIR)):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QPushButton

from nascar_import_panel import NascarImportPanel
from nascar_scanner import (
    SERIES_ORDER,
    EphemeralTestDriveSession,
    NascarCatalogEntry,
    NascarScanResult,
    scan_nascar_source,
)
from rst0 import pack_archive
from valt import compile_binary_valt

REAL_NASCAR_ISO = Path("/Users/monty/Downloads/NASCAR - Dirt to Daytona (USA).iso")
REAL_EOD_DISC = Path("/Users/monty/Library/Application Support/TDEODRecomp/disc")


def _make_valt_roster(drivers: list[dict[str, str]]) -> bytes:
    """Compile a minimal valid binary VALT roster with Driver sections."""
    sections = []
    for drv in drivers:
        sections.append(
            {
                "name": "Driver",
                "parent": "",
                "sec_vtable": 0x0040C200,
                "tool_ptr": 0,
                "pairs": list(drv.items()),
            }
        )
    model = {
        "is_v6": False,
        "tool_vtable": 0x0040C218,
        "root_count": len(sections),
        "sections": sections,
    }
    return compile_binary_valt(model)


def _pack_synthetic_rst0(files: dict[str, tuple[bytes, int, str]], out_path: Path) -> None:
    """Pack a synthetic 0TSR v6 archive containing the supplied entries."""
    with tempfile.TemporaryDirectory(prefix="eod-synth-rst0-") as tmp:
        stage = Path(tmp)
        entries = []
        for name, (blob, ver, fourcc) in files.items():
            (stage / name).write_bytes(blob)
            entries.append(
                {
                    "name": name,
                    "size": len(blob),
                    "version": ver,
                    "fourcc": fourcc,
                    "is_extractable": False,
                }
            )
        (stage / "manifest.json").write_text(
            json.dumps({"version": 6, "entries": entries}), encoding="utf-8"
        )
        out_path.parent.mkdir(parents=True, exist_ok=True)
        with contextlib.redirect_stdout(io.StringIO()):
            pack_archive(str(stage), str(out_path))


def _build_synthetic_nascar_dir(root: Path) -> Path:
    """Create a CI-safe extracted NASCAR DATA/ folder spanning all 5 series."""
    data_dir = root / "DATA"
    car_dir = data_dir / "CAR"
    res_dir = data_dir / "RES"
    car_dir.mkdir(parents=True, exist_ok=True)
    res_dir.mkdir(parents=True, exist_ok=True)

    for car_name in ("STOCK.CAR", "TRUCK.CAR", "MODIFIED.CAR", "HOBBY.CAR", "PACECAR.CAR"):
        (car_dir / car_name).write_bytes(b"0TSR-SYNTH-" + car_name.encode("ascii"))

    common_files = {
        "cup.val": (
            _make_valt_roster(
                [
                    {"name": "Jeff Gordon", "number": "24", "body_style": "chevy"},
                    {"name": "Jeff Burton", "number": "99", "body_style": "ford"},
                    {"name": "Custom Slot", "number": "77", "body_style": "chevy", "custom": "yes"},
                ]
            ),
            11,
            "VALT",
        ),
        "truck.val": (
            _make_valt_roster([{"name": "Bobby Hamilton", "number": "4", "body_style": "dodge"}]),
            11,
            "VALT",
        ),
        "modified.val": (
            _make_valt_roster([{"name": "Mike Stefanik", "number": "16", "body_style": "chevy"}]),
            11,
            "VALT",
        ),
        "dirt.val": (
            _make_valt_roster([{"name": "Dirt Rookie", "number": "7", "body_style": "chevy"}]),
            11,
            "VALT",
        ),
    }
    _pack_synthetic_rst0(common_files, res_dir / "COMMON.RES")

    dummy_tex = b"TEX9" + b"\x00" * 60
    _pack_synthetic_rst0(
        {
            "Car24.tex": (dummy_tex, 9, "TEX"),
            "Car99.tex": (dummy_tex, 9, "TEX"),
            "Car101.tex": (dummy_tex, 9, "TEX"),
        },
        res_dir / "PAINT256.RES",
    )
    _pack_synthetic_rst0({"Car4.tex": (dummy_tex, 9, "TEX")}, res_dir / "TRUCKSET.RES")
    _pack_synthetic_rst0({"Car16.tex": (dummy_tex, 9, "TEX")}, res_dir / "MODFYSET.RES")
    _pack_synthetic_rst0({"Car7.tex": (dummy_tex, 9, "TEX")}, res_dir / "HOBBYSET.RES")
    return root


class TestNascarScannerAndPanel(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = QApplication.instance() or QApplication(["test_nascar_scanner"])

    def test_scan_synthetic_directory_all_five_series(self) -> None:
        with tempfile.TemporaryDirectory(prefix="eod-nascar-synth-") as tmp:
            root = _build_synthetic_nascar_dir(Path(tmp))
            result = scan_nascar_source(root)

            self.assertIsInstance(result, NascarScanResult)
            self.assertEqual(result.source_path, root.resolve())
            # 2 Cup drivers + 1 Cup alternate + 1 Truck + 1 Modified + 1 Hobby + 1 Pace Car = 7
            self.assertEqual(len(result.entries), 7)
            self.assertEqual(
                result.series_counts,
                {"Cup": 3, "Truck": 1, "Modified": 1, "Hobby": 1, "Pace Car": 1},
            )
            self.assertIn("Recognized 7 NASCAR vehicles across 5 series", result.summary_text)
            for series in SERIES_ORDER:
                self.assertIn(series, result.summary_text)

            families = {e.family for e in result.entries}
            self.assertEqual(families, {"cup", "truck", "modified", "dirt", "pace"})

            gordon = next(e for e in result.entries if e.series == "Cup" and e.car_number == "24")
            self.assertEqual(gordon.driver, "Jeff Gordon")
            self.assertEqual(gordon.style, "chevy")
            self.assertEqual(gordon.livery_name, "Car24.tex")
            self.assertEqual(gordon.car_archive, "STOCK.CAR")
            self.assertEqual(gordon.chassis, "Cupcar")

            burton = next(e for e in result.entries if e.series == "Cup" and e.car_number == "99")
            self.assertEqual(burton.style, "ford")
            self.assertEqual(burton.chassis, "CupFord")

            pace = next(e for e in result.entries if e.series == "Pace Car")
            self.assertEqual(pace.family, "pace")
            self.assertEqual(pace.car_archive, "PACECAR.CAR")
            self.assertEqual(pace.livery_name, "Pacecar.tex")

    def test_scan_real_nascar_iso_if_present(self) -> None:
        if not REAL_NASCAR_ISO.is_file():
            self.skipTest(f"Real NASCAR ISO not present on disk: {REAL_NASCAR_ISO}")
        result = scan_nascar_source(REAL_NASCAR_ISO)
        self.assertGreaterEqual(len(result.entries), 100)
        self.assertEqual(set(result.series_counts.keys()), set(SERIES_ORDER))
        for series in SERIES_ORDER:
            self.assertGreaterEqual(result.series_counts[series], 1, f"Missing series {series}")
        self.assertEqual({e.family for e in result.entries}, {"cup", "truck", "modified", "dirt", "pace"})
        self.assertIn(f"Recognized {len(result.entries)} NASCAR vehicles across 5 series", result.summary_text)

    def test_ephemeral_session_keep_and_discard_zero_trace(self) -> None:
        entry = NascarCatalogEntry(
            series="Cup",
            family="cup",
            style="chevy",
            car_number="24",
            driver="Jeff Gordon",
            livery_name="Car24.tex",
            car_archive="STOCK.CAR",
            raw_record={"family": "stock", "chassis": "Cupcar"},
        )
        with tempfile.TemporaryDirectory(prefix="eod-ephemeral-test-") as tmp:
            tmp_root = Path(tmp)
            session = EphemeralTestDriveSession(
                tmp_root / "source",
                tmp_root / "game",
                entry,
                temp_parent=tmp_root,
            )
            self.assertTrue(session.temp_dir.is_dir())
            self.assertTrue(session.temp_dir.name.startswith("eod-nascar-testdrive-"))

            # Populate compiled overlay files inside temp_dir/overlay
            car_file = session.overlay_dir / "DATA" / "CAR" / "CUPCAR.CAR"
            res_file = session.overlay_dir / "DATA" / "RES" / "COMMON.RES"
            meta_file = session.overlay_dir / "vehicle.json"
            car_file.parent.mkdir(parents=True, exist_ok=True)
            res_file.parent.mkdir(parents=True, exist_ok=True)
            car_payload = b"COMPILED-CUPCAR-PAYLOAD-BYTES"
            res_payload = b"COMPILED-COMMON-RES-BYTES"
            car_file.write_bytes(car_payload)
            res_file.write_bytes(res_payload)
            meta_file.write_text(json.dumps({"chassis": "Cupcar", "number": 663}), encoding="utf-8")

            # keep_to_directory copies compiled files cleanly without re-transcoding
            kept_dir = tmp_root / "kept-vehicle"
            returned_path = session.keep_to_directory(kept_dir)
            self.assertEqual(returned_path, kept_dir.resolve())
            self.assertEqual((kept_dir / "DATA" / "CAR" / "CUPCAR.CAR").read_bytes(), car_payload)
            self.assertEqual((kept_dir / "DATA" / "RES" / "COMMON.RES").read_bytes(), res_payload)
            self.assertTrue((kept_dir / "vehicle.json").is_file())

            # discard() removes temp_dir cleanly with zero trace while kept_dir remains untouched
            temp_path = session.temp_dir
            session.discard()
            self.assertTrue(session.is_discarded)
            self.assertIsNone(session.vehicle)
            self.assertFalse(temp_path.exists())
            self.assertTrue((kept_dir / "DATA" / "CAR" / "CUPCAR.CAR").is_file())

            # Idempotent second discard and post-discard keep guard
            session.discard()
            with self.assertRaises(RuntimeError):
                session.keep_to_directory(tmp_root / "should-fail")

    def test_ephemeral_session_real_build_if_iso_and_disc_present(self) -> None:
        if not REAL_NASCAR_ISO.is_file() or not (REAL_EOD_DISC / "DATA" / "CAR" / "ALGAR.CAR").is_file():
            self.skipTest("Requires both real NASCAR ISO and retail EOD disc")
        scan = scan_nascar_source(REAL_NASCAR_ISO)
        gordon = next(e for e in scan.entries if e.series == "Cup" and e.car_number == "24")

        with tempfile.TemporaryDirectory(prefix="eod-real-ephemeral-") as tmp:
            kept_dir = Path(tmp) / "kept-cup24"
            session = EphemeralTestDriveSession(REAL_NASCAR_ISO, REAL_EOD_DISC, gordon)
            try:
                vehicle = session.build()
                self.assertIsNotNone(session.vehicle)
                self.assertEqual(vehicle.chassis, "Cupcar")
                self.assertTrue(vehicle.archive.is_file())
                self.assertTrue(vehicle.layer.resolve().is_relative_to(session.temp_dir.resolve()))
                session.keep_to_directory(kept_dir)
                self.assertTrue((kept_dir / "DATA" / "CAR" / "CUPCAR.CAR").is_file())
                self.assertTrue((kept_dir / "DATA" / "RES" / "COMMON.RES").is_file())
                self.assertTrue((kept_dir / "DATA" / "RES" / "CT.RES").is_file())
                self.assertTrue((kept_dir / "DATA" / "RES" / "RACE.RES").is_file())
                self.assertTrue((kept_dir / "DATA" / "RES" / "UICAR.RES").is_file())
                self.assertTrue((kept_dir / "vehicle.json").is_file())
            finally:
                temp_path = session.temp_dir
                session.discard()
            self.assertFalse(temp_path.exists())

    def test_panel_headless_series_filtering_and_zero_jargon(self) -> None:
        with tempfile.TemporaryDirectory(prefix="eod-panel-test-") as tmp:
            tmp_root = Path(tmp)
            nascar_dir = _build_synthetic_nascar_dir(tmp_root / "nascar")
            fake_game_dir = tmp_root / "game"
            (fake_game_dir / "DATA" / "CAR").mkdir(parents=True, exist_ok=True)

            panel = NascarImportPanel(fake_game_dir)
            self.addCleanup(panel.close)
            panel.show()
            self.app.processEvents()

            # Load synthetic NASCAR folder synchronously
            res = panel.load_source(nascar_dir, synchronous=True)
            self.assertIsNotNone(res)
            self.assertIn("Recognized 7 NASCAR vehicles across 5 series", panel.lbl_summary.text())
            self.assertEqual(panel.vehicle_list.count(), 7)

            # Filter by each series pill
            panel.set_series_filter("Cup")
            self.assertEqual(panel.vehicle_list.count(), 3)
            panel.set_series_filter("Truck")
            self.assertEqual(panel.vehicle_list.count(), 1)
            self.assertIn("Bobby Hamilton", panel.vehicle_list.item(0).text())
            panel.set_series_filter("Pace Car")
            self.assertEqual(panel.vehicle_list.count(), 1)
            self.assertIn("NASCAR Pace Car", panel.vehicle_list.item(0).text())

            # Search filter across All
            panel.set_series_filter("All")
            panel.search_input.setText("Gordon")
            self.assertEqual(panel.vehicle_list.count(), 1)
            self.assertIn("#24", panel.vehicle_list.item(0).text())
            panel.search_input.clear()
            self.assertEqual(panel.vehicle_list.count(), 7)

            # All 6 required action buttons exist
            button_texts = {btn.text() for btn in panel.findChildren(QPushButton)}
            for expected_btn in (
                "Test Drive (Ephemeral)",
                "Keep Last Test-Drive",
                "Discard Test-Drive",
                "Import Selected",
                "Import Series",
                "Import All",
            ):
                self.assertIn(expected_btn, button_texts)

            # Compatibility contract: testDriveRequested signal & cancel() method
            self.assertTrue(hasattr(panel, "testDriveRequested"))
            self.assertTrue(callable(panel.cancel))
            panel.cancel()

            # Zero jargon anywhere in labels, buttons, tooltips, or placeholders
            forbidden_jargon = ("eod template", "eod class", "car24.tex", "livery entry")
            collected_texts: list[str] = []
            for cls in (QLabel, QPushButton, QLineEdit):
                for widget in panel.findChildren(cls):
                    if isinstance(widget, QLineEdit):
                        collected_texts.append(widget.text())
                        collected_texts.append(widget.placeholderText())
                    else:
                        collected_texts.append(widget.text())
                    collected_texts.append(widget.toolTip())
            joined = "\n".join(collected_texts).lower()
            for term in forbidden_jargon:
                self.assertNotIn(term, joined, f"Forbidden jargon {term!r} found in NascarImportPanel UI")


if __name__ == "__main__":
    unittest.main()
