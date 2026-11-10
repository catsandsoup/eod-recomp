#!/usr/bin/env python3
"""Track collision and boundary inspector/editor (track.sol SOBL, track.bsc SCOL).

Every layout here follows the retail SLUS_209.10 loaders and consumers; the
evidence ledger is docs/COLLISION_AND_BOUNDARIES.md. Summary:

* track.sol, member type SOBL, directory version 3. Loader f_001A00B8
  (called from world setup f_00176768 at 0x00176BC4; result stored in
  0x004473FC). Header: u32 record count, u32 index-list length, three runtime
  pointer words (zero in the file). Then count x 0x108-byte records, the u16
  index list, then 8-byte quadtree nodes {u16 first, u16 end, u16 child, u16 0}.
  Record: +0x00 3x3 float rows (local X/Y/Z axes in world space), +0x24 vec3
  centre, +0x30 s32 link (kept), +0x34 u32 surface_type, +0x38 u32 shape tag
  ('BOX ', 'TUBE', 'POLY', 'SPHR', 'RING'), +0x40 shape object built in place by
  f_001A18E8. BOX (ctor f_001A06D0, field names from serializer f_0019E820):
  shape+0x20 rx, +0x24 ry, +0x28 rz (half extents, metres), +0x2C sphere_radius
  (= |r| + 0.2 in every retail record), +0x30 box_flags. TUBE (ctor f_0019E368):
  shape+0x20 radius, +0x24 half_length along local Z, with two cached end
  spheres; read-only here.
  Broadphase f_00177CA0 converts each moving body's X/Z to decimetres and walks
  the quadtree from root bounds +-40,000,000 (child = 2*(Z>midZ) + (X>midX)),
  testing every static shape listed on the visited nodes. A second set built at
  run time from event props (SolBuilder::StaticObjectList, f_001C0878 ->
  f_00179AF8 -> 0x00447400) is tested the same way; it is not in this file.
* track.bsc, member type SCOL, directory version 4 (f_00226768 panics "bad
  scol version"; pointer 0x003D2098). Header u32 count, u32 table1 offset (12),
  u32 table2 offset. Table 1, 32 bytes per triangle: three edges
  {i16 nx, i16 nz, f32 d} (inside when nx*x+nz*z+d >= 0, n scaled 1/32767),
  three i16 neighbours (-1 = open edge), u16 optional-terrain bits
  (f_00227830; tested against the event mask 0x003D1C70 by f_00217240).
  Table 2, 20 bytes: plane a,b,c,d (height y = -(a*x + c*z + d)/b, f_00227AE0),
  i16 terrain surface id (+0x10, returned by f_00227878/f_00227950), two u8
  per-triangle bytes (+0x12/+0x13, f_002277E8; meaning unverified).

Units are metres, Y up, native world coordinates (no axis flips).
Pure Python, no third-party modules. Run with `python3 -I` on disc data.
"""
import argparse
import json
import math
import os
import re
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rst0 import eod_crc  # noqa: E402
from valt import parse_binary_valt  # noqa: E402

SOBL_RECORD = 0x108
SOBL_HEADER = 0x14
SCOL_TRI = 32
SCOL_PLANE = 20
EDGE_SCALE = 1.0 / 32767.0   # 0x38000100, f_00226F40
SPHERE_PAD = 0.2             # sphere_radius - |(rx, ry, rz)| in every retail BOX
QUAD_ROOT = 40000000         # decimetres, f_00177CA0
SOBL_VERSION = 3
SCOL_VERSION = 4

# Name -> id table at 0x004090F0 (eventwob "surf_type"); 1000 = record with no shape.
SURFACE_NAMES = {100: "default", 101: "concrete", 105: "metal", 106: "plastic",
                 107: "rubber", 108: "wood", 110: "wall", 111: "hay", 1000: "none", 0: "zero"}
SURFACE_COLOURS = {100: (0.7, 0.7, 0.7), 101: (0.75, 0.72, 0.65), 105: (0.55, 0.6, 0.75),
                   106: (0.9, 0.5, 0.9), 107: (0.2, 0.2, 0.2), 108: (0.6, 0.4, 0.2),
                   110: (0.85, 0.2, 0.15), 111: (0.9, 0.8, 0.3), 0: (0.4, 0.8, 0.8)}
TERRAIN_COLOURS = [(0.55, 0.5, 0.4), (0.35, 0.6, 0.3), (0.5, 0.5, 0.55), (0.7, 0.6, 0.4),
                   (0.45, 0.45, 0.6), (0.6, 0.45, 0.3), (0.62, 0.55, 0.42), (0.3, 0.45, 0.3)]

TRACK_NAMES = ("Afton Anoka Baxter Bemidji Dennison Dundas Eagan Gilroy Hastings "
               "HomeBase Kenyon Mankato Springfield Welch").split()
EVENT_POLYGON_SECTIONS = ("bound", "ringout", "safe_bound", "play_area",
                          "home_endzone", "away_endzone")
VERTICAL_LIMIT = 0.85        # |row1.y| needed for a box to count as a wall
EDITABLE_FIELDS = ("ry", "top-above-ground")
UNVERIFIED_FIELDS = {
    "rx": "box thickness: shrinking moves faces inward but no native test has shown the result",
    "rz": "box length: same as rx",
    "box_flags": "bit meaning not recovered from the narrow phase",
    "surface_type": "surface ids select sounds/response; not a height limit",
    "sphere_radius": "derived; recomputed automatically from rx/ry/rz",
    "tube": "TUBE end spheres cache local and world centres; consistent rewrite unverified",
    "bound": "event polygons are VALT strings; edit with modding_backend VALT edits",
    "dy": "obstacle.val prop dimensions: half/full extent not yet traced to the box",
}


def default_disc():
    env = os.environ.get("EOD_DISC")
    if env:
        return Path(env)
    data = os.environ.get("EOD_DISC_DATA")
    if data:
        return Path(data).parent
    return Path.home() / "Library/Application Support/TDEODRecomp/disc"


