#!/usr/bin/env python3
"""VALT key/value table compiler and decompiler for Test Drive: Eve of Destruction.

Handles .val, .va0, .mvl, and .pln files.
Converts between binary VALT format and human-readable text / JSON.
"""

import argparse
import json
import os
import re
import struct
import sys


def parse_binary_valt(data: bytes) -> dict:
    """Parses binary VALT data into a Python dictionary. Supports both VALT v11 and VALT v6."""
    if len(data) < 16:
        raise ValueError(f"VALT data too short ({len(data)} bytes)")

    # Detect VALT v6 (NASCAR: Dirt to Daytona) vs VALT v11 (Test Drive: Eve of Destruction)
    svt_candidate = struct.unpack("<I", data[12:16])[0]
    name_candidate = data[16:20]
    is_v6 = (svt_candidate == 0x40A0E4 or (len(name_candidate) > 0 and 32 <= name_candidate[0] < 127))

    def decode_str(off):
        if off == 0 or off >= len(data):
            return ""
        end = data.find(b"\0", off)
        if end == -1:
            end = len(data)
        chunk = data[off:end]
        if chunk and chunk[0] & 0x80:
            return bytes([b ^ 0xA5 for b in chunk]).decode("latin1", errors="replace")
        return chunk.decode("latin1", errors="replace")

    if is_v6:
        vt, sec_count, unk1 = struct.unpack("<III", data[:12])
        sections = []
        for i in range(sec_count):
            soff = 12 + i * 0x50
            if soff + 0x50 > len(data):
                raise ValueError("Truncated VALT v6 section table")
            svt = struct.unpack("<I", data[soff : soff + 4])[0]
            sname = decode_str(soff + 4)
            npairs, pairs_off = struct.unpack("<II", data[soff + 0x24 : soff + 0x2C])
            pairs = []
            for p in range(npairs):
                poff = pairs_off + p * 8
                if poff + 8 > len(data):
                    raise ValueError("Truncated VALT v6 pair table")
                koff, voff = struct.unpack("<II", data[poff : poff + 8])
                pairs.append((decode_str(koff), decode_str(voff)))
            sections.append({
                "name": sname,
                "parent": "",
                "pairs": pairs,
                "sec_vtable": svt,
                "tool_ptr": 0,
            })
        return {
            "tool_vtable": vt,
            "unk1": unk1,
            "orig_sec_count": sec_count,
            "sections": sections,
            "is_v6": True,
        }

    vt, sec_count, unk1, unk2 = struct.unpack("<IIII", data[0:16])

    def get_str(off):
        if off == 0 or off >= len(data):
            return ""
        end = data.find(b"\0", off)
        if end == -1:
            end = len(data)
        return data[off:end].decode("latin1", errors="replace")

    sections = []
    for i in range(sec_count):
        soff = 16 + i * 28
        if soff + 28 > len(data):
            raise ValueError("Truncated VALT section table")
        svt, noff, poff, npairs, pairs_off, unk, tool_ptr = struct.unpack(
            "<IIIIIII", data[soff : soff + 28]
        )
        sname = get_str(noff)
        sparent = get_str(poff)
        pairs = []
        for p in range(npairs):
            poff_i = pairs_off + p * 8
            if poff_i + 8 > len(data):
                raise ValueError("Truncated VALT pair table")
            koff, voff = struct.unpack("<II", data[poff_i : poff_i + 8])
            pairs.append((get_str(koff), get_str(voff)))

        sections.append({
            "name": sname,
            "parent": sparent,
            "pairs": pairs,
            "sec_vtable": svt,
            "tool_ptr": tool_ptr,
        })

    return {
        "tool_vtable": vt,
        "unk1": unk1,
        "orig_sec_count": sec_count,
        "sections": sections,
        "is_v6": False,
    }


def compile_binary_valt(model: dict) -> bytes:
    """Compiles a VALT model dict into binary VALT format."""
    if model.get("is_v6"):
        raise ValueError("VALT v6 compilation is not supported; convert explicitly before targeting v11")
    sections = model.get("sections", [])
    vt = model.get("tool_vtable", 0x40C218)
    sec_count = len(sections)
    orig_sec_count = model.get("orig_sec_count", sec_count)
    orig_u1 = model.get("unk1", sec_count)

    # u1 is the root/accessible section count for get_section(index) bounds checking.
    # If all sections were root sections, u1 == sec_count.
    # If there were sub-sections (e.g. anonymous sub-tables), preserve the delta.
    if "root_count" in model:
        u1 = model["root_count"]
        if not isinstance(u1, int) or isinstance(u1, bool) or not 0 <= u1 <= sec_count:
            raise ValueError("root_count must be an integer within the section table")
    elif orig_u1 == sec_count or orig_u1 == orig_sec_count:
        u1 = sec_count
    else:
        delta = sec_count - orig_sec_count
        u1 = max(1, orig_u1 + delta)

    header_size = 16 + sec_count * 28
    total_pairs = sum(len(s.get("pairs", [])) for s in sections)
    strings_start = header_size + total_pairs * 8

    string_pool = bytearray()
    string_map = {}

    def add_str(s):
        if not s:
            return 0
        if s in string_map:
            return string_map[s]
        off = strings_start + len(string_pool)
        string_map[s] = off
        string_pool.extend(s.encode("latin1") + b"\0")
        return off

    sec_headers = []
    pairs_bytes = bytearray()

    for s in sections:
        noff = add_str(s.get("name", ""))
        poff = add_str(s.get("parent", ""))
        p_offset = header_size + len(pairs_bytes)
        pairs_list = s.get("pairs", [])
        npairs = len(pairs_list)
        svt = s.get("sec_vtable", 0x40C0D8)
        tptr = s.get("tool_ptr", 0)
        sec_headers.append((svt, noff, poff, npairs, p_offset, 0, tptr))

        for k, v in pairs_list:
            koff = add_str(str(k))
            voff = add_str(str(v))
            pairs_bytes.extend(struct.pack("<II", koff, voff))

    out = bytearray()
    out.extend(struct.pack("<IIII", vt, sec_count, u1, 0))
    for sh in sec_headers:
        out.extend(struct.pack("<IIIIIII", *sh))
    out.extend(pairs_bytes)
    out.extend(string_pool)
    return bytes(out)


