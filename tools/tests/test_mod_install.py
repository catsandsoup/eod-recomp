#!/usr/bin/env python3
"""Profile install/compose on synthetic 0TSR archives; retail composition when EOD_TEST_RETAIL_TEXTURES=1."""
import hashlib
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import mod_install as mi
import mod_package as mp
from test_mod_package import ANOKA, COMMON, RETAIL, RETAIL_ON, PackageCase, make_package, make_png, make_tex


def members(path):
    entries = mp.directory(path)
    return {e["name"]: mp.payload(path, e["name"], entries, verify_crc=False)[0] for e in entries}


class InstallCase(PackageCase):
    def setUp(self):
        super().setUp()
        self.profiles = self.root / "profiles"
        self.k = dict(profile="t", root=str(self.profiles))
        self.P = self.profiles / "t"

    def pkg(self, pid, edits, **kw):
        return make_package(self.root / pid, self.disc, pid, edits, **kw)

    def install(self, p, **kw):
        return mi.install(p, self.disc, **self.k, **kw)

    def active(self, rel=ANOKA):
        return self.P / "active" / rel

    def snapshot(self):
        return {str(p.relative_to(self.P)): p.read_bytes() for p in self.P.rglob("*") if p.is_file() and p.name != ".lock"}


class RoundtripTest(InstallCase):
    def test_install_enable_disable_uninstall(self):
        new = make_tex(21)
        self.install(self.pkg("a", {(ANOKA, "sky.tex"): new}))
        retail = members(self.disc / ANOKA)
        got = members(self.active())
        self.assertEqual(got["sky.tex"], new)
        self.assertEqual({k: v for k, v in got.items() if k != "sky.tex"}, {k: v for k, v in retail.items() if k != "sky.tex"})
        self.assertEqual(mi.launch_env(**self.k)["EOD_MOD_DIR"], str((self.P / "active").resolve()))
        text = (self.P / "active/PROFILE.TXT").read_text()
        self.assertIn("package a 1.0.0", text)
        mi.disable("a", **self.k)
        self.assertFalse(self.active().exists())
        mi.enable("a", **self.k)
        self.assertEqual(members(self.active())["sky.tex"], new)
        st = mi.status(**self.k)
        self.assertEqual((st["packages"][0]["enabled"], st["active"]["composed"], st["blocked"]), (True, True, []))
        mi.uninstall("a", **self.k)
        self.assertFalse(self.active().exists())
        self.assertFalse((self.P / "packages").exists() and any((self.P / "packages").rglob("*")))
        self.assertEqual(mi.status(**self.k)["packages"], [])
        self.assertEqual((self.disc / ANOKA).read_bytes(), (self.disc / ANOKA).read_bytes())

    def test_same_size_is_in_place_and_resizing_repacks(self):
        retail = members(self.disc / ANOKA)
        r = self.install(self.pkg("a", {(ANOKA, "sky.tex"): make_tex(21)}))
        self.assertEqual(r["compose"]["archives"][ANOKA]["method"], "in-place")
        grown = retail["rules.val"] + b" plus more rules"
        r = self.install(self.pkg("b", {(ANOKA, "rules.val"): grown}))
        self.assertEqual(r["compose"]["archives"][ANOKA]["method"], "repack")
        got = members(self.active())
        self.assertEqual(got["rules.val"], grown)
        self.assertEqual(got["sky.tex"], make_tex(21))
        for name in ("ground.tex", "mesh.mod"):
            self.assertEqual(got[name], retail[name])

    def test_two_nonconflicting_edits_in_one_archive_coexist(self):
        self.install(self.pkg("a", {(ANOKA, "sky.tex"): make_tex(21)}))
        self.install(self.pkg("b", {(ANOKA, "ground.tex"): make_tex(22), (COMMON, "pic.tex"): make_tex(23)}))
        got = members(self.active())
        self.assertEqual((got["sky.tex"], got["ground.tex"]), (make_tex(21), make_tex(22)))
        self.assertEqual(members(self.active(COMMON))["pic.tex"], make_tex(23))
        mi.disable("a", **self.k)
        self.assertEqual(members(self.active())["sky.tex"], members(self.disc / ANOKA)["sky.tex"])
        self.assertEqual(members(self.active())["ground.tex"], make_tex(22))

    def test_uninstall_keeps_stranger_files(self):
        self.install(self.pkg("a", {(ANOKA, "sky.tex"): make_tex(21)}))
        folder = self.P / "packages/a/1.0.0"
        stranger = folder / "notes.txt"
        stranger.write_text("mine")
        outside = self.P / "keep.txt"
        outside.write_text("keep")
        mi.uninstall("a", **self.k)
        self.assertEqual(stranger.read_text(), "mine")
        self.assertEqual(outside.read_text(), "keep")
        self.assertFalse((folder / "eodmod.json").exists())


