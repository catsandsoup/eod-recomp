"""Fallback view for types without a decoder: named unknown blocks, embedded text."""
import re

_TEXT = re.compile(rb"[\x20-\x7e]{4,}")


def decode(data, context):
    fourcc = context.get("fourcc", "????")
    blocks, cursor = [], 0
    for match in _TEXT.finditer(data):
        if match.start() > cursor:
            blocks.append(dict(name=f"Unknown block {len(blocks) + 1}", offset=cursor,
                               size=match.start() - cursor, known=False))
        blocks.append(dict(name="Text", offset=match.start(), size=match.end() - match.start(),
                           known=True, text=match.group().decode("ascii")))
        cursor = match.end()
    if cursor < len(data) or not data:
        blocks.append(dict(name=f"Unknown block {len(blocks) + 1}", offset=cursor,
                           size=len(data) - cursor, known=False))
    return dict(schema="eod.generic/1", fourcc=fourcc,
                title=f"{fourcc} file", confidence="low",
                summary=(f"The layout of {fourcc} files is not decoded yet. Contents are listed "
                         "as unknown blocks and any text found inside; saving keeps them unchanged."),
                fields=[], blocks=blocks, preview=None, warnings=[], editable=False)
