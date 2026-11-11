"""GRAF world parser, OBJ export and placement write-back.

Synthetic tests always run. Retail tests need EOD_TEST_RETAIL_MODELS=1 and the
disc at EOD_DISC (default ~/Library/Application Support/TDEODRecomp/disc).
Retail data is only read; outputs go to a temporary directory.
"""
import argparse
import json
import os
import re
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import world_graf as wg  # noqa: E402
import world_export as we  # noqa: E402

DISC = Path(os.environ.get("EOD_DISC", "~/Library/Application Support/TDEODRecomp/disc")).expanduser()
RETAIL = os.environ.get("EOD_TEST_RETAIL_MODELS") == "1" and (DISC / "DATA/TRACKS").is_dir()

# Exact display-list node counts per retail track (type -> count) and archive member
# counts. Anoka's 2517 leaves / 108 groups / 39 sphere nodes match the independent
# traversal recorded in work/track-environment-research/findings.md.
RETAIL_COUNTS = {
    "AFTON.TRK": (262, {"0x0": 315, "0x4": 1907, "0x102": 286}),
    "ANOKA.TRK": (369, {"0x0": 108, "0x4": 2517, "0x102": 39}),
    "BAXTER.TRK": (330, {"0x0": 793, "0x4": 2943, "0x101": 2, "0x102": 1}),
    "BEMIDJI.TRK": (290, {"0x0": 392, "0x4": 1960, "0x102": 1}),
    "DENNISON.TRK": (302, {"0x0": 198, "0x4": 1523, "0x102": 128, "0x103": 1}),
    "DUNDAS.TRK": (378, {"0x0": 256, "0x4": 2488, "0x102": 1}),
    "EAGAN.TRK": (295, {"0x0": 647, "0x4": 1595, "0x102": 3}),
    "EMPTYT_1.TRK": (20, {"0x4": 1}),
    "GILROY.TRK": (298, {"0x0": 141, "0x4": 3858, "0x102": 2}),
    "HASTINGS.TRK": (288, {"0x0": 82, "0x4": 1773, "0x101": 1, "0x102": 244, "0x103": 1}),
    "HOMEBASE.TRK": (449, {"0x0": 758, "0x4": 3503}),
    "KENYON.TRK": (289, {"0x0": 219, "0x4": 2297, "0x101": 1, "0x102": 176}),
    "MANKATO.TRK": (334, {"0x0": 72, "0x4": 3290, "0x101": 2}),
    "SPRING_1.TRK": (275, {"0x0": 1067, "0x4": 2155}),
    "WELCH.TRK": (200, {"0x0": 113, "0x4": 2264}),
}


def synthetic_graf():
    """header -> LOD group(2 children) -> [sphere -> leaf(kind0 model), leaf(null)]."""
    b = bytearray(0x200)
    struct.pack_into("<3I", b, 0, 0x10, 0, 0)
    # group @0x10: type0, next0, centre, count2, {dist2, child} x2
    struct.pack_into("<2I3fI", b, 0x10, 0, 0, 1.0, 2.0, 3.0, 2)
    struct.pack_into("<fIfI", b, 0x28, 2500.0, 0x40, 10000.0, 0x80)
    # sphere @0x40 -> leaf @0x60
    struct.pack_into("<3I4f", b, 0x40, 0x102, 0, 0x60, 0.0, 0.0, 0.0, 5.0)
    struct.pack_into("<3I", b, 0x60, 4, 0, 0x90)
    struct.pack_into("<3I", b, 0x80, 4, 0, 0)          # null model leaf
    # model record @0x90 (kind 0), embedded model at +0x70 -> 0x100 with 0 parts
    struct.pack_into("<3I", b, 0x90, 0, 0, 0x70)
    struct.pack_into("<6If3f6f", b, 0x100, 0, 0x50, 0, 0x50, 1, 0x50, 1.0, 0, 0, 0, -1, 1, -1, 1, -1, 1)
    return bytes(b)


