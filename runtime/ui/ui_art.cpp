// ui_art.cpp - see ui_art.h.
#include "../core/host_file.h"
#include "ui_art.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "../gs/gs_mem.h"
#include "../platform/platform.h"

namespace ui_art {

namespace {
uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
}  // namespace

bool Archive::load(const std::string &path) {
    FILE *f = host_fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0x28 || n > (64 << 20)) { fclose(f); return false; }
    data_.resize((size_t)n);
    const bool ok = fread(data_.data(), 1, data_.size(), f) == data_.size();
    fclose(f);
    if (!ok || memcmp(data_.data(), "0TSR", 4) != 0) return false;
    const uint32_t version = rd32(&data_[8]), dir = rd32(&data_[20]), count = rd32(&data_[24]);
    if (version != 6 || count > 4096 || (uint64_t)dir + (uint64_t)count * 0x44 > data_.size()) return false;
    entries_.clear();
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = &data_[dir + i * 0x44];
        Entry en;
        en.name.assign((const char *)e, strnlen((const char *)e, 32));
        en.size = rd32(e + 0x28);
        en.offset = dir + rd32(e + 0x2C);
        if ((uint64_t)en.offset + en.size > data_.size()) return false;
        entries_.push_back(en);
    }
    return true;
}

bool Archive::get(const std::string &name, std::vector<uint8_t> &out) const {
    for (const Entry &e : entries_)
        if (e.name == name) {
            out.assign(data_.begin() + e.offset, data_.begin() + e.offset + e.size);
            return true;
        }
    return false;
}

std::vector<std::string> Archive::names() const {
    std::vector<std::string> n;
    for (const Entry &e : entries_) n.push_back(e.name);
    return n;
}

bool decode_tex(const std::vector<uint8_t> &t, Image &out) {
    if (t.size() < 0x80 + 0x400) return false;
    const uint32_t w = rd32(&t[0]), h = rd32(&t[4]), pal = rd32(&t[0x18]), mip = rd32(&t[0x1C]);
    if (w < 8 || h < 8 || w > 1024 || h > 1024 || (w & (w - 1)) || (h & (h - 1))) return false;
    if (pal + 0x400 > t.size() || mip >= t.size()) return false;
    const size_t bytes = t.size() - mip;
    int bpp;
    if (bytes >= (size_t)w * h) bpp = 8;
    else if (bytes >= (size_t)w * h / 2) bpp = 4;
    else return false;
    uint32_t palette[256];
    for (int i = 0; i < 256; i++) {
        const uint8_t *p = &t[pal + i * 4];  // A, R, G, B
        palette[i] = p[1] | (p[2] << 8) | (p[3] << 16) | ((uint32_t)p[0] << 24);
    }
    const uint8_t *m = &t[mip];
    out.w = (int)w;
    out.h = (int)h;
    out.rgba.assign((size_t)w * h, 0);
    // swizzled when large enough to be uploaded as a CT32 rectangle
    const bool swizzled = bpp == 4 ? (w >= 128 && h >= 128) : (h >= 32);
    if (!swizzled) {
        for (uint32_t i = 0; i < w * h; i++) {
            const uint32_t v = bpp == 8 ? m[i] : (m[i >> 1] >> ((i & 1) * 4)) & 15;
            out.rgba[i] = palette[v];
        }
        return true;
    }
    // private GS memory image: write the CT32 rectangle, read back as T4/T8
    const uint32_t cw = w / 2, ch = bpp == 4 ? h / 4 : h / 2;
    const uint32_t bwc = std::max(1u, w / 128), bwt = std::max(2u, w / 64);
    const size_t vbytes = (size_t)w * h * 4 + 65536;
    std::vector<uint8_t> vram(vbytes);
    for (uint32_t y = 0; y < ch; y++)
        for (uint32_t x = 0; x < cw; x++) {
            const size_t src = 4 * ((size_t)y * cw + x);
            if (src + 4 > bytes) continue;
            const size_t a = (size_t)gs::addr32((int)x, (int)y, 0, bwc) * 4;
            if (a + 4 <= vbytes) memcpy(&vram[a], m + src, 4);
        }
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t v;
            if (bpp == 4) {
                const uint32_t a = gs::addr4((int)x, (int)y, 0, bwt);
                v = (a >> 1) < vbytes ? (vram[a >> 1] >> ((a & 1) * 4)) & 15 : 0;
            } else {
                const uint32_t a = gs::addr8((int)x, (int)y, 0, bwt);
                v = a < vbytes ? vram[a] : 0;
            }
            out.rgba[(size_t)y * w + x] = palette[v];
        }
    return true;
}

