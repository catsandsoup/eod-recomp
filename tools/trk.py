#!/usr/bin/env python3
"""Track archive, geometry, collision, and AI tool for Test Drive: Eve of Destruction.

Supported inspection and serialization tools:
1. Track container archives (.trk <-> directory with rst0).
2. AI racing lines and splines (.ild <-> JSON / Wavefront OBJ line tracks).
3. Unverified fixed-stride SOL record inspection (.sol -> JSON / OBJ values).
4. Track retexturing and asset manipulation.
"""

import argparse
import json
import math
import os
import struct
import sys
try:
    from rst0 import unpack_archive, pack_archive
except ImportError:
    from tools.rst0 import unpack_archive, pack_archive


def decompile_ild(ild_path: str) -> dict:
    """Decompiles an ILIN (*.ild) AI racing line container into a dict."""
    with open(ild_path, "rb") as f:
        data = f.read()

    if len(data) < 48:
        raise ValueError(f"{ild_path} too small for ILIN header")

    u0, hdr_sz, node_sz = struct.unpack("<III", data[:12])
    bbox = struct.unpack("<3f", data[12:24])
    curv, flags, nodes_off, total_nodes, unk2, node_count = struct.unpack("<fIIIII", data[24:48])

    if node_sz <= 0 or node_sz % 4:
        raise ValueError("ILIN node size must be a positive multiple of four")
    if not 48 <= nodes_off <= len(data) or (nodes_off - 48) % 4:
        raise ValueError("Invalid ILIN node offset/pointer table")
    ptr_count = (nodes_off - 48) // 4
    ptrs = list(struct.unpack(f"<{ptr_count}I", data[48:nodes_off]))
    payload = data[nodes_off:]

    # Parse node floats
    nodes = []
    num_floats_per_node = node_sz // 4
    actual_nodes = len(payload) // node_sz
    for i in range(actual_nodes):
        off = i * node_sz
        floats = struct.unpack(f"<{num_floats_per_node}f", payload[off : off + node_sz])
        nodes.append(list(floats))

    trailing = payload[actual_nodes * node_sz :]

    return {
        "unknown0": u0,
        "pointer_count": node_count,
        "header_size": hdr_sz,
        "node_size": node_sz,
        "bbox": list(bbox),
        "curvature_scale": curv,
        "flags": flags,
        "nodes_offset": nodes_off,
        "total_nodes": total_nodes,
        "unk2": unk2,
        "pointers": ptrs,
        "nodes": nodes,
        "node_hex": [payload[i * node_sz:(i + 1) * node_sz].hex() for i in range(actual_nodes)],
        "trailing_hex": trailing.hex() if trailing else "",
    }


