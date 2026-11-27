// runtime/remaster/ee_texture_decoder.cpp
// Self-contained retail PS2 texture decoder reading directly from EE RAM (*(0x4D2D30)).
// ZERO #include of runtime/gs/*, runtime/vu/*, or vif_mesh.h.
#include "ee_texture_decoder.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace remaster {

namespace {

constexpr uint32_t kTextureTablePtr = 0x004D2D30;
constexpr uint32_t kTextureSlots    = 512;
constexpr uint32_t kSlotStride      = 0x44;

inline bool in_bounds(uint32_t addr, uint32_t bytes, uint32_t ram_size) {
    return addr <= ram_size && bytes <= ram_size - addr;
}

inline uint32_t rd_u32(const uint8_t *ram, uint32_t addr) {
    uint32_t v = 0;
    std::memcpy(&v, ram + addr, 4);
    return v;
}

inline int32_t rd_s32(const uint8_t *ram, uint32_t addr) {
    int32_t v = 0;
    std::memcpy(&v, ram + addr, 4);
    return v;
}

inline uint16_t rd_u16(const uint8_t *ram, uint32_t addr) {
    uint16_t v = 0;
    std::memcpy(&v, ram + addr, 2);
    return v;
}

inline uint8_t expand_ps2_alpha(uint8_t a) {
    const uint32_t v = static_cast<uint32_t>(a) * 2u;
    return static_cast<uint8_t>(v > 255u ? 255u : v);
}

// Self-contained FNV-1a 64-bit texture identity keys matching material_table.inc
// (identical to runtime/gs/texture_replacements.cpp without any runtime/gs/* include).
constexpr uint64_t kFnvOffset = 0xCBF29CE484222325ull;
constexpr uint64_t kFnvPrime  = 0x100000001B3ull;

inline uint64_t fnv1a64(const void *data, size_t n, uint64_t h = kFnvOffset) {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * kFnvPrime;
    return h;
}

uint64_t compute_index_key(uint32_t bits, uint32_t width, uint32_t height, const uint8_t *indices) {
    uint8_t head[16];
    std::memcpy(head, "EODTEX1", 7);
    head[7] = static_cast<uint8_t>(bits);
    for (int i = 0; i < 4; ++i) {
        head[8 + i] = static_cast<uint8_t>(width >> (8 * i));
        head[12 + i] = static_cast<uint8_t>(height >> (8 * i));
    }
    return fnv1a64(indices, static_cast<size_t>(width) * height, fnv1a64(head, sizeof(head)));
}

uint64_t compute_palette_key(const uint32_t *pal, uint32_t n) {
    uint64_t h = kFnvOffset;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t rgb[3] = {
            static_cast<uint8_t>(pal[i] & 0xFFu),
            static_cast<uint8_t>((pal[i] >> 8) & 0xFFu),
            static_cast<uint8_t>((pal[i] >> 16) & 0xFFu),
        };
        h = fnv1a64(rgb, 3, h);
    }
    return h;
}

