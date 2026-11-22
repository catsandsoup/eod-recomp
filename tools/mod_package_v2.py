#!/usr/bin/env python3
"""Schema v2 (.eodmod) content-addressed mod package format and stack composer.

Schema: "eod.mod/2"
Container: ZIP archive containing:
  - manifest.json: declarative metadata, typed operations, and SHA-256 + BLAKE2b digests
  - blobs/<sha256>: content-addressed binary payloads (deduplicated)

Supported operation types:
  - addArchive: full archive addition/replacement (e.g. DATA/CAR/CUP24.CAR, DATA/TRACKS/CUSTOM.TRK, *.eodcol)
  - addMember: named member addition/replacement inside a target archive (CT.RES, RACE.RES, RACESND.RES, UICAR.RES, COMMON.RES, ...)
  - valtPatch: declarative semantic key/section merge for .val tables (carclass.val, car_stats.val, buycar.val, dirt.val, tracks.val, ...)
  - hostTexture: .eodhtx 4K/8K host texture replacement
"""

from __future__ import annotations

import copy
import hashlib
import json
import os
import re
import zipfile
from pathlib import Path
from typing import Any, Mapping, Sequence

SCHEMA_V2 = "eod.mod/2"
SUPPORTED_OPS = frozenset({"addArchive", "addMember", "valtPatch", "hostTexture"})
BLOB_OPS = frozenset({"addArchive", "addMember", "hostTexture"})

FORBIDDEN_EXTENSIONS = frozenset(
    {
        ".exe",
        ".dll",
        ".dylib",
        ".so",
        ".sh",
        ".bat",
        ".ps1",
        ".py",
    }
)

KNOWN_MEMBER_ARCHIVES = frozenset(
    {
        "CT.RES",
        "RACE.RES",
        "RACESND.RES",
        "UICAR.RES",
        "COMMON.RES",
    }
)

KNOWN_VALT_TABLES = frozenset(
    {
        "carclass.val",
        "car_stats.val",
        "buycar.val",
        "dirt.val",
        "tracks.val",
    }
)

_WINDOWS_DRIVE_RE = re.compile(r"^[A-Za-z]:")
_VERSIONED_SO_RE = re.compile(r"\.so(\.\d+)+$", re.IGNORECASE)
_FIXED_ZIP_DATE = (2025, 1, 1, 0, 0, 0)


class ModPackageError(ValueError):
    """Base error for malformed or invalid .eodmod v2 packages."""


class ModPackageSecurityError(ModPackageError):
    """Raised when path traversal or forbidden executable content is detected."""


class ModPackageIntegrityError(ModPackageError):
    """Raised when SHA-256, BLAKE2b, or size verification fails."""


def compute_digests(data: bytes) -> dict[str, Any]:
    """Return content-addressed SHA-256, BLAKE2b, and byte length for raw bytes."""
    return {
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "blake2b": hashlib.blake2b(data).hexdigest(),
    }


def _check_forbidden_extension(raw_path: str, *, context: str) -> None:
    """Reject executable extensions anywhere in a filename or path."""
    lowered = raw_path.replace("\\", "/").lower()
    for segment in lowered.split("/"):
        if not segment:
            continue
        _, ext = os.path.splitext(segment)
        if ext in FORBIDDEN_EXTENSIONS or _VERSIONED_SO_RE.search(segment):
            raise ModPackageSecurityError(
                f"Forbidden executable extension in {context}: {raw_path!r}"
            )


