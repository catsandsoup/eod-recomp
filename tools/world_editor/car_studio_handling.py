"""Qt handling sheet for the same retail physics.val backend used by the Mac studio."""

from __future__ import annotations

from hashlib import sha256
import math
import os
from pathlib import Path
import re
import struct

from PySide6.QtCore import QStandardPaths, Qt, QUrl, Signal, Slot
from PySide6.QtGui import QDesktopServices, QUndoCommand, QUndoStack
from PySide6.QtWidgets import (
    QComboBox, QFormLayout, QGroupBox, QHBoxLayout, QLabel, QLineEdit,
    QMessageBox, QPushButton, QScrollArea, QVBoxLayout, QWidget,
)

import modding_backend as backend
from workers import FunctionWorker, start_worker
from world_loader import discover_cars, editor_appdata_dir

_NUMBER = r"[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?"
_NUMERIC_VALUE = re.compile(rf"{_NUMBER}(?:(?:[ \t]*,[ \t]*|[ \t]+){_NUMBER})*")
_WHEEL_CLASS = re.compile(r"bus(?:rear)?|com|mid|mus")


def _number_shape(original: str) -> str:
    """Retain the native delimiters of vectors and calibration points."""
    separators = re.split(_NUMBER, original.strip())
    return _NUMBER.join(
        r"\s*,\s*" if "," in separator else r"\s+" if separator else ""
        for separator in separators
    )


def _typed_error(field: dict, value: str, gear_count: int, race_members: set[str]) -> str:
    original = field["original"]
    if value == original:
        return ""
    if field["unit"] == "class":
        return "" if _WHEEL_CLASS.fullmatch(value) else "Choose com, mid, mus, bus or busrear."
    if field["unit"] == "file":
        if re.fullmatch(r"[A-Za-z0-9_-]+\.val", value, re.IGNORECASE) and value.lower() in race_members:
            return ""
        return "Enter an existing .val name from RACE.RES."
    if field["section"] == "Transmission" and field["key"] == "num_gears":
        if not re.fullmatch(r"[0-9]+", value) or not 1 <= int(value) <= gear_count:
            return f"Enter a whole number between 1 and {gear_count}, the number of gear ratios in this car."
        return ""
    if not re.fullmatch(_number_shape(original), value):
        return "Use the numeric format shown in the original value."
    for token in re.findall(_NUMBER, value):
        try:
            stored = struct.unpack("<f", struct.pack("<f", float(token)))[0]
        except (OverflowError, struct.error):
            return "Enter a number the game can store."
        mantissa = token.lower().split("e", 1)[0]
        if not math.isfinite(stored) or (stored == 0 and re.search(r"[1-9]", mantissa)):
            return "Enter a number the game can store."
        if (field["section"], field["key"]) == ("Chassis", "mass") and stored <= 0:
            return "Enter a mass above zero."
        if field["min"] is not None and stored < field["min"]:
            return f"Enter a value of at least {field['min']}."
        if field["max"] is not None and stored > field["max"]:
            return f"Enter a value of at most {field['max']}."
    return ""


def _project_file(project_root: Path, disc: Path, archive: str) -> Path:
    install = sha256(os.fsencode(os.path.normcase(str(disc)))).hexdigest()[:16]
    return project_root / install / Path(archive).name / "handling.eodproject"


def _inspect(disc: Path, project_root: Path, archive: str, **_worker_context) -> tuple:
    project = _project_file(project_root, disc, archive)
    entries = backend.directory(backend.archive_path(disc, archive))
    asset = next((entry["name"] for entry in entries if entry["name"].lower() == "physics.val"), None)
    if asset is None:
        raise ValueError("This car does not contain physics.val.")
    request = dict(disc=str(disc), project=str(project), archive=archive, asset=asset)
    detail = backend.dispatch(dict(request, action="inspect"))
    if detail.get("kind") != "table":
        raise ValueError("This car's physics.val cannot be read as a handling sheet.")
    race_members = set()
    race = disc / "DATA/RES/RACE.RES"
    if race.is_file():
        try:
            race_members = {entry["name"].lower() for entry in backend.directory(backend.archive_path(
                disc, "DATA/RES/RACE.RES"))}
        except (ValueError, OSError):
            # The handling archive is independent of RACE.RES. Sound references
            # cannot be safely edited until their companion archive is readable.
            pass
    receipt = None
    warning = ""
    if project.is_file():
        try:
            checked = backend.dispatch(dict(request, action="validate"))
            output = backend.overlay_file(project, archive)
            receipt = dict(project=str(project), overlay=checked["overlay"], archivePath=str(output),
                           sha256=backend.archive_sha(output), bytes=output.stat().st_size)
        except (ValueError, OSError) as exc:
            warning = str(exc)
    return request, detail["sections"], race_members, receipt, warning


