"""Fallback view for types without a decoder: named unknown blocks, embedded text."""
import re

_TEXT = re.compile(rb"[\x20-\x7e]{6,}")


def _is_words(run):
    """Keep runs that read as text; skip pixel and table bytes that merely happen to be printable."""
    letters = sum(chr(b).isalpha() for b in run)
    return letters >= 4 and letters * 2 >= len(run)


def decode(data, context):
    fourcc = context.get("fourcc", "????")
    if not data:
        return dict(schema="eod.generic/1", fourcc=fourcc, title="Empty file", confidence="verified",
                    summary="This file is empty on the original disc, so there is nothing to see or edit. Changing it has no effect on the game.",
                    fields=[], blocks=[], preview=None, warnings=[], editable=False)
    blocks, cursor = [], 0
    for match in _TEXT.finditer(data):
        if not _is_words(match.group()):
            continue
        if match.start() > cursor:
            blocks.append(dict(name=f"Unknown block {len(blocks) + 1}", offset=cursor,
                               size=match.start() - cursor, known=False))
        blocks.append(dict(name="Text", offset=match.start(), size=match.end() - match.start(),
                           known=True, text=match.group().decode("ascii")))
        cursor = match.end()
    if cursor < len(data):
        blocks.append(dict(name=f"Unknown block {len(blocks) + 1}", offset=cursor,
                           size=len(data) - cursor, known=False))
    return dict(schema="eod.generic/1", fourcc=fourcc,
                title="Not yet understood", confidence="low",
                summary=("The Creation Kit cannot show this kind of file yet. Any readable text inside is listed "
                         "below, and the file is kept unchanged when you save."),
                fields=[], blocks=blocks, preview=None, warnings=[], editable=False)
