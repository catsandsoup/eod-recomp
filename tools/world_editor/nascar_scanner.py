"""Zero-jargon NASCAR: Dirt to Daytona ISO/folder auto-scanner & ephemeral test-drive lifecycle.

Recognizes all 5 NASCAR series (Cup, Truck, Modified, Hobby, Pace Car) directly
from a Dirt to Daytona .iso or extracted disc folder without requiring any EOD
template or class selection, and builds isolated ephemeral test-drive overlays
that can be kept without re-transcoding or discarded with zero trace.
"""

from __future__ import annotations

import contextlib
import copy
from dataclasses import dataclass, field
import io
import json
from pathlib import Path
import re
import shutil
import tempfile
from typing import Any, Callable, Iterable, Sequence

from create_custom_car import car_archive_filename
from home_hub import editor_appdata_dir
from import_nascar_cars import convert_tex_v9_to_v10, drop_top_mip, find_entry
from iso9660_reader import FAMILY_ARCHIVES, NascarSource, NascarSourceCancelled
from mod_package import slus_sha
import nascar_roster
from nascar_wheels import build_source_wheels
from rst0 import pack_archive, unpack_archive
from valt import parse_binary_valt
from vehicle_overlay import RegisteredVehicle, registered_vehicle

SERIES_ORDER: tuple[str, ...] = ("Cup", "Truck", "Modified", "Hobby", "Pace Car")

_ROSTER_TO_META: dict[str, tuple[str, str, str]] = {
    # roster_family -> (series_label, normalized_family, car_archive)
    "stock": ("Cup", "cup", "STOCK.CAR"),
    "truck": ("Truck", "truck", "TRUCK.CAR"),
    "modified": ("Modified", "modified", "MODIFIED.CAR"),
    "hobby": ("Hobby", "dirt", "HOBBY.CAR"),
    "pacecar": ("Pace Car", "pace", "PACECAR.CAR"),
}
_FAMILY_TO_ROSTER: dict[str, str] = {
    meta[1]: roster_key for roster_key, meta in _ROSTER_TO_META.items()
}
_SERIES_RANK: dict[str, int] = {name: idx for idx, name in enumerate(SERIES_ORDER)}


def _unpack_quiet(archive_path: Path, out_dir: Path) -> None:
    """Unpack an 0TSR archive without polluting stdout."""
    with contextlib.redirect_stdout(io.StringIO()):
        unpack_archive(str(archive_path), str(out_dir))


def _pack_quiet(in_dir: Path, out_archive: Path) -> None:
    """Pack an 0TSR v6 archive without polluting stdout."""
    out_archive.parent.mkdir(parents=True, exist_ok=True)
    with contextlib.redirect_stdout(io.StringIO()):
        pack_archive(str(in_dir), str(out_archive))


@dataclass
class NascarCatalogEntry:
    """One recognized NASCAR vehicle ready for browsing, test-drive, or import."""

    series: str
    family: str
    style: str
    car_number: str
    driver: str
    livery_name: str
    car_archive: str
    raw_record: dict[str, Any] = field(default_factory=dict)

    @property
    def roster_family(self) -> str:
        raw = self.raw_record.get("family")
        if isinstance(raw, str) and raw in _ROSTER_TO_META:
            return raw
        return _FAMILY_TO_ROSTER[self.family]

    @property
    def chassis(self) -> str:
        raw_chassis = self.raw_record.get("chassis")
        if isinstance(raw_chassis, str) and raw_chassis:
            return raw_chassis
        rf = self.roster_family
        choices = nascar_roster.CHASSIS[rf]
        return choices.get(self.style.lower(), choices.get("all", next(iter(choices.values()))))

    @property
    def display_label(self) -> str:
        return f"#{self.car_number} · {self.driver} · {self.series} · {self.chassis}"


@dataclass
class NascarScanResult:
    """Structured scan output for a NASCAR: Dirt to Daytona ISO or extracted folder."""

    source_path: Path
    entries: list[NascarCatalogEntry]
    series_counts: dict[str, int]
    summary_text: str


def _format_summary(entries: Sequence[NascarCatalogEntry], series_counts: dict[str, int]) -> str:
    total = len(entries)
    active = [(series, series_counts[series]) for series in SERIES_ORDER if series_counts.get(series, 0) > 0]
    breakdown = ", ".join(f"{count} {series}" for series, count in active)
    vehicle_word = "vehicle" if total == 1 else "vehicles"
    if breakdown:
        return f"Recognized {total} NASCAR {vehicle_word} across {len(active)} series ({breakdown})"
    return f"Recognized {total} NASCAR {vehicle_word}"