class SyntheticTests(unittest.TestCase):
    def test_walk_and_lod_paths(self):
        g = wg.parse_graf(synthetic_graf())
        self.assertEqual(wg.summarize(g)["node_types"], {"0x0": 1, "0x4": 2, "0x102": 1})
        a, b = g["leaves"]
        self.assertEqual((a["kind"], a["model_offset"]), (0, 0x100))
        self.assertEqual(wg.lod_choice(a["path"]), (0,))
        self.assertEqual(wg.lod_choice(b["path"]), (1,))
        self.assertTrue(we.leaf_selected(a, 0) and not we.leaf_selected(b, 0))
        self.assertTrue(we.leaf_selected(b, 1) and we.leaf_selected(b, 5))
        self.assertEqual(wg.parse_model(synthetic_graf(), 0x100)["n_parts"], 0)

    def test_cycle_and_bad_type_rejected(self):
        b = bytearray(synthetic_graf())
        struct.pack_into("<I", b, 0x84, 0x60)   # null leaf -> next = earlier leaf (cycle)
        with self.assertRaises(wg.GrafError):
            wg.parse_graf(bytes(b))
        b = bytearray(synthetic_graf())
        struct.pack_into("<I", b, 0x80, 7)
        with self.assertRaises(wg.GrafError):
            wg.parse_graf(bytes(b))

    def test_q199_scanner(self):
        # 4SPS part: header 0x38 bytes then STCYCL 1/1 + UNPACK V4-32 @198 x2.
        part = bytearray(0x38) + struct.pack("<I", 0x01000101) + struct.pack("<I", 0x6C0200C6)
        part += struct.pack("<8f", 0, 0, 0, 1, 10.0, 20.0, 30.0, 0)
        struct.pack_into("<4sI", part, 0, b"4SPS", len(part))
        offs = we.vif_q199_offsets(bytes(part), dict(offset=0, size=len(part)))
        self.assertEqual(offs, [0x38 + 8 + 16])

    def test_frame64_codec(self):
        vals = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, -56.25, 0.5, 35.75]
        text = we.encode_frame64(vals)
        self.assertEqual(len(text), 64)
        self.assertEqual(we.decode_frame64(text), vals)
        # f_00133A50: 3 bytes (big-endian group) -> 4 symbols, most significant first.
        self.assertEqual(text[:4], "aaca")   # bytes 00 00 80 -> groups 0,0,2,0

    def test_matrix_convention_roundtrip(self):
        rows = [0.0, 0.0, 1.0, 0.0, 1.0, 0.0, -1.0, 0.0, 0.0, 5.0, 6.0, 7.0]
        M = we.native_rows_to_matrix(rows)
        self.assertEqual(we.matrix_to_native_rows(M), rows)
        # local +Z maps to native row 2 (= world -X) plus position.
        self.assertEqual(we.apply(M, (0, 0, 1)), (4.0, 6.0, 7.0))


class AlphaTests(unittest.TestCase):
    def test_alpha_treatment(self):
        weight = bytes([42] * 100)                          # road-like: palette weight, no zero texels
        cut = bytes([0] * 5 + [255] * 95)                   # 5% zero
        self.assertEqual(we.alpha_treatment(weight, "cutout"), ("opaque", 0.0))
        self.assertEqual(we.alpha_treatment(cut, "cutout")[0], "cutout")
        self.assertEqual(we.alpha_treatment(weight, "blend")[0], "blend")
        self.assertEqual(we.alpha_treatment(bytes([255] * 4), "blend")[0], "opaque")
        self.assertEqual(we.alpha_treatment(cut, "opaque")[0], "opaque")

    def test_apply_alpha(self):
        rgba = bytes([1, 2, 3, 0, 4, 5, 6, 77])
        self.assertEqual(we.apply_alpha(rgba, "blend"), rgba)
        self.assertEqual(we.apply_alpha(rgba, "opaque"), bytes([1, 2, 3, 255, 4, 5, 6, 255]))
        self.assertEqual(we.apply_alpha(rgba, "cutout"), bytes([1, 2, 3, 0, 4, 5, 6, 255]))


