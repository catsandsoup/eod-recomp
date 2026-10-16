// Exact fast path for alpha-test FB_ONLY failure: color is always written,
// while only passing fragments write depth. All draws keep primitive order.
#pragma once
#include <cstdint>
namespace alpha_order {
enum class Test { Always, GreaterEqual, Greater, Other };
inline bool defer_depth(bool fb_only, bool writes_depth, Test test, uint32_t min_z, uint32_t max_z) {
    // Equal-Z GEQUAL visibility cannot change when an earlier fragment writes
    // Z: any previously passing sample remains passing. Strict GREATER and
    // varying depth do not have this property.
    return fb_only && writes_depth && min_z == max_z &&
           (test == Test::Always || test == Test::GreaterEqual);
}
}
