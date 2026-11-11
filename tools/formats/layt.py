"""LAYT: menu screen layouts (`*.lyt`): the widget tree of one menu, dialog or HUD overlay.

Contract: docs/FORMAT_VIEW_CONTRACT.md. Layout and loader trace: docs/LAYT_MENU_LAYOUTS.md.

File layout (little endian). The loader keeps the file in memory and walks it in place:

    0x00  u32        flag, 1 in 19 retail layouts, not read by the layout loader
    0x04  char[32]   window background: a `.img` picture, "dialog" (stock frame) or empty
    0x24  u8         1 = also build the shared `back.lyt` widgets
    0x25  u8         1 = play the camera track named at 0x28
    0x26  2 bytes    padding
    0x28  char[60]   camera track name ("main", "buycar", ...) or empty
    0x64  u32        build address; the loader replaces it with the address of the first widget
    0x68  widgets, 0x134 (308) bytes each, ending at the first record whose kind is 0
          the final record (the end marker) is a copy of build-tool scratch memory

Widget record (offsets inside the 0x134 bytes):

    0x00  u32  kind                      0x1C f32  text size (text kinds only)
    0x04  u32  style                     0x20 char[32] first string  (name / font)
    0x08  i32  x                         0x40 char[68] second string (text / picture)
    0x0C  i32  y                         0x84 u32  text effect
    0x10  i32  depth                     0x88 i32  page group
    0x14  i32  width                     0x8C u32  cue
    0x18  i32  height                    0x90..0xB7 colours and options, 0xB8..0x133 build defaults

Loader: LayoutLoad 0x00270F10 (ResourceGet type 'LAYT', version 5), MenuRun set-up 0x00270430,
one widget per record in 0x0026D5E8 (jump table 0x004101B0 on the kind), registration 0x0027E418.
"""
import hashlib
import struct
import tempfile
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

FOURCCS = ("LAYT",)

HEADER_SIZE = 0x68
RECORD = 0x134
SLOT1 = (0x20, 0x20)
SLOT2 = (0x40, 0x44)
SCREEN_W, SCREEN_H = 640, 480
MAX_RECORDS = 4096

EVIDENCE = "Widget builder 0x0026D5E8 (gen/rec_0*.c), jump table 0x004101B0"

# kind: (label, role of the first string, role of the second string, uses width and height)
# roles: bind = name the screen's code uses to find this widget; font; image; text; format; movie; none
KINDS = {
    2: ("Image button", "bind", "text", True),
    3: ("Picture button (retired)", "bind", "image", True),
    5: ("Static text", "bind", "text", False),
    6: ("Decimal readout", "bind", "format", False),
    7: ("Whole-number readout", "bind", "format", False),
    8: ("Arrow number selector", "bind", "none", True),
    9: ("Setting stepper", "bind", "text", True),
    10: ("Choice list", "bind", "text", True),
    11: ("Text entry", "bind", "text", True),
    12: ("Controller detector", "bind", "none", False),
    13: ("Check box", "bind", "text", False),
    14: ("Radio button", "bind", "text", False),
    15: ("Picture radio button", "bind", "image", False),
    16: ("Tab button", "bind", "text", False),
    17: ("Stamp roll (retired)", "bind", "image", False),
    18: ("Slider", "bind", "text", True),
    20: ("Vertical scroll bar", "bind", "text", True),
    21: ("Horizontal scroll bar", "bind", "text", True),
    23: ("List box", "bind", "text", True),
    26: ("Game-drawn area", "bind", "text", True),
    27: ("Picture tab button", "bind", "image", False),
    28: ("Text label", "font", "text", False),
    31: ("Table column", "font", "text", True),
    33: ("Art", "label", "image", True),
    34: ("Track backdrop", "none", "image", False),
    35: ("Widget type 35", "bind", "none", False),
    36: ("Bottom bar", "bind", "none", False),
    37: ("Bar tab", "bind", "text", False),
    38: ("Widget type 38", "bind", "none", False),
    39: ("Movie", "bind", "movie", True),
    41: ("Full-screen picture", "none", "image", True),
    42: ("Fading picture button", "bind", "image", True),
    43: ("Message text", "bind", "font", True),
    44: ("Button list", "bind", "text", True),
    45: ("Rotating picture", "label", "image", True),
    46: ("Controller button glyph", "label", "shown", True),
    47: ("Widget type 47", "bind", "none", True),
    48: ("Tab list", "bind", "text", True),
    49: ("Choice from list", "bind", "text", True),
    50: ("Volume slider", "bind", "text", True),
    51: ("Tab box", "label", "text", True),
}
TEXT_SIZE_KINDS = (28, 31, 43)
PICTURE_EXTENSIONS = (".img", ".art", ".tex", ".gtx", ".fnt", ".m2v")