def valt_to_text(model: dict) -> str:
    """Converts a VALT model to human-readable text."""
    lines = []
    lines.append(f"; VALT tool_vtable={hex(model.get('tool_vtable', 0x40C218))} unk1={model.get('unk1', 4)}")
    for s in model.get("sections", []):
        name = s.get("name", "")
        parent = s.get("parent", "")
        if parent:
            lines.append(f"[{name}] : {parent}")
        else:
            lines.append(f"[{name}]")
        for k, v in s.get("pairs", []):
            lines.append(f"  {k} = {v}")
        lines.append("")
    return "\n".join(lines)


def text_to_valt(text: str) -> dict:
    """Parses text format back into a VALT model dict."""
    tool_vtable = 0x40C218
    unk1 = 4
    sections = []
    cur_sec = None

    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        if line.startswith(";"):
            m = re.search(r"tool_vtable=(0x[0-9a-fA-F]+|\d+)", line)
            if m:
                tool_vtable = int(m.group(1), 0)
            m = re.search(r"unk1=(\d+)", line)
            if m:
                unk1 = int(m.group(1))
            continue

        if line.startswith("[") and "]" in line:
            if cur_sec is not None:
                sections.append(cur_sec)
            header_part = line[1:].split("]", 1)[0].strip()
            rest = line[1:].split("]", 1)[1].strip()
            parent = ""
            if ":" in rest:
                parent = rest.split(":", 1)[1].strip()
            cur_sec = {
                "name": header_part,
                "parent": parent,
                "pairs": [],
                "sec_vtable": 0x40C0D8,
                "tool_ptr": 0,
            }
        elif "=" in line and cur_sec is not None:
            k, v = line.split("=", 1)
            cur_sec["pairs"].append((k.strip(), v.strip()))

    if cur_sec is not None:
        sections.append(cur_sec)

    return {
        "tool_vtable": tool_vtable,
        "unk1": unk1,
        "sections": sections,
    }


def decompile_file(in_path: str, out_path: str, as_json: bool = False):
    """Decompiles binary VALT to text or JSON."""
    with open(in_path, "rb") as f:
        data = f.read()
    model = parse_binary_valt(data)
    with open(out_path, "w") as f:
        if as_json:
            json.dump(model, f, indent=2)
        else:
            f.write(valt_to_text(model))
    print(f"Decompiled {in_path} -> {out_path} ({len(model['sections'])} sections)")


def compile_file(in_path: str, out_path: str):
    """Compiles text or JSON to binary VALT."""
    with open(in_path, "r") as f:
        content = f.read()
    if in_path.endswith(".json"):
        model = json.loads(content)
    else:
        model = text_to_valt(content)
    bin_data = compile_binary_valt(model)
    with open(out_path, "wb") as f:
        f.write(bin_data)
    print(f"Compiled {in_path} -> {out_path} ({len(bin_data)} bytes)")


def main():
    parser = argparse.ArgumentParser(description="VALT compiler / decompiler")
    subparsers = parser.add_subparsers(dest="cmd", required=True)

    p_dec = subparsers.add_parser("decompile", help="Decompile binary VALT to text/json")
    p_dec.add_argument("input", help="Input .val/.va0 file")
    p_dec.add_argument("output", help="Output .txt/.json file")
    p_dec.add_argument("--json", action="store_true", help="Output as JSON")

    p_com = subparsers.add_parser("compile", help="Compile text/json to binary VALT")
    p_com.add_argument("input", help="Input .txt/.json file")
    p_com.add_argument("output", help="Output .val/.va0 file")

    args = parser.parse_args()
    if args.cmd == "decompile":
        decompile_file(args.input, args.output, as_json=args.json)
    elif args.cmd == "compile":
        compile_file(args.input, args.output)


if __name__ == "__main__":
    main()
