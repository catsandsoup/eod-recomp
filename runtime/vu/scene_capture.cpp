// scene_capture.cpp - see scene_capture.h. Format (little endian):
//   "EODSCN01", then records {u32 tag, u32 bytes, payload}:
//   'VBLK' u64 frame
//   'VU1R' u32 pc, top, itop, 0; vf[32] (16 bytes each); vi[0..15] (u32);
//          VU1 data memory (16 KB) - the state the micro program starts from
//   'GIFP' u32 path, u32 qwc, packet data (PATH1 = an XGKICK of the last VU1R)
// file.tex: one text line per textured GS draw state (header line first).
#include "scene_capture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../core/log.h"
#include "vu.h"

extern uint32_t g_vif1_top, g_vif1_itop;

namespace scene_capture {

namespace {
const char *g_path = getenv("EOD_SCENE_CAPTURE");
uint64_t env_u64(const char *name, uint64_t def) {
    const char *v = getenv(name);
    return v && *v ? strtoull(v, nullptr, 10) : def;
}
const uint64_t g_at = env_u64("EOD_SCENE_CAPTURE_AT", 600);
const uint64_t g_n = env_u64("EOD_SCENE_CAPTURE_N", 3);
uint64_t first() { return g_at > g_n ? g_at - g_n : 0; }

FILE *g_f;      // producer stream
bool g_done;
FILE *g_tex;    // GS side
bool g_tex_done;
uint32_t g_kicks, g_packets[4], g_cur_path;
uint64_t g_last_tex[3];

void rec(uint32_t tag, const void *a, uint32_t na, const void *b = nullptr, uint32_t nb = 0) {
    uint32_t h[2] = {tag, na + nb};
    fwrite(h, sizeof h, 1, g_f);
    fwrite(a, 1, na, g_f);
    if (nb) fwrite(b, 1, nb, g_f);
}
constexpr uint32_t fourcc(const char (&s)[5]) {
    return (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16 | (uint32_t)s[3] << 24;
}
}  // namespace

bool g_on = g_path && *g_path;

void vblank(uint64_t frame) {
    if (!g_on || g_done) return;
    if (!g_f && frame >= first()) {
        g_f = fopen(g_path, "wb");
        if (!g_f) {
            WARN("EOD_SCENE_CAPTURE: cannot write %s", g_path);
            g_done = true;
            return;
        }
        fwrite("EODSCN01", 1, 8, g_f);
    }
    if (!g_f) return;
    rec(fourcc("VBLK"), &frame, 8);
    if (frame > g_at) {
        fclose(g_f);
        g_f = nullptr;
        g_done = true;
        fprintf(stderr, "[scene-capture] wrote %s (vblanks %llu..%llu)\n", g_path, (unsigned long long)first(),
                (unsigned long long)frame);
    }
}

void vu1_start(const VUState *vu, uint32_t pc) {
    if (!g_f) return;
    uint8_t head[16 + 32 * 16 + 16 * 4];
    uint32_t w[4] = {pc, g_vif1_top, g_vif1_itop, 0};
    memcpy(head, w, 16);
    memcpy(head + 16, vu->vf, 32 * 16);
    memcpy(head + 16 + 32 * 16, vu->vi, 16 * 4);
    rec(fourcc("VU1R"), head, sizeof head, vu->mem, vu->mem_mask + 1);
}

void gif(int path, const uint8_t *data, uint32_t qwc) {
    if (!g_f) return;
    uint32_t w[2] = {(uint32_t)path, qwc};
    rec(fourcc("GIFP"), w, 8, data, qwc * 16);
}

void gs_vblank(uint64_t frame) {
    if (!g_on || g_tex_done) return;
    if (!g_tex && frame >= first()) {
        std::string p = std::string(g_path) + ".tex";
        g_tex = fopen(p.c_str(), "w");
        if (!g_tex) {
            g_tex_done = true;
            return;
        }
        fprintf(g_tex, "# vblank|draw path packet(path-local) kick tbp tbw psm tw th cpsm csa index_key palette_key\n");
    }
    if (!g_tex) return;
    fprintf(g_tex, "vblank %llu\n", (unsigned long long)frame);
    g_last_tex[0] = ~0ull;
    if (frame > g_at) {
        fclose(g_tex);
        g_tex = nullptr;
        g_tex_done = true;
    }
}

void gs_packet(int path) {
    g_cur_path = (uint32_t)path & 3;
    if (!g_tex) return;
    g_packets[g_cur_path]++;
    if (path == 1) g_kicks++;
}

bool gs_active() { return g_tex != nullptr; }

void gs_texture(uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th, uint32_t cpsm, uint32_t csa,
                uint64_t index_key, uint64_t palette_key) {
    if (!g_tex) return;
    // one line per packet and texture
    uint64_t id[3] = {(uint64_t)g_cur_path << 32 | g_packets[g_cur_path],
                      (uint64_t)tbp | (uint64_t)psm << 16 | (uint64_t)tw << 24 | (uint64_t)th << 40, palette_key};
    if (!memcmp(id, g_last_tex, sizeof id)) return;
    memcpy(g_last_tex, id, sizeof id);
    fprintf(g_tex, "draw %u %u %u %x %u %x %u %u %x %u %016llx %016llx\n", g_cur_path, g_packets[g_cur_path], g_kicks,
            tbp, tbw, psm, tw, th, cpsm, csa, (unsigned long long)index_key, (unsigned long long)palette_key);
}

}  // namespace scene_capture
