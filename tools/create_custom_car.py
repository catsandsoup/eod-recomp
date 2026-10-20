#!/usr/bin/env python3
"""Build and package the custom car 'PHANTOM' for Test Drive: Eve of Destruction.

This script:
1. Decompiles the base car model (ALGAR.CAR).
2. Generates custom textures and livery graphics (Phantom.tex / Phantom.tm0, 77.tex / 77.tm0, uicar_77.tex / uicar_77.tm0).
3. Modifies the 3D model parts to bind to the custom textures.
4. Customizes vehicle physics (mass, engine power, torque, redline, damage resistance).
5. Recompiles the new car archive into PHANTOM.CAR.
6. Integrates Phantom into the game's rosters (dirt.val), car classes (carclass.val),
   car stats (car_stats.val), career dealership (buycar.val), and selection menus (COMMON.RES, CT.RES, UICAR.RES).
7. Packages the entire mod into mods/phantom/DATA/... for seamless virtual disc mounting.
"""

import argparse
import math
import re
import tempfile
from pathlib import Path
import json
import os
import shutil
import struct
import subprocess
import sys

from rst0 import unpack_archive, pack_archive
from valt import parse_binary_valt, compile_binary_valt
from tex import compile_tex, decompile_tex, write_png
from modl import retexture_modl, parse_modl

