// vif.cpp - VIF0/VIF1 command processors.
//
// Words arriving through DMA or the FIFOs are queued until a complete
// command (code + payload) is available, then executed.  Micro programs
// run synchronously on the VU interpreter.
#include "vif.h"

#include <cstdlib>
#include <cstring>
#include <vector>
#include <cstdio>
#include <string>

#include "../core/log.h"
#include "../core/diff_harness.h"
#include "../core/r7_audit.h"
#include "ee.h"
#include "../gs/gs.h"
#include "../gs/material_state.h"
#include "../kernel/kernel.h"
#include "vu.h"
#include "../core/prof.h"
#include "../render/native_renderer.h"

extern uint32_t g_vif1_top, g_vif1_itop;
extern uint32_t g_vif1_dma_tag;
extern uint8_t *g_ram;
// VU1 execution dispatches per program address (addr / 8), for EOD_RENDER_TRACE.
uint32_t g_vu1_program_starts[0x800];
uint32_t g_vu1_nops_retired = 0;
uint32_t g_vu1_world_leaf_retires = 0;
// Called at each VU1 program start when set (EOD_RENDER_TRACE).
void (*g_vu1_start_hook)(uint32_t addr) = nullptr;
extern uint32_t g_vu_micro_gen[2];  // vu.cpp

