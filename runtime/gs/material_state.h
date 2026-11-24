// Ordered PATH1 state from TextureBindBuild and RenderStateCommit. The only
// supported register sequences are those emitted by these two EE producers.
#pragma once
#include <array>
#include <cstdint>
#include <cstddef>

namespace gs {
struct MaterialState {
    enum Field : unsigned { Texture = 1, Clamp = 2, Test = 4, Zbuf = 8, Frame = 16,
                           Alpha = 32, Fog = 64, Scissor = 128, Dither = 256, Init = 512 };
    uint32_t fields = 0;
    uint64_t miptbp1 = 0, miptbp2 = 0, tex0 = 0;
    uint64_t clamp = 0, test = 0, zbuf = 0, frame = 0, alpha = 0, fog = 0, scissor = 0;
    template<class Write> void each(Write write) const {
        if (fields == Texture) {
            write(0x34, miptbp1);
            write(0x36, miptbp2);
            write(0x3f, 0x3full);
            write(0x06, tex0);
            return;
        }
        if (fields & Clamp) write(0x08, clamp);
        if (fields & Test) write(0x47, test);
        if (fields & Zbuf) write(0x4e, zbuf);
        if (fields & Frame) write(0x4c, frame);
        if (fields & Alpha) write(0x42, alpha);
        if (fields & Fog) write(0x3d, fog);
        if (fields & Scissor) write(0x40, scissor);
        if (fields & Dither) write(0x45, 0ull);
        if (fields & Init) {
            write(0x18, 0x71ff00006bffull);
            write(0x1a, 1ull);
            write(0x49, 0ull);
            write(0x3b, 0x8000000000ull);
            write(0x4a, 0ull);
            write(0x43, 0x48ull);
            write(0x46, 1ull);
        }
    }
};
struct MaterialPacket {
    std::array<std::array<uint64_t, 2>, 16> qwords{};
    unsigned count = 1;
    explicit MaterialPacket(const MaterialState &state) {
        state.each([&](unsigned reg, uint64_t value) { qwords[count++] = {value, reg}; });
        qwords[0] = {0x1000000000008000ull | (count - 1), 0xe};
    }
};
// EE 0x363708 changes the shadow pass blend equation and vertex color.
inline std::array<std::array<uint64_t, 2>, 3> shadow_blend_packet(bool additive) {
    const uint64_t level = additive ? 15 : 7;
    return {{{0x2000000000008001ull, 0x1e}, {additive ? 0x8000000029ull : 0x80000000a1ull, 0x42},
             {level | level << 32, level}}};
}
void record_shadow_blend(uint32_t tag, bool additive);
bool match_shadow_blend(uint32_t tag, const void *data, bool &additive);
void submit_shadow_blend(bool additive);
bool material_state_enabled();
bool match_material_state(uint32_t dma_tag, const uint32_t *words, size_t size, size_t mscal,
                          const uint8_t *vu_top, MaterialState &out);
void submit_material_state(const MaterialState &state);
}
