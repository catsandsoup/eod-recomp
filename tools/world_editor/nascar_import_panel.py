"""Local Dirt to Daytona stock-car import and test-drive entry point."""

from pathlib import Path

from PySide6.QtCore import Signal
from PySide6.QtWidgets import (
    QComboBox, QFileDialog, QFormLayout, QHBoxLayout, QLabel, QLineEdit,
    QPushButton, QSpinBox, QVBoxLayout, QWidget,
)

from vehicle_overlay import RegisteredVehicle, import_local_stock, local_vehicles
from workers import CancelToken, FunctionWorker, start_worker
from world_loader import discover_cars


class NascarImportPanel(QWidget):
    testDriveRequested = Signal(object)

    def __init__(self, game_dir: Path, parent: QWidget | None = None):
        super().__init__(parent)
        self.game_dir = game_dir
        self.source: Path | None = None
        self.token: CancelToken | None = None

        layout = QVBoxLayout(self)
        form = QFormLayout()
        self.source_label = QLabel("Select your Dirt to Daytona ISO or extracted folder")
        self.source_label.setWordWrap(True)
        self.iso_button = QPushButton("Choose ISO…")
        self.folder_button = QPushButton("Choose Folder…")
        self.iso_button.clicked.connect(self._choose_iso)
        self.folder_button.clicked.connect(self._choose_folder)
        source_actions = QHBoxLayout()
        source_actions.addWidget(self.iso_button)
        source_actions.addWidget(self.folder_button)
        form.addRow(self.source_label)
        form.addRow("Source", source_actions)
        self.chassis = QLineEdit("Cupcar")
        self.chassis.setMaxLength(8)
        form.addRow("Chassis", self.chassis)
        self.number = QSpinBox()
        self.number.setRange(0, 2147483645)
        self.number.setValue(777)
        form.addRow("Vehicle number", self.number)
        self.paint = QLineEdit("Car24.tex")
        form.addRow("Livery entry", self.paint)
        self.donor = QComboBox()
        self.donor.addItems([path.stem.title() for path in discover_cars(game_dir) if len(path.stem) <= 8])
        self.donor.setToolTip("Use this retail car as the EOD archive template for the new NASCAR chassis.")
        form.addRow("EOD template", self.donor)
        self.car_class = QComboBox()
        self.car_class.addItems(("Compact", "Midsize", "Muscle"))
        self.car_class.setCurrentIndex(1)
        form.addRow("EOD class", self.car_class)
        layout.addLayout(form)

        self.imported = QComboBox()
        self.imported.setAccessibleName("Registered local cars")
        layout.addWidget(self.imported)
        actions = QHBoxLayout()
        self.import_button = QPushButton("Import Stock Car")
        self.import_button.setEnabled(False)
        self.import_button.clicked.connect(self._start)
        self.cancel_button = QPushButton("Cancel Import")
        self.cancel_button.setEnabled(False)
        self.cancel_button.clicked.connect(self.cancel)
        self.drive_button = QPushButton("Test Drive in PIE")
        self.drive_button.setEnabled(False)
        self.drive_button.clicked.connect(self._test_drive)
        self.imported.currentIndexChanged.connect(self._selection_changed)
        for button in (self.import_button, self.cancel_button, self.drive_button):
            actions.addWidget(button)
        layout.addLayout(actions)
        self.status = QLabel("Imports create a local vehicle layer for a test drive.")
        self.status.setWordWrap(True)
        layout.addWidget(self.status)
        layout.addStretch(1)
        self._refresh_imported()

    def _refresh_imported(self, selected: RegisteredVehicle | None = None) -> None:
        self.imported.clear()
        self.imported.addItem("Select a registered local car")
        for vehicle in local_vehicles(self.game_dir):
            self.imported.addItem(f"{vehicle.chassis} #{vehicle.number}", vehicle)
            if vehicle == selected:
                self.imported.setCurrentIndex(self.imported.count() - 1)

    def _selection_changed(self, _index: int) -> None:
        self.drive_button.setEnabled(self.token is None and self.imported.currentData() is not None)

    def _choose_iso(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Choose Dirt to Daytona ISO", "", "Disc image (*.iso)")
        self._set_source(path)

    def _choose_folder(self) -> None:
        self._set_source(QFileDialog.getExistingDirectory(self, "Choose Extracted NASCAR Disc or DATA Folder"))

    def _set_source(self, path: str) -> None:
        if path:
            self.source = Path(path)
            self.source_label.setText(self.source.name)
            self.source_label.setToolTip(str(self.source))
            self.import_button.setEnabled(self.donor.count() > 0)

    def _start(self) -> None:
        if self.source is None or self.token is not None:
            return
        paint = self.paint.text().strip()
        if paint and (Path(paint).name != paint or "/" in paint or "\\" in paint):
            self.status.setText("Enter a paint archive entry such as Car24.tex.")
            return
        self.token = CancelToken()
        self._set_busy(True)
        worker = FunctionWorker(
            import_local_stock, self.source, self.game_dir, self.chassis.text().strip(),
            self.number.value(), self.donor.currentText(), self.car_class.currentIndex(), paint,
            token=self.token, label="Importing stock car",
        )
        worker.signals.progress.connect(self._progress)
        worker.signals.finished.connect(self._finished)
        worker.signals.failed.connect(self._failed)
        worker.signals.cancelled.connect(self._cancelled)
        start_worker(worker)

    def _set_busy(self, busy: bool) -> None:
        for control in (self.iso_button, self.folder_button, self.chassis, self.number, self.paint,
                        self.donor, self.car_class, self.imported):
            control.setEnabled(not busy)
        self.import_button.setEnabled(not busy and self.source is not None and self.donor.count() > 0)
        self.cancel_button.setEnabled(busy)
        self._selection_changed(self.imported.currentIndex())

    def cancel(self) -> None:
        if self.token is not None:
            self.token.cancel()
            self.cancel_button.setEnabled(False)
            self.status.setText("Cancelling import…")

    def _progress(self, _generation: int, percent: int, message: str) -> None:
        self.status.setText(f"{percent}% · {message}")

    def _finished(self, _generation: int, vehicle: RegisteredVehicle) -> None:
        self.token = None
        self._set_busy(False)
        self._refresh_imported(selected=vehicle)
        self.status.setText(f"{vehicle.chassis} #{vehicle.number} is registered for a local test drive.")

    def _failed(self, _generation: int, error: str) -> None:
        self.token = None
        self._set_busy(False)
        self.status.setText(error.splitlines()[0])

    def _cancelled(self, _generation: int) -> None:
        self.token = None
        self._set_busy(False)
        self.status.setText("Import cancelled.")

    def _test_drive(self) -> None:
        vehicle = self.imported.currentData()
        if self.token is None and vehicle is not None:
            self.testDriveRequested.emit(vehicle)