_KIND_EXPLAIN = {
    2: "A selectable button. The name is how the screen's code finds it; the text is the label.",
    3: "The game's loader prints 'not supported anymore' and skips this record, so it never appears.",
    17: "The game's loader prints 'not supported anymore' and skips this record, so it never appears.",
    28: "Draws one line of text with the font in the first string.",
    33: "Draws a picture or border art. A screen can swap it by registering the same name.",
    41: "Fills the whole screen with one picture.",
    46: "Draws the button glyph for the player's controller (PlayStation or keyboard pictures).",
}


def kind_label(kind):
    return KINDS[kind][0] if kind in KINDS else f"Widget type {kind}"


def _cstr(raw):
    end = raw.find(b"\0")
    return raw[:end if end >= 0 else len(raw)].decode("latin-1")


def _parse(data):
    """Validated structure shared by decode and encode."""
    if len(data) < HEADER_SIZE + RECORD:
        raise ValueError("shorter than the header plus the end marker record")
    if 0 not in data[4:0x24] or 0 not in data[0x28:0x64]:
        raise ValueError("a header name is not terminated")
    widgets, pos = [], HEADER_SIZE
    while True:
        if pos + RECORD > len(data):
            raise ValueError("the widget list has no end marker")
        kind = struct.unpack_from("<I", data, pos)[0]
        if kind == 0:
            break
        if len(widgets) >= MAX_RECORDS:
            raise ValueError("too many widgets")
        for lo, size in (SLOT1, SLOT2):
            if 0 not in data[pos + lo:pos + lo + size]:
                raise ValueError(f"widget {len(widgets)} has a name that is not terminated")
        widgets.append(pos)
        pos += RECORD
    return widgets, pos


def _record(data, pos, index):
    kind, style, x, y, depth, w, h = struct.unpack_from("<I6i", data, pos)
    return dict(index=index, pos=pos, kind=kind, style=style, x=x, y=y, depth=depth, w=w, h=h,
                s1=_cstr(data[pos + 0x20:pos + 0x40]), s2=_cstr(data[pos + 0x40:pos + 0x84]),
                group=struct.unpack_from("<i", data, pos + 0x88)[0],
                cue=struct.unpack_from("<I", data, pos + 0x8C)[0],
                effect=struct.unpack_from("<I", data, pos + 0x84)[0],
                size=struct.unpack_from("<f", data, pos + 0x1C)[0])


def _roles(rec):
    label, r1, r2, _ = KINDS.get(rec["kind"], (None, "bind", "text", False))
    return r1, r2


def _table_cells(rec):
    """(name, picture or font, text) shown in the widget table."""
    r1, r2 = _roles(rec)
    name = rec["s1"] if r1 in ("bind", "label") else ""
    asset = ""
    text = ""
    if r1 == "font":
        asset = rec["s1"]
    if r2 in ("image", "movie", "font", "shown"):
        asset = rec["s2"]
    elif r2 in ("text", "format"):
        text = rec["s2"]
    return name, asset, text


def _widget_title(rec):
    name, asset, text = _table_cells(rec)
    return name or text or asset or kind_label(rec["kind"])


def _screen_name(member):
    stem = str(member or "").rsplit("#", 1)[-1]
    return stem[:-4] if stem.lower().endswith(".lyt") else stem


# Screens the game's code names explicitly (docs/UI_MENU_ENGINE.md); others are described from their contents.
KNOWN_SCREENS = {
    "s_and_m": "the single-player or multiplayer choice shown when Action mode starts",
    "main": "the main menu (Action, Career, Load, Options)",
    "arcade": "the Action mode menu (Race, Challenge, Save)",
    "dd_chsr": "the Action mode vehicle selection screen",
    "buy_car": "the car purchase screen",
    "track1": "the track selection list",
    "track23": "the track selection list",
    "track4": "the track selection list",
    "yesnocan": "a Yes / No / Cancel prompt",
}


