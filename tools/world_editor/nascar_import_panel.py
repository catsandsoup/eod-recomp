"""Zero-jargon NASCAR: Dirt to Daytona vehicle browser, ephemeral test-drive, and importer."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (
    QAbstractItemView,
    QButtonGroup,
    QCheckBox,
    QComboBox,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMenu,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

import nascar_firstparty
from nascar_scanner import (
    SERIES_ORDER,
    EphemeralTestDriveSession,
    NascarCatalogEntry,
    NascarScanResult,
    import_nascar_entries,
    scan_nascar_source,
)
from vehicle_overlay import RegisteredVehicle, local_vehicles
from workers import CancelToken, FunctionWorker, start_worker

_FILTER_SERIES: tuple[str, ...] = ("All", *SERIES_ORDER)


class NascarImportPanel(QWidget):
    """Visual browser for NASCAR: Dirt to Daytona ISOs/folders with Ephemeral Test-Drive."""

    testDriveRequested = Signal(object)

    def __init__(self, game_dir: Path, parent: QWidget | None = None):
        super().__init__(parent)
        self.game_dir = Path(game_dir)
        self.source: Path | None = None
        self.scan_result: NascarScanResult | None = None
        self.active_session: EphemeralTestDriveSession | None = None
        self.token: CancelToken | None = None
        self._active_series: str = "All"
        self.last_import_timings: dict[str, float] = {}

        root = QVBoxLayout(self)
        root.setSpacing(10)

        # --- Source picker bar ---
        source_row = QHBoxLayout()
        self.pick_source_button = QPushButton("Pick NASCAR ISO or Folder…")
        source_menu = QMenu(self.pick_source_button)
        self.act_choose_iso = source_menu.addAction("Choose NASCAR .iso Image…")
        self.act_choose_folder = source_menu.addAction("Choose Extracted NASCAR Folder…")
        self.act_choose_iso.triggered.connect(self._choose_iso)
        self.act_choose_folder.triggered.connect(self._choose_folder)
        self.pick_source_button.setMenu(source_menu)

        self.source_path_label = QLabel("No source selected")
        self.source_path_label.setWordWrap(True)
        source_row.addWidget(self.pick_source_button)
        source_row.addWidget(self.source_path_label, 1)
        root.addLayout(source_row)

        # --- Live scan summary ---
        self.lbl_summary = QLabel(
            "Pick a NASCAR: Dirt to Daytona ISO or extracted folder to browse all 5 series."
        )
        self.lbl_summary.setWordWrap(True)
        self.lbl_summary.setStyleSheet("font-weight: 600;")
        root.addWidget(self.lbl_summary)

        # --- Series filter pills + search box ---
        filter_row = QHBoxLayout()
        filter_row.setSpacing(6)
        self.series_group = QButtonGroup(self)
        self.series_group.setExclusive(True)
        self.series_buttons: dict[str, QPushButton] = {}
        for idx, series in enumerate(_FILTER_SERIES):
            btn = QPushButton(series)
            btn.setCheckable(True)
            btn.setChecked(series == "All")
            btn.clicked.connect(lambda _checked=False, s=series: self.set_series_filter(s))
            self.series_group.addButton(btn, idx)
            self.series_buttons[series] = btn
            filter_row.addWidget(btn)
        filter_row.addStretch(1)
        root.addLayout(filter_row)

        search_row = QHBoxLayout()
        self.search_input = QLineEdit()
        self.search_input.setPlaceholderText("Search by #number, driver, series, or chassis…")
        self.search_input.setClearButtonEnabled(True)
        self.search_input.textChanged.connect(lambda _text: self._populate_vehicle_list())
        self.lbl_match_count = QLabel("0 vehicles shown")
        search_row.addWidget(self.search_input, 1)
        search_row.addWidget(self.lbl_match_count)
        root.addLayout(search_row)

        # --- Multi-select vehicle browser ---
        self.vehicle_list = QListWidget()
        self.vehicle_list.setSelectionMode(QAbstractItemView.SelectionMode.ExtendedSelection)
        self.vehicle_list.setMinimumHeight(210)
        self.vehicle_list.itemSelectionChanged.connect(self._update_button_states)
        self.vehicle_list.itemDoubleClicked.connect(lambda _item: self._start_ephemeral_test_drive())
        root.addWidget(self.vehicle_list, 1)

        # --- Ephemeral Test-Drive controls ---
        td_row = QHBoxLayout()
        self.btn_test_drive = QPushButton("Test Drive (Ephemeral)")
        self.btn_test_drive.setToolTip(
            "Build only the selected vehicle into a temporary workspace and launch a test drive. "
            "Keep or discard afterwards with zero clutter."
        )
        self.btn_test_drive.clicked.connect(self._start_ephemeral_test_drive)

        self.btn_keep_test_drive = QPushButton("Keep Last Test-Drive")
        self.btn_keep_test_drive.setToolTip(
            "Save the already-compiled test-drive vehicle to your local library without re-transcoding."
        )
        self.btn_keep_test_drive.clicked.connect(self._keep_last_test_drive)

        self.btn_discard_test_drive = QPushButton("Discard Test-Drive")
        self.btn_discard_test_drive.setToolTip(
            "Delete the temporary test-drive workspace cleanly, leaving zero trace."
        )
        self.btn_discard_test_drive.clicked.connect(self._discard_test_drive)

        for btn in (self.btn_test_drive, self.btn_keep_test_drive, self.btn_discard_test_drive):
            td_row.addWidget(btn)
        root.addLayout(td_row)

        # --- First-party additive lifecycle options (all default ON) ---
        opts_row = QHBoxLayout()
        opts_row.setSpacing(10)
        self.chk_firstparty_icons = QCheckBox("Generate 3D HUD & Showroom Icons")
        self.chk_firstparty_icons.setChecked(True)
        self.chk_firstparty_icons.setToolTip(
            "Render 256x256 alpha-masked 3D showroom and HUD mini-icons for every imported car."
        )
        self.chk_sweeneys_skins = QCheckBox("Add 5 Unpainted Sweeney's Dealership Skins")
        self.chk_sweeneys_skins.setChecked(True)
        self.chk_sweeneys_skins.setToolTip(
            "Generate 5 clean unpainted primer colors (Gray, White, Red, Blue, Black) for Career dealership."
        )
        self.chk_paintkit_support = QCheckBox("Enable Full-Panel Paint Shop Support")
        self.chk_paintkit_support.setChecked(True)
        self.chk_paintkit_support.setToolTip(
            "Map all 7 body panels (hood, roof, doors, fenders, decklid) and canvas for in-game Paint Shop."
        )
        for chk in (self.chk_firstparty_icons, self.chk_sweeneys_skins, self.chk_paintkit_support):
            opts_row.addWidget(chk)
        opts_row.addStretch(1)
        root.addLayout(opts_row)

        # --- Batch / Permanent import controls ---
        import_row = QHBoxLayout()
        self.btn_import_selected = QPushButton("Import Selected")
        self.btn_import_selected.clicked.connect(self._import_selected)

        self.btn_import_series = QPushButton("Import Series")
        self.btn_import_series.clicked.connect(self._import_series)

        self.btn_import_all = QPushButton("Import All")
        self.btn_import_all.clicked.connect(self._import_all)

        self.cancel_button = QPushButton("Cancel")
        self.cancel_button.setEnabled(False)
        self.cancel_button.clicked.connect(self.cancel)

        for btn in (
            self.btn_import_selected,
            self.btn_import_series,
            self.btn_import_all,
            self.cancel_button,
        ):
            import_row.addWidget(btn)
        root.addLayout(import_row)

        # --- Saved local vehicles quick-launch ---
        saved_row = QHBoxLayout()
        saved_row.addWidget(QLabel("Saved Local Vehicles:"))
        self.imported = QComboBox()
        self.imported.setAccessibleName("Registered local cars")
        self.imported.currentIndexChanged.connect(lambda _idx: self._update_button_states())
        self.drive_saved_button = QPushButton("Test Drive Saved")
        self.drive_saved_button.clicked.connect(self._test_drive_saved)
        saved_row.addWidget(self.imported, 1)
        saved_row.addWidget(self.drive_saved_button)
        root.addLayout(saved_row)

        # --- Status line & Tracy telemetry summary ---
        self.status = QLabel("Ready. Ephemeral test drives leave zero permanent files until you click Keep.")
        self.status.setWordWrap(True)
        root.addWidget(self.status)

        self.lbl_profile_summary = QLabel("Tracy profile: idle")
        self.lbl_profile_summary.setWordWrap(True)
        self.lbl_profile_summary.setStyleSheet("color: #94a3b8; font-family: monospace; font-size: 11px;")
        root.addWidget(self.lbl_profile_summary)

        self._refresh_imported()
        self._update_button_states()

    # ------------------------------------------------------------------
    # Public browser helpers
    # ------------------------------------------------------------------
    @property
    def firstparty_options(self) -> dict[str, bool]:
        return {
            "enable_icons": self.chk_firstparty_icons.isChecked(),
            "enable_sweeneys": self.chk_sweeneys_skins.isChecked(),
            "enable_paintkit": self.chk_paintkit_support.isChecked(),
        }
    @property
    def active_series(self) -> str:
        return self._active_series

    def set_series_filter(self, series: str) -> None:
        if series not in self.series_buttons:
            series = "All"
        self._active_series = series
        btn = self.series_buttons[series]
        if not btn.isChecked():
            btn.setChecked(True)
        self._populate_vehicle_list()

    def load_source(self, path: Path | str, *, synchronous: bool = False) -> NascarScanResult | None:
        """Scan an ISO or extracted NASCAR folder and populate the browser."""
        target = Path(path)
        self.source = target
        self.source_path_label.setText(target.name)
        self.source_path_label.setToolTip(str(target))
        if synchronous:
            scan_ms: dict[str, float] = {}
            with nascar_firstparty.span("scan", timings=scan_ms):
                result = scan_nascar_source(target)
            self.last_import_timings = dict(scan_ms)
            self.lbl_profile_summary.setText(nascar_firstparty.format_profile_summary(scan_ms))
            self._apply_scan_result(result)
            return result
        if self.token is not None:
            return None
        self.token = CancelToken()
        self._update_button_states()
        self.status.setText(f"Scanning {target.name}…")
        worker = FunctionWorker(
            self._scan_worker_fn,
            target,
            token=self.token,
            label="Scanning NASCAR source",
        )
        worker.signals.progress.connect(self._on_progress)
        worker.signals.finished.connect(self._on_scan_finished)
        worker.signals.failed.connect(self._on_worker_failed)
        worker.signals.cancelled.connect(self._on_worker_cancelled)
        start_worker(worker)
        return None

    def selected_entries(self) -> list[NascarCatalogEntry]:
        entries: list[NascarCatalogEntry] = []
        for item in self.vehicle_list.selectedItems():
            entry = item.data(Qt.ItemDataRole.UserRole)
            if isinstance(entry, NascarCatalogEntry):
                entries.append(entry)
        return entries

    def visible_entries(self) -> list[NascarCatalogEntry]:
        entries: list[NascarCatalogEntry] = []
        for row in range(self.vehicle_list.count()):
            entry = self.vehicle_list.item(row).data(Qt.ItemDataRole.UserRole)
            if isinstance(entry, NascarCatalogEntry):
                entries.append(entry)
        return entries

    def cancel(self) -> None:
        if self.token is not None:
            self.token.cancel()
            self.cancel_button.setEnabled(False)
            self.status.setText("Cancelling operation…")

    # ------------------------------------------------------------------
    # Source selection & scanning
    # ------------------------------------------------------------------
    def _choose_iso(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "Pick NASCAR: Dirt to Daytona ISO", "", "Disc image (*.iso)"
        )
        if path:
            self.load_source(Path(path))

    def _choose_folder(self) -> None:
        path = QFileDialog.getExistingDirectory(
            self, "Pick Extracted NASCAR Disc or DATA Folder"
        )
        if path:
            self.load_source(Path(path))

    @staticmethod
    def _scan_worker_fn(path: Path, *, token: CancelToken, progress) -> NascarScanResult:
        progress(15, f"Reading NASCAR catalog from {path.name}")
        with nascar_firstparty.span("scan"):
            result = scan_nascar_source(path, cancelled=lambda: token.is_cancelled)
        progress(100, result.summary_text)
        return result

    def _on_scan_finished(self, _generation: int, result: NascarScanResult) -> None:
        self.token = None
        self._apply_scan_result(result)

    def _apply_scan_result(self, result: NascarScanResult) -> None:
        self.scan_result = result
        self.source = result.source_path
        self.source_path_label.setText(result.source_path.name)
        self.source_path_label.setToolTip(str(result.source_path))
        self.lbl_summary.setText(result.summary_text)
        self._populate_vehicle_list()
        self.status.setText(result.summary_text)
        self._update_button_states()

    def _populate_vehicle_list(self) -> None:
        self.vehicle_list.clear()
        if self.scan_result is None:
            self.lbl_match_count.setText("0 vehicles shown")
            self._update_button_states()
            return
        query = self.search_input.text().strip().lower()
        series_filter = self._active_series
        shown = 0
        for entry in self.scan_result.entries:
            if series_filter != "All" and entry.series != series_filter:
                continue
            label = entry.display_label
            if query and query not in label.lower() and query not in entry.style.lower():
                continue
            item = QListWidgetItem(label)
            item.setData(Qt.ItemDataRole.UserRole, entry)
            item.setToolTip(
                f"{entry.driver} (#{entry.car_number}) · {entry.series} · {entry.chassis} ({entry.style.title()})"
            )
            self.vehicle_list.addItem(item)
            shown += 1
        noun = "vehicle" if shown == 1 else "vehicles"
        self.lbl_match_count.setText(f"{shown} {noun} shown")
        self._update_button_states()

    # ------------------------------------------------------------------
    # Ephemeral Test-Drive -> Keep / Discard lifecycle
    # ------------------------------------------------------------------
    def _start_ephemeral_test_drive(self) -> None:
        if self.source is None or self.token is not None:
            return
        selected = self.selected_entries()
        if not selected:
            self.status.setText("Select a NASCAR vehicle from the list to test drive.")
            return
        entry = selected[0]
        if self.active_session is not None:
            self.active_session.discard()
            self.active_session = None

        self.token = CancelToken()
        self._update_button_states()
        self.status.setText(f"Preparing ephemeral test drive for {entry.display_label}…")
        worker = FunctionWorker(
            self._ephemeral_build_worker,
            self.source,
            self.game_dir,
            entry,
            token=self.token,
            label=f"Test Drive: {entry.driver}",
        )
        worker.signals.progress.connect(self._on_progress)
        worker.signals.finished.connect(self._on_ephemeral_ready)
        worker.signals.failed.connect(self._on_worker_failed)
        worker.signals.cancelled.connect(self._on_worker_cancelled)
        start_worker(worker)

    @staticmethod
    def _ephemeral_build_worker(
        source: Path, game_dir: Path, entry: NascarCatalogEntry, *, token: CancelToken, progress
    ) -> EphemeralTestDriveSession:
        session = EphemeralTestDriveSession(source, game_dir, entry)
        try:
            session.build(cancelled=lambda: token.is_cancelled, progress=progress)
            return session
        except BaseException:
            session.discard()
            raise

    def _on_ephemeral_ready(self, _generation: int, session: EphemeralTestDriveSession) -> None:
        self.token = None
        if self.active_session is not None and self.active_session is not session:
            self.active_session.discard()
        self.active_session = session
        self._update_button_states()
        if session.vehicle is not None:
            self.status.setText(
                f"Ephemeral test drive ready for {session.entry.display_label}. "
                "Click Keep Last Test-Drive to save or Discard Test-Drive to remove."
            )
            self.testDriveRequested.emit(session.vehicle)

    def _keep_last_test_drive(self) -> None:
        if self.active_session is None or self.token is not None:
            return
        try:
            saved = self.active_session.keep_to_local_vehicles()
            label = self.active_session.entry.display_label
            self.active_session.discard()
            self.active_session = None
        except Exception as exc:
            self.status.setText(f"Could not keep test-drive vehicle: {exc}")
            self._update_button_states()
            return
        self._refresh_imported(selected=saved)
        self.status.setText(f"Saved {label} to local vehicles without re-transcoding.")
        self._update_button_states()

    def _discard_test_drive(self) -> None:
        if self.active_session is None or self.token is not None:
            return
        label = self.active_session.entry.display_label
        self.active_session.discard()
        self.active_session = None
        self.status.setText(f"Discarded ephemeral test drive for {label} (zero trace).")
        self._update_button_states()

    # ------------------------------------------------------------------
    # Permanent imports: Selected / Series / All
    # ------------------------------------------------------------------
    def _import_selected(self) -> None:
        entries = self.selected_entries()
        if not entries:
            self.status.setText("Select one or more NASCAR vehicles to import.")
            return
        self._start_batch_import(entries, f"Importing {len(entries)} selected vehicle(s)")

    def _import_series(self) -> None:
        if self.scan_result is None:
            return
        target_series = self._active_series
        if target_series == "All":
            selected = self.selected_entries()
            if selected:
                target_series = selected[0].series
            else:
                self.status.setText("Pick a series filter pill (or select a vehicle) to import a series.")
                return
        entries = [e for e in self.scan_result.entries if e.series == target_series]
        if not entries:
            self.status.setText(f"No vehicles available in {target_series}.")
            return
        self._start_batch_import(entries, f"Importing {target_series} series ({len(entries)} vehicles)")

    def _import_all(self) -> None:
        if self.scan_result is None or not self.scan_result.entries:
            return
        entries = list(self.scan_result.entries)
        self._start_batch_import(entries, f"Importing all {len(entries)} NASCAR vehicles")

    def _start_batch_import(self, entries: list[NascarCatalogEntry], label: str) -> None:
        if self.source is None or self.token is not None or not entries:
            return
        self.token = CancelToken()
        self._update_button_states()
        self.status.setText(f"{label}…")
        opts = self.firstparty_options
        worker = FunctionWorker(
            self._batch_import_worker,
            self.source,
            self.game_dir,
            entries,
            opts,
            token=self.token,
            label=label,
        )
        worker.signals.progress.connect(self._on_progress)
        worker.signals.finished.connect(self._on_import_finished)
        worker.signals.failed.connect(self._on_worker_failed)
        worker.signals.cancelled.connect(self._on_worker_cancelled)
        start_worker(worker)

    @staticmethod
    def _batch_import_worker(
        source: Path,
        game_dir: Path,
        entries: list[NascarCatalogEntry],
        firstparty_opts: dict[str, bool],
        *,
        token: CancelToken,
        progress,
    ) -> tuple[list[RegisteredVehicle], dict[str, float], str]:
        vehicles = import_nascar_entries(
            source,
            game_dir,
            entries,
            cancelled=lambda: token.is_cancelled,
            progress=progress,
        )
        agg_timings: dict[str, float] = {}
        any_fp = any(firstparty_opts.values())
        eod_data = game_dir / "DATA"
        if any_fp and (eod_data / "RES" / "UIBIG.RES").is_file():
            for veh, entry in zip(vehicles, entries):
                if token.is_cancelled:
                    raise InterruptedError("Vehicle import cancelled")
                res = nascar_firstparty.apply_firstparty_to_overlay(
                    eod_data=eod_data,
                    overlay_dir=veh.layer,
                    chassis=veh.chassis,
                    livery_id=veh.number,
                    driver_name=entry.driver,
                    display_number=entry.car_number,
                    **firstparty_opts,
                )
                for k, v in res.get("timings_ms", {}).items():
                    agg_timings[k] = agg_timings.get(k, 0.0) + float(v)
        summary = nascar_firstparty.format_profile_summary(agg_timings) if agg_timings else "Tracy: import complete"
        return vehicles, agg_timings, summary

    def _on_import_finished(self, _generation: int, payload: object) -> None:
        self.token = None
        if isinstance(payload, tuple) and len(payload) == 3:
            vehicles, timings_ms, summary = payload
        else:
            vehicles, timings_ms, summary = payload, {}, ""
        selected = vehicles[-1] if vehicles else None
        self._refresh_imported(selected=selected)
        self.last_import_timings = dict(timings_ms)
        if summary:
            self.lbl_profile_summary.setText(summary)
        total_ms = timings_ms.get("total_ms", 0.0)
        suffix = f" in {total_ms:.0f} ms" if total_ms > 0.0 else ""
        self.status.setText(f"Imported {len(vehicles)} NASCAR vehicle(s) to local library{suffix}.")
        self._update_button_states()

    # ------------------------------------------------------------------
    # Saved local vehicles & worker callbacks
    # ------------------------------------------------------------------
    def _refresh_imported(self, selected: RegisteredVehicle | None = None) -> None:
        self.imported.clear()
        self.imported.addItem("Select a saved local vehicle")
        for vehicle in local_vehicles(self.game_dir):
            self.imported.addItem(f"{vehicle.chassis} #{vehicle.number}", vehicle)
            if vehicle == selected:
                self.imported.setCurrentIndex(self.imported.count() - 1)

    def _test_drive_saved(self) -> None:
        vehicle = self.imported.currentData()
        if self.token is None and vehicle is not None:
            self.testDriveRequested.emit(vehicle)

    def _update_button_states(self) -> None:
        busy = self.token is not None
        has_scan = self.scan_result is not None and bool(self.scan_result.entries)
        has_selection = bool(self.selected_entries())
        has_session = (
            self.active_session is not None
            and not self.active_session.is_discarded
            and self.active_session.vehicle is not None
        )
        has_series_target = has_scan and (self._active_series != "All" or has_selection)

        self.pick_source_button.setEnabled(not busy)
        for btn in self.series_buttons.values():
            btn.setEnabled(not busy)
        self.search_input.setEnabled(not busy)
        self.vehicle_list.setEnabled(not busy)
        self.imported.setEnabled(not busy)

        self.btn_test_drive.setEnabled(not busy and has_scan and has_selection)
        self.btn_keep_test_drive.setEnabled(not busy and has_session)
        self.btn_discard_test_drive.setEnabled(not busy and has_session)
        self.btn_import_selected.setEnabled(not busy and has_scan and has_selection)
        self.btn_import_series.setEnabled(not busy and has_series_target)
        self.btn_import_all.setEnabled(not busy and has_scan)
        self.chk_firstparty_icons.setEnabled(not busy)
        self.chk_sweeneys_skins.setEnabled(not busy)
        self.chk_paintkit_support.setEnabled(not busy)
        self.cancel_button.setEnabled(busy)
        self.drive_saved_button.setEnabled(not busy and self.imported.currentData() is not None)

    def _on_progress(self, _generation: int, percent: int, message: str) -> None:
        self.status.setText(f"{percent}% · {message}")

    def _on_worker_failed(self, _generation: int, error: str) -> None:
        self.token = None
        self.status.setText(error.splitlines()[0] if error else "Operation failed.")
        self._update_button_states()

    def _on_worker_cancelled(self, _generation: int) -> None:
        self.token = None
        self.status.setText("Operation cancelled.")
        self._update_button_states()