def _sort_key(entry: NascarCatalogEntry) -> tuple[int, int, str, str]:
    try:
        num = int(entry.car_number)
    except ValueError:
        num = 999999
    return (_SERIES_RANK.get(entry.series, 99), num, entry.car_number, entry.driver.lower())


def _record_to_entry(record: dict[str, Any]) -> NascarCatalogEntry:
    roster_family = str(record["family"])
    series, family, car_archive = _ROSTER_TO_META[roster_family]
    number = str(record["source_number"])
    style = str(record.get("body_style") or "all").lower()
    driver = str(record.get("name") or f"{series} #{number}").strip()
    livery = str(record.get("paint") or f"Car{number}.tex")
    raw = copy.deepcopy(record)
    pinned_key = f"{roster_family}:{number}"
    if pinned_key in nascar_roster.LIVERY_IDS:
        raw.setdefault("pinned_livery_id", int(nascar_roster.LIVERY_IDS[pinned_key]))
    return NascarCatalogEntry(
        series=series,
        family=family,
        style=style,
        car_number=number,
        driver=driver,
        livery_name=livery,
        car_archive=car_archive,
        raw_record=raw,
    )


def _collect_partial_roster(sources: dict[str, Path], available_families: Iterable[str]) -> list[dict[str, Any]]:
    """Collect records only for families actually present in a partial extracted directory."""
    records: list[dict[str, Any]] = []
    avail = set(available_families)
    for roster_family, cfg in nascar_roster.FAMILIES.items():
        if roster_family not in avail:
            continue
        if roster_family == "pacecar":
            records.append(
                dict(
                    family="pacecar",
                    source_number="0",
                    name="NASCAR Pace Car",
                    body_style="all",
                    paint="Pacecar.tex",
                    driver=None,
                    chassis="NPace",
                )
            )
            continue
        paint_dir = sources.get(cfg["paint"])
        common_dir = sources.get("COMMON")
        seen: set[str] = set()
        if common_dir is not None and paint_dir is not None:
            try:
                roster_file = find_entry(common_dir, cfg["roster"])
            except FileNotFoundError:
                roster_file = None
            if roster_file is not None:
                drivers = [
                    s
                    for s in parse_binary_valt(roster_file.read_bytes())["sections"]
                    if s["name"] == "Driver"
                ]
                for driver in drivers:
                    d = dict(driver["pairs"])
                    if d.get("custom") == "yes":
                        continue
                    number = d["number"]
                    try:
                        paint = find_entry(paint_dir, f"Car{number}.tex").name
                    except FileNotFoundError:
                        continue
                    seen.add(number)
                    style = d.get("body_style", "chevy").lower()
                    chassis = nascar_roster.CHASSIS[roster_family][
                        style if roster_family == "stock" else "all"
                    ]
                    records.append(
                        dict(
                            family=roster_family,
                            source_number=number,
                            name=d["name"],
                            body_style=style,
                            paint=paint,
                            driver=d,
                            chassis=chassis,
                        )
                    )
        if paint_dir is not None:
            for p in sorted(paint_dir.iterdir()):
                match = re.fullmatch(r"car(\d+)\.tex", p.name, re.I)
                if match and match[1] not in seen:
                    choices = nascar_roster.CHASSIS[roster_family]
                    records.append(
                        dict(
                            family=roster_family,
                            source_number=match[1],
                            name=f"{roster_family.title()} source alternate {match[1]}",
                            body_style="chevy" if roster_family == "stock" else "all",
                            paint=p.name,
                            driver=None,
                            chassis=choices.get("chevy", choices.get("all")),
                        )
                    )
    return records