def _summary(member, archive, records, header):
    screen = _screen_name(member)
    counts = {}
    for rec in records:
        counts[kind_label(rec["kind"])] = counts.get(kind_label(rec["kind"]), 0) + 1
    parts = ", ".join(f"{n} {k.lower()}" for k, n in sorted(counts.items(), key=lambda kv: -kv[1])[:4])
    names = []
    for rec in records:
        r1, _ = _roles(rec)
        if r1 == "bind" and rec["s1"] and not rec["s1"].startswith("#") and rec["s1"] not in names:
            names.append(rec["s1"])
    keys = []
    for rec in records:
        _, r2 = _roles(rec)
        if r2 == "text" and ":" in rec["s2"] and " " not in rec["s2"] and rec["s2"] not in keys:
            keys.append(rec["s2"])
    known = KNOWN_SCREENS.get(screen.lower())
    text = f"Menu layout '{screen}'" + (f" for {known}" if known else "")
    text += f": {len(records)} widgets" + (f" ({parts})" if parts else "") + ". "
    if names:
        text += "Controls: " + ", ".join(names[:8]) + (", ..." if len(names) > 8 else "") + ". "
    if keys:
        text += "Game text keys: " + ", ".join(keys[:4]) + (", ..." if len(keys) > 4 else "") + ". "
    if header["background"]:
        text += f"Window background: {header['background']}. "
    text += ("The game builds every widget from this file when the screen opens, in a 640 x 480 area measured "
             "from the top left. A text starting with # is filled in by the screen's code; other text is looked "
             "up as a game text key and shown as written when no key matches.")
    return text


def _header(data):
    return dict(flag=struct.unpack_from("<I", data, 0)[0], background=_cstr(data[4:0x24]),
                has_back=data[0x24], camera_on=data[0x25], camera=_cstr(data[0x28:0x64]))


