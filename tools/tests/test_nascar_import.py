#!/usr/bin/env python3
"""Portable regressions, optional retail fixtures and opt-in NASCAR runtime smoke.

Binary/package checks do not establish mesh, deformation, handling, Career,
AI or track gameplay acceptance. Runtime execution requires explicit opt-in.
"""

import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools"))

from rst0 import unpack_archive, pack_archive, eod_crc
from valt import parse_binary_valt, compile_binary_valt
from tex import compile_tex, decompile_tex
from modl import parse_modl, retexture_modl, export_obj, export_json
from trk import decompile_ild, compile_ild, decompile_nascar_ild, convert_nascar_ild_to_eod, export_ild_to_obj
from import_nascar import create_nascar_livery, create_uicar_thumbnail, CARS_CONFIG, validate_inputs, build_nascar_mod, compile_eod_table, set_archive_entry

NASCAR_DISC = Path(os.environ.get("NASCAR_DISC_DATA", "/Volumes/SLUS_20441/DATA"))
EOD_DISC = Path(os.environ.get("EOD_DISC_DATA", str(Path.home() / "Library/Application Support/TDEODRecomp/disc/DATA")))
EOD_BIN = REPO_ROOT / "build-mac" / "eod"
MODS_DIR = Path(os.environ.get("NASCAR_MOD_DIR", str(REPO_ROOT / "mods" / "nascar")))