class _HandlingEdit(QUndoCommand):
    def __init__(self, sheet: CarHandlingSheet, before: dict, after: dict, session: int = -1):
        super().__init__()
        self.sheet = sheet
        self.before = before
        self.after = after
        self.session = session

    def id(self) -> int:
        return 1 if self.session >= 0 else -1

    def mergeWith(self, command: QUndoCommand) -> bool:
        if isinstance(command, _HandlingEdit) and self.session == command.session and self.after.keys() == command.after.keys():
            self.after = command.after
            return True
        return False

    def undo(self) -> None:
        self.sheet._set_values(self.before)

    def redo(self) -> None:
        self.sheet._set_values(self.after)


class CarHandlingSheet(QWidget):
    """Edit real physics fields and save a private overlay for the selected car."""

    archiveSaved = Signal(object)

    def __init__(self, game_dir: Path, parent: QWidget | None = None, *, project_root: Path | None = None):
        super().__init__(parent)
        self.disc = game_dir.resolve()
        if project_root is None:
            project_root = editor_appdata_dir() / "car-studio"
        self._project_root = project_root.resolve()
        self._request: dict | None = None
        self.saved_archive: dict | None = None
        self._busy = False
        self.saving = False
        self._rebuild_needed = False
        self._car_index = -1
        self._edit_session = 0
        self._sections: list[dict] = []
        self._fields: dict[str, dict] = {}
        self._values: dict[str, str] = {}
        self._saved_values: dict[str, str] = {}
        self._invalid: dict[str, str] = {}
        self._race_members: set[str] = set()
        self._gear_count = 0
        self.field_ids: dict[tuple[str, str], str] = {}
        self.editors: dict[str, QLineEdit] = {}
        self._notice = ""
        self._undo = QUndoStack(self)

        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        selection = QFormLayout()
        selection.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
        self.car_picker = QComboBox()
        self.car_picker.setAccessibleName("Retail car")
        for path in discover_cars(self.disc):
            self.car_picker.addItem(path.stem.replace("_1", "").title(), path.relative_to(self.disc).as_posix())
        digit = next((i for i in range(self.car_picker.count())
                      if Path(self.car_picker.itemData(i)).stem.upper() == "DIGIT"), 0)
        self.car_picker.setCurrentIndex(digit)
        selection.addRow("Retail car", self.car_picker)
        self.view_mode = QComboBox()
        self.view_mode.setAccessibleName("Handling view")
        self.view_mode.addItems(["Featured Six", "All Handling", "Read-only Fields"])
        selection.addRow("Show", self.view_mode)
        root.addLayout(selection)

        self._scroll = QScrollArea()
        self._scroll.setWidgetResizable(True)
        root.addWidget(self._scroll, 1)

        history = QHBoxLayout()
        output = QHBoxLayout()
        self.undo_button = QPushButton("Undo")
        self.redo_button = QPushButton("Redo")
        self.reset_button = QPushButton("Reset to Retail")
        self.save_button = QPushButton("Save && Build")
        self.files_button = QPushButton("Show Files")
        self.save_button.setToolTip("Build a separate .CAR in the Car Studio project.")
        for button in (self.undo_button, self.redo_button, self.reset_button):
            button.setAutoDefault(False)
            history.addWidget(button)
        for button in (self.save_button, self.files_button):
            button.setAutoDefault(False)
            output.addWidget(button)
        root.addLayout(history)
        root.addLayout(output)
        self.status = QLabel()
        self.status.setWordWrap(True)
        self.status.setTextFormat(Qt.TextFormat.PlainText)
        root.addWidget(self.status)

        self.car_picker.currentIndexChanged.connect(self._select_car)
        self.view_mode.currentIndexChanged.connect(self._render)
        self.undo_button.clicked.connect(self._undo.undo)
        self.redo_button.clicked.connect(self._undo.redo)
        self.reset_button.clicked.connect(self._reset)
        self.save_button.clicked.connect(self._save)
        self.files_button.clicked.connect(self._show_files)
        self._undo.canUndoChanged.connect(self._refresh_status)
        self._undo.canRedoChanged.connect(self._refresh_status)
        self._select_car(self.car_picker.currentIndex())

    @property
    def current_archive(self) -> str | None:
        return self.car_picker.currentData()

    @property
    def dirty(self) -> bool:
        return self._values != self._saved_values

    def confirm_discard(self) -> bool:
        if not self.dirty:
            return True
        return QMessageBox.warning(
            self, "Unsaved handling changes", "Discard the unsaved handling changes for this car?",
            QMessageBox.StandardButton.Discard | QMessageBox.StandardButton.Cancel,
            QMessageBox.StandardButton.Cancel,
        ) == QMessageBox.StandardButton.Discard

    @Slot(int)
    def _select_car(self, index: int) -> None:
        if not self.confirm_discard():
            self.car_picker.blockSignals(True)
            self.car_picker.setCurrentIndex(self._car_index)
            self.car_picker.blockSignals(False)
            return
        self._car_index = index
        self._request = self.saved_archive = None
        self._rebuild_needed = False
        self.status.setToolTip("")
        self._sections = []
        self._fields.clear()
        self.field_ids.clear()
        self._values.clear()
        self._saved_values.clear()
        self._invalid.clear()
        self._undo.clear()
        self._render()
        archive = self.current_archive
        if not archive:
            self._notice = "No retail cars found. Open an extracted game folder containing DATA/CAR."
            self._refresh_status()
            return
        self._busy = True
        self._notice = f"Reading {Path(archive).name}…"
        self._refresh_status()
        worker = FunctionWorker(_inspect, self.disc, self._project_root, archive)
        worker.signals.finished.connect(self._loaded)
        worker.signals.failed.connect(self._failed)
        start_worker(worker)

    @Slot(int, object)
    def _loaded(self, _generation: int, result: tuple) -> None:
        self._request, self._sections, self._race_members, self.saved_archive, warning = result
        self._rebuild_needed = bool(warning)
        self._gear_count = 0
        gears = {field["key"] for section in self._sections if section["name"] == "Transmission"
                 for field in section["fields"]}
        while f"g{self._gear_count + 1}" in gears:
            self._gear_count += 1
        for section in self._sections:
            for field in section["fields"]:
                field["section"] = section["name"]
                if (field["used"] is True and field["editable"]
                        and (field["unit"] == "class"
                             or field["unit"] == "file" and self._race_members
                             or _NUMERIC_VALUE.fullmatch(field["original"].strip()))):
                    self._values[field["id"]] = field["value"]
                self._fields[field["id"]] = field
                self.field_ids[section["name"], field["key"]] = field["id"]
        self._saved_values = self._values.copy()
        self._busy = False
        self._notice = warning or ("Saved handling loaded." if self.saved_archive else "")
        if self.saved_archive:
            self.status.setToolTip(self.saved_archive["archivePath"])
        self._render()
        self._refresh_status()

    @Slot()
    def _render(self) -> None:
        previous = self._scroll.takeWidget()
        if previous is not None:
            previous.deleteLater()
        self.editors.clear()
        body = QWidget()
        layout = QVBoxLayout(body)
        mode = self.view_mode.currentIndex()
        featured = sum(field["featured"] for field in self._fields.values())
        if mode == 0 and self._fields and featured < len(backend.FIELDS):
            note = QLabel(f"{featured} of the Featured Six are present in this car's physics.val.")
            note.setWordWrap(True)
            layout.addWidget(note)
        for section in self._sections:
            groups: dict[str, list[dict]] = {}
            for field in section["fields"]:
                if ((mode == 0 and not field["featured"])
                        or (mode == 1 and field["used"] is not True)
                        or (mode == 2 and field["id"] in self._values)):
                    continue
                groups.setdefault(field["group"], []).append(field)
            if not groups:
                continue
            box = QGroupBox(section["label"])
            box.setToolTip(section["explanation"])
            form = QFormLayout(box)
            form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
            form.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow)
            for group, fields in groups.items():
                heading = QLabel(group)
                font = heading.font()
                font.setBold(True)
                heading.setFont(font)
                form.addRow(heading)
                for field in fields:
                    form.addRow(*self._field_row(field))
            layout.addWidget(box)
        layout.addStretch(1)
        self._scroll.setWidget(body)

    def _field_row(self, field: dict) -> tuple[QLabel, QWidget]:
        field_id = field["id"]
        name = field["label"]
        explanation = field["explanation"]
        if field["key"].endswith(tuple("0123456789")):
            name = f"{name} ({field['key']})"
        label = QLabel(name)
        label.setTextFormat(Qt.TextFormat.PlainText)
        label.setWordWrap(True)
        label.setToolTip(f"{field['section']} / {field['key']}\n{explanation}")
        row = QWidget()
        layout = QVBoxLayout(row)
        layout.setContentsMargins(0, 0, 0, 2)
        layout.setSpacing(1)
        line = QLineEdit(self._values.get(field_id, field["original"]))
        line.setAccessibleName(f"{field['section']} / {name}")
        line.setToolTip(explanation)
        label.setBuddy(line)
        layout.addWidget(line)
        unit = f" {field['unit']}" if field["unit"] else ""
        if field_id in self._values:
            self.editors[field_id] = line
            line.setMaxLength(128)
            line.textEdited.connect(lambda value: self._edit(field_id, value))
            line.editingFinished.connect(self._end_edit)
            detail = f"Original {field['original']}{unit}"
        else:
            line.setReadOnly(True)
            reason = ("Unused by the game" if field["used"] is False else "Reader unverified"
                      if field["used"] is None else "Read-only native value")
            detail = f"{reason} · Original {field['original']}{unit}"
        caption = QLabel(detail)
        caption.setTextFormat(Qt.TextFormat.PlainText)
        caption.setWordWrap(True)
        layout.addWidget(caption)
        return label, row

    def _end_edit(self) -> None:
        self._edit_session += 1

    def _edit(self, field_id: str, value: str) -> None:
        if value != self._values[field_id]:
            self._undo.push(_HandlingEdit(self, {field_id: self._values[field_id]},
                                          {field_id: value}, self._edit_session))

    def _set_values(self, values: dict[str, str]) -> None:
        self._notice = ""
        for field_id, value in values.items():
            self._values[field_id] = value
            editor = self.editors.get(field_id)
            if editor is not None and editor.text() != value:
                editor.setText(value)
            problem = _typed_error(self._fields[field_id], value, self._gear_count, self._race_members)
            if problem:
                self._invalid[field_id] = problem
            else:
                self._invalid.pop(field_id, None)
        self._refresh_status()

    def _reset(self) -> None:
        before = {key: val for key, val in self._values.items() if val != self._fields[key]["original"]}
        if before:
            self._undo.push(_HandlingEdit(self, before, {key: self._fields[key]["original"] for key in before}))

    @Slot()
    def _refresh_status(self) -> None:
        ready = self._request is not None and not self._busy
        self.car_picker.setEnabled(not self._busy and self.car_picker.count() > 0)
        self.view_mode.setEnabled(ready)
        self._scroll.setEnabled(ready)
        self.undo_button.setEnabled(ready and self._undo.canUndo())
        self.redo_button.setEnabled(ready and self._undo.canRedo())
        self.reset_button.setEnabled(ready and any(value != self._fields[key]["original"]
                                                   for key, value in self._values.items()))
        self.save_button.setEnabled(ready and not self._invalid and (self.dirty or self._rebuild_needed))
        self.files_button.setEnabled(ready and self.saved_archive is not None)
        message = self._notice
        if self._invalid:
            field_id, error = next(iter(self._invalid.items()))
            field = self._fields[field_id]
            message = f"{field['section']} / {field['label']} — {error}"
        elif self.dirty and not message:
            message = "Unsaved handling changes."
        self.status.setText(message)

    def _save(self) -> None:
        if not self.save_button.isEnabled():
            return
        edits = [{key: self._fields[field_id][key] for key in ("section", "key", "sectionIndex", "pairIndex")}
                 | {"value": value} for field_id, value in self._values.items()
                 if value != self._fields[field_id]["original"]]
        self._busy = self.saving = True
        self._notice = "Building the isolated car archive…"
        self._refresh_status()
        request = dict(self._request, action="save", edits=edits)
        worker = FunctionWorker(lambda **context: backend.dispatch(request))
        worker.signals.finished.connect(self._saved)
        worker.signals.failed.connect(self._failed)
        start_worker(worker)

    @Slot(int, object)
    def _saved(self, _generation: int, result: dict) -> None:
        self._busy = self.saving = False
        self._rebuild_needed = False
        self.saved_archive = result
        self._saved_values = self._values.copy()
        self._notice = f"Saved {Path(result['archivePath']).name} to the Car Studio project."
        self.status.setToolTip(result["archivePath"])
        self._refresh_status()
        self.archiveSaved.emit(result)

    @Slot(int, str)
    def _failed(self, _generation: int, error: str) -> None:
        self._busy = self.saving = False
        self._notice = error.split("\nTraceback", 1)[0]
        self._refresh_status()

    def _show_files(self) -> None:
        if self.saved_archive:
            QDesktopServices.openUrl(QUrl.fromLocalFile(str(Path(self.saved_archive["archivePath"]).parent)))
