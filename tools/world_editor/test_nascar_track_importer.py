#!/usr/bin/env python3
"""Automated verification suite for Sprint 3F-3A: NASCAR D2D Track (.TRK) Transcoder & World Studio Importer.

Runs against the REAL NASCAR: Dirt to Daytona ISO and retail EOD DATA directory:
  1. Scans all 32 D2D tracks in < 250 ms.
  2. Imports KENYON.TRK (ShortDirt), BRISTOL.TRK (Night Skybox), and DAYTONA.TRK (2.5-mile
     Superspeedway with > 1 km extents in .eodcol), verifying valid 0TSR v6 with SFX0 at
     resident tail, GRAF v18, SCOL v4 + .eodcol, SOBL v3, ILIN v27, VALT v11 tables, .gar.
  3. Opens imported BRISTOL.TRK & KENYON.TRK via world_loader.load_world(), verifying
     non-empty 3D meshes, textures, SOBL objects, ILIN AI splines, .eodcol, battle spawn
     markers, AND placing a prop + saving project round-trip cleanly.
  4. Validates .eodmod Schema v2 bundles (SHA-256, BLAKE2b, zero unsafe entries).
  5. Verifies Tracy cold import time < 2.5 s per track.
"""

from __future__ import annotations

import os
from pathlib import Path
import struct
import sys
import tempfile
import time

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS_DIR = REPO_ROOT / "tools"
EDITOR_DIR = Path(__file__).resolve().parent
for _p in (str(TOOLS_DIR), str(EDITOR_DIR)):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import mod_package_v2 as mp2  # noqa: E402
import nascar_track_importer as nti  # noqa: E402
import project_state as ps  # noqa: E402
import road_collision as rc  # noqa: E402
import rst0  # noqa: E402
import valt  # noqa: E402
import world_collision as wc  # noqa: E402
import world_graf as wg  # noqa: E402
import world_loader as wl  # noqa: E402
from formats import ilin as ilin_fmt  # noqa: E402

ISO_PATH = Path("/Users/monty/Downloads/NASCAR - Dirt to Daytona (USA).iso")
EOD_DATA = Path("/Users/monty/Library/Application Support/TDEODRecomp/disc/DATA")
EOD_DISC = EOD_DATA.parent


def _assert_0tsr_v6_and_sfx_tail(trk_path: Path) -> list[dict]:
    raw = trk_path.read_bytes()
    assert raw[:4] == b"0TSR", f"Bad 0TSR magic in {trk_path}"
    ver = struct.unpack_from("<I", raw, 8)[0]
    assert ver == 6, f"Expected 0TSR v6, got v{ver}"
    dir_off, count, ext_blk = struct.unpack_from("<III", raw, 20)
    assert rst0.eod_crc(raw[:0x24]) == struct.unpack_from("<I", raw, 0x24)[0], "Header CRC mismatch"

    entries = wc.read_directory(raw)
    assert len(entries) == count

    # Verify SFX0 resident-tail invariant: every resident SFX0 sits strictly after all non-SFX0 resident entries
    # and strictly before extract_block.
    seen_sfx = False
    sfx_count = 0
    for i in range(count):
        row = dir_off + i * 68
        off = struct.unpack_from("<I", raw, row + 44)[0]
        is_ext = off >= ext_blk
        fcc = entries[i]["type"].strip().upper()
        if not is_ext:
            if fcc == "SFX0":
                seen_sfx = True
                sfx_count += 1
            else:
                assert not seen_sfx, f"Resident entry {entries[i]['name']} placed AFTER SFX0 tail!"
    assert sfx_count >= 1, "Expected borrowed ambient SFX0 banks at resident tail"
    return entries


def test_1_scan_32_tracks() -> None:
    t0 = time.perf_counter()
    tracks = nti.scan_nascar_tracks(ISO_PATH)
    elapsed_ms = (time.perf_counter() - t0) * 1000.0
    print(f"[TEST 1] Scanned {len(tracks)} D2D tracks in {elapsed_ms:.1f} ms")
    assert len(tracks) == 32, f"Expected 32 tracks, got {len(tracks)}"
    assert elapsed_ms < 250.0, f"Scan exceeded 250 ms budget: {elapsed_ms:.1f} ms"
    by_key = {t.track_key: t for t in tracks}
    assert by_key["KENYON"].surface_class == "ShortDirt"
    assert by_key["BRISTOL"].surface_class == "ShortAsphalt"
    assert by_key["BRISTOL"].has_night_sky is True
    assert by_key["DAYTONA"].surface_class == "Superspeedway"
    assert by_key["WATKIN_1"].surface_class == "RoadCourse"