def scan_nascar_source(
    iso_or_dir: Path | str,
    *,
    cancelled: Callable[[], bool] | None = None,
) -> NascarScanResult:
    """Scan a NASCAR: Dirt to Daytona ISO or extracted folder across all 5 series."""
    source_path = Path(iso_or_dir).resolve()
    disc = NascarSource(source_path)

    stage_keys: list[str] = []
    if "RES/COMMON.RES" in disc.archives:
        stage_keys.append("RES/COMMON.RES")
    for roster_family in disc.available_families:
        cfg = nascar_roster.FAMILIES[roster_family]
        if cfg["paint"]:
            res_key = f"RES/{cfg['paint']}.RES"
            if res_key in disc.archives:
                stage_keys.append(res_key)

    with disc.stage(stage_keys, cancelled=cancelled) as staged_data:
        with tempfile.TemporaryDirectory(prefix="eod-nascar-scan-") as unpack_root:
            unpack_path = Path(unpack_root)
            sources: dict[str, Path] = {}
            for key in stage_keys:
                if cancelled is not None and cancelled():
                    raise NascarSourceCancelled("NASCAR source scan cancelled.")
                archive_name = Path(key).stem.upper()
                archive_file = staged_data / key
                dest = unpack_path / archive_name
                _unpack_quiet(archive_file, dest)
                sources[archive_name] = dest

            all_families_ready = (
                "COMMON" in sources
                and set(disc.available_families) >= set(FAMILY_ARCHIVES)
                and all(
                    cfg["paint"] in sources
                    for cfg in nascar_roster.FAMILIES.values()
                    if cfg["paint"]
                )
            )
            if all_families_ready:
                raw_records = nascar_roster.collect_roster(sources)
            else:
                raw_records = _collect_partial_roster(sources, disc.available_families)

    entries = sorted((_record_to_entry(rec) for rec in raw_records), key=_sort_key)
    counts_raw: dict[str, int] = {series: 0 for series in SERIES_ORDER}
    for entry in entries:
        counts_raw[entry.series] = counts_raw.get(entry.series, 0) + 1
    series_counts = {series: counts_raw[series] for series in SERIES_ORDER if counts_raw[series] > 0}
    summary = _format_summary(entries, series_counts)
    return NascarScanResult(
        source_path=source_path,
        entries=entries,
        series_counts=series_counts,
        summary_text=summary,
    )


def _find_donor_car_archive(eod_data: Path) -> Path:
    car_dir = eod_data / "CAR"
    preferred = car_dir / "ALGAR.CAR"
    if preferred.is_file():
        return preferred
    candidates = sorted(car_dir.glob("*.CAR")) if car_dir.is_dir() else []
    if not candidates:
        raise FileNotFoundError(f"No retail donor .CAR archive found under {car_dir}")
    return candidates[0]


def _pick_livery_id(entry: NascarCatalogEntry, used_ids: set[str]) -> int:
    key = f"{entry.roster_family}:{entry.car_number}"
    pinned = nascar_roster.LIVERY_IDS.get(key)
    if pinned is not None and str(pinned) not in used_ids:
        return int(pinned)
    for candidate in range(650, 800):
        if str(candidate) not in used_ids:
            return candidate
    for candidate in range(800, 999):
        if str(candidate) not in used_ids:
            return candidate
    raise ValueError("No free vehicle livery ID available in native CT.RES range")


def _entry_to_roster_record(entry: NascarCatalogEntry) -> dict[str, Any]:
    if entry.raw_record and "family" in entry.raw_record and "paint" in entry.raw_record:
        rec = copy.deepcopy(entry.raw_record)
        rec.setdefault("source_number", entry.car_number)
        rec.setdefault("name", entry.driver)
        rec.setdefault("body_style", entry.style)
        rec.setdefault("chassis", entry.chassis)
        return rec
    return {
        "family": entry.roster_family,
        "source_number": entry.car_number,
        "name": entry.driver,
        "body_style": entry.style,
        "paint": entry.livery_name,
        "driver": {"name": entry.driver, "number": entry.car_number, "body_style": entry.style},
        "chassis": entry.chassis,
    }


