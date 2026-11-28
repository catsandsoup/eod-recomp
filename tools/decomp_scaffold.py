#!/usr/bin/env python3
"""Fast decompilation scaffold & dossier generator for EOD-Recomp.

Given hex addresses (or --range LO:HI), extracts in ~1 second:
  - Exact generated recompiled C body from gen/rec_*.c
  - Existing Ghidra C output from D:/tools/ghidra_out/<ADDR>.c (if present)
  - Direct callees (f_XXXXXXXX) & symbols from config/symbols.txt / config/hle.txt
  - Optional ready-to-edit C++ skeleton with Frame RAII, EE helpers, and diff_run hooks.

Usage:
  python tools/decomp_scaffold.py 001779C0 00177B40 00178258 --dump d:/eod-build/t10_dossier.txt
  python tools/decomp_scaffold.py --range 00177900:00178400 --list
  python tools/decomp_scaffold.py 001779C0 00177B40 --skeleton src/decomp_mmo_sim.cpp
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GEN_DIR = ROOT / "gen"
SYM_FILE = ROOT / "config" / "symbols.txt"
HLE_FILE = ROOT / "config" / "hle.txt"
GHIDRA_OUT = Path(r"D:\tools\ghidra_out")


def load_symbols() -> dict[str, str]:
    syms: dict[str, str] = {}
    if SYM_FILE.exists():
        for line in SYM_FILE.read_text(encoding="utf-8", errors="replace").splitlines():
            m = re.match(r"^\s*([0-9A-Fa-f]{8})\s+(\S+)", line)
            if m:
                syms[m.group(1).upper()] = m.group(2)
    return syms


def load_hle() -> dict[str, str]:
    hle: dict[str, str] = {}
    if HLE_FILE.exists():
        for line in HLE_FILE.read_text(encoding="utf-8", errors="replace").splitlines():
            m = re.match(r"^\s*([0-9A-Fa-f]{8})\s+(.*)", line)
            if m:
                hle[m.group(1).upper()] = m.group(2).strip()
    return hle


def load_table_addrs() -> list[int]:
    t = GEN_DIR / "table.c"
    if not t.exists():
        return []
    return sorted(
        {int(m.group(1), 16) for m in re.finditer(r"0x([0-9A-Fa-f]{8}),\s*f_", t.read_text(encoding="utf-8", errors="replace"))}
    )


def extract_rec_bodies(target_addrs: set[str]) -> dict[str, tuple[str, str]]:
    """Return {ADDR8: (rec_filename, function_body_text)}."""
    found: dict[str, tuple[str, str]] = {}
    remaining = set(target_addrs)
    header_pat = re.compile(r"^void\s+f_([0-9A-Fa-f]{8})(?:_orig)?\s*\(\s*void\s*\)\s*\{")
    for rec in sorted(GEN_DIR.glob("rec_*.c")):
        if not remaining:
            break
        lines = rec.read_text(encoding="utf-8", errors="replace").splitlines()
        i = 0
        n = len(lines)
        while i < n and remaining:
            m = header_pat.match(lines[i])
            if m:
                addr = m.group(1).upper()
                if addr in remaining:
                    body = [lines[i]]
                    depth = lines[i].count("{") - lines[i].count("}")
                    i += 1
                    while i < n and depth > 0:
                        body.append(lines[i])
                        depth += lines[i].count("{") - lines[i].count("}")
                        i += 1
                    found[addr] = (rec.name, "\n".join(body))
                    remaining.remove(addr)
                    continue
            i += 1
    return found


def find_callees(body: str, self_addr: str) -> list[str]:
    callees = sorted({m.group(1).upper() for m in re.finditer(r"\bf_([0-9A-Fa-f]{8})\b", body)})
    return [c for c in callees if c != self_addr]


def generate_skeleton(addrs: list[str], bodies: dict[str, tuple[str, str]], syms: dict[str, str], out_cpp: Path) -> None:
    all_callees: set[str] = set()
    for a in addrs:
        if a in bodies:
            all_callees.update(find_callees(bodies[a][1], a))
    extern_decls = "\n".join(f"void f_{c}(void);" for c in sorted(all_callees))
    orig_decls = "\n".join(f"void f_{a}_orig(void);" for a in addrs)

    fn_stubs = []
    diff_wrappers = []
    for a in addrs:
        sym = syms.get(a, f"func_{a.lower()}")
        safe = re.sub(r"[^A-Za-z0-9_]", "_", sym)
        fn_stubs.append(f"""// ---- 0x{a} {sym} ----
void decomp_{a.lower()}() {{
    Frame frame;
    // TODO: implement bit-exact C++ for 0x{a} ({sym})
    f_{a}_orig();
}}
""")
        diff_wrappers.append(f'extern "C" void f_{a}(void) {{ diff_run(0x{a}u, decomp_{a.lower()}, f_{a}_orig); }}')

    cpp = f"""// Auto-scaffolded by tools/decomp_scaffold.py — replace TODO bodies with clean, bit-exact C++.
#include "core/cpu.h"
#include "core/diff_harness.h"
#include "core/memory.h"

#include <cstdint>
#include <cstring>