def test_2_import_kenyon_bristol_daytona_and_validate() -> None:
    specs = [
        ("KENYON", "day", 200.0),
        ("BRISTOL", "night", 250.0),
        ("DAYTONA", "day", 1000.0),  # > 1 km Superspeedway!
    ]
    with tempfile.TemporaryDirectory(prefix="eod_nascar_trk_") as td:
        for key, mode, min_span_m in specs:
            res = nti.import_nascar_track(ISO_PATH, EOD_DATA, key, Path(td) / key, lighting_mode=mode)
            span_x = res.eodcol_aabb_max[0] - res.eodcol_aabb_min[0]
            span_z = res.eodcol_aabb_max[2] - res.eodcol_aabb_min[2]
            max_span = max(span_x, span_z)
            print(
                f"[TEST 2] {key} ({mode}): total={res.total_ms:.1f} ms "
                f"(graf={res.timings_ms.get('transcode_graf',0):.0f}ms, pack={res.timings_ms.get('pack_trk',0):.0f}ms) | "
                f"models={res.graf_models} parts={res.graf_parts} scol={res.scol_tris} "
                f"sobl={res.sobl_records} ilin={res.ilin_nodes} tex={res.tex_count} span={max_span:.1f}m"
            )
            assert res.total_ms < 2500.0, f"{key} cold import exceeded 2.5 s: {res.total_ms:.1f} ms"
            assert max_span > min_span_m, f"{key} .eodcol span {max_span:.1f}m <= {min_span_m}m"
            assert res.minimap_path.is_file() and res.minimap_path.stat().st_size > 200
            assert res.metadata_path.is_file()

            # 0TSR v6 + SFX0 tail check
            entries = _assert_0tsr_v6_and_sfx_tail(res.trk_path)
            raw = res.trk_path.read_bytes()
            names_l = {e["name"].lower(): e for e in entries}

            # Required members + versions
            for req, req_type, req_ver in (
                ("track.grf", "GRAF", 18),
                ("track.bsc", "SCOL", 4),
                ("track.sol", "SOBL", 3),
                ("track.ild", "ILIN", 27),
                ("oval.ild", "ILIN", 27),
                ("bw.ild", "ILIN", 27),
                ("bw_oval.ild", "ILIN", 27),
                ("lineinfo.val", "VALT", 11),
                ("c_normal_race.va0", "VALT", 11),
                ("c_battle.va0", "VALT", 11),
                ("track.bp", "BPPT", None),
                ("track.bs", "BSPT", None),
            ):
                assert req in names_l, f"{key} missing {req}"
                assert names_l[req]["type"] == req_type, f"{key} {req} bad type {names_l[req]['type']}"
                if req_ver is not None:
                    assert names_l[req]["version"] == req_ver, f"{key} {req} bad ver {names_l[req]['version']}"

            assert any(n.endswith(".gar") and e["type"] == "SETU" for n, e in names_l.items()), f"{key} missing .gar"

            # Verify binary parsers succeed cleanly
            gr_data, _ = wc.member(raw, entries, "track.grf", verify=True)
            g = wg.parse_graf(gr_data)
            assert len(g["leaves"]) > 10

            sc_data, _ = wc.member(raw, entries, "track.bsc", verify=True)
            scol = wc.parse_scol(sc_data)
            assert scol["count"] == res.scol_tris

            sol_data, _ = wc.member(raw, entries, "track.sol", verify=True)
            sobl = wc.parse_sobl(sol_data)
            assert sobl["count"] == res.sobl_records

            for ild_name in ("track.ild", "oval.ild", "bw.ild"):
                ild_data, _ = wc.member(raw, entries, ild_name, verify=True)
                pts = ilin_fmt.points(ild_data)
                assert len(pts) >= 20, f"{key}/{ild_name} too few ILIN nodes: {len(pts)}"

            cb_data, _ = wc.member(raw, entries, "c_battle.va0", verify=True)
            cb = valt.parse_binary_valt(cb_data)
            assert any(s["name"].lower() == "respawn" for s in cb["sections"])

            # Verify .eodcol unpacks cleanly
            col32 = rc.unpack_eodcol(res.eodcol_path.read_bytes())
            assert len(col32.triangles) > 100
            col_span = max(float(col32.aabb_max[0] - col32.aabb_min[0]), float(col32.aabb_max[2] - col32.aabb_min[2]))
            assert col_span > min_span_m

            # Verify night skybox remapping on BRISTOL
            if key == "BRISTOL" and mode == "night":
                assert "sky01a.tex" in names_l and "nsky01a.tex" not in names_l

            # Verify .eodmod Schema v2 bundle (SHA-256 + BLAKE2b + zero unsafe entries)
            assert res.eodmod_path is not None and res.eodmod_path.is_file()
            insp = mp2.inspect_eodmod_v2(res.eodmod_path)
            assert insp["schema"] == mp2.SCHEMA_V2
            assert insp["verified"] is True
            assert insp["total_operations"] == 3


