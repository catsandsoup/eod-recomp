"""LANG: the game's text table (`*.lng`), one entry per on-screen string.

Contract: docs/FORMAT_VIEW_CONTRACT.md. Layout and loader trace: docs/LANG_GAME_TEXT.md.

File layout (little endian):

    0x00  char[8]   language name ("English"), not read by the game
    0x08  u32       build version 0x7E4 (2020)
    0x0C  12 x u32  build-time object bookkeeping (pointers from the tool that wrote
                    the file); the game never reads them
    0x40  u32       entry count N
    0x44  N x (u32 key offset, u32 text offset)   offsets from the start of the file
    ...   string pool: 2N NUL-terminated Latin-1 strings, packed with no gaps

The game copies the whole file into memory, adds the load address to every offset
(LangLoad, 0x001342A8) and finds a string by a case-insensitive linear search of the
key column (0x001347D0, 0x00134620). Entry order is therefore not significant to the
game; it is kept exactly as shipped.
"""
import re
import struct

FOURCCS = ("LANG",)

HEADER_SIZE = 0x44
COUNT_OFFSET = 0x40
ENCODING = "latin-1"
# Static headroom after the language buffer in the executable (traced, not run-tested).
MAX_FILE_BYTES = 50004

_SPEC = re.compile(r"%[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|L|z)?[diouxXeEfgGcsp%]")

_HEADER_WORDS = {
    0x0C: ("Build pointer A", "Pointer value written by the tool that built the file. The game does not read it."),
    0x10: ("Build flag", "Constant 1 in every retail file. The game does not read it."),
    0x14: ("Build pointer B", "Pointer value written by the tool that built the file. The game does not read it."),
    0x18: ("Reserved 1", "Always 0. The game does not read it."),
    0x1C: ("Build constant", "Always 4. The game does not read it."),
    0x20: ("Build pointer C", "Pointer value written by the tool that built the file. The game does not read it."),
    0x24: ("Build constant 2", "Always 4. The game does not read it."),
    0x28: ("Reserved 2", "Always 0. The game does not read it."),
    0x2C: ("Build pointer D", "Pointer value written by the tool that built the file; differs between the two retail files. The game does not read it."),
    0x30: ("Reserved 3", "Always 0. The game does not read it."),
    0x34: ("Reserved 4", "Always 0. The game does not read it."),
    0x38: ("Build code address", "Address of code in the tool that built the file. The game does not read it."),
    0x3C: ("Reserved 5", "Always 0. The game does not read it."),
}


def specifiers(text):
    """Printf-style placeholders in order, for example ['%d']."""
    return _SPEC.findall(text)


def _parse(data):
    """Validated structure shared by decode and encode."""
    if len(data) < HEADER_SIZE:
        raise ValueError("shorter than the 68-byte LANG header")
    count = struct.unpack_from("<I", data, COUNT_OFFSET)[0]
    pool = HEADER_SIZE + 8 * count
    if count == 0 or pool >= len(data):
        raise ValueError(f"entry count {count} does not fit the file")
    if data[-1] != 0:
        raise ValueError("the last string is not terminated")
    offsets = struct.unpack_from(f"<{2 * count}I", data, HEADER_SIZE)
    starts = sorted(offsets)
    if starts[0] != pool or len(set(starts)) != len(starts):
        raise ValueError("string offsets do not start at the end of the index or repeat")
    spans = {}
    for i, start in enumerate(starts):
        stop = starts[i + 1] if i + 1 < len(starts) else len(data)
        if start >= len(data) or data[stop - 1] != 0 or 0 in data[start:stop - 1]:
            raise ValueError("strings are not packed back to back with one terminator each")
        spans[start] = stop
    entries = []
    for i in range(count):
        key_at, text_at = offsets[2 * i], offsets[2 * i + 1]
        entries.append(dict(
            index=i, key_at=key_at, text_at=text_at,
            key=data[key_at:spans[key_at] - 1].decode(ENCODING),
            text=data[text_at:spans[text_at] - 1].decode(ENCODING),
            key_end=spans[key_at], text_end=spans[text_at]))
    return count, pool, entries


def _group(key):
    return key.split(":", 1)[0] if ":" in key else "General"


def _explain(text):
    parts = []
    if "\n" in text:
        parts.append("The line break splits this label over two lines.")
    for spec in specifiers(text):
        if spec != "%%":
            parts.append(f"{spec} is filled in by the game (for example a number) and must stay.")
            break
    return " ".join(parts)


def _summary(member, count):
    if member.lower() == "usenglish_lang.lng":
        return (f"Every piece of English text the game shows: menu labels, option names, dialogs, driver names, "
                f"upgrade names and descriptions, units, control names. {count} entries. The executable loads this file "
                "at start-up (LangLoad) and each screen looks its text up by the key shown for each line, ignoring "
                "capitals. A line that is missing from this file is shown as its key instead. "
                "A line break in a value is written \\n.")
    if member.lower() == "english.lng":
        return (f"A {count}-entry English text table stored in the boot resources. The retail executable does not "
                "name this file (it only loads the one in the locale resources), so editing it is not expected to "
                "change anything on screen; the layout is identical to the main text table.")
    return (f"A text table with {count} entries, each a key and the text shown in game. Screens look their text up by key, "
            "ignoring capitals; a missing key shows as the key itself.")