def decode(data, context):
    widgets, end = _parse(data)
    member = context.get("member")
    records = [_record(data, pos, i) for i, pos in enumerate(widgets)]
    header = _header(data)
    fields, blocks, warnings = [], [], []

    def field(fid, group, label, ftype, value, offset, size, explanation, evidence, confidence="high",
              unit="", editable=False, **extra):
        f = dict(id=fid, group=group, label=label, type=ftype, unit=unit, value=value, offset=offset, size=size,
                 editable=editable, confidence=confidence, explanation=explanation, evidence=evidence)
        f.update(extra)
        fields.append(f)

    def block(name, offset, size, explanation, known=True):
        blocks.append(dict(name=name, offset=offset, size=size, known=known, explanation=explanation))

    ev_head = "MenuRun set-up 0x00270430 and window builder 0x0027C980 (gen/rec_0*.c)"
    G = "Screen"
    field("header.flag", G, "Layout flag", "u32", header["flag"], 0, 4,
          "1 in 19 retail layouts and 0 in the rest. Nothing in the layout loader reads it.",
          "No reader found in 0x00270430 / 0x0027C980", "low")
    field("header.background", G, "Window background", "str", header["background"], 4, 32,
          "A .img picture drawn behind the widgets (it sets the window size and, in dialogs, centres the window), "
          "the word 'dialog' for the stock dialog frame, or empty for none. Missing pictures print 'missing "
          "background' and are skipped.", ev_head, "verified", editable=True, max_length=31)
    field("header.has_back", G, "Adds shared back widgets", "u8", header["has_back"], 0x24, 1,
          "1 also builds the widgets of the shared back.lyt layout on top of this screen.", ev_head, "high")
    field("header.camera_on", G, "Plays camera track", "u8", header["camera_on"], 0x25, 1,
          "1 plays the camera track named below while the screen is open; outside the 3D menu mode the game "
          "prints a message and skips it.", ev_head, "high")
    field("header.camera", G, "Camera track", "str", header["camera"], 0x28, 60,
          "Name of the 3D menu camera track (CameraDirectorPlayTrack 0x001D2DC8).", ev_head, "high")
    block("Padding", 0x26, 2, "Always 0. The game does not read it.")
    block("Build address", 0x64, 4, "Address from the tool that built the file. The loader replaces it with the "
          "address of the first widget when the file loads, so the stored value never matters.")

    ev_w = EVIDENCE
    for rec in records:
        pos, i, kind = rec["pos"], rec["index"], rec["kind"]
        label, r1, r2, uses_size = KINDS.get(kind, (f"Widget type {kind}", "bind", "text", False))
        group = f"{i + 1}. {_widget_title(rec)}"
        base = f"w{i}"
        skipped = kind in (3, 17)

        def wf(suffix, lab, ftype, value, off, size, expl, conf="high", unit="", editable=False, evidence=ev_w, **extra):
            field(f"{base}.{suffix}", group, lab, ftype, value, pos + off, size, expl, evidence, conf, unit, editable, **extra)

        wf("kind", "Widget type", "u32", kind, 0, 4,
           _KIND_EXPLAIN.get(kind, f"{label}. Selects which kind of control the game builds from this record."),
           "verified" if kind in KINDS else "medium")
        wf("style", "Style", "i32", rec["style"], 4, 4,
           "Variant number the control uses to choose its look or font size. The meaning differs by widget type.", "medium")
        edit_pos = not skipped
        why_skip = " The game skips this record, so a change has no effect." if skipped else ""
        wf("x", "X", "i32", rec["x"], 8, 4,
           "Distance from the left edge of the window (or screen) in pixels of a 640 x 480 area." + why_skip,
           "high", "px", edit_pos, min=-2048, max=2048)
        wf("y", "Y", "i32", rec["y"], 0xC, 4,
           "Distance from the top edge of the window (or screen) in pixels of a 640 x 480 area." + why_skip,
           "high", "px", edit_pos, min=-2048, max=2048)
        wf("depth", "Draw order", "i32", rec["depth"], 0x10, 4,
           "Third position value passed to the widget with x and y; widgets with the same value keep their record order.",
           "medium", "")
        size_note = "" if uses_size else " This widget type ignores it; its size comes from its picture or font."
        wf("w", "Width", "i32", rec["w"], 0x14, 4,
           "Width of the clickable and drawn area in pixels." + size_note + why_skip,
           "high" if uses_size else "medium", "px", edit_pos and uses_size, min=0, max=2048)
        wf("h", "Height", "i32", rec["h"], 0x18, 4,
           "Height of the clickable and drawn area in pixels." + size_note + why_skip,
           "high" if uses_size else "medium", "px", edit_pos and uses_size, min=0, max=2048)
        if kind in TEXT_SIZE_KINDS:
            wf("text_size", "Text size", "f32", round(rec["size"], 4), 0x1C, 4,
               "Height of the letters in pixels (read by the text builder 0x00280CE8).", "medium", "px")
        else:
            block(f"{group}: unused value", pos + 0x1C, 4, "Leftover bytes from the build tool; this widget type does not read them.")

        def string_field(suffix, lab, slot, role, value, expl_role):
            off, size = slot
            editable = bool(value) and not value.startswith("#") and not skipped
            if role in ("bind", "label", "none", "shown"):
                editable = False
            expl = {
                "bind": "Name the screen's code uses to find this control. Renaming it breaks that link, so it is read-only.",
                "label": "Name for this decoration; a screen can register a replacement under the same name.",
                "font": "Font file used to draw the text (.gtx glyph sheets, or .fnt in older screens). The name must exist in the game's resources.",
                "image": "Picture or art file drawn by this widget. The name must exist in the game's resources and keep its file ending.",
                "movie": "Video file played in this widget.",
                "text": "Game text key looked up in the language file (for example Generic:ACCEPT), or text shown as written when no key matches. A leading # means the screen's code supplies the text.",
                "format": "Number format used to print the value (printf style).",
                "none": "Not used by this widget type.",
                "shown": "The game picks the glyph sheet itself (ps2butns.img or ps2tbutns.img), so this name has no effect.",
            }[expl_role]
            conf = "high" if role != "none" else "medium"
            wf(suffix, lab, "str", value, off, size, expl, conf, "", editable, max_length=size - 1)

        lab1 = {"bind": "Name", "label": "Name", "font": "Font", "none": "First string", "image": "Picture"}[r1]
        string_field("s1", lab1, SLOT1, r1, rec["s1"], r1)
        lab2 = {"text": "Text", "format": "Number format", "image": "Picture", "movie": "Video", "font": "Font",
                "none": "Second string", "bind": "Name", "label": "Name", "shown": "Picture"}[r2]
        string_field("s2", lab2, SLOT2, r2, rec["s2"], r2)
        block(f"{group}: text effect", pos + 0x84, 4, "Text effect flag (2 in 240 text labels, otherwise 0).")
        wf("group", "Page group", "i32", rec["group"], 0x88, 4,
           "-1 keeps the widget out of page switching. 0 to 31 puts it in that group; the window shows, hides or "
           "animates a whole group at once (0x0027E418 stores the bit 1 << group, 0x0027E5A0 applies it).",
           "medium")
        wf("cue", "Interface cue", "u32", rec["cue"], 0x8C, 4,
           "Number the widget raises through the interface event table (0x002A0BA0) when it is used, 0 for none. "
           "What each number triggers is not mapped.", "low")
        block(f"{group}: colours and options", pos + 0x90, 0xB8 - 0x90,
              "Colour words (alpha, red, green, blue), text outline values and per-type options read by the widget builders. "
              "Not decoded field by field yet, kept unchanged.")
        block(f"{group}: build defaults", pos + 0xB8, RECORD - 0xB8,
              "A default style table copied from the build tool; the same values appear in every widget of a kind. Kept unchanged.")

    block("End marker", end, RECORD,
          "A record whose first word is 0 ends the list. The remaining bytes are scratch memory from the build tool "
          "and are not read.")
    tail = len(data) - end - RECORD
    if tail > 0:
        block("Trailing bytes", end + RECORD, tail, "Bytes after the end marker; the game does not read them.", False)
    blocks.sort(key=lambda b: b["offset"])

    rows = []
    for rec in records:
        name, asset, text = _table_cells(rec)
        rows.append([rec["index"] + 1, kind_label(rec["kind"]), name, rec["x"], rec["y"], rec["w"], rec["h"], asset, text])
    table = dict(kind="table", columns=["#", "Type", "Name", "X", "Y", "W", "H", "Image/Font", "Text"], rows=rows)
    view = dict(schema="eod.layt/1", fourcc="LAYT", title="Menu layout", confidence="high",
                summary=_summary(member, context.get("archive"), records, header), editable=True,
                fields=fields, blocks=blocks, preview=table, warnings=warnings)
    png = _schematic(data, context, records, header)
    if png:
        view["preview"] = dict(kind="image", png=png, width=SCREEN_W, height=SCREEN_H)
        view["table"] = table
    unknown = sorted({r["kind"] for r in records if r["kind"] not in KINDS})
    if unknown:
        warnings.append("Widget types " + ", ".join(map(str, unknown)) + " are not known; the game skips records "
                        "it has no builder for.")
    return view