def disc_track_name(name):
    return name.upper() if len(name) <= 8 else name[:6].upper() + "_1"


def resolve_track(track, disc):
    path = Path(track)
    if path.suffix.upper() == ".TRK" and path.is_file():
        return path
    tracks = Path(disc) / "DATA" / "TRACKS"
    wanted = track.upper().removesuffix(".TRK")
    for name in TRACK_NAMES + ["EmptyTrack"]:
        if wanted in (name.upper(), disc_track_name(name)):
            wanted = disc_track_name(name)
            break
    candidate = tracks / (wanted + ".TRK")
    if not candidate.is_file():
        raise ValueError(f"Track archive not found: {candidate}")
    return candidate


def track_display_name(archive_path):
    stem = Path(archive_path).stem.upper()
    for name in TRACK_NAMES + ["EmptyTrack"]:
        if disc_track_name(name) == stem:
            return name
    return stem


# ---------------------------------------------------------------- archive ---

def read_directory(data):
    """0TSR v6 directory (same contract as modding_backend.directory)."""
    if len(data) < 40 or data[:4] != b"0TSR" or struct.unpack_from("<I", data, 8)[0] != 6:
        raise ValueError("Only native EOD 0TSR v6 archives are supported")
    if eod_crc(data[:36]) != struct.unpack_from("<I", data, 36)[0]:
        raise ValueError("Archive header CRC mismatch")
    offset, count = struct.unpack_from("<II", data, 20)
    if offset < 40 or offset + count * 68 > len(data):
        raise ValueError("Truncated archive directory")
    entries = []
    for i in range(count):
        row = offset + i * 68
        name = data[row:row + 32].split(b"\0")[0].decode("latin1")
        version, length, position, crc = struct.unpack_from("<IIII", data, row + 36)
        start = offset + position
        if start + length > len(data):
            raise ValueError("Truncated archive member: " + name)
        entries.append(dict(index=i, name=name, type=data[row + 32:row + 36][::-1].decode("latin1").strip(),
                            version=version, size=length, offset=start, crc=crc,
                            crc_field=row + 48))
    return entries


def member(data, entries, name, verify=True):
    matches = [e for e in entries if e["name"].lower() == name.lower()]
    if len(matches) != 1:
        raise ValueError("Archive member not found: " + name)
    e = matches[0]
    payload = data[e["offset"]:e["offset"] + e["size"]]
    if verify and eod_crc(payload) != e["crc"]:
        raise ValueError("Archive member CRC mismatch: " + name)
    return payload, e


# ------------------------------------------------------------------- SOBL ---

def _tag(word):
    return word.to_bytes(4, "big").decode("latin1")