bool decode_sfx(const std::vector<uint8_t> &s, std::vector<int16_t> &out, int &rate) {
    if (s.size() < 0x30 || memcmp(&s[0], "", 0) != 0) return false;
    const uint32_t size = rd32(&s[0]), start = rd32(&s[4]);
    // +0x10 holds 11025 and +0x14 22050 in the retail menu sounds; the SPU plays them at 22050 [inferred].
    rate = (int)rd32(&s[0x14]);
    if (rate < 4000 || rate > 48000) rate = 22050;
    if (start >= s.size()) return false;
    const size_t end = std::min(s.size(), (size_t)start + size);
    static const int f0[5] = {0, 60, 115, 98, 122}, f1[5] = {0, 0, -52, -55, -60};
    int h1 = 0, h2 = 0;
    out.clear();
    for (size_t b = start; b + 16 <= end; b += 16) {
        const int shift = s[b] & 15, filter = std::min(4, s[b] >> 4), flags = s[b + 1];
        for (int i = 0; i < 28; i++) {
            const int byte = s[b + 2 + i / 2];
            int n = (i & 1) ? byte >> 4 : byte & 15;
            if (n >= 8) n -= 16;
            int v = (n << 12) >> shift;
            v += (h1 * f0[filter] + h2 * f1[filter] + 32) / 64;
            v = std::clamp(v, -32768, 32767);
            h2 = h1;
            h1 = v;
            out.push_back((int16_t)v);
        }
        if (flags & 1) break;  // loop end / end of sample
    }
    return !out.empty();
}

namespace {
bool g_tried, g_loaded;
Sprite g_sprites[(int)Spr::Count];
std::vector<int16_t> g_sounds[(int)Sfx::Count];
int g_rates[(int)Sfx::Count];
std::string g_summary = "UI art not loaded";

Sprite region(uint32_t tex, const Image &img, int x, int y, int w, int h) {
    Sprite s;
    s.tex = tex;
    s.u0 = (float)x / img.w;
    s.v0 = (float)y / img.h;
    s.u1 = (float)(x + w) / img.w;
    s.v1 = (float)(y + h) / img.h;
    s.w = (float)w;
    s.h = (float)h;
    return s;
}
}  // namespace