@unittest.skipUnless(RETAIL, "set EOD_TEST_RETAIL_MODELS=1 with a retail disc")
class RetailTests(unittest.TestCase):
    def test_all_tracks_parse_with_exact_counts(self):
        for name, (members, types) in RETAIL_COUNTS.items():
            with self.subTest(track=name):
                t = we.load_track(DISC, "DATA/TRACKS/" + name)
                self.assertEqual(len(t["entries"]), members)
                g = wg.parse_graf(t["graf"])
                self.assertEqual(wg.summarize(g)["node_types"], types)
                for leaf in g["leaves"]:
                    if leaf.get("model_offset"):
                        m = wg.parse_model(t["graf"], leaf["model_offset"])
                        for part in wg.decode_model(t["graf"], m):
                            for (a, b_, c) in part["tris"]:
                                self.assertLess(max(a, b_, c), len(part["verts"]))

    def export(self, track, dest, *extra):
        ns = argparse.Namespace(disc=str(DISC), track=track, dest=str(dest), lod=0, props_only=False,
                                terrain_only=False, blender=False, rh=False, include_sky=False,
                                include_unspawned=False, preview=False, alpha="cutout", double_sided=False)
        for k in extra:
            if "=" in k:
                setattr(ns, *k.split("=", 1))
            else:
                setattr(ns, k, True)
        return we.export(ns)

    def test_obj_well_formed_and_materials_exist(self):
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp)
            doc = self.export("DATA/TRACKS/ANOKA.TRK", dest)
            obj = (dest / "anoka_world.obj").read_text().splitlines()
            nv = nt = 0
            mats = set()
            for line in obj:
                if line.startswith("v "):
                    nv += 1
                elif line.startswith("vt "):
                    nt += 1
                elif line.startswith("usemtl "):
                    mats.add(line.split()[1])
                elif line.startswith("f "):
                    for ref in line.split()[1:]:
                        vi, ti = (int(x) for x in ref.split("/"))
                        self.assertTrue(1 <= vi <= nv and 1 <= ti <= nt, line)
            self.assertEqual(nv, nt)
            mtl = (dest / "anoka_world.mtl").read_text()
            defined = set(re.findall(r"^newmtl (\S+)", mtl, re.M))
            self.assertLessEqual(mats, defined)
            for png in re.findall(r"^map_(?:Kd|d) (\S+)", mtl, re.M):
                self.assertTrue((dest / png).is_file(), png)
            self.assertEqual(doc["stats"]["unresolvedMaterials"], [])
            self.assertEqual(sum(1 for line in obj if line.startswith("o ")), doc["stats"]["objects"])

    def test_alpha_modes_and_double_sided(self):
        def mtl_maps(d):
            txt = (d / "dennison_world.mtl").read_text()
            return {b.split("\n")[0] for b in txt.split("newmtl ")[1:] if "\nmap_d " in b}
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            cut = self.export("DATA/TRACKS/DENNISON.TRK", tmp / "c")
            blend = self.export("DATA/TRACKS/DENNISON.TRK", tmp / "b", "alpha=blend")
            opq = self.export("DATA/TRACKS/DENNISON.TRK", tmp / "o", "alpha=opaque")
            two = self.export("DATA/TRACKS/DENNISON.TRK", tmp / "d", "double_sided")
            cm, bm = mtl_maps(tmp / "c"), mtl_maps(tmp / "b")
            self.assertEqual(mtl_maps(tmp / "o"), set())
            self.assertLess(len(cm), len(bm))
            self.assertLessEqual(cm, bm)
            for road in ("rd-1", "rd-4", "rd-5", "rd-6", "fig-8"):
                self.assertIn(road, bm)
                self.assertNotIn(road, cm)
            for tree in ("p1", "trim", "fig-84"):
                self.assertIn(tree, cm)
            for t in cut["textures"].values():
                if t["status"] == "exported":
                    self.assertEqual(t["alphaKind"] == "cutout", t["zeroAlphaFraction"] >= we.CUTOUT_ZERO_FRACTION)
            nf = lambda d: sum(1 for l in (d / "dennison_world.obj").read_text().splitlines() if l.startswith("f "))
            self.assertEqual(nf(tmp / "d"), 2 * nf(tmp / "c"))
            self.assertEqual(two["options"]["doubleSided"], True)
            self.assertEqual(cut["options"]["alpha"], "cutout")

    def test_noop_placement_is_byte_identical(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "ANOKA.TRK"
            r = we.place(DISC, "DATA/TRACKS/ANOKA.TRK",
                         [dict(instance="i1352", translate=[0, 0, 0]), dict(instance="i0000", translate=[0, 0, 0])], out)
            self.assertTrue(r["identical"])
            self.assertEqual(out.read_bytes(), (DISC / "DATA/TRACKS/ANOKA.TRK").read_bytes())

    def test_moved_instance_changes_only_its_transform_bytes(self):
        src = DISC / "DATA/TRACKS/ANOKA.TRK"
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "ANOKA.TRK"
            delta = (3.0, 0.0, -2.0)
            r = we.place(DISC, "DATA/TRACKS/ANOKA.TRK", [dict(instance="i1352", translate=list(delta))], out)
            old_t = we.load_track(DISC, "DATA/TRACKS/ANOKA.TRK")
            import shutil
            fake = Path(tmp) / "disc"
            (fake / "DATA/TRACKS").mkdir(parents=True)
            shutil.copy(out, fake / "DATA/TRACKS/ANOKA.TRK")
            new_t = we.load_track(fake, "DATA/TRACKS/ANOKA.TRK")   # verifies directory + CRC
            a, b = old_t["graf"], new_t["graf"]
            g = wg.parse_graf(a)
            leaf = g["leaves"][1352]
            model = wg.parse_model(a, leaf["model_offset"])
            allowed = set()
            for part in model["parts"]:
                for q in we.vif_q199_offsets(a, part):
                    allowed.update(range(q, q + 12))
            mo = leaf["model_offset"]
            allowed.update(range(mo + 0x1C, mo + 0x40))
            key = struct.pack("<4f", *model["centre"], model["radius"])
            for info in g["mlist_info"]:
                k = a.find(key, info["offset"], info["offset"] + info["batches"]["offset"])
                if k >= 0:
                    allowed.update(range(k, k + 12))
            changed = [i for i in range(len(a)) if a[i] != b[i]]
            self.assertTrue(changed)
            self.assertTrue(set(changed) <= allowed, [hex(i) for i in changed if i not in allowed][:8])
            # Geometry moved by exactly delta (float32), everything else unchanged.
            before = wg.decode_model(a, model)
            after = wg.decode_model(b, wg.parse_model(b, mo))
            for p0, p1 in zip(before, after):
                for v0, v1 in zip(p0["verts"], p1["verts"]):
                    for ax in range(3):
                        self.assertAlmostEqual(v1["p"][ax] - v0["p"][ax], delta[ax], places=3)
            raw_old, raw_new = src.read_bytes(), out.read_bytes()
            off, size = old_t["graf_entry"]["offset"], old_t["graf_entry"]["size"]
            self.assertEqual(len(raw_old), len(raw_new))
            self.assertEqual(raw_old[off + size:], raw_new[off + size:])
            diff_dir = [i for i in range(off) if raw_old[i] != raw_new[i]]
            crc_pos = old_t["dir_offset"] + old_t["graf_index"] * 68 + 48
            self.assertTrue(set(diff_dir) <= set(range(crc_pos, crc_pos + 4)))
            self.assertEqual(r["edits"][0]["q199"], len([q for p in model["parts"] for q in we.vif_q199_offsets(a, p)]))

    def test_refuses_unverified_edits(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "X.TRK"
            with self.assertRaises(ValueError):
                we.place(DISC, "DATA/TRACKS/ANOKA.TRK", [dict(instance="i1352", rotate=[0, 90, 0])], out)
            sky = next(i for i, leaf in enumerate(wg.parse_graf(we.load_track(DISC, "DATA/TRACKS/ANOKA.TRK")["graf"])["leaves"])
                       if leaf.get("subtype") == wg.SKY)
            with self.assertRaises(ValueError):
                we.place(DISC, "DATA/TRACKS/ANOKA.TRK", [dict(instance="i%04d" % sky, translate=[1, 0, 0])], out)
            self.assertFalse(out.exists())

    def test_event_frames_orthonormal_and_roundtrip(self):
        t = we.load_track(DISC, "DATA/TRACKS/ANOKA.TRK")
        n = 0
        for e in t["entries"]:
            if not e["name"].endswith(".va0"):
                continue
            for p in we.event_props(t, e["name"]):
                f = p["frame"]
                R = [f[0:3], f[3:6], f[6:9]]
                for a in range(3):
                    for b in range(3):
                        self.assertAlmostEqual(sum(R[a][k] * R[b][k] for k in range(3)), float(a == b), places=2)
                self.assertEqual(we.decode_frame64(we.encode_frame64(f)), f)
                n += 1
        self.assertEqual(n, 2697)

    def test_event_prop_edits(self):
        t = we.load_track(DISC, "DATA/TRACKS/ANOKA.TRK")
        g, inst = we.build_scene(t, event="c_jump_race")
        wall = next(i for i in inst if i["category"] == "event-prop" and i["dyno"]["localFrame"])
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "ANOKA.TRK"
            r = we.place(DISC, "DATA/TRACKS/ANOKA.TRK",
                         [dict(event="c_jump_race", prop=wall["id"], matrix=wall["transform"])], out)
            self.assertTrue(r["identical"])
            we.place(DISC, "DATA/TRACKS/ANOKA.TRK",
                     [dict(event="c_jump_race", prop=wall["id"], translate=[0.0, 0.0, 4.0])], out)
            raw_new = out.read_bytes()
            changed = [i for i in range(len(t["raw"])) if t["raw"][i] != raw_new[i]]
            e = next(e for e in t["entries"] if e["name"] == "c_jump_race.va0")
            idx = t["entries"].index(e)
            start = e["offset"] + wall["source"]["frameMemberOffset"]
            crc = t["dir_offset"] + idx * 68 + 48
            self.assertTrue(changed and set(changed) <= set(range(start, start + 64)) | set(range(crc, crc + 4)))
            text = raw_new[start:start + 64].decode()
            self.assertAlmostEqual(we.decode_frame64(text)[11], wall["dyno"]["frame"][11] + 4.0, places=3)

    def test_dyno_matrix_edit(self):
        t = we.load_track(DISC, "DATA/TRACKS/ANOKA.TRK")
        g = wg.parse_graf(t["graf"])
        idx, leaf = next((i, l) for i, l in enumerate(g["leaves"])
                         if l.get("subtype") == wg.DYNO and l["dyno"]["mode"] != -1)
        M = we.native_rows_to_matrix(leaf["dyno"]["matrix"])
        M[3] += 1.5
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "ANOKA.TRK"
            we.place(DISC, "DATA/TRACKS/ANOKA.TRK", [dict(instance="i%04d" % idx, matrix=M)], out)
            raw_old, raw_new = t["raw"], out.read_bytes()
            changed = [i for i in range(len(raw_old)) if raw_old[i] != raw_new[i]]
            base = t["graf_entry"]["offset"] + leaf["record"] + wg.DYNO_MATRIX
            crc_pos = t["dir_offset"] + t["graf_index"] * 68 + 48
            self.assertTrue(set(changed) <= set(range(base + 36, base + 40)) | set(range(crc_pos, crc_pos + 4)))


if __name__ == "__main__":
    unittest.main()