def _build_single_entry_overlay(
    *,
    entry: NascarCatalogEntry,
    sources: dict[str, Path],
    game_dir: Path,
    work_dir: Path,
    overlay_dir: Path,
    cancelled: Callable[[], bool] | None = None,
    progress: Callable[[int, str], None] | None = None,
) -> RegisteredVehicle:
    """Compile ONE NASCAR entry into overlay_dir using nascar_roster.build_chassis."""

    def _emit(pct: int, msg: str) -> None:
        if progress is not None:
            progress(pct, msg)

    def _check_cancel() -> None:
        if cancelled is not None and cancelled():
            raise InterruptedError("Vehicle build cancelled")

    eod_data = game_dir / "DATA"
    donor_car = _find_donor_car_archive(eod_data)
    roster_family = entry.roster_family
    cfg = nascar_roster.FAMILIES[roster_family]
    chassis = entry.chassis
    style = entry.style if roster_family == "stock" else "all"
    record = _entry_to_roster_record(entry)

    _check_cancel()
    _emit(35, f"Unpacking retail base tables for {entry.driver}")
    with contextlib.redirect_stdout(io.StringIO()):
        native_common = nascar_roster.Archive(eod_data / "RES" / "COMMON.RES", work_dir / "native-COMMON")
        native_ct = nascar_roster.Archive(eod_data / "RES" / "CT.RES", work_dir / "native-CT")
        native_race = nascar_roster.Archive(eod_data / "RES" / "RACE.RES", work_dir / "native-RACE")
        native_base = nascar_roster.Archive(donor_car, work_dir / "native-base")

    tables = {n: native_common.table(n) for n in ("carclass.val", "car_stats.val", "dirt.val", "buycar.val")}
    used_ids: set[str] = {
        str(dict(s["pairs"])["number"])
        for name in ("dirt.val", "buycar.val")
        for s in tables[name]["sections"]
        if "number" in dict(s["pairs"])
    }
    used_ids.update(
        m[1]
        for e in native_ct.manifest["entries"]
        if (m := re.fullmatch(r"(\d+)\.(?:tex|tm0)", e["name"], re.I))
    )
    livery_id = _pick_livery_id(entry, used_ids)

    _check_cancel()
    _emit(55, f"Transcoding {entry.series} #{entry.car_number} ({entry.driver})")
    car_dir = work_dir / f"car-{chassis}"
    with contextlib.redirect_stdout(io.StringIO()):
        nascar_roster.build_chassis(
            roster_family,
            style,
            chassis,
            [record],
            sources,
            native_base,
            car_dir,
        )

    (overlay_dir / "DATA" / "CAR").mkdir(parents=True, exist_ok=True)
    (overlay_dir / "DATA" / "RES").mkdir(parents=True, exist_ok=True)
    _pack_quiet(car_dir, overlay_dir / "DATA" / "CAR" / car_archive_filename(chassis))

    _check_cancel()
    _emit(72, f"Registering {chassis} #{livery_id} livery and physics")
    target_class = next(s for s in tables["carclass.val"]["sections"] if s["name"] == "$0001$chassis")
    if not any(k.casefold() == chassis.casefold() for k, _ in target_class["pairs"]):
        target_class["pairs"].append((chassis, ""))

    speed, handling, durability = cfg["stats"]
    known_stats = {dict(s["pairs"]).get("chassis", "").casefold() for s in tables["car_stats.val"]["sections"]}
    if chassis.casefold() not in known_stats:
        tables["car_stats.val"]["sections"].append(
            nascar_roster.section_like(
                tables["car_stats.val"],
                "carstats",
                dict(chassis=chassis, speed=str(speed), handling=str(handling), durability=str(durability)),
            )
        )

    driver_pairs = (
        copy.deepcopy(record["driver"])
        if isinstance(record.get("driver"), dict)
        else dict(name=entry.driver, rank="101", body_color="0xFFFFFFFF")
    )
    driver_pairs.update(chassis=chassis, number=str(livery_id), display_number=str(entry.car_number))
    driver_pairs.pop("ui_image", None)
    tables["dirt.val"]["sections"].append(
        nascar_roster.section_like(tables["dirt.val"], "Driver", driver_pairs)
    )
    for table_name in ("carclass.val", "car_stats.val", "dirt.val"):
        native_common.write_table(table_name, tables[table_name])

    paint_dir = sources[cfg["paint"]] if cfg["paint"] else sources[cfg["archive"]]
    paint_blob = convert_tex_v9_to_v10(find_entry(paint_dir, entry.livery_name).read_bytes(), aux=0x100)
    native_ct.add(f"{livery_id}.tm0", paint_blob, 10, "TEX", True)
    native_ct.add(f"{livery_id}.tex", drop_top_mip(paint_blob), 10, "TEX", False)

    phys_source = parse_binary_valt(find_entry(sources[cfg["archive"]], "physics.val").read_bytes())
    wheel_prefix = "npc" if roster_family == "pacecar" else "ncu"
    wheel_resources, _ = build_source_wheels(sources["RACE"], phys_source, prefix=wheel_prefix)
    for res_name, (blob, ver, fourcc) in wheel_resources.items():
        native_race.add(res_name, blob, ver, fourcc, replace=True)

    _check_cancel()
    _emit(88, "Packing isolated overlay archives")
    with contextlib.redirect_stdout(io.StringIO()):
        native_common.pack(overlay_dir / "DATA" / "RES" / "COMMON.RES")
        native_ct.pack(overlay_dir / "DATA" / "RES" / "CT.RES")
        native_race.pack(overlay_dir / "DATA" / "RES" / "RACE.RES")
    shutil.copy2(eod_data / "RES" / "UICAR.RES", overlay_dir / "DATA" / "RES" / "UICAR.RES")

    try:
        disc_id = slus_sha(game_dir)
    except ValueError:
        disc_id = ""
    meta = {
        "chassis": chassis,
        "number": livery_id,
        "disc": disc_id,
        "series": entry.series,
        "family": entry.family,
        "style": entry.style,
        "car_number": entry.car_number,
        "driver": entry.driver,
        "livery_name": entry.livery_name,
        "car_archive": entry.car_archive,
    }
    (overlay_dir / "vehicle.json").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")

    _emit(98, "Verifying registered vehicle overlay")
    return registered_vehicle(overlay_dir, chassis, livery_id)


