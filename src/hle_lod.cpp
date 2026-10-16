// hle_lod.cpp - EOD_CAR_LOD_MULTIPLIER=x (a number >= 1, default 1): car
// models switch to their lower-detail versions x times farther away.
//
// The car model loader 0021ABF8 (a0 = the model object) reads each LOD level's
// switch distance into the object's own table: records of 16 bytes at
// +0xF8 (the distance first), the level count at +0x1F8, the loaded model
// handles at +0x74. Each frame the selectors (0021C6E0 for one car, 00218E68
// for a batch) pick the first level whose distance is above the car's scaled
// distance, through the pointers at +0x220 (records), +0x21C (handles) and
// +0x224 (count). Copies made by 0021C130 point into the original's table, so
// the distances are scaled once, right after the loader has read them, in the
// loader's own table only; each load reads them afresh.
//
// The table is changed only if all of it checks out (its pointers point at
// the object's own storage, 1-16 levels, the minimum level in range, every
// model loaded, distances finite and in increasing order); otherwise, and with
// the default multiplier of 1, nothing is written.
#include <cstdlib>
#include <cstring>

#include "core/cpu.h"
#include "core/log.h"
#include "core/memory.h"
#include "lod_thresholds.h"

extern "C" void f_0021ABF8_orig();

namespace {

float loadf(uint32_t a) {
    uint32_t u = ee_r32(a);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

void storef(uint32_t a, float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    ee_w32(a, u);
}

float multiplier() {
    static const float value = [] {
        float v;
        if (!eod_lod::parse_multiplier(std::getenv("EOD_CAR_LOD_MULTIPLIER"), v))
            WARN("EOD_CAR_LOD_MULTIPLIER must be a number >= 1 - using 1");
        else if (v != 1)
            LOG(LOG_CORE, "car LOD distances x%g", (double)v);
        return v;
    }();
    return value;
}

}  // namespace

extern "C" void f_0021ABF8() {
    const uint32_t object = arg_u32(0);  // (a0 doesn't survive the call)
    f_0021ABF8_orig();
    const float factor = multiplier();
    if (factor == 1) return;
    if (!object || (object & 3) || object > EE_RAM_SIZE - 0x554) return;
    const uint32_t records = ee_r32(object + 0x220), handles = ee_r32(object + 0x21C);
    const uint32_t count_ptr = ee_r32(object + 0x224);
    const uint32_t count = count_ptr == object + 0x1F8 ? ee_r32(count_ptr) : 0;
    bool valid = records == object + 0xF8 && handles && count >= 1 && count <= eod_lod::max_levels &&
                 ee_r32(object + 0x550) < count;  // the minimum level
    float before[eod_lod::max_levels], after[eod_lod::max_levels];
    for (uint32_t i = 0; valid && i < count; ++i) {
        before[i] = loadf(records + i * 16);
        valid = ee_r32(object + 0x74 + i * 4) != 0xFFFFFFFFu;  // the level's model was loaded
    }
    valid = valid && eod_lod::scale(before, after, count, factor);
    static unsigned rejected;
    if (!valid) {
        if (rejected++ < 16) LOG(LOG_CORE, "car LOD: table of object %08x left unchanged", object);
        return;
    }
    for (uint32_t i = 0; i < count; ++i) storef(records + i * 16, after[i]);
}