extern "C" {{
{extern_decls}
{orig_decls}
}}

namespace {{

inline uint32_t reg(int r) {{ return g_ee.r[r].uw[0]; }}
inline int32_t sreg(int r) {{ return g_ee.r[r].sw[0]; }}
inline void set(int r, int32_t v) {{ g_ee.r[r].sd[0] = v; }}
inline void setu(int r, uint32_t v) {{ g_ee.r[r].sd[0] = (int32_t)v; }}
inline uint32_t lwu(uint32_t a) {{ return MEM_LW(a); }}
inline int32_t lw(uint32_t a) {{ return (int32_t)MEM_LW(a); }}
inline void sw(uint32_t a, uint32_t v) {{ MEM_SW(a, v); }}
inline float bits_f(uint32_t u) {{ float f; std::memcpy(&f, &u, 4); return f; }}
inline uint32_t f_bits(float f) {{ uint32_t u; std::memcpy(&u, &f, 4); return u; }}
inline float rf(uint32_t a) {{ return bits_f(MEM_LW(a)); }}
inline void sf(uint32_t a, float f) {{ MEM_SW(a, f_bits(f)); }}
inline void call_fn(void (*fn)(void), uint32_t ra) {{ set(31, (int32_t)ra); fn(); }}

struct Frame {{
    uint64_t saved_sp;
    uint64_t saved_ra;
    Frame() : saved_sp(g_ee.r[29].ud[0]), saved_ra(g_ee.r[31].ud[0]) {{}}
    ~Frame() {{ g_ee.r[29].ud[0] = saved_sp; g_ee.r[31].ud[0] = saved_ra; }}
}};

{chr(10).join(fn_stubs)}
}}  // namespace

{chr(10).join(diff_wrappers)}
"""
    out_cpp.write_text(cpp, encoding="utf-8")
    print(f"Wrote skeleton: {out_cpp} ({len(addrs)} functions)")


def main() -> None:
    ap = argparse.ArgumentParser(description="EOD-Recomp decompilation dossier & skeleton generator")
    ap.add_argument("addrs", nargs="*", help="Hex addresses (e.g. 001779C0 0x177B40)")
    ap.add_argument("--range", dest="addr_range", help="Inclusive hex range LO:HI from gen/table.c")
    ap.add_argument("--list", action="store_true", help="List functions in range with line counts & HLE status")
    ap.add_argument("--dump", type=Path, help="Write full dossier (recomp C + Ghidra C + callees) to file")
    ap.add_argument("--skeleton", type=Path, help="Generate ready-to-edit C++ skeleton file")
    args = ap.parse_args()

    syms = load_symbols()
    hle = load_hle()

    addrs: list[str] = []
    for raw in args.addrs:
        a = raw.lower().removeprefix("0x").upper().zfill(8)
        addrs.append(a)

    if args.addr_range:
        lo_s, hi_s = args.addr_range.split(":")
        lo, hi = int(lo_s, 16), int(hi_s, 16)
        for a in load_table_addrs():
            if lo <= a <= hi:
                addrs.append(f"{a:08X}")

    addrs = list(dict.fromkeys(addrs))
    if not addrs:
        ap.error("Provide hex addresses or --range LO:HI")

    bodies = extract_rec_bodies(set(addrs))

    if args.list or (not args.dump and not args.skeleton):
        print(f"{'ADDR':10} {'LINES':6} {'CALLEES':8} {'HLE':6} {'GHIDRA':6} SYMBOL")
        for a in addrs:
            rec_info = bodies.get(a)
            lines_n = len(rec_info[1].splitlines()) if rec_info else 0
            callees = find_callees(rec_info[1], a) if rec_info else []
            has_hle = "yes" if a in hle else "-"
            has_gh = "yes" if (GHIDRA_OUT / f"{a}.c").exists() else "-"
            sym = syms.get(a, "")
            print(f"0x{a} {lines_n:6d} {len(callees):8d} {has_hle:6} {has_gh:6} {sym}")

    if args.dump:
        parts = []
        for a in addrs:
            sym = syms.get(a, f"f_{a}")
            rec_info = bodies.get(a)
            callees = find_callees(rec_info[1], a) if rec_info else []
            gh_path = GHIDRA_OUT / f"{a}.c"
            parts.append(f"{'='*80}\nFUNCTION 0x{a} ({sym})\nHLE: {hle.get(a, 'none')}\nCallees ({len(callees)}): {', '.join('0x'+c for c in callees)}\n")
            if rec_info:
                parts.append(f"--- {rec_info[0]} ({len(rec_info[1].splitlines())} lines) ---\n{rec_info[1]}\n")
            else:
                parts.append("--- NOT FOUND IN gen/rec_*.c ---\n")
            if gh_path.exists():
                parts.append(f"--- Ghidra ({gh_path}) ---\n{gh_path.read_text(encoding='utf-8', errors='replace')}\n")
        args.dump.write_text("\n".join(parts), encoding="utf-8")
        print(f"Wrote dossier: {args.dump} ({len(addrs)} functions)")

    if args.skeleton:
        generate_skeleton(addrs, bodies, syms, args.skeleton)


if __name__ == "__main__":
    main()
