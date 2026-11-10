#!/usr/bin/env python3
"""Mod package build/verify/zip on synthetic 0TSR archives; retail packaging when EOD_TEST_RETAIL_TEXTURES=1."""
import contextlib
import io
import json
import os
import random
import shutil
import stat
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mod_package as mp
import rst0
from tex import compile_tex, write_png

RETAIL = Path(os.environ.get("EOD_TEST_DISC", str(Path.home() / "Library/Application Support/TDEODRecomp/disc")))
RETAIL_ON = os.environ.get("EOD_TEST_RETAIL_TEXTURES") == "1"
ANOKA = "DATA/TRACKS/ANOKA.TRK"
COMMON = "DATA/RES/COMMON.RES"


def make_tex(seed, size=16, colors=8, bits=None):
    rng = random.Random(seed)
    palette = [bytes(rng.randrange(256) for _ in range(3)) + b"\xff" for _ in range(colors)]
    return compile_tex(size, size, b"".join(rng.choice(palette) for _ in range(size * size)))


def pack(path, members):
    with tempfile.TemporaryDirectory() as work:
        for name, _, data in members:
            Path(work, name).write_bytes(data)
        Path(work, "manifest.json").write_text(json.dumps(dict(
            version=6, entries=[dict(name=n, fourcc=f, version=1, is_extractable=False) for n, f, _ in members])))
        path.parent.mkdir(parents=True, exist_ok=True)
        with contextlib.redirect_stdout(io.StringIO()):
            rst0.pack_archive(work, str(path))


def make_disc(root, exe=b"synthetic executable"):
    disc = Path(root) / "disc"
    (disc / "DATA").mkdir(parents=True)
    (disc / "SLUS_209.10").write_bytes(exe)
    pack(disc / ANOKA, [("sky.tex", "TEX", make_tex(1)), ("ground.tex", "TEX", make_tex(2)),
                        ("rules.val", "VALT", b"retail rules " * 6), ("mesh.mod", "MODL", b"mesh" * 9)])
    pack(disc / COMMON, [("stats.val", "VALT", b"stats" * 10), ("pic.tex", "TEX", make_tex(3))])
    return disc


def make_package(out, disc, pid, edits, version="1.0.0", host=(), deps=(), name=None):
    """edits: {(archive, member): bytes}; host: [(asset, png path)]."""
    m = mp.new_manifest(disc, pid, version, name)
    m["dependencies"] = [dict(id=i, minVersion=v) for i, v in deps]
    out = Path(out)
    for (archive, member), data in edits.items():
        old, entry, path, spelled = mp.retail_member(disc, archive, member)
        rel = f"native/{spelled}/{entry['name']}"
        (out / rel).parent.mkdir(parents=True, exist_ok=True)
        (out / rel).write_bytes(data)
        m["native"].append(dict(archive=spelled, member=entry["name"], sourceArchiveSha256=mp.file_sha(path),
                                sourceMemberSha256=mp.sha(old), file=rel, sha256=mp.sha(data)))
    mp.write_manifest(out, m)
    for asset, png in host:
        mp.add_host(out, disc, asset, png)
    return out


def make_png(path, w, h, seed=0):
    rng = random.Random(seed)
    write_png(str(path), w, h, bytes(rng.randrange(256) for _ in range(w * h * 4)))
    return path