bool load(const std::string &game_root, AddTexture add) {
    if (g_tried) return g_loaded;
    g_tried = true;
    std::vector<std::string> roots;
    if (const char *mod = getenv("EOD_MOD_DIR")) roots.push_back(mod);
    roots.push_back(game_root);
    Archive ar;
    bool have = false;
    for (const std::string &r : roots) {
        char path[1024];
        if (!r.empty() && platform_find_file(r.c_str(), "DATA/RES/UIACTION.RES", path, sizeof path) && ar.load(path)) {
            have = true;
            break;
        }
    }
    if (!have) {
        g_summary = "UIACTION.RES not found: every frame, plate and glyph is drawn procedurally";
        fprintf(stderr, "[frontend] %s\n", g_summary.c_str());
        return false;
    }
    std::string used, missing;
    auto tex = [&](const char *name, Image &img) -> uint32_t {
        std::vector<uint8_t> raw;
        if (ar.get(name, raw) && decode_tex(raw, img)) {
            used += std::string(used.empty() ? "" : ", ") + name;
            return add(img.w, img.h, img.rgba.data());
        }
        missing += std::string(missing.empty() ? "" : ", ") + name;
        return 0;
    };
    Image box, plate, metal, chain, hazard, pad;
    if (uint32_t t = tex("tab_box.art", box)) {
        // riveted corner brackets, a horizontal bar and a vertical bar (measured from the decoded art)
        g_sprites[(int)Spr::CornerTL] = region(t, box, 0, 0, 32, 32);
        g_sprites[(int)Spr::CornerTR] = region(t, box, 33, 0, 32, 32);
        g_sprites[(int)Spr::CornerBL] = region(t, box, 0, 96, 32, 32);
        g_sprites[(int)Spr::CornerBR] = region(t, box, 42, 96, 32, 32);
        g_sprites[(int)Spr::BarH] = region(t, box, 0, 59, 96, 10);
        g_sprites[(int)Spr::BarV] = region(t, box, 105, 0, 7, 128);
    }
    if (uint32_t t = tex("menu_plate.art", plate)) g_sprites[(int)Spr::Plate] = region(t, plate, 14, 8, 98, 112);
    if (uint32_t t = tex("metal_back.art", metal)) g_sprites[(int)Spr::Metal] = region(t, metal, 0, 290, 512, 140);
    if (uint32_t t = tex("menu_chain.art", chain)) g_sprites[(int)Spr::Chain] = region(t, chain, 0, 229, 256, 19);
    if (uint32_t t = tex("chalplate.art", hazard)) g_sprites[(int)Spr::Hazard] = region(t, hazard, 0, 0, 128, 31);
    if (uint32_t t = tex("buttonps.art", pad)) {
        g_sprites[(int)Spr::PadDpad] = region(t, pad, 66, 100, 29, 28);
        g_sprites[(int)Spr::PadCross] = region(t, pad, 1, 34, 30, 30);
        g_sprites[(int)Spr::PadSquare] = region(t, pad, 34, 34, 30, 30);
        g_sprites[(int)Spr::PadCircle] = region(t, pad, 65, 34, 30, 30);
        g_sprites[(int)Spr::PadTriangle] = region(t, pad, 98, 34, 30, 30);
        g_sprites[(int)Spr::PadL1] = region(t, pad, 0, 73, 32, 19);
        g_sprites[(int)Spr::PadR1] = region(t, pad, 32, 73, 32, 19);
        g_sprites[(int)Spr::PadL2] = region(t, pad, 64, 68, 32, 28);
        g_sprites[(int)Spr::PadR2] = region(t, pad, 96, 68, 32, 29);
    }
    static const char *sfx[(int)Sfx::Count] = {"a_press.sfx", "a_accept.sfx", "a_back.sfx", "a_error.sfx"};
    std::string sounds;
    for (int i = 0; i < (int)Sfx::Count; i++) {
        std::vector<uint8_t> raw;
        if (ar.get(sfx[i], raw) && decode_sfx(raw, g_sounds[i], g_rates[i])) sounds += std::string(sounds.empty() ? "" : ", ") + sfx[i];
    }
    g_loaded = !used.empty();
    g_summary = "game art: " + (used.empty() ? std::string("none") : used) + (missing.empty() ? "" : "; undecoded: " + missing) +
                "; sounds: " + (sounds.empty() ? std::string("none") : sounds) +
                "; procedural: Xbox/keyboard glyphs, panel fills, tab strip, title plate lettering";
    fprintf(stderr, "[frontend] %s\n", g_summary.c_str());
    return g_loaded;
}

Sprite sprite(Spr s) { return g_sprites[(int)s]; }
std::string summary() { return g_summary; }

const std::vector<int16_t> &sound(Sfx s, int &rate) {
    rate = g_rates[(int)s];
    return g_sounds[(int)s];
}

}  // namespace ui_art
