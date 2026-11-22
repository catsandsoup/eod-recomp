#!/usr/bin/env python3
"""Lossless round-trip self-test for EOD World Studio & Blender 4.2+ Bridge.

Verifies:
  1. Zero-edit byte-exact 0TSR v6 archive unpack -> repack round-trip
     (on retail AFTON.TRK when disc is present, plus synthetic 0TSR v6).
  2. 100% lossless .eodcol 32-bit binary collision pack -> unpack -> pack.
  3. 100% lossless export_object_obj -> parse_obj_to_mesh_parts Blender
     LH <-> RH round-trip (max vertex error < 1e-4, UV error < 1e-4, winding preserved).
  4. Blender 4.2+ extension manifest validity (blender_manifest.toml) and
     headless __init__.py syntax + import hygiene (plus retail .CAR payload check if disc present).

Runnable both directly (`python3 tools/world_editor/selftest_roundtrip.py`)
and via unittest (`python -m unittest tools/world_editor/selftest_roundtrip.py -v`).
"""

from __future__ import annotations

import ast
import hashlib
import json
import sys
import tempfile
import tomllib
import unittest
from pathlib import Path

import numpy as np

THIS_DIR = Path(__file__).resolve().parent
TOOLS_DIR = THIS_DIR.parent
for _p in (TOOLS_DIR, THIS_DIR):
    if str(_p) not in sys.path:
        sys.path.insert(0, str(_p))

import rst0  # noqa: E402
import world_loader as wl  # noqa: E402
from eod_blender_addon import eod_bridge  # noqa: E402
from project_state import export_object_obj  # noqa: E402
from road_collision import (  # noqa: E402
    EodCollisionMesh32,
    SURFACE_OBJECT,
    SURFACE_ROAD,
    SURFACE_TERRAIN,
    pack_eodcol,
    unpack_eodcol,
)


