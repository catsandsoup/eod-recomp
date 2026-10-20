#!/usr/bin/env python3
"""CPU acceptance for conflict-aware composition of vehicle and Battle profiles."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from rst0 import pack_archive, unpack_archive
from build_mod_profiles import build_combined_profile


class CombinedProfilesTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.disc, self.phantom, self.battle, self.output = [self.root / n for n in ("disc", "phantom", "battle", "combined")]
        self.base = {"car_stats.val": (b"retail stats", 11), "events.val": (b"retail events", 11)}
        self.archive(self.disc, self.base)
        self.archive(self.phantom, dict(self.base, **{"car_stats.val": (b"Phantom stats", 11)}))
        self.archive(self.battle, dict(self.base, **{"events.val": (b"Battle events", 11)}))

    def archive(self, root, entries, *, version=6, unk=0):
        staging = self.root / (root.name + "-source")
        staging.mkdir(exist_ok=True)
        manifest = dict(version=version, build_time=123, unk=unk, entries=[])
        for name, (payload, resource_version) in entries.items():
            (staging / name).write_bytes(payload)
            manifest["entries"].append(dict(name=name, fourcc="VALT", version=resource_version, is_extractable=False))
        (staging / "manifest.json").write_text(json.dumps(manifest))
        archive = root / "DATA/RES/COMMON.RES"
        pack_archive(str(staging), str(archive))

    def build(self):
        return build_combined_profile(self.disc, self.phantom, self.battle, self.output)

    def contents(self):
        target = self.root / "inspection"
        unpack_archive(str(self.output / "DATA/RES/COMMON.RES"), str(target))
        manifest = json.loads((target / "manifest.json").read_text())
        return {entry["name"]: ((target / entry["name"]).read_bytes(), entry["version"]) for entry in manifest["entries"]}

    def test_preserves_vehicle_and_battle_changes_and_input_bytes(self):
        source_files = [p for root in (self.disc, self.phantom, self.battle) for p in root.rglob("*") if p.is_file()]
        before = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in source_files}
        (self.phantom / "profile-report.json").write_text('{"metadata":true}')
        extra = self.phantom / "DATA/CAR/PHANTOM.CAR"
        extra.parent.mkdir(parents=True)
        extra.write_bytes(b"vehicle payload")
        report = self.build()
        self.assertEqual(self.contents(), {"car_stats.val": (b"Phantom stats", 11), "events.val": (b"Battle events", 11)})
        self.assertEqual((self.output / "DATA/CAR/PHANTOM.CAR").read_bytes(), b"vehicle payload")
        self.assertFalse((self.output / "DATA/profile-report.json").exists())
        self.assertTrue((self.output / "composition-report.json").is_file())
        self.assertEqual(report["profile"], "combined")
        self.assertEqual(before, {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in before})

    def test_same_entry_different_changes_rejected_atomically(self):
        self.archive(self.battle, dict(self.base, **{"car_stats.val": (b"different stats", 11)}))
        with self.assertRaisesRegex(ValueError, "car_stats.val"):
            self.build()
        self.assertFalse(self.output.exists())
        self.assertFalse(list(self.root.glob(".combined-*")))

    def test_identical_changes_and_new_entry_metadata_survive(self):
        additions = dict(self.base, **{"car_stats.val": (b"Phantom stats", 11), "new.val": (b"new resource", 17)})
        self.archive(self.phantom, additions)
        self.archive(self.battle, additions)
        self.build()
        self.assertEqual(self.contents()["new.val"], (b"new resource", 17))

    def test_metadata_only_conflict_is_rejected(self):
        self.archive(self.phantom, dict(self.base, **{"car_stats.val": (b"retail stats", 12)}))
        self.archive(self.battle, dict(self.base, **{"car_stats.val": (b"retail stats", 13)}))
        with self.assertRaisesRegex(ValueError, "car_stats.val"):
            self.build()
        self.assertFalse(self.output.exists())

    def test_non_archive_collision_and_incompatible_header_rejected(self):
        for root in (self.phantom, self.battle):
            (root / "DATA/same.bin").write_bytes(root.name.encode())
        with self.assertRaisesRegex(ValueError, "same.bin"):
            self.build()
        self.assertFalse(self.output.exists())
        for root in (self.phantom, self.battle):
            (root / "DATA/same.bin").unlink()
        self.archive(self.battle, self.base, unk=1)
        with self.assertRaisesRegex(ValueError, "header"):
            self.build()
        self.assertFalse(self.output.exists())

    def test_single_side_deletion_is_preserved(self):
        self.archive(self.phantom, {"events.val": self.base["events.val"]})
        self.build()
        self.assertEqual(self.contents(), {"events.val": (b"Battle events", 11)})

    def test_atomic_publication_does_not_replace_racing_existing_target(self):
        from build_mod_profiles import _publish_exclusively
        staging = self.root / "publication-staging"
        staging.mkdir()
        (staging / "new").write_bytes(b"new")
        self.output.mkdir()
        sentinel = self.output / "keep"
        sentinel.write_bytes(b"existing")
        with self.assertRaises(FileExistsError):
            _publish_exclusively(staging, self.output)
        self.assertEqual(sentinel.read_bytes(), b"existing")
        self.assertTrue((staging / "new").is_file())

    def test_existing_output_and_nested_paths_are_rejected(self):
        self.output.mkdir()
        sentinel = self.output / "keep"
        sentinel.write_text("unchanged")
        with self.assertRaises(FileExistsError):
            self.build()
        self.assertEqual(sentinel.read_text(), "unchanged")
        nested = self.phantom / "new-output"
        with self.assertRaises(ValueError):
            build_combined_profile(self.disc, self.phantom, self.battle, nested)
        self.assertFalse(nested.exists())


if __name__ == "__main__":
    unittest.main()