# -- editing -------------------------------------------------------------------------------------

def _check_name(old, new, label, slot_size, keep_ending=True):
    if not isinstance(new, str):
        raise ValueError(f"{label} must be text.")
    try:
        raw = new.encode("latin-1")
    except UnicodeEncodeError:
        raise ValueError(f"{label} has a character the game's text encoding (Latin-1) cannot store.") from None
    if b"\0" in raw or any(c < 32 for c in raw):
        raise ValueError(f"{label} cannot contain control characters.")
    if len(raw) >= slot_size:
        raise ValueError(f"{label} can be at most {slot_size - 1} characters; this one has {len(raw)}. "
                         "Use a shorter name.")
    old_ext = old.lower().rsplit(".", 1)[-1] if "." in old else ""
    if keep_ending and old_ext in [e[1:] for e in PICTURE_EXTENSIONS] and not new.lower().endswith("." + old_ext):
        raise ValueError(f"{label} must keep the .{old_ext} file ending; the game picks how to load it from the ending.")
    return raw


def _int(value, label, lo, hi):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or int(value) != value:
        raise ValueError(f"{label} must be a whole number.")
    value = int(value)
    if not lo <= value <= hi:
        raise ValueError(f"{label} must be between {lo} and {hi}; {value} is outside that range.")
    return value


