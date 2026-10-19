#!/usr/bin/env python3
"""Extract native weapon routines from locally generated code; never ship output."""
import pathlib
import re
import sys


def main():
    source, target = map(pathlib.Path, sys.argv[1:])
    # Keep trigger, cooldown, timer transition and pose copy native. Only the
    # scene/physics boundary beneath these functions is provided by the test.
    names = ("0017E000", "0017E4D8", "0017E5C8", "0017AF80")
    functions = {}
    for path in sorted(source.glob("rec_*.c")):
        text = path.read_text()
        for name in names:
            match = re.search(r"void f_" + name + r"(?:_orig)?\(void\) \{", text)
            if not match:
                continue
            if name in functions:
                raise SystemExit(f"Duplicate generated function {name}")
            pos, depth = match.end(), 1
            while depth and pos < len(text):
                depth += (text[pos] == "{") - (text[pos] == "}")
                pos += 1
            if depth:
                raise SystemExit(f"Unterminated generated function {name}")
            functions[name] = text[match.start():pos]
    missing = set(names) - functions.keys()
    if missing:
        raise SystemExit("Generate rec_*.c before building native weapon tests: " + ", ".join(sorted(missing)))
    code = "\n\n".join(functions[name] for name in names)
    for name in names:
        code = re.sub(r"\bf_" + name + r"(?:_orig)?(?=\()", "reference_" + name, code)
    dependencies = sorted(set(re.findall(r"\bf_[0-9A-F]{8}(?=\()", code)))
    declarations = ["void reference_" + name + "(void);" for name in names]
    declarations += ["void " + name + "(void);" for name in dependencies]
    target.write_text('/* Local disc-derived test reference; never commit. */\n#include "ee.h"\n'
                      + "\n".join(declarations) + "\n\n" + code + "\n")


if __name__ == "__main__":
    main()
