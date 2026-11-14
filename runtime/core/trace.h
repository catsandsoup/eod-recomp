// trace.h - Tracy profiler zones (CMake -DEOD_TRACY=ON). Without EOD_TRACY every macro
// is empty, so the zones cost nothing in release builds.
//
//   EOD_ZONE("name")            CPU zone for the enclosing scope
//   EOD_ZONE_TEXT(str, len)     annotate the current zone
//   EOD_PLOT("name", value)     numeric plot (draws, batches, ...)
//   EOD_FRAME_MARK              end of a presented frame
//   EOD_THREAD_NAME("name")     name the calling thread in the profiler
#pragma once
#ifdef EOD_TRACY
#include <cstdio>  // Tracy 0.11.1 TracyScoped.hpp uses vsnprintf without including it
#include <tracy/Tracy.hpp>
#define EOD_ZONE_CAT2(a, b) a##b
#define EOD_ZONE_CAT(a, b) EOD_ZONE_CAT2(a, b)
// unique per line, so several zones can open in one scope (each runs to the scope end)
#define EOD_ZONE(name) ZoneNamedN(EOD_ZONE_CAT(eod_zone_, __LINE__), name, true)
#define EOD_ZONE_TEXT(s, n)
#define EOD_PLOT(name, v) TracyPlot(name, (int64_t)(v))
#define EOD_FRAME_MARK FrameMark
#define EOD_FRAME_MARK_NAMED(name) FrameMarkNamed(name)
#define EOD_THREAD_NAME(name) tracy::SetThreadName(name)
#else
#define EOD_ZONE(name)
#define EOD_ZONE_TEXT(s, n)
#define EOD_PLOT(name, v)
#define EOD_FRAME_MARK
#define EOD_FRAME_MARK_NAMED(name)
#define EOD_THREAD_NAME(name)
#endif