bool ieq_name(const char *a, const std::string &b) {
    size_t i = 0;
    for (; i < 16 && a[i] != '\0'; ++i) {
        if (i >= b.size()) return false;
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return i == b.size();
}

// Compact upload-order byte address for 8-bit indexed retail textures (from tools/tex.py upload_addr8).
inline uint32_t upload_addr8(uint32_t x, uint32_t y, uint32_t width) {
    const uint32_t block = (y & ~15u) * width + (x & ~15u) * 2u;
    const uint32_t swap = (((y + 2u) >> 2) & 1u) * 4u;
    const uint32_t row = (((y & ~3u) >> 1) + (y & 1u)) & 7u;
    const uint32_t column = row * width * 2u + ((x + swap) & 7u) * 4u;
    const uint32_t lane = ((y >> 1) & 1u) + ((x >> 2) & 2u);
    return block + column + lane;
}

// Generalized exact CT32-to-PSMT4 inverse swizzle table for arbitrary power-of-2 (w, h) with h >= 32.
// Matches tools/texture_preview.py (_source_lanes) bit-for-bit; pure arithmetic, zero GS headers.
const std::vector<uint32_t> &get_psmt4_table(uint32_t w, uint32_t h) {
    static std::mutex mtx;
    static std::unordered_map<uint32_t, std::vector<uint32_t>> cache;
    const uint32_t key = (w << 16) | h;
    std::lock_guard<std::mutex> lk(mtx);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    static constexpr uint8_t kB32[4][8] = {
        {0, 1, 4, 5, 16, 17, 20, 21},
        {2, 3, 6, 7, 18, 19, 22, 23},
        {8, 9, 12, 13, 24, 25, 28, 29},
        {10, 11, 14, 15, 26, 27, 30, 31},
    };
    static constexpr uint8_t kC32[8][8] = {
        {0, 1, 4, 5, 8, 9, 12, 13},
        {2, 3, 6, 7, 10, 11, 14, 15},
        {16, 17, 20, 21, 24, 25, 28, 29},
        {18, 19, 22, 23, 26, 27, 30, 31},
        {32, 33, 36, 37, 40, 41, 44, 45},
        {34, 35, 38, 39, 42, 43, 46, 47},
        {48, 49, 52, 53, 56, 57, 60, 61},
        {50, 51, 54, 55, 58, 59, 62, 63},
    };
    static constexpr uint8_t kB4[8][4] = {
        {0, 2, 8, 10}, {1, 3, 9, 11},
        {4, 6, 12, 14}, {5, 7, 13, 15},
        {16, 18, 24, 26}, {17, 19, 25, 27},
        {20, 22, 28, 30}, {21, 23, 29, 31},
    };
    static constexpr uint16_t kC4[16][32] = {
        {0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106, 4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110},
        {16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122, 20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126},
        {65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43, 69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47},
        {81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59, 85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63},
        {192, 200, 224, 232, 128, 136, 160, 168, 194, 202, 226, 234, 130, 138, 162, 170, 196, 204, 228, 236, 132, 140, 164, 172, 198, 206, 230, 238, 134, 142, 166, 174},
        {208, 216, 240, 248, 144, 152, 176, 184, 210, 218, 242, 250, 146, 154, 178, 186, 212, 220, 244, 252, 148, 156, 180, 188, 214, 222, 246, 254, 150, 158, 182, 190},
        {129, 137, 161, 169, 193, 201, 225, 233, 131, 139, 163, 171, 195, 203, 227, 235, 133, 141, 165, 173, 197, 205, 229, 237, 135, 143, 167, 175, 199, 207, 231, 239},
        {145, 153, 177, 185, 209, 217, 241, 249, 147, 155, 179, 187, 211, 219, 243, 251, 149, 157, 181, 189, 213, 221, 245, 253, 151, 159, 183, 191, 215, 223, 247, 255},
        {256, 264, 288, 296, 320, 328, 352, 360, 258, 266, 290, 298, 322, 330, 354, 362, 260, 268, 292, 300, 324, 332, 356, 364, 262, 270, 294, 302, 326, 334, 358, 366},
        {272, 280, 304, 312, 336, 344, 368, 376, 274, 282, 306, 314, 338, 346, 370, 378, 276, 284, 308, 316, 340, 348, 372, 380, 278, 286, 310, 318, 342, 350, 374, 382},
        {321, 329, 353, 361, 257, 265, 289, 297, 323, 331, 355, 363, 259, 267, 291, 299, 325, 333, 357, 365, 261, 269, 293, 301, 327, 335, 359, 367, 263, 271, 295, 303},
        {337, 345, 369, 377, 273, 281, 305, 313, 339, 347, 371, 379, 275, 283, 307, 315, 341, 349, 373, 381, 277, 285, 309, 317, 343, 351, 375, 383, 279, 287, 311, 319},
        {448, 456, 480, 488, 384, 392, 416, 424, 450, 458, 482, 490, 386, 394, 418, 426, 452, 460, 484, 492, 388, 396, 420, 428, 454, 462, 486, 494, 390, 398, 422, 430},
        {464, 472, 496, 504, 400, 408, 432, 440, 466, 474, 498, 506, 402, 410, 434, 442, 468, 476, 500, 508, 404, 412, 436, 444, 470, 478, 502, 510, 406, 414, 438, 446},
        {385, 393, 417, 425, 449, 457, 481, 489, 387, 395, 419, 427, 451, 459, 483, 491, 389, 397, 421, 429, 453, 461, 485, 493, 391, 399, 423, 431, 455, 463, 487, 495},
        {401, 409, 433, 441, 465, 473, 497, 505, 403, 411, 435, 443, 467, 475, 499, 507, 405, 413, 437, 445, 469, 477, 501, 509, 407, 415, 439, 447, 471, 479, 503, 511},
    };

    const bool full_pages = (w >= 128u && h >= 128u);
    const uint32_t upload_w = full_pages ? (w >> 1) : (h >> 1);
    const uint32_t upload_h = full_pages ? (h >> 2) : (w >> 2);
    const uint32_t upload_pitch = full_pages ? (((w + 63u) / 64u + 1u) >> 1)
                                             : (((h + 63u) / 64u + 1u) >> 1);
    const uint32_t max_pages = std::max(1u, upload_pitch) * std::max(1u, (upload_h + 31u) >> 5) + 1u;

    std::vector<uint32_t> src_byte(static_cast<size_t>(max_pages) * 2048u, 0u);
    for (uint32_t y = 0; y < upload_h; ++y) {
        for (uint32_t x = 0; x < upload_w; ++x) {
            const uint32_t page = (y >> 5) * upload_pitch + (x >> 6);
            const uint32_t blk  = kB32[(y >> 3) & 3u][(x >> 3) & 7u];
            const uint32_t vw   = ((page * 32u + blk) * 64u) + kC32[y & 7u][x & 7u];
            if (vw < src_byte.size()) src_byte[vw] = (y * upload_w + x) * 4u;
        }
    }

    const uint32_t pitch = ((w + 63u) / 64u) >> 1;
    std::vector<uint32_t> nibbles(static_cast<size_t>(w) * h, 0u);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t page = (y >> 7) * pitch + (x >> 7);
            const uint32_t blk  = kB4[(y >> 4) & 7u][(x >> 5) & 3u];
            const uint32_t addr = ((page * 32u + blk) * 512u) + kC4[y & 15u][x & 31u];
            const uint32_t byte = addr >> 1;
            const uint32_t nib  = addr & 1u;
            const uint32_t vw   = byte >> 2;
            const uint32_t src  = (vw < src_byte.size() ? src_byte[vw] : 0u) + (byte & 3u);
            nibbles[static_cast<size_t>(y) * w + x] = src * 2u + nib;
        }
    }
    return cache.emplace(key, std::move(nibbles)).first->second;
}

} // namespace

