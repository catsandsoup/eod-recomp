"""ILIN: racing lines (`.ild`) and ghost lap lines (`.ili`).

Contract: docs/FORMAT_VIEW_CONTRACT.md. Layout notes: docs/ILIN_RACING_LINES.md.
The node layout is the one tools/trk.py (decompile_ild) and
tools/world_export.py (racing_line) already read: 48-byte header, a pointer
fix-up table, then 60-byte nodes with the position in X/Z at +0/+4. This module
labels every byte of that layout; it does not introduce a second parser.
"""
import math
import re
import struct

from . import generic

FOURCCS = ("ILIN",)

HEADER_SIZE = 48
NODE_SIZE = 60
_NODE_ID = re.compile(r"^node\.(\d+)\.(x|z)$")
_LIMIT = 10000.0  # metres; the largest retail line spans about 3.5 km


def _f32(data, offset):
    return struct.unpack_from("<f", data, offset)[0]


def _u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def _layout(data):
    """Validated structural facts shared by decode, encode and the tests."""
    if len(data) < HEADER_SIZE:
        raise ValueError("shorter than the 48-byte ILIN header")
    _tag, header_size, node_size = struct.unpack_from("<3I", data, 0)
    if header_size != HEADER_SIZE or node_size != NODE_SIZE:
        raise ValueError(f"header size {header_size} / node size {node_size}, expected 48 / 60")
    flags = _u32(data, 28)
    nodes_offset, _secondary, record_offset, fixups = struct.unpack_from("<4I", data, 32)
    primary = flags >> 16
    if nodes_offset < HEADER_SIZE or (nodes_offset - HEADER_SIZE) % 4 or nodes_offset > len(data):
        raise ValueError("node offset is not after a whole pointer table")
    if (nodes_offset - HEADER_SIZE) // 4 != fixups:
        raise ValueError("pointer table length disagrees with the header")
    if record_offset:  # ghost line: nodes end where the ghost record begins
        node_end = record_offset
        if (record_offset - nodes_offset) % NODE_SIZE or record_offset < nodes_offset or record_offset + 20 > len(data):
            raise ValueError("ghost record offset does not follow a whole number of nodes")
        kind = "ghost"
    else:
        count = (len(data) - nodes_offset) // NODE_SIZE
        node_end = nodes_offset + count * NODE_SIZE
        if len(data) - node_end not in (0, 16):
            raise ValueError("unexpected bytes after the node array")
        kind = "line"
    count = (node_end - nodes_offset) // NODE_SIZE
    return dict(kind=kind, nodes_offset=nodes_offset, node_end=node_end, count=count,
                primary=primary, fixups=fixups, record_offset=record_offset)


def _node_index(layout, offset):
    start = layout["nodes_offset"]
    if start <= offset < layout["node_end"] and (offset - start) % NODE_SIZE == 0:
        return (offset - start) // NODE_SIZE
    return None


def main_loop(data):
    """Node indices along the main line, following each node's next pointer from the first node."""
    layout = _layout(data)
    order, seen, cursor = [], set(), 0
    closed = False
    while cursor is not None and cursor not in seen:
        seen.add(cursor)
        order.append(cursor)
        nxt = _node_index(layout, _u32(data, layout["nodes_offset"] + cursor * NODE_SIZE + 20))
        if nxt == 0:
            closed = True
            break
        cursor = nxt
    return order, closed


def points(data):
    """[[x, 0, z], ...] of the main line (the ILIN is planar: it stores no height)."""
    layout = _layout(data)
    order, _ = main_loop(data)
    base = layout["nodes_offset"]
    return [[_f32(data, base + i * NODE_SIZE), 0.0, _f32(data, base + i * NODE_SIZE + 4)] for i in order]


def _finite(value):
    return value if math.isfinite(value) else None


