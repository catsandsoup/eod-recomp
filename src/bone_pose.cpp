// Skinned-mesh bone palette upload (SLUS_209.10), see docs/EE_RENDER_SUBMISSION.md.
//
//   0x34C9F8 BonePaletteUpload(first, bones, count): forwards to 0x35DBD8.
//   0x35DBD8 BonePaletteWrite(first, bones, count): widens count 3x4 bone matrices (12 floats,
//            stride 0x30) to 4x4 in its stack frame and appends them to the current VU1 DMA packet
//            with 0x35A810, targeting VU1 qword 0x90 + 4*first; returns the packet pointer or 0.
//   0x35A810 VifWriteUnpackMscal(dst, qwc, addr, components, count, src): header word, UNPACK
//            V<components>-32 of count vectors to VU1 qword 0x8F + addr, the data, then
//            MSCAL 0x24D8 (the skin setup program) padded to a quadword.
//   0x35DD28 DentOffsetWrite(addr, offsets, count) (via 0x34CA18): car dent offsets, V3-32 to VU1
//            qword 0x8F + addr (stage R3, docs/NATIVE_RENDERER.md).
//   Every packet 0x35A810 builds is also handed to the native renderer (native::ee_vu1_upload).
//   Callers of 0x34C9F8: 0x20B8D0 (pedestrians, drawn by 0x20DE68) and 0x20C800 (drivers, from
//   0x1E4610 and 0x235D98); both evaluate the animation inline and pass the pose array.
//
// EOD_FREEZE_POSE=<vblank> (diagnostic): from that vblank on, every upload replays the pose the
// same call (by return address and order within the frame) uploaded on that vblank.
#include "core/config.h"
#include "core/cpu.h"
#include "core/diff_harness.h"
#include "core/memory.h"
#include "kernel/kernel.h"
#include "remaster/scene_bridge.h"
#include "render/native_renderer.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

extern "C" {
void f_0034C9F8_orig(void);
void f_0035DBD8_orig(void);
void f_0035A810_orig(void);
void f_00366C88(void);  // PacketReserve(qwc) -> host-visible guest pointer or 0
void f_0035DD28_orig(void);
void f_0034CA18_orig(void);
void f_0035DD28(void);
}