bool decode_ee_texture_header(const uint8_t *ram, uint32_t ram_size,
                              uint32_t tex_hdr_addr, CpuImageRgba8 &out) {
    if (!ram || !tex_hdr_addr || (tex_hdr_addr & 3u) || !in_bounds(tex_hdr_addr, 0x44, ram_size))
        return false;

    const uint32_t bpp_type = rd_u32(ram, tex_hdr_addr + 0x10);
    const uint16_t log2_w   = rd_u16(ram, tex_hdr_addr + 0x18);
    const uint16_t log2_h   = rd_u16(ram, tex_hdr_addr + 0x1A);
    if (log2_w > 10 || log2_h > 10) return false;
    const uint32_t w = 1u << log2_w;
    const uint32_t h = 1u << log2_h;
    const uint32_t data_ptr = rd_u32(ram, tex_hdr_addr + 0x24);
    if (!data_ptr || data_ptr >= ram_size) return false;

    out.width = w;
    out.height = h;
    out.is_srgb = true;
    out.index_key = 0;
    out.palette_key = 0;
    out.has_indexed_keys = false;
    out.pixels.resize(static_cast<size_t>(w) * h * 4);

    if (bpp_type == 8u) {
        // 4-bit indexed (PSMT4)
        const uint32_t pal_ptr = rd_u32(ram, tex_hdr_addr + 0x40);
        const uint32_t data_bytes = (w * h) / 2u;
        if (!pal_ptr || !in_bounds(pal_ptr, 24 * 4, ram_size) || !in_bounds(data_ptr, data_bytes, ram_size))
            return false;
        uint32_t clut[16];
        for (uint32_t i = 0; i < 16; ++i) {
            clut[i] = rd_u32(ram, pal_ptr + 4u * (i + (i & 8u)));
        }
        const uint8_t *src = ram + data_ptr;
        const bool swizzled = (h >= 32u && w >= 16u);
        const std::vector<uint32_t> *tbl = swizzled ? &get_psmt4_table(w, h) : nullptr;
        std::vector<uint8_t> plane(static_cast<size_t>(w) * h);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const uint32_t nib = tbl ? (*tbl)[static_cast<size_t>(y) * w + x] : (y * w + x);
                const uint32_t byte_i = nib >> 1;
                const uint8_t idx = (byte_i < data_bytes) ? ((src[byte_i] >> ((nib & 1u) * 4u)) & 0xFu) : 0u;
                plane[static_cast<size_t>(y) * w + x] = idx;
                const uint32_t c = clut[idx];
                uint8_t *dst = out.pixels.data() + (static_cast<size_t>(y) * w + x) * 4;
                dst[0] = static_cast<uint8_t>(c & 0xFFu);
                dst[1] = static_cast<uint8_t>((c >> 8) & 0xFFu);
                dst[2] = static_cast<uint8_t>((c >> 16) & 0xFFu);
                dst[3] = expand_ps2_alpha(static_cast<uint8_t>((c >> 24) & 0xFFu));
            }
        }
        out.index_key = compute_index_key(4u, w, h, plane.data());
        out.palette_key = compute_palette_key(clut, 16u);
        out.has_indexed_keys = true;
        return true;
    } else if (bpp_type == 4u) {
        // 8-bit indexed (PSMT8)
        const uint32_t pal_ptr = rd_u32(ram, tex_hdr_addr + 0x40);
        const uint32_t data_bytes = w * h;
        if (!pal_ptr || !in_bounds(pal_ptr, 256 * 4, ram_size) || !in_bounds(data_ptr, data_bytes, ram_size))
            return false;
        uint32_t clut[256];
        for (uint32_t i = 0; i < 256; ++i) {
            const uint32_t ci = (i & ~0x18u) | ((i & 8u) << 1) | ((i & 16u) >> 1);
            clut[i] = rd_u32(ram, pal_ptr + 4u * ci);
        }
        const uint8_t *src = ram + data_ptr;
        const bool swizzled = (h >= 32u && w >= 16u);
        std::vector<uint8_t> plane(static_cast<size_t>(w) * h);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const uint32_t addr = swizzled ? upload_addr8(x, y, w) : (y * w + x);
                const uint8_t idx = (addr < data_bytes) ? src[addr] : 0u;
                plane[static_cast<size_t>(y) * w + x] = idx;
                const uint32_t c = clut[idx];
                uint8_t *dst = out.pixels.data() + (static_cast<size_t>(y) * w + x) * 4;
                dst[0] = static_cast<uint8_t>(c & 0xFFu);
                dst[1] = static_cast<uint8_t>((c >> 8) & 0xFFu);
                dst[2] = static_cast<uint8_t>((c >> 16) & 0xFFu);
                dst[3] = expand_ps2_alpha(static_cast<uint8_t>((c >> 24) & 0xFFu));
            }
        }
        out.index_key = compute_index_key(8u, w, h, plane.data());
        out.palette_key = compute_palette_key(clut, 256u);
        out.has_indexed_keys = true;
        return true;
    } else if (bpp_type == 0u || bpp_type == 1u) {
        // 32-bit direct RGBA
        const uint32_t data_bytes = w * h * 4u;
        if (!in_bounds(data_ptr, data_bytes, ram_size)) return false;
        const uint8_t *src = ram + data_ptr;
        for (uint32_t i = 0; i < w * h; ++i) {
            out.pixels[i * 4 + 0] = src[i * 4 + 0];
            out.pixels[i * 4 + 1] = src[i * 4 + 1];
            out.pixels[i * 4 + 2] = src[i * 4 + 2];
            out.pixels[i * 4 + 3] = expand_ps2_alpha(src[i * 4 + 3]);
        }
        return true;
    } else if (bpp_type == 2u) {
        // 16-bit RGBA5551
        const uint32_t data_bytes = w * h * 2u;
        if (!in_bounds(data_ptr, data_bytes, ram_size)) return false;
        for (uint32_t i = 0; i < w * h; ++i) {
            const uint16_t p = rd_u16(ram, data_ptr + i * 2u);
            out.pixels[i * 4 + 0] = static_cast<uint8_t>((p & 0x1Fu) << 3);
            out.pixels[i * 4 + 1] = static_cast<uint8_t>(((p >> 5) & 0x1Fu) << 3);
            out.pixels[i * 4 + 2] = static_cast<uint8_t>(((p >> 10) & 0x1Fu) << 3);
            out.pixels[i * 4 + 3] = (p & 0x8000u) ? 255u : 0u;
        }
        return true;
    }
    return false;
}

