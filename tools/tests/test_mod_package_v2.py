#!/usr/bin/env python3
"""Unit tests for .eodmod Schema v2 packaging, inspection, composition, and security."""

from __future__ import annotations

import hashlib
import json
import tempfile
import unittest
import zipfile
from pathlib import Path

from mod_package_v2 import (
    ModPackageError,
    ModPackageIntegrityError,
    ModPackageSecurityError,
    SCHEMA_V2,
    compose_eodmod_v2_stack,
    inspect_eodmod_v2,
    pack_eodmod_v2,
)


class TestModPackageV2(unittest.TestCase):
    def test_pack_and_inspect_all_operation_types(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            pkg_path = root / "cup24_pack.eodmod"

            car_bytes = b"CAR\x00CUP24_MESH_PAYLOAD"
            col_bytes = b"EODC\x01CUSTOM_TRACK_COLLISION"
            member_bytes = b"MEMBER_VAL_DATA_FOR_CT_RES"
            htx_bytes = b"EODHTX\x00\x014K_TEXTURE_BYTES"

            manifest = {
                "schema": SCHEMA_V2,
                "id": "cup24_full",
                "name": "Cup 24 Full Pack",
                "version": "2.1.0",
                "operations": [
                    {
                        "op": "addArchive",
                        "target": "DATA/CAR/CUP24.CAR",
                        "blob": "assets/CUP24.CAR",
                    },
                    {
                        "op": "addArchive",
                        "target": "DATA/TRACKS/CUSTOM.eodcol",
                        "blob": "assets/CUSTOM.eodcol",
                    },
                    {
                        "op": "addMember",
                        "archive": "CT.RES",
                        "member": "CUP24.VAL",
                        "blob": "members/CUP24.VAL",
                    },
                    {
                        "op": "valtPatch",
                        "table": "carclass.val",
                        "patches": {
                            "CLASS_4": {
                                "CUP24": {"price": 15000, "top_speed": 185}
                            }
                        },
                    },
                    {
                        "op": "hostTexture",
                        "target": "textures/cup24_albedo.eodhtx",
                        "blob": "textures/cup24_albedo.eodhtx",
                        "width": 4096,
                        "height": 4096,
                    },
                ],
            }
            blobs = {
                "assets/CUP24.CAR": car_bytes,
                "assets/CUSTOM.eodcol": col_bytes,
                "members/CUP24.VAL": member_bytes,
                "textures/cup24_albedo.eodhtx": htx_bytes,
            }

            out = pack_eodmod_v2(manifest, blobs, pkg_path)
            self.assertEqual(out, pkg_path)
            self.assertTrue(pkg_path.is_file())

            info = inspect_eodmod_v2(pkg_path)
            self.assertTrue(info["verified"])
            self.assertEqual(info["schema"], SCHEMA_V2)
            self.assertEqual(info["id"], "cup24_full")
            self.assertEqual(info["total_operations"], 5)
            self.assertEqual(info["counts"]["addArchive"], 2)
            self.assertEqual(info["counts"]["addMember"], 1)
            self.assertEqual(info["counts"]["valtPatch"], 1)
            self.assertEqual(info["counts"]["hostTexture"], 1)

            # Content-addressed SHA-256 + BLAKE2b digests match exact payloads.
            car_meta = info["blobs"]["assets/CUP24.CAR"]
            self.assertEqual(car_meta["sha256"], hashlib.sha256(car_bytes).hexdigest())
            self.assertEqual(car_meta["blake2b"], hashlib.blake2b(car_bytes).hexdigest())
            self.assertEqual(car_meta["size"], len(car_bytes))
            self.assertEqual(car_meta["zip_entry"], f"blobs/{car_meta['sha256']}")

            htx_meta = info["blobs"]["textures/cup24_albedo.eodhtx"]
            self.assertEqual(htx_meta["sha256"], hashlib.sha256(htx_bytes).hexdigest())
            self.assertEqual(htx_meta["blake2b"], hashlib.blake2b(htx_bytes).hexdigest())

    def test_compose_two_non_conflicting_valt_patch_mods(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            pkg_a = root / "mod_nascar.eodmod"
            pkg_b = root / "mod_oval.eodmod"
            stage_dir = root / "stage"

            pack_eodmod_v2(
                {
                    "id": "nascar_cup24",
                    "name": "NASCAR Cup 24",
                    "version": "1.0.0",
                    "operations": [
                        {
                            "op": "addArchive",
                            "target": "DATA/CAR/CUP24.CAR",
                            "blob": "CUP24.CAR",
                        },
                        {
                            "op": "addMember",
                            "archive": "UICAR.RES",
                            "member": "CUP24_ICON.BIN",
                            "blob": "CUP24_ICON.BIN",
                        },
                        {
                            "op": "valtPatch",
                            "table": "carclass.val",
                            "patches": {
                                "CLASS_4": {
                                    "CUP24": {"name": "Cup 24", "class": 4}
                                }
                            },
                        },
                        {
                            "op": "valtPatch",
                            "table": "car_stats.val",
                            "patches": {
                                "CUP24": {"hp": 750, "weight": 3400}
                            },
                        },
                    ],
                },
                {
                    "CUP24.CAR": b"CUP24_ARCHIVE",
                    "CUP24_ICON.BIN": b"ICON_DATA",
                },
                pkg_a,
            )

            pack_eodmod_v2(
                {
                    "id": "late_model_dirt",
                    "name": "Late Model & Dirt Oval",
                    "version": "1.0.0",
                    "operations": [
                        {
                            "op": "addArchive",
                            "target": "DATA/TRACKS/CUSTOM.TRK",
                            "blob": "CUSTOM.TRK",
                        },
                        {
                            "op": "hostTexture",
                            "target": "textures/oval_clay.eodhtx",
                            "blob": "oval_clay.eodhtx",
                        },
                        {
                            "op": "valtPatch",
                            "table": "carclass.val",
                            "patches": {
                                "CLASS_4": {
                                    "LATEMODEL": {"name": "Late Model", "class": 4}
                                }
                            },
                        },
                        {
                            "op": "valtPatch",
                            "table": "tracks.val",
                            "patches": {
                                "CUSTOM_OVAL": {"laps": 25, "surface": "clay"}
                            },
                        },
                    ],
                },
                {
                    "CUSTOM.TRK": b"CUSTOM_TRACK_DATA",
                    "oval_clay.eodhtx": b"EODHTX_CLAY_8K",
                },
                pkg_b,
            )

            composed = compose_eodmod_v2_stack([pkg_a, pkg_b], stage_dir)

            # Exact counts and zero conflicts across both mods patching carclass.val!
            self.assertEqual(composed["counts"]["packages"], 2)
            self.assertEqual(composed["counts"]["addArchive"], 2)
            self.assertEqual(composed["counts"]["addMember"], 1)
            self.assertEqual(composed["counts"]["valtPatch"], 4)
            self.assertEqual(composed["counts"]["hostTexture"], 1)
            self.assertEqual(composed["counts"]["total_operations"], 8)
            self.assertEqual(composed["valt_conflicts"], [])

            # Both mods' semantic entries survive cleanly in stage_dir/valt_patches.json.
            valt_json = json.loads((stage_dir / "valt_patches.json").read_text("utf-8"))
            self.assertEqual(valt_json["conflicts"], [])
            class4 = valt_json["tables"]["carclass.val"]["CLASS_4"]
            self.assertIn("CUP24", class4)
            self.assertIn("LATEMODEL", class4)
            self.assertEqual(valt_json["tables"]["car_stats.val"]["CUP24"]["hp"], 750)
            self.assertEqual(valt_json["tables"]["tracks.val"]["CUSTOM_OVAL"]["laps"], 25)

            # Archives, members, and host textures staged cleanly.
            self.assertEqual((stage_dir / "DATA/CAR/CUP24.CAR").read_bytes(), b"CUP24_ARCHIVE")
            self.assertEqual((stage_dir / "DATA/TRACKS/CUSTOM.TRK").read_bytes(), b"CUSTOM_TRACK_DATA")
            self.assertEqual((stage_dir / "textures/oval_clay.eodhtx").read_bytes(), b"EODHTX_CLAY_8K")
            self.assertEqual(
                (stage_dir / "archive_members/UICAR.RES/CUP24_ICON.BIN").read_bytes(),
                b"ICON_DATA",
            )
            self.assertTrue((stage_dir / "compose_manifest.json").is_file())
            self.assertIn("sha256", composed["manifest_digest"])
            self.assertIn("blake2b", composed["manifest_digest"])

    def test_security_rejects_path_traversal_on_pack_and_inspect(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)

            # Reject '..' in addArchive target
            with self.assertRaises(ModPackageSecurityError):
                pack_eodmod_v2(
                    {
                        "id": "bad_traversal",
                        "name": "Bad Traversal",
                        "version": "1.0.0",
                        "operations": [
                            {"op": "addArchive", "target": "../evil.car", "blob": "evil.car"}
                        ],
                    },
                    {"evil.car": b"nope"},
                    root / "evil1.eodmod",
                )

            # Reject absolute path in target
            with self.assertRaises(ModPackageSecurityError):
                pack_eodmod_v2(
                    {
                        "id": "bad_abs",
                        "name": "Bad Abs",
                        "version": "1.0.0",
                        "operations": [
                            {"op": "addArchive", "target": "/etc/passwd", "blob": "ok.car"}
                        ],
                    },
                    {"ok.car": b"nope"},
                    root / "evil2.eodmod",
                )

            # Reject handcrafted malicious ZIP containing ../evil entry during inspect & compose
            malicious_zip = root / "malicious.eodmod"
            with zipfile.ZipFile(malicious_zip, "w") as zf:
                zf.writestr(
                    "manifest.json",
                    json.dumps(
                        {
                            "schema": SCHEMA_V2,
                            "id": "crafty",
                            "name": "Crafty",
                            "version": "1.0.0",
                            "operations": [],
                            "blobs": {},
                        }
                    ),
                )
                zf.writestr("../evil.txt", b"pwned")

            with self.assertRaises(ModPackageSecurityError):
                inspect_eodmod_v2(malicious_zip)
            with self.assertRaises(ModPackageSecurityError):
                compose_eodmod_v2_stack([malicious_zip], root / "stage")

    def test_security_rejects_executable_payloads(self) -> None:
        forbidden = [
            "payload.exe",
            "DATA/CAR/inject.dll",
            "libhack.dylib",
            "libhack.so",
            "scripts/run.sh",
            "install.bat",
            "hook.ps1",
            "plugin.py",
        ]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for idx, bad_name in enumerate(forbidden):
                with self.subTest(bad_name=bad_name):
                    with self.assertRaises(ModPackageSecurityError):
                        pack_eodmod_v2(
                            {
                                "id": f"bad_exec_{idx}",
                                "name": "Bad Exec",
                                "version": "1.0.0",
                                "operations": [
                                    {
                                        "op": "addArchive",
                                        "target": bad_name,
                                        "blob": bad_name,
                                    }
                                ],
                            },
                            {bad_name: b"#!/bin/sh\necho nope\n"},
                            root / f"bad_{idx}.eodmod",
                        )

            # Handcrafted ZIP smuggling a .dll or .sh entry must also be rejected on inspect
            for bad_entry in ("bin/helper.dll", "scripts/hook.sh"):
                evil_zip = root / f"crafted_{Path(bad_entry).name}.eodmod"
                with zipfile.ZipFile(evil_zip, "w") as zf:
                    zf.writestr(
                        "manifest.json",
                        json.dumps(
                            {
                                "schema": SCHEMA_V2,
                                "id": "smuggler",
                                "name": "Smuggler",
                                "version": "1.0.0",
                                "operations": [],
                                "blobs": {},
                            }
                        ),
                    )
                    zf.writestr(bad_entry, b"MZ_OR_SH")
                with self.assertRaises(ModPackageSecurityError):
                    inspect_eodmod_v2(evil_zip)

    def test_integrity_detects_tampered_blob(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            pkg = root / "tamper.eodmod"
            pack_eodmod_v2(
                {
                    "id": "tamper_test",
                    "name": "Tamper Test",
                    "version": "1.0.0",
                    "operations": [
                        {
                            "op": "addArchive",
                            "target": "DATA/CAR/TEST.CAR",
                            "blob": "TEST.CAR",
                        }
                    ],
                },
                {"TEST.CAR": b"ORIGINAL_BYTES"},
                pkg,
            )

            tampered = root / "tampered.eodmod"
            with zipfile.ZipFile(pkg, "r") as src, zipfile.ZipFile(tampered, "w") as dst:
                for name in src.namelist():
                    data = src.read(name)
                    if name.startswith("blobs/"):
                        data = b"CORRUPTED_BYTE"
                    dst.writestr(name, data)

            with self.assertRaises(ModPackageIntegrityError):
                inspect_eodmod_v2(tampered)

            # Also verify non-.eodhtx rejected for hostTexture
            with self.assertRaises(ModPackageError):
                pack_eodmod_v2(
                    {
                        "id": "bad_tex",
                        "name": "Bad Tex",
                        "version": "1.0.0",
                        "operations": [
                            {
                                "op": "hostTexture",
                                "target": "textures/albedo.png",
                                "blob": "albedo.png",
                            }
                        ],
                    },
                    {"albedo.png": b"PNG"},
                    root / "bad_tex.eodmod",
                )


if __name__ == "__main__":
    unittest.main()
