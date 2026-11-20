"""Check how an imported vehicle is registered in a local mod layer."""

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
import re
import shutil
import tempfile

from home_hub import editor_appdata_dir
from import_nascar_cars import import_vehicle
from iso9660_reader import FAMILY_ARCHIVES, NascarSource
from modding_backend import directory, payload
from mod_package import scan_tree, slus_sha
from project_state import export_mod_layer, mark_road_edits_for_fresh_bake
from valt import parse_binary_valt


@dataclass(frozen=True)
class RegisteredVehicle:
    layer: Path
    archive: Path
    chassis: str
    number: int


def local_vehicles(game_dir: Path) -> list[RegisteredVehicle]:
    folder = editor_appdata_dir() / "local-vehicles"
    try:
        disc = slus_sha(game_dir)
    except ValueError:
        return []
    results = []
    for entry in sorted(folder.glob("*/vehicle.json")):
        if entry.parent.is_symlink() or entry.is_symlink():
            continue
        try:
            info = json.loads(entry.read_text(encoding="utf-8"))
            chassis, number = info["chassis"], info["number"]
            if (info["disc"] != disc or not isinstance(number, int)
                    or not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]{0,7}", chassis)):
                continue
            archive = entry.parent / "DATA" / "CAR" / f"{chassis.upper()}.CAR"
            if archive.is_file():
                results.append(RegisteredVehicle(entry.parent, archive, chassis, number))
        except (KeyError, OSError, TypeError, ValueError):
            continue
    return results


def import_local_stock(source: Path, game_dir: Path, chassis: str, number: int,
                       donor: str, car_class: int, paint: str, *, token, progress) -> RegisteredVehicle:
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]{0,7}", chassis):
        raise ValueError("Use a chassis name of 1 to 8 letters, digits or underscores")
    disc_id = slus_sha(game_dir)
    folder = editor_appdata_dir() / "local-vehicles"
    folder.mkdir(parents=True, exist_ok=True)
    destination = folder / f"{chassis.lower()}-{number}"
    if destination.exists():
        raise ValueError("A local import already uses this chassis and number")
    progress(10, "Checking the Dirt to Daytona source")
    disc = NascarSource(source)
    required = {*FAMILY_ARCHIVES["stock"], "RES/RACE.RES"}
    with tempfile.TemporaryDirectory(prefix=".import-", dir=folder) as scratch:
        stage = Path(scratch) / "overlay"
        with disc.stage(required, cancelled=lambda: token.is_cancelled) as staged_data:
            progress(35, "Converting the stock car, paint and game resources")
            import_vehicle("stock", staged_data, game_dir / "DATA", stage,
                           chassis, number, donor, car_class, paint or None)
        if token.is_cancelled:
            raise InterruptedError("Vehicle import cancelled")
        progress(85, "Checking the exact vehicle and livery registration")
        checked = registered_vehicle(stage, chassis, number)
        (stage / "vehicle.json").write_text(
            json.dumps({"chassis": checked.chassis, "number": number, "disc": disc_id}) + "\n",
            encoding="utf-8",
        )
        if token.is_cancelled:
            raise InterruptedError("Vehicle import cancelled")
        stage.rename(destination)
    return registered_vehicle(destination, chassis, number)


def registered_vehicle(layer: Path, chassis: str, number: int) -> RegisteredVehicle:
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]{0,7}", chassis):
        raise ValueError("Use a chassis name of 1 to 8 letters, digits or underscores")
    if not 0 <= number < 2147483646:
        raise ValueError("Choose a valid vehicle number")
    root = layer.resolve()
    archive = root / "DATA" / "CAR" / f"{chassis.upper()}.CAR"
    resources = root / "DATA" / "RES"
    common = resources / "COMMON.RES"
    textures = resources / "CT.RES"
    portraits = resources / "UICAR.RES"
    for path in (archive, common, textures, portraits):
        if not path.is_file() or not path.resolve().is_relative_to(root):
            raise ValueError(f"Missing imported vehicle resource: {path.name}")

    classes = parse_binary_valt(payload(common, "carclass.val")[0])["sections"]
    name = chassis.casefold()
    if not any(key.casefold() == name for section in classes for key, _ in section["pairs"]):
        raise ValueError(f"{chassis} is absent from the car class list")
    roster = parse_binary_valt(payload(common, "dirt.val")[0])["sections"]
    matches = [dict(section["pairs"]) for section in roster if section["name"].casefold() == "driver"]
    matches = [record for record in matches if record.get("number") == str(number)]
    if len(matches) != 1 or matches[0].get("chassis", "").casefold() != name:
        raise ValueError(f"Vehicle #{number} does not uniquely identify {chassis} in the action roster")
    texture_names = {entry["name"].casefold() for entry in directory(textures)}
    if not {f"{number}.tex", f"{number}.tm0"} <= texture_names:
        raise ValueError(f"Missing numbered livery for vehicle #{number} in CT.RES")
    return RegisteredVehicle(root, archive, matches[0]["chassis"], number)


def build_vehicle_pie_layer(world, project_name: str, vehicle: RegisteredVehicle,
                            *, token, progress):
    vehicle = registered_vehicle(vehicle.layer, vehicle.chassis, vehicle.number)
    workspace = tempfile.TemporaryDirectory(prefix="eod-vehicle-pie-")
    try:
        layer = Path(workspace.name) / "overlay"
        mark_road_edits_for_fresh_bake(world, token)
        report = export_mod_layer(world, project_name, layer, token=token, allow_delta=False,
                                  progress=lambda pct, message: progress(10 + pct * 7 // 10, message))
        progress(85, f"Composing {vehicle.chassis} #{vehicle.number} with the track")
        for name in scan_tree(vehicle.layer):
            if token.is_cancelled:
                raise InterruptedError("Vehicle test drive cancelled")
            if not name.startswith(("DATA/CAR/", "DATA/RES/")):
                continue
            target = layer / name
            if target.exists():
                raise ValueError(f"The vehicle and track both supply {name}")
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(vehicle.layer / name, target)
        registered_vehicle(layer, vehicle.chassis, vehicle.number)
        progress(100, "Registered vehicle ready to test")
        return {**report, "pie_overlay": workspace, "pie_layer": layer, "pie_vehicle": vehicle}
    except BaseException:
        workspace.cleanup()
        raise