namespace {

struct Vif {
    int n;
    VUState *vu;
    std::vector<uint32_t> q;  // pending words
    size_t qpos = 0;
    // registers
    uint32_t stat = 0, err = 0, mark = 0, cycle = 0, mode = 0, num = 0, mask = 0, code = 0;
    uint32_t itops = 0, base = 0, ofst = 0, tops = 0, itop = 0, top = 0;
    uint32_t row[4] = {}, col[4] = {};
    bool dbf = false;
};

Vif g_vif[2];

uint32_t avail(const Vif &v) { return (uint32_t)(v.q.size() - v.qpos); }

bool matches_draw_ref(const Vif &v, const uint32_t *payload, size_t words, uint32_t itop) {
    const uint32_t tag = g_vif1_dma_tag;
    if (!payload || !words || words > 0xFFFFu * 4 ||
        (tag & 15) || tag > EE_RAM_SIZE - 16 ||
        MEM_LW(tag) != (0x30000000u | (words / 4)) ||
        MEM_LW(tag + 8) != (0x04000000u | itop) || MEM_LW(tag + 12) != 0x05000000)
        return false;
    // The native feed independently binds this source to the committed part + 0x38.
    const uint32_t source = MEM_LW(tag + 4);
    return !(source & 15) && source <= EE_RAM_SIZE - words * 4 &&
           payload == reinterpret_cast<const uint32_t *>(g_ram + source) &&
           v.base == 0xE0 && v.ofst == 0x190 && v.mode == 0 && v.cycle == 0x0101 &&
           v.itop == itop && v.top == (v.dbf ? 0xE0u : 0x270u) &&
           v.tops == (v.dbf ? 0x270u : 0xE0u);
}

unsigned clipped_world_chunk_vertices(const Vif &v, const uint32_t *payload, size_t offset) {
    // The same REF can carry many chunks. Check the one ending at this MSCNT,
    // including the NUM field and relative location of each vertex UNPACK.
    uint32_t source_tag;
    std::memcpy(&source_tag, v.vu->mem + 16 * v.top, sizeof source_tag);
    const unsigned count = source_tag & 0x7FFF;
    const unsigned color_words = (count + 1) / 2;
    const size_t chunk_words = 34 + 4 * count + color_words;
    if ((source_tag & ~0x7FFFu) != 0x8000 || count < 4 || count > 60 ||
        offset <= chunk_words || payload[offset] != 0x17000000)
        return 0;
    const uint32_t *chunk = payload + offset - chunk_words;
    const uint32_t num = count << 16;
    const uint32_t *color = chunk + 12 + 3 * count;
    const uint32_t *uv = color + 2 + color_words;
    const uint32_t *bases = uv + 9 + count;
    return chunk[0] == 0x6001C000 && chunk[1] == source_tag &&
           chunk[2] == 0x01000103 && chunk[3] == 0x30000000 &&
           chunk[4] == 0 && chunk[5] == 0 && chunk[6] == 0 && chunk[7] == 0x3F800000 &&
           chunk[8] == 0x05000001 && chunk[9] == 0x20000000 && chunk[10] == 0x40404040 &&
           chunk[11] == (0x7800C002 | num) &&
           color[0] == 0x05000000 && color[1] == (0x6F00C001 | num) &&
           uv[0] == 0x05000001 && uv[1] == 0x20000000 && uv[2] == 0x50505050 &&
           uv[3] == 0x30000000 && uv[6] == 0 && uv[7] == 0x8000 &&
           uv[8] == (0x7500C003 | num) &&
           bases[0] == 0x05000000 && bases[1] == 0x01000101 && bases[2] == 0x6C02C0C6
        ? count : 0;
}

void start_micro(Vif &v, uint32_t addr, VuLaunch launch,
                 const uint32_t *ref_payload = nullptr, size_t ref_words = 0, size_t offset = 0) {
    if (v.n == 1) {
        v.itop = v.itops;
        v.top = v.tops;
        v.dbf = !v.dbf;
        v.tops = v.dbf ? v.base + v.ofst : v.base;
        g_vif1_top = v.top;
        g_vif1_itop = v.itop;
        native::WorldTextureLod world_lod{};
        bool clipped_world_continuation = false;
        bool bypass_vu1 = false;
#ifdef EOD_PLUME
        // Explicit state dispatches still own their GIF/register side effects
        // when the same DMA range also supplies a native draw.
        const bool state_launch = launch != VuLaunch::Mscnt && !g_vu1_start_hook && native::vu1_bypass_enabled() &&
            (addr == 0x2538 || addr == 0x25F0 || addr == 0x0000 || addr == 0x24D8);
        if (state_launch && launch == VuLaunch::Mscal) {
            if (addr == 0x2538) {
                gs::MaterialState material;
                const bool known = gs::material_state_enabled() && v.top <= 1024 - 16 &&
                    gs::match_material_state(g_vif1_dma_tag, ref_payload, ref_words, offset,
                                             v.vu->mem + 16 * v.top, material);
                if (vu_retire_xgkick_top(v.vu, addr, v.top, known ? &material : nullptr)) {
                    r7_audit::add(r7_audit::Counter::Vu1Bypassed);
                    return;
                }
            }
            if (addr == 0x25F0 && vu_retire_giftag_setup(v.vu, addr, v.top)) {
                r7_audit::add(r7_audit::Counter::Vu1Bypassed);
                return;
            }
            if (addr == 0x0000 && vu_retire_setup_0000(v.vu, addr)) {
                r7_audit::add(r7_audit::Counter::Vu1Bypassed);
                return;
            }
        }
        // The verified E-bit NOP pair is shared by ordinary MSCAL producers.
        static const char *nop_setting = std::getenv("EOD_NATIVE_CAMERA_NOP");
        if (launch == VuLaunch::Mscal && addr == 0x24D8 && (!nop_setting || nop_setting[0] != '0') &&
            native::enabled() && !diff_in_original_side() && vu_retire_mscal_nop(v.vu, addr)) {
            ++g_vu1_nops_retired;
            return;
        }
#else
        constexpr bool state_launch = false;
#endif
        const bool retire_world_leaf = !state_launch && native::vu1_start(
            addr, v.itop, launch == VuLaunch::Mscnt, world_lod, clipped_world_continuation,
            g_vu1_start_hook ? nullptr : &bypass_vu1);
        if (bypass_vu1) {
            r7_audit::add(r7_audit::Counter::Vu1Bypassed);
            return;
        }
#ifndef EOD_R7_RELEASE
        if (retire_world_leaf && matches_draw_ref(v, ref_payload, ref_words, 0x80) &&
            vu_retire_world_leaf(v.vu, addr, v.top, world_lod)) {
            ++g_vu1_world_leaf_retires;
            return;
        }
        if (launch == VuLaunch::Mscnt && clipped_world_continuation &&
            matches_draw_ref(v, ref_payload, ref_words, 0x280)) {
            const unsigned count = clipped_world_chunk_vertices(v, ref_payload, offset);
            if (count && vu_retire_clipped_world_strip(v.vu, v.top, count))
                return;
        }
        if (((launch == VuLaunch::Mscal && addr == 0x02E0) ||
             (launch == VuLaunch::Mscnt && addr == 0x0D48)) &&
            native::queue_dent_check_enabled() && matches_draw_ref(v, ref_payload, ref_words, 0x80))
            native::check_queue_dent_input(v.top, offset, launch == VuLaunch::Mscnt);
        if (launch == VuLaunch::Mscnt && addr == 0x0D48 &&
            matches_draw_ref(v, ref_payload, ref_words, 0x80)) {
            native::QueueDentChunk candidate{};
            if (native::queue_dent_candidate(offset, candidate) &&
                vu_retire_queue_dent(v.vu, v.top, candidate)) {
                native::queue_dent_selected(v.top, candidate.omission);
                if (!candidate.omission.reference) return;
            }
        }
#else
        (void)retire_world_leaf;
        (void)ref_payload;
        (void)ref_words;
        (void)offset;
#endif
        g_vu1_program_starts[(addr >> 3) & 0x7FF]++;
        if (g_vu1_start_hook) g_vu1_start_hook(addr);
    } else {
        v.itop = v.itops;
    }
#ifdef EOD_R7_RELEASE
    vu_reject_launch(*v.vu, addr, launch);
#else
    vu_execute(v.vu, addr, launch);
#endif
}

// Number of data words an UNPACK consumes.
uint32_t unpack_words(const Vif &v, uint32_t c) {
    uint32_t vn = (c >> 26) & 3, vl = (c >> 24) & 3;
    uint32_t num = (c >> 16) & 0xFF;
    if (!num) num = 256;
    uint32_t cl = v.cycle & 0xFF, wl = (v.cycle >> 8) & 0xFF;
    if (!wl) wl = 256;  // hardware treats WL=0 as 256
    if (!cl) cl = 256;
    uint32_t inputs;
    if (wl <= cl)
        inputs = num;
    else
        inputs = (num / wl) * cl + ((num % wl) < cl ? (num % wl) : cl);
    uint32_t bits_per = (vl == 3) ? 16 : (32 >> vl) * (vn + 1);
    uint32_t bitsz = inputs * bits_per;
    return (bitsz + 31) / 32;
}

// Fast path for the common unpacks: no mask, WL <= CL, not V4-5.
template <uint32_t VN, uint32_t VL, bool USN>
void unpack_fast(Vif &v, const uint8_t *src, uint32_t addr, uint32_t num, uint32_t cl, uint32_t wl) {
    constexpr uint32_t eb = 4 >> VL, nc = VN + 1;
    uint8_t *mem = v.vu->mem;
    uint32_t qmask = (v.vu->mem_mask + 1) / 16 - 1;
    uint32_t dst = addr, cyc = 0;
    const uint32_t mode = v.mode;
    for (uint32_t i = 0; i < num; i++, src += eb * nc) {
        uint32_t comp[4];
        for (uint32_t k = 0; k < nc; k++) {
            if (VL == 0) {
                memcpy(&comp[k], src + k * 4, 4);
            } else if (VL == 1) {
                uint16_t h;
                memcpy(&h, src + k * 2, 2);
                comp[k] = USN ? h : (uint32_t)(int32_t)(int16_t)h;
            } else {
                comp[k] = USN ? src[k] : (uint32_t)(int32_t)(int8_t)src[k];
            }
        }
        uint32_t val[4];
        switch (VN) {
        case 0: val[0] = val[1] = val[2] = val[3] = comp[0]; break;
        case 1: val[0] = comp[0]; val[1] = comp[1]; val[2] = comp[0]; val[3] = comp[1]; break;
        case 2: val[0] = comp[0]; val[1] = comp[1]; val[2] = comp[2]; val[3] = 0; break;
        default: val[0] = comp[0]; val[1] = comp[1]; val[2] = comp[2]; val[3] = comp[3]; break;
        }
        if (mode == 1) {
            for (int k = 0; k < 4; k++) val[k] += v.row[k];
        } else if (mode == 2) {
            for (int k = 0; k < 4; k++) val[k] = v.row[k] += val[k];
        }
        memcpy(mem + (dst & qmask) * 16, val, 16);
        if (++cyc == wl) {
            cyc = 0;
            dst += cl - wl + 1;
        } else {
            dst++;
        }
    }
}

typedef void (*UnpackFn)(Vif &, const uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);
#define UF(vn, vl) &unpack_fast<vn, vl, false>, &unpack_fast<vn, vl, true>
const UnpackFn g_unpack_fast[4][3][2] = {
    {{UF(0, 0)}, {UF(0, 1)}, {UF(0, 2)}},
    {{UF(1, 0)}, {UF(1, 1)}, {UF(1, 2)}},
    {{UF(2, 0)}, {UF(2, 1)}, {UF(2, 2)}},
    {{UF(3, 0)}, {UF(3, 1)}, {UF(3, 2)}},
};
#undef UF

// Any unpack (mask, filling write, V4-5): the format is a template
// parameter, the mask codes are decoded once per command and positions are
// counted instead of divided.  (VL == 3: V4-5, one 16-bit RGBA5551 element.)
template <uint32_t VN, uint32_t VL, bool USN>
void unpack_gen(Vif &v, const uint8_t *src, uint32_t addr, uint32_t num, uint32_t cl, uint32_t wl, bool masked) {
    constexpr uint32_t eb = VL == 3 ? 2 : (4 >> VL), nc = VL == 3 ? 1 : VN + 1;
    uint8_t *mem = v.vu->mem;
    const uint32_t qmask = (v.vu->mem_mask + 1) / 16 - 1;
    // write mode per row (0..3) and component: 0 data, 1 row, 2 col, 3 protected
    uint8_t mcode[4][4];
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) mcode[r][k] = masked ? (uint8_t)((v.mask >> (r * 8 + k * 2)) & 3) : 0;
    const bool fill = wl > cl;
    const uint32_t mode = v.mode;
    uint32_t cyc = 0, dst = addr;
    for (uint32_t i = 0; i < num; i++) {
        const bool has_input = !fill || cyc < cl;
        uint32_t val[4] = {0, 0, 0, 0};
        if (has_input) {
            if (VL == 3) {
                uint16_t p;
                memcpy(&p, src, 2);
                val[0] = (p & 0x1F) << 3;
                val[1] = ((p >> 5) & 0x1F) << 3;
                val[2] = ((p >> 10) & 0x1F) << 3;
                val[3] = (uint32_t)(p >> 15) << 7;
            } else {
                uint32_t comp[4] = {0, 0, 0, 0};
                for (uint32_t k = 0; k < nc; k++) {
                    if (VL == 0) {
                        memcpy(&comp[k], src + k * 4, 4);
                    } else if (VL == 1) {
                        uint16_t h;
                        memcpy(&h, src + k * 2, 2);
                        comp[k] = USN ? h : (uint32_t)(int32_t)(int16_t)h;
                    } else {
                        comp[k] = USN ? src[k] : (uint32_t)(int32_t)(int8_t)src[k];
                    }
                }
                switch (VN) {
                case 0: val[0] = val[1] = val[2] = val[3] = comp[0]; break;
                case 1: val[0] = comp[0]; val[1] = comp[1]; val[2] = comp[0]; val[3] = comp[1]; break;
                case 2: val[0] = comp[0]; val[1] = comp[1]; val[2] = comp[2]; val[3] = 0; break;
                default: memcpy(val, comp, 16); break;
                }
            }
            src += eb * nc;
        }
        uint8_t *q = mem + (dst & qmask) * 16;
        const uint32_t row_sel = cyc < 3 ? cyc : 3;
        for (int k = 0; k < 4; k++) {
            uint32_t m = mcode[row_sel][k];
            if (!has_input && m == 0) m = 1;  // filling write without input data: row register
            uint32_t out;
            if (m == 0) {
                out = val[k];
                if (mode == 1) out += v.row[k];
                else if (mode == 2) out = v.row[k] += val[k];
            } else if (m == 1) {
                out = v.row[k];
            } else if (m == 2) {
                out = v.col[row_sel];
            } else {
                continue;  // write protected
            }
            memcpy(q + k * 4, &out, 4);
        }
        // next position: WL <= CL skips CL - WL quadwords after each WL; filling writes are contiguous
        if (++cyc == wl) {
            cyc = 0;
            dst += fill ? 1 : cl - wl + 1;
        } else {
            dst++;
        }
    }
}