def compile_ild(model: dict) -> bytes:
    """Compiles an ILIN model dict back into binary .ild format."""
    hdr_sz = model.get("header_size", 48)
    node_sz = model.get("node_size", 60)
    bbox = model.get("bbox", [0.0, 0.0, 0.0])
    curv = model.get("curvature_scale", 1.0)
    flags = model.get("flags", 0x800000)
    nodes = model.get("nodes", [])
    ptrs = model.get("pointers", [])
    nodes_off = model.get("nodes_offset", 48 + len(ptrs) * 4)
    total_nodes = model.get("total_nodes", len(nodes))
    unk2 = model.get("unk2", 0)

    if node_sz <= 0 or node_sz % 4:
        raise ValueError("ILIN node size must be a positive multiple of four")
    if nodes_off != 48 + len(ptrs) * 4:
        raise ValueError("ILIN node offset must immediately follow the pointer table")
    if any(len(node) != node_sz // 4 for node in nodes):
        raise ValueError("Every ILIN node must match node_size exactly")
    out = bytearray()
    hdr = struct.pack(
        "<III3ffIIIII",
        model.get("unknown0", 0), hdr_sz, node_sz, *bbox,
        curv, flags, nodes_off, total_nodes, unk2, model.get("pointer_count", len(ptrs))
    )
    out.extend(hdr)
    out.extend(struct.pack(f"<{len(ptrs)}I", *ptrs))

    if len(out) < nodes_off:
        out.extend(b"\0" * (nodes_off - len(out)))

    num_floats = node_sz // 4
    for index, n in enumerate(nodes):
        originals = model.get("node_hex", [])
        raw = bytes.fromhex(originals[index]) if index < len(originals) else None
        if raw is not None and len(raw) == node_sz:
            original_values = struct.unpack(f"<{num_floats}f", raw)
            if all(a == b or (math.isnan(a) and math.isnan(b)) for a, b in zip(n, original_values)):
                # Nodes mix floats and packed pointers/metadata. Preserve exact
                # bits (including NaN payloads) for unchanged opaque records.
                out.extend(raw)
                continue
        out.extend(struct.pack(f"<{num_floats}f", *n))

    trailing_hex = model.get("trailing_hex", "")
    if trailing_hex:
        out.extend(bytes.fromhex(trailing_hex))

    return bytes(out)


def decompile_nascar_ild(ild_bytes: bytes) -> dict:
    """Inspect candidate 76-byte NASCAR records; semantic fields are unknown."""
    if len(ild_bytes) < 16:
        raise ValueError("NASCAR ILIN data too short")

    u0, ptrs_off, node_sz = struct.unpack("<III", ild_bytes[:12])
    if node_sz != 76:
        raise ValueError(f"Expected 76-byte NASCAR records, got {node_sz}")
    if not 12 <= ptrs_off <= len(ild_bytes) or ptrs_off % 4:
        raise ValueError("Invalid NASCAR record offset")
    if (len(ild_bytes) - ptrs_off) % node_sz:
        raise ValueError("Truncated NASCAR node records")
    num_nodes = (len(ild_bytes) - ptrs_off) // node_sz

    nodes = []
    num_floats = node_sz // 4
    for i in range(num_nodes):
        noff = ptrs_off + i * node_sz
        floats = struct.unpack(f"<{num_floats}f", ild_bytes[noff : noff + node_sz])
        nodes.append(list(floats))

    return {
        "format": "nascar_v1",
        "node_size": node_sz,
        "pointers_offset": ptrs_off,
        "node_count": len(nodes),
        "nodes": nodes,
    }


def convert_nascar_ild_to_eod(nascar_ild_bytes: bytes) -> bytes:
    """Serialize an EXPERIMENTAL 19-to-15-float candidate, for research only.

    Field semantics, pointer layout, units and flags are not verified. This
    truncation does not establish a drivable track or safe AI navigation.
    """
    nascar_model = decompile_nascar_ild(nascar_ild_bytes)
    nascar_nodes = nascar_model["nodes"]

    eod_nodes = []
    for n in nascar_nodes:
        # Map 19-float NASCAR node into 15-float EOD node
        eod_n = list(n[:15]) if len(n) >= 15 else list(n) + [0.0] * (15 - len(n))
        eod_nodes.append(eod_n)

    eod_model = {
        "header_size": 48,
        "node_size": 60,
        "bbox": [1500.0, 1500.0, 1500.0],
        "curvature_scale": 0.05,
        "flags": 0x800000,
        "pointers": [i * 60 for i in range(len(eod_nodes))],
        "nodes": eod_nodes,
    }
    return compile_ild(eod_model)


def export_ild_to_obj(model: dict, obj_path: str):
    """Export the known EOD X/Z centerline as a planar OBJ line strip."""
    nodes = model.get("nodes", [])
    lines = [f"# ILIN Racing Line Export ({len(nodes)} waypoints)"]
    for i, n in enumerate(nodes):
        if len(n) < 2:
            raise ValueError("ILIN node has no planar position")
        # EOD 60-byte nodes store X/Z at +0/+4, followed by tangent.
        # See src/battle_navigation.cpp; no elevation is inferred here.
        lines.append(f"v {n[0]:.4f} 0.0000 {n[1]:.4f}")

    lines.append("g racing_line")
    idx_strs = [str(i + 1) for i in range(len(nodes))]
    if idx_strs:
        lines.append("l " + " ".join(idx_strs))

    os.makedirs(os.path.dirname(os.path.abspath(obj_path)), exist_ok=True)
    with open(obj_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"Exported {len(nodes)} waypoints -> {obj_path}")


def decompile_sol(sol_path: str) -> dict:
    """Experimental 64-byte record view; SOBL surface semantics are unverified."""
    with open(sol_path, "rb") as f:
        data = f.read()

    if len(data) < 16:
        raise ValueError(f"{sol_path} too small for SOBL collision header")

    rec_count, unk1, unk2, unk3 = struct.unpack("<IIII", data[:16])
    stride = 64
    boxes = []
    total_recs = min(rec_count, (len(data) - 16) // stride)

    for i in range(total_recs):
        off = 16 + i * stride
        chunk = data[off : off + stride]
        if len(chunk) < stride:
            break
        floats = struct.unpack("<16f", chunk)
        boxes.append({
            "index": i,
            "planes": [list(floats[p * 4 : (p + 1) * 4]) for p in range(3)],
            "extents": list(floats[12:16]),
        })

    return {
        "interpretation": "unverified fixed-stride record view, not collision geometry",
        "record_count": rec_count,
        "stride": stride,
        "boxes": boxes,
    }


def export_sol_to_obj(sol_model: dict, obj_path: str):
    """Export experimental record values; these are not verified collision centers."""
    boxes = sol_model.get("boxes", [])
    lines = [f"# UNVERIFIED SOL record values ({len(boxes)} candidates); not collision geometry"]
    for b in boxes:
        ext = b.get("extents", [0, 0, 0, 0])
        lines.append(f"v {ext[0]:.3f} {ext[1]:.3f} {ext[2]:.3f}")

    os.makedirs(os.path.dirname(os.path.abspath(obj_path)), exist_ok=True)
    with open(obj_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"Exported {len(boxes)} unverified record values -> {obj_path}")


def retexture_track_dir(track_dir: str, old_tex: str, new_tex: str):
    """Retexture known MODL part fields; opaque track.grf files stay untouched."""
    try:
        from modl import retexture_modl
    except ImportError:
        from tools.modl import retexture_modl
    if not old_tex:
        raise ValueError("Source texture name must not be empty")
    count = 0
    for root, _, files in os.walk(track_dir):
        for name in files:
            if not name.lower().endswith(".mod"):
                continue
            path = os.path.join(root, name)
            with open(path, "rb") as handle:
                original = handle.read()
            updated = retexture_modl(original, old_tex, new_tex)
            if updated != original:
                with open(path, "wb") as handle:
                    handle.write(updated)
                count += 1
    print(f"Retextured {count} MODL files; opaque GRF layout is not implemented")


def inspect_track(trk_path: str):
    """Inspects a track archive and its entries."""
    with open(trk_path, "rb") as f:
        data = f.read()

    magic = data[:4]
    if magic != b"0TSR":
        raise ValueError(f"{trk_path} is not a 0TSR track archive")

    hdr_size, version, total_size = struct.unpack("<III", data[4:16])
    build_time, dir_offset, entry_count = struct.unpack("<III", data[16:28])

    entries = []
    for i in range(entry_count):
        e_bytes = data[dir_offset + i * 0x44 : dir_offset + (i + 1) * 0x44]
        name = e_bytes[:32].split(b"\0")[0].decode("latin1")
        fourcc = e_bytes[0x20:0x24][::-1].decode("latin1", errors="replace")
        c_ver, size, offset, crc = struct.unpack("<IIII", e_bytes[0x24:0x34])
        entries.append((name, fourcc, size))

    ild_lines = [name for name, fcc, sz in entries if name.endswith(".ild")]
    tex_files = [name for name, fcc, sz in entries if fcc.strip() == "TEX"]
    mod_files = [name for name, fcc, sz in entries if name.endswith(".mod")]

    print(f"Track Archive {trk_path}:")
    print(f"  Size: {total_size} bytes, Entries: {entry_count}, Version: {version}")
    print(f"  Textures: {len(tex_files)}, Props/Models: {len(mod_files)}")
    print(f"  AI Racing lines ({len(ild_lines)}): {ild_lines}")


def main():
    parser = argparse.ArgumentParser(description="Track archive & asset pipeline for Test Drive: Eve of Destruction")
    subparsers = parser.add_subparsers(dest="cmd", required=True)

    # unpack
    p_unp = subparsers.add_parser("unpack", help="Unpack .trk archive to directory")
    p_unp.add_argument("input", help="Input .trk archive")
    p_unp.add_argument("out_dir", help="Output directory")

    # pack
    p_pck = subparsers.add_parser("pack", help="Pack directory to .trk archive")
    p_pck.add_argument("in_dir", help="Input directory")
    p_pck.add_argument("output", help="Output .trk archive")

    # export-ai
    p_eai = subparsers.add_parser("export-ai", help="Export .ild AI racing line to JSON and OBJ")
    p_eai.add_argument("input", help="Input .ild file")
    p_eai.add_argument("out_json", help="Output .json file")
    p_eai.add_argument("--obj", default=None, help="Optional output .obj file for 3D visualization")

    # import-ai
    p_iai = subparsers.add_parser("import-ai", help="Compile JSON back into binary .ild file")
    p_iai.add_argument("in_json", help="Input .json file")
    p_iai.add_argument("output", help="Output .ild file")

    # export-sol
    p_esol = subparsers.add_parser("export-sol", help="Inspect unverified fixed-stride SOL records (not collision geometry)")
    p_esol.add_argument("input", help="Input track.sol file")
    p_esol.add_argument("out_json", help="Output .json file")
    p_esol.add_argument("--obj", default=None, help="Optional output .obj file")

    # retexture
    p_ret = subparsers.add_parser("retexture", help="Retexture track models and scenery")
    p_ret.add_argument("track_dir", help="Unpacked track directory")
    p_ret.add_argument("--old-tex", required=True, help="Old texture name")
    p_ret.add_argument("--new-tex", required=True, help="New texture name")

    # inspect
    p_ins = subparsers.add_parser("inspect", help="Inspect .trk archive")
    p_ins.add_argument("input", help="Input .trk file")

    args = parser.parse_args()
    if args.cmd == "unpack":
        unpack_archive(args.input, args.out_dir)
    elif args.cmd == "pack":
        pack_archive(args.in_dir, args.output)
    elif args.cmd == "export-ai":
        m = decompile_ild(args.input)
        with open(args.out_json, "w") as fp:
            json.dump(m, fp, indent=2)
        print(f"Decompiled {args.input} -> {args.out_json} ({len(m['nodes'])} waypoints)")
        if args.obj:
            export_ild_to_obj(m, args.obj)
    elif args.cmd == "import-ai":
        with open(args.in_json, "r") as fp:
            m = json.load(fp)
        data = compile_ild(m)
        with open(args.output, "wb") as fp:
            fp.write(data)
        print(f"Compiled {args.in_json} -> {args.output} ({len(data)} bytes)")
    elif args.cmd == "export-sol":
        m = decompile_sol(args.input)
        with open(args.out_json, "w") as fp:
            json.dump(m, fp, indent=2)
        print(f"Inspected {args.input} -> {args.out_json} ({len(m['boxes'])} unverified records)")
        if args.obj:
            export_sol_to_obj(m, args.obj)
    elif args.cmd == "retexture":
        retexture_track_dir(args.track_dir, args.old_tex, args.new_tex)
    elif args.cmd == "inspect":
        inspect_track(args.input)


if __name__ == "__main__":
    main()
