// hle_heapcheck.cpp - developer heap checker for the game's own stack allocator
// (MemAlloc 0x12DDB8 / MemFree 0x12DB78, pool 0x3CC438). Off unless EOD_HEAP_CHECK is set.
//
// Block layout (from 0x12DDB8): 0x40-byte header { +0 'STAK', +4 name[0x20], +0x24 in-use,
// +0x28 size (request rounded up to 64), +0x2C request, +0x30 previous block, +0x3C 'STAK' },
// then the data, then a 'STAK' footer word at data + ((request + 3) & ~3). Blocks are
// contiguous from pool+0xC for pool+4 bytes. The native MemFree checks only the freed
// block's own cookies, so an overrun is reported far from the write that caused it; this
// walks every block at every allocation and free and names the block in front of the
// first bad cookie, which is the one that was overrun.
//
//   EOD_HEAP_CHECK=1  verify the whole pool at each MemAlloc / MemFree, report the first failure
//   EOD_HEAP_CHECK=2  also log every allocation and free
#include "core/cpu.h"
#include "core/memory.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

extern "C" {
void f_0012DDB8_orig(void);
void f_0012DB78_orig(void);
}

namespace {
constexpr uint32_t kPool = 0x003CC438;
constexpr uint32_t kStak = 0x53544B41u; // bytes 'A','K','T','S' as the game writes lui 0x5354 / ori 0x4B41
constexpr uint32_t kHeader = 0x40;

struct Site { uint32_t ra, caller; char name[0x21]; uint32_t size; };  // caller: the wrapper's own caller (its frame keeps $ra at +0x30)
std::unordered_map<uint32_t, Site> g_sites; // header address -> allocation site
int g_mode = -1;
bool g_reported = false;
uint64_t g_ops = 0;

int mode() {
    if (g_mode < 0) {
        const char *e = getenv("EOD_HEAP_CHECK");
        g_mode = e && *e ? atoi(e) : 0;
        if (e && *e && !g_mode) g_mode = 1;
    }
    return g_mode;
}

bool in_ram(uint32_t a, uint32_t n) { return a < EE_RAM_SIZE && n <= EE_RAM_SIZE - a; }

void name_of(uint32_t hdr, char out[0x21]) {
    for (int i = 0; i < 0x20; ++i) {
        const uint8_t c = ee_r8(hdr + 4 + i);
        out[i] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : (c ? '?' : 0);
        if (!c) { out[i] = 0; return; }
    }
    out[0x20] = 0;
}

void dump_block(const char *tag, uint32_t hdr) {
    if (!in_ram(hdr, 0x50)) return;
    char nm[0x21]; name_of(hdr, nm);
    fprintf(stderr, "[HEAPCHECK]   %s hdr=%08x name='%s' inuse=%u size=0x%x req=0x%x prev=%08x cookie0=%08x cookie3C=%08x",
            tag, hdr, nm, ee_r32(hdr + 0x24), ee_r32(hdr + 0x28), ee_r32(hdr + 0x2C), ee_r32(hdr + 0x30),
            ee_r32(hdr), ee_r32(hdr + 0x3C));
    auto it = g_sites.find(hdr);
    if (it != g_sites.end()) fprintf(stderr, " allocated by ra=%08x caller=%08x as '%s' (%u bytes)", it->second.ra, it->second.caller, it->second.name, it->second.size);
    fprintf(stderr, "\n");
}

void report(const char *op, uint32_t arg, const char *what, uint32_t bad, uint32_t prev, uint32_t prev2) {
    if (g_reported) return;
    g_reported = true;
    fprintf(stderr, "[HEAPCHECK] CORRUPTION found during %s(%08x) ra=%08x op#%llu: %s at %08x\n", op, arg,
            g_ee.r[31].uw[0], static_cast<unsigned long long>(g_ops), what, bad);
    if (prev2) dump_block("2 before", prev2);
    if (prev) dump_block("before  ", prev);
    dump_block("bad     ", bad);
    if (in_ram(bad, 0x60)) {
        fprintf(stderr, "[HEAPCHECK]   words at bad block:");
        for (uint32_t i = 0; i < 0x60; i += 4) fprintf(stderr, "%s%08x", (i & 0xF) ? " " : "\n[HEAPCHECK]    ", ee_r32(bad + i));
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

// Returns true when the pool is intact.
bool verify(const char *op, uint32_t arg, uint32_t pool) {
    if (!in_ram(pool, 0x20)) return true;
    const uint32_t base = ee_r32(pool + 0xC), top = ee_r32(pool + 4);
    if (!base || !in_ram(base, top)) return true;
    uint32_t prev = 0, prev2 = 0;
    for (uint32_t off = 0; off < top;) {
        const uint32_t hdr = base + off;
        if (!in_ram(hdr, kHeader) || ee_r32(hdr) != kStak) { report(op, arg, "bad header cookie", hdr, prev, prev2); return false; }
        const uint32_t size = ee_r32(hdr + 0x28), req = ee_r32(hdr + 0x2C);
        if (ee_r32(hdr + 0x3C) != kStak) { report(op, arg, "bad header cookie +3C", hdr, prev, prev2); return false; }
        if (size & 0x3F || size < req || off + kHeader + size > top) { report(op, arg, "bad block size", hdr, prev, prev2); return false; }
        if (ee_r32(hdr + 0x24)) {
            const uint32_t foot = hdr + kHeader + ((req + 3) & ~3u);
            if (foot + 4 <= base + top && ee_r32(foot) != kStak) { report(op, arg, "bad footer cookie (overrun of this block)", hdr, prev, prev2); return false; }
        }
        prev2 = prev; prev = hdr;
        off += kHeader + size;
    }
    return true;
}
} // namespace

// MemAlloc(pool, size, name)
extern "C" void f_0012DDB8(void) {
    if (!mode()) { f_0012DDB8_orig(); return; }
    const uint32_t pool = arg_u32(0), size = arg_u32(1), name = arg_u32(2), ra = g_ee.r[31].uw[0];
    ++g_ops;
    verify("MemAlloc", size, pool);
    f_0012DDB8_orig();
    const uint32_t hdr = g_ee.r[2].uw[0] - kHeader;
    Site s{ra, ee_r32(g_ee.r[29].uw[0] + 0x30), {0}, size};
    if (in_ram(name, 0x20)) { for (int i = 0; i < 0x20; ++i) { uint8_t c = ee_r8(name + i); if (!c) break; s.name[i] = static_cast<char>(c); } }
    g_sites[hdr] = s;
    if (mode() >= 2) fprintf(stderr, "[HEAPCHECK] alloc %08x size=%u '%s' ra=%08x\n", hdr + kHeader, size, s.name, ra);
}

// MemFree(ptr)
extern "C" void f_0012DB78(void) {
    if (!mode()) { f_0012DB78_orig(); return; }
    const uint32_t ptr = arg_u32(0);
    ++g_ops;
    verify("MemFree", ptr, kPool);
    if (mode() >= 2) fprintf(stderr, "[HEAPCHECK] free  %08x ra=%08x\n", ptr, g_ee.r[31].uw[0]);
    if (ptr && !(in_ram(ptr - kHeader, kHeader) && ee_r32(ptr - kHeader) == kStak))
        report("MemFree", ptr, "freeing a pointer that is not a block (bad header cookie)", ptr - kHeader, 0, 0);
    f_0012DB78_orig();
    g_sites.erase(ptr - kHeader);
}

// MRAnim create (0x20D220): the native object is a fixed 0x740-byte "mranm" block whose track arrays hold
// 31 bones (inputs at +0x50 stride 0x28, outputs at +0x540 stride 0x10, bone count at +8); the
// initialisation and every update loop bone_count + 1 times without a bound.
extern "C" {
void f_0020D220_orig(void);
}
extern "C" void f_0020D220(void) {
    const uint32_t name = arg_u32(0);
    f_0020D220_orig();
    if (!mode()) return;
    const uint32_t obj = g_ee.r[2].uw[0];
    char text[0x21] = {};
    for (int i = 0; i < 0x20 && in_ram(name + i, 1) && ee_r8(name + i); ++i) text[i] = static_cast<char>(ee_r8(name + i));
    if (in_ram(obj, 0x10))
        fprintf(stderr, "[HEAPCHECK] mranm %08x '%s' bones=%u%s\n", obj, text, ee_r32(obj + 8), ee_r32(obj + 8) > 30 ? "  <-- OVER CAPACITY" : "");
}
