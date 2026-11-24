#include "core/cpu.h"
#include "core/diff_harness.h"
#include "gs/material_state.h"
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

extern "C" void f_0035F9A0_orig();
extern "C" void f_00361F80_orig();

namespace gs {
namespace {
// The EE builds recipes and the VIF worker can consume them asynchronously.
std::unordered_map<uint32_t, MaterialState> recipes;
std::unordered_map<uint32_t, bool> shadow_blends;
std::mutex recipe_mutex;

bool pixel_format(uint64_t type, uint32_t &psm) {
    switch (type) {
    case 0: case 1: psm = 0; return true;
    case 2: psm = 2; return true;
    case 4: psm = 0x13; return true;
    case 8: psm = 0x14; return true;
    case 0xffff: psm = 0xff; return true;
    default: return false;
    }
}

MaterialState texture_bind() {
    MaterialState s{};
    const uint32_t levels = R[5].uw[0];
    const unsigned tw = R[7].uw[0] & 0xffff, th = R[8].uw[0] & 0xffff;
    uint32_t psm, cpsm;
    if (!levels || (levels & 3) || levels > EE_RAM_SIZE - 28 || tw > 15 || th > 15 ||
        !pixel_format(R[9].ud[0], psm) || !pixel_format(R[10].ud[0], cpsm) ||
        R[29].uw[0] > EE_RAM_SIZE - 4) return s;
    const auto level = [&](int n) -> uint64_t { return MEM_LW(levels + 4 * n) & 0x3fff; };
    const auto width = [&](int n) -> uint64_t {
        const unsigned unit = 64u << n;
        return (((1u << tw) + unit - 1) / unit) & 63;
    };
    const auto mips = [&](int n) {
        return level(n) | width(n) << 14 | level(n+1) << 20 | width(n+1) << 34 |
               level(n+2) << 40 | width(n+2) << 54;
    };
    s.fields = s.Texture;
    s.miptbp1 = mips(1);
    s.miptbp2 = mips(4);
    s.tex0 = level(0) | width(0) << 14 | uint64_t(psm & 63) << 20 |
             uint64_t(tw) << 26 | uint64_t(th) << 30 | 1ull << 34 |
             uint64_t(MEM_LW(R[29].uw[0]) ? 3 : 0) << 35 |
             (R[6].ud[0] & 0x3fff) << 37 | uint64_t(cpsm & 15) << 51 | 1ull << 61;
    return s;
}

MaterialState render_state() {
    MaterialState s{};
    const uint32_t flags = MEM_LW(0x4d8bc8);
    uint32_t dirty = (((flags ^ MEM_LW(0x4d8bcc)) & 0x3fff) | MEM_LW(0x4d8b74)) & 0xffff7ff3;
    if (dirty & 0x30) {
        s.fields |= s.Clamp;
        s.clamp = !(flags & 0x10) | (unsigned(!(flags & 0x20)) << 2);
    }
    // RenderStateCommit clears 0x108 while scheduling PRIM. Bit zero
    // selects alpha testing and must also reach the TEST write.
    if (dirty & 0x241) {
        s.fields |= s.Test;
        const uint64_t ztst = flags & 0x40 ? 2 : 1;
        s.test = (ztst << 17) | (!(flags & 0x200) ? 0x12001 : flags & 1 ? 0x1160b : 0x10000);
    }
    if (dirty & 0x80) {
        s.fields |= s.Zbuf;
        s.zbuf = (MEM_LW(0x4d8bd0) & 511) | uint64_t(MEM_LW(0x4d8bd4) & 15) << 24 |
                 uint64_t(!(flags & 0x80)) << 32;
    }
    if (dirty & 0x20000) {
        s.fields |= s.Frame;
        s.frame = (MEM_LW(0x4d8b90) & 511) | uint64_t((MEM_LW(0x4d8b98) >> 6) & 63) << 16 |
                  uint64_t(MEM_LW(0x4d8b94) & 63) << 24;
    }
    if (dirty & 0x80000) { s.fields |= s.Alpha; s.alpha = MEM_LW(0x4d8b78) ? 0x44 : 0x48; }
    if (dirty & 0x10000) { s.fields |= s.Fog; s.fog = MEM_LW(0x4d8b80); }
    if (dirty & 0x40000) {
        s.fields |= s.Scissor;
        for (unsigned i = 0; i < 4; ++i)
            s.scissor |= uint64_t(MEM_LW(0x4d8b9c + i*4) & 0x7ff) << (i*16);
    }
    if (dirty & 0x4000) s.fields |= s.Dither;
    if (dirty & 0x100000) s.fields |= s.Init;
    return s;
}
}

bool material_state_enabled() {
    static const bool enabled = [] {
        const char *value = std::getenv("EOD_NATIVE_MATERIAL_STATE");
#ifdef EOD_R7_RELEASE
        const bool requested = !value || *value != '0';
#else
        const bool requested = value && *value == '1';
#endif
        return requested && !std::getenv("EOD_GS_DUMP") && !std::getenv("EOD_GS_DUMP_COLD_START") &&
               !std::getenv("EOD_SCENE_CAPTURE") && !std::getenv("EOD_SCENE_EMIT") &&
               !std::getenv("EOD_MODERN") && !std::getenv("EOD_GS_TRACE") && !std::getenv("EOD_GS_CENSUS");
    }();
    return enabled;
}

void record_shadow_blend(uint32_t tag, bool additive) {
    if (tag && !(tag & 15) && tag <= EE_RAM_SIZE - 64) {
        std::lock_guard<std::mutex> lock(recipe_mutex);
        shadow_blends[tag] = additive;
    }
}

bool match_shadow_blend(uint32_t tag, const void *data, bool &additive) {
    if ((tag & 15) || tag > EE_RAM_SIZE - 64) return false;
    std::lock_guard<std::mutex> lock(recipe_mutex);
    const auto it = shadow_blends.find(tag);
    if (it == shadow_blends.end()) return false;
    const uint32_t dma[] = {0x10000003, 0, 0x11000000, 0x50000003};
    const auto packet = shadow_blend_packet(it->second);
    if (memcmp(g_ram + tag, dma, sizeof dma) || memcmp(data, packet.data(), sizeof packet) ||
        memcmp(g_ram + tag + 16, packet.data(), sizeof packet)) return false;
    additive = it->second;
    return true;
}

bool match_material_state(uint32_t tag, const uint32_t *words, size_t size, size_t mscal,
                          const uint8_t *top, MaterialState &out) {
    if (!words || !top || !size || size > 18 * 4 || mscal != size - 1 ||
        (tag & 15) || tag > EE_RAM_SIZE - 16) return false;
    const uint32_t dma = MEM_LW(tag);
    const bool ref = dma == (0x30000000u | (size / 4));
    if ((size & 3) || (!ref && dma != (0x10000000u | (size / 4))) ||
        MEM_LW(tag + 8) || MEM_LW(tag + 12)) return false;
    const uint32_t header = ref ? MEM_LW(tag + 4) : tag + 16;
    if (!header || (header & 15) || header > EE_RAM_SIZE - size * 4 ||
        reinterpret_cast<const uint8_t *>(words) != g_ram + header) return false;
    std::lock_guard<std::mutex> lock(recipe_mutex);
    const auto r = recipes.find(header);
    if (r == recipes.end() || ref != (r->second.fields == MaterialState::Texture)) return false;
    const MaterialPacket packet(r->second);
    const uint32_t pre[] = {0x01000101, 0x05000000, 0x1337, 0x6c00c000u | packet.count << 16};
    const uint32_t post[] = {0, 0, 0, 0x140004a7};
    if (size != (packet.count + 2) * 4 || std::memcmp(words, pre, sizeof pre) ||
        std::memcmp(words + size - 4, post, sizeof post) ||
        std::memcmp(words + 4, packet.qwords.data(), packet.count * 16) ||
        std::memcmp(top, packet.qwords.data(), packet.count * 16)) return false;
    out = r->second;
    return true;
}
}