def _build_synthetic_0tsr_v6(dest: Path) -> Path:
    """Create a valid synthetic 0TSR v6 archive via rst0.pack_archive."""
    src_dir = dest / "synth_src"
    src_dir.mkdir(parents=True, exist_ok=True)
    payloads = [
        ("track.txt", "TEXT", 1, b"EOD SYNTHETIC 0TSR V6 ROUNDTRIP TEST\n"),
        ("mesh.mod", "MODL", 9, bytes(range(256)) * 3),
        ("wall.tex", "TEX ", 10, b"\xAA\x55" * 128),
    ]
    manifest = {
        "version": 6,
        "build_time": 1089733550,
        "unk": 0,
        "entries": [],
    }
    for name, fourcc, ver, blob in payloads:
        (src_dir / name).write_bytes(blob)
        manifest["entries"].append(
            {"name": name, "fourcc": fourcc, "version": ver, "is_extractable": False}
        )
    (src_dir / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    out_arc = dest / "SYNTH.TRK"
    rst0.pack_archive(str(src_dir), str(out_arc))
    return out_arc


def _make_test_world_and_object() -> tuple[wl.TrackWorld, wl.WorldObject]:
    """Build a multi-part, non-axis-aligned transformed WorldObject for OBJ round-trip."""
    # Part 1: asymmetric tetrahedron wedge (4 verts, 4 tris)
    v1 = np.array(
        [
            [0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.10, 0.20],
            [3.25, 0.40, -1.10, 0.0, 1.0, 0.0, 0.85, 0.25],
            [0.75, 0.15, 2.80, 0.0, 1.0, 0.0, 0.45, 0.90],
            [1.30, 2.65, 0.90, 0.0, 1.0, 0.0, 0.50, 0.50],
        ],
        dtype=np.float32,
    )
    i1 = np.array([0, 1, 2, 0, 3, 1, 1, 3, 2, 2, 3, 0], dtype=np.uint32)
    part1 = wl.MeshPart(texture="asphalt01.tex", vertices=v1, indices=i1, tri_count=4)

    # Part 2: tilted quad deck (4 verts, 2 tris)
    v2 = np.array(
        [
            [-2.0, 1.0, -1.5, 0.0, 1.0, 0.0, 0.0, 0.0],
            [2.0, 1.25, -1.5, 0.0, 1.0, 0.0, 1.0, 0.0],
            [2.0, 1.50, 1.5, 0.0, 1.0, 0.0, 1.0, 1.0],
            [-2.0, 1.25, 1.5, 0.0, 1.0, 0.0, 0.0, 1.0],
        ],
        dtype=np.float32,
    )
    i2 = np.array([0, 1, 2, 0, 2, 3], dtype=np.uint32)
    part2 = wl.MeshPart(texture="guardrail.tex", vertices=v2, indices=i2, tri_count=2)

    all_pts = np.vstack([v1[:, :3], v2[:, :3]])
    wm = wl.WorldModel(
        key="prop:roundtrip_ramp",
        name="RoundtripRamp",
        category="prop",
        model_store="procedural:test",
        hash_hex="deadbeef0001",
        parts=[part1, part2],
        tri_count=6,
        textures=["asphalt01.tex", "guardrail.tex"],
        bbox_min=all_pts.min(axis=0),
        bbox_max=all_pts.max(axis=0),
        pivot=(0.0, 0.0, 0.0),
    )
    obj = wl.WorldObject(
        obj_id="obj_rt_001",
        name="RoundtripRamp",
        model_key=wm.key,
        source="Placed by the editor",
        category="prop",
        subtype="Simple Model",
        section_mlist="editor_layer",
        model_store=wm.model_store,
        hash_hex=wm.hash_hex,
        tri_count=wm.tri_count,
        textures=list(wm.textures),
        position=(142.375, 11.625, -88.125),
        rotation_hpr=(37.5, -8.25, 12.0),
        scale=(1.2, 0.95, 1.35),
    )
    rgba = bytes([200, 180, 160, 255] * 4)
    textures = {
        t: wl.TextureAsset(
            name=t, width=2, height=2, rgba=rgba, alpha_kind="opaque", average_rgb=(200, 180, 160), asset_id=t
        )
        for t in ("asphalt01.tex", "guardrail.tex")
    }
    world = wl.TrackWorld(
        game_dir=Path("."),
        trk_path=Path("ROUNDTRIP.TRK"),
        trk_rel="DATA/TRACKS/ROUNDTRIP.TRK",
        track_name="ROUNDTRIP",
        display_name="Roundtrip Test",
        entries=[],
        graf_summary={},
        mlist_info=[],
        models={wm.key: wm},
        objects=[obj],
        textures=textures,
        collision=None,  # type: ignore[arg-type]
        road_graph=wl.RoadGraph(line_name="main"),
        all_road_graphs={},
        markers=[],
        world_center=(0.0, 0.0, 0.0),
        world_radius=200.0,
    )
    return world, obj


class TestLosslessRoundTrip(unittest.TestCase):
    def _assert_0tsr_byte_exact(self, arc_path: Path) -> None:
        orig_bytes = arc_path.read_bytes()
        with tempfile.TemporaryDirectory() as td:
            unpacked = Path(td) / "unpacked"
            repacked = Path(td) / "repacked.trk"
            rst0.unpack_archive(str(arc_path), str(unpacked))
            rst0.pack_archive(str(unpacked), str(repacked))
            new_bytes = repacked.read_bytes()
        self.assertEqual(len(new_bytes), len(orig_bytes), f"Size drift on {arc_path.name}")
        self.assertEqual(
            hashlib.sha256(new_bytes).hexdigest(),
            hashlib.sha256(orig_bytes).hexdigest(),
            f"SHA-256 mismatch on zero-edit 0TSR v6 round-trip of {arc_path.name}",
        )

    def test_0tsr_v6_zero_edit_byte_exact_roundtrip(self) -> None:
        gd = wl.default_game_dir()
        afton = gd / "DATA" / "TRACKS" / "AFTON.TRK" if gd.is_dir() else Path("__missing__")
        if afton.is_file():
            self._assert_0tsr_byte_exact(afton)
        with tempfile.TemporaryDirectory() as td:
            synth = _build_synthetic_0tsr_v6(Path(td))
            self._assert_0tsr_byte_exact(synth)

    def test_eodcol_32bit_lossless_roundtrip(self) -> None:
        verts = np.array(
            [
                [-120.5, 0.0, -80.25],
                [120.5, 4.75, -80.25],
                [120.5, 9.5, 95.125],
                [-120.5, 2.25, 95.125],
                [0.0, 14.0, 12.5],
            ],
            dtype=np.float32,
        )
        tris = np.array([[0, 1, 2], [0, 2, 3], [1, 4, 2], [3, 2, 4]], dtype=np.uint32)
        surf = np.array([SURFACE_ROAD, SURFACE_ROAD, SURFACE_TERRAIN, SURFACE_OBJECT], dtype=np.uint8)
        mesh = EodCollisionMesh32(
            vertices=verts,
            triangles=tris,
            surface_ids=surf,
            wall_boxes=[],
            aabb_min=verts.min(axis=0),
            aabb_max=verts.max(axis=0),
        )
        blob = pack_eodcol(mesh)
        unpacked = unpack_eodcol(blob)
        np.testing.assert_array_equal(unpacked.vertices, mesh.vertices)
        np.testing.assert_array_equal(unpacked.triangles, mesh.triangles)
        np.testing.assert_array_equal(unpacked.surface_ids, mesh.surface_ids)
        np.testing.assert_array_equal(unpacked.aabb_min, mesh.aabb_min)
        np.testing.assert_array_equal(unpacked.aabb_max, mesh.aabb_max)
        self.assertEqual(pack_eodcol(unpacked), blob)

    def test_blender_obj_lh_rh_roundtrip(self) -> None:
        # Verify exact coordinate inverse
        sample = (12.345, -67.89, 101.25)
        self.assertEqual(eod_bridge.blender_rh_to_eod_lh(eod_bridge.eod_lh_to_blender_rh(sample)), sample)

        world, obj = _make_test_world_and_object()
        model = world.models[obj.model_key]
        m4 = obj.world_matrix()

        with tempfile.TemporaryDirectory() as td:
            obj_path = export_object_obj(world, obj, Path(td))
            parts = eod_bridge.parse_obj_to_mesh_parts(obj_path)

            self.assertEqual(len(parts), len(model.parts))
            max_vert_err = 0.0
            max_uv_err = 0.0
            for orig_part, rt_part in zip(model.parts, parts):
                self.assertEqual(rt_part.texture, orig_part.texture)
                self.assertEqual(rt_part.tri_count, orig_part.tri_count)

                # Compute expected world-space triangle vertices & UVs
                pts = orig_part.vertices[:, 0:3]
                ones = np.ones((len(pts), 1), dtype=np.float32)
                wpts = (np.hstack([pts, ones]) @ m4.T)[:, :3]
                orig_tris = orig_part.indices.reshape(-1, 3)
                rt_tris = rt_part.indices.reshape(-1, 3)

                for ot, rt in zip(orig_tris, rt_tris):
                    p_orig = wpts[ot]
                    p_rt = rt_part.vertices[rt, 0:3]
                    err = float(np.max(np.abs(p_orig - p_rt)))
                    max_vert_err = max(max_vert_err, err)

                    uv_orig = orig_part.vertices[ot, 6:8]
                    uv_rt = rt_part.vertices[rt, 6:8]
                    max_uv_err = max(max_uv_err, float(np.max(np.abs(uv_orig - uv_rt))))

                    # Verify triangle winding preserved (face normal cosine > 0.999)
                    n_orig = np.cross(p_orig[1] - p_orig[0], p_orig[2] - p_orig[0])
                    n_rt = np.cross(p_rt[1] - p_rt[0], p_rt[2] - p_rt[0])
                    cos_sim = float(np.dot(n_orig, n_rt) / (np.linalg.norm(n_orig) * np.linalg.norm(n_rt)))
                    self.assertGreater(cos_sim, 0.999)

            self.assertLess(max_vert_err, 1e-4)
            self.assertLess(max_uv_err, 1e-4)

            # Verify WorldModel builder sets procedural:blender_obj for Sprint 2C .TRK compiler
            wm, pivot, pngs = eod_bridge.build_world_model_from_obj(obj_path)
            self.assertEqual(wm.model_store, "procedural:blender_obj")
            self.assertEqual(wm.tri_count, 6)
            self.assertEqual(len(pngs), 2)
            self.assertEqual(len(pivot), 3)

            # Verify quad & pentagon N-gon fan triangulation in parse_obj_to_mesh_parts
            ngon_obj = Path(td) / "ngon.obj"
            ngon_obj.write_text(
                "v 0 0 0\nv 2 0 0\nv 3 2 0\nv 1 3 0\nv -1 2 0\n"
                "vt 0 0\nvt 1 0\nvt 1 1\nvt 0.5 1\nvt 0 1\n"
                "usemtl ramp_mat\nf 1/1 2/2 3/3 4/4 5/5\n",
                encoding="utf-8",
            )
            ngon_parts = eod_bridge.parse_obj_to_mesh_parts(ngon_obj)
            self.assertEqual(len(ngon_parts), 1)
            self.assertEqual(ngon_parts[0].tri_count, 3)  # 5-gon -> 3 fan tris

    def test_blender_addon_manifest_and_syntax(self) -> None:
        addon_dir = TOOLS_DIR / "eod_blender_addon"
        manifest_path = addon_dir / "blender_manifest.toml"
        init_path = addon_dir / "__init__.py"
        self.assertTrue(manifest_path.is_file())
        self.assertTrue(init_path.is_file())

        manifest = tomllib.loads(manifest_path.read_text(encoding="utf-8"))
        self.assertEqual(manifest["schema_version"], "1.0.0")
        self.assertEqual(manifest["id"], "eod_world_studio")
        self.assertEqual(manifest["type"], "add-on")
        self.assertEqual(manifest["blender_version_min"], "4.2.0")

        # Syntax check + graceful non-Blender import
        ast.parse(init_path.read_text(encoding="utf-8"), filename=str(init_path))
        import eod_blender_addon

        self.assertEqual(eod_blender_addon.ADDON_ID, "eod_world_studio")
        self.assertFalse(eod_blender_addon.HAS_BPY)
        eod_blender_addon.register()
        eod_blender_addon.unregister()

        # Smoke-test real .CAR assembly payload (wheel offsets + Bone.mvl) when disc is present
        gd = wl.default_game_dir()
        cars = wl.discover_cars(gd) if gd.is_dir() else []
        if cars:
            payload = eod_bridge.load_car_assembly_payload(cars[0], game_dir=gd)
            self.assertGreater(len(payload["body_parts"]), 0)
            self.assertEqual(len(payload["wheels"]), 4)
            self.assertGreater(len(payload["bones"]), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
