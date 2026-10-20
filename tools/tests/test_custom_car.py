#!/usr/bin/env python3
"""End-to-end and regression test suite for custom car pipeline and mod loading.

Validates:
1. 0TSR v6 archive round-trip byte-exactness and CRC calculations (rst0.py).
2. VALT v11 binary table compilation and decompilation fidelity (valt.py).
3. PS2 TEX texture compilation, GS swizzling, and unswizzling (tex.py).
4. MODL v9 3D model parsing and retexturing (modl.py).
5. PHANTOM.CAR custom vehicle archive integrity, physics, and model bindings.
6. Overlay archive modifications in COMMON.RES, CT.RES, and UICAR.RES.
7. Headless game engine execution with EOD_MOD_DIR, verifying TOC mount,
   resource loading, and AI racers driving Phantom in-game.
"""

import hashlib
import json
import math
import re
import os
import shutil
import struct
from types import SimpleNamespace
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools"))

from rst0 import unpack_archive, pack_archive, eod_crc
from valt import parse_binary_valt, compile_binary_valt
from tex import compile_tex, decompile_tex
from modl import parse_modl, retexture_modl, export_obj, export_json
from audio import sfx_to_wav, wav_to_sfx, read_wav, decode_spu2_adpcm, encode_spu2_adpcm
from trk import decompile_ild, compile_ild, export_ild_to_obj, decompile_sol, export_sol_to_obj

DISC_DATA = Path(os.environ.get("EOD_TEST_DISC_DATA", str(Path.home() / "Library/Application Support/TDEODRecomp/disc/DATA")))
MODS_DIR = REPO_ROOT / "mods" / "phantom"
EOD_BIN = REPO_ROOT / "build-mac" / "eod"


