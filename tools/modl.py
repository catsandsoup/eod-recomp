#!/usr/bin/env python3
"""MODL (3D Model v9) parser and manipulator for Test Drive: Eve of Destruction.

Inspects, decompiles, and updates PS2 SPS4/VIF mesh models (.mod).
Supports retexturing and metadata/bounding-box inspection. OBJ export contains
only the model bounds; VIF mesh decoding and geometry compilation are not implemented.
"""

import argparse
import json
import os
import struct
import sys


def parse_modl(data: bytes) -> dict:
    """Parses a MODL v9 file."""
    if len(data) < 0x58:
        raise ValueError(f"MODL data too small: {len(data)} bytes")

    sub_ver, hdr_size, unk1, data_sz, part_slots, unk3 = struct.unpack("<IIIIII", data[:24])
    radius, cx, cy, cz = struct.unpack("<ffff", data[24:40])
    bbox = struct.unpack("<ffffff", data[40:64])

    # Part offsets table starts at 0x58
    if 0x58 + part_slots * 4 > len(data):
        raise ValueError("Truncated MODL part table")
    offsets = struct.unpack(f"<{part_slots}I", data[0x58 : 0x58 + part_slots * 4])

    parts = []
    for i, poff in enumerate(offsets):
        if poff == 0xE3E3E3E3 or poff + 28 > len(data):
            parts.append({
                "index": i,
                "valid": False,
                "offset": poff,
            })
            continue

        magic = data[poff : poff + 4].decode("latin1", errors="replace")
        part_size = struct.unpack("<I", data[poff + 4 : poff + 8])[0]
        tex_raw = data[poff + 8 : poff + 24].split(b"\0")[0]
        tex_name = tex_raw.decode("latin1", errors="replace")
        group_id = struct.unpack("<I", data[poff + 24 : poff + 28])[0]

        parts.append({
            "index": i,
            "valid": True,
            "offset": poff,
            "size": part_size,
            "magic": magic,
            "texture": tex_name,
            "group_id": group_id,
        })

    return {
        "sub_version": sub_ver,
        "header_size": hdr_size,
        "part_slots": part_slots,
        "radius": radius,
        "center": [cx, cy, cz],
        "bbox": list(bbox),
        "parts": parts,
    }


def retexture_modl(data: bytes, old_tex: str, new_tex: str) -> bytes:
    """Replaces occurrences of old_tex with new_tex in all 4SPS parts."""
    parse_modl(data)
    if not new_tex or len(new_tex.encode("latin1")) > 15 or "\0" in new_tex:
        raise ValueError("Texture name must fit the 16-byte null-terminated MODL field")
    out = bytearray(data)
    part_slots = struct.unpack("<I", data[0x10:0x14])[0]
    if 0x58 + part_slots * 4 > len(data):
        raise ValueError("Truncated MODL part table")
    offsets = struct.unpack(f"<{part_slots}I", data[0x58 : 0x58 + part_slots * 4])

    old_bytes = old_tex.lower().encode("latin1")
    new_bytes = new_tex.encode("latin1")[:16].ljust(16, b"\0")

    replacements = 0
    for poff in offsets:
        if poff == 0xE3E3E3E3 or poff + 24 > len(data):
            continue
        magic = data[poff : poff + 4]
        if magic != b"4SPS":
            continue
        current_tex_raw = data[poff + 8 : poff + 24]
        current_tex_str = current_tex_raw.split(b"\0")[0].lower()
        if current_tex_str == old_bytes or (old_tex == "*" and current_tex_str):
            out[poff + 8 : poff + 24] = new_bytes
            replacements += 1

    print(f"Retextured {replacements} parts: {old_tex} -> {new_tex}")
    return bytes(out)


