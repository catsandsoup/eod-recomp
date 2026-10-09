#!/usr/bin/env python3
"""Function discovery for the EE executable.

Finds function entry points (call targets, code pointers in data, addresses
built with lui/addiu), detects switch jump tables and computes the set of
instructions that belongs to every function by control-flow traversal.

Output: build/functions.json, consumed by recomp.py.
"""
import json
import os
import sys
import time

import rabbitizer as rz

sys.path.insert(0, os.path.dirname(__file__))
from elf import Elf  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CAT = rz.InstrCategory.R5900
rz.config.pseudos_enablePseudos = False


def extra_valid(w):
    """Encodings rabbitizer rejects but the EE executes."""
    if (w >> 26) == 0x10 and ((w >> 21) & 31) in (0, 4) and ((w >> 11) & 31) == 25:
        return True  # mfpc/mtpc/mfps/mtps (performance counters)
    if (w & 0xFFE0003F) == 0x46000004:
        return True  # sqrt.s fd, ft
    if (w >> 26) == 0 and (w & 0x3F) in SHIFTS and ((w >> 11) & 31) == 0:
        return True  # shift into $zero with a non-zero rs field (data words executed as no-ops, e.g. libmpeg)
    return False

SHIFTS = (0x00, 0x02, 0x03, 0x38, 0x3A, 0x3B, 0x3C, 0x3E, 0x3F)

# Registers
RA = 31


def load_overrides(path):
    """config/functions.txt: lines '+addr' force a function, '-addr' reject a seed."""
    add, rem = set(), set()
    if os.path.exists(path):
        for line in open(path):
            line = line.split("#")[0].strip()
            if not line:
                continue
            if line[0] == "+":
                add.add(int(line[1:], 16))
            elif line[0] == "-":
                rem.add(int(line[1:], 16))
    return add, rem


