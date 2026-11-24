#include "core/cpu.h"
#include "core/diff_harness.h"
#include "gs/texture_upload.h"
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

extern "C" void f_0035FC40_orig();
extern "C" void f_003896E8_orig();
extern "C" void f_00365878_orig();

namespace gs {
namespace {
// Eviction simply restores ordinary guest DMA. Byte validation below also
// covers freeing/reusing an asset or patching an existing upload in place.
struct Recipe { uint32_t packet = 0; TextureUpload transfer{}; };
struct RecipeSet { std::array<Recipe, 4> ways{}; unsigned next = 0; };
std::array<RecipeSet, 4096> recipes;
std::mutex recipes_mutex;
struct Fence { bool end, eop; };
std::unordered_map<uint32_t, Fence> fences;
RecipeSet &slot(uint32_t packet) { return recipes[((packet >> 4) * 2654435761u) >> 20]; }

void build_texture_upload() {
    const uint32_t packet = R[4].uw[0], source = R[6].uw[0];
    const uint32_t width = R[7].uw[0] & 0xffff, height = R[9].uw[0] & 0xffff;
    const uint64_t type = R[11].ud[0];
    uint8_t psm;
    uint32_t bpp;
    switch (type) {
    case 0: case 1: psm = 0; bpp = 32; break;
    case 2: psm = 2; bpp = 16; break;
    case 4: psm = 0x13; bpp = 8; break;
    case 8: psm = 0x14; bpp = 4; break;
    default: return f_0035FC40_orig();
    }
    const uint64_t bits = uint64_t(width) * height * bpp;
    const uint32_t qwc = uint32_t(((bits >> 3) + 15) >> 4);
    if (!packet || (packet & 15) || packet > EE_RAM_SIZE - sizeof(UploadChain) ||
        !width || width > 4095 || !height || height > 4095 || !qwc || qwc > 0x7fff ||
        (source & 15) || source > EE_RAM_SIZE - qwc * 16)
        return f_0035FC40_orig();

    const TextureUpload upload{source, qwc, uint16_t(R[5].uw[0] & 0x3fff),
        uint16_t(width), uint16_t(height), uint16_t(R[10].uw[0] & 0x7ff),
        uint8_t(R[8].uw[0] & 63), psm};
    const bool eop = MEM_LW(R[29].uw[0] + 8) != 0;
    const auto chain = texture_upload_chain(upload, MEM_LW(R[29].uw[0]) != 0, eop);
    const size_t count = eop ? 13 : 11;
    ee_set_hilo(0, 0, int64_t(bits));
    R[2].ud[0] = count;
    std::lock_guard<std::mutex> lock(recipes_mutex);
    memcpy(g_ram + packet, chain.data(), count * 16);
    fences[packet + 9 * 16] = {chain[9].lo == 0x70000001, eop};
    if (eop) fences[packet + 11 * 16] = {false, false};
    else fences.erase(packet + 11 * 16);
    auto &set = slot(packet);
    for (auto &entry : set.ways) {
        if (entry.packet == packet) { entry.transfer = upload; return; }
    }
    set.ways[set.next++ % set.ways.size()] = {packet, upload};
}
} // namespace

bool native_texture_uploads_enabled() {
    static const bool enabled = [] {
        const char *v = std::getenv("EOD_NATIVE_TEXTURE_UPLOAD");
#ifdef EOD_R7_RELEASE
        const bool requested = !v || *v != '0';
#else
        const bool requested = v && *v == '1';
#endif
        return requested && !std::getenv("EOD_GS_DUMP") && !std::getenv("EOD_GS_DUMP_COLD_START") &&
               !std::getenv("EOD_SCENE_CAPTURE") &&
               !std::getenv("EOD_SCENE_EMIT") && !std::getenv("EOD_MODERN") &&
               !std::getenv("EOD_GS_TRACE") && !std::getenv("EOD_GS_CENSUS");
    }();
    return enabled;
}

bool find_texture_fence(uint32_t tag, const uint8_t *guest, bool &eop) {
    if (!tag || (tag & 15) || tag > EE_RAM_SIZE - 32) return false;
    std::lock_guard<std::mutex> lock(recipes_mutex);
    const auto it = fences.find(tag);
    if (it == fences.end()) return false;
    const Fence &f = it->second;
    const UploadQword expected[] = {{f.end ? 0x70000001ull : 0x10000001ull, 0}, texture_fence(f.eop)};
    if (memcmp(guest, expected, sizeof expected)) return false;
    eop = f.eop;
    return true;
}

bool find_texture_upload(uint32_t packet, const uint8_t *guest, TextureUpload &out) {
    if (!packet || (packet & 15) || packet > EE_RAM_SIZE - kUploadPrefixQwords * 16) return false;
    std::lock_guard<std::mutex> lock(recipes_mutex);
    for (const auto &entry : slot(packet).ways) {
        if (entry.packet == packet) {
            out = entry.transfer;
            const auto expected = texture_upload_chain(out, false, false);
            return memcmp(guest, expected.data(), kUploadPrefixQwords * 16) == 0;
        }
    }
    return false;
}
} // namespace gs