def validate_safe_rel_path(
    raw_path: str,
    *,
    context: str = "path",
    allow_nested: bool = True,
    check_executable: bool = True,
) -> str:
    """Validate and normalize a relative POSIX path, blocking traversal & executables."""
    if not isinstance(raw_path, str) or not raw_path.strip():
        raise ModPackageSecurityError(f"Empty or non-string {context}")
    if "\x00" in raw_path or any(ord(ch) < 32 for ch in raw_path):
        raise ModPackageSecurityError(f"Control character in {context}: {raw_path!r}")
    if raw_path.startswith("/") or raw_path.startswith("\\"):
        raise ModPackageSecurityError(f"Absolute path forbidden in {context}: {raw_path!r}")
    if _WINDOWS_DRIVE_RE.match(raw_path):
        raise ModPackageSecurityError(f"Drive-prefixed path forbidden in {context}: {raw_path!r}")

    normalized_slashes = raw_path.replace("\\", "/")
    raw_parts = normalized_slashes.split("/")
    clean_parts: list[str] = []
    for part in raw_parts:
        if part == "" or part == ".":
            continue
        if part == "..":
            raise ModPackageSecurityError(f"Path traversal ('..') forbidden in {context}: {raw_path!r}")
        if ":" in part:
            raise ModPackageSecurityError(f"Colon forbidden in {context}: {raw_path!r}")
        clean_parts.append(part)

    if not clean_parts:
        raise ModPackageSecurityError(f"Empty normalized {context}: {raw_path!r}")
    if not allow_nested and len(clean_parts) != 1:
        raise ModPackageSecurityError(f"Nested path not allowed in {context}: {raw_path!r}")

    normalized = "/".join(clean_parts)
    if check_executable:
        _check_forbidden_extension(normalized, context=context)
    return normalized


def _validate_zip_member_name(name: str) -> str:
    """Validate an entry path inside a .eodmod ZIP container."""
    if not name or name.endswith("/"):
        # Directory entries still must not traverse or be absolute.
        stripped = name.rstrip("/")
        if stripped:
            validate_safe_rel_path(stripped, context="zip entry")
        return name
    return validate_safe_rel_path(name, context="zip entry")


def _read_blob_bytes(blob_value: bytes | bytearray | memoryview | Path | str) -> bytes:
    if isinstance(blob_value, (bytes, bytearray, memoryview)):
        return bytes(blob_value)
    if isinstance(blob_value, (Path, str)):
        return Path(blob_value).read_bytes()
    raise ModPackageError(f"Unsupported blob payload type: {type(blob_value).__name__}")


def _validate_manifest_header(manifest: Mapping[str, Any]) -> dict[str, Any]:
    if not isinstance(manifest, Mapping):
        raise ModPackageError("Manifest must be a JSON object")

    schema = manifest.get("schema", SCHEMA_V2)
    if schema != SCHEMA_V2:
        raise ModPackageError(f"Unsupported schema {schema!r}; expected {SCHEMA_V2!r}")

    mod_id = manifest.get("id")
    if not isinstance(mod_id, str) or not mod_id.strip():
        raise ModPackageError("Manifest requires non-empty string 'id'")
    validate_safe_rel_path(mod_id, context="manifest.id", allow_nested=False)

    name = manifest.get("name")
    if not isinstance(name, str) or not name.strip():
        raise ModPackageError("Manifest requires non-empty string 'name'")

    version = manifest.get("version")
    if not isinstance(version, str) or not version.strip():
        raise ModPackageError("Manifest requires non-empty string 'version'")

    operations = manifest.get("operations")
    if not isinstance(operations, list):
        raise ModPackageError("Manifest requires 'operations' list")

    header: dict[str, Any] = {
        "schema": SCHEMA_V2,
        "id": mod_id.strip(),
        "name": name.strip(),
        "version": version.strip(),
    }
    for optional_key in ("author", "description", "dependencies", "tags"):
        if optional_key in manifest:
            header[optional_key] = copy.deepcopy(manifest[optional_key])
    return header


def _validate_valt_patches_tree(node: Any, *, path: str) -> None:
    if not isinstance(node, Mapping):
        raise ModPackageError(f"valtPatch {path} must be a JSON object")
    for key, value in node.items():
        if not isinstance(key, str) or not key.strip():
            raise ModPackageError(f"valtPatch keys must be non-empty strings at {path}")
        if isinstance(value, Mapping):
            _validate_valt_patches_tree(value, path=f"{path}.{key}")