namespace {

constexpr uint32_t kSkinSetupProgram = 0x24D8;              // VU1 setup run after each upload
constexpr uint32_t kMscalSkinSetup = 0x14000000u | (kSkinSetupProgram >> 3);
constexpr uint32_t kVifUnpackBase = 0x8F;                   // VU1 qword of addr 0
constexpr uint32_t kBoneMatrixBytes = 0x30;                 // 3x4 floats in the pose array

// 0x35A810
void vif_write_unpack_mscal(uint32_t dst, uint32_t qwc, uint32_t addr, uint32_t components, uint32_t count,
                            uint32_t src) {
    // the native renderer's copy of what this packet uploads (stage R3: palettes, dents, sun quad)
    native::ee_vu1_upload(dst, (addr + kVifUnpackBase) & 0x3FF, components, count, src);
    const uint32_t vn = components == 3 ? 0x08000000u : 0x0C000000u;  // V3-32 or V4-32
    MEM_SW(dst, 0x14000000u | (qwc << 16) | (kSkinSetupProgram >> 3));
    MEM_SW(dst + 4, 0x60000000u | vn | ((count & 0xFF) << 16) | 0x4000u | ((addr + kVifUnpackBase) & 0x3FF));
    const uint32_t bytes = components * count * 4;
    std::memmove(g_ram + dst + 8, g_ram + src, bytes);
    uint32_t p = dst + 8 + bytes;
    MEM_SW(p, kMscalSkinSetup);
    for (p += 4; ((p - dst) >> 2) & 3; p += 4) MEM_SW(p, 0);
}

void f_0035A810_new() {
    vif_write_unpack_mscal(g_ee.r[4].uw[0], g_ee.r[5].uw[0], g_ee.r[6].uw[0], g_ee.r[7].uw[0], g_ee.r[8].uw[0],
                           g_ee.r[9].uw[0]);
    g_ee.r[2].ud[0] = 0;
}

// 0x35DBD8
uint32_t bone_palette_write(uint32_t first, uint32_t bones, int32_t count) {
    const uint32_t sp = g_ee.r[29].uw[0];
    const uint32_t palette = sp - 0x550;  // the original's frame; the 4x4 palette sits at its base
    for (int32_t i = 0; i < count; i++) {
        const uint32_t s = bones + (uint32_t)i * kBoneMatrixBytes, d = palette + (uint32_t)i * 64;
        for (uint32_t row = 0; row < 4; row++) {
            for (uint32_t c = 0; c < 3; c++) MEM_SW(d + row * 16 + c * 4, MEM_LW(s + row * 12 + c * 4));
            MEM_SW(d + row * 16 + 12, row == 3 ? 0x3F800000u : 0);  // w: 0, 0, 0, 1
        }
    }
    const int32_t qwc = (16 * count + 6 >= 0 ? 16 * count + 6 : 16 * count + 9) >> 2;  // header + data + MSCAL
    g_ee.r[29].ud[0] = (uint64_t)(int64_t)(int32_t)palette;
    g_ee.r[4].sd[0] = qwc;
    f_00366C88();
    const uint32_t packet = g_ee.r[2].uw[0];
    if (packet) {
        // 4 components per vector, 4 vectors per bone, to VU1 qword 0x90 + 4*first
        vif_write_unpack_mscal(packet, (uint32_t)qwc, 4 * first + 1, 4, 4 * (uint32_t)count, palette);
        native::ee_pose_check(packet, bones, (uint32_t)count);
        if (g_config.remaster_engine && count > 0) {
            remaster::ee_record_bone_palette(packet, reinterpret_cast<const float *>(g_ram + palette),
                                             static_cast<uint32_t>(count), first);
        }
    }
    g_ee.r[29].ud[0] = (uint64_t)(int64_t)(int32_t)sp;
    return packet;
}

// 0x35DD28 DentOffsetWrite(addr, offsets, count): count V3-32 vectors (car dent offsets, 3 floats per
// bone) appended to the current VU1 DMA packet for VU1 qword 0x8F + addr; returns the packet or 0.
// Caller 0x34CA18 (VisualCarSubmitModel 0x2189C8: addr 1 = qword 0x90, offsets = the car's deform
// array + 12 * first bone of the damage zone); also the particle emitter 0x363B98.
uint32_t dent_offset_write(uint32_t addr, uint32_t offsets, int32_t count) {
    const uint32_t sp = g_ee.r[29].uw[0];
    const int32_t n3 = 3 * count;
    const int32_t qwc = (n3 + 6 > -1 ? n3 + 6 : n3 + 9) >> 2;  // header + data + MSCAL, in qwords
    g_ee.r[29].ud[0] = (uint64_t)(int64_t)(int32_t)(sp - 0x60);
    g_ee.r[4].sd[0] = qwc;
    g_ee.r[31].ud[0] = 0x35DD78;
    f_00366C88();
    const uint32_t packet = g_ee.r[2].uw[0];
    if (packet) {
        vif_write_unpack_mscal(packet, (uint32_t)qwc, addr, 3, (uint32_t)count, offsets);
        if (g_config.remaster_engine && count > 0) {
            remaster::ee_record_dent_offsets(packet, reinterpret_cast<const float *>(g_ram + offsets),
                                             static_cast<uint32_t>(count));
        }
    }
    g_ee.r[29].ud[0] = (uint64_t)(int64_t)(int32_t)sp;
    return packet;
}

void f_0035DD28_new() {
    const uint32_t addr = g_ee.r[4].uw[0], offsets = g_ee.r[5].uw[0];
    const int32_t count = g_ee.r[6].sw[0];
    const uint64_t ra = g_ee.r[31].ud[0];
    const uint32_t packet = dent_offset_write(addr, offsets, count);
    g_ee.r[31].ud[0] = ra;
    g_ee.r[2].sd[0] = (int64_t)(int32_t)packet;
}

// 0x34CA18 DentOffsetUpload(addr, offsets, count): forwards to 0x35DD28 (own 0x10-byte frame).
void f_0034CA18_new() {
    const uint32_t sp = g_ee.r[29].uw[0];
    const uint64_t ra = g_ee.r[31].ud[0];
    g_ee.r[29].ud[0] = (uint64_t)(int64_t)(int32_t)(sp - 0x10);
    g_ee.r[31].ud[0] = 0x34CA24;
    f_0035DD28();
    g_ee.r[29].ud[0] = (uint64_t)(int64_t)(int32_t)sp;
    g_ee.r[31].ud[0] = ra;
}

void f_0035DBD8_new() {
    const uint32_t first = g_ee.r[4].uw[0], bones = g_ee.r[5].uw[0];
    const int32_t count = g_ee.r[6].sw[0];
    g_ee.r[2].sd[0] = (int64_t)(int32_t)bone_palette_write(first, bones, count);
}

const uint64_t kFreezeAt = [] {
    const char *e = std::getenv("EOD_FREEZE_POSE");
    return e && *e ? std::strtoull(e, nullptr, 10) : ~0ull;
}();
std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> g_frozen;  // (ra, order) -> pose
uint64_t g_frame = ~0ull;
uint32_t g_order = 0;

void freeze_pose() {
    const uint64_t vb = kernel_vblank_count();
    if (vb != g_frame) g_frame = vb, g_order = 0;
    const uint32_t bones = g_ee.r[5].uw[0], count = g_ee.r[6].uw[0], ra = g_ee.r[31].uw[0];
    const auto key = std::make_pair(ra, g_order++);
    const size_t bytes = (size_t)count * kBoneMatrixBytes;
    if (count == 0 || count > 64 || bones + bytes >= EE_RAM_SIZE) return;
    if (vb == kFreezeAt) {
        g_frozen[key].assign(g_ram + bones, g_ram + bones + bytes);
    } else if (vb > kFreezeAt) {
        auto it = g_frozen.find(key);
        if (it != g_frozen.end() && it->second.size() == bytes) std::memcpy(g_ram + bones, it->second.data(), bytes);
    }
}

}  // namespace

extern "C" void f_0034C9F8(void) {
    if (kFreezeAt != ~0ull) freeze_pose();
    f_0034C9F8_orig();  // a tail call to 0x35DBD8
}

extern "C" void f_0035DBD8(void) { diff_run(0x0035DBD8, f_0035DBD8_new, f_0035DBD8_orig); }
extern "C" void f_0035A810(void) { diff_run(0x0035A810, f_0035A810_new, f_0035A810_orig); }
extern "C" void f_0035DD28(void) { diff_run(0x0035DD28, f_0035DD28_new, f_0035DD28_orig); }
extern "C" void f_0034CA18(void) { diff_run(0x0034CA18, f_0034CA18_new, f_0034CA18_orig); }