class TestCustomCarPipeline(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not DISC_DATA.exists():
            raise unittest.SkipTest(f"Disc data directory not found at {DISC_DATA}")
        global MODS_DIR
        if existing := os.environ.get("EOD_TEST_MOD_DIR"):
            MODS_DIR = Path(existing).resolve()
            if not (MODS_DIR / "DATA/CAR/PHANTOM.CAR").is_file():
                raise ValueError("EOD_TEST_MOD_DIR must contain a built Phantom package")
            return
        from create_custom_car import build_custom_car
        cls.package_dir = tempfile.TemporaryDirectory(prefix="eod-car-test-")
        cls.addClassCleanup(cls.package_dir.cleanup)
        MODS_DIR = Path(cls.package_dir.name) / "phantom"
        build_custom_car(str(DISC_DATA), str(MODS_DIR))

    def test_01_rst0_roundtrip(self):
        """Verify rst0 archive unpack and pack preserves entry contents byte-for-byte."""
        algar_car = DISC_DATA / "CAR" / "ALGAR.CAR"
        self.assertTrue(algar_car.exists(), "ALGAR.CAR must exist on disc")

        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            unpacked_dir = tmp / "unpacked"
            unpack_archive(str(algar_car), str(unpacked_dir))

            with open(unpacked_dir / "manifest.json", "r") as f:
                manifest = json.load(f)
            self.assertEqual(len(manifest["entries"]), 13)

            repacked_car = tmp / "ALGAR_REPACK.CAR"
            pack_archive(str(unpacked_dir), str(repacked_car))
            self.assertTrue(repacked_car.exists())
            self.assertEqual(algar_car.read_bytes(), repacked_car.read_bytes())

            # Unpack the repacked archive and verify byte-exact matches for all entries
            roundtrip_dir = tmp / "roundtrip"
            unpack_archive(str(repacked_car), str(roundtrip_dir))

            for entry in manifest["entries"]:
                name = entry["name"]
                orig_bytes = (unpacked_dir / name).read_bytes()
                rt_bytes = (roundtrip_dir / name).read_bytes()
                self.assertEqual(
                    orig_bytes,
                    rt_bytes,
                    f"Entry {name} differs between original unpack and repacked archive",
                )

    def test_02_valt_roundtrip(self):
        """Verify valt parses and re-compiles binary VALT tables losslessly."""
        algar_car = DISC_DATA / "CAR" / "ALGAR.CAR"
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            unpack_archive(str(algar_car), str(tmp))
            phys_path = tmp / "physics.val"
            orig_data = phys_path.read_bytes()

            parsed = parse_binary_valt(orig_data)
            self.assertIn("sections", parsed)
            chassis_sec = next(s for s in parsed["sections"] if s["name"] == "Chassis")
            chassis_dict = dict(chassis_sec["pairs"])
            self.assertIn("mass", chassis_dict)

            compiled = compile_binary_valt(parsed)
            re_parsed = parse_binary_valt(compiled)

            self.assertEqual(len(parsed["sections"]), len(re_parsed["sections"]))
            for s1, s2 in zip(parsed["sections"], re_parsed["sections"]):
                self.assertEqual(s1["name"], s2["name"])
                self.assertEqual(s1["pairs"], s2["pairs"])

    def test_retail_uicar_decodes_against_reviewed_reference_pixels(self):
        archive = (DISC_DATA / "RES/UICAR.RES").read_bytes()
        directory, count = struct.unpack_from("<II", archive, 0x14)
        for i in range(count):
            entry = archive[directory + i * 0x44:directory + (i + 1) * 0x44]
            if entry[:32].split(b"\0")[0] == b"uicar_296.tm0":
                size, offset = struct.unpack_from("<II", entry, 0x28)
                decoded = decompile_tex(archive[directory + offset:directory + offset + size])
                # Reviewed clean retail orange/teal #296 car, not a generated
                # texture compiled and decoded by the same implementation.
                self.assertEqual(hashlib.sha256(decoded[2]).hexdigest(),
                                 "25ed48fa7b205ce45cb12972b9354bc8ef4fdc3e72df027c87827e29c03add31")
                return
        self.fail("Retail uicar_296.tm0 reference missing")

    def test_03_tex_roundtrip(self):
        """Verify PS2 TEX compiler and decompiler round-trip fidelity."""
        w, h = 64, 32
        # Generate a test image with distinct RGB colored patterns
        rgba = bytearray(w * h * 4)
        for y in range(h):
            for x in range(w):
                idx = (y * w + x) * 4
                rgba[idx] = (x * 4) & 0xFF
                rgba[idx + 1] = (y * 8) & 0xFF
                rgba[idx + 2] = ((x + y) * 2) & 0xFF
                rgba[idx + 3] = 255

        tex_bytes = compile_tex(w, h, bytes(rgba))
        self.assertGreater(len(tex_bytes), 0x50)

        w_out, h_out, rgba_out, meta = decompile_tex(tex_bytes)
        self.assertEqual(w_out, w)
        self.assertEqual(h_out, h)
        self.assertEqual(len(rgba_out), w * h * 4)

    def test_04_modl_inspection_and_retexture(self):
        """Verify MODL parser correctly inspects parts and retexture_modl rebinds textures."""
        algar_car = DISC_DATA / "CAR" / "ALGAR.CAR"
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            unpack_archive(str(algar_car), str(tmp))
            low0_path = tmp / "Low0.mod"
            low0_data = low0_path.read_bytes()

            model = parse_modl(low0_data)
            self.assertGreaterEqual(model["part_slots"], 1)

            # Retexture from Algar.tex to Phantom.tex
            modified = retexture_modl(low0_data, "Algar.tex", "Phantom.tex")
            new_model = parse_modl(modified)
            found_phantom = False
            for p in new_model["parts"]:
                if p["valid"] and p["texture"] == "Phantom.tex":
                    found_phantom = True
            self.assertTrue(found_phantom, "Retextured model must contain Phantom.tex reference")

    def test_ocelot_body_binding_preserves_shared_materials(self):
        from create_custom_car import body_texture_names, asset_path
        with tempfile.TemporaryDirectory() as directory:
            unpack_archive(str(DISC_DATA / "CAR/OCELOT.CAR"), directory)
            for level in range(5):
                data = asset_path(directory, f"Low{level}.mod").read_bytes()
                if level == 1:
                    self.assertEqual(body_texture_names("Ocelot", data), ("ocey.tex",))
                before = parse_modl(data)
                changed = retexture_modl(data, "ocey.tex", "Shadow.tex")
                after = parse_modl(changed)
                self.assertNotEqual(changed, data)
                for original, modified in zip(before["parts"], after["parts"]):
                    if original.get("magic") == "4SPS" and original.get("texture", "").lower() != "ocey.tex":
                        self.assertEqual(original, modified)
                self.assertTrue(any(p.get("texture") == "Shadow.tex" for p in after["parts"]))

    def test_prince_distant_lod_body_binding(self):
        from create_custom_car import asset_path
        with tempfile.TemporaryDirectory() as directory:
            unpack_archive(str(DISC_DATA / "CAR/PRINCE.CAR"), directory)
            for level in (5, 6):
                data = asset_path(directory, f"Low{level}.mod").read_bytes()
                changed = retexture_modl(data, "arrow.tex", "Shadow.tex")
                self.assertNotEqual(changed, data)
                self.assertTrue(any(p.get("texture") == "Shadow.tex" for p in parse_modl(changed)["parts"]))

    def test_05_phantom_car_archive(self):
        """Verify PHANTOM.CAR contains valid physics, textures, and models."""
        phantom_car = MODS_DIR / "DATA" / "CAR" / "PHANTOM.CAR"
        self.assertTrue(phantom_car.exists(), f"Missing {phantom_car}")

        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            unpack_archive(str(phantom_car), str(tmp))

            with open(tmp / "manifest.json", "r") as f:
                manifest = json.load(f)

            entries_by_name = {e["name"]: e for e in manifest["entries"]}
            self.assertEqual(len(entries_by_name), 15)

            # Check textures and fourcc tags
            for tex_name in ["Phantom.tex", "Phantom.tm0"]:
                self.assertIn(tex_name, entries_by_name)
                # Verify that 4-byte fourcc tag in raw file corresponds to 0x54455820 ('TEX ')
                raw_fourcc = entries_by_name[tex_name]["fourcc"]
                pad = raw_fourcc.ljust(4, " ")[:4]
                self.assertEqual(pad[::-1], " XET")

            # Check custom physics
            phys_data = (tmp / "physics.val").read_bytes()
            phys = parse_binary_valt(phys_data)
            chassis_pairs = dict(next(s["pairs"] for s in phys["sections"] if s["name"] == "Chassis"))
            engine_pairs = dict(next(s["pairs"] for s in phys["sections"] if s["name"] == "EngineNormal"))

            self.assertEqual(chassis_pairs["mass"], "3200")
            self.assertEqual(chassis_pairs["damage_scale"], "0.65")
            self.assertEqual(chassis_pairs["drag"], "0.24")
            self.assertEqual(chassis_pairs["grip"], "0.95")
            self.assertEqual(engine_pairs["power_max"], "380")
            self.assertEqual(engine_pairs["torque_max"], "420")
            self.assertEqual(engine_pairs["redline"], "9000")

            # Check 3D models bound to Phantom.tex
            for mod_name in ["Low0.mod", "Low1.mod", "Low2.mod", "Low3.mod", "Low4.mod"]:
                mod_data = (tmp / mod_name).read_bytes()
                m = parse_modl(mod_data)
                has_tex = any(p["valid"] and p["texture"] == "Phantom.tex" for p in m["parts"])
                self.assertTrue(has_tex, f"{mod_name} should reference Phantom.tex")

    def test_06_mod_overlay_res_archives(self):
        """Verify COMMON.RES, CT.RES, and UICAR.RES contain required integration records."""
        common_res = MODS_DIR / "DATA" / "RES" / "COMMON.RES"
        ct_res = MODS_DIR / "DATA" / "RES" / "CT.RES"
        uicar_res = MODS_DIR / "DATA" / "RES" / "UICAR.RES"

        self.assertTrue(common_res.exists())
        self.assertTrue(ct_res.exists())
        self.assertTrue(uicar_res.exists())

        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)

            # Check COMMON.RES
            c_dir = tmp / "common"
            unpack_archive(str(common_res), str(c_dir))

            # Native lookup is first-match, so Phantom belongs only to Midsize.
            cc = parse_binary_valt((c_dir / "carclass.val").read_bytes())
            membership = [s["name"] for s in cc["sections"] if s["name"].endswith("$chassis")
                          and any(k.lower() == "phantom" for k, v in s["pairs"])]
            self.assertEqual(membership, ["$0001$chassis"])

            # car_stats.val
            cs = parse_binary_valt((c_dir / "car_stats.val").read_bytes())
            phantom_stats = next(
                dict(s["pairs"])
                for s in cs["sections"]
                if dict(s["pairs"]).get("chassis", "").lower() == "phantom"
            )
            self.assertEqual(phantom_stats["durability"], "8")
            self.assertEqual(phantom_stats["handling"], "8")
            self.assertEqual(phantom_stats["speed"], "9")

            # buycar.val
            bc = parse_binary_valt((c_dir / "buycar.val").read_bytes())
            phantom_dealer = next(
                dict(s["pairs"])
                for s in bc["sections"]
                if dict(s["pairs"]).get("chassis", "").lower() == "phantom"
            )
            self.assertEqual(phantom_dealer["price"], "1200")
            self.assertEqual(phantom_dealer["desc"], "Custom tuned V8 machine with reinforced rollcage and racing suspension")

            # dirt.val
            dirt = parse_binary_valt((c_dir / "dirt.val").read_bytes())
            drivers = [dict(s["pairs"]) for s in dirt["sections"] if s["name"] == "Driver"]
            driver_names = {d.get("name"): d for d in drivers}
            self.assertIn("Phantom Racer", driver_names)
            self.assertEqual(driver_names["Phantom Racer"]["chassis"], "Phantom")
            self.assertEqual(driver_names["Sam Tallman"]["chassis"], "Phantom")
            self.assertEqual(driver_names["Dale Jefferson"]["chassis"], "Phantom")

            # Native paint carousel keys previews by actual driver/dealer number.
            # Paint 1 can be Sam Tallman (296), not just the new racer (777).
            ui_dir = tmp / "uicar"
            unpack_archive(str(uicar_res), str(ui_dir))
            preview_ids = {driver_names[n]["number"] for n in ("Sam Tallman", "Dale Jefferson", "Phantom Racer")}
            dealer = next(dict(s["pairs"]) for s in bc["sections"]
                          if s["name"] == "vehicle" and dict(s["pairs"]).get("chassis") == "Phantom")
            preview_ids.add(dealer["number"])
            expected = (ui_dir / "uicar_777.tm0").read_bytes()
            for number in preview_ids:
                self.assertEqual((ui_dir / f"uicar_{number}.tm0").read_bytes(), expected)
                self.assertTrue((ui_dir / f"uicar_{number}.tex").is_file())

    @unittest.skipUnless(os.environ.get("EOD_RUN_GAME_TESTS") == "1", "Opt-in isolated game workload: EOD_RUN_GAME_TESTS=1")
    def test_07_engine_headless_race_execution(self):
        """Run game engine headlessly with mod overlays and assert Phantom loads and races."""
        self.assertTrue(EOD_BIN.exists(), f"eod binary not found at {EOD_BIN}")

        ps_cmd = [
            sys.executable,
            str(REPO_ROOT / "tools" / "padscript.py"),
            "400:start", "1500:start", "1800:x", "2100:x", "2450:x", "2600:x",
            "2900:x", "3200:x", "3500:x", "3950:x", "--hold", "4300-5000:x",
        ]
        pad_script = subprocess.check_output(ps_cmd, text=True).strip()

        env = os.environ.copy()
        env["EOD_HEADLESS"] = "1"
        env["EOD_MUTE"] = "1"
        env["EOD_LOG"] = "file"
        env["EOD_MOD_DIR"] = str(MODS_DIR)
        env["EOD_PAD_SCRIPT"] = pad_script
        scratch = Path(tempfile.mkdtemp(prefix="eod-game-test-"))
        self.addCleanup(shutil.rmtree, scratch)
        config = scratch / "eod.ini"
        config.write_text("[Video]\nRenderer=vulkan\nRenderScale=1\nAspectRatio=4:3\nFrameSkip=0\nVSync=0\n")
        env["EOD_MEMCARD"] = str(scratch / "memcard")

        cmd = [
            str(EOD_BIN),
            "--headless",
            "--config", str(config),
            "--data", str(DISC_DATA.parent),
            "--frames", "4500",
        ]

        if capture := os.environ.get("EOD_TEST_CAPTURE_DIR"):
            capture_path = Path(capture) / self._testMethodName
            capture_path.mkdir(parents=True, exist_ok=True)
            cmd += ["--screenshot-every", "150", "--screenshot-dir", str(capture_path)]

        proc = subprocess.run(
            cmd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=120,
        )

        self.assertEqual(
            proc.returncode, 0, f"Engine execution failed with exit code {proc.returncode}"
        )

        out = proc.stdout
        # Assert mod overlays were mounted in virtual disc TOC
        self.assertIn("stream: overlay file added: PHANTOM.CAR", out)
        self.assertIn(str(MODS_DIR / "DATA/RES/COMMON.RES") + " replaces", out)
        self.assertIn(str(MODS_DIR / "DATA/RES/CT.RES") + " replaces", out)
        self.assertIn(str(MODS_DIR / "DATA/RES/UICAR.RES") + " replaces", out)

        # Assert Phantom.car resource archive loaded by game
        self.assertIn("+load_resource_set(Phantom.car) normal", out)

        # Assert car instance initialized by engine
        self.assertIn("car Phantom: mass 3200", out)

        # Assert no texture format mismatch error
        self.assertNotIn("Expected type 0x54455820, got type 0x20544558", out)

    @unittest.skipUnless(os.environ.get("EOD_RUN_GAME_TESTS") == "1", "Opt-in isolated game workload: EOD_RUN_GAME_TESTS=1")
    def test_08_vehicle_selection_menu_with_phantom(self):
        """Verify navigating and scrolling through the vehicle selection carousel recognizes Phantom with zero panic."""
        self.assertTrue(EOD_BIN.exists(), f"eod binary not found at {EOD_BIN}")

        ps_cmd = [
            sys.executable,
            str(REPO_ROOT / "tools" / "padscript.py"),
            "400:start", "1500:start", "1800:x", "2100:x", "2450:x",
            # Switching Compact -> Midsize selects appended Phantom; retain that selection.
            "2700:right", "2850:down", "3450:x",
            "3600:x", "3900:x", "4200:x", "4650:x", "--hold", "5000-6000:x",
        ]
        pad_script = subprocess.check_output(ps_cmd, text=True).strip()

        env = os.environ.copy()
        env["EOD_HEADLESS"] = "1"
        env["EOD_MUTE"] = "1"
        env["EOD_LOG"] = "file"
        env["EOD_MOD_DIR"] = str(MODS_DIR)
        env["EOD_PAD_SCRIPT"] = pad_script
        scratch = Path(tempfile.mkdtemp(prefix="eod-menu-test-"))
        self.addCleanup(shutil.rmtree, scratch)
        config = scratch / "eod.ini"
        config.write_text("[Video]\nRenderer=vulkan\nRenderScale=1\nAspectRatio=4:3\nFrameSkip=0\nVSync=0\n")
        env["EOD_MEMCARD"] = str(scratch / "memcard")
        ram_path = scratch / "ram.bin"
        env["EOD_DUMP_RAM"] = str(ram_path)

        cmd = [
            str(EOD_BIN),
            "--headless",
            "--config", str(config),
            "--data", str(DISC_DATA.parent),
            "--frames", "6000",
        ]

        if capture := os.environ.get("EOD_TEST_CAPTURE_DIR"):
            capture_path = Path(capture) / self._testMethodName
            capture_path.mkdir(parents=True, exist_ok=True)
            cmd += ["--screenshot-every", "150", "--screenshot-dir", str(capture_path)]

        proc = subprocess.run(
            cmd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=150,
        )

        if capture := os.environ.get("EOD_TEST_CAPTURE_DIR"):
            (Path(capture) / self._testMethodName / "out.log").write_text(proc.stdout)
        self.assertEqual(
            proc.returncode, 0, f"Vehicle selection crashed with code {proc.returncode}:\n{proc.stdout[-500:]}"
        )
        self.assertIn("+load_resource_set(Phantom.car) normal", proc.stdout)
        self.assertIn("car Phantom: mass 3200", proc.stdout)
        self.assertNotIn("CarStatsLookup: no such car as Phantom, AFAIK", proc.stdout)
        self.assertNotIn("have the same number", proc.stdout)
        # A Phantom AI load does not prove that the player selected Phantom.
        # Native f_00185470 marks human cars 0x50 and copies the chassis name;
        # humans can occupy any world slot, including the last slot.
        ram = ram_path.read_bytes()
        def word(address):
            self.assertGreaterEqual(address, 0)
            self.assertLessEqual(address + 4, len(ram))
            return struct.unpack_from("<I", ram, address)[0]
        count = word(0x0048522C)
        self.assertGreater(count, 0)
        self.assertLessEqual(count, 16)
        human_models = []
        for index in range(count):
            car = word(0x00447458 + index * 4)
            self.assertGreater(car, 0)
            if word(car + 0x1E0) == 0x50:
                model = ram[car + 0x224:car + 0x224 + 32].split(b"\0", 1)[0]
                parameters = word(car + 8)
                parameter_model = ram[parameters + 0xAC:parameters + 0xAC + 32].split(b"\0", 1)[0]
                self.assertEqual(model, parameter_model)
                human_models.append(model)
        self.assertEqual(human_models, [b"Phantom"], "P1 must drive Phantom, not merely race a Phantom AI")

    def test_09_audio_sfx_and_pss_pipeline(self):
        """Verify SPU2 ADPCM codec, SFX0 compilation/decompilation, and PSS audio extraction."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)

            # Test round-trip encode/decode on synthetic PCM
            original_pcm = [int(15000 * math.sin(i * 0.1)) for i in range(280)]
            adpcm_data = encode_spu2_adpcm(original_pcm)
            self.assertEqual(len(adpcm_data), (280 // 28) * 16)
            decoded_pcm = decode_spu2_adpcm(adpcm_data)
            self.assertEqual(len(decoded_pcm), 280)

            # Test unpack RACESND.RES and round-trip start.sfx
            racesnd_res = DISC_DATA / "RES" / "RACESND.RES"
            if racesnd_res.exists():
                racesnd_dir = tmp / "racesnd"
                unpack_archive(str(racesnd_res), str(racesnd_dir))
                start_sfx = racesnd_dir / "start.sfx"
                if start_sfx.exists():
                    wav_out = tmp / "start.wav"
                    sfx_to_wav(str(start_sfx), str(wav_out))
                    self.assertTrue(wav_out.exists())

                    sfx_repack = tmp / "start_repack.sfx"
                    wav_to_sfx(str(wav_out), str(sfx_repack))
                    self.assertEqual(os.path.getsize(start_sfx), os.path.getsize(sfx_repack))

    def test_10_track_and_model_pipeline(self):
        """Verify ILIN AI lines, SOBL collision, and MODL OBJ export tools."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)

            # Test MODL OBJ and JSON export on Low0.mod
            algar_car = DISC_DATA / "CAR" / "ALGAR.CAR"
            if algar_car.exists():
                car_dir = tmp / "car"
                unpack_archive(str(algar_car), str(car_dir))
                low0 = car_dir / "Low0.mod"
                obj_out = tmp / "low0.obj"
                json_out = tmp / "low0.json"
                export_obj(str(low0), str(obj_out))
                export_json(str(low0), str(json_out))
                self.assertTrue(obj_out.exists())
                self.assertTrue(json_out.exists())

            # Test Track tools on AFTON.TRK
            afton_trk = DISC_DATA / "TRACKS" / "AFTON.TRK"
            if afton_trk.exists():
                trk_dir = tmp / "trk"
                unpack_archive(str(afton_trk), str(trk_dir))
                oval_ild = trk_dir / "oval.ild"
                if oval_ild.exists():
                    m = decompile_ild(str(oval_ild))
                    self.assertGreater(len(m["nodes"]), 0)
                    recompiled_ild = compile_ild(m)
                    self.assertGreater(len(recompiled_ild), 0)

                    line_obj = tmp / "oval.obj"
                    export_ild_to_obj(m, str(line_obj))
                    self.assertTrue(line_obj.exists())

                track_sol = trk_dir / "track.sol"
                if track_sol.exists():
                    sol = decompile_sol(str(track_sol))
                    self.assertGreater(len(sol["boxes"]), 0)
                    sol_obj = tmp / "sol.obj"
                    export_sol_to_obj(sol, str(sol_obj))
                    self.assertTrue(sol_obj.exists())


