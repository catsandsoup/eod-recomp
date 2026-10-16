// Per-composed-frame aspect. Only the EE chooses game context; the GS worker
// receives its snapshot in command order, without reading guest RAM.
#pragma once
#include <cstdint>
namespace presentation {
inline thread_local int frame_aspect = -1;
inline int current(int startup_aspect) { return frame_aspect < 0 ? startup_aspect : frame_aspect; }
inline void set(int aspect) { frame_aspect = aspect >= 0 && aspect <= 3 ? aspect : -1; }
// Zero is reserved for old GS captures, which retain their launch aspect.
inline uint32_t tag(int aspect) { return 0x41535000u | uint32_t(aspect + 1); }
inline int from_tag(uint32_t value) {
    return (value & ~7u) == 0x41535000u && (value & 7u) >= 1 && (value & 7u) <= 4 ? int(value & 7u) - 1 : -1;
}
inline int for_context(int launch_aspect, bool racing) {
    return (launch_aspect == 1 || launch_aspect == 3) && !racing ? 0 : launch_aspect;
}
} // namespace presentation
// Target-specific producer, called only on the EE thread.
int eod_presentation_aspect();