class EphemeralTestDriveSession:
    """Isolated temporary build of ONE NASCAR vehicle for instant Test Drive -> Keep/Discard.

    Never writes to ~/.eod_world_editor or the retail disc directory.
    """

    def __init__(
        self,
        source_path: Path | str,
        game_dir: Path | str,
        entry: NascarCatalogEntry,
        *,
        temp_parent: Path | str | None = None,
    ) -> None:
        self.source_path = Path(source_path)
        self.game_dir = Path(game_dir)
        self.entry = entry
        self._tmpdir: tempfile.TemporaryDirectory[str] | None = tempfile.TemporaryDirectory(
            prefix="eod-nascar-testdrive-",
            dir=str(temp_parent) if temp_parent is not None else None,
        )
        self.temp_dir: Path = Path(self._tmpdir.name)
        self.overlay_dir: Path = self.temp_dir / "overlay"
        self.vehicle: RegisteredVehicle | None = None
        self._discarded: bool = False

    @property
    def is_discarded(self) -> bool:
        return self._discarded

    def build(
        self,
        *,
        cancelled: Callable[[], bool] | None = None,
        progress: Callable[[int, str], None] | None = None,
    ) -> RegisteredVehicle:
        """Build ONLY the selected NASCAR vehicle into self.temp_dir / 'overlay'."""
        if self._discarded:
            raise RuntimeError("Cannot build inside a discarded EphemeralTestDriveSession")
        if progress is not None:
            progress(10, f"Staging {self.entry.series} #{self.entry.car_number} from source")

        disc = NascarSource(self.source_path)
        roster_family = self.entry.roster_family
        required = set(FAMILY_ARCHIVES[roster_family]) | {"RES/COMMON.RES", "RES/RACE.RES"}
        if "RES/PAINT256.RES" in disc.archives:
            required.add("RES/PAINT256.RES")

        work_dir = self.temp_dir / "_work"
        work_dir.mkdir(parents=True, exist_ok=True)
        try:
            with disc.stage(sorted(required), cancelled=cancelled) as staged_data:
                if progress is not None:
                    progress(25, "Unpacking source car and texture archives")
                sources: dict[str, Path] = {}
                for key in sorted(required):
                    if cancelled is not None and cancelled():
                        raise InterruptedError("Vehicle build cancelled")
                    arc_name = Path(key).stem.upper()
                    arc_path = staged_data / key.removeprefix("CAR/")
                    dest = work_dir / f"source-{arc_name}"
                    _unpack_quiet(arc_path, dest)
                    sources[arc_name] = dest

                if self.overlay_dir.exists():
                    shutil.rmtree(self.overlay_dir)
                self.overlay_dir.mkdir(parents=True, exist_ok=True)
                vehicle = _build_single_entry_overlay(
                    entry=self.entry,
                    sources=sources,
                    game_dir=self.game_dir,
                    work_dir=work_dir,
                    overlay_dir=self.overlay_dir,
                    cancelled=cancelled,
                    progress=progress,
                )
        finally:
            shutil.rmtree(work_dir, ignore_errors=True)

        self.vehicle = vehicle
        if progress is not None:
            progress(100, f"Ready to test drive {self.entry.series} #{self.entry.car_number}")
        return vehicle

    def keep_to_directory(self, dest_dir: Path | str) -> Path:
        """Copy compiled overlay assets to dest_dir cleanly without re-transcoding."""
        if self._discarded:
            raise RuntimeError("Test-drive session was already discarded")
        if not self.overlay_dir.is_dir():
            raise RuntimeError("No compiled test-drive assets are available to keep")
        target = Path(dest_dir).resolve()
        target.mkdir(parents=True, exist_ok=True)
        shutil.copytree(self.overlay_dir, target, dirs_exist_ok=True)
        return target

    def keep_to_local_vehicles(self) -> RegisteredVehicle:
        """Persist the compiled test-drive vehicle into ~/.eod_world_editor/local-vehicles."""
        if self.vehicle is None:
            raise RuntimeError("Build the test-drive vehicle before keeping it")
        folder = editor_appdata_dir() / "local-vehicles"
        folder.mkdir(parents=True, exist_ok=True)
        dest = folder / f"{self.vehicle.chassis.lower()}-{self.vehicle.number}"
        self.keep_to_directory(dest)
        return registered_vehicle(dest, self.vehicle.chassis, self.vehicle.number)

    def discard(self) -> None:
        """Delete the temporary workspace cleanly, leaving zero trace on disk."""
        self.vehicle = None
        self._discarded = True
        if self._tmpdir is not None:
            try:
                self._tmpdir.cleanup()
            finally:
                self._tmpdir = None
        if self.temp_dir.exists():
            shutil.rmtree(self.temp_dir, ignore_errors=True)

    def __enter__(self) -> "EphemeralTestDriveSession":
        return self

    def __exit__(self, _exc_type, _exc, _tb) -> None:
        self.discard()

    def __del__(self) -> None:
        try:
            self.discard()
        except Exception:
            pass