typedef void (*UnpackGenFn)(Vif &, const uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, bool);
#define UG(vn, vl) &unpack_gen<vn, vl, false>, &unpack_gen<vn, vl, true>
const UnpackGenFn g_unpack_gen[4][4][2] = {
    {{UG(0, 0)}, {UG(0, 1)}, {UG(0, 2)}, {UG(0, 3)}},
    {{UG(1, 0)}, {UG(1, 1)}, {UG(1, 2)}, {UG(1, 3)}},
    {{UG(2, 0)}, {UG(2, 1)}, {UG(2, 2)}, {UG(2, 3)}},
    {{UG(3, 0)}, {UG(3, 1)}, {UG(3, 2)}, {UG(3, 3)}},
};
#undef UG

void do_unpack(Vif &v, uint32_t c, const uint32_t *data) {
    {   // EOD_BONE_SRC=1 (diagnostic): VU1 unpacks into the bone area (qwords 0x90..0xff), and
        // where their raw bytes are in EE RAM
        static const bool on = getenv("EOD_BONE_SRC") != nullptr;
        if (on && v.n == 1 && kernel_vblank_count() % 120 == 0) {
            static uint64_t lvb = ~0ull;
            static int cnt = 0;
            if (lvb != kernel_vblank_count()) lvb = kernel_vblank_count(), cnt = 0;
            uint32_t a0 = c & 0x3FF;
            if (((c >> 15) & 1)) a0 += v.tops;
            uint32_t num0 = (c >> 16) & 0xFF;
            if (!num0) num0 = 256;
            if (a0 < 0xe0 && a0 + num0 > 0x90 && a0 != 0xac && cnt++ < 40) {
                const uint8_t *raw = reinterpret_cast<const uint8_t *>(data);
                std::string hits;
                for (size_t a = 0; a + 16 <= (32u << 20) && hits.size() < 80; a += 4)
                    if (memcmp(g_ram + a, raw, 16) == 0) { char t[16]; snprintf(t, sizeof t, " %08zx", a); hits += t; }
                fprintf(stderr, "[bonevif] vb %llu UNPACK %08x vn%u vl%u addr %03x num %u cycle %04x mode %u raw %08x %08x %08x %08x ram:%s\n",
                        (unsigned long long)kernel_vblank_count(), c, (c >> 26) & 3, (c >> 24) & 3, a0, num0, v.cycle, v.mode,
                        data[0], data[1], data[2], data[3], hits.c_str());
            }
        }
    }
    uint32_t vn = (c >> 26) & 3, vl = (c >> 24) & 3;
    bool masked = (c >> 28) & 1;
    uint32_t num = (c >> 16) & 0xFF;
    if (!num) num = 256;
    uint32_t imm = c & 0xFFFF;
    uint32_t addr = imm & 0x3FF;
    bool usn = (imm >> 14) & 1;
    bool flg = (imm >> 15) & 1;
    if (v.n == 1 && flg) addr += v.tops;
    uint32_t cl = v.cycle & 0xFF, wl = (v.cycle >> 8) & 0xFF;
    if (!wl) wl = 256;
    if (!cl) cl = 256;
    const uint8_t *src = reinterpret_cast<const uint8_t *>(data);
    static const bool check = getenv("EOD_VIF_CHECK") != nullptr;  // compare with the generic code
    static uint8_t fast_mem[0x4000];
    static uint32_t fast_row[4];
    const bool fast = !masked && wl <= cl && vl != 3;
    if (!check) {
        if (fast) g_unpack_fast[vn][vl][usn](v, src, addr, num, cl, wl);
        else g_unpack_gen[vn][vl][usn](v, src, addr, num, cl, wl, masked);
        v.num = 0;
        return;
    }
    {
        // EOD_VIF_CHECK: run the specialised code, then the reference loop below, and compare
        static uint8_t saved[0x4000];
        uint32_t saved_row[4];
        size_t msize = v.vu->mem_mask + 1;
        memcpy(saved, v.vu->mem, msize);
        memcpy(saved_row, v.row, 16);
        if (fast) g_unpack_fast[vn][vl][usn](v, src, addr, num, cl, wl);
        else g_unpack_gen[vn][vl][usn](v, src, addr, num, cl, wl, masked);
        memcpy(fast_mem, v.vu->mem, msize);
        memcpy(fast_row, v.row, 16);
        memcpy(v.vu->mem, saved, msize);
        memcpy(v.row, saved_row, 16);
    }
    uint32_t mem_qw = (v.vu->mem_mask + 1) / 16;
    uint32_t elem_bytes = vl == 3 ? 2 : (4 >> vl);
    uint32_t ncomp = vl == 3 ? 1 : vn + 1;
    uint32_t in_index = 0;  // input element counter (in elements)
    for (uint32_t i = 0; i < num; i++) {
        uint32_t cyc = i % wl;          // position within the write cycle
        uint32_t dst = addr + (i / wl) * (wl <= cl ? cl : wl) + cyc;
        if (wl > cl) dst = addr + i;    // filling write: contiguous
        bool has_input = (wl <= cl) || cyc < cl;
        uint32_t val[4] = {0, 0, 0, 0};
        if (has_input) {
            if (vl == 3) {  // V4-5 (RGBA5551)
                uint16_t p;
                memcpy(&p, src + in_index * 2, 2);
                val[0] = (p & 0x1F) << 3;
                val[1] = ((p >> 5) & 0x1F) << 3;
                val[2] = ((p >> 10) & 0x1F) << 3;
                val[3] = (p >> 15) << 7;
                in_index++;
            } else {
                uint32_t comp[4];
                for (uint32_t k = 0; k < ncomp; k++) {
                    const uint8_t *p = src + (in_index + k) * elem_bytes;
                    uint32_t e;
                    if (elem_bytes == 4) {
                        memcpy(&e, p, 4);
                    } else if (elem_bytes == 2) {
                        uint16_t h;
                        memcpy(&h, p, 2);
                        e = usn ? h : (uint32_t)(int32_t)(int16_t)h;
                    } else {
                        e = usn ? *p : (uint32_t)(int32_t)(int8_t)*p;
                    }
                    comp[k] = e;
                }
                in_index += ncomp;
                switch (vn) {
                case 0: val[0] = val[1] = val[2] = val[3] = comp[0]; break;
                case 1: val[0] = comp[0]; val[1] = comp[1]; val[2] = comp[0]; val[3] = comp[1]; break;
                case 2: val[0] = comp[0]; val[1] = comp[1]; val[2] = comp[2]; val[3] = 0; break;
                default: memcpy(val, comp, 16); break;
                }
            }
        }
        uint8_t *q = v.vu->mem + (dst % mem_qw) * 16;
        uint32_t row_sel = cyc < 3 ? cyc : 3;
        for (int k = 0; k < 4; k++) {
            uint32_t m = 0;
            if (masked) m = (v.mask >> (row_sel * 8 + k * 2)) & 3;
            if (!has_input && m == 0) m = 1;  // filling write without input data: row register
            uint32_t out;
            switch (m) {
            case 0:
                out = val[k];
                if (v.mode == 1) out += v.row[k];
                else if (v.mode == 2) {
                    v.row[k] += val[k];
                    out = v.row[k];
                }
                break;
            case 1: out = v.row[k]; break;
            case 2: out = v.col[row_sel]; break;
            default: continue;  // write protected
            }
            memcpy(q + k * 4, &out, 4);
        }
    }
    v.num = 0;
    if (memcmp(fast_mem, v.vu->mem, v.vu->mem_mask + 1) || memcmp(fast_row, v.row, 16))
        WARN("VIF%d unpack %08x (cycle %x mode %d mask %08x): %s path differs", v.n, c, v.cycle, v.mode, v.mask,
             fast ? "fast" : "generic");
}