class TestNascarImportPipeline(unittest.TestCase):
    @unittest.skipUnless(NASCAR_DISC.is_dir(), "Owner-supplied NASCAR disc data unavailable")
    def test_01_rst0_v2_unpacking(self):
        """Verify 0TSR v2 archive unpacking on NASCAR car and track archives."""
        stock_car = NASCAR_DISC / "STOCK.CAR"
        self.assertTrue(stock_car.exists(), "STOCK.CAR must exist on NASCAR disc")

        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            unpacked_dir = tmp / "unpacked_stock"
            unpack_archive(str(stock_car), str(unpacked_dir))

            manifest_file = unpacked_dir / "manifest.json"
            self.assertTrue(manifest_file.exists())
            with open(manifest_file, "r") as f:
                manifest = json.load(f)

            self.assertEqual(manifest["version"], 2)
            self.assertEqual(len(manifest["entries"]), 27)

            # Ensure all key assets were extracted with non-zero size
            for expected in ["physics.val", "bone.val", "Low0.mod", "Low1.mod", "Cockpit.tex"]:
                fpath = unpacked_dir / expected
                self.assertTrue(fpath.exists(), f"Expected {expected} in unpacked STOCK.CAR")
                self.assertGreater(fpath.stat().st_size, 0)

    @unittest.skipUnless(NASCAR_DISC.is_dir(), "Owner-supplied NASCAR disc data unavailable")
    def test_02_valt_v6_xor_decryption(self):
        """Verify VALT v6 parsing and XOR 0xA5 decryption of physics.val and bone.val."""
        stock_car = NASCAR_DISC / "STOCK.CAR"
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            unpack_archive(str(stock_car), str(tmp))

            phys_file = tmp / "physics.val"
            self.assertTrue(phys_file.exists())
            phys_model = parse_binary_valt(phys_file.read_bytes())

            self.assertTrue(phys_model["is_v6"])
            self.assertEqual(len(phys_model["sections"]), 7)

            sec_names = [s["name"] for s in phys_model["sections"]]
            self.assertIn("Chassis", sec_names)
            self.assertIn("EngineNormal", sec_names)

            chassis_sec = next(s for s in phys_model["sections"] if s["name"] == "Chassis")
            chassis_pairs = dict(chassis_sec["pairs"])

            # Verify decrypted strings
            self.assertIn("mass", chassis_pairs)
            self.assertIn("drag", chassis_pairs)
            self.assertEqual(chassis_pairs["mass"], "3500")
            self.assertEqual(chassis_pairs["drag"], "0.30")

            # Verify round-trip compilation to VALT v11 format
            v11_bytes = compile_eod_table(phys_model)
            v11_parsed = parse_binary_valt(v11_bytes)
            self.assertFalse(v11_parsed["is_v6"])
            self.assertEqual(len(v11_parsed["sections"]), 7)
            self.assertEqual([(s["name"], s["pairs"]) for s in v11_parsed["sections"]],
                             [(s["name"], s["pairs"]) for s in phys_model["sections"]])
            self.assertEqual(v11_parsed["tool_vtable"], 0x40C218)
            self.assertTrue(phys_model["is_v6"], "Conversion must not mutate the source model")

    @unittest.skipUnless(NASCAR_DISC.is_dir(), "Owner-supplied NASCAR disc data unavailable")
    def test_03_modl_damage_hierarchy_and_obj(self):
        """Inspect NASCAR MODL metadata and bounds-only OBJ export; deformation remains unverified."""
        stock_car = NASCAR_DISC / "STOCK.CAR"
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            unpack_archive(str(stock_car), str(tmp))

            low0_file = tmp / "Low0.mod"
            self.assertTrue(low0_file.exists())

            m = parse_modl(low0_file.read_bytes())
            self.assertGreaterEqual(m["sub_version"], 9)
            valid_parts = [p for p in m["parts"] if p["valid"]]
            self.assertGreaterEqual(len(valid_parts), 40)

            # Test Wavefront OBJ and JSON export
            obj_path = tmp / "stock_low0.obj"
            json_path = tmp / "stock_low0.json"
            export_obj(str(low0_file), str(obj_path))
            export_json(str(low0_file), str(json_path))
            self.assertTrue(obj_path.exists())
            self.assertTrue(json_path.exists())
            self.assertGreater(obj_path.stat().st_size, 1000)

            # Verify retexturing tool
            retextured = retexture_modl(low0_file.read_bytes(), "Stock1.tex", "Nascar.tex")
            m_ret = parse_modl(retextured)
            p1 = next(p for p in m_ret["parts"] if p["valid"] and p["texture"] == "Nascar.tex")
            self.assertIsNotNone(p1)

    def test_04_livery_and_texture_compilation(self):
        """Verify custom NASCAR livery generation and PS2 GS PSMT8 texture compilation."""
        livery_rgba = create_nascar_livery(256, 128, "stock", "424")
        self.assertEqual(len(livery_rgba), 256 * 128 * 4)

        tex_bytes = compile_tex(256, 128, livery_rgba)
        self.assertGreater(len(tex_bytes), 0x50)

        w_out, h_out, rgba_out, meta = decompile_tex(tex_bytes)
        self.assertEqual(w_out, 256)
        self.assertEqual(h_out, 128)

        # Verify UI thumbnail compilation
        ui16_rgba = create_uicar_thumbnail(16, 16, "stock")
        ui_tex = compile_tex(16, 16, ui16_rgba, fmt=0x201, flags=0x320600)
        self.assertGreater(len(ui_tex), 0x50)

    @unittest.skipUnless(MODS_DIR.is_dir(), "Build a candidate NASCAR package first")
    def test_05_mod_package_integrity(self):
        """Verify all 6 imported NASCAR vehicles and overlay RES archives in mods/nascar/."""
        self.assertTrue(MODS_DIR.exists(), f"mods/nascar directory must exist at {MODS_DIR}")

        expected_cars = ["NASCAR.CAR", "NTRUCK.CAR", "MODIFIED.CAR", "DIRTCAR.CAR", "HOBBY.CAR", "PACECAR.CAR"]
        for car in expected_cars:
            car_path = MODS_DIR / "DATA" / "CAR" / car
            self.assertTrue(car_path.exists(), f"{car} must exist under mods/nascar/DATA/CAR/")
            self.assertGreater(car_path.stat().st_size, 100000)

        # Verify COMMON.RES contents
        common_path = MODS_DIR / "DATA" / "RES" / "COMMON.RES"
        self.assertTrue(common_path.exists())

        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            unpack_archive(str(common_path), str(tmp))

            # Check carclass.val: tournament registration
            cc_model = parse_binary_valt((tmp / "carclass.val").read_bytes())
            classes_dict = {s["name"]: [k for k, _ in s["pairs"]] for s in cc_model["sections"]}
            for cfg in CARS_CONFIG:
                chassis = cfg["chassis"]
                for cls_name in cfg["classes"]:
                    self.assertIn(cls_name, classes_dict)
                    self.assertTrue(
                        any(c.lower() == chassis.lower() for c in classes_dict[cls_name]),
                        f"Chassis {chassis} must be registered in {cls_name}",
                    )

            # Check car_stats.val: accessible section count invariant (unk1 == sec_count)
            cs_data = (tmp / "car_stats.val").read_bytes()
            cs_model = parse_binary_valt(cs_data)
            self.assertEqual(
                cs_model["unk1"],
                len(cs_model["sections"]),
                "car_stats.val accessible section count (unk1) must equal len(sections)",
            )

            # Check buycar.val & dirt.val: racing number uniqueness invariant
            bc_model = parse_binary_valt((tmp / "buycar.val").read_bytes())
            dirt_model = parse_binary_valt((tmp / "dirt.val").read_bytes())

            used_numbers = []
            for s in bc_model["sections"]:
                for k, v in s["pairs"]:
                    if k == "number":
                        used_numbers.append(int(v))
            for s in dirt_model["sections"]:
                for k, v in s["pairs"]:
                    if k == "number":
                        used_numbers.append(int(v))

            self.assertEqual(
                len(used_numbers),
                len(set(used_numbers)),
                f"All vehicle/driver racing numbers must be strictly unique! Duplicates found: {set([x for x in used_numbers if used_numbers.count(x) > 1])}",
            )

            # Verify CT.RES and UICAR.RES contain all 6 car numbers
            ct_staging = tmp / "ct"
            unpack_archive(str(MODS_DIR / "DATA" / "RES" / "CT.RES"), str(ct_staging))
            uicar_staging = tmp / "uicar"
            unpack_archive(str(MODS_DIR / "DATA" / "RES" / "UICAR.RES"), str(uicar_staging))

            for cfg in CARS_CONFIG:
                num = cfg["dealer_number"]
                self.assertTrue((ct_staging / f"{num}.tex").exists(), f"{num}.tex missing in CT.RES")
                self.assertTrue((ct_staging / f"{num}.tm0").exists(), f"{num}.tm0 missing in CT.RES")
                self.assertTrue((uicar_staging / f"uicar_{num}.tex").exists(), f"uicar_{num}.tex missing in UICAR.RES")
                self.assertTrue((uicar_staging / f"uicar_{num}.tm0").exists(), f"uicar_{num}.tm0 missing in UICAR.RES")

    @unittest.skipUnless(NASCAR_DISC.is_dir(), "Owner-supplied NASCAR disc data unavailable")
    def test_06_nascar_spline_conversion(self):
        """Reject unsupported actual Daytona layout instead of silently truncating records."""
        daytona_trk = NASCAR_DISC / "TRACKS2" / "DAYTONA.TRK"
        if not daytona_trk.exists():
            daytona_trk = NASCAR_DISC / "TRACKS" / "HASTINGS.TRK"
        self.assertTrue(daytona_trk.exists())

        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            unpack_archive(str(daytona_trk), str(tmp))

            track_ild = tmp / "track.ild"
            self.assertTrue(track_ild.exists())

            # This real Daytona source has 68 trailing bytes beyond the old
            # guessed record extent. Do not silently truncate an unknown layout.
            raw = track_ild.read_bytes()
            self.assertEqual(struct.unpack_from("<I", raw, 8)[0], 76)
            with self.assertRaises(ValueError):
                decompile_nascar_ild(raw)

    @unittest.skipUnless(os.environ.get("EOD_RUN_NASCAR_GAME") == "1", "Opt in with EOD_RUN_NASCAR_GAME=1; coordinate game workloads")
    def test_07_eod_headless_mod_mount_and_carousel(self):
        """Run headless EOD binary with NASCAR mod overlay and verify vehicle initialization and carousel navigation."""
        self.assertTrue(EOD_BIN.exists(), f"eod binary not found at {EOD_BIN}")

        ps_cmd = [
            sys.executable,
            str(REPO_ROOT / "tools" / "padscript.py"),
            "400:start", "1500:start", "1800:x", "2100:x", "2450:x",
            "2700:right", "2850:right", "3000:right", "3150:right",
        ]
        pad_script = subprocess.check_output(ps_cmd, text=True).strip()

        env = os.environ.copy()
        env["EOD_HEADLESS"] = "1"
        env["EOD_MUTE"] = "1"
        env["EOD_LOG"] = "file"
        env["EOD_MOD_DIR"] = str(MODS_DIR)
        env["EOD_PAD_SCRIPT"] = pad_script
        memcard_dir = tempfile.TemporaryDirectory(prefix="eod-nascar-memcard-")
        self.addCleanup(memcard_dir.cleanup)
        env["EOD_MEMCARD"] = str(Path(memcard_dir.name) / "memcard")

        cmd = [
            str(EOD_BIN),
            "--headless",
            "--data", str(EOD_DISC.parent),
            "--frames", "3300",
        ]

        proc = subprocess.run(
            cmd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=90,
        )

        self.assertEqual(
            proc.returncode, 0, f"Headless engine execution crashed with code {proc.returncode}:\n{proc.stdout[-500:]}"
        )

        out = proc.stdout
        # Assert virtual disc TOC injected custom NASCAR car archives
        self.assertIn("stream: overlay file added: NASCAR.CAR", out)
        self.assertIn("stream: overlay file added: NTRUCK.CAR", out)
        self.assertIn("stream: overlay file added: MODIFIED.CAR", out)
        self.assertIn(f"{MODS_DIR}/DATA/RES/COMMON.RES replaces", out)
        self.assertIn(f"{MODS_DIR}/DATA/RES/CT.RES replaces", out)
        self.assertIn(f"{MODS_DIR}/DATA/RES/UICAR.RES replaces", out)

        # Assert no table panics or duplicate number crashes
        self.assertNotIn("CarStatsLookup: no such car as", out)
        self.assertNotIn("have the same number", out)
        self.assertNotIn("FATAL: game panic", out)