def _normalize_operation_for_pack(
    op: Mapping[str, Any],
    index: int,
    blobs_map: Mapping[str, bytes],
) -> tuple[dict[str, Any], str | None, bytes | None]:
    if not isinstance(op, Mapping):
        raise ModPackageError(f"Operation #{index} must be a mapping")
    op_type = op.get("op")
    if op_type not in SUPPORTED_OPS:
        raise ModPackageError(
            f"Operation #{index} has unsupported op {op_type!r}; supported: {sorted(SUPPORTED_OPS)}"
        )

    if op_type == "addArchive":
        target = validate_safe_rel_path(op.get("target", ""), context=f"op[{index}].target")
        blob_key = validate_safe_rel_path(
            op.get("blob", target), context=f"op[{index}].blob"
        )
        if blob_key not in blobs_map:
            raise ModPackageError(f"Operation #{index} references missing blob {blob_key!r}")
        data = blobs_map[blob_key]
        digests = compute_digests(data)
        zip_entry = f"blobs/{digests['sha256']}"
        norm = {
            "op": "addArchive",
            "target": target,
            "blob": blob_key,
            "zip_entry": zip_entry,
            **digests,
        }
        return norm, blob_key, data

    if op_type == "addMember":
        archive = validate_safe_rel_path(op.get("archive", ""), context=f"op[{index}].archive")
        member = validate_safe_rel_path(op.get("member", ""), context=f"op[{index}].member")
        default_blob = f"{archive}/{member}"
        blob_key = validate_safe_rel_path(
            op.get("blob", default_blob), context=f"op[{index}].blob"
        )
        if blob_key not in blobs_map:
            raise ModPackageError(f"Operation #{index} references missing blob {blob_key!r}")
        data = blobs_map[blob_key]
        digests = compute_digests(data)
        zip_entry = f"blobs/{digests['sha256']}"
        norm = {
            "op": "addMember",
            "archive": archive,
            "member": member,
            "blob": blob_key,
            "zip_entry": zip_entry,
            **digests,
        }
        return norm, blob_key, data

    if op_type == "hostTexture":
        target = validate_safe_rel_path(op.get("target", ""), context=f"op[{index}].target")
        if not target.lower().endswith(".eodhtx"):
            raise ModPackageError(
                f"hostTexture operation #{index} target must end with '.eodhtx': {target!r}"
            )
        blob_key = validate_safe_rel_path(
            op.get("blob", target), context=f"op[{index}].blob"
        )
        if blob_key not in blobs_map:
            raise ModPackageError(f"Operation #{index} references missing blob {blob_key!r}")
        data = blobs_map[blob_key]
        digests = compute_digests(data)
        zip_entry = f"blobs/{digests['sha256']}"
        norm = {
            "op": "hostTexture",
            "target": target,
            "blob": blob_key,
            "zip_entry": zip_entry,
            **digests,
        }
        if "width" in op:
            norm["width"] = int(op["width"])
        if "height" in op:
            norm["height"] = int(op["height"])
        return norm, blob_key, data

    # valtPatch
    table = validate_safe_rel_path(op.get("table", ""), context=f"op[{index}].table")
    if not table.lower().endswith(".val"):
        raise ModPackageError(
            f"valtPatch operation #{index} table must end with '.val': {table!r}"
        )
    patches = op.get("patches")
    _validate_valt_patches_tree(patches, path=f"op[{index}].patches")
    norm = {
        "op": "valtPatch",
        "table": table,
        "patches": copy.deepcopy(dict(patches)),
    }
    return norm, None, None


def _write_zip_entry(zf: zipfile.ZipFile, arcname: str, data: bytes) -> None:
    info = zipfile.ZipInfo(filename=arcname, date_time=_FIXED_ZIP_DATE)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    zf.writestr(info, data)


