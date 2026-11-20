"""Native Qt checks for Car Studio's verified retail handling sheet."""

from __future__ import annotations

import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import time
import unittest

EDITOR_DIR = Path(__file__).resolve().parent
if str(EDITOR_DIR) not in sys.path:
    sys.path.insert(0, str(EDITOR_DIR))

from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QScrollArea

from car_studio_handling import CarHandlingSheet
import modding_backend as backend
from world_loader import default_game_dir


def _valt_rows(data: bytes) -> dict[tuple[int, int, bytes, bytes], bytes]:
    """Read VALT v11 row strings directly from the on-disk pointers."""
    def string_at(offset: int) -> bytes:
        return data[offset:data.index(0, offset)] if offset else b""

    rows = {}
    section_count, = struct.unpack_from("<I", data, 4)
    for section in range(section_count):
        _, name, _, count, pairs, _, _ = struct.unpack_from("<7I", data, 16 + 28 * section)
        for index in range(count):
            key, value = struct.unpack_from("<2I", data, pairs + 8 * index)
            rows[section, index, string_at(name), string_at(key)] = string_at(value)
    return rows


class TestCarStudioHandling(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = QApplication.instance() or QApplication(["test_car_studio_handling"])

    def wait_for(self, predicate, sheet: CarHandlingSheet) -> None:
        deadline = time.monotonic() + 10
        while not predicate() and time.monotonic() < deadline:
            self.app.processEvents()
            time.sleep(0.005)
        self.assertTrue(predicate(), sheet.status.text())

    def edit(self, sheet: CarHandlingSheet, field_id: str, value: str) -> None:
        line = sheet.editors[field_id]
        line.setFocus()
        line.selectAll()
        QTest.keyClicks(line, value)
        self.assertEqual(line.text(), value)
        line.editingFinished.emit()

    def check_native_delta(self, retail: Path, overlay: Path) -> None:
        source_entries = backend.directory(retail)
        built_entries = backend.directory(overlay)
        self.assertEqual([entry["name"] for entry in built_entries],
                         [entry["name"] for entry in source_entries])
        for entry in source_entries:
            name = entry["name"]
            original, _ = backend.payload(retail, name, source_entries, verify_crc=False)
            built, _ = backend.payload(overlay, name, built_entries, verify_crc=name.lower() == "physics.val")
            if name.lower() == "physics.val":
                source_rows, built_rows = _valt_rows(original), _valt_rows(built)
                self.assertEqual(list(source_rows), list(built_rows))
                changed = [(*row[2:], value, built_rows[row])
                           for row, value in source_rows.items() if value != built_rows[row]]
                self.assertEqual(changed, [
                    (b"Chassis", b"mass", b"1800", b"2000"),
                    (b"Transmission", b"rear_end_ratio", b"6.9", b"7.2"),
                ])
            else:
                self.assertEqual(original, built, name)

    def test_digit_handling_saves_only_two_native_values_and_reopens(self) -> None:
        disc = default_game_dir()
        retail = disc / "DATA/CAR/DIGIT.CAR"
        if not retail.is_file():
            self.skipTest("Set EOD_DISC to an extracted retail disc containing DIGIT.CAR.")
        before = hashlib.sha256(retail.read_bytes()).digest()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            sheet = CarHandlingSheet(disc, project_root=root)
            self.addCleanup(sheet.close)
            sheet.resize(420, 500)
            sheet.show()
            self.wait_for(lambda: ("Chassis", "mass") in sheet.field_ids, sheet)
            self.assertEqual(sheet.current_archive, "DATA/CAR/DIGIT.CAR")
            self.assertEqual(len(sheet.editors), 5)
            mass = sheet.field_ids["Chassis", "mass"]
            ratio = sheet.field_ids["Transmission", "rear_end_ratio"]
            wheel_lock = sheet.field_ids["Chassis", "wheel_lock"]

            self.assertEqual(sheet.editors[mass].text(), "1800")
            for invalid in ("NaN", "0", "1e50", "1e-900"):
                with self.subTest(invalid=invalid):
                    self.edit(sheet, mass, invalid)
                    self.assertFalse(sheet.save_button.isEnabled())
                    sheet.save_button.click()
            self.assertEqual(list(root.rglob("*")), [])

            self.edit(sheet, mass, "2000")
            sheet.undo_button.click()
            self.assertEqual(sheet.editors[mass].text(), "1e-900")
            sheet.redo_button.click()
            self.assertEqual(sheet.editors[mass].text(), "2000")

            sheet.view_mode.setCurrentText("All Handling")
            self.assertIn(ratio, sheet.editors)
            self.assertNotIn(wheel_lock, sheet.editors)
            self.app.processEvents()
            scroll = sheet.findChild(QScrollArea)
            self.assertGreater(scroll.verticalScrollBar().maximum(), 0)
            self.assertEqual(scroll.horizontalScrollBar().maximum(), 0)
            self.edit(sheet, ratio, "7.2")
            sheet.reset_button.click()
            self.assertEqual(sheet.editors[mass].text(), "1800")
            self.assertEqual(sheet.editors[ratio].text(), "6.9")
            sheet.undo_button.click()
            self.assertEqual(sheet.editors[mass].text(), "2000")
            self.assertEqual(sheet.editors[ratio].text(), "7.2")

            results = []
            sheet.archiveSaved.connect(results.append)
            sheet.save_button.click()
            self.wait_for(lambda: bool(results), sheet)
            output = Path(results[0]["archivePath"])
            self.assertTrue(output.is_relative_to(root))
            self.assertEqual(output.name, "DIGIT.CAR")
            self.check_native_delta(retail, output)
            self.assertEqual(before, hashlib.sha256(retail.read_bytes()).digest())

            reopened = CarHandlingSheet(disc, project_root=root)
            self.addCleanup(reopened.close)
            self.wait_for(lambda: mass in reopened.editors, reopened)
            self.assertEqual(reopened.editors[mass].text(), "2000")
            self.assertEqual(reopened.saved_archive["archivePath"], str(output))
            self.assertFalse(reopened.save_button.isEnabled())


if __name__ == "__main__":
    unittest.main()