def encode(data, edits):
    widgets, end = _parse(data)
    out = bytearray(data)
    for key, value in edits.items():
        if key == "header.background":
            raw = _check_name(_cstr(data[4:0x24]), value, "The window background", 0x20, keep_ending=False)
            if raw and raw != b"dialog" and not raw.lower().endswith(b".img"):
                raise ValueError("The window background must be a .img picture, the word dialog, or empty.")
            _put_string(out, data, 4, 0x20, raw)
            continue
        head, _, item = key.partition(".")
        if not (head.startswith("w") and head[1:].isdigit() and int(head[1:]) < len(widgets)):
            raise ValueError(f"'{key}' is not an editable item. Use header.background or w<N>.x, .y, .w, .h, .s1, .s2.")
        i = int(head[1:])
        pos = widgets[i]
        rec = _record(data, pos, i)
        kind = rec["kind"]
        uses_size = KINDS.get(kind, (None, None, None, False))[3]
        who = f"Widget {i + 1} ({_widget_title(rec)})"
        if kind in (3, 17) and item in ("x", "y", "w", "h", "s1", "s2"):
            raise ValueError(f"{who} is a retired widget type that the game skips, so editing it has no effect.")
        if item in ("x", "y"):
            struct.pack_into("<i", out, pos + (8 if item == "x" else 0xC), _int(value, f"{who} {item.upper()}", -2048, 2048))
        elif item in ("w", "h"):
            if not uses_size:
                raise ValueError(f"{who} ignores width and height; its size comes from its picture or font.")
            struct.pack_into("<i", out, pos + (0x14 if item == "w" else 0x18), _int(value, f"{who} {'width' if item == 'w' else 'height'}", 0, 2048))
        elif item in ("s1", "s2"):
            r1, r2 = _roles(rec)
            role = r1 if item == "s1" else r2
            old = rec[item]
            if role in ("bind", "label", "none", "shown") or old.startswith("#") or not old:
                raise ValueError(f"{who}: this string names the control or is supplied by the screen's code, "
                                 "so changing it would break the link. Only fonts, pictures and visible text can be edited.")
            off, size = SLOT1 if item == "s1" else SLOT2
            raw = _check_name(old, value, f"{who} {'font or picture' if role in ('font', 'image', 'movie') else 'text'}", size)
            _put_string(out, data, pos + off, size, raw)
        else:
            raise ValueError(f"'{key}' is not an editable item. Use w{i}.x, .y, .w, .h, .s1 or .s2.")
    return bytes(out)


def _put_string(out, data, offset, size, raw):
    """Write the string and its terminator; bytes after the old terminator are left as they were."""
    old_end = data.find(b"\0", offset, offset + size)
    if old_end < 0:
        old_end = offset + size - 1
    new_end = offset + len(raw)
    out[offset:new_end] = raw
    for at in range(new_end, max(new_end + 1, old_end + 1)):
        out[at] = 0


# -- schematic preview ---------------------------------------------------------------------------

_GLYPHS = {
    "0": "111101101101111", "1": "010110010010111", "2": "111001111100111", "3": "111001111001111",
    "4": "101101111001001", "5": "111100111001111", "6": "111100111101111", "7": "111001001001001",
    "8": "111101111101111", "9": "111101111001111",
    "A": "010101111101101", "B": "110101110101110", "C": "011100100100011", "D": "110101101101110",
    "E": "111100110100111", "F": "111100110100100", "G": "011100101101011", "H": "101101111101101",
    "I": "111010010010111", "J": "001001001101010", "K": "101101110101101", "L": "100100100100111",
    "M": "101111111101101", "N": "110101101101101", "O": "010101101101010", "P": "110101110100100",
    "Q": "010101101111011", "R": "110101110101101", "S": "011100010001110", "T": "111010010010010",
    "U": "101101101101111", "V": "101101101101010", "W": "101101111111101", "X": "101101010101101",
    "Y": "101101010010010", "Z": "111001010100111",
    "_": "000000000000111", "-": "000000111000000", ".": "000000000000010", ":": "000010000010000",
    "#": "101111101111101", "%": "101001010100101", "/": "001001010100100", "!": "010010010000010",
    "?": "110001010000010", "(": "010100100100010", ")": "010001001001010", ",": "000000000010100",
    " ": "000000000000000", "'": "010010000000000", "+": "000010111010000", "=": "000111000111000",
    "*": "101010111010101", "<": "001010100010001", ">": "100010001010100",
}
_KIND_COLOURS = {
    "button": (110, 190, 255), "text": (240, 220, 120), "picture": (150, 220, 150),
    "input": (255, 160, 110), "other": (190, 160, 255),
}
_BUTTONS = (2, 3, 15, 16, 27, 42, 44, 46, 48, 51)
_TEXTS = (5, 6, 7, 28, 31, 43)
_PICTURES = (33, 34, 39, 41, 45, 17)


