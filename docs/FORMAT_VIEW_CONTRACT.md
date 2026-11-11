# Format view contract (v1)

Every native asset type opens in the Creation Kit, and through MCP, as a
labelled view a modder can read and edit. A raw hex dump is an expert toggle,
never the primary view. Nothing on the disc is encrypted; a type without a
decoder still shows its structure as named unknown blocks.

## Decoder modules

`tools/formats/<name>.py`, one per type family:

- `FOURCCS`: tuple of type codes handled (for example `("SETU",)`).
- `decode(data: bytes, context: dict) -> dict`: the view below. `context` may
  carry `archive`, `member`, `disc` (read-only resolution of related assets).
  Raise `ValueError` for unrecognised layouts; the registry then falls back to
  the generic view with a plain warning.
- `encode(data: bytes, edits: dict) -> bytes` (editable types only): apply
  `{field id: new value}` to the original payload. With no edits the output is
  byte-identical. Unknown blocks are preserved byte-for-byte. Values outside a
  field's `min`/`max` raise `ValueError` with a plain sentence.

Callers use `tools/formats/__init__.py` (`describe`, `apply_edits`,
`decoder_for`, `supported`) only.

## View

```
schema      "eod.<name>/<version>"
fourcc      type code
title       what the file is, in player terms ("Car setup sheet")
summary     what it controls in game and how it was established
confidence  verified | high | medium | low   (whole-file)
editable    bool
fields      [Field]
blocks      [Block]      every byte range not covered by a field
preview     null | {kind: "image", png: <path>} | {kind: "path", points: [[x,y,z]...], closed: bool}
                 | {kind: "table", columns: [...], rows: [[...]]}
warnings    [plain sentences]
```

Field:

```
id           stable dotted id used for edits ("gear.1")
group        heading ("Gearbox", "Front left wheel")
label        "1st gear ratio"
type         f32 | u32 | i32 | u16 | i16 | u8 | str | enum | bool | vec3
unit         "psi", "m", "deg", "ratio", "s", "%", "" (none)
value        decoded value
offset,size  byte range in the payload
editable     bool
min,max      optional numeric limits enforced by encode
options      enum only: [{value, label}]
confidence   verified | high | medium | low
explanation  what changing it does in game, in plain words
evidence     how it was established (loader function in gen/, native run, data pattern)
```

Block: `{name, offset, size, known, explanation?, text?}`. Unknown regions are
named "Unknown block N" with their size.

Confidence words: `verified` = traced to the game's loader and observed in a
native run; `high` = traced to the loader or unambiguous data; `medium` =
consistent data pattern; `low` = guess. Labels for anything below `high` say
so in `explanation`.

## Product language

Titles, labels, explanations and warnings follow AGENTS.md: identify,
explain a consequence, resolve uncertainty or help recovery. Errors state what
happened, what it means for the modder, and what to do.

## Acceptance per type

A type is done when: its decoder covers every byte (fields or named blocks);
no-edit encode is byte-identical for every retail instance; the meaning of
editable fields is traced in `gen/` (cite the function); where meaningful an
edit is observed in a native game run; tests run headless. Status lives in
`docs/FORMAT_COVERAGE.md`.