def decode(data, context):
    count, pool, entries = _parse(data)
    member = str(context.get("member") or "")
    fields, blocks = [], []

    name = data[0:8].split(b"\0", 1)[0].decode(ENCODING)
    fields.append(dict(
        id="header.name", group="File header", label="Language name", type="str", unit="", value=name,
        offset=0, size=8, editable=False, confidence="high",
        explanation="Label of the language. The game takes its language name from the executable, not from this field.",
        evidence="LangInit 0x00134048 stores the display name 'English (US)' itself; no read of this field in gen/."))
    fields.append(dict(
        id="header.version", group="File header", label="Table version", type="u32", unit="",
        value=struct.unpack_from("<I", data, 8)[0], offset=8, size=4, editable=False, confidence="medium",
        explanation="Build version of the table, 0x7E4 in both retail files. The game does not check it.",
        evidence="Retail data; LangLoad 0x001342A8 reads only the count and entries."))
    for offset, (label, explanation) in _HEADER_WORDS.items():
        fields.append(dict(
            id=f"header.{offset:02x}", group="File header", label=label, type="u32", unit="",
            value=struct.unpack_from("<I", data, offset)[0], offset=offset, size=4, editable=False,
            confidence="medium", explanation=explanation,
            evidence="Retail data pattern; LangLoad 0x001342A8 never touches offsets 0x08 to 0x3F."))
    fields.append(dict(
        id="header.count", group="File header", label="Number of entries", type="u32", unit="",
        value=count, offset=COUNT_OFFSET, size=4, editable=False, confidence="verified",
        explanation="How many key/text pairs follow. Kept in step automatically when the file is saved.",
        evidence="LangLoad 0x001342A8 loops over this count to add the load address to every offset; "
                 "LangGetString 0x001347D0 stops searching at it."))
    blocks.append(dict(
        name="String index", offset=HEADER_SIZE, size=8 * count, known=True,
        explanation="For each entry, where its key and its text start in the file. Rebuilt automatically when text changes length."))

    for e in entries:
        text = e["text"]
        blocks.append(dict(
            name=f"Key {e['index']}", offset=e["key_at"], size=e["key_end"] - e["key_at"], known=True,
            text=e["key"], explanation="Name a screen uses to look this text up. Changing it would make the text unreachable."))
        field = dict(
            id=f"string.{e['index']}", group=_group(e["key"]), label=e["key"], type="str", unit="", value=text,
            offset=e["text_at"], size=e["text_end"] - e["text_at"], editable=True, confidence="verified",
            explanation=_explain(text),
            evidence="Returned by LangGetString 0x001347D0 / LocalizedStringResolve 0x00134620 for this key.")
        fields.append(field)
    blocks.sort(key=lambda b: b["offset"])

    rows = [[e["index"], e["key"], e["text"].replace("\n", "\\n"), " ".join(specifiers(e["text"]))] for e in entries]
    warnings = []
    if len(data) > MAX_FILE_BYTES:
        warnings.append("This file is larger than the memory the game reserves after the text buffer is believed "
                        "to allow; the game may crash while loading it.")
    return dict(
        schema="eod.lang/1", fourcc="LANG", title="Game text", confidence="high",
        summary=_summary(member, count), editable=True, fields=fields, blocks=blocks,
        preview=dict(kind="table", columns=["ID", "Key", "Text", "Placeholders"], rows=rows),
        warnings=warnings)


def validate_text(old, new, label="text"):
    """Raise ValueError with a plain sentence when `new` cannot replace `old`."""
    if not isinstance(new, str):
        raise ValueError(f"{label} must be text.")
    if "\0" in new:
        raise ValueError(f"{label} cannot contain a null character; it ends the text early in game.")
    try:
        new.encode(ENCODING)
    except UnicodeEncodeError as error:
        raise ValueError(f"{label} contains '{new[error.start]}', which the game's text encoding (Latin-1) "
                         "cannot store. Use letters, digits, punctuation and Western European accents.") from None
    for ch in new:
        if ord(ch) < 32 and ch != "\n" or 0x7F <= ord(ch) < 0xA0:
            raise ValueError(f"{label} contains a control character (code {ord(ch)}) the game does not understand. "
                             "Only the line break is allowed.")
    if specifiers(new) != specifiers(old):
        want = " ".join(specifiers(old)) or "none"
        got = " ".join(specifiers(new)) or "none"
        raise ValueError(f"{label} must keep the game's placeholders in the same order (needs: {want}; has: {got}). "
                         "The game fills them in with numbers or names; changing them can crash it.")


def encode(data, edits):
    count, pool, entries = _parse(data)
    by_id = {f"string.{e['index']}": e for e in entries}
    texts = {e["index"]: e["text"] for e in entries}
    for field_id, value in edits.items():
        entry = by_id.get(field_id)
        if entry is None:
            raise ValueError(f"'{field_id}' is not an editable line of this text file. Only string.N lines can be changed.")
        validate_text(entry["text"], value, f"Line '{entry['key']}'")
        texts[entry["index"]] = value
    # Re-pack the pool in its original order so unedited files are byte-identical.
    slots = sorted([(e["key_at"], e["index"], "key") for e in entries] +
                   [(e["text_at"], e["index"], "text") for e in entries])
    by_index = {e["index"]: e for e in entries}
    out = bytearray(data[:pool])
    offsets = {}
    for _, index, kind in slots:
        offsets[(index, kind)] = len(out)
        raw = (by_index[index]["key"] if kind == "key" else texts[index]).encode(ENCODING)
        out += raw + b"\0"
    for index in range(count):
        struct.pack_into("<II", out, HEADER_SIZE + 8 * index, offsets[(index, "key")], offsets[(index, "text")])
    if len(out) > MAX_FILE_BYTES:
        raise ValueError(f"The edited text file is {len(out)} bytes; the game reserves room for about {MAX_FILE_BYTES}. "
                         "Shorten some text so the game does not overrun its buffer.")
    return bytes(out)