def import_nascar_entries(
    source_path: Path | str,
    game_dir: Path | str,
    entries: Sequence[NascarCatalogEntry],
    *,
    dest_root: Path | None = None,
    cancelled: Callable[[], bool] | None = None,
    progress: Callable[[int, str], None] | None = None,
) -> list[RegisteredVehicle]:
    """Import one or more NASCAR catalog entries into local-vehicles (staging ISO archives once)."""
    items = list(entries)
    if not items:
        return []
    disc = NascarSource(source_path)
    required: set[str] = {"RES/COMMON.RES", "RES/RACE.RES"}
    for entry in items:
        required.update(FAMILY_ARCHIVES[entry.roster_family])
    if "RES/PAINT256.RES" in disc.archives:
        required.add("RES/PAINT256.RES")

    target_root = dest_root if dest_root is not None else (editor_appdata_dir() / "local-vehicles")
    target_root.mkdir(parents=True, exist_ok=True)

    imported: list[RegisteredVehicle] = []
    if progress is not None:
        progress(5, f"Staging NASCAR source archives for {len(items)} vehicle(s)")

    with disc.stage(sorted(required), cancelled=cancelled) as staged_data:
        with tempfile.TemporaryDirectory(prefix="eod-nascar-batch-") as scratch:
            scratch_path = Path(scratch)
            sources: dict[str, Path] = {}
            for key in sorted(required):
                if cancelled is not None and cancelled():
                    raise InterruptedError("Vehicle import cancelled")
                arc_name = Path(key).stem.upper()
                arc_path = staged_data / key.removeprefix("CAR/")
                dest = scratch_path / f"source-{arc_name}"
                _unpack_quiet(arc_path, dest)
                sources[arc_name] = dest

            for idx, entry in enumerate(items):
                if cancelled is not None and cancelled():
                    raise InterruptedError("Vehicle import cancelled")
                base_pct = 10 + int((idx * 85) / max(1, len(items)))
                if progress is not None:
                    progress(base_pct, f"Importing ({idx + 1}/{len(items)}): #{entry.car_number} {entry.driver}")
                item_work = scratch_path / f"work-{idx}"
                item_overlay = scratch_path / f"overlay-{idx}"
                item_work.mkdir(parents=True, exist_ok=True)
                item_overlay.mkdir(parents=True, exist_ok=True)
                try:
                    built = _build_single_entry_overlay(
                        entry=entry,
                        sources=sources,
                        game_dir=Path(game_dir),
                        work_dir=item_work,
                        overlay_dir=item_overlay,
                        cancelled=cancelled,
                        progress=None,
                    )
                    dest_dir = target_root / f"{built.chassis.lower()}-{built.number}"
                    if dest_dir.exists():
                        shutil.rmtree(dest_dir)
                    shutil.copytree(item_overlay, dest_dir)
                    imported.append(registered_vehicle(dest_dir, built.chassis, built.number))
                finally:
                    shutil.rmtree(item_work, ignore_errors=True)
                    shutil.rmtree(item_overlay, ignore_errors=True)

    if progress is not None:
        progress(100, f"Imported {len(imported)} NASCAR vehicle(s)")
    return imported
