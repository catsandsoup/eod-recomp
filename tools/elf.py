"""Minimal ELF32 loader for the PS2 executable."""
import struct


class Section:
    def __init__(self, name, typ, flags, addr, off, size):
        self.name, self.type, self.flags = name, typ, flags
        self.addr, self.off, self.size = addr, off, size

    def contains(self, va):
        return self.addr <= va < self.addr + self.size

    def __repr__(self):
        return f"<{self.name} {self.addr:08x}+{self.size:x}>"


class Elf:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        assert d[:4] == b"\x7fELF"
        (self.entry, phoff, shoff) = struct.unpack_from("<III", d, 0x18)
        (phentsize, phnum, shentsize, shnum, shstrndx) = struct.unpack_from("<HHHHH", d, 0x2A)
        raw = []
        for i in range(shnum):
            raw.append(struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize))
        strtab_off = raw[shstrndx][4]
        self.sections = []
        for (name, typ, flags, addr, off, size, *_r) in raw:
            end = d.index(b"\0", strtab_off + name)
            nm = d[strtab_off + name:end].decode()
            self.sections.append(Section(nm, typ, flags, addr, off, size))
        self.segments = []
        for i in range(phnum):
            (typ, off, va, pa, fsz, msz, flg, al) = struct.unpack_from("<IIIIIIII", d, phoff + i * phentsize)
            if typ == 1:
                self.segments.append((va, off, fsz, msz))

    def section(self, name):
        for s in self.sections:
            if s.name == name:
                return s
        return None

    def file_offset(self, va):
        for (sva, off, fsz, msz) in self.segments:
            if sva <= va < sva + fsz:
                return off + va - sva
        return None

    def read32(self, va):
        off = self.file_offset(va)
        if off is None:
            return None
        return struct.unpack_from("<I", self.data, off)[0]

    def words(self, va, count):
        off = self.file_offset(va)
        return struct.unpack_from("<%dI" % count, self.data, off)
