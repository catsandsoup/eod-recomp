#!/usr/bin/env python3
"""0TSR archive container unpacker and repacker for Test Drive: Eve of Destruction (PS2 / Xbox).

Handles .car, .res, and .trk files.
Computes bit-exact CRCs using the game's polynomial and table at f_00135FC8.
"""

import base64
import hashlib
import argparse
import json
import os
import struct
import sys

# Precomputed CRC table from SLUS_209.10 at 0x003F2598 (polynomial 0x04C11DB7, MSB-first)
CRC_TABLE = [
    0x00000000, 0x04C11DB7, 0x09823B6E, 0x0D4326D9, 0x130476DC, 0x17C56B6B, 0x1A864DB2, 0x1E475005,
    0x2608EDB8, 0x22C9F00F, 0x2F8AD6D6, 0x2B4BCB61, 0x350C9B64, 0x31CD86D3, 0x3C8EA00A, 0x384FBDBD,
    0x4C11DB70, 0x48D0C6C7, 0x4593E01E, 0x4152FDA9, 0x5F15ADAC, 0x5BD4B01B, 0x569796C2, 0x52568B75,
    0x6A1936C8, 0x6ED82B7F, 0x639B0DA6, 0x675A1011, 0x791D4014, 0x7DDC5DA3, 0x709F7B7A, 0x745E66CD,
    0x9823B6E0, 0x9CE2AB57, 0x91A18D8E, 0x95609039, 0x8B27C03C, 0x8FE6DD8B, 0x82A5FB52, 0x8664E6E5,
    0xBE2B5B58, 0xBAEA46EF, 0xB7A96036, 0xB3687D81, 0xAD2F2D84, 0xA9EE3033, 0xA4AD16EA, 0xA06C0B5D,
    0xD4326D90, 0xD0F37027, 0xDDB056FE, 0xD9714B49, 0xC7361B4C, 0xC3F706FB, 0xCEB42022, 0xCA753D95,
    0xF23A8028, 0xF6FB9D9F, 0xFBB8BB46, 0xFF79A6F1, 0xE13EF6F4, 0xE5FFEB43, 0xE8BCCD9A, 0xEC7DD02D,
    0x34867077, 0x30476DC0, 0x3D044B19, 0x39C556AE, 0x278206AB, 0x23431B1C, 0x2E003DC5, 0x2AC12072,
    0x128E9DCF, 0x164F8078, 0x1B0CA6A1, 0x1FCDBB16, 0x018AEB13, 0x054BF6A4, 0x0808D07D, 0x0CC9CDCA,
    0x7897AB07, 0x7C56B6B0, 0x71159069, 0x75D48DDE, 0x6B93DDDB, 0x6F52C06C, 0x6211E6B5, 0x66D0FB02,
    0x5E9F46BF, 0x5A5E5B08, 0x571D7DD1, 0x53DC6066, 0x4D9B3063, 0x495A2DD4, 0x44190B0D, 0x40D816BA,
    0xACA5C697, 0xA864DB20, 0xA527FDF9, 0xA1E6E04E, 0xBFA1B04B, 0xBB60ADFC, 0xB6238B25, 0xB2E29692,
    0x8AAD2B2F, 0x8E6C3698, 0x832F1041, 0x87EE0DF6, 0x99A95DF3, 0x9D684044, 0x902B669D, 0x94EA7B2A,
    0xE0B41DE7, 0xE4750050, 0xE9362689, 0xEDF73B3E, 0xF3B06B3B, 0xF771768C, 0xFA325055, 0xFEF34DE2,
    0xC6BCF05F, 0xC27DEDE8, 0xCF3ECB31, 0xCBFFD686, 0xD5B88683, 0xD1799B34, 0xDC3ABDED, 0xD8FBA05A,
    0x690CE0EE, 0x6DCDFD59, 0x608EDB80, 0x644FC637, 0x7A089632, 0x7EC98B85, 0x738AAD5C, 0x774BB0EB,
    0x4F040D56, 0x4BC510E1, 0x46863638, 0x42472B8F, 0x5C007B8A, 0x58C1663D, 0x558240E4, 0x51435D53,
    0x251D3B9E, 0x21DC2629, 0x2C9F00F0, 0x285E1D47, 0x36194D42, 0x32D850F5, 0x3F9B762C, 0x3B5A6B9B,
    0x0315D626, 0x07D4CB91, 0x0A97ED48, 0x0E56F0FF, 0x1011A0FA, 0x14D0BD4D, 0x19939B94, 0x1D528623,
    0xF12F560E, 0xF5EE4BB9, 0xF8AD6D60, 0xFC6C70D7, 0xE22B20D2, 0xE6EA3D65, 0xEBA91BBC, 0xEF68060B,
    0xD727BBB6, 0xD3E6A601, 0xDEA580D8, 0xDA649D6F, 0xC423CD6A, 0xC0E2D0DD, 0xCDA1F604, 0xC960EBB3,
    0xBD3E8D7E, 0xB9FF90C9, 0xB4BCB610, 0xB07DABA7, 0xAE3AFBA2, 0xAAFBE615, 0xA7B8C0CC, 0xA379DD7B,
    0x9B3660C6, 0x9FF77D71, 0x92B45BA8, 0x9675461F, 0x8832161A, 0x8CF30BAD, 0x81B02D74, 0x857130C3,
    0x5D8A9099, 0x594B8D2E, 0x5408ABF7, 0x50C9B640, 0x4E8EE645, 0x4A4FFBF2, 0x470CDD2B, 0x43CDC09C,
    0x7B827D21, 0x7F436096, 0x7200464F, 0x76C15BF8, 0x68860BFD, 0x6C47164A, 0x61043093, 0x65C52D24,
    0x119B4BE9, 0x155A565E, 0x18197087, 0x1CD86D30, 0x029F3D35, 0x065E2082, 0x0B1D065B, 0x0FDC1BEC,
    0x3793A651, 0x3352BBE6, 0x3E119D3F, 0x3AD08088, 0x2497D08D, 0x2056CD3A, 0x2D15EBE3, 0x29D4F654,
    0xC5A92679, 0xC1683BCE, 0xCC2B1D17, 0xC8EA00A0, 0xD6AD50A5, 0xD26C4D12, 0xDF2F6BCB, 0xDBEE767C,
    0xE3A1CBC1, 0xE760D676, 0xEA23F0AF, 0xEEE2ED18, 0xF0A5BD1D, 0xF464A0AA, 0xF9278673, 0xFDE69BC4,
    0x89B8FD09, 0x8D79E0BE, 0x803AC667, 0x84FBDBD0, 0x9ABC8BD5, 0x9E7D9662, 0x933EB0BB, 0x97FFAD0C,
    0xAFB010B1, 0xAB710D06, 0xA6322BDF, 0xA2F33668, 0xBCB4666D, 0xB8757BDA, 0xB5365D03, 0xB1F740B4,
]


