#!/usr/bin/env python3
"""Native track sky tile-set export, panorama preview, reimport and test artwork.

Evidence (docs/SKY_EXPORT.md): each retail track.grf has one leaf node (type 4)
whose display record has type 1 / subtype 4 ("Sky", ELF table 0x406B40,
constructor 0x221E00). Record+8 is a record-relative offset to an embedded MODL;
record+0x2C holds 32 culling cones (dir xyz, half-angle rad) indexed by the 4SPS
part group. Draw 0x223858 copies the camera position into the instance
translation (identity rotation), so mesh directions from the model origin are
view directions. Tiles are 4SPS parts named skyNN[AB].tex.

Conventions: world azimuth = atan2(x, z) in degrees [0, 360); elevation =
atan2(y, hypot(x, z)). Texture (u, v) = (0, 0) is the PNG top-left texel corner.
Run with `python3 -I` on retail data. No third-party dependencies.
"""
import argparse
import hashlib
import json
import math
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import modding_backend as mb  # noqa: E402
import vif_mesh as vm  # noqa: E402
from rst0 import eod_crc  # noqa: E402
from tex import read_png, write_png  # noqa: E402
from texture_identity import texture_identity  # noqa: E402
from texture_preview import decode_preview  # noqa: E402
from valt import parse_binary_valt  # noqa: E402

TOOL_VERSION = "1"
TILE_RE = re.compile(r"^sky(\d\d)([ab])\.tex$", re.I)
DISPLAY_TYPES = {4: "Sky", 9: "Sun", 15: "Ambient", 16: "Shadow", 17: "SunBright", 18: "FogColor"}
EDGES = ("u0", "u1", "v0", "v1")


def sha(data):
    return hashlib.sha256(data).hexdigest()


def write_json(path, obj):
    Path(path).write_text(json.dumps(obj, indent=1) + "\n")


# --------------------------------------------------------------------------- native data

def display_records(grf):
    """(leaf offset, record offset, subtype) for every type-4 leaf -> type-1 display record."""
    out = []
    for n in range(0, len(grf) - 12, 4):
        if struct.unpack_from("<I", grf, n)[0] != 4:
            continue
        r = struct.unpack_from("<I", grf, n + 8)[0]
        if r % 4 or r + 0x30 > len(grf) or struct.unpack_from("<I", grf, r)[0] != 1:
            continue
        out.append((n, r, struct.unpack_from("<I", grf, r + 16)[0]))
    return out


def sky_record(grf):
    hits = [(n, r) for n, r, t in display_records(grf) if t == 4]
    if len(hits) != 1:
        raise ValueError(f"Expected one Sky display record, found {len(hits)}")
    leaf, rec = hits[0]
    modl = rec + struct.unpack_from("<I", grf, rec + 8)[0]
    cones = [struct.unpack_from("<4f", grf, rec + 0x2C + 16 * i) for i in range(32)]
    return leaf, rec, modl, cones


def decode_parts(grf, modl):
    """All 4SPS parts of the embedded MODL: word0 = part count, word1 = table offset."""
    m = memoryview(grf)[modl:]
    count, table = struct.unpack_from("<2I", m, 0)
    if not 0 < count <= 64:
        raise ValueError("Implausible sky MODL part count")
    parts = []
    for slot, o in enumerate(struct.unpack_from(f"<{count}I", m, table)):
        if o == vm.EMPTY or o + 0x38 > len(m) or bytes(m[o:o + 4]) != b"4SPS":
            raise ValueError(f"Sky MODL slot {slot} is not a 4SPS part")
        size = struct.unpack_from("<I", m, o + 4)[0]
        verts, tris = [], []
        batches = vm.decode_part(bytes(m[o:o + size]), dict(offset=0, size=size))
        for b in batches:
            base = len(verts)
            bv = b["verts"]
            for a, c, d in vm.strip_triangles(bv):  # winding is irrelevant here
                if len({bv[a]["p"], bv[c]["p"], bv[d]["p"]}) == 3:
                    tris.append((a + base, c + base, d + base))
            verts += [(v["p"], v["uv"]) for v in bv]
        # Deduplicate (position, uv) so manifests stay small.
        index, uniq, remap = {}, [], []
        for p, uv in verts:
            key = tuple(round(c, 4) for c in p) + tuple(round(c, 6) for c in uv)
            remap.append(index.setdefault(key, len(uniq)))
            if remap[-1] == len(uniq):
                uniq.append(list(key))
        parts.append(dict(slot=slot, texture=bytes(m[o + 8:o + 24]).split(b"\0")[0].decode("latin1"),
                          group=struct.unpack_from("<I", m, o + 24)[0],
                          mscal=sorted({b["mscal"] for b in batches}),
                          vertices=uniq, triangles=[[remap[i] for i in t] for t in tris]))
    return parts


def direction(p):
    x, y, z = p
    h = math.hypot(x, z)
    el = math.degrees(math.atan2(y, h))
    az = None if h < 1e-3 * max(1e-9, math.sqrt(h * h + y * y)) else math.degrees(math.atan2(x, z)) % 360
    return az, el


def circ_mean(values):
    s = sum(math.sin(math.radians(a)) for a in values)
    c = sum(math.cos(math.radians(a)) for a in values)
    return math.degrees(math.atan2(s, c)) % 360


def wrap180(a):
    return (a + 180) % 360 - 180


# --------------------------------------------------------------------------- geometry analysis