class TestNascarPortable(unittest.TestCase):
    def test_distinct_vehicle_numbers(self):
        numbers = [cfg[key] for cfg in CARS_CONFIG for key in ("dealer_number", "driver_number")]
        self.assertEqual(len(numbers), len(set(numbers)))
        self.assertEqual(len({cfg["chassis"].lower() for cfg in CARS_CONFIG}), 6)

    def test_missing_disc_inputs_do_not_create_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            out = root / "candidate"
            with self.assertRaises(FileNotFoundError):
                build_nascar_mod(root / "nascar", root / "eod", out)
            self.assertFalse(out.exists())

    def test_output_cannot_overlap_either_disc_tree(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for out in (root, root / "nascar", root / "nascar" / "mods", root / "eod" / "mods"):
                with self.subTest(out=out), self.assertRaises(ValueError):
                    validate_inputs(root / "nascar", root / "eod", out)

    def test_texture_entry_replacement_is_case_insensitive(self):
        manifest = {"entries": [{"name": "hobby.tex", "version": 1}]}
        set_archive_entry(manifest, {"name": "Hobby.tex", "version": 10})
        self.assertEqual(manifest["entries"], [{"name": "Hobby.tex", "version": 10}])

    def test_failed_build_does_not_publish_partial_overlay(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            nascar, eod, out = root / "nascar", root / "eod", root / "output"
            nascar.mkdir()
            (eod / "RES").mkdir(parents=True)
            for cfg in CARS_CONFIG:
                (nascar / cfg["source_archive"]).touch()
            for name in ("COMMON.RES", "CT.RES", "UICAR.RES"):
                (eod / "RES" / name).touch()
            with patch("import_nascar._build_nascar_mod", side_effect=ValueError("fixture failure")):
                with self.assertRaisesRegex(ValueError, "fixture failure"):
                    build_nascar_mod(nascar, eod, out)
            self.assertFalse(out.exists())
            self.assertEqual(sorted(path.name for path in root.iterdir()), ["eod", "nascar"])

    def test_ild_serialization_preserves_unknown_header(self):
        model = {"unknown0": 123, "pointer_count": 7, "node_size": 60,
                 "nodes": [[float(i) for i in range(15)]], "pointers": [0]}
        raw = compile_ild(model)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "fixture.ild"
            path.write_bytes(raw)
            decoded = decompile_ild(str(path))
        self.assertEqual(compile_ild(decoded), raw)

    def test_ild_obj_uses_position_not_tangent(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "line.obj"
            export_ild_to_obj({"nodes": [[10., 20., 0.5, -0.5] + [0.] * 11]}, str(path))
            self.assertIn("v 10.0000 0.0000 20.0000", path.read_text())

    def test_ild_opaque_pointer_bits_survive(self):
        raw_node = struct.pack("<15I", 0x7fa12345, *([0] * 14))
        raw = compile_ild({"node_size": 60, "nodes": [[0.] * 15]})
        raw = raw[:48] + raw_node
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "opaque.ild"
            path.write_bytes(raw)
            model = decompile_ild(str(path))
        self.assertEqual(compile_ild(model), raw)

    def test_malformed_nascar_records_are_rejected(self):
        for size, offset, payload in ((0, 12, b""), (60, 12, b""), (76, 1000, b""), (76, 16, b"x")):
            raw = struct.pack("<III", 0, offset, size) + b"\0" * 4 + payload
            with self.subTest(size=size, offset=offset), self.assertRaises(ValueError):
                decompile_nascar_ild(raw)

    def test_all_liveries_round_trip_pixels(self):
        for cfg in CARS_CONFIG:
            with self.subTest(chassis=cfg["chassis"]):
                pixels = create_nascar_livery(256, 128, cfg["livery_type"], cfg["dealer_number"])
                w, h, decoded, _ = decompile_tex(compile_tex(256, 128, pixels))
                self.assertEqual((w, h), (256, 128))
                # PS2 alpha is quantized; compare color channels exactly.
                self.assertEqual(decoded[0::4], pixels[0::4])
                self.assertEqual(decoded[1::4], pixels[1::4])
                self.assertEqual(decoded[2::4], pixels[2::4])

if __name__ == "__main__":
    unittest.main()