def eod_crc(data: bytes) -> int:
    """Computes the game's exact CRC32 value."""
    if len(data) < 4:
        c = 0xFFFFFFFF
        i = 0
    else:
        w = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3]
        c = (~w) & 0xFFFFFFFF
        i = 4
    for b in data[i:]:
        idx = (c >> 24) & 0xFF
        c = (((c << 8) | b) ^ CRC_TABLE[idx]) & 0xFFFFFFFF
    return (~c) & 0xFFFFFFFF


def unpack_archive(archive_path: str, out_dir: str):
    """Unpacks a 0TSR archive into out_dir."""
    with open(archive_path, "rb") as f:
        data = f.read()

    magic = data[0:4]
    if magic != b"0TSR":
        raise ValueError(f"Not a 0TSR archive: {magic}")

    first_u32 = struct.unpack("<I", data[4:8])[0]
    if first_u32 == 2:
        # 0TSR v2 (NASCAR: Dirt to Daytona)
        version = 2
        dir_offset = 0x18
        total_size, build_time, entry_count = struct.unpack("<III", data[12:24])
        extract_block = 0
        unk = 0
        hdr_crc = 0
        print(f"Detected 0TSR v2 archive (build_time={build_time}, {entry_count} entries)")

        os.makedirs(out_dir, exist_ok=True)
        manifest = {
            "version": version,
            "build_time": build_time,
            "extract_block": extract_block,
            "unk": unk,
            "entries": [],
        }

        dir_end = dir_offset + entry_count * 0x38
        cur_pos = data.find(b"!IGM", dir_end)
        if cur_pos == -1:
            cur_pos = ((dir_end - 0x14 + 0x7F) & ~0x7F) + 0x14

        for i in range(entry_count):
            e_bytes = data[dir_offset + i * 0x38 : dir_offset + (i + 1) * 0x38]
            name = e_bytes[:36].split(b"\0")[0].decode("latin1")
            fourcc = e_bytes[0x24:0x28][::-1].decode("latin1", errors="replace")
            c_ver, size = struct.unpack("<II", e_bytes[0x28:0x30])

            if cur_pos != -1 and cur_pos < len(data) and data[cur_pos : cur_pos + 4] == b"!IGM":
                # Strip 4-byte container magic for direct tool consumption
                entry_data = data[cur_pos + 4 : cur_pos + size]
                has_igm = True
            else:
                entry_data = data[cur_pos : cur_pos + size]
                has_igm = False

            if not name or name in (".", "..", "manifest.json") or "/" in name or "\\" in name:
                raise ValueError(f"Unsafe archive entry name: {name!r}")
            out_file = os.path.join(out_dir, name)
            with open(out_file, "wb") as f_out:
                f_out.write(entry_data)

            manifest["entries"].append({
                "name": name,
                "fourcc": fourcc,
                "version": c_ver,
                "size": len(entry_data),
                "orig_size": size,
                "offset": cur_pos,
                "is_extractable": True,
                "has_igm": has_igm,
                "crc": 0,
            })

            # Next entry location: find next !IGM marker after this entry
            next_pos = data.find(b"!IGM", cur_pos + size)
            if next_pos != -1:
                cur_pos = next_pos
            else:
                cur_pos += (size + 0x7F) & ~0x7F

        with open(os.path.join(out_dir, "manifest.json"), "w") as f_m:
            json.dump(manifest, f_m, indent=2)

        print(f"Unpacked {entry_count} entries from {archive_path} to {out_dir}")
        return

    # 0TSR v6 (Test Drive: Eve of Destruction)
    if len(data) < 0x28:
        raise ValueError("Truncated 0TSR header")
    hdr_size, version, total_size = struct.unpack("<III", data[4:16])
    build_time, dir_offset, entry_count = struct.unpack("<III", data[16:28])
    extract_block, unk = struct.unpack("<II", data[28:36])
    hdr_crc = struct.unpack("<I", data[0x24:0x28])[0]

    if version != 6 or dir_offset + entry_count * 0x44 > len(data):
        raise ValueError("Unsupported or truncated 0TSR directory")

    calc_hdr_crc = eod_crc(data[:0x24])
    if calc_hdr_crc != hdr_crc:
        print(f"Warning: Header CRC mismatch ({hex(calc_hdr_crc)} != {hex(hdr_crc)})")

    os.makedirs(out_dir, exist_ok=True)
    manifest = {
        "version": version,
        "build_time": build_time,
        "extract_block": extract_block,
        "unk": unk,
        "entries": [],
    }

    for i in range(entry_count):
        e_bytes = data[dir_offset + i * 0x44 : dir_offset + (i + 1) * 0x44]
        name = e_bytes[:32].split(b"\0")[0].decode("latin1")
        fourcc = e_bytes[0x20:0x24][::-1].decode("latin1", errors="replace")
        c_ver, size, offset, crc = struct.unpack("<IIII", e_bytes[0x24:0x34])

        if dir_offset + offset + size > len(data):
            raise ValueError(f"Truncated archive entry: {name}")
        entry_data = data[dir_offset + offset : dir_offset + offset + size]
        calc_crc = eod_crc(entry_data)
        if calc_crc != crc:
            print(f"Warning: CRC mismatch on {name} ({hex(calc_crc)} != {hex(crc)})")

        if not name or name in (".", "..", "manifest.json") or "/" in name or "\\" in name:
            raise ValueError(f"Unsafe archive entry name: {name!r}")
        out_file = os.path.join(out_dir, name)
        with open(out_file, "wb") as f_out:
            f_out.write(entry_data)

        manifest["entries"].append({
            "name": name,
            "fourcc": fourcc,
            "version": c_ver,
            "size": size,
            "offset": offset,
            "is_extractable": (offset >= extract_block),
            "crc": crc,
        })

    # Preserve original header, directory, padding and trailing bytes without
    # duplicating asset payloads. Unedited extraction/repack can be byte exact.
    ranges = sorted((dir_offset + e["offset"], dir_offset + e["offset"] + e["size"])
                    for e in manifest["entries"])
    gaps, cursor = [], 0
    for start, end in ranges:
        if start > cursor:
            gaps.append([cursor, base64.b64encode(data[cursor:start]).decode("ascii")])
        cursor = max(cursor, end)
    if cursor < len(data):
        gaps.append([cursor, base64.b64encode(data[cursor:]).decode("ascii")])
    manifest["original_layout"] = dict(version=version, build_time=build_time, unk=unk, extract_block=extract_block, size=len(data), dir_offset=dir_offset, gaps=gaps,
        entries=[dict(entry, sha256=hashlib.sha256(data[dir_offset + entry["offset"]:dir_offset + entry["offset"] + entry["size"]]).hexdigest())
                 for entry in manifest["entries"]])

    with open(os.path.join(out_dir, "manifest.json"), "w") as f_m:
        json.dump(manifest, f_m, indent=2)

    print(f"Unpacked {entry_count} entries from {archive_path} to {out_dir}")


