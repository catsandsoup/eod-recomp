"""Synthetic-disc verification for the World Editor's local profile manager."""

from __future__ import annotations

import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

EDITOR_DIR = Path(__file__).resolve().parent
if str(EDITOR_DIR) not in sys.path:
    sys.path.insert(0, str(EDITOR_DIR))

from PySide6.QtWidgets import QApplication, QFileDialog, QInputDialog, QMessageBox

from main_window import EodWorldEditorWindow
from tools.tests import test_mod_package as fixtures
from workers import shutdown_editor_workers
import mod_install
import mod_package


class TestModManager(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = QApplication.instance() or QApplication(["test_mod_manager"])

    @classmethod
    def tearDownClass(cls) -> None:
        shutdown_editor_workers()

    def wait_for_profile(self, dialog, *, expect_error: bool = False) -> None:
        deadline = time.monotonic() + 10
        while dialog._token is not None and time.monotonic() < deadline:
            self.app.processEvents()
            time.sleep(0.005)
        self.assertIsNone(dialog._token)
        self.assertEqual(dialog.status_label.text().startswith("Failed:"), expect_error, dialog.status_label.text())

    def test_file_menu_opens_local_profile_dialog(self) -> None:
        with tempfile.TemporaryDirectory() as work:
            disc = fixtures.make_disc(Path(work))
            window = EodWorldEditorWindow(game_dir=disc, auto_load=False)
            try:
                file_menu = next(action.menu() for action in window.menuBar().actions() if action.text() == "&File")
                actions = {action.text(): action for action in file_menu.actions()}
                self.assertIn("Mod Manager && Profiles...", actions)
                with patch("mod_manager.ModManagerDialog") as dialog:
                    dialog.return_value.pie_request = None
                    actions["Mod Manager && Profiles..."].trigger()
                    dialog.assert_called_once_with(disc, parent=window, allow_pie=False)
                    dialog.return_value.exec.assert_called_once()
            finally:
                window.close()

    def test_profile_picker_install_toggle_conflicts_and_uninstall(self) -> None:
        from mod_manager import ModManagerDialog

        with tempfile.TemporaryDirectory() as work:
            scratch = Path(work)
            disc = fixtures.make_disc(scratch)
            retail = (disc / fixtures.ANOKA).read_bytes()
            profiles = scratch / "profiles"
            zips = []
            for name, seed in (("first", 11), ("second", 12)):
                package = fixtures.make_package(
                    scratch / name, disc, name, {(fixtures.ANOKA, "sky.tex"): fixtures.make_tex(seed)}
                )
                zip_path = scratch / f"{name}.eodmod"
                mod_package.zip_package(package, zip_path)
                zips.append(zip_path)

            dialog = ModManagerDialog(disc, root=profiles)
            try:
                self.wait_for_profile(dialog)
                self.assertFalse(profiles.exists())
                with patch.object(QInputDialog, "getText", return_value=("test-profile", True)):
                    dialog.new_profile_button.click()
                self.wait_for_profile(dialog)
                self.assertEqual(dialog.profile_picker.currentText(), "test-profile")
                self.assertFalse(profiles.exists())
                for zip_path in zips:
                    with patch.object(QFileDialog, "getOpenFileName", return_value=(str(zip_path), "")):
                        dialog.install_button.click()
                    self.wait_for_profile(dialog)

                status = mod_install.status(profile="test-profile", root=profiles)
                self.assertEqual([(m["id"], m["enabled"]) for m in status["packages"]],
                                 [("first", False), ("second", False)])
                self.assertEqual(dialog.conflicts.topLevelItemCount(), 1)
                self.assertIn("sky.tex", dialog.conflicts.topLevelItem(0).text(0))
                self.assertIn("Unresolved", dialog.conflicts.topLevelItem(0).text(2))

                dialog.packages.setCurrentItem(dialog.packages.topLevelItem(0))
                dialog.toggle_button.click()
                self.wait_for_profile(dialog)
                self.assertEqual(dialog.packages.topLevelItem(0).text(3), "Enabled")
                dialog.packages.setCurrentItem(dialog.packages.topLevelItem(1))
                dialog.toggle_button.click()
                self.wait_for_profile(dialog, expect_error=True)
                self.assertIn("Conflicting edits", dialog.status_label.text())
                self.assertEqual(dialog.packages.topLevelItem(1).text(3), "Disabled")
                with patch.object(QInputDialog, "getItem", return_value=("second", True)):
                    dialog.conflicts.setCurrentItem(dialog.conflicts.topLevelItem(0))
                    dialog.winner_button.click()
                self.wait_for_profile(dialog)
                dialog.packages.setCurrentItem(dialog.packages.topLevelItem(1))
                dialog.toggle_button.click()
                self.wait_for_profile(dialog)
                status = mod_install.status(profile="test-profile", root=profiles)
                self.assertEqual(status["conflicts"][0]["winner"], "second")
                self.assertTrue(status["packages"][1]["enabled"])
                self.assertEqual(dialog.conflicts.topLevelItem(0).text(2), "Winner: second")
                active = profiles / "test-profile" / "active" / fixtures.ANOKA
                member, _ = mod_package.payload(active, "sky.tex")
                self.assertEqual(member, fixtures.make_tex(12))

                dialog.toggle_button.click()
                self.wait_for_profile(dialog)
                self.assertEqual(dialog.packages.topLevelItem(1).text(3), "Disabled")
                with patch.object(QMessageBox, "question", return_value=QMessageBox.StandardButton.Yes):
                    dialog.uninstall_button.click()
                self.wait_for_profile(dialog)
                self.assertEqual(dialog.packages.topLevelItemCount(), 1)
                self.assertEqual(dialog.conflicts.topLevelItemCount(), 0)
                dialog.profile_picker.setCurrentText("default")
                self.wait_for_profile(dialog)
                self.assertEqual(dialog.packages.topLevelItemCount(), 0)
                self.assertFalse((profiles / "default").exists())
                self.assertEqual((disc / fixtures.ANOKA).read_bytes(), retail)
            finally:
                dialog.close()