extern "C" void f_0035FC40() {
    if (!gs::native_texture_uploads_enabled() || diff_in_original_side()) return f_0035FC40_orig();
    diff_run(0x0035FC40, gs::build_texture_upload, f_0035FC40_orig);
}

// 0x35B060 clears all 4 MiB through sixteen real copies of its 256 KiB
// staging block. Read the block even on boot: its value belongs to the EE.
extern "C" void f_003896E8() {
    if (!gs::native_texture_uploads_enabled() || diff_in_original_side()) return f_003896E8_orig();
    uint32_t page = R[17].uw[0];
    switch (R[31].uw[0]) {
    case 0x35b0d8: break;
    case 0x35b124: page += 1; break;
    case 0x35b170: page += 2; break;
    case 0x35b1c0: page -= 1; break; // caller advanced its loop counter before the last submit
    default: return f_003896E8_orig();
    }
    const uint32_t packet = R[4].uw[0], source = R[5].uw[0];
    if (page >= 16 || !source || (source & 15) || source > EE_RAM_SIZE - 0x40000 ||
        (packet & 15) || packet > EE_RAM_SIZE - 96)
        return f_003896E8_orig();
    const gs::TextureUpload u{source, 0x4000, uint16_t(page << 10), 1024, 64, 0, 16, 0, true};
    const auto header = gs::boot_upload_header(u);
    if (memcmp(g_ram + packet, header.data(), sizeof header) || !gs::try_boot_texture_upload(u, packet))
        return f_003896E8_orig();
    R[2].ud[0] = 0;
}

// The finalizer appends CNT(1)/EOP at the active bank cursor before sealing
// the DMA list. DMA can consume the other bank later in a different frame.
extern "C" void f_00365878() {
    if (!gs::native_texture_uploads_enabled() || diff_in_original_side()) return f_00365878_orig();
    const uint32_t manager = MEM_LW(0x3e597c);
    if (!manager || manager > EE_RAM_SIZE - 44) return f_00365878_orig();
    const uint32_t bank = MEM_LW(manager + 40);
    if (bank > 1) return f_00365878_orig();
    const uint32_t desc = manager + bank * 20;
    const uint32_t base = MEM_LW(desc + 4), used = MEM_LW(desc + 8), capacity = MEM_LW(desc + 16);
    const uint64_t tag = uint64_t(base) + uint64_t(used) * 16;
    if (uint64_t(used) + 3 <= capacity && tag && !(tag & 15) && tag <= EE_RAM_SIZE - 32) {
        std::lock_guard<std::mutex> lock(gs::recipes_mutex);
        gs::fences[uint32_t(tag)] = {false, true};
    }
    f_00365878_orig();
}