def pack_archive(in_dir: str, out_archive_path: str):
    """Packs directory in_dir into a 0TSR archive."""
    manifest_path = os.path.join(in_dir, "manifest.json")
    if not os.path.exists(manifest_path):
        raise FileNotFoundError(f"Missing manifest.json in {in_dir}")

    with open(manifest_path, "r") as f:
        manifest = json.load(f)

    version = manifest.get("version", 6)
    build_time = manifest.get("build_time", 1089733550)
    unk = manifest.get("unk", 0)
    entries_meta = manifest.get("entries", [])

    if version != 6:
        raise ValueError("Packing supports 0TSR v6 only; v2 conversion is not a lossless repack")
    names = [e["name"] for e in entries_meta]
    if len(set(n.lower() for n in names)) != len(names):
        raise ValueError("Duplicate archive entry names")
    for name in names:
        if not name or len(name.encode("latin1")) > 31 or "\0" in name or name in (".", "..", "manifest.json") or "/" in name or "\\" in name:
            raise ValueError(f"Invalid archive entry name: {name!r}")

    layout = manifest.get("original_layout")
    if layout and len(entries_meta) == len(layout["entries"]):
        unchanged = True
        payloads = []
        for current, original in zip(entries_meta, layout["entries"]):
            with open(os.path.join(in_dir, current["name"]), "rb") as handle:
                payload = handle.read()
            payloads.append(payload)
            if (any(current.get(key) != original.get(key) for key in
                    ("name", "fourcc", "version", "size", "offset", "is_extractable")) or
                    hashlib.sha256(payload).hexdigest() != original["sha256"]):
                unchanged = False
        if unchanged and all(manifest.get(k) == layout.get(k, manifest.get(k)) for k in ("version", "build_time", "unk", "extract_block")):
            output = bytearray(layout["size"])
            for offset, encoded in layout["gaps"]:
                gap = base64.b64decode(encoded)
                output[offset:offset + len(gap)] = gap
            for entry, payload in zip(layout["entries"], payloads):
                offset = layout["dir_offset"] + entry["offset"]
                output[offset:offset + len(payload)] = payload
            os.makedirs(os.path.dirname(os.path.abspath(out_archive_path)), exist_ok=True)
            with open(out_archive_path, "wb") as handle:
                handle.write(output)
            print(f"Packed original layout byte-for-byte into {out_archive_path}")
            return

    entry_count = len(entries_meta)
    dir_offset = 0x80
    dir_size = entry_count * 0x44
    first_data_offset = (dir_offset + dir_size + 0x7F) & ~0x7F

    resident_entries = []
    extractable_entries = []
    for em in entries_meta:
        name = em["name"]
        file_path = os.path.join(in_dir, name)
        with open(file_path, "rb") as f:
            content = f.read()
        crc = eod_crc(content)
        entry_record = {
            "name": name,
            "fourcc": em["fourcc"],
            "version": em.get("version", 1),
            "data": content,
            "size": len(content),
            "crc": crc,
            "is_extractable": em.get("is_extractable", False),
            "orig_offset": em.get("offset"),
        }
        if entry_record["is_extractable"]:
            extractable_entries.append(entry_record)
        else:
            resident_entries.append(entry_record)

    all_entries = resident_entries + extractable_entries

    cur_file_offset = first_data_offset
    first_extractable_rel_offset = None

    for i, e in enumerate(all_entries):
        # Recompute all positions: preserving offsets after edits overlaps entries.
        cur_file_offset = (cur_file_offset + 0x7F) & ~0x7F
        rel_offset = cur_file_offset - dir_offset
        if i < len(all_entries) - 1:
            cur_file_offset += ((e["size"] + 0x80) & ~0x7F)
        else:
            cur_file_offset += e["size"]
        e["offset"] = rel_offset
        if e["is_extractable"] and first_extractable_rel_offset is None:
            first_extractable_rel_offset = rel_offset

    total_file_size = cur_file_offset
    if first_extractable_rel_offset is None:
        first_extractable_rel_offset = total_file_size - dir_offset

    # Build Header (bytes 0x00 to 0x24)
    hdr_size = 0x28
    header_pre = struct.pack(
        "<4sIIIIIIII",
        b"0TSR",
        hdr_size,
        version,
        total_file_size,
        build_time,
        dir_offset,
        entry_count,
        first_extractable_rel_offset,
        unk,
    )
    hdr_crc = eod_crc(header_pre)
    header = header_pre + struct.pack("<I", hdr_crc)

    # Pad header up to dir_offset (0x80)
    out_bytes = bytearray()
    out_bytes.extend(header)
    if len(out_bytes) < dir_offset:
        out_bytes.extend(b"\x00" * (dir_offset - len(out_bytes)))

    # Write directory entries
    for e in all_entries:
        name_bytes = e["name"].encode("latin1")[:32].ljust(32, b"\0")
        fcc = e.get("fourcc", "")
        fcc_padded = fcc.ljust(4, " ")[:4]
        fourcc_bytes = fcc_padded[::-1].encode("latin1")
        entry_head = name_bytes + fourcc_bytes
        entry_info = struct.pack(
            "<IIII4I",
            e["version"],
            e["size"],
            e["offset"],
            e["crc"],
            0, 0, 0, 0,
        )
        out_bytes.extend(entry_head + entry_info)

    # Write data blocks with 0x80 alignment
    for e in all_entries:
        target_file_off = dir_offset + e["offset"]
        if len(out_bytes) < target_file_off:
            out_bytes.extend(b"\x00" * (target_file_off - len(out_bytes)))
        out_bytes.extend(e["data"])

    # Final padding to total_file_size
    if len(out_bytes) < total_file_size:
        out_bytes.extend(b"\x00" * (total_file_size - len(out_bytes)))

    os.makedirs(os.path.dirname(os.path.abspath(out_archive_path)), exist_ok=True)
    with open(out_archive_path, "wb") as f_out:
        f_out.write(out_bytes)

    print(f"Packed {len(all_entries)} entries into {out_archive_path} ({len(out_bytes)} bytes)")


def main():
    parser = argparse.ArgumentParser(description="0TSR archive packer/unpacker")
    subparsers = parser.add_subparsers(dest="cmd", required=True)

    p_unpack = subparsers.add_parser("unpack", help="Unpack archive to directory")
    p_unpack.add_argument("archive", help="Input 0TSR archive file")
    p_unpack.add_argument("outdir", help="Output directory")

    p_pack = subparsers.add_parser("pack", help="Pack directory to archive")
    p_pack.add_argument("indir", help="Input directory")
    p_pack.add_argument("archive", help="Output 0TSR archive file")

    args = parser.parse_args()
    if args.cmd == "unpack":
        unpack_archive(args.archive, args.outdir)
    elif args.cmd == "pack":
        pack_archive(args.indir, args.archive)


if __name__ == "__main__":
    main()
