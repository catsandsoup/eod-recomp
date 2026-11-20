"""Compose an isolated profile overlay for one World Editor PIE session."""

from __future__ import annotations

import shutil
import tempfile
from collections.abc import Callable
from pathlib import Path
from typing import Any

from project_state import export_mod_layer, mark_road_edits_for_fresh_bake
from workers import CancelToken
from world_loader import TrackWorld
import mod_install
import mod_package


def _merge_profile_archive(retail: Path, installed: Path, authored: Path) -> None:
    """Retain authored changes and add installed deltas for untouched retail members."""
    if not authored.is_file():
        authored.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(installed, authored)
        return
    retail_entries = {item["name"].lower(): item for item in mod_package.directory(retail)}
    authored_entries = {item["name"].lower(): item for item in mod_package.directory(authored)}
    installed_entries = mod_package.directory(installed)
    changes = {}
    for entry in installed_entries:
        key = entry["name"].lower()
        base = retail_entries.get(key)
        edit = authored_entries.get(key)
        if base is None or edit is None:
            continue
        if (base["size"], base["crc"]) == (entry["size"], entry["crc"]):
            continue
        if (base["size"], base["crc"]) != (edit["size"], edit["crc"]):
            continue
        original, _ = mod_package.payload(retail, base["name"], verify_crc=False)
        current, _ = mod_package.payload(authored, edit["name"], verify_crc=False)
        if current == original:
            changes[edit["name"]] = mod_package.payload(installed, entry["name"])[0]
    if changes:
        data, _ = mod_package.build_archive(authored, changes)
        mod_package.atomic_write(authored, data)


def _host_rows(layer: Path) -> list[list[str]]:
    manifest = layer / "HOSTTEX" / "replacements.tsv"
    if not manifest.is_file():
        return []
    lines = manifest.read_text(encoding="utf-8").splitlines()
    if not lines or lines[0] != "eod-host-textures 1":
        raise ValueError(f"Invalid HOSTTEX manifest in {layer}")
    rows = [line.split("\t") for line in lines[1:] if line]
    if any(len(row) != 7 for row in rows):
        raise ValueError(f"Invalid HOSTTEX entry in {layer}")
    return rows


def _merge_profile_host_textures(installed: Path, authored: Path) -> None:
    rows = _host_rows(authored)
    authored_assets = {row[5].lower() for row in rows}
    authored_identities = {(row[0].lower(), row[1].lower()) for row in rows}
    keep = [
        row for row in _host_rows(installed)
        if row[5].lower() not in authored_assets and (row[0].lower(), row[1].lower()) not in authored_identities
    ]
    if not keep:
        return
    dest = authored / "HOSTTEX"
    dest.mkdir(parents=True, exist_ok=True)
    profile_textures = Path(tempfile.mkdtemp(prefix="installed-", dir=dest))
    for source_row in keep:
        row = list(source_row)
        relative = mod_package.check_rel(row[4])
        copied = profile_textures / relative
        copied.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(installed / "HOSTTEX" / relative, copied)
        row[4] = copied.relative_to(dest).as_posix()
        rows.append(row)
    manifest = "\n".join(["eod-host-textures 1", *("\t".join(row) for row in rows)]) + "\n"
    mod_package.atomic_write(dest / "replacements.tsv", manifest.encode("utf-8"))


def build_profile_pie_layer(
    world: TrackWorld,
    project_name: str,
    profile: str,
    root: Path,
    *,
    token: CancelToken,
    progress: Callable[[int, str], None],
) -> dict[str, Any]:
    """Bake project state and layer installed content into a temporary child-only root."""
    workspace = tempfile.TemporaryDirectory(prefix="eod-profile-pie-")
    try:
        layer = Path(workspace.name) / "overlay"
        progress(5, "Checking racing lines against retail")
        mark_road_edits_for_fresh_bake(world, token)
        report = export_mod_layer(
            world, project_name, layer, token=token, allow_delta=False,
            progress=lambda pct, message: progress(10 + pct * 7 // 10, message),
        )
        if token.is_cancelled:
            raise InterruptedError("PIE profile build cancelled")
        profile_dir = mod_install.profile_dir(root, profile)
        if profile_dir.is_symlink() or not (profile_dir / "profile.json").is_file():
            raise ValueError(f"No local mod profile named {profile}")
        progress(85, f"Merging installed profile {profile}")
        with mod_install.locked(profile_dir):
            selected = mod_install.load(profile_dir, profile)
            mod_install.use_disc(selected, world.game_dir)
            active = Path(mod_install.launch_env(profile=profile, root=root)["EOD_MOD_DIR"])
            merged_archives = {}
            for relative in mod_package.scan_tree(active):
                if token.is_cancelled:
                    raise InterruptedError("PIE profile build cancelled")
                if relative.upper().startswith("DATA/"):
                    base, spelled = mod_package.retail_archive(world.game_dir, relative)
                    output = layer / spelled
                    _merge_profile_archive(base, active / relative, output)
                    merged_archives[spelled] = mod_package.file_sha(output)
            _merge_profile_host_textures(active, layer)
        profile_text = layer / "PROFILE.TXT"
        records = [
            line for line in profile_text.read_text(encoding="utf-8").splitlines()
            if not any(line.startswith(f"native {name} ") for name in merged_archives)
        ]
        records += [f"installed_profile {profile}", *(
            f"native {name} {digest}" for name, digest in sorted(merged_archives.items())
        )]
        profile_text.write_text("\n".join(records) + "\n", encoding="utf-8")
        progress(100, f"Ready to play with profile {profile}")
        return {**report, "pie_overlay": workspace, "pie_layer": layer, "pie_profile": profile}
    except BaseException:
        workspace.cleanup()
        raise