def _field(fid, group, label, ftype, value, offset, size, confidence, explanation,
           evidence="", unit="", editable=False, **extra):
    # `expert` fields (layout internals) are hidden in the app until "Show file internals" is on.
    field = dict(id=fid, group=group, label=label, type=ftype, unit=unit, value=value,
                 offset=offset, size=size, editable=editable, confidence=confidence,
                 explanation=explanation, evidence=evidence, expert=not editable)
    field.update(extra)
    return field


_LAYOUTS = (("bw_oval", "oval", True), ("bw_fig8", "fig8", True), ("oval", "oval", False),
            ("fig8", "fig8", False), ("track", "track", False), ("bw", "track", True))
_LAYOUT_NAMES = {"oval": "Oval", "fig8": "Figure-8", "track": "Full course"}


def line_name(member):
    """Player-facing identity of a line file from its name: prefix + optional `bw` + layout.

    The prefix is the event's line basename and the layout/direction suffix is the one the game's
    file-name builder appends (f_0013D9A8). `bw` is confirmed as "driven the other way round" by the
    data: in every checked track the `bw` file visits the same course with its points in reverse
    order and opposite direction vectors.
    """
    base = str(member or "").replace("\\", "/").rsplit("/", 1)[-1].lower()
    stem, _, ext = base.rpartition(".")
    if not stem:
        stem, ext = base, ""
    prefix, layout, backwards = stem, None, False
    for suffix, name, back in _LAYOUTS:
        if stem.endswith(suffix):
            prefix, layout, backwards = stem[:-len(suffix)], name, back
            break
    if prefix == "":
        who = "Base layout"
    elif prefix in ("rr", "roadrace"):
        who = "Road Race"
    elif prefix.startswith("opt"):
        who = "HomeBase option " + (prefix[3:].strip("_").upper() or "?")
    else:
        who = prefix
    return dict(prefix=prefix, who=who, layout=layout, layout_name=_LAYOUT_NAMES.get(layout, ""),
                backwards=backwards, ext=ext)


def _title(name):
    ghost = name["ext"] == "ili"
    if ghost:
        return f"{name['who']} ghost line"
    layout = name["layout_name"]
    if name["prefix"]:
        parts = [f"{name['who']} racing line"] + ([layout] if layout else [])
    else:
        parts = [f"{layout} racing line" if layout else "Racing line"]
    if name["backwards"]:
        parts.append("reverse direction")
    return " \u00b7 ".join(parts)


def _empty(data, context):
    name = line_name(context.get("member"))
    ghost = name["ext"] == "ili"
    what = "ghost line" if ghost else "racing line"
    return dict(
        schema="eod.ilin/1", fourcc="ILIN", title=_title(name) + " (empty)", confidence="high", editable=False,
        summary=(f"This {what} file is empty on the original disc, so there is nothing to see or edit. "
                 "Changing it has no effect on the game."),
        fields=[], blocks=[], preview=None, status="empty",
        howWeKnow=[dict(text="The file is 0 bytes in the game's archive. Its directory entry still carries a "
                             "checksum from a longer original that is not on the disc.", confidence="verified")],
        warnings=[])


