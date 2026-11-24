#include "netplay_damage_detail.h"
#include "netplay_wire.h"
#include "core/memory.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace netplay_damage {
namespace {
constexpr unsigned kHeader = 5, kBytesPerBone = 7;
constexpr float kDentScale = 1024.f;
unsigned bones(uint32_t car) {
    if (!car || (car & 3) || car > EE_RAM_SIZE - 0x1C34) return 0;
    const uint32_t count = ee_r32(car + 0x4B4), model = ee_r32(car + 0x25C);
    return model && !(model & 3) && model <= EE_RAM_SIZE - 0x2450 &&
           count <= kMaxBones && count == ee_r32(model + 0x1C) ? count : 0;
}
}

std::vector<uint8_t> capture(uint32_t car) {
    const unsigned count = bones(car);
    if (!count) return {};
    netplay::Writer w;
    w.b.reserve(kHeader + kBytesPerBone * count);
    unsigned panels = 0, wheels = 0;
    for (unsigned i = 0; i < 8; ++i)
        panels |= (ee_r32(car + 0x13E8 + 0x18 * i + 0x14) & 3u) << (2*i);
    for (unsigned i = 0; i < 4; ++i)
        if (ee_r32(car + 0x1A00 + 0x54*i + 0x50) & 0x80) wheels |= 1u << i;
    const uint32_t side = ee_r32(car + 0x1C04);
    w.u8(count); w.u16(panels); w.u8(wheels); w.u8(side < 4 ? side + 1 : 0);
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t dent = car + 0x4B8 + 12*i;
        uint8_t shade = 0;
        for (unsigned axis = 0; axis < 3; ++axis) {
            uint32_t bits = ee_r32(dent + 4*axis);
            if (axis == 2) { shade = bits & 255; bits &= 0xFFFFFF00u; }
            float value; std::memcpy(&value, &bits, sizeof(value));
            if (!std::isfinite(value) || std::fabs(value) >= 32) return {};
            const long quantized = std::lround(value * kDentScale);
            w.u16(static_cast<uint16_t>(std::min(32767L, quantized)));
        }
        w.u8(shade);
    }
    return std::move(w.b);
}

bool valid(const uint8_t *data, size_t size) {
    return size >= kHeader && data[0] && data[0] <= kMaxBones &&
           data[3] <= 15 && data[4] <= 4 && size == kHeader + kBytesPerBone * data[0];
}

// Adjacent undented bones often share all seven bytes. Use a raw checkpoint
// whenever run lengths would expand it. Even the largest raw model fits in 1,126 bytes including the format byte.
std::vector<uint8_t> pack(const std::vector<uint8_t> &detail) {
    if (!valid(detail.data(), detail.size())) return {};
    std::vector<uint8_t> encoded{1};
    encoded.insert(encoded.end(), detail.begin(), detail.begin() + kHeader);
    for (size_t pos = kHeader; pos < detail.size();) {
        size_t end = pos + kBytesPerBone;
        while (end < detail.size() && std::memcmp(&detail[pos], &detail[end], kBytesPerBone) == 0)
            end += kBytesPerBone;
        encoded.push_back(static_cast<uint8_t>((end - pos) / kBytesPerBone));
        encoded.insert(encoded.end(), detail.begin() + pos, detail.begin() + pos + kBytesPerBone);
        pos = end;
    }
    if (encoded.size() > detail.size()) {
        encoded.assign(1, 0);
        encoded.insert(encoded.end(), detail.begin(), detail.end());
    }
    return encoded;
}

std::vector<uint8_t> unpack(const uint8_t *data, size_t size) {
    if (size < 1 + kHeader || size > 1 + kHeader + kBytesPerBone * kMaxBones) return {};
    const uint8_t format = *data++;
    --size;
    if (format == 0) return valid(data, size) ? std::vector<uint8_t>(data, data + size) : std::vector<uint8_t>{};
    if (format != 1 || !data[0] || data[0] > kMaxBones || (size - kHeader) % (1 + kBytesPerBone)) return {};
    std::vector<uint8_t> decoded(data, data + kHeader);
    unsigned remaining = data[0];
    for (size_t pos = kHeader; pos < size; pos += 1 + kBytesPerBone) {
        const unsigned run = data[pos];
        if (!run || run > remaining) return {};
        remaining -= run;
        for (unsigned i = 0; i < run; ++i)
            decoded.insert(decoded.end(), data + pos + 1, data + pos + 1 + kBytesPerBone);
    }
    return valid(decoded.data(), decoded.size()) ? decoded : std::vector<uint8_t>{};
}

bool apply(uint32_t car, const std::vector<uint8_t> &detail) {
    if (!valid(detail.data(), detail.size()) || bones(car) != detail[0]) return false;
    netplay::Reader r(detail.data(), detail.size());
    const unsigned count = r.u8(), panels = r.u16(), wheels = r.u8();
    const int side = static_cast<int>(r.u8()) - 1;
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t dent = car + 0x4B8 + 12*i;
        for (unsigned axis = 0; axis < 3; ++axis) {
            float value = r.i16(kDentScale);
            uint32_t bits; std::memcpy(&bits, &value, sizeof(bits));
            if (axis == 2) bits = (bits & 0xFFFFFF00u) | r.u8();
            ee_w32(dent + 4*axis, bits);
        }
    }
    apply_parts(car, panels, wheels, side);
    return true;
}
}