class ConflictTest(InstallCase):
    def two(self):
        self.install(self.pkg("a", {(ANOKA, "sky.tex"): make_tex(21)}))
        self.install(self.pkg("b", {(ANOKA, "sky.tex"): make_tex(22)}), enable=False)

    def test_conflict_blocks_until_accepted_and_is_deterministic(self):
        self.two()
        before = self.snapshot()
        with self.assertRaisesRegex(ValueError, "accept-conflict"):
            mi.enable("b", **self.k)
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(mi.status(**self.k)["conflicts"], [])  # b is disabled
        with self.assertRaisesRegex(ValueError, "Winner must be"):
            mi.accept_conflict(f"{ANOKA}#sky.tex", "zzz", **self.k)
        mi.accept_conflict(f"{ANOKA}#sky.tex", "b", **self.k)
        mi.enable("b", **self.k)
        self.assertEqual(mi.status(**self.k)["conflicts"][0]["winner"], "b")
        first = self.active().read_bytes()
        self.assertEqual(members(self.active())["sky.tex"], make_tex(22))
        mi.order(["b", "a"], **self.k)
        self.assertEqual(self.active().read_bytes(), first)
        mi.compose(**self.k)
        self.assertEqual(self.active().read_bytes(), first)
        mi.accept_conflict(f"{ANOKA}#sky.tex", "a", **self.k)
        self.assertEqual(members(self.active())["sky.tex"], make_tex(21))

    def test_identical_edits_do_not_conflict(self):
        self.install(self.pkg("a", {(ANOKA, "sky.tex"): make_tex(21)}))
        self.install(self.pkg("b", {(ANOKA, "sky.tex"): make_tex(21)}))
        self.assertEqual(mi.status(**self.k)["conflicts"], [])

    def test_disabling_resolves_and_acceptance_accepted_winner_survives_disable(self):
        self.two()
        mi.accept_conflict(f"{ANOKA}#sky.tex", "a", **self.k)
        mi.enable("b", **self.k)
        self.assertEqual(members(self.active())["sky.tex"], make_tex(21))
        mi.disable("b", **self.k)
        self.assertEqual(mi.status(**self.k)["conflicts"], [])


class RejectionTest(InstallCase):
    def test_tampered_payload(self):
        p = self.pkg("a", {(ANOKA, "sky.tex"): make_tex(21)})
        f = p / f"native/{ANOKA}/sky.tex"
        f.write_bytes(f.read_bytes()[:-1] + b"Z")
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            self.install(p)
        self.assertFalse((self.P / "profile.json").exists())

    def test_wrong_source_hash_and_wrong_game(self):
        p = self.pkg("a", {(ANOKA, "sky.tex"): make_tex(21)})
        m = json.loads((p / "eodmod.json").read_text())
        m["native"][0]["sourceMemberSha256"] = "0" * 64
        (p / "eodmod.json").write_text(json.dumps(m))
        with self.assertRaisesRegex(ValueError, "different version"):
            self.install(p)
        from test_mod_package import make_disc
        other = make_disc(self.root / "o", exe=b"other release")
        with self.assertRaisesRegex(ValueError, "different game executable"):
            mi.install(self.pkg("c", {(ANOKA, "sky.tex"): make_tex(1)}), other, **self.k)

    def test_missing_dependency_and_dependents(self):
        p = self.pkg("child", {(ANOKA, "sky.tex"): make_tex(21)}, deps=[("base", "1.0.0")])
        with self.assertRaisesRegex(ValueError, "requires base"):
            self.install(p)
        self.install(self.pkg("base", {(COMMON, "pic.tex"): make_tex(5)}, version="1.2.0"))
        self.install(p)
        with self.assertRaisesRegex(ValueError, "depend on base"):
            mi.disable("base", **self.k)
        with self.assertRaisesRegex(ValueError, "depend on base"):
            mi.uninstall("base", **self.k)

    def test_zip_traversal_rejected_on_install(self):
        import zipfile
        z = self.root / "evil.eodmod"
        with zipfile.ZipFile(z, "w") as f:
            f.writestr("../escape.txt", b"x")
        with self.assertRaises(ValueError):
            self.install(z)
        self.assertFalse((self.root / "escape.txt").exists())

    def test_tampered_after_install_blocks_compose(self):
        self.install(self.pkg("a", {(ANOKA, "sky.tex"): make_tex(21)}))
        (self.P / f"packages/a/1.0.0/native/{ANOKA}/sky.tex").write_bytes(b"x" * 10)
        before = self.snapshot()
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            mi.compose(**self.k)
        self.assertEqual(self.snapshot(), before)


