#!/usr/bin/env python3
"""Regression test for config/symbols.txt, tools/recomp.py, and tools/dis.py."""
import json
import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SYMBOLS_PATH = os.path.join(ROOT, "config", "symbols.txt")
HLE_PATH = os.path.join(ROOT, "config", "hle.txt")
FUNCS_PATH = os.path.join(ROOT, "build", "functions.json")

LINE_RE = re.compile(
    r"^([0-9A-Fa-f]{8})\s+([A-Za-z_][A-Za-z0-9_]*)\s+(\[[VI]\])\s+#\s+(.+)$"
)
DOC_LINK_RE = re.compile(r"->\s*(docs/[A-Za-z0-9_./-]+\.md)")


class TestSymbolsDB(unittest.TestCase):
    def test_symbols_format_and_uniqueness(self):
        self.assertTrue(os.path.exists(SYMBOLS_PATH))
        seen_addrs = {}
        seen_names = {}
        entries = []

        with open(SYMBOLS_PATH, encoding="utf-8") as f:
            for lineno, raw in enumerate(f, 1):
                s = raw.strip()
                if not s or s.startswith("#"):
                    continue
                m = LINE_RE.match(s)
                self.assertIsNotNone(
                    m, f"config/symbols.txt:{lineno}: invalid format: {s!r}"
                )
                addr_s, name, tag, note = m.groups()
                addr = int(addr_s, 16)
                self.assertNotIn(
                    addr,
                    seen_addrs,
                    f"duplicate address 0x{addr:08X} at line {lineno}",
                )
                self.assertNotIn(
                    name,
                    seen_names,
                    f"duplicate symbol name {name!r} at line {lineno}",
                )
                seen_addrs[addr] = lineno
                seen_names[name] = lineno
                entries.append((addr, name, tag, note, lineno))

                for doc_rel in DOC_LINK_RE.findall(note):
                    doc_abs = os.path.join(ROOT, doc_rel)
                    self.assertTrue(
                        os.path.exists(doc_abs),
                        f"config/symbols.txt:{lineno}: missing linked doc {doc_rel}",
                    )

        self.assertGreaterEqual(len(entries), 130)
        # Ensure entries are sorted by ascending address
        addrs = [e[0] for e in entries]
        self.assertEqual(addrs, sorted(addrs), "config/symbols.txt must be sorted by address")

    def test_all_hle_addresses_are_named(self):
        with open(SYMBOLS_PATH, encoding="utf-8") as f:
            sym_addrs = {
                int(line.split()[0], 16)
                for line in f
                if line.strip() and not line.strip().startswith("#")
            }
        with open(HLE_PATH, encoding="utf-8") as f:
            for lineno, raw in enumerate(f, 1):
                body = raw.split("#")[0].strip()
                if not body:
                    continue
                addr = int(body.split()[0], 16)
                self.assertIn(
                    addr,
                    sym_addrs,
                    f"config/hle.txt:{lineno}: HLE address 0x{addr:08X} missing from config/symbols.txt",
                )

    def test_function_symbols_match_analyzed_functions(self):
        if not os.path.exists(FUNCS_PATH):
            self.skipTest("build/functions.json not present")
        with open(FUNCS_PATH, encoding="utf-8") as f:
            funcs = json.load(f)["functions"]
        ranges = [(lo, hi) for fn in funcs for (lo, hi) in fn["ranges"]]

        with open(SYMBOLS_PATH, encoding="utf-8") as f:
            for lineno, raw in enumerate(f, 1):
                s = raw.strip()
                if not s or s.startswith("#"):
                    continue
                addr = int(s.split()[0], 16)
                if 0x00100000 <= addr < 0x003BC290:
                    self.assertEqual(addr % 4, 0, f"config/symbols.txt:{lineno}: unaligned text address 0x{addr:08X}")
                    in_range = any(lo <= addr < hi for (lo, hi) in ranges)
                    self.assertTrue(
                        in_range,
                        f"config/symbols.txt:{lineno}: text address 0x{addr:08X} is outside analyzed code ranges in build/functions.json",
                    )


if __name__ == "__main__":
    unittest.main()