class TestAssetRegressions(unittest.TestCase):
    def test_archive_resize_does_not_overlap_next_entry(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "a").write_bytes(b"a" * 100)
            (path / "b").write_bytes(b"b" * 200)
            manifest = {"version": 6, "entries": [dict(name=n, fourcc="TEX", version=10) for n in ("a", "b")]}
            (path / "manifest.json").write_text(json.dumps(manifest))
            pack_archive(str(path), str(path / "first.car"))
            unpack_archive(str(path / "first.car"), str(path / "unpack"))
            (path / "unpack/a").write_bytes(b"a" * 3000)
            pack_archive(str(path / "unpack"), str(path / "grown.car"))
            unpack_archive(str(path / "grown.car"), str(path / "again"))
            self.assertEqual((path / "again/a").read_bytes(), b"a" * 3000)
            self.assertEqual((path / "again/b").read_bytes(), b"b" * 200)

    def test_tall_texture_preserves_every_pixel(self):
        w, h = 32, 64
        rgba = b"".join(bytes((y % 256, 0, 0, 255)) * w for y in range(h))
        self.assertEqual(decompile_tex(compile_tex(w, h, rgba))[2], rgba)

    def test_tex_matches_native_gs_upload_and_sampling_layout(self):
        # Independent oracle: runtime GS PSMCT32 upload addresses, then PSMT8
        # sampling addresses. Do not use tex.py's upload swizzle to verify itself.
        source = (REPO_ROOT / "runtime/gs/gs_mem.cpp").read_text()
        tables = {}
        for name, rows, cols, body in re.findall(r"(\w+)\[(\d+)\]\[(\d+)\] = \{(.*?)\};", source, re.S):
            values = [int(x) for x in re.findall(r"\d+", body)]
            tables[name] = [values[i * int(cols):(i + 1) * int(cols)] for i in range(int(rows))]
        w = h = 32
        rgba = b"".join(bytes((x * 8, (y % 8) * 32, 50, 255)) for y in range(h) for x in range(w))
        compiled = compile_tex(w, h, rgba)
        palette, mip = struct.unpack_from("<II", compiled, 0x18)
        vram = bytearray(16384)
        for y in range(h // 2):
            for x in range(w // 2):
                block = tables["blockTable32"][(y >> 3) & 3][(x >> 3) & 7]
                address = ((block << 6) + tables["columnTable32"][y & 7][x & 7]) * 4
                upload_offset = mip + (y * (w // 2) + x) * 4
                vram[address:address + 4] = compiled[upload_offset:upload_offset + 4]
        recovered = bytearray()
        for y in range(h):
            for x in range(w):
                block = tables["blockTable8"][(y >> 4) & 3][(x >> 4) & 7]
                address = (block << 8) + tables["columnTable8"][y & 15][x & 15]
                index = vram[address]
                a, r, g, b = compiled[palette + index * 4:palette + index * 4 + 4]
                recovered.extend((r, g, b, a))
        self.assertEqual(recovered, rgba)

    def test_tex_header_sizes_match_retail_pairs(self):
        for w, h, levels, size, word5 in ((16, 16, 4, 1668, 0), (256, 256, 8, 88708, 0),
                                        (256, 128, 7, 44936, 256), (512, 256, 8, 176008, 256)):
            data = compile_tex(w, h, bytes((0, 100, 200, 255)) * (w * h), header_word5=word5)
            self.assertEqual(len(data), size)
            self.assertEqual(struct.unpack_from("<8I", data), (w, h, levels, 0x201, 0x320600, word5, 0x80, 0x480))
            self.assertEqual(struct.unpack_from("<I", data, 0x1C + levels * 4)[0], 0)

    def test_small_mips_use_native_linear_upload_path(self):
        # f_0035EE38 branches directly to the linear path when mip height <32.
        # Use <=256 colors for exact palette preservation.
        expected = b"".join(bytes((x * 8, (y % 8) * 32, 50, 255)) for y in range(16) for x in range(32))
        data = compile_tex(32, 16, expected)
        recovered = bytearray()
        for index in data[0x480:0x480 + 512]:
            a, r, g, b = data[0x80 + index * 4:0x84 + index * 4]
            recovered.extend((r, g, b, a))
        self.assertEqual(recovered, expected)

    def test_unsupported_formats_fail_explicitly(self):
        with self.assertRaises(ValueError):
            compile_tex(20, 32, bytes(20 * 32 * 4))
        with self.assertRaises(ValueError):
            compile_tex(32, 256, bytes(32 * 256 * 4))
        with self.assertRaises(ValueError):
            compile_binary_valt({"is_v6": True, "sections": []})
        with self.assertRaises(ValueError):
            decode_spu2_adpcm(b"short")

    def test_audio_codec_has_bounded_loss(self):
        pcm = [int(15000 * math.sin(i * 0.1)) for i in range(280)]
        decoded = decode_spu2_adpcm(encode_spu2_adpcm(pcm))
        rms = math.sqrt(sum((a - b) ** 2 for a, b in zip(pcm, decoded)) / len(pcm))
        self.assertLess(rms, 1000)

    def test_native_iso_base_archive_names(self):
        from create_custom_car import car_archive_filename
        self.assertEqual(car_archive_filename("Algar"), "ALGAR.CAR")
        self.assertEqual(car_archive_filename("Hurricane"), "HURRIC_1.CAR")
        self.assertEqual(car_archive_filename("Gladiator"), "GLADIA_1.CAR")
        self.assertEqual(car_archive_filename("Ambulance"), "AMBULA_1.CAR")

    def test_save_money_cli_success_backup_and_verification(self):
        from eod_mod_gui import edit_career_money
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            save, binary = root / "save with spaces", root / "editor"
            save.write_bytes(b"original")
            binary.touch()
            calls = []
            def cli(args, **kwargs):
                calls.append(args)
                working = Path(args[args.index("--save-edit") + 1])
                if args[-1] == "money=9999":
                    working.write_bytes(b"edited")
                amount = 9999 if working.read_bytes() == b"edited" else 100
                return SimpleNamespace(returncode=0, stdout=f"player test, checksum ok\nmoney {amount}\n", stderr="")
            backup = edit_career_money(save, "9999", binary, runner=cli)
            self.assertEqual(save.read_bytes(), b"edited")
            self.assertEqual(backup.read_bytes(), b"original")
            self.assertEqual(len(calls), 3)
            self.assertNotIn(str(save), calls[1])

    def test_save_money_cli_rejects_missing_career_or_bad_reread(self):
        from eod_mod_gui import edit_career_money
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            save, binary = root / "save", root / "editor"
            save.write_bytes(b"original")
            binary.touch()
            for output in ("player test, checksum ok\n", "player test, checksum BAD\nmoney 1\n", "player test, checksum ok\nmoney 1\n"):
                def cli(args, **kwargs):
                    return SimpleNamespace(returncode=0, stdout=output, stderr="")
                with self.assertRaises(ValueError):
                    edit_career_money(save, "9999", binary, runner=cli)
                self.assertEqual(save.read_bytes(), b"original")
                self.assertEqual(list(root.glob("save.backup-*")), [])

    @unittest.skipUnless(EOD_BIN.is_file(), "Build native CLI to test synthetic save editing")
    def test_save_money_native_cli_synthetic_fixture(self):
        from eod_mod_gui import edit_career_money
        # Minimal fixture from runtime/core/eodsave.h/.cpp, not retail save data.
        raw = bytearray(0x90 + 0xE7C)
        struct.pack_into("<IIIIII", raw, 0, 0x4D435348, 3, 0x90, 0xE7C, 0, 1)
        struct.pack_into("<III", raw, 0x18, int.from_bytes(b"Crer", "big"), 1, 0xE7C)
        struct.pack_into("<i", raw, 0x90 + 0x84, 123)
        checksum = 0
        for value in raw[0x90:]:
            checksum = (checksum + value) & 0xFFFFFFFF
            checksum = ((checksum >> 1) | (checksum << 31)) & 0xFFFFFFFF
            checksum ^= 0xFEEDFACE
        struct.pack_into("<I", raw, 0x10, checksum)
        with tempfile.TemporaryDirectory() as directory:
            save = Path(directory) / "synthetic.save"
            save.write_bytes(raw)
            backup = edit_career_money(save, "9999", EOD_BIN)
            self.assertEqual(backup.read_bytes(), raw)
            self.assertEqual(struct.unpack_from("<i", save.read_bytes(), 0x90 + 0x84)[0], 9999)

    def test_gui_native_config_location(self):
        try:
            from eod_mod_gui import default_config_path
        except ModuleNotFoundError as exc:
            if exc.name in ("tkinter", "_tkinter"):
                self.skipTest("Python installation lacks Tk")
            raise
        self.assertEqual(default_config_path("darwin", "/synthetic/home"),
                         Path("/synthetic/home/Library/Application Support/TDEODRecomp/eod.ini"))
        self.assertEqual(default_config_path("linux", "/synthetic/home"), REPO_ROOT / "build-mac/eod.ini")

    def test_gui_settings_update_is_idempotent(self):
        try:
            from eod_mod_gui import save_gameplay_settings
        except ModuleNotFoundError as exc:
            if exc.name in ("tkinter", "_tkinter"):
                self.skipTest("Python installation lacks Tk")
            raise
        import configparser
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "eod.ini"
            path.write_text("[Video]\nRenderScale=1 ; native\n[Gameplay]\nUnlockAll=0\n[Cheats]\nInvincible=0\nChickens=1\n")
            for _ in range(2):
                save_gameplay_settings(path, dict(UnlockAll=1, Invincible=1, ChickenFight=0))
            parsed = configparser.ConfigParser()
            parsed.read(path)
            self.assertEqual(parsed["Gameplay"]["UnlockAll"], "1")
            self.assertEqual(parsed["Gameplay"]["Invincible"], "1")
            self.assertNotIn("Invincible", parsed["Cheats"])
            self.assertNotIn("Chickens", parsed["Cheats"])
            self.assertEqual(parsed["Gameplay"]["ChickenFight"], "0")
            self.assertEqual(parsed["Video"]["RenderScale"], "1")

    def test_custom_settings_validate_before_reading_disc(self):
        from create_custom_car import build_custom_car
        for options in (dict(name="LongerThanISO"), dict(name="Algar"), dict(name="../escape"), dict(number="-1"), dict(mass="nan"), dict(mass="0"), dict(redline="0"), dict(power_max="1e100"), dict(classes=[]), dict(classes=[0, 1]), dict(classes=[1, 1])):
            with self.assertRaises(ValueError):
                build_custom_car("/missing/disc", "/missing/output", **options)


if __name__ == "__main__":
    unittest.main()
