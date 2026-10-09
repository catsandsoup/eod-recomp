#!/usr/bin/env python3
"""Builds an EOD_PAD_SCRIPT value from button presses.

  python3 tools/padscript.py 400:start 1500:start 1700:down 1900:x ...
  python3 tools/padscript.py --hold 3000-6000:x+left 400:start ...

Each FRAME:BUTTONS taps the buttons for 10 vblanks; --hold A-B:BUTTONS holds
them from vblank A to B.  Buttons: start select x o square triangle up down
left right l1 r1 l2 r2, joined with '+'.  Example (Career mode with an
empty memory card, EOD_MEMCARD=/tmp/mc):
  EOD_PAD_SCRIPT=$(python3 tools/padscript.py 400:start 1500:start 1700:down \\
      1900:x 2200:x 2500:x 2800:x 3100:x) ./eod --data <disc folder>
"""
import sys

BUTTONS = {"select": 0x1, "start": 0x8, "up": 0x10, "right": 0x20, "down": 0x40, "left": 0x80,
           "l2": 0x100, "r2": 0x200, "l1": 0x400, "r1": 0x800, "triangle": 0x1000, "o": 0x2000,
           "x": 0x4000, "square": 0x8000}


def mask(names):
    v = 0
    for n in names.split("+"):
        v |= BUTTONS[n.lower()]
    return v


def main(args):
    spans = []  # (start, end, mask)
    it = iter(args)
    for a in it:
        if a == "--hold":
            rng, names = next(it).split(":")
            s, e = rng.split("-")
            spans.append((int(s), int(e), mask(names)))
        else:
            f, names = a.split(":")
            spans.append((int(f), int(f) + 10, mask(names)))
    # the script holds the last value set, so emit the state at every boundary
    points = sorted({p for s, e, _ in spans for p in (s, e)})
    out = []
    for p in points:
        v = 0
        for s, e, m in spans:
            if s <= p < e:
                v |= m
        out.append("%d:%04x" % (p, v))
    print(",".join(out))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    main(sys.argv[1:])