def _kind_colour(kind):
    if kind in _BUTTONS:
        return _KIND_COLOURS["button"]
    if kind in _TEXTS:
        return _KIND_COLOURS["text"]
    if kind in _PICTURES:
        return _KIND_COLOURS["picture"]
    if kind in (8, 9, 10, 11, 13, 14, 18, 20, 21, 23, 49, 50):
        return _KIND_COLOURS["input"]
    return _KIND_COLOURS["other"]


class _Canvas:
    def __init__(self, width, height, bg):
        self.w, self.h = width, height
        self.px = bytearray(bytes(bg) * (width * height))

    def put(self, x, y, colour):
        if 0 <= x < self.w and 0 <= y < self.h:
            i = (y * self.w + x) * 4
            self.px[i:i + 4] = bytes(colour)

    def hline(self, x0, x1, y, colour):
        if not 0 <= y < self.h:
            return
        x0, x1 = max(x0, 0), min(x1, self.w - 1)
        if x1 >= x0:
            self.px[(y * self.w + x0) * 4:(y * self.w + x1 + 1) * 4] = bytes(colour) * (x1 - x0 + 1)

    def vline(self, x, y0, y1, colour):
        if not 0 <= x < self.w:
            return
        for y in range(max(y0, 0), min(y1, self.h - 1) + 1):
            self.put(x, y, colour)

    def rect(self, x0, y0, x1, y1, colour):
        self.hline(x0, x1, y0, colour)
        self.hline(x0, x1, y1, colour)
        self.vline(x0, y0, y1, colour)
        self.vline(x1, y0, y1, colour)

    def text(self, x, y, string, colour, limit_x=None, scale=2):
        limit = self.w if limit_x is None else min(limit_x, self.w)
        for ch in string.upper():
            if x + 3 * scale > limit:
                break
            bits = _GLYPHS.get(ch, "111101101101111")
            for row in range(5):
                for col in range(3):
                    if bits[row * 3 + col] == "1":
                        for dy in range(scale):
                            for dx in range(scale):
                                self.put(x + col * scale + dx, y + row * scale + dy, colour)
            x += 4 * scale

    def blit(self, x0, y0, width, height, rgba):
        for row in range(height):
            y = y0 + row
            if not 0 <= y < self.h:
                continue
            sx0 = max(0, -x0)
            sx1 = min(width, self.w - x0)
            if sx1 <= sx0:
                continue
            src = rgba[(row * width + sx0) * 4:(row * width + sx1) * 4]
            alphas = src[3::4]
            dst = (y * self.w + x0 + sx0) * 4
            if min(alphas) == 255:
                self.px[dst:dst + len(src)] = src
            elif max(alphas) == 0:
                continue
            else:
                for k, a in enumerate(alphas):
                    if a >= 64:
                        self.px[dst + 4 * k:dst + 4 * k + 4] = src[4 * k:4 * k + 3] + b"\xff"


def _find_image(context, name, cache):
    """Decoded first frame (width, height, rgba) of a .img found in the layout's archive or COMMON.RES."""
    key = name.lower()
    if key in cache:
        return cache[key]
    result = None
    try:
        import modding_backend as backend  # deferred: modding_backend imports this package
        from . import imag
        disc = Path(context["disc"])
        for archive in (context["archive"], "DATA/RES/COMMON.RES", "DATA/RES/UIBIG.RES"):
            path = backend.archive_path(disc, archive)
            entries = cache.setdefault(("dir", archive), backend.directory(path))
            match = [e for e in entries if e["name"].lower() == key]
            if match:
                payload, _ = backend.payload(path, match[0]["name"], entries, verify_crc=False)
                width, height, count, offsets, stride = imag._parse(payload)
                result = (width, height, imag._frame_rgba(payload, width, height, offsets[0]))
                break
    except Exception:  # unreadable or missing pictures just leave the box empty
        result = None
    cache[key] = result
    return result