class Program:
    def __init__(self, elf_path):
        self.elf = Elf(elf_path)
        text = self.elf.section(".text")
        self.text_lo = text.addr
        self.text_hi = text.addr + text.size
        n = text.size // 4
        self.raw = self.elf.words(self.text_lo, n)
        self.ins = [rz.Instruction(w, self.text_lo + 4 * k, CAT) for k, w in enumerate(self.raw)]

    def in_text(self, va):
        return self.text_lo <= va < self.text_hi and va % 4 == 0

    def at(self, va):
        return self.ins[(va - self.text_lo) >> 2]

    # ------------------------------------------------------------------ seeds
    def jal_targets(self):
        out = set()
        for i in self.ins:
            if i.getOpcodeName() == "jal":
                t = i.getInstrIndexAsVram()
                if self.in_text(t):
                    out.add(t)
        return out

    def data_pointers(self):
        """Words in initialised data sections that point into .text."""
        out = {}
        for name in (".data", ".rodata", ".sdata"):
            s = self.elf.section(name)
            if not s or s.size == 0:
                continue
            for k, w in enumerate(self.elf.words(s.addr, s.size // 4)):
                if self.in_text(w):
                    out.setdefault(w, []).append(s.addr + 4 * k)
        return out

    def jump_targets(self):
        """Targets of `j` (GCC uses it for tail calls)."""
        out = set()
        for i in self.ins:
            if i.getOpcodeName() == "j":
                t = i.getInstrIndexAsVram()
                if self.in_text(t):
                    out.add(t)
        return out

    def code_pointers(self):
        """Addresses into .text materialised by lui + addiu/ori."""
        out = set()
        hi = {}
        for i in self.ins:
            op = i.getOpcodeName()
            if op == "lui":
                hi[i.rt.value] = (i.getImmediate() << 16) & 0xFFFFFFFF
                continue
            if op in ("addiu", "daddiu", "ori") and i.rs.value in hi:
                base = hi[i.rs.value]
                imm = i.getProcessedImmediate() if op != "ori" else i.getImmediate()
                v = (base + imm) & 0xFFFFFFFF if op != "ori" else base | imm
                if self.in_text(v):
                    out.add(v)
            # any write invalidates
            try:
                d = i.getDestinationGpr()
            except Exception:
                d = None
            if d is not None and d.value in hi and not (op == "lui"):
                del hi[d.value]
        return out

    def looks_like_start(self, va):
        """Heuristic: previous instructions end a function."""
        if not self.in_text(va):
            return False
        if not self.at(va).isValid() and not extra_valid(self.at(va).getRaw()):
            return False
        if va == self.text_lo:
            return True
        i0 = self.at(va)
        if i0.getOpcodeName() in ("addiu", "daddiu") and i0.rs.value == 29 and i0.rt.value == 29 \
                and i0.getProcessedImmediate() < 0:
            return True  # stack frame prologue
        p1 = self.at(va - 4)
        if p1.getOpcodeName() in ("syscall", "break"):
            return True
        if p1.isNop():
            return True
        if va - 8 >= self.text_lo:
            p2 = self.at(va - 8)
            op = p2.getOpcodeName()
            if p2.isJrRa() or op in ("j", "b", "jr", "eret"):
                return True
        if p1.getOpcodeName() == "eret":
            return True
        return False

    # ------------------------------------------------------------ jump tables
    def find_jump_tables(self):
        """Return {jr_addr: {base, targets, limit}} for switch-style jumps.

        A tiny forward symbolic evaluation over the instructions preceding
        each `jr reg` tracks constants (lui/addiu/ori), "table base + index"
        sums (addu) and loads from them (lw).  The bound comes from the
        closest preceding sltiu.
        """
        secs = [self.elf.section(".rodata"), self.elf.section(".data")]
        tables = {}
        for idx, i in enumerate(self.ins):
            if i.getOpcodeName() != "jr" or i.rs.value == RA:
                continue
            jr_va = self.text_lo + 4 * idx
            val = {}   # reg -> ("c", value) | ("t", base) | ("l", tablebase)
            limit = None
            for k in range(max(idx - 24, 0), idx):
                p = self.ins[k]
                op = p.getOpcodeName()
                if op == "lui":
                    val[p.rt.value] = ("c", (p.getImmediate() << 16) & 0xFFFFFFFF)
                elif op in ("addiu", "daddiu") and val.get(p.rs.value, ("",))[0] == "c":
                    val[p.rt.value] = ("c", (val[p.rs.value][1] + p.getProcessedImmediate()) & 0xFFFFFFFF)
                elif op == "ori" and val.get(p.rs.value, ("",))[0] == "c":
                    val[p.rt.value] = ("c", val[p.rs.value][1] | p.getImmediate())
                elif op in ("addu", "daddu"):
                    a, b = val.get(p.rs.value), val.get(p.rt.value)
                    if a and a[0] == "c" and not b:
                        val[p.rd.value] = ("t", a[1])
                    elif b and b[0] == "c" and not a:
                        val[p.rd.value] = ("t", b[1])
                    else:
                        val.pop(p.rd.value, None)
                elif op == "lw" and val.get(p.rs.value, ("",))[0] == "t":
                    val[p.rt.value] = ("l", (val[p.rs.value][1] + p.getProcessedImmediate()) & 0xFFFFFFFF)
                elif op == "sltiu":
                    limit = p.getImmediate()
                    val.pop(p.rt.value, None)
                else:
                    try:
                        d = p.getDestinationGpr()
                    except Exception:
                        d = None
                    if d is not None:
                        val.pop(d.value, None)
            v = val.get(i.rs.value)
            if not v or v[0] != "l":
                continue
            tbase = v[1]
            sec = next((s for s in secs if s and s.contains(tbase)), None)
            if sec is None:
                continue
            targets = []
            n = 0
            while (limit is None and n < 1024) or (limit is not None and n < limit):
                va = tbase + 4 * n
                if not sec.contains(va):
                    break
                w = self.elf.read32(va)
                if not self.in_text(w) or abs(w - jr_va) > 0x40000:
                    break
                targets.append(w)
                n += 1
            if targets:
                tables[jr_va] = {"base": tbase, "targets": targets, "limit": limit}
        return tables

    # -------------------------------------------------------------- traversal
    def traverse(self, start, seeds, tables):
        """Collect instructions reachable from start. Returns (addrs, info)."""
        seen = set()
        work = [start]
        tailcalls = set()
        bad = None
        while work:
            va = work.pop()
            while True:
                if va in seen:
                    break
                if not self.in_text(va):
                    bad = f"ran off text at {va:08x}"
                    break
                if va != start and va in seeds:
                    # fell through into the next function: tail call
                    tailcalls.add(va)
                    break
                i = self.at(va)
                if not i.isValid() and not extra_valid(i.getRaw()):
                    bad = f"invalid instruction at {va:08x}"
                    break
                seen.add(va)
                op = i.getOpcodeName()
                if op == "eret":
                    break
                if i.hasDelaySlot():
                    seen.add(va + 4)
                    nxt = va + 8
                    if op in ("jal", "jalr", "bltzal", "bgezal", "bltzall", "bgezall"):
                        va = nxt
                        continue
                    if i.isJrRa():
                        break
                    if op == "jr":
                        if va in tables:
                            for t in tables[va]["targets"]:
                                if t in seeds and t != start:
                                    tailcalls.add(t)
                                else:
                                    work.append(t)
                        break
                    if op == "j":
                        t = i.getInstrIndexAsVram()
                        if t in seeds and t != start:
                            tailcalls.add(t)
                        else:
                            work.append(t)
                        break
                    # branch
                    t = i.getBranchVramGeneric()
                    if t in seeds and t != start:
                        tailcalls.add(t)
                    else:
                        work.append(t)
                    if i.isUnconditionalBranch() or op == "b":
                        break
                    va = nxt
                    continue
                if op == "eret":
                    break
                va += 4
        return seen, tailcalls, bad


def main():
    t0 = time.time()
    elf_path = os.path.join(ROOT, "SLUS_209.10")
    out_path = os.path.join(ROOT, "build", "functions.json")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    p = Program(elf_path)
    print(f"decoded {len(p.ins)} instructions in {time.time()-t0:.1f}s")

    add, rem = load_overrides(os.path.join(ROOT, "config", "functions.txt"))
    tables = p.find_jump_tables()
    table_targets = set(t for v in tables.values() for t in v["targets"])
    print(f"jump tables: {len(tables)} ({len(table_targets)} targets)")

    jal = p.jal_targets()
    dptr = p.data_pointers()
    cptr = p.code_pointers()
    seeds = set(jal) | {p.elf.entry} | add
    weak = (set(dptr) | cptr | p.jump_targets()) - seeds - table_targets
    accepted = {va for va in weak if p.looks_like_start(va)}
    if os.environ.get("ANALYZE_VERBOSE"):
        for va in sorted(weak - accepted):
            print("  rejected seed %08x (refs %s)" % (va, ",".join("%08x" % r for r in dptr.get(va, [])[:3])))
    seeds |= accepted
    seeds -= rem
    print(f"seeds: jal={len(jal)} dataptr={len(dptr)} codeptr={len(cptr)} weak-accepted={len(accepted)}/{len(weak)} total={len(seeds)}")

    # Unreferenced code after a function end (dead code / missed pointers):
    # start a function at each valid instruction following jr ra + delay +
    # padding that isn't covered by anything else.
    funcs = {}
    covered = set()
    order = sorted(seeds)
    for s in order:
        seen, tails, bad = p.traverse(s, seeds, tables)
        funcs[s] = (seen, tails, bad)
        covered |= seen
    # sweep for uncovered code
    extra = set()
    va = p.text_lo
    while va < p.text_hi:
        if va not in covered and va not in seeds:
            i = p.at(va)
            if i.isValid() and not i.isNop() and p.looks_like_start(va):
                extra.add(va)
                seen, tails, bad = p.traverse(va, seeds | extra, tables)
                funcs[va] = (seen, tails, bad)
                covered |= seen
        va += 4
    print(f"orphan functions: {len(extra)}")

    nbad = 0
    out = {"entry": p.elf.entry, "functions": [], "jump_tables": {}}
    for s in sorted(funcs):
        seen, tails, bad = funcs[s]
        if bad:
            nbad += 1
        out["functions"].append({
            "addr": s,
            "size": (max(seen) + 4 - s) if seen else 0,
            "n": len(seen),
            "ranges": ranges(seen),
            "bad": bad,
            "kind": "jal" if s in jal else ("data" if s in accepted else ("orphan" if s in extra else "forced")),
        })
    for k, v in tables.items():
        out["jump_tables"][str(k)] = v
    uncovered = sum(1 for k in range(len(p.ins)) if (p.text_lo + 4 * k) not in covered and not p.ins[k].isNop())
    print(f"functions: {len(out['functions'])}, bad: {nbad}, uncovered non-nop instrs: {uncovered}")
    json.dump(out, open(out_path, "w"))
    print(f"wrote {out_path} in {time.time()-t0:.1f}s")


def ranges(addrs):
    r = []
    for a in sorted(addrs):
        if r and r[-1][1] == a:
            r[-1][1] = a + 4
        else:
            r.append([a, a + 4])
    return r


if __name__ == "__main__":
    main()