// Executes the complete commands at the start of w[0..n); returns the
// number of words consumed.
size_t run(Vif &v, const uint32_t *w, size_t n) {
#ifdef EOD_PLUME
    const bool vu1_bypass = v.n == 1 && !g_vu1_start_hook && native::vu1_bypass_enabled();
    const bool skip_dead_unpack = vu1_bypass && native::vu1_bypass_dma_tag(g_vif1_dma_tag);
#else
    const bool vu1_bypass = false;
    const bool skip_dead_unpack = false;
#endif
    size_t pos = 0;
    for (;;) {
        size_t left = n - pos;
        if (left == 0) break;
        uint32_t c = w[pos];
        uint32_t cmd = (c >> 24) & 0x7F;
        uint32_t imm = c & 0xFFFF;
        uint32_t numf = (c >> 16) & 0xFF;
        uint32_t need = 1;
        if (cmd == 0x20) need = 2;
        else if (cmd == 0x30 || cmd == 0x31) need = 5;
        else if (cmd == 0x4A) need = 1 + (numf ? numf : 256) * 2;
        else if (cmd == 0x50 || cmd == 0x51) need = 1 + (imm ? imm : 65536) * 4;
        else if ((cmd & 0x60) == 0x60) need = 1 + unpack_words(v, c);
        if (left < need) break;
        const uint32_t *d = &w[pos + 1];
        v.code = c;
        switch (cmd) {
        case 0x00: break;  // NOP
        case 0x01: v.cycle = imm; break;  // STCYCL
        case 0x02:  // OFFSET
            v.ofst = imm & 0x3FF;
            v.dbf = false;
            v.tops = v.base;
            break;
        case 0x03: v.base = imm & 0x3FF; break;  // BASE
        case 0x04: v.itops = imm & 0x3FF; break;  // ITOP
        case 0x05: v.mode = imm & 3; break;       // STMOD
        case 0x06:  // MSKPATH3: a mask takes effect at the next packet boundary
            LOG(LOG_VIF, "VIF1 MSKPATH3 %s", (imm & 0x8000) ? "mask" : "unmask");
            if (imm & 0x8000) {
                gif_set_path3_mask(true);
            } else {
                gif_set_path3_mask(false);
                gif_path3_run(1);
            }
            break;
        case 0x07: v.mark = imm; break;           // MARK
        case 0x10: case 0x11: case 0x13:  // FLUSHE/FLUSH/FLUSHA
            LOG(LOG_VIF, "VIF%d FLUSH%s", v.n, cmd == 0x13 ? "A" : cmd == 0x11 ? "" : "E");
            if (cmd == 0x13) gif_path3_run(-1);  // waits for PATH3 (if unmasked)
            break;
        case 0x14:  // MSCAL
            LOG(LOG_VIF, "VIF%d MSCAL %04x", v.n, imm * 8);
            // Admission checks the committed REF; later commands run from the same VIF stream.
            start_micro(v, imm * 8, VuLaunch::Mscal, v.n == 1 && !(n & 3) ? w : nullptr, n, pos);
            break;
        case 0x15: start_micro(v, imm * 8, VuLaunch::Mscalf); break;
        case 0x17:
            start_micro(v, v.vu->tpc, VuLaunch::Mscnt, v.n == 1 && !(n & 3) ? w : nullptr, n, pos);
            break;
        case 0x20: v.mask = d[0]; break;                 // STMASK
        case 0x30: memcpy(v.row, d, 16); break;          // STROW
        case 0x31: memcpy(v.col, d, 16); break;          // STCOL
        case 0x4A: {                                      // MPG
            uint32_t n = numf ? numf : 256;
            uint32_t a = imm * 8;
            // The game re-sends the same micro programs often; only a real change of the code
            // invalidates the compiled-program lookup (which then re-matches every program).
            bool changed = false;
            for (uint32_t k = 0; k < n * 2; k++) {
                uint32_t w = d[k];
                uint8_t *dst = v.vu->micro + ((a + k * 4) & v.vu->micro_mask);
                if (!changed && memcmp(dst, &w, 4) != 0) changed = true;
                memcpy(dst, &w, 4);
            }
            if (changed) g_vu_micro_gen[v.n]++;
            break;
        }
        case 0x50:
        case 0x51: {  // DIRECT / DIRECTHL
            const uint32_t qwc = imm ? imm : 65536;
            bool additive;
            if (v.n == 1 && qwc == 3 && gs::material_state_enabled() &&
                gs::match_shadow_blend(g_vif1_dma_tag, d, additive)) {
                gs::submit_shadow_blend(additive);
                break;
            }
            native::SceneClear clear;
            native::DisplayCopy copy;
            const bool has_clear = v.n == 1 && native::take_scene_clear(g_vif1_dma_tag, qwc, clear);
            const bool has_copy = v.n == 1 && native::take_display_copy(g_vif1_dma_tag, qwc, copy);
            const auto shadow = v.n == 1 ? native::take_shadow_pass(g_vif1_dma_tag, qwc) : native::ShadowPass::None;
            native::BootstrapQuad quad;
            const bool boot_quad = v.n == 1 && !diff_in_original_side() &&
                                   native::take_bootstrap_quad(g_vif1_dma_tag, qwc, quad);
            gif_transfer(GIF_PATH2, reinterpret_cast<const uint8_t *>(d), qwc,
                         has_clear ? &clear : nullptr, has_copy ? &copy : nullptr, shadow,
                         boot_quad ? &quad : nullptr);
            break;
        }
        default:
            if ((cmd & 0x60) == 0x60) {
                bool skip = false;
                if (v.mode != 2) {
                    if (skip_dead_unpack) {
                        skip = true;
                    } else if (vu1_bypass) {
                        const bool flg = (c >> 15) & 1;
                        if (!flg && v.base == 0xE0 && v.cycle == 0x0101) {
                            // Static VU1 data RAM below TOP (camera/bones/dents/sun-quad) is unread when bypassed;
                            // 0x2538 A+D state packets always unpack via FLG=1 to TOP (0xE0 or 0x270).
                            const uint32_t dst_qw = c & 0x3FF;
                            const uint32_t cnt = numf ? numf : 256;
                            if (dst_qw + cnt <= 0xE0) skip = true;
                        } else if (flg && pos + need < n) {
                            // Skip FLG=1 UNPACKs feeding retired setup 0x0000 (0x35CD28 matrix/lighting) or
                            // retired NOP 0x24D8; NEVER skip 0x2538 (MSCAL 0x4A7) A+D state packets.
                            for (size_t look = pos + need; look < n && look < pos + need + 4; ++look) {
                                const uint32_t nw = w[look];
                                const uint32_t ncmd = (nw >> 24) & 0x7F;
                                if (ncmd == 0x00 || ncmd == 0x01 || ncmd == 0x04 || ncmd == 0x05) continue;
                                if (nw == 0x14000000u || nw == 0x1400049Bu) skip = true;
                                break;
                            }
                        }
                    }
                }
                if (skip) v.num = 0;
                else do_unpack(v, c, d);
            } else {
                WARN("VIF%d: unknown command %08x", v.n, c);
            }
        }
        if (c & 0x80000000u) {
            // IRQ bit: signal the EE (the VIF would stall until cleared)
            intc_raise(v.n ? INTC_VIF1 : INTC_VIF0);
        }
        pos += need;
    }
    return pos;
}