def _scaled(image, width, height):
    """Nearest-neighbour resize, the way the game stretches a picture over a widget's rectangle."""
    sw, sh, rgba = image
    cols = [min(sw - 1, c * sw // width) * 4 for c in range(width)]
    rows = []
    for r in range(height):
        sy = min(sh - 1, r * sh // height)
        line = rgba[sy * sw * 4:(sy + 1) * sw * 4]
        rows.append(b"".join(line[c:c + 4] for c in cols))
    return width, height, b"".join(rows)


def _schematic(data, context, records, header):
    """640 x 480 outline of the widgets with their pictures drawn in place; None when it cannot be made."""
    try:
        cache_dir = context.get("cache")
        cache_dir = Path(cache_dir) if cache_dir else Path(tempfile.gettempdir()) / "eod-format-cache"
        has_disc = bool(context.get("disc") and context.get("archive"))
        sha = hashlib.sha256(data + (b"d" if has_disc else b"n")).hexdigest()
        path = cache_dir / f"layt-{sha}.png"
        if path.exists():
            return str(path)
        canvas = _Canvas(SCREEN_W, SCREEN_H, (24, 27, 33, 255))
        for gx in range(0, SCREEN_W, 80):
            canvas.vline(gx, 0, SCREEN_H - 1, (36, 40, 48, 255))
        for gy in range(0, SCREEN_H, 80):
            canvas.hline(0, SCREEN_W - 1, gy, (36, 40, 48, 255))
        canvas.rect(0, 0, SCREEN_W - 1, SCREEN_H - 1, (90, 98, 112, 255))
        images = {}
        ox = oy = 0
        bg = header["background"]
        if bg.lower().endswith(".img") and has_disc:
            found = _find_image(context, bg, images)
            if found:
                width, height, rgba = found
                ox, oy = (SCREEN_W - width) // 2, (SCREEN_H - height) // 2
                canvas.blit(ox, oy, width, height, rgba)
                canvas.rect(ox, oy, ox + width - 1, oy + height - 1, (120, 128, 144, 255))
        for rec in records:
            retired = rec["kind"] in (3, 17)
            colour = (110, 116, 128, 255) if retired else _kind_colour(rec["kind"]) + (255,)
            x, y = ox + rec["x"], oy + rec["y"]
            uses_size = KINDS.get(rec["kind"], (None, None, None, False))[3]
            _, r2 = _roles(rec)
            if r2 in ("image", "shown") and not retired and has_disc and rec["s2"].lower().endswith(".img"):
                found = _find_image(context, rec["s2"], images)
                if found:
                    if rec["kind"] in (33, 41) and rec["w"] > 0 and rec["h"] > 0 and (rec["w"], rec["h"]) != found[:2]:
                        found = _scaled(found, min(rec["w"], SCREEN_W * 2), min(rec["h"], SCREEN_H * 2))
                    canvas.blit(x, y, *found)
                    if not uses_size:
                        canvas.rect(x, y, x + found[0] - 1, y + found[1] - 1, colour)
            if uses_size:
                canvas.rect(x, y, x + max(rec["w"], 1) - 1, y + max(rec["h"], 1) - 1, colour)
                label_x, limit = x + 2, x + rec["w"] - 2
            else:
                canvas.rect(x - 2, y - 2, x + 2, y + 2, colour)
                label_x, limit = x + 5, SCREEN_W
            title = rec["s2"] if r2 in ("image", "movie", "shown") and rec["s2"] else _widget_title(rec)
            if r2 in ("image", "movie", "shown") and "." in title:
                title = title.rsplit(".", 1)[0]
            ty = min(max(y + 2, 1), SCREEN_H - 12)
            lx = max(label_x, 2)
            canvas.text(lx + 1, ty + 1, title, (0, 0, 0, 255), limit + 1)
            canvas.text(lx, ty, title, colour, limit)
        cache_dir.mkdir(parents=True, exist_ok=True)
        _write_png(path, SCREEN_W, SCREEN_H, bytes(canvas.px))
        return str(path)
    except Exception:
        return None


def _write_png(path, width, height, rgba):
    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
    tmp = path.with_suffix(".tmp")
    tmp.write_bytes(png)
    tmp.replace(path)