def tile_geometry(part, cone):
    V = part["vertices"]
    dirs = [direction(v[:3]) for v in V]
    on = {"u0": lambda v: abs(v[3]) < 1e-4, "u1": lambda v: abs(v[3] - 1) < 1e-4,
          "v0": lambda v: abs(v[4]) < 1e-4, "v1": lambda v: abs(v[4] - 1) < 1e-4}
    edge = {}
    for name in EDGES:
        idx = [i for i, v in enumerate(V) if on[name](v)]
        az = [dirs[i][0] for i in idx if dirs[i][0] is not None]
        edge[name] = dict(vertices=idx, azimuth=round(circ_mean(az), 3) if az else None,
                          elevation=round(sum(dirs[i][1] for i in idx) / len(idx), 3) if idx else None)
    a0, a1 = edge["u0"]["azimuth"], edge["u1"]["azimuth"]
    span = wrap180(a1 - a0) if a0 is not None and a1 is not None else None
    els = [d[1] for d in dirs]
    area = sum(abs((V[b][3] - V[a][3]) * (V[c][4] - V[a][4]) - (V[c][3] - V[a][3]) * (V[b][4] - V[a][4])) / 2
               for a, b, c in part["triangles"])
    apex = {tuple(v[:3]) for v in V if abs(v[4]) < 1e-4}
    cdir, chalf = cone[:3], cone[3]
    worst = max(math.degrees(math.acos(max(-1.0, min(1.0, sum(v[k] * cdir[k] for k in range(3)) /
                                                    math.sqrt(sum(c * c for c in v[:3])))))) for v in V)
    return dict(
        azimuth=dict(atU0=a0, atU1=a1, spanDeg=round(abs(span), 3) if span is not None else None,
                     uIncreasesWith="decreasing-azimuth" if span is not None and span < 0 else "increasing-azimuth"),
        elevation=dict(min=round(min(els), 3), max=round(max(els), 3),
                       atV0=edge["v0"]["elevation"], atV1=edge["v1"]["elevation"]),
        upEdge="v0 (PNG top)" if edge["v0"]["elevation"] > edge["v1"]["elevation"] else "v1 (PNG bottom)",
        apexAtV0=len(apex) == 1,
        meanY=round(sum(v[1] for v in V) / len(V), 3),
        uvRect=[min(v[3] for v in V), min(v[4] for v in V), max(v[3] for v in V), max(v[4] for v in V)],
        uvCoverage=round(area, 4),
        nativeCull=dict(group=part["group"] & 0xFFFF, coneDir=[round(c, 5) for c in cdir],
                        coneHalfAngleDeg=round(math.degrees(chalf), 3), maxVertexAngleDeg=round(worst, 3)),
        _edges=edge)


def link_neighbours(tiles):
    """Edges sharing >= 2 vertex positions are seams; compare the seam parameter on both sides."""
    keyed = {}
    for t in tiles:
        V = t["mesh"]["vertices"]
        for e in EDGES:
            for i in t["geometry"]["_edges"][e]["vertices"]:
                keyed.setdefault(tuple(round(c, 1) for c in V[i][:3]), []).append((t["member"], e, i))
    by = {t["member"]: t for t in tiles}
    for t in tiles:
        t["neighbours"] = {}
        V = t["mesh"]["vertices"]
        for e in EDGES:
            shared = {}
            for i in t["geometry"]["_edges"][e]["vertices"]:
                for other, oe, oi in keyed[tuple(round(c, 1) for c in V[i][:3])]:
                    if other != t["member"]:
                        shared.setdefault((other, oe), []).append((i, oi))
            best = max(shared.items(), key=lambda kv: len(kv[1]), default=None)
            if not best or len(best[1]) < 2:
                t["neighbours"][e] = None
                continue
            (other, oe), pairs = best
            k = 4 if e[0] == "u" else 3  # along a u edge the seam parameter is v, and vice versa
            ov = by[other]["mesh"]["vertices"]
            t["neighbours"][e] = dict(tile=other, edge=oe, sharedVertices=len(pairs),
                                      seamParamMaxDelta=round(max(abs(V[i][k] - ov[j][k]) for i, j in pairs), 6))


def validate_topology(tiles):
    checks = []
    num = {}
    for t in tiles:
        n, band = TILE_RE.match(t["member"]).groups()
        num.setdefault(int(n), {})[band.upper()] = t
    nums = sorted(num)
    checks.append(("16 numbered sectors, each with A and B", nums == list(range(1, 17)) and
                   all(set(v) == {"A", "B"} for v in num.values()), f"{len(tiles)} tiles"))
    if checks[-1][1]:
        bad = [n for n in nums
               if (num[n]["A"]["neighbours"]["v0"] or {}).get("tile") != num[n]["B"]["member"]
               or (num[n]["B"]["neighbours"]["v1"] or {}).get("tile") != num[n]["A"]["member"]]
        checks.append(("A top edge (v0) seams to B bottom edge (v1) of same number", not bad, bad))
        bad = [t["member"] for n in nums for b in "AB" for t in [num[n][b]]
               if (t["neighbours"]["u1"] or {}).get("tile") != num[n % 16 + 1][b]["member"]
               or (t["neighbours"]["u1"] or {}).get("edge") != "u0"]
        checks.append(("u1 edge of NN seams to u0 edge of NN+1 (16 wraps to 01)", not bad, bad))
        bad = [n for n in nums if num[n]["A"]["geometry"]["meanY"] >= num[n]["B"]["geometry"]["meanY"]]
        checks.append(("A band lies below B cap (model +Y)", not bad, bad))
        spans = [num[n]["A"]["geometry"]["azimuth"]["spanDeg"] for n in nums]
        checks.append(("A spans sum to 360 deg", abs(sum(spans) - 360) < 0.5,
                       [round(min(spans), 3), round(max(spans), 3)]))
        seams = [v["seamParamMaxDelta"] for t in tiles for v in t["neighbours"].values() if v]
        checks.append(("Seam parameters match across shared vertices", max(seams) < 1e-3, max(seams)))
        checks.append(("B v0 edge collapses to one apex point",
                       all(num[n]["B"]["geometry"]["apexAtV0"] for n in nums), None))
    return [dict(check=c, ok=bool(ok), detail=d) for c, ok, d in checks]


