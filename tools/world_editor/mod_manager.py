"""Local .eodmod package management for a selected Creation Kit profile."""

from __future__ import annotations

from collections.abc import Callable
from functools import partial
from pathlib import Path
from typing import Any

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFileDialog,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QMessageBox,
    QPushButton,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

from workers import CancelToken, FunctionWorker, start_worker
import mod_install


def _profile_snapshot(
    root: Path,
    profile: str,
    disc: Path,
    mutate: Callable[[], Any] | None,
    *,
    token: CancelToken,
    progress: Callable[[int, str], None],
) -> dict[str, Any]:
    directory = mod_install.profile_dir(root, profile)
    if directory.is_symlink() or directory.resolve().is_relative_to(disc.resolve()):
        raise ValueError("Choose a profile directory outside the game folder.")
    if token.is_cancelled:
        raise InterruptedError("Profile operation cancelled")
    if mutate is not None:
        mutate()
    snapshot = mod_install.status(profile=profile, root=root)
    try:
        snapshot["all_conflicts"] = mod_install.conflicts(directory, mod_install.load(directory, profile), everything=True)
    except (OSError, ValueError) as exc:
        snapshot["all_conflicts"] = []
        snapshot["conflict_error"] = str(exc)
    return snapshot


class ModManagerDialog(QDialog):
    """Browse local profiles and run installer transactions in the editor worker pool."""

    def __init__(
        self, game_dir: Path, parent: QWidget | None = None, *, root: Path | None = None, allow_pie: bool = False,
    ) -> None:
        super().__init__(parent)
        self.game_dir = game_dir
        self.root = Path(root or mod_install.DEFAULT_ROOT).expanduser().resolve()
        self.snapshot: dict[str, Any] = {}
        self.pie_request: tuple[str, str] | None = None
        self._token: CancelToken | None = None
        self._completion_message = ""
        self.setWindowTitle("Mod Manager & Profiles")
        self.resize(780, 560)

        self.profile_picker = QComboBox()
        self.profile_picker.addItem("default")
        if self.root.is_dir():
            self.profile_picker.addItems(sorted(
                entry.name for entry in self.root.iterdir()
                if entry.name != "default" and mod_install.NAME_RE.fullmatch(entry.name)
                and not entry.is_symlink() and (entry / "profile.json").is_file()
            ))
        self.profile_picker.currentTextChanged.connect(self._load_selected_profile)
        self.new_profile_button = QPushButton("New Profile...")
        self.new_profile_button.clicked.connect(self._choose_new_profile)
        profile_row = QHBoxLayout()
        profile_row.addWidget(QLabel("Profile"))
        profile_row.addWidget(self.profile_picker, 1)
        profile_row.addWidget(self.new_profile_button)

        self.active_label = QLabel()
        self.active_label.setWordWrap(True)
        self.active_label.setTextFormat(Qt.TextFormat.PlainText)
        self.active_label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.packages = QTreeWidget()
        self.packages.setHeaderLabels(["Package", "ID", "Version", "State"])
        self.packages.setRootIsDecorated(False)
        self.packages.currentItemChanged.connect(self._update_controls)
        self.install_button = QPushButton("Install .eodmod...")
        self.install_button.setToolTip("Install disabled into the selected profile")
        self.install_button.clicked.connect(self._install)
        self.toggle_button = QPushButton("Enable")
        self.toggle_button.clicked.connect(self._toggle_enabled)
        self.uninstall_button = QPushButton("Uninstall...")
        self.uninstall_button.clicked.connect(self._uninstall)
        package_row = QHBoxLayout()
        package_row.addWidget(self.install_button)
        package_row.addWidget(self.toggle_button)
        package_row.addWidget(self.uninstall_button)
        package_row.addStretch()

        self.conflicts = QTreeWidget()
        self.conflicts.setHeaderLabels(["Asset", "Packages", "Resolution"])
        self.conflicts.setRootIsDecorated(False)
        self.conflicts.currentItemChanged.connect(self._update_controls)
        self.winner_button = QPushButton("Choose Conflict Winner...")
        self.winner_button.clicked.connect(self._choose_winner)

        self.status_label = QLabel()
        self.status_label.setWordWrap(True)
        self.status_label.setTextFormat(Qt.TextFormat.PlainText)
        self.status_label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        self.close_button = buttons.button(QDialogButtonBox.StandardButton.Close)
        buttons.rejected.connect(self.reject)
        layout = QVBoxLayout(self)
        layout.addLayout(profile_row)
        layout.addWidget(self.active_label)
        layout.addWidget(QLabel("Installed Packages"))
        layout.addWidget(self.packages, 2)
        layout.addLayout(package_row)
        layout.addWidget(QLabel("Conflicts Across Installed Packages"))
        layout.addWidget(self.conflicts, 1)
        layout.addWidget(self.winner_button, alignment=Qt.AlignmentFlag.AlignLeft)
        layout.addWidget(self.status_label)
        self.play_free_roam_button = QPushButton("Play Free Roam in PIE")
        self.play_free_roam_button.clicked.connect(lambda: self._choose_pie("free_roam"))
        self.play_event_button = QPushButton("Play Event in PIE")
        self.play_event_button.clicked.connect(lambda: self._choose_pie("event"))
        for play in (self.play_free_roam_button, self.play_event_button):
            buttons.addButton(play, QDialogButtonBox.ButtonRole.ActionRole)
            play.setVisible(allow_pie)
        self.allow_pie = allow_pie
        layout.addWidget(buttons)
        self._load_selected_profile()

    def reject(self) -> None:
        if self._token is None:
            super().reject()

    def _selected_package(self) -> QTreeWidgetItem | None:
        return self.packages.currentItem()

    def _update_controls(self, *_args: Any) -> None:
        idle = self._token is None
        item = self._selected_package()
        self.profile_picker.setEnabled(idle)
        self.new_profile_button.setEnabled(idle)
        self.install_button.setEnabled(idle and bool(self.snapshot))
        self.toggle_button.setEnabled(idle and item is not None)
        self.toggle_button.setText("Disable" if item is not None and item.text(3) == "Enabled" else "Enable")
        self.uninstall_button.setEnabled(idle and item is not None)
        self.winner_button.setEnabled(idle and self.conflicts.currentItem() is not None)
        self.close_button.setEnabled(idle)
        disc = self.snapshot.get("disc")
        can_play = (
            self.allow_pie and idle and self.snapshot.get("active", {}).get("composed")
            and bool(disc) and Path(disc).resolve() == self.game_dir.resolve()
            and any(package["enabled"] for package in self.snapshot["packages"])
        )
        self.play_free_roam_button.setEnabled(bool(can_play))
        self.play_event_button.setEnabled(bool(can_play))

    def _start_job(self, message: str, *, mutate: Callable[[], Any] | None = None, complete: str = "") -> None:
        if self._token is not None:
            return
        self._token = CancelToken()
        self._completion_message = complete
        self.status_label.setText(message)
        self._update_controls()
        disc = Path(self.snapshot.get("disc") or self.game_dir)
        worker = FunctionWorker(
            _profile_snapshot, self.root, self.profile_picker.currentText(), disc, mutate,
            token=self._token,
        )
        worker.signals.finished.connect(self._show_snapshot)
        worker.signals.failed.connect(self._show_error)
        start_worker(worker)

    def _load_selected_profile(self) -> None:
        self.snapshot = {}
        self.packages.clear()
        self.conflicts.clear()
        self.active_label.clear()
        self.active_label.setToolTip("")
        self._start_job("Reading profile")

    def _show_snapshot(self, _generation: int, snapshot: dict[str, Any]) -> None:
        selected = self._selected_package()
        selected_id = selected.text(1) if selected is not None else ""
        self.snapshot = snapshot
        self.packages.clear()
        for record in snapshot["packages"]:
            item = QTreeWidgetItem([
                record.get("name", record["id"]), record["id"], record["version"],
                "Enabled" if record["enabled"] else "Disabled",
            ])
            if record.get("error"):
                item.setToolTip(0, record["error"])
            self.packages.addTopLevelItem(item)
            if record["id"] == selected_id:
                self.packages.setCurrentItem(item)
        self.conflicts.clear()
        blocked = set(snapshot["blocked"])
        for conflict in snapshot["all_conflicts"]:
            resolution = "Unresolved" if conflict["key"] in blocked else "Unresolved (potential)"
            if conflict["accepted"]:
                resolution = f"Winner: {conflict['winner']}"
            item = QTreeWidgetItem([conflict["key"], ", ".join(conflict["packages"]), resolution])
            item.setData(0, Qt.ItemDataRole.UserRole, conflict["packages"])
            self.conflicts.addTopLevelItem(item)
        self.packages.resizeColumnToContents(0)
        self.conflicts.resizeColumnToContents(0)
        active = snapshot["active"]
        self.active_label.setText(
            f"Composed overlay: {active['path']}" if active["composed"] else "No composed overlay for this profile."
        )
        if snapshot["disc"]:
            self.active_label.setToolTip(f"Game folder: {snapshot['disc']}")
        self.status_label.setText(snapshot.get("conflict_error") or self._completion_message)
        self._token = None
        self._update_controls()

    def _show_error(self, _generation: int, error: str) -> None:
        self._token = None
        self.status_label.setText(f"Failed: {error.splitlines()[0]}")
        self._update_controls()

    def _choose_new_profile(self) -> None:
        profile, ok = QInputDialog.getText(self, "New Local Mod Profile", "Profile name")
        if not ok:
            return
        profile = profile.strip()
        if not mod_install.NAME_RE.fullmatch(profile):
            self.status_label.setText("Use 1 to 64 letters, numbers, dots, underscores, or hyphens. Start with a letter or number.")
            return
        if self.profile_picker.findText(profile) < 0:
            self.profile_picker.addItem(profile)
        self.profile_picker.setCurrentText(profile)

    def _profile_args(self) -> dict[str, Any]:
        return {
            "profile": self.profile_picker.currentText(),
            "root": self.root,
            "disc": self.snapshot.get("disc") or self.game_dir,
        }

    def _install(self) -> None:
        package, _ = QFileDialog.getOpenFileName(self, "Install EOD Mod Package", str(Path.home()), "EOD Mod (*.eodmod)")
        if package:
            self._start_job(
                "Installing package",
                mutate=partial(mod_install.install, package, enable=False, **self._profile_args()),
                complete=f"Installed {Path(package).name} as disabled.",
            )

    def _toggle_enabled(self) -> None:
        item = self._selected_package()
        if item is None:
            return
        package_id = item.text(1)
        enabled = item.text(3) == "Disabled"
        self._start_job(
            "Composing profile",
            mutate=partial(mod_install.set_enabled, package_id, enabled, **self._profile_args()),
            complete=f"{package_id} {'enabled' if enabled else 'disabled'}.",
        )

    def _uninstall(self) -> None:
        item = self._selected_package()
        if item is None:
            return
        package_id = item.text(1)
        confirmed = QMessageBox.question(
            self, "Uninstall Local Mod",
            f"Uninstall {package_id} from {self.profile_picker.currentText()}?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No, QMessageBox.StandardButton.No,
        )
        if confirmed == QMessageBox.StandardButton.Yes:
            self._start_job(
                "Uninstalling package", mutate=partial(mod_install.uninstall, package_id, **self._profile_args()),
                complete=f"Uninstalled {package_id}.",
            )

    def _choose_winner(self) -> None:
        item = self.conflicts.currentItem()
        if item is None:
            return
        asset = item.text(0)
        packages = item.data(0, Qt.ItemDataRole.UserRole)
        winner, ok = QInputDialog.getItem(self, "Choose Conflict Winner", asset, packages, editable=False)
        if ok:
            self._start_job(
                "Recording conflict winner",
                mutate=partial(mod_install.accept_conflict, asset, winner, **self._profile_args()),
                complete=f"{winner} selected for {asset}.",
            )

    def _choose_pie(self, mode: str) -> None:
        self.pie_request = (self.profile_picker.currentText(), mode)
        self.accept()