bool decode_ee_texture_by_slot(const uint8_t *ram, uint32_t ram_size,
                               int32_t slot, CpuImageRgba8 &out,
                               std::string *out_name) {
    if (!ram || slot < 0 || static_cast<uint32_t>(slot) >= kTextureSlots ||
        !in_bounds(kTextureTablePtr, 4, ram_size))
        return false;
    const uint32_t table = rd_u32(ram, kTextureTablePtr);
    if (!table || !in_bounds(table, kTextureSlots * kSlotStride, ram_size)) return false;

    const uint32_t entry = table + static_cast<uint32_t>(slot) * kSlotStride;
    if (rd_s32(ram, entry + 0x00) <= 0) return false;
    if (out_name) {
        char buf[17] = {};
        std::memcpy(buf, ram + entry + 0x04, 16);
        *out_name = buf;
    }
    uint32_t tex_hdr = rd_u32(ram, entry + 0x38);
    if (!tex_hdr) tex_hdr = rd_u32(ram, entry + 0x18);
    return decode_ee_texture_header(ram, ram_size, tex_hdr, out);
}

std::string get_ee_texture_slot_name(const uint8_t *ram, uint32_t ram_size, int32_t slot) {
    if (!ram || slot < 0 || static_cast<uint32_t>(slot) >= kTextureSlots ||
        !in_bounds(kTextureTablePtr, 4, ram_size))
        return {};
    const uint32_t table = rd_u32(ram, kTextureTablePtr);
    if (!table || !in_bounds(table, kTextureSlots * kSlotStride, ram_size)) return {};
    const uint32_t entry = table + static_cast<uint32_t>(slot) * kSlotStride;
    if (rd_s32(ram, entry + 0x00) <= 0) return {};
    char buf[17] = {};
    std::memcpy(buf, ram + entry + 0x04, 16);
    return std::string(buf);
}

bool decode_ee_texture_by_name(const uint8_t *ram, uint32_t ram_size,
                               const std::string &name, CpuImageRgba8 &out) {
    if (!ram || name.empty() || !in_bounds(kTextureTablePtr, 4, ram_size)) return false;
    const uint32_t table = rd_u32(ram, kTextureTablePtr);
    if (!table || !in_bounds(table, kTextureSlots * kSlotStride, ram_size)) return false;

    for (uint32_t s = 0; s < kTextureSlots; ++s) {
        const uint32_t entry = table + s * kSlotStride;
        if (rd_s32(ram, entry + 0x00) <= 0) continue;
        const char *slot_name = reinterpret_cast<const char *>(ram + entry + 0x04);
        if (!ieq_name(slot_name, name)) continue;
        uint32_t tex_hdr = rd_u32(ram, entry + 0x38);
        if (!tex_hdr) tex_hdr = rd_u32(ram, entry + 0x18);
        if (decode_ee_texture_header(ram, ram_size, tex_hdr, out)) return true;
    }
    return false;
}

} // namespace remaster
