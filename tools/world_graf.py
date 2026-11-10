#!/usr/bin/env python3
"""Native GRAF (track.grf, directory version 18) world scene parser.

Every structure here follows the retail loader, not a guessed schema. See
docs/WORLD_FORMAT.md for the evidence ledger. Summary:

* f_00214678 loads member type GRAF, panics unless directory version == 18,
  relocates header word 0 (display-list root) and calls
  f_00214258 (fixup_display_list) / f_00214468 (init_display_list). Header
  words 1 and 2 are two self-relative "mlist" draw lists relocated by
  f_00358F50; f_00214B68 draws word 1, or word 2 when global 0x3D1B0C != 0.
* Display nodes (all offsets file-relative, 0 = null):
    +0 u32 type, +4 u32 next sibling
    type 0     LOD/group: +8 vec3 centre, +0x14 u32 count,
               then count x {+0x18 f32 maxDistanceSquared, +0x1C u32 child}
    type 0x101 condition: +8 child, +0xC s32 value (-500 sentinel)
    type 0x102 cull sphere: +8 child, +0xC vec3 centre, +0x18 f32 radius
    type 0x103 condition: +8 child, +0x1C s32 value
    type 4     leaf: +8 u32 model record
* Model record: +0 u32 kind (0 = simple model, 1 = display object),
  +4 runtime instance handle (stale in file), +8 u32 embedded model offset
  relative to the record, +0x10 subtype (kind 1, table 0x406B40),
  +0x14 per-subtype index. Dyno (subtype 3) extends the record with a
  spawn block at +0x2C (mode bit, dyno kind, 'dyno' tag, 4x3 matrix at
  +0x38, three floats at +0x68).
* Embedded model (mrModel, fixed up by f_0034B530 "simple_model"):
  +0 nParts, +4 partTable, +8 nExtras, +0xC extraTable, +0x10 nInstances,
  +0x14 instanceTable (all offsets relative to the model), +0x18 radius,
  +0x1C centre, +0x28 bbox (minX,maxX,minY,maxY,minZ,maxZ). Parts are the
  same 4SPS VIF parts decoded by tools/vif_mesh.py.

Pure Python, read-only. Run with `python3 -I` on disc data.
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vif_mesh  # noqa: E402

GRAF_VERSION = 18
NODE_GROUP, NODE_LEAF = 0, 4
NODE_COND_A, NODE_SPHERE, NODE_COND_B = 0x101, 0x102, 0x103
COND_SENTINEL = -500

# ELF table 0x406B40: (name, constructor, pool capacity), index = subtype.
DISPLAY_SUBTYPES = [
    ("Simple Model", 0x221F28), ("Point-Facing", 0x221EF0), ("Axis-Facing", 0x221EB8),
    ("Dyno", 0x221E80), ("Sky", 0x221E00), ("Multi-Facing", 0x221C38), ("Old Flag", 0x22B9E8),
    ("Rotator", 0x221C90), ("Crowd", 0x221A18), ("Sun", 0x221DC8), ("People", 0x221A18),
    ("People-Zone", 0x221A18), ("Animation", 0x225BC0), ("Pit-Arm", 0x225BA0), ("Unknown", 0x221A18),
    ("Ambient", 0x221D88), ("Shadow", 0x221D48), ("SunBright", 0x221D08), ("FogColor", 0x221CC8),
    ("NamedPoint", 0x221C00), ("Optional Terrain", 0x221BC8), ("Old Banner", 0x22BA38),
    ("Linear Path", 0x233600), ("Linear Drone", 0x2335C8), ("Leader Board", 0x221B90),
    ("Cross Fader", 0x221B50), ("Flare Source", 0x221B18), ("Fireworks", 0x221AE0),
    ("Water", 0x221AA8), ("Effect source", 0x221A50), ("Hermite path", 0x233598),
    ("Hermite drone", 0x233560), ("Fabric", 0x22B9A0), ("Facing Test", 0x2219E0),
]

DYNO = 3
SKY = 4
OPTIONAL_TERRAIN = 20
# Dyno record offsets (relative to the model record). The spawn block
# (f_00223128 -> f_002243E0 returns record+0x2C) is read by f_00225808.
DYNO_MODE = 0x2C        # s32; -1 never spawned, else game-mode bit (1-based) in session+0x3C
DYNO_KIND = 0x30        # u32; jump table 0x408560 (0..7)
DYNO_TAG = 0x34         # b'onyd' (u32 'dyno')
DYNO_MATRIX = 0x38      # 12 f32: block+0xC..+0x3B copied into WobbleObject init (f_00225330/f_002256A8)
DYNO_PARAMS = 0x68      # 3 f32: block+0x3C/+0x40/+0x44 copied to init +0x58/+0x5C/+0x60
DYNO_RECORD_END = 0x74


def _u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def _s32(d, o):
    return struct.unpack_from("<i", d, o)[0]


def _f32s(d, o, n):
    return list(struct.unpack_from("<%df" % n, d, o))


class GrafError(ValueError):
    pass


def parse_model(data, off):
    """Parse an embedded mrModel header at absolute offset `off`."""
    if off + 0x40 > len(data):
        raise GrafError("model header out of range at %#x" % off)
    n_parts, part_tbl, n_extra, extra_tbl, n_inst, inst_tbl = struct.unpack_from("<6I", data, off)
    if n_parts > 4096 or n_extra > 64 or n_inst > 4096:
        raise GrafError("implausible model header at %#x" % off)
    radius = struct.unpack_from("<f", data, off + 0x18)[0]
    centre = _f32s(data, off + 0x1C, 3)
    b = _f32s(data, off + 0x28, 6)
    parts = []
    for i in range(n_parts):
        p = off + _u32(data, off + part_tbl + 4 * i)
        if data[p:p + 4] != b"4SPS":
            raise GrafError("model %#x part %d is not 4SPS" % (off, i))
        size = _u32(data, p + 4)
        tex = data[p + 8:p + 24].split(b"\0")[0].decode("latin1")
        group = _u32(data, p + 24)
        parts.append(dict(index=i, offset=p, size=size, texture=tex, group=group))
    extras = []
    for i in range(n_extra):
        e = off + _u32(data, off + extra_tbl + 4 * i)
        extras.append(dict(offset=e, tag=data[e:e + 4].decode("latin1", "replace")))
    return dict(offset=off, n_parts=n_parts, n_extras=n_extra, n_instances=n_inst,
                radius=radius, centre=centre,
                bbox=dict(min=[b[0], b[2], b[4]], max=[b[1], b[3], b[5]]),
                parts=parts, extras=extras)


def decode_model(data, model):
    """Decode every 4SPS part: returns list of dict(texture, group, verts, tris)."""
    out = []
    for part in model["parts"]:
        batches = vif_mesh.decode_part(data, part)
        verts, tris = [], []
        for bt in batches:
            base = len(verts)
            for (a, b, c) in vif_mesh.triangulate_batch(bt):
                pa, pb, pc = bt["verts"][a]["p"], bt["verts"][b]["p"], bt["verts"][c]["p"]
                if pa == pb or pb == pc or pa == pc:
                    continue
                tris.append((a + base, b + base, c + base))
            verts.extend(bt["verts"])
        out.append(dict(texture=part["texture"], group=part["group"], verts=verts, tris=tris,
                        mscal=sorted(set(b["mscal"] for b in batches))))
    return out


def parse_graf(data):
    """Walk the native display list. Returns a dict with header, nodes, leaves."""
    if len(data) < 12:
        raise GrafError("GRAF payload too short")
    root, mlist_a, mlist_b = struct.unpack_from("<3I", data, 0)
    nodes, leaves = [], []
    seen = set()

    def node_at(off):
        if off == 0:
            return None
        if off % 4 or off + 8 > len(data):
            raise GrafError("node offset %#x out of range" % off)
        if off in seen:
            raise GrafError("display list cycle at %#x" % off)
        seen.add(off)
        return off

    # Iterative depth-first walk mirroring f_00214258's recursion order.
    stack = [(root, [])]
    while stack:
        off, path = stack.pop()
        chain = []
        n = off
        while n:
            node_at(n)
            t = _u32(data, n)
            rec = dict(offset=n, type=t, depth=len(path))
            if t == NODE_GROUP:
                count = _u32(data, n + 0x14)
                if count > 4096:
                    raise GrafError("group %#x child count %d" % (n, count))
                rec.update(centre=_f32s(data, n + 8, 3), count=count, children=[])
                for i in range(count):
                    dist2 = struct.unpack_from("<f", data, n + 0x18 + 8 * i)[0]
                    child = _u32(data, n + 0x1C + 8 * i)
                    rec["children"].append(dict(child=child, max_dist2=dist2))
                    chain.append((child, path + [dict(kind="lod", node=n, index=i, count=count,
                                                      max_dist2=dist2)]))
            elif t in (NODE_COND_A, NODE_SPHERE, NODE_COND_B):
                child = _u32(data, n + 8)
                rec["child"] = child
                if t == NODE_SPHERE:
                    rec.update(centre=_f32s(data, n + 0xC, 3), radius=struct.unpack_from("<f", data, n + 0x18)[0])
                    step = dict(kind="sphere", node=n)
                else:
                    value = _s32(data, n + (0xC if t == NODE_COND_A else 0x1C))
                    rec["value"] = value
                    step = dict(kind="cond", node=n, type=t, value=value)
                chain.append((child, path + [step]))
            elif t == NODE_LEAF:
                m = _u32(data, n + 8)
                rec["model_record"] = m
                leaves.append(_leaf(data, n, m, path))
            else:
                raise GrafError("unrecognized DisplayNodeType %#x at %#x" % (t, n))
            nodes.append(rec)
            n = _u32(data, n + 4)
        for item in reversed(chain):
            stack.append(item)
    return dict(root=root, mlists=[mlist_a, mlist_b], nodes=nodes, leaves=leaves,
                mlist_info=[parse_mlist_header(data, mlist_a), parse_mlist_header(data, mlist_b)])


def _leaf(data, node, m, path):
    leaf = dict(node=node, record=m, path=path)
    if m == 0:
        leaf.update(kind=None)
        return leaf
    if m + 0x18 > len(data):
        raise GrafError("model record %#x out of range" % m)
    kind = _u32(data, m)
    modl = m + _u32(data, m + 8)
    leaf.update(kind=kind, model_offset=modl)
    if kind == 1:
        st = _u32(data, m + 0x10)
        leaf.update(subtype=st, subtype_name=DISPLAY_SUBTYPES[st][0] if st < len(DISPLAY_SUBTYPES) else "?",
                    index=_u32(data, m + 0x14))
        if st == DYNO:
            leaf["dyno"] = dict(
                mode=_s32(data, m + DYNO_MODE), dyno_kind=_u32(data, m + DYNO_KIND),
                tag=data[m + DYNO_TAG:m + DYNO_TAG + 4].decode("latin1", "replace"),
                vec20=_f32s(data, m + 0x20, 3))
            if leaf["dyno"]["mode"] != -1:
                leaf["dyno"].update(matrix=_f32s(data, m + DYNO_MATRIX, 12),
                                    params=_f32s(data, m + DYNO_PARAMS, 3),
                                    matrix_offset=m + DYNO_MATRIX)
        elif st == OPTIONAL_TERRAIN:
            # f_002230F8 turns +0x2C (1-based mode) into a bit mask at fixup;
            # f_00223008 tests it against session(0x495BE0)+0x3C and toggles
            # mlist A entry +0x1C / mlist B entry +0x18 via f_00359248.
            leaf["optional_terrain"] = dict(mode=_s32(data, m + 0x2C), mlist_a_entry=_u32(data, m + 0x1C),
                                            mlist_b_entry=_u32(data, m + 0x18))
        elif st in (1, 2, 5):
            leaf["anchor"] = _f32s(data, m + 0x20, 3)
    return leaf


def parse_mlist_header(data, off):
    """Self-relative draw list header (f_00358F50). Field names are provisional."""
    if off == 0 or off + 0x98 > len(data):
        return None
    w = struct.unpack_from("<38I", data, off)
    return dict(offset=off, word0=w[0], spheres=dict(count=w[4], offset=w[5]),
                table1c=dict(count=w[6], offset=w[7]), table24=dict(count=w[8], offset=w[9]),
                batches=dict(count=w[10], offset=w[11], stride=12),
                textures=dict(count=w[12], offset=w[13], stride=0x1C), table90=dict(count=w[35], offset=w[36]))


def mlist_textures(data, info):
    t = info["textures"]
    out = []
    for i in range(t["count"]):
        o = info["offset"] + t["offset"] + 0x1C * i
        out.append(data[o:o + 16].split(b"\0")[0].decode("latin1"))
    return out


def lod_choice(path):
    """Return the tuple of LOD child indices taken along a leaf path."""
    return tuple(s["index"] for s in path if s["kind"] == "lod" and s["count"] > 1)


def leaf_lod_level(path):
    """Index chosen in the innermost multi-child group (0 = nearest/highest detail), or None."""
    levels = [s["index"] for s in path if s["kind"] == "lod" and s["count"] > 1]
    return levels[-1] if levels else None


def summarize(graf):
    from collections import Counter
    types = Counter(n["type"] for n in graf["nodes"])
    kinds = Counter()
    for leaf in graf["leaves"]:
        if leaf.get("kind") is None:
            kinds["null"] += 1
        elif leaf["kind"] == 0:
            kinds["simple"] += 1
        else:
            kinds[leaf.get("subtype_name", "?")] += 1
    return dict(node_types={hex(k): v for k, v in sorted(types.items())}, leaves=len(graf["leaves"]),
                leaf_kinds=dict(sorted(kinds.items())))


if __name__ == "__main__":
    import json
    payload = Path(sys.argv[1]).read_bytes()
    g = parse_graf(payload)
    print(json.dumps(summarize(g), indent=1))
