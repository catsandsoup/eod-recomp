#!/usr/bin/env python3
"""Fingerprint of every input that decides the contents of gen/.

  gen_inputs.py              print the sha256 of all inputs
  gen_inputs.py --list       print the input paths (absolute, one per line)
  gen_inputs.py --check DIR  exit 0 when DIR/.inputs.sha256 matches, 1 otherwise

tools/recomp.py writes the stamp after a full generation; CMake compares it at
configure time and regenerates gen/ when it differs or is missing.
"""
import hashlib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STAMP = ".inputs.sha256"
INPUTS = (
    "SLUS_209.10",
    "config/hle.txt",
    "config/symbols.txt",
    "config/functions.txt",
    "src/battle_layout.h",
    "tools/analyze.py",
    "tools/recomp.py",
    "tools/elf.py",
    "tools/gen_inputs.py",
)


def input_paths():
    return [os.path.join(ROOT, p) for p in INPUTS]


def compute():
    h = hashlib.sha256()
    for rel in INPUTS:
        path = os.path.join(ROOT, rel)
        h.update(rel.encode() + b"\0")
        if os.path.isfile(path):
            with open(path, "rb") as f:
                h.update(hashlib.sha256(f.read()).digest())
        else:
            h.update(b"missing")
    return h.hexdigest()


def write_stamp(gen_dir):
    with open(os.path.join(gen_dir, STAMP), "w") as f:
        f.write(compute() + "\n")


def matches(gen_dir):
    try:
        with open(os.path.join(gen_dir, STAMP)) as f:
            return f.read().strip() == compute()
    except OSError:
        return False


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--list":
        print("\n".join(input_paths()))
    elif len(sys.argv) > 2 and sys.argv[1] == "--check":
        sys.exit(0 if matches(sys.argv[2]) else 1)
    else:
        print(compute())