class AtomicityTest(InstallCase):
    def test_interrupted_install_leaves_profile_and_active(self):
        self.install(self.pkg("a", {(ANOKA, "sky.tex"): make_tex(21)}))
        before = self.snapshot()
        b = self.pkg("b", {(ANOKA, "ground.tex"): make_tex(22)})
        for target in ("mod_install.compose_stage", "mod_install.commit"):
            fn = target.split(".")[1]
            real = getattr(mi, fn)
            def boom(*a, **k):
                if fn == "commit":
                    real(*a, **k)  # swap done, then fail writing state
                raise KeyboardInterrupt("simulated")
            with mock.patch.object(mi, fn, boom) if fn == "compose_stage" else mock.patch.object(mi, "atomic_write", side_effect=OSError("disk full")):
                with self.assertRaises((KeyboardInterrupt, OSError)):
                    self.install(b)
            self.assertEqual(self.snapshot(), before, target)
            self.assertFalse(list(self.P.glob(".active.*")))

    def test_failed_compose_keeps_previous_active(self):
        self.install(self.pkg("a", {(ANOKA, "sky.tex"): make_tex(21)}))
        before = self.snapshot()
        with mock.patch.object(mi.mp, "build_archive", side_effect=ValueError("boom")):
            with self.assertRaisesRegex(ValueError, "boom"):
                mi.compose(**self.k)
        self.assertEqual(self.snapshot(), before)

    def test_lock_is_exclusive(self):
        with mi.locked(self.P):
            with self.assertRaisesRegex(ValueError, "Another Kit operation"):
                with mi.locked(self.P):
                    pass


class HostTextureTest(InstallCase):
    def test_eodhtx_chain_and_tsv(self):
        png = make_png(self.root / "g.png", 32, 32, seed=5)
        self.install(self.pkg("h", {}, host=[(f"{ANOKA}#ground.tex", png)]))
        tsv = (self.P / "active/HOSTTEX/replacements.tsv").read_text().splitlines()
        self.assertEqual(tsv[0], "eod-host-textures 1")
        key, pal, ow, oh, file, asset, pkg = tsv[1].split("\t")
        self.assertEqual((ow, oh, asset, pkg), ("16", "16", f"{ANOKA}#ground.tex", "h"))
        data = (self.P / "active/HOSTTEX" / file).read_bytes()
        self.assertEqual(data[:8], b"EODHTX1\0")
        self.assertEqual(struct.unpack_from("<4H", data, 8), (32, 32, 6, 0))
        self.assertEqual(len(data), 16 + sum(max(1, 32 >> i) ** 2 * 4 for i in range(6)))
        self.assertEqual(mi.check_eodhtx(data), (32, 32, 6))
        from tex import read_png
        self.assertEqual(data[16:16 + 32 * 32 * 4], read_png(str(png))[2])
        self.assertFalse(self.active().exists())  # host-only package leaves archives untouched

    def test_box_filter_straight_alpha_and_odd_sizes(self):
        px = bytes([0, 0, 0, 0, 255, 255, 255, 255] * 2)  # 2x2: alpha 0 / 255, no premultiply
        levels = mi.box_levels(2, 2, px)
        self.assertEqual(levels[1], bytes([128, 128, 128, 128]))
        levels = mi.box_levels(3, 5, bytes(range(60)))
        self.assertEqual([len(x) for x in levels], [60, 8, 4])  # 3x5 -> 1x2 -> 1x1
        self.assertEqual(len(mi.box_levels(1, 1, b"\1\2\3\4")), 1)


@unittest.skipUnless(RETAIL_ON and (RETAIL / "SLUS_209.10").is_file(), "set EOD_TEST_RETAIL_TEXTURES=1 with a retail disc")
class RetailInstallTest(unittest.TestCase):
    def test_anoka_compose_equals_project_build(self):
        src = Path(__file__).resolve().parents[2] / "mods/AnokaEnvironment"
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            shutil.copytree(src, temp / "AnokaEnvironment", symlinks=True)
            project = temp / "AnokaEnvironment/project.eodproject"
            mp.from_project(project, RETAIL, "anoka-env", "1.0.0", temp / "pkg")
            mi.install(temp / "pkg", RETAIL, profile="dev", root=str(temp / "profiles"))
            overlay = temp / "AnokaEnvironment/project.eodproject.assets/overlay"
            active = temp / "profiles/dev/active"
            built = sorted(p.relative_to(overlay) for p in overlay.rglob("*") if p.is_file())
            self.assertIn(Path(ANOKA), built)
            self.assertEqual(sorted(p.relative_to(active) for p in (active / "DATA").rglob("*") if p.is_file()),
                             [Path("DATA") / p.relative_to("DATA") for p in built])
            for rel in built:  # every composed archive equals the project's built overlay archive
                self.assertEqual((active / rel).read_bytes(), (overlay / rel).read_bytes(), str(rel))

if __name__ == "__main__":
    unittest.main()