def pack_eodmod_v2(
    manifest: Mapping[str, Any],
    blobs: Mapping[str, bytes | bytearray | memoryview | Path | str] | None,
    out_path: Path | str,
) -> Path:
    """Pack a validated Schema v2 .eodmod ZIP package with SHA-256 + BLAKE2b digests."""
    header = _validate_manifest_header(manifest)

    # Validate every supplied blob key up front (even unreferenced blobs cannot smuggle traversal/executables).
    normalized_blobs: dict[str, bytes] = {}
    for raw_key, raw_val in (blobs or {}).items():
        clean_key = validate_safe_rel_path(raw_key, context="blobs key")
        if clean_key in normalized_blobs:
            raise ModPackageError(f"Duplicate normalized blob key: {clean_key!r}")
        normalized_blobs[clean_key] = _read_blob_bytes(raw_val)

    normalized_ops: list[dict[str, Any]] = []
    manifest_blobs: dict[str, dict[str, Any]] = {}
    content_addressed_store: dict[str, bytes] = {}

    for idx, op in enumerate(manifest["operations"]):
        norm_op, blob_key, data = _normalize_operation_for_pack(op, idx, normalized_blobs)
        normalized_ops.append(norm_op)
        if blob_key is not None and data is not None:
            zip_entry = norm_op["zip_entry"]
            content_addressed_store[zip_entry] = data
            manifest_blobs[blob_key] = {
                "zip_entry": zip_entry,
                "size": norm_op["size"],
                "sha256": norm_op["sha256"],
                "blake2b": norm_op["blake2b"],
            }

    final_manifest: dict[str, Any] = {
        **header,
        "operations": normalized_ops,
        "blobs": dict(sorted(manifest_blobs.items())),
    }

    out = Path(out_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    manifest_bytes = (
        json.dumps(final_manifest, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")

    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        _write_zip_entry(zf, "manifest.json", manifest_bytes)
        for zip_entry in sorted(content_addressed_store):
            _write_zip_entry(zf, zip_entry, content_addressed_store[zip_entry])

    return out


def _verify_blob_entry(
    zf: zipfile.ZipFile,
    zip_entry: str,
    *,
    expected_sha256: str,
    expected_blake2b: str,
    expected_size: int | None,
    context: str,
) -> bytes:
    clean_entry = validate_safe_rel_path(zip_entry, context=context)
    try:
        data = zf.read(clean_entry)
    except KeyError as exc:
        raise ModPackageIntegrityError(
            f"Missing blob entry {clean_entry!r} referenced by {context}"
        ) from exc

    digests = compute_digests(data)
    if expected_size is not None and digests["size"] != expected_size:
        raise ModPackageIntegrityError(
            f"Size mismatch for {context} ({clean_entry}): expected {expected_size}, got {digests['size']}"
        )
    if digests["sha256"] != expected_sha256:
        raise ModPackageIntegrityError(
            f"SHA-256 mismatch for {context} ({clean_entry}): expected {expected_sha256}, got {digests['sha256']}"
        )
    if digests["blake2b"] != expected_blake2b:
        raise ModPackageIntegrityError(
            f"BLAKE2b mismatch for {context} ({clean_entry}): expected {expected_blake2b}, got {digests['blake2b']}"
        )
    return data


def _load_and_verify_package(path: Path | str) -> tuple[dict[str, Any], dict[str, bytes], dict[str, Any]]:
    pkg_path = Path(path)
    if not pkg_path.is_file():
        raise ModPackageError(f"Package not found: {pkg_path}")

    pkg_digests = compute_digests(pkg_path.read_bytes())

    with zipfile.ZipFile(pkg_path, "r") as zf:
        names = zf.namelist()
        for name in names:
            _validate_zip_member_name(name)

        if "manifest.json" not in names:
            raise ModPackageError("Package missing required 'manifest.json'")

        try:
            manifest = json.loads(zf.read("manifest.json").decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ModPackageError(f"Invalid manifest.json: {exc}") from exc

        _validate_manifest_header(manifest)

        blobs_section = manifest.get("blobs", {})
        if not isinstance(blobs_section, Mapping):
            raise ModPackageError("manifest.blobs must be a JSON object")

        verified_entries: dict[str, bytes] = {}
        for blob_key, meta in blobs_section.items():
            validate_safe_rel_path(blob_key, context="manifest.blobs key")
            if not isinstance(meta, Mapping):
                raise ModPackageError(f"manifest.blobs[{blob_key!r}] must be an object")
            zip_entry = meta.get("zip_entry", f"blobs/{meta.get('sha256', '')}")
            data = _verify_blob_entry(
                zf,
                zip_entry,
                expected_sha256=str(meta.get("sha256", "")),
                expected_blake2b=str(meta.get("blake2b", "")),
                expected_size=meta.get("size"),
                context=f"manifest.blobs[{blob_key!r}]",
            )
            verified_entries[zip_entry] = data

        for idx, op in enumerate(manifest["operations"]):
            if not isinstance(op, Mapping):
                raise ModPackageError(f"Operation #{idx} must be an object")
            op_type = op.get("op")
            if op_type not in SUPPORTED_OPS:
                raise ModPackageError(f"Operation #{idx} has unsupported op {op_type!r}")

            if op_type == "addArchive":
                validate_safe_rel_path(op.get("target", ""), context=f"op[{idx}].target")
                validate_safe_rel_path(op.get("blob", op.get("target", "")), context=f"op[{idx}].blob")
            elif op_type == "addMember":
                validate_safe_rel_path(op.get("archive", ""), context=f"op[{idx}].archive")
                validate_safe_rel_path(op.get("member", ""), context=f"op[{idx}].member")
                if "blob" in op:
                    validate_safe_rel_path(op["blob"], context=f"op[{idx}].blob")
            elif op_type == "hostTexture":
                target = validate_safe_rel_path(op.get("target", ""), context=f"op[{idx}].target")
                if not target.lower().endswith(".eodhtx"):
                    raise ModPackageError(
                        f"hostTexture operation #{idx} target must end with '.eodhtx': {target!r}"
                    )
                if "blob" in op:
                    validate_safe_rel_path(op["blob"], context=f"op[{idx}].blob")
            elif op_type == "valtPatch":
                table = validate_safe_rel_path(op.get("table", ""), context=f"op[{idx}].table")
                if not table.lower().endswith(".val"):
                    raise ModPackageError(
                        f"valtPatch operation #{idx} table must end with '.val': {table!r}"
                    )
                _validate_valt_patches_tree(op.get("patches"), path=f"op[{idx}].patches")

            if op_type in BLOB_OPS:
                sha256 = str(op.get("sha256", ""))
                blake2b = str(op.get("blake2b", ""))
                zip_entry = str(op.get("zip_entry", f"blobs/{sha256}"))
                if not sha256 or not blake2b:
                    raise ModPackageIntegrityError(
                        f"Operation #{idx} ({op_type}) missing required sha256/blake2b digests"
                    )
                data = _verify_blob_entry(
                    zf,
                    zip_entry,
                    expected_sha256=sha256,
                    expected_blake2b=blake2b,
                    expected_size=op.get("size"),
                    context=f"op[{idx}]",
                )
                verified_entries[zip_entry] = data

    return manifest, verified_entries, pkg_digests


def inspect_eodmod_v2(path: Path | str) -> dict[str, Any]:
    """Validate and inspect a Schema v2 .eodmod package, verifying all blob digests."""
    manifest, _, pkg_digests = _load_and_verify_package(path)
    counts = {op_name: 0 for op_name in sorted(SUPPORTED_OPS)}
    for op in manifest["operations"]:
        counts[op["op"]] += 1

    return {
        "path": str(Path(path)),
        "schema": manifest["schema"],
        "id": manifest["id"],
        "name": manifest["name"],
        "version": manifest["version"],
        "operations": copy.deepcopy(manifest["operations"]),
        "blobs": copy.deepcopy(manifest.get("blobs", {})),
        "counts": counts,
        "total_operations": len(manifest["operations"]),
        "package_size": pkg_digests["size"],
        "package_sha256": pkg_digests["sha256"],
        "package_blake2b": pkg_digests["blake2b"],
        "verified": True,
    }


def _safe_stage_target(stage_root: Path, rel_path: str) -> Path:
    clean_rel = validate_safe_rel_path(rel_path, context="staged target")
    resolved_root = stage_root.resolve()
    dest = (resolved_root / clean_rel).resolve()
    try:
        dest.relative_to(resolved_root)
    except ValueError as exc:
        raise ModPackageSecurityError(f"Staged target escapes stage_dir: {rel_path!r}") from exc
    return dest


def _merge_valt_tree(
    dst: dict[str, Any],
    src: Mapping[str, Any],
    *,
    table: str,
    mod_id: str,
    provenance: dict[str, str],
    conflicts: list[dict[str, Any]],
    prefix: str = "",
) -> None:
    for key, val in src.items():
        dotted = f"{prefix}.{key}" if prefix else key
        if isinstance(val, Mapping):
            existing = dst.get(key)
            if isinstance(existing, dict):
                _merge_valt_tree(
                    existing,
                    val,
                    table=table,
                    mod_id=mod_id,
                    provenance=provenance,
                    conflicts=conflicts,
                    prefix=dotted,
                )
            else:
                if key in dst:
                    conflicts.append(
                        {
                            "table": table,
                            "key": dotted,
                            "previous_mod": provenance.get(dotted, ""),
                            "override_mod": mod_id,
                            "previous_value": copy.deepcopy(existing),
                            "new_value": copy.deepcopy(dict(val)),
                        }
                    )
                new_branch: dict[str, Any] = {}
                _merge_valt_tree(
                    new_branch,
                    val,
                    table=table,
                    mod_id=mod_id,
                    provenance=provenance,
                    conflicts=conflicts,
                    prefix=dotted,
                )
                dst[key] = new_branch
        else:
            if key in dst and dst[key] != val:
                conflicts.append(
                    {
                        "table": table,
                        "key": dotted,
                        "previous_mod": provenance.get(dotted, ""),
                        "override_mod": mod_id,
                        "previous_value": copy.deepcopy(dst[key]),
                        "new_value": copy.deepcopy(val),
                    }
                )
            dst[key] = copy.deepcopy(val)
            provenance[dotted] = mod_id


def compose_eodmod_v2_stack(
    package_paths: Sequence[Path | str],
    stage_dir: Path | str,
) -> dict[str, Any]:
    """Compose an ordered stack of Schema v2 .eodmod packages into stage_dir.

    Operations are applied in package order. `valtPatch` operations are semantically
    merged by table/section/key into `stage_dir/valt_patches.json` with zero false
    conflicts across distinct entries. All blobs are integrity-verified before staging.
    """
    stage_root = Path(stage_dir)
    stage_root.mkdir(parents=True, exist_ok=True)

    counts = {
        "packages": 0,
        "addArchive": 0,
        "addMember": 0,
        "valtPatch": 0,
        "hostTexture": 0,
        "total_operations": 0,
    }
    applied_packages: list[dict[str, Any]] = []
    staged_files: dict[str, dict[str, Any]] = {}
    archive_members: dict[str, dict[str, dict[str, Any]]] = {}
    valt_tables: dict[str, dict[str, Any]] = {}
    valt_provenance: dict[str, dict[str, str]] = {}
    valt_conflicts: list[dict[str, Any]] = []

    for pkg_path in package_paths:
        manifest, entries, pkg_digests = _load_and_verify_package(pkg_path)
        mod_id = manifest["id"]
        counts["packages"] += 1
        applied_packages.append(
            {
                "id": mod_id,
                "name": manifest["name"],
                "version": manifest["version"],
                "path": str(Path(pkg_path)),
                "sha256": pkg_digests["sha256"],
                "blake2b": pkg_digests["blake2b"],
                "size": pkg_digests["size"],
            }
        )

        for op in manifest["operations"]:
            op_type = op["op"]
            counts[op_type] += 1
            counts["total_operations"] += 1

            if op_type == "addArchive":
                target = validate_safe_rel_path(op["target"], context="addArchive.target")
                data = entries[op["zip_entry"]]
                dest = _safe_stage_target(stage_root, target)
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(data)
                staged_files[target] = {
                    "op": "addArchive",
                    "mod_id": mod_id,
                    "size": op["size"],
                    "sha256": op["sha256"],
                    "blake2b": op["blake2b"],
                }

            elif op_type == "addMember":
                archive = validate_safe_rel_path(op["archive"], context="addMember.archive")
                member = validate_safe_rel_path(op["member"], context="addMember.member")
                data = entries[op["zip_entry"]]
                rel_staged = f"archive_members/{archive}/{member}"
                dest = _safe_stage_target(stage_root, rel_staged)
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(data)
                member_record = {
                    "archive": archive,
                    "member": member,
                    "staged_path": rel_staged,
                    "mod_id": mod_id,
                    "size": op["size"],
                    "sha256": op["sha256"],
                    "blake2b": op["blake2b"],
                }
                archive_members.setdefault(archive, {})[member] = member_record
                staged_files[rel_staged] = {
                    "op": "addMember",
                    "archive": archive,
                    "member": member,
                    "mod_id": mod_id,
                    "size": op["size"],
                    "sha256": op["sha256"],
                    "blake2b": op["blake2b"],
                }

            elif op_type == "hostTexture":
                target = validate_safe_rel_path(op["target"], context="hostTexture.target")
                data = entries[op["zip_entry"]]
                dest = _safe_stage_target(stage_root, target)
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(data)
                staged_files[target] = {
                    "op": "hostTexture",
                    "mod_id": mod_id,
                    "size": op["size"],
                    "sha256": op["sha256"],
                    "blake2b": op["blake2b"],
                }

            elif op_type == "valtPatch":
                table_key = validate_safe_rel_path(op["table"], context="valtPatch.table").lower()
                table_dst = valt_tables.setdefault(table_key, {})
                table_prov = valt_provenance.setdefault(table_key, {})
                _merge_valt_tree(
                    table_dst,
                    op["patches"],
                    table=table_key,
                    mod_id=mod_id,
                    provenance=table_prov,
                    conflicts=valt_conflicts,
                )

    valt_payload = {
        "schema": SCHEMA_V2,
        "tables": dict(sorted(valt_tables.items())),
        "conflicts": valt_conflicts,
    }
    valt_bytes = (json.dumps(valt_payload, indent=2, sort_keys=True) + "\n").encode("utf-8")
    valt_path = stage_root / "valt_patches.json"
    valt_path.write_bytes(valt_bytes)
    valt_digests = compute_digests(valt_bytes)
    staged_files["valt_patches.json"] = {
        "op": "valtPatch",
        **valt_digests,
    }

    if archive_members:
        members_manifest_bytes = (
            json.dumps(
                {"schema": SCHEMA_V2, "archives": dict(sorted(archive_members.items()))},
                indent=2,
                sort_keys=True,
            )
            + "\n"
        ).encode("utf-8")
        members_manifest_path = stage_root / "archive_members.json"
        members_manifest_path.write_bytes(members_manifest_bytes)
        staged_files["archive_members.json"] = {
            "op": "addMemberManifest",
            **compute_digests(members_manifest_bytes),
        }

    digest_manifest = {
        "schema": SCHEMA_V2,
        "stage_dir": str(stage_root),
        "packages": applied_packages,
        "counts": counts,
        "valt_patches_path": str(valt_path),
        "valt_tables": dict(sorted(valt_tables.items())),
        "valt_conflicts": valt_conflicts,
        "archive_members": dict(sorted(archive_members.items())),
        "files": dict(sorted(staged_files.items())),
    }
    manifest_bytes = (json.dumps(digest_manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")
    (stage_root / "compose_manifest.json").write_bytes(manifest_bytes)
    digest_manifest["manifest_digest"] = compute_digests(manifest_bytes)
    return digest_manifest
