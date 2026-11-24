// Host transfer built by EE texture manager 0x35FC40. A cached chain can be
// called many frames after creation, so its source bytes are read at DMA time.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace gs {
struct alignas(16) TextureUpload {
    uint32_t source, qwc;
    uint16_t dbp, width, height, y;
    uint8_t pitch, psm;
    bool bootstrap = false;

    uint64_t bitbltbuf() const {
        const uint64_t layout = uint64_t(pitch) << 16 | uint64_t(psm) << 24;
        return (bootstrap ? 0 : layout) | ((layout | dbp) << 32);
    }
    uint64_t trxreg() const { return width | (uint64_t(height) << 32); }
    uint64_t trxpos() const { return uint64_t(y) << 48; }
    uint64_t image_tag() const { return (2ull << 58) | (bootstrap ? 0x8000 : 0) | qwc; }
};
struct UploadQword { uint64_t lo, hi; };
using UploadChain = std::array<UploadQword, 13>;
constexpr size_t kUploadPrefixQwords = 9;
inline UploadChain texture_upload_chain(const TextureUpload &u, bool end_chain, bool eop) {
    constexpr uint64_t ad = 1ull << 60;
    return {{{0x10000005, 0}, {ad | 4, 14}, {u.bitbltbuf(), 0x50},
             {u.trxreg(), 0x52}, {u.trxpos(), 0x51}, {0, 0x53},
             {0x10000001, 0}, {u.image_tag(), 0},
             {0x30000000ull | u.qwc | uint64_t(u.source) << 32, 0},
             {end_chain ? 0x70000001ull : 0x10000001ull, 0},
             {ad | (eop ? 0x8000 : 0), 14}, {0x10000001, 0}, {ad, 14}}};
}
// 0x389500 SDK normal-DMA layout used by the 16 boot pages in 0x35B060.
inline std::array<UploadQword, 6> boot_upload_header(const TextureUpload &u) {
    return {{{(1ull << 60) | 4, 14}, {u.bitbltbuf(), 0x50}, {u.trxpos(), 0x51},
             {u.trxreg(), 0x52}, {0, 0x53}, {u.image_tag(), 0}}};
}
constexpr UploadQword texture_fence(bool eop) { return {(1ull << 60) | (eop ? 0x8000u : 0u), 14}; }
bool find_texture_fence(uint32_t tag, const uint8_t *guest, bool &eop);
void enqueue_texture_fence(bool eop);
bool native_texture_uploads_enabled();
bool try_boot_texture_upload(const TextureUpload &upload, uint32_t packet);
bool immediate_texture_upload_ready();
bool find_texture_upload(uint32_t packet, const uint8_t *guest, TextureUpload &out);
void enqueue_texture_upload(const TextureUpload &upload, const uint8_t *bytes);
} // namespace gs