def export_obj(mod_path: str, obj_path: str):
    """Exports MODL 3D structure and bounding geometry to Wavefront OBJ."""
    with open(mod_path, "rb") as f:
        data = f.read()

    model = parse_modl(data)
    valid_parts = [p for p in model["parts"] if p["valid"]]

    lines = [
        f"# MODL v{model['sub_version']} Export: {os.path.basename(mod_path)}",
        f"# Valid Parts: {len(valid_parts)}",
        "# BOUNDING BOX ONLY: no VIF mesh vertices decoded",
    ]

    # Write bounding box cube for whole model
    b = model["bbox"]
    min_x, min_y, min_z, max_x, max_y, max_z = b[0], b[1], b[2], b[3], b[4], b[5]
    v_base = 1

    corners = [
        (min_x, min_y, min_z),
        (max_x, min_y, min_z),
        (max_x, max_y, min_z),
        (min_x, max_y, min_z),
        (min_x, min_y, max_z),
        (max_x, min_y, max_z),
        (max_x, max_y, max_z),
        (min_x, max_y, max_z),
    ]

    for c in corners:
        lines.append(f"v {c[0]:.4f} {c[1]:.4f} {c[2]:.4f}")

    lines.append("g model_bounds")
    faces = [
        (1, 2, 3, 4),
        (5, 8, 7, 6),
        (1, 5, 6, 2),
        (2, 6, 7, 3),
        (3, 7, 8, 4),
        (5, 1, 4, 8),
    ]
    for face in faces:
        lines.append(f"f {face[0]} {face[1]} {face[2]} {face[3]}")
    v_base += 8

    # Write each part group
    for p in valid_parts:
        tex = p["texture"] or "untextured"
        lines.append(f"g part_{p['index']}_{p['magic']}_{tex.split('.')[0]}")
        lines.append(f"usemtl {tex}")

    os.makedirs(os.path.dirname(os.path.abspath(obj_path)), exist_ok=True)
    with open(obj_path, "w") as f:
        f.write("\n".join(lines) + "\n")

    print(f"Exported {mod_path} -> {obj_path} ({len(valid_parts)} parts)")


def export_json(mod_path: str, json_path: str):
    """Exports MODL structure to JSON."""
    with open(mod_path, "rb") as f:
        data = f.read()
    m = parse_modl(data)
    os.makedirs(os.path.dirname(os.path.abspath(json_path)), exist_ok=True)
    with open(json_path, "w") as f:
        json.dump(m, f, indent=2)
    print(f"Exported {mod_path} -> {json_path}")


def inspect_file(mod_path: str):
    """Prints a summary of a MODL file."""
    with open(mod_path, "rb") as f:
        data = f.read()
    model = parse_modl(data)
    valid_parts = [p for p in model["parts"] if p["valid"]]
    textures = set(p["texture"] for p in valid_parts if p["texture"])
    print(f"MODL {mod_path}:")
    print(f"  Size: {len(data)} bytes, Slots: {model['part_slots']}, Valid parts: {len(valid_parts)}")
    print(f"  Bounding sphere: center={model['center']}, radius={model['radius']:.3f}")
    print(f"  Textures referenced ({len(textures)}): {sorted(list(textures))}")


def main():
    parser = argparse.ArgumentParser(description="MODL 3D model tool")
    subparsers = parser.add_subparsers(dest="cmd", required=True)

    p_inspect = subparsers.add_parser("inspect", help="Inspect .mod file")
    p_inspect.add_argument("input", help="Input .mod file")

    p_retex = subparsers.add_parser("retexture", help="Retexture .mod parts")
    p_retex.add_argument("input", help="Input .mod file")
    p_retex.add_argument("output", help="Output .mod file")
    p_retex.add_argument("--old-tex", required=True, help="Old texture name to match (or * for all car liveries)")
    p_retex.add_argument("--new-tex", required=True, help="New texture name (e.g. Phantom.tex)")

    p_obj = subparsers.add_parser("export-obj", help="Export .mod to Wavefront OBJ")
    p_obj.add_argument("input", help="Input .mod file")
    p_obj.add_argument("output", help="Output .obj file")

    p_json = subparsers.add_parser("export-json", help="Export .mod hierarchy to JSON")
    p_json.add_argument("input", help="Input .mod file")
    p_json.add_argument("output", help="Output .json file")

    args = parser.parse_args()
    if args.cmd == "inspect":
        inspect_file(args.input)
    elif args.cmd == "retexture":
        with open(args.input, "rb") as f:
            data = f.read()
        out_data = retexture_modl(data, args.old_tex, args.new_tex)
        with open(args.output, "wb") as f:
            f.write(out_data)
    elif args.cmd == "export-obj":
        export_obj(args.input, args.output)
    elif args.cmd == "export-json":
        with open(args.input, "rb") as f:
            data = f.read()
        m = parse_modl(data)
        with open(args.output, "w") as f:
            json.dump(m, f, indent=2)
        print(f"Exported {args.input} -> {args.output}")


if __name__ == "__main__":
    main()
