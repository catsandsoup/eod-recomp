#!/usr/bin/env python3
"""Build an experimental NASCAR vehicle overlay from owner-supplied disc data.

Archive/table conversion and synthetic liveries are supported. Runtime geometry,
physics, damage, career and AI compatibility still require player validation.
Converted track splines are research exports, not playable track imports.
"""

import argparse
import copy
import json
import os
import shutil
import struct
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools"))

from rst0 import unpack_archive, pack_archive
from valt import parse_binary_valt, compile_binary_valt
from tex import compile_tex, decompile_tex
from modl import parse_modl, retexture_modl
from trk import convert_nascar_ild_to_eod, decompile_nascar_ild, decompile_ild, compile_ild


def draw_racing_number(rgba, w, h, x_start, y_start, num_str, r_fg, g_fg, b_fg):
    """Draws a legible block racing number into the RGBA buffer."""
    # 5x7 block digit bitmaps
    DIGITS = {
        "0": ["111", "101", "101", "101", "111"],
        "1": ["010", "110", "010", "010", "111"],
        "2": ["111", "001", "111", "100", "111"],
        "3": ["111", "001", "111", "001", "111"],
        "4": ["101", "101", "111", "001", "001"],
        "5": ["111", "100", "111", "001", "111"],
        "6": ["111", "100", "111", "101", "111"],
        "7": ["111", "001", "010", "010", "010"],
        "8": ["111", "101", "111", "101", "111"],
        "9": ["111", "101", "111", "001", "111"],
    }
    scale = max(2, h // 28)
    cur_x = x_start
    for char in num_str:
        bitmap = DIGITS.get(char, DIGITS["0"])
        for row_idx, row in enumerate(bitmap):
            for col_idx, bit in enumerate(row):
                if bit == "1":
                    for sy in range(scale):
                        for sx in range(scale):
                            px = cur_x + col_idx * scale + sx
                            py = y_start + row_idx * scale + sy
                            if 0 <= px < w and 0 <= py < h:
                                off = (py * w + px) * 4
                                rgba[off : off + 4] = bytes([r_fg, g_fg, b_fg, 255])
        cur_x += 4 * scale + scale // 2


def create_nascar_livery(w, h, livery_type="stock", number="424"):
    """Generate a synthetic racing livery; this does not preserve the source artwork."""
    rgba = bytearray(w * h * 4)

    # Color palette schemes based on livery type
    if livery_type == "stock":
        # #424: Petty Blue (#0090D0) with STP Day-Glo Red (#E01020) and White Roof
        c_base = (0, 144, 208)
        c_stripe = (224, 16, 32)
        c_accent = (255, 255, 255)
    elif livery_type == "truck":
        # #426: GM Goodwrench stealth carbon black with silver skirts and red pin-stripes
        c_base = (24, 24, 28)
        c_stripe = (190, 195, 205)
        c_accent = (220, 20, 20)
    elif livery_type == "modified":
        # #428: Featherlite Modified bright competition yellow with asphalt black flanks
        c_base = (245, 210, 0)
        c_stripe = (20, 20, 24)
        c_accent = (255, 60, 0)
    elif livery_type == "dirtcar":
        # #430: Dirt Late Model blaze orange with deep metallic midnight blue panels
        c_base = (240, 90, 10)
        c_stripe = (10, 30, 90)
        c_accent = (255, 255, 255)
    elif livery_type == "hobby":
        # #432: Hobby Stock pearl white with metallic crimson fenders
        c_base = (240, 240, 240)
        c_stripe = (160, 20, 30)
        c_accent = (40, 40, 40)
    else:  # pacecar
        # #434: Official Safety Pace Car radiant silver with neon safety strobe accents
        c_base = (180, 185, 195)
        c_stripe = (235, 255, 0)
        c_accent = (20, 40, 180)

    for y in range(h):
        for x in range(w):
            idx = (y * w + x) * 4
            r, g, b, a = c_base[0], c_base[1], c_base[2], 255

            # Longitudinal center racing stripes
            center_x = w // 2
            stripe_half_w = max(4, w // 20)
            if abs(x - center_x) < stripe_half_w:
                r, g, b = c_stripe
            elif abs(x - center_x) < stripe_half_w + max(2, w // 80):
                r, g, b = c_accent

            # Lower body rocker skirts
            if y > int(h * 0.72):
                r, g, b = c_stripe

            # Roof panel
            if int(h * 0.28) <= y <= int(h * 0.58) and int(w * 0.35) <= x <= int(w * 0.65):
                r = (r + c_accent[0]) // 2
                g = (g + c_accent[1]) // 2
                b = (b + c_accent[2]) // 2

            rgba[idx : idx + 4] = bytes([r, g, b, a])

    # Left and Right Door Number plates
    plate_w = max(16, w // 5)
    plate_h = max(12, h // 4)

    # Left door plate
    x1, y1 = int(w * 0.12), int(h * 0.25)
    for py in range(y1, min(h, y1 + plate_h)):
        for px in range(x1, min(w, x1 + plate_w)):
            off = (py * w + px) * 4
            rgba[off : off + 4] = bytes([255, 255, 255, 255])
    draw_racing_number(rgba, w, h, x1 + 4, y1 + 3, number, 10, 10, 10)

    # Right door plate
    x2, y2 = int(w * 0.68), int(h * 0.25)
    for py in range(y2, min(h, y2 + plate_h)):
        for px in range(x2, min(w, x2 + plate_w)):
            off = (py * w + px) * 4
            rgba[off : off + 4] = bytes([255, 255, 255, 255])
    draw_racing_number(rgba, w, h, x2 + 4, y2 + 3, number, 10, 10, 10)

    # Roof number
    rx, ry = int(w * 0.42), int(h * 0.35)
    draw_racing_number(rgba, w, h, rx, ry, number, c_stripe[0], c_stripe[1], c_stripe[2])

    return bytes(rgba)


def create_uicar_thumbnail(w=16, h=16, livery_type="stock"):
    """Generates low-res 16x16 HUD carousel selection icon."""
    rgba = bytearray(w * h * 4)
    colors = {
        "stock": (0, 144, 208),
        "truck": (24, 24, 28),
        "modified": (245, 210, 0),
        "dirtcar": (240, 90, 10),
        "hobby": (240, 240, 240),
        "pacecar": (180, 185, 195),
    }
    car_rgb = colors.get(livery_type, (200, 20, 20))

    for y in range(h):
        for x in range(w):
            idx = (y * w + x) * 4
            if 3 <= y <= 12 and 2 <= x <= 13:
                if y <= 6 and (4 <= x <= 11):
                    # Roof / glass
                    r, g, b, a = 140, 200, 240, 255
                elif y > 6:
                    # Car body
                    r, g, b, a = car_rgb[0], car_rgb[1], car_rgb[2], 255
                else:
                    r, g, b, a = 0, 0, 0, 0
            else:
                r, g, b, a = 0, 0, 0, 0
            rgba[idx : idx + 4] = bytes([r, g, b, a])
    return bytes(rgba)


CARS_CONFIG = [
    {
        "source_archive": "STOCK.CAR",
        "chassis": "Nascar",
        "livery_type": "stock",
        "dealer_number": "424",
        "driver_number": "425",
        "driver_name": "Richard Petty",
        "price": "1800",
        "classes": ["$0000$chassis", "$0001$chassis", "$0002$chassis", "$0003$chassis"],
        "stats": {"durability": "9", "handling": "9", "speed": "10"},
        "desc": "Official 720 HP Winston Cup stock car. Maximum oval top speed, high downforce aerodynamics, and heavy-gauge steel rollcage.",
    },
    {
        "source_archive": "TRUCK.CAR",
        "chassis": "Ntruck",
        "livery_type": "truck",
        "dealer_number": "426",
        "driver_number": "427",
        "driver_name": "Dale Earnhardt Jr",
        "price": "1650",
        "classes": ["$0001$chassis", "$0002$chassis", "$0003$chassis"],
        "stats": {"durability": "10", "handling": "8", "speed": "9"},
        "desc": "NASCAR Craftsman Truck Series racer. 650 HP V8 torque monster with reinforced front bumper and high durability.",
    },
    {
        "source_archive": "MODIFIED.CAR",
        "chassis": "Modified",
        "livery_type": "modified",
        "dealer_number": "428",
        "driver_number": "429",
        "driver_name": "Mike Stefanik",
        "price": "1450",
        "classes": ["$0000$chassis", "$0003$chassis"],
        "stats": {"durability": "7", "handling": "10", "speed": "9"},
        "desc": "Featherlite Asphalt Modified. Lightweight 2610 lb open-wheel chassis with razor-sharp steering and rapid throttle response.",
    },
    {
        "source_archive": "DIRTCAR.CAR",
        "chassis": "Dirtcar",
        "livery_type": "dirtcar",
        "dealer_number": "430",
        "driver_number": "431",
        "driver_name": "Scott Bloomquist",
        "price": "1900",
        "classes": ["$0002$chassis", "$0003$chassis"],
        "stats": {"durability": "8", "handling": "9", "speed": "10"},
        "desc": "Dirt Late Model monster. 800 HP wide-track oval weapon designed for high-angle sliding, dirt clay tracks, and sudden acceleration.",
    },
    {
        "source_archive": "HOBBY.CAR",
        "chassis": "Hobby",
        "livery_type": "hobby",
        "dealer_number": "432",
        "driver_number": "433",
        "driver_name": "Jimmy Spencer",
        "price": "950",
        "classes": ["$0000$chassis", "$0001$chassis"],
        "stats": {"durability": "9", "handling": "7", "speed": "7"},
        "desc": "Grassroots Hobby Stock / Pure Stock. Sturdy domestic V8 chassis, great for demolition contacts, figure-8s, and beginner heats.",
    },
    {
        "source_archive": "PACECAR.CAR",
        "chassis": "Pacecar",
        "livery_type": "pacecar",
        "dealer_number": "434",
        "driver_number": "435",
        "driver_name": "Official Safety",
        "price": "1500",
        "classes": ["$0001$chassis", "$0002$chassis", "$0003$chassis"],
        "stats": {"durability": "9", "handling": "9", "speed": "9"},
        "desc": "Official NASCAR Pace Vehicle. Aerodynamically tuned sport coupe equipped for leading packs and high-speed emergency response.",
    },
]


def set_archive_entry(manifest, entry):
    """Replace a texture with the same engine name instead of adding duplicates."""
    entries = manifest["entries"]
    entries[:] = [item for item in entries if item["name"].casefold() != entry["name"].casefold()]
    entries.append(entry)


def compile_eod_table(model):
    """Explicit structural conversion; does not assert equivalent physics semantics."""
    model = copy.deepcopy(model)
    if model.get("is_v6"):
        model["is_v6"] = False
        model["tool_vtable"] = 0x40C218
        model["unk1"] = len(model["sections"])
        model["orig_sec_count"] = len(model["sections"])
        for section in model["sections"]:
            section["sec_vtable"] = 0x40C0D8
            section["tool_ptr"] = 0
    return compile_binary_valt(model)


def validate_inputs(nascar_disc_data, eod_disc_data, mod_out_dir):
    """Fail before writing anything when inputs are incomplete or overlap output."""
    nascar = Path(nascar_disc_data).expanduser().resolve()
    eod = Path(eod_disc_data).expanduser().resolve()
    out = Path(mod_out_dir).expanduser().resolve()
    for source in (nascar, eod):
        if out == source or out in source.parents or source in out.parents:
            raise ValueError(f"Output and source directories must not overlap: {out}, {source}")
    required = [nascar / cfg["source_archive"] for cfg in CARS_CONFIG]
    required += [eod / "RES" / name for name in ("COMMON.RES", "CT.RES", "UICAR.RES")]
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise FileNotFoundError("Missing required disc files:\n" + "\n".join(missing))
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        raise ValueError(f"Output must be a new or empty directory: {out}")
    return nascar, eod, out


def build_nascar_mod(nascar_disc_data: str, eod_disc_data: str, mod_out_dir: str, *, export_research_spline=False):
    """Build a candidate overlay; source assets are never modified."""
    nascar, eod, out = validate_inputs(nascar_disc_data, eod_disc_data, mod_out_dir)
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="eod-nascar-") as work:
        with tempfile.TemporaryDirectory(prefix=".eod-nascar-", dir=out.parent) as publish:
            candidate = Path(publish) / "overlay"
            _build_nascar_mod(nascar, eod, candidate, Path(work), export_research_spline)
            if out.exists():
                out.rmdir()  # Preflight accepted only an empty destination.
            candidate.rename(out)
    print(f"Published candidate overlay: {out}")


def _build_nascar_mod(nascar_disc_data, eod_disc_data, mod_out_dir, work_dir, export_research_spline):
    print("=" * 60)
    print("  NASCAR: Dirt to Daytona -> Test Drive: Eve of Destruction")
    print("  Cross-Engine Vehicle & Spline Importer Pipeline (/boost)")
    print("=" * 60)

    mod_out = Path(mod_out_dir)
    mod_car_dir = mod_out / "DATA" / "CAR"
    mod_res_dir = mod_out / "DATA" / "RES"
    research_dir = mod_out / "research"

    mod_car_dir.mkdir(parents=True, exist_ok=True)
    mod_res_dir.mkdir(parents=True, exist_ok=True)


    # -------------------------------------------------------------
    # Step 1: Process Each NASCAR Vehicle
    # -------------------------------------------------------------
    for cfg in CARS_CONFIG:
        src_name = cfg["source_archive"]
        chassis = cfg["chassis"]
        car_out_name = f"{chassis.upper()}.CAR"
        print(f"\n--- Processing {src_name} -> {car_out_name} (Chassis: {chassis}) ---")

        src_path = Path(nascar_disc_data) / src_name
        staging = work_dir / chassis
        staging.mkdir(parents=True, exist_ok=True)

        # Unpack NASCAR 0TSR v2 archive
        unpack_archive(str(src_path), str(staging))

        # Generate custom textures
        tex_base_name = f"{chassis}"
        tex256 = create_nascar_livery(256, 128, cfg["livery_type"], cfg["dealer_number"])
        tex512 = create_nascar_livery(512, 256, cfg["livery_type"], cfg["dealer_number"])
        tex_bytes = compile_tex(256, 128, tex256)
        tm0_bytes = compile_tex(512, 256, tex512)

        (staging / f"{tex_base_name}.tex").write_bytes(tex_bytes)
        (staging / f"{tex_base_name}.tm0").write_bytes(tm0_bytes)

        # Retexture all model files in archive
        mod_files = list(staging.glob("*.mod"))
        print(f"  Retexturing {len(mod_files)} model files...")
        for mf in mod_files:
            m_data = mf.read_bytes()
            # Retexture primary body texture references to new custom texture
            for old_pat in ["Stock1.tex", "Algar.tex", "truck1.tex", "mod1.tex", "Modfied1.tex"]:
                m_data = retexture_modl(m_data, old_pat, f"{tex_base_name}.tex")
            mf.write_bytes(m_data)

        # Preserve source table values. The two games' physics units and keys
        # are not proven equivalent; guessed horsepower/mass keys are unsafe.
        for vf in staging.glob("*.val"):
            v_model = parse_binary_valt(vf.read_bytes())
            vf.write_bytes(compile_eod_table(v_model))

        # Update manifest entries for packaging
        manifest_path = staging / "manifest.json"
        with open(manifest_path, "r") as f:
            manifest = json.load(f)

        manifest["version"] = 6
        manifest["extract_block"] = 0x60000

        # Add custom textures to manifest
        set_archive_entry(manifest, {
            "name": f"{tex_base_name}.tex",
            "fourcc": "TEX",
            "version": 10,
            "size": len(tex_bytes),
            "is_extractable": False,
        })
        set_archive_entry(manifest, {
            "name": f"{tex_base_name}.tm0",
            "fourcc": "TEX",
            "version": 10,
            "size": len(tm0_bytes),
            "is_extractable": True,
        })

        # Clear offsets to let pack_archive recompute 0x80 alignment
        for em in manifest["entries"]:
            em.pop("orig_offset", None)
            em.pop("offset", None)
            em.pop("has_igm", None)
            em.pop("orig_size", None)

        with open(manifest_path, "w") as f:
            json.dump(manifest, f, indent=2)

        out_car_file = mod_car_dir / car_out_name
        pack_archive(str(staging), str(out_car_file))
        print(f"  -> Built {out_car_file} ({out_car_file.stat().st_size} bytes)")

    # -------------------------------------------------------------
    # Step 2: Ingest into COMMON.RES
    # -------------------------------------------------------------
    print("\n--- Modifying COMMON.RES (Car Classes, Dealer, Stats, Rosters) ---")
    common_staging = work_dir / "common_res"
    unpack_archive(str(Path(eod_disc_data) / "RES" / "COMMON.RES"), str(common_staging))

    # 2a. carclass.val
    cc_path = common_staging / "carclass.val"
    cc_model = parse_binary_valt(cc_path.read_bytes())
    for cfg in CARS_CONFIG:
        chassis = cfg["chassis"]
        for target_cls in cfg["classes"]:
            for sec in cc_model["sections"]:
                if sec["name"] == target_cls:
                    if not any(k.lower() == chassis.lower() for k, _ in sec["pairs"]):
                        sec["pairs"].append((chassis, ""))
    cc_path.write_bytes(compile_binary_valt(cc_model))

    # 2b. car_stats.val
    cs_path = common_staging / "car_stats.val"
    cs_model = parse_binary_valt(cs_path.read_bytes())
    for cfg in CARS_CONFIG:
        chassis = cfg["chassis"]
        has_cs = any(
            any(k == "chassis" and v.lower() == chassis.lower() for k, v in s["pairs"])
            for s in cs_model["sections"]
        )
        if not has_cs:
            cs_model["sections"].append({
                "name": "carstats",
                "parent": "",
                "sec_vtable": 0x40C0D8,
                "tool_ptr": 0,
                "pairs": [
                    ("chassis", chassis),
                    ("durability", cfg["stats"]["durability"]),
                    ("handling", cfg["stats"]["handling"]),
                    ("speed", cfg["stats"]["speed"]),
                ],
            })
    # Enforce unk1 = sec_count invariant
    cs_model["unk1"] = len(cs_model["sections"])
    cs_model["orig_sec_count"] = len(cs_model["sections"])
    cs_path.write_bytes(compile_binary_valt(cs_model))

    # 2c. buycar.val (Career Dealership)
    bc_path = common_staging / "buycar.val"
    bc_model = parse_binary_valt(bc_path.read_bytes())
    for cfg in CARS_CONFIG:
        chassis = cfg["chassis"]
        num = cfg["dealer_number"]
        price = cfg["price"]
        desc = cfg["desc"]

        bc_model["sections"].append({
            "name": "car",
            "parent": "",
            "sec_vtable": 0x40C0D8,
            "tool_ptr": 0,
            "pairs": [
                ("chassis", chassis),
                ("desc", desc),
                ("price", price),
            ],
        })
        bc_model["sections"].append({
            "name": "vehicle",
            "parent": "",
            "sec_vtable": 0x40C0D8,
            "tool_ptr": 0,
            "pairs": [
                ("body_color", "255, 255, 255"),
                ("chassis", chassis),
                ("group_1", num),
                ("number", num),
                ("price", price),
            ],
        })
    bc_model["unk1"] = len(bc_model["sections"])
    bc_model["orig_sec_count"] = len(bc_model["sections"])
    bc_path.write_bytes(compile_binary_valt(bc_model))

    # 2d. dirt.val (AI Competitor Roster)
    dirt_path = common_staging / "dirt.val"
    dirt_model = parse_binary_valt(dirt_path.read_bytes())
    for cfg in CARS_CONFIG:
        chassis = cfg["chassis"]
        dnum = cfg["driver_number"]
        dname = cfg["driver_name"]

        dirt_model["sections"].append({
            "name": "Driver",
            "parent": "",
            "sec_vtable": 0x40C0D8,
            "tool_ptr": 0,
            "pairs": [
                ("body_color", "0xFF0090D0"),
                ("chassis", chassis),
                ("group_1", cfg["dealer_number"]),
                ("name", dname),
                ("number", dnum),
                ("rank", "850"),
            ],
        })
    dirt_model["unk1"] = len(dirt_model["sections"])
    dirt_model["orig_sec_count"] = len(dirt_model["sections"])
    dirt_path.write_bytes(compile_binary_valt(dirt_model))

    # 2e. catalog.val (Vehicle Descriptions)
    cat_path = common_staging / "catalog.val"
    if cat_path.exists():
        cat_model = parse_binary_valt(cat_path.read_bytes())
        for cfg in CARS_CONFIG:
            cat_model["sections"].append({
                "name": "catalog",
                "parent": "",
                "sec_vtable": 0x40C0D8,
                "tool_ptr": 0,
                "pairs": [
                    ("chassis", cfg["chassis"]),
                    ("title", f"NASCAR {cfg['chassis']} #{cfg['dealer_number']}"),
                    ("desc", cfg["desc"]),
                ],
            })
        cat_model["unk1"] = len(cat_model["sections"])
        cat_model["orig_sec_count"] = len(cat_model["sections"])
        cat_path.write_bytes(compile_binary_valt(cat_model))

    # Repack COMMON.RES
    common_manifest_path = common_staging / "manifest.json"
    with open(common_manifest_path, "r") as f:
        c_manifest = json.load(f)
    for em in c_manifest["entries"]:
        em.pop("orig_offset", None)
        em.pop("offset", None)
    with open(common_manifest_path, "w") as f:
        json.dump(c_manifest, f, indent=2)

    out_common = mod_res_dir / "COMMON.RES"
    pack_archive(str(common_staging), str(out_common))
    print(f"  -> Built {out_common} ({out_common.stat().st_size} bytes)")

    # -------------------------------------------------------------
    # Step 3: Ingest Livery Plates into CT.RES
    # -------------------------------------------------------------
    print("\n--- Modifying CT.RES (Selection Livery Number Plates) ---")
    ct_staging = work_dir / "ct_res"
    unpack_archive(str(Path(eod_disc_data) / "RES" / "CT.RES"), str(ct_staging))

    ct_manifest_path = ct_staging / "manifest.json"
    with open(ct_manifest_path, "r") as f:
        ct_manifest = json.load(f)

    for cfg in CARS_CONFIG:
        num = cfg["dealer_number"]
        tex256 = create_nascar_livery(256, 128, cfg["livery_type"], num)
        tex512 = create_nascar_livery(512, 256, cfg["livery_type"], num)
        c_tex = compile_tex(256, 128, tex256)
        c_tm0 = compile_tex(512, 256, tex512)

        (ct_staging / f"{num}.tex").write_bytes(c_tex)
        (ct_staging / f"{num}.tm0").write_bytes(c_tm0)

        set_archive_entry(ct_manifest, {
            "name": f"{num}.tex",
            "fourcc": "TEX",
            "version": 10,
            "size": len(c_tex),
            "is_extractable": False,
        })
        set_archive_entry(ct_manifest, {
            "name": f"{num}.tm0",
            "fourcc": "TEX",
            "version": 10,
            "size": len(c_tm0),
            "is_extractable": True,
        })

    for em in ct_manifest["entries"]:
        em.pop("orig_offset", None)
        em.pop("offset", None)
    with open(ct_manifest_path, "w") as f:
        json.dump(ct_manifest, f, indent=2)

    out_ct = mod_res_dir / "CT.RES"
    pack_archive(str(ct_staging), str(out_ct))
    print(f"  -> Built {out_ct} ({out_ct.stat().st_size} bytes)")

    # -------------------------------------------------------------
    # Step 4: Ingest Selection Thumbnails into UICAR.RES
    # -------------------------------------------------------------
    print("\n--- Modifying UICAR.RES (HUD Carousel Selection Thumbnails) ---")
    uicar_staging = work_dir / "uicar_res"
    unpack_archive(str(Path(eod_disc_data) / "RES" / "UICAR.RES"), str(uicar_staging))

    uicar_manifest_path = uicar_staging / "manifest.json"
    with open(uicar_manifest_path, "r") as f:
        uicar_manifest = json.load(f)

    for cfg in CARS_CONFIG:
        num = cfg["dealer_number"]
        ui16 = create_uicar_thumbnail(16, 16, cfg["livery_type"])
        ui256 = create_nascar_livery(256, 256, cfg["livery_type"], num)
        ui_tex = compile_tex(16, 16, ui16, fmt=0x201, flags=0x320600)
        ui_tm0 = compile_tex(256, 256, ui256, fmt=0x201, flags=0x320600)

        (uicar_staging / f"uicar_{num}.tex").write_bytes(ui_tex)
        (uicar_staging / f"uicar_{num}.tm0").write_bytes(ui_tm0)

        set_archive_entry(uicar_manifest, {
            "name": f"uicar_{num}.tex",
            "fourcc": "TEX",
            "version": 10,
            "size": len(ui_tex),
            "is_extractable": False,
        })
        set_archive_entry(uicar_manifest, {
            "name": f"uicar_{num}.tm0",
            "fourcc": "TEX",
            "version": 10,
            "size": len(ui_tm0),
            "is_extractable": True,
        })

    for em in uicar_manifest["entries"]:
        em.pop("orig_offset", None)
        em.pop("offset", None)
    with open(uicar_manifest_path, "w") as f:
        json.dump(uicar_manifest, f, indent=2)

    out_uicar = mod_res_dir / "UICAR.RES"
    pack_archive(str(uicar_staging), str(out_uicar))
    print(f"  -> Built {out_uicar} ({out_uicar.stat().st_size} bytes)")

    # -------------------------------------------------------------
    # Step 5: Convert & Ingest NASCAR Track AI Racing Line
    # -------------------------------------------------------------
    print("\n--- Converting NASCAR Track AI Racing Line (Daytona / Hastings -> EOD) ---")
    daytona_path = Path(nascar_disc_data) / "TRACKS2" / "DAYTONA.TRK"
    if not daytona_path.exists():
        daytona_path = Path(nascar_disc_data) / "TRACKS" / "HASTINGS.TRK"

    if export_research_spline and daytona_path.exists():
        trk_staging = work_dir / "daytona_unpack"
        unpack_archive(str(daytona_path), str(trk_staging))
        nascar_ild = trk_staging / "track.ild"
        if nascar_ild.exists():
            eod_ild_bytes = convert_nascar_ild_to_eod(nascar_ild.read_bytes())
            print(f"  Converted {nascar_ild.name} ({len(eod_ild_bytes)} bytes EOD ILIN)")

            # A racing line without matching geometry/coordinates is not a
            # playable track. Keep research data outside the mounted DATA tree.
            research_dir.mkdir(parents=True, exist_ok=True)
            (research_dir / f"{daytona_path.stem.lower()}_converted.ild").write_bytes(eod_ild_bytes)

    print("\n" + "=" * 60)
    print("  Experimental NASCAR overlay files built; runtime acceptance pending.")
    print(f"  Output directory: {mod_out_dir}")
    print("=" * 60)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nascar-data", default="/Volumes/SLUS_20441/DATA")
    parser.add_argument("--eod-data", default=str(Path.home() / "Library/Application Support/TDEODRecomp/disc/DATA"))
    parser.add_argument("--output", default="mods/nascar", help="New or empty output directory")
    parser.add_argument("--export-research-spline", action="store_true", help="Export an unverified 19-to-15-float spline candidate outside DATA")
    args = parser.parse_args()
    try:
        build_nascar_mod(args.nascar_data, args.eod_data, args.output, export_research_spline=args.export_research_spline)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"Error: {exc}\n")


if __name__ == "__main__":
    main()