class PackageCase(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.disc = make_disc(self.root)

    def pkg(self, name="p", pid="pack", edits=None, **kw):
        edits = {(ANOKA, "sky.tex"): make_tex(11)} if edits is None else edits
        return make_package(self.root / name, self.disc, pid, edits, **kw)


class PackageTest(PackageCase):
    def test_verify_inspect_and_source_hashes(self):
        p = self.pkg()
        info = mp.verify(p, self.disc)
        self.assertTrue(info["gameCompatible"])
        self.assertEqual(mp.inspect(p)["native"], [f"{ANOKA}#sky.tex"])

    def test_tampered_payload_rejected(self):
        p = self.pkg()
        f = p / f"native/{ANOKA}/sky.tex"
        f.write_bytes(f.read_bytes()[:-1] + b"X")
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            mp.verify(p)

    def test_unlisted_file_rejected(self):
        p = self.pkg()
        (p / "native/extra.bin").write_bytes(b"x")
        with self.assertRaisesRegex(ValueError, "unlisted"):
            mp.verify(p)

    def test_wrong_source_hash_and_wrong_game(self):
        p = self.pkg()
        other = make_disc(self.root / "other", exe=b"another release")
        with self.assertRaisesRegex(ValueError, "different game executable"):
            mp.verify(p, other)
        pack(self.disc / ANOKA, [("sky.tex", "TEX", make_tex(99)), ("ground.tex", "TEX", make_tex(2)),
                                 ("rules.val", "VALT", b"retail rules " * 6), ("mesh.mod", "MODL", b"mesh" * 9)])
        mp._SHA_CACHE.clear()
        with self.assertRaisesRegex(ValueError, "different version"):
            mp.verify(p, self.disc)

    def test_manifest_path_safety(self):
        p = self.pkg()
        m = json.loads((p / "eodmod.json").read_text())
        for bad in ("native/../x", "/abs", "native\\x", "native//x"):
            m["native"][0]["file"] = bad
            with self.assertRaises(ValueError):
                mp.validate_manifest(m)

    def test_symlink_in_package_rejected(self):
        p = self.pkg()
        os.symlink("/etc/hosts", p / "native/link")
        with self.assertRaisesRegex(ValueError, "symbolic link"):
            mp.verify(p)

    def test_from_project_emits_only_changed_members(self):
        source = self.disc / ANOKA
        entries = mp.directory(source)
        new = make_tex(12)
        built, _ = mp.build_archive(source, {"sky.tex": new})
        project = self.root / "proj/project.eodproject"
        overlay = project.parent / "project.eodproject.assets/overlay" / ANOKA
        overlay.parent.mkdir(parents=True)
        overlay.write_bytes(built)
        project.write_text(json.dumps(dict(version=1, archives={ANOKA: dict(
            sourceSha256=mp.file_sha(source), assets={"sky.tex": []}, outputSha256=mp.sha(built))})))
        info = mp.from_project(project, self.disc, "proj-mod", "1.0.0", self.root / "out")
        self.assertEqual(info["native"], 1)
        self.assertTrue(info["archiveReport"][0]["reproducesProjectBuild"])
        self.assertEqual((self.root / f"out/native/{ANOKA}/sky.tex").read_bytes(), new)
        self.assertEqual(len(entries), 4)

    def test_from_project_detects_modified_build(self):
        source = self.disc / ANOKA
        built, _ = mp.build_archive(source, {"sky.tex": make_tex(12)})
        project = self.root / "proj/project.eodproject"
        overlay = project.parent / "project.eodproject.assets/overlay" / ANOKA
        overlay.parent.mkdir(parents=True)
        overlay.write_bytes(built + b"\0")
        project.write_text(json.dumps(dict(version=1, archives={ANOKA: dict(
            sourceSha256=mp.file_sha(source), assets={}, outputSha256=mp.sha(built))})))
        with self.assertRaisesRegex(ValueError, "missing or modified"):
            mp.from_project(project, self.disc, "x", "1.0.0", self.root / "out")

    def test_add_host_accepts_power_of_two_scales_only(self):
        p = self.pkg(edits={})
        info = mp.add_host(p, self.disc, f"{ANOKA}#ground.tex", make_png(self.root / "g.png", 32, 32))
        host = json.loads((p / "eodmod.json").read_text())["host"][0]
        self.assertEqual((host["origWidth"], host["width"], host["assetId"]), (16, 32, f"{ANOKA}#ground.tex"))
        self.assertEqual(len(host["indexKey"]), 16)
        self.assertEqual(info["host"], 1)
        for w, h in ((48, 48), (32, 16), (16, 16 * 512)):
            with self.assertRaisesRegex(ValueError, "power of two|times"):
                mp.add_host(p, self.disc, f"{ANOKA}#sky.tex", make_png(self.root / "b.png", w, h))
        big = self.root / "big.png"  # header only: the size check must fail before decoding
        import struct as st
        big.write_bytes(b"\x89PNG\r\n\x1a\n" + st.pack(">I4sII", 13, b"IHDR", 8192, 8192) + b"\x08\x06\0\0\0" + bytes(8))
        with self.assertRaisesRegex(ValueError, "at most 4096"):
            mp.add_host(p, self.disc, f"{ANOKA}#sky.tex", big)
        with self.assertRaisesRegex(ValueError, "not a TEX"):
            mp.add_host(p, self.disc, f"{ANOKA}#rules.val", make_png(self.root / "g.png", 32, 32))

    def test_add_host_refuses_direct_colour(self):
        pack(self.disc / COMMON, [("stats.val", "VALT", b"s" * 40), ("pic.tex", "TEX", compile_tex(16, 16, bytes(range(256)) * 4))])
        raw = (self.disc / COMMON).read_bytes()
        p = self.pkg(edits={})
        entry = next(e for e in mp.directory(self.disc / COMMON) if e["name"] == "pic.tex")
        data = bytearray(raw[entry["offset"]:entry["offset"] + entry["size"]])
        if data[12] & 255 in (2, 3):
            with self.assertRaisesRegex(ValueError, "direct-colour"):
                mp.add_host(p, self.disc, f"{COMMON}#pic.tex", make_png(self.root / "d.png", 32, 32))
        else:  # synthetic compiler emitted indexed data: forge a direct-colour header
            fake = bytearray(make_tex(3)); fake[12] = 3
            with self.assertRaisesRegex(ValueError, "direct-colour"):
                mp.host_identity(bytes(fake), "x")


class ZipTest(PackageCase):
    def raw_zip(self, entries):
        path = self.root / "bad.eodmod"
        with zipfile.ZipFile(path, "w") as z:
            for item, data in entries:
                z.writestr(item, data)
        return path

    def test_roundtrip(self):
        p = self.pkg(host=[(f"{ANOKA}#ground.tex", make_png(self.root / "g.png", 32, 32))])
        z = mp.zip_package(p, self.root / "pack.eodmod")
        again = mp.zip_package(p, self.root / "pack2.eodmod")
        self.assertEqual(z["sha256"], again["sha256"])  # deterministic
        out = mp.unzip(z["path"], self.root / "unz")
        self.assertEqual(out["files"], mp.verify(p)["files"])
        self.assertEqual(mp.inspect(z["path"])["id"], "pack")

    def test_rejects_traversal_and_absolute(self):
        for name in ("../evil", "native/../../evil", "/abs/evil", "a\\b"):
            with self.assertRaises(ValueError):
                mp.unzip(self.raw_zip([(name, b"x")]), self.root / "o")
        self.assertFalse((self.root / "evil").exists())

    def test_rejects_symlink(self):
        info = zipfile.ZipInfo("native/link")
        info.external_attr = (stat.S_IFLNK | 0o777) << 16
        path = self.root / "l.eodmod"
        with zipfile.ZipFile(path, "w") as z:
            z.writestr(info, "/etc/hosts")
        with self.assertRaisesRegex(ValueError, "symbolic link"):
            mp.unzip(path, self.root / "o")

    def test_rejects_case_collision_and_duplicates(self):
        with self.assertRaisesRegex(ValueError, "collide by case"):
            mp.unzip(self.raw_zip([("eodmod.json", b"{}"), ("native/A", b"1"), ("native/a", b"2")]), self.root / "o")
        with self.assertRaisesRegex(ValueError, "duplicated|collide"):
            mp.unzip(self.raw_zip([("eodmod.json", b"{}"), ("native/A", b"1"), ("native/A", b"2")]), self.root / "o2")

    def test_rejects_tampered_zip_content(self):
        p = self.pkg()
        z = mp.zip_package(p, self.root / "t.eodmod")["path"]
        out = self.root / "t2.eodmod"
        with zipfile.ZipFile(z) as src, zipfile.ZipFile(out, "w") as dst:
            for item in src.infolist():
                data = src.read(item)
                dst.writestr(item.filename, data + b"!" if item.filename.startswith("native/") else data)
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            mp.unzip(out, self.root / "o3")
        self.assertFalse((self.root / "o3").exists())


@unittest.skipUnless(RETAIL_ON and (RETAIL / "SLUS_209.10").is_file(), "set EOD_TEST_RETAIL_TEXTURES=1 with a retail disc")
class RetailPackageTest(unittest.TestCase):
    def test_anoka_project_package(self):
        src = Path(__file__).resolve().parents[2] / "mods/AnokaEnvironment"
        with tempfile.TemporaryDirectory() as temp:
            shutil.copytree(src, Path(temp) / "AnokaEnvironment", symlinks=True)
            info = mp.from_project(Path(temp) / "AnokaEnvironment/project.eodproject", RETAIL, "anoka-env", "1.0.0", Path(temp) / "pkg")
            self.assertGreater(info["native"], 0)
            self.assertTrue(all(r["reproducesProjectBuild"] for r in info["archiveReport"]))
            self.assertTrue(mp.verify(Path(temp) / "pkg", RETAIL)["gameCompatible"])


if __name__ == "__main__":
    unittest.main()