// Executes as many complete commands as are queued.
void process(Vif &v) {
    v.qpos += run(v, v.q.data() + v.qpos, v.q.size() - v.qpos);
    // compact the queue
    if (v.qpos == v.q.size()) {
        v.q.clear();
        v.qpos = 0;
    } else if (v.qpos > 65536) {
        v.q.erase(v.q.begin(), v.q.begin() + (long)v.qpos);
        v.qpos = 0;
    }
}

}  // namespace

void vif_init() {
    g_vif[0] = Vif();
    g_vif[0].n = 0;
    g_vif[0].vu = &g_vu0;
    g_vif[1] = Vif();
    g_vif[1].n = 1;
    g_vif[1].vu = &g_vu1;
}

void vif_write(int n, const uint32_t *words, uint32_t count) {
    ProfScope prof(P_EE_VIF);
    Vif &v = g_vif[n & 1];
    if (avail(v) == 0) {
        // nothing pending: run straight from the caller's buffer, keep a partial command
        size_t used = run(v, words, count);
        v.q.assign(words + used, words + count);
        v.qpos = 0;
        return;
    }
    v.q.insert(v.q.end(), words, words + count);
    process(v);
}

uint32_t vif_reg_read(int n, uint32_t off) {
    Vif &v = g_vif[n & 1];
    switch (off) {
    case 0x00: return v.dbf ? 0x80 : 0;  // STAT: idle, DBF
    case 0x20: return v.err;
    case 0x30: return v.mark;
    case 0x40: return v.cycle;
    case 0x50: return v.mode;
    case 0x60: return v.num;
    case 0x70: return v.mask;
    case 0x80: return v.code;
    case 0x90: return v.itops;
    case 0xA0: return v.base;
    case 0xB0: return v.ofst;
    case 0xC0: return v.tops;
    case 0xD0: return v.itop;
    case 0xE0: return v.top;
    case 0x100: case 0x110: case 0x120: case 0x130: return v.row[(off - 0x100) >> 4];
    case 0x140: case 0x150: case 0x160: case 0x170: return v.col[(off - 0x140) >> 4];
    }
    return 0;
}

void vif_reg_write(int n, uint32_t off, uint32_t val) {
    Vif &v = g_vif[n & 1];
    switch (off) {
    case 0x10:  // FBRST
        if (val & 1) {  // reset
            v.q.clear();
            v.qpos = 0;
            v.mask = v.mode = v.cycle = 0;
        }
        break;
    case 0x20: v.err = val; break;
    case 0x30: v.mark = val; break;
    case 0x100: case 0x110: case 0x120: case 0x130: v.row[(off - 0x100) >> 4] = val; break;
    case 0x140: case 0x150: case 0x160: case 0x170: v.col[(off - 0x140) >> 4] = val; break;
    }
}

void vif1_read_fifo(uint32_t out[4]) { out[0] = out[1] = out[2] = out[3] = 0; }