def test_3_world_loader_and_prop_roundtrip() -> None:
    with tempfile.TemporaryDirectory(prefix="eod_nascar_wl_") as td:
        for key, mode in (("BRISTOL", "night"), ("KENYON", "day")):
            res = nti.import_nascar_track(ISO_PATH, EOD_DATA, key, Path(td) / key, lighting_mode=mode, export_eodmod=False)
            world = wl.load_world(res.trk_path, game_dir=EOD_DISC, include_cars=False, use_cache=False)
            print(
                f"[TEST 3] world_loader.load_world({key}): models={len(world.models)} "
                f"objects={len(world.objects)} tex={len(world.textures)} "
                f"sobl={world.collision.sobl_count} scol={world.collision.scol_count} "
                f"road_nodes={len(world.road_graph.nodes)} markers={len(world.markers)} "
                f"eodcol={'yes' if world.collision.eodcol_mesh is not None else 'no'} "
                f"load_ms={world.load_ms:.1f}"
            )
            assert len(world.models) > 50
            assert len(world.objects) > 50
            assert len(world.textures) > 20
            assert world.collision.sobl_count > 50
            assert world.collision.scol_count > 500
            assert world.collision.eodcol_mesh is not None
            assert len(world.road_graph.nodes) >= 50
            assert any(m.note == "c_battle.va0" for m in world.markers), f"{key} missing c_battle.va0 spawn markers"

            # Place a new prop + save project + reload round-trip cleanly!
            sample_mkey = next(iter(world.models.keys()))
            sample_model = world.models[sample_mkey]
            placed = wl.WorldObject(
                obj_id="u_test_prop_1",
                name="Placed_Test_Prop",
                model_key=sample_mkey,
                source="Placed by the editor",
                category="static",
                subtype=sample_model.subtype,
                section_mlist="editor_placed",
                model_store=sample_model.model_store,
                hash_hex=sample_model.hash_hex,
                tri_count=sample_model.tri_count,
                textures=list(sample_model.textures),
                position=(12.5, 2.0, -8.5),
                written=False,
            )
            world.objects.append(placed)
            proj_path = Path(td) / f"{key}_project.eodworld.json"
            saved_proj = ps.save_project_overlay(proj_path, f"{key}_test", world)
            assert saved_proj.is_file()

            # Reload freshly and apply project overlay
            world2 = wl.load_world(res.trk_path, game_dir=EOD_DISC, include_cars=False, use_cache=False)
            ps.apply_project_overlay(world2, saved_proj)
            found = next((o for o in world2.objects if o.obj_id == "u_test_prop_1"), None)
            assert found is not None, f"{key} placed prop lost on save/reload roundtrip!"
            assert abs(found.position[0] - 12.5) < 1e-3 and abs(found.position[2] + 8.5) < 1e-3


def test_4_main_window_menu_action_wired() -> None:
    from PySide6.QtWidgets import QApplication
    import main_window as mw

    app = QApplication.instance() or QApplication([])
    win = mw.EodWorldEditorWindow(game_dir=EOD_DISC, auto_load=False, show_home_hub=False)
    try:
        assert hasattr(win, "act_import_nascar_track")
        assert "Import NASCAR Track" in win.act_import_nascar_track.text()
        assert callable(win.open_import_nascar_track_dialog)
        print("[TEST 4] EodWorldEditorWindow File -> Import NASCAR Track from ISO... action verified")
    finally:
        win.close()
        app.processEvents()


def main() -> int:
    assert ISO_PATH.is_file(), f"Missing real D2D ISO at {ISO_PATH}"
    assert EOD_DATA.is_dir(), f"Missing retail EOD DATA at {EOD_DATA}"
    test_1_scan_32_tracks()
    test_2_import_kenyon_bristol_daytona_and_validate()
    test_3_world_loader_and_prop_roundtrip()
    test_4_main_window_menu_action_wired()
    print("ALL 4 NASCAR TRACK IMPORTER SUITES PASSED 100%!")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
