"""Real archive/project contracts; retail game acceptance is a separate run."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from rst0 import pack_archive, unpack_archive
from valt import compile_binary_valt, parse_binary_valt


class ModdingBackendTest(unittest.TestCase):
    def setUp(self):
        module_path = TOOLS / "modding_backend.py"
        self.assertTrue(module_path.is_file(), "Real asset backend has not been implemented")
        spec = importlib.util.spec_from_file_location("modding_backend", module_path)
        self.backend = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.backend)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.disc = self.root / "disc"
        (self.disc / "DATA/CAR").mkdir(parents=True)
        self.project = self.root / "my project/project.eodproject"
        stage = self.root / "stage"
        stage.mkdir()
        table = dict(tool_vtable=0, unk1=0, unk2=0, sections=[
            dict(name="Chassis", parent="", sec_vtable=0, tool_ptr=0,
                 pairs=[("mass", "2800"), ("grip", "0.90"), ("opaque", "keep me")])])
        (stage / "physics.val").write_bytes(compile_binary_valt(table))
        (stage / "untouched.bin").write_bytes(bytes(range(256)))
        (stage / "manifest.json").write_text(json.dumps(dict(version=6, entries=[
            dict(name="physics.val", fourcc="VALT", version=11, is_extractable=False),
            dict(name="untouched.bin", fourcc="TEST", version=1, is_extractable=True)])))
        self.archive = self.disc / "DATA/CAR/ALGAR.CAR"
        with contextlib.redirect_stdout(io.StringIO()):
            pack_archive(str(stage), str(self.archive))
        self.original = self.archive.read_bytes()

    def request(self, action, **kwargs):
        return self.backend.dispatch(dict(action=action, disc=str(self.disc),
            project=str(self.project), archive="DATA/CAR/ALGAR.CAR", asset="physics.val", **kwargs))

    def save(self, value="4200"):
        return self.request("save", edits=[dict(section="Chassis", key="mass", value=value)])

    def test_catalog_reads_native_directory_and_inspection_reads_real_values(self):
        catalog = self.request("catalog")
        self.assertEqual(catalog["assetCount"], 2)
        self.assertEqual(catalog["archives"][0]["path"], "DATA/CAR/ALGAR.CAR")
        fields = self.request("inspect")["sections"][0]["fields"]
        self.assertEqual(fields[0]["value"], "2800")
        self.assertTrue(fields[0]["editable"])
        self.assertTrue(fields[2]["editable"])

    def test_edit_save_reopen_preserves_source_and_unrelated_payload(self):
        saved = self.save()
        loaded = self.request("load")
        self.assertEqual(loaded["selectedArchive"], "DATA/CAR/ALGAR.CAR")
        self.assertEqual(self.request("inspect")["sections"][0]["fields"][0]["value"], "4200")
        output = self.root / "verify"
        with contextlib.redirect_stdout(io.StringIO()):
            unpack_archive(saved["archivePath"], str(output))
        self.assertEqual((output / "untouched.bin").read_bytes(), bytes(range(256)))
        table = parse_binary_valt((output / "physics.val").read_bytes())
        self.assertEqual(dict(table["sections"][0]["pairs"])["mass"], "4200")
        self.assertEqual(self.archive.read_bytes(), self.original)

    def test_invalid_edits_do_not_overwrite_saved_project(self):
        self.save()
        before = self.project.read_bytes()
        for invalid in ["nan", "inf", "-1", "0", "1e-60", "1e50", "not a number", "4_200", "٤٢٠٠"]:
            with self.subTest(value=invalid), self.assertRaises(ValueError):
                self.save(invalid)
        self.assertEqual(self.project.read_bytes(), before)
        with self.assertRaises(ValueError):
            self.request("save", edits=[dict(section="Chassis", key="missing_key", value="destroy")])

    def test_mod_values_are_not_limited_by_invented_editor_caps(self):
        stage = self.root / "stage"
        table = parse_binary_valt((stage / "physics.val").read_bytes())
        table["sections"][0]["pairs"].append(("damage_scale", "1"))
        (stage / "physics.val").write_bytes(compile_binary_valt(table))
        with contextlib.redirect_stdout(io.StringIO()):
            pack_archive(str(stage), str(self.archive))
        for value in ["0", "0.01", "0.05", "10", "100"]:
            with self.subTest(value=value):
                self.request("save", edits=[dict(section="Chassis", key="damage_scale", value=value)])
        self.request("save", edits=[dict(section="Chassis", key="mass", value="1"),
                                    dict(section="Chassis", key="grip", value="50")])

    def test_existing_native_table_values_can_be_edited_without_a_new_schema(self):
        result = self.request("save", edits=[dict(section="Chassis", key="opaque", value="modified native text")])
        output = self.root / "table-edit"
        with contextlib.redirect_stdout(io.StringIO()): unpack_archive(result["archivePath"], str(output))
        fields = dict(parse_binary_valt((output / "physics.val").read_bytes())["sections"][0]["pairs"])
        self.assertEqual(fields["opaque"], "modified native text")

    def test_repeated_native_keys_are_edited_by_row_identity(self):
        stage = self.root / "stage"
        table = parse_binary_valt((stage / "physics.val").read_bytes())
        table["sections"][0]["pairs"] += [("repeated", "first"), ("repeated", "second")]
        (stage / "physics.val").write_bytes(compile_binary_valt(table))
        with contextlib.redirect_stdout(io.StringIO()): pack_archive(str(stage), str(self.archive))
        fields = self.request("inspect")["sections"][0]["fields"]
        self.assertTrue(fields[3]["editable"])
        self.assertTrue(fields[4]["editable"])
        saved = self.request("save", edits=[dict(section="Chassis", key="repeated", sectionIndex=0, pairIndex=4, value="changed")])
        output = self.root / "repeated"
        with contextlib.redirect_stdout(io.StringIO()): unpack_archive(saved["archivePath"], str(output))
        pairs = parse_binary_valt((output / "physics.val").read_bytes())["sections"][0]["pairs"]
        self.assertEqual(pairs[-2:], [("repeated", "first"), ("repeated", "changed")])

    def test_stale_source_and_corrupt_overlay_cannot_be_tested(self):
        saved = self.save()
        Path(saved["archivePath"]).write_bytes(b"broken")
        with self.assertRaises(ValueError):
            self.request("validate")
        self.save()
        self.archive.write_bytes(self.original[:-1] + bytes([self.original[-1] ^ 1]))
        with self.assertRaises(ValueError):
            self.save()

    def test_rejects_paths_into_disc_and_archive_traversal(self):
        with self.assertRaises(ValueError):
            self.backend.dispatch(dict(action="save", disc=str(self.disc),
                project=str(self.disc / "project.eodproject"), archive="DATA/CAR/ALGAR.CAR",
                asset="physics.val", edits=[dict(section="Chassis", key="mass", value="4200")]))
        with self.assertRaises(ValueError):
            self.backend.dispatch(dict(action="entries", disc=str(self.disc), archive="../outside.CAR"))

    def test_launch_plan_selects_saved_native_overlay_and_isolates_saves(self):
        saved = self.save()
        exe = self.root / "game binary"
        exe.write_text("#!/bin/sh\nexit 0\n")
        exe.chmod(0o755)
        plan = self.request("launch_plan", executable=str(exe), headless=True, frames=1800)
        self.assertEqual(plan["environment"]["EOD_MOD_DIR"], saved["overlay"])
        self.assertIn("car=Algar", plan["environment"]["EOD_DIRECT_START"])
        self.assertEqual(plan["environment"]["EOD_MUTE"], "1")
        self.assertNotIn(str(self.disc / "memcard"), plan["environment"]["EOD_MEMCARD"])
        self.assertIn("--headless", plan["arguments"])
        self.assertEqual(plan["environment"]["EOD_EDITOR_TEST"], "1")

    def test_project_edit_cannot_claim_an_old_overlay_contains_new_values(self):
        self.save()
        document = json.loads(self.project.read_text())
        document["archives"]["DATA/CAR/ALGAR.CAR"]["assets"]["physics.val"][0]["value"] = "5600"
        self.project.write_text(json.dumps(document))
        with self.assertRaises(ValueError):
            self.request("validate")

    def test_output_directory_symlink_cannot_write_outside_project(self):
        self.project.parent.mkdir(parents=True)
        outside = self.root / "outside"
        outside.mkdir()
        storage = self.project.parent / (self.project.name + ".assets")
        storage.mkdir()
        (storage / "overlay").symlink_to(outside, target_is_directory=True)
        with self.assertRaises(ValueError):
            self.save()
        self.assertEqual(list(outside.iterdir()), [])

    def test_model_material_can_resolve_its_native_high_detail_pair(self):
        # Retail CAR containers can carry Foo.tm0 while MODL binds Foo.tex.
        from tex import compile_tex
        stage = self.root / "material-stage"
        stage.mkdir()
        texture = compile_tex(16, 16, bytes((30, 50, 70, 255)) * 256)
        (stage / "Bus1.tm0").write_bytes(texture)
        (stage / "manifest.json").write_text(json.dumps(dict(version=6, entries=[
            dict(name="Bus1.tm0", fourcc="TEX ", version=10, is_extractable=True)])))
        with contextlib.redirect_stdout(io.StringIO()):
            pack_archive(str(stage), str(self.archive))
        self.assertEqual(self.backend.resolve_material(self.disc, self.archive, "Bus1.tex"), texture)

    def test_material_resolves_native_resident_part_archive(self):
        from tex import compile_tex
        stage = self.root / "part-stage"
        stage.mkdir()
        texture = compile_tex(16,16,bytes((50,70,90,255))*256)
        (stage/"Brake.tex").write_bytes(texture)
        (stage/"manifest.json").write_text(json.dumps(dict(version=6,entries=[
            dict(name="Brake.tex",fourcc="TEX ",version=10,is_extractable=True)])))
        res = self.disc/"DATA/RES"
        res.mkdir()
        with contextlib.redirect_stdout(io.StringIO()):
            pack_archive(str(stage),str(res/"PART.RES"))
        self.assertEqual(self.backend.resolve_material(self.disc,self.archive,"Brake.tex"),texture)

    def test_body_material_resolves_numbered_livery_for_its_own_chassis(self):
        from tex import compile_tex
        common_stage = self.root / "common-stage"; common_stage.mkdir()
        table = dict(tool_vtable=0,unk1=0,unk2=0,sections=[
            dict(name="Driver",parent="",sec_vtable=0,tool_ptr=0,pairs=[("chassis","Ambulance"),("number","601"),("name","Ambulance driver")]),
            dict(name="Driver",parent="",sec_vtable=0,tool_ptr=0,pairs=[("chassis","Algar"),("number","199"),("name","Algar driver")])])
        (common_stage/'dirt.val').write_bytes(compile_binary_valt(table))
        (common_stage/'manifest.json').write_text(json.dumps(dict(version=6,entries=[dict(name='dirt.val',fourcc='VALT',version=11,is_extractable=False)])))
        res = self.disc/'DATA/RES';res.mkdir()
        paints = self.root/'paints';paints.mkdir()
        expected=compile_tex(16,16,bytes((80,120,200,255))*256)
        other=compile_tex(16,16,bytes((200,40,20,255))*256)
        (paints/'199.tm0').write_bytes(expected);(paints/'601.tm0').write_bytes(other)
        (paints/'manifest.json').write_text(json.dumps(dict(version=6,entries=[dict(name=name,fourcc='TEX ',version=10,is_extractable=True) for name in ['199.tm0','601.tm0']])))
        with contextlib.redirect_stdout(io.StringIO()):
            pack_archive(str(common_stage),str(res/'COMMON.RES'))
            pack_archive(str(paints),str(res/'CT.RES'))
        self.assertEqual(self.backend.resolve_material(self.disc,self.archive,'Algar.tex'),expected)
        stage = self.root / "stage"
        (stage / "Algar.tex").write_bytes(other)
        manifest = json.loads((stage / "manifest.json").read_text())
        manifest["entries"].append(dict(name="Algar.tex",fourcc="TEX ",version=10,is_extractable=True))
        (stage / "manifest.json").write_text(json.dumps(manifest))
        with contextlib.redirect_stdout(io.StringIO()):
            pack_archive(str(stage),str(self.archive))
        self.assertEqual(self.backend.resolve_material(self.disc,self.archive,'Algar.tex'),other,
                         "An embedded body texture must take precedence over the default paint")
        self.assertIsNone(self.backend.resolve_material(self.disc,self.archive,'amby.tex'), 'Another chassis paint must never be substituted')

    def test_projects_in_one_folder_never_overwrite_each_others_overlay(self):
        first = self.save("4200")
        original = Path(first["archivePath"]).read_bytes()
        second_project = self.project.with_name("second.eodproject")
        second = self.backend.dispatch(dict(action="save", disc=str(self.disc), project=str(second_project),
            archive="DATA/CAR/ALGAR.CAR", asset="physics.val", edits=[dict(section="Chassis", key="mass", value="5600")]))
        self.assertNotEqual(first["overlay"], second["overlay"])
        self.assertEqual(Path(first["archivePath"]).read_bytes(), original)
        self.request("validate")

    def test_run_and_cache_paths_cannot_follow_links_to_disc(self):
        self.save()
        storage = self.project.parent / (self.project.name + ".assets")
        storage.mkdir(exist_ok=True)
        (storage / ".cache").symlink_to(self.disc, target_is_directory=True)
        with self.assertRaises(ValueError):
            self.request("inspect")
        (storage / ".cache").unlink()
        (storage / "runs").symlink_to(self.disc, target_is_directory=True)
        exe = self.root / "game"
        exe.write_text("#!/bin/sh\nexit 0\n"); exe.chmod(0o755)
        with self.assertRaises(ValueError):
            self.request("launch_plan", executable=str(exe))


if __name__ == "__main__":
    unittest.main()