def create_phantom_texture(w=256, h=128, body_color="#141414", stripe_style="Cyan + Magenta Dual Stripe", number="777"):
    """Generates custom racing livery RGBA image for Phantom (black with cyan/magenta stripes & #77)."""
    rgba = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            idx = (y * w + x) * 4
            # Dark carbon background
            base = 18 if ((x // 8) + (y // 8)) % 2 == 0 else 24
            r, g, b, a = base, base, base, 255

            # Dual racing stripes down the center
            center_x = w // 2
            if abs(x - (center_x - 12)) < 6:
                # Cyan racing stripe
                r, g, b = 0, 230, 255
            elif abs(x - (center_x + 12)) < 6:
                # Magenta racing stripe
                r, g, b = 255, 0, 128
            elif abs(x - center_x) < 2:
                # Gold pinstripe
                r, g, b = 255, 215, 0

            if stripe_style == "None (Stealth)" or (r == g == b):
                r, g, b = tuple(int(body_color[i:i+2], 16) for i in (1, 3, 5))
            elif stripe_style != "Cyan + Magenta Dual Stripe":
                r, g, b = {"Solid Gold Stripe": (255, 215, 0), "Neon Lime Stripe": (128, 255, 0), "Classic White Double": (255, 255, 255)}[stripe_style]

            # Side accent panels
            if y > h * 0.6 and (x < w * 0.25 or x > w * 0.75):
                r, g, b = 40, 45, 60

            # Procedural 3x5 number plate scales with the requested image size.
            glyphs = ("111101101101111", "010110010010111", "111001111100111",
                      "111001111001111", "101101111001001", "111100111001111",
                      "111100111101111", "111001001001001", "111101111101111", "111101111001111")
            scale = max(1, min(w // (len(str(number)) * 4 + 2), h // 10))
            px, py = x - w // 8, y - h // 5
            if 0 <= px < (len(str(number)) * 4 + 1) * scale and 0 <= py < 7 * scale:
                r, g, b = 240, 240, 240
                gx, gy = px // scale - 1, py // scale - 1
                digit, column = divmod(gx, 4)
                if 0 <= digit < len(str(number)) and column < 3 and 0 <= gy < 5:
                    if glyphs[int(str(number)[digit])][gy * 3 + column] == "1":
                        r, g, b = 10, 10, 10

            rgba[idx : idx + 4] = bytes([r, g, b, a])
    return bytes(rgba)


def create_uicar_thumbnail(w=16, h=16):
    """Generates custom selection menu thumbnail."""
    rgba = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            idx = (y * w + x) * 4
            if 3 <= y <= 12 and 2 <= x <= 13:
                # Car silhouette
                if y <= 6 and (4 <= x <= 11):
                    # Roof/windshield (cyan)
                    r, g, b = 0, 220, 255
                elif y > 6:
                    # Body (dark/magenta)
                    r, g, b = 220, 0, 120 if x % 2 == 0 else 20
                else:
                    r, g, b = 0, 0, 0
                a = 255
            else:
                r, g, b, a = 0, 0, 0, 0
            rgba[idx : idx + 4] = bytes([r, g, b, a])
    return bytes(rgba)


# Body material identifiers inspected in the retail Low1.mod SPS4 headers.
# Shared cockpit, engine and chassis textures are deliberately excluded.
BODY_TEXTURES = {
    "algar": ("Algar.tex",), "ambulance": ("amby.tex",), "arrow": ("arrow.tex",),
    "blitz": ("mira.tex",), "bulldog": ("Marmot.tex",), "bus": ("Bus1.tex", "Bus2.tex"),
    "cannon": ("Cruiser.tex",), "copcar": ("police.tex",), "coyote": ("Bandimer.tex",),
    "desio": ("desio.tex",), "digit": ("Digit.tex",), "durand": ("Durand.tex",),
    "empire": ("ariel.tex",), "fillmore": ("fillmore.tex",), "gladiator": ("muscle.tex",),
    "guerilla": ("Jagger.tex",), "hearse": ("hearse.tex",), "hunter": ("Dirtcar.tex",),
    "hurricane": ("Vaquero.tex",), "limo": ("limo.tex",), "mongoose": ("gisarme.tex",),
    "moth": ("Gizmo.tex",), "ocelot": ("ocey.tex",), "orbital": ("Junker.tex",),
    "outlaw": ("guerrero.tex",), "palo": ("Palo.tex",), "postal": ("pojeep.tex",),
    "prince": ("caravel.tex",), "rocket": ("Olympia.tex",), "taxi": ("taxi.tex",),
    "top": ("Courier.tex",), "venom": ("pony.tex",),
}


def body_texture_names(base, model_data):
    expected = BODY_TEXTURES.get(base.lower())
    if not expected:
        raise ValueError(f"No verified retail body binding for {base}")
    materials = {p["texture"].lower() for p in parse_modl(model_data)["parts"]
                 if p["valid"] and p["magic"] == "4SPS"}
    if not all(name.lower() in materials for name in expected):
        raise ValueError(f"Retail body material binding missing for {base}: {expected}")
    return expected


def asset_path(directory, name):
    matches = [p for p in Path(directory).iterdir() if p.name.lower() == name.lower()]
    if len(matches) != 1:
        raise ValueError(f"Missing or ambiguous archive asset: {name}")
    return matches[0]


def car_archive_filename(name):
    """Match native f_00331708 ISO 8.3 chassis-name mapping."""
    stem = name if len(name) <= 8 else name[:6] + "_1"
    return stem.upper() + ".CAR"


def build_custom_car(disc_data_dir: str, mod_out_dir: str, **options):
    """Build a renamed/retextured retail chassis, not newly compiled mesh geometry."""
    settings = dict(name="Phantom", base="Algar", number="777", classes=[1],
                    power_max="380", torque_max="420", redline="9000", mass="3200",
                    damage_scale="0.65", grip="0.95", price="1200",
                    description="Custom tuned V8 machine with reinforced rollcage and racing suspension",
                    body_color="#141414", stripe_style="Cyan + Magenta Dual Stripe")
    unknown = options.keys() - settings.keys()
    if unknown:
        raise ValueError(f"Unknown vehicle options: {sorted(unknown)}")
    settings.update(options)
    for key in ("name", "base"):
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]{0,10}", settings[key]):
            raise ValueError(f"{key} must be 1-11 letters, digits or underscores")
    if len(settings["name"]) > 8:
        raise ValueError("Custom car names must be at most 8 characters for native ISO lookup")
    if settings["name"].lower() == settings["base"].lower():
        raise ValueError("Custom name must differ from the retail base chassis")
    for key in ("power_max", "torque_max", "redline", "mass", "damage_scale", "grip", "price"):
        value = float(settings[key])
        if not math.isfinite(value) or value < 0 or value > 3.402823466e38:
            raise ValueError(f"{key} must be a finite, nonnegative PS2 float")
        if key in ("mass", "redline") and value == 0:
            raise ValueError(f"{key} must be positive")
        settings[key] = str(settings[key])
    if not str(settings["number"]).isdigit() or not 0 <= int(settings["number"]) < 2147483646:
        raise ValueError("Racing number must be a nonnegative integer below 2147483646")
    settings["number"] = str(int(settings["number"]))
    if len(settings["classes"]) != 1 or settings["classes"][0] not in (0, 1, 2):
        raise ValueError("Select exactly one class: native chassis lookup returns only the first matching class")
    if not re.fullmatch(r"#[0-9a-fA-F]{6}", settings["body_color"]):
        raise ValueError("Body color must be #RRGGBB")
    if settings["stripe_style"] not in ("Cyan + Magenta Dual Stripe", "Solid Gold Stripe", "Neon Lime Stripe", "Classic White Double", "None (Stealth)"):
        raise ValueError("Unsupported stripe style")
    settings["description"].encode("latin1")
    disc = Path(disc_data_dir).resolve()
    output = Path(mod_out_dir).resolve()
    if disc == output or disc in output.parents or output in disc.parents:
        raise ValueError("Mod output must be separate from original disc data")
    with tempfile.TemporaryDirectory(prefix="eod-car-") as staging:
        package = Path(staging) / "package"
        _build_custom_car(str(disc), str(package), staging, settings)
        for source in package.rglob("*"):
            if source.is_file():
                target = output / source.relative_to(package)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)


def _build_custom_car(disc_data_dir, mod_out_dir, staging, settings):
    print("=== Building Custom Car 'PHANTOM' ===")

    # Fail before publishing any package if required retail resources are absent.
    for required in ["CAR/" + car_archive_filename(settings["base"]), "RES/COMMON.RES", "RES/CT.RES", "RES/UICAR.RES"]:
        if not (Path(disc_data_dir) / required).is_file():
            raise FileNotFoundError(str(Path(disc_data_dir) / required))

    # 1. Decompile base car
    algar_car = os.path.join(disc_data_dir, "CAR", car_archive_filename(settings["base"]))
    car_staging = os.path.join(staging, "car_files")
    unpack_archive(algar_car, car_staging)

    # 2. Generate custom textures
    print("Generating custom livery textures (Phantom.tex / Phantom.tm0)...")
    tex256 = create_phantom_texture(256, 128, settings["body_color"], settings["stripe_style"], settings["number"])
    tex512 = create_phantom_texture(512, 256, settings["body_color"], settings["stripe_style"], settings["number"])
    phantom_tex_bytes = compile_tex(256, 128, tex256, header_word5=0x100)
    phantom_tm0_bytes = compile_tex(512, 256, tex512, header_word5=0x100)

    with open(os.path.join(car_staging, settings["name"] + ".tex"), "wb") as f:
        f.write(phantom_tex_bytes)
    with open(os.path.join(car_staging, settings["name"] + ".tm0"), "wb") as f:
        f.write(phantom_tm0_bytes)

    # 3. Retexture 3D model parts
    print("Retexturing 3D model meshes...")
    body_names = body_texture_names(settings["base"], asset_path(car_staging, "Low1.mod").read_bytes())
    # Prince retail distant LODs reuse Arrow body geometry/materials.
    extra_names = ("arrow.tex",) if settings["base"].lower() == "prince" else ()
    for required in ("Low1.mod", "Low2.mod", "Low3.mod", "Low4.mod"):
        asset_path(car_staging, required)
    lod_paths = sorted(p for p in Path(car_staging).iterdir() if re.fullmatch(r"low[0-9]+\.mod", p.name, re.IGNORECASE))
    for mod_path in lod_paths:
        mod_name = mod_path.name
        with open(mod_path, "rb") as f:
            mod_data = f.read()
        mod_updated = mod_data
        for old_texture in body_names + extra_names:
            mod_updated = retexture_modl(mod_updated, old_texture, settings["name"] + ".tex")
        if mod_updated == mod_data and mod_name.lower() in ("low1.mod", "low2.mod", "low3.mod", "low4.mod"):
            raise ValueError(f"No body material replaced in {mod_name}; refusing an unpainted package")
        with open(mod_path, "wb") as f:
            f.write(mod_updated)

    # 4. Customize physics specs
    print("Modifying performance specs in physics.val...")
    phys_path = os.path.join(car_staging, "physics.val")
    with open(phys_path, "rb") as f:
        phys_model = parse_binary_valt(f.read())

    for sec in phys_model["sections"]:
        if sec["name"] == "Chassis":
            new_pairs = []
            for k, v in sec["pairs"]:
                if k == "mass":
                    new_pairs.append((k, settings["mass"]))
                elif k == "damage_scale":
                    new_pairs.append((k, settings["damage_scale"]))
                elif k == "drag":
                    new_pairs.append((k, "0.24"))
                elif k == "grip":
                    new_pairs.append((k, settings["grip"]))
                else:
                    new_pairs.append((k, v))
            sec["pairs"] = new_pairs
        elif sec["name"] == "EngineNormal":
            new_pairs = []
            for k, v in sec["pairs"]:
                if k == "power_max":
                    new_pairs.append((k, settings["power_max"]))
                elif k == "torque_max":
                    new_pairs.append((k, settings["torque_max"]))
                elif k == "redline":
                    new_pairs.append((k, settings["redline"]))
                else:
                    new_pairs.append((k, v))
            sec["pairs"] = new_pairs

    phys_bytes = compile_binary_valt(phys_model)
    with open(phys_path, "wb") as f:
        f.write(phys_bytes)

    # Update manifest for car
    manifest_path = os.path.join(car_staging, "manifest.json")
    with open(manifest_path, "r") as f:
        car_manifest = json.load(f)

    # Add Phantom.tex and Phantom.tm0 to car archive
    car_manifest["entries"].append({
        "name": settings["name"] + ".tex",
        "fourcc": "TEX",
        "version": 10,
        "size": len(phantom_tex_bytes),
        "is_extractable": False,
    })
    car_manifest["entries"].append({
        "name": settings["name"] + ".tm0",
        "fourcc": "TEX",
        "version": 10,
        "size": len(phantom_tm0_bytes),
        "is_extractable": True,
    })
    # Reset exact offsets since sizes changed
    for em in car_manifest["entries"]:
        em.pop("orig_offset", None)
        em.pop("offset", None)

    with open(manifest_path, "w") as f:
        json.dump(car_manifest, f, indent=2)

    # Pack PHANTOM.CAR
    mod_car_dir = os.path.join(mod_out_dir, "DATA", "CAR")
    os.makedirs(mod_car_dir, exist_ok=True)
    out_phantom_car = os.path.join(mod_car_dir, settings["name"].upper() + ".CAR")
    pack_archive(car_staging, out_phantom_car)
    print(f"Successfully created {out_phantom_car}")

    # 5. Modify COMMON.RES
    print("Modifying COMMON.RES (car classes, rosters, dealer, stats)...")
    common_staging = os.path.join(staging, "common_res")
    unpack_archive(os.path.join(disc_data_dir, "RES", "COMMON.RES"), common_staging)

    roster = parse_binary_valt((Path(common_staging) / "dirt.val").read_bytes())
    dealer = parse_binary_valt((Path(common_staging) / "buycar.val").read_bytes())
    used = {dict(sec["pairs"]).get("number") for table in (roster, dealer) for sec in table["sections"]}
    if settings["number"] in used or str(int(settings["number"]) + 1) in used:
        raise ValueError("Racing number or adjacent dealer number is already used; choose another number")

    # 5a. carclass.val: Add Phantom
    cc_path = os.path.join(common_staging, "carclass.val")
    with open(cc_path, "rb") as f:
        cc_model = parse_binary_valt(f.read())
    for sec in cc_model["sections"]:
        if sec["name"] in [f"${c:04d}$chassis" for c in settings["classes"]]:  # Compact, Midsize & Muscle
            has_p = any(k.lower() == settings["name"].lower() for k, v in sec["pairs"])
            if not has_p:
                sec["pairs"].append((settings["name"], ""))
    with open(cc_path, "wb") as f:
        f.write(compile_binary_valt(cc_model))

    # 5b. car_stats.val: Add Phantom
    cs_path = os.path.join(common_staging, "car_stats.val")
    with open(cs_path, "rb") as f:
        cs_model = parse_binary_valt(f.read())
    has_cs = any(any(k == "chassis" and v.lower() == settings["name"].lower() for k, v in s["pairs"]) for s in cs_model["sections"])
    if not has_cs:
        cs_model["sections"].append({
            "name": "carstats",
            "parent": "",
            "sec_vtable": 0x40C0D8,
            "tool_ptr": 0,
            "pairs": [
                ("chassis", settings["name"]),
                ("durability", "8"),
                ("handling", "8"),
                ("speed", "9"),
            ],
        })
    with open(cs_path, "wb") as f:
        f.write(compile_binary_valt(cs_model))

    # 5c. buycar.val: Add Phantom to dealer
    bc_path = os.path.join(common_staging, "buycar.val")
    with open(bc_path, "rb") as f:
        bc_model = parse_binary_valt(f.read())
    bc_model["sections"].append({
        "name": "car",
        "parent": "",
        "sec_vtable": 0x40C0D8,
        "tool_ptr": 0,
        "pairs": [
            ("chassis", settings["name"]),
            ("desc", settings["description"]),
            ("price", settings["price"]),
        ],
    })
    bc_model["sections"].append({
        "name": "vehicle",
        "parent": "",
        "sec_vtable": 0x40C0D8,
        "tool_ptr": 0,
        "pairs": [
            ("body_color", "20, 20, 20"),
            ("chassis", settings["name"]),
            ("group_1", settings["number"]),
            ("number", str(int(settings["number"]) + 1)),
            ("price", settings["price"]),
        ],
    })
    with open(bc_path, "wb") as f:
        f.write(compile_binary_valt(bc_model))

    # 5d. dirt.val: Add Phantom to driver roster & assign to AI racers
    dirt_path = os.path.join(common_staging, "dirt.val")
    with open(dirt_path, "rb") as f:
        dirt_model = parse_binary_valt(f.read())
    # Add new Phantom driver
    dirt_model["sections"].append({
        "name": "Driver",
        "parent": "",
        "sec_vtable": 0x40C0D8,
        "tool_ptr": 0,
        "pairs": [
            ("body_color", "0xFF101010"),
            ("chassis", settings["name"]),
            ("group_1", settings["number"]),
            ("name", settings["name"] + " Racer"),
            ("number", settings["number"]),
            ("rank", "500"),
        ],
    })
    # Update AI racers Sam Tallman and Dale Jefferson
    for sec in dirt_model["sections"]:
        if sec["name"] == "Driver":
            dname = dict(sec["pairs"]).get("name", "")
            if dname in ["Sam Tallman", "Dale Jefferson"]:
                new_pairs = []
                for k, v in sec["pairs"]:
                    if k == "chassis":
                        new_pairs.append((k, settings["name"]))
                    elif k == "group_1":
                        new_pairs.append((k, settings["number"]))
                    else:
                        new_pairs.append((k, v))
                sec["pairs"] = new_pairs
    with open(dirt_path, "wb") as f:
        f.write(compile_binary_valt(dirt_model))

    # Repack COMMON.RES
    common_manifest_path = os.path.join(common_staging, "manifest.json")
    with open(common_manifest_path, "r") as f:
        c_manifest = json.load(f)
    for em in c_manifest["entries"]:
        em.pop("orig_offset", None)
        em.pop("offset", None)
    with open(common_manifest_path, "w") as f:
        json.dump(c_manifest, f, indent=2)

    mod_res_dir = os.path.join(mod_out_dir, "DATA", "RES")
    os.makedirs(mod_res_dir, exist_ok=True)
    out_common_res = os.path.join(mod_res_dir, "COMMON.RES")
    pack_archive(common_staging, out_common_res)
    print(f"Successfully created {out_common_res}")

    # 6. Add custom livery to CT.RES
    print("Modifying CT.RES (custom livery 77.tex / 77.tm0)...")
    ct_staging = os.path.join(staging, "ct_res")
    unpack_archive(os.path.join(disc_data_dir, "RES", "CT.RES"), ct_staging)

    with open(os.path.join(ct_staging, settings["number"] + ".tex"), "wb") as f:
        f.write(phantom_tex_bytes)
    with open(os.path.join(ct_staging, settings["number"] + ".tm0"), "wb") as f:
        f.write(phantom_tm0_bytes)

    ct_manifest_path = os.path.join(ct_staging, "manifest.json")
    with open(ct_manifest_path, "r") as f:
        ct_manifest = json.load(f)
    ct_manifest["entries"] = [e for e in ct_manifest["entries"] if e["name"].lower() not in (settings["number"] + ".tex", settings["number"] + ".tm0")]
    ct_manifest["entries"].append({
        "name": settings["number"] + ".tex",
        "fourcc": "TEX",
        "version": 10,
        "size": len(phantom_tex_bytes),
        "is_extractable": False,
    })
    ct_manifest["entries"].append({
        "name": settings["number"] + ".tm0",
        "fourcc": "TEX",
        "version": 10,
        "size": len(phantom_tm0_bytes),
        "is_extractable": True,
    })
    for em in ct_manifest["entries"]:
        em.pop("orig_offset", None)
        em.pop("offset", None)
    with open(ct_manifest_path, "w") as f:
        json.dump(ct_manifest, f, indent=2)

    out_ct_res = os.path.join(mod_res_dir, "CT.RES")
    pack_archive(ct_staging, out_ct_res)
    print(f"Successfully created {out_ct_res}")

    # 7. Add custom UI selection thumbnail to UICAR.RES
    print("Modifying UICAR.RES (selection menu thumbnail uicar_77.tex / uicar_77.tm0)...")
    uicar_staging = os.path.join(staging, "uicar_res")
    unpack_archive(os.path.join(disc_data_dir, "RES", "UICAR.RES"), uicar_staging)

    ui16 = create_uicar_thumbnail(16, 16)
    ui_tex_bytes = compile_tex(16, 16, ui16, fmt=0x201, flags=0x320600)
    ui256 = create_phantom_texture(256, 256, settings["body_color"], settings["stripe_style"], settings["number"])
    ui_tm0_bytes = compile_tex(256, 256, ui256, fmt=0x201, flags=0x320600)

    # Native CarChooseWidget enumerates paint IDs from every driver/dealer
    # assigned to this chassis, not only the new driver's chosen number.
    # Reassigned retail AI keep their unique numbers and need matching previews.
    preview_numbers = sorted({str(dict(sec["pairs"])["number"])
        for table in (dirt_model, bc_model) for sec in table["sections"]
        if dict(sec["pairs"]).get("chassis", "").lower() == settings["name"].lower()
        and "number" in dict(sec["pairs"])})
    uicar_manifest_path = os.path.join(uicar_staging, "manifest.json")
    with open(uicar_manifest_path, "r") as f:
        uicar_manifest = json.load(f)
    preview_names = {f"uicar_{number}.{suffix}" for number in preview_numbers for suffix in ("tex", "tm0")}
    uicar_manifest["entries"] = [entry for entry in uicar_manifest["entries"]
                                  if entry["name"].lower() not in preview_names]
    for number in preview_numbers:
        for suffix, content, extractable in (("tex", ui_tex_bytes, False), ("tm0", ui_tm0_bytes, True)):
            name = f"uicar_{number}.{suffix}"
            (Path(uicar_staging) / name).write_bytes(content)
            uicar_manifest["entries"].append(dict(name=name, fourcc="TEX", version=10,
                                                size=len(content), is_extractable=extractable))
    for em in uicar_manifest["entries"]:
        em.pop("orig_offset", None)
        em.pop("offset", None)
    with open(uicar_manifest_path, "w") as f:
        json.dump(uicar_manifest, f, indent=2)

    out_uicar_res = os.path.join(mod_res_dir, "UICAR.RES")
    pack_archive(uicar_staging, out_uicar_res)
    print(f"Successfully created {out_uicar_res}")

    print("=== Custom Car 'PHANTOM' Complete! ===")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disc-data", default=str(Path.home() / "Library/Application Support/TDEODRecomp/disc/DATA"))
    parser.add_argument("--output", default="mods/phantom")
    parser.add_argument("--name", default="Phantom")
    parser.add_argument("--base", default="Algar")
    parser.add_argument("--class", dest="car_class", choices=("compact", "midsize", "muscle"), default="midsize")
    args = parser.parse_args()
    build_custom_car(args.disc_data, args.output, name=args.name, base=args.base, classes=[("compact", "midsize", "muscle").index(args.car_class)])


if __name__ == "__main__":
    main()