# --------------------------------------------------------------------------- image helpers

def bilinear(img, w, h, x, y):
    """Clamp-to-edge bilinear sample at continuous texel coordinates (centres at i + 0.5)."""
    x = min(max(x - 0.5, 0.0), w - 1.0)
    y = min(max(y - 0.5, 0.0), h - 1.0)
    x0, y0 = int(x), int(y)
    x1, y1 = min(x0 + 1, w - 1), min(y0 + 1, h - 1)
    fx, fy = x - x0, y - y0
    o00, o10, o01, o11 = (y0 * w + x0) * 4, (y0 * w + x1) * 4, (y1 * w + x0) * 4, (y1 * w + x1) * 4
    return [(img[o00 + c] * (1 - fx) + img[o10 + c] * fx) * (1 - fy) +
            (img[o01 + c] * (1 - fx) + img[o11 + c] * fx) * fy for c in range(4)]


def upscale(rgba, w, h, k):
    if k == 1:
        return bytes(rgba)
    out = bytearray()
    for y in range(h):
        row = b"".join(bytes(rgba[(y * w + x) * 4:(y * w + x) * 4 + 4]) * k for x in range(w))
        out += row * k
    return bytes(out)


def downscale(rgba, w, h, k):
    """Box average k*k blocks (w, h are the source size)."""
    ow, oh = w // k, h // k
    out = bytearray(ow * oh * 4)
    n = k * k
    for y in range(oh):
        for x in range(ow):
            acc = [0, 0, 0, 0]
            for yy in range(y * k, y * k + k):
                o = (yy * w + x * k) * 4
                for i in range(o, o + 4 * k, 4):
                    for c in range(4):
                        acc[c] += rgba[i + c]
            out[(y * ow + x) * 4:(y * ow + x) * 4 + 4] = bytes((a + n // 2) // n for a in acc)
    return bytes(out)


def is_pow2(k):
    return k >= 1 and not k & (k - 1)


# --------------------------------------------------------------------------- panorama

def pano_params(manifest, width=None):
    p = dict(manifest["panorama"]["projection"])
    w = width or p["width"]
    return p["azimuthAtX0"], p["xDirection"], w, w // 4


def pano_xy(az, el, az0, xdir, w, h):
    return ((az - az0) * xdir % 360) / 360 * w, (90 - el) / 90 * h


def triangle_setup(V, tri):
    P0, P1, P2 = (V[i][:3] for i in tri)
    e1 = [P1[k] - P0[k] for k in range(3)]
    e2 = [P2[k] - P0[k] for k in range(3)]
    n = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
    a = [e2[1] * n[2] - e2[2] * n[1], e2[2] * n[0] - e2[0] * n[2], e2[0] * n[1] - e2[1] * n[0]]
    b = [n[1] * e1[2] - n[2] * e1[1], n[2] * e1[0] - n[0] * e1[2], n[0] * e1[1] - n[1] * e1[0]]
    dot = lambda p, q: p[0] * q[0] + p[1] * q[1] + p[2] * q[2]  # noqa: E731
    return dict(P0=P0, e1=e1, e2=e2, n=n, a=a, b=b, nn=dot(n, n), k=dot(n, P0), cs=dot(P0, a), ct=dot(P0, b),
                uv=[V[i][3:5] for i in tri])


def ray_hit(T, d):
    nd = T["n"][0] * d[0] + T["n"][1] * d[1] + T["n"][2] * d[2]
    if abs(nd) < 1e-12:
        return None
    lam = T["k"] / nd
    if lam <= 0:
        return None
    s = (lam * (d[0] * T["a"][0] + d[1] * T["a"][1] + d[2] * T["a"][2]) - T["cs"]) / T["nn"]
    t = (lam * (d[0] * T["b"][0] + d[1] * T["b"][1] + d[2] * T["b"][2]) - T["ct"]) / T["nn"]
    if s < -1e-5 or t < -1e-5 or s + t > 1 + 1e-5:
        return None
    return lam, s, t


def render_panorama(manifest, images, width=None):
    """Ray-cast every panorama pixel centre into the actual sky triangles (nearest hit).

    Pass 1 visits each triangle's boundary-sampled bounding box. Near an apex
    that is not exactly at the zenith the azimuth bound is unreliable, so pass 2
    retests still-empty pixels against every triangle spanning that row.
    """
    az0, xdir, W, H = pano_params(manifest, width)
    out = bytearray(W * H * 4)
    depth = [math.inf] * (W * H)
    cols = [(math.sin(math.radians(az0 + xdir * (x + 0.5) * 360 / W)),
             math.cos(math.radians(az0 + xdir * (x + 0.5) * 360 / W))) for x in range(W)]
    rows = [(math.cos(math.radians(90 - (y + 0.5) * 90 / H)), math.sin(math.radians(90 - (y + 0.5) * 90 / H)))
            for y in range(H)]
    by_row = [[] for _ in range(H)]

    def shade(T, tex, x, y):
        ce, se = rows[y]
        sa, ca = cols[x]
        hit = ray_hit(T, (ce * sa, se, ce * ca))
        if not hit or hit[0] >= depth[y * W + x]:
            return
        lam, s, t = hit
        depth[y * W + x] = lam
        (u0, v0), (u1, v1), (u2, v2) = T["uv"]
        img, tw, th = tex
        px = bilinear(img, tw, th, (u0 + s * (u1 - u0) + t * (u2 - u0)) * tw, (v0 + s * (v1 - v0) + t * (v2 - v0)) * th)
        out[(y * W + x) * 4:(y * W + x) * 4 + 4] = bytes(min(255, int(c + 0.5)) for c in px)

    for tile in manifest["tiles"]:
        tex = images[tile["member"]]
        V = tile["mesh"]["vertices"]
        for tri in tile["mesh"]["triangles"]:
            T = triangle_setup(V, tri)
            xs, ys = [], []
            for i in range(3):
                pa, pb = V[tri[i]][:3], V[tri[(i + 1) % 3]][:3]
                for f in range(17):
                    az, el = direction([pa[k] + (pb[k] - pa[k]) * f / 16 for k in range(3)])
                    ys.append((90 - el) / 90 * H)
                    if az is not None:
                        x = pano_xy(az, el, az0, xdir, W, H)[0]
                        xs.append(x if not xs else x + W * round((xs[0] - x) / W))
            yr = range(max(0, int(min(ys)) - 2), min(H, int(max(ys)) + 3))
            for y in yr:
                by_row[y].append((T, tex))
                for xx in range(int(min(xs)) - 2, int(max(xs)) + 3):
                    shade(T, tex, xx % W, y)
    for y in range(H):
        for x in range(W):
            if depth[y * W + x] == math.inf:
                for T, tex in by_row[y]:
                    shade(T, tex, x, y)
    return W, H, bytes(out)


def sample_panorama(pano, pw, ph, x, y):
    """Bilinear with azimuth wrap; texels with alpha 0 (outside the sky) get no weight."""
    x -= 0.5
    y = min(max(y - 0.5, 0.0), ph - 1.0)
    x0, y0 = math.floor(x), int(y)
    fx, fy = x - x0, y - y0
    acc, wsum = [0.0] * 4, 0.0
    for xx, yy, w in ((x0, y0, (1 - fx) * (1 - fy)), (x0 + 1, y0, fx * (1 - fy)),
                      (x0, min(y0 + 1, ph - 1), (1 - fx) * fy), (x0 + 1, min(y0 + 1, ph - 1), fx * fy)):
        o = (yy * pw + xx % pw) * 4
        if w > 0 and pano[o + 3]:
            wsum += w
            for c in range(4):
                acc[c] += pano[o + c] * w
    return [c / wsum for c in acc] if wsum else None


def tile_from_panorama(manifest, tile, pano, pw, ph, scale, original):
    """Resample one tile at native*scale from the panorama via its UV triangles."""
    az0, xdir, _, _ = pano_params(manifest, pw)
    w, h = tile["width"] * scale, tile["height"] * scale
    out = bytearray(upscale(original, tile["width"], tile["height"], scale))
    covered = bytearray(w * h)
    V = tile["mesh"]["vertices"]
    for tri in tile["mesh"]["triangles"]:
        (u0, v0), (u1, v1), (u2, v2) = (V[i][3:5] for i in tri)
        det = (u1 - u0) * (v2 - v0) - (u2 - u0) * (v1 - v0)
        if abs(det) < 1e-12:
            continue
        P0, P1, P2 = (V[i][:3] for i in tri)
        for j in range(max(0, int(min(v0, v1, v2) * h) - 1), min(h, int(max(v0, v1, v2) * h) + 2)):
            v = (j + 0.5) / h
            for i in range(max(0, int(min(u0, u1, u2) * w) - 1), min(w, int(max(u0, u1, u2) * w) + 2)):
                if covered[j * w + i]:
                    continue
                u = (i + 0.5) / w
                s = ((u - u0) * (v2 - v0) - (u2 - u0) * (v - v0)) / det
                t = ((u1 - u0) * (v - v0) - (u - u0) * (v1 - v0)) / det
                if s < -1e-7 or t < -1e-7 or s + t > 1 + 1e-7:
                    continue
                p = [P0[k] + s * (P1[k] - P0[k]) + t * (P2[k] - P0[k]) for k in range(3)]
                az, el = direction(p)
                x, y = pano_xy(az if az is not None else 0.0, el, az0, xdir, pw, ph)
                px = sample_panorama(pano, pw, ph, x, y)
                if px is None:
                    continue
                covered[j * w + i] = 1
                out[(j * w + i) * 4:(j * w + i) * 4 + 4] = bytes(min(255, int(c + 0.5)) for c in px)
    return bytes(out), w * h - sum(covered)


# --------------------------------------------------------------------------- export

def load_track(disc, track):
    path = mb.archive_path(Path(disc).resolve(), track)
    entries = mb.directory(path)
    grf, _ = mb.payload(path, "track.grf", entries)
    return path, entries, grf


def analyse(grf):
    leaf, rec, modl, cones = sky_record(grf)
    parts = decode_parts(grf, modl)
    tiles, others = [], []
    for p in parts:
        if TILE_RE.match(p["texture"]):
            tiles.append(dict(member=p["texture"], part=dict(slot=p["slot"], group=p["group"], vuProgram=p["mscal"]),
                              mesh=dict(vertices=p["vertices"], triangles=p["triangles"]),
                              geometry=tile_geometry(p, cones[p["group"] & 31])))
        else:
            els = [direction(v[:3])[1] for v in p["vertices"]]
            others.append(dict(texture=p["texture"], slot=p["slot"], group=p["group"] & 0xFFFF,
                               elevationDeg=[round(min(els), 2), round(max(els), 2)], triangles=len(p["triangles"])))
    link_neighbours(tiles)
    checks = validate_topology(tiles)
    for t in tiles:
        for e in t["geometry"]["_edges"].values():
            e.pop("vertices")
        t["geometry"]["edges"] = t["geometry"].pop("_edges")
    tiles.sort(key=lambda t: (TILE_RE.match(t["member"]).group(2).upper(), int(TILE_RE.match(t["member"]).group(1))))
    return dict(graf=dict(skyLeaf=leaf, skyRecord=rec, modlOffset=modl), tiles=tiles, nonTileParts=others,
                topologyChecks=checks, topology="ring16-horizon-band-A-plus-cap-B" if all(c["ok"] for c in checks)
                else "unverified")


def export(disc, track, dest, pano_width=None, panorama=True):
    disc = Path(disc).resolve()
    dest = Path(dest).resolve()
    if mb.inside(dest, disc):
        raise ValueError("Destination must be outside the disc")
    path, entries, grf = load_track(disc, track)
    info = analyse(grf)
    if not info["tiles"]:
        raise ValueError("No sky tiles in the Sky model")
    archive_sha = sha(path.read_bytes())
    by_sha = {}
    (dest / "tiles").mkdir(parents=True, exist_ok=True)
    images = {}
    for t in info["tiles"]:
        entry = next(e for e in entries if e["name"].lower() == t["member"].lower())
        data, _ = mb.payload(path, entry["name"], entries, verify_crc=False)
        w, h, rgba, meta = decode_preview(data)
        ident = texture_identity(data)
        out = Path("tiles") / f"{entry['name']}.png"
        write_png(str(dest / out), w, h, rgba)
        t["geometry"]["modelTexture"] = t["member"]
        t.update(assetId=f"{track}#{entry['name']}", archive=track, member=entry["name"], type=entry["type"],
                 archiveSha256=archive_sha, memberSha256=sha(data), crcOk=eod_crc(data) == entry["crc"],
                 width=w, height=h, levels=meta["levels"], format=meta["format"], flags=meta["flags"],
                 indexKey=ident["indexKey"], paletteKey=ident["paletteKey"], role="sky", roleConfidence="high",
                 status="exported", alpha="straight-0-255-from-palette", output=out.as_posix(),
                 outputSha256=sha((dest / out).read_bytes()), decodedRgbaSha256=sha(rgba),
                 reimport=["native-same-size", "host-replacement"])
        by_sha.setdefault(t["memberSha256"], []).append(t["assetId"])
        images[entry["name"]] = (rgba, w, h)
    for t in info["tiles"]:
        t["duplicateOf"] = [a for a in by_sha[t["memberSha256"]] if a != t["assetId"]]
    keys = {}
    for t in info["tiles"]:
        keys.setdefault((t["indexKey"], t["paletteKey"]), []).append(t["assetId"])
    for t in info["tiles"]:
        t["runtimeKeyShared"] = [a for a in keys[(t["indexKey"], t["paletteKey"])] if a != t["assetId"]]
    first = min((t for t in info["tiles"] if TILE_RE.match(t["member"]).group(2).upper() == "A"),
                key=lambda t: int(TILE_RE.match(t["member"]).group(1)))
    g = first["geometry"]["azimuth"]
    width = pano_width or sum(t["width"] for t in info["tiles"] if TILE_RE.match(t["member"]).group(2).upper() == "A")
    slus = disc / "SLUS_209.10"
    manifest = dict(
        schemaVersion=1, tool="sky_export", toolVersion=TOOL_VERSION, kind="sky-tile-set",
        disc=dict(slusSha256=sha(slus.read_bytes()) if slus.is_file() else None), archive=track,
        archiveSha256=archive_sha, conventions=dict(
            azimuth="degrees atan2(x, z) of the model-space direction, [0, 360)",
            elevation="degrees atan2(y, hypot(x, z)); +Y is up",
            uv="(0,0) = PNG top-left corner; texel (i, j) centre = ((i+0.5)/w, (j+0.5)/h)",
            origin="Sky draw 0x223858 translates the model to the camera position with identity rotation",
            screenHandedness="Anoka native k=32 run: increasing azimuth is to the viewer's right; tile u runs "
                             "right-to-left on screen (tiles appear mirrored); 01A,16A,15A,14A left to right"),
        **info,
        panorama=dict(file="panorama.png" if panorama else None, lossy=True, projection=dict(
            type="equirectangular-upper-hemisphere", width=width, height=width // 4,
            azimuthAtX0=g["atU1"] if g["uIncreasesWith"] == "decreasing-azimuth" else g["atU0"], xDirection=1,
            elevationTop=90, elevationBottom=0,
            note="player view: x increases with azimuth (viewer turning right), sky01A starts at x=0, tile "
                 "numbers decrease left to right and tile art appears mirrored; rows are linear in elevation; "
                 "pixels ray-cast "
                 "through the decoded triangles, bilinear clamp-to-tile texture sampling; alpha 0 = no sky")))
    rename = {t["geometry"].get("modelTexture", t["member"]): t["member"] for t in manifest["tiles"]}
    for t in manifest["tiles"]:  # neighbour names use the archive spelling; keep them last for reading
        t["neighbours"] = {e: (dict(nb, tile=rename.get(nb["tile"], nb["tile"])) if nb else None)
                           for e, nb in t.pop("neighbours").items()}
    if panorama:
        W, H, rgba = render_panorama(manifest, images, width)
        write_png(str(dest / "panorama.png"), W, H, rgba)
        manifest["panorama"]["sha256"] = sha((dest / "panorama.png").read_bytes())
    write_json(dest / "sky-manifest.json", manifest)
    return manifest


# --------------------------------------------------------------------------- reimport

def load_manifest(path):
    path = Path(path)
    return json.loads(path.read_text()), path.parent


def original_tile(root, t):
    w, h, rgba = read_png(str(root / t["output"]))
    if sha(rgba) != t["decodedRgbaSha256"] or (w, h) != (t["width"], t["height"]):
        raise ValueError(f"Exported tile changed since export: {t['output']}")
    return rgba


def reimport(manifest_path, out, tiles_dir=None, panorama=None, k=1):
    if not is_pow2(k) or k > 32:
        raise ValueError("k must be a power of two from 1 to 32")
    if (tiles_dir is None) == (panorama is None):
        raise ValueError("Give exactly one of --from-tiles or --from-panorama")
    manifest, root = load_manifest(manifest_path)
    out = Path(out)
    records = []
    if panorama:
        pw, ph, pano = read_png(str(panorama))
        if pw != 4 * ph:
            raise ValueError("Panorama must be 4:1 (360 x 90 degrees)")
        base = manifest["panorama"]["projection"]["width"]
        sk = 1
        while sk * 2 <= k and pw >= base * sk * 2:
            sk *= 2
    for t in manifest["tiles"]:
        orig = original_tile(root, t)
        w, h = t["width"], t["height"]
        rec = dict(assetId=t["assetId"], member=t["member"], indexKey=t["indexKey"], paletteKey=t["paletteKey"],
                   origWidth=w, origHeight=h, files={})
        if tiles_dir:
            iw, ih, img = read_png(str(Path(tiles_dir) / f"{t['member']}.png"))
            kin = iw // w
            if iw != w * kin or ih != h * kin or not is_pow2(kin):
                raise ValueError(f"{t['member']}: {iw}x{ih} is not the native size times a power of two")
            native = img if kin == 1 else downscale(img, iw, ih, kin)
            if k > 1 and kin not in (1, k):
                raise ValueError(f"{t['member']}: input is {kin}x, requested {k}x")
            big = img if kin == k else upscale(native, w, h, k)
            rec["method"] = ("direct-lossless" if kin == 1 else f"direct-box-downsample-from-{kin}x") + (
                "" if k == 1 else f"; {k}x " + ("as given" if kin == k else "nearest-neighbour upscale"))
            rec["uncoveredTexels"] = 0
        else:
            native, rec["uncoveredTexels"] = tile_from_panorama(manifest, t, pano, pw, ph, 1, orig)
            if k > 1:
                fine, _ = tile_from_panorama(manifest, t, pano, pw, ph, sk, orig)
                big = upscale(fine, w * sk, h * sk, k // sk)
            rec["method"] = "panorama-resampled (lossy)" + ("" if k == 1 else f"; {k}x sampled at {sk}x"
                                                            + ("" if sk == k else f" then nearest {k // sk}x"))
        for label, img, s in (("native", native, 1),) + ((("k%d" % k, big, k),) if k > 1 else ()):
            f = out / label / f"{t['member']}.png"
            f.parent.mkdir(parents=True, exist_ok=True)
            write_png(str(f), w * s, h * s, img)
            rec["files"][label] = dict(file=f.relative_to(out).as_posix(), width=w * s, height=h * s,
                                       sha256=sha(f.read_bytes()), rgbaSha256=sha(img))
        rec["changedTexels"] = sum(native[i:i + 4] != orig[i:i + 4] for i in range(0, len(orig), 4))
        rec["nativeColours"] = len({native[i:i + 4] for i in range(0, len(native), 4)})
        rec["nativeIndexedImportable"] = rec["nativeColours"] <= 256
        records.append(rec)
    result = dict(schemaVersion=1, tool="sky_export", kind="sky-reimport", archive=manifest["archive"],
                  source=dict(tiles=str(tiles_dir)) if tiles_dir else dict(panorama=str(panorama)), k=k,
                  tiles=records)
    write_json(out / "sky-reimport.json", result)
    return result


# --------------------------------------------------------------------------- orientation test artwork

FONT = dict(zip("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+-<>.", (
    "111101101101111 010110010010111 111001111100111 111001111001111 101101111001001 111100111001111 "
    "111100111101111 111001001001001 111101111101111 111101111001111 010101111101101 110101110101110 "
    "011100100100011 110101101101110 111100110100111 111100110100100 011100101101011 101101111101101 "
    "111010010010111 001001001101010 101101110101101 100100100100111 101111111101101 110101101101101 "
    "010101101101010 110101110100100 010101101110011 110101110101101 011100010001110 111010010010010 "
    "101101101101111 101101101101010 101101111111101 101101010101101 101101010010010 111001010100111 "
    "000010111010000 000000111000000 001010100010001 100010001010100 000000000000010").split()))


def rect(img, w, h, x0, y0, x1, y1, rgb):
    x0, x1, y0, y1 = max(0, int(x0)), min(w, int(x1)), max(0, int(y0)), min(h, int(y1))
    if x1 > x0:
        row = bytes(rgb) + b"\xff"
        for y in range(y0, y1):
            img[(y * w + x0) * 4:(y * w + x1) * 4] = row * (x1 - x0)


def text(img, w, h, s, cx, cy, size, rgb):
    """Centred 3x5 bitmap text, `size` = pixels per font cell."""
    width = (4 * len(s) - 1) * size
    x, y = cx - width / 2, cy - 2.5 * size
    for ch in s:
        for i, bit in enumerate(FONT.get(ch, "0" * 15)):
            if bit == "1":
                rect(img, w, h, x + (i % 3) * size, y + (i // 3) * size, x + (i % 3 + 1) * size, y + (i // 3 + 1) * size, rgb)
        x += 4 * size


def arrow(img, w, h, cx, cy, length, size, direction, rgb):
    """Filled arrow; direction one of 'up', 'down', 'left', 'right'."""
    horiz = direction in ("left", "right")
    sign = -1 if direction in ("up", "left") else 1
    half = length / 2
    for i in range(int(length)):
        p = -half + i  # position along the arrow axis, head at +half*sign
        dist = half - p  # distance to the tip along the axis
        thick = size / 2 if dist > 3 * size else max(0.5, dist / 2)
        a = cx + p * sign if horiz else cx
        b = cy if horiz else cy + p * sign
        if horiz:
            rect(img, w, h, a, b - thick, a + 1, b + thick, rgb)
        else:
            rect(img, w, h, a - thick, b, a + thick, b + 1, rgb)


def hsv(hue, sat, val):
    i = int(hue * 6) % 6
    f = hue * 6 - int(hue * 6)
    p, q, t = val * (1 - sat), val * (1 - f * sat), val * (1 - (1 - f) * sat)
    return tuple(int(c * 255 + 0.5) for c in ((val, t, p), (q, val, p), (p, val, t), (p, q, val), (t, p, val), (val, p, q))[i])


def test_tile(t, scale, label_k):
    w, h = t["width"] * scale, t["height"] * scale
    n, band = TILE_RE.match(t["member"]).groups()
    img = bytearray(w * h * 4)
    hue = (int(n) - 1) / 16
    for i in range(8):  # eight value bands along v: a coarse ruler toward the zenith
        val = (0.95 - i * 0.04) if band.upper() == "A" else (0.7 + i * 0.03)
        rect(img, w, h, 0, i * h / 8, w, (i + 1) * h / 8, hsv(hue, 0.45, val))
    b = max(2, w // 32)
    colours = dict(v0=(255, 220, 0), v1=(0, 70, 255), u0=(230, 0, 0), u1=(0, 190, 0))
    rect(img, w, h, 0, 0, b, h, colours["u0"])
    rect(img, w, h, w - b, 0, w, h, colours["u1"])
    rect(img, w, h, 0, 0, w, b, colours["v0"])
    rect(img, w, h, 0, h - b, w, h, colours["v1"])
    s = max(1, w // 40)
    ink, white = (0, 0, 0), (255, 255, 255)
    up_top = t["geometry"]["upEdge"].startswith("v0")
    cy = h * (0.6 if band.upper() == "B" else 0.5)  # keep clear of the unused B corner
    text(img, w, h, n + band.upper(), w / 2, cy, s * 2, ink)
    arrow(img, w, h, w / 2, cy + (-1 if up_top else 1) * h * 0.24, h * 0.16, s, "up" if up_top else "down", ink)
    text(img, w, h, "UP", w / 2 + s * 9, cy + (-1 if up_top else 1) * h * 0.24, s, ink)
    arrow(img, w, h, w / 2, cy + h * 0.17, w * 0.2, s, "right", white)
    for e, x in (("u0", b + s * 7), ("u1", w - b - s * 7)):
        nb = t["neighbours"].get(e)
        if nb:
            m = TILE_RE.match(nb["tile"])
            text(img, w, h, m.group(1) + m.group(2).upper(), x, h - b - s * 4, s, ink)
    if label_k > 1:
        text(img, w, h, "K%d" % label_k, b + s * 7, h * 0.3, s, white)
    return w, h, bytes(img)


def testart(manifest_path, out, k=32):
    if not is_pow2(k) or k > 32:
        raise ValueError("k must be a power of two from 1 to 32")
    manifest, _ = load_manifest(manifest_path)
    out = Path(out)
    info = dict(schemaVersion=1, tool="sky_export", kind="sky-orientation-test-artwork",
                archive=manifest["archive"], k=k, legend=dict(
                    border=dict(v0="yellow", v1="blue", u0="red", u1="green"),
                    label="tile number + band; mirrored text in game means u runs right-to-left on screen",
                    upArrow="points toward the zenith side (from decoded elevation)",
                    uArrow="white arrow points toward increasing u (toward the u1/green edge)",
                    sideLabels="numbers near red/green borders name the decoded seam neighbours",
                    k="native files have no K label; host files show K<k>"), tiles=[])
    images = {}
    for label, scale in (("native", 1),) + ((("k%d" % k, k),) if k > 1 else ()):
        (out / label).mkdir(parents=True, exist_ok=True)
        for t in manifest["tiles"]:
            w, h, rgba = test_tile(t, scale, scale)
            f = out / label / f"{t['member']}.png"
            write_png(str(f), w, h, rgba)
            if scale == 1:
                images[t["member"]] = (rgba, w, h)
            info["tiles"].append(dict(assetId=t["assetId"], indexKey=t["indexKey"], paletteKey=t["paletteKey"],
                                      origWidth=t["width"], origHeight=t["height"], scale=scale,
                                      file=f.relative_to(out).as_posix(), sha256=sha(f.read_bytes()),
                                      colours=len({rgba[i:i + 4] for i in range(0, len(rgba), 4)})))
    W, H, rgba = render_panorama(manifest, images)
    write_png(str(out / "testart-panorama.png"), W, H, rgba)
    write_json(out / "testart.json", info)
    return info


# --------------------------------------------------------------------------- environment registry

def registry(disc):
    disc = Path(disc).resolve()
    tracks = []
    for path in sorted((disc / "DATA/TRACKS").glob("*.TRK")):
        rel = path.relative_to(disc).as_posix()
        rec = dict(archive=rel, values=[])
        add = lambda name, status, evidence: rec["values"].append(dict(name=name, status=status, evidence=evidence))  # noqa: E731
        try:
            entries = mb.directory(path)
            grf, _ = mb.payload(path, "track.grf", entries)
        except ValueError as error:
            rec["error"] = str(error)
            tracks.append(rec)
            continue
        names = {e["name"].lower(): e["name"] for e in entries}
        counts = {}
        for _, _, sub in display_records(grf):
            if sub in DISPLAY_TYPES:
                counts[DISPLAY_TYPES[sub]] = counts.get(DISPLAY_TYPES[sub], 0) + 1
        rec["displayNodeCounts"] = counts
        try:
            info = analyse(grf)
            rec["skyTiles"] = len(info["tiles"])
            rec["skyTopology"] = info["topology"]
            native_run = rel.upper() == "DATA/TRACKS/ANOKA.TRK"
            add("sky tile textures (palette RGB, same-size indexed PNG)",
                "verified-editable" if native_run else "editable-unverified",
                "native purple-sky run (docs/TRACK_ENVIRONMENT_WORKFLOW.md)" if native_run
                else f"{len(info['tiles'])} tiles decode with the Anoka format; no native run")
            add("sky tile host replacement (k x)", "verified-editable" if native_run else "editable-unverified",
                "native k=32 labelled run (docs/HOST_TEXTURE_REPLACEMENT.md)" if native_run
                else "indexKey/paletteKey exported; no native host run")
            add("sky dome geometry / tile placement", "baked", "GRAF-embedded MODL; no GRAF writer")
            add("sky per-group cull cones", "baked", "GRAF Sky record +0x2C")
            add("horizon cards in sky model", "baked",
                ", ".join(p["texture"] for p in info["nonTileParts"]) or "none")
        except ValueError as error:
            rec["skyError"] = str(error)
        if "shadow.val" in names:
            table = parse_binary_valt(mb.payload(path, names["shadow.val"], entries)[0])
            pairs = [p for s in table["sections"] if s["name"] == "global" for p in s["pairs"]]
            add("shadow.val global/brightness", "unknown",
                f"native field {dict(pairs)} read by 0x32AC00; visible effect not measured")
        if "track.val" in names:
            table = parse_binary_valt(mb.payload(path, names["track.val"], entries)[0])
            gfx = [s["pairs"] for s in table["sections"] if s["name"] == "Graphics"]
            add("track.val Graphics section", "unknown" if gfx and gfx[0] else "absent",
                f"pairs={gfx[0] if gfx else None}")
        for name in ("Sun", "Ambient", "Shadow", "SunBright", "FogColor"):
            add(f"GRAF {name} display node", "baked" if counts.get(name) else "absent",
                f"{counts.get(name, 0)} records; payload semantics not decoded")
        add("fog range", "unknown", "camera-driven (0x212B58 -> 0x33ECB8); no per-track table found")
        tracks.append(rec)
    return dict(schemaVersion=1, tool="sky_export", kind="environment-registry",
                statuses=dict(**{"verified-editable": "edited and observed in a native run",
                                 "editable-unverified": "same native format as a verified edit; no native run",
                                 "baked": "present in native data; no writer or no editable meaning",
                                 "unknown": "native value exists; effect or editability not established",
                                 "absent": "not present in this track"}), tracks=tracks)


# --------------------------------------------------------------------------- CLI

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("export", help="tiles, manifest and panorama preview")
    e.add_argument("--disc", required=True)
    e.add_argument("--track", required=True, help="disc-relative archive, e.g. DATA/TRACKS/ANOKA.TRK")
    e.add_argument("--dest", required=True)
    e.add_argument("--panorama-width", type=int, help="default: sum of A tile widths (one texel per pixel)")
    e.add_argument("--no-panorama", action="store_true")
    r = sub.add_parser("reimport", help="edited tiles or panorama back to per-tile PNGs")
    r.add_argument("--manifest", required=True)
    r.add_argument("--out", required=True)
    src = r.add_mutually_exclusive_group(required=True)
    src.add_argument("--from-tiles")
    src.add_argument("--from-panorama")
    r.add_argument("--k", type=int, default=1, help="also write k x tiles for host replacement")
    t = sub.add_parser("testart", help="labelled orientation artwork, native and k x")
    t.add_argument("--manifest", required=True)
    t.add_argument("--out", required=True)
    t.add_argument("--k", type=int, default=32)
    g = sub.add_parser("registry", help="per-track environment value registry")
    g.add_argument("--disc", required=True)
    g.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    if a.cmd == "export":
        m = export(a.disc, a.track, a.dest, a.panorama_width, not a.no_panorama)
        print(json.dumps(dict(tiles=len(m["tiles"]), topology=m["topology"],
                              failed=[c["check"] for c in m["topologyChecks"] if not c["ok"]])))
    elif a.cmd == "reimport":
        res = reimport(a.manifest, a.out, a.from_tiles, a.from_panorama, a.k)
        print(json.dumps(dict(tiles=len(res["tiles"]), changed=sum(t["changedTexels"] > 0 for t in res["tiles"]))))
    elif a.cmd == "testart":
        info = testart(a.manifest, a.out, a.k)
        print(json.dumps(dict(files=len(info["tiles"]), maxColours=max(t["colours"] for t in info["tiles"]))))
    else:
        reg = registry(a.disc)
        write_json(a.out, reg)
        print(json.dumps({t["archive"]: t.get("skyTopology", t.get("skyError")) for t in reg["tracks"]}))


if __name__ == "__main__":
    main()