def decode(data, context):
    data = bytes(data)
    member = str(context.get("member") or "").lower()
    if not data:
        return _empty(data, context)
    layout = _layout(data)
    ghost = layout["kind"] == "ghost"
    count, primary, base = layout["count"], layout["primary"], layout["nodes_offset"]
    loop, closed = main_loop(data)
    lap = _f32(data, 12)

    compact = bool(context.get("compact"))
    fields, blocks = [], []

    # ---- header (48 bytes) ----
    g = "Header"
    fields += [
        _field("header.tag", g, "Load-time tag slot", "u32", _u32(data, 0), 0, 4, "high",
               "Always 0 in the file. The game writes its own marker here after loading; leave it alone.",
               "f_0013D9A8 stores 0x104F128B at +0 after loading the file"),
        _field("header.header_size", g, "Header size", "u32", _u32(data, 4), 4, 4, "high",
               "Structural: the loader rejects anything but 48.", "native loader check (docs/BATTLE_NAVIGATION.md)"),
        _field("header.node_size", g, "Node size", "u32", _u32(data, 8), 8, 4, "high",
               "Structural: each node is 60 bytes.", "native loader check (docs/BATTLE_NAVIGATION.md)"),
    ]
    for i, axis in enumerate("abc"):
        fields.append(_field(
            f"header.lap_length_{axis}", g, f"Lap length copy {i + 1}", "f32", _f32(data, 12 + 4 * i),
            12 + 4 * i, 4, "medium",
            "Length of the main line in metres, stored three times. Editing nodes does not update it.",
            f"equals the sum of the main-line segment lengths in every retail file (this one: {lap:.2f} m)",
            unit="m"))
    fields += [
        _field("header.unknown_1", g, "Unknown value 1", "f32", _finite(_f32(data, 24)), 24, 4, "low",
               "Small number (0 to 0.27) that differs per line. Purpose not found.", "data pattern only"),
        _field("header.unknown_2", g, "Unknown value 2", "u16", struct.unpack_from("<H", data, 28)[0], 28, 2, "low",
               "0 in every retail file.", "data pattern only"),
        _field("header.main_node_count", g, "Main line node count", "u16", primary, 30, 2, "high",
               "How many of the first nodes form the main line. The game wraps around after this many.",
               "native 0x0013AF20 wraps by this halfword (docs/BATTLE_NAVIGATION.md)"),
        _field("header.nodes_offset", g, "Node array offset", "u32", base, 32, 4, "high",
               "Structural: where the first node starts.", "native loader"),
        _field("header.secondary_start", g, "Side line start offset", "u32", _u32(data, 36), 36, 4, "medium",
               ("Offset of the first node of the last side line (pit or shortcut), or 0 when there is none."
                if not ghost else "0 or an offset to a node of the line."),
               "points at a node boundary in every retail file that sets it"),
        _field("header.record_offset", g, "Ghost record offset" if ghost else "Unknown value 3", "u32",
               _u32(data, 40), 40, 4, "high" if ghost else "medium",
               ("Offset of the ghost's recorded-run block that follows the nodes." if ghost else
                "0 in every racing line (.ild)."),
               "a node boundary after the last node in every ghost line" if ghost else "data pattern only"),
        _field("header.pointer_count", g, "Pointer table entries", "u32", layout["fixups"], 44, 4, "high",
               "Structural: number of fix-up entries between the header and the nodes.", "matches the table size"),
    ]

    # ---- pointer fix-up table ----
    if base > HEADER_SIZE:
        blocks.append(dict(
            name="Pointer fix-up table", offset=HEADER_SIZE, size=base - HEADER_SIZE, known=True,
            explanation=("File offsets of every pointer stored in this file (the header node offset, each node's "
                         "next link and the side-line pointers). The game adds the load address to each. Not "
                         "editable: adding or removing nodes would need this table rebuilt.")))

    # ---- nodes ----
    table_rows = []
    for i in range(count):
        o = base + i * NODE_SIZE
        grp = f"Node {i}"
        nxt = _u32(data, o + 20)
        nxt_index = _node_index(layout, nxt)
        role = ("Main" if i < primary else "Side")
        x, z = _f32(data, o), _f32(data, o + 4)
        node_fields = [
            _field(f"node.{i}.x", grp, "X position", "f32", x, o, 4, "high",
                   "East-west position in metres in the game's world axes (image right on a top-down map). "
                   "Moving it shifts the line, but the stored direction and segment length are not recalculated.",
                   "native 0x0013AF20 and src/battle_navigation.cpp read X at +0; red line in track previews",
                   unit="m", editable=True, min=-_LIMIT, max=_LIMIT),
            _field(f"node.{i}.z", grp, "Z position", "f32", z, o + 4, 4, "high",
                   "North-south position in metres in the game's world axes (image up on a top-down map). "
                   "There is no height value: the line is flat.",
                   "native 0x0013AF20 and src/battle_navigation.cpp read Z at +4", unit="m",
                   editable=True, min=-_LIMIT, max=_LIMIT),
            _field(f"node.{i}.direction_x", grp, "Direction X", "f32", _finite(_f32(data, o + 8)), o + 8, 4, "high",
                   "X part of the unit direction of travel at this node.",
                   "src/battle_navigation.cpp uses it as the course tangent"),
            _field(f"node.{i}.direction_z", grp, "Direction Z", "f32", _finite(_f32(data, o + 12)), o + 12, 4, "high",
                   "Z part of the unit direction of travel at this node.",
                   "src/battle_navigation.cpp uses it as the course tangent"),
            _field(f"node.{i}.line_position", grp, "Position on the main line", "u32", _u32(data, o + 16), o + 16, 4,
                   "medium",
                   "Fixed-point node number: high 16 bits are the main-line node index, low 16 bits the fraction. "
                   "Main-line nodes hold their own index; side-line nodes hold where they meet the main line.",
                   "data pattern: main nodes read index x 65536 in every retail file; the loader names an "
                   "'ilpos' conversion (convert_meters_to_ilpos)"),
            _field(f"node.{i}.next", grp, "Next node" + (" (index)" if nxt_index is not None else " (raw)"),
                   "u32", nxt_index if nxt_index is not None else nxt, o + 20, 4, "high",
                   "The node the line continues to. Not editable: a changed link needs the pointer table rebuilt.",
                   "src/battle_navigation.cpp validates next = following node, wrapping at the last"),
            _field(f"node.{i}.segment_length", grp, "Segment length", "f32", _finite(_f32(data, o + 24)), o + 24, 4,
                   "high", "Distance in metres from this node to the next one. Not recalculated when a node moves.",
                   "src/battle_navigation.cpp uses +0x18 as section length; matches the node spacing in retail data",
                   unit="m"),
            _field(f"node.{i}.distance", grp, "Distance along the line", "f32", _finite(_f32(data, o + 28)), o + 28, 4,
                   "medium", "Metres travelled along the main line from the first node to this one.",
                   "running total of segment lengths in retail main lines", unit="m"),
            _field(f"node.{i}.unknown_1", grp, "Unknown value 1", "f32", _finite(_f32(data, o + 32)), o + 32, 4, "low",
                   "35.556 on main lines, exactly half on side lines, 31-37 on ghost lines. May be a width or speed.",
                   "data pattern only"),
            _field(f"node.{i}.unknown_2", grp, "Unknown value 2", "f32", _finite(_f32(data, o + 36)), o + 36, 4, "low",
                   "Small, smoothly varying number (0 to 0.3). May relate to how tight the corner is.",
                   "data pattern only"),
            _field(f"node.{i}.unknown_3", grp, "Unknown value 3", "f32", _finite(_f32(data, o + 40)), o + 40, 4, "low",
                   "Constant for stretches of the line, 4 to 25. May be a target speed. Purpose not confirmed.",
                   "data pattern only"),
            _field(f"node.{i}.unknown_4", grp, "Unknown value 4", "f32", _finite(_f32(data, o + 44)), o + 44, 4, "low",
                   "Zero except near junctions between lines.", "data pattern only"),
            _field(f"node.{i}.unknown_5", grp, "Unknown value 5", "f32", _finite(_f32(data, o + 48)), o + 48, 4, "low",
                   "Zero except near junctions between lines.", "data pattern only"),
            _field(f"node.{i}.unknown_6", grp, "Unknown value 6", "u32", _u32(data, o + 52), o + 52, 4, "low",
                   "Bit flags: 256 on every main-line node, other values (258, 1026, 1282, 2051) on side lines.",
                   "data pattern only"),
            _field(f"node.{i}.branch_list", grp, "Side-line list pointer", "u32", _u32(data, o + 56), o + 56, 4,
                   "medium", "0 for almost every node; one node per line points at the list of side lines.",
                   "src/battle_navigation.cpp notes secondary lists at +0x38"),
        ]
        if not compact:
            fields += node_fields
        table_rows.append([i, role, round(x, 3), round(z, 3), round(_finite(_f32(data, o + 24)) or 0.0, 2),
                           round(_finite(_f32(data, o + 28)) or 0.0, 1)])

    # ---- tail ----
    tail_start = layout["node_end"]
    if ghost:
        record_start = layout["record_offset"]
        last = len(data) - 20
        sub = generic.decode(data[record_start:last], {"fourcc": "ILIN"})
        for block in sub["blocks"]:
            block = dict(block, offset=block["offset"] + record_start)
            if not block["known"]:
                block["name"] = "Ghost record data (not decoded)"
            blocks.append(block)
        g = "Ghost lap summary"
        fields += [
            _field("ghost.flags", g, "Unknown value 1", "u32", _u32(data, last), last, 4, "low",
                   "Four bytes that vary per ghost.", "data pattern only"),
            _field("ghost.lap_time", g, "Recorded lap time", "f32", _finite(_f32(data, last + 4)), last + 4, 4,
                   "medium", "Seconds of the recorded run.", "matches the frame count below divided by 59.94 in all 86 retail ghost lines",
                   unit="s"),
            _field("ghost.lap_frames", g, "Recorded lap length", "u32", _u32(data, last + 8), last + 8, 4, "medium",
                   "Frames of the recorded run (about 59.94 per second).", "equals lap time x 59.94 in all 86 retail ghost lines"),
            _field("ghost.unknown_2", g, "Unknown value 2", "u32", _u32(data, last + 12), last + 12, 4, "low",
                   "20 in every ghost line (this record's own size).", "data pattern only"),
            _field("ghost.unknown_3", g, "Unknown value 3", "u32", _u32(data, last + 16), last + 16, 4, "low",
                   "Number that differs per ghost; about ten times the lap time on entry lines, near the node count on lap lines.", "data pattern only"),
        ]
    elif len(data) > tail_start:
        g = "Side-line list"
        fields += [
            _field("tail.unknown_1", g, "Unknown value 1", "u32", _u32(data, tail_start), tail_start, 4, "low",
                   "0 in every retail line.", "data pattern only"),
            _field("tail.unknown_2", g, "Unknown value 2", "u32", _u32(data, tail_start + 4), tail_start + 4, 4, "low",
                   "0 in every retail line.", "data pattern only"),
            _field("tail.unknown_3", g, "Unknown value 3", "u32", _u32(data, tail_start + 8), tail_start + 8, 4, "low",
                   "2 in every retail line.", "data pattern only"),
            _field("tail.first_side_node", g, "First side-line node (offset)", "u32", _u32(data, tail_start + 12),
                   tail_start + 12, 4, "medium",
                   "File offset of the first side-line node, reached from the node that points at this list.",
                   "points at a node boundary in every retail line that has side lines"),
        ]

    name = line_name(member)
    side = count - primary
    how = [
        dict(text="The AI driver setup (f_00137A68) loads this file and stops with \"ai: Can't load centerline\" if it is missing.",
             confidence="high"),
        dict(text="The file name is the event's line prefix plus the layout and direction, built by f_0013D9A8 "
                  "(0 fig8, 1 oval, 2 track, 0x200+ the same three with `bw`).", confidence="high"),
        dict(text="`bw` means the line is driven the other way round: in every file pair checked, the bw file visits "
                  "the same points in reverse order with opposite direction vectors.", confidence="high"),
        dict(text="The same line feeds the road-smoke and tyre-wear effects (f_002EC2BC, f_002ECFEC: "
                  "\"couldn't create ideal line / wear line\").", confidence="medium"),
        dict(text="Point positions are read as X and Z in metres (native 0x0013AF20, src/battle_navigation.cpp). "
                  "No height is stored.", confidence="high"),
        dict(text="Not traced: whether lap counting or wrong-way detection reads this file.", confidence="low"),
    ]
    if ghost:
        entry = member.endswith("_e.ili")
        title = "Ghost entry line" if entry else "Ghost lap line"
        summary = (("The route a ghost car takes into the track, " if entry else "The route of a recorded ghost lap, ") +
                   f"stored with its lap time ({_f32(data, len(data) - 16):.2f} s) and the ghost car's details. "
                   "The game reads it from the ghost car archive when it shows a ghost.")
        how = [dict(text="The ghost loader (f_001174D0) reads \"ghost: Loading trackline\" from the ghost car "
                         "archive by track and layout name.", confidence="medium")]
        notes = ["Moving points changes the ghost's route. The recorded lap time and car data stay as they are."]
    else:
        title = _title(name) if member else "Racing line (AI driving path)"
        direction = (" It is the same course as the matching forward line, driven in the opposite direction."
                     if name["backwards"] else "")
        summary = (f"The route computer-driven cars follow, {lap:.0f} m per lap"
                   + (", as a closed loop" if closed else "") + "." + direction +
                   (f" {side} extra points form side routes such as pit road or shortcuts." if side else "") +
                   " The road smoke and tyre-wear effects use it too.")
        notes = ["Each track has one line per layout and per direction, so the files in a track look alike: "
                 "the reverse-direction file visits the same course with its points in the opposite order.",
                 "Moving a point changes where the AI drives. Lap length and the stored direction at each point "
                 "are not recalculated, so keep moves small."]
    columns = [
        dict(key="index", label="#"),
        dict(key="route", label="Route"),
        dict(key="x", label="X", unit="m", type="f32", edit="node.{id}.x", min=-_LIMIT, max=_LIMIT),
        dict(key="z", label="Z", unit="m", type="f32", edit="node.{id}.z", min=-_LIMIT, max=_LIMIT),
        dict(key="segment", label="To next point", unit="m"),
        dict(key="distance", label="Distance along line", unit="m"),
    ]
    view = dict(
        schema="eod.ilin/1", fourcc="ILIN", title=title, summary=summary, confidence="high", editable=True,
        fields=fields, blocks=blocks, howWeKnow=how, notes=notes,
        preview=dict(kind="path", points=points(data), nodes=loop, closed=closed, unit="m",
                     start="Start/finish", direction=True),
        table=dict(name="Path points", columns=columns, rows=table_rows, role="path", searchable=False),
        warnings=[])
    if compact:
        # Only the lap length is useful outside the file-internals view.
        for f in fields:
            f["expert"] = True
    return view


def encode(data, edits):
    data = bytearray(data)
    if not edits:
        return bytes(data)
    layout = _layout(bytes(data))
    for fid, value in edits.items():
        match = _NODE_ID.match(str(fid))
        if not match:
            raise ValueError(f"'{fid}' cannot be edited. Only node X and Z positions (node.<n>.x, node.<n>.z) can.")
        index, axis = int(match.group(1)), match.group(2)
        if index >= layout["count"]:
            raise ValueError(f"This line has {layout['count']} nodes, so node {index} does not exist.")
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
            raise ValueError(f"Node {index} {axis.upper()} must be a number in metres.")
        if abs(value) > _LIMIT:
            raise ValueError(f"Node {index} {axis.upper()} must be between -{_LIMIT:.0f} and {_LIMIT:.0f} metres.")
        struct.pack_into("<f", data, layout["nodes_offset"] + index * NODE_SIZE + (0 if axis == "x" else 4), float(value))
    return bytes(data)