extern "C" void f_0035F9A0() {
    const uint32_t packet = R[4].uw[0];
    const bool capture = gs::material_state_enabled() && !diff_in_original_side() &&
                         packet && !(packet & 15) && packet <= EE_RAM_SIZE - 112;
    const auto state = capture ? gs::texture_bind() : gs::MaterialState{};
    f_0035F9A0_orig();
    if (capture) {
        std::lock_guard<std::mutex> lock(gs::recipe_mutex);
        if (state.fields) gs::recipes[packet] = state;
        else gs::recipes.erase(packet);
    }
}

extern "C" void f_00361F80() {
    // This call site is the boundary between RenderStateCommit's ordered GS
    // writes and its separate TEX1 and PRIM VU setup packets.
    const uint32_t packet = R[4].uw[0];
    if (R[31].uw[0] == 0x362588 && R[5].ud[0] && !R[6].ud[0] && !R[7].ud[0] &&
        packet && !(packet & 15) && packet <= EE_RAM_SIZE - 19*16 &&
        gs::material_state_enabled() && !diff_in_original_side()) {
        const auto state = gs::render_state();
        std::lock_guard<std::mutex> lock(gs::recipe_mutex);
        if (state.fields) gs::recipes[packet + 16] = state;
        else gs::recipes.erase(packet + 16);
    }
    f_00361F80_orig();
}