def parse_sobl(data):
    if len(data) < SOBL_HEADER:
        raise ValueError("SOBL too short")
    count, list_len, p0, p1, p2 = struct.unpack_from("<5I", data, 0)
    list_off = SOBL_HEADER + count * SOBL_RECORD
    tree_off = list_off + list_len * 2
    if tree_off > len(data) or (len(data) - tree_off) % 8:
        raise ValueError("SOBL section sizes do not match the member length")
    if (p0, p1, p2) != (0, 0, 0):
        raise ValueError("SOBL runtime pointer words are not zero")
    records = []
    for i in range(count):
        off = SOBL_HEADER + i * SOBL_RECORD
        rows = struct.unpack_from("<9f", data, off)
        rec = dict(index=i, offset=off,
                   rows=[list(rows[0:3]), list(rows[3:6]), list(rows[6:9])],
                   centre=list(struct.unpack_from("<3f", data, off + 0x24)),
                   link=struct.unpack_from("<i", data, off + 0x30)[0],
                   surface_type=struct.unpack_from("<I", data, off + 0x34)[0],
                   tag=_tag(struct.unpack_from("<I", data, off + 0x38)[0]))
        rec["surface_name"] = SURFACE_NAMES.get(rec["surface_type"], "id%d" % rec["surface_type"])
        if rec["tag"] == "BOX ":
            rx, ry, rz, sr = struct.unpack_from("<4f", data, off + 0x60)
            rec.update(kind="box", half_extents=[rx, ry, rz], sphere_radius=sr,
                       box_flags=struct.unpack_from("<I", data, off + 0x70)[0])
        elif rec["tag"] == "TUBE":
            radius, half = struct.unpack_from("<2f", data, off + 0x60)
            rec.update(kind="tube", radius=radius, half_length=half)
        else:
            rec.update(kind="other")
        records.append(rec)
    index = list(struct.unpack_from("<%dH" % list_len, data, list_off))
    nodes = [struct.unpack_from("<4H", data, tree_off + 8 * i) for i in range((len(data) - tree_off) // 8)]
    if any(v >= max(count, 1) for v in index):
        raise ValueError("SOBL index list references a missing record")
    for first, end, child, pad in nodes:
        if first > end or end > list_len or (child and child + 3 >= len(nodes)):
            raise ValueError("SOBL quadtree node out of range")
    return dict(count=count, records=records, index=index, nodes=nodes,
                list_offset=list_off, tree_offset=tree_off, size=len(data))


def quadtree_cells(sobl):
    """Yield (node, x0, x1, z0, z1, depth) in metres, as walked by f_00177CA0."""
    out = []
    if not sobl["nodes"]:
        return out
    stack = [(0, -QUAD_ROOT, QUAD_ROOT, -QUAD_ROOT, QUAD_ROOT, 0)]
    while stack:
        idx, x0, x1, z0, z1, depth = stack.pop()
        out.append((idx, x0 / 10, x1 / 10, z0 / 10, z1 / 10, depth))
        child = sobl["nodes"][idx][2]
        if child:
            mx, mz = (x0 + x1) >> 1, (z0 + z1) >> 1
            stack += [(child, x0, mx, z0, mz, depth + 1), (child + 1, mx, x1, z0, mz, depth + 1),
                      (child + 2, x0, mx, mz, z1, depth + 1), (child + 3, mx, x1, mz, z1, depth + 1)]
    return out


def box_corners(rec, half=None):
    rx, ry, rz = half or rec["half_extents"]
    c, (a, b, d) = rec["centre"], rec["rows"]
    pts = []
    for sx in (-1, 1):
        for sy in (-1, 1):
            for sz in (-1, 1):
                pts.append([c[k] + sx * rx * a[k] + sy * ry * b[k] + sz * rz * d[k] for k in range(3)])
    return pts  # index = 4*(sx>0) + 2*(sy>0) + (sz>0)


BOX_EDGES = [(0, 1), (2, 3), (4, 5), (6, 7), (0, 2), (1, 3), (4, 6), (5, 7), (0, 4), (1, 5), (2, 6), (3, 7)]
BOX_FACES = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]


def footprint(rec):
    """World X/Z half-extent of a record (used for quadtree checks and rendering)."""
    rows = rec["rows"]
    if rec["kind"] == "box":
        e = rec["half_extents"]
        return (sum(abs(rows[k][0]) * e[k] for k in range(3)), sum(abs(rows[k][2]) * e[k] for k in range(3)))
    if rec["kind"] == "tube":
        return (abs(rows[2][0]) * rec["half_length"] + rec["radius"],
                abs(rows[2][2]) * rec["half_length"] + rec["radius"])
    return (0.0, 0.0)


def vertical_span(rec):
    rows, c = rec["rows"], rec["centre"]
    if rec["kind"] == "box":
        e = rec["half_extents"]
        h = sum(abs(rows[k][1]) * e[k] for k in range(3))
    elif rec["kind"] == "tube":
        h = abs(rows[2][1]) * rec["half_length"] + rec["radius"]
    else:
        h = 0.0
    return c[1] - h, c[1] + h


# ------------------------------------------------------------------- SCOL ---

def parse_scol(data):
    if len(data) < 12:
        raise ValueError("SCOL too short")
    count, t1, t2 = struct.unpack_from("<3I", data, 0)
    if t1 != 12 or t2 != t1 + count * SCOL_TRI or len(data) != t2 + count * SCOL_PLANE:
        raise ValueError("SCOL table sizes do not match the member length")
    tris = []
    for i in range(count):
        r, p = t1 + i * SCOL_TRI, t2 + i * SCOL_PLANE
        edges = [struct.unpack_from("<hhf", data, r + 8 * k) for k in range(3)]
        neighbours = list(struct.unpack_from("<3h", data, r + 24))
        flags = struct.unpack_from("<H", data, r + 30)[0]
        plane = struct.unpack_from("<4f", data, p)
        surface = struct.unpack_from("<h", data, p + 16)[0]
        aux = (data[p + 18], data[p + 19])
        verts = []
        for a, b in ((0, 1), (1, 2), (2, 0)):
            a1, b1, c1 = edges[a][0] * EDGE_SCALE, edges[a][1] * EDGE_SCALE, edges[a][2]
            a2, b2, c2 = edges[b][0] * EDGE_SCALE, edges[b][1] * EDGE_SCALE, edges[b][2]
            det = a1 * b2 - a2 * b1
            if abs(det) < 1e-12:
                raise ValueError("SCOL triangle %d has parallel edges" % i)
            x, z = (-c1 * b2 + c2 * b1) / det, (-a1 * c2 + a2 * c1) / det
            verts.append([x, -(plane[0] * x + plane[2] * z + plane[3]) / plane[1], z])
        tris.append(dict(index=i, edges=edges, neighbours=neighbours, flags=flags,
                         plane=plane, surface=surface, aux=aux, verts=verts))
    return dict(count=count, tris=tris)


class Terrain:
    """Point lookup equivalent to the native brute-force selector f_00226F40,
    accelerated with an X/Z grid. Where triangles overlap, the highest one not
    above `below` wins (closest ground under an object)."""

    def __init__(self, scol, cell=32.0):
        self.tris, self.cell, self.grid = scol["tris"], cell, {}
        for t in self.tris:
            xs = [v[0] for v in t["verts"]]
            zs = [v[2] for v in t["verts"]]
            for gx in range(int(math.floor(min(xs) / cell)), int(math.floor(max(xs) / cell)) + 1):
                for gz in range(int(math.floor(min(zs) / cell)), int(math.floor(max(zs) / cell)) + 1):
                    self.grid.setdefault((gx, gz), []).append(t)

    @staticmethod
    def inside(t, x, z):
        return min(nx * EDGE_SCALE * x + nz * EDGE_SCALE * z + d for nx, nz, d in t["edges"])

    @staticmethod
    def height(t, x, z):
        a, b, c, d = t["plane"]
        return -(a * x + c * z + d) / b

    def ground(self, x, z, below=None, opt_mask=None):
        best = None
        key = (int(math.floor(x / self.cell)), int(math.floor(z / self.cell)))
        for t in self.grid.get(key, ()):
            if opt_mask is not None and t["flags"] and not (t["flags"] & opt_mask):
                continue
            if self.inside(t, x, z) >= -1e-3:
                y = self.height(t, x, z)
                if below is not None and y > below + 0.5:
                    continue
                if best is None or y > best[0]:
                    best = (y, t["index"])
        if best:
            return dict(y=best[0], tri=best[1], inside=True)
        if not self.tris:
            return dict(y=0.0, tri=-1, inside=False)
        t = max(self.tris, key=lambda t: self.inside(t, x, z))
        return dict(y=self.height(t, x, z), tri=t["index"], inside=False)


# ------------------------------------------------------------------ track ---

def load_track(archive_path, disc=None):
    data = Path(archive_path).read_bytes()
    entries = read_directory(data)
    sol_raw, sol_entry = member(data, entries, "track.sol")
    bsc_raw, bsc_entry = member(data, entries, "track.bsc")
    if sol_entry["type"] != "SOBL" or sol_entry["version"] != SOBL_VERSION:
        raise ValueError("track.sol is not SOBL version 3")
    if bsc_entry["type"] != "SCOL" or bsc_entry["version"] != SCOL_VERSION:
        raise ValueError("track.bsc is not SCOL version 4")
    track = dict(path=str(archive_path), name=track_display_name(archive_path), data=data,
                 entries=entries, sol_entry=sol_entry, bsc_entry=bsc_entry,
                 sobl=parse_sobl(sol_raw), scol=parse_scol(bsc_raw))
    other = {}
    for name in ("track.bsp", "track.bpp"):
        try:
            raw, e = member(data, entries, name, verify=False)
            other[name] = dict(type=e["type"], version=e["version"], size=e["size"])
        except ValueError:
            pass
    track["opaque"] = other
    track["events"] = event_polygons(data, entries)
    track["props"] = event_wall_props(data, entries, disc)
    track["opt_terrain"] = opt_terrain_masks(track["name"], disc)
    return track


def _floats(text):
    return [float(v) for v in re.findall(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?", text)]


def event_polygons(data, entries):
    """bound/ringout style X/Z polygons from c_*.va0 (arenabnd, f_ strings 'bound%d')."""
    out = []
    for e in entries:
        if not (e["name"].lower().startswith("c_") and e["type"] == "VALT"):
            continue
        table = parse_binary_valt(data[e["offset"]:e["offset"] + e["size"]])
        for s in table["sections"]:
            if s["name"] not in EVENT_POLYGON_SECTIONS:
                continue
            pts = []
            for k, v in s["pairs"]:
                m = re.fullmatch(r"bound(\d+)", k)
                if m:
                    xz = _floats(v)
                    if len(xz) == 2:
                        pts.append((int(m.group(1)), xz))
            pts.sort()
            if pts:
                out.append(dict(event=e["name"], section=s["name"], points=[p for _, p in pts]))
    return out


def _valt_member(path, name):
    data = Path(path).read_bytes()
    entries = read_directory(data)
    raw, _ = member(data, entries, name, verify=False)
    return parse_binary_valt(raw)


def event_wall_props(data, entries, disc):
    """Static event props of type wall: dyno name -> dx/dy/dz and use counts."""
    defs = {}
    sources = []
    try:
        raw, _ = member(data, entries, "obstacle.val", verify=False)
        sources.append(("track obstacle.val", parse_binary_valt(raw)))
    except ValueError:
        pass
    race = Path(disc) / "DATA/RES/RACE.RES" if disc else None
    if race and race.is_file():
        sources.append(("RACE.RES obstacle.val", _valt_member(race, "obstacle.val")))
    for label, table in sources:
        by_name = {s["name"]: s for s in table["sections"]}

        def resolved(section, depth=0):
            values = {}
            parent = section.get("parent")
            if parent and parent in by_name and depth < 8:
                values.update(resolved(by_name[parent], depth + 1))
            values.update(dict(section["pairs"]))
            return values
        for s in table["sections"]:
            if s["name"] != "dyno":
                continue
            v = resolved(s)
            if v.get("type") == "wall" and "name" in v and v["name"] not in defs:
                defs[v["name"]] = dict(source=label, model=v.get("model", ""),
                                       dx=v.get("dx"), dy=v.get("dy"), dz=v.get("dz"),
                                       surf_type=v.get("surf_type", ""))
    uses = {}
    for e in entries:
        if not (e["name"].lower().startswith("c_") and e["type"] == "VALT"):
            continue
        table = parse_binary_valt(data[e["offset"]:e["offset"] + e["size"]])
        for s in table["sections"]:
            for k, v in s["pairs"]:
                if k == "dyno" and v in defs:
                    uses.setdefault(v, {}).setdefault(e["name"], 0)
                    uses[v][e["name"]] += 1
    return [dict(name=n, uses=uses.get(n, {}), **d) for n, d in sorted(defs.items())]


def opt_terrain_masks(track_name, disc):
    common = Path(disc) / "DATA/RES/COMMON.RES" if disc else None
    if not common or not common.is_file():
        return []
    try:
        table = _valt_member(common, track_name.lower() + "_eves.val")
    except ValueError:
        return []
    out = []
    for s in table["sections"]:
        for k, v in s["pairs"]:
            if k in ("opt_terrain", "terrain_mask"):
                out.append(dict(section=s["name"], key=k, value=v))
    return out


# --------------------------------------------------------------- analysis ---

def wall_heights(track, terrain=None):
    terrain = terrain or Terrain(track["scol"])
    out = []
    for rec in track["sobl"]["records"]:
        if rec["kind"] not in ("box", "tube"):
            continue
        lo, hi = vertical_span(rec)
        g = terrain.ground(rec["centre"][0], rec["centre"][2], below=rec["centre"][1])
        uy = abs(rec["rows"][1][1]) if rec["kind"] == "box" else abs(rec["rows"][2][1])
        out.append(dict(index=rec["index"], kind=rec["kind"], surface=rec["surface_name"],
                        centre=rec["centre"], bottom=lo, top=hi, ground=g["y"],
                        ground_inside=g["inside"], above_ground=hi - g["y"],
                        vertical=uy >= VERTICAL_LIMIT,
                        ry=rec.get("half_extents", [None, None])[1],
                        box_flags=rec.get("box_flags")))
    return out


def limits_report(track):
    heights = wall_heights(track)
    boxes = [h for h in heights if h["kind"] == "box"]
    tall = sorted((h for h in boxes if h["above_ground"] >= 5.0), key=lambda h: -h["above_ground"])
    ry_hist = {}
    for rec in track["sobl"]["records"]:
        if rec["kind"] == "box":
            key = "%.3f" % rec["half_extents"][1]
            ry_hist[key] = ry_hist.get(key, 0) + 1
    flags_hist = {}
    for t in track["scol"]["tris"]:
        flags_hist[t["flags"]] = flags_hist.get(t["flags"], 0) + 1
    return dict(
        track=track["name"], archive=track["path"],
        sobl=dict(records=track["sobl"]["count"], boxes=len(boxes),
                  tubes=sum(1 for h in heights if h["kind"] == "tube"),
                  box_half_height_histogram=dict(sorted(ry_hist.items(), key=lambda kv: -kv[1])[:8]),
                  walls_at_least_5m_above_ground=len(tall),
                  walls_at_least_10m_above_ground=sum(1 for h in tall if h["above_ground"] >= 10),
                  max_above_ground=max((h["above_ground"] for h in boxes), default=0.0),
                  tallest=[dict(record=h["index"], above_ground=round(h["above_ground"], 2),
                                top=round(h["top"], 2), ground=round(h["ground"], 2),
                                ry=round(h["ry"], 3), centre=[round(v, 2) for v in h["centre"]],
                                editable=h["vertical"]) for h in tall[:12]]),
        terrain=dict(triangles=track["scol"]["count"],
                     optional_terrain_bits=dict(sorted(flags_hist.items()))),
        event_polygons=[dict(event=p["event"], section=p["section"], vertices=len(p["points"]),
                             x=[round(min(q[0] for q in p["points"]), 2), round(max(q[0] for q in p["points"]), 2)],
                             z=[round(min(q[1] for q in p["points"]), 2), round(max(q[1] for q in p["points"]), 2)])
                        for p in track["events"]],
        event_wall_props=track["props"],
        opt_terrain=track["opt_terrain"],
        opaque=track["opaque"])


def print_limits(rep):
    s = rep["sobl"]
    print(f"{rep['track']}: {s['records']} SOBL records ({s['boxes']} boxes, {s['tubes']} tubes), "
          f"{rep['terrain']['triangles']} SCOL triangles")
    print(f"  Box half-height ry [m] (count): " +
          ", ".join(f"{k}x{v}" for k, v in s["box_half_height_histogram"].items()))
    print(f"  Static walls >=5 m above ground: {s['walls_at_least_5m_above_ground']}, "
          f">=10 m: {s['walls_at_least_10m_above_ground']}, max {s['max_above_ground']:.2f} m")
    for t in s["tallest"]:
        print(f"    record {t['record']:5d}  top {t['top']:8.2f}  ground {t['ground']:8.2f}  "
              f"+{t['above_ground']:6.2f} m  ry {t['ry']:.3f}  centre {t['centre']}"
              + ("" if t["editable"] else "  (tilted; not editable)"))
    print(f"  Optional-terrain triangle bits: {rep['terrain']['optional_terrain_bits']}")
    for m in rep["opt_terrain"]:
        print(f"    event {m['section']}: {m['key']} = {m['value']}")
    for p in rep["event_polygons"]:
        print(f"  {p['event']} [{p['section']}] {p['vertices']} vertices, X {p['x']}, Z {p['z']}")
    for prop in rep["event_wall_props"]:
        if prop["uses"]:
            print(f"  wall prop {prop['name']} ({prop['source']}): dx {prop['dx']} dy {prop['dy']} "
                  f"dz {prop['dz']}  used in {', '.join(f'{k} x{v}' for k, v in sorted(prop['uses'].items()))}")


# ------------------------------------------------------------- write-back ---

def _parse_records(spec, count):
    out = []
    for part in str(spec).split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-", 1)
            out += list(range(int(a), int(b) + 1))
        else:
            out.append(int(part))
    if not out or any(not 0 <= i < count for i in out):
        raise ValueError("Record selection outside 0..%d" % (count - 1))
    return sorted(set(out))


def plan_box_edit(rec, field, value, anchor="bottom", terrain=None):
    """Return {ry, centre, sphere_radius} for a shrink-only BOX edit, or raise."""
    if field not in EDITABLE_FIELDS:
        reason = UNVERIFIED_FIELDS.get(field, "not a verified collision limit field")
        raise ValueError(f"Refusing unverified field '{field}': {reason}")
    if rec["kind"] != "box":
        raise ValueError(f"Record {rec['index']} is a {rec['tag'].strip()}; only BOX heights are editable")
    if anchor not in ("bottom", "centre", "center"):
        raise ValueError("anchor must be bottom or centre")
    rows, (rx, ry, rz) = rec["rows"], rec["half_extents"]
    uy = rows[1][1]
    if abs(uy) < VERTICAL_LIMIT:
        raise ValueError(f"Record {rec['index']} is tilted (|local Y . world Y| = {abs(uy):.2f}); refusing")
    if abs(rec["sphere_radius"] - (math.sqrt(rx * rx + ry * ry + rz * rz) + SPHERE_PAD)) > 1e-4:
        raise ValueError(f"Record {rec['index']} sphere_radius does not follow |r|+0.2; refusing")
    extra = abs(rows[0][1]) * rx + abs(rows[2][1]) * rz
    if field == "ry":
        new_ry = float(value)
    else:
        if terrain is None:
            raise ValueError("top-above-ground needs terrain")
        ground = terrain.ground(rec["centre"][0], rec["centre"][2], below=rec["centre"][1])["y"]
        top = ground + float(value)
        if anchor == "bottom":
            bottom = rec["centre"][1] - abs(uy) * ry - extra
            new_ry = (top - bottom - 2 * extra) / (2 * abs(uy))
        else:
            new_ry = (top - rec["centre"][1] - extra) / abs(uy)
    new_ry = struct.unpack("<f", struct.pack("<f", new_ry))[0]
    if not math.isfinite(new_ry) or new_ry < 0.01:
        raise ValueError(f"Record {rec['index']}: requested height leaves ry={new_ry:.3f} m (< 0.01); "
                         "the box would have to move below its original volume")
    if new_ry > ry:
        raise ValueError(f"Record {rec['index']}: ry {new_ry:.3f} > original {ry:.3f}; only shrinking is "
                         "safe because the quadtree registration is not rebuilt")
    centre = list(rec["centre"])
    if new_ry != ry and anchor == "bottom":
        up = [v if uy > 0 else -v for v in rows[1]]
        centre = [centre[k] - up[k] * (ry - new_ry) for k in range(3)]
    sphere = rec["sphere_radius"] if new_ry == ry else math.sqrt(rx * rx + new_ry * new_ry + rz * rz) + SPHERE_PAD
    return dict(ry=new_ry, centre=centre, sphere_radius=sphere)


def apply_box_edits(archive_bytes, edits, anchor="bottom"):
    """edits: list of (record, field, value). Returns (new_bytes, report).

    The archive keeps its size; only the edited records' centre (12 bytes),
    ry (4) and sphere_radius (4) plus the track.sol directory CRC may change.
    """
    data = bytearray(archive_bytes)
    entries = read_directory(archive_bytes)
    sol, entry = member(archive_bytes, entries, "track.sol")
    bsc, _ = member(archive_bytes, entries, "track.bsc")
    sobl = parse_sobl(sol)
    terrain = Terrain(parse_scol(bsc)) if any(f == "top-above-ground" for _, f, _ in edits) else None
    allowed, report = set(), []
    payload = bytearray(sol)
    for index, field, value in edits:
        rec = sobl["records"][index]
        plan = plan_box_edit(rec, field, value, anchor, terrain)
        off = rec["offset"]
        before = dict(ry=rec["half_extents"][1], centre=rec["centre"], sphere_radius=rec["sphere_radius"],
                      top=vertical_span(rec)[1])
        if plan["centre"] != rec["centre"]:
            struct.pack_into("<3f", payload, off + 0x24, *plan["centre"])
        if plan["ry"] != rec["half_extents"][1]:
            struct.pack_into("<f", payload, off + 0x64, plan["ry"])
            struct.pack_into("<f", payload, off + 0x6C, plan["sphere_radius"])
        for start, length in ((0x24, 12), (0x64, 4), (0x6C, 4)):
            allowed.update(range(entry["offset"] + off + start, entry["offset"] + off + start + length))
        after_rec = parse_sobl(bytes(payload))["records"][index]
        report.append(dict(record=index, field=field, value=value, before=before,
                           after=dict(ry=after_rec["half_extents"][1], centre=after_rec["centre"],
                                      sphere_radius=after_rec["sphere_radius"],
                                      top=vertical_span(after_rec)[1])))
    data[entry["offset"]:entry["offset"] + entry["size"]] = payload
    struct.pack_into("<I", data, entry["crc_field"], eod_crc(bytes(payload)))
    allowed.update(range(entry["crc_field"], entry["crc_field"] + 4))
    changed = [i for i in range(len(data)) if data[i] != archive_bytes[i]] if len(data) == len(archive_bytes) else None
    if changed is None or any(i not in allowed for i in changed):
        raise AssertionError("Write-back touched bytes outside the edited fields")
    parse_sobl(bytes(payload))
    read_directory(bytes(data))
    return bytes(data), report


# ----------------------------------------------------------------- export ---

def _mtl(path, materials):
    with open(path, "w") as f:
        for name, (r, g, b) in materials.items():
            f.write(f"newmtl {name}\nKd {r:.3f} {g:.3f} {b:.3f}\nKa 0 0 0\nd 1\n\n")


def export_track(track, out_dir, render=True):
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    terrain = Terrain(track["scol"])
    heights = {h["index"]: h for h in wall_heights(track, terrain)}
    materials, lines = {}, ["# EOD collision (track.bsc SCOL terrain + track.sol SOBL solids)",
                            "# native world coordinates, metres, Y up", "mtllib collision.mtl"]
    vcount = 0
    groups = {}
    for t in track["scol"]["tris"]:
        key = "terrain_s%d" % t["surface"] + ("_opt%d" % t["flags"] if t["flags"] else "")
        groups.setdefault(key, []).append(t)
    for key, tris in sorted(groups.items()):
        sid = int(key.split("_s")[1].split("_")[0])
        mat = "terrain_surface_%d" % sid + ("_optional" if "_opt" in key else "")
        base = TERRAIN_COLOURS[sid % len(TERRAIN_COLOURS)]
        materials[mat] = tuple(min(1, c * 0.7 + 0.3) for c in base) if "_opt" in key else base
        lines += ["g " + key, "usemtl " + mat]
        for t in tris:
            for v in t["verts"]:
                lines.append("v %.4f %.4f %.4f" % tuple(v))
            # stored winding is clockwise seen from +Y; emit counter-clockwise (normal up)
            lines.append("f %d %d %d" % (vcount + 1, vcount + 3, vcount + 2))
            vcount += 3
    for rec in track["sobl"]["records"]:
        mat = "solid_" + rec["surface_name"]
        materials.setdefault(mat, SURFACE_COLOURS.get(rec["surface_type"], (0.5, 0.5, 0.5)))
        if rec["kind"] == "box":
            lines += ["g sobl_box_%d" % rec["index"], "usemtl " + mat]
            for p in box_corners(rec):
                lines.append("v %.4f %.4f %.4f" % tuple(p))
            for face in BOX_FACES:
                lines.append("f " + " ".join(str(vcount + 1 + i) for i in face))
            vcount += 8
        elif rec["kind"] == "tube":
            lines += ["g sobl_tube_%d" % rec["index"], "usemtl " + mat]
            a, b, axis, c = rec["rows"][0], rec["rows"][1], rec["rows"][2], rec["centre"]
            for end in (-1, 1):
                for k in range(8):
                    ang = 2 * math.pi * k / 8
                    lines.append("v %.4f %.4f %.4f" % tuple(
                        c[i] + end * rec["half_length"] * axis[i] + rec["radius"] *
                        (math.cos(ang) * a[i] + math.sin(ang) * b[i]) for i in range(3)))
            for k in range(8):
                n = (k + 1) % 8
                lines.append("f %d %d %d %d" % (vcount + 1 + k, vcount + 1 + n, vcount + 9 + n, vcount + 9 + k))
            vcount += 16
    (out / "collision.obj").write_text("\n".join(lines) + "\n")
    _mtl(out / "collision.mtl", materials)

    bmat = {"wall_ge10m": (1, 0.1, 0.1), "wall_3_10m": (1, 0.55, 0.1), "wall_lt3m": (1, 0.9, 0.2),
            "event_bound": (0.1, 0.9, 0.9), "event_ringout": (0.2, 0.9, 0.2), "event_other": (0.6, 0.4, 1)}
    blines = ["# EOD boundary volumes: SOBL boxes as wireframes by height above terrain,",
              "# event X/Z polygons extruded between terrain min Y and max Y + 20 m", "mtllib boundaries.mtl"]
    vcount = 0
    for rec in track["sobl"]["records"]:
        if rec["kind"] != "box":
            continue
        h = heights[rec["index"]]["above_ground"]
        cls = "wall_ge10m" if h >= 10 else "wall_3_10m" if h >= 3 else "wall_lt3m"
        blines += ["g box_%d_%s" % (rec["index"], cls), "usemtl " + cls]
        for p in box_corners(rec):
            blines.append("v %.4f %.4f %.4f" % tuple(p))
        for a, b in BOX_EDGES:
            blines.append("l %d %d" % (vcount + 1 + a, vcount + 1 + b))
        vcount += 8
    ys = [v[1] for t in track["scol"]["tris"] for v in t["verts"]] or [0.0]
    lo, hi = min(ys), max(ys) + 20
    for poly in track["events"]:
        cls = "event_bound" if poly["section"] in ("bound", "safe_bound", "play_area") else \
              "event_ringout" if poly["section"] == "ringout" else "event_other"
        blines += ["g %s_%s" % (poly["event"].replace(".", "_"), poly["section"]), "usemtl " + cls]
        n = len(poly["points"])
        for y in (lo, hi):
            for x, z in poly["points"]:
                blines.append("v %.4f %.4f %.4f" % (x, y, z))
        for k in range(n):
            m = (k + 1) % n
            blines.append("l %d %d" % (vcount + 1 + k, vcount + 1 + m))
            blines.append("l %d %d" % (vcount + 1 + n + k, vcount + 1 + n + m))
            blines.append("l %d %d" % (vcount + 1 + k, vcount + 1 + n + k))
        vcount += 2 * n
    (out / "boundaries.obj").write_text("\n".join(blines) + "\n")
    _mtl(out / "boundaries.mtl", bmat)
    rep = limits_report(track)
    manifest = dict(track=track["name"], archive=Path(track["path"]).name,
                    units="metres, native world axes, Y up",
                    sources=dict(sol=dict(crc=track["sol_entry"]["crc"], size=track["sol_entry"]["size"]),
                                 bsc=dict(crc=track["bsc_entry"]["crc"], size=track["bsc_entry"]["size"])),
                    counts=dict(scol_triangles=track["scol"]["count"], sobl_records=track["sobl"]["count"],
                                sobl_boxes=rep["sobl"]["boxes"], sobl_tubes=rep["sobl"]["tubes"],
                                quadtree_nodes=len(track["sobl"]["nodes"]),
                                event_polygons=len(track["events"])),
                    limits=rep, files=["collision.obj", "collision.mtl", "boundaries.obj", "boundaries.mtl"])
    if render:
        render_topdown(track, out / "topdown.png", terrain=terrain, heights=heights)
        manifest["files"].append("topdown.png")
        manifest["topdown_legend"] = dict(
            terrain="grey, brighter = higher; blue tint = optional-terrain triangles",
            walls="SOBL box footprints: red >=10 m above ground, orange 3-10 m, yellow <3 m",
            tubes="magenta", event_bound="cyan outline", event_ringout="green outline",
            orientation="+X right, +Z down")
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


# ----------------------------------------------------------------- render ---

def _png(path, w, h, rgb):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgb[y * w * 3:(y + 1) * w * 3]

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


def render_topdown(track, path, size=1400, terrain=None, heights=None):
    tris = track["scol"]["tris"]
    xs = [v[0] for t in tris for v in t["verts"]] + [r["centre"][0] for r in track["sobl"]["records"]]
    zs = [v[2] for t in tris for v in t["verts"]] + [r["centre"][2] for r in track["sobl"]["records"]]
    ys = [v[1] for t in tris for v in t["verts"]] or [0.0]
    x0, x1, z0, z1 = min(xs) - 5, max(xs) + 5, min(zs) - 5, max(zs) + 5
    scale = (size - 1) / max(x1 - x0, z1 - z0)
    w, h = int((x1 - x0) * scale) + 1, int((z1 - z0) * scale) + 1
    img = bytearray(b"\x14\x14\x18" * (w * h))
    ylo, yhi = min(ys), max(ys)

    def px(x, z):
        return (x - x0) * scale, (z - z0) * scale

    def put(x, y, c):
        if 0 <= x < w and 0 <= y < h:
            i = (y * w + x) * 3
            img[i:i + 3] = bytes(c)

    def fill(poly, c):
        pts = [px(x, z) for x, z in poly]
        ymin, ymax = max(0, int(min(p[1] for p in pts))), min(h - 1, int(max(p[1] for p in pts)) + 1)
        for yy in range(ymin, ymax + 1):
            cy, xs_ = yy + 0.5, []
            for i in range(len(pts)):
                (ax, ay), (bx, by) = pts[i], pts[(i + 1) % len(pts)]
                if (ay <= cy < by) or (by <= cy < ay):
                    xs_.append(ax + (cy - ay) * (bx - ax) / (by - ay))
            xs_.sort()
            for a, b in zip(xs_[0::2], xs_[1::2]):
                for xx in range(max(0, int(a + 0.5)), min(w - 1, int(b + 0.5)) + 1):
                    i = (yy * w + xx) * 3
                    img[i:i + 3] = bytes(c)

    def line(p, q, c):
        (ax, ay), (bx, by) = px(*p), px(*q)
        n = int(max(abs(bx - ax), abs(by - ay))) + 1
        for k in range(n + 1):
            t = k / n
            put(int(ax + (bx - ax) * t), int(ay + (by - ay) * t), c)

    for t in tris:
        shade = 0.35 + 0.45 * ((sum(v[1] for v in t["verts"]) / 3 - ylo) / max(1e-6, yhi - ylo))
        c = [int(255 * shade)] * 3 if not t["flags"] else [int(140 * shade), int(170 * shade), int(255 * shade)]
        fill([(v[0], v[2]) for v in t["verts"]], c)
    heights = heights or {hh["index"]: hh for hh in wall_heights(track, terrain)}
    order = sorted(track["sobl"]["records"], key=lambda r: heights.get(r["index"], {}).get("above_ground", 0))
    for rec in order:
        if rec["kind"] == "box":
            hgt = heights[rec["index"]]["above_ground"]
            c = (235, 40, 30) if hgt >= 10 else (255, 140, 20) if hgt >= 3 else (250, 230, 60)
            a, d, ctr, e = rec["rows"][0], rec["rows"][2], rec["centre"], rec["half_extents"]
            rx, rz = max(e[0], 0.6 / scale), max(e[2], 0.6 / scale)
            corners = [(ctr[0] + sx * rx * a[0] + sz * rz * d[0], ctr[2] + sx * rx * a[2] + sz * rz * d[2])
                       for sx, sz in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
            fill(corners, c)
            for i in range(4):
                line(corners[i], corners[(i + 1) % 4], c)
        elif rec["kind"] == "tube":
            cx, cy = px(rec["centre"][0], rec["centre"][2])
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    put(int(cx) + dx, int(cy) + dy, (230, 60, 230))
    for poly in track["events"]:
        c = (40, 230, 230) if poly["section"] in ("bound", "safe_bound", "play_area") else \
            (60, 230, 60) if poly["section"] == "ringout" else (170, 120, 255)
        pts = poly["points"]
        for i in range(len(pts)):
            line(pts[i], pts[(i + 1) % len(pts)], c)
    _png(path, w, h, img)
    return dict(width=w, height=h, x=[x0, x1], z=[z0, z1], pixels_per_metre=scale)


# -------------------------------------------------------------------- CLI ---

def census(disc):
    rows = []
    for path in sorted((Path(disc) / "DATA/TRACKS").glob("*.TRK")):
        data = path.read_bytes()
        entries = read_directory(data)
        sobl = parse_sobl(member(data, entries, "track.sol")[0])
        scol = parse_scol(member(data, entries, "track.bsc")[0])
        kinds = {}
        for r in sobl["records"]:
            kinds[r["tag"].strip()] = kinds.get(r["tag"].strip(), 0) + 1
        rows.append(dict(archive=path.name, sobl_records=sobl["count"], shapes=kinds,
                         index_entries=len(sobl["index"]), quadtree_nodes=len(sobl["nodes"]),
                         scol_triangles=scol["count"],
                         open_edges=sum(1 for t in scol["tris"] for n in t["neighbours"] if n == -1)))
    return rows


def main(argv=None):
    ap = argparse.ArgumentParser(description="EOD track collision and boundary tool")
    ap.add_argument("--disc", default=str(default_disc()), help="Disc root containing DATA/")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("census", help="Exact collision counts for every retail track")
    p = sub.add_parser("export", help="Export collision/boundary OBJ+MTL, manifest and top-down PNG")
    p.add_argument("track")
    p.add_argument("out")
    p.add_argument("--no-render", action="store_true")
    p = sub.add_parser("list-limits", help="Print the data-driven limits of a track")
    p.add_argument("track")
    p.add_argument("--json", action="store_true")
    p = sub.add_parser("set-limit", help="Lower SOBL wall boxes in a copy of the track archive")
    p.add_argument("track")
    p.add_argument("--records", required=True, help="Record list, e.g. 3,6,9-12")
    p.add_argument("--field", required=True, help="ry | top-above-ground")
    p.add_argument("--value", required=True, type=float, help="metres")
    p.add_argument("--anchor", default="bottom", choices=("bottom", "centre"))
    p.add_argument("--out", required=True, help="Output .TRK (never the source)")
    p.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)

    if args.cmd == "census":
        for row in census(args.disc):
            print(json.dumps(row))
        return 0
    archive = resolve_track(args.track, args.disc)
    if args.cmd == "export":
        manifest = export_track(load_track(archive, args.disc), args.out, render=not args.no_render)
        print(json.dumps(manifest["counts"]))
        return 0
    if args.cmd == "list-limits":
        rep = limits_report(load_track(archive, args.disc))
        if args.json:
            print(json.dumps(rep, indent=2))
        else:
            print_limits(rep)
        return 0
    if args.cmd == "set-limit":
        out = Path(args.out).resolve()
        if out == archive.resolve() or Path(args.disc).resolve() in out.parents:
            raise SystemExit("Refusing to write inside the disc folder; choose a project/overlay path")
        source = archive.read_bytes()
        count = struct.unpack_from("<I", member(source, read_directory(source), "track.sol")[0], 0)[0]
        edits = [(i, args.field, args.value) for i in _parse_records(args.records, count)]
        try:
            new, report = apply_box_edits(source, edits, anchor=args.anchor)
        except ValueError as exc:
            raise SystemExit(str(exc))
        for r in report:
            b, a = r["before"], r["after"]
            print(f"record {r['record']}: ry {b['ry']:.3f} -> {a['ry']:.3f}, top {b['top']:.2f} -> {a['top']:.2f}, "
                  f"centre Y {b['centre'][1]:.3f} -> {a['centre'][1]:.3f}")
        if args.dry_run:
            print("dry run: nothing written")
            return 0
        out.parent.mkdir(parents=True, exist_ok=True)
        tmp = out.with_suffix(out.suffix + ".tmp")
        tmp.write_bytes(new)
        os.replace(tmp, out)
        print(f"wrote {out} ({len(new)} bytes, same size as source)")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
